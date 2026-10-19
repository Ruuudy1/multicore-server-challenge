#!/usr/bin/env bash
# CPU profile under load: runs tools/localbench.py and samples the server and the Postgres backends with perf
# (cpu-clock) during the measured window. Needs perf and kernel.perf_event_paranoid <= 1.
#   bash tools/profile.sh <challenge repo> [users, default 9000] [extra localbench args...]
set -euo pipefail
ROOT="${1:?usage: tools/profile.sh <challenge repo> [users]}"
USERS="${2:-9000}"
shift 2 || true
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PERF="${PERF:-$(ls -d /usr/lib/linux-tools/*/perf 2>/dev/null | grep -v microsoft | head -1)}"
OUT="${OUT:-/tmp/profile}"
mkdir -p "$OUT"
python3 "$HERE/tools/localbench.py" --repo "$ROOT" --server c --users "$USERS" --warmup 20 --ramp 20 --measure 40 --label profile "$@" &
BENCH=$!
# the measured window starts after the database copy, the warm-up (10 s ramp + 20 s + 5 s tail) and the 20 s ramp
for _ in $(seq 120); do pgrep -f "bin/conduit" > /dev/null && break; sleep 1; done
sleep 62
APP=$(pgrep -f "c-iouring-Ruuudy1/bin/conduit" | head -1)
PG=$(pgrep -d, -f "postgres: 18/main: conduit bench_")
"$PERF" record -e cpu-clock -F 1999 -g -p "$APP" -o "$OUT/app.data" -- sleep 25 > /dev/null 2>&1 &
${SUDO:-} "$PERF" record -e cpu-clock -F 499 -g -p "$PG" -o "$OUT/pg.data" -- sleep 25 > /dev/null 2>&1 &
wait $BENCH
sleep 2
"$PERF" report -i "$OUT/app.data" --no-children --sort symbol --stdio 2>/dev/null | grep -v "^#" | grep -v "^$" | head -60 > "$OUT/app.txt"
${SUDO:-} "$PERF" report -i "$OUT/pg.data" --no-children --sort symbol --stdio 2>/dev/null | grep -v "^#" | grep -v "^$" | head -70 > "$OUT/pg.txt"
echo "== server"; grep -E "^ +[0-9]" "$OUT/app.txt" | head -40
echo "== postgres"; grep -E "^ +[0-9]" "$OUT/pg.txt" | head -50
