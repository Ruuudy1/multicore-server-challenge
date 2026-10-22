#!/usr/bin/env bash
# The maintainer's search, from the first load box (root, with ssh keys to the target and the other load boxes):
#   bash run-search.sh <target private ip> [other load box ips...]
# Results land in results/c-iouring-Ruuudy1-<time>/ (SUMMARY.txt has the score). The search starts at 500 users and
# doubles; -start 8000 skips the early doublings for a server that is known to be fast.
set -euo pipefail
TARGET="${1:?usage: run-search.sh <target ip> [load box ips...]}"
shift
BOXES="local=1"
for b in "$@"; do BOXES="$BOXES,root@$b=1"; done
ulimit -n 1048576
export CONDUIT_JWT_SECRET=multicore-server-challenge
OUT="results/c-iouring-Ruuudy1-$(date +%Y%m%d-%H%M)"
conduit-loadgen search -target-ssh "root@$TARGET" -target-url "http://$TARGET" -lang c-iouring-Ruuudy1 \
  -boxes "$BOXES" -start "${START:-8000}" -outdir "$OUT"
cat "$OUT/SUMMARY.txt"
