#!/usr/bin/env bash
# Differential read test against the Go reference (examples/go): both servers on one fresh database copy.
#   bash tools/diffcheck.sh <challenge repo checkout> [template database, default conduit_dev] [requests, default 2500]
set -euo pipefail
ROOT="$(cd "${1:?usage: tools/diffcheck.sh <challenge repo> [template] [n]}" && pwd)"
TEMPLATE="${2:-conduit_dev}"
N="${3:-2500}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DB="diffcheck_$$"
SECRET=multicore-server-challenge
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d postgres"
TMP="$(mktemp -d)"
$PSQL -c "CREATE DATABASE $DB TEMPLATE $TEMPLATE STRATEGY FILE_COPY"
PIDS=""
cleanup() {
  for p in $PIDS; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  $PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" || true
  rm -rf "$TMP"
}
trap cleanup EXIT
bash "$ROOT/examples/go/build.sh" > /dev/null
URL="postgres://conduit:conduit@127.0.0.1:5432/$DB"
DATABASE_URL="$URL" JWT_SECRET=$SECRET HOST=127.0.0.1 PORT=3101 DB_POOL_SIZE=8 "$ROOT/examples/go/bin/conduit" > "$TMP/go.log" 2>&1 &
PIDS="$PIDS $!"
# TEST_BIN, TEST_ENV ("KEY=VALUE ...") and DIFF_ARGS (e.g. "--threads 32 --gzip") select experiment variants.
# shellcheck disable=SC2086
env DATABASE_URL="$URL" JWT_SECRET=$SECRET HOST=127.0.0.1 PORT=3102 ${TEST_ENV:-} "${TEST_BIN:-$HERE/bin/conduit}" > "$TMP/c.log" 2>&1 &
PIDS="$PIDS $!"
for port in 3101 3102; do
  for _ in $(seq 120); do curl -sf -o /dev/null "http://127.0.0.1:$port/api/tags" && break; sleep 0.5; done
done
users=$(psql -X -At -d "$DB" -c "SELECT max(id) FROM users")
# shellcheck disable=SC2086
python3 "$HERE/tools/diffcheck.py" --ref http://127.0.0.1:3101 --test http://127.0.0.1:3102 --secret $SECRET --n "$N" --users "$users" ${DIFF_ARGS:-}
grep -E "failed|lost|ERROR" "$TMP/c.log" | head -5 || true
grep -o "[0-9]* grouped" "$TMP/c.log" | awk '{s += $1} END {if (s) print s " requests answered by grouped executions"}' || true
