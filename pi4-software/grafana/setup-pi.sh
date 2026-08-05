#!/usr/bin/env bash
#
# Wire this repo's Grafana configuration into a Grafana installed from the
# apt.grafana.com package on Raspberry Pi OS.
#
#   ./setup-pi.sh
#
# Run it as your normal login user, NOT with sudo — it needs to know who you are
# so the database directory ends up owned by you, and it calls sudo itself for
# the few steps that need root.
#
# SAFE TO RE-RUN. That is the intended way to push a dashboard change: edit the
# JSON in this repo, run this again, and Grafana picks it up within 30 seconds
# without a restart.
#
# What it does, all of which is reversible:
#   1. installs the SQLite datasource plugin
#   2. creates /var/lib/fridge, owned by you with group `grafana`
#   3. copies provisioning/ into /etc/grafana/provisioning/
#   4. copies dashboards/ into /var/lib/grafana/dashboards/
#   5. turns on anonymous viewing via /etc/default/grafana-server
#   6. restarts grafana-server

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB_DIR="/var/lib/fridge"
DASH_DIR="/var/lib/grafana/dashboards"
PROV_DIR="/etc/grafana/provisioning"
DEFAULTS="/etc/default/grafana-server"
PLUGIN="frser-sqlite-datasource"

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

if [[ "${EUID}" -eq 0 ]]; then
    echo "Run this as your login user, not with sudo." >&2
    echo "It calls sudo itself where root is actually needed." >&2
    exit 1
fi

if ! command -v grafana-cli >/dev/null 2>&1; then
    echo "grafana-cli not found - is Grafana installed from apt.grafana.com?" >&2
    exit 1
fi

# --- 1. The plugin ----------------------------------------------------------
# SQLite is not a core Grafana datasource. Without this every panel reports
# "Datasource not found", which looks like a provisioning fault and is not one.

say "Installing the SQLite datasource plugin"
if sudo grafana-cli plugins ls 2>/dev/null | grep -q "${PLUGIN}"; then
    echo "    already installed"
else
    sudo grafana-cli plugins install "${PLUGIN}"
fi

# --- 2. The database directory ----------------------------------------------
# Not in a home directory. Grafana runs as `grafana` and a WAL database CANNOT
# be opened read-only — a reader takes a lock in the -shm wal-index, so it needs
# write access to that file and to the directory holding it. Granting that
# inside /home would mean opening up the home directory's traversal bits, whose
# default mode varies between Raspberry Pi OS releases.
#
# Owned by you (fridged writes it), group `grafana` (Grafana reads it), setgid
# so files created here inherit the group instead of yours.

say "Creating ${DB_DIR}"
sudo mkdir -p "${DB_DIR}"
sudo chown "${USER}:grafana" "${DB_DIR}"
sudo chmod 2775 "${DB_DIR}"
ls -ld "${DB_DIR}"

if [[ -f "${DB_DIR}/fridge.db" ]]; then
    # Fix up a database created before the group was set. Harmless otherwise.
    sudo chgrp -R grafana "${DB_DIR}"
    sudo chmod -R g+rw "${DB_DIR}"
fi

# --- 3. Provisioning --------------------------------------------------------

say "Installing provisioning files into ${PROV_DIR}"
sudo mkdir -p "${PROV_DIR}/datasources" "${PROV_DIR}/dashboards"
sudo cp "${HERE}/provisioning/datasources/sqlite.yml" "${PROV_DIR}/datasources/"
sudo cp "${HERE}/provisioning/dashboards/fridge.yml" "${PROV_DIR}/dashboards/"

# --- 4. Dashboards ----------------------------------------------------------
# Copied, not symlinked: a symlink into the checkout would need `grafana` to
# have execute permission on every directory from / down to it, including your
# home directory.

say "Copying dashboards into ${DASH_DIR}"
sudo mkdir -p "${DASH_DIR}"
sudo cp "${HERE}"/dashboards/*.json "${DASH_DIR}/"
sudo chown -R grafana:grafana "${DASH_DIR}"
ls -l "${DASH_DIR}"

# --- 5. Anonymous viewing ---------------------------------------------------
# dashboard.md section 2: local network only, so there is no login to manage and
# no account for a committee member to lose.
#
# Set through the environment file rather than by editing /etc/grafana/grafana.ini,
# because that file is owned by the package: editing it makes every `apt upgrade`
# stop and ask about a modified config file.

say "Enabling anonymous viewing"
if [[ ! -f "${DEFAULTS}" ]]; then
    echo "    ${DEFAULTS} does not exist - skipping."
    echo "    Set [auth.anonymous] enabled=true in /etc/grafana/grafana.ini by hand."
else
    for setting in \
        "GF_AUTH_ANONYMOUS_ENABLED=true" \
        "GF_AUTH_ANONYMOUS_ORG_ROLE=Viewer"
    do
        key="${setting%%=*}"
        if sudo grep -q "^${key}=" "${DEFAULTS}"; then
            echo "    ${key} already set"
        else
            echo "${setting}" | sudo tee -a "${DEFAULTS}" >/dev/null
            echo "    added ${setting}"
        fi
    done
fi

# --- 6. Restart -------------------------------------------------------------

say "Restarting grafana-server"
sudo systemctl restart grafana-server
sleep 3
systemctl is-active --quiet grafana-server \
    && echo "    running" \
    || { echo "    NOT running - sudo journalctl -u grafana-server -n 40"; exit 1; }

# --- Done -------------------------------------------------------------------

cat <<EOF

$(say "Done")
Grafana:   http://$(hostname -I | awk '{print $1}'):3000
Dashboard: Fridge - Overview

The database must live at ${DB_DIR}/fridge.db for the datasource to find it.
Point fridged at it with FRIDGE_DB, from the pi4-software directory:

    export FRIDGE_DB=${DB_DIR}/fridge.db
    python3 -m fridged.seed --days 14
    python3 -m fridged --port sim

Then name the three sensors - the tiles stay empty until you do, because the
board only ever reports ROM codes:

    sqlite3 ${DB_DIR}/fridge.db \\
      "UPDATE sensor SET zone_label='freezer' WHERE rom_code='28FF...';"

Valid labels: freezer, fridge_top, fridge_bottom
EOF
