# Deployment — running the fridge for real

Grafana is set up separately, in [../grafana/](../grafana/). This directory is
about `fridged`: making it a service that survives reboots, backing up the
database, and swapping the simulated parts for real ones.

**Do the Grafana setup first.** It creates `/var/lib/fridge` with the ownership
both processes need, and puts you in the `grafana` group.

---

## Install

```bash
cd ~/CC3501-project-software/pi4-software/deploy
bash install.sh
```

As your normal user, not with `sudo`. Safe to re-run — re-running after a
`git pull` is how you deploy a change.

Afterwards `fridged` starts at boot, restarts if it crashes, and is no longer
tied to your terminal. Close the `tmux` session if you were using one, or you
will have two services fighting over the same database.

```bash
sudo systemctl status fridged
journalctl -u fridged -f
```

---

## Changing what it runs

**Edit `/etc/default/fridged`, never the unit file.**

```bash
sudoedit /etc/default/fridged
sudo systemctl restart fridged
journalctl -u fridged -f
```

`install.sh` will not overwrite that file once it exists — it holds local
choices, and clobbering them on every deploy would be its own bug.

---

## Swapping the simulated parts for real ones

**One at a time, confirming each before starting the next.** Three swaps done
together give a failure with three possible causes; done separately, each has
one. This is the same reasoning as plan.md stage 15.5, which insisted on a
laptop run before a Pi run for exactly this reason.

### 1. The real board

Requires the firmware built with `PI_LINK_BACKEND=serial`.

```bash
sudo apt install python3-serial       # not pip: PEP 668
ls -l /dev/ttyACM0                    # appears when the board is plugged in
```

Set `FRIDGED_ARGS=--port /dev/ttyACM0 --square fake`, restart, then watch:

```bash
journalctl -u fridged -f
```

**What good looks like:** `board booted: fw=...`, then a status line every 30 s
with `bad=0` and `link=up`. `bad` climbing means framing trouble — the offending
bytes are in `raw_line`, which is what that table is for.

**What to check afterwards:** the temperature tiles are showing the real
sensors' ROM codes, which will be different from the simulated ones and will
therefore appear **unnamed**. Name them as in [../grafana/README.md](../grafana/README.md).

### 2. Real Square

Needs `online-payment/tokens.py` and `sudo apt install python3-requests`.

Set `--square real`, restart, and walk a purchase: take a drink, close the door,
tap ONLINE, scan the QR, pay with the sandbox test card `4111 1111 1111 1111`.

It stays on the **sandbox** permanently — this project will never take real
money, so `SANDBOX = True` in `square.py` is the final state, not a step.

### 3. The camera

**Not ready, and it is not a `fridged` change.** `picapture` currently:

- prints its packet wrapped in debug text every 30 frames rather than on demand;
- calls `cv::imshow`, so it needs a display and cannot run under systemd;
- never writes `latest.jpg`;
- detects only one colour (documentation.md §7 issue 6).

Until those are fixed, `fridged` answers `CMD SCAN` from its simulated shelf and
everything downstream — stock, burn-down, reconciliation — is running on that.
It is honest data about a simulated shelf, not a broken camera.

When picapture is ready, the `fridged` side is a `PiCapture` class beside
`SimCamera` with the same two methods, plus the `/latest.jpg` endpoint that
stage D4 was deferred waiting for.

---

## Backups

Nightly at 03:30 into `/var/lib/fridge/backups`, fourteen kept.

```bash
bash backup.sh                       # take one now
systemctl list-timers fridge-backup.timer
```

**Not `cp`.** dashboard.md §13 says the database is one file so a backup is a
`cp`; that was true before WAL. The live database is three files, and copying
the first while `fridged` is writing captures a database missing every commit
still in the write-ahead log. `backup.sh` uses `sqlite3 .backup`, which takes a
consistent snapshot of a database being written to, then verifies the result
opens and passes `PRAGMA integrity_check`.

Restoring is decompressing one file over `fridge.db` with the service stopped:

```bash
sudo systemctl stop fridged
gzip -dc /var/lib/fridge/backups/fridge-YYYYMMDD-HHMMSS.db.gz \
  | sudo -u "$USER" tee /var/lib/fridge/fridge.db >/dev/null
sudo systemctl start fridged
```

---

## Measuring the load

```bash
bash measure-load.sh 300
```

dashboard.md §13 lists Grafana and OpenCV sharing one Pi 4 as a hazard and says
to measure it early rather than during the demo. Run it with the dashboard open
in a browser, and later with the camera running — the number that matters is all
of them together.

Above 80 °C the Pi throttles, and a throttled Pi during a demo looks like a
software fault. If the numbers are tight, the cheapest wins are raising
`FLUSH_INTERVAL_S` in `fridged/config.py` and lowering the dashboard's 30 s
refresh.

---

## Reaching it by name

Raspberry Pi OS ships `avahi-daemon`, so `http://<hostname>.local:3000` usually
works with nothing installed. Try it before doing anything:

```bash
hostname          # then try http://<that>.local:3000 from another machine
```

If it does not resolve, `sudo apt install avahi-daemon`. Either way, give the Pi
a **DHCP reservation** on the router — mDNS is convenient, a stable address is
what you fall back on when someone's laptop refuses to resolve `.local`.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `fridged` will not start | `journalctl -u fridged -n 50`. Usually `FRIDGE_DB` pointing somewhere the service cannot write, or a `--port` that does not exist |
| Starts, then restarts every 5 s | A crash loop. The traceback is in the journal; `Restart=always` is doing its job and making it obvious |
| Permission errors on the database | `grafana/setup-pi.sh` was not run, or you were added to the `grafana` group and have not logged out and back in |
| Grafana shows data that stopped | `systemctl status fridged`. Grafana only reads the file; if nothing writes it, the graph flatlines with no error anywhere |
| Two sets of data interleaved | A `tmux` session still running `fridged` alongside the service. Only one writer |
