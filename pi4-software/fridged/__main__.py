"""Wiring and the service loop. Everything else is in a module of its own.

This file decides *which* pieces are connected together; it contains no protocol
knowledge, no SQL, and no simulated behaviour. That is deliberate — it is the one
place the sim/real choice is made, so there is exactly one place to look when
asking which configuration is running.

    python -m fridged --port sim                    simulated board
    python -m fridged --port sim --sim-activity 30  a busy fridge, for
                                                   watching a live dashboard
    python -m fridged --port sim --sim-speed 200    a day of fridge in minutes,
                                                   for exercising reboots
    python -m fridged --port COM7                   the real board (Windows)
    python -m fridged --port /dev/ttyACM0           the real board (Pi)
    python -m fridged --port sim --camera picapture the real camera, simulated
                                                   board - the useful hybrid
                                                   for testing the vision
    python -m fridged --port /dev/ttyACM0 --camera picapture      the fridge
"""

import argparse
import logging
import random
import os
import signal
import time

from . import config
from .camera import PiCapture, SimCamera
from .payments import BACKENDS, PaymentService
from .ingest import Ingest
from .link import Link, ReconnectingSerial
from .store import SCHEMA_VERSION, Store

log = logging.getLogger("fridged")


def build_transport(args):
    """Open whatever `--port` asked for. The only sim/real branch in the service.

    Note the asymmetry that makes this safe: the simulated board is imported only
    when it is asked for, so a deployment that never passes `--port sim` cannot
    accidentally be running against fabricated data. The firmware makes the same
    point about its own stand-ins — with them compiled in, anyone who can reach
    the USB socket can fake a transaction (documentation.md section 3.2).
    """
    if args.port == "sim":
        from fake_board import FakeBoard  # tools/, on the path via __init__.py
        log.warning("SIMULATED BOARD - no hardware involved, "
                    "speed=%gx seed=%s", args.sim_speed, args.sim_seed)
        return FakeBoard(speed=args.sim_speed, seed=args.sim_seed,
                         activity=args.sim_activity)

    try:
        import serial
    except ImportError:
        # Raspberry Pi OS is an externally managed environment (PEP 668), so the
        # obvious `pip install pyserial` is refused and people reach for
        # --break-system-packages. It is packaged; say so rather than letting a
        # bare ImportError traceback be the whole message.
        raise SystemExit(
            "pyserial is not installed, and it is needed for a real serial "
            "port.\n"
            "  Raspberry Pi OS / Debian:  sudo apt install python3-serial\n"
            "  elsewhere:                 python3 -m pip install pyserial\n"
            "\n"
            "`--port sim` does not need it.")

    log.info("opening %s at %d baud", args.port, config.SERIAL_BAUD)

    # ReconnectingSerial rather than serial.Serial directly. It reopens the port
    # after the board is reset or the cable is nudged — neither of which the raw
    # port survives, because the device does not come back on the same file
    # descriptor — and it does not raise when the node is not there yet, which
    # is the normal state for the first second or two of a cold boot.
    return ReconnectingSerial(args.port, config.SERIAL_BAUD)


def build_camera(args):
    """Whatever `--camera` asked for. The second sim/real branch in the service.

    Deliberately parallel to `build_transport()` and to `--square`, and
    deliberately defaulting to the simulation for the same reason: a deployment
    that never asks for the real camera cannot accidentally be running on
    fabricated stock. That asymmetry is the point — the safe option is the one
    you get by saying nothing.

    Independent of `--port`, so the real camera can be pointed at a real shelf
    while a simulated board drives the transaction flow. That combination is the
    useful one for testing the vision without standing at the fridge pressing
    buttons.
    """
    if args.camera == "sim":
        return SimCamera(random.Random(args.sim_seed))

    log.warning("REAL CAMERA - counting a physical shelf with %s",
                config.PICAPTURE_BINARY)
    return PiCapture()


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
        "--sim-activity", type=float, default=1.0,
        help="how heavily the simulated fridge is used. 1 is realistic (~34 "
             "door opens a day); 30 gives one every couple of minutes, which "
             "is what you want when watching a live dashboard. Unlike "
             "--sim-speed this does not distort the clock, so timestamps stay "
             "honest. Ignored unless --port sim")
    parser.add_argument(
        "--camera", default="sim", choices=["sim", "picapture"],
        help="what answers CMD SCAN: 'sim' is a modelled shelf that needs no "
             "hardware; 'picapture' runs the real vision subprocess and counts "
             "the actual shelf. Independent of --port, so a real camera can be "
             "used with a simulated board")
    parser.add_argument(
        "--square", default="fake", choices=sorted(BACKENDS),
        help="card payments: 'fake' needs no credentials or network; 'real' "
             "talks to the Square account configured in "
             "online-payment/tokens.py. Independent of --port, so a real "
             "payment can be taken against a simulated board")
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

    # The board cannot tell the difference: it sends CMD SCAN and something
    # answers.
    camera = build_camera(args)

    # Independent of --port on purpose: a REAL payment against a SIMULATED
    # board is the hybrid test in documentation.md section 7.5, and it works because
    # the board cannot tell who answers CMD SQUARE_LINK.
    payments = PaymentService(BACKENDS[args.square]())
    log.info("card payments: %s", payments.backend.name)

    # `simulated` is what keeps a `--port sim` run from being mistaken for a real
    # one later. It is passed here rather than sniffed inside Ingest because
    # this is the only place that knows what --port was.
    ingest = Ingest(store, link, camera, payments,
                    simulated=(args.port == "sim"))
    if args.port == "sim":
        log.warning("SIMULATED BOARD - rows are tagged boot.reason='sim:*' so "
                    "they can be told from real ones in %s", store.path)

    next_status = time.monotonic() + config.SELF_METRIC_INTERVAL_S
    log.info("running - Ctrl-C to stop")

    try:
        while True:
            for event in link.poll():
                ingest.handle(event)

            ingest.drain_payments()
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
        # Camera first: it owns a child process, and an orphaned picapture keeps
        # the camera device open so the NEXT start fails to acquire it.
        camera.close()
        payments.close()
        store.close()
        link.close()
        log.info("wrote %d rows; frames in=%d out=%d bad=%d",
                 store.rows_written, link.frames_in, link.frames_out, link.bad)


if __name__ == "__main__":
    main()
