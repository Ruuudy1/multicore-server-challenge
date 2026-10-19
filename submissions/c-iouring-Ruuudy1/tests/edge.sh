#!/usr/bin/env bash
# Starts the server on a fresh copy of the dev seed and runs the HTTP edge-case tests against it.
#   bash tests/edge.sh [binary, default bin/conduit]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${1:-bin/conduit}"
DB="edge_$$"
PORT="${PORT:-3120}"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d postgres"
$PSQL -c "CREATE DATABASE $DB TEMPLATE conduit_dev STRATEGY FILE_COPY"
LOG="$(mktemp)"
DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" JWT_SECRET=multicore-server-challenge HOST=127.0.0.1 PORT="$PORT" \
  "$BIN" > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; $PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)"; rm -f "$LOG"' EXIT
for _ in $(seq 60); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/tags" && break; sleep 0.5; done
rc=0
python3 tests/http_edge.py 127.0.0.1 "$PORT" || rc=$?
kill -0 $PID 2>/dev/null || { echo "server died:"; tail -20 "$LOG"; exit 1; }
exit $rc
