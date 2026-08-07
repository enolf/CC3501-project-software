#!/usr/bin/env bash
#
# Empty the dashboard so a demonstration populates it live.
#
#   bash reset-demo.sh
#
# Run as your normal login user, NOT with sudo. It calls sudo itself for the
# two systemctl calls and nothing else — see the warning below about what
# touching this database as root does to it.
#
# WHAT IT DELETES: every row of history. Measurements, transactions, door
# events, coin events, stock snapshots, raw lines, boots.
#
# WHAT IT KEEPS, deliberately:
#
#   sensor  ROM code -> which shelf it is on. These names were mapped by hand
#           by warming one sensor and seeing which row moved. Lose them and the
#           temperature panels show 16-digit ROM codes for the whole demo.
#   member  card UID -> whose card it is. Lose them and every sale is
#           attributed to a raw hex UID instead of a person.
#
# Both are configuration that happens to live in the same file as the history,
# which is exactly why `rm fridge.db` is the wrong tool even though it looks
# like the obvious one.
#
# A backup is taken first and is NOT optional. The run you are about to erase
# is also the evidence that the thing worked.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="${FRIDGE_DB:-/var/lib/fridge/fridge.db}"

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

if [[ "${EUID}" -eq 0 ]]; then
    # Not a style preference. SQLite in WAL mode creates fridge.db-wal and
    # fridge.db-shm beside the database, inheriting the caller's ownership. Run
    # as root and those come out root-owned, and Grafana — which must WRITE the
    # wal-index even just to read — is locked out of its own datasource. The
    # dashboard then fails with a permission error that points at Grafana and
    # has nothing to do with it.
    echo "Run this as your login user, not with sudo." >&2
    echo "Root-owned -wal/-shm files lock Grafana out of the database." >&2
    exit 1
fi

if [[ ! -f "${DB}" ]]; then
    echo "${DB} does not exist. Nothing to reset." >&2
    exit 1
fi

# --- 1. Stop the writer ------------------------------------------------------

say "Stopping fridged"
sudo systemctl stop fridged
echo "    stopped"

# --- 2. Back up, and refuse to continue if that fails ------------------------

say "Backing up first"
bash "${HERE}/backup.sh"

# --- 3. Show what is about to go --------------------------------------------

say "Currently in the database"
sqlite3 -header -column "${DB}" "
  SELECT 'measurement' AS table_name, COUNT(*) AS rows FROM measurement
  UNION ALL SELECT 'txn',            COUNT(*) FROM txn
  UNION ALL SELECT 'door_event',     COUNT(*) FROM door_event
  UNION ALL SELECT 'stock_snapshot', COUNT(*) FROM stock_snapshot
  UNION ALL SELECT 'raw_line',       COUNT(*) FROM raw_line
  UNION ALL SELECT '-- keeping --',  NULL
  UNION ALL SELECT 'sensor',         COUNT(*) FROM sensor
  UNION ALL SELECT 'member',         COUNT(*) FROM member;"

# --- 4. Delete the history ---------------------------------------------------
#
# Children before parents. fridged runs with PRAGMA foreign_keys=ON and the
# sqlite3 CLI does not, so this order is not strictly required today — but a
# delete script that only works because a pragma happens to be off is a trap
# for whoever runs it next.
#
# sqlite_sequence is what makes boot_id restart at 1 rather than continuing from
# 35, which matters only for how the demo reads, and reads much better.

say "Clearing history"
sqlite3 "${DB}" <<'SQL'
BEGIN;
DELETE FROM txn_item;
DELETE FROM coin_event;
DELETE FROM txn;
DELETE FROM rfid_event;
DELETE FROM door_event;
DELETE FROM stock_snapshot;
DELETE FROM cash_count;
DELETE FROM measurement;
DELETE FROM raw_line;
DELETE FROM boot;
DELETE FROM sqlite_sequence WHERE name = 'boot';
COMMIT;
SQL

# VACUUM cannot run inside a transaction, and it is what actually returns the
# space and folds the WAL back in. Without it the file stays its old size and
# the -wal is still full of the rows just deleted.
sqlite3 "${DB}" "VACUUM;"
echo "    cleared and vacuumed"

# --- 5. Prove the configuration survived -------------------------------------

say "Kept"
sqlite3 -header -column "${DB}" \
    "SELECT rom_code, zone_label FROM sensor ORDER BY zone_label;"
sqlite3 -header -column "${DB}" \
    "SELECT uid, label, active FROM member ORDER BY label;"

say "Integrity"
sqlite3 "${DB}" "PRAGMA integrity_check;" | sed 's/^/    /'

# --- 6. Permissions ----------------------------------------------------------
#
# Checked rather than assumed. VACUUM rewrites the database file, and a rewrite
# is exactly the moment a mode or a group can quietly change.

say "Permissions (group must be grafana, and group-writable)"
ls -l "${DB}"* | sed 's/^/    /'

# --- 7. Back on ---------------------------------------------------------------

say "Starting fridged"
sudo systemctl start fridged
sleep 8

if systemctl is-active --quiet fridged; then
    echo "    running"
else
    echo "    FAILED to start:" >&2
    sudo journalctl -u fridged -n 30 --no-pager >&2
    exit 1
fi

cat <<EOF

$(say "Empty and recording")

  In Grafana, before anyone is watching:

    1. Time range   ->  Last 15 minutes      (NOT the default 24h — on a 24h
                                              axis a live demo is one dot in
                                              the far right corner)
    2. Refresh      ->  10s                  (already the dashboard default)

  Then leave it on screen. It fills as the fridge is used.

  Expect the first scan after this restart to be low confidence — the camera
  has a frame or two and cannot settle yet. "Scans nobody should trust" will
  read 1, not 0, and that is correct.

  Backups are in /var/lib/fridge/backups if you need today's data back.
EOF
