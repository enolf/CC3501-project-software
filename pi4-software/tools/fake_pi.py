#!/usr/bin/env python3
"""Stand in for the Raspberry Pi, from a laptop, over the USB serial port.

Speaks the frame protocol in plan.md section 2 so the RP2040's real serial
backend can be exercised before the Pi side exists — and, just as usefully,
before the camera works.

The codec itself is in protocol.py, next to this file and shared with the real
bridge. `python protocol.py` self-tests it against the C++ header.

    pip install pyserial
    python fake_pi.py COM7            (Windows)
    python fake_pi.py /dev/ttyACM0    (Linux)

What it does automatically:

    CMD SCAN           ->  EVT INV with the current simulated shelf
    CMD SQUARE_LINK    ->  RSP SQUARE_URL after a short delay
    CMD SQUARE_CANCEL  ->  RSP SQUARE_CANCELLED, and any unsent link is dropped

Type these and press Enter to drive it:

    1 2 3 4   take a Coke / Fanta / Mountain Dew / Solo off the shelf
    Q W E R   put one back (the shifted key above the one that took it)
    r         restock
    i         show the shelf
    p         tell the board Square reports the payment received
    e         tell the board Square reports a failure
    l         tell the board money arrived AFTER the link was cancelled
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

# The codec lives in protocol.py, imported by this tool and by the real bridge.
# It used to be copied inline here; see the note at the top of protocol.py for
# why a second copy was a bug waiting to happen rather than a convenience.
from protocol import build, parse, field, PREFIXES, ProtocolError

try:
    import serial  # type: ignore
except ImportError:
    sys.exit("pyserial is not installed.  Run:  pip install pyserial")


# --- Simulated world --------------------------------------------------------

#: Wire keys, matching catalogue::wire_key() on the board. Order matters: it is
#: what the 1-4 and Q-R keys index into.
DRINKS = ["coke", "fanta", "mtndew", "solo"]
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
        self.last_txn = "0"

    def schedule(self, delay, data: bytes):
        self.seq += 1
        self.pending.put((time.monotonic() + delay, self.seq, data))

    def drop_pending(self, type_: bytes) -> int:
        """Remove scheduled-but-unsent frames of one type. Returns how many.

        A PriorityQueue has no way to remove a specific item, so the whole
        backlog is drained and the survivors put back. It is never more than a
        handful of frames, and doing it properly here keeps the fake honest
        about a race the real bridge genuinely has.
        """
        kept = []
        while not self.pending.empty():
            kept.append(self.pending.get())

        removed = 0
        for item in kept:
            if f" {type_.decode()} ".encode() in item[2]:
                removed += 1
            else:
                self.pending.put(item)
        return removed

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
            self.last_txn = txn
            self.schedule(SQUARE_DELAY_S,
                          build("RSP", "SQUARE_URL", f"id={txn} url={SQUARE_URL}"))
            print("      (type 'p' to confirm payment, 'e' to fail it)")
        elif type_ == "SQUARE_CANCEL":
            txn = field(payload, "id", "0")
            self.last_txn = txn

            # Drop a link reply that has not gone out yet. The real bridge has
            # the same job in a harder form: the board can cancel a checkout the
            # Square API has not finished creating, and that pending link must
            # still end up cancelled rather than left live.
            dropped = self.drop_pending(b"SQUARE_URL")
            if dropped:
                print(f"      dropped {dropped} unsent link reply "
                      f"- the board gave up before it arrived")

            self.schedule(0.3, build("RSP", "SQUARE_CANCELLED", f"id={txn} ok=1"))
            print("      (type 'l' to simulate the money arriving anyway)")

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
        elif text in ("Q", "W", "E", "R"):
            # Put one back. Handled HERE and not forwarded as a debug keypress,
            # because with the serial backend compiled the board has no shelf of
            # its own — this process is the camera, so this process owns the
            # drinks. It is also the only way to exercise the board's
            # change-your-mind path against real firmware.
            drink = DRINKS["QWER".index(text)]
            if self.shelf[drink] >= FULL_SHELF:
                print(f"      the shelf is already full of {drink}")
            else:
                self.shelf[drink] += 1
                print(f"      put a {drink} back (silently - no scan yet)")
        elif text == "r":
            self.shelf = {d: FULL_SHELF for d in DRINKS}
            print("      shelf restocked")
        elif text == "i":
            print("      shelf:", self.inventory_payload())
        elif text == "p":
            self.send_now(build("EVT", "SQUARE_PAID", "order=SIM1"))
        elif text == "e":
            self.send_now(build("RSP", "SQUARE_ERR", "reason=simulated"))
        elif text == "l":
            # The race the design cannot close: the customer's payment and our
            # cancellation cross in flight. The board's only correct response is
            # to record that a refund is owed, and this is how that path gets
            # exercised on demand instead of once in a hundred runs.
            self.send_now(build("EVT", "SQUARE_LATE_PAID",
                                f"id={self.last_txn} order=SIM1 cents=600"))
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
            print("      ? try 1-4, r, i, p, e, l, stats, q, or a single debug key")


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
                try:
                    pi.handle(*decoded)
                except ProtocolError as exc:
                    # A reply we cannot legally build — almost always because a
                    # value taken from the board's payload is longer than
                    # expected. Reported rather than raised: this runs on the
                    # reader thread, and an exception here would kill the link
                    # silently and leave the tool apparently still running.
                    print(f"  !!! CANNOT REPLY: {exc}")
            elif line[:3] in PREFIXES:
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
            except ProtocolError as exc:
                print(f"  !!! CANNOT SEND: {exc}")
    except KeyboardInterrupt:
        pass
    finally:
        pi.running = False
        port.close()
        print(f"\nin={pi.frames_in} out={pi.frames_out} bad={pi.bad}")


if __name__ == "__main__":
    main()
