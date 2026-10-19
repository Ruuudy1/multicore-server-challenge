#!/usr/bin/env bash
# Fuzzes the request parsers with libFuzzer + ASan + UBSan (needs clang).
#   bash tests/fuzz.sh [seconds, default 60]
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tests/bin tests/corpus
SECS="${1:-60}"
clang -std=gnu2x -O1 -g -march=native -pthread -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
  -o tests/bin/fuzz tests/fuzz.c src/util.c src/sha256.c src/json.c src/jwt.c src/http.c -lm
# seeds: requests and bodies like the load generator's
printf 'GET /api/articles?limit=10&offset=0&tag=a+b HTTP/1.1\r\nHost: x\r\nAuthorization: Token eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxIiwiaWF0IjoxLCJleHAiOjE4MDAwMDAwMDB9.x\r\n\r\n' > tests/corpus/get
printf 'POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n0\r\n\r\n' > tests/corpus/chunked
printf '{"article":{"title":"T \\u00e9","description":"d","body":"b\\n","tagList":["x","y"]}}' > tests/corpus/article
./tests/bin/fuzz -max_total_time="$SECS" -max_len=4096 -print_final_stats=1 tests/corpus 2>&1 | tail -15
