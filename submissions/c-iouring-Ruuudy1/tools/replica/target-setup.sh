#!/usr/bin/env bash
# Turns a fresh Ubuntu 24.04 droplet into the benchmark target, as root, from a checkout of the challenge repo that
# contains this submission:
#   bash submissions/c-iouring-Ruuudy1/tools/replica/target-setup.sh
# Runs box/setup-box.sh (Postgres 18 with the box's pgtune values, sysctls, the challenge user and unit), builds the
# full seed as conduit_seed, installs and builds the submission as user challenge, installs the search scripts,
# and serves directly on port 80 (Nginx stopped: Front: direct).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
SUB="$REPO/submissions/c-iouring-Ruuudy1"
bash "$REPO/box/setup-box.sh"
# Go for seedgen (the version the examples pin)
bash "$REPO/examples/go/install.sh"
export PATH=/usr/local/go/bin:$PATH
chmod -R a+rX "$REPO"
chown -R postgres "$REPO/seed" # make-seed.sh writes its generator and data files there
runuser -u postgres -- env PATH="$PATH" HOME=/var/lib/postgresql bash -c "cd '$REPO' && bash seed/make-seed.sh"
bash "$SUB/install.sh"
rm -rf /opt/challenge/c-iouring-Ruuudy1
cp -r "$SUB" /opt/challenge/c-iouring-Ruuudy1
chown -R challenge:challenge /opt/challenge/c-iouring-Ruuudy1
runuser -u challenge -- bash /opt/challenge/c-iouring-Ruuudy1/build.sh
ln -sfn /opt/challenge/c-iouring-Ruuudy1 /opt/challenge/current
cat > /etc/challenge/env <<'ENV'
DATABASE_URL=postgres://conduit:conduit@127.0.0.1:5432/conduit
JWT_SECRET=multicore-server-challenge
HOST=0.0.0.0
PORT=80
ENV
chmod 640 /etc/challenge/env && chgrp challenge /etc/challenge/env
systemctl disable --now nginx || true
install -d /opt/conduit-tools /root/conduit-monitor
install -m 755 "$SUB/tools/replica/switch.sh" "$SUB/tools/replica/monitor.sh" /opt/conduit-tools/
/opt/conduit-tools/switch.sh c-iouring-Ruuudy1
echo "target ready: $(curl -s http://127.0.0.1/api/tags | head -c 80)..."
cat /root/versions.txt
