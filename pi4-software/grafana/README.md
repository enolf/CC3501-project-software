# Grafana on the Pi 4 — setup

Everything Grafana needs is in this directory and is committed. This file is the
procedure for wiring it up.

**Grafana runs on the Pi and nowhere else.** The development machine builds
`fridged` and the panel JSON; the Pi is the only place Grafana is installed.

**Installed from the `apt.grafana.com` package**, running under systemd as the
`grafana` user. (An earlier draft of this plan used Docker; the native package
is what is actually deployed, and the compose file was removed rather than left
to contradict this one.)

---

## The whole thing

```bash
cd ~/CC3501-project-software/pi4-software/grafana
bash setup-pi.sh
```

(`bash setup-pi.sh` rather than `./setup-pi.sh`, because the repo is edited on
Windows and the executable bit does not reliably survive the round trip. `chmod
+x setup-pi.sh` once if you prefer the shorter form.)

Run it as your normal user, **not** with `sudo` — it needs to know who you are so
the database ends up owned by you, and it calls `sudo` itself where root is
genuinely needed. Read it first if you like; it is short and every step is
reversible.

**Re-running it is how you deploy a dashboard change.** Edit the JSON here, run
it again, and Grafana picks the change up within 30 seconds without a restart.

Then give it data:

```bash
cd ~/CC3501-project-software/pi4-software
export FRIDGE_DB=/var/lib/fridge/fridge.db
python3 -m pip install pyserial
python3 -m fridged.seed --days 14
python3 -m fridged --port sim
```

Open `http://<pi-address>:3000` and pick *Fridge — Overview*.

---

## Styling the dashboard

`allowUiUpdates` is **true** while the layout is being designed, so the Save
button works and changes made in the browser stick. Restyle freely.

**Get it back into the repo before you re-run `setup-pi.sh`,** which re-copies
from `dashboards/` and will overwrite anything that only exists in Grafana:

1. *Dashboard settings → JSON Model*, or *Export → Export as JSON*
2. Replace `dashboards/fridge-overview.json` in the repo with it
3. Commit

Two fields to preserve when you paste: `"uid": "fridge-overview"` and each
panel's `datasource.uid` of `fridge-sqlite`. The test suite checks both, so
`python tests/test_fridged.py` on the dev machine will tell you if an export
dropped them — along with re-running every panel query against the schema.

Once the design has settled, set `allowUiUpdates: false` in
`provisioning/dashboards/fridge.yml` and re-run the script. The files become the
source of truth again and a stray browser edit can no longer diverge from git.

---

## The two things that are not obvious

### The database cannot live in your home directory

`/var/lib/fridge/fridge.db`, created by the setup script, owned by you with group
`grafana` and the setgid bit set.

The reason is not tidiness. **SQLite cannot open a WAL database read-only.** A
reader has to take a lock in the `-shm` wal-index file, and that is a write — so
`grafana` needs write access both to the database's directory and to the `-wal`
and `-shm` files beside it. Granting that inside `/home` would mean opening up
the home directory's traversal permissions, whose default mode has changed
between Raspberry Pi OS releases.

Grafana is still read-only in the sense that matters: the datasource only ever
runs `SELECT`s, and `fridged` remains the single writer (dashboard.md §3.1).

`fridged` sets `umask 002` itself so the files it creates are group-writable —
otherwise the `-shm` comes out mode 644 and every Grafana query fails with
*attempt to write a readonly database*, which reads like a Grafana bug and is a
file-mode one.

### The temperature tiles start empty, and that is correct

The board reports ROM codes and knows nothing about which shelf a sensor is on
(dashboard.md §4.3), so until the sensors are named there is no `freezer` for the
tile to find.

The table at the bottom of the dashboard shows every sensor with a live reading
and `(not named yet)` in the Zone column. Warm one with your hand, watch which
row moves, then:

```bash
sqlite3 /var/lib/fridge/fridge.db \
  "UPDATE sensor SET zone_label='freezer' WHERE rom_code='28FF...';"
```

Labels must be exactly **`freezer`**, **`fridge_top`**, **`fridge_bottom`**.

Naming a sensor relabels its **entire history**, not just readings from that
moment on — readings are stored under the ROM code and the name is joined on at
read time by the `temperature` view. The browser endpoint that does this without
SQL arrives at stage D4.

---

## If something does not work

| Symptom | Cause |
|---|---|
| *Datasource not found* on every panel | The plugin did not install. `sudo grafana-cli plugins ls`, then `sudo systemctl restart grafana-server` |
| *attempt to write a readonly database* | Permissions on `/var/lib/fridge`. Check `ls -l` shows group `grafana` and `rw` for the group **on the `-shm` and `-wal` files too**, not only on `fridge.db`. Re-running `setup-pi.sh` fixes an existing directory |
| *unable to open database file* | The path in the datasource does not exist. `FRIDGE_DB` was probably not exported, so `fridged` wrote to `pi4-software/fridge.db` instead |
| Panels load, no error, no data | Time range. Seeded history ends when you ran the seeder — try *Last 7 days*. If the graph has data but the tiles are empty, the sensors are not named |
| Dashboard missing entirely | `ls /var/lib/grafana/dashboards`, and `sudo journalctl -u grafana-server | grep -i provision` |
| Changes to the JSON do nothing | You edited the repo copy; the served copy is `/var/lib/grafana/dashboards`. Re-run `setup-pi.sh` |

To test a query by hand use **Explore** against the *Fridge* datasource — it
shows SQLite's own error text, which is far more useful than a blank panel.

### Checking the plumbing without Grafana

```bash
sqlite3 /var/lib/fridge/fridge.db \
  "SELECT zone, COUNT(*), ROUND(MIN(celsius),2), ROUND(MAX(celsius),2)
   FROM temperature GROUP BY zone;"
```

If that returns rows and the dashboard does not, the problem is Grafana-side. If
it returns nothing, `fridged` is not writing and Grafana is innocent.

---

## What is deliberately not here yet

- **No systemd unit for `fridged`** — stage D8. Until then run it in `tmux` or a
  terminal. It handles `SIGTERM` cleanly already, so the unit will be short.
- **No mDNS hostname.** `fridge.local:3000` needs Avahi and a DHCP reservation,
  also D8. Use the IP for now.
- **No backup.** The database is one file, so a nightly `cp` is enough, but it is
  not set up.
- **No alerting.** Discord contact point and the temperature / door-left-open
  rules are stage D7.
- **No admin password set.** Anonymous viewing is on and the admin account is
  still `admin`/`admin`. Fine on a bench, not fine on the society network —
  change it in the UI before this goes up anywhere.
