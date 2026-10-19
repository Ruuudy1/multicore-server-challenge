#!/usr/bin/env python3
# Differential and plan checks for statements.py: candidate SQL must return what the SPEC.md reference SQL returns.
# Usage: PGDSN="host=127.0.0.1 dbname=conduit_check user=postgres" python3 check.py [--seed N] [--rounds N] [--default-costs]
# Run it on a disposable database: write checks roll back, but rolled-back tag inserts still dirty visibility maps.
import os
import random
import re
import sys
import psycopg2
from statements import (ARTICLE, ARTICLE_BY_SLUG, COMMENTS, FULL_SELECT, REFERENCE, REFERENCE_COMMENTS, REFERENCE_USERS, SIGNATURES,
                        USER_SIGNATURES, USERS, V1, V2, WRITES)

SESSION = ["SET plan_cache_mode = force_generic_plan", "SET jit = off", "SET max_parallel_workers_per_gather = 0", "SET TimeZone = 'UTC'"]
# The box's pgtune values (box/pg-conf.sh for 8 vCPU / 16 GB); generic plans differ without them.
BOX_COSTS = ["SET random_page_cost = 1.1", "SET effective_cache_size = '12GB'", "SET effective_io_concurrency = 200", "SET work_mem = '9709kB'"]
BIG_TABLES = ("articles", "users", "favorites", "follows", "comments", "article_tags")
TRICKY_TITLES = ["Hello, World!", "  --Leading and trailing--  ", "Ångström über naïve", "123 go", "!!!", "", "A_B-C.D", "Ünïcödé only ÅÅ", "tab\tand\nnewline"]


def slugify(title):
    # Byte-level mirror of trim(both '-' from lower(regexp_replace(x, '[^A-Za-z0-9]+', '-', 'g'))): the algorithm C must implement.
    out, pending = bytearray(), False
    for byte in title.encode():
        if 48 <= byte <= 57 or 65 <= byte <= 90 or 97 <= byte <= 122:
            if pending and out:
                out.append(45)
            out.append(byte + 32 if 65 <= byte <= 90 else byte)
            pending = False
        else:
            pending = True
    return out.decode()


TALLY = {"checks": 0, "mismatches": 0}


def report(label, expected, actual):
    TALLY["checks"] += 1
    if expected != actual:
        TALLY["mismatches"] += 1
        print(f"MISMATCH {label}\n  expected: {str(expected)[:300]}\n  actual:   {str(actual)[:300]}")


def fetch(cur, sql, args=()):
    cur.execute(sql, args)
    return [row[0] for row in cur.fetchall()]


def prepare(cur):
    for kind, (page, count) in REFERENCE.items():
        cur.execute(f"PREPARE ref_{kind}_page(bigint, int, int, text) AS {page}")
        cur.execute(f"PREPARE ref_{kind}_count(bigint, int, int, text) AS {count}")
        cur.execute(f"PREPARE v1_{kind}(bigint, int, int, text) AS {V1[kind]}")
        cur.execute(f"PREPARE v2_{kind}(bigint, int, int, text) AS {V2[kind]}")
    cur.execute(f"PREPARE ref_article(bigint, text) AS {ARTICLE_BY_SLUG}")
    cur.execute(f"PREPARE article(bigint, bigint, text) AS {ARTICLE}")
    cur.execute(f"PREPARE ref_comments(bigint, text) AS {REFERENCE_COMMENTS}")
    cur.execute(f"PREPARE comments(bigint, bigint, text) AS {COMMENTS}")
    for name, sql in WRITES.items():
        cur.execute(f"PREPARE {name}{SIGNATURES[name]} AS {sql}")
    for name, sql in USERS.items():
        cur.execute(f"PREPARE {name}{USER_SIGNATURES[name]} AS {sql}")
    for name, sql in REFERENCE_USERS.items():
        cur.execute(f"PREPARE ref_{name}{USER_SIGNATURES[name]} AS {sql}")


def reference_list(cur, kind, params):
    cur.execute(f"EXECUTE ref_{kind}_page(%s, %s, %s, %s)", params)
    rows = [tuple(row) for row in cur.fetchall()]
    cur.execute(f"EXECUTE ref_{kind}_count(%s, %s, %s, %s)", params)
    return cur.fetchone()[0], rows


def v2_list(cur, kind, params):
    # Mirrors the C assembly: index the header facts, then decorate article rows sorted newest first.
    cur.execute(f"EXECUTE v2_{kind}(%s, %s, %s, %s)", params)
    rows = cur.fetchall()
    headers = [row for row in rows if row[0] == 0]
    assert len(headers) == 1, f"{kind}: expected one header row, got {len(headers)}"
    header = headers[0]
    counts = dict(zip(header[12] or [], header[13] or []))
    favorites, follows, tags = set(header[14] or []), set(header[15] or []), {}
    for article_id, name in zip(header[16] or [], header[17] or []):
        tags.setdefault(article_id, []).append(name)
    articles = sorted((row for row in rows if row[0] == 1), key=lambda row: -row[1])
    return header[11], [(row[1], row[3], row[4], row[5], row[6], row[7], tags.get(row[1], []), counts.get(row[1], 0), row[1] in favorites,
                         row[8], row[9], row[10], kind == "feed" or row[2] in follows) for row in articles]


def v1_list(cur, kind, params):
    cur.execute(f"EXECUTE v1_{kind}(%s, %s, %s, %s)", params)
    rows = cur.fetchall()
    return rows[0][0], [(row[1], row[3], row[4], row[5], row[6], row[7], row[8], row[9], row[10], row[11], row[12], row[13], row[14]) for row in rows if row[1]]


def candidate_comments(cur, viewer, article_id, slug):
    cur.execute("EXECUTE comments(%s, %s, %s)", (viewer, article_id, slug))
    rows = cur.fetchall()
    header = next((row for row in rows if row[0] == 0), None)
    if not header:
        return None
    follows = set(header[9] or [])
    return [(row[1], row[2], row[3], row[4], row[6], row[7], row[8], row[5] in follows)
            for row in sorted((row for row in rows if row[0] == 1), key=lambda row: (row[3], row[1]), reverse=True)]


def reference_comments(cur, viewer, slug):
    if not fetch(cur, "SELECT id FROM articles WHERE slug = %s", (slug,)):
        return None
    cur.execute("EXECUTE ref_comments(%s, %s)", (viewer, slug))
    return [tuple(row) for row in cur.fetchall()]


def gap_offsets(cur):
    # Offsets whose 10-row window straddles a hole in the id sequence, where a wrong range would show.
    return fetch(cur, "SELECT greatest((SELECT count(*) FROM articles b WHERE b.id > g.id) - 5, 0) FROM articles g "
                      "WHERE NOT EXISTS (SELECT 1 FROM articles n WHERE n.id = g.id + 1) AND g.id < (SELECT max(id) FROM articles) LIMIT 4")


def check_reads(cur, rng, rounds):
    hot_article = fetch(cur, "SELECT article_id FROM favorites GROUP BY 1 ORDER BY count(*) DESC LIMIT 1")[0]
    viewers = (fetch(cur, "SELECT id FROM users ORDER BY random() LIMIT 40") + fetch(cur, "SELECT follower_id FROM follows GROUP BY 1 ORDER BY count(*) DESC LIMIT 5") +
               fetch(cur, "SELECT user_id FROM favorites WHERE article_id = %s LIMIT 3", (hot_article,)) + [0])
    usernames = (fetch(cur, "SELECT username FROM users WHERE id IN (SELECT author_id FROM articles GROUP BY 1 ORDER BY count(*) DESC LIMIT 6)") +
                 fetch(cur, "SELECT username FROM users ORDER BY random() LIMIT 6") + ["nobody-zz"])
    tag_names = fetch(cur, "SELECT name FROM tags ORDER BY random() LIMIT 8") + ["missing-zz"]
    total = fetch(cur, "SELECT count(*) FROM articles")[0]
    # The reference SQL decorates every row OFFSET skips, so deep and past-the-end offsets are only cheap on small databases.
    offsets = [0, 0, 10, 20, 30, 40, min(total // 2, 2000)] + ([max(total - 15, 0), total + 100] if total <= 50000 else []) + gap_offsets(cur)
    for kind in REFERENCE:
        for _ in range(rounds):
            viewer, limit = rng.choice(viewers), rng.choice([1, 5, 10, 10, 20, 100])
            offset = rng.choice(offsets) if kind == "global" else rng.choice([0, 0, 10, 20, 40, 100, 5000])
            needle = rng.choice(tag_names) if kind == "tag" else rng.choice(usernames) if kind in ("author", "favorited") else None
            params = (viewer, limit, offset, needle)
            expected = reference_list(cur, kind, params)
            report(f"v1 {kind} {params}", expected, v1_list(cur, kind, params))
            report(f"v2 {kind} {params}", expected, v2_list(cur, kind, params))
    cur.execute("SELECT id, slug FROM articles ORDER BY random() LIMIT 60")
    articles = cur.fetchall()
    cur.execute("SELECT id, slug FROM articles WHERE id IN (%s, (SELECT article_id FROM comments GROUP BY 1 ORDER BY count(*) DESC LIMIT 1))", (hot_article,))
    articles += cur.fetchall() + [(10 ** 12, "missing-1000000000000")]
    for article_id, slug in articles:
        viewer = rng.choice(viewers)
        for candidate_slug in (slug, slug + "x"):
            cur.execute("EXECUTE ref_article(%s, %s)", (viewer, candidate_slug))
            expected = [tuple(row)[2:] for row in cur.fetchall()]
            cur.execute("EXECUTE article(%s, %s, %s)", (viewer, article_id, candidate_slug))
            report(f"article {article_id} {candidate_slug}", expected, [tuple(row) for row in cur.fetchall()])
            report(f"comments {article_id} {candidate_slug}", reference_comments(cur, viewer, candidate_slug), candidate_comments(cur, viewer, article_id, candidate_slug))
    for title in TRICKY_TITLES:
        report(f"slugify {title!r}", fetch(cur, "SELECT trim(both '-' from lower(regexp_replace(%s, '[^A-Za-z0-9]+', '-', 'g')))", (title,))[0], slugify(title))


def full_article(cur, viewer, article_id):
    cur.execute(FULL_SELECT.replace("$1", "%s") + " WHERE a.id = %s", (viewer, viewer, article_id))
    row = cur.fetchone()
    return tuple(row)[2:] if row else None


def check_writes(conn):
    cur = conn.cursor()
    article_id = fetch(cur, "SELECT article_id FROM favorites GROUP BY 1 ORDER BY count(*) DESC LIMIT 1")[0]
    cur.execute("SELECT slug, author_id FROM articles WHERE id = %s", (article_id,))
    slug, author = cur.fetchone()
    fan = fetch(cur, "SELECT user_id FROM favorites WHERE article_id = %s LIMIT 1", (article_id,))[0]
    stranger = fetch(cur, "SELECT u.id FROM users u WHERE NOT EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = u.id AND f.article_id = %s) LIMIT 1", (article_id,))[0]
    for viewer, statement in ((stranger, "fav_add"), (fan, "fav_add"), (fan, "fav_del"), (stranger, "fav_del")):
        cur.execute(f"EXECUTE {statement}(%s, %s, %s)", (viewer, article_id, slug))
        candidate = tuple(cur.fetchone())
        conn.rollback()
        cur.execute("INSERT INTO favorites (user_id, article_id) VALUES (%s, %s) ON CONFLICT DO NOTHING" if statement == "fav_add" else
                    "DELETE FROM favorites WHERE user_id = %s AND article_id = %s", (viewer, article_id))
        report(f"{statement} viewer {viewer}", full_article(cur, viewer, article_id), candidate)
        conn.rollback()
    cur.execute("EXECUTE fav_add(%s, %s, %s)", (stranger, article_id, slug + "x"))
    report("fav_add wrong slug -> 404", 0, cur.rowcount)
    conn.rollback()
    target = fetch(cur, "SELECT username FROM users WHERE id <> %s ORDER BY id LIMIT 1", (stranger,))[0]
    cur.execute("EXECUTE follow(%s, %s)", (stranger, target))
    report("follow returns profile", target, cur.fetchone()[0])
    report("follow row exists", 1, fetch(cur, "SELECT count(*) FROM follows WHERE follower_id = %s AND followee_id = (SELECT id FROM users WHERE username = %s)", (stranger, target))[0])
    conn.rollback()
    cur.execute("EXECUTE unfollow(%s, 'nobody-zz')", (stranger,))
    report("unfollow unknown -> 404", 0, cur.rowcount)
    conn.rollback()
    cur.execute("EXECUTE comment_add(%s, %s, %s, %s)", (stranger, article_id, slug, 'hello "quoted"'))
    row = cur.fetchone()
    report("comment_add", ('hello "quoted"', fetch(cur, "SELECT username FROM users WHERE id = %s", (stranger,))[0]), (row[1], row[4]))
    conn.rollback()
    cur.execute("EXECUTE comment_add(%s, %s, %s, 'x')", (stranger, article_id, slug + "x"))
    report("comment_add wrong slug -> 404", 0, cur.rowcount)
    conn.rollback()
    cur.execute("SELECT c.id, c.author_id, a.id, a.slug FROM comments c JOIN articles a ON a.id = c.article_id LIMIT 1")
    comment_id, comment_author, comment_article, comment_slug = cur.fetchone()
    cur.execute("EXECUTE comment_del(%s, %s, %s, %s)", (comment_author, comment_article, comment_slug, comment_id))
    report("comment_del own", comment_author, cur.fetchone()[0])
    report("comment_del removed", 0, fetch(cur, "SELECT count(*) FROM comments WHERE id = %s", (comment_id,))[0])
    conn.rollback()
    cur.execute("EXECUTE comment_del(%s, %s, %s, %s)", (comment_author + 1, comment_article, comment_slug, comment_id))
    report("comment_del other -> 403", comment_author, cur.fetchone()[0])
    report("comment_del kept", 1, fetch(cur, "SELECT count(*) FROM comments WHERE id = %s", (comment_id,))[0])
    conn.rollback()
    known = fetch(cur, "SELECT name FROM tags ORDER BY id LIMIT 2")
    cur.execute("EXECUTE article_add(%s, %s, 'd', 'b', %s, %s)", (stranger, "Hello, World!", slugify("Hello, World!"), ["zz-new-tag", known[0], known[0], known[1]]))
    row = cur.fetchone()
    new_id = int(row[0].rsplit("-", 1)[1])
    report("article_add slug", fetch(cur, "SELECT slug FROM articles WHERE id = %s", (new_id,))[0], row[0])
    report("article_add matches full select", full_article(cur, stranger, new_id), tuple(row[1:]))
    conn.rollback()
    cur.execute("EXECUTE article_add(%s, 'No tags', 'd', 'b', 'no-tags', '{}')", (stranger,))
    report("article_add without tags", [], cur.fetchone()[6])
    conn.rollback()
    cur.execute("EXECUTE article_put(%s, %s, %s, 'New Title!', NULL, NULL, %s)", (author, article_id, slug, slugify("New Title!")))
    row = cur.fetchone()
    report("article_put slug", f"new-title-{article_id}", row[1])
    report("article_put matches full select", full_article(cur, author, article_id), tuple(row[2:]))
    conn.rollback()
    cur.execute("EXECUTE article_put(%s, %s, %s, 'X', NULL, NULL, 'x')", (author + 1, article_id, slug))
    row = cur.fetchone()
    report("article_put other -> 403", (author, None), (row[0], row[1]))
    conn.rollback()
    cur.execute("EXECUTE article_del(%s, %s, %s)", (author, article_id, slug))
    report("article_del own", author, cur.fetchone()[0])
    report("article_del removed", 0, fetch(cur, "SELECT count(*) FROM articles WHERE id = %s", (article_id,))[0])
    conn.rollback()
    cur.execute("EXECUTE tags")
    candidate = cur.fetchone()[0]
    report("tags", fetch(cur, "SELECT name FROM tags ORDER BY name"), candidate)
    conn.rollback()


def check_users(conn):
    # User and profile statements against the SPEC.md reference (reads), and the user writes in rolled-back transactions.
    cur = conn.cursor()
    viewers = fetch(cur, "SELECT follower_id FROM follows GROUP BY 1 ORDER BY count(*) DESC LIMIT 3") + [0]
    cur.execute("SELECT id, username, email FROM users ORDER BY random() LIMIT 25")
    users = cur.fetchall()
    followed = fetch(cur, "SELECT u.username FROM users u WHERE u.id IN (SELECT followee_id FROM follows WHERE follower_id = %s LIMIT 3)", (viewers[0],))
    for uid, username, email in users:
        cur.execute("EXECUTE ref_user_get(%s)", (uid,))
        expected = [tuple(row)[1:] for row in cur.fetchall()]
        cur.execute("EXECUTE user_get(%s)", (uid,))
        report(f"user_get {uid}", expected, [tuple(row) for row in cur.fetchall()])
        cur.execute("SELECT id, username, email, bio, image, password_hash FROM users WHERE email = %s", (email,))
        expected = [tuple(row) for row in cur.fetchall()]
        cur.execute("EXECUTE user_login(%s)", (email,))
        report(f"user_login {email}", expected, [tuple(row) for row in cur.fetchall()])
    for viewer in viewers:
        for username in [u[1] for u in users[:8]] + followed + ["nobody-zz"]:
            cur.execute("EXECUTE ref_profile(%s, %s)", (viewer, username))
            expected = [tuple(row)[1:] for row in cur.fetchall()]
            cur.execute("EXECUTE profile(%s, %s)", (viewer, username))
            report(f"profile {viewer} {username}", expected, [tuple(row) for row in cur.fetchall()])
    cur.execute("EXECUTE user_get(%s)", (10 ** 12,))
    report("user_get unknown -> 401", 0, cur.rowcount)
    cur.execute("EXECUTE user_login('nobody@example.invalid')")
    report("user_login unknown -> 401", 0, cur.rowcount)
    conn.rollback()
    uid, username, email = users[0]
    cur.execute("EXECUTE user_register('sqlcheck_new', 'sqlcheck_new@example.com', '$2a$10$x')")
    row = cur.fetchone()
    report("user_register returns the row", ("sqlcheck_new", "sqlcheck_new@example.com", None, None), tuple(row[1:]))
    conn.rollback()
    for label, args in (("username", (username, "sqlcheck_other@example.com")), ("email", ("sqlcheck_other", email))):
        try:
            cur.execute("EXECUTE user_register(%s, %s, '$2a$10$x')", args)
            report(f"user_register duplicate {label} -> 23505", "23505", None)
        except psycopg2.Error as e:
            report(f"user_register duplicate {label} -> 23505", "23505", e.pgcode)
        conn.rollback()
    cur.execute("SELECT id, username, email, bio, image FROM users WHERE id = %s", (uid,))
    before = tuple(cur.fetchone())
    cur.execute("EXECUTE user_update(%s, NULL, NULL, NULL, NULL, NULL)", (uid,))
    report("user_update nulls unchanged", before, tuple(cur.fetchone()))
    cur.execute("EXECUTE user_update(%s, NULL, NULL, NULL, '', 'https://example.com/i.png')", (uid,))
    report("user_update bio and image", before[:3] + ("", "https://example.com/i.png"), tuple(cur.fetchone()))
    conn.rollback()
    try:
        cur.execute("EXECUTE user_update(%s, %s, NULL, NULL, NULL, NULL)", (uid, users[1][2]))
        report("user_update taken email -> 23505", "23505", None)
    except psycopg2.Error as e:
        report("user_update taken email -> 23505", "23505", e.pgcode)
    conn.rollback()
    cur.execute("EXECUTE user_update(%s, NULL, NULL, NULL, 'x', NULL)", (10 ** 12,))
    report("user_update unknown -> 401", 0, cur.rowcount)
    conn.rollback()


def check_plans(cur):
    # Flags plan shapes that read whole key ranges or whole big tables; the one 200-row scan of tags is expected.
    viewer = fetch(cur, "SELECT follower_id FROM follows GROUP BY 1 ORDER BY count(*) DESC LIMIT 1")[0]
    tag = fetch(cur, "SELECT name FROM tags t WHERE t.id = (SELECT tag_id FROM article_tags GROUP BY 1 ORDER BY count(*) DESC LIMIT 1)")[0]
    author = fetch(cur, "SELECT username FROM users WHERE id = (SELECT author_id FROM articles GROUP BY 1 ORDER BY count(*) DESC LIMIT 1)")[0]
    cur.execute("SELECT id, slug FROM articles WHERE id = (SELECT article_id FROM comments GROUP BY 1 ORDER BY count(*) DESC LIMIT 1)")
    hot_id, hot_slug = cur.fetchone()
    samples = [(f"{shape}_{kind}", (viewer, 10, 20, tag if kind == "tag" else author if kind in ("author", "favorited") else None)) for kind in V2 for shape in ("v1", "v2")]
    samples += [("article", (viewer, hot_id, hot_slug)), ("comments", (viewer, hot_id, hot_slug)), ("user_get", (viewer,)),
                ("profile", (viewer, author)), ("tags", ())]
    warnings = []
    for name, params in samples:
        cur.execute(f"EXPLAIN (COSTS OFF) EXECUTE {name}{'(' + ', '.join(['%s'] * len(params)) + ')' if params else ''}", params)
        for line in (row[0] for row in cur.fetchall()):
            if "Bitmap" in line or re.search(r"Seq Scan on (%s)\b" % "|".join(BIG_TABLES), line):
                warnings.append(f"{name}: {line.strip()}")
    return warnings


def main():
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 7
    conn = psycopg2.connect(os.environ.get("PGDSN", "host=127.0.0.1 dbname=conduit user=postgres"))
    cur = conn.cursor()
    for statement in SESSION + ([] if "--default-costs" in sys.argv else BOX_COSTS):
        cur.execute(statement)
    prepare(cur)
    conn.commit()
    check_reads(cur, random.Random(seed), int(sys.argv[sys.argv.index("--rounds") + 1]) if "--rounds" in sys.argv else 150)
    conn.rollback()
    check_writes(conn)
    check_users(conn)
    warnings = check_plans(cur)
    print("\n".join(f"PLAN WARNING {warning}" for warning in warnings))
    print(f"{TALLY['checks']} checks, {TALLY['mismatches']} mismatches, {len(warnings)} plan warnings")
    sys.exit(1 if TALLY["mismatches"] else 0)


if __name__ == "__main__":
    main()
