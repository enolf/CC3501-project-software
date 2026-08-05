#!/usr/bin/env python3
"""Stand in for the RP2040, in-process, so `fridged` can be built without one.

The mirror image of `fake_pi.py`: that one is a fake Pi talking to a real board,
this one is a fake board talking to a real `fridged`. Run both and the whole
system exercises end to end on a laptop with no hardware at all.

    python -m fridged --port sim

WHY IT IMITATES A SERIAL PORT RATHER THAN CALLING fridged DIRECTLY
------------------------------------------------------------------
`FakeBoard` duck-types the handful of `serial.Serial` methods the service uses —
`read`, `write`, `flush`, `close` — so `fridged` cannot tell it from a real port,
because on its side of the interface the difference does not exist. Every line of
the ingest path (bytes -> parse -> Frame -> handler -> SQLite) therefore runs
today exactly as it will in production, and going live is one command-line
argument.

The alternative, a "demo mode" writing plausible rows straight into SQLite, gets
a chart on screen a day sooner and leaves the entire ingest path unrun until the
moment it has to work. See dashboard-plan.md section 0.

WHY THE DATA IS NOT TIDY
------------------------
The easiest way to waste this work is to emit a clean stream, build panels that
assume that cleanliness, and discover on demo day that the firmware never
promised it. So this deliberately reproduces the awkward things the real board
actually does: log output interleaved with frames on the same stream, the
occasional corrupted frame, and reboots — including reboots whose `BOOT` frame is
lost, which is what the Pi's `ms` rollback detector exists to catch.

Stage D1 emits `BOOT` and `HB`. Temperature, door, health and transactions arrive
with the stages that consume them.
"""

import bisect
import math
import random
import time

import protocol

#: Firmware version string, as `pi_link_serial.cpp` reports it.
FW_VERSION = "0.2"

#: `SAMPLE_INTERVAL_MS` in temperature_task.cpp.
TEMP_SAMPLE_S = 30.0

#: A 12-bit DS18B20 conversion takes ~750 ms, and the firmware starts the
#: conversion, leaves, and reads the scratchpads when it finishes — so the
#: readings are stamped about three quarters of a second after the sample point,
#: not at it.
TEMP_CONVERSION_S = 0.75

#: The firmware reads ONE sensor per superloop pass, deliberately: a scratchpad
#: read is ~6 ms of bit-banged 1-Wire timing that cannot be interrupted, so three
#: back to back would be a visible hitch. The frames therefore arrive together
#: but not simultaneously.
TEMP_PER_SENSOR_S = 0.004

#: 12-bit resolution is 1/16 of a degree. Readings land on these steps and
#: nowhere in between, which is worth reproducing: a graph of impossibly smooth
#: values would not look like this sensor.
DS18B20_STEP_C = 0.0625

#: `HEALTH_INTERVAL_MS` in main.cpp.
HEALTH_INTERVAL_S = 30.0

# --- Door behaviour ---------------------------------------------------------

#: Relative door-opening rate by hour of the LOCAL day. A society fridge is not
#: used uniformly: it is busy at lunch and again after work, and almost nobody
#: opens it at 3am. Flat rates would make "sales by hour" (stage D5) and the
#: whole diurnal story meaningless.
HOUR_WEIGHT = (0.05, 0.02, 0.02, 0.02, 0.02, 0.05,
               0.20, 0.50, 0.80, 0.90, 1.00, 1.50,
               3.00, 3.00, 1.20, 1.00, 1.20, 2.50,
               2.50, 1.50, 1.00, 0.60, 0.30, 0.10)

OPENS_PER_DAY = 40.0

#: A normal open: someone takes a drink and shuts the door.
NORMAL_OPEN_S = (4.0, 40.0)

#: Occasionally the door is left ajar. **This is the failure that actually
#: happens**, and the one the alert exists for (dashboard.md section 10), so the
#: simulator has to produce it or the alert is built against a case that never
#: occurs in testing.
LEFT_OPEN_RATE = 0.015
LEFT_OPEN_S = (600.0, 1500.0)

#: Warming while the door is open, and recovery after it shuts. The recovery
#: constant is the slower of the two because a fridge loses cold air quickly and
#: gets it back slowly — which is exactly what makes "temperature recovery after
#: a door open" worth a panel.
DOOR_TAU_OPEN_S = 90.0
DOOR_TAU_RECOVER_S = 420.0

#: How far back excursion() looks. Ten recovery constants leaves under a
#: hundredth of a percent of any earlier excursion, so older intervals cannot
#: change the answer and iterating them would be pure cost.
LOOKBACK_S = 10.0 * DOOR_TAU_RECOVER_S

# --- Coin box ---------------------------------------------------------------
# A stand-in until stage D5 makes the box mass follow real transactions. Tying
# it to door closes rather than inventing a smooth ramp keeps it correlated with
# something real, so the money-box panel and the door panels tell a consistent
# story.

COIN_ON_CLOSE_RATE = 0.5
COIN_MASSES_G = (9.00, 6.60)

#: ~$50 in $1 coins against a 500 g cell (dashboard.md section 10). Past this
#: the box needs emptying, which is an operational necessity rather than a
#: curiosity.
BOX_FULL_G = 450.0

#: The board sends its heartbeat every 10 s (`HEARTBEAT_INTERVAL_MS` in main.cpp).
HEARTBEAT_S = 10.0

#: Chance that any given outgoing frame is corrupted on the wire. Small, but not
#: zero: a link that never produces a bad frame never exercises the code that
#: counts them, and `raw_line`'s reject column would go untested until it mattered.
CORRUPTION_RATE = 0.004

#: Simulated hours between spontaneous reboots. The reset counter and the
#: `(boot_id, txn_id)` scoping both exist because of these, so they have to
#: actually happen. At `--sim-speed 1` you will wait six hours to see one; at
#: 200x it is every two minutes, which is the point of the speed control.
REBOOT_INTERVAL_HOURS = 6.0

#: Fraction of reboots whose `BOOT` frame never arrives — a cable pulled at the
#: wrong moment. Forces the `ms_rollback` path, which is otherwise dead code that
#: nobody finds out is wrong.
LOST_BOOT_FRAME_RATE = 0.25

#: The board's log format, from `logging.cpp`. These lines share the stream with
#: frames by design (decision D1) and must be discarded by the parser without
#: disturbing it.
BOOT_BANNER = [
    "=== Smart Fridge ===",
    "[{t:.3f} Information]: build: bench (simulation stand-ins ENABLED)",
    "[{t:.3f} Information]: pi_link: backend=serial",
    "[{t:.3f} Information]: switches: door starts CLOSED",
]


class DoorSchedule:
    """When the door opens and closes, and what that does to the temperature.

    Shared by the live simulator and by `fridged/seed.py`. That sharing is the
    point: seeded history and live data have to describe ONE fridge, so that a
    door event in the timeline lines up with a warming bump in the temperature
    graph above it. Two independent models would put the bumps in the wrong
    places and every panel would be validated against a fridge that cannot
    exist.

    Works entirely in absolute unix time, for the same reason.
    """

    def __init__(self, rng, start_unix, activity=1.0):
        self.rng = rng
        #: Multiplier on the opening rate. 1.0 is a realistic society fridge —
        #: about 34 opens a day. Higher values do NOT distort time: the clock
        #: still runs at its normal speed and every timestamp is honest, the
        #: fridge is simply used more. That is what makes it usable for watching
        #: a live dashboard, where `--sim-speed` is not: speeding the clock up
        #: compresses hours of events into the few seconds of wall time the Pi
        #: stamps them with, giving a vertical wall of points instead of a graph.
        self.activity = activity
        #: (open_time, close_time) pairs, in order and non-overlapping.
        self.intervals = []
        #: (time, is_open) edges, in order. What the board actually emits.
        self.edges = []
        #: Close times only, kept sorted so excursion() can binary-search.
        self._closes = []
        self._next_open = self._next_open_after(start_unix)

    def _next_open_after(self, t):
        """The time of the next door open, by thinning.

        WHY NOT JUST SAMPLE A GAP FROM THE CURRENT HOUR'S RATE
        ------------------------------------------------------
        Because the gap can be longer than the hour it was drawn in, and then
        the rate that produced it no longer applies. A sample taken at 3am, when
        the weight is 0.02, has a mean gap of about thirty hours — so one draw
        skips the whole of the next day including both its busy periods. The
        first version of this did exactly that and produced 16 opens a day
        instead of 40, with entire days missing.

        Thinning fixes it properly: propose events at the PEAK rate, then keep
        each one with probability weight(hour)/peak. The survivors have exactly
        the intended hourly rate, and no proposal ever spans more time than the
        peak rate allows.
        """
        peak = max(HOUR_WEIGHT)
        rate = (OPENS_PER_DAY / 86400.0) * peak * self.activity
        while True:
            t += self.rng.expovariate(rate)
            # Local time, not UTC: the busy periods have to land at local
            # lunchtime to look right on a dashboard drawn in local time.
            hour = time.localtime(t).tm_hour
            if self.rng.random() < HOUR_WEIGHT[hour] / peak:
                return t

    def ensure_until(self, t):
        """Generate every door event up to time `t`. Idempotent and cheap."""
        while self._next_open <= t:
            open_t = self._next_open
            if self.rng.random() < LEFT_OPEN_RATE:
                duration = self.rng.uniform(*LEFT_OPEN_S)
            else:
                duration = self.rng.uniform(*NORMAL_OPEN_S)
            close_t = open_t + duration

            self.intervals.append((open_t, close_t))
            self.edges.append((open_t, True))
            self.edges.append((close_t, False))
            self._closes.append(close_t)
            self._next_open = self._next_open_after(close_t)

    def excursion(self, t, peak_c):
        """How far above baseline the temperature sits at time `t`, in degrees.

        The elevation is a STATE that relaxes: towards `peak_c` while the door
        is open, towards zero while it is shut. It is therefore continuous, and
        each door event carries forward whatever the last one left behind.

        The first version of this took a shortcut — use only the most recent
        interval — on the reasoning that earlier excursions have decayed away.
        That holds when opens are well separated and fails badly when they are
        not: a brief 18-second open following a 24-minute one RESET the state to
        the small value that short open alone would produce, so the model showed
        the fridge cooling by 3 degrees because somebody opened the door. A
        short open cannot cool a fridge, and a panel about recovery time built on
        that would have been measuring an artefact.

        Cost is kept down by starting from `LOOKBACK_S` ago instead of from the
        beginning of time: after ten recovery constants the residue is under a
        hundredth of a percent, so intervals older than that cannot matter.
        """
        if peak_c <= 0.0:
            return 0.0

        begin = t - LOOKBACK_S
        # Intervals are generated in order, so their close times are sorted and
        # the first one that can still matter is a binary search away.
        index = bisect.bisect_left(self._closes, begin)

        elevation = 0.0
        last_t = begin
        for open_t, close_t in self.intervals[index:]:
            if open_t > t:
                break
            if open_t > last_t:            # shut: decay towards zero
                elevation *= math.exp(-(open_t - last_t) / DOOR_TAU_RECOVER_S)
                last_t = open_t
            end = min(t, close_t)          # open: rise towards peak
            elevation = peak_c + (elevation - peak_c) * math.exp(
                -(end - last_t) / DOOR_TAU_OPEN_S)
            last_t = end
            if t <= close_t:
                return elevation

        return elevation * math.exp(-(t - last_t) / DOOR_TAU_RECOVER_S)


class Zone:
    """One DS18B20: a setpoint, a compressor cycle around it, and sensor noise.

    The board has no idea which shelf this sensor is on and neither does this
    class — it carries a ROM code and a thermal behaviour, and nothing else.
    Which zone it *is* gets decided on the Pi, in the `sensor` table
    (dashboard.md section 4.3). That separation is the thing being tested, so
    breaking it here to make the simulator tidier would defeat the exercise.
    """

    def __init__(self, rom, setpoint_c, swing_c, period_s, phase,
                 door_rise_c=0.0, noise_c=0.05):
        self.rom = rom
        self.setpoint_c = setpoint_c
        self.swing_c = swing_c
        self.period_s = period_s
        self.phase = phase
        #: Peak warming this zone would reach with the door held open
        #: indefinitely. The freezer is a separate compartment and barely
        #: notices; the top shelf sits right behind the door and notices most.
        self.door_rise_c = door_rise_c
        self.noise_c = noise_c

    def celsius(self, t, rng, door=None):
        # A fridge does not hold a temperature, it cycles around one: the
        # compressor runs, overshoots cold, stops, drifts warm, runs again. A
        # flat line with noise on it would look nothing like the real thing, and
        # the threshold colours on the stat tiles would never be exercised.
        value = self.setpoint_c + self.swing_c * math.sin(
            2.0 * math.pi * t / self.period_s + self.phase)
        if door is not None:
            value += door.excursion(t, self.door_rise_c)
        value += rng.gauss(0.0, self.noise_c)
        return round(value / DS18B20_STEP_C) * DS18B20_STEP_C


#: Three sensors with fixed, plausible ROM codes: family code 0x28 (DS18B20)
#: followed by a factory serial. Fixed rather than random so a database survives
#: a restart of the simulator with its zone names still attached — otherwise
#: naming a sensor would be undone by every restart and the naming workflow could
#: never be tested.
DEFAULT_ZONES = [
    Zone("28FF3A1C92160341", -18.0, 1.6, 2400.0, 0.0, door_rise_c=0.8),
    Zone("28FF7B4E5501A2C7",   4.0, 0.9, 2100.0, 1.1, door_rise_c=3.5),
    Zone("28FFD1082B640F19",   6.2, 1.1, 2100.0, 2.4, door_rise_c=2.0),
]


class FakeBoard:
    """A simulated RP2040 that speaks the frame protocol over a fake serial port.

    All timing is against a *simulated* clock, so `speed` compresses a day of
    fridge behaviour into minutes without changing a single interval constant.
    """

    def __init__(self, speed=1.0, seed=None, corruption_rate=CORRUPTION_RATE,
                 reboot_interval_hours=REBOOT_INTERVAL_HOURS, zones=None,
                 activity=1.0):
        self.speed = float(speed)
        self.corruption_rate = corruption_rate
        self.reboot_interval_s = reboot_interval_hours * 3600.0
        self.zones = DEFAULT_ZONES if zones is None else zones

        # Seeded so a run is reproducible. A simulator that cannot reproduce the
        # sequence that broke something is only half a test rig.
        self.rng = random.Random(seed)

        self._wall_start = time.monotonic()

        #: None while the board follows the wall clock (the live case). Set by
        #: advance(), which tests use to make timing independent of host speed.
        self._manual_t = None

        # Where this simulated fridge sits on the real calendar. The thermal
        # model is a function of absolute time, not of time-since-boot, so that
        # seeded history (fridged/seed.py, which uses unix timestamps) and this
        # live simulation describe ONE fridge with one continuous compressor
        # cycle. Without it the graph steps by a degree or so at the moment the
        # service starts — not a real event, but indistinguishable from one, and
        # exactly the sort of artefact that gets designed around by mistake.
        self._unix_start = time.time()

        self._out = bytearray()

        # Set by _reboot()
        self._boot_sim_t = 0.0
        self._next_hb = 0.0
        self._next_reboot = 0.0
        self._next_temp = 0.0
        self._next_health = 0.0

        self.frames_out = 0
        self.frames_in = 0
        self.reboots = 0
        self.lost_boot_frames = 0

        #: The door lives in absolute time and SURVIVES A REBOOT, unlike the
        #: board's timers. A fridge door does not care that the microcontroller
        #: restarted, and pretending otherwise would hide the case where the
        #: board misses an edge because it was down.
        self.door = DoorSchedule(self.rng, self._unix_start, activity)
        self._door_index = 0
        self.door_open = False

        self.box_g = 0.0
        self.emptied = 0

        self._reboot(0.0, first=True)

    # --- The simulated clock ------------------------------------------------

    def sim_time(self):
        """Seconds of simulated time since this board was created."""
        if self._manual_t is not None:
            return self._manual_t
        return (time.monotonic() - self._wall_start) * self.speed

    def advance(self, seconds):
        """Step the simulated clock by hand instead of following the wall clock.

        Calling this once switches the board to a manual clock for good.

        WHY TESTS NEED THIS. With the wall clock, how much simulated time a test
        covers depends on how many times the host manages to go round the loop —
        so the same test drained 8,591 frames on a laptop and 404 on a Pi 4, and
        an assertion about reboots happening passed on one and failed on the
        other. The test was measuring the host's speed, not the simulator's
        behaviour.

        Stepping the clock explicitly makes a test deterministic on any machine.
        Live runs keep the wall clock, which is the whole point of `--sim-speed`.
        """
        self._manual_t = (0.0 if self._manual_t is None else self._manual_t)
        self._manual_t += seconds

    def unix_time(self, at=None):
        """The simulated fridge's position on the real calendar.

        At `--sim-speed 1` this is simply the wall clock. Above that the fridge
        genuinely experiences time faster — a day of thermal cycling in a few
        minutes — which is the point of the speed control, so the thermal model
        is driven from here rather than from the wall clock directly.
        """
        return self._unix_start + (self.sim_time() if at is None else at)

    def ms_since_boot(self, at=None):
        """The board's `to_ms_since_boot()`, at a given simulated time.

        `at` defaults to now, but callers pass the time the event was *due*. The
        distinction is invisible at 1x and glaring at 10,000x: one pump pass then
        covers seconds of simulated time, and stamping frames with "now" would
        scatter heartbeats that are meant to be exactly 10 s apart. The real
        board stamps within a millisecond of due, so this matches it.
        """
        if at is None:
            at = self.sim_time()
        return int((at - self._boot_sim_t) * 1000.0)

    # --- Emitting -----------------------------------------------------------

    def _emit_line(self, text):
        """Send a raw line that is not a frame — the board's own log output."""
        self._out += (text + "\n").encode()

    def _emit(self, prefix, type_, payload="", at=None):
        wire = protocol.build(prefix, type_, payload,
                              ms=self.ms_since_boot(at))

        # Corruption is applied to the finished bytes, after the CRC is computed,
        # which is exactly how a real disturbance on a shared stream behaves: the
        # checksum is right for what was *meant* and wrong for what arrived.
        if self.rng.random() < self.corruption_rate:
            body = bytearray(wire)
            index = self.rng.randrange(4, max(5, len(body) - 4))
            body[index] = body[index] ^ 0x20  # flips a letter's case, usually
            wire = bytes(body)

        self._out += wire
        self.frames_out += 1

    def _reboot(self, at, first=False):
        """Restart: the ms counter goes back to zero and the banner reprints."""
        self._boot_sim_t = at
        self._next_hb = at
        self._next_reboot = at + self.reboot_interval_s
        # The board's first temperature sample is one full interval after boot —
        # `init()` sets `next_sample_ms = now + SAMPLE_INTERVAL_MS`. So a reboot
        # leaves a 30 s gap in the temperature graph, which is real and which the
        # dashboard should show rather than paper over.
        self._next_temp = at + TEMP_SAMPLE_S
        self._next_health = at + HEALTH_INTERVAL_S
        if not first:
            self.reboots += 1

        for line in BOOT_BANNER:
            self._emit_line(line.format(t=self.rng.uniform(0.01, 0.2)))

        # Sometimes the BOOT frame simply does not arrive. The Pi then has to
        # notice the ms counter restarting instead, which is the only other
        # evidence a reset leaves.
        if first or self.rng.random() >= LOST_BOOT_FRAME_RATE:
            self._emit("EVT", "BOOT", f"fw={FW_VERSION}", at=at)
        else:
            self.lost_boot_frames += 1

    # --- The pump -----------------------------------------------------------

    def _pump(self):
        """Generate whatever the simulated clock says is now due.

        Every generator adds `(due_time, ...)` tuples to one list, and the list
        is SORTED before anything is written. That is not tidiness.

        One pump pass can cover a lot of simulated time — at 3000x, a 10 ms wall
        pause is half a simulated minute — so several timers come due together.
        Draining them one generator at a time emits, say, all the heartbeats up
        to 70 s and only then the temperatures at 60.75 s, and the board's `ms`
        field goes *backwards* on the wire. The Pi reads a backwards jump as the
        board having reset (dashboard.md section 6) and opens a spurious boot
        record. So: merge by due time, exactly as a real board would, where the
        events genuinely happen in the order the clock reaches them.
        """
        now = self.sim_time()

        if now >= self._next_reboot:
            self._reboot(self._next_reboot)
            return  # a reboot resets every other timer; pick them up next pass

        due = []

        while now >= self._next_hb:
            due.append((self._next_hb, "EVT", "HB", ""))
            self._next_hb += HEARTBEAT_S

        while now >= self._next_temp:
            sampled_at = self._next_temp
            self._next_temp += TEMP_SAMPLE_S
            # Stamped when the readings come OUT, not when the conversion was
            # started, and spread by one superloop pass each.
            for index, zone in enumerate(self.zones):
                celsius = zone.celsius(self.unix_time(sampled_at), self.rng,
                                       self.door)
                due.append((sampled_at + TEMP_CONVERSION_S +
                            index * TEMP_PER_SENSOR_S,
                            "EVT", "TEMP",
                            f"rom={zone.rom} c={celsius:.3f}"))

        # The door. Generated in absolute time, then converted back to
        # sim-relative for the frame's `ms` stamp.
        self.door.ensure_until(self.unix_time(now))
        while self._door_index < len(self.door.edges):
            edge_t, is_open = self.door.edges[self._door_index]
            at = edge_t - self._unix_start
            if at > now:
                break
            self._door_index += 1
            self.door_open = is_open
            if not is_open:
                self._maybe_take_payment()
            due.append((at, "EVT", "DOOR",
                        f"state={'open' if is_open else 'closed'}"))

        while now >= self._next_health:
            at = self._next_health
            self._next_health += HEALTH_INTERVAL_S
            due.append((at, "EVT", "HEALTH",
                        f"die_c={self._die_celsius(at):.1f} "
                        f"box_g={self.box_g:.2f} faults=0"))

        due.sort(key=lambda item: item[0])
        for at, prefix, type_, payload in due:
            self._emit(prefix, type_, payload, at=at)

    def _die_celsius(self, at):
        """RP2040 die temperature: ADC channel 4.

        Only accurate to several degrees absolute (dashboard.md section 7), so
        it is a trend signal rather than a measurement — modelled as a slow
        daily swing around a warm-ish idle, not as anything precise.
        """
        unix_t = self.unix_time(at)
        return (32.0 + 2.5 * math.sin(2.0 * math.pi * unix_t / 86400.0)
                + self.rng.gauss(0.0, 0.25))

    def _maybe_take_payment(self):
        """Some door closes end in coins going in the box.

        A stand-in until stage D5 drives this from real transactions. Hung off
        door closes rather than a smooth ramp so the money-box panel and the
        door panels agree with each other.
        """
        if self.rng.random() >= COIN_ON_CLOSE_RATE:
            return
        for _ in range(self.rng.randint(1, 2)):
            self.box_g += self.rng.choice(COIN_MASSES_G)
        if self.box_g >= BOX_FULL_G:
            # The treasurer emptied it. A sawtooth, which is what the fill-level
            # panel is for: the drop is the takings.
            self.box_g = 0.0
            self.emptied += 1

    # --- The serial.Serial surface `fridged` uses ---------------------------

    def read(self, size=1):
        self._pump()
        if not self._out:
            return b""
        take = min(size, len(self._out))
        chunk = bytes(self._out[:take])
        del self._out[:take]
        return chunk

    def write(self, data):
        """Receive what the Pi sends. Frames are counted; the rest is ignored.

        Stage D1 has nothing to reply to — `EVT HB` from the Pi is the only
        traffic, and its arrival is the entire message. `CMD SCAN` and
        `CMD SQUARE_LINK` get answers when the stages that send them arrive.
        """
        for raw in bytes(data).split(b"\n"):
            line = raw.decode("utf-8", "replace").strip()
            if line and protocol.parse(line) is not None:
                self.frames_in += 1
        return len(data)

    def flush(self):
        pass

    def close(self):
        pass


# --- Standalone inspection --------------------------------------------------
# `python fake_board.py` prints what the board would say, fast. Useful on its own
# and, more to the point, it means a change here can be eyeballed without
# starting the service and opening a database.

if __name__ == "__main__":
    import sys

    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    board = FakeBoard(speed=10_000.0, seed=1)

    print(f"=== {minutes:g} simulated minutes of board output ===\n")
    target = minutes * 60.0
    text = bytearray()
    while board.sim_time() < target:
        text += board.read(4096)
        time.sleep(0.001)

    good = bad = other = 0
    for raw in bytes(text).split(b"\n"):
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        print(f"  {line}")
        if protocol.parse(line) is not None:
            good += 1
        elif line[:3] in protocol.PREFIXES:
            bad += 1
        else:
            other += 1

    print(f"\n  {good} valid frames, {bad} corrupted, {other} log lines, "
          f"{board.reboots} reboots ({board.lost_boot_frames} with the BOOT "
          f"frame lost)")
