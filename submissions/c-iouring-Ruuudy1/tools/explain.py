#!/usr/bin/env python3
# EXPLAIN (ANALYZE, BUFFERS) of a catalog statement under the server's session settings and the box's costs.
#   PGDSN="dbname=conduit_seed" python3 tools/explain.py feed v1 [viewer] [limit] [offset]
#   PGDSN=... python3 tools/explain.py global v2
import os
import sys

import psycopg2

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "sqlcheck"))
import statements as S  # noqa: E402

kind, shape = sys.argv[1], sys.argv[2]
conn = psycopg2.connect(os.environ.get("PGDSN", "dbname=conduit_seed"))
cur = conn.cursor()
for s in ["SET plan_cache_mode = force_generic_plan", "SET jit = off", "SET max_parallel_workers_per_gather = 0",
          "SET enable_bitmapscan = off", "SET random_page_cost = 1.1", "SET effective_cache_size = '12GB'", "SET work_mem = '9709kB'"]:
    cur.execute(s)
sql = (S.V1 if shape == "v1" else S.V2)[kind]
cur.execute(f"PREPARE q(bigint, int, int, text) AS {sql}")
if len(sys.argv) > 3:
    viewer = int(sys.argv[3])
else:
    cur.execute("SELECT follower_id FROM follows GROUP BY 1 HAVING count(*) BETWEEN 15 AND 25 ORDER BY follower_id LIMIT 1")
    viewer = cur.fetchone()[0]
limit = int(sys.argv[4]) if len(sys.argv) > 4 else 10
offset = int(sys.argv[5]) if len(sys.argv) > 5 else 0
needle = None
if kind == "tag":
    cur.execute("SELECT name FROM tags ORDER BY id LIMIT 1")
    needle = cur.fetchone()[0]
elif kind in ("author", "favorited"):
    cur.execute("SELECT username FROM users WHERE id = (SELECT author_id FROM articles GROUP BY 1 ORDER BY count(*) DESC LIMIT 1)")
    needle = cur.fetchone()[0]
for _ in range(3):  # warm the plan cache and the buffers
    cur.execute("EXECUTE q(%s, %s, %s, %s)", (viewer, limit, offset, needle))
cur.execute("EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING ON) EXECUTE q(%s, %s, %s, %s)", (viewer, limit, offset, needle))
print(f"-- {kind} {shape} viewer={viewer} limit={limit} offset={offset} needle={needle}")
print("\n".join(r[0] for r in cur.fetchall()))
