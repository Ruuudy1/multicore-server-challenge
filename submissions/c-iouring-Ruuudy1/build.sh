#!/usr/bin/env bash
# Builds bin/conduit as a normal user: one gcc call over src/*.c, with liburing linked statically. No downloads.
# It is built on the box, so -march=native targets the box's CPU.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O3 -march=native -flto=auto -fno-plt -fno-semantic-interposition}"
# shellcheck disable=SC2086
"$CC" -std=gnu2x $CFLAGS -pthread -Wall -Wextra -Wno-unused-parameter -o bin/conduit src/*.c -l:liburing.a -lm
echo "built bin/conduit ($("$CC" --version | head -1))"
