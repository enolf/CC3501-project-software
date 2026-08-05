#!/usr/bin/env bash
#
# Nightly backup of the fridge database. Run by fridge-backup.timer.
#
#   bash backup.sh [destination-directory]
#
# WHY NOT `cp`
# ------------
# dashboard.md section 13 says the database is one file so a backup is a `cp`.
# That was true before WAL. It is not true now: the live database is THREE files
# — fridge.db, fridge.db-wal and fridge.db-shm — and `cp` of the first while
# fridged is writing captures a database missing every commit still sitting in
# the write-ahead log, or worse, a torn page.
#
# `sqlite3 .backup` uses SQLite's online backup API, which takes a consistent
# snapshot of a database that is being written to, and produces a single
# self-contained file. It is the difference between a backup and a file that
# looks like one.

set -euo pipefail

DB="${FRIDGE_DB:-/var/lib/fridge/fridge.db}"
DEST="${1:-/var/lib/fridge/backups}"

#: How many nightly backups to keep. Two weeks of a database that is a few tens
#: of megabytes is nothing, and the point of a backup is having an older one.
KEEP=14

if [[ ! -f "${DB}" ]]; then
    echo "no database at ${DB}" >&2
    exit 1
fi
if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "sqlite3 is not installed:  sudo apt install sqlite3" >&2
    exit 1
fi

mkdir -p "${DEST}"
stamp="$(date +%Y%m%d-%H%M%S)"
out="${DEST}/fridge-${stamp}.db"

# Written to a temporary name and moved into place, so a backup interrupted
# half-way never leaves a truncated file wearing a name that says it is good.
sqlite3 "${DB}" ".backup '${out}.partial'"
mv "${out}.partial" "${out}"
gzip -f "${out}"

echo "backed up to ${out}.gz ($(du -h "${out}.gz" | cut -f1))"

# Prune. `ls -t` newest first, keep the first KEEP, delete the rest.
mapfile -t old < <(ls -1t "${DEST}"/fridge-*.db.gz 2>/dev/null | tail -n "+$((KEEP + 1))")
if [[ ${#old[@]} -gt 0 ]]; then
    rm -f "${old[@]}"
    echo "pruned ${#old[@]} old backup(s), keeping ${KEEP}"
fi

# A backup nobody has ever opened is a hope, not a backup. Decompressing it and
# counting a table costs a second and turns "the file exists" into "the file is
# a database that opens".
check="$(mktemp)"
trap 'rm -f "${check}"' EXIT
gzip -dc "${out}.gz" > "${check}"
rows="$(sqlite3 "${check}" 'SELECT COUNT(*) FROM measurement')"
integrity="$(sqlite3 "${check}" 'PRAGMA integrity_check')"
if [[ "${integrity}" != "ok" ]]; then
    echo "BACKUP FAILED ITS INTEGRITY CHECK: ${integrity}" >&2
    exit 1
fi
echo "verified: opens cleanly, ${rows} measurement rows"
