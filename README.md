# CC3501 Smart Fridge

A self-service drinks fridge that watches its own shelf. A camera counts what
was taken, the customer pays by coin or by scanning a QR code, and every sale,
temperature and fault is recorded to a live dashboard.

Built for the Cairns Engineering Society fridge as the design project for
CC3501 (Embedded Systems Design and Interfacing) by **Alister Maltby**,
**Damien Turner** and **Nils Eisen**.

The work is split across two computers: a custom RP2040 board runs the
terminal — touchscreen, RFID, coin scale, temperature sensors — and a Raspberry
Pi 4 does the vision, the card payments and the internet.

---

## Contents

- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Installation](#installation)
  - [1. Get the code](#1-get-the-code)
  - [2. Create the two secret files](#2-create-the-two-secret-files)
  - [3. Check it builds, before any hardware](#3-check-it-builds-before-any-hardware)
  - [4. Build and flash the RP2040](#4-build-and-flash-the-rp2040)
  - [5. Set up the Raspberry Pi](#5-set-up-the-raspberry-pi)
  - [6. Install the dashboard](#6-install-the-dashboard)
  - [7. Install the service](#7-install-the-service)
  - [8. Swap the simulations for real hardware](#8-swap-the-simulations-for-real-hardware)
- [Running it day to day](#running-it-day-to-day)
- [Further documentation](#further-documentation)

---

## How it works

```
   customer opens the door
            |
            v
   the Pi freezes what the shelf looked like BEFORE the door moved
            |
   customer takes drinks, shuts the door
            |
            v
   the Pi waits for the picture to hold still, then counts again
            |
   the difference is the basket, and the board charges for it
            |
            +--- CASH:   coins land on a load cell; $1 and $2 are told apart
            |            by mass, and the screen counts up to the total
            |
            +--- ONLINE: the Pi asks Square for a payment link, the board
                         shows it as a QR code and waits for confirmation
            |
            v
   thank you  ->  idle
   (nobody pays -> the drink is recorded as stolen; theft is logged, not
    prevented, because there is no lock)
```

Everything the board does is reported over USB serial to the Pi, which writes it
to SQLite for Grafana to draw.

## Repository layout

```
CC3501-project-software/
├── README.md                 this file
├── documentation.md          how it works and why, in detail
│
├── rp2040-software/          the terminal firmware (C++17, Pico SDK 2.2.0)
│   ├── src/
│   │   ├── board.h           ALL pin numbers and bus addresses
│   │   ├── main.cpp          the superloop, and nothing else
│   │   ├── drivers/          one per device: ds18b20, mfrc522, ili9341, ...
│   │   └── peripherals/      behaviour: checkout, basket, coin_acceptor, ...
│   ├── lib/                  lvgl and pico-scale (SUBMODULES) + fatfs
│   └── tests/host/           off-hardware unit tests
│
└── pi4-software/             the Pi side (Python 3, C++17)
    ├── fridged/              the service: serial in, SQLite out
    ├── picapture/            OpenCV can recognition
    ├── grafana/              dashboard JSON and provisioning
    ├── deploy/               systemd units and install scripts
    ├── tools/                the shared wire codec and the fake board / fake Pi
    └── tests/                off-hardware tests for the service
```

---

## Installation

Follow these in order. Steps 1–4 need only a laptop and the board; steps 5–8
need the Pi.

### Prerequisites

| Where | What |
|---|---|
| Laptop | VS Code with the **Raspberry Pi Pico** extension (supplies the SDK, ARM toolchain, CMake and Ninja), or those four installed by hand |
| Raspberry Pi | Raspberry Pi OS (64-bit), Grafana from `apt.grafana.com` |
| Both | Git, Python 3 |

### 1. Get the code

**The submodules are not optional.** `lvgl` and `pico-scale` are submodules, and
`pico-scale` has a nested submodule of its own. A plain `git clone` leaves those
directories empty and the firmware fails to compile on a missing `lvgl.h` or
`hx711.h`.

```bash
git clone --recurse-submodules https://github.com/enolf/CC3501-project-software.git
cd CC3501-project-software
```

Already cloned without them:

```bash
git submodule update --init --recursive
```

Check you got all three:

```bash
git submodule status --recursive
```

```
 933b2352... rp2040-software/lib/lvgl (v9.2.2-10-g933b2352b)
 0deb4dab... rp2040-software/lib/pico-scale (heads/main)
 43296dfa... rp2040-software/lib/pico-scale/extern/hx711-pico-c (v2-6-g43296df)
```

To make future pulls keep submodules in step (once per machine — a repository
cannot configure this for you):

```bash
git config --global submodule.recurse true
```

### 2. Create the two secret files

Both are gitignored on purpose, so **neither arrives with a clone** and the
build fails loudly until you write them. That is deliberate: a build that stops
is better than one that silently ships somebody else's placeholder card UIDs.

**a. The approved card list** —
`rp2040-software/src/peripherals/access_control/access_list.h`

```c
#pragma once

// Card UIDs, paired with names. GITIGNORED - see documentation.md section 3.5.
//
// uid_len is 4 for single-size UIDs (Mifare Classic) or 7 for double-size
// (NTAG, Mifare Ultralight, DESFire). Only the first uid_len bytes are
// compared, so a 4-byte entry can leave the remaining slots at zero.

static const ApprovedUser approved_users[] = {
    { "Jane Smith", 7, { 0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } },
    { "Spare Fob",  4, { 0xDE, 0xAD, 0xBE, 0xEF } },
};
```

An empty list is valid — a fridge nobody is enrolled on yet. To enrol somebody,
hold their card to the reader and copy the UID out of the serial log.

**b. The Square credentials** — `pi4-software/online-payment/tokens.py`.
Only needed for real card payments; skip it if you are staying on `--square fake`.

```python
SQUARE_ACCESS_TOKEN = "<your sandbox token>"
LOCATION_ID         = "<your sandbox location id>"
```

> **Never commit either file, and never commit anything derived from them.** A
> Square token once reached this public repository inside a `__pycache__/*.pyc`,
> which embeds the string constants of the module it compiled even though
> `tokens.py` itself was correctly ignored. `documentation.md` section 3.5 has
> the incident and the pre-commit checks that catch the whole class.

### 3. Check it builds, before any hardware

Every one of these runs on a laptop with no board, no Pi and no camera. Run
them after every `git pull`; if any fails, stop.

```bash
# Firmware logic: basket arithmetic, coin discrimination, the wire format
cmake -S rp2040-software/tests/host -B build-host
cmake --build build-host
./build-host/firmware_tests                     # expect: 367 checks, 0 failed

# picapture tuning file and confidence maths (needs no OpenCV)
cmake -S pi4-software/picapture -B build-picapture
cmake --build build-picapture
./build-picapture/picapture_tests                # expect: 102 checks, 0 failed

# The wire protocol — asserts both ends still agree
cd pi4-software && python3 tools/protocol.py     # expect: ALL CHECKS PASSED

# The Pi service, end to end against a simulated board
python3 tests/test_fridged.py                    # expect: ALL CHECKS PASSED
```

### 4. Build and flash the RP2040

**From VS Code:** open `rp2040-software/`, pick the `arm-none-eabi` kit, and use
the extension's build and run buttons. `CMakeLists.txt` auto-detects the
cross-compiler.

**From the command line:**

```bash
cd rp2040-software
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1

cmake -S . -B build -G Ninja \
  -DPICO_BOARD=pico \
  -DCMAKE_C_COMPILER=$PICO_TOOLCHAIN_PATH/bin/arm-none-eabi-gcc \
  -DCMAKE_CXX_COMPILER=$PICO_TOOLCHAIN_PATH/bin/arm-none-eabi-g++
cmake --build build
```

Flash `build/labs.uf2` by holding **BOOTSEL** while plugging the board in and
copying the file onto the `RPI-RP2` drive that appears.

The build has three switches that matter:

| Option | Default | What it does |
|---|---|---|
| `-DPI_LINK_BACKEND=sim\|serial` | `sim` | `sim` fakes the camera and Square from the keyboard, so the whole system runs with no Pi at all. `serial` is the real link. |
| `-DSIM_ALL=OFF` | on | Removes the keyboard stand-ins. **Use this for the build that goes in the fridge** — the debug keys can fake a payment. |
| `set(IDLE_LOGO ON)` in `CMakeLists.txt` | `OFF` | Shows the society logo instead of a black idle screen. |

Serial output is on **USB CDC**, so the cable you flashed with is also the debug
console:

```bash
python3 -m serial.tools.miniterm /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00 115200
```

Press `?` for the debug key list. Prefer `miniterm` over `screen`: `screen`
detaches instead of exiting and keeps holding the port.

### 5. Set up the Raspberry Pi

```bash
sudo apt update
sudo apt install python3-serial python3-requests sqlite3 \
                 libopencv-dev gstreamer1.0-tools \
                 gstreamer1.0-plugins-base-apps gstreamer1.0-libcamera
```

`apt`, not `pip` — Raspberry Pi OS is an externally managed Python environment
(PEP 668).

Then build the vision program:

```bash
cd pi4-software/picapture
cmake -S . -B build && cmake --build build
```

This needs **OpenCV 4**. OpenCV 5 removed the `Moments` class the code depends
on. If OpenCV is missing, the configure step still succeeds but builds only the
tests and says so — on the Pi, that is a fault, not a convenience.

### 6. Install the dashboard

Install Grafana from `apt.grafana.com` first, then:

```bash
cd pi4-software/grafana
./setup-pi.sh
```

Run it as your login user, **not** with sudo. It installs the SQLite datasource
plugin, creates `/var/lib/fridge`, provisions the dashboards, and enables
anonymous viewing.

**Log out and back in afterwards** — it puts you in the `grafana` group, and
group membership only takes effect on a new login.

### 7. Install the service

```bash
cd pi4-software/deploy
bash install.sh
```

Again as your login user, not with sudo. It writes `/etc/default/fridged`,
installs the systemd units, and starts `fridged` plus a nightly backup timer.
Safe to re-run — that is how you deploy a change after a `git pull`.

The dashboard is now at `http://<pi-address>:3000`.

To give it some history to draw before the fridge has any:

```bash
cd pi4-software
python3 -m fridged.seed --days 14 --stock coke=6,fanta=4,mtndew=5,solo=3
```

`--stock` anchors the invented history to what is really on the shelf, so the
first real scan agrees with it. `python3 -m fridged.seed --clear` removes
seeded data and leaves live data alone.

### 8. Swap the simulations for real hardware

`fridged` starts fully simulated. Turn on one real thing at a time, checking
each before moving to the next, by editing `/etc/default/fridged` and running
`sudo systemctl restart fridged`.

| Order | Change | Why this order |
|---|---|---|
| 1 | `--port sim` → `--port /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00` | The board is the thing everything else reports through |
| 2 | `--camera sim` → `--camera picapture` | This is the swap that fails on permissions, and under systemd that looks like a crash loop |
| 3 | `--square fake` → `--square real` | Prove the rest works before real money is involved |

Use the `by-id` path, never `/dev/ttyACM0` — the number moves when the board is
reset while the Pi is up.

---

## Running it day to day

```bash
sudo systemctl status fridged          # is it alive
journalctl -u fridged -f               # what is it doing
sudoedit /etc/default/fridged          # change its arguments
sudo systemctl restart fridged         # apply them
bash pi4-software/deploy/backup.sh     # take a backup now
```

To run the whole system on a laptop with no hardware whatsoever:

```bash
cd pi4-software
python3 -m fridged --port sim --sim-activity 30
```

Rows produced this way are tagged `boot.reason = 'sim:*'` in the database, so a
demonstration can never be mistaken for real takings later.

## Further documentation

| Document | Covers |
|---|---|
| [documentation.md](documentation.md) | The whole system: design decisions, every subsystem, the wire protocol, the hardware test procedure and the known issues |
| [pi4-software/deploy/README.md](pi4-software/deploy/README.md) | systemd units, backups, going from simulated to real |
| [pi4-software/grafana/README.md](pi4-software/grafana/README.md) | Dashboard panels, naming sensors and cards, editing and exporting |
| [pi4-software/picapture/README.md](pi4-software/picapture/README.md) | The vision pipeline and how to tune it |
| [rp2040-software/docs/bringup/](rp2040-software/docs/bringup/) | Standalone bring-up programs for exercising one device at a time |
