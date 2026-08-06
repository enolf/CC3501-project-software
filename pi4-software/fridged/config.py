"""Every path, interval and host quirk, in one file.

WHY THIS FILE EXISTS
--------------------
`CLAUDE.md` section 4 makes `board.h` the single place a wiring fact may appear
on the firmware side, so that a hardware change is one edit rather than a hunt.
This is the same rule for the Pi.

The concrete payoff: this code is developed on a Windows laptop against a
simulated board and deployed to a Raspberry Pi against a real one. Those two
machines disagree about where the database lives, what the serial port is
called, and whether `/sys/class/thermal` exists. If those disagreements are
spread through the codebase, the move to the Pi is a debugging session. Kept
here, it is an edit to one file and a systemd unit.

**If a path literal or a bare number appears anywhere else in `fridged/`, that
is the bug.**
"""

import os
from pathlib import Path

# --- Paths ------------------------------------------------------------------

#: Root of `pi4-software/`, worked out from this file rather than assumed, so
#: the service runs correctly from any working directory (systemd starts it
#: from `/`).
PI4_ROOT = Path(__file__).resolve().parent.parent

#: Where the database lives. Overridden by `--db`, then by the `FRIDGE_DB`
#: environment variable, then this default.
#:
#: The default is deliberately inside the repo rather than `/var/lib/fridge`:
#: that directory needs root to create, which would make the first run on a
#: laptop fail for a reason that has nothing to do with the code. The systemd
#: unit sets `FRIDGE_DB` explicitly on the Pi (stage D8). `*.db` is gitignored.
DEFAULT_DB_PATH = Path(os.environ.get("FRIDGE_DB", PI4_ROOT / "fridge.db"))

#: Umask for the files this service creates.
#:
#: WHY BOTH THIS AND THE EXPLICIT chmod IN store.py ARE NEEDED
#: ----------------------------------------------------------
#: Grafana runs as a different user (`grafana`) and SQLite **cannot open a WAL
#: database read-only**: a reader has to take a lock in the -shm wal-index, and
#: that is a write. So `grafana` needs write access to `fridge.db`,
#: `fridge.db-wal` and `fridge.db-shm` alike.
#:
#: Getting that takes two separate things, and the first attempt here used only
#: the umask and did not work:
#:
#:   * **umask can only REMOVE permission bits, never add them.** SQLite creates
#:     the database with mode 0644 built in, so there is no group-write bit for
#:     any umask to preserve. Hence `Store.open()` chmods the file explicitly.
#:
#:   * SQLite then creates the -wal and -shm files with **the database file's
#:     mode**, which the process umask is applied to. So without this, a 0664
#:     database still yields 0644 sidecars under the default 022 umask, and the
#:     failure comes back on the next write.
#:
#: Set here rather than left to whoever starts the service: a daemon whose
#: output another process must write is entitled to say so itself, and
#: "remember to run `umask 002` first" is a step that gets forgotten exactly
#: once. On Windows this is inert.
DB_FILE_UMASK = 0o002

#: Mode the database file is forced to, for the reason above. Group-writable so
#: Grafana can take its WAL read lock; not world-writable.
DB_FILE_MODE = 0o664

# --- The camera -------------------------------------------------------------

#: Where `picapture` lives, and what to run.
#:
#: The DIRECTORY matters as much as the binary: picapture reads `picapture.conf`
#: from its working directory, and started anywhere else it runs perfectly
#: happily on compiled-in defaults that nobody tuned — announcing it only on
#: stderr, where a running service's output is easy not to read.
PICAPTURE_DIR = Path(os.environ.get("FRIDGE_PICAPTURE", PI4_ROOT / "picapture"))
PICAPTURE_BINARY = PICAPTURE_DIR / "build" / "PiCapture"

#: How old the newest count may be before its confidence starts being marked
#: down.
#:
#: picapture's own period is 250 ms, so a healthy camera produces four packets a
#: second. A second of silence is four missed frames — a stall, not jitter.
CAMERA_STALE_S = 1.0

#: How old the newest count may be before it is not worth answering with at all.
#: Past this the Pi sends no INV, and the board faults out of service after its
#: own `RECOUNT_TIMEOUT_MS` (8 s). See decision D1 in IMPLEMENTATION.md: a
#: fridge that cannot see its shelf should stop selling, not guess.
#:
#: Comfortably inside the board's budget so the fault is OUR verdict rather than
#: a timeout nobody decided on.
CAMERA_DEAD_S = 5.0

#: How long to wait before restarting a picapture that died, and the ceiling
#: that doubling backs off to. A camera that cannot start — no device, another
#: process holding it — should not spin.
CAMERA_RESTART_DELAY_S = 2.0
CAMERA_RESTART_MAX_S = 30.0

#: How long to give picapture to exit on shutdown before killing it. An
#: orphaned picapture keeps the camera open and the NEXT start then fails.
CAMERA_STOP_TIMEOUT_S = 3.0

# --- Settling: deciding when the picture is worth believing ------------------
#
# EVERY NUMBER IN THIS BLOCK IS A PLACEHOLDER UNTIL IT IS MEASURED.
#
# They are named constants precisely so that tuning them is an edit rather than
# a rewrite. The measurements that should set them are in `TEST.md`:
#
#   T3.6  how much do counts wander on a shelf nobody is touching?
#         -> SETTLE_FRAMES. If a still shelf already produces a run of three
#            identical frames in well under a second, three is enough. If it
#            never produces three in a row, the problem is the tuning, not this.
#   T5.4  when a real purchase is miscounted, how is it wrong, and how long
#         after the door shuts does the picture actually stabilise?
#         -> SETTLE_TIMEOUT_S.
#
# Guessing them is what this block exists to avoid, so treat the values below
# as "somewhere to start", exactly as picapture's colour defaults are.

#: How many consecutive frames must report the same counts before the shelf is
#: judged to be holding still.
#:
#: Compared on counts alone, not on whole readings — confidence moves every
#: frame, so anything else would never agree with anything.
#:
#: Three at picapture's 250 ms period is 750 ms of agreement, which is short
#: enough to fit the budget several times over and long enough to reject a
#: single frame caught mid-movement. Two would be satisfied by any two
#: consecutive bad frames that happened to be bad the same way, which a hand
#: held still for half a second produces easily.
SETTLE_FRAMES = 3

#: How long to wait for that agreement before answering anyway.
#:
#: Must leave room inside `SCAN_ANSWER_BUDGET_S` below, which in turn sits
#: inside the board's `RECOUNT_TIMEOUT_MS` (8 s). A shelf that has not settled
#: in four seconds is not going to, and an answer marked down is worth more than
#: a fault.
SETTLE_TIMEOUT_S = 4.0

#: How long `_on_scan` may hold a reply open before forcing one out.
#:
#: THE BACKSTOP, NOT THE NORMAL PATH. Settling resolves itself at
#: SETTLE_TIMEOUT_S; this catches the case where it somehow does not, so that an
#: owed reply can never be held forever. Two seconds of margin inside the
#: board's 8 s, because the fault should be our verdict rather than a timeout
#: nobody decided on.
SCAN_ANSWER_BUDGET_S = 6.0

#: What an unsettled answer's confidence is multiplied by.
#:
#: "I ran out of time" is a materially different claim from "the shelf held
#: still", and the dashboard is the only place that difference will ever be
#: visible — the counts themselves look identical. Halved rather than nudged so
#: that a threshold can actually be set on it later.
UNSETTLED_CONFIDENCE_SCALE = 0.5

#: How many unparseable stdout lines to log before going quiet about them. The
#: debug modes print tuning readouts to stdout as well as counts, so a few are
#: normal; a flood is a version mismatch and the count is what shows it.
CAMERA_MALFORMED_LOG_LIMIT = 5

# --- The serial link --------------------------------------------------------

#: The board's USB CDC endpoint ignores the baud rate entirely — it is a virtual
#: port — but pyserial requires one and 115200 is what every tool in this repo
#: has been opened with, including `fake_pi.py`. Consistency is worth more than
#: the number.
SERIAL_BAUD = 115200

#: How often to send `EVT HB`.
#:
#: NOT cosmetic, and not a number to tune casually. The board declares the link
#: dead after `LINK_TIMEOUT_MS` (30 s) without a valid frame and takes itself out
#: of service — and the Pi has nothing to say while the fridge is idle, so
#: without this a perfectly healthy link looks dead after 30 s of quiet. This was
#: found during firmware stage 13; see plan.md section 2, the `HB` row.
HEARTBEAT_INTERVAL_S = 10.0

#: How long without a frame from the board before we consider the link down.
#: Matches the board's own judgement of us, so both ends agree on what "dead"
#: means.
LINK_TIMEOUT_S = 30.0

#: Cap on bytes drained from the transport per service pass. A stuck or babbling
#: sender must cost latency, not the loop. The firmware caps its own drain at 64
#: bytes per pass for the same reason; the Pi can afford far more.
MAX_READ_PER_POLL = 4096

# --- Write batching ---------------------------------------------------------
#
# dashboard.md section 13: continuous writes kill SD cards. One transaction per
# row would fsync per row; batching turns a day of telemetry into a few thousand
# commits instead of a few hundred thousand.

#: Flush pending rows once this many are queued...
FLUSH_ROWS = 200

#: ...or this long has passed, whichever comes first. The time bound matters as
#: much as the row bound: an idle fridge produces a trickle, and a row that sits
#: unwritten for an hour is a row the dashboard cannot show.
FLUSH_INTERVAL_S = 5.0

#: How long to wait for a lock before giving up. Grafana reads the same file, and
#: WAL means readers never block writers, but a checkpoint briefly can.
SQLITE_BUSY_TIMEOUT_MS = 5000

# --- The service loop -------------------------------------------------------

#: Idle pause between service passes. The loop is entirely I/O bound, so this is
#: purely about not spinning a core; anything under ~50 ms is imperceptible to
#: the link.
LOOP_SLEEP_S = 0.01

#: How often the service records metrics about *itself* — link age, bad-frame
#: count, host SoC temperature.
#:
#: Matched to the board's own telemetry cadence so the health row updates at one
#: rate rather than two. The board's half is `HEALTH_INTERVAL_MS` and
#: `TEMP_SAMPLE_INTERVAL_MS` in `rp2040-software/src/timings.h`; if you change
#: this, change those, and vice versa. Half the health row arriving three times
#: as often as the other half looks like a fault in whichever half is slower.
SELF_METRIC_INTERVAL_S = 10.0

#: A drop in the board's `ms` field this large means it rebooted without us
#: seeing the `BOOT` frame — the counter restarting is the signal
#: (dashboard.md section 6). The stream is strictly ordered, so any decrease is
#: real and the detector does not need to be sensitive; a genuine reset drops
#: `ms` from possibly hours to near zero. The slack simply keeps a single odd
#: frame from opening a spurious boot record.
MS_ROLLBACK_SLACK = 1000

# --- Diagnostics ------------------------------------------------------------

#: What to keep in `raw_line`.
#:
#:   "all"      every line, frames and board log output alike
#:   "nonframe" only log output and rejected frames
#:   "bad"      only rejected frames
#:
#: "all" is roughly 12,000 rows a day, which SQLite will not notice and which is
#: worth every byte the first time something looks wrong the night before a demo
#: (dashboard.md section 5.2). Turn it down if the database ever lives somewhere
#: precious.
RAW_LINE_KEEP = "all"

# --- Host facts -------------------------------------------------------------

#: Where Linux exposes the SoC temperature, in millidegrees. Absent on Windows,
#: which is the whole reason `soc_temperature_c()` exists rather than an inline
#: `open()`.
SOC_TEMP_PATH = Path("/sys/class/thermal/thermal_zone0/temp")


def soc_temperature_c():
    """The host SoC temperature in degrees C, or None if this host has none.

    Returns None on Windows and on any Linux box without that thermal zone,
    rather than raising or reporting a fake number — a missing metric should
    leave a gap in the graph, which is true, instead of a plausible constant,
    which is a lie.
    """
    try:
        return int(SOC_TEMP_PATH.read_text().strip()) / 1000.0
    except (OSError, ValueError):
        return None
