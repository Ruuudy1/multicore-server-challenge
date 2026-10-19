# C + io_uring + a hand-written Postgres client

**Front: direct.** The server listens on `HOST:PORT` itself and holds every user's keep-alive connection.

| Piece | Version |
|---|---|
| Language | C (`-std=gnu2x`), GCC 13.3 from Ubuntu 24.04 (`build-essential`) |
| I/O | io_uring through liburing 2.5 (Ubuntu 24.04 `liburing-dev=2.5-1build1`, linked statically) |
| HTTP/1.1, JSON, JWT HS256, SHA-256 (SHA-NI), HMAC, PBKDF2, bcrypt | hand-written, in `src/` |
| Postgres driver | hand-written protocol v3 client (`src/pg.c`, `src/sched.c`): SCRAM-SHA-256, prepared statements, binary results, pipelined batches |

`install.sh` installs the compiler and liburing from the Ubuntu archive. `build.sh` is one `gcc` call. `start.sh`
runs `bin/conduit`, configured by `DATABASE_URL`, `JWT_SECRET`, `HOST` and `PORT` only.

## Design

Postgres uses about 70% of the box at the limit, so the score is mostly Postgres work per request. The server
tries to cost Postgres as little as possible, then to cost as little as possible itself.

### Event loops

- `LOOPS` (2) threads, each with its own io_uring (`SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN`, falling back
  when the kernel lacks them), its own `SO_REUSEPORT` listener, its own Postgres connections and its own queues.
  Nothing is shared on the request path.
- Multishot accept straight into the ring's registered file table (as large as `RLIMIT_NOFILE`, 200,000 slots per
  ring under the service unit); plain descriptors if registration fails.
- One multishot receive per connection with buffers from a provided-buffer ring (4,096 x 4 KiB per loop): an idle
  keep-alive connection owns no buffer, only its 64-byte record. If the kernel rejects buffer rings, the loop uses
  multishot poll plus `recv()`.
- Each response is one buffer with the header written right in front of the body, sent with one `IORING_OP_SEND`.
- Completion batching: after a pass with 16 or more completions the loop waits for 32 completions or 250 µs.
- Idle connections are never closed. Pipelined requests are answered in order. Headers are capped at 16 KiB and
  bodies at 1 MiB (Nginx's default).

### Postgres client and scheduler

- Every statement is parsed once per connection at startup and described; the server refuses to start if a
  result layout differs from what its decoders expect. Session settings go in the startup packet:
  `plan_cache_mode=force_generic_plan`, `jit=off`, `max_parallel_workers_per_gather=0`, `enable_bitmapscan=off`,
  `statement_timeout=9s`, `lock_timeout=5s`.
- Binary parameters and binary results: integers, booleans, `timestamptz` (formatted in C) and arrays arrive as
  compact binary values; C writes all the JSON.
- **Reads**: one FIFO per loop. A read connection with fewer than `PG_DEPTH` (2) batches in flight takes up to
  `READ_BATCH` (24) queued requests, encodes them back to back (Bind + Execute each) with one Sync, and sends
  them in one write. Each request is answered at its own CommandComplete, not at the end of the batch. If a
  statement fails, Postgres skips the rest of its batch; those requests go back to the head of the FIFO.
- **Writes**: group commit. While a group's commit is in flight, writes queue up; the next group (up to
  `WRITE_BATCH`, 128) is one implicit transaction with one Sync, and every response in it waits for
  ReadyForQuery, so writes are committed before the response. If anything in a group fails, the group rolled
  back and each write is retried alone. Sign-ups and user updates always run alone, because a unique violation
  there is an expected 422. Deadlocks and serialization failures are retried once alone; a write whose commit is
  uncertain (the connection dropped) is answered 503 and never retried.
- Connections that drop (or whose backend reports FATAL, e.g. when terminated) are re-established by a
  connector thread with backoff; their unanswered reads go back to the head of the FIFO.

### SQL

All SQL lives in `tools/sqlcheck/statements.py`; `tools/gen_sql.py` copies it verbatim into `src/sql_gen.h`.
`tools/sqlcheck/check.py` compares every statement with SPEC.md's reference SQL on random viewers, limits,
offsets and filters (plus write checks inside rolled-back transactions) and flags plans with bitmap scans or
sequential scans of big tables.

- **One round trip per request.** Look-ups and their actions travel together in one statement: a favorite toggle
  writes and returns the article (the count corrected by whether the viewer's own row was in the statement's
  snapshot); follow, comment, update, delete and create are each one statement.
- **Slug suffix as the primary key.** Every slug ends in `-<id>`, so article statements take the id from the slug
  and use `a.id = $2 AND a.slug = $3::text COLLATE "C"`, which forces the primary key (the collation makes the
  slug index unusable for that predicate) while the slug still has to match exactly.
- **Bounded pages.** Page ids come from `... OFFSET $3 LIMIT 100` with the outer `LIMIT $2` picking the page, so
  generic plans expect at most 100 rows instead of 10% of the table (which brings Memoize and hash joins).
- **Capped counts that can stop.** `EXISTS (... ORDER BY key OFFSET 999 LIMIT 1)` or
  `count(*) FROM (... ORDER BY key LIMIT 1000)` walk an ordered index from the oldest (frozen, all-visible) end
  and stop at the 1,000th entry; an unordered `LIMIT 1000` can get a bitmap scan that reads the whole key range.
- **Fenced per-row look-ups** (`LATERAL (... OFFSET 0)`), so generic plans never hash-join all of `users`.
- **No writes to `tags` on the hot path**: an article creation inserts only tag names that don't exist yet.
- Comments: the article check and the newest 100 comments in one statement; the header row carries the viewer's
  follow set and C computes each comment's `following`.
- **Lists as page facts (v2, the default):** one header row carries the page's facts, gathered with a few range
  (global list: every id between the page's min and max is on it) or `= ANY` scans: favorite counts, the viewer's
  favorites and follows, and tag names. Plain article rows follow, and C joins them. On an aged database this cut
  CPU per request 6.1% against per-row decoration (v1), which stays available with `LIST_SHAPE_<KIND>=v1`.

### bcrypt

- Cost 10 for every sign-up and password change, a full verify for every login, the key schedule of
  `golang.org/x/crypto/bcrypt` (what the reference uses).
- Several independent hashes run in lockstep in one thread so their S-box loads overlap. The width (3, 4 or 6:
  six keep the S-boxes inside a 32 KB L1) is calibrated at startup; on one core of the development machine a
  hash costs 51.6 ms of CPU one at a time and 18.0 ms with six in lockstep. A bit-exact self-test (Openwall's
  vectors, the seed's hash, hashes made by Go's bcrypt, and every width against the single-lane result) runs at
  startup; the server refuses to start on a mismatch.
- Two worker threads at nice 5 (`SCHED_BATCH`): the first waits up to 250 ms to fill a batch, the second only
  takes full batches or jobs older than 500 ms. Results come back to the owning loop with `IORING_OP_MSG_RING`.

### HTTP, JSON and JWT

- The request parser reads the method, target, `Authorization`, `Content-Length`, `Transfer-Encoding`,
  `Connection` and `Expect`; chunked bodies and `100 Continue` work. Routing follows Go 1.22's ServeMux as the
  reference uses it (GET patterns also answer HEAD, `/api/articles/feed` beats `{slug}`, everything unmatched is
  a JSON 404). Query strings follow `url.ParseQuery` (`+` is a space, pairs with `;` or bad escapes are dropped,
  the first value wins).
- The JSON body parser follows `encoding/json` as the reference uses it: the whole body must be valid, the last
  duplicate key wins, invalid UTF-8 and lone surrogates become U+FFFD.
- JWT: HS256 with precomputed HMAC midstates and SHA-NI, a fast path for the canonical header and the
  `{"sub","iat","exp"}` claims, and a general path that accepts exactly what golang-jwt v5 accepts
  (`exp` required, `nbf` checked, typed registered claims, `alg` must be `HS256`).

## Correctness

- `test/run.sh`: 197/197 on the dev seed and on the full seed.
- The conformance transcript (`tools/transcript.sh`) is byte-identical to the Go reference's
  `golden-dev.jsonl`: every status and every normalized body, not just the checks.
- `tools/diffcheck.sh`: 3,000 random read requests each on the dev and the full seed (all list filters, deep
  offsets, feeds, articles, comments, profiles, users, bad parameters and tokens) against the Go example on the
  same database: 0 differences.
- `tools/sqlcheck/check.py` on PostgreSQL 18.6: 1,891 checks and 0 mismatches each on the dev seed and on the
  edge-case dataset, 791 checks and 0 mismatches on the full seed (40 rounds), 0 plan warnings.
- `tests/run.sh`: 490 unit checks (SHA-256 against FIPS vectors with and without SHA-NI, HMAC against RFC 4231,
  PBKDF2 against RFC 7914, base64url, the JSON parser, JWT acceptance and rejection, bcrypt, timestamps, slugify,
  the HTTP parser), also under ASan and UBSan. `tests/edge.sh` (27 checks) covers pipelining, split requests,
  chunked bodies, `Expect: 100-continue`, HEAD, HTTP/1.0, `Connection: close` and malformed or oversized
  requests; `tests/fuzz.sh` ran 2.4 million libFuzzer inputs against the parsers without a finding.
- `tests/killtest.sh`: 9 of the server's backends terminated under load, 9 reconnected, 0 failed requests out of
  158,694. `tests/idle.py`: 100,000 keep-alive connections served and then held idle with 50.8 MB of server RSS.
- The whole server built with ASan and UBSan passes the conformance suite and two minutes of load (8,200 req/s)
  without a report. Both fallbacks (`NO_BUF_RING=1`, `NO_FIXED_FILES=1`) pass the conformance suite with an
  identical transcript and the edge cases.

## Measurements (development machine, not the benchmark box)

WSL2 Ubuntu 24.04 on an Intel Core Ultra 9 185H, PostgreSQL 18.6 with the box's pgtune values and the full seed
(a fresh copy per run). Postgres and the server share 8 vCPUs (cgroup cpuset and affinity); the load generator
runs on the other 14 with think times scaled by 0.1, over loopback. Each run: 30 s warm-up, 20 s ramp, 45 to 60 s
measured (`tools/localbench.py`). These numbers rank servers on one machine; the score comes from the box.

| Server | Users (x0.1) | req/s | p95 | p99 | Errors | CPU per request (server + Postgres) |
|---|---|---|---|---|---|---|
| this one | 9,000 | 24,998 | 13 ms | 23 ms | 0 | 284 µs (51 + 232) |
| this one | 12,000 | 27,786 | 139 ms | 162 ms | 0 | 288 µs (53 + 235) |
| this one | 16,000 | 24,337 | 419 ms | 452 ms | 0 | 327 µs (70 + 257); passes |
| this one | 19,000 | 25,196 | 532 ms | 584 ms | 0 | 315 µs (74 + 242); fails on p95 |
| PR #10 (C + io_uring) | 9,000 | 22,534 | 83 ms | 112 ms | 0 | 338 µs (78 + 260) |
| PR #10 (C + io_uring) | 12,000 | 20,804 | 194 ms | 413 ms | 0.69% | 360 µs |
| Go example | 5,000 | 9,337 | 293 ms | 381 ms | 0 | 859 µs (320 + 539) |

The single runs above are a sweep; the comparison that counts is interleaved. Three ABBA pairs against PR #10 at
9,000 users (`tools/abba.py`): **16.5% less CPU per request** (274.2 against 328.2 µs, 95% interval
[-17.2%, -16.1%]), about 20% more capacity on the same CPUs, with p95 10.3 against 64.4 ms and p99 18.9 against
91.2 ms. PR #10 ran with the settings it picks on an 8-vCPU box (`THREADS=2 DB_CONNS=32 BCRYPT_THREADS=4`). Past the knee,
throughput falls because logins grow with the number of users, not with throughput: bcrypt took exactly the
0.22 extra cores between 12,000 and 16,000 users.

Where the CPU goes (`tools/profile.sh`, perf at 9,000 users, about 25,000 req/s):

- Postgres is 82% of the CPU per request. In its backends, `ExecutorRun` is 70% (index scans 29%, per-row
  subplans 25%), executor startup 12%, snapshots 0.13%, transaction start 0.2%, socket I/O about 1%. Batching made
  the per-statement protocol and transaction costs negligible.
- By statement (`pg_stat_statements`, local only): lists are 73% of Postgres execution time (feed 34%, global 21%,
  tag 10%, author 6%, favorited 2%), comments 10.5%, single articles 7.4%, the tag list 4.4%, writes about 3%.
- The server is 18%: bcrypt about 40% of it, the kernel's TCP and io_uring work about 40%, this server's own
  code 10 to 12% (about 6 µs per request).
- bcrypt on one core: 51.6 ms per cost-10 hash one at a time, 18.0 ms with 6 in lockstep (`tests/bcrypt_bench.c`).

The table and the profile above predate v2 lists (they ran v1).

### The maintainer's search, on this machine

`tools/localsearch.sh` runs `loadgen search` as the maintainer does (3-minute warm-up, 2-minute probes doubling
then bisected to 5%, 5-minute holds) with its local testing switches: the server as `challenge.service`, Postgres
and the server on 8 CPUs, the load generator on the other 14, think times and sessions scaled by 0.1, from a fresh
copy of the full seed. Result (v2 lists, all defaults):

| Phase | Users (x0.1) | req/s | p50 | p95 | p99 | Errors |
|---|---|---|---|---|---|---|
| probe | 16,000 | 29,613 | 173 ms | 220 ms | 241 ms | 0 |
| probe | 20,000 | 27,954 | 353 ms | 401 ms | 427 ms | 0 |
| probe | 21,000 | 27,994 | 389 ms | 448 ms | 478 ms | 0 |
| probe | 22,000 | 27,122 | 450 ms | 533 ms (fail) | 568 ms | 0 |
| **hold, 5 minutes** | **21,000** | **27,241** | 411 ms | **476 ms** | 508 ms | **0 of 8.2 million** |

Not load-generator limited (load box CPU 47%, never behind). Over the hold the server used about 1.8 cores and
Postgres 6.3, the whole 8. By the closed-loop math of the build spec (users = req/s x (7 s + 1.86 calls x 0.41 s) /
1.86 calls), 27,241 req/s at this latency is about 113,000 users at the real 7-second think time. That is an
estimate for 8 threads of a laptop's Core Ultra 9 (hybrid cores, local loopback), not a score for the box: only
`tools/replica/` on the benchmark hardware gives one.

### Experiments (section 8 of the build spec)

`tools/campaign.sh` runs each comparison as two ABBA cycles (4 paired runs of each variant) at 9,000 users, on one
database that ages through the campaign (the scored hold runs on an aged database too). Aging is large: midway the
database had 34% more articles, 23% more comments and 13% more favorites than the seed, and the same v1 server went
from about 340 µs of CPU per request (during E1) to about 485 µs (the last sweep). `tools/abba.py` gives the paired change
and a bootstrap 95% interval. The rule: adopt only if CPU per request falls at least 3% with an interval that
excludes zero and p99 does not regress by more than 10%. All runs on AC power with nothing else on the machine.

| | Change (B against the default A) | CPU per request | p95 / p99 | Decision |
|---|---|---|---|---|
| E1 | v2 lists, all kinds | **-6.1%** [-9.2, -3.0] | -40% / -35% | **adopted**: v2 is the default |
| E2 | point reads grouped per statement (`E2_BATCH=1`) | +1.7% [-0.5, +3.9] | +7% / +11% | rejected |
| E7 | v2 favorite counts as raw ids counted in C (`v2c`) | -1.4% [-4.0, +0.7] | -8% / -1% | rejected |
| E8 | v2 tag names by key probes (`v2t`) | -1.8% [-3.3, -0.3] | -20% / -19% | below the bar; first to retry on the box |
| E9 | gzip of bodies over 4 KB (`GZIP=1` build) | +4.7% [+2.5, +7.7] | +21% / +18% | rejected (on loopback) |
| E10 | v1 with one `users` look-up per author (`v1d`) | +1.7% [-0.8, +5.1] | | rejected |
| E5 | `LOOPS` 2 → 1 | -2.4% [-4.9, +0.2] | -16% / -16% | below the bar; retry on the box |
| E5 | `PG_READ_CONNS` 8 → 12 | +6.5% [-0.1, +16.1] | +54% / +67% | rejected |
| E5 | `PG_DEPTH` 2 → 4 | -2.7% [-9.2, +3.2] | -3% / +1% | inconclusive; 2 stays |
| E5 | `READ_BATCH` 24 → 12 | +2.1% [-0.3, +4.9] | +5% / +5% | rejected |

Notes:

- E1 earlier showed no change on a fresh database (Postgres 237.5 against 239.2 µs per request in one run with
  `pg_stat_statements`); the gain appears as the database ages, which is where the score is measured.
- E2 matches PR #11's finding: grouping point reads saves per-statement overhead that batching had already made
  small, and `unnest` costs more than it saves. E9 ran over loopback, where sending is cheap; on the box's virtio
  NIC it could differ, and it is one build flag away.
- E2 and E10 ran before the others, partly on battery; the rest re-ran on AC power after the machine slept
  mid-campaign (the battery runs of E1 gave -7.4%, the same answer).
- **E3, one snapshot per batch:** dropped without a run, since snapshots are 0.13% of backend CPU
  (`READ_SNAPSHOT=batch` exists).
- **E6, bcrypt width:** calibrated over 3, 4 and 6 lanes. 8 lanes (34 KB of S-boxes) won on an idle core but not
  under load, when Postgres shares the core.
- **Fallbacks:** multishot poll + `recv()` (`NO_BUF_RING=1`) measured within noise of the buffer ring (25,206 req/s,
  50 µs of server CPU per request), so a kernel that rejects buffer rings costs nothing measurable.
- **E4, completion batching:** single runs at 12,000 users suggested `URING_WAIT_US=500` was 7% cheaper per
  request, but 4 ABBA pairs (`tools/abba.py`) put the change at -0.0%, 95% interval [-2.6%, +2.6%]: the single
  runs had ridden a drift of the machine itself (CPU per request fell from 302 to 252 µs over 25 minutes, whatever
  the setting). 250 µs stays.
- **Correctness of the variants:** every one keeps the conformance transcript identical to the reference's,
  `check.py` finds 0 mismatches in 4,949 checks, and E2 and E9 pass the differential test under 32 to 96 concurrent
  requests (E2 also under ASan and UBSan), so each stays switchable for A/B work on the box.

## Rules

- Every response is built from statements executed for that request; only connections, prepared statements,
  the HMAC key schedule and the bcrypt calibration persist. Identical concurrent reads are not coalesced.
- Writes are committed before the response (group commit answers at ReadyForQuery); an uncertain commit is never
  retried.
- bcrypt cost 10 every time; JWT signature and `exp` verified on every request; HS256 only.
- No schema, database or server-wide changes: only session settings in the startup packet. 10 Postgres
  connections by default (at most 95).
- No load-test detection, no shedding: no 503 at a queue threshold, nothing parked near the 10 s timeout.

## Tuning knobs

Only `DATABASE_URL`, `JWT_SECRET`, `HOST` and `PORT` are set on the box, so these defaults are the tuned values;
the variables exist for local sweeps.

| Variable | Default | Meaning |
|---|---|---|
| `LOOPS` | 2 | event-loop threads |
| `PG_READ_CONNS` | 8 | read connections in total, split over the loops |
| `PG_WRITE_CONNS` | 2 | write connections in total |
| `PG_DEPTH` | 2 | batches in flight per read connection |
| `READ_BATCH` | 24 | statements per read Sync |
| `WRITE_BATCH` | 128 | writes per group commit |
| `URING_BUSY_CQE` | 16 | completions in a pass that switch on batched waits |
| `URING_MIN_CQE` | 32 | completions to wait for when busy |
| `URING_WAIT_US` | 250 | longest batched wait |
| `BCRYPT_THREADS` | 2 | bcrypt workers |
| `BCRYPT_LANES` | auto | hashes in lockstep (calibrated at startup) |
| `BCRYPT_WAIT_MS` | 250 | how long a job may wait for its batch to fill |
| `BCRYPT_NICE` | 5 | nice level of the bcrypt workers |
| `LIST_SHAPE_GLOBAL`, `_TAG`, `_AUTHOR`, `_FAVORITED`, `_FEED` | v2 | list SQL shape: v1, v2, v1d (E10), v2c (E7), v2t (E8) |
| `E2_BATCH`, `E2_MIN` | 0, 2 | E2: group concurrent point reads of one statement (at least `E2_MIN` of them) |
| `READ_SNAPSHOT` | statement | `batch`: repeatable read on read connections (E3) |
| `GZIP_MIN`, `GZIP_LEVEL` | 0, 1 | E9: gzip bodies of at least this many bytes (only in a `GZIP=1` build) |
| `MAX_BODY` | 1048576 | request body limit |
| `STATS` | 0 | 1 prints one line per loop per second |

## Layout

```
src/main.c        startup, configuration, signals
src/loop.c        io_uring rings, accept, receive, send, completion batching, cross-thread posts
src/sched.c       read FIFO, batches, write groups, retries, the Postgres protocol on the loop
src/pg.c          connection setup: DATABASE_URL, startup, SCRAM-SHA-256, Parse + Describe
src/sql.c         the statement catalog (src/sql_gen.h is generated from tools/sqlcheck/statements.py)
src/http.c        request parser, chunked bodies, query parameters
src/api.c         routing, authentication, response framing
src/api_*.c       the endpoints
src/json.c        request JSON parser
src/jwt.c         HS256 verify and issue
src/sha256.c      SHA-256 (SHA-NI and portable), HMAC, PBKDF2
src/bcrypt.c      interleaved EksBlowfish, calibration, self-test, workers
src/util.c        buffer pool, JSON output, timestamps, base64url, slugify, percent-decoding
src/stats.c       STATS=1 output
tests/            unit tests, HTTP edge cases, fuzzing
tools/            SQL harness and generator, transcript and differential checks, local load test
```
