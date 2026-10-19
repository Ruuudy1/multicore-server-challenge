/* api_comments.c: list, create and delete comments. The list statement returns a header row (the article
 * exists and the slug matches; it carries the viewer's follow set) and up to 100 comment rows. */
#include "conduit.h"

static void fail(struct req *r, int status, const char *msg) {
    resp_error(r, status, msg);
    req_complete(r);
}

struct crow { int64_t created, id; uint32_t row; };
static int crow_desc(const void *a, const void *b) {
    const struct crow *x = a, *y = b;
    if (x->created != y->created) return x->created < y->created ? 1 : -1;
    return (x->id < y->id) - (x->id > y->id);
}

static void render_comment(obuf_t *b, const struct pgres *res, uint32_t row, int id, int body, int created, int updated,
                           int user, bool following) {
    OB_LIT(b, "{\"id\":");
    ob_i64(b, pv_i64(res, row, (uint32_t)id));
    OB_LIT(b, ",\"createdAt\":");
    ob_ts(b, pv_i64(res, row, (uint32_t)created));
    OB_LIT(b, ",\"updatedAt\":");
    ob_ts(b, pv_i64(res, row, (uint32_t)updated));
    OB_LIT(b, ",\"body\":");
    str_t s = pv_str(res, row, (uint32_t)body);
    ob_json_str(b, s.p, s.n);
    OB_LIT(b, ",\"author\":");
    render_profile(b, pv_str(res, row, (uint32_t)user), pv_str(res, row, (uint32_t)user + 1), pv_str(res, row, (uint32_t)user + 2), following);
    ob_ch(b, '}');
}

/* Columns: 0 k, 1 id, 2 body, 3 created_at, 4 updated_at, 5 author_id, 6 username, 7 bio, 8 image, 9 my_follows. */
static void list_done(struct req *r, struct pgres *res) {
    int hdr = -1;
    struct crow rows[128];
    uint32_t n = 0;
    bool sorted = true;
    for (uint32_t i = 0; i < res->nrows; i++) {
        if (pv_i32(res, i, 0) == 0) { hdr = (int)i; continue; }
        if (n == ARRAY_LEN(rows)) continue;
        rows[n] = (struct crow){pv_i64(res, i, 3), pv_i64(res, i, 1), i};
        if (n && crow_desc(&rows[n - 1], &rows[n]) > 0) sorted = false;
        n++;
    }
    if (hdr < 0) { resp_error(r, 404, "article not found"); return; } /* no article with this id and slug */
    if (!sorted) qsort(rows, n, sizeof rows[0], crow_desc);
    int64_t buf[1024], *follows;
    uint32_t nf = pgarr_i64_sorted(pv_p(res, (uint32_t)hdr, 9), pv_n(res, (uint32_t)hdr, 9), buf, ARRAY_LEN(buf), &follows);
    resp_begin(r, res->bytes + n * 160 + 64);
    OB_LIT(&r->out, "{\"comments\":[");
    for (uint32_t k = 0; k < n; k++) {
        if (k) ob_ch(&r->out, ',');
        uint32_t row = rows[k].row;
        render_comment(&r->out, res, row, 1, 2, 3, 4, 6, sorted_has(follows, nf, pv_i64(res, row, 5)));
    }
    OB_LIT(&r->out, "]}");
    if (follows != buf) free(follows);
    resp_finish(r, 200);
}

void api_comments_get(struct req *r) {
    prm_i64(r, 0, r->viewer);
    prm_i64(r, 1, r->aid);
    prm_str(r, 2, r->slug);
    db_read(r, S_C_LIST, 3, list_done);
}

/* Columns: 0 id, 1 body, 2 created_at, 3 updated_at, 4 username, 5 bio, 6 image. */
static void add_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "article not found"); return; }
    resp_begin(r, res->bytes + 256);
    OB_LIT(&r->out, "{\"comment\":");
    render_comment(&r->out, res, 0, 0, 1, 2, 3, 4, false);
    ob_ch(&r->out, '}');
    resp_finish(r, 201);
}

void api_comment_post(struct req *r, const char *body, uint32_t n) {
    struct jfield f[1] = {{.name = "body"}};
    if (!json_wrapper(r, body, n, "comment", f, 1)) { fail(r, 422, "invalid body"); return; }
    if (f[0].type != JF_STRING || !f[0].s.n) { fail(r, 422, "body is required"); return; }
    prm_i64(r, 0, r->viewer);
    prm_i64(r, 1, r->aid);
    prm_str(r, 2, r->slug);
    prm_str(r, 3, f[0].s);
    db_write(r, S_C_ADD, 4, add_done, false);
}

static void del_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "comment not found"); return; }
    if (pv_i64(res, 0, 0) != r->viewer) { resp_error(r, 403, "not the author"); return; }
    resp_no_content(r);
}

void api_comment_del(struct req *r, str_t id) {
    int64_t cid;
    if (!parse_u63(id.p, id.n, &cid)) { fail(r, 404, "comment not found"); return; }
    prm_i64(r, 0, r->viewer);
    prm_i64(r, 1, r->aid);
    prm_str(r, 2, r->slug);
    prm_i64(r, 3, cid);
    db_write(r, S_C_DEL, 4, del_done, false);
}
