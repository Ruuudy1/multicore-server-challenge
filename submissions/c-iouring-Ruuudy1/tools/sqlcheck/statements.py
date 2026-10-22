# Candidate SQL for the multicore challenge server and the SPEC.md reference SQL it must reproduce.
# List placeholders: $1 viewer id (0 = anonymous), $2 limit (1 to 100), $3 offset, $4 filter (tag name or username).

LIST_SELECT = """SELECT a.id, a.slug, a.title, a.description, a.created_at, a.updated_at,
       ARRAY(SELECT t.name FROM article_tags at JOIN tags t ON t.id = at.tag_id WHERE at.article_id = a.id ORDER BY t.name) AS tag_list,
       (SELECT count(*) FROM favorites f WHERE f.article_id = a.id) AS favorites_count,
       EXISTS (SELECT 1 FROM favorites f WHERE f.article_id = a.id AND f.user_id = $1) AS favorited,
       u.username, u.bio, u.image,
       EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = a.author_id) AS following
  FROM articles a JOIN users u ON u.id = a.author_id"""

FULL_SELECT = LIST_SELECT.replace("a.description, a.created_at", "a.description, a.body, a.created_at")

# Reference page and capped count per list kind, verbatim from SPEC.md.
REFERENCE = {
    "global": (LIST_SELECT + " ORDER BY a.id DESC LIMIT $2 OFFSET $3",
               "SELECT count(*) FROM (SELECT 1 FROM articles a LIMIT 1000) s"),
    "tag": (LIST_SELECT + " WHERE a.id IN (SELECT at.article_id FROM article_tags at WHERE at.tag_id = (SELECT t.id FROM tags t WHERE t.name = $4) ORDER BY at.article_id DESC LIMIT $2 OFFSET $3) ORDER BY a.id DESC",
            "SELECT count(*) FROM (SELECT 1 FROM article_tags at WHERE at.tag_id = (SELECT t.id FROM tags t WHERE t.name = $4) LIMIT 1000) s"),
    "author": (LIST_SELECT + " WHERE a.author_id = (SELECT au.id FROM users au WHERE au.username = $4) ORDER BY a.id DESC LIMIT $2 OFFSET $3",
               "SELECT count(*) FROM (SELECT 1 FROM articles a WHERE a.author_id = (SELECT u.id FROM users u WHERE u.username = $4) LIMIT 1000) s"),
    "favorited": (LIST_SELECT + " WHERE a.id IN (SELECT f.article_id FROM favorites f WHERE f.user_id = (SELECT fu.id FROM users fu WHERE fu.username = $4) ORDER BY f.article_id DESC LIMIT $2 OFFSET $3) ORDER BY a.id DESC",
                  "SELECT count(*) FROM (SELECT 1 FROM favorites f WHERE f.user_id = (SELECT fu.id FROM users fu WHERE fu.username = $4) LIMIT 1000) s"),
    "feed": (LIST_SELECT + " WHERE a.id IN (SELECT x.id FROM follows fo CROSS JOIN LATERAL (SELECT a2.id FROM articles a2 WHERE a2.author_id = fo.followee_id ORDER BY a2.id DESC LIMIT ($2::int + $3::int)) x WHERE fo.follower_id = $1 ORDER BY x.id DESC LIMIT $2 OFFSET $3) ORDER BY a.id DESC",
             "SELECT count(*) FROM (SELECT 1 FROM articles a WHERE a.author_id IN (SELECT fo.followee_id FROM follows fo WHERE fo.follower_id = $1) LIMIT 1000) s"),
}

REFERENCE_COMMENTS = """SELECT c.id, c.body, c.created_at, c.updated_at, u.username, u.bio, u.image,
       EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = c.author_id) AS following
  FROM comments c JOIN users u ON u.id = c.author_id
 WHERE c.article_id = (SELECT id FROM articles WHERE slug = $2)
 ORDER BY c.created_at DESC, c.id DESC LIMIT 100"""

FOLLOWEES = "ARRAY(SELECT fo.followee_id FROM follows fo WHERE fo.follower_id = $1)"
TAG_ID = "(SELECT id FROM tid)"
AUTHOR_ID = "(SELECT au.id FROM users au WHERE au.username = $4)"
FAN_ID = "(SELECT fu.id FROM users fu WHERE fu.username = $4)"

# Page ids per list kind: offset and a constant inner LIMIT 100 here, the outer LIMIT $2 picks the page, so generic plans expect few rows.
PAGES = {
    "global": "SELECT id FROM articles ORDER BY id DESC OFFSET $3 LIMIT 100",
    "tag": f"SELECT at.article_id AS id FROM article_tags at WHERE at.tag_id = {TAG_ID} ORDER BY at.article_id DESC OFFSET $3 LIMIT 100",
    "author": f"SELECT a.id FROM articles a WHERE a.author_id = {AUTHOR_ID} ORDER BY a.id DESC OFFSET $3 LIMIT 100",
    "favorited": f"SELECT f.article_id AS id FROM favorites f WHERE f.user_id = {FAN_ID} ORDER BY f.article_id DESC OFFSET $3 LIMIT 100",
    "feed": ("SELECT x.id FROM follows fo CROSS JOIN LATERAL (SELECT a2.id FROM articles a2 WHERE a2.author_id = fo.followee_id ORDER BY a2.id DESC "
             "LIMIT ($2::int + $3::int)) x WHERE fo.follower_id = $1 ORDER BY x.id DESC OFFSET $3 LIMIT 100"),
}

# Capped counts walk an ordered index from the oldest end and stop at the 1,000th entry; a bitmap scan would read the whole key range.
COUNTS = {
    "global": "CASE WHEN EXISTS (SELECT 1 FROM articles ORDER BY id OFFSET 999 LIMIT 1) THEN 1000::bigint ELSE (SELECT count(*) FROM articles) END",
    "tag": (f"CASE WHEN EXISTS (SELECT 1 FROM article_tags at WHERE at.tag_id = {TAG_ID} ORDER BY at.article_id OFFSET 999 LIMIT 1) THEN 1000::bigint "
            f"ELSE (SELECT count(*) FROM (SELECT 1 FROM article_tags at WHERE at.tag_id = {TAG_ID} ORDER BY at.article_id LIMIT 1000) s) END"),
    "author": f"(SELECT count(*) FROM (SELECT 1 FROM articles a WHERE a.author_id = {AUTHOR_ID} ORDER BY a.id LIMIT 1000) s)",
    "favorited": f"(SELECT count(*) FROM (SELECT 1 FROM favorites f WHERE f.user_id = {FAN_ID} ORDER BY f.article_id LIMIT 1000) s)",
    "feed": (f"CASE WHEN EXISTS (SELECT 1 FROM articles a WHERE a.author_id = ANY({FOLLOWEES}) OFFSET 999 LIMIT 1) THEN 1000::bigint "
             f"ELSE (SELECT count(*) FROM articles a WHERE a.author_id = ANY({FOLLOWEES})) END"),
}

PREFIXES = {"tag": "tid AS MATERIALIZED (SELECT t.id FROM tags t WHERE t.name = $4)"}


def per_row_list(kind):
    # v1: per-row decoration with the bounded page, the ordered capped count and a fenced author lookup.
    following = "true" if kind == "feed" else "EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = p.author_id)"
    return (f"{'WITH ' + PREFIXES[kind] + ' ' if kind in PREFIXES else ''}"
            "SELECT c.n, p.id, p.author_id, p.slug, p.title, p.description, p.created_at, p.updated_at, "
            "ARRAY(SELECT (SELECT t.name FROM tags t WHERE t.id = at.tag_id) FROM article_tags at WHERE at.article_id = p.id ORDER BY 1) AS tag_list, "
            "(SELECT count(*) FROM favorites f WHERE f.article_id = p.id) AS favorites_count, "
            "EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = $1 AND f.article_id = p.id) AS favorited, "
            f"u.username, u.bio, u.image, {following} AS following "
            f"FROM (SELECT {COUNTS[kind]} AS n) c "
            "LEFT JOIN LATERAL (SELECT a.id, a.author_id, a.slug, a.title, a.description, a.created_at, a.updated_at "
            f"FROM ({PAGES[kind]}) b JOIN articles a ON a.id = b.id ORDER BY a.id DESC LIMIT $2) p ON true "
            "LEFT JOIN LATERAL (SELECT username, bio, image FROM users WHERE id = p.author_id OFFSET 0) u ON true "
            "ORDER BY p.id DESC")


# v2: one header row (k = 0) of per-page facts, then plain article rows (k = 1). C joins them.
HEADER_COLUMNS = ("0 AS k, NULL::bigint AS id, NULL::bigint AS author_id, NULL::text AS slug, NULL::text AS title, "
                  "NULL::text AS description, NULL::timestamptz AS created_at, NULL::timestamptz AS updated_at, "
                  "NULL::text AS username, NULL::text AS bio, NULL::text AS image")


def favorite_counts(match, counts="group"):
    # match(column) restricts favorites rows to the page's article ids.
    # counts="raw" (E7): ids lists the article id of every favorite on the page and cnts is NULL; C counts them.
    if counts == "group":
        return (f"SELECT array_agg(g.article_id ORDER BY g.article_id) AS ids, array_agg(g.n ORDER BY g.article_id) AS cnts\n"
                f"                       FROM (SELECT f.article_id, count(*) AS n FROM favorites f WHERE {match('f.article_id')} GROUP BY f.article_id) g")
    return f"SELECT ARRAY(SELECT f.article_id FROM favorites f WHERE {match('f.article_id')}) AS ids, NULL::bigint[] AS cnts"


def tag_names(match, tags="hash"):
    # (article id, tag name) pairs ordered by name. tags="probe" (E8): names by primary-key probes instead of a hash join on tags.
    if tags == "hash":
        return ("SELECT array_agg(at.article_id ORDER BY t.name, at.article_id) AS aids, array_agg(t.name ORDER BY t.name, at.article_id) AS names\n"
                f"                       FROM article_tags at JOIN tags t ON t.id = at.tag_id WHERE {match('at.article_id')}")
    return ("SELECT array_agg(x.article_id ORDER BY x.name, x.article_id) AS aids, array_agg(x.name ORDER BY x.name, x.article_id) AS names "
            "FROM (SELECT at.article_id, t.name FROM article_tags at "
            "CROSS JOIN LATERAL (SELECT tg.name FROM tags tg WHERE tg.id = at.tag_id OFFSET 0) t "
            f"WHERE {match('at.article_id')}) x")


def header_facts(kind, match, counts="group", tags="hash"):
    fc, tg = favorite_counts(match, counts), tag_names(match, tags)
    return f"""SELECT {HEADER_COLUMNS},
       {COUNTS[kind]} AS n, fc.ids, fc.cnts,
       ARRAY(SELECT f.article_id FROM favorites f WHERE f.user_id = $1 AND {match('f.article_id')}) AS my_favs,
       {"'{}'::bigint[]" if kind == "feed" else FOLLOWEES} AS my_follows, tg.aids, tg.names
  FROM r
  LEFT JOIN LATERAL ({fc}) fc ON true
  LEFT JOIN LATERAL ({tg}) tg ON true"""


def header_list(kind, counts="group", tags="hash"):
    # Every existing id between the global page's min and max is on the page, so its ranges are exact; other kinds use = ANY(ids).
    dense = kind == "global"
    ranges = "r AS MATERIALIZED (SELECT min(id) AS lo, max(id) AS hi FROM p)" if dense else "r AS MATERIALIZED (SELECT coalesce(array_agg(id), '{}') AS ids FROM p)"
    return (f"WITH {PREFIXES[kind] + ', ' if kind in PREFIXES else ''}p AS MATERIALIZED (SELECT id FROM ({PAGES[kind]}) b LIMIT $2), {ranges} " +
            header_facts(kind, (lambda column: f"{column} BETWEEN r.lo AND r.hi") if dense else (lambda column: f"{column} = ANY(r.ids)"), counts, tags) +
            " UNION ALL SELECT 1, a.id, a.author_id, a.slug, a.title, a.description, a.created_at, a.updated_at, u.username, u.bio, u.image, "
            "NULL::bigint, NULL::bigint[], NULL::bigint[], NULL::bigint[], NULL::bigint[], NULL::bigint[], NULL::text[] "
            "FROM p JOIN articles a ON a.id = p.id CROSS JOIN LATERAL (SELECT username, bio, image FROM users WHERE id = a.author_id OFFSET 0) u")


def per_row_list_dedup(kind):
    # E10: v1 with the page materialized and one users look-up per distinct author on it (feed pages repeat authors).
    following = "true" if kind == "feed" else "EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = p.author_id)"
    return (f"WITH {PREFIXES[kind] + ', ' if kind in PREFIXES else ''}"
            "p AS MATERIALIZED (SELECT a.id, a.author_id, a.slug, a.title, a.description, a.created_at, a.updated_at "
            f"FROM ({PAGES[kind]}) b JOIN articles a ON a.id = b.id ORDER BY a.id DESC LIMIT $2), "
            "au AS MATERIALIZED (SELECT u.id, u.username, u.bio, u.image FROM (SELECT DISTINCT author_id FROM p) d "
            "CROSS JOIN LATERAL (SELECT id, username, bio, image FROM users WHERE id = d.author_id OFFSET 0) u) "
            "SELECT c.n, p.id, p.author_id, p.slug, p.title, p.description, p.created_at, p.updated_at, "
            "ARRAY(SELECT (SELECT t.name FROM tags t WHERE t.id = at.tag_id) FROM article_tags at WHERE at.article_id = p.id ORDER BY 1) AS tag_list, "
            "(SELECT count(*) FROM favorites f WHERE f.article_id = p.id) AS favorites_count, "
            "EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = $1 AND f.article_id = p.id) AS favorited, "
            f"au.username, au.bio, au.image, {following} AS following "
            f"FROM (SELECT {COUNTS[kind]} AS n) c LEFT JOIN p ON true LEFT JOIN au ON au.id = p.author_id "
            "ORDER BY p.id DESC")


V1 = {kind: per_row_list(kind) for kind in PAGES}
V2 = {kind: header_list(kind) for kind in PAGES}
V1D = {kind: per_row_list_dedup(kind) for kind in PAGES}                 # E10
V2C = {kind: header_list(kind, counts="raw") for kind in PAGES}          # E7
V2T = {kind: header_list(kind, tags="probe") for kind in PAGES}          # E8

# Shared lists (after PR #7): one execution answers several list requests of one kind. The feed is left out: its
# pages depend on the viewer. Element k of the arrays is request k (ord from 1): $1 viewer, $2 limit, $3 offset,
# $4 filter. Each distinct filter is looked up and counted once (fn), each distinct page is read once (pg), and each
# distinct article on those pages is read once (k = 1) with its favorites count and tag names in one facts row
# (k = 2). Each request's own header row (k = 0) carries its count, its page's ids and its viewer's favorites and
# follows. C rebuilds every request's v2 result from these rows (api_articles.c).
SHARED_REQUESTS = "unnest($1::bigint[], $2::int[], $3::int[], $4::text[]) WITH ORDINALITY AS r(viewer, lim, off, filter, ord)"
FILTER_IDS = {"global": "NULL::bigint", "tag": "(SELECT t.id FROM tags t WHERE t.name = fv.filter)",
              "author": "(SELECT au.id FROM users au WHERE au.username = fv.filter)",
              "favorited": "(SELECT fu.id FROM users fu WHERE fu.username = fv.filter)"}


def shared_list(kind):
    def by_filter(sql, alias):
        for lookup in (TAG_ID, AUTHOR_ID, FAN_ID):
            sql = sql.replace(lookup, f"{alias}.fid")
        return sql
    match = lambda column: f"{column} = ANY(s.ids)"  # noqa: E731
    return ("WITH r AS MATERIALIZED (SELECT * FROM " + SHARED_REQUESTS + "), "
            f"fi AS MATERIALIZED (SELECT fv.filter, {FILTER_IDS[kind]} AS fid FROM (SELECT DISTINCT filter FROM r) fv), "
            f"fn AS MATERIALIZED (SELECT fi.filter, fi.fid, {by_filter(COUNTS[kind], 'fi')} AS n FROM fi), "
            "pk AS MATERIALIZED (SELECT DISTINCT lim, off, filter FROM r), "
            f"pg AS MATERIALIZED (SELECT pk.lim, pk.off, pk.filter, fn.n, ARRAY(SELECT b.id FROM ({by_filter(PAGES[kind], 'fn').replace('$3', 'pk.off')}) b "
            "LIMIT pk.lim) AS ids FROM pk JOIN fn ON fn.filter IS NOT DISTINCT FROM pk.filter), "
            "ids AS MATERIALIZED (SELECT DISTINCT p.id FROM pg CROSS JOIN LATERAL unnest(pg.ids) AS p(id)), "
            "s AS MATERIALIZED (SELECT coalesce(array_agg(id), '{}') AS ids FROM ids) "
            f"SELECT {HEADER_COLUMNS}, pg.n, NULL::bigint[] AS fc_ids, NULL::bigint[] AS fc_cnts, "
            "ARRAY(SELECT fa.article_id FROM favorites fa WHERE fa.user_id = r.viewer AND fa.article_id = ANY(pg.ids)) AS my_favs, "
            "ARRAY(SELECT fo.followee_id FROM follows fo WHERE fo.follower_id = r.viewer) AS my_follows, "
            "NULL::bigint[] AS tg_aids, NULL::text[] AS tg_names, r.ord, pg.ids AS page "
            "FROM r JOIN pg ON pg.lim = r.lim AND pg.off = r.off AND pg.filter IS NOT DISTINCT FROM r.filter "
            "UNION ALL SELECT 2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, fc.ids, fc.cnts, NULL, NULL, tg.aids, tg.names, NULL, NULL "
            f"FROM s LEFT JOIN LATERAL ({favorite_counts(match)}) fc ON true LEFT JOIN LATERAL ({tag_names(match, 'probe')}) tg ON true "
            "UNION ALL SELECT 1, a.id, a.author_id, a.slug, a.title, a.description, a.created_at, a.updated_at, u.username, u.bio, u.image, "
            "NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL "
            "FROM ids JOIN articles a ON a.id = ids.id CROSS JOIN LATERAL (SELECT username, bio, image FROM users WHERE id = a.author_id OFFSET 0) u")


SHARED = {kind: shared_list(kind) for kind in PAGES if kind != "feed"}
SHARED_SIGNATURE = "(bigint[], int[], int[], text[])"

# $2 is the id parsed from the slug's trailing "-<id>"; COLLATE "C" stops the planner from preferring the slug index.
ARTICLE = """SELECT a.title, a.description, a.body, a.created_at, a.updated_at,
       ARRAY(SELECT (SELECT t.name FROM tags t WHERE t.id = at.tag_id) FROM article_tags at WHERE at.article_id = a.id ORDER BY 1) AS tag_list,
       (SELECT count(*) FROM favorites f WHERE f.article_id = a.id) AS favorites_count,
       EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = $1 AND f.article_id = a.id) AS favorited,
       u.username, u.bio, u.image,
       EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = a.author_id) AS following
  FROM articles a JOIN users u ON u.id = a.author_id
 WHERE a.id = $2 AND a.slug = $3::text COLLATE "C\""""

# No header row means 404. The header carries the viewer's follow set; C computes each comment's following flag.
COMMENTS = """SELECT 0 AS k, NULL::bigint AS id, NULL::text AS body, NULL::timestamptz AS created_at, NULL::timestamptz AS updated_at,
       NULL::bigint AS author_id, NULL::text AS username, NULL::text AS bio, NULL::text AS image,
       ARRAY(SELECT fo.followee_id FROM follows fo WHERE fo.follower_id = $1) AS my_follows
  FROM articles a WHERE a.id = $2 AND a.slug = $3::text COLLATE "C"
UNION ALL
SELECT 1, c.id, c.body, c.created_at, c.updated_at, c.author_id, u.username, u.bio, u.image, NULL
  FROM (SELECT id, body, created_at, updated_at, author_id FROM comments
         WHERE article_id = $2 ORDER BY created_at DESC, id DESC LIMIT 100) c
  CROSS JOIN LATERAL (SELECT username, bio, image FROM users WHERE id = c.author_id OFFSET 0) u"""

TAG_NAME = "(SELECT tg.name FROM tags tg WHERE tg.id = at.tag_id)"
OWN_FAVORITE = "EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = $1 AND f.article_id = a.id)"
FIND = 'WITH t AS (SELECT id, author_id FROM articles WHERE id = $2 AND slug = $3::text COLLATE "C")'


def article_columns(alias, count_sql, favorited_sql):
    return (f"{alias}.title, {alias}.description, {alias}.body, {alias}.created_at, {alias}.updated_at, "
            f"ARRAY(SELECT {TAG_NAME} FROM article_tags at WHERE at.article_id = {alias}.id ORDER BY 1) AS tag_list, "
            f"{count_sql} AS favorites_count, {favorited_sql} AS favorited, us.username, us.bio, us.image, "
            f"EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = {alias}.author_id) AS following")


WRITES = {
    # The SELECT sees the statement's snapshot (before the write), so the count is corrected by the viewer's own row.
    "fav_add": (FIND + ", w AS (INSERT INTO favorites (user_id, article_id) SELECT $1, id FROM t ON CONFLICT DO NOTHING) SELECT " +
                article_columns("a", f"(SELECT count(*) FROM favorites f WHERE f.article_id = a.id) + (NOT {OWN_FAVORITE})::int", "true") +
                " FROM articles a JOIN users us ON us.id = a.author_id WHERE a.id = (SELECT id FROM t)"),
    "fav_del": (FIND + ", w AS (DELETE FROM favorites WHERE user_id = $1 AND article_id = (SELECT id FROM t)) SELECT " +
                article_columns("a", f"(SELECT count(*) FROM favorites f WHERE f.article_id = a.id) - ({OWN_FAVORITE})::int", "false") +
                " FROM articles a JOIN users us ON us.id = a.author_id WHERE a.id = (SELECT id FROM t)"),
    "follow": ("WITH t AS (SELECT id, username, bio, image FROM users WHERE username = $2), "
               "w AS (INSERT INTO follows (follower_id, followee_id) SELECT $1, id FROM t ON CONFLICT DO NOTHING) SELECT username, bio, image FROM t"),
    "unfollow": ("WITH t AS (SELECT id, username, bio, image FROM users WHERE username = $2), "
                 "w AS (DELETE FROM follows WHERE follower_id = $1 AND followee_id = (SELECT id FROM t)) SELECT username, bio, image FROM t"),
    "comment_add": ('WITH c AS (INSERT INTO comments (article_id, author_id, body) SELECT a.id, $1, $4 FROM articles a '
                    'WHERE a.id = $2 AND a.slug = $3::text COLLATE "C" RETURNING id, body, created_at, updated_at) '
                    "SELECT c.id, c.body, c.created_at, c.updated_at, u.username, u.bio, u.image FROM c CROSS JOIN users u WHERE u.id = $1"),
    "comment_del": ('WITH t AS (SELECT c.id, c.author_id FROM comments c JOIN articles a ON a.id = c.article_id '
                    'WHERE c.id = $4 AND a.id = $2 AND a.slug = $3::text COLLATE "C"), '
                    "d AS (DELETE FROM comments WHERE id = (SELECT id FROM t WHERE author_id = $1)) SELECT author_id FROM t"),
    # $5 is the slug prefix computed in C (byte-identical SLUGIFY of $2), $6 the tag list, which may be empty.
    "article_add": ("WITH n AS (SELECT nextval(pg_get_serial_sequence('articles', 'id')) AS id), "
                    "a AS (INSERT INTO articles (id, slug, author_id, title, description, body) SELECT n.id, $5 || '-' || n.id, $1, $2, $3, $4 FROM n "
                    "RETURNING id, slug, title, description, body, created_at, updated_at, author_id), "
                    "want AS (SELECT DISTINCT unnest($6::text[]) AS name), "
                    "existing AS MATERIALIZED (SELECT t.id, t.name FROM tags t WHERE t.name IN (SELECT name FROM want)), "
                    "ins AS (INSERT INTO tags (name) SELECT w.name FROM want w WHERE NOT EXISTS (SELECT 1 FROM existing e WHERE e.name = w.name) "
                    "ORDER BY w.name ON CONFLICT (name) DO UPDATE SET name = EXCLUDED.name RETURNING id, name), "
                    "alltags AS (SELECT id, name FROM existing UNION ALL SELECT id, name FROM ins), "
                    "x AS (INSERT INTO article_tags (article_id, tag_id) SELECT a.id, alltags.id FROM a, alltags) "
                    "SELECT a.slug, a.title, a.description, a.body, a.created_at, a.updated_at, ARRAY(SELECT name FROM alltags ORDER BY name), "
                    "0::bigint, false, u.username, u.bio, u.image, "
                    "EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = $1) FROM a CROSS JOIN users u WHERE u.id = $1"),
    # $7 is the C-computed slug prefix of the new title $4 and is unused when $4 is NULL.
    "article_put": (FIND + ", u AS (UPDATE articles SET title = COALESCE($4, title), description = COALESCE($5, description), body = COALESCE($6, body), "
                    "slug = CASE WHEN $4::text IS NULL THEN slug ELSE $7 || '-' || id END, updated_at = now() "
                    "WHERE id = (SELECT id FROM t WHERE author_id = $1) RETURNING id, slug, title, description, body, created_at, updated_at, author_id) "
                    "SELECT t.author_id, x.* FROM t LEFT JOIN LATERAL (SELECT u.slug, " +
                    article_columns("u", "(SELECT count(*) FROM favorites f WHERE f.article_id = u.id)",
                                    "EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = $1 AND f.article_id = u.id)") +
                    " FROM u JOIN users us ON us.id = u.author_id) x ON true"),
    "article_del": (FIND + ", d AS (DELETE FROM articles WHERE id = (SELECT id FROM t WHERE author_id = $1)) SELECT author_id FROM t"),
    "tags": "SELECT ARRAY(SELECT name FROM tags ORDER BY name)",
}

SIGNATURES = {
    "fav_add": "(bigint, bigint, text)", "fav_del": "(bigint, bigint, text)", "follow": "(bigint, text)", "unfollow": "(bigint, text)",
    "comment_add": "(bigint, bigint, text, text)", "comment_del": "(bigint, bigint, text, bigint)",
    "article_add": "(bigint, text, text, text, text, text[])", "article_put": "(bigint, bigint, text, text, text, text, text)",
    "article_del": "(bigint, bigint, text)", "tags": "",
}

# Users and profiles: the SPEC.md statements minus the columns the response doesn't need (the id of GET /api/user
# and of the profile). User writes never share a group commit: a unique violation (23505) is an expected 422.
USERS = {
    "user_get": "SELECT username, email, bio, image FROM users WHERE id = $1",
    "user_login": "SELECT id, username, email, bio, image, password_hash FROM users WHERE email = $1",
    "user_register": "INSERT INTO users (username, email, password_hash) VALUES ($1, $2, $3) RETURNING id, username, email, bio, image",
    "user_update": ("UPDATE users SET email = COALESCE($2, email), username = COALESCE($3, username), password_hash = COALESCE($4, password_hash), "
                    "bio = COALESCE($5, bio), image = COALESCE($6, image) WHERE id = $1 RETURNING id, username, email, bio, image"),
    "profile": ("SELECT u.username, u.bio, u.image, EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = u.id) AS following "
                "FROM users u WHERE u.username = $2"),
}

USER_SIGNATURES = {"user_get": "(bigint)", "user_login": "(text)", "user_register": "(text, text, text)",
                   "user_update": "(bigint, text, text, text, text, text)", "profile": "(bigint, text)"}

# SPEC.md reference SQL for the user and profile endpoints.
REFERENCE_USERS = {
    "user_get": "SELECT id, username, email, bio, image FROM users WHERE id = $1",
    "profile": ("SELECT u.id, u.username, u.bio, u.image, EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = $1 AND fo.followee_id = u.id) AS following "
                "FROM users u WHERE u.username = $2"),
}

# The slug-only article lookup, for a slug whose suffix is not "-<id>" (ref_article in check.py).
ARTICLE_BY_SLUG = FULL_SELECT + " WHERE a.slug = $2"

# E2: several concurrent point reads in one statement. Each request is one element of the parameter arrays and gets
# back exactly the rows its own statement would return, tagged with its position (ord, from 1); nothing is shared
# between requests (the tag list is left out on purpose: batching it would coalesce identical reads).
BATCH_ROWS = "unnest($1::bigint[], $2::bigint[], $3::text[]) WITH ORDINALITY AS r(viewer, id, slug, ord)"
BATCHED = {
    "user_get_b": ("SELECT r.ord, u.username, u.email, u.bio, u.image "
                   "FROM unnest($1::bigint[]) WITH ORDINALITY AS r(id, ord) JOIN users u ON u.id = r.id"),
    "profile_b": ("SELECT r.ord, u.username, u.bio, u.image, "
                  "EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = r.viewer AND fo.followee_id = u.id) AS following "
                  "FROM unnest($1::bigint[], $2::text[]) WITH ORDINALITY AS r(viewer, username, ord) JOIN users u ON u.username = r.username"),
    "article_b": ("SELECT r.ord, a.title, a.description, a.body, a.created_at, a.updated_at, "
                  "ARRAY(SELECT (SELECT t.name FROM tags t WHERE t.id = at.tag_id) FROM article_tags at WHERE at.article_id = a.id ORDER BY 1) AS tag_list, "
                  "(SELECT count(*) FROM favorites f WHERE f.article_id = a.id) AS favorites_count, "
                  "EXISTS (SELECT 1 FROM favorites f WHERE f.user_id = r.viewer AND f.article_id = a.id) AS favorited, "
                  "u.username, u.bio, u.image, "
                  "EXISTS (SELECT 1 FROM follows fo WHERE fo.follower_id = r.viewer AND fo.followee_id = a.author_id) AS following "
                  f"FROM {BATCH_ROWS} JOIN articles a ON a.id = r.id AND a.slug = r.slug COLLATE \"C\" JOIN users u ON u.id = a.author_id"),
    "comments_b": ("SELECT r.ord, 0 AS k, NULL::bigint AS id, NULL::text AS body, NULL::timestamptz AS created_at, NULL::timestamptz AS updated_at, "
                   "NULL::bigint AS author_id, NULL::text AS username, NULL::text AS bio, NULL::text AS image, "
                   "ARRAY(SELECT fo.followee_id FROM follows fo WHERE fo.follower_id = r.viewer) AS my_follows "
                   f"FROM {BATCH_ROWS} JOIN articles a ON a.id = r.id AND a.slug = r.slug COLLATE \"C\" "
                   "UNION ALL "
                   "SELECT r.ord, 1, c.id, c.body, c.created_at, c.updated_at, c.author_id, u.username, u.bio, u.image, NULL "
                   f"FROM {BATCH_ROWS} "
                   "CROSS JOIN LATERAL (SELECT id, body, created_at, updated_at, author_id FROM comments "
                   "WHERE article_id = r.id ORDER BY created_at DESC, id DESC LIMIT 100) c "
                   "CROSS JOIN LATERAL (SELECT username, bio, image FROM users WHERE id = c.author_id OFFSET 0) u"),
}
BATCHED_SIGNATURES = {"user_get_b": "(bigint[])", "profile_b": "(bigint[], text[])", "article_b": "(bigint[], bigint[], text[])",
                      "comments_b": "(bigint[], bigint[], text[])"}
