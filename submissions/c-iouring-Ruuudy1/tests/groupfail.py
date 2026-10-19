#!/usr/bin/env python3
# Group commit under failure: many concurrent comments share write groups, and one of them carries a NUL byte
# (\u0000, valid JSON that Postgres rejects with 22021). Its group must roll back and every write in it be retried
# alone: all the others get 201 and are stored, only the bad one fails.
#   python3 tests/groupfail.py <port> <database> [writers, default 40]
import base64
import hashlib
import hmac
import http.client
import json
import subprocess
import sys
import threading
import time

port, db = int(sys.argv[1]), sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
SECRET = b"multicore-server-challenge"


def b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def token(uid):
    now = int(time.time())
    head = b64(b'{"alg":"HS256","typ":"JWT"}')
    body = b64(json.dumps({"sub": str(uid), "iat": now, "exp": now + 3600}, separators=(",", ":")).encode())
    return f"{head}.{body}.{b64(hmac.new(SECRET, f'{head}.{body}'.encode(), hashlib.sha256).digest())}"


def psql(sql):
    return subprocess.run(["psql", "-X", "-At", "-d", db, "-c", sql], capture_output=True, text=True, check=True).stdout.strip()


slug = psql("SELECT slug FROM articles ORDER BY id DESC LIMIT 1")
before = int(psql(f"SELECT count(*) FROM comments WHERE article_id = (SELECT id FROM articles WHERE slug = '{slug}')"))
bad = n // 2
results = [None] * n
start = threading.Barrier(n)


def post(i):
    text = "group commit bad \\u0000 byte" if i == bad else f"group commit comment {i}"
    body = '{"comment":{"body":"' + text + '"}}'
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    start.wait()
    for round_ in range(3):  # three waves on the same connections, so writes queue behind a commit in flight
        c.request("POST", f"/api/articles/{slug}/comments", body=body,
                  headers={"Authorization": "Token " + token(1000 + i), "Content-Type": "application/json"})
        r = c.getresponse()
        r.read()
        results[i] = (results[i] or []) + [r.status]


threads = [threading.Thread(target=post, args=(i,)) for i in range(n)]
for t in threads:
    t.start()
for t in threads:
    t.join()
after = int(psql(f"SELECT count(*) FROM comments WHERE article_id = (SELECT id FROM articles WHERE slug = '{slug}')"))
good = sum(s == 201 for i, r in enumerate(results) if i != bad for s in r)
bad_statuses = results[bad]
print(f"{n} writers x 3: {good} of {3 * (n - 1)} good comments answered 201, the bad one answered {bad_statuses}; "
      f"{after - before} comments stored (want {3 * (n - 1)})")
ok = good == 3 * (n - 1) and all(s == 500 for s in bad_statuses) and after - before == 3 * (n - 1)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
