#!/usr/bin/env bash
# Unit tests for the pure modules (no network, no Postgres): optimized, then with ASan + UBSan.
#   bash tests/run.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/bin
SRC="src/util.c src/sha256.c src/json.c src/jwt.c src/bcrypt.c src/http.c tests/test_unit.c"
gcc -std=gnu2x -O2 -march=native -pthread -Wall -Wextra -Wno-unused-parameter -o tests/bin/unit $SRC -lm
./tests/bin/unit
gcc -std=gnu2x -O1 -g -march=native -pthread -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -o tests/bin/unit-asan $SRC -lm
./tests/bin/unit-asan > /dev/null && echo "sanitizers: clean"
