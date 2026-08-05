"""Frames in, rows out. The only place the protocol is interpreted.

Knows what every message *means*, and no SQL: it calls named methods on `Store`.
Knows nothing about transports either. So the handlers below run identically
against a real board and a simulated one, which is the whole reason the
simulation happens at the wire rather than at the database
(dashboard-plan.md section 0).

STAGE D1 HANDLES: BOOT, HB, and recording the raw stream.
Anything else is counted as unhandled and left for the stage that owns it — so
`stats` naming a type is "not wired up yet", not "dropped on the floor".
"""

import logging
import time

import protocol  # tools/protocol.py; see the sys.path note in __init__.py

from . import config

log = logging.getLogger("fridged.ingest")


class Ingest:
    """Turns link events into database rows."""

    def __init__(self, store, link):
        self.store = store
        self.link = link

        #: Which `boot` row everything currently belongs to. None until the first
        #: frame arrives — we may have been started before the board, or after.
        self.boot_id = None

        #: The board's `ms` on the previous frame, for reset detection.
        self.last_ms = None

        #: Message types seen but not handled by any stage yet. Deliberately
        #: visible: a type sitting in here is a to-do with a count attached.
        self.unhandled = {}

        self._next_self_metric = 0.0

    # --- Entry point --------------------------------------------------------

    def handle(self, event):
        self._record_raw(event)

        if event.kind == "bad":
            # repr(), because a corrupted frame very often contains control
            # characters — that is what corruption looks like — and writing them
            # raw to a terminal makes the log unreadable and unsearchable. The
            # exact bytes are in `raw_line` where they can be examined properly.
            log.warning("bad frame: %r", event.line)
            return
        if event.kind == "log":
            # The board's own logging, sharing this stream by design (decision
            # D1). Kept in raw_line, echoed at debug level, otherwise ignored.
            log.debug("[board] %s", event.line)
            return

        frame = event.frame
        self._track_boot(frame, event.ts)

        handler = getattr(self, f"_on_{frame.type.lower()}", None)
        if handler is None:
            self.unhandled[frame.type] = self.unhandled.get(frame.type, 0) + 1
            return
        handler(frame, event.ts)

    def _record_raw(self, event):
        keep = config.RAW_LINE_KEEP
        if keep == "bad" and event.kind != "bad":
            return
        if keep == "nonframe" and event.kind == "frame":
            return
        self.store.raw_line(event.ts, event.line, reject_reason=event.reason)

    # --- Boot tracking ------------------------------------------------------

    def _track_boot(self, frame, ts):
        """Keep `self.boot_id` pointing at the right `boot` row.

        Three ways a boot row gets opened, and all three happen in practice:

        - an `EVT BOOT` frame — the normal case;
        - `resumed`, when this service starts while the board is already running,
          so no BOOT was ever seen. Without this every subsequent row would have
          a NULL boot_id and transactions could not be keyed at all;
        - `ms_rollback`, when the board resets and the BOOT frame is lost — over
          a cable being unplugged, say. The counter restarting is the signal
          (dashboard.md section 6), and catching it is what stops the new boot's
          transaction ids colliding with the old boot's.
        """
        if frame.type == "BOOT":
            fw = protocol.field(frame.payload, "fw")
            self.boot_id = self.store.open_boot(ts, fw, "boot_frame")
            self.last_ms = frame.ms
            log.info("board booted: fw=%s, boot_id=%d", fw, self.boot_id)
            return

        if self.boot_id is None:
            self.boot_id = self.store.open_boot(ts, None, "resumed")
            log.info("joined a board that was already running, boot_id=%d",
                     self.boot_id)
        elif (self.last_ms is not None and
              frame.ms + config.MS_ROLLBACK_SLACK < self.last_ms):
            self.boot_id = self.store.open_boot(ts, None, "ms_rollback")
            log.warning("board ms went backwards (%d -> %d): it reset and the "
                        "BOOT frame was lost. boot_id=%d",
                        self.last_ms, frame.ms, self.boot_id)

        self.last_ms = frame.ms

    # --- Handlers -----------------------------------------------------------
    #
    # One `_on_<type>` method per message type, found by name. Adding a message
    # is adding a method; there is no dispatch table to forget to update.

    def _on_boot(self, frame, ts):
        # The row was already opened by _track_boot, which has to run before
        # dispatch so that every other handler has a boot_id to attach to. This
        # method exists so BOOT is not counted as unhandled.
        pass

    def _on_hb(self, frame, ts):
        # Liveness only. The arrival itself is the whole message, and `Link`
        # has already stamped `last_frame_ts` from it.
        pass

    def _on_temp(self, frame, ts):
        """`EVT TEMP rom=<16 hex> c=<celsius>`.

        Stored under the ROM code, never under a zone name — see the
        `temperature` view in store.py for why that distinction matters. The
        board deliberately knows nothing about zones (dashboard.md section 4.3):
        it reports which physical sensor said what, and where that sensor is
        installed is a fact about maintenance, resolved here.
        """
        rom = protocol.field(frame.payload, "rom")
        raw = protocol.field(frame.payload, "c")
        if rom is None or raw is None:
            log.warning("TEMP frame missing rom= or c=: %s", frame.payload)
            return
        try:
            celsius = float(raw)
        except ValueError:
            log.warning("TEMP frame has an unreadable temperature: %r", raw)
            return

        # The DS18B20's 85 C power-on default — the classic "the conversion never
        # actually happened" reading — is already filtered on the board
        # (temperature_task.cpp:183), so it is deliberately NOT re-checked here.
        # Two places deciding what counts as a valid reading is two places to
        # disagree. This bound only catches a frame that parsed but cannot be a
        # temperature at all.
        if not (-55.0 <= celsius <= 125.0):
            log.warning("TEMP %s out of the sensor's range: %.2f C", rom, celsius)
            return

        # Auto-insert, so a sensor nobody has named yet still appears on the
        # dashboard showing a live temperature, as an unmapped row. That is the
        # entire discovery workflow: fit a sensor, see it arrive, name it.
        self.store.note_sensor(rom, ts)
        self.store.measurement(ts, f"temp.rom.{rom}", celsius)

    def _on_door(self, frame, ts):
        """`EVT DOOR state=open|closed`.

        The door is on the critical path for the whole transaction flow, not
        just a graph annotation (dashboard.md section 4.2) — it is what tells
        the camera when the scene is stable enough to count. Here it is only
        recorded; stage D6 uses it to trigger a scan.
        """
        state = protocol.field(frame.payload, "state")
        if state == "open":
            self.store.door_opened(ts)
        elif state == "closed":
            duration = self.store.door_closed(ts)
            if duration is None:
                # Started while the fridge was already open. Normal on a restart
                # and not an error, but it means one open has no duration.
                log.info("door closed with no open on record "
                         "(fridged probably started while it was open)")
            elif duration > 120.0:
                log.warning("door was open for %.0f s", duration)
        else:
            log.warning("DOOR frame with an unrecognised state: %r", state)

    def _on_health(self, frame, ts):
        """`EVT HEALTH die_c= box_g= faults=`, every 30 s.

        Three unrelated things share one frame because the board assembles it in
        `main.cpp` from three tasks that should not have to know about each
        other. Unpacked into three metrics here, because on the dashboard they
        belong in different places: a temperature, a mass, and a fault count.
        """
        for key, metric in (("die_c", "temp.rp2040_die"),
                            ("box_g", "coinbox.mass_g"),
                            ("faults", "health.faults")):
            raw = protocol.field(frame.payload, key)
            if raw is None:
                continue
            try:
                self.store.measurement(ts, metric, float(raw))
            except ValueError:
                log.warning("HEALTH %s is not a number: %r", key, raw)

    # --- Our own telemetry --------------------------------------------------

    def tick(self):
        """Record what the service knows about itself. Called every pass; acts
        on a timer.

        These go into `measurement` alongside the fridge's own metrics rather
        than into a table of their own — dashboard.md section 5.1 exists so that
        adding a metric is never a schema change, and the health row is exactly
        the case it was meant for.
        """
        now = time.monotonic()
        if now < self._next_self_metric:
            return
        self._next_self_metric = now + config.SELF_METRIC_INTERVAL_S

        ts = time.time()
        age = self.link.age_s
        if age is not None:
            self.store.measurement(ts, "link.age_s", age)
        self.store.measurement(ts, "link.frames_bad", self.link.bad)

        soc = config.soc_temperature_c()
        if soc is not None:
            self.store.measurement(ts, "temp.pi_soc", soc)
