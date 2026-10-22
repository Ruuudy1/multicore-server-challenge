/* json.c: the request-body parser. It follows Go's encoding/json as the reference server uses it
 * (json.Unmarshal into map[string]json.RawMessage): the whole body must be valid JSON, keys match exactly and
 * the last duplicate wins, invalid UTF-8 and lone surrogates decode to U+FFFD, and nesting stops at 10,000. */
#include "conduit.h"

#define MAX_DEPTH 10000

static inline const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static inline int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

/* Scans a string starting at the opening quote. Returns the position after the closing quote, or NULL.
 * *plain is set when the content needs no decoding (no escapes, only ASCII). */
static const char *scan_string(const char *p, const char *e, bool *plain) {
    bool pl = true;
    p++;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') { *plain = pl; return p + 1; }
        if (c < 0x20) return NULL;
        if (c == '\\') {
            pl = false;
            if (p + 1 >= e) return NULL;
            char x = p[1];
            if (x == 'u') {
                if (p + 6 > e || hex4(p + 2) < 0) return NULL;
                p += 6;
            } else if (x == '"' || x == '\\' || x == '/' || x == 'b' || x == 'f' || x == 'n' || x == 'r' || x == 't') {
                p += 2;
            } else {
                return NULL;
            }
            continue;
        }
        if (c >= 0x80) pl = false;
        p++;
    }
    return NULL;
}

static const char *scan_number(const char *p, const char *e) {
    if (p < e && *p == '-') p++;
    if (p >= e) return NULL;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (p < e && *p >= '0' && *p <= '9') p++; }
    else return NULL;
    if (p < e && *p == '.') {
        p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    return p;
}

static const char *scan_value(const char *p, const char *e, int depth);

static const char *scan_container(const char *p, const char *e, int depth) {
    if (depth > MAX_DEPTH) return NULL;
    bool obj = *p == '{';
    char close = obj ? '}' : ']';
    p = ws(p + 1, e);
    if (p < e && *p == close) return p + 1;
    for (;;) {
        if (obj) {
            bool plain;
            if (p >= e || *p != '"' || !(p = scan_string(p, e, &plain))) return NULL;
            p = ws(p, e);
            if (p >= e || *p != ':') return NULL;
            p = ws(p + 1, e);
        }
        if (!(p = scan_value(p, e, depth + 1))) return NULL;
        p = ws(p, e);
        if (p >= e) return NULL;
        if (*p == close) return p + 1;
        if (*p != ',') return NULL;
        p = ws(p + 1, e);
    }
}

static const char *scan_value(const char *p, const char *e, int depth) {
    if (p >= e) return NULL;
    bool plain;
    switch (*p) {
    case '{': case '[': return scan_container(p, e, depth);
    case '"': return scan_string(p, e, &plain);
    case 't': return e - p >= 4 && memcmp(p, "true", 4) == 0 ? p + 4 : NULL;
    case 'f': return e - p >= 5 && memcmp(p, "false", 5) == 0 ? p + 5 : NULL;
    case 'n': return e - p >= 4 && memcmp(p, "null", 4) == 0 ? p + 4 : NULL;
    default: return scan_number(p, e);
    }
}

/* Go's utf8.DecodeRune: returns the byte length of a valid sequence, or 0 for an invalid one. */
static uint32_t utf8_len(const unsigned char *s, const unsigned char *e) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;
    uint32_t n = c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF5 ? 4 : 0;
    if (!n || s + n > e) return 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c == 0xE0) lo = 0xA0;
    else if (c == 0xED) hi = 0x9F;
    else if (c == 0xF0) lo = 0x90;
    else if (c == 0xF4) hi = 0x8F;
    if (s[1] < lo || s[1] > hi) return 0;
    for (uint32_t i = 2; i < n; i++) if (s[i] < 0x80 || s[i] > 0xBF) return 0;
    return n;
}

static uint32_t put_utf8(char *d, uint32_t cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x80) { d[0] = (char)cp; return 1; }
    if (cp < 0x800) { d[0] = (char)(0xC0 | cp >> 6); d[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { d[0] = (char)(0xE0 | cp >> 12); d[1] = (char)(0x80 | (cp >> 6 & 63)); d[2] = (char)(0x80 | (cp & 63)); return 3; }
    d[0] = (char)(0xF0 | cp >> 18); d[1] = (char)(0x80 | (cp >> 12 & 63)); d[2] = (char)(0x80 | (cp >> 6 & 63)); d[3] = (char)(0x80 | (cp & 63));
    return 4;
}

const char *json_scan(const char *p, const char *e) { return scan_value(ws(p, e), e, 1); }

/* Decodes the content [p, e) of a validated string into d (>= 3 * (e - p) + 4 bytes). Returns the length. */
uint32_t json_unescape(char *d, const char *p, const char *e) {
    char *o = d;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            char x = p[1];
            if (x != 'u') {
                *o++ = x == 'b' ? '\b' : x == 'f' ? '\f' : x == 'n' ? '\n' : x == 'r' ? '\r' : x == 't' ? '\t' : x;
                p += 2;
                continue;
            }
            uint32_t cp = (uint32_t)hex4(p + 2);
            p += 6;
            if (cp >= 0xD800 && cp < 0xE000) {
                /* a high surrogate followed by an escaped low surrogate combines; anything else is U+FFFD */
                if (cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    int lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                        p += 6;
                        o += put_utf8(o, cp);
                        continue;
                    }
                }
                cp = 0xFFFD;
            }
            o += put_utf8(o, cp);
            continue;
        }
        if (c < 0x80) { *o++ = (char)c; p++; continue; }
        uint32_t k = utf8_len((const unsigned char *)p, (const unsigned char *)e);
        if (k) { memcpy(o, p, k); o += k; p += k; }
        else { o += put_utf8(o, 0xFFFD); p++; }
    }
    return (uint32_t)(o - d);
}

/* Decodes the (already validated) string whose content is [p, e) into the arena. */
static str_t decode_string(struct req *r, const char *p, const char *e, bool plain) {
    uint32_t n = (uint32_t)(e - p);
    if (plain) return arena_dup(r, p, n);
    char *d = arena_alloc(r, 3 * n + 4);
    return (str_t){d, json_unescape(d, p, e)};
}

/* Compares a raw key (between quotes) with a name, decoding escapes when present. */
static bool key_is(struct req *r, const char *p, const char *e, bool plain, const char *name) {
    size_t nn = strlen(name);
    if (plain) return (size_t)(e - p) == nn && memcmp(p, name, nn) == 0;
    str_t k = decode_string(r, p, e, false);
    return k.n == nn && memcmp(k.p, name, nn) == 0;
}

bool json_wrapper(struct req *r, const char *body, uint32_t n, const char *key, struct jfield *f, int nf) {
    const char *e = body + n, *p = ws(body, e);
    if (p >= e || *p != '{') return false;
    const char *end = scan_value(p, e, 1);
    if (!end || ws(end, e) != e) return false;
    /* Find the wrapper (the last occurrence wins). The document is valid, so the walk can be light. */
    const char *wv = NULL;
    p = ws(p + 1, e);
    while (*p != '}') {
        bool plain;
        const char *ks = p + 1, *ke = scan_string(p, e, &plain);
        bool match = key_is(r, ks, ke - 1, plain, key);
        p = ws(ke, e) + 1; /* ':' */
        p = ws(p, e);
        const char *v = p;
        p = scan_value(p, e, 2);
        if (match) wv = v;
        p = ws(p, e);
        if (*p == ',') p = ws(p + 1, e);
    }
    if (!wv || *wv != '{') return false;
    for (int i = 0; i < nf; i++) f[i].type = JF_ABSENT;
    p = ws(wv + 1, e);
    while (*p != '}') {
        bool plain;
        const char *ks = p + 1, *ke = scan_string(p, e, &plain);
        int which = -1;
        for (int i = 0; i < nf; i++) if (key_is(r, ks, ke - 1, plain, f[i].name)) { which = i; break; }
        p = ws(ws(ke, e) + 1, e);
        const char *v = p;
        p = scan_value(p, e, 3);
        if (which >= 0) {
            struct jfield *x = &f[which];
            switch (*v) {
            case 'n': x->type = JF_NULL; break;
            case '"': {
                bool pl;
                const char *se = scan_string(v, e, &pl);
                x->type = JF_STRING;
                x->s = decode_string(r, v + 1, se - 1, pl);
                break;
            }
            case '[': x->type = JF_ARRAY; x->raw = v; x->raw_n = (uint32_t)(p - v); break;
            default: x->type = JF_OTHER;
            }
        }
        p = ws(p, e);
        if (*p == ',') p = ws(p + 1, e);
    }
    return true;
}

bool json_string_array(struct req *r, const char *raw, uint32_t n, str_t **items, uint32_t *count) {
    const char *e = raw + n, *p = ws(raw + 1, e);
    uint32_t cap = 1;
    for (uint32_t i = 0; i < n; i++) if (raw[i] == ',') cap++;
    str_t *out = (str_t *)arena_alloc(r, cap * (uint32_t)sizeof(str_t));
    uint32_t k = 0;
    while (p < e && *p != ']') {
        if (*p != '"') return false;
        bool plain;
        const char *se = scan_string(p, e, &plain);
        str_t s = decode_string(r, p + 1, se - 1, plain);
        if (s.n == 0) return false;
        out[k++] = s;
        p = ws(se, e);
        if (p < e && *p == ',') p = ws(p + 1, e);
    }
    *items = out;
    *count = k;
    return true;
}
