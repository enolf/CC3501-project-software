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
python3 -m fridged.seed --days 14
python3 -m fridged --port sim
```

**Two packages come from apt, not pip.** Raspberry Pi OS is an externally
managed Python environment (PEP 668), so `pip install` into the system
interpreter is refused:

```bash
sudo apt install python3-serial sqlite3
```

`python3-serial` is only needed for a real board — `--port sim` never imports it.
`sqlite3` is the **command line**, which is a different package from Python's
built-in `sqlite3` module: the module is always present, the command is not
installed by default. `setup-pi.sh` installs it, because naming the sensors and
every troubleshooting step below need it.

Resist `--break-system-packages`: both are packaged, and a venv would then have
to be activated by the systemd unit at D8 for no benefit.

Open `http://<pi-address>:3000` and pick *Fridge — Overview*.

---

## Styling the dashboard

`allowUiUpdates` is **true** while the layout is being designed, so the Save
button works and changes made in the browser stick. Restyle freely.

### What survives what

| Action | Effect on your UI changes |
|---|---|
| Saving in the browser | Stored in Grafana's own database |
| `systemctl restart grafana-server`, reboot | **Kept** |
| `git pull` on the Pi | **Kept** — Grafana serves `/var/lib/grafana/dashboards`, not the checkout |
| `bash setup-pi.sh` | **DESTROYED** — it re-copies from the repo and the provisioner overwrites |

So the one dangerous sequence is: style it, pull a change to
`fridge-overview.json`, re-run `setup-pi.sh`.

### Export before you pull

```bash
bash export-dashboard.sh
```

Fetches the live dashboard from Grafana's API and writes it back over
`dashboards/fridge-overview.json`. Commit that, and the repo and the browser
agree again. It strips Grafana's internal `id` and `version` fields, which are
meaningless in a file and would otherwise make every export show a diff.

Then, on whichever machine holds the repo:

```bash
python3 tests/test_fridged.py       # from pi4-software/
```

That re-runs every panel query against the schema and checks the export kept
`"uid": "fridge-overview"` and each panel's `datasource.uid` of `fridge-sqlite`
— the two fields whose loss would silently break the dashboard on a fresh
install.

### Avoiding the collision entirely

Once you start styling, **you own `fridge-overview.json`.** Later stages add
their panels as separate files or as JSON to paste in, rather than editing that
one — because a three-way merge of a 10,000-line generated JSON file is not
something anybody should have to do.

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
| *Could not find config defaults, make sure homepath command line parameter is set* | `grafana cli` cannot locate the server config on its own. It needs `--homepath /usr/share/grafana --config /etc/grafana/grafana.ini --pluginsDir /var/lib/grafana/plugins`; `setup-pi.sh` passes all three. Check with `ls /var/lib/grafana/plugins`, not with `plugins ls` — that subcommand hits the same problem |
| *Datasource not found* on every panel | The plugin is missing or unreadable. `ls -l /var/lib/grafana/plugins/frser-sqlite-datasource` — if it exists but is owned by `root`, `sudo chown -R grafana:grafana /var/lib/grafana/plugins` and restart. A root-owned plugin directory looks exactly like no plugin at all |
| *attempt to write a readonly database* | Permissions on `/var/lib/fridge`. `ls -l` must show group `grafana` and `rw` for the group **on the `-shm` and `-wal` files too**, not only on `fridge.db`. Re-running `setup-pi.sh` fixes an existing directory |
| `fridged` gets that error but Grafana is fine (or vice versa) | The `-shm` was created by whichever process opened the database first and is owned by that user. Both need to be in the `grafana` group — `id -nG` must list it. `setup-pi.sh` adds you, but **group membership only applies to a new login session**, so log out and back in |
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
