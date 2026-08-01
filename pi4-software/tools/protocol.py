"""The RP2040 <-> Pi wire format, in Python. One copy, imported by everything.

    import protocol
    port.write(protocol.build("EVT", "INV", "coke=5 sprite=4"))
    frame = protocol.parse(line)
    if frame and frame.type == "SCAN": ...

WHY THIS FILE EXISTS
--------------------
The codec now has two independent implementations: this one and the C++ one in
rp2040-software/src/peripherals/pi_link/frame.{h,cpp}. Two is already one more
than ideal, but it is unavoidable — the two ends are different languages. What
is entirely avoidable is a THIRD and a FOURTH: fake_pi.py carried its own copy
of the builder, parser and CRC, and bridge.py was about to grow another.

They would drift. That is not a worry, it is a near-certainty, and the failure
mode is nasty: a drifted CRC or a limit changed on one side only gives a link
where frames are silently rejected and nothing anywhere explains why. Stage 15.3
produced exactly that bug in miniature — MAX_TYPE_LEN was one byte too small for
SQUARE_CANCELLED, so every cancellation would have been refused at the sender
with no symptom downstream.

So: one Python implementation, and a self-test at the bottom that reads the
limits straight out of the C++ header and fails if they no longer agree. Run
`python protocol.py` to check.

FORMAT
------
    <PREFIX> <MS> <TYPE> <LEN> <PAYLOAD> *<CRC>\\n

    EVT 12345 DOOR 10 state=open *3F
    CMD 12350 SCAN 0 *A1                    (no payload, so no payload field)

  PREFIX   EVT / CMD / RSP. Direction, and the token the parser syncs on
  MS       decimal milliseconds; ordering, and reset detection when it jumps back
  TYPE     uppercase token
  LEN      byte count of PAYLOAD; a mismatch rejects the frame before parsing
  PAYLOAD  key=value pairs, omitted entirely when LEN is 0
  CRC      2 hex digits, CRC-8/ATM over PREFIX[0] up to but NOT including the
           space before '*'

Frames share the USB CDC stream with the board's debug logging, so parse()
returns None for anything that is not a frame — that is the normal fate of log
output, not an error.
"""

import time
from collections import namedtuple

# --- Limits -----------------------------------------------------------------
# These MUST match frame.h. The self-test at the bottom checks that they do.
# A limit that is larger here than on the board produces frames the board
# rejects; smaller, and we refuse to send frames it would have accepted.

MAX_FRAME_LEN = 128     #: Longest complete frame, including the newline
MAX_TYPE_LEN = 20       #: Longest type token INCLUDING the null, so 19 usable
MAX_PAYLOAD_LEN = 96    #: Longest payload, including the null

PREFIXES = ("EVT", "CMD", "RSP")

#: A decoded frame. A named tuple rather than a bare tuple so callers read
#: `frame.type` instead of `frame[2]` — the old 4-tuple was easy to unpack in
#: the wrong order.
Frame = namedtuple("Frame", "prefix ms type payload")


class ProtocolError(ValueError):
    """A frame could not be BUILT — the payload or type is too long.

    Only build() raises. parse() never does: bad input from a shared serial
    stream is expected and is reported by returning None.
    """


def crc8(data: bytes) -> int:
    """CRC-8/ATM: polynomial 0x07, init 0x00, no reflection, no final xor.

    The published check value is crc8(b"123456789") == 0xF4, which the C++ side
    pins too. Both implementations being self-consistent is not enough — they
    have to be consistent with each OTHER, and only a shared external check
    value proves that.
    """
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build(prefix: str, type_: str, payload: str = "", ms: int = None) -> bytes:
    """Render one frame, including the trailing newline.

    `ms` defaults to a monotonic millisecond count. Pass it explicitly only for
    tests — real traffic wants the real clock.

    Raises ProtocolError rather than truncating. A truncated frame that still
    passed its own CRC would be acted on at the far end, which is far worse than
    a frame that never goes out.
    """
    if prefix not in PREFIXES:
        raise ProtocolError(f"prefix {prefix!r} is not one of {PREFIXES}")
    if len(type_) >= MAX_TYPE_LEN:
        raise ProtocolError(
            f"type {type_!r} is {len(type_)} chars; the board's limit is "
            f"{MAX_TYPE_LEN - 1} and it will reject the frame")
    if len(payload) >= MAX_PAYLOAD_LEN:
        raise ProtocolError(
            f"payload is {len(payload)} chars; the board's limit is "
            f"{MAX_PAYLOAD_LEN - 1}")

    if ms is None:
        ms = int(time.monotonic() * 1000) & 0xFFFFFFFF

    # Everything up to but not including the space before '*'. This is exactly
    # the span the CRC covers, so it is built once and checksummed, rather than
    # being reconstructed afterwards and risking a one-byte disagreement.
    if payload:
        head = f"{prefix} {ms} {type_} {len(payload)} {payload}"
    else:
        head = f"{prefix} {ms} {type_} 0"

    wire = f"{head} *{crc8(head.encode()):02X}\n".encode()
    if len(wire) > MAX_FRAME_LEN:
        raise ProtocolError(
            f"frame is {len(wire)} bytes; the board drops anything over "
            f"{MAX_FRAME_LEN}")
    return wire


def parse(line: str):
    """Decode one line. Returns a Frame, or None if it is not a valid frame.

    None covers both "this was never a frame" (the board's log output, sharing
    the stream) and "this was frame-shaped but failed validation". Callers that
    care about the difference should check the prefix themselves — fake_pi.py
    does, because a frame-shaped line that fails is the number worth watching.
    """
    if len(line) < 4 or line[:3] not in PREFIXES or line[3] != " ":
        return None

    star = line.rfind(" *")
    if star < 0 or len(line) - star != 4:
        return None

    try:
        stated = int(line[star + 2:], 16)
    except ValueError:
        return None
    if stated != crc8(line[:star].encode()):
        return None

    # split(" ", 4) caps the split at four separators, so a payload containing
    # spaces survives intact in the final element. Splitting unbounded would
    # scatter "id=1 url=x" across several fields.
    parts = line[:star].split(" ", 4)
    if len(parts) < 4:
        return None

    prefix, ms_text, type_, len_text = parts[0], parts[1], parts[2], parts[3]
    try:
        ms, payload_len = int(ms_text), int(len_text)
    except ValueError:
        return None

    payload = parts[4] if len(parts) > 4 else ""
    # The length field is what makes a payload containing spaces safe. Checking
    # it is what makes a frame interrupted by a log line fail loudly instead of
    # parsing into a plausible wrong value.
    if len(payload) != payload_len:
        return None

    return Frame(prefix, ms, type_, payload)


def field(payload: str, key: str, default=None):
    """Read one `key=value` out of a payload. Returns `default` if absent.

    Matches a WHOLE field, so asking for "id" does not find the "id" inside
    "order_id" — the kind of bug that only appears with a particular combination
    of fields present.
    """
    for item in payload.split(" "):
        name, sep, value = item.partition("=")
        if sep and name == key:
            return value
    return default


def u32(payload: str, key: str, default=None):
    """Read a `key=value` whose value is a plain decimal number.

    Returns `default` if the key is absent or the value is not a number, rather
    than half-reading something like "4.250" as 4.
    """
    raw = field(payload, key)
    if raw is None or not raw.isdigit():
        return default
    return int(raw)


# --- Self-test --------------------------------------------------------------
# Run `python protocol.py`. Checks this file against the C++ header rather than
# against itself, because self-consistency is exactly what a drifted codec has.

if __name__ == "__main__":
    import pathlib
    import re
    import sys

    failures = []

    def check(label, condition):
        print(f"  {'ok  ' if condition else 'FAIL'}  {label}")
        if not condition:
            failures.append(label)

    print("\n=== protocol.py self-test ===\n")

    # --- The check value both sides pin ---
    check("CRC-8/ATM gives 0xF4 for '123456789'", crc8(b"123456789") == 0xF4)

    # --- The limits still agree with frame.h ---
    header = (pathlib.Path(__file__).resolve().parents[2] / "rp2040-software" /
              "src" / "peripherals" / "pi_link" / "frame.h")
    if not header.exists():
        check(f"frame.h found at {header}", False)
    else:
        text = header.read_text(encoding="utf-8", errors="replace")
        for name, ours in (("MAX_FRAME_LEN", MAX_FRAME_LEN),
                           ("MAX_TYPE_LEN", MAX_TYPE_LEN),
                           ("MAX_PAYLOAD_LEN", MAX_PAYLOAD_LEN)):
            match = re.search(rf"constexpr\s+size_t\s+{name}\s*=\s*(\d+)\s*;", text)
            if match is None:
                check(f"{name} found in frame.h", False)
            else:
                theirs = int(match.group(1))
                check(f"{name} is {ours} on both sides"
                      + ("" if theirs == ours else f" (frame.h says {theirs})"),
                      theirs == ours)

    # --- Golden frames, pinned byte for byte INCLUDING the checksum ---
    #
    # The same three literals appear in rp2040-software/tests/host/
    # test_frame.cpp. That is the point: these are independent implementations
    # in different languages, and pinning both to the same external constant is
    # the only check a drifted pair of codecs would actually fail. Each one
    # checked against itself would pass happily while the link stayed dead.
    #
    # If one of these fails, do NOT update the literal to match the new output —
    # the literal is the specification. Find out which side changed.
    for prefix, type_, payload, ms, expected in (
        ("EVT", "DOOR", "state=open", 12345,
         "EVT 12345 DOOR 10 state=open *06\n"),
        ("CMD", "SCAN", "", 7,
         "CMD 7 SCAN 0 *05\n"),
        ("RSP", "SQUARE_CANCELLED", "id=99 ok=1", 4242,
         "RSP 4242 SQUARE_CANCELLED 10 id=99 ok=1 *8F\n"),
    ):
        try:
            got = build(prefix, type_, payload, ms=ms).decode()
        except ProtocolError as exc:
            # A limit that has drifted too small refuses the frame outright.
            # Reported as a failure like any other so the run still reaches its
            # summary rather than dying on the first golden frame.
            got = f"<refused: {exc}>"
        check(f"golden frame {type_} matches the C++ codec byte for byte",
              got == expected)
        if got != expected:
            print(f"        expected {expected!r}\n        got      {got!r}")

    # --- Every message type in the protocol ---
    cases = [
        ("EVT", "BOOT", "fw=0.2 reset=power"),
        ("EVT", "HB", ""),
        ("EVT", "DOOR", "state=closed"),
        ("EVT", "TEMP", "rom=28FF1234 c=4.250"),
        ("EVT", "RFID", "uid=DEADBEEF granted=1"),
        ("EVT", "TXN_START", "id=99 items=coke:2,fanta:1 owed=600"),
        ("EVT", "TXN_METHOD", "id=99 method=cash"),
        ("EVT", "COIN", "id=99 denom=200 delta_g=6.60"),
        ("EVT", "COIN_REJECT", "delta_g=4.00"),
        ("EVT", "TXN_END", "id=99 outcome=paid paid=600"),
        ("EVT", "HEALTH", "die_c=31 box_g=250 faults=0"),
        ("CMD", "SCAN", ""),
        ("CMD", "SQUARE_LINK", "cents=200 id=99"),
        ("EVT", "INV", "coke=5 sprite=4 fanta=5 pasito=5 conf=98"),
        ("RSP", "SQUARE_URL", "id=99 url=https://sq.co/abc123"),
        ("EVT", "SQUARE_PAID", "id=99 order=ORD42"),
        ("RSP", "SQUARE_ERR", "id=99 reason=timeout"),
        ("CMD", "SQUARE_CANCEL", "id=99"),
        ("RSP", "SQUARE_CANCELLED", "id=99 ok=1"),
        ("EVT", "SQUARE_LATE_PAID", "id=99 order=ORD42 cents=600"),
    ]
    bad = []
    for prefix, type_, payload in cases:
        # Caught rather than allowed to propagate, so one type breaking still
        # lets the remaining checks and the summary run. A limit that has
        # drifted too small shows up here as a refusal, and the exception
        # message names the offending type and both limits.
        try:
            wire = build(prefix, type_, payload, ms=4242).decode().rstrip()
        except ProtocolError as exc:
            bad.append(f"{type_}: {exc}")
            continue
        got = parse(wire)
        if got != Frame(prefix, 4242, type_, payload):
            bad.append(f"{type_}: {got}")
    check(f"all {len(cases)} message types round trip", not bad)
    for b in bad:
        print(f"        {b}")

    # --- The real Square URL, which is what the limits were sized for ---
    real = build("RSP", "SQUARE_URL",
                 "id=4294967295 url=https://sandbox.square.link/u/uzx6CElw")
    check(f"a real Square URL frame fits ({len(real)} of {MAX_FRAME_LEN} bytes)",
          len(real) <= MAX_FRAME_LEN)

    # --- Refusals, not truncation ---
    for label, args in (("an over-long type", ("EVT", "T" * MAX_TYPE_LEN, "x=1")),
                        ("an over-long payload", ("EVT", "X", "y" * MAX_PAYLOAD_LEN)),
                        ("an unknown prefix", ("LOG", "X", "y=1"))):
        try:
            build(*args)
            check(f"{label} is refused", False)
        except ProtocolError:
            check(f"{label} is refused", True)

    # --- Anything that is not a frame ---
    for label, line in (
        ("a log line is not a frame",
         "[12.345 Information]: switches: door starts CLOSED"),
        ("a corrupted payload fails the CRC", "EVT 1 DOOR 10 state=Xpen *3F"),
        ("a LEN that disagrees is rejected", "EVT 1 DOOR 99 state=open *00"),
        ("a missing checksum is rejected", "EVT 1 DOOR 10 state=open"),
        ("a non-hex checksum is rejected", "EVT 1 DOOR 10 state=open *ZZ"),
        ("a truncated frame is rejected", "EVT 1"),
        ("an empty line is rejected", ""),
    ):
        check(label, parse(line) is None)

    # --- Payloads with spaces, and field extraction ---
    f = parse(build("EVT", "X", "a=1 b=2 c=3 d=4", ms=1).decode().rstrip())
    check("a payload full of spaces survives", f and f.payload == "a=1 b=2 c=3 d=4")
    check("field() reads a value", field("coke=5 sprite=4", "sprite") == "4")
    check("field() reports an absent key", field("coke=5", "sprite") is None)
    check("field() does not match the tail of a longer key",
          field("order_id=42", "id") is None)
    check("u32() reads a number", u32("cents=600 id=9", "cents") == 600)
    check("u32() refuses a non-number", u32("c=4.250", "c") is None)

    # --- Timestamp boundaries ---
    check("ms=0 and ms=2^32-1 both survive",
          parse(build("EVT", "HB", "", ms=0).decode().rstrip()).ms == 0 and
          parse(build("EVT", "HB", "", ms=2**32 - 1).decode().rstrip()).ms
          == 2**32 - 1)

    print("\n" + "=" * 50)
    if failures:
        print(f"{len(failures)} FAILED:")
        for item in failures:
            print(f"  - {item}")
    else:
        print("ALL CHECKS PASSED")
    print("=" * 50)
    sys.exit(1 if failures else 0)
