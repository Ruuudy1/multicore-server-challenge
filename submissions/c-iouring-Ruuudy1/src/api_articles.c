/* api_articles.c: tags, article lists (v1 per-row decoration, v2 header facts joined here), single articles,
 * create / update / delete, and favorites. */
#include "conduit.h"

/* Column positions of the article fields in each statement's rows (-1: not in the row). */
struct acols { int8_t slug, title, desc, body, created, updated, tags, count, favorited, username, bio, image, following; };
static const struct acols COLS_ARTICLE = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};   /* A_GET, F_ADD, F_DEL */
static const struct acols COLS_BY_SLUG = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};  /* A_GET_SLUG */
static const struct acols COLS_ADD = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};       /* A_ADD */
static const struct acols COLS_PUT = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};      /* A_PUT */
static const struct acols COLS_LIST1 = {3, 4, 5, -1, 6, 7, 8, 9, 10, 11, 12, 13, 14};  /* L1_* */

static void put_str(obuf_t *b, str_t s) { ob_json_str(b, s.p, s.n); }

static void render_article(obuf_t *b, const struct pgres *res, uint32_t row, const struct acols *c, str_t slug) {
    OB_LIT(b, "{\"slug\":");
    put_str(b, c->slug >= 0 ? pv_str(res, row, (uint32_t)c->slug) : slug);
    OB_LIT(b, ",\"title\":");
    put_str(b, pv_str(res, row, (uint32_t)c->title));
    OB_LIT(b, ",\"description\":");
    put_str(b, pv_str(res, row, (uint32_t)c->desc));
    if (c->body >= 0) {
        OB_LIT(b, ",\"body\":");
        put_str(b, pv_str(res, row, (uint32_t)c->body));
    }
    OB_LIT(b, ",\"tagList\":");
    ob_json_text_array(b, pv_p(res, row, (uint32_t)c->tags), pv_n(res, row, (uint32_t)c->tags));
    OB_LIT(b, ",\"createdAt\":");
    ob_ts(b, pv_i64(res, row, (uint32_t)c->created));
    OB_LIT(b, ",\"updatedAt\":");
    ob_ts(b, pv_i64(res, row, (uint32_t)c->updated));
    OB_LIT(b, ",\"favorited\":");
    ob_bool(b, pv_bool(res, row, (uint32_t)c->favorited));
    OB_LIT(b, ",\"favoritesCount\":");
    ob_i64(b, pv_i64(res, row, (uint32_t)c->count));
    OB_LIT(b, ",\"author\":");
    render_profile(b, pv_str(res, row, (uint32_t)c->username), pv_str(res, row, (uint32_t)c->bio),
                   pv_str(res, row, (uint32_t)c->image), pv_bool(res, row, (uint32_t)c->following));
    ob_ch(b, '}');
}

static void article_out(struct req *r, const struct pgres *res, const struct acols *c, int status) {
    resp_begin(r, res->bytes + res->bytes / 8 + 512);
    OB_LIT(&r->out, "{\"article\":");
    render_article(&r->out, res, 0, c, r->slug);
    ob_ch(&r->out, '}');
    resp_finish(r, status);
}

static void fail(struct req *r, int status, const char *msg) {
    resp_error(r, status, msg);
    req_complete(r);
}

/* ---------- tags ---------- */

static void tags_done(struct req *r, struct pgres *res) {
    resp_begin(r, res->bytes + 64);
    OB_LIT(&r->out, "{\"tags\":");
    if (res->nrows) ob_json_text_array(&r->out, pv_p(res, 0, 0), pv_n(res, 0, 0));
    else OB_LIT(&r->out, "[]");
    ob_ch(&r->out, '}');
    resp_finish(r, 200);
}

void api_tags(struct req *r) { db_read(r, S_T_LIST, 0, tags_done); }

/* ---------- lists ---------- */

static void list1_done(struct req *r, struct pgres *res) {
    resp_begin(r, res->bytes + res->nrows * 192 + 128);
    OB_LIT(&r->out, "{\"articles\":[");
    int64_t count = 0;
    bool first = true;
    for (uint32_t i = 0; i < res->nrows; i++) {
        count = pv_i64(res, i, 0);
        if (pv_null(res, i, 1)) continue; /* an empty page is one row with only the count */
        if (!first) ob_ch(&r->out, ',');
        first = false;
        render_article(&r->out, res, i, &COLS_LIST1, (str_t){NULL, 0});
    }
    OB_LIT(&r->out, "],\"articlesCount\":");
    ob_i64(&r->out, count);
    ob_ch(&r->out, '}');
    resp_finish(r, 200);
}

struct art { int64_t id; uint32_t row; };
static int art_desc(const void *a, const void *b) {
    int64_t x = ((const struct art *)a)->id, y = ((const struct art *)b)->id;
    return (x < y) - (x > y);
}
struct kv { int64_t k, v; };
static int kv_asc(const void *a, const void *b) {
    int64_t x = ((const struct kv *)a)->k, y = ((const struct kv *)b)->k;
    return (x > y) - (x < y);
}

/* v2: one header row (k = 0) of page facts, plain article rows (k = 1); counts, favorited, following and tags are
 * joined here. Columns: 0 k, 1 id, 2 author_id, 3 slug, 4 title, 5 description, 6 created_at, 7 updated_at,
 * 8 username, 9 bio, 10 image, 11 n, 12 fc.ids, 13 fc.cnts, 14 my_favs, 15 my_follows, 16 tg.aids, 17 tg.names. */
static void list2_done(struct req *r, struct pgres *res) {
    int hdr = -1;
    struct art arts[128];
    uint32_t na = 0;
    for (uint32_t i = 0; i < res->nrows; i++) {
        if (pv_i32(res, i, 0) == 0) hdr = (int)i;
        else if (na < ARRAY_LEN(arts)) arts[na++] = (struct art){pv_i64(res, i, 1), i};
    }
    if (hdr < 0) { resp_error(r, 500, "internal error"); return; }
    uint32_t h = (uint32_t)hdr;
    qsort(arts, na, sizeof arts[0], art_desc);
    /* favorites counts: (id, count) pairs (a shared execution's facts cover every page in it, so any number) */
    struct kv cbuf[128], *counts = cbuf;
    uint32_t nc = 0;
    struct pgarr ids, cnts;
    if (pv_null(res, h, 13)) {
        /* E7: one article id per favorite on the page, counted here */
        int64_t raw_buf[1024], *raw;
        uint32_t nr = pgarr_i64_sorted(pv_p(res, h, 12), pv_n(res, h, 12), raw_buf, ARRAY_LEN(raw_buf), &raw);
        if (nr > ARRAY_LEN(cbuf) && !(counts = malloc(sizeof *counts * nr))) die("out of memory");
        for (uint32_t i = 0; i < nr;) {
            uint32_t j = i;
            while (j < nr && raw[j] == raw[i]) j++;
            counts[nc++] = (struct kv){raw[i], (int64_t)(j - i)};
            i = j;
        }
        if (raw != raw_buf) free(raw);
    } else if (pgarr_open(&ids, pv_p(res, h, 12), pv_n(res, h, 12)) && pgarr_open(&cnts, pv_p(res, h, 13), pv_n(res, h, 13))) {
        if (ids.n > ARRAY_LEN(cbuf) && !(counts = malloc(sizeof *counts * ids.n))) die("out of memory");
        for (uint32_t i = 0; i < ids.n; i++) {
            const char *a, *b;
            int32_t an, bn;
            if (!pgarr_next(&ids, &a, &an) || !pgarr_next(&cnts, &b, &bn) || an != 8 || bn != 8) break;
            counts[nc++] = (struct kv){(int64_t)rd64(a), (int64_t)rd64(b)};
        }
        qsort(counts, nc, sizeof counts[0], kv_asc);
    }
    int64_t fav_buf[128], fol_buf[1024], *favs, *fols;
    uint32_t nfav = pgarr_i64_sorted(pv_p(res, h, 14), pv_n(res, h, 14), fav_buf, ARRAY_LEN(fav_buf), &favs);
    uint32_t nfol = pgarr_i64_sorted(pv_p(res, h, 15), pv_n(res, h, 15), fol_buf, ARRAY_LEN(fol_buf), &fols);
    /* tag (article id, name) pairs, already ordered by name, chained per article of the page in that order */
    struct pgarr aids, names;
    bool have_tags = pgarr_open(&aids, pv_p(res, h, 16), pv_n(res, h, 16)) && pgarr_open(&names, pv_p(res, h, 17), pv_n(res, h, 17));
    uint32_t nt = have_tags ? MIN(aids.n, names.n) : 0;
    struct { const char *s; int32_t n, next; } *tags = nt ? malloc(sizeof *tags * nt) : NULL;
    int32_t thead[ARRAY_LEN(arts)], ttail[ARRAY_LEN(arts)];
    for (uint32_t k = 0; k < na; k++) thead[k] = ttail[k] = -1;
    for (uint32_t i = 0; i < nt; i++) {
        const char *a;
        int32_t an;
        if (!pgarr_next(&aids, &a, &an) || !pgarr_next(&names, &tags[i].s, &tags[i].n) || an != 8) break;
        struct art key = {(int64_t)rd64(a), 0}, *hit = bsearch(&key, arts, na, sizeof arts[0], art_desc);
        if (!hit) continue; /* another page's article */
        uint32_t k = (uint32_t)(hit - arts);
        tags[i].next = -1;
        if (ttail[k] >= 0) tags[ttail[k]].next = (int32_t)i;
        else thead[k] = (int32_t)i;
        ttail[k] = (int32_t)i;
    }
    bool feed = r->kind == K_FEED;
    obuf_t *b = &r->out;
    resp_begin(r, res->bytes + na * 256 + 128);
    OB_LIT(b, "{\"articles\":[");
    for (uint32_t k = 0; k < na; k++) {
        uint32_t row = arts[k].row;
        int64_t id = arts[k].id;
        if (k) ob_ch(b, ',');
        OB_LIT(b, "{\"slug\":");
        put_str(b, pv_str(res, row, 3));
        OB_LIT(b, ",\"title\":");
        put_str(b, pv_str(res, row, 4));
        OB_LIT(b, ",\"description\":");
        put_str(b, pv_str(res, row, 5));
        OB_LIT(b, ",\"tagList\":[");
        for (int32_t i = thead[k]; i >= 0; i = tags[i].next) {
            if (i != thead[k]) ob_ch(b, ',');
            ob_json_nstr(b, tags[i].s, tags[i].n);
        }
        OB_LIT(b, "],\"createdAt\":");
        ob_ts(b, pv_i64(res, row, 6));
        OB_LIT(b, ",\"updatedAt\":");
        ob_ts(b, pv_i64(res, row, 7));
        OB_LIT(b, ",\"favorited\":");
        ob_bool(b, sorted_has(favs, nfav, id));
        OB_LIT(b, ",\"favoritesCount\":");
        struct kv key = {id, 0}, *hit = bsearch(&key, counts, nc, sizeof counts[0], kv_asc);
        ob_i64(b, hit ? hit->v : 0);
        OB_LIT(b, ",\"author\":");
        render_profile(b, pv_str(res, row, 8), pv_str(res, row, 9), pv_str(res, row, 10),
                       feed || sorted_has(fols, nfol, pv_i64(res, row, 2)));
        ob_ch(b, '}');
    }
    OB_LIT(b, "],\"articlesCount\":");
    ob_i64(b, pv_i64(res, h, 11));
    ob_ch(b, '}');
    free(tags);
    if (counts != cbuf) free(counts);
    if (favs != fav_buf) free(favs);
    if (fols != fol_buf) free(fols);
    resp_finish(r, 200);
}

/* ---------- shared lists (item 2) ---------- */

/* A shared execution (statements.py SHARED) has the v2 columns plus 18 ord and 19 page: k = 0 one header row per
 * request (its count, its viewer's favorites and follows, its page's ids), k = 1 every article on any of the pages,
 * k = 2 one row of facts (favorites counts, tag names) for all of them. */
enum { L2_NCOLS = 18, LS_ORD = 18, LS_PAGE = 19, LS_NCOLS = 20 };

static int share_by_id(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

void share_index(struct share_ix *x, const struct pgres *res) {
    x->n = 0;
    x->facts = UINT32_MAX;
    memset(x->hdr, 0xff, sizeof x->hdr);
    if (res->ncols != LS_NCOLS) return;
    if (res->nrows > x->cap) {
        x->cap = res->nrows * 2;
        if (!(x->a = realloc(x->a, sizeof *x->a * x->cap))) die("out of memory");
    }
    for (uint32_t i = 0; i < res->nrows; i++) {
        int32_t k = pv_i32(res, i, 0);
        if (k == 1) {
            x->a[x->n].id = pv_i64(res, i, 1);
            x->a[x->n++].row = i;
        } else if (k == 2) {
            x->facts = i;
        } else if (k == 0) {
            int64_t ord = pv_i64(res, i, LS_ORD);
            if (ord > 0 && ord < (int64_t)ARRAY_LEN(x->hdr)) x->hdr[ord] = i;
        }
    }
    qsort(x->a, x->n, sizeof *x->a, share_by_id);
}

/* Request ord's own v2 rows: its header with the shared facts, then the articles of its own page. */
bool share_view(struct share_ix *x, const struct pgres *res, int64_t ord, struct pgres *view) {
    if (ord <= 0 || ord >= (int64_t)ARRAY_LEN(x->hdr) || x->hdr[ord] == UINT32_MAX || x->facts == UINT32_MAX) return false;
    uint32_t h = x->hdr[ord];
    struct pgarr page;
    if (!pgarr_open(&page, pv_p(res, h, LS_PAGE), pv_n(res, h, LS_PAGE))) return false;
    uint32_t need = (1 + page.n) * L2_NCOLS;
    if (need > x->vcap) {
        x->vcap = need * 2;
        if (!(x->v = realloc(x->v, sizeof *x->v * x->vcap))) die("out of memory");
    }
    memcpy(x->v, pv(res, h, 0), sizeof *x->v * L2_NCOLS);
    static const uint8_t facts[] = {12, 13, 16, 17};   /* favorites ids and counts, tag article ids and names */
    for (size_t i = 0; i < ARRAY_LEN(facts); i++) x->v[facts[i]] = *pv(res, x->facts, facts[i]);
    uint32_t rows = 1;
    const char *item;
    int32_t len;
    while (pgarr_next(&page, &item, &len)) {
        if (len != 8) return false;
        int64_t id = (int64_t)rd64(item);
        const __typeof__(*x->a) *hit = bsearch(&id, x->a, x->n, sizeof *x->a, share_by_id);
        if (!hit) continue; /* gone between the page and the article read: its own statement would skip it too */
        memcpy(&x->v[rows++ * L2_NCOLS], pv(res, hit->row, 0), sizeof *x->v * L2_NCOLS);
    }
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < rows * L2_NCOLS; i++) if (x->v[i].len > 0) bytes += (uint32_t)x->v[i].len;
    *view = (struct pgres){.base = res->base, .v = x->v, .bytes = bytes, .nrows = rows, .ncols = L2_NCOLS};
    return true;
}

void api_list(struct req *r, const char *q, uint32_t qn, bool feed) {
    int64_t limit = 20, offset = 0;
    str_t v;
    if (q && query_get(r, q, qn, "limit", &v) && (!parse_u63(v.p, v.n, &limit) || limit < 1 || limit > 100)) {
        fail(r, 422, "limit must be 1-100 and offset >= 0");
        return;
    }
    if (q && query_get(r, q, qn, "offset", &v) && !parse_u63(v.p, v.n, &offset)) {
        fail(r, 422, "limit must be 1-100 and offset >= 0");
        return;
    }
    int kind = feed ? K_FEED : K_GLOBAL;
    str_t filter = {NULL, 0};
    if (!feed && q) {
        /* the first non-empty filter in the order tag, author, favorited applies */
        if (query_get(r, q, qn, "tag", &v) && v.n) { kind = K_TAG; filter = v; }
        else if (query_get(r, q, qn, "author", &v) && v.n) { kind = K_AUTHOR; filter = v; }
        else if (query_get(r, q, qn, "favorited", &v) && v.n) { kind = K_FAVORITED; filter = v; }
    }
    /* Offsets past any possible table size give the same empty page; this keeps $3 and $2 + $3 in int4. */
    if (offset > 2000000000) offset = 2000000000;
    r->kind = (uint8_t)kind;
    static const int base[] = {[SHAPE_V1] = S_L1_GLOBAL, [SHAPE_V2] = S_L2_GLOBAL, [SHAPE_V1D] = S_L1D_GLOBAL,
                               [SHAPE_V2C] = S_L2C_GLOBAL, [SHAPE_V2T] = S_L2T_GLOBAL};
    int shape = cfg.list_shape[kind];
    bool v2 = shape == SHAPE_V2 || shape == SHAPE_V2C || shape == SHAPE_V2T;
    prm_i64(r, 0, r->viewer);
    prm_i32(r, 1, (int32_t)limit);
    prm_i32(r, 2, (int32_t)offset);
    prm_str(r, 3, filter);
    db_read(r, base[shape] + kind, 4, v2 ? list2_done : list1_done);
}

/* ---------- single article ---------- */

static void get_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "article not found"); return; }
    article_out(r, res, &COLS_ARTICLE, 200);
}

static void get_slug_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "article not found"); return; }
    article_out(r, res, &COLS_BY_SLUG, 200);
}

void api_article_get(struct req *r) {
    prm_i64(r, 0, r->viewer);
    if (r->aid > 0) {
        prm_i64(r, 1, r->aid);
        prm_str(r, 2, r->slug);
        db_read(r, S_A_GET, 3, get_done);
    } else {
        /* the slug has no "-<id>" suffix: the reference lookup by slug alone */
        prm_str(r, 1, r->slug);
        db_read(r, S_A_GET_SLUG, 2, get_slug_done);
    }
}

/* ---------- create ---------- */

static void add_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 500, "internal error"); return; }
    article_out(r, res, &COLS_ADD, 201);
}

void api_article_post(struct req *r, const char *body, uint32_t n) {
    struct jfield f[4] = {{.name = "title"}, {.name = "description"}, {.name = "body"}, {.name = "tagList"}};
    if (!json_wrapper(r, body, n, "article", f, 4)) { fail(r, 422, "invalid body"); return; }
    for (int i = 0; i < 3; i++) {
        if (f[i].type != JF_STRING || !f[i].s.n) { fail(r, 422, "title, description and body are required"); return; }
    }
    str_t *tags = NULL;
    uint32_t ntags = 0;
    if (f[3].type == JF_ARRAY) {
        if (!json_string_array(r, f[3].raw, f[3].raw_n, &tags, &ntags)) {
            fail(r, 422, "tagList must be an array of non-empty strings");
            return;
        }
    } else if (f[3].type != JF_ABSENT && f[3].type != JF_NULL) {
        fail(r, 422, "tagList must be an array of non-empty strings");
        return;
    }
    char *prefix = arena_alloc(r, f[0].s.n + 1);
    uint32_t pn = slugify(prefix, f[0].s.p, f[0].s.n);
    prm_i64(r, 0, r->viewer);
    prm_str(r, 1, f[0].s);
    prm_str(r, 2, f[1].s);
    prm_str(r, 3, f[2].s);
    prm_str(r, 4, (str_t){prefix, pn});
    prm_text_array(r, 5, tags, ntags);
    db_write(r, S_A_ADD, 6, add_done, false);
}

/* ---------- update ---------- */

static void put_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "article not found"); return; }
    if (pv_null(res, 0, 1)) { resp_error(r, 403, "not the author"); return; }
    article_out(r, res, &COLS_PUT, 200);
}

void api_article_put(struct req *r, const char *body, uint32_t n) {
    struct jfield f[3] = {{.name = "title"}, {.name = "description"}, {.name = "body"}};
    if (!json_wrapper(r, body, n, "article", f, 3)) { fail(r, 422, "invalid body"); return; }
    for (int i = 0; i < 3; i++) {
        if (f[i].type == JF_OTHER || f[i].type == JF_ARRAY || (f[i].type == JF_STRING && !f[i].s.n)) {
            fail(r, 422, "title, description and body must be non-empty strings");
            return;
        }
    }
    prm_i64(r, 0, r->viewer);
    prm_i64(r, 1, r->aid);
    prm_str(r, 2, r->slug);
    for (int i = 0; i < 3; i++) prm_str(r, 3 + i, f[i].type == JF_STRING ? f[i].s : (str_t){NULL, 0});
    if (f[0].type == JF_STRING) {
        char *prefix = arena_alloc(r, f[0].s.n + 1);
        prm_str(r, 6, (str_t){prefix, slugify(prefix, f[0].s.p, f[0].s.n)});
    } else {
        prm_null(r, 6);
    }
    db_write(r, S_A_PUT, 7, put_done, false);
}

/* ---------- delete ---------- */

static void del_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "article not found"); return; }
    if (pv_i64(res, 0, 0) != r->viewer) { resp_error(r, 403, "not the author"); return; }
    resp_no_content(r);
}

void api_article_del(struct req *r) {
    prm_i64(r, 0, r->viewer);
    prm_i64(r, 1, r->aid);
    prm_str(r, 2, r->slug);
    db_write(r, S_A_DEL, 3, del_done, false);
}

/* ---------- favorites ---------- */

static void fav_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "article not found"); return; }
    article_out(r, res, &COLS_ARTICLE, 200);
}

void api_favorite(struct req *r, bool add) {
    prm_i64(r, 0, r->viewer);
    prm_i64(r, 1, r->aid);
    prm_str(r, 2, r->slug);
    db_write(r, add ? S_F_ADD : S_F_DEL, 3, fav_done, false);
}
