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
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from fake_board import DEFAULT_ZONES, TEMP_SAMPLE_S  # noqa: E402  (tools/)

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


def seed_temperature(store, start_ts, end_ts, rng):
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
            batch.append((ts, metric[zone.rom], zone.celsius(ts, rng)))
        ts += TEMP_SAMPLE_S

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
        rows = seed_temperature(store, start_ts, end_ts, rng)
        store.raw_line(end_ts, f"{MARKER_PREFIX}{end_ts}", source="seed")
        store.flush(force=True)

        print(f"seeded {rows:,} temperature readings over {args.days:g} days "
              f"({len(DEFAULT_ZONES)} sensors at {TEMP_SAMPLE_S:g}s) "
              f"in {time.monotonic() - began:.1f}s")
        print(f"  from {time.strftime('%Y-%m-%d %H:%M', time.localtime(start_ts))}"
              f"  to {time.strftime('%Y-%m-%d %H:%M', time.localtime(end_ts))}")
        print(f"  database: {store.path}")
    finally:
        store.close()


if __name__ == "__main__":
    main()
