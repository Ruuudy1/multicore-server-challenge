#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT. The defaults of the tuning
# variables (README.md) are the tuned values, so nothing else needs to be set.
set -euo pipefail
exec "$(cd "$(dirname "$0")" && pwd)/bin/conduit"
