#!/usr/bin/env bash
# Robustness under load: terminates the server's Postgres backends (readers and a writer) while the load
# generator runs, then checks the server survived, reconnected, and answered almost everything.
#   bash tests/killtest.sh [loadgen binary, default ~/loadgen]
set -euo pipefail
cd "$(dirname "$0")/.."
LG="${1:-$HOME/loadgen}"
DB="kill_$$"
PORT="${PORT:-3130}"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d postgres"
$PSQL -c "CREATE DATABASE $DB TEMPLATE conduit_dev STRATEGY FILE_COPY"
LOG="$(mktemp)"
DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" JWT_SECRET=multicore-server-challenge HOST=127.0.0.1 PORT="$PORT" \
  bin/conduit > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; $PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)"; rm -f "$LOG" /tmp/kill-$$.json' EXIT
for _ in $(seq 60); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/tags" && break; sleep 0.5; done
"$LG" run -target "http://127.0.0.1:$PORT" -users 1500 -ramp 5s -measure 40s -tail 3s -secret multicore-server-challenge \
  -articles 10000 -think-scale 0.1 -run-id kill$$ -out /tmp/kill-$$.json -q &
LGPID=$!
for round in 1 2 3; do
  sleep 10
  n=$(psql -X -At -d postgres -c "SELECT count(pg_terminate_backend(pid)) FROM (SELECT pid FROM pg_stat_activity WHERE datname = '$DB' AND application_name = 'conduit-c-iouring' ORDER BY random() LIMIT 3) s")
  echo "round $round: terminated $n backends"
done
wait $LGPID
kill -0 $PID || { echo "FAIL: server died"; tail -30 "$LOG"; exit 1; }
python3 - "$LOG" /tmp/kill-$$.json <<'PY'
import json, sys
log = open(sys.argv[1]).read()
lost, restored = log.count("lost:"), log.count("restored")
eps = json.load(open(sys.argv[2]))["endpoints"]
total = sum(e["count"] for e in eps.values()) if isinstance(eps, dict) else sum(e["count"] for e in eps)
statuses = {}
for e in (eps.values() if isinstance(eps, dict) else eps):
    for k, v in (e.get("statuses") or {}).items():
        statuses[k] = statuses.get(k, 0) + v
    for k, v in (e.get("errors") or {}).items():
        if not k.startswith("http_"):  # non-2xx statuses are already counted above
            statuses["err:" + k] = statuses.get("err:" + k, 0) + v
bad = sum(v for k, v in statuses.items() if not k.startswith("2") and not k.startswith("3"))
print(f"connections lost {lost}, restored {restored}; {total} requests, statuses {statuses}")
ok = lost == restored and lost > 0 and bad <= max(5, total // 1000)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
