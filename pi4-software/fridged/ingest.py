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
from .camera import PENDING

log = logging.getLogger("fridged.ingest")


class Ingest:
    """Turns link events into database rows."""

    def __init__(self, store, link, camera=None, payments=None,
                 clock=time.monotonic):
        self.store = store
        self.link = link
        #: Injectable so the deferred-scan deadline can be driven by a test
        #: rather than by waiting six real seconds for it.
        self._clock = clock
        #: Answers CMD SCAN. None means stock is not wired up, which
        #: is a warning rather than a failure - every other metric
        #: still works without a camera.
        self.camera = camera
        #: Answers CMD SQUARE_LINK. None means card payments are not
        #: configured, which is answered with SQUARE_ERR rather than
        #: silence - the board would otherwise sit on 'Contacting payment
        #: service' until its own timeout.
        self.payments = payments

        #: Which `boot` row everything currently belongs to. None until the first
        #: frame arrives — we may have been started before the board, or after.
        self.boot_id = None

        #: The board's `ms` on the previous frame, for reset detection.
        self.last_ms = None

        #: Message types seen but not handled by any stage yet. Deliberately
        #: visible: a type sitting in here is a to-do with a count attached.
        self.unhandled = {}

        #: The transaction coins are currently being attributed to. Set by
        #: TXN_START, cleared by TXN_END. Needed because `EVT COIN` carries
        #: no id of its own.
        self.open_txn_id = None

        #: A `CMD SCAN` that has arrived and not yet been answered, as
        #: `(frame_ts, deadline, trigger)`. See `_on_scan`.
        self.scan_owed = None

        #: Whether the door is open, or None if no DOOR frame has arrived yet.
        #: Read by `scan_trigger()` to tell a baseline scan from a recount —
        #: `EVT DOOR` always precedes the `CMD SCAN` it belongs to, because the
        #: board runs `notify_door()` ahead of the switch that calls
        #: `request_scan()`.
        self.door_open = None

        #: An approved card that has tapped but whose owner has not opened the
        #: door yet, as `(uid, ts)`. See `_on_rfid` for why this is a separate
        #: thing from `active_member`.
        self.pending_tap = None

        #: The card the transaction in progress belongs to, or None for a sale
        #: nobody identified themselves for — which the dashboard shows as
        #: "other". Promoted from `pending_tap` when the door opens, cleared by
        #: TXN_END.
        self.active_member = None

        #: When `active_member` was last known to still be at the fridge. Any
        #: door event refreshes it; `SESSION_WINDOW_S` measures from it.
        self.active_member_ts = 0.0

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

    def _on_scan(self, frame, ts):
        """`CMD SCAN` — the board asking what is on the shelf.

        The only message that gets an answer. Replying is not optional: the
        board sits in `Recount` waiting, and gives up into a Fault screen after
        `RECOUNT_TIMEOUT_MS` if nothing arrives.

        The snapshot is written here, at the moment the answer is given, because
        this is the point at which the Pi commits to a count.
        """
        if self.camera is None:
            log.warning("CMD SCAN arrived but no camera is configured")
            return

        # THREE ANSWERS, AND THE MIDDLE ONE IS WHY THIS IS NOT A ONE-LINER.
        #
        #   a reading  send it
        #   PENDING    the picture has not settled. Hold the reply open and
        #              send it from tick() when it does.
        #   None       the camera cannot answer. Send NOTHING (decision D1):
        #              the board's Recount times out and it goes out of
        #              service, which is the honest outcome for a fridge that
        #              cannot see what it is selling. Repeating a stale count as
        #              though it were current moves money on a number nobody
        #              measured, and nothing in the data would ever show it.
        #
        # Deferring rather than blocking, because this same loop drains the
        # serial link and flushes the database — sleeping here would stall the
        # link we are trying to answer on.
        answer = self.camera.payload()

        # WHICH scan this is, captured now rather than when the answer goes out.
        # A deferred reply can be sent seconds later, by which point the door
        # may have moved again — recording the state at that point would file a
        # baseline as a recount and quietly corrupt the one column that says
        # which of the two readings a snapshot was.
        trigger = self.scan_trigger()

        if answer is PENDING:
            deadline = self._clock() + config.SCAN_ANSWER_BUDGET_S
            if self.scan_owed is None:
                log.debug("scan deferred while the picture settles")
            self.scan_owed = (ts, deadline, trigger)
            return

        self.scan_owed = None
        if answer is None:
            log.error("CMD SCAN cannot be answered; letting the board fault "
                      "rather than guessing what is on the shelf")
            return

        self._send_inventory(answer, ts, trigger)

    def scan_trigger(self):
        """Which of the two scans this is, from the door state.

        The board takes a baseline the instant the door opens and a recount
        after it shuts, and the two are not equivalent measurements: the
        baseline is latched from before anybody reached in, the recount waits
        for the picture to settle. Storing which is which is what makes them
        comparable afterwards — `stock_snapshot.trigger` has had the column
        since the schema was written and nothing had ever filled it honestly.
        """
        if self.door_open is None:
            return "untriggered"        # a scan with no door event behind it
        return "door_open" if self.door_open else "door_close"

    def _send_inventory(self, answer, ts, trigger):
        payload, counts, confidence = answer
        if not self.link.send("EVT", "INV", payload):
            log.error("could not send the INV reply: %s", payload)
            return

        self.store.stock_snapshot(ts, counts, trigger=trigger)

        # THE ONLY RECORD OF HOW MUCH THAT COUNT WAS WORTH BELIEVING.
        #
        # It went out on the wire and was thrown away. The counts themselves are
        # identical whether the shelf held still or the answer was forced out on
        # a deadline, so without this there is no way, ever, to look back at a
        # disputed charge and see whether the measurement behind it was sound.
        # Stored as a `measurement` because that table exists precisely so that
        # adding a metric is not a schema change.
        self.store.measurement(ts, "camera.confidence", float(confidence))

        if confidence < config.LOW_CONFIDENCE_THRESHOLD:
            log.warning("%s scan answered at confidence %d (below %d) - this "
                        "sale rests on a count nobody should trust",
                        trigger, confidence, config.LOW_CONFIDENCE_THRESHOLD)

    def _answer_owed_scan(self):
        """Finish a scan that was deferred. Called every service pass.

        NOT on the self-metric timer that guards the rest of `tick()`. The board
        is sitting in `Recount` with a stopwatch running; a reply that waits for
        a ten-second telemetry tick would arrive after it had already faulted.
        """
        if self.scan_owed is None or self.camera is None:
            return

        ts, deadline, trigger = self.scan_owed
        expired = self._clock() >= deadline

        answer = self.camera.payload(force=expired)
        if answer is PENDING:
            if not expired:
                return
            # force=True should have produced something. A camera that still
            # says PENDING is not honouring the contract, and holding the reply
            # open would mean holding it forever.
            log.error("the camera would not answer even when forced; "
                      "dropping the scan")
            answer = None

        self.scan_owed = None
        if answer is None:
            log.error("the deferred CMD SCAN cannot be answered; letting the "
                      "board fault rather than guessing")
            return

        if expired:
            log.warning("answering CMD SCAN on the deadline (%.0fs), before "
                        "the picture settled", config.SCAN_ANSWER_BUDGET_S)
        self._send_inventory(answer, ts, trigger)

    def _on_door(self, frame, ts):
        """`EVT DOOR state=open|closed`.

        The door is on the critical path for the whole transaction flow, not
        just a graph annotation (dashboard.md section 4.2) — it is what tells
        the camera when the scene is stable enough to count. Here it is only
        recorded; stage D6 uses it to trigger a scan.
        """
        state = protocol.field(frame.payload, "state")
        if state == "open":
            self.door_open = True
            # The moment a tap becomes an attribution. See `_on_rfid`: this
            # mirrors the board's own Greeting -> Selecting transition, so both
            # ends agree about whose purchase this is.
            self._claim_tap(ts)
            # Somebody is still at the fridge, so an attribution already running
            # is still live. This is what lets one tap cover the look-shut-look
            # -again cycle without being dropped halfway through.
            if self.active_member is not None:
                self.active_member_ts = ts
            # The camera is told BEFORE the scan arrives, and that ordering is
            # guaranteed rather than lucky: the board runs `notify_door()` in
            # the common section of `handle_event()`, ahead of the per-state
            # switch that calls `request_scan()`. Stage 4 depends on it to
            # freeze the pre-open reading.
            if self.camera is not None:
                self.camera.door_opened(ts)
            self.store.door_opened(ts)
        elif state == "closed":
            self.door_open = False
            # The shelf changes here, NOT when the door opened.
            #
            # The board takes a baseline scan the instant the door opens, and
            # that scan has to see a full shelf — the customer has not reached in
            # yet. Removing the drinks at the open instead makes the baseline
            # already reflect them, the two scans agree, and every transaction
            # silently disappears. Which is exactly what happened when this was
            # written the other way round.
            #
            # `CMD SCAN` for the recount arrives just after this, so by the time
            # the Pi answers, the shelf is right.
            #
            # One method, not two, and named for the EVENT rather than for what
            # the simulation does with it. `customer_takes()` and
            # `maybe_restock()` are statements about a world the caller
            # controls; a real camera observes a world it does not, and would
            # have had to grow no-op versions of both to fit. What every backend
            # can honestly be told is that the door shut.
            if self.camera is not None:
                self.camera.door_closed(ts)

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

    # --- Who is buying ------------------------------------------------------

    #: How long an approved tap stays valid without the door being opened.
    #: Matched to the firmware's `GREETING_TIMEOUT_MS`, because that is exactly
    #: how long the board holds the greeting before giving up and returning to
    #: Idle. Two ends disagreeing about this would attribute a sale to somebody
    #: the board had already forgotten.
    TAP_WINDOW_S = 30.0

    #: How long an attribution survives with no door activity and no
    #: transaction. Derived from the firmware's own timeout budget rather than
    #: picked round — it is the longest a customer can legitimately be standing
    #: at the terminal between opening the door and the sale being reported:
    #:
    #:     SELECT_TIMEOUT_MS   30 s   deciding, door open
    #:     RECOUNT_TIMEOUT_MS   8 s   waiting for our own scan reply
    #:     PAYMENT_TIMEOUT_MS 120 s   choosing and completing payment
    #:                        ------
    #:                        158 s, rounded up for the frames in between
    #:
    #: It matters because a card sale reports TXN_START at the END, so the gap
    #: between the tap and the only message carrying the sale can be minutes.
    #: Past this, the board has certainly given up and returned to Idle, so a
    #: still-held attribution could only ever be wrong.
    SESSION_WINDOW_S = 180.0

    def _on_rfid(self, frame, ts):
        """`EVT RFID uid= granted=` — somebody presented a card.

        WHY A TAP IS NOT IMMEDIATELY AN ATTRIBUTION
        -------------------------------------------
        Tapping in and then walking off is a real thing people do, and the board
        models it: `Greeting` returns to `Idle` after `GREETING_TIMEOUT_MS` with
        nothing bought. If a tap attributed the *next* transaction
        unconditionally, that abandoned tap would be charged to whoever opened
        the door afterwards — quietly, and with no way to tell from the data.

        So a tap only ARMS an attribution. The door opening is what commits it,
        which mirrors the firmware's own `Greeting -> Selecting` transition. A
        sale with no armed tap behind it is stored with a NULL `member_uid`, and
        that is what the dashboard reports as "other".

        Denied cards are recorded but never attributed: `granted=0` means the
        board did not recognise the card, so there is no member to attribute to.
        """
        uid = protocol.field(frame.payload, "uid")
        if not uid:
            log.warning("RFID frame with no uid: %s", frame.payload)
            return

        granted = protocol.u32(frame.payload, "granted", 0) == 1
        self.store.rfid_event(ts, uid, granted)

        if not granted:
            log.info("card %s presented and DENIED", uid)
            return

        self.pending_tap = (uid, ts)
        log.debug("card %s tapped in; armed for %.0fs", uid, self.TAP_WINDOW_S)

    def _claim_tap(self, ts):
        """Promote an armed tap to the active member. Called on door open."""
        if self.pending_tap is None:
            return
        uid, tapped_at = self.pending_tap
        self.pending_tap = None

        if ts - tapped_at > self.TAP_WINDOW_S:
            # The board has already returned to Idle and forgotten this person,
            # so attributing to them now would disagree with what the terminal
            # showed the customer.
            log.debug("tap by %s expired before the door opened", uid)
            return

        self.active_member = uid
        self.active_member_ts = ts
        label = self.store.label_for_uid(uid)
        log.info("this purchase is attributed to %s", label or f"unnamed card {uid}")

    def _member_for_txn(self, ts):
        """Who this transaction belongs to, or None for "other".

        Checked against the clock rather than trusted outright. One tap can span
        several door cycles — people open the fridge, look, shut it, and open it
        again, and the board treats that as one visit — so the attribution is
        NOT dropped at the next door open. What bounds it instead is
        `SESSION_WINDOW_S`, past which the board has certainly returned to Idle.
        """
        if self.active_member is None:
            return None
        if ts - self.active_member_ts > self.SESSION_WINDOW_S:
            log.debug("attribution for %s expired; recording as other",
                      self.active_member)
            self.active_member = None
            return None
        return self.active_member

    # --- Transactions -------------------------------------------------------

    def _on_txn_start(self, frame, ts):
        """`EVT TXN_START id= items=Coke:2,Fanta:1 owed= method=`.

        NOT a reliable "transaction begins" marker, despite the name. See
        `Store.upsert_txn` for the full shape; the short version is that this
        arrives twice for a cash sale and once, at the end, for a card sale.
        """
        txn_id = protocol.u32(frame.payload, "id")
        if txn_id is None:
            log.warning("TXN_START with no id: %s", frame.payload)
            return

        owed = protocol.u32(frame.payload, "owed")
        method = protocol.field(frame.payload, "method")
        # "none" is what the firmware reports for a method not yet chosen. Stored
        # as NULL so the payment-split panel does not grow a third slice that
        # means "we do not know".
        if method == "none":
            method = None

        self.store.upsert_txn(self.boot_id, txn_id, ts_start=ts,
                              owed_cents=owed, method=method,
                              member_uid=self._member_for_txn(ts))
        self.open_txn_id = txn_id

        items = self._parse_items(protocol.field(frame.payload, "items"))
        if items:
            total = sum(items.values())
            # The frame carries no PER-ITEM price, only the basket total, so the
            # unit price is derived. Exact while every drink costs the same,
            # which catalogue.h says they do; if prices ever diverge, this
            # becomes an average and revenue-per-drink needs the firmware to
            # send prices.
            unit = owed // total if owed and total else None
            self.store.set_txn_items(self.boot_id, txn_id, items, unit)

    def _on_txn_end(self, frame, ts):
        """`EVT TXN_END id= outcome=paid|stolen|cancelled owed= paid=`."""
        txn_id = protocol.u32(frame.payload, "id")
        if txn_id is None:
            log.warning("TXN_END with no id: %s", frame.payload)
            return

        outcome = protocol.field(frame.payload, "outcome")
        if outcome not in ("paid", "stolen", "cancelled"):
            log.warning("TXN_END with an unrecognised outcome: %r", outcome)
            outcome = None

        # `member_uid` is passed HERE as well as in TXN_START, and it is not
        # redundant: an abandoned card sale sends only TXN_END, for a
        # transaction we have never heard of (see `Store.upsert_txn`). Without
        # it, the one outcome where knowing who walked off matters most —
        # `stolen` — would be the one always recorded as "other".
        member_uid = self._member_for_txn(ts)
        self.store.upsert_txn(
            self.boot_id, txn_id, ts_end=ts, outcome=outcome,
            owed_cents=protocol.u32(frame.payload, "owed"),
            paid_cents=protocol.u32(frame.payload, "paid"),
            member_uid=member_uid)

        if outcome == "stolen":
            label = self.store.label_for_uid(member_uid) if member_uid else None
            log.info("transaction %s ended unpaid (%s)", txn_id,
                     label or "nobody identified")

        self.open_txn_id = None
        # The customer is done. A tap must not carry over into whatever the next
        # person does, and the board has already returned to Idle.
        self.active_member = None
        self.pending_tap = None

    def _on_coin(self, frame, ts):
        """`EVT COIN denom= delta_g=` — no id, so it is attributed here."""
        self.store.coin_event(
            ts, self.boot_id, self.open_txn_id,
            protocol.u32(frame.payload, "denom"),
            self._float(frame.payload, "delta_g"), classified=True)

    def _on_coin_reject(self, frame, ts):
        """`EVT COIN_REJECT delta_g=` — a slug, a washer, or a knock on the box.

        Worth recording rather than discarding: a rising count is either someone
        trying it on or the classifier drifting, and both are things you would
        want to find out about from a graph rather than from a shortfall.
        """
        self.store.coin_event(
            ts, self.boot_id, self.open_txn_id, None,
            self._float(frame.payload, "delta_g"), classified=False)

    @staticmethod
    def _parse_items(text):
        """`Coke:2,Fanta:1` -> {'Coke': 2, 'Fanta': 1}. `none` -> {}."""
        if not text or text == "none":
            return {}
        items = {}
        for part in text.split(","):
            name, _, qty = part.partition(":")
            if name and qty.isdigit():
                items[name] = items.get(name, 0) + int(qty)
        return items

    @staticmethod
    def _float(payload, key):
        raw = protocol.field(payload, key)
        try:
            return float(raw)
        except (TypeError, ValueError):
            return None

    # --- Card payments ------------------------------------------------------

    def _on_square_link(self, frame, ts):
        """`CMD SQUARE_LINK cents= id= desc=` — make a checkout.

        Answered asynchronously: the reply goes out from `drain_payments()` when
        the network call finishes, because doing it here would stall the loop
        that also drains the serial link.
        """
        txn_id = protocol.u32(frame.payload, "id")
        cents = protocol.u32(frame.payload, "cents")
        if txn_id is None or cents is None:
            log.warning("SQUARE_LINK missing id= or cents=: %s", frame.payload)
            return
        if self.payments is None:
            self.link.send("RSP", "SQUARE_ERR", f"id={txn_id} reason=nopayments")
            return

        # `desc` becomes the line item on the customer's receipt, so it should
        # say what they bought rather than "Test Drink" (decision D20).
        description = protocol.field(frame.payload, "desc") or "Fridge drinks"
        self.payments.request_link(txn_id, cents, description)

    def _on_square_cancel(self, frame, ts):
        """`CMD SQUARE_CANCEL id=` — the customer changed their mind.

        May name a link that does not exist yet: the board can give up while the
        Square API call is still in flight, and that is exactly the case where an
        orphaned live link would otherwise be created and never killed. The
        payment service remembers the cancellation and applies it on arrival.
        """
        txn_id = protocol.u32(frame.payload, "id")
        if txn_id is None or self.payments is None:
            return
        self.payments.request_cancel(txn_id)

    def drain_payments(self):
        """Turn finished Square work into frames. Called every service pass."""
        if self.payments is None:
            return
        for result in self.payments.drain():
            kind, txn_id = result[0], result[1]
            if kind == "url":
                self.link.send("RSP", "SQUARE_URL", f"id={txn_id} url={result[2]}")
            elif kind == "error":
                self.link.send("RSP", "SQUARE_ERR",
                               f"id={txn_id} reason={result[2]}")
            elif kind == "paid":
                self.link.send("EVT", "SQUARE_PAID",
                               f"id={txn_id} order={result[2]}")
            elif kind == "cancelled":
                ok = 1 if result[2] else 0
                if not ok:
                    # The link is still payable. Worth a warning: it is money
                    # that can still arrive against a transaction nobody expects.
                    log.warning("cancel for txn %s did not take", txn_id)
                self.link.send("RSP", "SQUARE_CANCELLED", f"id={txn_id} ok={ok}")
            elif kind == "late_paid":
                _, _, order_id, cents = result
                log.error("LATE PAYMENT on txn %s (order %s, %d cents) - "
                          "a refund is owed and must be issued by hand",
                          txn_id, order_id, cents)
                self.link.send("EVT", "SQUARE_LATE_PAID",
                               f"id={txn_id} order={order_id} cents={cents}")

    # --- Our own telemetry --------------------------------------------------

    def tick(self):
        """Record what the service knows about itself. Called every pass; acts
        on a timer.

        These go into `measurement` alongside the fridge's own metrics rather
        than into a table of their own — dashboard.md section 5.1 exists so that
        adding a metric is never a schema change, and the health row is exactly
        the case it was meant for.
        """
        # First, and outside the timer below: a board waiting in Recount cannot
        # afford to wait for a telemetry tick.
        self._answer_owed_scan()

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
