#!/usr/bin/env bash
# Builds bin/conduit as a normal user: one gcc call over src/*.c, with liburing linked statically. No downloads.
# It is built on the box, so -march=native targets the box's CPU.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O3 -march=native -flto=auto -fno-plt -fno-semantic-interposition}"
OUT="${OUT:-bin/conduit}"
DEFS="" LIBS=""
# GZIP=1 builds experiment E9 (gzip of large bodies, needs libdeflate-dev); the submission's default build is without it.
if [ "${GZIP:-0}" = 1 ]; then DEFS="-DWITH_GZIP"; LIBS="-l:libdeflate.a"; fi
# shellcheck disable=SC2086
"$CC" -std=gnu2x $CFLAGS $DEFS -pthread -Wall -Wextra -Wno-unused-parameter -o "$OUT" src/*.c -l:liburing.a $LIBS -lm
echo "built $OUT ($("$CC" --version | head -1))"
