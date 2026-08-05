"""Backfill plausible history, so the dashboard has something to show today.

    python -m fridged.seed --days 14
    python -m fridged.seed --clear          remove seeded data, keep live data

THE ONE PLACE THAT WRITES ROWS WITHOUT A WIRE
---------------------------------------------
Everything else in `fridged` simulates at the wire and goes through the real
ingest path, deliberately (dashboard-plan.md section 0). This module is the
documented exception, and the reason is that it writes the *past*: no serial link
can carry two weeks ago. dashboard.md section 12 makes the point that history
accumulates in wall-clock time and cannot be back-filled once the system is real
— but it can be *simulated* now, and that is what makes the panel design
reviewable before there is a fridge to look at.

WHY IT IMPORTS THE SIMULATED BOARD'S THERMAL MODEL
---------------------------------------------------
The seeded past and the live simulation have to be the same fridge. If this file
carried its own idea of what a freezer does, the graph would show one fridge
until the moment you started the service and a different one afterwards, and
every panel would be validated against a fridge that does not exist. So the
`Zone` definitions come from `fake_board`, and there is one description of the
hardware's behaviour rather than two.

TELLING SEEDED DATA APART
-------------------------
Seeded rows are, by construction, older than anything live. The boundary is
recorded as a `raw_line` marker with `source='seed'`, so `--clear` knows exactly
what it may delete and a human reading the database can see at a glance where
the invented history stops.
"""

import argparse
import math
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from fake_board import (  # noqa: E402  (tools/)
    BOX_FULL_G, COIN_MASSES_G, COIN_ON_CLOSE_RATE, DEFAULT_ZONES,
    DoorSchedule, HEALTH_INTERVAL_S, TEMP_SAMPLE_S,
)

from . import config          # noqa: E402
from .store import Store      # noqa: E402

#: Marker text written into `raw_line`. Parsed by `--clear`, so it is a format,
#: not a comment — hence a constant rather than an inline string.
MARKER_PREFIX = "seeded history up to ts="

#: Rows per executemany. Large enough that 120,000 rows take a moment rather than
#: a minute; small enough not to build the whole list in memory first.
CHUNK = 5000


def clear(store):
    """Delete seeded data, leaving anything live untouched.

    The boundary comes from the marker rather than from a guess, because
    "everything older than two weeks" would also delete real history the moment
    this system has been running for longer than the seed covered.
    """
    rows = store._conn.execute(
        "SELECT line FROM raw_line WHERE source='seed' ORDER BY ts").fetchall()
    if not rows:
        print("nothing was seeded; nothing to clear")
        return

    boundary = max(float(r[0].removeprefix(MARKER_PREFIX)) for r in rows)
    deleted = store._conn.execute(
        "DELETE FROM measurement WHERE ts <= ?", (boundary,)).rowcount
    store._conn.execute("DELETE FROM boot WHERE reason='seed'")
    store._conn.execute("DELETE FROM raw_line WHERE source='seed'")
    print(f"cleared {deleted} seeded measurements up to "
          f"{time.strftime('%Y-%m-%d %H:%M', time.localtime(boundary))}")


def seed_temperature(store, start_ts, end_ts, rng, door):
    """Temperature for every sensor, at the interval the firmware really uses."""
    for zone in DEFAULT_ZONES:
        store.note_sensor(zone.rom, start_ts)

    metric = {zone.rom: f"temp.rom.{zone.rom}" for zone in DEFAULT_ZONES}
    batch = []
    written = 0
    ts = start_ts
    while ts < end_ts:
        for zone in DEFAULT_ZONES:
            # `ts` rather than seconds-since-start, so the compressor cycle is a
            # continuous function of wall time and the seam where seeded history
            # meets live data has no phase jump in it.
            #
            # `door` is passed for the same reason at a different scale: every
            # warming bump in this history has to sit under a real door event in
            # the timeline panel, or the two panels contradict each other.
            batch.append((ts, metric[zone.rom], zone.celsius(ts, rng, door)))
        ts += TEMP_SAMPLE_S

        if len(batch) >= CHUNK:
            written += _write(store, batch)
            batch = []

    written += _write(store, batch)
    return written


def seed_doors(store, door):
    """Door events, with `duration_s` filled in exactly as ingest would."""
    rows = []
    for open_t, close_t in door.intervals:
        rows.append((open_t, "open", close_t - open_t))
        rows.append((close_t, "closed", None))

    store._conn.execute("BEGIN")
    store._conn.executemany(
        "INSERT INTO door_event (ts, state, duration_s) VALUES (?,?,?)", rows)
    store._conn.execute("COMMIT")
    return len(rows)


def seed_health(store, start_ts, end_ts, rng, door):
    """Die temperature, coin-box mass and fault count, every 30 s.

    The box mass follows door closes using the same rule as the live simulator,
    so the sawtooth in the seeded history and the sawtooth after it are produced
    by one model rather than two that will disagree.
    """
    closes = [close_t for _, close_t in door.intervals]
    next_close = 0
    box_g = 0.0

    batch = []
    written = 0
    ts = start_ts
    while ts < end_ts:
        # Apply every door close that has happened since the last sample.
        while next_close < len(closes) and closes[next_close] <= ts:
            next_close += 1
            if rng.random() < COIN_ON_CLOSE_RATE:
                for _ in range(rng.randint(1, 2)):
                    box_g += rng.choice(COIN_MASSES_G)
                if box_g >= BOX_FULL_G:
                    box_g = 0.0

        die_c = (32.0 + 2.5 * math.sin(2.0 * math.pi * ts / 86400.0)
                 + rng.gauss(0.0, 0.25))
        # The Pi's own SoC, which on a live system comes from
        # /sys/class/thermal. Warmer than the board and it climbs under load.
        soc_c = 52.0 + 4.0 * math.sin(2.0 * math.pi * ts / 86400.0 + 1.0) \
            + rng.gauss(0.0, 0.8)

        batch.append((ts, "temp.rp2040_die", die_c))
        batch.append((ts, "coinbox.mass_g", box_g))
        batch.append((ts, "health.faults", 0.0))
        batch.append((ts, "temp.pi_soc", soc_c))
        ts += HEALTH_INTERVAL_S

        if len(batch) >= CHUNK:
            written += _write(store, batch)
            batch = []

    written += _write(store, batch)
    return written


def _write(store, batch):
    if not batch:
        return 0
    store._conn.execute("BEGIN")
    store._conn.executemany(
        "INSERT INTO measurement (ts, metric, value) VALUES (?,?,?)", batch)
    store._conn.execute("COMMIT")
    return len(batch)


def main(argv=None):
    import random

    parser = argparse.ArgumentParser(
        prog="python -m fridged.seed",
        description="Backfill simulated history so the dashboard has data.")
    parser.add_argument("--db", default=None,
                        help=f"database file. Default: {config.DEFAULT_DB_PATH}")
    parser.add_argument("--days", type=float, default=14.0,
                        help="how much history to invent. Default: 14")
    parser.add_argument("--seed", type=int, default=1,
                        help="RNG seed, so a run is reproducible")
    parser.add_argument("--clear", action="store_true",
                        help="delete previously seeded data and exit")
    args = parser.parse_args(argv)

    # Seeding usually runs BEFORE the service, so this is often the process that
    # creates fridge.db and its WAL sidecars. It therefore needs the same umask,
    # or Grafana cannot read a database that was seeded but never yet served.
    os.umask(config.DB_FILE_UMASK)

    store = Store(args.db).open()
    try:
        if args.clear:
            clear(store)
            return

        end_ts = time.time()
        start_ts = end_ts - args.days * 86400.0
        rng = random.Random(args.seed)

        # Seeded rows belong to a boot of their own, so nothing invented is ever
        # attributed to a real run of the board.
        store.open_boot(start_ts, fw=None, reason="seed")

        began = time.monotonic()

        # The door schedule is generated FIRST and in full, because both the
        # temperature and the coin box are functions of it. Its own RNG so that
        # changing the noise model cannot shuffle the door times, which would
        # silently re-roll the whole history.
        door = DoorSchedule(random.Random(args.seed + 1), start_ts)
        door.ensure_until(end_ts)

        temps = seed_temperature(store, start_ts, end_ts, rng, door)
        doors = seed_doors(store, door)
        health = seed_health(store, start_ts, end_ts,
                             random.Random(args.seed + 2), door)
        store.raw_line(end_ts, f"{MARKER_PREFIX}{end_ts}", source="seed")
        store.flush(force=True)

        left_open = sum(1 for o, c in door.intervals if c - o > 120.0)
        print(f"seeded over {args.days:g} days in "
              f"{time.monotonic() - began:.1f}s:")
        print(f"  {temps:>8,} temperature readings "
              f"({len(DEFAULT_ZONES)} sensors at {TEMP_SAMPLE_S:g}s)")
        print(f"  {doors:>8,} door events "
              f"({len(door.intervals)} opens, {left_open} left open >2 min)")
        print(f"  {health:>8,} health readings (at {HEALTH_INTERVAL_S:g}s)")
        print(f"  from {time.strftime('%Y-%m-%d %H:%M', time.localtime(start_ts))}"
              f"  to {time.strftime('%Y-%m-%d %H:%M', time.localtime(end_ts))}")
        print(f"  database: {store.path}")
    finally:
        store.close()


if __name__ == "__main__":
    main()
