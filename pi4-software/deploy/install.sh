#!/usr/bin/env bash
#
# Install fridged and its nightly backup as systemd services.
#
#   bash install.sh
#
# Run as your normal login user, NOT with sudo — it needs to know who you are,
# because the service runs as you and the database must stay writable by both
# you and Grafana. It calls sudo itself where root is needed.
#
# SAFE TO RE-RUN. Re-running after a `git pull` reinstalls the units and
# restarts the service, which is the intended way to deploy a change.
#
# Assumes grafana/setup-pi.sh has already run: that is what creates
# /var/lib/fridge with the right ownership and puts you in the grafana group.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="$(cd "${HERE}/.." && pwd)"      # pi4-software/
UNIT_DIR="/etc/systemd/system"
DEFAULTS="/etc/default/fridged"
DB_DIR="/var/lib/fridge"

say() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

if [[ "${EUID}" -eq 0 ]]; then
    echo "Run this as your login user, not with sudo." >&2
    exit 1
fi

if [[ ! -d "${DB_DIR}" ]]; then
    echo "${DB_DIR} does not exist. Run grafana/setup-pi.sh first — it creates" >&2
    echo "the directory with the ownership Grafana needs." >&2
    exit 1
fi
if ! id -nG "${USER}" | tr ' ' '\n' | grep -qx grafana; then
    echo "${USER} is not in the grafana group. Run grafana/setup-pi.sh, then" >&2
    echo "LOG OUT AND BACK IN before running this." >&2
    exit 1
fi

# --- 1. Runtime configuration -----------------------------------------------
# Never overwritten. Once it exists it holds local choices — which port, which
# Square backend — and clobbering those on every deploy would be its own bug.

say "Runtime configuration"
if [[ -f "${DEFAULTS}" ]]; then
    echo "    ${DEFAULTS} already exists, leaving it alone"
    echo "    (edit it to change --port or --square, then restart the service)"
else
    sudo cp "${HERE}/fridged.default" "${DEFAULTS}"
    echo "    wrote ${DEFAULTS}"
fi

# --- 2. The units ------------------------------------------------------------
# __USER__ and __WORKDIR__ are substituted rather than hardcoded, so the same
# files work on a Pi whose account is not `pi` and a checkout that is not in the
# expected place.

say "Installing units into ${UNIT_DIR}"
for unit in fridged.service fridge-backup.service fridge-backup.timer; do
    sed -e "s|__USER__|${USER}|g" -e "s|__WORKDIR__|${WORKDIR}|g" \
        "${HERE}/${unit}" | sudo tee "${UNIT_DIR}/${unit}" >/dev/null
    echo "    ${unit}"
done
sudo systemctl daemon-reload

# --- 3. Start ----------------------------------------------------------------

say "Enabling and starting"
sudo systemctl enable --now fridged.service
sudo systemctl enable --now fridge-backup.timer
sleep 3

if systemctl is-active --quiet fridged; then
    echo "    fridged is running"
else
    echo "    fridged FAILED to start:" >&2
    sudo journalctl -u fridged -n 30 --no-pager >&2
    exit 1
fi
systemctl list-timers fridge-backup.timer --no-pager | sed 's/^/    /'

# --- Done --------------------------------------------------------------------

ADDRESS="$(hostname -I | awk '{print $1}')"
cat <<EOF

$(say "Done")
  Service   sudo systemctl status fridged
  Logs      journalctl -u fridged -f
  Change    sudoedit ${DEFAULTS}   then   sudo systemctl restart fridged
  Backups   ${DB_DIR}/backups     (nightly 03:30, 14 kept)
            bash ${HERE}/backup.sh     to take one now

  Grafana   http://${ADDRESS}:3000
            http://$(hostname).local:3000   (mDNS - try it, Raspberry Pi OS
                                             ships avahi and it usually just
                                             works)

fridged now starts at boot and restarts if it crashes. It is no longer tied to
your terminal, so close the tmux session if you were using one.

NEXT, one at a time, checking each before the next (deploy/README.md):
  1. the board       --port sim     -> --port /dev/ttyACM0
  2. card payments   --square fake  -> --square real
  3. the camera      simulated      -> picapture, once it can run headless
EOF
