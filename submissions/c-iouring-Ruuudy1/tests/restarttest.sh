#!/usr/bin/env bash
# Postgres restarted under load: every connection drops at once. The server must survive, reconnect all of them,
# and fail almost nothing (reads are asked again; only writes whose commit was in flight get a 503).
# The restart itself needs root; run this and, 15 s later, `systemctl restart postgresql@18-main` as root:
#   bash tests/restarttest.sh & sleep 15; sudo systemctl restart postgresql@18-main; wait
set -euo pipefail
cd "$(dirname "$0")/.."
LG="${LG:-$HOME/loadgen}"
DB="restart_$$"
PORT="${PORT:-3160}"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d postgres"
$PSQL -c "CREATE DATABASE $DB TEMPLATE conduit_dev STRATEGY FILE_COPY"
LOG="$(mktemp)"
DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" JWT_SECRET=multicore-server-challenge HOST=127.0.0.1 PORT="$PORT" \
  bin/conduit > "$LOG" 2>&1 &
PID=$!
cleanup() {
  kill $PID 2>/dev/null; wait $PID 2>/dev/null
  for _ in $(seq 30); do pg_isready -q && break; sleep 1; done
  $PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" || true
  rm -f "$LOG" "/tmp/restart-$$.json"
}
trap cleanup EXIT
for _ in $(seq 60); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/tags" && break; sleep 0.5; done
"$LG" run -target "http://127.0.0.1:$PORT" -users 1500 -ramp 5s -measure 40s -tail 3s -secret multicore-server-challenge \
  -articles 10000 -think-scale 0.1 -run-id restart$$ -out "/tmp/restart-$$.json" -q
sleep 3
kill -0 $PID || { echo "FAIL: server died"; tail -30 "$LOG"; exit 1; }
python3 - "$LOG" "/tmp/restart-$$.json" <<'PY'
import json, sys
log = open(sys.argv[1]).read()
lost, restored = log.count("lost:"), log.count("restored")
eps = json.load(open(sys.argv[2]))["endpoints"]
eps = eps.values() if isinstance(eps, dict) else eps
total, statuses = 0, {}
for e in eps:
    total += e["count"]
    for k, v in (e.get("statuses") or {}).items():
        statuses[k] = statuses.get(k, 0) + v
    for k, v in (e.get("errors") or {}).items():
        if not k.startswith("http_"):
            statuses["err:" + k] = statuses.get("err:" + k, 0) + v
bad = sum(v for k, v in statuses.items() if not k.startswith(("2", "3")))
print(f"connections lost {lost}, restored {restored}; {total} requests, statuses {statuses}")
ok = lost > 0 and lost == restored and bad <= max(10, total // 1000)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
