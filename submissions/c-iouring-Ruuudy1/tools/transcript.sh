#!/usr/bin/env bash
# Runs the conformance suite with -transcript on a fresh copy of the dev seed and diffs the normalized
# transcript with the Go reference's (bench/cmd/conformance/golden-dev.jsonl): byte-identical means every
# status and every JSON body matches the reference, not just the 197 checks.
#   bash tools/transcript.sh <challenge repo checkout>
set -euo pipefail
ROOT="$(cd "${1:?usage: tools/transcript.sh <challenge repo>}" && pwd)"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DB="transcript_$$"
PORT="${PORT:-3999}"
TMP="$(mktemp -d)"
PSQL="psql -X -q -v ON_ERROR_STOP=1 -d postgres"
$PSQL -c "CREATE DATABASE $DB TEMPLATE conduit_dev STRATEGY FILE_COPY"
PID=""
cleanup() {
  [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null || true
  $PSQL -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" || true
  rm -rf "$TMP"
}
trap cleanup EXIT
(cd "$ROOT/bench" && go build -o "$TMP/conformance" ./cmd/conformance)
DATABASE_URL="postgres://conduit:conduit@127.0.0.1:5432/$DB" JWT_SECRET=multicore-server-challenge HOST=127.0.0.1 PORT="$PORT" \
  "${BIN:-$HERE/bin/conduit}" > "$TMP/server.log" 2>&1 &
PID=$!
for _ in $(seq 60); do curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/tags" && break; sleep 0.5; done
"$TMP/conformance" -base "http://127.0.0.1:$PORT" -secret multicore-server-challenge -q -transcript "$TMP/transcript.jsonl" | tail -2
if diff "$ROOT/bench/cmd/conformance/golden-dev.jsonl" "$TMP/transcript.jsonl" > "$TMP/diff.txt"; then
  echo "transcript: identical to golden-dev.jsonl ($(wc -l < "$TMP/transcript.jsonl") steps)"
else
  echo "transcript: differs from golden-dev.jsonl"
  head -40 "$TMP/diff.txt"
  exit 1
fi
