/* http.c: HTTP/1.x request parsing (request line plus the few headers we use), chunked request bodies, and
 * query parameters with the semantics of Go's url.ParseQuery as the reference server reads them. */
#include "conduit.h"

static inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool name_is(const char *s, uint32_t n, const char *lit) {
    size_t ln = strlen(lit);
    if (n != ln) return false;
    for (uint32_t i = 0; i < n; i++) if (lower(s[i]) != lit[i]) return false;
    return true;
}

static bool has_token(const char *v, uint32_t n, const char *tok) {
    /* comma-separated list, case-insensitive */
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        uint32_t s = i;
        while (i < n && v[i] != ',') i++;
        uint32_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e > s && name_is(v + s, e - s, tok)) return true;
    }
    return false;
}

static inline bool tchar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

int http_parse(const char *buf, uint32_t n, struct http_req *h, uint32_t max_header) {
    const char *e = buf + n;
    const char *nl = memchr(buf, '\n', n);
    if (!nl) return n > max_header ? -2 : 0;
    /* request line: METHOD SP target SP HTTP/1.x */
    const char *le = nl > buf && nl[-1] == '\r' ? nl - 1 : nl;
    const char *sp1 = memchr(buf, ' ', (size_t)(le - buf));
    if (!sp1 || sp1 == buf) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1));
    if (!sp2 || sp2 == sp1 + 1) return -1;
    if (le - sp2 - 1 != 8 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0 || sp2[8] < '0' || sp2[8] > '9') return -1;
    uint32_t ml = (uint32_t)(sp1 - buf);
    h->method = ml == 3 && !memcmp(buf, "GET", 3) ? M_GET : ml == 4 && !memcmp(buf, "POST", 4) ? M_POST :
                ml == 3 && !memcmp(buf, "PUT", 3) ? M_PUT : ml == 6 && !memcmp(buf, "DELETE", 6) ? M_DELETE :
                ml == 4 && !memcmp(buf, "HEAD", 4) ? M_HEAD : M_OTHER;
    for (const char *q = buf; q < sp1; q++) if (!tchar((unsigned char)*q)) return -1;
    h->target = sp1 + 1;
    h->target_n = (uint32_t)(sp2 - sp1 - 1);
    for (uint32_t i = 0; i < h->target_n; i++) if ((unsigned char)h->target[i] <= ' ' || h->target[i] == 0x7f) return -1;
    h->minor = (uint8_t)(sp2[8] - '0');
    h->keepalive = h->minor >= 1;
    h->expect_continue = h->chunked = h->auth_present = false;
    h->auth = NULL;
    h->auth_n = 0;
    h->content_length = -1;
    bool conn_close = false, conn_keep = false, te = false;
    const char *p = nl + 1;
    for (;;) {
        if ((uint32_t)(p - buf) > max_header) return -2;
        const char *l = memchr(p, '\n', (size_t)(e - p));
        if (!l) return (uint32_t)(e - buf) > max_header ? -2 : 0;
        const char *end = l > p && l[-1] == '\r' ? l - 1 : l;
        if (end == p) { p = l + 1; break; } /* blank line */
        if (*p == ' ' || *p == '\t') return -1; /* obsolete line folding */
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (!colon || colon == p) return -1;
        for (const char *q = p; q < colon; q++) if (!tchar((unsigned char)*q)) return -1;
        const char *v = colon + 1, *ve = end;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        uint32_t nn = (uint32_t)(colon - p), vn = (uint32_t)(ve - v);
        switch (lower(*p)) {
        case 'a':
            if (name_is(p, nn, "authorization") && !h->auth_present && vn) { h->auth_present = true; h->auth = v; h->auth_n = vn; }
            break;
        case 'c':
            if (name_is(p, nn, "content-length")) {
                int64_t cl;
                if (!parse_u63(v, vn, &cl)) return -1;
                if (h->content_length >= 0 && h->content_length != cl) return -1;
                h->content_length = cl;
            } else if (name_is(p, nn, "connection")) {
                if (has_token(v, vn, "close")) conn_close = true;
                if (has_token(v, vn, "keep-alive")) conn_keep = true;
            }
            break;
        case 'e':
            if (name_is(p, nn, "expect") && name_is(v, vn, "100-continue")) h->expect_continue = true;
            break;
        case 't':
            if (name_is(p, nn, "transfer-encoding")) {
                if (te || !name_is(v, vn, "chunked")) return -3;
                te = h->chunked = true;
            }
            break;
        }
        p = l + 1;
    }
    if (h->chunked) h->content_length = -1;
    if (conn_close) h->keepalive = false;
    else if (h->minor == 0) h->keepalive = conn_keep;
    h->header_n = (uint32_t)(p - buf);
    return 1;
}

static inline int hexd(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int http_chunked(const char *src, uint32_t n, char *dst, uint32_t *dn, uint32_t *consumed, uint32_t max_body) {
    uint32_t i = 0, o = 0;
    for (;;) {
        const char *nl = memchr(src + i, '\n', n - i);
        if (!nl) return n - i > 64 ? -1 : 0;
        uint32_t le = (uint32_t)(nl - src);
        uint64_t size = 0;
        uint32_t j = i, digits = 0;
        while (j < le && hexd(src[j]) >= 0) {
            size = size * 16 + (uint64_t)hexd(src[j++]);
            if (++digits > 8) return -1;
        }
        if (!digits) return -1;
        while (j < le && (src[j] == ' ' || src[j] == '\t')) j++;
        if (j < le && src[j] != ';' && !(src[j] == '\r' && j + 1 == le)) return -1;
        i = le + 1;
        if (size == 0) {
            /* trailers until a blank line */
            for (;;) {
                const char *t = memchr(src + i, '\n', n - i);
                if (!t) return 0;
                uint32_t te = (uint32_t)(t - src);
                bool blank = te == i || (te == i + 1 && src[i] == '\r');
                i = te + 1;
                if (blank) break;
            }
            *dn = o;
            *consumed = i;
            return 1;
        }
        if (o + size > max_body) return -1;
        if (n - i < size + 1) return 0;
        memmove(dst + o, src + i, size);
        o += (uint32_t)size;
        i += (uint32_t)size;
        if (src[i] == '\r') {
            if (n - i < 2) return 0;
            if (src[i + 1] != '\n') return -1;
            i += 2;
        } else if (src[i] == '\n') {
            i += 1;
        } else {
            return -1;
        }
    }
}

bool query_get(struct req *r, const char *q, uint32_t qn, const char *key, str_t *out) {
    size_t kl = strlen(key);
    uint32_t i = 0;
    while (i < qn) {
        uint32_t s = i;
        while (i < qn && q[i] != '&') i++;
        uint32_t e = i++;
        if (e == s || memchr(q + s, ';', e - s)) continue; /* Go drops pairs containing ';' */
        const char *eq = memchr(q + s, '=', e - s);
        uint32_t kn = eq ? (uint32_t)(eq - q - s) : e - s;
        char kb[64];
        uint32_t kd;
        if (kn > sizeof kb || !pct_decode(kb, &kd, q + s, kn, true)) continue;
        uint32_t vs = eq ? (uint32_t)(eq - q) + 1 : e, vn = e - vs;
        if (kd != kl || memcmp(kb, key, kl) != 0) {
            continue;
        }
        char *vb = arena_alloc(r, vn + 1);
        uint32_t vd;
        if (!pct_decode(vb, &vd, q + vs, vn, true)) continue; /* an undecodable pair is dropped */
        *out = (str_t){vb, vd};
        return true;
    }
    *out = (str_t){NULL, 0};
    return false;
}
