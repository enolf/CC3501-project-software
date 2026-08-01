# CC3501 Design Project — Software Documentation

Closed-loop vending system for the Cairns Engineering Society fridge. Product
selection is recognised visually, payment is taken by cash or card, and stock
and faults are tracked for a dashboard.

**Authors:** Alister Maltby, Damien Turner, Nils Eisen
**Repository:** `github.com/enolf/CC3501-project-software`
**This document reflects the state of the code as of 29 July 2026.** It is a
living document — update it as subsystems land.

---

## 1. System overview

Two computers share the work:

| | RP2040 (custom PCB) | Raspberry Pi 4 |
|---|---|---|
| **Role** | Sensing, user interface, payment | Vision, internet |
| **Language** | C++17 / C11, Pico SDK 2.2.0 | C++20 (OpenCV), Python 3 |
| **Handles** | TFT + touch, RFID, temperature, load cell / coins | Camera, can recognition, Square API |

### Intended transaction flow

```
IDLE
  -> Pi4 recognises cans removed from the fridge
  -> RP2040 shows the total owed and the payment choice on the TFT
  -> Customer picks cash or card on the touchscreen
       |
       +-- CARD: Pi4 asks Square for a payment link, QR shown on the TFT,
       |         Pi4 polls until the order is paid
       |
       +-- CASH: customer drops $1/$2 coins into the money box; the load
                 cell works out what was added; TFT shows a running total
  -> THANK YOU screen
  -> IDLE

  (timeout on either path -> log the drink as stolen -> IDLE)
```

### What is actually implemented

**Individually working subsystems, none of them yet connected to each other.**
Each is currently exercised by its own standalone test program. See
[§6 Integration status](#6-integration-status) for the gap list — it is the
most important section in this document.

---

## 2. Repository layout

```
CC3501-project-software/
├── documentation.md            <- this file
├── README.md                   project blurb
├── .gitmodules                 lvgl + pico-scale submodule definitions
│
├── rp2040-software/
│   ├── CMakeLists.txt          firmware build (+ a legacy native branch)
│   ├── CLAUDE.md               coding standards for team-written code
│   ├── src/
│   │   ├── board.h             ALL board wiring & calibration
│   │   ├── main.cpp            entry point — a set of switchable test blocks
│   │   ├── drivers/            hardware adapters (one per device)
│   │   └── peripherals/        behaviour / application logic
│   ├── lib/
│   │   ├── lv_conf.h           LVGL configuration
│   │   ├── lvgl/               submodule, v9.2
│   │   └── pico-scale/         submodule (has its OWN nested submodule)
│   └── tests/mocks/            LEGACY — see §4.7
│
└── pi4-software/
    ├── picapture/              OpenCV can recognition
    │   ├── CMakeLists.txt
    │   ├── src/main.cpp        the real program
    │   ├── src/main2.cpp       stale copy, NOT in the build
    │   ├── src/trackbar.h      tuning slider defaults
    │   └── cans_test/          sample images for offline testing
    └── online-payment/
        └── square.py           Square payment link + QR + polling
```

---

## 3. Building and running

### 3.1 Cloning — read this first

The repository uses submodules, and `pico-scale` has a **nested** submodule of
its own. A plain `git clone` leaves you with empty directories and a build that
fails on missing `hx711.h`.

```bash
git clone --recurse-submodules https://github.com/enolf/CC3501-project-software.git
```

Already cloned without them:

```bash
git submodule update --init --recursive
```

To make future pulls keep submodules in step (run once per machine — git will
not let the repository configure this for you):

```bash
git config --global submodule.recurse true
```

Verify with `git submodule status --recursive`; you should see two lines, one
for `lvgl` and one for `pico-scale`, plus the nested `hx711-pico-c`.

### 3.2 RP2040 firmware

**Via VS Code (normal route):** install the *Raspberry Pi Pico* extension. It
supplies the SDK, the ARM toolchain, CMake and Ninja, and adds the build/flash
buttons to the status bar. `CMakeLists.txt` auto-detects the cross-compiler.

**From the command line (verified working):**

```bash
cd rp2040-software
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1
cmake -S . -B build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=~/.pico-sdk/ninja/v1.12.1/ninja.exe \
  -DPICO_BOARD=pico \
  -DCMAKE_C_COMPILER=$PICO_TOOLCHAIN_PATH/bin/arm-none-eabi-gcc.exe \
  -DCMAKE_CXX_COMPILER=$PICO_TOOLCHAIN_PATH/bin/arm-none-eabi-g++.exe
ninja -C build
```

Output: `build/labs.uf2`. Flash by holding BOOTSEL while plugging in the board
and copying the file across, or use the extension's Run button.

Current footprint: **108 KB flash, 4.9 KB RAM (bss)** — comfortable on the
RP2040's 2 MB / 264 KB.

**Serial output is on USB CDC** (`pico_enable_stdio_usb`), so the same cable
used to flash the board carries the debug console. To move it back to the UART
header, swap the two `pico_enable_stdio_*` lines in `CMakeLists.txt`.

### 3.3 Pi4 — camera

On the Raspberry Pi:

```bash
sudo apt install libopencv-dev gstreamer1.0-tools \
                 gstreamer1.0-plugins-base-apps gstreamer1.0-libcamera
cd pi4-software/picapture && mkdir build && cd build
cmake .. && make && ./PiCapture
```

Requires **OpenCV 4.x** — OpenCV 5.0.0 lacks the `moments` class this code
uses. Needs a real display; VNC or X-forwarding is too slow for the preview
windows. A direct HDMI output is recommended.

### 3.4 Pi4 — payments

```bash
pip install requests qrcode[pil]
cd pi4-software/online-payment && python square.py
```

Needs a `tokens.py` alongside `square.py`, which is **deliberately gitignored**
and therefore absent from a fresh clone. Create it with:

```python
SQUARE_ACCESS_TOKEN = "<your sandbox token>"
LOCATION_ID         = "<your sandbox location id>"
```

Currently points at `connect.squareupsandbox.com` — sandbox, not live money.

---

## 4. RP2040 software

### 4.1 Conventions

Set out in `rp2040-software/CLAUDE.md`. The rules apply to team-written code
only; the vendored submodules (`lvgl`, `pico-scale`, the Pico SDK) are exempt.
The three that shape the layout:

- **All board wiring lives in `board.h`.** Pin numbers, bus addresses,
  calibration constants. A board revision should be a header edit, nothing more.
- **`drivers/` know hardware; `peripherals/` know behaviour.** Application code
  calls driver functions and never touches a register.
- **`main` stays thin** — top-level sequencing only.

Two places currently break the first rule; see §7.

### 4.2 `board.h`

Single source of truth for wiring. Covers:

| Group | Contents |
|---|---|
| DS18B20 | Bus pin (GP7), parasite-power mode, strong-pull-up FET pin (GP8) and polarity |
| RFID | I2C instance, SDA/SCL (GP4/GP5), 100 kHz, module address `0x2C` |
| TFT | Orientation selector that cascades resolution, MADCTL value and touch axis flags; raw touch calibration bounds; calibration mode flag |
| HX711 | Data/clock (GP10/GP11), sample rate strap, counts-per-gram calibration |

The orientation block is a nice pattern worth keeping: set
`ACTIVE_DISPLAY_ORIENTATION` and every dependent constant follows from it.

### 4.3 Drivers

| Driver | Lines | Device | Status |
|---|---:|---|---|
| `DS18B20/` | 684 | 1-Wire temperature sensors | Complete |
| `mfrc522/` | 420 | PiicoDev RFID reader (I2C) | Complete |
| `ili9341/` | 242 | TFT + XPT2046 touch (SPI) | Complete |
| `mass_sensor/` | 250 | HX711 load cell amplifier | Complete |
| `logging/` | 60 | Severity-filtered serial log | Complete |
| `WS2812/` | 45 | Addressable LED PIO program | Unused, not compiled |

**`DS18B20`** is the most thorough piece of code in the repository. Full 1-Wire
ROM search so multiple sensors share one pin, CRC-8 verification on both ROM
codes and scratchpad reads, and both power modes. The parasite-power path
interlocks the data pin against the strong-pull-up FET so the two can never
fight — without that, driving the data line low while the FET is on shorts 3V3
to ground through the RP2040 pin. Supports blocking and non-blocking conversion.

**`mfrc522`** implements the ISO 14443A stack: REQA, anticollision across
cascade levels, and both 4-byte and 7-byte UIDs. Verifies `VersionReg` at init
before configuring anything. Registers and bit masks are all named with
datasheet references.

**`ili9341` / `xpt2046`** share one SPI bus at two different clock speeds
(30 MHz display, 2 MHz touch), switching baud rate around each touch read. The
touch controller needs a dummy transaction at init to clear the RP2040's RX
FIFO, otherwise it locks up — a hardware quirk that cost real debugging time and
is commented in place.

**`mass_sensor`** wraps the HX711 for coin detection. Two deliberate choices:
reads are **non-blocking** (`poll()` returns false until a conversion is ready,
so the caller is never stalled for 100 ms), and readings are **relative to a
baseline** captured by `tare()` rather than absolute. The latter means the money
box's own weight and any coins already in it drop out of the arithmetic, and the
stored zero offset never has to be correct. `init()` proves the chip is present
by demanding an actual conversion within a timeout — a missing HX711 never pulls
DOUT low, so no data is ever clocked out — and rejects a saturated first reading,
which indicates an open load cell bridge.

**`logging`** prints `[seconds.millis Level]: message` with a global threshold.
Takes a plain string, so formatted output needs `snprintf` into a buffer first.

### 4.4 Peripherals

| Peripheral | Lines | Purpose | Status |
|---|---:|---|---|
| `coin_acceptor/` | 283 | Identify $1/$2 coins by mass | Complete, tested |
| `sensor_health/` | 225 | Track sensors going missing / failing | Complete |
| `access_control/` | 72 | UID → name whitelist | Works; list is hardcoded |
| `tft_display/` | 232 | LVGL port + touch input | Port done, UI is a demo |
| `load_cell/` | 316 | Original HX711 example | Superseded, see §7 |
| `camera_side/` | 95 | Inventory + prices | Skeleton only |

**`sensor_health`** tracks sensors by ROM code rather than by count, so it can
report *which* sensor stopped answering rather than just that the number
changed. Tolerates a few consecutive read failures before declaring a fault,
and rate-limits reminder logs. Takes arrays of ROM codes rather than a driver
object, so it can be exercised with synthetic data off hardware.

**`tft_display`** is a working LVGL 9.2 port: partial-render framebuffer, flush
callback into the ILI9341 window, a 5 ms repeating timer for LVGL's tick, and a
touch input device with rotation/inversion handled from `board.h`. What is
missing is any actual UI — the public API is `init()`, `write_text()`, `run()`
and `create_dual_switches()`, the last being a two-toggle demo that only prints.

#### Planned UI — payment terminal only

**Scope is deliberately narrow: the TFT is the payment terminal and nothing
else.** It displays no temperatures, no stock, no diagnostics — all of that
belongs on the dashboard (see [dashboard.md](dashboard.md)). Five screens:

| Screen | Shows | Leaves when |
|---|---|---|
| **Idle** | Black | Items are removed and a transaction starts |
| **Payment select** | Total owed, plus two touch targets: Cash / Card | One is tapped |
| **Cash** | Paid vs owed, updating live as coins land (e.g. `$2.00 / $4.00`) | Paid reaches owed, or timeout |
| **Card** | QR code linking to the Square payment page | Square reports the order paid, or timeout |
| **Thank you** | Confirmation message | Short delay, then back to Idle |

Notes for whoever builds this:

- **There is no separate basket screen.** An earlier description of the flow
  mentioned "showing purchases", but the confirmed scope is five screens, so the
  total owed is shown on the payment-select screen instead. If an itemised list
  ("2 × Coke, 1 × Fanta") is wanted, that screen is where it goes — worth
  confirming before building.
- The cash figure comes from `CoinAcceptor::cents_total()`; redraw on coin
  events only, not every loop.
- The QR needs LVGL's `lv_qrcode` widget — present in the vendored LVGL, not
  currently used. The URL arrives from the Pi (dashboard.md §6).
- Idle being **black** is deliberate: no burn-in, low power, and an unambiguous
  "nothing in progress" state.
- On timeout, both payment paths log a stolen drink and return to Idle.
- Keep LVGL detail inside this module; expose semantic calls such as
  `show_cash_progress(paid_cents, owed_cents)` and `show_qr(url)` so the
  checkout state machine never touches a widget.

**`camera_side/inventory`** holds per-can price and count with a `Can` enum
(Coke, Sprite, Fanta, Pasito) and a `sync_dashboard()` hook for cloud price
overrides. The hook is a stub (see §7), so it is not yet usable.

### 4.5 Coin payment subsystem

The most recently completed subsystem, and the one with the least obvious
design, so it is documented in full.

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
+9.0 g or +6.6 g — **2.4 g apart**, a wide margin against sub-gram noise, and one
that stays constant no matter how full the box is.

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
For a single coin the nearest rival is a full 2.4 g away.

**Multi-coin events.** Coins landing within one settle window merge into a single
step. Handled: two-coin totals (13.2 / 15.6 / 18.0 g) are still 2.4 g apart.

**Timing.** At the board's strapped 10 samples/second, expect **~1 s per coin** —
about 400 ms for the HX711's filter to track the step (datasheet figure at
10 Hz), then 500 ms of stability. 10 Hz is the *lower noise* of the chip's two
modes, so this costs latency, not accuracy.

**Verification.** 17 synthetic tests pass, covering every 1-to-3 coin
combination, a worn 6.45 g $2 still reading correctly, a 4 g foreign object
rejected and not counted, 1.2 g of drift producing no event, and a ringing beam
refusing to settle then reporting correctly once it stops. `CoinAcceptor` takes
grams and returns coin counts with no hardware dependency, which is what makes
this testable off-board.

**Note for the checkout logic, when it exists.** The eventual "has the customer
paid?" decision should **not** simply trust `cents_total()`. For a known amount
owed, the valid ways to pay it differ in mass by **11.4 g** at a time
(`W(b) = 9v − 11.4b` for `v` dollars owed and `b` two-dollar coins), so checking
total mass against that short list is far more robust than assuming every coin
was tracked correctly. Running total for showing the customer progress; mass
check for deciding payment.

### 4.6 `main.cpp` — the test-block convention

`main.cpp` holds several complete `main()` functions, **all commented out except
one**. Each is a self-contained bring-up program for one subsystem. To switch,
comment the active block and uncomment another.

| Block | Exercises | State |
|---|---|---|
| TFT display | Text on screen | Commented |
| DS18B20 | Multi-sensor temperature + health | Commented |
| RFID | UID read + access decision | Commented |
| HX711 (original) | `Load_cell::measure()` | Commented |
| **Coin detector** | **$1/$2 discrimination** | **ACTIVE** |

This is a bring-up convention, not the final architecture — the real system
needs one `main()` running a state machine (§6).

**Coin detector usage.** Flash, open the serial monitor over USB. It banners,
waits 2 s for the terminal to reattach, initialises the scale, tares, then
waits. Type into the monitor while it runs:

| Key | Action |
|---|---|
| `d` | Toggle raw sample dump |
| `t` | Re-tare (zero where it currently sits) |
| `r` | Reset the coin tally |

**Do the `d` check first**, with nothing touching the box. The spread you see is
the noise floor and must be well under 0.50 g or the reading will never settle.
If it is not, the cause is almost always mechanical — the beam clamped at both
ends instead of free to flex, or the box resting against something.

**Calibration check:** three $1 coins should read 27.0 g. A consistent
proportional error means `LOADCELL_COUNTS_PER_GRAM` in `board.h` needs redoing;
the 2945 figure came from an earlier bare-Pico test rig.

### 4.7 `tests/mocks/` — legacy

Stub implementations of Pico SDK functions (`gpio`, `pio`, `sync`, `timer`,
`stdlib`, `time`) that once let parts of the firmware build natively for
off-hardware testing. **They are dev-board leftovers and are not part of this
project's build.** The native branch of `CMakeLists.txt` that consumes them is
broken (§7). The ARM/firmware target is the only build that matters.

---

## 5. Pi4 software

### 5.1 `picapture` — can recognition

Classical colour-blob vision, no machine learning. Per frame:

1. Capture through a GStreamer pipeline — `libcamerasrc` at 800×600, downscaled
   to 400×300, rotated 180°.
2. Convert BGR → HSV.
3. `inRange` threshold against six trackbar-controlled bounds.
4. Morphological open (remove speckle) then close (fill holes), both optional
   and sized by trackbar.
5. `findContours`, filtered by area between min and max trackbars.
6. Per contour: area, centroid from image moments, bounding box.
7. Draw overlays, show three windows (raw, thresholded, morphology).
8. Every 30 frames, print FPS and can count.

**Tuning aids.** All parameters are live trackbars with defaults in
`trackbar.h`. Clicking a pixel in the camera window prints its BGR and HSV and
**auto-sets the six threshold sliders** around that colour (±10 hue,
±20 saturation/value) — the fastest way to lock onto a new can.

Defining `TEST` swaps the camera for a still image from `cans_test/`, so the
pipeline can be developed without a Pi.

**Current limitation.** Only one colour is detected. Lines 275-281 call
`visualise_contours` four times with identical arguments, each overwriting the
previous result; the `//red //blue //purple` comments show the intent, but there
is one shared HSV mask, so `can_color_names` is `{"COKE"}` to match. Per-colour
masking is the work remaining.

### 5.2 `online-payment/square.py` — card payments

Three functions against the Square sandbox:

- `create_payment_link()` — POSTs a `quick_pay` order, returns the checkout URL
  and order ID. Uses a timestamp as the idempotency key.
- `display_qr_code(url)` — renders a QR and opens it with `img.show()`.
- `poll_for_payment(order_id)` — GETs the order every 3 s for up to 120 s.

**Detecting payment.** It checks the `tenders` array rather than the order
`state`, because a Square order can report a state that does not guarantee money
arrived; a non-empty `tenders` array means a payment is actually attached. The
inline comment calls this "the fix", so it was learned the hard way. Worth
preserving in any rewrite.

On timeout it prints that the drink is flagged as stolen — the theft-logging
behaviour the RP2040 side will eventually own.

---

## 6. Integration status

**This is the critical section.** Every subsystem works alone; almost none are
connected.

| Link | Status |
|---|---|
| Pi4 camera → RP2040 | **Missing entirely** |
| RP2040 → Square payments | **Missing entirely** |
| QR code → TFT display | **Missing entirely** |
| Coin acceptor → checkout logic | **Missing** (subsystem ready) |
| Transaction state machine | **Does not exist** |
| Theft tally | **Does not exist** |
| Cloud dashboard | **Does not exist** |

### 6.1 No RP2040 ↔ Pi4 link

No UART, no I2C, no USB protocol on either side. `picapture` builds a packet
string in `serialize_image()` and only `printf`s it every 30 frames; nothing
reads it, and the RP2040 has no receive path. **This is the single biggest gap**
— "waiting for picam → showing purchases" has no transport at all.

### 6.2 No state machine

None of the five screens in §4.4 exist in any form. The TFT can print one
centred string. `main.cpp` runs one bring-up block.

### 6.3 QR never reaches the display

The QR is a PIL image opened on the Pi's own desktop. LVGL ships `lv_qrcode`,
unused — and there is no path to get the URL to the RP2040 anyway.

### 6.4 Pricing is not wired up

`square.py` has a hardcoded `PRICE = 500`. `Inventory` has its own default.
`CoinAcceptor` reports cents. Three unconnected notions of money.

---

## 7. Known issues

Ordered by how much they will hurt.

| # | Issue | Location | Impact |
|---|---|---|---|
| 1 | `opt.buffer` is set to a **stack-local** array that dies when `init()` returns; later `scale_weight()` calls write through the dangling pointer | `load_cell.cpp:98-101` | Memory corruption if used. Not currently called — `mass_sensor` replaces it |
| 2 | Native/test-harness CMake branch references `src/tasks/sensor_health.cpp`, which does not exist (moved to `src/peripherals/`) | `CMakeLists.txt:136` | That build target is broken. Legacy, see §4.7 |
| 3 | `DEFAULT_PRICE_CENTS = 2000` is **$20**, not the intended $2 | `inventory.hpp:24` | Wrong prices once wired up |
| 4 | `fetch_price()` returns a default-constructed value, so `sync_dashboard()` resets every can to the default on every call | `inventory.cpp:24-28` | Price overrides silently do nothing |
| 5 | `simulate_dashboard_get()` is a **non-inline function defined in a header** | `inventory.hpp:13` | Multiple-definition link error the moment a second file includes it |
| 6 | Four identical `visualise_contours()` calls, each overwriting the last | `picapture/main.cpp:275-281` | Only one can colour is ever detected |
| 7 | Display pins live in `ili9341.h`; superseded HX711 pins (GP14/15) live in `load_cell.h` | both | Violates the `board.h` rule; two sets of HX711 pins now exist |
| 8 | `main2.cpp` is a stale copy of `main.cpp` with `TEST` enabled, not in the build | `picapture/src/` | Confusing; will drift further |
| 9 | Guard reads `if (a.size() != b.size() && "")` — a string literal is always truthy, so the `&& ""` does nothing | `picapture/main.cpp:75` | Looks like a failed attempt to disable the check |
| 10 | `hx711_reader.pio.h` **defines** its init functions and has no `extern "C"` guard | submodule | Including it from a second C++ file causes duplicate symbols. Already hit and worked around in `mass_sensor.cpp` — do not include it there |
| 11 | Signed/unsigned comparisons in `for` loops over `.size()` | `inventory.cpp:17`, `picapture/main.cpp:79` | Warnings only |
| 12 | `tokens.py` is gitignored, so `square.py` cannot run from a fresh clone without manual setup | `online-payment/` | Documented in §3.4 |

---

## 8. Next steps

Roughly in dependency order:

1. **Define the Pi4 ↔ RP2040 protocol.** Everything else waits on this. A
   line-based serial format over UART is the obvious first choice — the Pi
   already produces a string in `serialize_image()`.
2. **Per-colour detection in `picapture`** so more than one can type is
   reported.
3. **Transaction state machine** on the RP2040, with the coin acceptor and a
   Square path slotting into a shared `AWAIT_PAYMENT` state.
4. **TFT screens** — idle, payment select, cash running total, QR, thank you.
   Scope is fixed and narrow; see §4.4 for the full screen table.
5. **Theft tally** — per-can counts, RAM-only for now. Surviving a power cycle
   would need flash or an EEPROM on a future board revision.
6. **Fix issues 3, 4, 5** before `Inventory` becomes load-bearing.
7. **Dashboard upload** once the Pi has something worth sending.

### Decisions already made

- **Cash payment timeout resets on each accepted coin**, so a slow customer is
  never cut off mid-payment.
- **No persistence across power cycles.** All tallies are RAM-only; a reboot
  clears them. EEPROM is a future-board consideration.
- **Exact cash payment only** — no change is given.
- **Theft is logged, not prevented.** Someone taking a drink and walking away
  is recorded as a stolen drink and the system returns to idle.
