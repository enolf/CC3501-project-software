#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// The wire format for the RP2040 <-> Pi link.
//
//   <PREFIX> <MS> <TYPE> <LEN> <PAYLOAD> *<CRC>\n
//
// e.g.  EVT 12345 DOOR 10 state=open *3F
//       CMD 12350 SCAN 0 *A1                 (no payload, so no payload field)
//
// | Field   | Format             | Why it is there                            |
// |---------|--------------------|--------------------------------------------|
// | PREFIX  | EVT / CMD / RSP    | Direction, AND the sync token (see below)   |
// | MS      | decimal            | Ordering, and reset detection when it jumps |
// |         |                    | backwards. The Pi stamps wall-clock time.   |
// | TYPE    | uppercase token    | Message type, present from frame one so     |
// |         |                    | temperature, inventory, RFID and sales can  |
// |         |                    | all share one link                          |
// | LEN     | decimal            | Byte count of PAYLOAD. A mismatch rejects   |
// |         |                    | the frame before anything parses it         |
// | PAYLOAD | key=value pairs    | Omitted entirely when LEN is 0              |
// | CRC     | 2 hex digits       | CRC-8/ATM over every byte from PREFIX[0] up |
// |         |                    | to but NOT including the space before '*'   |
//
// WHY TEXT RATHER THAN A PACKED BINARY STRUCT
// -------------------------------------------
// The brief asked for type, length and checksum; dashboard.md section 6 asked
// for something readable in a plain serial monitor. This satisfies both. At
// these data rates compactness is worth nothing, and being able to watch the
// link in a terminal — or grep a captured log — is worth a great deal while
// two machines are being brought up against each other.
//
// WHY THE PREFIX DOUBLES AS A SYNC TOKEN
// --------------------------------------
// Debug logging and protocol frames share one USB CDC stream (decision D1), so
// the parser must cope with lines it did not send. Any line not beginning with
// a known prefix is discarded in silence — that is how `[12.345 Information]:
// ...` passes through harmlessly. A log line that lands in the MIDDLE of a
// frame corrupts it, and that is what the CRC is for: the frame is rejected and
// counted rather than acted on.
//
// This header is deliberately free of any hardware dependency, so the whole
// codec is exercised on a PC before a wire is involved.

namespace frame {

/// Longest complete frame including the terminating newline. Anything longer
/// is dropped rather than buffered, so a stuck sender cannot exhaust memory.
constexpr size_t MAX_FRAME_LEN = 128;

/// Longest message type token, INCLUDING the terminating null — so 19 usable
/// characters.
///
/// Raised from 16 at stage 15.3. `SQUARE_CANCELLED` and `SQUARE_LATE_PAID` are
/// both exactly 16 characters, which needs 17 bytes with the null, and both
/// build() and the parser reject an over-long type outright. The failure would
/// have been a cancellation that silently never left the board — the frame is
/// refused at the sender, so there is nothing on the wire to look at.
constexpr size_t MAX_TYPE_LEN = 20;

/// Longest payload, including the terminating null.
constexpr size_t MAX_PAYLOAD_LEN = 96;

/// Direction and kind. Also the token the parser synchronises on.
enum class Prefix : uint8_t {
    Event,      ///< EVT — something happened
    Command,    ///< CMD — please do something
    Response,   ///< RSP — the answer to a command
};

/// A decoded frame.
struct Frame {
    Prefix   prefix = Prefix::Event;
    uint32_t ms = 0;
    char     type[MAX_TYPE_LEN] = {};
    char     payload[MAX_PAYLOAD_LEN] = {};
};

/// What feeding a byte to the parser produced.
enum class Result : uint8_t {
    NeedMore,   ///< Nothing complete yet
    Complete,   ///< A valid frame is available
    Ignored,    ///< A finished line that was not a frame (log output)
    Rejected,   ///< A frame-shaped line that failed validation
};

/// CRC-8/ATM: polynomial 0x07, initial value 0x00, no reflection, no final xor.
/// Exposed because both the builder and the tests need it.
uint8_t crc8(const char *data, size_t length);

/// Render a frame into `out`.
///
/// `payload` may be null or empty, in which case the payload field is omitted
/// and LEN is 0. Returns the number of characters written (excluding the
/// terminating null), or 0 if the result would not fit within MAX_FRAME_LEN.
/// The output ends with '\n'.
size_t build(char *out, size_t out_length, Prefix prefix, uint32_t ms,
             const char *type, const char *payload);

/// Byte-at-a-time frame parser.
///
/// A struct with explicit state rather than file-scope statics, so tests can
/// run independent instances and so a second link could exist later without
/// this being rewritten.
struct Parser {
    /// Feed one received byte. Never blocks and never allocates.
    ///
    /// On Complete, the decoded frame is in `frame` until the next call.
    Result feed(char c);

    /// Discard any partial line and start again.
    void reset();

    Frame frame;                 ///< Valid immediately after Complete

    // Counters. All are diagnostic; `rejected` in particular should be zero on
    // a healthy link, and a rising count is the first sign of interference on
    // the shared stream.
    uint32_t accepted = 0;
    uint32_t rejected = 0;
    uint32_t ignored = 0;
    uint32_t overlong = 0;

private:
    char   line[MAX_FRAME_LEN] = {};
    size_t used = 0;
    bool   line_too_long = false;

    Result finish_line();
};

/// Read one `key=value` out of a payload.
///
/// Returns false if the key is absent. `out` receives the value, truncated to
/// `out_length` if necessary and always null-terminated.
bool value_for_key(const char *payload, const char *key,
                   char *out, size_t out_length);

/// Read a `key=value` whose value is a decimal number.
/// Returns false if the key is absent or the value is not a plain number.
bool u32_for_key(const char *payload, const char *key, uint32_t &out);

/// Name of a prefix, for logging and for building.
const char *prefix_name(Prefix prefix);

} // namespace frame
