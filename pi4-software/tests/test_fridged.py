#!/usr/bin/env python3
"""Host tests for `fridged`. No hardware, no network, no Grafana.

    python tests/test_fridged.py          (from pi4-software/)

Same idea as `rp2040-software/tests/host/` on the firmware side: the parts that
can be checked off-hardware are checked every time, so correctness is something
re-verified rather than hoped for (CLAUDE.md section 8).

WHAT IS WORTH TESTING HERE
--------------------------
Not "does SQLite insert a row". The things below are the ones where being wrong
produces no symptom at the time and a hole in the data later:

- close() flushing queued rows, because the alternative is a small invisible gap
  in the history at every restart;
- boot tracking, because getting it wrong lets two reboots' transaction ids
  collide and silently overwrite each other;
- the ms-rollback path, which is otherwise exercised only by a reboot whose BOOT
  frame was lost — rare, real, and untested until it matters;
- unhandled message types being counted rather than dropped, so a stage that has
  not been wired up yet is visible instead of silent.
"""

import json
import math
import os
import re
import sqlite3
import stat
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from fridged import config          # noqa: E402  (path set above)
from fridged.ingest import Ingest   # noqa: E402
from fridged.link import Link, ReconnectingSerial  # noqa: E402
from fridged.store import SchemaMismatch, Store  # noqa: E402

import protocol                     # noqa: E402  (tools/, via fridged/__init__)
from fake_board import FakeBoard    # noqa: E402

FAILURES = []


def check(label, condition):
    print(f"  {'ok  ' if condition else 'FAIL'}  {label}")
    if not condition:
        FAILURES.append(label)


class StubTransport:
    """A transport whose input is scripted, so a test can post exact bytes."""

    def __init__(self, incoming=b""):
        self.incoming = bytearray(incoming)
        self.written = bytearray()

    def read(self, size=1):
        take = min(size, len(self.incoming))
        chunk = bytes(self.incoming[:take])
        del self.incoming[:take]
        return chunk

    def write(self, data):
        self.written += data
        return len(data)

    def flush(self):
        pass

    def close(self):
        pass


def temp_db():
    return Path(tempfile.mkdtemp(prefix="fridged-test-")) / "t.db"


# --- Store ------------------------------------------------------------------

def test_store():
    print("\n--- store ---")
    path = temp_db()

    with Store(path) as store:
        tables = {r[0] for r in store._conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}
    expected = {"measurement", "boot", "sensor", "door_event", "txn",
                "txn_item", "coin_event", "rfid_event", "member",
                "stock_snapshot", "cash_count", "raw_line"}
    check(f"every table in the schema is created ({len(expected)})",
          expected <= tables)

    conn = sqlite3.connect(str(path))
    check("WAL is on and persists in the file header",
          conn.execute("PRAGMA journal_mode").fetchone()[0] == "wal")
    conn.close()

    # The guarantee that stops every restart punching a hole in the history.
    store = Store(path).open()
    store.measurement(1.0, "test.metric", 42.0)
    store.raw_line(1.0, "a line")
    store.close()                       # no explicit flush() anywhere
    conn = sqlite3.connect(str(path))
    check("close() flushes queued rows without an explicit flush",
          conn.execute("SELECT COUNT(*) FROM measurement").fetchone()[0] == 1 and
          conn.execute("SELECT COUNT(*) FROM raw_line").fetchone()[0] == 1)
    conn.close()

    # Grafana runs as another user and cannot open a WAL database read-only, so
    # the file has to be group-writable. Checked on the machine it matters on:
    # this is a no-op on Windows, and the suite is meant to be run on the Pi too.
    if os.name == "posix":
        store = Store(path).open()
        store.close()
        mode = stat.S_IMODE(path.stat().st_mode)
        check(f"the database is group-writable for Grafana (mode {mode:o})",
              bool(mode & stat.S_IWGRP))
        # The bug this replaced: a umask alone cannot do this, because umask
        # only ever REMOVES permission bits and SQLite creates the file 0644.
        path.chmod(0o644)
        Store(path).open().close()
        check("...and a 0644 database is corrected on open",
              bool(stat.S_IMODE(path.stat().st_mode) & stat.S_IWGRP))
    else:
        print("  --    group-writable checks skipped (not POSIX)")

    # A sensor someone has named must not lose its name on the next reading.
    store = Store(path).open()
    store.note_sensor("28FF1234", 1.0)
    store._conn.execute("UPDATE sensor SET zone_label='freezer'")
    store.note_sensor("28FF1234", 2.0)
    check("re-seeing a sensor does not clobber its zone label",
          store.zone_for_rom("28FF1234") == "freezer")
    check("an unnamed sensor reports None", store.zone_for_rom("28FFDEAD") is None)
    store.close()

    # A schema that has moved on must refuse, not write rows into the wrong shape.
    conn = sqlite3.connect(str(path))
    conn.execute("PRAGMA user_version=999")
    conn.close()
    try:
        Store(path).open()
        check("a schema version mismatch is refused", False)
    except SchemaMismatch:
        check("a schema version mismatch is refused", True)


# --- Link -------------------------------------------------------------------

def test_link():
    print("\n--- link ---")

    good = protocol.build("EVT", "DOOR", "state=open", ms=12345)
    log_line = b"[12.345 Information]: switches: door starts CLOSED\n"
    corrupt = bytearray(protocol.build("EVT", "HB", "", ms=999))
    corrupt[5] ^= 0x20

    link = Link(StubTransport(good + log_line + bytes(corrupt)))
    events = link.poll()
    kinds = [e.kind for e in events]
    check("a valid frame, a log line and a corrupt frame are told apart",
          kinds == ["frame", "log", "bad"])
    check("the decoded frame carries its type and payload",
          events[0].frame.type == "DOOR" and
          events[0].frame.payload == "state=open")
    check("counters agree with what arrived",
          (link.frames_in, link.ignored, link.bad) == (1, 1, 1))
    check("a rejected frame carries a reason for raw_line",
          events[2].reason is not None and events[0].reason is None)

    # Not cosmetic: without this the board declares the link dead after 30 s of
    # quiet and takes itself out of service.
    check("the heartbeat goes out on the first poll",
          protocol.parse(link.transport.written.decode().strip()).type == "HB")

    link2 = Link(StubTransport())
    check("no frames yet means the link is not healthy",
          link2.age_s is None and not link2.healthy)

    # A frame split across reads must still be assembled.
    whole = protocol.build("EVT", "TEMP", "rom=28FF1234 c=4.250", ms=1)
    link3 = Link(StubTransport())
    got = []
    for i in range(0, len(whole), 5):
        link3.transport.incoming += whole[i:i + 5]
        got += link3.poll()
    check("a frame arriving in fragments is reassembled",
          len(got) == 1 and got[0].frame.type == "TEMP")


class FlakySerial:
    """A serial port that can be made to vanish, as a real one does.

    `alive = False` makes every call raise, which is what a pyserial handle does
    once its USB device has gone: the descriptor stays valid and every operation
    on it fails.
    """

    def __init__(self, incoming=b""):
        self.incoming = incoming
        self.written = b""
        self.alive = True
        self.closed = False

    def read(self, size):
        if not self.alive:
            raise OSError(5, "Input/output error")
        chunk, self.incoming = self.incoming[:size], self.incoming[size:]
        return chunk

    def write(self, data):
        if not self.alive:
            raise OSError(5, "Input/output error")
        self.written += data
        return len(data)

    def close(self):
        self.closed = True


def test_reconnecting_serial():
    """The board is reset, or the cable is nudged. It has to come back.

    This is the difference between a fridge that survives a demonstration and
    one that needs somebody to SSH in. Before this class, a re-enumerated device
    left the service running, reporting itself alive, and permanently deaf.
    """
    print("\n--- reconnecting serial ---")

    now = [1000.0]
    clock = lambda: now[0]                                  # noqa: E731

    # 1. The port is not there yet. This is the ordinary state for the first
    #    second or two of a cold boot, when the Pi and the board power up
    #    together, and it must NOT be a crash.
    attempts = []

    def refuse(port, baud):
        attempts.append(port)
        raise OSError(2, "No such file or directory")

    port = ReconnectingSerial("/dev/nope", 115200, opener=refuse, clock=clock)
    check("a missing port does not raise on construction", not port.connected)
    check("reading a missing port yields no data, not an exception",
          port.read(64) == b"")
    check("writing to a missing port is swallowed", port.write(b"x") == 0)

    # 2. Retries are spaced. A tight loop against an unplugged cable would burn
    #    the CPU the service needs for everything else.
    before = len(attempts)
    for _ in range(50):
        port.read(64)
    check("retries are rate limited, not attempted every pass",
          len(attempts) == before)

    # 3. The board turns up. The port opens itself, with nobody restarting
    #    anything.
    device = FlakySerial(protocol.build("EVT", "HB", "", ms=1))
    now[0] += 30.0
    port._opener = lambda p, b: device
    got = port.read(64)
    check("the port opens itself once the device appears", port.connected)
    check("data flows as soon as it reconnects", got.startswith(b"EVT"))

    # 4. RESET on the RP2040. The descriptor survives; the device does not.
    device.alive = False
    check("a vanished device reads as no data", port.read(64) == b"")
    check("the dead handle is dropped, not held", not port.connected)
    check("the reconnect is counted so the link can resynchronise",
          port.reopens == 1)

    replacement = FlakySerial(protocol.build("EVT", "HB", "", ms=2))
    now[0] += 30.0
    port._opener = lambda p, b: replacement
    check("it comes back on its own after a reset",
          port.read(64).startswith(b"EVT") and port.connected)


def test_link_resynchronises():
    """A reconnect must not manufacture a corrupt frame out of two halves."""
    print("\n--- link resynchronisation ---")

    whole = protocol.build("EVT", "TEMP", "rom=28FF1234 c=4.250", ms=1)
    device = FlakySerial(whole[:12])            # cut off mid-frame

    now = [0.0]
    port = ReconnectingSerial("/dev/x", 115200, opener=lambda p, b: device,
                              clock=lambda: now[0])
    link = Link(port)
    link.poll()                                 # buffers the front half
    check("the truncated half is held, not reported", link.bad == 0)

    # The board resets. The half-frame in the buffer is now meaningless, and the
    # bytes that follow are the middle of a different frame.
    device.alive = False
    link.poll()

    now[0] += 30.0
    tail = protocol.build("EVT", "HB", "", ms=2)
    port._opener = lambda p, b: FlakySerial(tail)
    events = link.poll()

    check("the stale half-frame is discarded across a reconnect",
          link.bad == 0)
    check("the first frame after reconnecting is read normally",
          [e.kind for e in events] == ["frame"] and
          events[0].frame.type == "HB")


def test_seed_clear():
    """`--clear` must remove the seed and NOTHING else.

    It used to delete `measurement`, then hit FOREIGN KEY constraint failed on
    the seed's `boot` row — which `txn` still referenced — leaving the database
    with two weeks of sales and no temperatures. A half-cleared database is
    worse than an uncleared one, because it looks fine until someone reads it.
    """
    print("\n--- seed --clear ---")
    import contextlib
    import io

    from fridged import seed as seed_module

    path = str(temp_db())
    quiet = io.StringIO()
    with contextlib.redirect_stdout(quiet):
        seed_module.main(["--db", path, "--days", "0.05"])

    conn = sqlite3.connect(path)
    conn.execute("PRAGMA foreign_keys=ON")
    seeded = {t: conn.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
              for t in ("measurement", "door_event", "stock_snapshot", "txn")}
    check("seeding writes history across several tables",
          all(n > 0 for n in seeded.values()))

    # Live rows, dated now — long after the seed's boundary. These must survive,
    # which is the whole reason `--clear` works off a marker and not off an age.
    now = time.time()
    live_boot = conn.execute(
        "INSERT INTO boot (ts, fw, reason) VALUES (?,?,?)",
        (now, "0.2", "boot_frame")).lastrowid
    conn.execute("INSERT INTO measurement (ts, metric, value) VALUES (?,?,?)",
                 (now, "camera.confidence", 91.0))
    conn.execute("INSERT INTO door_event (ts, state) VALUES (?,?)",
                 (now, "open"))
    conn.execute("INSERT INTO stock_snapshot (ts, drink, count, trigger) "
                 "VALUES (?,?,?,?)", (now, "coke", 5, "door_open"))
    conn.execute("INSERT INTO txn (boot_id, txn_id, ts_start, method, outcome) "
                 "VALUES (?,?,?,?,?)", (live_boot, 1, now, "cash", "paid"))
    conn.execute("INSERT INTO txn_item (boot_id, txn_id, drink, qty) "
                 "VALUES (?,?,?,?)", (live_boot, 1, "coke", 1))
    conn.commit()
    sensors_before = conn.execute("SELECT COUNT(*) FROM sensor").fetchone()[0]
    conn.close()

    with contextlib.redirect_stdout(quiet):
        seed_module.main(["--db", path, "--clear"])

    conn = sqlite3.connect(path)
    after = {t: conn.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
             for t in ("measurement", "door_event", "stock_snapshot", "txn",
                       "txn_item", "boot", "raw_line", "sensor")}
    integrity = conn.execute("PRAGMA integrity_check").fetchone()[0]
    conn.close()

    check("every seeded table is emptied, not just measurement",
          (after["measurement"], after["door_event"],
           after["stock_snapshot"]) == (1, 1, 1))
    check("seeded transactions go with their boot",
          (after["txn"], after["txn_item"], after["boot"]) == (1, 1, 1))
    check("the seed marker is removed", after["raw_line"] == 0)
    check("live rows written after the seed survive",
          after["measurement"] == 1 and after["txn"] == 1)
    check("sensor names are configuration and are never cleared",
          after["sensor"] == sensors_before)
    check("the database is still sound afterwards", integrity == "ok")


# --- Ingest -----------------------------------------------------------------

def feed(ingest, link, wire):
    link.transport.incoming += wire
    for event in link.poll():
        ingest.handle(event)


def test_ingest():
    print("\n--- ingest ---")
    store = Store(temp_db()).open()
    link = Link(StubTransport())
    ingest = Ingest(store, link)

    feed(ingest, link, protocol.build("EVT", "BOOT", "fw=0.2", ms=0))
    store.flush(force=True)
    row = store._conn.execute("SELECT fw, reason FROM boot").fetchone()
    check("a BOOT frame opens a boot row carrying the firmware version",
          row == ("0.2", "boot_frame"))

    feed(ingest, link, protocol.build("EVT", "HB", "", ms=10_000))
    check("a later frame stays in the same boot",
          store._conn.execute("SELECT COUNT(*) FROM boot").fetchone()[0] == 1)

    # The board reset and the BOOT frame was lost. The counter restarting is the
    # only evidence left, and missing it would let the new boot's transaction ids
    # collide with the old boot's.
    feed(ingest, link, protocol.build("EVT", "HB", "", ms=5))
    reasons = [r[0] for r in store._conn.execute(
        "SELECT reason FROM boot ORDER BY id")]
    check("the ms counter going backwards opens a new boot ('ms_rollback')",
          reasons == ["boot_frame", "ms_rollback"])

    # Starting the service against a board that is already running.
    store2 = Store(temp_db()).open()
    link2 = Link(StubTransport())
    ingest2 = Ingest(store2, link2)
    feed(ingest2, link2, protocol.build("EVT", "HB", "", ms=900_000))
    check("joining a running board opens a 'resumed' boot",
          store2._conn.execute(
              "SELECT reason FROM boot").fetchone()[0] == "resumed")

    # A type nobody has wired up yet must be visible, not silently dropped.
    # Asserted with a type that will never gain a handler, so this keeps testing
    # the mechanism rather than failing every time a stage lands.
    feed(ingest2, link2, protocol.build("EVT", "NOTASTAGEYET", "x=1", ms=910_000))
    check("an unhandled message type is counted, not dropped",
          ingest2.unhandled.get("NOTASTAGEYET") == 1)
    store2.flush(force=True)
    check("...and its line is still in raw_line either way",
          store2._conn.execute(
              "SELECT COUNT(*) FROM raw_line WHERE line LIKE '%NOTASTAGEYET%'"
          ).fetchone()[0] == 1)

    # Temperature: stored under the ROM code, and the sensor auto-registered.
    feed(ingest2, link2, protocol.build(
        "EVT", "TEMP", "rom=28FF3A1C92160341 c=-17.688", ms=920_000))
    store2.flush(force=True)
    check("a TEMP frame registers the sensor and stores under its ROM code",
          store2._conn.execute(
              "SELECT value FROM measurement WHERE metric='temp.rom."
              "28FF3A1C92160341'").fetchone()[0] == -17.688 and
          store2.zone_for_rom("28FF3A1C92160341") is None)

    # The view is what panels query, and it must show an unnamed sensor rather
    # than hiding it — that is the whole discovery workflow.
    row = store2._conn.execute(
        "SELECT zone, unmapped, celsius FROM temperature").fetchone()
    check("the temperature view shows an unnamed sensor as unmapped",
          row is not None and row[1] == 1 and "unmapped" in row[0])

    # Naming it must relabel its HISTORY, not just readings from now on.
    store2._conn.execute(
        "UPDATE sensor SET zone_label='freezer' WHERE rom_code=?",
        ("28FF3A1C92160341",))
    row = store2._conn.execute(
        "SELECT zone, unmapped FROM temperature").fetchone()
    check("naming a sensor relabels readings taken before it was named",
          row == ("freezer", 0))

    ingest2.tick()
    store2.flush(force=True)
    metrics = {r[0] for r in store2._conn.execute(
        "SELECT DISTINCT metric FROM measurement")}
    check("the service records its own link health as measurements",
          {"link.age_s", "link.frames_bad"} <= metrics)

    store.close()
    store2.close()


# --- The simulated board ----------------------------------------------------

#: Simulated seconds per step. Smaller than the shortest interval that matters
#: (the 10 s heartbeat), so nothing is ever generated in a lump.
STEP_S = 5.0


def run_simulated(board, total_sim_s, on_bytes=None):
    """Drive the board over `total_sim_s` of simulated time, deterministically.

    The clock is STEPPED rather than raced. A wall-clock version of this covered
    however much simulated time the host managed to get through, which made
    every count in the tests a measurement of the machine rather than of the
    code — the end-to-end test drained 8,591 frames on a laptop and 404 on a
    Pi 4, and failed on the Pi for no reason that had anything to do with a bug.

    Each step drains until the board goes quiet, because `_pump()` returns early
    on a reboot and leaves the rest of that moment's traffic for the next call.
    """
    collected = bytearray()
    for _ in range(int(total_sim_s / STEP_S)):
        board.advance(STEP_S)
        while True:
            chunk = board.read(65536)
            if not chunk:
                break
            if on_bytes is None:
                collected += chunk
            else:
                on_bytes(chunk)
    return bytes(collected)


def test_fake_board():
    print("\n--- fake board ---")
    board = FakeBoard(seed=11)
    lines = [r.decode("utf-8", "replace").strip()
             for r in run_simulated(board, 8 * 3600).split(b"\n") if r.strip()]
    frames = [f for f in (protocol.parse(line) for line in lines) if f]

    # THE regression test for this simulator. One pump pass can cover minutes of
    # simulated time, so several timers come due at once; if they are drained one
    # generator at a time the `ms` field goes backwards on the wire, and the Pi
    # reads that as the board having reset. The bug produces plausible-looking
    # output and a database full of boot records that never happened.
    # A reset takes `ms` back to near zero. Any other backwards step is the bug,
    # whether or not a BOOT frame accompanied it — which is the same rule the Pi
    # applies, so the test and the detector agree on what a reset looks like.
    RESET_CEILING_MS = 60_000
    backwards = [(previous, f.ms)
                 for previous, f in zip([x.ms for x in frames], frames[1:])
                 if f.ms < previous and f.ms > RESET_CEILING_MS]
    check(f"`ms` never goes backwards except at a reset "
          f"({len(backwards)} bad jumps in {len(frames)} frames)", not backwards)
    for jump in backwards[:3]:
        print(f"        {jump[0]} -> {jump[1]}")

    temps = [f for f in frames if f.type == "TEMP"]
    roms = {protocol.field(f.payload, "rom") for f in temps}
    check(f"all three sensors report ({len(roms)} distinct ROM codes)",
          len(roms) == 3)
    check("every ROM code is a 16-digit hex DS18B20 address starting 28",
          all(r and len(r) == 16 and r.startswith("28") for r in roms))

    values = [float(protocol.field(f.payload, "c")) for f in temps]
    check("temperatures are plausible fridge values",
          all(-25.0 < v < 15.0 for v in values))
    # 1/16 C is the sensor's 12-bit resolution, but the firmware sends `c=%.3f`,
    # so the wire value is that step rounded to three decimals — -17.6875 leaves
    # the board as "-17.688". The tolerance is the formatting, not slop.
    #
    # Half of the sixteen steps end in a half-millidegree (.0625, .1875, ...) and
    # are displaced by EXACTLY 0.0005, so the bound has to be strictly above that
    # rather than equal to it. A `< 0.0005` here fails on eight steps in sixteen.
    check("readings land on the DS18B20's 1/16 degree steps (to wire precision)",
          all(abs(v - round(v / 0.0625) * 0.0625) < 0.00051 for v in values))
    check("the freezer actually cycles rather than sitting flat",
          max(v for v in values if v < -10) -
          min(v for v in values if v < -10) > 1.0)


# --- Door and health --------------------------------------------------------

def test_door_model():
    print("\n--- door model ---")
    import random

    from fake_board import (DOOR_TAU_RECOVER_S, DoorSchedule, HOUR_WEIGHT,
                            OPENS_PER_DAY)

    start = time.time() - 14 * 86400
    door = DoorSchedule(random.Random(4), start)
    door.ensure_until(start + 14 * 86400)

    opens_per_day = len(door.intervals) / 14.0
    # The first version sampled each gap from the CURRENT hour's rate. A gap
    # drawn at 3am has a mean of thirty hours, so one draw skipped a whole day
    # including both busy periods, and the rate came out at 40% of target.
    check(f"the open rate is close to the configured {OPENS_PER_DAY:g}/day "
          f"(got {opens_per_day:.1f})",
          0.6 * OPENS_PER_DAY < opens_per_day < 1.6 * OPENS_PER_DAY)

    by_hour = [0] * 24
    for open_t, _ in door.intervals:
        by_hour[time.localtime(open_t).tm_hour] += 1
    busiest = by_hour.index(max(by_hour))
    check(f"the busiest hour is one of the configured peaks (got {busiest:02d}:00)",
          HOUR_WEIGHT[busiest] >= max(HOUR_WEIGHT) * 0.8)
    check("the small hours are quieter than the peak",
          sum(by_hour[1:5]) < max(by_hour))

    check("the door is sometimes left open for minutes, not seconds",
          any(close - open_t > 300 for open_t, close in door.intervals))

    # THE regression test for this model, on a HAND-BUILT scenario rather than
    # a random one: the case that broke needs a short open shortly after a long
    # one, and waiting for the RNG to produce that is not a test.
    #
    # The first version used only the most recent interval, so the brief open
    # below reset the elevation to the small value that 20 seconds alone would
    # produce — the fridge appeared to COOL by three degrees because somebody
    # opened the door.
    peak = 3.5
    scripted = DoorSchedule(random.Random(0), 0.0)
    scripted.intervals = [(1000.0, 2200.0),    # 20 minutes, wide open
                          (2300.0, 2320.0)]    # 20 seconds, 100 s later
    scripted._closes = [close for _, close in scripted.intervals]

    samples = [(t, scripted.excursion(t, peak))
               for t in [i * 10.0 for i in range(0, 800)]]

    # Rises are fine and fast — that is what opening a door does. Only DROPS are
    # constrained, by how much the recovery constant permits in ten seconds.
    max_drop = peak * (1.0 - math.exp(-10.0 / DOOR_TAU_RECOVER_S)) * 1.05
    drops = [(a[0], a[1], b[1]) for a, b in zip(samples, samples[1:])
             if a[1] - b[1] > max_drop]
    check(f"the excursion never drops faster than the recovery constant allows "
          f"({len(drops)} violations, limit {max_drop:.3f} C per 10 s)", not drops)
    for t, before, after in drops[:3]:
        print(f"        t={t:.0f}s  {before:.3f} -> {after:.3f}")

    check("the excursion never exceeds the zone's peak",
          all(value <= peak + 1e-9 for _, value in samples))
    check("the excursion is never negative — a door cannot cool a fridge",
          all(value >= -1e-9 for _, value in samples))

    # The specific failure, stated directly: the short open must leave the
    # elevation at least as high as simply carrying on decaying would have.
    before_short = scripted.excursion(2299.0, peak)
    after_short = scripted.excursion(2321.0, peak)
    check(f"a brief open does not cool the fridge "
          f"({before_short:.2f} C before, {after_short:.2f} C after)",
          after_short >= before_short - 0.05)

    # A door open must show up in the temperature at all, or the timeline panel
    # and the graph above it are telling unrelated stories.
    during = scripted.excursion(2199.0, peak)
    later = scripted.excursion(2320.0 + 8 * DOOR_TAU_RECOVER_S, peak)
    check(f"a long open warms the zone ({during:.2f} C when it shuts)",
          during > 0.9 * peak)
    check(f"and it recovers once the door stays shut ({later:.3f} C later)",
          later < 0.02 * peak)


def test_door_and_health_ingest():
    print("\n--- door and health ingest ---")
    store = Store(temp_db()).open()
    link = Link(StubTransport())
    ingest = Ingest(store, link)

    feed(ingest, link, protocol.build("EVT", "BOOT", "fw=0.2", ms=0))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=1000))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=closed", ms=2000))
    store.flush(force=True)

    rows = store._conn.execute(
        "SELECT state, duration_s FROM door_event ORDER BY ts").fetchall()
    check("an open and a close are both recorded",
          [r[0] for r in rows] == ["open", "closed"])
    check("the open row gets its duration filled in on close",
          rows[0][1] is not None and rows[0][1] > 0)
    check("the close row carries no duration of its own", rows[1][1] is None)

    # Starting while the door is already open. Legitimate, and it must not
    # attach the duration to some unrelated earlier open.
    store2 = Store(temp_db()).open()
    link2 = Link(StubTransport())
    ingest2 = Ingest(store2, link2)
    feed(ingest2, link2, protocol.build("EVT", "DOOR", "state=closed", ms=500))
    store2.flush(force=True)
    check("a close with no open on record is recorded but matched to nothing",
          store2._conn.execute(
              "SELECT COUNT(*) FROM door_event WHERE duration_s IS NOT NULL"
          ).fetchone()[0] == 0)

    # A missed close must strand ONE open, not corrupt the next pair.
    feed(ingest2, link2, protocol.build("EVT", "DOOR", "state=open", ms=1000))
    feed(ingest2, link2, protocol.build("EVT", "DOOR", "state=open", ms=2000))
    feed(ingest2, link2, protocol.build("EVT", "DOOR", "state=closed", ms=3000))
    store2.flush(force=True)
    unterminated = store2._conn.execute(
        "SELECT COUNT(*) FROM door_event "
        "WHERE state='open' AND duration_s IS NULL").fetchone()[0]
    check("a missed close strands exactly one open, not both",
          unterminated == 1)

    feed(ingest2, link2, protocol.build(
        "EVT", "HEALTH", "die_c=31.4 box_g=250.5 faults=0", ms=4000))
    store2.flush(force=True)
    got = dict(store2._conn.execute(
        "SELECT metric, value FROM measurement "
        "WHERE metric IN ('temp.rp2040_die','coinbox.mass_g','health.faults')"))
    check("HEALTH is unpacked into three separate metrics",
          got == {"temp.rp2040_die": 31.4, "coinbox.mass_g": 250.5,
                  "health.faults": 0.0})

    store.close()
    store2.close()


# --- Transactions -----------------------------------------------------------

def test_transaction_ingest():
    print("\n--- transaction ingest ---")
    import random

    from fake_board import plan_transaction

    store = Store(temp_db()).open()
    link = Link(StubTransport())
    ingest = Ingest(store, link)
    feed(ingest, link, protocol.build("EVT", "BOOT", "fw=0.2", ms=0))

    # A CASH sale, in the firmware's real order: start, coins, start AGAIN, end.
    # The duplicate is the whole point — an INSERT would raise, and naive item
    # handling would double the basket and the units-sold panel with it.
    for payload in (
        ("TXN_START", "id=7 items=coke:2,fanta:1 owed=600 method=cash"),
        ("COIN", "denom=200 delta_g=6.60"),
        ("COIN", "denom=200 delta_g=6.60"),
        ("COIN_REJECT", "delta_g=3.40"),
        ("COIN", "denom=200 delta_g=6.60"),
        ("TXN_START", "id=7 items=coke:2,fanta:1 owed=600 method=cash"),
        ("TXN_END", "id=7 outcome=paid owed=600 paid=600"),
    ):
        feed(ingest, link, protocol.build("EVT", payload[0], payload[1], ms=1))
    store.flush(force=True)

    check("a duplicated TXN_START produces ONE transaction row",
          store._conn.execute("SELECT COUNT(*) FROM txn").fetchone()[0] == 1)
    row = store._conn.execute(
        "SELECT method, owed_cents, paid_cents, outcome FROM txn").fetchone()
    check("the sale is complete after TXN_END",
          row == ("cash", 600, 600, "paid"))
    items = dict(store._conn.execute("SELECT drink, qty FROM txn_item"))
    check(f"the basket is not doubled by the duplicate ({items})",
          items == {"coke": 2, "fanta": 1})
    check("coins are attributed to the open transaction",
          store._conn.execute(
              "SELECT COUNT(*) FROM coin_event WHERE txn_id = 7 "
              "AND classified = 1").fetchone()[0] == 3)
    check("a rejected coin is recorded with no denomination",
          store._conn.execute(
              "SELECT COUNT(*) FROM coin_event WHERE classified = 0 "
              "AND denom_cents IS NULL").fetchone()[0] == 1)

    # An abandoned CARD sale sends TXN_END for a transaction we never saw start.
    # A plain UPDATE would silently affect nothing and the theft would vanish.
    feed(ingest, link, protocol.build(
        "EVT", "TXN_END", "id=9 outcome=stolen owed=400 paid=0", ms=2))
    store.flush(force=True)
    row = store._conn.execute(
        "SELECT outcome, owed_cents, ts_start FROM txn WHERE txn_id = 9"
    ).fetchone()
    check("TXN_END alone still creates the transaction (abandoned card sale)",
          row is not None and row[0] == "stolen" and row[1] == 400)
    check("...with no start time, because the board never sent one",
          row[2] is None)

    # Coins with nothing open must not be attached to the last transaction.
    feed(ingest, link, protocol.build("EVT", "COIN", "denom=100 delta_g=9.00",
                                      ms=3))
    store.flush(force=True)
    check("a coin with no transaction open is recorded with a NULL txn_id",
          store._conn.execute(
              "SELECT COUNT(*) FROM coin_event WHERE txn_id IS NULL"
          ).fetchone()[0] == 1)

    # --- Changing your mind: the basket is REPLACED, never merged ---
    #
    # The board lets a customer reopen the door mid-transaction and swap one
    # drink for another, then re-sends TXN_START with the corrected basket. The
    # second frame names a drink the first one never mentioned, and drops one it
    # did.
    #
    # Merging instead of replacing would record BOTH drinks as sold, one of
    # which was put back. Revenue would still be right — that comes from
    # txn.owed_cents — which is exactly what makes it nasty: the units-sold and
    # stock reconciliation panels would drift away from the money with nothing
    # anywhere reporting an error.
    feed(ingest, link, protocol.build(
        "EVT", "TXN_START", "id=13 items=coke:1 owed=200 method=cash", ms=6))
    feed(ingest, link, protocol.build(
        "EVT", "TXN_START", "id=13 items=mtndew:1 owed=200 method=cash", ms=7))
    store.flush(force=True)
    items = dict(store._conn.execute(
        "SELECT drink, qty FROM txn_item WHERE txn_id = 13"))
    check(f"swapping a drink replaces the basket rather than adding to it "
          f"({items})", items == {"mtndew": 1})

    # Two drinks down to one: the same rule, and the case a naive "delete only
    # when the basket is empty" fix would still get wrong.
    feed(ingest, link, protocol.build(
        "EVT", "TXN_START", "id=14 items=coke:1,solo:2 owed=600 method=cash",
        ms=8))
    feed(ingest, link, protocol.build(
        "EVT", "TXN_START", "id=14 items=solo:1 owed=200 method=cash", ms=9))
    store.flush(force=True)
    items = dict(store._conn.execute(
        "SELECT drink, qty FROM txn_item WHERE txn_id = 14"))
    check(f"putting drinks back shrinks the basket ({items})",
          items == {"solo": 1})

    # A multi-word drink has to survive the wire at all. `items=` sits inside a
    # space-separated payload, so a display name would truncate the field here
    # and every drink after it would be lost.
    check("a multi-word drink is carried by its wire key, not its name",
          "mtndew" in dict(store._conn.execute(
              "SELECT drink, qty FROM txn_item WHERE txn_id = 13")))

    # The duplicate arrives at completion; it must not drag ts_start forward.
    feed(ingest, link, protocol.build(
        "EVT", "TXN_START", "id=11 items=coke:1 owed=200 method=cash", ms=4))
    first = store._conn.execute(
        "SELECT ts_start FROM txn WHERE txn_id = 11").fetchone()[0]
    time.sleep(0.01)
    feed(ingest, link, protocol.build(
        "EVT", "TXN_START", "id=11 items=coke:1 owed=200 method=cash", ms=5))
    second = store._conn.execute(
        "SELECT ts_start FROM txn WHERE txn_id = 11").fetchone()[0]
    check("ts_start keeps the EARLIEST value, not the duplicate's",
          second == first)

    # And the generator really does produce all three shapes.
    shapes = set()
    rng = random.Random(1)
    for i in range(400):
        types = [f[2] for f in plan_transaction(rng, i, 0.0, {"Coke": 1}, 200)]
        starts = types.count("TXN_START")
        stolen = any("TXN_END" == t for t in types)
        shapes.add((starts, stolen))
    check(f"the simulator emits 0, 1 and 2 TXN_STARTs across transactions "
          f"({sorted(s[0] for s in shapes)})",
          {0, 1, 2} <= {s[0] for s in shapes})

    store.close()


# --- Who bought it -----------------------------------------------------------

def test_member_attribution():
    print("\n--- member attribution ---")

    UID = "04404C22C06780"
    OTHER_UID = "DEADBEEF"

    store = Store(temp_db()).open()
    link = Link(StubTransport())
    ingest = Ingest(store, link)
    feed(ingest, link, protocol.build("EVT", "BOOT", "fw=0.2", ms=0))

    def sale(txn_id, ms):
        """One complete cash sale, in the firmware's real order."""
        feed(ingest, link, protocol.build(
            "EVT", "TXN_START",
            f"id={txn_id} items=Coke:1 owed=200 method=cash", ms=ms))
        feed(ingest, link, protocol.build(
            "EVT", "TXN_END",
            f"id={txn_id} outcome=paid owed=200 paid=200", ms=ms))

    def member_of(txn_id):
        return store._conn.execute(
            "SELECT member_uid FROM txn WHERE txn_id = ?", (txn_id,)).fetchone()[0]

    # --- The normal path: tap, open the door, buy something ---
    feed(ingest, link, protocol.build("EVT", "RFID", f"uid={UID} granted=1", ms=1))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=2))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=closed", ms=3))
    sale(1, 4)
    store.flush(force=True)

    check("an approved tap is recorded", store._conn.execute(
        "SELECT COUNT(*) FROM rfid_event WHERE uid = ? AND granted = 1",
        (UID,)).fetchone()[0] == 1)
    check("a card seen for the first time gets a member row with NO name",
          store._conn.execute("SELECT label FROM member WHERE uid = ?",
                              (UID,)).fetchone() == (None,))
    check("the sale is attributed to the card that tapped",
          member_of(1) == UID)

    # --- TXN_END clears it: the next person is not charged to the last one ---
    sale(2, 5)
    store.flush(force=True)
    check("a sale with no tap behind it is 'other' (NULL member_uid)",
          member_of(2) is None)

    # --- A denied card is history, never an attribution ---
    feed(ingest, link, protocol.build(
        "EVT", "RFID", f"uid={OTHER_UID} granted=0", ms=6))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=7))
    sale(3, 8)
    store.flush(force=True)
    check("a DENIED tap is still recorded", store._conn.execute(
        "SELECT COUNT(*) FROM rfid_event WHERE uid = ? AND granted = 0",
        (OTHER_UID,)).fetchone()[0] == 1)
    check("a denied card is never attributed a purchase",
          member_of(3) is None)

    # --- Tapping and walking away must not charge the next customer ---
    # The board gives up after GREETING_TIMEOUT_MS and forgets them; the age is
    # forced here because wall-clock deltas inside one test are ~0.
    feed(ingest, link, protocol.build("EVT", "RFID", f"uid={UID} granted=1", ms=9))
    uid_only, tapped_at = ingest.pending_tap
    ingest.pending_tap = (uid_only, tapped_at - ingest.TAP_WINDOW_S - 1)
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=10))
    sale(4, 11)
    store.flush(force=True)
    check("a tap that expired before the door opened is not attributed",
          member_of(4) is None)

    # --- One tap covers look, shut, look again — the board treats it as one
    # visit, so dropping it at the second door open would be wrong ---
    feed(ingest, link, protocol.build("EVT", "RFID", f"uid={UID} granted=1", ms=12))
    for ms in (13, 14, 15, 16):
        state = "open" if ms % 2 else "closed"
        feed(ingest, link, protocol.build("EVT", "DOOR", f"state={state}", ms=ms))
    sale(5, 17)
    store.flush(force=True)
    check("one tap survives several door cycles in the same visit",
          member_of(5) == UID)

    # --- An abandoned CARD sale sends only TXN_END. Theft is the outcome where
    # knowing who walked off matters most, so it must still carry the uid ---
    feed(ingest, link, protocol.build("EVT", "RFID", f"uid={UID} granted=1", ms=18))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=19))
    feed(ingest, link, protocol.build(
        "EVT", "TXN_END", "id=6 outcome=stolen owed=400 paid=0", ms=20))
    store.flush(force=True)
    check("an abandoned sale (TXN_END only) is still attributed",
          member_of(6) == UID)

    # --- An attribution older than the board could possibly hold is dropped ---
    feed(ingest, link, protocol.build("EVT", "RFID", f"uid={UID} granted=1", ms=21))
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=22))
    ingest.active_member_ts -= ingest.SESSION_WINDOW_S + 1
    sale(7, 23)
    store.flush(force=True)
    check("an attribution past SESSION_WINDOW_S falls back to 'other'",
          member_of(7) is None)

    # --- Naming a card names its whole history, because the join is at read
    # time. This is the sensor-naming pattern, and it is what the panel does ---
    store._conn.execute("UPDATE member SET label = ? WHERE uid = ?",
                        ("Damien Turner", UID))
    check("label_for_uid reads the name back",
          store.label_for_uid(UID) == "Damien Turner")
    named = store._conn.execute(
        "SELECT COALESCE(m.label, 'other'), COUNT(*) "
        "FROM txn t LEFT JOIN member m ON m.uid = t.member_uid "
        "GROUP BY 1 ORDER BY 2 DESC").fetchall()
    check(f"naming the card names its whole history retrospectively ({named})",
          dict(named).get("Damien Turner") == 3)

    # A tap seen a second time must not have the name wiped back to NULL.
    feed(ingest, link, protocol.build("EVT", "RFID", f"uid={UID} granted=1", ms=24))
    store.flush(force=True)
    check("re-tapping a named card does not erase its name",
          store.label_for_uid(UID) == "Damien Turner")

    store.close()


# --- Stock -------------------------------------------------------------------

def test_stock():
    print("\n--- stock and the scan loop ---")
    import random

    from fridged.camera import DRINKS, SimCamera

    camera = SimCamera(random.Random(2))
    check("a full shelf starts at capacity",
          all(v == camera.capacity for v in camera.shelf.values()))

    payload, counts, _conf = camera.payload()
    check(f"the INV payload uses the wire's lower-case drink keys ({payload})",
          all(f"{d}=" in payload for d in DRINKS) and "conf=" in payload)
    check("the payload's counts match the shelf", counts == camera.shelf)

    # Restocking is what makes absolute counts self-correcting: no message
    # announces it, the next scan simply reports more drinks.
    for _ in range(200):
        camera.customer_takes()
        camera.maybe_restock()
    check(f"the shelf restocks rather than emptying ({camera.restocks} times)",
          camera.restocks > 0 and sum(camera.shelf.values()) > 0)
    check("counts never go negative",
          all(v >= 0 for v in camera.shelf.values()))

    # The whole loop: board asks, Pi answers, board diffs, sale appears. This is
    # the first BIDIRECTIONAL traffic in the system, and the piece where stock
    # flows Pi -> board rather than the other way.
    store = Store(temp_db()).open()
    board = FakeBoard(seed=6, activity=400.0)
    link = Link(board)
    ingest = Ingest(store, link, SimCamera(random.Random(9)))

    for _ in range(int(6 * 3600 / STEP_S)):
        board.advance(STEP_S)
        while True:
            events = link.poll()
            if not events:
                break
            for event in events:
                ingest.handle(event)
        store.flush()
    store.flush(force=True)

    scans = store._conn.execute(
        "SELECT COUNT(DISTINCT ts) FROM stock_snapshot").fetchone()[0]
    check(f"the board's CMD SCAN gets answered and snapshots are written "
          f"({scans} scans)", scans > 4)

    sold = store._conn.execute("SELECT COALESCE(SUM(qty),0) FROM txn_item "
                               "").fetchone()[0]
    check(f"baskets are derived from the scan difference ({sold} units sold)",
          sold > 0)

    # The strong claim: nothing was sold that the shelf did not lose. Sales are
    # computed from camera differences, so a basket bigger than the drop would
    # mean the diff logic is inventing drinks.
    dropped = store._conn.execute(
        "WITH s AS (SELECT drink, ts, count, "
        "                  LAG(count) OVER (PARTITION BY drink ORDER BY ts) p "
        "           FROM stock_snapshot) "
        "SELECT COALESCE(SUM(MAX(p - count, 0)), 0) FROM s WHERE p IS NOT NULL"
    ).fetchone()[0]
    check(f"units sold never exceed the drop the camera saw "
          f"(sold {sold}, shelf lost {dropped})", sold <= dropped)

    check("every snapshot covers all four drinks",
          store._conn.execute(
              "SELECT MIN(n) FROM (SELECT COUNT(*) n FROM stock_snapshot "
              "GROUP BY ts)").fetchone()[0] == len(DRINKS))
    store.close()


# --- The real camera backend -------------------------------------------------

#: A stand-in for picapture: prints whatever it is told, then optionally exits.
#: Run with this interpreter, so the subprocess machinery below is exercised for
#: real — Popen, the pipes, line buffering, the reader threads and close() — on
#: any machine, with no camera and no OpenCV.
FAKE_PICAPTURE = r"""
import sys, time
lines = sys.argv[1].split("|")
repeat = sys.argv[2] == "repeat"
while True:
    for line in lines:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
        time.sleep(0.02)
    if not repeat:
        break
    sys.stderr.write("still here\n")
    sys.stderr.flush()
"""


def fake_picapture(*lines, repeat=True):
    return [sys.executable, "-c", FAKE_PICAPTURE, "|".join(lines),
            "repeat" if repeat else "once"]


def wait_until(predicate, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


def test_camera_parsing():
    print("\n--- picapture packet parsing ---")
    from fridged.camera import DRINKS, build_payload, parse_packet

    good = "coke:5,fanta:4,mtndew:3,solo:2;conf=87;"
    check(f"a well-formed packet parses ({parse_packet(good)})",
          parse_packet(good) == ({"coke": 5, "fanta": 4, "mtndew": 3,
                                 "solo": 2}, 87))
    check("trailing whitespace and newlines are tolerated",
          parse_packet("  " + good + "\r\n") == parse_packet(good))
    check("a zero count is a count, not a missing one",
          parse_packet("coke:0,fanta:0,mtndew:0,solo:0;conf=100;")[0]
          == {d: 0 for d in DRINKS})

    # The strictness that matters. A picapture built from a different brand list
    # must be REFUSED, not filtered down to the drinks that happen to match:
    # partial acceptance means counts land on the wrong drinks and both ends go
    # on agreeing about the wrong numbers with nothing reporting a fault.
    refusals = {
        "a missing drink": "coke:5,fanta:4,mtndew:3;conf=87;",
        "an unknown drink": "coke:5,fanta:4,mtndew:3,pepsi:2;conf=87;",
        "an extra drink alongside the right four":
            "coke:5,fanta:4,mtndew:3,solo:2,pepsi:1;conf=87;",
        "the same drink twice": "coke:5,coke:4,mtndew:3,solo:2;conf=87;",
        "a negative count": "coke:-1,fanta:4,mtndew:3,solo:2;conf=87;",
        "a non-numeric count": "coke:many,fanta:4,mtndew:3,solo:2;conf=87;",
        "no confidence field": "coke:5,fanta:4,mtndew:3,solo:2;",
        "confidence above 100": "coke:5,fanta:4,mtndew:3,solo:2;conf=101;",
        "a diagnostic line": "  tuning: Coke H 0 S 247 V 190",
        "an empty line": "",
        "a half-written line": "coke:5,fanta:4,mtn",
    }
    for label, line in refusals.items():
        check(f"refused: {label}", parse_packet(line) is None)

    # Both backends must produce byte-identical payloads or the board can tell
    # them apart, which defeats the whole point of the interface.
    counts = {"coke": 5, "fanta": 4, "mtndew": 3, "solo": 2}
    check(f"the INV payload is in DRINKS order "
          f"({build_payload(counts, 87)})",
          build_payload(counts, 87)
          == "coke=5 fanta=4 mtndew=3 solo=2 conf=87")


def test_camera_staleness():
    print("\n--- answering, and refusing to answer (decision D1) ---")
    from fridged import config as cfg
    from fridged.camera import PiCapture

    now = [1000.0]
    camera = PiCapture(autostart=False, clock=lambda: now[0])

    # Never produced a count. Not "0 of everything" — that is a claim about the
    # shelf, and we have no basis for it.
    check("a camera that has never counted refuses to answer",
          camera.payload() is None)

    # One frame is not a settled shelf, and saying so is the point of stage 4:
    # a lone frame is exactly what a hand halfway into the fridge produces.
    camera._accept("coke:5,fanta:4,mtndew:3,solo:2;conf=90;")
    payload, counts, _conf = camera.payload()
    lone = int(payload.split("conf=")[1])
    check(f"a single unagreed frame answers, but marked down ({lone} of 90)",
          0 < lone < 90 and counts["coke"] == 5)

    # ...and once the frames agree, the mark-down goes away.
    for _ in range(cfg.SETTLE_FRAMES):
        camera._accept("coke:5,fanta:4,mtndew:3,solo:2;conf=90;")
    payload, counts, _conf = camera.payload()
    check(f"a settled shelf answers at full confidence ({payload})",
          "conf=90" in payload and counts["coke"] == 5)

    # Wobbling: still running, just behind. Answer anyway — this is the common
    # case and stopping the fridge for it would mean stopping it constantly.
    now[0] += (cfg.CAMERA_STALE_S + cfg.CAMERA_DEAD_S) / 2
    payload, _counts, _conf = camera.payload()
    stale_conf = int(payload.split("conf=")[1])
    check(f"a stale count still answers, at reduced confidence "
          f"({stale_conf} was 90)", 0 < stale_conf < 90)

    # Dead: refuse. The board faults and goes out of service, which is the
    # honest state for a fridge that cannot see its shelf.
    now[0] += cfg.CAMERA_DEAD_S
    check("a count older than CAMERA_DEAD_S is not answered with at all",
          camera.payload() is None)

    # ...and it recovers on its own the moment counts come back, rather than
    # needing a restart.
    camera._accept("coke:1,fanta:1,mtndew:1,solo:1;conf=80;")
    check("a fresh count after a dead spell answers again",
          camera.payload() is not None)

    check("malformed lines are counted, not silently dropped",
          (camera._accept("rubbish"), camera.malformed == 1)[1])
    check("...and do not become the answer",
          camera.payload()[1] == {"coke": 1, "fanta": 1, "mtndew": 1,
                                  "solo": 1})
    camera.close()


def test_camera_subprocess():
    print("\n--- picapture as a subprocess ---")
    from fridged.camera import PiCapture

    # The whole machinery for real: Popen, pipes, both reader threads, parsing,
    # and close(). Only the OpenCV is stubbed out.
    camera = PiCapture(command=fake_picapture(
        "coke:5,fanta:4,mtndew:3,solo:2;conf=88;"), workdir=".")
    try:
        check("counts arrive from a real child process",
              wait_until(lambda: camera.packets > 0))
        answer = camera.payload()
        check(f"and become an INV payload ({answer[0] if answer else None})",
              answer is not None and answer[1]["coke"] == 5)
        check("the age of the newest count is known",
              camera.age_s is not None and camera.age_s < 5.0)
    finally:
        camera.close()
    check("close() stops the child",
          camera._process is None or camera._process.poll() is not None)

    # picapture's stdout is not a pure count stream: the debug modes print
    # tuning readouts to it. Those must be dropped, and must not become counts.
    camera = PiCapture(command=fake_picapture(
        "  tuning:",
        "    1 Coke  H 0 S 247 V 190",
        "coke:1,fanta:2,mtndew:3,solo:4;conf=70;"), workdir=".")
    try:
        check("diagnostics on stdout are filtered out",
              wait_until(lambda: camera.packets > 0 and camera.malformed > 0))
        check("only the packet became the answer",
              camera.payload()[1] == {"coke": 1, "fanta": 2, "mtndew": 3,
                                      "solo": 4})
    finally:
        camera.close()

    # A picapture that dies must be restarted, or the fridge silently stops
    # being able to answer for the rest of the day.
    camera = PiCapture(command=fake_picapture(
        "coke:2,fanta:2,mtndew:2,solo:2;conf=60;", repeat=False), workdir=".")
    try:
        check("a picapture that exits is restarted",
              wait_until(lambda: camera.starts >= 2, timeout=20.0))
    finally:
        camera.close()

    # A missing binary must say what to do about it, not raise
    # FileNotFoundError out of a daemon thread where nobody ever sees it and
    # the service goes on believing it has a camera.
    camera = PiCapture(command=["./definitely-not-built"], workdir=".")
    try:
        # The supervisor has to SURVIVE the failure and keep retrying — a dead
        # supervisor means the camera never comes back even once the binary is
        # built, and nothing would say so.
        check("a missing binary leaves the supervisor running, retrying",
              wait_until(lambda: camera._threads and
                         all(t.is_alive() for t in camera._threads)))
        check("...having started nothing and produced no counts",
              camera.starts == 0 and camera.packets == 0)
        check("...and it answers no scan rather than inventing one",
              camera.payload() is None)
    finally:
        camera.close()


def test_camera_in_the_loop():
    print("\n--- the real backend answering a real board ---")
    from fridged.camera import PiCapture

    # End to end with everything except OpenCV: a simulated board sends
    # CMD SCAN over a Link, and the answer comes from counts that travelled out
    # of a child process's stdout.
    store = Store(temp_db()).open()
    board = FakeBoard(seed=11, activity=400.0)
    link = Link(board)
    camera = PiCapture(command=fake_picapture(
        "coke:6,fanta:5,mtndew:4,solo:3;conf=91;"), workdir=".")
    try:
        wait_until(lambda: camera.packets > 0)
        ingest = Ingest(store, link, camera)

        for _ in range(int(3 * 3600 / STEP_S)):
            board.advance(STEP_S)
            while True:
                events = link.poll()
                if not events:
                    break
                for event in events:
                    ingest.handle(event)
            store.flush()
        store.flush(force=True)
    finally:
        camera.close()

    snapshots = store._conn.execute(
        "SELECT COUNT(DISTINCT ts) FROM stock_snapshot").fetchone()[0]
    check(f"the board's scans are answered from the subprocess "
          f"({snapshots} snapshots)", snapshots > 2)
    check("the stored counts are the ones picapture printed",
          store._conn.execute(
              "SELECT DISTINCT count FROM stock_snapshot WHERE drink='coke'"
          ).fetchall() == [(6,)])
    check("every message type still has a handler", ingest.unhandled == {})
    store.close()


def _settled(camera, counts_line, frames=None):
    """Feed enough identical frames that the shelf counts as holding still."""
    from fridged import config as cfg
    for _ in range(frames or cfg.SETTLE_FRAMES):
        camera._accept(counts_line)


def test_baseline_latch():
    print("\n--- the baseline is frozen before the door swings ---")
    from fridged import config as cfg
    from fridged.camera import PENDING, PiCapture

    now = [1000.0]
    camera = PiCapture(autostart=False, clock=lambda: now[0])

    # A full shelf, held still, with the door shut.
    _settled(camera, "coke:5,fanta:5,mtndew:5,solo:5;conf=95;")

    camera.door_opened(now[0])

    # THE FRAME THIS STAGE EXISTS TO IGNORE. The door is swinging, the light is
    # changing and a hand is across the shelf, so the camera reports two Cokes
    # missing that are still there. Before stage 4 this became the baseline, and
    # the recount would then have shown a shelf that had GAINED two cans.
    camera._accept("coke:3,fanta:5,mtndew:5,solo:5;conf=60;")

    payload, counts, _conf = camera.payload()
    check(f"the baseline scan answers from before the door moved ({payload})",
          counts["coke"] == 5)
    check("...at the confidence of that settled reading, not the bad frame",
          "conf=95" in payload)

    # THE BUG THIS TEST EXISTS FOR, found on hardware.
    #
    # A shelf that has not moved is the BEST possible baseline, and it was being
    # graded as the worst. Every frame of a still shelf agrees, so `_stable` is
    # reassigned to `_latest` each time and the two become the same object; a
    # "settled" test written as `latched is not latest` then reported unsettled
    # precisely when nothing had moved, and halved the confidence. On the fridge
    # every baseline came back at 40 against a shelf measured stable for 567
    # consecutive frames.
    steady = PiCapture(autostart=False, clock=lambda: now[0])
    for _ in range(30):                    # a long, completely still period
        now[0] += 0.25
        steady._accept("coke:5,fanta:5,mtndew:5,solo:5;conf=92;")
    steady.door_opened(now[0])
    payload, _counts, _conf = steady.payload()
    check(f"a shelf that never moved gives a FULL-confidence baseline "
          f"({payload})", "conf=92" in payload)
    steady.close()

    # ...and the genuinely unsettled case must still be marked down, or the fix
    # would have simply inverted the bug.
    jumpy = PiCapture(autostart=False, clock=lambda: now[0])
    for i in range(6):                     # never two frames the same
        now[0] += 0.25
        jumpy._accept(f"coke:{i},fanta:5,mtndew:5,solo:5;conf=92;")
    jumpy.door_opened(now[0])
    check("a shelf that never held still is still marked down",
          int(jumpy.payload()[0].split("conf=")[1]) < 92)
    jumpy.close()

    # Repeated scans while the door is open keep getting the frozen value: the
    # board can legitimately rescan, and the answer must not drift.
    camera._accept("coke:2,fanta:4,mtndew:5,solo:5;conf=55;")
    check("a second scan while the door is open gets the same baseline",
          camera.payload()[1]["coke"] == 5)

    # A BASELINE IS SUPPOSED TO BE OLD. Somebody standing at an open fridge
    # deciding is normal — the board allows thirty seconds of it — and the
    # baseline describes a moment when nothing could have changed the shelf, so
    # the time since then is not evidence against it.
    #
    # Measuring its age up to the present instead of up to the moment it was
    # frozen took a healthy camera and a deliberating customer to conf=0.
    patient = PiCapture(autostart=False, clock=lambda: now[0])
    _settled(patient, "coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    patient.door_opened(now[0])
    for _ in range(60):                    # 15 s of an open door, camera fine
        now[0] += 0.25
        patient._accept("coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    check("a long deliberation does not decay the baseline's confidence",
          "conf=95" in patient.payload()[0])

    # ...but a baseline that was ALREADY stale when it was frozen still is.
    now[0] += 100.0
    stale = PiCapture(autostart=False, clock=lambda: now[0])
    _settled(stale, "coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    now[0] += cfg.CAMERA_STALE_S * 2
    stale.door_opened(now[0])
    stale._accept("coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    check("a baseline frozen from an already-stale reading is marked down",
          int(stale.payload()[0].split("conf=")[1]) < 95)
    patient.close()
    stale.close()

    # A door that opens before the camera has ever counted cannot be answered —
    # inventing a full shelf would be the worst possible guess.
    fresh = PiCapture(autostart=False, clock=lambda: now[0])
    fresh.door_opened(now[0])
    check("a door opening before the first count cannot be answered",
          fresh.payload() is None)
    fresh.close()
    camera.close()


def test_settling():
    print("\n--- the recount waits for the picture to hold still ---")
    from fridged import config as cfg
    from fridged.camera import PENDING, PiCapture

    now = [2000.0]
    camera = PiCapture(autostart=False, clock=lambda: now[0])
    _settled(camera, "coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    camera.door_opened(now[0])
    camera.payload()                       # the baseline
    camera.door_closed(now[0])

    # One can genuinely taken, but the scene has not settled: the door has just
    # moved and the light is still changing back.
    camera._accept("coke:4,fanta:5,mtndew:5,solo:5;conf=80;")
    check("one frame after the door shuts is not an answer yet",
          camera.payload() is PENDING)

    # A frame that disagrees restarts the run — this is the hand still in shot.
    camera._accept("coke:3,fanta:5,mtndew:5,solo:5;conf=70;")
    check("a disagreeing frame keeps it waiting",
          camera.payload() is PENDING)

    _settled(camera, "coke:4,fanta:5,mtndew:5,solo:5;conf=88;")
    payload, counts, _conf = camera.payload()
    check(f"once the frames agree, the recount is answered ({payload})",
          counts["coke"] == 4 and "conf=88" in payload)

    # The agreement run must be reset at door_closed, or a run that began BEFORE
    # the door shut would satisfy the recount instantly — with exactly the
    # pre-door frames this is meant to exclude.
    camera.door_opened(now[0])
    camera.payload()
    camera.door_closed(now[0])
    check("a run of agreement from before the door shut does not count",
          camera.payload() is PENDING)
    camera.close()


def test_settling_deadline():
    print("\n--- settling gives up rather than letting the board fault ---")
    from fridged import config as cfg
    from fridged.camera import PENDING, PiCapture

    now = [3000.0]
    camera = PiCapture(autostart=False, clock=lambda: now[0])
    _settled(camera, "coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    camera.door_closed(now[0])

    # A shelf that never holds still: every frame disagrees with the last.
    for i in range(6):
        camera._accept(f"coke:{i % 3},fanta:5,mtndew:5,solo:5;conf=90;")
    check("a shelf that will not hold still keeps the reply open",
          camera.payload() is PENDING)

    now[0] += cfg.SETTLE_TIMEOUT_S
    answer = camera.payload()
    check("past SETTLE_TIMEOUT_S it answers anyway rather than faulting",
          answer is not None and answer is not PENDING)
    forced = int(answer[0].split("conf=")[1])
    check(f"...marked down, because it never settled ({forced} of 90)",
          0 < forced < 90)

    # The whole point of the mark-down: a dashboard has to be able to tell this
    # apart from a clean read, because the counts themselves look identical.
    check("an unsettled answer scores below a settled one",
          forced < 90 * cfg.UNSETTLED_CONFIDENCE_SCALE + 1)

    # A camera that has gone dead during settling must still refuse, not wait
    # out the timeout to then answer from a corpse.
    camera.door_closed(now[0])
    now[0] += cfg.CAMERA_DEAD_S
    check("a camera that dies while settling refuses instead of waiting",
          camera.payload() is None)
    camera.close()


def test_deferred_scan():
    print("\n--- the service holds a scan open and answers it later ---")
    from fridged import config as cfg
    from fridged.camera import PiCapture

    now = [4000.0]
    store = Store(temp_db()).open()
    transport = StubTransport()
    link = Link(transport)
    camera = PiCapture(autostart=False, clock=lambda: now[0])
    ingest = Ingest(store, link, camera, clock=lambda: now[0])

    _settled(camera, "coke:5,fanta:5,mtndew:5,solo:5;conf=95;")
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=closed", ms=10))
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=11))

    check("an unsettled scan is deferred, not answered",
          ingest.scan_owed is not None and
          b"INV" not in bytes(transport.written))
    check("...and nothing is written to the database yet",
          (store.flush(force=True),
           store._conn.execute(
               "SELECT COUNT(*) FROM stock_snapshot").fetchone()[0] == 0)[1])

    # tick() is what finishes it. Deliberately NOT on the self-metric timer —
    # a reply that waited ten seconds for telemetry would arrive after the
    # board had already faulted.
    ingest.tick()
    check("tick() with nothing settled yet still does not answer",
          b"INV" not in bytes(transport.written))

    _settled(camera, "coke:4,fanta:5,mtndew:5,solo:5;conf=88;")
    ingest.tick()
    store.flush(force=True)
    check("once it settles, tick() sends the deferred reply",
          b"INV" in bytes(transport.written))
    check("...the reply carries the settled counts",
          b"coke=4" in bytes(transport.written))
    check("...the snapshot is written with them",
          store._conn.execute(
              "SELECT count FROM stock_snapshot WHERE drink='coke'"
          ).fetchone()[0] == 4)
    check("and nothing is still owed", ingest.scan_owed is None)
    store.close()
    camera.close()


def test_deferred_scan_deadline():
    print("\n--- the deferred scan has a backstop ---")
    from fridged import config as cfg
    from fridged.camera import PENDING, PiCapture

    now = [5000.0]
    store = Store(temp_db()).open()
    transport = StubTransport()
    link = Link(transport)

    # A camera that never settles and never gives up on its own, so the only
    # thing that can end this is the service's own budget. Without the backstop
    # the reply would be held forever and the board would fault every time.
    class NeverSettles(PiCapture):
        def payload(self, force=False):
            return PENDING if not force else (
                "coke=1 fanta=1 mtndew=1 solo=1 conf=10",
                {"coke": 1, "fanta": 1, "mtndew": 1, "solo": 1}, 10)

    camera = NeverSettles(autostart=False, clock=lambda: now[0])
    ingest = Ingest(store, link, camera, clock=lambda: now[0])

    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=10))
    check("held open while inside the budget", ingest.scan_owed is not None)

    for _ in range(5):
        ingest.tick()
    check("...and stays held while inside it",
          b"INV" not in bytes(transport.written))

    now[0] += cfg.SCAN_ANSWER_BUDGET_S
    ingest.tick()
    store.flush(force=True)
    check("past SCAN_ANSWER_BUDGET_S a reply is forced out",
          b"INV" in bytes(transport.written))
    check("and the scan is no longer owed", ingest.scan_owed is None)

    # The budget has to fit inside the board's own patience, or the backstop
    # fires after the fault it exists to prevent.
    check(f"the budget is inside the board's RECOUNT_TIMEOUT_MS "
          f"({cfg.SCAN_ANSWER_BUDGET_S}s of 8s)",
          cfg.SCAN_ANSWER_BUDGET_S < 8.0 and
          cfg.SETTLE_TIMEOUT_S < cfg.SCAN_ANSWER_BUDGET_S)

    # A camera that will not answer even when forced must not hold the reply
    # open forever — that would be a permanent silent stall.
    class NeverAnswers(PiCapture):
        def payload(self, force=False):
            return PENDING

    stubborn = NeverAnswers(autostart=False, clock=lambda: now[0])
    ingest2 = Ingest(store, link, stubborn, clock=lambda: now[0])
    feed(ingest2, link, protocol.build("CMD", "SCAN", "", ms=20))
    now[0] += cfg.SCAN_ANSWER_BUDGET_S
    ingest2.tick()
    check("a camera that ignores force does not stall the service forever",
          ingest2.scan_owed is None)
    store.close()
    camera.close()
    stubborn.close()


def test_simcamera_never_defers():
    print("\n--- the simulated shelf still answers immediately ---")
    import random

    from fridged.camera import PENDING, SimCamera

    # Every existing test depends on this, and so does every demo: the sim path
    # must not have acquired stage 4's latency.
    camera = SimCamera(random.Random(4))
    camera.door_opened(1.0)
    check("a simulated baseline answers at once",
          camera.payload() is not PENDING)
    camera.door_closed(1.0)
    answer = camera.payload()
    check("a simulated recount answers at once",
          answer is not PENDING and answer is not None)
    check("and it accepts force= like the real one does",
          camera.payload(force=True) is not None)
    camera.close()


def test_full_door_cycle():
    print("\n--- a whole door cycle, end to end ---")
    from fridged.camera import PENDING, PiCapture

    # The thing the entire project is for: door opens, a can leaves, door
    # shuts, and the difference between the two answers is what gets charged.
    # Driven through Ingest and Link with a fake clock, so the timing is exact
    # rather than hoped for.
    now = [6000.0]
    store = Store(temp_db()).open()
    transport = StubTransport()
    link = Link(transport)
    camera = PiCapture(autostart=False, clock=lambda: now[0])
    ingest = Ingest(store, link, camera, clock=lambda: now[0])

    def frames(line, count=4, step=0.25):
        for _ in range(count):
            now[0] += step
            camera._accept(line)

    full = "coke:5,fanta:5,mtndew:5,solo:5;conf=94;"
    minus_one = "coke:4,fanta:5,mtndew:5,solo:5;conf=94;"

    frames(full, count=8)                          # a settled, shut fridge

    # --- the door opens ---
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=100))
    # ...and the very next frames are rubbish: light flooding in, a hand across
    # the shelf. These are exactly what the latch exists to exclude.
    camera._accept("coke:2,fanta:3,mtndew:5,solo:5;conf=40;")
    now[0] += 0.25
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=200))

    baseline = bytes(transport.written)
    check("the baseline answers a full shelf, not the hand in the way",
          b"coke=5" in baseline)

    # --- the customer takes one Coke and shuts the door ---
    now[0] += 6.0                                  # deliberating
    camera._accept(minus_one)
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=closed", ms=8000))
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=8100))

    check("the recount is deferred while the picture settles",
          ingest.scan_owed is not None)

    before = len(bytes(transport.written))
    frames(minus_one, count=4)
    ingest.tick()
    store.flush(force=True)

    recount = bytes(transport.written)[before:]
    check(f"the recount answers one Coke short ({recount.decode().strip()})",
          b"coke=4" in recount)
    check("and it is not owed any more", ingest.scan_owed is None)

    counts = dict(store._conn.execute(
        "SELECT count, COUNT(*) FROM stock_snapshot WHERE drink='coke' "
        "GROUP BY count"))
    check(f"both scans are on record, and they differ by exactly one can "
          f"({counts})", counts == {5: 1, 4: 1})

    # The whole cycle has to fit the board's budget, or the fault fires before
    # the answer does. Measured, not assumed.
    check("the reply came inside the board's 8 s recount window",
          config.SETTLE_TIMEOUT_S < 8.0 and
          config.SCAN_ANSWER_BUDGET_S < 8.0)
    store.close()
    camera.close()


def test_confidence_is_recorded():
    print("\n--- the confidence and the trigger reach the database ---")
    from fridged import config as cfg
    from fridged.camera import PiCapture

    now = [7000.0]
    store = Store(temp_db()).open()
    link = Link(StubTransport())
    camera = PiCapture(autostart=False, clock=lambda: now[0])
    ingest = Ingest(store, link, camera, clock=lambda: now[0])

    def scans():
        return dict(store._conn.execute(
            "SELECT trigger, COUNT(DISTINCT ts) FROM stock_snapshot "
            "GROUP BY trigger"))

    def confidences():
        return [r[0] for r in store._conn.execute(
            "SELECT value FROM measurement WHERE metric = 'camera.confidence' "
            "ORDER BY ts")]

    _settled(camera, "coke:5,fanta:5,mtndew:5,solo:5;conf=93;")

    # --- baseline ---
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=10))
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=11))
    store.flush(force=True)
    check(f"a scan after the door opens is stored as a baseline ({scans()})",
          scans() == {"door_open": 1})

    # THE POINT OF STAGE 5: it went out on the wire and was thrown away. The
    # counts alone cannot tell a settled answer from one forced out on a
    # deadline, so without this a disputed charge has no record behind it.
    check(f"...with the confidence beside it ({confidences()})",
          confidences() == [93.0])

    # --- recount ---
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=closed", ms=20))
    _settled(camera, "coke:4,fanta:5,mtndew:5,solo:5;conf=88;")
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=21))
    store.flush(force=True)
    check(f"a scan after the door shuts is stored as a recount ({scans()})",
          scans() == {"door_open": 1, "door_close": 1})
    check("the two scans are distinguishable in the data, not just in the log",
          store._conn.execute(
              "SELECT COUNT(DISTINCT trigger) FROM stock_snapshot"
          ).fetchone()[0] == 2)

    # --- a deferred reply must keep the trigger it was ASKED under ---
    #
    # The subtle one. A deferred reply can go out seconds after the scan, by
    # which time the door may have moved again. Recording the door state at
    # send time would file a baseline as a recount and quietly corrupt the one
    # column that says which of the two readings a snapshot is.
    feed(ingest, link, protocol.build("EVT", "DOOR", "state=open", ms=30))
    camera.door_closed(now[0])          # force the camera into settling
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=31))
    check("the scan is deferred", ingest.scan_owed is not None)

    feed(ingest, link, protocol.build("EVT", "DOOR", "state=closed", ms=40))
    _settled(camera, "coke:3,fanta:5,mtndew:5,solo:5;conf=77;")
    ingest.tick()
    store.flush(force=True)
    check(f"a deferred reply keeps the trigger from when it was asked "
          f"({scans()})", scans()["door_open"] == 2)

    # --- a scan nobody opened a door for ---
    fresh_store = Store(temp_db()).open()
    fresh = Ingest(fresh_store, Link(StubTransport()), camera,
                   clock=lambda: now[0])
    feed(fresh, fresh.link, protocol.build("CMD", "SCAN", "", ms=5))
    fresh_store.flush(force=True)
    check("a scan with no door behind it is labelled honestly",
          [r[0] for r in fresh_store._conn.execute(
              "SELECT DISTINCT trigger FROM stock_snapshot")] == ["untriggered"])

    # The dashboard hardcodes this same number; they have to agree or the panel
    # counts a different population from the one the log warns about.
    check(f"the low-confidence threshold is the unsettled score "
          f"({cfg.LOW_CONFIDENCE_THRESHOLD})",
          cfg.LOW_CONFIDENCE_THRESHOLD == int(
              100 * cfg.UNSETTLED_CONFIDENCE_SCALE))

    fresh_store.close()
    store.close()
    camera.close()


def test_camera_selection():
    print("\n--- choosing a camera ---")
    from fridged.__main__ import build_camera, parse_args
    from fridged.camera import PiCapture, SimCamera

    # The safe option is the one you get by saying nothing. Same asymmetry as
    # --port and --square: a deployment that never asks for the real camera
    # cannot end up running on fabricated stock.
    check("the default is the simulated shelf",
          parse_args(["--port", "sim"]).camera == "sim")
    check("...and it builds a SimCamera",
          isinstance(build_camera(parse_args([])), SimCamera))

    check("an unknown camera is refused rather than defaulted",
          _rejects_args(["--camera", "webcam"]))

    # Independent of --port on purpose: real camera, simulated board is the
    # useful hybrid for testing vision without standing at the fridge.
    camera = build_camera(parse_args(["--port", "sim",
                                      "--camera", "picapture"]))
    try:
        check("--camera picapture builds the real backend",
              isinstance(camera, PiCapture))
        check("...pointed at the configured binary and working directory",
              camera.command[0] == str(config.PICAPTURE_BINARY) and
              "--headless" in camera.command and
              camera.workdir == str(config.PICAPTURE_DIR))
    finally:
        camera.close()

    # Both backends must be closeable the same way, because the service's
    # shutdown path calls close() without knowing which one it has.
    sim = build_camera(parse_args([]))
    sim.close()
    check("both backends share door_opened/door_closed/payload/close",
          all(hasattr(sim, name) and hasattr(camera, name)
              for name in ("door_opened", "door_closed", "payload", "close")))


def _rejects_args(argv):
    from fridged.__main__ import parse_args
    try:
        parse_args(argv)
    except SystemExit:
        return True
    return False


def test_camera_cannot_answer():
    print("\n--- a scan that cannot be answered ---")
    from fridged.camera import PiCapture

    # The board must be left to fault rather than handed a made-up count. This
    # is the one place in the system where being quietly wrong moves money.
    store = Store(temp_db()).open()
    transport = StubTransport()
    link = Link(transport)
    camera = PiCapture(autostart=False)          # never produced a count
    ingest = Ingest(store, link, camera)

    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=10))
    store.flush(force=True)

    check("no INV is sent when the camera cannot answer",
          b"INV" not in bytes(transport.written))
    check("and no stock snapshot is invented",
          store._conn.execute(
              "SELECT COUNT(*) FROM stock_snapshot").fetchone()[0] == 0)

    # The same scan, once counts exist, must be answered normally — the refusal
    # is about not knowing, not a latched failure.
    camera._accept("coke:3,fanta:3,mtndew:3,solo:3;conf=95;")
    feed(ingest, link, protocol.build("CMD", "SCAN", "", ms=20))
    store.flush(force=True)
    check("the same board is answered once counts exist",
          b"INV" in bytes(transport.written))
    check("and the snapshot is written",
          store._conn.execute(
              "SELECT COUNT(*) FROM stock_snapshot").fetchone()[0] == 4)
    store.close()


# --- Card payments -----------------------------------------------------------

def wait_for(service, kinds, timeout=20.0):
    """Collect results until every kind in `kinds` has been seen, or time out."""
    seen = {}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not set(kinds) <= set(seen):
        for result in service.drain():
            seen[result[0]] = result
        time.sleep(0.05)
    return seen


def test_payments():
    print("\n--- card payments ---")
    from fridged.payments import BACKENDS, FakeSquare, PaymentService

    check("both backends are selectable by name",
          set(BACKENDS) == {"fake", "real"})

    # The happy path: link, then payment.
    service = PaymentService(FakeSquare())
    service.request_link(7, 400, "2xCoke")
    seen = wait_for(service, ("url", "paid"))
    check("a checkout link comes back", "url" in seen and seen["url"][1] == 7)
    check("and the payment is reported once Square shows it",
          "paid" in seen and seen["paid"][1] == 7)
    service.close()

    # Cancelling a link that EXISTS.
    service = PaymentService(FakeSquare())
    service.request_link(8, 200, "1xFanta")
    wait_for(service, ("url",))
    service.request_cancel(8)
    seen = wait_for(service, ("cancelled",), timeout=5.0)
    check("cancelling an existing link is acknowledged",
          "cancelled" in seen and seen["cancelled"][2] is True)
    check("a cancelled order is not then reported as paid",
          "paid" not in wait_for(service, ("nothing",), timeout=3.0))
    service.close()

    # THE RACE THAT MATTERS. The board can give up while the Square API call is
    # still in flight, so the cancel names a link that does not exist yet. If
    # the cancellation is dropped, a live payable checkout is orphaned and
    # nothing will ever kill it (plan.md stage 15.3).
    service = PaymentService(FakeSquare())
    service.request_cancel(9)          # cancel FIRST
    time.sleep(0.05)
    service.request_link(9, 200, "1xCoke")
    seen = wait_for(service, ("cancelled",), timeout=8.0)
    check("a cancel arriving BEFORE the link still cancels it",
          "cancelled" in seen)
    check("...and no URL is handed to the board for a dead checkout",
          "url" not in seen)
    service.close()

    # A backend that throws must produce SQUARE_ERR, not a hung terminal.
    class Broken(FakeSquare):
        def create(self, txn_id, cents, description):
            raise RuntimeError("square is down")

    service = PaymentService(Broken())
    service.request_link(10, 200, "1xCoke")
    seen = wait_for(service, ("error",), timeout=5.0)
    check("a backend failure is reported as an error, not silence",
          "error" in seen and seen["error"][1] == 10)
    service.close()

    # And the frames those results turn into.
    store = Store(temp_db()).open()
    link = Link(StubTransport())
    service = PaymentService(FakeSquare())
    ingest = Ingest(store, link, None, service)
    ingest.boot_id = store.open_boot(time.time(), "0.2")
    feed(ingest, link, protocol.build(
        "CMD", "SQUARE_LINK", "cents=400 id=11 desc=2xCoke", ms=1))

    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        ingest.drain_payments()
        if b"SQUARE_PAID" in link.transport.written:
            break
        time.sleep(0.05)
    sent = link.transport.written.decode("utf-8", "replace")
    check("CMD SQUARE_LINK is answered with RSP SQUARE_URL",
          "RSP" in sent and "SQUARE_URL" in sent and "id=11" in sent)
    check("and EVT SQUARE_PAID follows when the money arrives",
          "SQUARE_PAID" in sent)
    service.close()
    store.close()


# --- Grafana dashboards -----------------------------------------------------

#: How `frser-sqlite-datasource` expands its ONLY grouping macro:
#: `$__unixEpochGroupSeconds("ts", 60)` becomes `cast(("ts" / 60) as int) * 60`.
GROUP_MACRO = re.compile(
    r'\$__unixEpochGroupSeconds\(\s*("[^"]+")\s*,\s*(\d+)\s*\)')


def expand_grafana(sql, now):
    """Substitute the variables Grafana interpolates, as Grafana interpolates them.

    `${__from:date:seconds}` and `${__to:date:seconds}` are Grafana *global*
    variables and yield a unix-seconds epoch, which is what this schema stores.
    The plugin itself supports exactly one macro, above.

    Deliberately NOT supported, and the reason this function exists as a check
    rather than a convenience: `$__timeFilter()`, `$__timeFrom()` and
    `$__unixEpochFrom()` — all of which are common in Grafana SQL and none of
    which this plugin implements. dashboard.md section 5.1 shipped an example
    query using `$__timeFilter(ts)` that would never have run.
    """
    sql = sql.replace("${__from:date:seconds}", str(int(now - 86400)))
    sql = sql.replace("${__to:date:seconds}", str(int(now)))
    return GROUP_MACRO.sub(r"cast((\1 / \2) as int) * \2", sql)


def test_dashboards():
    """Every panel query must parse and run against the real schema.

    Grafana runs on the Pi and nowhere else, so a broken panel would otherwise
    not be discovered until deployment — by which point the failure is a blank
    graph with a message in a container log. Running the SQL here turns that into
    a test failure on a laptop.
    """
    print("\n--- grafana dashboards ---")
    root = Path(__file__).resolve().parent.parent
    files = sorted((root / "grafana" / "dashboards").glob("*.json"))
    check(f"dashboard JSON files exist ({len(files)})", bool(files))

    store = Store(temp_db()).open()

    # A fixture covering EVERY table the panels touch. Without the door and
    # health rows the queries still parsed and still "passed" while returning
    # nothing — which is exactly the failure mode a panel has on the Pi, so a
    # test that cannot tell the two apart is not testing the thing that matters.
    now = time.time()
    for offset in range(0, 600, 30):
        for rom, base in (("28FF3A1C92160341", -18.0),
                          ("28FF7B4E5501A2C7", 4.0),
                          ("28FFD1082B640F19", 6.2)):
            store.note_sensor(rom, now - offset)
            store.measurement(now - offset, f"temp.rom.{rom}", base)
        for metric, value in (("temp.rp2040_die", 32.0),
                              ("temp.pi_soc", 52.0),
                              ("coinbox.mass_g", 120.0),
                              ("link.age_s", 1.2),
                              ("health.faults", 0.0)):
            store.measurement(now - offset, metric, value)

    store._conn.execute(
        "UPDATE sensor SET zone_label = CASE rom_code "
        "WHEN '28FF3A1C92160341' THEN 'freezer' "
        "WHEN '28FF7B4E5501A2C7' THEN 'fridge_top' "
        "ELSE 'fridge_bottom' END")

    store.door_opened(now - 500)
    store.door_closed(now - 480)
    store.door_opened(now - 300)
    store.door_closed(now - 90)

    boot_id = store.open_boot(now - 600, "0.2")

    # Two cards: one named, one never named. Both are needed — the members
    # table sorts unnamed cards to the top, and a fixture with only named ones
    # would leave that half of the panel unexercised.
    NAMED_UID, UNNAMED_UID = "04404C22C06780", "DEADBEEF01"
    store.rfid_event(now - 470, NAMED_UID, True)
    store.rfid_event(now - 310, NAMED_UID, True)
    store.rfid_event(now - 250, UNNAMED_UID, True)
    store.rfid_event(now - 240, "BADCARD99", False)
    store._conn.execute("UPDATE member SET label = ? WHERE uid = ?",
                        ("Damien Turner", NAMED_UID))

    # One cash sale, one card sale, one walked away from — enough for every
    # transaction panel to have something in each of its slices. The third is
    # left unattributed on purpose: 'other' is a real category, and a fixture
    # where every sale has a name would never show it was rendered.
    store.upsert_txn(boot_id, 1001, ts_start=now - 460, ts_end=now - 430,
                     method="cash", owed_cents=400, paid_cents=400,
                     outcome="paid", member_uid=NAMED_UID)
    # Keyed by WIRE name, as the board sends them: the `items=` list on a
    # TXN_START carries catalogue::wire_key(), not the display name.
    store.set_txn_items(boot_id, 1001, {"coke": 1, "fanta": 1}, 200)
    store.upsert_txn(boot_id, 1002, ts_start=now - 300, ts_end=now - 260,
                     method="card", owed_cents=200, paid_cents=200,
                     outcome="paid", member_uid=UNNAMED_UID)
    store.set_txn_items(boot_id, 1002, {"mtndew": 1}, 200)
    store.upsert_txn(boot_id, 1003, ts_start=now - 200, ts_end=now - 80,
                     owed_cents=200, paid_cents=0, outcome="stolen")
    store.set_txn_items(boot_id, 1003, {"solo": 1}, 200)
    store.coin_event(now - 450, boot_id, 1001, 200, 6.60, True)
    store.coin_event(now - 445, boot_id, 1001, 200, 6.60, True)
    store.coin_event(now - 440, boot_id, 1001, None, 3.10, False)

    # Stock over three scans, with a restock in the middle, so the
    # reconciliation panel's restock-detection has something to detect.
    #
    # Each one carries its trigger and a matching `camera.confidence` reading at
    # the SAME ts, because that is how the real thing writes them — the
    # confidence panel joins the two on timestamp, so a fixture that wrote only
    # one of the pair would leave the join untested and the panel blank on the
    # Pi. One low reading is included so the "nobody should trust" stat has
    # something to count; a fixture where everything is healthy would never show
    # that panel renders.
    for scan_ts, trigger, counts, conf in (
            (now - 500, "door_open",  {"coke": 6, "fanta": 6, "mtndew": 6, "solo": 6}, 93.0),
            (now - 300, "door_close", {"coke": 5, "fanta": 5, "mtndew": 5, "solo": 6}, 88.0),
            (now - 100, "door_open",  {"coke": 6, "fanta": 6, "mtndew": 6, "solo": 6}, 41.0)):
        store.stock_snapshot(scan_ts, counts, trigger=trigger)
        store.measurement(scan_ts, "camera.confidence", conf)
    store.flush(force=True)

    for path in files:
        dashboard = json.loads(path.read_text(encoding="utf-8"))
        check(f"{path.name}: has a stable uid and a title",
              bool(dashboard.get("uid")) and bool(dashboard.get("title")))

        for panel in dashboard.get("panels", []):
            title = panel.get("title", "?")
            for target in panel.get("targets", []):
                sql = expand_grafana(target["queryText"], now)
                check(f"{title[:38]}: no unexpanded Grafana syntax left",
                      "$__" not in sql and "${" not in sql)
                try:
                    rows = store._conn.execute(sql).fetchall()
                    ok, detail = True, f"{len(rows)} rows"
                except sqlite3.Error as exc:
                    ok, detail = False, str(exc)
                check(f"{title[:38]}: query runs ({detail})", ok)

                # A query that parses and returns nothing is a blank panel, and
                # blank is how a broken panel looks on the Pi. The fixture above
                # populates every table these panels read, so zero rows here
                # means the query is wrong, not that the fridge is quiet.
                if ok:
                    check(f"{title[:38]}: returns data for the fixture",
                          len(rows) > 0)

                # A time column named here but absent from the result means the
                # panel plots nothing, with no error anywhere.
                if target.get("timeColumns"):
                    names = [c[0] for c in
                             store._conn.execute(sql).description]
                    check(f"{title[:38]}: declares time columns that exist",
                          all(c in names for c in target["timeColumns"]))

            # A panel pointing at a uid the provisioning file does not define
            # comes up as "Datasource not found" on a fresh container.
            uid = (panel.get("datasource") or {}).get("uid")
            check(f"{title[:38]}: uses the provisioned datasource uid",
                  uid == "fridge-sqlite")
    store.close()


# --- End to end -------------------------------------------------------------

def test_end_to_end():
    print("\n--- end to end: fake board -> link -> ingest -> sqlite ---")
    store = Store(temp_db()).open()
    board = FakeBoard(seed=3)
    link = Link(board)
    ingest = Ingest(store, link)

    # 14 simulated hours: long enough to cross the 6-hour reboot interval twice,
    # so "reboots are recorded" is not riding on a single boundary. The clock is
    # stepped, so this covers exactly 14 hours on any machine.
    for _ in range(int(14 * 3600 / STEP_S)):
        board.advance(STEP_S)
        while True:
            events = link.poll()
            if not events:
                break
            for event in events:
                ingest.handle(event)
        store.flush()
    store.flush(force=True)

    frames = store._conn.execute(
        "SELECT COUNT(*) FROM raw_line WHERE reject_reason IS NULL").fetchone()[0]
    rejected = store._conn.execute(
        "SELECT COUNT(*) FROM raw_line WHERE reject_reason IS NOT NULL"
    ).fetchone()[0]
    boots = store._conn.execute("SELECT COUNT(*) FROM boot").fetchone()[0]

    check(f"the board's frames reach the database ({frames} lines)", frames > 100)
    check(f"corrupted frames are rejected and recorded ({rejected})", rejected > 0)
    check(f"reboots are recorded ({boots} boot rows)", boots >= 2)
    check("no frame was silently lost: every line is in raw_line",
          frames + rejected == link.frames_in + link.ignored + link.bad)
    check("every message type the board sends has a handler",
          ingest.unhandled == {})
    store.close()


if __name__ == "__main__":
    print("=== fridged host tests ===")
    # RAW_LINE_KEEP is a deployment setting; the tests assert on stored lines, so
    # they pin it rather than inheriting whatever the machine is configured for.
    config.RAW_LINE_KEEP = "all"

    test_store()
    test_link()
    test_reconnecting_serial()
    test_link_resynchronises()
    test_seed_clear()
    test_ingest()
    test_fake_board()
    test_door_model()
    test_door_and_health_ingest()
    test_transaction_ingest()
    test_member_attribution()
    test_stock()
    test_camera_parsing()
    test_camera_staleness()
    test_camera_subprocess()
    test_camera_in_the_loop()
    test_camera_selection()
    test_confidence_is_recorded()
    test_baseline_latch()
    test_settling()
    test_settling_deadline()
    test_deferred_scan()
    test_deferred_scan_deadline()
    test_simcamera_never_defers()
    test_full_door_cycle()
    test_camera_cannot_answer()
    test_payments()
    test_dashboards()
    test_end_to_end()

    print("\n" + "=" * 50)
    if FAILURES:
        print(f"{len(FAILURES)} FAILED:")
        for item in FAILURES:
            print(f"  - {item}")
    else:
        print("ALL CHECKS PASSED")
    print("=" * 50)
    sys.exit(1 if FAILURES else 0)
