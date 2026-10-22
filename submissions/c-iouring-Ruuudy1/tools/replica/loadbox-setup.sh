#!/usr/bin/env bash
# Prepares a load box (fresh Ubuntu 24.04), as root, from a checkout of the challenge repo: the box sysctls, a high
# nofile limit, and Go (the first load box builds the loadgen and drives the others over ssh).
#   bash submissions/c-iouring-Ruuudy1/tools/replica/loadbox-setup.sh
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
install -m 644 "$REPO/box/sysctl.conf" /etc/sysctl.d/90-challenge.conf
sysctl -q --system
printf 'root soft nofile 1048576\nroot hard nofile 1048576\n* soft nofile 1048576\n* hard nofile 1048576\n' > /etc/security/limits.d/90-nofile.conf
apt-get update -q && apt-get install -y -q curl jq
bash "$REPO/examples/go/install.sh"
(cd "$REPO/bench" && /usr/local/go/bin/go build -o /usr/local/bin/conduit-loadgen ./cmd/loadgen)
echo "load box ready: $(nproc) vCPU, $(conduit-loadgen 2>&1 | head -1)"
