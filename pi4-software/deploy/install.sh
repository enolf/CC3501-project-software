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

# --- 0. The devices the service will need ------------------------------------
# fridged.service names `dialout` and `video` in SupplementaryGroups=. Checked
# here rather than discovered at boot, because the failure is a bare EACCES
# inside a five-second restart loop and reads like a flaky cable.
#
# A warning, not an error: --port sim with a simulated shelf needs neither, and
# refusing to install would be wrong for that entirely valid setup.

check_device_group() {
    local want="$1" path
    for path in "${@:2}"; do
        [[ -e "${path}" ]] || continue
        local owner
        owner="$(stat -c '%G' "${path}")"
        if [[ "${owner}" != "${want}" ]]; then
            echo "    WARNING: ${path} is group '${owner}', not '${want}'." >&2
            echo "             Add '${owner}' to SupplementaryGroups= in" >&2
            echo "             ${UNIT_DIR}/fridged.service or the service" >&2
            echo "             cannot open it." >&2
        else
            # Said out loud even though nothing is wrong. An all-clear that
            # prints NOTHING is indistinguishable from a check that did not run,
            # and this section exists precisely to be reassuring before a
            # deployment.
            printf '    ok       %-24s %s\n' "${path}" "${owner}"
        fi
        return 0
    done
    printf '    absent   %-24s (no %s device plugged in yet)\n' "${2}" "${want}"
}

say "Devices"
check_device_group dialout /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0
check_device_group video   /dev/video0 /dev/media0
# The one people forget: libcamera allocates through dma_heap, and a Pi that can
# open /dev/video0 but not this fails later, during streaming, not at open.
check_device_group video   /dev/dma_heap/vc4 /dev/dma_heap/linux,cma

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
  1. the board       --port sim     -> --port /dev/serial/by-id/...
  2. the camera      simulated      -> --camera picapture
  3. card payments   --square fake  -> --square real

  Use the by-id path, not /dev/ttyACM0 — the number moves when the board is
  reset. See ${DEFAULTS} and \`ls -l /dev/serial/by-id/\`.
EOF
