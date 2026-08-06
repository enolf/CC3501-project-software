"""What is on the shelf. The Pi's answer to `CMD SCAN`.

THIS IS THE ONE THING THAT FLOWS THE OTHER WAY
-----------------------------------------------
Every other metric goes board -> Pi. Stock does not: the board asks
(`CMD SCAN`) and the Pi answers (`EVT INV`), because the camera is the source
of truth for what is on the shelf and the board has no way of knowing
(dashboard.md section 6.3). The board then diffs two answers — one from when
the door opened, one from when it shut — and *that* is the basket.

The consequence worth stating: `stock_snapshot` rows are written by `fridged`
when it ANSWERS a scan. The board never reports stock and never could.

WHY THE SHELF IS SELF-CORRECTING
--------------------------------
Counts are absolute, never differences. A restock needs no manual entry and no
message of its own: the next scan simply reports more drinks, and the shelf is
right again. A missed or corrupted frame costs one stale reading rather than
permanently offsetting the count (dashboard.md section 2).

Two backends, one interface — the same shape as `pi_link` on the firmware side:

    SimCamera       a modelled shelf, for development
    PiCapture       the picapture subprocess, watching the real shelf

THE INTERFACE
-------------
    door_opened(ts)   the door moved. SimCamera ignores it; PiCapture will
                      freeze its baseline here in stage 4.
    door_closed(ts)   SimCamera moves its shelf. PiCapture will begin settling.
    payload()         answer a CMD SCAN, or None if it cannot be answered.
    close()

`payload()` returning None is load-bearing rather than an error path nobody
takes. It is how a camera says "I do not know", and the board's response — no
INV, `Recount` times out, out of service — is the correct one for a fridge that
cannot see what it is selling.
"""

import logging
import random
import subprocess
import threading
import time

from . import config

log = logging.getLogger("fridged.camera")

#: Drink keys as they appear on the wire. These are `catalogue::wire_key()` on
#: the firmware side — a column of its own in `catalogue.h`, NOT the display
#: name lowercased.
#:
#: They used to be the display name lowercased, and that quietly stopped working
#: when a drink acquired a space in it: a payload is space-separated `key=value`
#: pairs, so `mountain dew=5` is not one field but a key nobody looks for
#: followed by a stray token. Both ends would have gone on running and agreed on
#: the wrong numbers. Keep these short, lower case and space-free.
#:
#: ORDER MATTERS. It matches `catalogue::Can` on the board and `colors_vec` in
#: picapture, which reports its counts positionally.
DRINKS = ("coke", "fanta", "mtndew", "solo")

#: Display names, for `txn_item` and the panels. Same order. This is the one
#: place the pretty name lives on the Pi side, exactly as `Entry::name` is on
#: the board.
DISPLAY = {"coke": "Coke", "fanta": "Fanta", "mtndew": "Mountain Dew",
           "solo": "Solo"}

#: How many of each drink a full shelf holds.
SHELF_CAPACITY = 6

#: Restock when the total drops to this fraction of a full shelf. Somebody
#: notices it is getting empty and refills it, which is the event that makes
#: the camera's self-correction visible on the dashboard.
RESTOCK_AT = 0.3


def build_payload(counts, confidence):
    """The `EVT INV` payload: `coke=5 fanta=4 mtndew=5 solo=3 conf=98`.

    Shared by both backends so the board cannot tell them apart, and so a change
    to the wire format is one edit. Emitted in `DRINKS` order, which is
    `catalogue::Can`'s order — the board parses by key, but a human reading two
    logs side by side should not have to.
    """
    pairs = " ".join(f"{drink}={counts[drink]}" for drink in DRINKS)
    return f"{pairs} conf={confidence}"


def parse_packet(line, drinks=DRINKS):
    """`coke:5,fanta:4,mtndew:5,solo:3;conf=87;` -> `({...}, 87)`, or None.

    STRICT ON PURPOSE, AND THE STRICTNESS IS THE POINT.
    ---------------------------------------------------
    picapture's stdout is not a pure count stream — the debug modes print
    tuning readouts to it, and a partially written line can appear if it is
    killed mid-print. Anything that is not exactly a well-formed packet must be
    discarded rather than salvaged.

    The key set is checked as a whole, not filtered down to what is recognised.
    A picapture built with a different brand list would otherwise have its
    counts silently accepted for whichever drinks happened to match, and both
    ends would go on agreeing about the wrong numbers with nothing reporting a
    fault. `vision_config.h` and `catalogue.h` already say the brand order is
    part of the protocol; this is where that claim gets enforced.

    Returns None for anything malformed. The caller logs and drops it.
    """
    parts = line.strip().split(";")
    if len(parts) < 2:
        return None

    counts = {}
    for item in parts[0].split(","):
        key, separator, value = item.partition(":")
        if not separator or not value.isdigit():
            return None
        if key in counts:
            return None                 # the same drink twice: not a packet
        counts[key] = int(value)

    if set(counts) != set(drinks):
        return None

    conf_field = parts[1]
    if not conf_field.startswith("conf="):
        return None
    raw = conf_field[len("conf="):]
    if not raw.isdigit() or int(raw) > 100:
        return None

    return counts, int(raw)


class SimCamera:
    """A modelled shelf that customers take drinks from.

    Deliberately owns the shelf rather than being told what was taken. In the
    real system the camera *observes* a physical world it does not control, and
    the board learns what happened by diffing two observations. Letting the
    simulated board decide the basket and telling the camera afterwards would
    invert that and leave the diff logic untested.
    """

    def __init__(self, rng=None, capacity=SHELF_CAPACITY):
        self.rng = rng or random.Random()
        self.capacity = capacity
        self.shelf = {drink: capacity for drink in DRINKS}
        self.restocks = 0
        self.scans = 0

    # --- The door ------------------------------------------------------------
    #
    # The shared interface. A real camera OBSERVES a world it does not control,
    # so these are the only two things it can be told; what happened to the
    # shelf is something it has to work out for itself.

    def door_opened(self, ts=None):
        """Nothing to do. The shelf changes when the door SHUTS, not now.

        Deliberately empty rather than absent. Taking the drinks at the open
        would make the board's baseline scan already reflect them, the two scans
        would agree, and every transaction would silently disappear — which is
        exactly what happened when this was first written the other way round.
        """

    def door_closed(self, ts=None):
        """Somebody has been at the fridge. Decide what changed."""
        self.customer_takes()
        self.maybe_restock()

    def close(self):
        """Nothing to shut down. Present so both backends can be closed alike."""

    # --- The physical world -------------------------------------------------

    def customer_takes(self):
        """Somebody opened the door. Decide what, if anything, left the shelf.

        Returns what was removed, which the caller does not need but the tests
        do. The board is not told: it finds out by scanning.
        """
        if self.rng.random() >= 0.55:
            return {}           # looked, changed their mind

        roll = self.rng.random()
        count = 1 if roll < 0.72 else (2 if roll < 0.94 else 3)

        taken = {}
        available = [d for d in DRINKS if self.shelf[d] > 0]
        for _ in range(count):
            if not available:
                break
            drink = self.rng.choice(available)
            self.shelf[drink] -= 1
            taken[drink] = taken.get(drink, 0) + 1
            available = [d for d in DRINKS if self.shelf[d] > 0]
        return taken

    def maybe_restock(self):
        """Refill if it is looking empty. Returns True if it happened.

        No message announces this and none is needed — the next scan reports the
        higher counts. That is the whole argument for making the camera the
        source of truth (dashboard.md section 2).
        """
        total = sum(self.shelf.values())
        if total > self.capacity * len(DRINKS) * RESTOCK_AT:
            return False
        self.shelf = {drink: self.capacity for drink in DRINKS}
        self.restocks += 1
        log.info("shelf restocked")
        return True

    # --- What the board asks for --------------------------------------------

    def scan(self):
        """Answer a `CMD SCAN`: absolute counts plus a confidence figure.

        `conf` is the vision system's own reliability signal (dashboard.md
        section 7). The real CV is the least trustworthy subsystem in the
        project, so the number exists to be graphed; here it wanders a little
        rather than sitting at 100, so a panel watching it has something to
        show.
        """
        self.scans += 1
        confidence = max(60, min(100, int(self.rng.gauss(95, 6))))
        return dict(self.shelf), confidence

    def payload(self):
        """The `EVT INV` payload and the counts behind it.

        Never None: a simulated shelf always knows what is on it. The real
        backend does not have that luxury, which is why the interface allows
        the answer to be "I do not know".
        """
        counts, confidence = self.scan()
        return build_payload(counts, confidence), counts


class PiCapture:
    """Counts from the real shelf, read from a `picapture` subprocess.

    WHY A SUBPROCESS AND NOT A LIBRARY
    ----------------------------------
    The vision is C++ and OpenCV; this is Python. Running it as a child process
    keeps the serial link owned by exactly one process — `fridged` — so there is
    never a window where the board asks and two things try to answer, and no
    window where the camera is up but the link is not. A second systemd unit
    would have to be ordered against this one and could still drift out of step.

    NEWEST WINS, AND THERE IS NO QUEUE
    ----------------------------------
    `payments.py` uses a `queue.Queue` because every card request must be
    processed. This is the opposite: only the most recent count means anything,
    and a backlog would be a liability — after a stall it would hand the board a
    reading of the shelf as it was several seconds ago, presented as current. So
    the reader keeps one value under a lock, with the time it arrived, and the
    age is what the staleness rules below are built on.

    WHAT HAPPENS WHEN IT CANNOT ANSWER  (decision D1)
    -------------------------------------------------
    Two different faults, two different responses:

    * **Wobbling** — running, but the newest packet is older than
      `CAMERA_STALE_S`. Answer from it anyway, with the confidence scaled down
      in proportion to how stale it is. The sale proceeds. This is the common
      case and must not stop the fridge.
    * **Dead** — nothing for `CAMERA_DEAD_S`, or the process is not running.
      Answer nothing. `_on_scan` sends no INV, the board's `Recount` times out
      and it goes out of service. That is honest: a fridge that cannot see its
      shelf does not know what it is selling.

    Never the third option — inventing a count, or repeating a old one as though
    it were current. A wrong count is invisible in the data, and money moves on
    it.
    """

    def __init__(self, command=None, workdir=None, clock=time.monotonic,
                 autostart=True):
        #: Overridable so the tests can drive a stand-in that prints packets,
        #: which exercises every line of the subprocess machinery without a
        #: camera. Defaults to the real binary.
        self.command = list(command) if command else [
            str(config.PICAPTURE_BINARY), "--headless"]

        #: picapture reads `picapture.conf` from its WORKING DIRECTORY. Get this
        #: wrong and it starts perfectly happily on compiled-in defaults that
        #: nobody tuned, and says so only on stderr.
        self.workdir = str(workdir or config.PICAPTURE_DIR)

        self._clock = clock
        self._lock = threading.Lock()

        #: (counts, confidence, arrival) or None. One value, never a queue.
        self._latest = None

        self._process = None
        self._running = False
        self._threads = []
        self._restart_delay = config.CAMERA_RESTART_DELAY_S

        #: Diagnostics worth having on the dashboard eventually, and worth
        #: having in a log now.
        self.packets = 0
        self.malformed = 0
        self.starts = 0

        if autostart:
            self.start()

    # --- Lifetime ------------------------------------------------------------

    def start(self):
        self._running = True
        thread = threading.Thread(target=self._supervise, daemon=True,
                                  name="picapture")
        thread.start()
        self._threads.append(thread)
        return self

    def close(self):
        """Stop reading and kill the child.

        KILLING THE CHILD IS NOT OPTIONAL. An orphaned picapture keeps the
        camera device open, so the next `fridged` start cannot open it and
        fails with what looks like a hardware fault. The same trap catches a
        `--debug-all` session left running in another terminal.
        """
        self._running = False
        process = self._process
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=config.CAMERA_STOP_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                log.warning("picapture ignored terminate; killing it")
                process.kill()
        for thread in self._threads:
            thread.join(timeout=config.CAMERA_STOP_TIMEOUT_S)

    # --- The door ------------------------------------------------------------

    def door_opened(self, ts=None):
        """Stage 4 freezes the pre-open reading here. Nothing to do yet."""

    def door_closed(self, ts=None):
        """Stage 4 begins settling here. Nothing to do yet.

        Emphatically NOT the place to move a shelf: this camera observes a world
        it does not control. That `SimCamera` does move one here is the
        simulation's business and no part of the interface's meaning.
        """

    # --- Answering a scan ----------------------------------------------------

    @property
    def age_s(self):
        """How old the newest count is, or None if there has never been one."""
        with self._lock:
            if self._latest is None:
                return None
            return self._clock() - self._latest[2]

    def payload(self):
        """The `EVT INV` payload and counts, or None if it cannot be answered."""
        answer = self.scan()
        if answer is None:
            return None
        counts, confidence = answer
        return build_payload(counts, confidence), counts

    def scan(self):
        """`(counts, confidence)` from the newest reading, or None."""
        with self._lock:
            latest = self._latest
            age = None if latest is None else self._clock() - latest[2]

        if latest is None:
            log.error("the camera has not produced a single count yet - "
                      "not answering, rather than guessing")
            return None

        counts, confidence, _ = latest

        if age >= config.CAMERA_DEAD_S:
            log.error("the newest count is %.1fs old (dead above %.1fs) - "
                      "not answering. The board will fault, which is correct: "
                      "we cannot see the shelf.",
                      age, config.CAMERA_DEAD_S)
            return None

        # Ramped rather than a cliff, and downward only. There is no age at
        # which a count abruptly stops being informative; it decays, and the
        # number the board and the dashboard see should decay with it.
        if age > config.CAMERA_STALE_S:
            span = config.CAMERA_DEAD_S - config.CAMERA_STALE_S
            scale = max(0.0, 1.0 - (age - config.CAMERA_STALE_S) / span)
            was = confidence
            confidence = int(confidence * scale)
            log.warning("answering from a %.1fs old count; confidence %d -> %d",
                        age, was, confidence)

        return counts, confidence

    # --- The reader ----------------------------------------------------------

    def _supervise(self):
        """Run picapture, and keep running it. One restart at a time."""
        while self._running:
            started = self._spawn()
            if not started:
                # Backoff, but responsively: a sleep of the full delay would
                # make close() wait for it.
                self._sleep_interruptibly(self._restart_delay)
                self._restart_delay = min(self._restart_delay * 2,
                                          config.CAMERA_RESTART_MAX_S)
                continue

            self._read_stdout()          # returns when the process ends

            if not self._running:
                return
            code = self._process.poll()
            log.error("picapture exited (code %s); restarting in %.0fs",
                      code, self._restart_delay)
            self._sleep_interruptibly(self._restart_delay)
            self._restart_delay = min(self._restart_delay * 2,
                                      config.CAMERA_RESTART_MAX_S)

    def _spawn(self):
        # Each restart adds a stderr reader. Without this the list grows for the
        # life of the service — the threads themselves do exit when their pipe
        # closes, but the references would not, and a camera that flaps for a
        # week would accumulate thousands.
        self._threads = [t for t in self._threads if t.is_alive()]

        try:
            self._process = subprocess.Popen(
                self.command, cwd=self.workdir,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1)
        except FileNotFoundError:
            log.error("picapture is not built: %s does not exist.\n"
                      "  Build it with:\n"
                      "    cd %s && cmake -S . -B build && cmake --build build",
                      self.command[0], self.workdir)
            return False
        except OSError as exc:
            log.error("could not start picapture: %s", exc)
            return False

        self.starts += 1
        log.info("picapture started (pid %d) in %s", self._process.pid,
                 self.workdir)

        # stderr gets a thread of its own because it MUST be drained. picapture
        # writes its startup banner and its dark-frame warnings there, and a
        # pipe nobody reads fills up and blocks the writer — which would stall
        # the counts with no error anywhere.
        thread = threading.Thread(target=self._read_stderr, daemon=True,
                                  args=(self._process,), name="picapture-err")
        thread.start()
        self._threads.append(thread)
        return True

    def _read_stdout(self):
        for line in self._process.stdout:
            if not self._running:
                break
            self._accept(line)

    def _read_stderr(self, process):
        # At INFO, not WARNING. Most of it is the startup banner and the
        # pipeline string, which are exactly what you want in the log when
        # counts look wrong; logging all of it as a warning would train
        # everybody to ignore the one line that matters.
        for line in process.stderr:
            line = line.rstrip()
            if line:
                log.info("[picapture] %s", line)

    def _accept(self, line):
        """One line of stdout. Either a packet or something to throw away."""
        parsed = parse_packet(line)
        if parsed is None:
            # Not an error on its own: the debug modes print tuning readouts to
            # stdout too. Counted so that a picapture emitting nothing BUT
            # rubbish is visible rather than silent.
            self.malformed += 1
            if self.malformed <= config.CAMERA_MALFORMED_LOG_LIMIT:
                log.debug("ignoring non-count line from picapture: %r",
                          line.strip())
            return

        counts, confidence = parsed
        with self._lock:
            self._latest = (counts, confidence, self._clock())
        self.packets += 1
        # A packet arrived, so whatever was wrong is over. Reset the backoff
        # here rather than at spawn: a process that starts and immediately dies
        # would otherwise never back off at all.
        self._restart_delay = config.CAMERA_RESTART_DELAY_S

    def _sleep_interruptibly(self, seconds):
        deadline = time.monotonic() + seconds
        while self._running and time.monotonic() < deadline:
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
