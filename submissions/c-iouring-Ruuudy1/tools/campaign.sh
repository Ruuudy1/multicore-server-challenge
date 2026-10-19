#!/usr/bin/env bash
# The A/B campaign for the build spec's experiments (section 8), on one aged database: each comparison is ABBA
# pairs at a fixed load just under saturation, analyzed with tools/abba.py. Stops when the disk gets low.
#   bash tools/campaign.sh <challenge repo> <database> [pairs, default 3] [users, default 9000]
set -euo pipefail
ROOT="${1:?usage: tools/campaign.sh <challenge repo> <database> [pairs] [users]}"
DB="${2:?database}"
PAIRS="${3:-3}"
USERS="${4:-9000}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
ALL() { for k in GLOBAL TAG AUTHOR FAVORITED FEED; do printf -- "--env LIST_SHAPE_%s=%s " "$k" "$1"; done; }

run() { # label, then localbench arguments
  local label="$1"; shift
  local free_kb
  free_kb=$(df -k /mnt/c 2>/dev/null | awk 'NR == 2 {print $4}')
  if [ -n "$free_kb" ] && [ "$free_kb" -lt 1000000 ]; then echo "STOP: under 1 GB free on C:"; exit 3; fi
  # shellcheck disable=SC2068
  python3 tools/localbench.py --repo "$ROOT" --db "$DB" --users "$USERS" --warmup 15 --ramp 20 --measure 40 --label "$label" $@ \
    | grep '^{' || echo "run $label failed"
}

compare() { # name, A label, A args, B label, B args
  local name="$1" al="$2" aa="$3" bl="$4" ba="$5"
  echo "== $name"
  for _ in $(seq "$PAIRS"); do
    # shellcheck disable=SC2086
    run "$al" $aa; run "$bl" $ba; run "$bl" $ba; run "$al" $aa
  done
  python3 tools/abba.py "$al" "$bl"
}

GZ="--server cmd --cmd $HERE/bin/conduit-gzip"
for exp in ${EXPERIMENTS:-e2 e10 e1 e7 e8 e9 loops conns depth batch}; do
  case $exp in
    e2) compare "E2 batched point reads" "e2 off" "--server c" "e2 on" "--server c --env E2_BATCH=1" ;;
    e10) compare "E10 feed author dedup" "e10 v1" "--server c" "e10 v1d" "--server c --env LIST_SHAPE_FEED=v1d" ;;
    e1) compare "E1 v2 lists (all kinds)" "e1 v1" "--server c" "e1 v2" "--server c $(ALL v2)" ;;
    e7) compare "E7 v2 counts in C" "e7 v2" "--server c $(ALL v2)" "e7 v2c" "--server c $(ALL v2c)" ;;
    e8) compare "E8 v2 tag names by key probes" "e8 v2" "--server c $(ALL v2)" "e8 v2t" "--server c $(ALL v2t)" ;;
    e9) compare "E9 gzip over 4 KB" "e9 off" "$GZ" "e9 on" "$GZ --env GZIP_MIN=4096" ;;
    loops) compare "E5 LOOPS" "loops 2" "--server c" "loops 1" "--server c --env LOOPS=1" ;;
    conns) compare "E5 PG_READ_CONNS" "conns 8" "--server c" "conns 12" "--server c --env PG_READ_CONNS=12" ;;
    depth) compare "E5 PG_DEPTH" "depth 2" "--server c" "depth 4" "--server c --env PG_DEPTH=4" ;;
    batch) compare "E5 READ_BATCH" "batch 24" "--server c" "batch 12" "--server c --env READ_BATCH=12" ;;
  esac
done
