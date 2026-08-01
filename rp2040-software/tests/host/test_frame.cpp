// Tests for the serial frame codec.
//
// This is the one module where a bug is both easy to write and expensive to
// find on hardware: a mis-parsed frame becomes a wrong price or a sale credited
// to nobody, and the evidence is a byte that vanished down a wire. So the
// malformed cases matter more than the happy path, and there are more of them.

#include "test_support.h"

#include <string.h>

#include "peripherals/pi_link/frame.h"

namespace {

/// Push a whole string through the parser and return the last result.
frame::Result feed_all(frame::Parser &parser, const char *text)
{
    frame::Result last = frame::Result::NeedMore;
    for (const char *c = text; *c != '\0'; c++) {
        last = parser.feed(*c);
    }
    return last;
}

/// Build a frame and immediately parse it back.
bool round_trip(frame::Prefix prefix, uint32_t ms, const char *type,
                const char *payload, frame::Frame &out)
{
    char wire[frame::MAX_FRAME_LEN];
    const size_t length = frame::build(wire, sizeof(wire), prefix, ms, type, payload);
    if (length == 0) {
        return false;
    }

    frame::Parser parser;
    if (feed_all(parser, wire) != frame::Result::Complete) {
        return false;
    }
    out = parser.frame;
    return true;
}

} // namespace

void test_frame_build()
{
    testing::suite("frame building");

    // The published check value for CRC-8/ATM: "123456789" must give 0xF4.
    // This pins the implementation to the standard rather than merely to
    // itself, which matters because the Pi-side codec in
    // pi4-software/tools/fake_pi.py is a separate implementation that must
    // agree byte for byte. Two self-consistent but different CRCs would produce
    // a link where every frame is rejected and nothing explains why.
    CHECK_EQ(frame::crc8("123456789", 9), 0xF4);
    testing::case_passed("CRC-8/ATM matches the published check value");

    char wire[frame::MAX_FRAME_LEN];

    const size_t length = frame::build(wire, sizeof(wire), frame::Prefix::Event,
                                       12345, "DOOR", "state=open");
    CHECK(length > 0);
    CHECK(wire[length - 1] == '\n');
    // The shape must be exactly as documented, because a person reading a
    // serial monitor is a first-class consumer of this format.
    CHECK(strncmp(wire, "EVT 12345 DOOR 10 state=open *", 30) == 0);
    testing::case_passed("a frame looks like the documented format");

    // An empty payload omits the field entirely rather than leaving a gap.
    const size_t empty = frame::build(wire, sizeof(wire), frame::Prefix::Command,
                                      7, "SCAN", nullptr);
    CHECK(empty > 0);
    CHECK(strncmp(wire, "CMD 7 SCAN 0 *", 14) == 0);
    testing::case_passed("no payload means no payload field");

    // A payload too large for the buffer must fail rather than truncate: half a
    // message that still passes its own CRC is far worse than no message.
    char huge[frame::MAX_PAYLOAD_LEN + 10];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    CHECK_EQ(frame::build(wire, sizeof(wire), frame::Prefix::Event, 1, "BIG", huge), 0);
    testing::case_passed("an oversized payload is refused, not truncated");

    // A buffer too small must also fail cleanly.
    char tiny[10];
    CHECK_EQ(frame::build(tiny, sizeof(tiny), frame::Prefix::Event, 12345,
                          "DOOR", "state=open"), 0);
    testing::case_passed("a buffer too small is refused");
}

void test_frame_round_trip()
{
    testing::suite("frame round trip");

    frame::Frame f;

    // Every message type from plan.md section 2, so the format is proven
    // against the real traffic rather than against invented examples.
    struct Case { frame::Prefix prefix; const char *type; const char *payload; };
    const Case cases[] = {
        { frame::Prefix::Event,    "BOOT",        "fw=0.2 reset=power" },
        { frame::Prefix::Event,    "HB",          nullptr },
        { frame::Prefix::Event,    "DOOR",        "state=closed" },
        { frame::Prefix::Event,    "TEMP",        "rom=28FF1234 c=4.250" },
        { frame::Prefix::Event,    "RFID",        "uid=DEADBEEF granted=1" },
        { frame::Prefix::Event,    "TXN_START",   "id=99 items=coke:2,fanta:1 owed=600" },
        { frame::Prefix::Event,    "TXN_METHOD",  "id=99 method=cash" },
        { frame::Prefix::Event,    "COIN",        "id=99 denom=200 delta_g=6.60" },
        { frame::Prefix::Event,    "COIN_REJECT", "delta_g=4.00" },
        { frame::Prefix::Event,    "TXN_END",     "id=99 outcome=paid paid=600" },
        { frame::Prefix::Event,    "HEALTH",      "die_c=31 box_g=250 faults=0" },
        { frame::Prefix::Command,  "SCAN",        nullptr },
        { frame::Prefix::Command,  "SQUARE_LINK", "cents=200 id=99" },
        { frame::Prefix::Event,    "INV",         "coke=5 sprite=4 fanta=5 pasito=5 conf=98" },
        { frame::Prefix::Response, "SQUARE_URL",  "id=99 url=https://sq.co/abc123" },
        { frame::Prefix::Event,    "SQUARE_PAID", "id=99 order=ORD42" },
        { frame::Prefix::Response, "SQUARE_ERR",  "id=99 reason=timeout" },
        // Cancellation, stage 15.3.
        { frame::Prefix::Command,  "SQUARE_CANCEL",    "id=99" },
        { frame::Prefix::Response, "SQUARE_CANCELLED", "id=99 ok=1" },
        { frame::Prefix::Event,    "SQUARE_LATE_PAID", "id=99 order=ORD42 cents=600" },
    };

    for (const Case &c : cases) {
        const bool ok = round_trip(c.prefix, 4242, c.type, c.payload, f);
        CHECK(ok);
        if (!ok) {
            continue;
        }
        CHECK(f.prefix == c.prefix);
        CHECK_EQ(f.ms, 4242);
        CHECK(strcmp(f.type, c.type) == 0);
        CHECK(strcmp(f.payload, c.payload == nullptr ? "" : c.payload) == 0);
    }
    testing::case_passed("all 20 message types survive a round trip");

    // The type-length boundary, pinned deliberately.
    //
    // This caught a real bug at stage 15.3: MAX_TYPE_LEN was 16, and both
    // SQUARE_CANCELLED and SQUARE_LATE_PAID are exactly 16 characters, so a
    // cancellation would have been refused by build() and never reached the
    // wire at all. Nothing downstream would have shown a symptom — the frame is
    // rejected at the sender — so the link would simply have stopped cancelling
    // with no evidence anywhere. The two checks below make the limit itself the
    // thing under test, so adding a longer type fails here rather than in the
    // field.
    {
        char longest[frame::MAX_TYPE_LEN];
        memset(longest, 'T', sizeof(longest) - 1);
        longest[sizeof(longest) - 1] = '\0';
        CHECK(round_trip(frame::Prefix::Event, 1, longest, "x=1", f));
        CHECK(strcmp(f.type, longest) == 0);

        char too_long[frame::MAX_TYPE_LEN + 1];
        memset(too_long, 'T', sizeof(too_long) - 1);
        too_long[sizeof(too_long) - 1] = '\0';
        char wire[frame::MAX_FRAME_LEN];
        CHECK_EQ(frame::build(wire, sizeof(wire), frame::Prefix::Event, 1,
                              too_long, "x=1"), 0);
        testing::case_passed("the longest allowed type fits and one longer is refused");
    }

    // Every type actually sent by the firmware must fit, checked against the
    // limit rather than against a count someone has to remember to update.
    {
        const char *types[] = {
            "BOOT", "HB", "DOOR", "TEMP", "RFID", "TXN_START", "TXN_METHOD",
            "COIN", "COIN_REJECT", "TXN_END", "HEALTH", "SCAN", "SQUARE_LINK",
            "INV", "SQUARE_URL", "SQUARE_PAID", "SQUARE_ERR", "SQUARE_CANCEL",
            "SQUARE_CANCELLED", "SQUARE_LATE_PAID",
        };
        for (const char *t : types) {
            CHECK(strlen(t) < frame::MAX_TYPE_LEN);
        }
        testing::case_passed("no message type in the protocol exceeds MAX_TYPE_LEN");
    }

    // Payloads contain spaces, which is exactly why the length field exists —
    // the parser must take the payload by count, not by scanning for a space.
    CHECK(round_trip(frame::Prefix::Event, 1, "X", "a=1 b=2 c=3 d=4", f));
    CHECK(strcmp(f.payload, "a=1 b=2 c=3 d=4") == 0);
    testing::case_passed("a payload full of spaces survives intact");

    // Boundary values on the timestamp.
    CHECK(round_trip(frame::Prefix::Event, 0, "HB", nullptr, f));
    CHECK_EQ(f.ms, 0);
    CHECK(round_trip(frame::Prefix::Event, 4294967295u, "HB", nullptr, f));
    CHECK_EQ(f.ms, 4294967295u);
    testing::case_passed("timestamps of 0 and 2^32-1 both survive");
}

void test_frame_rejection()
{
    testing::suite("frame rejection");

    // --- A single corrupted byte ---
    // The realistic failure on a stream shared with debug logging.
    {
        char wire[frame::MAX_FRAME_LEN];
        frame::build(wire, sizeof(wire), frame::Prefix::Event, 12345, "DOOR",
                     "state=open");
        char *payload = strstr(wire, "state=open");
        CHECK(payload != nullptr);
        payload[6] = 'X';               // "state=Xpen"

        frame::Parser parser;
        CHECK(feed_all(parser, wire) == frame::Result::Rejected);
        CHECK_EQ(parser.rejected, 1);
        CHECK_EQ(parser.accepted, 0);
        testing::case_passed("one flipped byte is caught by the CRC");
    }

    // --- A length that disagrees with the payload ---
    {
        frame::Parser parser;
        // LEN says 99 but the payload is far shorter.
        CHECK(feed_all(parser, "EVT 1 DOOR 99 state=open *00\n")
              == frame::Result::Rejected);
        testing::case_passed("a LEN longer than the line is rejected");
    }
    {
        frame::Parser parser;
        // LEN says 4 but the payload is 10 characters, so the character after
        // the counted region is not the expected space.
        CHECK(feed_all(parser, "EVT 1 DOOR 4 state=open *00\n")
              == frame::Result::Rejected);
        testing::case_passed("a LEN shorter than the payload is rejected");
    }

    // --- Structural damage ---
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "EVT 1 DOOR 10 state=open\n") == frame::Result::Rejected);
        testing::case_passed("a missing checksum is rejected");
    }
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "EVT 1 DOOR 10 state=open *ZZ\n")
              == frame::Result::Rejected);
        testing::case_passed("a non-hex checksum is rejected");
    }
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "EVT notanumber DOOR 0 *00\n")
              == frame::Result::Rejected);
        testing::case_passed("a non-numeric timestamp is rejected");
    }
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "EVT 1\n") == frame::Result::Rejected);
        testing::case_passed("a truncated frame is rejected");
    }

    // --- Nothing is ever accepted by accident ---
    {
        frame::Parser parser;
        feed_all(parser, "EVT 1 DOOR 10 state=open *00\n");
        CHECK_EQ(parser.accepted, 0);
        testing::case_passed("no malformed line is ever counted as accepted");
    }
}

void test_frame_stream()
{
    testing::suite("frame parsing in a shared stream");

    // --- Debug logging on the same wire ---
    // Decision D1 puts protocol frames and log output on one USB CDC stream, so
    // this is the normal case, not an edge case.
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "[12.345 Information]: switches: door starts CLOSED\n")
              == frame::Result::Ignored);
        CHECK_EQ(parser.ignored, 1);
        CHECK_EQ(parser.rejected, 0);

        // ...and a real frame straight afterwards still parses.
        char wire[frame::MAX_FRAME_LEN];
        frame::build(wire, sizeof(wire), frame::Prefix::Event, 5, "HB", nullptr);
        CHECK(feed_all(parser, wire) == frame::Result::Complete);
        CHECK(strcmp(parser.frame.type, "HB") == 0);
        testing::case_passed("log lines are ignored without disturbing the parser");
    }

    // --- Split across several reads ---
    // A real serial port delivers whatever happens to be in the buffer.
    {
        char wire[frame::MAX_FRAME_LEN];
        const size_t length = frame::build(wire, sizeof(wire), frame::Prefix::Event,
                                           777, "DOOR", "state=open");
        frame::Parser parser;
        for (size_t i = 0; i < length - 1; i++) {
            CHECK(parser.feed(wire[i]) == frame::Result::NeedMore);
        }
        CHECK(parser.feed(wire[length - 1]) == frame::Result::Complete);
        CHECK_EQ(parser.frame.ms, 777);
        testing::case_passed("a frame arriving one byte at a time parses correctly");
    }

    // --- An overlong line ---
    {
        frame::Parser parser;
        char flood[300];
        memset(flood, 'A', sizeof(flood) - 2);
        flood[sizeof(flood) - 2] = '\n';
        flood[sizeof(flood) - 1] = '\0';
        CHECK(feed_all(parser, flood) == frame::Result::Rejected);
        CHECK_EQ(parser.overlong, 1);

        // The parser must still be usable afterwards. A stuck sender should
        // cost one dropped line, not the link.
        char wire[frame::MAX_FRAME_LEN];
        frame::build(wire, sizeof(wire), frame::Prefix::Event, 9, "HB", nullptr);
        CHECK(feed_all(parser, wire) == frame::Result::Complete);
        testing::case_passed("a 300-byte line is dropped and the parser recovers");
    }

    // --- CRLF senders ---
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "EVT 1 HB 0 *5F\r\n") != frame::Result::NeedMore);
        testing::case_passed("a carriage return does not break the parser");
    }

    // --- A frame interrupted by a log line ---
    // Exactly what a printf landing mid-frame looks like. The result must be
    // rejection, never a plausible-looking wrong value.
    {
        frame::Parser parser;
        CHECK(feed_all(parser, "EVT 1 DOOR 10 sta[12.3 Info]: hello\n")
              == frame::Result::Rejected);
        CHECK_EQ(parser.accepted, 0);
        testing::case_passed("a frame interrupted mid-payload is rejected, not misread");
    }
}

void test_frame_payload_fields()
{
    testing::suite("payload field extraction");

    char value[32];

    CHECK(frame::value_for_key("state=open", "state", value, sizeof(value)));
    CHECK(strcmp(value, "open") == 0);

    CHECK(frame::value_for_key("coke=5 sprite=4 fanta=3", "sprite", value, sizeof(value)));
    CHECK(strcmp(value, "4") == 0);

    CHECK(frame::value_for_key("coke=5 sprite=4 fanta=3", "fanta", value, sizeof(value)));
    CHECK(strcmp(value, "3") == 0);
    testing::case_passed("keys are found at the start, middle and end");

    CHECK(!frame::value_for_key("coke=5", "sprite", value, sizeof(value)));
    testing::case_passed("an absent key reports absent");

    // A key must match a whole field. Without this, asking for "id" would find
    // the "id" inside "order_id" and return the wrong value — the kind of bug
    // that only shows up with a particular combination of fields.
    CHECK(!frame::value_for_key("order_id=42", "id", value, sizeof(value)));
    CHECK(frame::value_for_key("order_id=42 id=7", "id", value, sizeof(value)));
    CHECK(strcmp(value, "7") == 0);
    testing::case_passed("a key does not match the tail of a longer key");

    // Numeric extraction
    uint32_t number = 0;
    CHECK(frame::u32_for_key("cents=200 id=99", "cents", number));
    CHECK_EQ(number, 200);
    CHECK(frame::u32_for_key("cents=200 id=99", "id", number));
    CHECK_EQ(number, 99);
    testing::case_passed("numeric fields are read as numbers");

    CHECK(!frame::u32_for_key("state=open", "state", number));
    CHECK(!frame::u32_for_key("c=4.250", "c", number));
    testing::case_passed("a non-numeric value is refused rather than half-read");

    // A value longer than the caller's buffer is truncated, never overflowed.
    char small[5];
    CHECK(frame::value_for_key("url=https://example.com/very/long", "url",
                               small, sizeof(small)));
    CHECK_EQ(strlen(small), 4);
    testing::case_passed("an oversized value truncates safely");
}
