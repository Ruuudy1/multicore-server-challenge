#!/usr/bin/env bash
# Runs tests/groupfail.py against a fresh copy of the dev seed and shows the server's log lines about the group.
#   bash tests/groupfail.sh [binary, default bin/conduit]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${1:-bin/conduit}"
DB="groupfail_$$"
PORT="${PORT:-3150}"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d postgres"
$PSQL -c "CREATE DATABASE $DB TEMPLATE conduit_dev STRATEGY FILE_COPY"
LOG="$(mktemp)"
DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" JWT_SECRET=multicore-server-challenge HOST=127.0.0.1 PORT="$PORT" STATS=1 \
  "$BIN" > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; $PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)"; rm -f "$LOG"' EXIT
for _ in $(seq 60); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/tags" && break; sleep 0.5; done
rc=0
python3 tests/groupfail.py "$PORT" "$DB" 40 || rc=$?
sleep 1.5
grep -E "rolled back|failed" "$LOG" | sort | uniq -c | head -5
grep -o "writes [0-9]* in [0-9]* groups ([0-9.]*)" "$LOG" | grep -v "writes 0 " | head -3
kill -0 $PID 2>/dev/null || { echo "server died:"; tail -20 "$LOG"; exit 1; }
exit $rc
