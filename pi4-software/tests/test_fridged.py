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
from fridged.link import Link       # noqa: E402
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
    test_ingest()
    test_fake_board()
    test_door_model()
    test_door_and_health_ingest()
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
