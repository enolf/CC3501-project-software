#!/usr/bin/env bash
#
# Sample CPU, memory and temperature while the fridge software runs.
#
#   bash measure-load.sh [seconds]        default 300
#
# WHY THIS EXISTS
# ---------------
# documentation.md section 7.3 lists "Grafana + OpenCV on one Pi 4 is not free" as a
# hazard and says to measure it early rather than discovering it during the
# demo. This is that measurement. It also produces a table worth putting in the
# report, which is a better reason to run it than fear.
#
# Run it while the dashboard is open in a browser and, once picapture is wired
# up, while the camera is running — the interesting number is the three of them
# together, not any one alone.

set -euo pipefail

DURATION="${1:-300}"
INTERVAL=5
SAMPLES=$((DURATION / INTERVAL))

printf 'sampling every %ss for %ss (%s samples)\n\n' \
    "${INTERVAL}" "${DURATION}" "${SAMPLES}"
printf '%8s  %6s %8s  %6s %8s  %6s %8s  %7s %7s %6s\n' \
    'elapsed' 'graf%' 'grafMB' 'frid%' 'fridMB' 'pic%' 'picMB' \
    'load1' 'freeMB' 'degC'

usage() {   # $1 = process pattern; prints "cpu% rssMB", or "- -" if absent
    ps -eo pcpu,rss,comm,args --no-headers 2>/dev/null \
        | awk -v pat="$1" '
            $0 ~ pat { cpu += $1; rss += $2 }
            END { if (rss > 0) printf "%.1f %.0f", cpu, rss / 1024;
                  else printf "0.0 0" }'
}

peak_g=0; peak_f=0; peak_p=0
for ((i = 0; i < SAMPLES; i++)); do
    read -r gc gm <<<"$(usage 'grafana')"
    read -r fc fm <<<"$(usage 'fridged')"
    read -r pc pm <<<"$(usage 'picapture')"
    load="$(awk '{print $1}' /proc/loadavg)"
    freemb="$(awk '/MemAvailable/ {printf "%.0f", $2 / 1024}' /proc/meminfo)"
    if [[ -r /sys/class/thermal/thermal_zone0/temp ]]; then
        degc="$(awk '{printf "%.1f", $1 / 1000}' /sys/class/thermal/thermal_zone0/temp)"
    else
        degc="-"
    fi

    printf '%8s  %6s %8s  %6s %8s  %6s %8s  %7s %7s %6s\n' \
        "$((i * INTERVAL))s" "$gc" "$gm" "$fc" "$fm" "$pc" "$pm" \
        "$load" "$freemb" "$degc"

    awk -v a="$gm" -v b="$peak_g" 'BEGIN{exit !(a>b)}' && peak_g=$gm
    awk -v a="$fm" -v b="$peak_f" 'BEGIN{exit !(a>b)}' && peak_f=$fm
    awk -v a="$pm" -v b="$peak_p" 'BEGIN{exit !(a>b)}' && peak_p=$pm
    sleep "${INTERVAL}"
done

total="$(awk '/MemTotal/ {printf "%.0f", $2 / 1024}' /proc/meminfo)"
cat <<EOF

peak resident memory
  grafana    ${peak_g} MB
  fridged    ${peak_f} MB
  picapture  ${peak_p} MB
  total RAM  ${total} MB

Above 80 C the Pi throttles, and a throttled Pi during a demo looks like a
software fault. If the numbers are uncomfortable, the cheapest wins are raising
fridged's FLUSH_INTERVAL_S and lowering Grafana's dashboard refresh from 30s.
EOF
