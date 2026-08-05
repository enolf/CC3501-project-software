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

# Where the Debian package puts things. The CLI does NOT infer these: run it
# from anywhere but the homepath and it dies with "Could not find config
# defaults, make sure homepath command line parameter is set or working
# directory is homepath" — before it has done anything at all.
GRAFANA_HOME="/usr/share/grafana"
GRAFANA_CONFIG="/etc/grafana/grafana.ini"
PLUGIN_DIR="/var/lib/grafana/plugins"

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

if [[ "${EUID}" -eq 0 ]]; then
    echo "Run this as your login user, not with sudo." >&2
    echo "It calls sudo itself where root is actually needed." >&2
    exit 1
fi

# Grafana 11 renamed `grafana-cli` to the `grafana cli` subcommand and now warns
# about the old form on every invocation. Both still work; prefer the new one so
# the output is not buried in a deprecation notice.
if command -v grafana >/dev/null 2>&1; then
    GRAFANA_CLI=(sudo grafana cli)
elif command -v grafana-cli >/dev/null 2>&1; then
    GRAFANA_CLI=(sudo grafana-cli)
else
    echo "Neither 'grafana' nor 'grafana-cli' found." >&2
    echo "Is Grafana installed from apt.grafana.com?" >&2
    exit 1
fi

# --- 1. The plugin ----------------------------------------------------------
# SQLite is not a core Grafana datasource. Without this every panel reports
# "Datasource not found", which looks like a provisioning fault and is not one.

say "Installing the SQLite datasource plugin"

# Presence is checked on the FILESYSTEM rather than with `plugins ls`, which has
# to load the server config and is the command most likely to fail with the
# homepath error. The directory either exists or it does not; nothing to
# initialise, nothing to go wrong.
if [[ -d "${PLUGIN_DIR}/${PLUGIN}" ]]; then
    echo "    already installed at ${PLUGIN_DIR}/${PLUGIN}"
else
    # All three flags are needed. --homepath alone gets past the init error but
    # then resolves the plugin directory to ${GRAFANA_HOME}/data/plugins, which
    # is not where the packaged server looks — so the install "succeeds" and the
    # plugin never loads.
    "${GRAFANA_CLI[@]}" \
        --homepath "${GRAFANA_HOME}" \
        --config "${GRAFANA_CONFIG}" \
        --pluginsDir "${PLUGIN_DIR}" \
        plugins install "${PLUGIN}"
fi

# Installed under sudo, so the files land owned by root and the grafana user
# cannot read them. The symptom is identical to the plugin not being installed
# at all, which makes it a nuisance to diagnose.
sudo chown -R grafana:grafana "${PLUGIN_DIR}"

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

# --- 6. grafana-server's umask ----------------------------------------------

say "Setting grafana-server's umask"
DROPIN_DIR="/etc/systemd/system/grafana-server.service.d"
sudo mkdir -p "${DROPIN_DIR}"
sudo tee "${DROPIN_DIR}/umask.conf" >/dev/null <<'EOF'
# Written by pi4-software/grafana/setup-pi.sh — safe to delete, see that script.
#
# fridged and Grafana both write the SQLite -wal and -shm files beside
# fridge.db, as different users sharing the `grafana` group. Whichever process
# creates one of those files decides its mode, and SQLite asks for the database
# file's mode masked by the creating process's umask.
#
# So BOTH ends need a umask that keeps the group-write bit. With systemd's
# default 0022 Grafana would create a 0644 -shm that fridged cannot write, or
# fridged would create one Grafana cannot write — and WHICH of those happens
# depends on start order. An intermittent, order-dependent permissions failure
# is the worst kind to chase, so it is closed at both ends rather than one.
#
# A drop-in rather than an edit to the packaged unit file, so `apt upgrade`
# never has to ask about a locally modified service.
[Service]
UMask=0002
EOF
sudo systemctl daemon-reload
echo "    ${DROPIN_DIR}/umask.conf"

# --- 7. Restart -------------------------------------------------------------

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
