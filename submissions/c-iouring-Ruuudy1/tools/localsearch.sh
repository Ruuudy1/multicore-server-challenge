#!/usr/bin/env bash
# The maintainer's search (`loadgen search`: warm-up, doubling probes, bisection, 5-minute holds) on this one machine,
# with the loadgen's own local switches: the server runs its start.sh as a systemd unit named challenge.service, as on
# the box (so tools/replica/monitor.sh splits the CPU the same way), Postgres and the server share CPUs 0-7, the load
# generator runs on the rest, and think times are scaled by 0.1 (the search's testing switch). Needs root.
#   bash tools/localsearch.sh <challenge repo> [database, default bench_aged] [think scale, default 0.1]
# Any submission: SUBMISSION=<its folder> LABEL=<name>; it must be built first (its build.sh). RUN_AS=<user> runs it
# as that user (default: the sudo user, else root); START and MAX bound the probes.
set -euo pipefail
ROOT="$(cd "${1:?usage: tools/localsearch.sh <challenge repo> [database] [think scale]}" && pwd)"
DB="${2:-bench_aged}"
THINK="${3:-0.1}"
SUB="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="$(cd "${SUBMISSION:-$SUB}" && pwd)"
LABEL="${LABEL:-$(basename "$TARGET")}"
USER_="${RUN_AS:-${SUDO_USER:-root}}"
HOME_="$(getent passwd "$USER_" | cut -d: -f6)"
LG="${LG:-/tmp/conduit-loadgen}"
[ -x "$LG" ] || (cd "$ROOT/bench" && go build -buildvcs=false -o "$LG" ./cmd/loadgen)
OUT="${OUT:-/root/results/local-search-$LABEL-$(date +%Y%m%d-%H%M)}"
systemctl stop challenge 2>/dev/null || true
systemctl reset-failed challenge 2>/dev/null || true
systemctl set-property --runtime postgresql@18-main.service AllowedCPUs=0-7
# like box/challenge.service: start.sh in the submission's folder, its HOME, LimitNOFILE=200000; plus the CPU split
# SERVER_ENV="KEY=VALUE ..." adds settings for experiments (the box sets only the four contract variables)
extra=()
for kv in ${SERVER_ENV:-}; do extra+=("--setenv=$kv"); done
systemd-run --unit=challenge --property=AllowedCPUs=0-7 --property=LimitNOFILE=200000 --uid="$USER_" \
  --working-directory="$TARGET" --setenv=HOME="$HOME_" "${extra[@]}" \
  --setenv=DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" --setenv=JWT_SECRET=multicore-server-challenge \
  --setenv=HOST=127.0.0.1 --setenv=PORT=3110 /bin/bash "$TARGET/start.sh"
for _ in $(seq 60); do curl -sf -o /dev/null http://127.0.0.1:3110/api/tags && break; sleep 1; done
curl -sf -o /dev/null http://127.0.0.1:3110/api/tags || { echo "GET /api/tags not 200 after 60 s"; journalctl -u challenge -n 20 --no-pager; exit 1; }
mkdir -p /root/conduit-monitor "$OUT"
ulimit -n 1048576
taskset -c 8-"$(($(getconf _NPROCESSORS_ONLN) - 1))" "$LG" search -target-ssh local -target-url http://127.0.0.1:3110 \
  -lang "$LABEL" -no-switch -monitor "$SUB/tools/replica/monitor.sh" -think-scale "$THINK" -articles 500000 \
  -start "${START:-2000}" -max "${MAX:-40000}" -outdir "$OUT" -secret multicore-server-challenge || true
echo "postgres connections lost during the search: $(journalctl -u challenge --no-pager | grep -c "lost:" || true)"
systemctl stop challenge
cat "$OUT/SUMMARY.txt"
