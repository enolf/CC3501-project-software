#!/usr/bin/env python3
"""Stand in for the Raspberry Pi, from a laptop, over the USB serial port.

Speaks the frame protocol in plan.md section 2 so the RP2040's real serial
backend can be exercised before the Pi side exists — and, just as usefully,
before the camera works.

    pip install pyserial
    python fake_pi.py COM7            (Windows)
    python fake_pi.py /dev/ttyACM0    (Linux)

What it does automatically:

    CMD SCAN         ->  EVT INV with the current simulated shelf
    CMD SQUARE_LINK  ->  RSP SQUARE_URL after a short delay

Type these and press Enter to drive it:

    1 2 3 4   take a Coke / Sprite / Fanta / Pasito off the shelf
    r         restock
    i         show the shelf
    p         tell the board Square reports the payment received
    e         tell the board Square reports a failure
    o c b n   forward a single-character debug key to the board
    stats     frames in, frames out, bad frames
    q         quit

It also prints everything the board says. Lines that are not frames — the
board's own log output, sharing the same stream — are shown dimmed, which is
exactly the behaviour the real bridge needs.
"""

import sys
import threading
import time
import queue

try:
    import serial  # type: ignore
except ImportError:
    sys.exit("pyserial is not installed.  Run:  pip install pyserial")


# --- Protocol ---------------------------------------------------------------

DRINKS = ["coke", "sprite", "fanta", "pasito"]
FULL_SHELF = 5

# How long the "camera" takes to answer a scan. Deliberately not instant: an
# immediate reply would hide ordering bugs in the board's state machine, which
# are the ones worth finding.
SCAN_DELAY_S = 0.6
SQUARE_DELAY_S = 1.0

# The board decides the link is dead after 30 s without a valid frame, so this
# side must speak up periodically. Nothing needs saying while the fridge is
# idle, which is exactly why silence cannot be allowed to mean "broken".
# The real bridge must do this too.
HEARTBEAT_S = 10.0

SQUARE_URL = "https://example.com/pay/SIMULATED"


def crc8(data: bytes) -> int:
    """CRC-8/ATM: polynomial 0x07, init 0x00, no reflection, no final xor."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build(prefix: str, type_: str, payload: str = "") -> bytes:
    """Render one frame, including the trailing newline."""
    ms = int(time.monotonic() * 1000) & 0xFFFFFFFF
    if payload:
        head = f"{prefix} {ms} {type_} {len(payload)} {payload}"
    else:
        head = f"{prefix} {ms} {type_} 0"
    return f"{head} *{crc8(head.encode()):02X}\n".encode()


def parse(line: str):
    """Decode a frame. Returns (prefix, ms, type, payload) or None.

    None means "not a frame" — which is the normal fate of the board's log
    output on this shared stream, not an error.
    """
    if len(line) < 4 or line[:3] not in ("EVT", "CMD", "RSP") or line[3] != " ":
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

    body = line[:star]
    parts = body.split(" ", 4)
    if len(parts) < 4:
        return None

    prefix, ms_text, type_, len_text = parts[0], parts[1], parts[2], parts[3]
    try:
        ms, payload_len = int(ms_text), int(len_text)
    except ValueError:
        return None

    payload = parts[4] if len(parts) > 4 else ""
    if len(payload) != payload_len:
        return None

    return prefix, ms, type_, payload


def field(payload: str, key: str, default=None):
    for item in payload.split(" "):
        name, _, value = item.partition("=")
        if name == key:
            return value
    return default


# --- State ------------------------------------------------------------------

class FakePi:
    def __init__(self, port):
        self.port = port
        self.shelf = {d: FULL_SHELF for d in DRINKS}
        self.pending = queue.PriorityQueue()   # (due_time, frame_bytes)
        self.seq = 0
        self.frames_in = 0
        self.frames_out = 0
        self.bad = 0
        self.ignored = 0
        self.running = True
        self.next_heartbeat = 0.0

    def schedule(self, delay, data: bytes):
        self.seq += 1
        self.pending.put((time.monotonic() + delay, self.seq, data))

    def send_now(self, data: bytes):
        self.port.write(data)
        self.port.flush()
        self.frames_out += 1
        print(f"  --> {data.decode().rstrip()}")

    def flush_due(self):
        while not self.pending.empty():
            due, seq, data = self.pending.queue[0]
            if due > time.monotonic():
                break
            self.pending.get()
            self.send_now(data)

        # Keep the link alive. Quietly: printing every heartbeat would bury the
        # traffic that matters.
        if time.monotonic() >= self.next_heartbeat:
            self.next_heartbeat = time.monotonic() + HEARTBEAT_S
            self.port.write(build("EVT", "HB"))
            self.port.flush()
            self.frames_out += 1

    def inventory_payload(self):
        return " ".join(f"{d}={self.shelf[d]}" for d in DRINKS) + " conf=100"

    def handle(self, prefix, ms, type_, payload):
        self.frames_in += 1
        print(f"  <-- {prefix} {type_} {payload}")

        if type_ == "SCAN":
            self.schedule(SCAN_DELAY_S,
                          build("EVT", "INV", self.inventory_payload()))
        elif type_ == "SQUARE_LINK":
            txn = field(payload, "id", "0")
            self.schedule(SQUARE_DELAY_S,
                          build("RSP", "SQUARE_URL", f"id={txn} url={SQUARE_URL}"))
            print("      (type 'p' to confirm payment, 'e' to fail it)")

    def command(self, text):
        text = text.strip()
        if not text:
            return

        if text == "q":
            self.running = False
        elif text in ("1", "2", "3", "4"):
            drink = DRINKS[int(text) - 1]
            if self.shelf[drink] == 0:
                print(f"      no {drink} left")
            else:
                self.shelf[drink] -= 1
                print(f"      took a {drink} (silently - no scan yet)")
        elif text == "r":
            self.shelf = {d: FULL_SHELF for d in DRINKS}
            print("      shelf restocked")
        elif text == "i":
            print("      shelf:", self.inventory_payload())
        elif text == "p":
            self.send_now(build("EVT", "SQUARE_PAID", "order=SIM1"))
        elif text == "e":
            self.send_now(build("RSP", "SQUARE_ERR", "reason=simulated"))
        elif text == "stats":
            print(f"      in={self.frames_in} out={self.frames_out} "
                  f"bad={self.bad} ignored={self.ignored}")
        elif len(text) == 1:
            # Single character: forwarded raw as a debug keypress. The board
            # treats a one-character line as a key rather than as protocol.
            self.port.write(f"{text}\n".encode())
            self.port.flush()
            print(f"      sent debug key '{text}'")
        else:
            print("      ? try 1-4, r, i, p, e, stats, q, or a single debug key")


def reader(pi: FakePi):
    """Consume the board's output. Runs on its own thread."""
    buffer = b""
    while pi.running:
        try:
            chunk = pi.port.read(256)
        except Exception:
            break
        if not chunk:
            continue
        buffer += chunk
        while b"\n" in buffer:
            raw, _, buffer = buffer.partition(b"\n")
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if not line:
                continue

            decoded = parse(line)
            if decoded:
                pi.handle(*decoded)
            elif line[:3] in ("EVT", "CMD", "RSP"):
                # Frame-shaped but failed validation. This is the number that
                # matters: on a healthy link it stays at zero.
                pi.bad += 1
                print(f"  !!! BAD FRAME: {line}")
            else:
                pi.ignored += 1
                print(f"      [board] {line}")


def keyboard(pi: FakePi, commands: queue.Queue):
    """Read typed lines. Runs on its own thread.

    A thread rather than a non-blocking read: select() does not work on Windows
    console handles, and the msvcrt alternative reports a key the instant it is
    pressed, so readline() would then block mid-word — stalling the scheduler
    and delaying the very frames whose timing this tool exists to imitate.
    """
    while pi.running:
        try:
            line = sys.stdin.readline()
        except Exception:
            break
        if not line:
            break
        commands.put(line)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    port_name = sys.argv[1]
    try:
        port = serial.Serial(port_name, 115200, timeout=0.1)
    except Exception as exc:
        sys.exit(f"could not open {port_name}: {exc}")

    pi = FakePi(port)
    print(f"connected to {port_name}.  1-4 take a drink, r restock, "
          f"p pay, q quit.\n")

    commands: queue.Queue = queue.Queue()
    threading.Thread(target=reader, args=(pi,), daemon=True).start()
    threading.Thread(target=keyboard, args=(pi, commands), daemon=True).start()

    try:
        while pi.running:
            # Delayed frames are released here, so scan and payment-link replies
            # keep their timing no matter what is being typed.
            pi.flush_due()
            try:
                pi.command(commands.get(timeout=0.02))
            except queue.Empty:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        pi.running = False
        port.close()
        print(f"\nin={pi.frames_in} out={pi.frames_out} bad={pi.bad}")


if __name__ == "__main__":
    main()
