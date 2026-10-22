#!/usr/bin/env bash
# /opt/conduit-tools/switch.sh <build>: what `loadgen search` runs on the target before a search. Points the
# service at /opt/challenge/<build>, recreates the database from the seed, drops caches, restarts Postgres and
# the service, and waits until GET /api/tags answers (MULTICORE_BUILD_SPEC.md section 10.2).
set -euo pipefail
ln -sfn "/opt/challenge/$1" /opt/challenge/current
systemctl stop challenge || true
runuser -u postgres -- psql -qX -c "DROP DATABASE IF EXISTS conduit WITH (FORCE)" \
  -c "CREATE DATABASE conduit TEMPLATE conduit_seed STRATEGY FILE_COPY"
sync && echo 3 > /proc/sys/vm/drop_caches
systemctl restart postgresql@18-main && systemctl start challenge
. /etc/challenge/env
for _ in $(seq 60); do curl -sf -o /dev/null "http://127.0.0.1:${PORT:-80}/api/tags" && exit 0; sleep 1; done
journalctl -u challenge -n 30 --no-pager
exit 1
