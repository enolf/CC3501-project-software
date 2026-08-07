# Hardware test plan

Everything in here has been written and passes its off-hardware tests, but has
**never run on the fridge**. This file is the list of what still needs proving,
in the order it makes sense to prove it, with the exact commands.

Work through a stage, then paste the output back using the prompt at the end of
that stage. Each prompt names its stage so the results can be read against the
right expectations.

## What has and has not been checked

| | Status |
| --- | --- |
| Firmware host tests (`fridge_tests`) | Passing, off hardware |
| picapture config tests (`config_tests`) | Passing, off hardware |
| `protocol.py`, `test_fridged.py` | Passing, off hardware |
| `checkout.cpp` re-entrant basket, `RefundOwed` | **PASSED on the board** — T2.1–T2.10, commit `79a07c2` |
| Drink rename end to end (Coke/Fanta/Mountain Dew/Solo) | **PASSED on the board** — `items=coke:1`, `items=fanta:1` |
| picapture patch sampling + colour-centre storage | **PASSED on the Pi** — Coke now stores at hue 0, not 3 |
| Area-based can counting after the colour refactor | **PASSED** — 2 touching Cokes held at `coke:2` for 567 frames |
| `hue_weight` raised above 4.0 | **Not needed.** A still shelf shows zero count changes; nothing to fix |
| Per-frame confidence (`conf=`) | **PASSED on the Pi** — 87-90 still, 50-83 with a hand in frame |
| `fridged` running picapture as a subprocess | Tested against a stand-in, **never run against the real binary** |
| Refusing to answer a scan (decision D1) | Tested with a fake clock, **never run with a real camera** |
| Baseline latch and recount settling (stage 4) | **PASSED on the fridge** — 15/15 door cycles, T6 |
| The four settling constants | **Measured and confirmed**, T6 2026-08-07 |
| Confidence + trigger stored, dashboard panels (stage 5) | Passing off-hardware; **dashboard never rendered on the Pi** — T7 |

Record which commit you tested, so the results can be matched to the code:

```bash
git rev-parse --short HEAD
```

---

# T1 — Off-hardware. Any machine, no fridge, no board.

Run this first after every `git pull`. If anything here fails, stop — nothing
below will mean anything.

```bash
cd ~/CC3501-project-software

# 1. Firmware logic (basket arithmetic, coin gate, wire format)
cmake -S rp2040-software/tests/host -B build-host
cmake --build build-host
./build-host/fridge_tests

# 2. picapture tuning file and colour maths
cd pi4-software/picapture
cmake -S . -B build && cmake --build build
./build/config_tests
cd ../..

# 3. Wire protocol, both ends agree
cd pi4-software && python3 tools/protocol.py

# 4. The Pi service, end to end against a simulated board
python3 tests/test_fridged.py
```

**Expected:** `367 checks, 0 failed` · `102 checks, 0 failed` ·
`ALL CHECKS PASSED` × 2.

### T1.5 — The renamed drinks reach the database

```bash
cd ~/CC3501-project-software/pi4-software
rm -f /tmp/t1.db*
timeout 25 python3 -m fridged --port sim --sim-speed 400 --sim-activity 25 \
    --sim-seed 3 --db /tmp/t1.db --log-level WARNING
python3 - /tmp/t1.db <<'EOF'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
print('txn_item :', c.execute('SELECT drink, SUM(qty) FROM txn_item GROUP BY drink').fetchall())
print('stock    :', c.execute('SELECT drink, COUNT(*) FROM stock_snapshot GROUP BY drink').fetchall())
print('outcomes :', c.execute('SELECT outcome, COUNT(*) FROM txn GROUP BY outcome').fetchall())
EOF
```

**Expected:** both tables use `coke`, `fanta`, `mtndew`, `solo` — the *same*
keys in both. A `Coke`/`coke` mismatch means the wire-key change did not take.

> **Prompt to send me:**
> `Results from TEST.md T1 (off-hardware suites). Commit: <hash>` then paste all output.

---

# T2 — Firmware on the RP2040. Board + USB only, no Pi needed.

This is the **biggest untested block**: the whole change-your-mind basket logic,
the refund screen, and the drink rename. It runs against the built-in simulator,
so no camera, no coins and no network.

## Build and flash

```bash
cd ~/CC3501-project-software/rp2040-software
cmake -S . -B build -DPI_LINK_BACKEND=sim
cmake --build build
# copy build/labs.uf2 onto the RPI-RP2 drive (hold BOOTSEL while plugging in)
```

Open a serial terminal at 115200. `pyserial` is already installed for `fridged`,
so this needs nothing extra:

```bash
python3 -m serial.tools.miniterm \
    /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00 115200    # Ctrl-] to quit
```

> **Prefer `miniterm` over `screen`, and this is not a style preference.**
> `screen` **detaches instead of exiting** — closing the terminal, dropping the
> SSH connection, or a stray `Ctrl-A D` all leave it running and **still holding
> the serial port**. Everything that then tries to open it fails with
> `[Errno 16] Device or resource busy`, including `fridged`, which under
> `Restart=always` retries every five seconds forever and looks like a crash
> loop with an unrelated cause.
>
> This cost an hour once. When a port is unexpectedly busy, the first command
> is not a reboot:
>
> ```bash
> sudo fuser -v /dev/ttyACM0      # names the process holding it
> screen -ls                      # detached sessions, with their names
> screen -X -S <name-from-above> quit
> ```
>
> `miniterm` has no detached state: `Ctrl-]` exits and releases the port.

`screen /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00 115200` works too
(`Ctrl-A` then `K` to quit **properly**), but `screen` is not installed by
default on Raspberry Pi OS.

> **Use the `by-id` path, not `/dev/ttyACM0`.** The number changes between
> replugs and reboots, and chasing it wastes more time than it sounds like it
> should. `ls -l /dev/serial/by-id/` shows the stable name.

> **Stop `fridged` first** — `sudo systemctl stop fridged`. It holds the port,
> and the unit is `Restart=always`, so it comes back after a reboot. Keep it
> stopped for the whole session.

**Check the boot banner before doing anything else.** Reset the board with the
terminal attached; it prints which backend is compiled in:

```
pi link backend: sim        ← what T2 needs
pi link backend: serial     ← wrong build, reflash. See below.
```

**A serial build faults `002` (`FAULT_PI_UNREACHABLE`) about 30 s after boot**
with nothing talking to it, which is correct behaviour and looks exactly like a
hardware problem. The sim backend cannot produce that fault — its `is_healthy()`
returns `true` unconditionally — so fault 002 on this stage means the wrong
binary, every time.

The firmware **cannot be built on the Pi** (no ARM cross-compiler; CMake refuses
with "the active CMake kit is a host compiler"). Build on a machine with the
arm-none-eabi toolchain, and flash by holding BOOTSEL, plugging into *that*
machine, and dragging `labs.uf2` onto the `RPI-RP2` drive. **The drive
disappearing is the success signal.** Going via the Pi and WinSCP adds two
failure modes and no benefit.

`sim` is the default in `CMakeLists.txt`, so a **clean** build directory gives
the right thing — `rm -rf build` is the reliable fix when the cache holds
`serial` from earlier. Confirm `-- Pi link backend: sim` in the configure output
before flashing.

With the **sim** backend, keys take effect immediately — no Enter needed.
Press `?` for the key list; a serial build ignores stdin entirely, which is a
second independent check on which binary is running.

## ...or run T2 without reflashing at all

**If the board already has a `serial` build on it, you do not have to reflash.**
Everything T2 tests can be driven through `fake_pi.py` over the real link:

```bash
sudo systemctl stop fridged
cd ~/CC3501-project-software/pi4-software/tools
python3 fake_pi.py /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00
```

The split of responsibilities changes, but nothing under test does:

- `o c , . / 5 6` are forwarded raw as debug keys and handled locally by
  `main.cpp`, exactly as they are under the sim backend.
- `1 2 3 4` and `Q W E R` are handled by **`fake_pi` itself**, which owns the
  shelf — with the serial backend compiled the board has no shelf of its own.
  `fake_pi.py` says so in a comment, and calls it "the only way to exercise the
  board's change-your-mind path against real firmware".
- `CMD SQUARE_LINK` and `CMD SQUARE_CANCEL` are answered automatically, so T2.7
  works unchanged.

**Every command needs Enter**, because the board no longer owns the keyboard.

This is arguably the better run: it exercises the real serial link and the real
`EVT INV` path at the same time, folding T4.1 into T2. The checkout state
machine — the actual subject of T2 — is identical either way.

| Key | Does |
| --- | --- |
| `o` / `c` | Door open / closed |
| `1 2 3 4` | Take a Coke / Fanta / Mountain Dew / Solo |
| `Q W E R` | Put one back (shifted key above the one that took it) |
| `i` | Show the simulated shelf |
| `r` | Restock |
| `,` `.` `/` | Touch CASH / ONLINE / BACK |
| `5` / `6` | Insert a $1 / $2 coin |
| `p` | Square reports payment received |

Every drink is **$2.00**. The shelf starts with 5 of each.

Watch the `checkout: X -> Y` lines in the log — those are the evidence.

## T2.1 — A plain purchase still works

```
o        (door opens)
1        (take a Coke)
c        (door closes)
```
**Expect:** `Selecting -> Recount -> PaymentSelect`, log says `1 item(s), owed 200c`.

```
,        (tap CASH)
6        (insert $2)
```
**Expect:** `PayCash -> ThankYou`, `EVT TXN_START ... items=coke:1`, `outcome=paid`.

> Note the item is `coke:1` — **lower case, wire key**. `Coke:1` means the
> rename did not take effect.

## T2.2 — Put it back → nothing is owed

```
o  1  c        (take a Coke, close)      -> PaymentSelect, owed 200c
o  Q  c        (reopen, put it back, close)
```
**Expect:** `checkout: nothing taken, nobody is charged`, then `-> Idle`.
**Fail if:** it stays on the payment screen, or logs a restock, or still charges.

## T2.3 — Swap → the price follows the new drink

```
o  1  c        -> PaymentSelect, Coke, owed 200c
o  Q  2  c     (put the Coke back, take a Fanta, close)
```
**Expect:** back to `PaymentSelect`, screen now shows **Fanta**, owed 200c.
**Fail if:** it charges for both (400c), or still says Coke.

## T2.4 — Several drinks at once

```
o  1  1  2  c
```
**Expect:** `3 item(s), owed 600c`.

```
o  Q  c        (put one Coke back)
```
**Expect:** `2 item(s), owed 400c`.

## T2.5 — Money in the box, then everything returned → RefundOwed

```
o  1  c        -> PaymentSelect, owed 200c
,              (tap CASH)
5              (insert ONE $1 - not enough, stays on PayCash)
o  Q  c        (reopen, put the can back, close)
```
**Expect:** the screen says **you are owed $1.00**, and the log has
`REFUND OWED` plus an SD line `REFUND_OWED id=... cents=100`.
**Fail if:** it goes silently to Idle, or to ThankYou, or keeps asking for money.

## T2.6 — Part-paid, then a can returned → already covered

```
o  1  1  c     -> owed 400c
,              (CASH)
6              (one $2 - not enough for 400c)
o  Q  c        (put one Coke back -> owed 200c)
```
**Expect:** `the new basket is already covered by the coins in the box`, then
`-> ThankYou`. The $2 already in the box pays for the one remaining can.

## T2.7 — Reopening during an online payment kills the link

```
o  1  c   .    (tap ONLINE, wait for the QR screen)
o              (open the door again)
```
**Expect:** `CMD SQUARE_CANCEL id=...` in the log, and `PayOnlineQr -> Selecting`.
**Fail if:** no cancel is sent — a live payment link would outlive its basket.

## T2.8 — Walking away with the door open is still a theft

```
o  1  c        -> PaymentSelect
o              (reopen and leave it open, wait 30 s)
```
**Expect:** `door left open with an unpaid basket`, then `outcome=stolen`.
**Fail if:** it goes quietly to Idle — that would erase a sale already announced.

## T2.9 — The first purchase after a reboot is charged

Reset the board, then immediately:
```
o  1  c
```
**Expect:** `PaymentSelect`, owed 200c.
**Fail if:** it says `restocked, nobody is charged` — that was the old bug where
the first sale after every boot was free.

## T2.10 — A genuine restock charges nobody

```
1  1           (take two Cokes with the door shut - the camera has not looked)
o              (open: baseline is captured here, Coke = 3)
r              (restock to 5)
c
```
**Expect:** `restocked, nobody is charged`, `-> Idle`.

## Result — 2026-08-07, commit `79a07c2`: **10/10 passed**

Run through `fake_pi.py` over the real serial link rather than the sim backend.
Two timing constants landed on their nominal values, which is worth recording:

- T2.5 `RefundOwed -> Idle` took **10.008 s** against `REFUND_OWED_MS = 10000`
- T2.8 theft fired at **exactly 30 s** against `SELECT_TIMEOUT_MS`

The late-payment race (`l`) was exercised as a bonus and behaved:
`REFUND OWED - Square took 600c ... after the link was cancelled`.

Observations that are not faults:

- `loop: pass took 77-203 ms` accompanies every state change. It is the TFT
  repaint; the 203 ms case is rendering the QR. Idle sits at 17 ms, and nothing
  here threatens the 8 s recount budget.
- `box_g` drifts to about -0.2 g when empty. The mass gate tolerance is 1.00 g
  and coins are 6.60/9.80 g, so this is comfortably inside it.
- `fake_pi.py` dies with `[Errno 5] Input/output error` if the board resets
  under it. A **manual** `fridged` run will do the same; under systemd
  `Restart=always` covers it. Worth knowing before T5 and T6, where resetting
  the board mid-run is a normal thing to do.

> **Prompt to send me:**
> `Results from TEST.md T2 (firmware simulator — re-entrant basket + refund). Commit: <hash>`
> then paste the serial log, and say which of T2.1–T2.10 passed.

---

# T3 — picapture on the Pi. Camera needed.

## T3.0 — Get the right code onto the Pi, and build it there

**Do not skip this.** This has already gone wrong once: the Pi was running a
binary built before the changes, and the results looked like a colour-tuning
problem for an afternoon. T1 builds picapture as a side effect, but only on the
machine T1 was run on — if you ran T1 on a laptop, the Pi still has the old
binary.

```bash
cd ~/CC3501-project-software
git pull
git rev-parse --short HEAD          # ← this is the hash to quote in the prompt

cd pi4-software/picapture
cmake -S . -B build && cmake --build build
```

**Expect:** several files compiling, including `main.cpp` **and**
`vision_config.cpp`, then `PiCapture` and `config_tests` both linking.

**Fail if:** only one source file compiles — that is a stale build directory.
`rm -rf build` and run the two cmake lines again.

Prove the binary is the new one before testing anything with it:

```bash
./build/config_tests | tail -3
```

**Expect:** `102 checks, 0 failed`. If it says `84`, the pull did not take.

## T3.1 — Your existing tuning still loads

The config format changed from low/high bands to a single colour. Old files are
supposed to convert.

```bash
cd ~/CC3501-project-software/pi4-software/picapture
cat picapture.conf                 # keep a copy of what it says now
./build/PiCapture --headless
```

**Expect:** `tuning loaded from picapture.conf`, and count lines that now end
with a confidence figure: `coke:5,fanta:4,mtndew:5,solo:3;conf=87;`
**Fail if:** it refuses to start — paste the error, the file needs converting.
**Fail if:** there is no `conf=` — that is the old binary, rebuild.

## T3.2 — Retune with patch sampling

Each click now samples a 13×13 patch, not one pixel, so this is worth redoing
from scratch.

```bash
./build/PiCapture --debug-all
```

**First, before clicking anything: is the picture the right way up?**
`rotate_180` is now `false`, having previously been `true` and flipping an
already-upright image. Nothing downstream will ever complain about this — a
flipped frame detects cans perfectly well — so it has to be looked at.
If it is upside down, add `rotate_180 = true` as a line in `picapture.conf`.

For each drink: press its number, **check the camera window names it**, then
click twice on that one can — brightest part, then dullest. Then `p`:

**Expect** single values, not ranges:
```
 1 Coke           H   0  S 247  V 190   can=not set
 2 Fanta          H   9  S 247  V 220   can=not set
 3 Mountain Dew   H  44  S 220  V 150   can=not set
 4 Solo           H  27  S 247  V 200   can=not set
```

**The number that matters is Coke's hue.** It should be at or very near **0**.
The old format could not store hue 0 and shifted it to 3 — straight towards
Fanta, throwing away a third of the separation.

Then click each can again and read the distances. Every drink's own colour
should be clearly nearest.

**Also prove the guards work**, because they exist for a mistake that actually
happened — Solo stayed armed while four clicks landed on a Coke, a Fanta and a
Mountain Dew, and its colour was dragged to between two drinks and saved:

| Try this | Expect |
| --- | --- |
| Press `4` (Solo), then click a Coke and a Mountain Dew | `IGNORED: the sampled pixels span N hue…` and **Solo unchanged**. Check with `p`. |
| Retune a drink, then press `u` | `undone: … is back to its previous colour` |
| Press `r` | That drink returns to its built-in colour |

**Fail if:** clicking two different drinks silently changes anything. That guard
is the difference between a bad click costing one keystroke and costing an
afternoon.

## T3.3 — Calibrate can sizes

For each drink: put **exactly one** can of it in view, press its number, press `c`.

**Expect:** `one Coke is ~6000 pixels - two touching cans will now count as two`.

Then `s` to save, and commit:
```bash
git add picapture.conf && git commit -m "Retune picapture after the colour format change"
```

`picapture.conf` is not in the repo yet — it only exists on the Pi, created by
pressing `s`. This commit is what puts it there, and it is worth doing: it is a
measurement of a real fridge under real lights that took a person standing at
the door to produce, and losing the SD card would mean doing it all again.

## T3.4 — The merge test (repeat after retuning)

```
two Cokes a few cm apart   -> press a
```
**Expect:** `Coke  2 blob(s) -> 2 can(s)`, two similar areas.

```
same two Cokes pushed together -> press a
```
**Expect:** `Coke  1 blob(s) -> 2 can(s)` and one area about twice a single can.
**Fail if:** it says 1 can (merge not handled) or 3 (divisor too small).

## T3.5 — Does raising `hue_weight` help or hurt?

Coke and Fanta are genuinely close. Raising `hue_weight` makes hue count for
more than brightness:

| `hue_weight` | Coke↔Fanta separation |
| --- | --- |
| 4 (now) | 36.8 |
| 6 | 54.5 |
| 8 | 72.4 |

```bash
echo "hue_weight = 6.0" >> picapture.conf
./build/PiCapture --debug-all
```

That is a **line appended to `picapture.conf`**, not a shell setting. Appending
a second `hue_weight` line is fine — the last one in the file wins — but after
trying two or three values, open the file and delete the stragglers so it says
what you think it says.

**Check both things:**
1. Saving no longer warns that Coke and Fanta are too close.
2. **Every drink still forms one solid blob** in the *after cleanup* window.

The cost of a high `hue_weight` is that a can whose own hue varies — Mountain
Dew spans about ten — starts shedding its edge pixels and fragmenting. If Dew
goes patchy or its count drops, try `5.0`, and tell me what happens at each.

## T3.6 — Two minutes of stability

Load the shelf as it would actually be stocked, then leave it completely alone:

```bash
./build/PiCapture --headless | tee /tmp/stability.log
# leave for 2 minutes, Ctrl-C, then:
cut -d';' -f1 /tmp/stability.log | sort | uniq -c | sort -rn | head
```

The `cut` drops the `conf=` field. It moves by a point or two every frame, so
counting whole lines would report every frame as unique and tell you nothing.

**Expect:** one line with almost all the count, matching what is really on the
shelf. Several lines with similar counts means it is still wandering — that is
the thing stage 4's settling filter has to work with, so I need to know how bad
it is.

Then the confidence over the same run:

```bash
grep -o 'conf=[0-9]*' /tmp/stability.log | sort | uniq -c | sort -rn | head
```

**Expect:** clustered in a narrow band. Wide scatter on a shelf nobody touched
means the figure is measuring noise rather than quality, and I need to know.

## T3.7 — Confidence responds to things being wrong

New in stage 2. The point of `conf=` is that a wrong count is otherwise
invisible — the packet is perfectly well-formed either way. So the test is not
"is the number plausible", it is "does it move when something is actually
wrong".

Run `--debug-all` and press `a` after each step, so you get the full breakdown
rather than just the score:

| Do this | Expect |
| --- | --- |
| Normal, well-lit, calibrated shelf | High. If any drink is uncalibrated the breakdown should say so by name. |
| Push two cans of one drink together | Confidence should hold up **if** `can_area` is right for that drink — the blob divides cleanly. If it drops a lot, the can area is wrong. |
| Half-cover one can with your hand | Should fall. The blob is now an odd fraction of a can. |
| Dim the light right down | Should fall hard, and the breakdown should say **the picture is dark** rather than blaming the tuning. |

**Fail if:** the number sits at the same value through all four. That means it
is not measuring anything and I have built a decoration.

Also worth one line: what does it read on a completely **empty** shelf in good
light? It should be near 100 — correctly seeing nothing is a good frame, not a
bad one, and if it reads low the figure means two different things at once and
no threshold can be set on it.

## Result — 2026-08-07, commit `79a07c2`

T3.0-T3.4, T3.6, T3.7 **passed**. T3.5 **deliberately skipped** — see below.

**T3.6 is the headline: 567 of 568 frames identical.** The shelf was two Cokes
touching, then Fanta, Solo and Mountain Dew side by side with no gaps, and it
reported `coke:2,fanta:1,mtndew:1,solo:1` correctly for two and a half minutes
with **not one count change**. Only frame 1 differed, at `conf=51`, before the
exposure settled.

Confidence over the same run: 309x89, 125x88, 122x90, 11x87, 1x51. Four values.

Consequences:

- **The Coke/Fanta instability seen earlier was hands in frame, not tuning.**
  Every burst lines up with somebody reaching in. A still shelf does not wander.
- **T3.5 (`hue_weight`) is therefore not worth running.** There is no
  instability left for it to fix, and raising it risks fragmenting Mountain Dew,
  whose own hue spread would start falling outside `max_brand_dist`. Do not take
  that risk for a problem that is not there. `hue_weight = 4` stands.
- Recalibrating `can_area` fixed a real double-count: Mountain Dew read `2` with
  the stale divisor 5618 and `1` after remeasuring at 6557.
- A hand reads as cans — skin sits around hue 5-20, between Coke and Solo. No
  colour tuning separates that. It is what the stage 4 baseline latch is for.

Measured `can_area`: Coke 6350, Fanta 6915, Mountain Dew 6557, Solo 6270.

> **Prompt to send me:**
> `Results from TEST.md T3 (picapture vision on the Pi). Commit: <hash>`
> then paste: the `p` output after retuning, the `a` output from T3.4, what
> happened at `hue_weight` 4 / 5 / 6, both `uniq -c` tables from T3.6, and the
> four `a` breakdowns from T3.7.

---

# T4 — Board and Pi together. Both, plus the serial cable.

Confirms the renamed drinks survive a real serial link. picapture is **not**
wired into `fridged` yet — that is stage 3 — so this still runs the simulated
camera. The `conf=100` below comes from `fake_pi.py`, not from the camera.

## Build the firmware for the real link

```bash
cd ~/CC3501-project-software/rp2040-software
cmake -S . -B build -DPI_LINK_BACKEND=serial
cmake --build build
# flash build/labs.uf2
```

With the **serial** backend the board no longer owns the keyboard, so debug keys
need a single character **followed by Enter**, sent by `fake_pi.py`.

## T4.1 — fake_pi drives the board

```bash
sudo systemctl stop fridged        # only one process may own the port
cd ~/CC3501-project-software/pi4-software/tools
python3 fake_pi.py /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00
```

Then, typing into fake_pi:
```
o        (door open)
1        (take a Coke - fake_pi owns the shelf now)
c        (door close)
```
**Expect:** `CMD SCAN` out, `EVT INV coke=4 fanta=5 mtndew=5 solo=5 conf=100`
back, and the board reaching PaymentSelect.
**Fail if:** the board logs `INV frame carried no drink counts` — the wire keys
disagree between the two ends.

Then test put-back over the real link:
```
o  Q  c
```
**Expect:** the board returns to Idle with nothing owed.

## T4.2 — fridged records it

```bash
sudo systemctl start fridged
sleep 5 && systemctl status fridged --no-pager | head -20
sudo sqlite3 /var/lib/fridge/fridge.db \
  "SELECT drink, qty FROM txn_item ORDER BY rowid DESC LIMIT 10;"
```

**Expect:** drinks stored as `coke` / `fanta` / `mtndew` / `solo`.

> **Prompt to send me:**
> `Results from TEST.md T4 (board + Pi over the real serial link). Commit: <hash>`
> then paste the fake_pi output, the board log and the SQL result.

---

# T5 — `fridged` driving the real camera. Pi + camera; board optional.

New in stage 3. picapture is now a **child process of `fridged`**, so the two
halves that had never been connected are connected. Everything here has been
tested against a stand-in that prints packets; none of it has run against the
real binary.

**Only one process may hold the camera.** Close any `--debug-all` window and
stop the service before starting another copy, or the second one fails to open
the device and it looks like a hardware fault.

## T5.1 — The hybrid: real camera, simulated board

The most useful configuration for this stage. No board needed.

```bash
cd ~/CC3501-project-software/pi4-software
sudo systemctl stop fridged
rm -f /tmp/t5.db*
timeout 60 python3 -m fridged --port sim --sim-speed 200 --sim-activity 30 \
    --sim-seed 3 --db /tmp/t5.db --camera picapture
```

**Expect**, in this order:
- `REAL CAMERA - counting a physical shelf with .../build/PiCapture`
- `picapture started (pid NNNN) in .../picapture`
- `[picapture] tuning loaded from picapture.conf` — **if this says "using
  built-in defaults", the working directory is wrong and it is running on
  tuning nobody chose.**
- `[picapture] libcamerasrc ...` — the pipeline it actually asked for
- no `CMD SCAN cannot be answered` lines

**Fail if:** `picapture is not built` — run T3.0.
**Fail if:** a flood of `CMD SCAN cannot be answered` — the camera is producing
nothing; check the `[picapture]` lines for why.

Then check what was stored:

```bash
sqlite3 /tmp/t5.db "SELECT drink, count FROM stock_snapshot
                    ORDER BY ts DESC LIMIT 8;"
```

**Expect:** the counts to match what is physically on the shelf. This is the
first time in the project that a row in the database describes a real object.

## T5.2 — Take the camera away

The D1 decision, on real hardware.

> **Do not unplug the CSI ribbon to do this.** The camera connector is not
> hot-pluggable, and disconnecting it on a powered Pi risks the camera module
> and the Pi. Killing the process exercises exactly the same code path — death,
> restart, staleness, refusal — and costs nothing.

With T5.1 still running, in a second terminal:

```bash
pkill -f PiCapture          # once: does it come back?
```

**Expect:** `picapture exited (code N); restarting in 2s`, then
`picapture started (pid NNNN)`, then counts resume. `fridged` itself never
stops.

Now hold it down, so the counts genuinely go stale:

```bash
while true; do pkill -f PiCapture; sleep 0.5; done
```

**Expect:** the restart delay doubling — 2s, 4s, 8s — and once the newest count
is older than `CAMERA_DEAD_S` (5 s),
`CMD SCAN cannot be answered; letting the board fault rather than guessing`.

Ctrl-C that loop.

**Expect:** it recovers on its own, with no restart of `fridged`.

**Fail if:** the service crashes, or keeps answering with the last counts it saw
before picapture died. Answering from a shelf it can no longer see is the one
behaviour this must never have.

If you do want to prove the *physical* camera-gone case, power the Pi down
first, disconnect, and power back up — then picapture fails at startup instead,
which is the `could not open the camera` path.

## T5.3 — Nothing is left holding the camera

```bash
# Ctrl-C the service, then:
pgrep -a PiCapture
```

**Expect:** no output. An orphaned picapture keeps the device open and the next
start fails for a reason that looks nothing like the cause.

## T5.4 — The whole fridge

Board, Pi, camera, real serial link. Flash the `serial` backend as in T4.

```bash
sudo systemctl stop fridged        # only one process may own the serial port
cd ~/CC3501-project-software/pi4-software
python3 -m fridged --port /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00 \
    --db /tmp/t5full.db --camera picapture
```

Then, at the fridge: open the door, take one can, close it.

**Expect:** two `stock_snapshot` groups a few seconds apart, differing by
exactly the one can, and the terminal reaching the payment screen with that
drink and $2.00.

```bash
sqlite3 /tmp/t5full.db "SELECT ts, drink, count FROM stock_snapshot
                        ORDER BY ts DESC LIMIT 8;"
```

**This is the first real end-to-end purchase, and it is the one most likely to
disagree.** The baseline scan happens as the door swings open — the worst frame
of the cycle, with the light changing and a hand possibly already in shot — and
stage 3 does no settling whatsoever. **A wrong count here is expected**, not a
bug: it is exactly what stage 4 exists to fix. What matters is *how* it is
wrong, so record what you took versus what it charged for.

> **Prompt to send me:**
> `Results from TEST.md T5 (fridged driving the real camera). Commit: <hash>`
> then paste: the startup lines from T5.1, the `stock_snapshot` rows, what
> happened while picapture was being killed, and for T5.4 **what you took
> versus what it charged for** — that last one is the input stage 4 needs.

---

# T6 — Measuring the four settling constants. Pi + camera + board.

**This stage is not a pass/fail check. It is a measurement.**

Stage 4's machinery is written and tested against a fake clock, but it runs on
four numbers that were guessed:

| Constant | Now | What should set it |
| --- | --- | --- |
| `SETTLE_FRAMES` | 3 | T6.1 — how many frames a still shelf needs to agree |
| `SETTLE_TIMEOUT_S` | 4.0 | T6.2 — how long after the door shuts it settles |
| `SCAN_ANSWER_BUDGET_S` | 6.0 | derived; must stay under the board's 8 s |
| `UNSETTLED_CONFIDENCE_SCALE` | 0.5 | T6.3 — how often it fails to settle at all |

They live in `pi4-software/fridged/config.py` under a comment naming this stage.
**Send me the numbers and I will set them** — do not tune them by feel, because
the failure mode of getting them wrong is a fridge that charges the wrong person
occasionally, which is exactly the thing no amount of staring at it will reveal.

## T6.1 — How quickly does a still shelf agree with itself?

Stock the shelf normally, shut the door, leave it completely alone.

```bash
cd ~/CC3501-project-software/pi4-software/picapture
./build/PiCapture --headless | ts '%.s' | tee /tmp/t6-still.log
# leave for 60 seconds, Ctrl-C
```

If `ts` is missing: `sudo apt install moreutils`. If you would rather not, drop
`| ts '%.s'` — the run is still useful, just without timings.

```bash
cut -d';' -f1 /tmp/t6-still.log | sed 's/^[0-9.]* //' | uniq -c | head -20
```

**What I need:** that table. It shows the *runs* — how many identical frames in
a row the camera produces on a shelf nobody is touching.

- Long runs (20+) mean `SETTLE_FRAMES = 3` is comfortable and could go higher.
- Runs of 1 and 2 mean the counts are flickering, and **no settling value will
  fix that** — it is a tuning problem, and T3 is where it gets fixed.

## T6.2 — How long after the door shuts does it actually settle?

The number `SETTLE_TIMEOUT_S` has to cover. Board connected, real serial link.

```bash
sudo systemctl stop fridged
cd ~/CC3501-project-software/pi4-software
python3 -m fridged --port /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00 --db /tmp/t6.db \
    --camera picapture --log-level DEBUG 2>&1 | tee /tmp/t6-cycles.log
```

Do **ten** door cycles: five taking one can, five taking nothing. Close the door
normally each time — no lingering, no holding it half open.

```bash
grep -E "scan deferred|did not hold still|on the deadline" /tmp/t6-cycles.log
```

**What I need:** how many of the ten produced `did not hold still` or
`on the deadline`.

- None: 4 s is generous, and could come down — a shorter timeout means a faster
  terminal.
- Most of them: 4 s is too short, or the picture genuinely never stabilises.
  Those are different problems and the `[picapture]` lines will say which.

## T6.3 — Does it get the right answer?

The acceptance question, and the one that decides whether stage 4 worked.

From the same run, for each of the ten cycles record **what you took** and
**what the board charged for**:

```bash
sqlite3 /tmp/t6.db "
  SELECT ts, drink, count FROM stock_snapshot ORDER BY ts, drink;"
sqlite3 -header -column /tmp/t6.db "
  SELECT t.txn_id, t.owed_cents, t.paid_cents, t.outcome, i.drink, i.qty
  FROM txn t
  LEFT JOIN txn_item i ON i.boot_id = t.boot_id AND i.txn_id = t.txn_id
  ORDER BY t.ts_start;"
```

**Expect:** the five empty cycles charge nobody, and the five single-can cycles
charge $2.00 for the drink you actually took.

**The five that took nothing are the more important half.** A cycle where
nothing was taken and nothing was charged is the case that used to break: the
baseline caught mid-swing, one can short, and the recount then showing a shelf
that had *gained* a can. If any of those five produced a transaction, paste the
whole cycle's log — that is the latch not working.

## T6.4 — The confidence actually separates good from bad

```bash
grep -oE "conf=[0-9]+" /tmp/t6-cycles.log | sort | uniq -c | sort -rn
```

**Expect:** two clusters — a high one for settled answers, a low one for anything
forced out. If everything is one number, the mark-down is not distinguishing
anything and `UNSETTLED_CONFIDENCE_SCALE` needs revisiting.

## Result — 2026-08-07: **15 of 15 cycles correct.** Constants confirmed.

Run against the real camera, the real board and a real shelf.

| Constant | Value | What the measurement said |
| --- | --- | --- |
| `SETTLE_FRAMES` | 3 | Recount answered 550-650 ms after entering Recount |
| `SETTLE_TIMEOUT_S` | 4.0 | **Never fired**, in 15 cycles. ~3.4 s spare |
| `SCAN_ANSWER_BUDGET_S` | 6.0 | Never fired |
| `UNSETTLED_CONFIDENCE_SCALE` | 0.5 | Applies only when genuinely unsettled |

**Nothing was changed.** Raising `SETTLE_FRAMES` to 5 was considered and
rejected: fifteen correct cycles with no timeouts is not a system asking for
more evidence per decision, and it would cost 500 ms on every purchase.

Cycles: four single-can takes, one **two-can** take (Solo + Mountain Dew), five
put-backs, five open-and-close with nothing touched. Every one correct. No
transaction rows, correctly — no payment method was ever chosen.

**Two bugs were found by this stage, both invisible off-hardware:**

1. **A still shelf was graded "unsettled".** `settled` was inferred as
   `latched is not latest`, but on a motionless shelf every frame agrees, so
   `_stable` is reassigned to `_latest` each frame and the two ARE the same
   object. Every baseline came back at confidence 40 against a shelf measured
   stable for 567 consecutive frames. Fixed by recording the flag at latch time.
2. **A healthy answer logged nothing at all.** Both branches of `_grade()` only
   spoke up when a penalty applied, so success was indistinguishable from the
   camera having gone quiet — which is exactly how it was misread here. A
   DEBUG line now states the good case explicitly.

> **Prompt to send me:**
> `Results from TEST.md T6 (settling constants). Commit: <hash>`
> then paste: the run table from T6.1, the grep counts from T6.2, **for each of
> the ten cycles what you took versus what it charged for** (T6.3), and the
> confidence clusters from T6.4.
>
> With those four I can set the constants from measurements and close stage 4.

---

# T7 — The confidence on the dashboard. Pi + camera + board + Grafana.

New in stage 5. `conf=` used to go out on the wire and be discarded; it is now
stored, and `stock_snapshot.trigger` finally records which of the two scans a
row came from. Everything here passes off-hardware — what has not been seen is
the dashboard actually rendering.

## T7.1 — The data lands

Ten door cycles is plenty. Reuse the T6 run if you still have `/tmp/t6b.db`,
otherwise:

```bash
sudo systemctl stop fridged
cd ~/CC3501-project-software/pi4-software
python3 -m fridged --port /dev/serial/by-id/usb-Raspberry_Pi_Pico_*-if00 \
    --db /tmp/t7.db --camera picapture 2>&1 | tee /tmp/t7.log
```

Take a can on some cycles, nothing on others, and **pay for at least one**.

```bash
sqlite3 -header -column /tmp/t7.db "
  SELECT trigger, COUNT(DISTINCT ts) AS scans FROM stock_snapshot
  GROUP BY trigger;"

sqlite3 -header -column /tmp/t7.db "
  SELECT COUNT(*) AS readings, MIN(value) AS worst,
         ROUND(AVG(value),1) AS mean, MAX(value) AS best
  FROM measurement WHERE metric = 'camera.confidence';"
```

**Expect:** roughly equal numbers of `door_open` and `door_close` scans, and one
confidence reading per scan.

**Fail if:** every row says `door_close` — the trigger is not being recorded and
the confidence panel will show one series instead of two.

**Fail if:** `readings` is 0 — the confidence is still being thrown away.

Worth a look at the two side by side, since this is the pairing the panel joins
on:

```bash
sqlite3 -header -column /tmp/t7.db "
  SELECT datetime(s.ts,'unixepoch','localtime') AS at, s.trigger,
         m.value AS confidence
  FROM (SELECT DISTINCT ts, trigger FROM stock_snapshot) s
  JOIN measurement m ON m.ts = s.ts AND m.metric = 'camera.confidence'
  ORDER BY s.ts DESC LIMIT 12;"
```

## T7.2 — The panels render

Point Grafana at the database the run above wrote, or re-run against
`/var/lib/fridge/fridge.db` with the service. Open **Fridge — Overview** and
scroll to the stock row.

| Panel | Expect |
| --- | --- |
| **Camera confidence** | Two series, **Baseline** and **Recount**, stepped, 0-100. Not one line, not empty. |
| **Scans nobody should trust** | A number. Green at 0. |

**Fail if:** either panel says "No data" — the query is wrong, and
`python3 tests/test_fridged.py` should have caught it, so tell me.

**Fail if:** only one series appears — see T7.1, the trigger is not landing.

## T7.3 — A bad scan is visible afterwards

The whole point of the stage: with no camera frames kept, this is the **only**
record that a count was doubtful. The counts themselves look identical.

Provoke one. Open the door and hold your hand across the shelf while you close
it, so the recount never settles:

```bash
grep -E "below 50|on the deadline|did not hold still" /tmp/t7.log
```

**Expect:** a `confidence NN (below 50) - this sale rests on a count nobody
should trust` line, the **Scans nobody should trust** stat going amber, and a
visible dip in the graph.

**Fail if:** the log warns but the panel stays at 0 — the threshold in the panel
has drifted from `LOW_CONFIDENCE_THRESHOLD` in `fridged/config.py`. Grafana
cannot read Python, so the 50 is written in both places by hand.

> **Prompt to send me:**
> `Results from TEST.md T7 (confidence on the dashboard). Commit: <hash>`
> then paste: the three SQL results from T7.1, whether both panels rendered with
> two series, and what happened when you provoked a bad scan.

---

# If something fails

Send the stage prompt with the output regardless — a failure is more useful than
a skipped test. Include:

- which numbered test it was (`T2.5`, `T3.4`, …)
- what you expected versus what happened
- the surrounding log lines, not just the error

For picapture specifically, `p` (current tuning) and `a` (blob areas and
brightness) are worth including in any report — most vision problems are visible
in one of those two.
