"""Wiring and the service loop. Everything else is in a module of its own.

This file decides *which* pieces are connected together; it contains no protocol
knowledge, no SQL, and no simulated behaviour. That is deliberate — it is the one
place the sim/real choice is made, so there is exactly one place to look when
asking which configuration is running.

    python -m fridged --port sim                    simulated board
    python -m fridged --port sim --sim-speed 200    a day of fridge in minutes
    python -m fridged --port COM7                   the real board (Windows)
    python -m fridged --port /dev/ttyACM0           the real board (Pi)
"""

import argparse
import logging
import os
import signal
import time

from . import config
from .ingest import Ingest
from .link import Link
from .store import SCHEMA_VERSION, Store

log = logging.getLogger("fridged")


def build_transport(args):
    """Open whatever `--port` asked for. The only sim/real branch in the service.

    Note the asymmetry that makes this safe: the simulated board is imported only
    when it is asked for, so a deployment that never passes `--port sim` cannot
    accidentally be running against fabricated data. The firmware makes the same
    point about its own stand-ins — with them compiled in, anyone who can reach
    the USB socket can fake a transaction (plan.md stage 10a).
    """
    if args.port == "sim":
        from fake_board import FakeBoard  # tools/, on the path via __init__.py
        log.warning("SIMULATED BOARD - no hardware involved, "
                    "speed=%gx seed=%s", args.sim_speed, args.sim_seed)
        return FakeBoard(speed=args.sim_speed, seed=args.sim_seed)

    import serial
    log.info("opening %s at %d baud", args.port, config.SERIAL_BAUD)
    # timeout=0 makes read() non-blocking, which is what the service loop needs:
    # it must come back and flush the database and answer the board even when
    # nothing has arrived.
    return serial.Serial(args.port, config.SERIAL_BAUD, timeout=0)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="python -m fridged",
        description="The fridge's data service: serial link in, SQLite out.")
    parser.add_argument(
        "--port", default="sim",
        help="serial device (COM7, /dev/ttyACM0), or 'sim' for a simulated "
             "board. Default: sim")
    parser.add_argument(
        "--db", default=None,
        help=f"database file. Default: {config.DEFAULT_DB_PATH}")
    parser.add_argument(
        "--sim-speed", type=float, default=1.0,
        help="simulated-clock multiplier; 200 turns a day into ~7 minutes. "
             "Ignored unless --port sim")
    parser.add_argument(
        "--sim-seed", type=int, default=None,
        help="seed the simulator's RNG so a run is reproducible")
    parser.add_argument(
        "--log-level", default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    return parser.parse_args(argv)


def install_shutdown_handler():
    """Make SIGTERM behave like Ctrl-C, so the final flush always runs.

    Python's default SIGTERM handler terminates the interpreter outright — no
    `finally`, no flush, and up to `FLUSH_INTERVAL_S` of queued rows discarded.
    That matters because SIGTERM is exactly how systemd stops a service
    (stage D8), so every restart and every reboot would punch a small hole in
    the history, and the holes would be invisible.

    Raising KeyboardInterrupt means the loop already has the right handler.
    """
    def on_term(signum, _frame):
        raise KeyboardInterrupt(f"signal {signum}")

    signal.signal(signal.SIGTERM, on_term)


def main(argv=None):
    args = parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S")

    install_shutdown_handler()

    # Before the database is opened, so the WAL sidecar files it creates are
    # group-writable and Grafana — running as a different user — can read them.
    # See the long note on DB_FILE_UMASK; this is a one-line fix for a failure
    # that otherwise presents as an unexplained Grafana error.
    os.umask(config.DB_FILE_UMASK)

    store = Store(args.db).open()
    log.info("database %s (schema v%d)", store.path, SCHEMA_VERSION)

    link = Link(build_transport(args))
    ingest = Ingest(store, link)

    next_status = time.monotonic() + config.SELF_METRIC_INTERVAL_S
    log.info("running - Ctrl-C to stop")

    try:
        while True:
            for event in link.poll():
                ingest.handle(event)

            ingest.tick()
            store.flush()

            if time.monotonic() >= next_status:
                next_status = time.monotonic() + config.SELF_METRIC_INTERVAL_S
                age = link.age_s
                log.info("in=%d out=%d bad=%d log=%d rows=%d link=%s%s",
                         link.frames_in, link.frames_out, link.bad,
                         link.ignored, store.rows_written,
                         "up" if link.healthy else "DOWN",
                         f" ({age:.1f}s)" if age is not None else "")
                if ingest.unhandled:
                    log.info("not wired up yet: %s", dict(ingest.unhandled))

            time.sleep(config.LOOP_SLEEP_S)

    except KeyboardInterrupt:
        log.info("stopping")
    finally:
        # close() flushes. Anything queued when Ctrl-C landed is still written,
        # which matters more than it sounds: the batch is up to five seconds of
        # data, and losing it every time the service is restarted would put
        # small holes throughout the history.
        store.close()
        link.close()
        log.info("wrote %d rows; frames in=%d out=%d bad=%d",
                 store.rows_written, link.frames_in, link.frames_out, link.bad)


if __name__ == "__main__":
    main()
