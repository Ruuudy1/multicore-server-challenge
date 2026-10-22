#!/usr/bin/env bash
# Runs tools/localsearch.sh for several submissions back to back, each from a fresh copy of the full seed, and prints
# one line per submission. Needs root; every submission must already be built (its install.sh and build.sh).
#   bash tools/compare.sh <challenge repo> <label>=<submission folder>[@KEY=VALUE,KEY=VALUE] ...
# The optional @... part sets experiment variables for that run (passed as SERVER_ENV to tools/localsearch.sh).
# Bracketing the list with the same submission first and last shows how much the machine drifted meanwhile.
set -euo pipefail
ROOT="${1:?usage: tools/compare.sh <challenge repo> <label>=<folder> ...}"
shift
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="runuser -u postgres -- psql -X -q -v ON_ERROR_STOP=1 -d postgres"
i=0
for spec in "$@"; do
  i=$((i + 1))
  label="${spec%%=*}" rest="${spec#*=}" db="compare_$i"
  folder="${rest%%@*}" env=""
  [ "$rest" != "$folder" ] && env="${rest#*@}" && env="${env//,/ }"
  $PSQL -c "DROP DATABASE IF EXISTS $db WITH (FORCE)" -c "CREATE DATABASE $db TEMPLATE conduit_seed STRATEGY FILE_COPY"
  OUT="/root/results/compare/$i-$label" SUBMISSION="$folder" LABEL="$label" START="${START:-4000}" MAX="${MAX:-40000}" \
    SERVER_ENV="$env" \
    bash "$HERE/localsearch.sh" "$ROOT" "$db" "${THINK:-0.1}" > "/root/results/compare-$i-$label.log" 2>&1 || true
  $PSQL -c "DROP DATABASE IF EXISTS $db WITH (FORCE)"
  echo "$i $label: $(grep -m1 -E "^SCORE|no_pass|LG-limited" "/root/results/compare/$i-$label/SUMMARY.txt" 2>/dev/null || echo "no summary")"
  grep -m1 "^hold at" "/root/results/compare/$i-$label/SUMMARY.txt" 2>/dev/null || true
done
