# Camera integration — the remaining path

How the OpenCV can detection gets from "a program that prints counts" to "the
thing the fridge charges people from".

**Stages 1–4 are done and proven on the fridge** (2026-08-07): the firmware
basket logic, the vision, the subprocess, and the baseline latch with settling.
Stages 5–7 remain. `TEST.md` records what was measured and how.

## Where we are

```
                                          ┌─ SimCamera ── makes numbers up
  board ──CMD SCAN──> fridged ────────────┤
        <──EVT INV───                     └─ PiCapture ──stdout── picapture
                                                                  (real shelf)
```

The two halves are connected. `--camera picapture` runs the vision as a child
process and answers the board from what it actually sees; `--camera sim` is
unchanged and still the default.

The two scans are answered *differently*: the baseline from the last reading
that held still while the door was shut, the recount only once the picture stops
moving. Measured on the fridge — fifteen door cycles, fifteen correct answers,
recount settling in 550-650 ms and never once hitting a timeout.

The firmware needs no further changes. It already asks at the right two moments,
diffs the answers, and handles put-backs, swaps and restocks — see
`checkout.cpp`. It is waiting on an honest answer.

## The path

| Stage | What it delivers | Size | Blocked by |
| --- | --- | --- | --- |
| 2 ✅ | picapture says how sure it is | Small | — |
| 3 ✅ | `fridged` runs picapture and reads its counts | Medium | 2 |
| 4 ✅ | The two scans are answered differently, with settling | **Large** | 3 |
|   | └ 15/15 door cycles correct on the fridge, T6 | | |
| 5 | The confidence reaches the dashboard | Small | 2, 4 |
| 6 | It survives a reboot as a service | Small | 3 |
| 7 | Acceptance against a real shelf and real money | — | all |

Stage 4 is the one with the real design content. Stages 2, 3 and 6 are
plumbing; stage 5 is presentation.

---

# Stage 2 — picapture reports how sure it is ✅

**Goal:** the packet carries a quality figure, so that a count nobody should
have trusted can be told from one that was solid.

```
coke:5,fanta:4,mtndew:5,solo:3;conf=87;
```

This is a **per-frame** figure — how well-formed *this* look was. It is not the
same as the confidence eventually sent to the board, which also accounts for
whether successive frames agreed (stage 4). Keeping them separate matters: one
says "the picture was good", the other says "the shelf held still".

Four deductions from 100, every one of them a thing observed:

- **Uncalibrated drinks.** A drink with no `can_area` counts one can per blob,
  so it cannot notice two touching cans and cannot supply the area evidence
  below. It reports lower confidence rather than pretending.
- **Blobs off a whole can.** The most direct evidence available. A blob at 1.02×
  or 1.98× the size of one can says plainly what it is; one at 1.5× is a coin
  toss whose outcome decides whether somebody is charged.
- **Discarded blobs.** A blob thrown out by the area filter is something the
  camera saw and could not explain. Only ones at least half of
  `contour_min_area` count — every mask holds dozens of few-pixel fragments and
  none of them says anything about the count. Without that floor the deduction
  would sit at its cap permanently and carry no information at all.
- **Exposure.** Outside a sensible brightness band, hue is guessed rather than
  measured. Ramped, not a cliff.

Deductions rather than factors multiplied together, because the number has to be
explainable. `a` in `--debug-all` prints the arithmetic in full, and it is the
same arithmetic that produced the figure on the wire.

An empty shelf scores 100 — correctly seeing nothing is a good frame. The
candidate-pixel fraction is deliberately not an input, even though the dark-frame
detector uses it, because an empty shelf honestly has almost no candidate pixels
and letting that lower the score would make the figure mean two things at once.

**No frames are written.** The original plan had picapture saving an annotated
JPEG for the dashboard; that was dropped by decision D3 below.

### Files

`picapture/src/vision_config.{h,cpp}` · `picapture/src/main.cpp` ·
`picapture/tests/test_vision_config.cpp` · `picapture/README.md` ·
`documentation.md`

### Done when

Config tests pass — 102 checks, up from 84. Still needs proving on hardware
(`TEST.md` stage T3): the packet carries `conf=`, and it visibly drops when you
half-cover a can or dim the light.

---

# Stage 3 — `fridged` runs picapture ✅

**Goal:** replace `SimCamera` with something reading real counts, without
disturbing the simulated path.

### 3.1 The subprocess

`fridged` spawns `PiCapture --headless` and reads its stdout on a daemon thread,
with a second thread on stderr — a pipe nobody drains fills up and blocks the
writer, which would stall the counts with no error anywhere.

**One value under a lock, not a queue.** `payments.py` uses a `queue.Queue`
because every card request must be processed; this is the opposite. Only the
newest count means anything, and a backlog is a liability — after a stall it
would hand the board a reading of the shelf as it was seconds ago, presented as
current. The arrival time is kept with it, and the age is what the staleness
rules are built on.

The rest, all of it load-bearing:

- **Filter the output.** picapture prints diagnostics to stdout too.
  `parse_packet()` is strict, and it checks the key set **as a whole** rather
  than filtering down to what it recognises: a picapture built from a different
  brand list would otherwise have its counts accepted for whichever drinks
  happened to match, and both ends would go on agreeing about the wrong numbers.
- **stderr goes to the log at INFO.** Most of it is the startup banner and the
  pipeline string, which are exactly what you want when counts look wrong.
  Logging all of it as a warning would train everybody to ignore the one line
  that matters.
- **Restart if it dies**, doubling backoff to a ceiling, reset when a packet
  arrives rather than when a process starts — otherwise something that starts
  and immediately dies would never back off at all.
- **Kill the child on shutdown.** An orphaned picapture keeps the camera device
  open, so the next start fails with what looks like a hardware fault.
- **Working directory** is the picapture folder, or `picapture.conf` is not
  found and it runs on compiled-in defaults nobody tuned.
- **A missing binary names the build command** and leaves the supervisor
  retrying, rather than raising out of a daemon thread where nobody sees it.

### 3.2 Answering, and refusing to answer — decision D1

Implemented here rather than deferred, because it decides the shape of
`payload()`. Stage 4 adds settling on top; these rules are checked first, since
there is nothing to wait for if the newest reading is already dead:

| Age of the newest count | What happens |
| --- | --- |
| under `CAMERA_STALE_S` (1 s) | Answered at full confidence |
| 1 s to `CAMERA_DEAD_S` (5 s) | Answered, confidence ramped down with age |
| over 5 s, or never | **Not answered at all** |

`payload()` returns `None` for that last case and `_on_scan` sends no INV, so
the board's `Recount` times out and it goes out of service. Comfortably inside
the board's 8 s budget, so the fault is our verdict rather than a timeout nobody
decided on.

### 3.3 The interface changed shape

`_on_door` used to call `camera.customer_takes()` and `camera.maybe_restock()` —
statements about a world the *caller* controls. A real camera observes a world
it does not, and would have had to grow no-op versions of both to fit. Replaced
with `door_opened(ts)` / `door_closed(ts)`, named for the event rather than for
what the simulation does with it. `SimCamera` moves its shelf in `door_closed`;
that is the simulation's business and no part of the interface's meaning.

This is the seam stage 4 needs, which is why it was worth doing now rather than
twice.

### 3.4 Choosing a camera

`--camera sim|picapture`, deliberately parallel to `--port` and `--square`:
simulated by default, real hardware opt-in. **The safe option is the one you get
by saying nothing.** Independent of `--port`, so a real camera against a
simulated board is available — the useful hybrid for testing the vision without
standing at the fridge pressing buttons.

### Files

`fridged/camera.py` · `fridged/ingest.py` · `fridged/__main__.py` ·
`fridged/config.py` · `tests/test_fridged.py`

`FRAMES_DIR` and `LATEST_JPG` were deleted from `config.py` here rather than in
stage 5 — D3 means nothing will ever write there, and a reservation that looks
like an intention is worse than no reservation.

### Done when

Done. 39 new checks, including the subprocess machinery driven for real against
a stand-in that prints packets: Popen, both pipes, restart-on-exit, the missing
binary, and a full `FakeBoard` → `Link` → `Ingest` loop answered from a child
process's stdout. **Still needs the real binary on the Pi** — see `TEST.md` T5.

---

# Stage 4 — Answering the two scans ✅

**The stage with the actual design in it.** Everything else is plumbing.

> **Status: done, and validated on the fridge (2026-08-07).** Fifteen door
> cycles, fifteen correct answers — single cans, a two-can basket, put-backs,
> and five cycles where nothing was touched. No false charge and no missed one.
>
> The four constants were placeholders and are now **measurements confirmed
> rather than changed**: the recount settles in 550-650 ms, and neither
> `SETTLE_TIMEOUT_S` nor `SCAN_ANSWER_BUDGET_S` has ever fired. Raising
> `SETTLE_FRAMES` to 5 was considered and **rejected on the evidence** — it
> would add 500 ms to every purchase to buy margin nothing has asked for.

### 4.1 Why one answer will not do

picapture free-runs; the board asks on demand and faults after
`RECOUNT_TIMEOUT_MS` (8 s). Handing back whatever the newest packet says would
be simple and wrong, because the two scan moments are physically different:

**Baseline, at door-open.** The door is swinging, light is flooding in, and a
hand may already be reaching. This is the *worst* frame of the whole cycle. The
answer should come from the last stable reading taken **while the door was still
shut** — which is what the baseline is supposed to describe anyway.

**Recount, at door-close.** The scene is good but not yet settled: the door has
just moved, the light is changing back, and cans may still be rocking. Wait for
several consecutive agreeing packets, then answer.

### 4.2 Ordering makes the latch possible

The board sends `EVT DOOR state=open` **before** `CMD SCAN` — `notify_door()`
runs in the common section of `handle_event()`, ahead of the per-state switch
that calls `request_scan()`. So `fridged` always sees the door move first and can
freeze the pre-open value before the scan arrives. This is worth stating because
the design depends on it and it is not obvious from either file alone.

### 4.3 `_on_scan` has to be allowed to defer

Currently `_on_scan` replies immediately. It cannot block — the same loop drains
the serial link and flushes the database — so settling means:

1. `CMD SCAN` arrives. If a settled answer exists, reply now.
2. Otherwise record that a reply is owed, and reply from `tick()` when the
   camera settles or the deadline arrives.

This is the single largest change to `ingest.py` and the one most likely to
introduce a subtle bug. It needs tests that drive it with a fake clock.

Budget: answer by about 6 s, leaving 2 s of margin inside the board's 8 s.

### 4.4 The camera interface — three answers, not two

```
door_opened(ts)     SimCamera: nothing.        PiCapture: freeze the latch.
door_closed(ts)     SimCamera: move its shelf. PiCapture: begin settling.
payload(force=)     -> (payload, counts) | PENDING | None
```

`PENDING` is the piece that could not be added early. `None` means *cannot*;
`PENDING` means *ask me again in a moment*. Collapsing them would either take
the fridge out of service on every unsettled scan, or make a dead camera look
like a slow one — the caller's response to the two is opposite.

`SimCamera` never returns `PENDING` and accepts `force=` so the service's
deferred path can call it without asking which backend it holds. Every existing
test stays meaningful.

### 4.4a A baseline is *supposed* to be old

Found by testing rather than by reading, and worth recording because the fix is
not obvious: the age penalty must be measured **up to the moment the baseline
was frozen**, not up to the present.

Measured to the present, a perfectly healthy camera and a customer deliberating
for fifteen seconds — half the board's own `SELECT_TIMEOUT_MS` — drove the
baseline's confidence to **zero**. Nothing could have changed the shelf between
that reading and the door opening, so the time since is not evidence against it.

What the penalty still catches is a baseline that was already stale *when it was
frozen* — a camera that had stopped producing before the door was touched.

### 4.5 Confidence, properly

picapture's per-frame figure from stage 2 is a statement about the *picture*.
Everything this side adds is a statement about how much of it still applies, and
so both adjustments are **reductions that can never raise the number**:

| Reduction | Because |
| --- | --- |
| Age ramp | The shelf may have changed since. Ramped, not a cliff. |
| `UNSETTLED_CONFIDENCE_SCALE` | The picture never held still — "I ran out of time" is a different claim from "the shelf was steady". |

The second one is the only trace an unsettled answer leaves. **The counts
themselves look identical to a clean read**, so without it a forced answer and a
settled one are indistinguishable in the data forever after.

### 4.6 What to do when the camera cannot answer

**Decided (D1).** Two different faults, two different responses, and the
distinction is what makes this workable:

**Wobbling.** picapture is running and answering, but the frames have not
settled by the deadline — a hand still in shot, the light still changing. Answer
anyway, from the most recent reading, with a low confidence that is logged and
graphed. The sale proceeds. This is the common case and it must not stop the
fridge.

**Dead.** The subprocess is not running, or has produced nothing for long
enough that "most recent reading" is meaningless. Do not answer. The board
faults after `RECOUNT_TIMEOUT_MS` and goes out of service, which is honest: a
fridge whose camera is gone cannot know what it is selling.

The threshold between them is `CAMERA_DEAD_S`, and **it is checked before
settling, not after**. There is nothing to wait for if the reading everything
would be built on is already dead; waiting the full settle timeout to then
refuse would spend the board's budget to reach the same answer.

Nothing is charged for a can nobody can see, and nothing is *given away* for
one either. The `--camera sim` path is unaffected: it always answers
immediately.

### The two timers, and why there are two

```
door shuts ──┬─ SETTLE_TIMEOUT_S (4 s) ── camera gives up, answers marked down
             └─ SCAN_ANSWER_BUDGET_S (6 s) ── service forces a reply
                                     └─ RECOUNT_TIMEOUT_MS (8 s) ── board faults
```

The camera's timer is the one that normally fires. The service's is a
**backstop**: it exists so that an owed reply can never be held forever if the
camera stops resolving. Both sit inside the board's 8 s, so a fault is our
verdict rather than a timeout nobody decided on — and a test asserts that
ordering, so shortening the board's budget without revisiting these fails
loudly.

### Files

`fridged/camera.py` · `fridged/ingest.py` · `fridged/config.py` ·
`tests/test_fridged.py`

### Done when

Machinery: **done.** 38 new checks — the latch excluding a hand across the
shelf, the agreement run resetting at door-close, both deadlines, a camera dying
mid-settle, `SimCamera` still answering instantly, and a whole door cycle
producing two snapshots that differ by exactly one can.

Tuning: **not done.** `TEST.md` T6 measures the four constants. Until then the
acceptance criteria stand unproven: a door cycle with nothing taken reliably
reports no change, and taking one can reports exactly one.

---

# Stage 5 — The confidence on the dashboard

**Goal:** a person can see how much to trust what the camera reported.

**No camera frames, by decision D3.** Counts and confidence are stored; pictures
are not. That removes the HTTP server this stage originally called for, along
with the whole question of who on the network could watch it — which is why it
dropped from Medium to Small. `FRAMES_DIR` and `LATEST_JPG` in `config.py` were
reserved for a frame server that is now never going to exist, and should be
deleted rather than left as an invitation.

### 5.1 Store the confidence

`conf=` currently goes out on the wire and is **thrown away** — `_on_scan`
writes `stock_snapshot` and nothing else. Store it as a `measurement`, which is
exactly what the generic metrics table exists for.

Also worth recording which scan a snapshot came from: `stock_snapshot.trigger`
already has a column for it, and `'door_open'` versus `'door_close'` is the
difference between a baseline and a recount.

### 5.2 Panels

- Confidence over time, with the existing stock graph.
- A low-confidence annotation, so a suspicious sale can be traced to a scan
  nobody trusted.

Without the frame, this graph is the *only* way to tell a solid count from a
guess after the fact. That makes it more important than it was when there was a
picture to fall back on, not less.

### Files

`fridged/ingest.py` · `fridged/config.py` ·
`grafana/dashboards/fridge-overview.json` · `tests/test_fridged.py`

### Done when

The dashboard shows a confidence graph alongside stock, low-confidence scans are
visibly marked, and the dashboard test suite passes with the new panels.

---

# Stage 6 — Deployment

**Goal:** it comes back on its own after a power cut.

`fridged.service` runs as `User=__USER__`, `Group=grafana`. Adding a camera
subprocess needs:

- `SupplementaryGroups=video` — picapture cannot open the camera without it.
- A working directory picapture can find `picapture.conf` from. Without it the
  service silently runs on compiled-in defaults that nobody tuned.
- Check whether `PrivateTmp=true` and `ProtectSystem=full` interfere with
  libcamera. They may not; find out deliberately rather than by outage.

**One process, not two.** `fridged` spawning picapture keeps the camera's
lifetime and the serial link in the same process, so there is no window where
the board asks and nothing is listening. A second unit would have to be ordered
against the first and could still drift out of step.

`deploy/install.sh` needs the same changes so a fresh install matches.

### Files

`deploy/fridged.service` · `deploy/install.sh` · `deploy/README.md`

### Done when

`sudo reboot` brings the whole thing back with no manual step, and
`systemctl status fridged` is clean.

---

# Stage 7 — Acceptance

Not code. The tests that decide whether this is fit to take money.

1. **Stock a real shelf.** Counts match reality for five minutes untouched.
2. **Buy one can.** Charged for exactly that can, at $2.00.
3. **Put it back.** Returns to Idle, nobody charged.
4. **Swap it.** The new drink and the right price.
5. **Take three.** $6.00.
6. **Touching cans.** Two of one drink pushed together still count as two.
7. **Restock.** Refill mid-session; nobody charged.
8. **Twenty cycles.** Count the disagreements. This is the number that decides
   whether it goes in the fridge.
9. **Pull the camera cable mid-transaction.** Whatever 4.6 decided should be
   what happens.

Test 8 is the real gate. A system that is right nineteen times in twenty is
wrong about one sale a day, and each wrong sale is somebody's $2.

---

# Decisions, settled

These were the three that changed the code. All answered.

### D1 — What happens when the camera cannot give a trustworthy answer?

**Intermittent trouble: answer anyway from the latest reading, at low
confidence. Genuinely dead: do not answer, and let the board fault out of
service.** Written up in full at 4.6.

The split is what makes it workable. A camera that is running but unsettled is
the ordinary case — a hand in shot, the light still changing — and stopping the
fridge for it would mean stopping it constantly. A camera that is *gone* is a
fridge that cannot know what it is selling, and being out of service is the
honest state to be in.

### D2 — Does the camera frame go on the LAN or stay on localhost?

**Moot — no frames are served.** The question only arose because stage 5
originally put the annotated frame on the dashboard, which needed an HTTP server,
which needed a bind address. D3 removed the frame; D2 went with it.

Worth recording why the question existed, because it will come back if anyone
proposes putting the picture on the dashboard later: the camera points at a
shelf in a shared space and will catch people, so a LAN-visible feed is a
privacy decision rather than a convenience one.

### D3 — How much history of frames?

**None. Counts only.** No `latest.jpg`, no ring buffer, nothing written to disk
but numbers. The image exists inside picapture for the length of one frame and
is visible only in the debug modes, to whoever is standing at the fridge.

Consequences, stated plainly:

- The frame-writing half of stage 2 was dropped before it was built, and stage 5
  lost its HTTP server. Both got smaller.
- `FRAMES_DIR` and `LATEST_JPG` in `config.py` are now dead reservations and
  should be deleted in stage 5, not left looking like an intention.
- **A disputed charge cannot be audited against a picture.** The confidence
  figure and the two stock snapshots are the whole record. That is the trade,
  and it is the reason stage 5's confidence graph matters more than it did.

---

# Outside this plan

Real, but not part of the camera work:

- **The Square sandbox token in git history** (`documentation.md` issue 13).
  Sandbox money, but it is committed and pushed. Rotate it.
- **`fridged` runs `--square real` in production** — worth confirming that is
  intended.
- **Hardware verification of everything already written**, which is `TEST.md`.
  Stage 3 onwards can be built without it, but nothing should reach the fridge
  until T2 has passed on the board.

---

# If the vision does not work well enough

Worth deciding early rather than at the end.

The escape hatch is that `fridged` keeps a camera *interface*, not a camera. If
counts prove unreliable enough that test 7.8 fails, the options are, roughly in
order of preference:

1. **Change the physical problem, not the software.** Separate the cans, add a
   diffuse light, move the camera square-on. Most of the difficulty so far has
   been lighting and geometry rather than algorithm.
2. **Reduce what is asked of it.** Detecting *that the shelf changed* is much
   easier than detecting *by how much*; a fallback that charges a flat price per
   door cycle is worse but workable.
3. **Fall back to `--camera sim`** for a demo, with the limitation stated
   plainly rather than hidden.

Being able to say which of these is happening, from the dashboard, is itself an
argument for finishing stage 5.
