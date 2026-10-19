#!/usr/bin/env bash
# The maintainer's search (`loadgen search`: warm-up, doubling probes, bisection, 5-minute holds) on this one machine,
# with the loadgen's own local switches: the server runs as a systemd unit named challenge.service (so
# tools/replica/monitor.sh splits the CPU as on the box), Postgres and the server share CPUs 0-7, the load
# generator runs on the rest, and think times are scaled by 0.1 (the search's testing switch). Needs root.
#   bash tools/localsearch.sh <challenge repo> [database, default bench_aged] [think scale, default 0.1]
set -euo pipefail
ROOT="$(cd "${1:?usage: tools/localsearch.sh <challenge repo> [database] [think scale]}" && pwd)"
DB="${2:-bench_aged}"
THINK="${3:-0.1}"
SUB="$(cd "$(dirname "$0")/.." && pwd)"
LG="${LG:-/tmp/conduit-loadgen}"
[ -x "$LG" ] || (cd "$ROOT/bench" && go build -o "$LG" ./cmd/loadgen)
OUT="${OUT:-/root/results/local-search-$(date +%Y%m%d-%H%M)}"
systemctl stop challenge 2>/dev/null || true
systemctl reset-failed challenge 2>/dev/null || true
systemctl set-property --runtime postgresql@18-main.service AllowedCPUs=0-7
systemd-run --unit=challenge --property=AllowedCPUs=0-7 --property=LimitNOFILE=200000 --uid="${RUN_AS:-${SUDO_USER:-root}}" \
  --setenv=DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" --setenv=JWT_SECRET=multicore-server-challenge \
  --setenv=HOST=127.0.0.1 --setenv=PORT=3110 "$SUB/bin/conduit"
for _ in $(seq 60); do curl -sf -o /dev/null http://127.0.0.1:3110/api/tags && break; sleep 1; done
mkdir -p /root/conduit-monitor "$OUT"
ulimit -n 1048576
taskset -c 8-"$(($(nproc) - 1))" "$LG" search -target-ssh local -target-url http://127.0.0.1:3110 -lang c-iouring-Ruuudy1 \
  -no-switch -monitor "$SUB/tools/replica/monitor.sh" -think-scale "$THINK" -articles 500000 \
  -start "${START:-2000}" -max "${MAX:-40000}" -outdir "$OUT" -secret multicore-server-challenge || true
echo "postgres connections lost during the search: $(journalctl -u challenge --no-pager | grep -c "lost:" || true)"
systemctl stop challenge
cat "$OUT/SUMMARY.txt"
