#include "peripherals/pi_link/frame.h"

#include <stdio.h>
#include <string.h>

namespace frame {

namespace {

/// Longest a decimal uint32_t can be ("4294967295").
constexpr size_t MAX_DECIMAL_DIGITS = 10;

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/// Convert one hex digit, or return -1.
int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// Read a decimal number starting at `pos`, stopping at the first non-digit.
/// Advances `pos` past the digits. Returns false if there were none, or if the
/// value overflows.
bool read_decimal(const char *line, size_t &pos, size_t limit, uint32_t &out)
{
    const size_t start = pos;
    uint64_t value = 0;

    while (pos < limit && is_digit(line[pos])) {
        value = value * 10 + (uint64_t)(line[pos] - '0');
        if (value > 0xFFFFFFFFull) {
            return false;
        }
        pos++;
    }

    if (pos == start || pos - start > MAX_DECIMAL_DIGITS) {
        return false;
    }
    out = (uint32_t)value;
    return true;
}

/// Match one of the three prefixes at the start of a line.
bool read_prefix(const char *line, size_t limit, Prefix &out)
{
    if (limit < 4 || line[3] != ' ') {
        return false;
    }
    if (memcmp(line, "EVT", 3) == 0) { out = Prefix::Event;    return true; }
    if (memcmp(line, "CMD", 3) == 0) { out = Prefix::Command;  return true; }
    if (memcmp(line, "RSP", 3) == 0) { out = Prefix::Response; return true; }
    return false;
}

} // namespace

uint8_t crc8(const char *data, size_t length)
{
    // CRC-8/ATM, computed a bit at a time. The table-driven version would be
    // faster and 256 bytes larger; at one frame every few hundred milliseconds
    // that trade is not worth making.
    uint8_t crc = 0x00;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint8_t)data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

const char *prefix_name(Prefix prefix)
{
    switch (prefix) {
        case Prefix::Event:    return "EVT";
        case Prefix::Command:  return "CMD";
        case Prefix::Response: return "RSP";
    }
    return "???";
}

size_t build(char *out, size_t out_length, Prefix prefix, uint32_t ms,
             const char *type, const char *payload)
{
    if (out == nullptr || out_length == 0 || type == nullptr) {
        return 0;
    }

    const size_t payload_len = (payload != nullptr) ? strlen(payload) : 0;
    if (payload_len >= MAX_PAYLOAD_LEN || strlen(type) >= MAX_TYPE_LEN) {
        return 0;
    }

    // Everything up to but not including the space before the '*'. This is
    // exactly the span the CRC covers, so it is built first and then checksummed
    // rather than being reconstructed afterwards.
    int written;
    if (payload_len > 0) {
        written = snprintf(out, out_length, "%s %lu %s %u %s",
                           prefix_name(prefix), (unsigned long)ms, type,
                           (unsigned)payload_len, payload);
    } else {
        written = snprintf(out, out_length, "%s %lu %s 0",
                           prefix_name(prefix), (unsigned long)ms, type);
    }

    if (written < 0 || (size_t)written >= out_length) {
        return 0;
    }

    const uint8_t crc = crc8(out, (size_t)written);

    const int total = snprintf(out + written, out_length - (size_t)written,
                               " *%02X\n", crc);
    if (total < 0) {
        return 0;
    }

    const size_t length = (size_t)written + (size_t)total;
    if (length >= out_length || length > MAX_FRAME_LEN) {
        return 0;
    }
    return length;
}

void Parser::reset()
{
    used = 0;
    line_too_long = false;
}

Result Parser::feed(char c)
{
    if (c == '\r') {
        return Result::NeedMore;    // tolerate CRLF senders
    }

    if (c != '\n') {
        if (used < sizeof(line) - 1) {
            line[used++] = c;
        } else {
            // Too long. Keep consuming until the newline so the parser
            // resynchronises cleanly, but remember to throw the line away.
            line_too_long = true;
        }
        return Result::NeedMore;
    }

    const Result result = finish_line();
    used = 0;
    line_too_long = false;
    return result;
}

Result Parser::finish_line()
{
    if (line_too_long) {
        overlong++;
        return Result::Rejected;
    }

    if (used == 0) {
        return Result::NeedMore;    // blank line, nothing to report
    }

    line[used] = '\0';

    // --- Is this even one of ours? ---
    // Anything else is log output sharing the stream and is dropped silently.
    Prefix prefix;
    if (!read_prefix(line, used, prefix)) {
        ignored++;
        return Result::Ignored;
    }

    size_t pos = 4;     // past "EVT "

    uint32_t ms = 0;
    if (!read_decimal(line, pos, used, ms) || pos >= used || line[pos] != ' ') {
        rejected++;
        return Result::Rejected;
    }
    pos++;

    // --- TYPE ---
    const size_t type_start = pos;
    while (pos < used && line[pos] != ' ') {
        pos++;
    }
    const size_t type_len = pos - type_start;
    if (type_len == 0 || type_len >= MAX_TYPE_LEN || pos >= used) {
        rejected++;
        return Result::Rejected;
    }
    pos++;      // past the space

    // --- LEN ---
    uint32_t payload_len = 0;
    if (!read_decimal(line, pos, used, payload_len)) {
        rejected++;
        return Result::Rejected;
    }
    if (payload_len >= MAX_PAYLOAD_LEN) {
        rejected++;
        return Result::Rejected;
    }
    if (pos >= used || line[pos] != ' ') {
        rejected++;
        return Result::Rejected;
    }
    pos++;

    // --- PAYLOAD ---
    // Taken by LENGTH, not by scanning for a delimiter, which is the whole
    // point of carrying a length: the payload may contain spaces, and a
    // truncated line cannot masquerade as a complete one.
    const size_t payload_start = pos;
    if (payload_len > 0) {
        if (payload_start + payload_len > used) {
            rejected++;     // line ended early: LEN disagrees with reality
            return Result::Rejected;
        }
        pos += payload_len;
        if (pos >= used || line[pos] != ' ') {
            rejected++;     // no space where the payload was supposed to end
            return Result::Rejected;
        }
        pos++;
    }

    // --- CRC ---
    if (pos + 3 != used || line[pos] != '*') {
        rejected++;
        return Result::Rejected;
    }
    const int high = hex_value(line[pos + 1]);
    const int low  = hex_value(line[pos + 2]);
    if (high < 0 || low < 0) {
        rejected++;
        return Result::Rejected;
    }
    const uint8_t stated = (uint8_t)((high << 4) | low);

    // The covered span stops one character before the '*', excluding the space
    // that separates them.
    const uint8_t computed = crc8(line, pos - 1);
    if (stated != computed) {
        rejected++;
        return Result::Rejected;
    }

    // --- Everything checks out; publish it ---
    frame = Frame{};
    frame.prefix = prefix;
    frame.ms = ms;
    memcpy(frame.type, line + type_start, type_len);
    frame.type[type_len] = '\0';
    if (payload_len > 0) {
        memcpy(frame.payload, line + payload_start, payload_len);
    }
    frame.payload[payload_len] = '\0';

    accepted++;
    return Result::Complete;
}

bool value_for_key(const char *payload, const char *key,
                   char *out, size_t out_length)
{
    if (payload == nullptr || key == nullptr || out == nullptr || out_length == 0) {
        return false;
    }

    const size_t key_len = strlen(key);
    const char *cursor = payload;

    while (*cursor != '\0') {
        // A key only counts at the start of the payload or just after a space,
        // so looking for "id" does not match the "id" inside "order_id".
        const bool at_field_start = (cursor == payload) || (cursor[-1] == ' ');

        if (at_field_start &&
            strncmp(cursor, key, key_len) == 0 &&
            cursor[key_len] == '=') {

            const char *value = cursor + key_len + 1;
            size_t length = 0;
            while (value[length] != '\0' && value[length] != ' ') {
                length++;
            }
            if (length >= out_length) {
                length = out_length - 1;
            }
            memcpy(out, value, length);
            out[length] = '\0';
            return true;
        }
        cursor++;
    }
    return false;
}

bool u32_for_key(const char *payload, const char *key, uint32_t &out)
{
    char text[MAX_DECIMAL_DIGITS + 2] = {};
    if (!value_for_key(payload, key, text, sizeof(text))) {
        return false;
    }

    size_t pos = 0;
    const size_t length = strlen(text);
    if (length == 0) {
        return false;
    }

    uint32_t value = 0;
    if (!read_decimal(text, pos, length, value) || pos != length) {
        return false;   // trailing rubbish, or not a number at all
    }
    out = value;
    return true;
}

} // namespace frame
