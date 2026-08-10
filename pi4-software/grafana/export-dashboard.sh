#!/usr/bin/env bash
#
# Pull the LIVE dashboard out of Grafana and write it back into this repo.
#
#   bash export-dashboard.sh                    # default uid, fridge-overview
#   bash export-dashboard.sh fridge-overview
#
# The counterpart to setup-pi.sh, and the thing that makes styling in the
# browser safe.
#
# WHY THIS EXISTS
# ---------------
# `allowUiUpdates: true` lets you save design changes from the Grafana UI, and
# they persist in Grafana's own database. What they do NOT survive is
# setup-pi.sh being run again: that re-copies the JSON from this repo, the
# provisioner sees a changed file, and it overwrites the database copy. Every
# tweak made in the browser and never exported is lost at that moment.
#
# So the rule is: EXPORT BEFORE YOU PULL. Run this, commit the result, and the
# repo and the browser agree again.
#
# Uses python3 rather than jq, which is not installed on Raspberry Pi OS by
# default and would be one more thing to discover the hard way.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UID_ARG="${1:-fridge-overview}"
GRAFANA_URL="${GRAFANA_URL:-http://localhost:3000}"
GRAFANA_USER="${GRAFANA_USER:-admin}"
GRAFANA_PASSWORD="${GRAFANA_ADMIN_PASSWORD:-admin}"
OUT="${HERE}/dashboards/${UID_ARG}.json"

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is not installed:  sudo apt install curl" >&2
    exit 1
fi

echo "==> Fetching ${UID_ARG} from ${GRAFANA_URL}"

# Written to a temporary file rather than a shell variable, because the Python
# below takes its SCRIPT on stdin via a heredoc — so the JSON has to arrive by
# some other route, and a path argument is the least surprising one.
RESPONSE_FILE="$(mktemp)"
trap 'rm -f "${RESPONSE_FILE}"' EXIT

# Anonymous access is Viewer-only and cannot read the dashboard API, so this
# needs the admin credentials even though looking at the dashboard does not.
curl -sS --fail-with-body -o "${RESPONSE_FILE}" \
    -u "${GRAFANA_USER}:${GRAFANA_PASSWORD}" \
    "${GRAFANA_URL}/api/dashboards/uid/${UID_ARG}" || {
        echo >&2
        echo "Fetch failed. Common causes:" >&2
        echo "  * wrong password - export GRAFANA_ADMIN_PASSWORD=... first" >&2
        echo "  * wrong uid      - check the dashboard URL in the browser" >&2
        echo "  * Grafana down   - systemctl status grafana-server" >&2
        exit 1
    }

python3 - "${RESPONSE_FILE}" "${OUT}" <<'PYTHON'
import json
import sys

response_path, out_path = sys.argv[1], sys.argv[2]
with open(response_path, encoding="utf-8") as handle:
    payload = json.load(handle)
dashboard = payload["dashboard"]

# `id` is Grafana's internal primary key for ITS database. A provisioned file
# carrying one can collide with an existing row on another install, so it is
# stripped: `uid` is the identity that matters and is preserved.
dashboard.pop("id", None)

# `version` counts saves in Grafana's database and means nothing in a file. Left
# in, every export would show a diff even when the panels are identical, which
# buries the changes that matter in noise.
dashboard.pop("version", None)

with open(out_path, "w", encoding="utf-8") as handle:
    json.dump(dashboard, handle, indent=2, ensure_ascii=False)
    handle.write("\n")

panels = dashboard.get("panels", [])
print(f"    {len(panels)} panels, uid={dashboard.get('uid')}")
print(f"    written to {out_path}")
PYTHON

cat <<EOF

==> Done

Now, on the machine holding the repo:

    python3 tests/test_fridged.py     (from pi4-software/ - checks every
                                       panel query still runs, and that the
                                       export kept the datasource uids)

then commit ${OUT#"${HERE}/"}.
EOF
