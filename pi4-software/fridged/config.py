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
#: 0o002 clears the group-write bit from the mask, so `fridge.db` and — the ones
#: that actually matter — `fridge.db-wal` and `fridge.db-shm` are created mode
#: 664 rather than 644.
#:
#: This is not tidiness. Grafana runs as a different user (`grafana`) and SQLite
#: **cannot open a WAL database read-only**: a reader has to take a lock in the
#: -shm wal-index, which is a write. With the default umask the -shm comes out
#: group-read-only and every Grafana query fails with "attempt to write a
#: readonly database" — which reads like a Grafana bug and is a file-mode one.
#:
#: Set here rather than left to whoever starts the service, because a daemon
#: whose output another process must write is entitled to say so itself, and
#: "remember to run `umask 002` first" is a step that gets forgotten exactly
#: once. On Windows this is inert.
DB_FILE_UMASK = 0o002

#: Where `picapture` writes the annotated frame that the dashboard displays.
#: Served by the HTTP API (stage D4) because Grafana can only show an image over
#: HTTP.
FRAMES_DIR = Path(os.environ.get("FRIDGE_FRAMES", PI4_ROOT / "frames"))
LATEST_JPG = FRAMES_DIR / "latest.jpg"

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
#: count, host SoC temperature. Matched to the board's own 30 s telemetry cadence
#: so the health row updates at one rate rather than two.
SELF_METRIC_INTERVAL_S = 30.0

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
