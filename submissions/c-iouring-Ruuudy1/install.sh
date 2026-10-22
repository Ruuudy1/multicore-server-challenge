#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: GCC 13 (build-essential) and liburing 2.5 from the Ubuntu archive.
# liburing is pinned to the noble version; everything else (HTTP, JSON, JWT, SHA-256, bcrypt, the Postgres
# protocol) is in src/.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q build-essential pkg-config liburing-dev=2.5-1build1 liburing2=2.5-1build1
gcc --version | head -1
dpkg-query -W liburing-dev
