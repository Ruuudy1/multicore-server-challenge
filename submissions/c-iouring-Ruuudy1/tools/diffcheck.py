#!/usr/bin/env python3
# Differential test: sends the same read requests to the Go reference server and to this one (both on the same
# database) and compares status codes and JSON bodies (normalized: key order ignored, error messages ignored).
#   python3 tools/diffcheck.py --ref http://127.0.0.1:3101 --test http://127.0.0.1:3102 --secret S [--n 2500] [--seed 1]
# Only reads, so both servers see the same data. Exit code 0 only with zero differences.
import argparse
import base64
import hashlib
import hmac
import http.client
import json
import random
import sys
import time
import urllib.parse


def b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def token(secret, uid, exp_delta=30 * 24 * 3600):
    now = int(time.time())
    head = b64(b'{"alg":"HS256","typ":"JWT"}')
    body = b64(json.dumps({"sub": str(uid), "iat": now, "exp": now + exp_delta}, separators=(",", ":")).encode())
    sig = b64(hmac.new(secret.encode(), f"{head}.{body}".encode(), hashlib.sha256).digest())
    return f"{head}.{body}.{sig}"


class Client:
    def __init__(self, base):
        u = urllib.parse.urlsplit(base)
        self.host, self.port = u.hostname, u.port
        self.conn = None

    def get(self, path, auth=None):
        headers = {"Authorization": auth} if auth else {}
        for attempt in range(2):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection(self.host, self.port, timeout=30)
                self.conn.request("GET", path, headers=headers)
                r = self.conn.getresponse()
                return r.status, r.read()
            except (http.client.HTTPException, ConnectionError, OSError):
                self.conn = None
                if attempt:
                    raise


def normalize(status, raw):
    if not raw:
        return status, None
    try:
        v = json.loads(raw)
    except ValueError:
        return status, "<not json>"
    if isinstance(v, dict) and isinstance(v.get("errors"), dict):
        return status, "<error>"
    return status, json.dumps(v, sort_keys=True, ensure_ascii=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--secret", required=True)
    ap.add_argument("--n", type=int, default=2500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--users", type=int, default=100000)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    ref, test = Client(a.ref), Client(a.test)

    # discover data through the reference server
    _, raw = ref.get("/api/tags")
    tags = json.loads(raw)["tags"]
    slugs, authors = [], set()
    for off in (0, 20, 40, 100, 500, 1000, 5000):
        st, raw = ref.get(f"/api/articles?limit=100&offset={off}")
        for art in json.loads(raw)["articles"]:
            slugs.append(art["slug"])
            authors.add(art["author"]["username"])
    authors = sorted(authors)
    max_id = max(int(s.rsplit("-", 1)[1]) for s in slugs)

    def viewer():
        return None if rng.random() < 0.3 else "Token " + token(a.secret, rng.randint(1, a.users))

    def page():
        parts = []
        if rng.random() < 0.8:
            parts.append(f"limit={rng.choice([1, 2, 5, 10, 10, 20, 50, 100])}")
        if rng.random() < 0.7:
            parts.append(f"offset={rng.choice([0, 0, 5, 10, 20, 30, 40, 99, 100, 250, 1000, 5000, 10**8])}")
        return parts

    gens = []

    def g_global():
        return "/api/articles?" + "&".join(page()), viewer()

    def g_tag():
        t = rng.choice(tags + ["no_such_tag_zz"])
        return "/api/articles?" + "&".join([f"tag={urllib.parse.quote(t)}"] + page()), viewer()

    def g_author():
        u = rng.choice(authors + ["no_such_user_zz"])
        return "/api/articles?" + "&".join([f"author={u}"] + page()), viewer()

    def g_fav():
        u = rng.choice(authors + [f"x{rng.randint(1, 99)}"])
        return "/api/articles?" + "&".join([f"favorited={u}"] + page()), viewer()

    def g_combo():
        q = [f"author={rng.choice(authors)}", f"tag={rng.choice(tags)}", "tag=", f"favorited={rng.choice(authors)}"]
        rng.shuffle(q)
        return "/api/articles?" + "&".join(q[: rng.randint(1, 4)] + page()), viewer()

    def g_feed():
        return "/api/articles/feed?" + "&".join(page()), "Token " + token(a.secret, rng.randint(1, a.users))

    def g_article():
        r = rng.random()
        if r < 0.6:
            s = rng.choice(slugs)
        elif r < 0.8:
            s = f"anything-{rng.randint(1, max_id)}"
        elif r < 0.9:
            s = rng.choice(slugs) + "x"
        else:
            s = rng.choice(["nope", "no-such-article-999999999", "-5", "abc-0", "a-%32"])
        return f"/api/articles/{s}", viewer()

    def g_comments():
        p, v = g_article()
        return p + "/comments", v

    def g_profile():
        return f"/api/profiles/{rng.choice(authors + ['no_such_user_zz'])}", viewer()

    def g_user():
        return "/api/user", "Token " + token(a.secret, rng.choice([rng.randint(1, a.users), 999999999]))

    def g_bad():
        return rng.choice([
            ("/api/articles?limit=0", None), ("/api/articles?limit=101", None), ("/api/articles?offset=-1", None),
            ("/api/articles?limit=abc", None), ("/api/articles?limit=", None), ("/api/articles?offset=99999999999999999999", None),
            ("/api/articles", "Token garbage"), ("/api/articles/feed", None), ("/api/user", "Bearer x"),
            ("/api/articles", "Token " + token(a.secret, 5, -10)), ("/api/nope", None), ("/api/tags/", None),
            ("/api/articles?limit=%2B5", None), ("/api/articles?limit=05&offset=005", None), ("/api/articles?tag=a+b", None),
        ])

    weights = [(g_global, 12), (g_tag, 8), (g_author, 8), (g_fav, 5), (g_combo, 4), (g_feed, 10), (g_article, 20),
               (g_comments, 20), (g_profile, 6), (g_user, 3), (g_bad, 4), (lambda: ("/api/tags", viewer()), 2)]
    total = sum(w for _, w in weights)
    diffs = 0
    statuses = {}
    t0 = time.time()
    for i in range(a.n):
        x = rng.uniform(0, total)
        for g, w in weights:
            x -= w
            if x <= 0:
                break
        path, auth = g()
        r1 = normalize(*ref.get(path, auth))
        r2 = normalize(*test.get(path, auth))
        statuses[r1[0]] = statuses.get(r1[0], 0) + 1
        if r1 != r2:
            diffs += 1
            if diffs <= 10:
                print(f"DIFF {path} auth={'yes' if auth else 'no'}\n  ref:  {str(r1)[:400]}\n  test: {str(r2)[:400]}")
    print(f"{a.n} requests in {time.time() - t0:.1f}s, statuses {dict(sorted(statuses.items()))}, {diffs} differences")
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
