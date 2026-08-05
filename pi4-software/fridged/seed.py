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

import protocol                # noqa: E402  (tools/)
from fake_board import (       # noqa: E402  (tools/)
    BOX_FULL_G, DEFAULT_ZONES, DoorSchedule, HEALTH_INTERVAL_S,
    PRICE_CENTS, TEMP_SAMPLE_S, plan_transaction,
)

from . import config          # noqa: E402
from .camera import DISPLAY, SimCamera  # noqa: E402
from .ingest import Ingest    # noqa: E402
from .link import LinkEvent   # noqa: E402
from .store import Store      # noqa: E402

#: Marker text written into `raw_line`. Parsed by `--clear`, so it is a format,
#: not a comment — hence a constant rather than an inline string.
MARKER_PREFIX = "seeded history up to ts="

#: Rows per executemany. Large enough that 120,000 rows take a moment rather than
#: a minute; small enough not to build the whole list in memory first.
CHUNK = 5000


class _StubLink:
    """Ingest only touches the link in tick(), which seeding never calls."""
    age_s = None
    bad = 0


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


def plan_all_transactions(door, rng, start_ts):
    """Walk the shelf through every door cycle, exactly as the live system does.

    Returns the transaction frames, the coin deposits they imply, and the stock
    snapshots the camera would have produced.

    THE BASKET COMES FROM THE SHELF, not from a dice roll. A `SimCamera` is
    driven through open-scan, customer, close-scan for each door cycle, and the
    basket is the difference between the two scans — the same path the board
    takes over the wire. A restock therefore shows up as a NEGATIVE difference
    and produces no transaction at all, which is decision D8's "restock returns
    silently to Idle" arriving for free rather than being special-cased.
    """
    camera = SimCamera(rng)
    frames = []
    deposits = []          # (time, grams) landing in the coin box
    snapshots = []         # (time, {drink: count})

    for open_t, close_t in door.intervals:
        before, _ = camera.scan()               # the baseline, at the open
        snapshots.append((open_t, before))

        camera.customer_takes()
        camera.maybe_restock()

        after, _ = camera.scan()                # the recount, at the close
        snapshots.append((close_t, after))

        taken = {DISPLAY[d]: before[d] - after[d]
                 for d in before if before[d] > after.get(d, before[d])}
        if not taken:
            continue

        # `begin_selecting()` stamps the id when the door OPENS. Offsets from
        # the start of the seeded period stand in for ms-since-boot.
        txn_id = int((open_t - start_ts) * 1000.0)
        owed = sum(taken.values()) * PRICE_CENTS
        for at, prefix, type_, payload in plan_transaction(
                rng, txn_id, close_t, taken, owed):
            frames.append((at, prefix, type_, payload))
            if type_ in ("COIN", "COIN_REJECT"):
                # Rejected mass counts: a washer still weighs something, and the
                # gap between logged takings and measured mass is what the cash
                # reconciliation check is for.
                deposits.append((at, float(payload.split("delta_g=")[1])))

    frames.sort(key=lambda item: item[0])
    deposits.sort()
    snapshots.sort(key=lambda item: item[0])
    return frames, deposits, snapshots


def seed_stock(store, snapshots):
    """The camera's answers, one row per drink per scan."""
    rows = []
    for ts, counts in snapshots:
        for drink, count in counts.items():
            rows.append((ts, drink, count, "door_close"))
    store._conn.execute("BEGIN")
    store._conn.executemany(
        "INSERT INTO stock_snapshot (ts, drink, count, trigger) "
        "VALUES (?,?,?,?)", rows)
    store._conn.execute("COMMIT")
    return len(rows)


def seed_transactions(store, ingest, frames):
    """Replay planned transaction frames through the REAL ingest path.

    Not a shortcut into SQL. Every row here is produced by the same handlers
    that process a live board, so the upserts, the coin attribution and the
    duplicate-`TXN_START` handling are all exercised while the history is
    built. If the ingest has a bug, the seeded fortnight has it too — which is
    the honest outcome, and far better than a seeder that quietly writes tidy
    rows the live path could never produce.
    """
    for at, prefix, type_, payload in frames:
        wire = protocol.build(prefix, type_, payload, ms=0).decode().rstrip()
        parsed = protocol.parse(wire)
        ingest.handle(LinkEvent("frame", at, wire, parsed, None))
    store.flush(force=True)
    return len(frames)


def seed_health(store, start_ts, end_ts, rng, deposits):
    """Die temperature, coin-box mass and fault count, every 30 s.

    The box mass is integrated from the SAME coin events the transactions
    produced, so the money-box sawtooth and the revenue panels are two views of
    one set of coins rather than two inventions that will disagree.
    """
    next_deposit = 0
    box_g = 0.0

    batch = []
    written = 0
    ts = start_ts
    while ts < end_ts:
        # Apply every coin that has gone in since the last sample.
        while next_deposit < len(deposits) and deposits[next_deposit][0] <= ts:
            box_g += deposits[next_deposit][1]
            next_deposit += 1
            if box_g >= BOX_FULL_G:
                box_g = 0.0     # the treasurer emptied it

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
        seed_boot_id = store.open_boot(start_ts, fw=None, reason="seed")

        began = time.monotonic()

        # The door schedule is generated FIRST and in full, because both the
        # temperature and the coin box are functions of it. Its own RNG so that
        # changing the noise model cannot shuffle the door times, which would
        # silently re-roll the whole history.
        door = DoorSchedule(random.Random(args.seed + 1), start_ts)
        door.ensure_until(end_ts)

        # Transactions are PLANNED before anything is written, because the
        # coin box mass in seed_health has to integrate the very coins these
        # produce. Its own RNG so a change to one model cannot reshuffle another.
        txn_frames, deposits, snapshots = plan_all_transactions(
            door, random.Random(args.seed + 3), start_ts)

        temps = seed_temperature(store, start_ts, end_ts, rng, door)
        doors = seed_doors(store, door)
        stock = seed_stock(store, snapshots)
        health = seed_health(store, start_ts, end_ts,
                             random.Random(args.seed + 2), deposits)

        # Replayed through a real Ingest. `raw_line` is muted for the duration:
        # these frames were never received from anything, and recording them as
        # if they had been would be a lie in the one table kept for forensics.
        keep, config.RAW_LINE_KEEP = config.RAW_LINE_KEEP, "bad"
        ingest = Ingest(store, _StubLink())
        ingest.boot_id = seed_boot_id
        try:
            sales = seed_transactions(store, ingest, txn_frames)
        finally:
            config.RAW_LINE_KEEP = keep

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
        paid, stolen = store._conn.execute(
            "SELECT SUM(outcome='paid'), SUM(outcome='stolen') FROM txn"
        ).fetchone()
        revenue = store._conn.execute(
            "SELECT SUM(paid_cents) FROM txn WHERE outcome='paid'").fetchone()[0]
        print(f"  {sales:>8,} transaction frames -> {paid} paid, {stolen} "
              f"unpaid, ${(revenue or 0) / 100:,.2f} taken")
        print(f"  {stock:>8,} stock snapshot rows")
        print(f"  from {time.strftime('%Y-%m-%d %H:%M', time.localtime(start_ts))}"
              f"  to {time.strftime('%Y-%m-%d %H:%M', time.localtime(end_ts))}")
        print(f"  database: {store.path}")
    finally:
        store.close()


if __name__ == "__main__":
    main()
