"""The serial link to the RP2040: bytes in, frames out, heartbeat maintained.

Knows about framing and about the transport. Knows nothing about what any
message *means* — that is `ingest.py`. The split matters because it is what lets
the whole ingest path be exercised against a simulated board: `Link` cannot tell
a real port from a fake one, because the difference does not exist on its side of
the interface.

The codec itself is `tools/protocol.py`, shared with `fake_pi.py` and pinned to
the C++ implementation in `frame.h` by three golden frames. Nothing here
re-implements it.
"""

import time
from collections import namedtuple

import protocol  # tools/protocol.py; see the sys.path note in __init__.py

from . import config

#: One thing that arrived on the link.
#:
#:   kind == "frame"  a valid frame; `frame` is a protocol.Frame
#:   kind == "log"    a line that was never a frame — the board's own debug
#:                    output, which shares this stream by design (decision D1)
#:   kind == "bad"    frame-shaped, but it failed validation
#:
#: `ts` is wall-clock at arrival. The board's own `ms` is inside the frame and is
#: used for ordering and reset detection only; the Pi owns wall time because the
#: RP2040 has no RTC (dashboard.md section 6).
LinkEvent = namedtuple("LinkEvent", "kind ts line frame reason")

#: Coarse on purpose. `protocol.parse()` returns None for every kind of failure
#: and is not going to grow a reason code — it is the shared codec, and widening
#: its contract to satisfy one caller's logging is how a shared thing stops being
#: shared. The offending bytes are stored next to this string in `raw_line`, and
#: the bytes are what actually gets debugged.
REJECT_REASON = "frame-shaped, failed validation (CRC, LEN or syntax)"


class Link:
    """Services one transport: drains it, decodes it, and keeps it alive."""

    def __init__(self, transport, name="board"):
        self.transport = transport
        self.name = name
        self._buffer = b""
        self._next_heartbeat = 0.0

        # Counters. `bad` is the one to watch: it should be zero on a healthy
        # link, and a rising count is the first sign of interference on the
        # shared stream.
        self.frames_in = 0
        self.frames_out = 0
        self.bad = 0
        self.ignored = 0

        #: Wall-clock time the last valid frame arrived, or None if none has.
        self.last_frame_ts = None

    # --- Receiving ----------------------------------------------------------

    def poll(self):
        """Drain what is waiting, and send the heartbeat if it is due.

        Returns a list rather than a generator, deliberately: the heartbeat is
        sent from in here, and a generator the caller forgot to exhaust would
        silently stop keeping the link alive.
        """
        self._heartbeat_if_due()

        try:
            chunk = self.transport.read(config.MAX_READ_PER_POLL)
        except Exception:
            # A disconnected USB device raises rather than returning nothing.
            # Treated as "no data": the loop keeps running, `age_s` climbs, and
            # the health panel shows the link down — which is the truth, and is
            # more useful than a traceback that stops the service.
            return []

        if not chunk:
            return []

        self._buffer += chunk
        events = []
        while b"\n" in self._buffer:
            raw, _, self._buffer = self._buffer.partition(b"\n")
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if line:
                events.append(self._classify(line))

        # A sender that never emits a newline would otherwise grow this without
        # limit. The board drops its own over-long lines at 128 bytes; being
        # somewhat more generous here means a board bug shows up as rejected
        # frames rather than as a Pi that quietly eats memory.
        if len(self._buffer) > config.MAX_READ_PER_POLL:
            self._buffer = b""

        return events

    def _classify(self, line):
        ts = time.time()
        decoded = protocol.parse(line)
        if decoded is not None:
            self.frames_in += 1
            self.last_frame_ts = ts
            return LinkEvent("frame", ts, line, decoded, None)
        if line[:3] in protocol.PREFIXES:
            self.bad += 1
            return LinkEvent("bad", ts, line, None, REJECT_REASON)
        self.ignored += 1
        return LinkEvent("log", ts, line, None, None)

    # --- Sending ------------------------------------------------------------

    def send(self, prefix, type_, payload=""):
        """Build and write one frame. Returns False if it could not be sent.

        `build()` raises rather than truncating, because a truncated frame that
        still passed its own CRC would be acted on at the far end. Caught here so
        one bad payload cannot take the service down: the failure is reported to
        the caller and the link stays up.
        """
        try:
            wire = protocol.build(prefix, type_, payload)
        except protocol.ProtocolError:
            return False
        try:
            self.transport.write(wire)
            self.transport.flush()
        except Exception:
            return False
        self.frames_out += 1
        return True

    def _heartbeat_if_due(self):
        now = time.monotonic()
        if now < self._next_heartbeat:
            return
        self._next_heartbeat = now + config.HEARTBEAT_INTERVAL_S
        # Not optional. The board judges the link on how recently ANY frame
        # arrived and takes itself out of service after 30 s of silence — and the
        # Pi has nothing to say while nobody is buying anything. Without this a
        # healthy link looks dead simply because the fridge was quiet.
        self.send("EVT", "HB")

    # --- Health -------------------------------------------------------------

    @property
    def age_s(self):
        """Seconds since the last valid frame, or None if none has ever arrived."""
        if self.last_frame_ts is None:
            return None
        return time.time() - self.last_frame_ts

    @property
    def healthy(self):
        age = self.age_s
        return age is not None and age < config.LINK_TIMEOUT_S

    def close(self):
        try:
            self.transport.close()
        except Exception:
            pass
