#!/usr/bin/env bash
# Build spec M8: the server built with ASan + UBSan holds 150,000 keep-alive connections under load for 30 minutes.
# Three load generators of 50,000 users each target 127.0.0.1, .2 and .3 (one source address only has ~64k ports
# toward one destination); think times are stretched x6 so the sanitizer build (~3x slower) keeps up. Needs root.
#   bash tests/soak.sh <challenge repo> [minutes, default 30] [users per generator, default 50000]
# A user connects on its first request (think x6: up to 7 minutes in), so a short run sees under 100% connected.
set -euo pipefail
ROOT="$(cd "${1:?usage: tests/soak.sh <challenge repo> [minutes] [users per generator]}" && pwd)"
MIN="${2:-30}"
PER="${3:-50000}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
LG="${LG:-/tmp/conduit-loadgen}"
[ -x "$LG" ] || (cd "$ROOT/bench" && go build -buildvcs=false -o "$LG" ./cmd/loadgen)
USER_="${RUN_AS:-${SUDO_USER:-root}}"
DB="soak_$$"
OUT="${OUT:-/root/results/soak-$(date +%Y%m%d-%H%M)}"
mkdir -p "$OUT"
PSQL="runuser -u postgres -- psql -X -q -v ON_ERROR_STOP=1 -d postgres"
$PSQL -c "CREATE DATABASE $DB TEMPLATE conduit_dev STRATEGY FILE_COPY"
runuser -u "$USER_" -- gcc -std=gnu2x -O1 -g -march=native -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -fno-omit-frame-pointer -pthread -Wall -Wextra -Wno-unused-parameter -o "$HERE/bin/conduit-asan" "$HERE"/src/*.c -l:liburing.a -lm
systemctl stop soak 2>/dev/null || true
systemctl reset-failed soak 2>/dev/null || true
systemd-run --unit=soak --property=LimitNOFILE=200000 --uid="$USER_" \
  --setenv=ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 --setenv=UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  --setenv=DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" --setenv=JWT_SECRET=multicore-server-challenge \
  --setenv=HOST=0.0.0.0 --setenv=PORT=3170 --setenv=STATS=1 "$HERE/bin/conduit-asan"
for _ in $(seq 60); do curl -sf -o /dev/null http://127.0.0.1:3170/api/tags && break; sleep 1; done
ulimit -n 1048576
start=$(( $(date +%s%3N) + 5000 ))
pids=""
for i in 0 1 2; do
  "$LG" run -target "http://127.0.0.$((i + 1)):3170" -users "$PER" -box-index "$i" -id-offset $((i * PER)) \
    -ramp 180s -measure "${MIN}m" -tail 10s -start-at "$start" -think-scale 6 -articles 10000 \
    -secret multicore-server-challenge -run-id "soak$$" -out "$OUT/box$i.json" -q > "$OUT/box$i.log" 2>&1 &
  pids="$pids $!"
done
peak=0
while kill -0 $pids 2>/dev/null; do
  n=$(ss -Htn state established "( sport = :3170 )" | wc -l)
  [ "$n" -gt "$peak" ] && peak=$n
  sleep 30
done
wait $pids || true
alive=no; systemctl is-active -q soak && alive=yes
rss=$(ps -o rss= -p "$(systemctl show -p MainPID --value soak)" 2>/dev/null || echo 0)
"$LG" merge -o "$OUT/merged.json" "$OUT"/box*.json > "$OUT/merged.txt" 2>&1 || true
reports=$(journalctl -u soak --no-pager | grep -cE "AddressSanitizer|runtime error|LeakSanitizer" || true)
systemctl stop soak 2>/dev/null || true
$PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)"
grep -E "^ALL|verdict" "$OUT/merged.txt" || true
echo "peak established connections: $peak; server alive at the end: $alive; RSS $((rss / 1024)) MB; sanitizer reports: $reports"
[ "$alive" = yes ] && [ "$reports" = 0 ] && [ "$peak" -ge $((3 * PER * 90 / 100)) ] && echo PASS || { echo FAIL; exit 1; }
