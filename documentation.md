# CC3501 Smart Fridge — Software Documentation

Closed-loop vending system for the Cairns Engineering Society fridge. Product
selection is recognised visually, payment is taken by cash or card, and stock,
temperature and faults are recorded for a dashboard.

**Authors:** Alister Maltby, Damien Turner, Nils Eisen
**Repository:** `github.com/enolf/CC3501-project-software`

This is the technical reference: how each part works and, more importantly, why
it works that way. For getting it installed and running, start with
[README.md](README.md).

**State of the code as of 8 August 2026.** Every subsystem below is written and
building; §8.2 records which have been proven on the fridge and which have not.

---

## Contents

1. [System overview](#1-system-overview)
2. [Repository layout](#2-repository-layout)
3. [Building, and the switches that matter](#3-building-and-the-switches-that-matter)
4. [Secrets, and what must never be committed](#4-secrets-and-what-must-never-be-committed)
5. [RP2040 software](#5-rp2040-software)
6. [The RP2040 ↔ Pi link](#6-the-rp2040--pi-link)
7. [Pi4 software](#7-pi4-software)
8. [Testing](#8-testing)
9. [Known issues](#9-known-issues)
10. [Decisions, settled](#10-decisions-settled)

---

## 1. System overview

Two computers share the work.

| | RP2040 (custom PCB) | Raspberry Pi 4 |
|---|---|---|
| **Role** | Sensing, user interface, payment terminal | Vision, internet, storage |
| **Language** | C++17 / C11, Pico SDK 2.2.0 | Python 3, C++17 (OpenCV) |
| **Owns** | TFT + touch, RFID, temperature, coin scale, door switch, SD log | Camera, can recognition, Square API, SQLite, Grafana |

The board is the thing that decides. The Pi answers questions and records
answers; it never drives the transaction.

### The transaction, end to end

```
IDLE  (black screen, or the society logo)
  |
  |  door opens, or an approved card is tapped (-> GREETING, by name)
  v
SELECTING          the Pi is asked to scan. It answers with the shelf as it
  |                was BEFORE the door moved — see §7.1
  |  door closes
  v
RECOUNT            the Pi waits for the picture to hold still, then counts
  |                again. basket = baseline - recount
  |
  +-- nothing taken ------------------------------> IDLE
  +-- more on the shelf than before (a restock) --> IDLE
  |
  v
PAYMENT SELECT     the basket and the total, with CASH and ONLINE targets
  |
  +-- CASH    -> coins land on the load cell, $1 and $2 told apart by mass,
  |              screen counts up to the total
  |
  +-- ONLINE  -> the Pi asks Square for a payment link, the board renders it
  |              as a QR code and waits for Square to confirm
  v
THANK YOU  ->  IDLE

  timeout on either path -> ABANDONED: the drink is recorded as stolen, and
  the system returns to IDLE. Theft is logged, not prevented — there is no lock.
```

The door is what gates progress: nothing advances toward payment until it is
shut, because that is the only moment the camera can see a stable, unobstructed
shelf.

## 2. Repository layout

```
CC3501-project-software/
├── README.md                     what it is, and how to install it
├── documentation.md              this file
├── .gitmodules                   lvgl + pico-scale
│
├── rp2040-software/
│   ├── CMakeLists.txt            firmware build and all its switches
│   ├── src/
│   │   ├── board.h               ALL wiring and calibration
│   │   ├── timings.h             every interval that shapes how it feels
│   │   ├── sim_config.h          which keyboard stand-ins are compiled
│   │   ├── main.cpp              the superloop, and nothing else
│   │   ├── drivers/              hardware adapters, one per device
│   │   └── peripherals/          behaviour and application logic
│   ├── lib/
│   │   ├── lv_conf.h             LVGL configuration
│   │   ├── lvgl/                 SUBMODULE, release/v9.2
│   │   ├── pico-scale/           SUBMODULE (has a nested submodule)
│   │   └── fatfs/                vendored ChaN FatFs R0.16
│   ├── tests/host/               off-hardware unit tests (firmware_tests)
│   └── docs/bringup/             standalone per-device bring-up programs
│
└── pi4-software/
    ├── fridged/                  the service
    │   ├── __main__.py           argument parsing and the service loop
    │   ├── link.py               framed serial transport, self-reopening
    │   ├── ingest.py             frames -> meaning. No SQL.
    │   ├── store.py              the schema and every write. No protocol.
    │   ├── camera.py             what answers CMD SCAN
    │   ├── payments.py           Square, on a worker thread
    │   ├── config.py             every path, interval and host quirk
    │   └── seed.py               backfill plausible history for the dashboard
    ├── picapture/                OpenCV can recognition (C++)
    ├── grafana/                  dashboard JSON + provisioning
    ├── deploy/                   systemd units, install and backup scripts
    ├── tools/                    protocol.py, fake_board.py, fake_pi.py
    └── tests/                    test_fridged.py
```

### The rule that shapes all of it

`ingest.py` knows the wire protocol and no SQL. `store.py` knows SQL and nothing
about the wire. That line is what makes a protocol change one file and a schema
change one other file. The firmware has the same split: `drivers/` know
hardware, `peripherals/` know behaviour, and `main.cpp` knows only sequencing.

## 3. Building, and the switches that matter

[README.md](README.md) has the step-by-step install. This section covers what
the build options do.

### 3.1 Submodules

`lvgl` and `pico-scale` are submodules, and `pico-scale` carries a nested one
(`hx711-pico-c`). A plain `git clone` leaves them empty and the firmware fails
on a missing `lvgl.h` or `hx711.h`. Clone with `--recurse-submodules`, or run
`git submodule update --init --recursive` afterwards, and confirm with
`git submodule status --recursive` that all three appear.

> **Historical note, because the symptom was confusing.** `lvgl` was declared in
> `.gitmodules` but its 2,775 files were also committed directly to this
> repository, so `--init` populated nothing and the declaration was simply
> untrue. Both its gitdir pointer and its `core.worktree` were left over from
> before the repository was split into `rp2040-software/` and `pi4-software/`
> and pointed one level too high. Fixed by verifying the vendored tree was
> byte-identical to upstream `release/v9.2` and then converting it to a real
> gitlink.

### 3.2 Firmware build switches

| Switch | Default | Effect |
|---|---|---|
| `PI_LINK_BACKEND` | `sim` | Which implementation of `pi_link.h` is compiled. `sim` fabricates the camera and Square from the keyboard; `serial` is the real transport. Only one is built, so the other costs no flash. |
| `SIM_ALL` (and `SIM_DOOR`, `SIM_NFC`, `SIM_COINS`, `SIM_TOUCH`) | `ON` | Whether keyboard stand-ins exist alongside the real hardware. Real hardware is *always* compiled and always attempted; these only add the fake inputs. |
| `IDLE_LOGO` | `OFF` | Black idle screen, or the society logo. A plain `set()` rather than `option()` so editing the file always wins over a stale cache. |

**A build that goes in the fridge must be `-DSIM_ALL=OFF`.** With the stand-ins
compiled in, anyone who can reach the USB socket can fake a paid transaction by
typing a character. The firmware says which kind of build it is in its banner,
at `WARNING` level, so the two can never be confused in a log.

### 3.3 Footprint

Measured on the `sim` backend, `-DSIM_ALL=ON`, `IDLE_LOGO=OFF`:

```
   text     data      bss
 597776        0   111868
```

That is roughly **584 KB of the RP2040's 2 MB flash** and **109 KB of its
264 KB RAM**. The great majority of both is LVGL and its buffers.

### 3.4 Warnings

`-Wall -Wextra` is applied to *this project's* sources only, listed by name in
`PROJECT_SOURCES`. It is applied per source file rather than with
`target_compile_options`, which looks like the obvious choice but is wrong here:
`pico-scale` is an INTERFACE library, so its sources compile into the `labs`
target and would inherit the flags, producing 67 warnings in vendored code this
project does not police.

**The baseline is 2 warnings, both third-party**, from
`hx711_multi.h` reporting `static` functions declared but never defined,
surfaced through `mass_sensor.cpp`. GCC raises `-Wunused-function` at end of
translation unit rather than at the header, so `-isystem` does not suppress it.
Silencing it would need `-Wno-unused-function` on that file, which would also
hide genuinely unused statics in our half of it.

**"No new warnings" means this build stays at 2, with 0 from `src/`.**

## 4. Secrets, and what must never be committed

**The rule.** Credentials and personal data live in files every developer
creates by hand and nobody commits. Nothing else in the repository may contain a
token, a key, a password, or a card UID — not in a comment, not in a test
fixture, not in a commit message.

| Pattern | Ignored in | Why |
|---|---|---|
| `tokens.py`, `secrets.py` | root + `pi4-software/` | The credential files themselves |
| `__pycache__/`, `*.py[cod]` | root + `pi4-software/` | **Compiled bytecode contains the token — see below** |
| `credentials.json`, `*.pem`, `*.key` | root | Standard credential formats, ignored pre-emptively |
| `.env`, `.env.*` | root | Ditto |
| `access_list.h` | `rp2040-software/` | Card UIDs paired with people's names |

Both `tokens.py` rules are written twice, once as a path and once as a bare
name, so a copy left in a scratch folder is caught as well. `access_list.h` is
written twice for the same reason.

### 4.1 `access_list.h` — the approved card list

`src/peripherals/access_control/access_list.h` holds the RFID UIDs the fridge
greets by name. README.md §2 has the file to create.

**Why this is treated as a secret, and why it is worse than a token.** A UID is
not a credential you can change. It is burned into the card at manufacture, so
there is no rotation step — the only remedy for publishing one is issuing that
person a new card. Paired with a name it is also personal data about somebody
who did not choose to be in a public repository.

**It does not arrive with a clone**, so a fresh checkout will not build until
you write it. That is deliberate, and the `#error` names this section. A build
that failed loudly is better than one that silently shipped somebody else's
placeholder UIDs.

The type it fills in, `ApprovedUser`, is declared in `access_control.cpp`
immediately above the `#include`. That ordering is why the include sits in the
middle of the file rather than at the top.

### 4.2 The bytecode trap — a real incident, not a hypothetical

`tokens.py` was correctly gitignored from the start. **The token still reached a
public GitHub repository**, inside
`pi4-software/online-payment/__pycache__/tokens.cpython-313.pyc`.

Importing a module makes CPython write a `.pyc` next to it, and **a `.pyc`
embeds the string constants of the module it compiled**. So the moment
`square.py` ran `import tokens`, a second file containing
`SQUARE_ACCESS_TOKEN` in plain text appeared — one the `tokens.py` rule did not
match. `strings` recovers both the token and the location id from it without any
tooling. The `.pyc` also embedded the absolute source path, exposing the build
machine's username.

The lesson generalises past Python: **ignoring a secret file is not the same as
ignoring its derivatives.** Compiled output, caches, editor backups, logs and
core dumps can all carry a copy of something the original rule protected. When
adding a secret to `.gitignore`, ask what else on disk will contain the same
bytes.

**Checking before you commit** — this catches the whole class, not just Python:

```bash
# Anything staged that looks like a Square credential
git diff --cached | grep -iE "EAAA[A-Za-z0-9_-]{20,}|sq0[a-z]{3}-|access_token"

# Anything staged that is compiled or cached rather than written by hand
git diff --cached --name-only | grep -E "__pycache__|\.py[cod]$|\.env"
```

Both should print nothing.

**If a credential does get committed, rotate it.** Removing the file in a later
commit does not help — the old commit still contains it, and on a public repo it
must be assumed scraped within minutes. Rotation at the provider is the only fix
that works; history rewriting is optional cleanup afterwards.

## 5. RP2040 software

### 5.1 Conventions

Three rules shape the layout:

- **All board wiring lives in `board.h`.** Pin numbers, bus addresses,
  calibration constants. A board revision should be a header edit, nothing more.
- **`drivers/` know hardware; `peripherals/` know behaviour.** Application code
  calls driver functions and never touches a register.
- **`main` stays thin** — top-level sequencing only.

These apply to code this team wrote. The vendored libraries (`lvgl`,
`pico-scale`, `fatfs`, the Pico SDK) are exempt: they are dependencies, not our
source.

### 5.2 `board.h`

Single source of truth for wiring. Set `ACTIVE_DISPLAY_ORIENTATION` and the
resolution, MADCTL value and touch axis flags all follow from it — a pattern
worth keeping.

| Group | Contents |
|---|---|
| DS18B20 | Bus pin (GP7), power mode, strong-pull-up FET pin (GP8) and polarity |
| RFID | `i2c1` on GP2/GP3, 100 kHz, module address `0x2C` |
| Switches | Door limit switch (GP6), user button (GP15), both active-high |
| TFT | `spi0` on GP16–GP22, touch CS/IRQ on GP24/GP25, two bus rates |
| microSD | Shares the display's SPI bus; only CS (GP23) is its own |
| HX711 | Data/clock (GP10/GP11), rate strap, counts-per-gram calibration |

**One nuance worth knowing about the SPI bus.** The display, the touch
controller and the SD card all share SCK/MOSI/MISO and run at very different
clock rates, so every device has to leave the other two deselected and put the
bus rate back where it found it. A card that fails to release MISO when
deselected will corrupt touch readings — suspect that first if touch misbehaves
after fitting one.

### 5.3 Drivers

| Driver | Lines | Device |
|---|---:|---|
| `ds18b20/` | 684 | 1-Wire temperature sensors |
| `sd_card/` | 624 | microSD over SPI, plus the FatFs disk layer |
| `mfrc522/` | 420 | PiicoDev RFID reader (I2C) |
| `mass_sensor/` | 252 | HX711 load cell amplifier |
| `ili9341/` | 230 | TFT + XPT2046 touch (SPI) |
| `digital_switch/` | 179 | Debounced GPIO switch |
| `logging/` | 116 | Severity-filtered serial log |

**`ds18b20`** is the most thorough piece of code here. Full 1-Wire ROM search so
multiple sensors share one pin, CRC-8 verification on both ROM codes and
scratchpad reads, and both power modes. The parasite-power path interlocks the
data pin against the strong-pull-up FET so the two can never fight — without
that, driving the data line low while the FET is on shorts 3V3 to ground through
the RP2040 pin.

**`mfrc522`** implements the ISO 14443A stack: REQA, anticollision across
cascade levels, and both 4-byte and 7-byte UIDs. Verifies `VersionReg` at init
before configuring anything. **Every I2C return value is checked** and a failure
is logged once rather than on every retry.

**`ili9341` / `xpt2046`** share one SPI bus at two clock speeds (30 MHz display,
2 MHz touch), switching baud rate around each touch read. The touch controller
needs a dummy transaction at init to clear the RP2040's RX FIFO, otherwise it
locks up — a hardware quirk that cost real debugging time and is commented in
place. The SPI writes deliberately do not check their return values, and the
reason is stated in the file: SPI has no acknowledgement, so `spi_write_blocking`
always returns the length it was given and a check could never be false.

**`mass_sensor`** wraps the HX711 for coin detection. Reads are **non-blocking**
(`poll()` returns false until a conversion is ready, so the caller is never
stalled for 100 ms), and readings are **relative to a baseline** captured by
`tare()` rather than absolute — so the money box's own weight and any coins
already in it drop out of the arithmetic. `init()` proves the chip is present by
demanding an actual conversion within a timeout, and rejects a saturated first
reading, which indicates an open load cell bridge.

### 5.4 Peripherals

| Peripheral | Lines | Purpose |
|---|---:|---|
| `pi_link/` | 1535 | The link to the Pi: codec, plus a `sim` and a `serial` backend |
| `checkout/` | 1368 | The transaction state machine |
| `tft_display/` | 855 | LVGL port, touch input, and every screen |
| `sd_log/` | 403 | RAM-buffered log, dumped to the card on demand |
| `coin_acceptor/` | 309 | Identify $1/$2 coins by mass |
| `events/` | 290 | The single-producer event queue |
| `basket/` | 278 | Inventory diffing and the cash payment gate |
| `scale_task/` | 247 | Drives `mass_sensor`, raises coin events |
| `temperature_task/` | 243 | Samples the sensors and the RP2040 die |
| `sensor_health/` | 225 | Tracks sensors going missing or failing |
| `nfc_task/` | 221 | Polls the reader, raises card events |
| `switches/` | 157 | Door and button, debounced, raises events |
| `access_control/` | 120 | UID → name lookup |
| `catalogue/` | 111 | The drink list and the prices, header-only |
| `sim_input/` | 79 | Reads the debug keyboard |

**`sensor_health`** tracks sensors by ROM code rather than by count, so it
reports *which* sensor stopped answering rather than just that the number
changed. It takes arrays of ROM codes rather than a driver object, so it can be
exercised with synthetic data off hardware.

**`catalogue`** owns the drink list and the prices, and `basket` owns the counts
and the diffing. Keeping those separate is what guarantees there is exactly one
definition of what a drink costs.

### 5.5 `main.cpp` — the superloop

`main.cpp` holds top-level control flow and nothing else: bring the hardware up,
then call each task once per pass, forever.

```
run_debug_input()          keyboard stand-ins
switches::run_switches()   door and button
scale::run_scale()         coin events from the load cell
nfc::run_nfc()             card events, self-throttling
temperature::run_temperature()
pi_link::run()             scan results and Square replies
checkout::run_checkout()   THE ONLY CONSUMER, and the only thing that decides
Display::run()             LVGL redraws, animations, touch sampling
run_health_report()
run_heartbeat()
```

**The one rule for everything called from here: return promptly.** No
`sleep_ms()`, no waiting for a sensor, no blocking bus transaction. A task that
needs to wait remembers where it got to in a `static` local and picks up on the
next pass. The whole system shares one thread, so anything that blocks freezes
the display, stops coins being counted and stalls the link at the same time.
`run_heartbeat()` measures each pass and logs a warning past
`LOOP_TIME_WARN_MS` (50 ms), so a violation announces itself.

Producers run before the consumer, so an event raised this pass is handled on
the same pass rather than waiting for the next one.

Three things deliberately block, and all three are outside the loop or behind a
keypress: display init (~750 ms, required by the datasheet), the coin scale tare
(~1 s, refused unless idle), and the SD card probe (up to ~1 s, on demand only).

The previous contents of this file — one standalone `main()` per peripheral, all
commented out but one — are preserved in `docs/bringup/bringup_examples.cpp.txt`.
Each is still a known-good minimal program for exercising one device in
isolation, which is what you want when a peripheral misbehaves and you need to
know whether the fault is in the driver or in the system around it.

### 5.6 The state machine

`checkout.cpp` is **the only code in the system that changes state.** Peripheral
tasks raise events; what those events mean is decided in one place.

Seventeen states. Beyond the transaction flow in §1:

| State | What it is for |
|---|---|
| `Greeting` | An approved card was tapped. Makes "someone has identified themselves but has not opened the door" visible, rather than indistinguishable from a customer already choosing. Deliberately does *not* request a scan — that happens when the door opens, so both ways in produce the same sequence. |
| `AccessDenied` | An unknown card. A message, not an enforcement point; there is no lock. |
| `RefundOwed` | Every drink was put back *after* coins went in. There is no hopper, only a one-way box, so the only honest thing it can do is say so on screen and log the amount for a human to settle. |
| `Fault` | Not fit to trade. A terse code on screen; the detail goes to the log. |
| `UtilityMenu`, `SdResult`, `TareResult`, `UselessButton` | Maintenance, reached from Idle by holding the panel button. Things a person does *to* the fridge rather than things a customer does *with* it. |

There is no state for the SD write itself: `sd_log::dump_to_card()` blocks the
superloop start to finish, so no state machine pass could observe one. The
"Writing…" screen is drawn and forced out to the panel immediately before the
call.

Every timeout lives in `src/timings.h` rather than next to the state machine,
because several of them have to agree with numbers nowhere near it —
`SQUARE_LINK_TIMEOUT_MS` against the Pi's HTTP timeout, `LINK_TIMEOUT_MS`
against `fridged/config.py`. A constant whose correctness depends on another
file is best kept with the others.

### 5.7 Coin payment

The subsystem with the least obvious design, so it is documented in full.

**The problem.** Australian gold coins:

| Coin | Mass | Value |
|---|---:|---:|
| $1 | 9.00 g | 100c |
| $2 | 6.60 g | 200c |

The $2 is **lighter** than the $1. Mass does not increase with value, so no
scale factor converts grams to dollars — every reading must be resolved into a
specific combination of coins.

**Why not solve the whole pile.** Every achievable total is
`9.00a + 6.60b = 0.6(15a + 11b)` grams, so possible totals sit on a 0.6 g grid.
Distinguishing arbitrary combinations from absolute mass alone would need better
than ±0.3 g, and it gets harder as the box fills.

**What is done instead.** Each *change* is classified. A single coin is either
+9.0 g or +6.6 g — **2.4 g apart**, a wide margin against sub-gram noise, and
one that stays constant no matter how full the box is.

`CoinAcceptor::update()` runs four stages per sample:

1. **Rolling window** — newest reading into a 5-sample buffer.
2. **Settle test** — peak-to-peak must be under `SETTLE_BAND_GRAMS` (0.50 g).
   Peak-to-peak rather than variance, so one stray reading withholds a decision;
   while the beam rings from the impact the spread stays wide and nothing fires.
3. **Delta** — settled mean minus last accepted level. Under
   `EVENT_THRESHOLD_GRAMS` (3.00 g) is ignored as drift.
4. **Classify** — exhaustively try every 1-to-3-coin combination, keep the best
   fit within `MATCH_TOLERANCE_GRAMS` (0.80 g).

**Why 0.80 g.** Across all 1-to-3 coin combinations the two closest possible
masses are 18.0 g (two $1) and 19.8 g (three $2) — 1.8 g apart. Staying under
half that gap guarantees a reading can never be in range of two answers at once.

**Timing.** At the board's strapped 10 samples/second, expect **~1 s per coin** —
about 400 ms for the HX711's filter to track the step, then 500 ms of stability.
10 Hz is the *lower noise* of the chip's two modes, so this costs latency, not
accuracy.

**The payment gate does not simply trust the running total.** For a known amount
owed, the valid ways to pay it differ in mass by **11.4 g** at a time
(`W(b) = 9v − 11.4b` for `v` dollars owed and `b` two-dollar coins), so checking
total mass against that short list is far more robust than assuming every coin
was tracked correctly. Running total for showing the customer progress; mass
check for deciding payment.

## 6. The RP2040 ↔ Pi link

A framed line protocol over USB CDC.

```
<PREFIX> <MS> <TYPE> <LEN> <PAYLOAD> *<CRC>\n

EVT 12345 DOOR 10 state=open *3F
CMD 12350 SCAN 0 *A1                    (no payload, so no payload field)
```

| Field | Format | Why it is there |
|---|---|---|
| PREFIX | `EVT` / `CMD` / `RSP` | Direction, and the sync token |
| MS | decimal | Ordering, and reset detection when it jumps backwards |
| TYPE | uppercase token | Message type, so everything shares one link |
| LEN | decimal | Byte count of PAYLOAD; a mismatch rejects the frame before anything parses it |
| PAYLOAD | `key=value` pairs | Omitted entirely when LEN is 0 |
| CRC | 2 hex digits | CRC-8/ATM over every byte up to but not including the space before `*` |

**Why text rather than a packed binary struct.** The brief asked for type,
length and checksum; being able to watch the link in a plain serial monitor, or
grep a captured log, is worth a great deal while two machines are being brought
up against each other. At these data rates compactness is worth nothing.

**Why the prefix doubles as a sync token.** Debug logging and protocol frames
share one USB CDC stream, so the parser must cope with lines it did not send.
Any line not beginning with a known prefix is discarded in silence — that is how
`[12.345 Information]: ...` passes through harmlessly. A log line landing in the
*middle* of a frame corrupts it, and that is what the CRC is for: rejected and
counted, never acted on.

**There are two implementations of this codec** — `frame.{h,cpp}` in C++ and
`tools/protocol.py` in Python — because the two ends are different languages.
Two is one more than ideal and they would drift, so `python tools/protocol.py`
reads the limits straight out of the C++ header and fails if they no longer
agree. That check exists because the drift already happened once in miniature:
`MAX_TYPE_LEN` was one byte too small for `SQUARE_CANCELLED`, which would have
made every cancellation silently never leave the board.

**Heartbeats are not cosmetic.** The board declares the link dead after
`LINK_TIMEOUT_MS` (30 s) without a valid frame and takes itself out of service,
and the Pi has nothing to say while the fridge is idle — so without `EVT HB`
every 10 s a perfectly healthy link looks dead after half a minute.

## 7. Pi4 software

### 7.1 `fridged/camera.py` — what answers `CMD SCAN`

Two backends behind one interface: `--camera sim` is a modelled shelf,
`--camera picapture` runs the real vision.

**picapture is a child process of `fridged`, not a service of its own.** That
keeps the serial link owned by exactly one process, so there is never a window
where the board asks and two things try to answer, and none where the camera is
up but the link is not. A second systemd unit would have to be ordered against
the first and could still drift out of step.

**Newest wins; there is no queue.** `payments.py` queues because every card
request must be processed. Here only the most recent count means anything, and a
backlog would be actively harmful: after a stall it would hand the board a
reading of the shelf as it was seconds ago, presented as current.

#### The two scans are answered differently

This is the part with the real design content in it.

**Baseline, at door-open.** The door is swinging, light is flooding in, and a
hand may already be reaching. This is the *worst* frame of the whole cycle. So
the answer comes from the last stable reading taken **while the door was still
shut** — which is what a baseline is supposed to describe anyway.

This is possible because the board sends `EVT DOOR state=open` *before*
`CMD SCAN`, so `fridged` always sees the door move first and can freeze the
pre-open value before the scan arrives.

**Recount, at door-close.** The scene is good but not yet settled: the door has
just moved, the light is changing back, and cans may still be rocking. Wait for
`SETTLE_FRAMES` (3) consecutive agreeing packets, then answer.

**A baseline is *supposed* to be old.** The age penalty is measured up to the
moment the baseline was *frozen*, not up to the present. Measured to the present,
a perfectly healthy camera and a customer deliberating for fifteen seconds drove
the baseline's confidence to zero — but nothing could have changed the shelf
between that reading and the door opening, so the time since is not evidence
against it. What the penalty still catches is a baseline that was already stale
*when it was frozen*.

#### When it cannot answer, it does not

| Age of the newest count | What happens |
|---|---|
| under `CAMERA_STALE_S` (1 s) | Answered at full confidence |
| 1 s to `CAMERA_DEAD_S` (5 s) | Answered, confidence ramped down with age |
| over 5 s, or never | **Not answered at all** |

Two different faults, two different responses:

- **Wobbling** — picapture is running and answering, but the frames have not
  settled by the deadline. Answer anyway, from the most recent reading, with a
  low confidence that is logged and graphed. The sale proceeds. This is the
  common case and it must not stop the fridge.
- **Dead** — the subprocess is not running, or has produced nothing for long
  enough that "most recent reading" is meaningless. Do not answer. The board
  faults after `RECOUNT_TIMEOUT_MS` and goes out of service, which is honest: a
  fridge whose camera is gone cannot know what it is selling.

`CAMERA_DEAD_S` is checked **before** settling, not after. There is nothing to
wait for if the reading everything would be built on is already dead.

Three timers, nested deliberately so a fault is our verdict rather than a
timeout nobody decided on:

```
door shuts ──┬─ SETTLE_TIMEOUT_S (4 s) ── camera gives up, answer marked down
             └─ SCAN_ANSWER_BUDGET_S (6 s) ── service forces a reply
                                     └─ RECOUNT_TIMEOUT_MS (8 s) ── board faults
```

A test asserts that ordering, so shortening the board's budget without revisiting
these fails loudly.

**Parsing is strict, and checks the key set as a whole.** A picapture built from
a different brand list is refused outright rather than filtered down to the
drinks that happen to match — partial acceptance puts counts on the wrong drinks
and both ends go on agreeing about the wrong numbers with nothing reporting a
fault.

### 7.2 `picapture` — can recognition

Classical colour-blob vision, no machine learning. Per frame:

1. Capture through a GStreamer pipeline — `libcamerasrc` at 800×600, downscaled
   to 400×300, rotated 180°. Captured high and processed low because the sensor
   gives a better image downsampled than asked for a small one directly.
2. **Flat-field correct**: estimate the illumination as a heavily blurred
   greyscale copy and divide it out, so the lit and shadowed sides of one can
   are still the same colour.
3. Convert BGR → HSV.
4. **Classify every pixel to its nearest brand**, once, for all four drinks.
5. Per brand: mask, morphological open (speckle) then close (glare bands),
   `findContours`, filter by area, count.
6. Print one line: `coke:5,fanta:4,mtndew:5,solo:3;conf=87;`

**Why nearest-centre and not `inRange` per brand.** The original ran an
independent threshold per drink against the full frame. Coke, Fanta, Mountain
Dew and Solo occupy adjacent hue bands, so glare, JPEG blocking and anti-aliased
edges routinely satisfy two or three at once and a single can picks up several
labels simultaneously. Assigning each pixel to whichever brand centre it is
nearest gives every pixel exactly one owner. Hue is weighted above saturation and
value because it is the only one of the three that does not move with lighting
across a single can.

**Closing is deliberately asymmetric** — the kernel is seven times taller than
it is wide. A can is a tall thin blob and a band of glare across its middle
splits it in two; closing vertically rejoins it without bridging two cans
standing side by side.

#### `conf=` is how a wrong count becomes visible

Every other fault in this system announces itself — a corrupt frame fails its
CRC, a missing payment has no row. A miscounted shelf produces a perfectly
well-formed packet with the wrong numbers in it, and nothing anywhere reports a
fault. The only evidence available is how equivocal the measurement was, so it
is measured and carried. Four deductions from 100:

- **Uncalibrated drinks** — a drink with no `can_area` counts one can per blob,
  so it cannot notice two touching cans. It reports lower confidence rather than
  pretending.
- **Blobs off a whole can** — the most direct evidence available. A blob at
  1.02× or 1.98× the size of one can says plainly what it is; one at 1.5× is a
  coin toss whose outcome decides whether somebody is charged.
- **Discarded blobs** — something the camera saw and could not explain. Only
  ones at least half of `contour_min_area` count, or the deduction would sit at
  its cap permanently and carry no information.
- **Exposure** — outside a sensible brightness band, hue is guessed rather than
  measured. Ramped, not a cliff.

Deductions from 100 rather than factors multiplied together, because the number
has to be explainable — `--debug-all`'s `a` key prints the arithmetic, and it is
the same arithmetic that produced the figure on the wire. An empty shelf scores
100: correctly seeing nothing is a good frame.

**What the hardware taught that the tests could not: a hand held across the
shelf scores 76 and reports cans taken.** Confidence measures how sure the vision
is of its own decisions, not whether they are right — a still hand is three
identical frames, well exposed, with colours near a brand centre.

**Tuning.** Every threshold lives in `src/vision_config.h` and loads from
`picapture.conf` if present, so tuning survives a rebuild and never needs a
compiler. `--debug-all` gives live sliders and click-to-sample. A malformed or
unknown key is refused at startup rather than ignored — running with tuning
nobody chose looks identical to running with tuning that was applied.

`vision_config` has no OpenCV dependency, deliberately, so its tests build and
run on any laptop.

**Current limitation: a contour is not a can.** Two cans of the same drink
touching each other merge into one blob. Since the firmware charges for the
*difference* between two counts, a merge appearing or disappearing between the
baseline and the recount invents or hides a purchase. Separating the cans is the
current mitigation; dividing blob area by a calibrated single-can area is the
fix, and is what `can_area` above exists for.

### 7.3 The database

SQLite, WAL mode, one writer. Having the firmware bridge, the vision program and
the payment script all write directly is how you get `database is locked` during
a demo, plus three copies of the schema drifting apart in three languages.
Everything that writes goes through `store.py`; Grafana connects read-only.

Notable choices:

- **`txn` rather than `transaction`**, which is a reserved word in SQL. Every
  panel query would have to quote it forever, and a forgotten quote is a syntax
  error at display time rather than at write time.
- **One generic `measurement` table** for every scalar, so adding a metric is
  never a schema change.
- **A `boot` table**, because a transaction id is `ms_since_boot` and is
  therefore unique only *within* a boot. Two reboots in a day can mint the same
  id, and without this the second would overwrite the first.
- **Temperatures are stored under the sensor's ROM code**, never its zone name,
  with the name attached at read time by the `temperature` view. That is the
  difference between naming a sensor relabelling its whole history and naming a
  sensor putting a discontinuity in the graph at the moment you named it.
- **`raw_line` keeps everything that came down the wire**, including lines that
  were not frames. A rising bad-frame count next to the offending bytes is a
  five-minute debug; the counter on its own is a two-hour one.

#### Telling real data from invented data

Three kinds of row can reach the same database, and `boot.reason` is what
separates them:

| `boot.reason` | Means |
|---|---|
| `boot_frame`, `resumed`, `ms_rollback` | A real RP2040 |
| `sim:boot_frame`, `sim:resumed`, `sim:ms_rollback` | A `--port sim` run against `fake_board` |
| `seed` | Invented history from `seed.py` |

**This matters because `--port sim` is the default and `--db` defaults to the
file the dashboard reads.** `python -m fridged` with no arguments writes invented
sales into the real revenue panels under exactly the same table and metric names
as genuine ones. That is intended — it is how the panels were designed before
there was a fridge — but a demonstration and a week of real takings must not
become one indistinguishable pile. Everything else in the schema is keyed to a
boot, so this one column answers "was this real?" for every transaction and coin
event too.

`seed.py` additionally writes a `raw_line` marker with `source='seed'`, so
`python -m fridged.seed --clear` knows exactly what it may delete.

### 7.4 Reopening the serial port

`serial.Serial` was originally opened once, at startup, and never again. Two
consequences, the second serious:

1. At boot the port may not exist yet — the Pi and the RP2040 power up together,
   and systemd would restart every 5 s until it appeared.
2. **A device that goes away never came back.** `link.poll()` caught the
   `OSError` and returned "no data", so the loop kept running with a dead file
   descriptor: service alive, reporting itself healthy, permanently deaf.

The second had already been hit; the workaround during testing was *stop the
service, reset the board, start the service* — three manual steps at a keyboard,
which is not available mid-demonstration. Pressing RESET on the board or nudging
the cable is enough to trigger it.

`ReconnectingSerial` reopens with backoff from 1 s to 10 s and never raises on
open. The 10 s ceiling is chosen against the board's 30 s `LINK_TIMEOUT_MS`, so
recovery lands inside the window where the board has not yet declared the Pi
dead. `Link` discards its buffer across a reconnect — without that, the front
half of a frame cut off mid-flight gets glued to the first bytes after recovery,
and a clean reconnect reports a corrupt frame that never existed on the wire.

**The cost, taken deliberately:** a mistyped port path now retries forever
instead of failing loudly. Mitigated by logging the first failure at ERROR *with
the ports that do exist*, so a typo is obvious in `journalctl`.

### 7.5 Card payments

`payments.py` runs Square on a worker thread with a queue, because every card
request must be processed and an HTTP call must never block the service loop.

**Detecting payment.** It checks the `tenders` array rather than the order
`state`, because a Square order can report a state that does not guarantee money
arrived; a non-empty `tenders` array means a payment is actually attached. This
was learned the hard way and is worth preserving in any rewrite.

`--square fake` needs no credentials or network and is the default. It is
independent of `--port`, so a real payment against a simulated board is
available.

## 8. Testing

### 8.1 Off-hardware — any machine, no fridge, no board

Run after every `git pull`. If any of these fails, stop.

```bash
cmake -S rp2040-software/tests/host -B build-host
cmake --build build-host
./build-host/firmware_tests                      # 367 checks, 0 failed

cmake -S pi4-software/picapture -B build-picapture
cmake --build build-picapture
./build-picapture/picapture_tests                # 102 checks, 0 failed

cd pi4-software
python3 tools/protocol.py                        # ALL CHECKS PASSED
python3 tests/test_fridged.py                    # ALL CHECKS PASSED
```

What is worth testing here is not "does SQLite insert a row". It is the things
where being wrong produces no symptom at the time and a hole in the data later:
`close()` flushing queued rows, boot tracking, the ms-rollback path, and
unhandled message types being counted rather than dropped.

`tests/host/` deliberately does **not** use Pico SDK stubs. That approach needs
a fake for every SDK function the code touches and breaks whenever the firmware
grows a new dependency; testing only the hardware-free modules needs no fakes at
all. (An older `tests/mocks/` tree did exactly that and has been deleted along
with the build target that consumed it.)

### 8.2 On hardware — what has and has not been proven

| | Status |
|---|---|
| Firmware host tests, picapture config tests, `protocol.py`, `test_fridged.py` | Passing, off hardware |
| `checkout.cpp` re-entrant basket, `RefundOwed` | **PASSED on the board** |
| Drink rename end to end (Coke/Fanta/Mountain Dew/Solo) | **PASSED on the board** |
| picapture patch sampling + colour-centre storage | **PASSED on the Pi** |
| Area-based can counting after the colour refactor | **PASSED** — 2 touching Cokes held at `coke:2` for 567 frames |
| Per-frame confidence (`conf=`) | **PASSED on the Pi** — 87–90 still, 50–83 with a hand in frame |
| Baseline latch and recount settling | **PASSED on the fridge** — 15/15 door cycles, recount settling in 550–650 ms, no timeout ever fired |
| Confidence + trigger stored, dashboard panels | **PASSED on the fridge** — 40 scans paired one-for-one with 40 confidence readings |
| `fridged` running picapture as a subprocess | Tested against a stand-in, **never run against the real binary** |
| The service under systemd with real board + camera | **Never run.** Every hardware test so far was a foreground process with a terminal attached |
| Surviving a power cut | **Never tried** |
| Reopening the port after a board reset | Covered by host tests; **never seen against a real USB device** |

Record which commit you tested, so results can be matched to code:
`git rev-parse --short HEAD`.

### 8.3 The acceptance tests that decide whether it can take money

Not code. The tests that decide whether this is fit for the fridge.

1. **Stock a real shelf.** Counts match reality for five minutes untouched.
2. **Buy one can.** Charged for exactly that can, at $2.00.
3. **Put it back.** Returns to Idle, nobody charged.
4. **Swap it.** The new drink and the right price.
5. **Take three.** $6.00.
6. **Touching cans.** Two of one drink pushed together still count as two.
7. **Restock.** Refill mid-session; nobody charged.
8. **Twenty cycles.** Count the disagreements.
9. **Pull the camera cable mid-transaction.** The board should fault out of
   service, per §7.1.

**Test 8 is the real gate.** A system that is right nineteen times in twenty is
wrong about one sale a day, and each wrong sale is somebody's $2.

### 8.4 If the vision does not work well enough

Worth deciding early rather than at the end. `fridged` keeps a camera
*interface*, not a camera, so the options are:

1. **Change the physical problem, not the software.** Separate the cans, add a
   diffuse light, move the camera square-on. Most of the difficulty so far has
   been lighting and geometry rather than algorithm.
2. **Reduce what is asked of it.** Detecting *that the shelf changed* is much
   easier than detecting *by how much*; a flat price per door cycle is worse but
   workable.
3. **Fall back to `--camera sim`** for a demo, with the limitation stated
   plainly rather than hidden.

## 9. Known issues

| # | Issue | Location | Impact |
|---|---|---|---|
| 1 | `hx711_reader.pio.h` **defines** its init functions and has no `extern "C"` guard | submodule | Including it from a second C++ file causes duplicate symbols. `mass_sensor.cpp` is the only includer and must stay that way; the failure would be at link time, naming neither file. |
| 2 | Two cans of the same drink touching each other merge into one blob | `picapture` | Miscounts a purchase. `can_area` division is the fix; separating the cans is the mitigation. See §7.2. |
| 3 | A hand held still across the shelf scores 76 and reports cans taken | `picapture` | Confidence measures certainty, not correctness. Recorded next to the penalty constants in `vision_config.h`. |
| 4 | `tokens.py` is gitignored, so card payments need manual setup on a fresh clone | `online-payment/` | Documented in README.md §2 and §4 here |
| 5 | **Square sandbox credentials were committed and pushed to a public repo** inside a `.pyc` | history, commit `c8cd1b6` | Untracked and now ignored, **but still present in history**. Sandbox only, so play money — **rotate the token.** See §4.2 |
| 6 | `fridged` runs `--square real` in production | `deploy/fridged.default` | Worth confirming that is intended before a demonstration |

Resolved during the pre-merge audit, recorded because earlier notes referenced
them: the dangling `opt.buffer` in the old `load_cell` driver, the `$20` default
price and the header-defined function in the old `camera_side/inventory`, and
display pins living in `ili9341.h`. The first three were deleted with their
modules; the fourth was moved into `board.h`.

## 10. Decisions, settled

- **Cash payment timeout resets on each accepted coin**, so a slow customer is
  never cut off mid-payment.
- **Exact cash payment only** — no change is given. There is no hopper, only a
  one-way box. A customer who overpays and puts everything back reaches
  `RefundOwed`, which is a person's job to settle.
- **Theft is logged, not prevented.** Someone taking a drink and walking away is
  recorded as a stolen drink and the system returns to idle.
- **No persistence across power cycles.** All tallies are RAM-only; a reboot
  clears them. EEPROM is a future-board consideration.
- **No camera frames are stored.** Counts and confidence only — no `latest.jpg`,
  no ring buffer, nothing written to disk but numbers. The image exists inside
  picapture for the length of one frame.

  Consequences, stated plainly: **a disputed charge cannot be audited against a
  picture.** The confidence figure and the two stock snapshots are the whole
  record, which is exactly why §7.2's confidence graph matters. It also removed
  the HTTP server an on-dashboard frame would have needed, and with it the
  question of who on the network could watch a camera pointed at a shared space
  — a privacy decision rather than a convenience one, if it is ever revisited.
