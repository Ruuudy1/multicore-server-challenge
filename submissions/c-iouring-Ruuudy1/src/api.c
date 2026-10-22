/* api.c: routing (Go 1.22 ServeMux semantics as the reference uses them), authentication, response framing and
 * the dispatch of each request to its handler. */
#include "conduit.h"

/* ---------- responses ---------- */

void resp_begin(struct req *r, uint32_t cap) {
    if (!r->out.p) ob_init(&r->out, cap + HDR_RESERVE);
    else ob_need(&r->out, cap);
    r->out.len = HDR_RESERVE;
}

static const char *status_line(int status, uint32_t *n) {
    const char *s;
    switch (status) {
    case 200: s = "HTTP/1.1 200 OK\r\n"; break;
    case 201: s = "HTTP/1.1 201 Created\r\n"; break;
    case 204: s = "HTTP/1.1 204 No Content\r\n"; break;
    case 400: s = "HTTP/1.1 400 Bad Request\r\n"; break;
    case 401: s = "HTTP/1.1 401 Unauthorized\r\n"; break;
    case 403: s = "HTTP/1.1 403 Forbidden\r\n"; break;
    case 404: s = "HTTP/1.1 404 Not Found\r\n"; break;
    case 413: s = "HTTP/1.1 413 Request Entity Too Large\r\n"; break;
    case 422: s = "HTTP/1.1 422 Unprocessable Entity\r\n"; break;
    case 431: s = "HTTP/1.1 431 Request Header Fields Too Large\r\n"; break;
    case 501: s = "HTTP/1.1 501 Not Implemented\r\n"; break;
    case 503: s = "HTTP/1.1 503 Service Unavailable\r\n"; break;
    default: s = "HTTP/1.1 500 Internal Server Error\r\n"; break;
    }
    *n = (uint32_t)strlen(s);
    return s;
}

#define PUT(lit) do { memcpy(h + n, lit, sizeof(lit) - 1); n += sizeof(lit) - 1; } while (0)

#ifdef WITH_GZIP
#include <libdeflate.h>
/* E9: replaces the body with its gzip encoding when that is smaller. One compressor per loop thread. */
static __thread struct libdeflate_compressor *gz;
static bool gzip_body(struct req *r) {
    uint32_t n = r->out.len - HDR_RESERVE;
    if (!gz && !(gz = libdeflate_alloc_compressor(cfg.gzip_level))) return false;
    size_t bound = libdeflate_gzip_compress_bound(gz, n);
    obuf_t z;
    ob_init(&z, HDR_RESERVE + (uint32_t)bound);
    size_t zn = libdeflate_gzip_compress(gz, r->out.p + HDR_RESERVE, n, z.p + HDR_RESERVE, bound);
    if (!zn || zn >= n) { ob_free(&z); return false; }
    ob_free(&r->out);
    r->out = z;
    r->out.len = HDR_RESERVE + (uint32_t)zn;
    return true;
}
#endif

static void finish(struct req *r, int status, bool body) {
    char h[HDR_RESERVE];
    uint32_t n, sl;
    const char *line = status_line(status, &sl);
    memcpy(h, line, sl);
    n = sl;
    bool gzipped = false;
#ifdef WITH_GZIP
    if (body && cfg.gzip_min && (r->flags & RQ_GZIP) && status < 300 && r->out.len - HDR_RESERVE >= (uint32_t)cfg.gzip_min)
        gzipped = gzip_body(r);
#endif
    if (body) {
        PUT("Content-Type: application/json\r\nContent-Length: ");
        n += fmt_i64(h + n, (int64_t)(r->out.len - HDR_RESERVE));
        PUT("\r\n");
    }
    if (gzipped) PUT("Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
    if (r->flags & RQ_CLOSE) PUT("Connection: close\r\n");
    else if (r->flags & RQ_KEEPALIVE10) PUT("Connection: keep-alive\r\n");
    PUT("\r\n");
    memcpy(r->out.p + HDR_RESERVE - n, h, n);
    r->hdr_off = (uint16_t)(HDR_RESERVE - n);
    r->status = (int16_t)status;
    r->flags |= RQ_READY;
}

void resp_finish(struct req *r, int status) { finish(r, status, true); }

void resp_no_content(struct req *r) {
    resp_begin(r, 0);
    finish(r, 204, false);
}

void resp_error(struct req *r, int status, const char *msg) {
    resp_begin(r, 128);
    OB_LIT(&r->out, "{\"errors\":{\"body\":[");
    ob_json_str(&r->out, msg, (uint32_t)strlen(msg));
    OB_LIT(&r->out, "]}}");
    finish(r, status, true);
}

void resp_reset(struct req *r) {
    ob_free(&r->out);
    r->flags &= (uint16_t)~RQ_READY;
    r->hdr_off = 0;
}

void prm_text_array(struct req *r, int i, const str_t *items, uint32_t n) {
    uint32_t size = n ? 20 : 12;
    for (uint32_t k = 0; k < n; k++) size += 4 + items[k].n;
    char *p = arena_alloc(r, size), *o = p;
    wr32(o, n ? 1 : 0); wr32(o + 4, 0); wr32(o + 8, OID_TEXT); o += 12;
    if (n) { wr32(o, n); wr32(o + 4, 1); o += 8; }
    for (uint32_t k = 0; k < n; k++) {
        wr32(o, items[k].n);
        memcpy(o + 4, items[k].p, items[k].n);
        o += 4 + items[k].n;
    }
    r->prm[i] = (struct pgparam){p, (int32_t)size};
}

/* ---------- routing ---------- */

static bool seg_is(str_t s, const char *lit) { return str_is(s, lit); }

/* Splits the path into percent-decoded segments. Returns the count, or -1 for a bad escape. */
static int split_path(struct req *r, const char *p, uint32_t n, str_t *segs, int max) {
    if (!n || p[0] != '/') return -1;
    int k = 0;
    uint32_t i = 1;
    for (;;) {
        uint32_t s = i;
        while (i < n && p[i] != '/') i++;
        if (k == max) return max + 1; /* too many segments: no route has them */
        uint32_t dn;
        char *d = arena_alloc(r, i - s + 1);
        if (!pct_decode(d, &dn, p + s, i - s, false)) return -1;
        segs[k++] = (str_t){d, dn};
        if (i >= n) break;
        i++;
    }
    return k;
}

int route_match(const struct http_req *h, struct req *r, struct route_args *a) {
    const char *t = h->target;
    uint32_t tn = h->target_n;
    /* absolute-form targets: skip scheme and authority */
    if (tn > 8 && (!memcmp(t, "http://", 7) || !memcmp(t, "https://", 8))) {
        const char *p = memchr(t + 8, '/', tn - 8);
        if (!p) return R_NOT_FOUND;
        tn -= (uint32_t)(p - t);
        t = p;
    }
    const char *q = memchr(t, '?', tn);
    uint32_t pn = q ? (uint32_t)(q - t) : tn;
    a->q = q ? q + 1 : NULL;
    a->qn = q ? tn - pn - 1 : 0;
    const char *hash = memchr(t, '#', pn);
    if (hash) pn = (uint32_t)(hash - t);
    str_t s[6];
    int n = split_path(r, t, pn, s, 5);
    if (n < 0) return -1;
    int m = h->method;
    if (m == M_HEAD) { r->flags |= RQ_HEAD; m = M_GET; }
    if (n < 2 || n > 5 || !seg_is(s[0], "api")) return R_NOT_FOUND;
    for (int i = 0; i < n; i++) if (!s[i].n) return R_NOT_FOUND; /* wildcards don't match empty segments */
    str_t x = s[1];
    if (n == 2) {
        if (seg_is(x, "users")) return m == M_POST ? R_REGISTER : R_NOT_FOUND;
        if (seg_is(x, "user")) return m == M_GET ? R_USER_GET : m == M_PUT ? R_USER_PUT : R_NOT_FOUND;
        if (seg_is(x, "articles")) return m == M_GET ? R_LIST : m == M_POST ? R_ARTICLE_POST : R_NOT_FOUND;
        if (seg_is(x, "tags")) return m == M_GET ? R_TAGS : R_NOT_FOUND;
        return R_NOT_FOUND;
    }
    a->seg[0] = s[2];
    if (seg_is(x, "users")) return n == 3 && seg_is(s[2], "login") && m == M_POST ? R_LOGIN : R_NOT_FOUND;
    if (seg_is(x, "profiles")) {
        if (n == 3) return m == M_GET ? R_PROFILE_GET : R_NOT_FOUND;
        if (n == 4 && seg_is(s[3], "follow")) return m == M_POST ? R_FOLLOW : m == M_DELETE ? R_UNFOLLOW : R_NOT_FOUND;
        return R_NOT_FOUND;
    }
    if (!seg_is(x, "articles")) return R_NOT_FOUND;
    if (n == 3) {
        if (m == M_GET) return seg_is(s[2], "feed") ? R_FEED : R_ARTICLE_GET;
        return m == M_PUT ? R_ARTICLE_PUT : m == M_DELETE ? R_ARTICLE_DEL : R_NOT_FOUND;
    }
    if (n == 4) {
        if (seg_is(s[3], "favorite")) return m == M_POST ? R_FAVORITE : m == M_DELETE ? R_UNFAVORITE : R_NOT_FOUND;
        if (seg_is(s[3], "comments")) return m == M_GET ? R_COMMENTS_GET : m == M_POST ? R_COMMENT_POST : R_NOT_FOUND;
        return R_NOT_FOUND;
    }
    if (seg_is(s[3], "comments") && m == M_DELETE) { a->seg[1] = s[4]; return R_COMMENT_DEL; }
    return R_NOT_FOUND;
}

/* The article id from the slug's trailing "-<digits>", or 0 when there is none (no article has id 0). */
static int64_t slug_id(str_t s) {
    uint32_t i = s.n;
    while (i > 0 && s.p[i - 1] >= '0' && s.p[i - 1] <= '9') i--;
    if (i == s.n || i == 0 || s.p[i - 1] != '-') return 0;
    int64_t v;
    return parse_u63(s.p + i, s.n - i, &v) ? v : 0;
}

void api_dispatch(struct req *r, const struct http_req *h, const char *body, uint32_t body_n) {
    struct route_args a = {0};
    int rt = route_match(h, r, &a);
    if (rt < 0) {
        r->flags |= RQ_CLOSE;
        resp_error(r, 400, "bad request");
        req_complete(r);
        return;
    }
    r->route = (uint8_t)rt;
    switch (rt) {
    case R_NOT_FOUND: resp_error(r, 404, "not found"); req_complete(r); return;
    case R_TAGS: api_tags(r); return;
    case R_REGISTER: api_register(r, body, body_n); return;
    case R_LOGIN: api_login(r, body, body_n); return;
    default: break;
    }
    /* auth: required everywhere else except the four optional read routes */
    bool optional = rt == R_PROFILE_GET || rt == R_LIST || rt == R_ARTICLE_GET || rt == R_COMMENTS_GET;
    int64_t uid = 0;
    if (h->auth_present && h->auth_n > 6 && !memcmp(h->auth, "Token ", 6))
        uid = jwt_verify(h->auth + 6, h->auth_n - 6, loop_now_ms(r->loop));
    if (!uid && (!optional || h->auth_present)) {
        resp_error(r, 401, "unauthorized");
        req_complete(r);
        return;
    }
    r->viewer = uid;
    switch (rt) {
    case R_USER_GET:
    case R_USER_PUT:
        r->token = arena_dup(r, h->auth + 6, h->auth_n - 6);
        if (rt == R_USER_GET) api_user_get(r);
        else api_user_put(r, body, body_n);
        return;
    case R_PROFILE_GET: api_profile_get(r, a.seg[0]); return;
    case R_FOLLOW: api_follow(r, a.seg[0], true); return;
    case R_UNFOLLOW: api_follow(r, a.seg[0], false); return;
    case R_LIST: api_list(r, a.q, a.qn, false); return;
    case R_FEED: api_list(r, a.q, a.qn, true); return;
    case R_ARTICLE_POST: api_article_post(r, body, body_n); return;
    default: break;
    }
    r->slug = a.seg[0];
    r->aid = slug_id(r->slug);
    switch (rt) {
    case R_ARTICLE_GET: api_article_get(r); return;
    case R_ARTICLE_PUT: api_article_put(r, body, body_n); return;
    case R_ARTICLE_DEL: api_article_del(r); return;
    case R_FAVORITE: api_favorite(r, true); return;
    case R_UNFAVORITE: api_favorite(r, false); return;
    case R_COMMENTS_GET: api_comments_get(r); return;
    case R_COMMENT_POST: api_comment_post(r, body, body_n); return;
    case R_COMMENT_DEL: api_comment_del(r, a.seg[1]); return;
    default: resp_error(r, 404, "not found"); req_complete(r); return;
    }
}
