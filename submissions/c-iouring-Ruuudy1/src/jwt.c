/* jwt.c: HS256 verify and issue. Accepts exactly what the reference (golang-jwt v5, ParseWithClaims into
 * RegisteredClaims with WithValidMethods(HS256) and WithExpirationRequired) accepts: three unpadded base64url
 * segments, alg "HS256", a valid signature, exp in the future, nbf (if present) in the past, and well-typed
 * registered claims. The token's sub must be a positive decimal string. */
#include "conduit.h"

#include <errno.h>
#include <math.h>

static hmac_key key;
/* base64url of {"alg":"HS256","typ":"JWT"}: what the load generator and golang-jwt produce */
static const char HDR[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
#define HDR_N (sizeof(HDR) - 1)
#define TTL_S (30 * 24 * 3600)

void jwt_setup(const char *secret) { hmac_init(&key, secret, strlen(secret)); }

static inline const char *skip_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static bool ieq(const char *a, uint32_t an, const char *b) {
    size_t bn = strlen(b);
    if (an != bn) return false;
    for (uint32_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (x != y) return false;
    }
    return true;
}

/* Iterates members of a validated JSON object. *p starts after '{' or after the previous value. */
static bool next_member(const char **pp, const char *e, char *kbuf, uint32_t kcap, uint32_t *kn, const char **v, const char **ve) {
    const char *p = skip_ws(*pp, e);
    if (p < e && *p == ',') p = skip_ws(p + 1, e);
    if (p >= e || *p != '"') return false;
    const char *ks = p + 1, *kend = json_scan(p, e);
    if (!kend) return false;
    uint32_t raw = (uint32_t)(kend - 1 - ks);
    if (3 * raw + 4 <= kcap) *kn = json_unescape(kbuf, ks, kend - 1);
    else *kn = UINT32_MAX; /* too long to be a registered claim */
    p = skip_ws(kend, e) + 1; /* ':' */
    p = skip_ws(p, e);
    *v = p;
    *ve = json_scan(p, e);
    if (!*ve) return false;
    *pp = *ve;
    return true;
}

/* NumericDate: a JSON number, or a string holding a valid JSON number (json.Number); truncated to seconds. */
static bool numeric_date(const char *v, const char *ve, int64_t *out) {
    char buf[64];
    const char *s = v, *se = ve;
    if (*v == '"') { s = v + 1; se = ve - 1; if (s == se || json_scan(s, se) != se || (*s != '-' && (*s < '0' || *s > '9'))) return false; }
    else if (*v != '-' && (*v < '0' || *v > '9')) return false;
    size_t n = (size_t)(se - s);
    if (n == 0 || n >= sizeof buf) return false;
    memcpy(buf, s, n);
    buf[n] = 0;
    errno = 0;
    double d = strtod(buf, NULL);
    if (errno == ERANGE || !isfinite(d)) return false;
    double t = trunc(d);
    if (t > 9.2e15) t = 9.2e15;
    if (t < -9.2e15) t = -9.2e15;
    *out = (int64_t)t;
    return true;
}

static bool is_string_or_null(const char *v) { return *v == '"' || *v == 'n'; }

/* The general claims path: any member order, case-insensitive names (encoding/json struct decoding). */
static int64_t claims_slow(const char *c, uint32_t cn, int64_t now_s) {
    const char *e = c + cn, *p = skip_ws(c, e);
    if (p >= e || *p != '{' || !json_scan(p, e)) return 0; /* Decoder.Decode: trailing bytes are ignored */
    p++;
    if (*skip_ws(p, e) == '}') return 0;
    char kbuf[64], sub[64];
    int64_t exp = 0, nbf = 0, uid = 0;
    bool have_exp = false, have_nbf = false;
    uint32_t sub_n = 0;
    const char *v, *ve;
    uint32_t kn;
    while (next_member(&p, e, kbuf, sizeof kbuf, &kn, &v, &ve)) {
        if (kn == UINT32_MAX) continue;
        if (ieq(kbuf, kn, "sub")) {
            if (!is_string_or_null(v)) return 0;
            if (*v == '"') { /* null leaves a string field unchanged */
                uint32_t raw = (uint32_t)(ve - v - 2);
                if (3 * raw + 4 > sizeof sub) { sub_n = UINT32_MAX; continue; }
                sub_n = json_unescape(sub, v + 1, ve - 1);
            }
        } else if (ieq(kbuf, kn, "iss") || ieq(kbuf, kn, "jti")) {
            if (!is_string_or_null(v)) return 0;
        } else if (ieq(kbuf, kn, "aud")) {
            if (*v == '[') {
                const char *q = skip_ws(v + 1, ve);
                while (*q != ']') {
                    if (*q != '"') return 0;
                    q = skip_ws(json_scan(q, ve), ve);
                    if (*q == ',') q = skip_ws(q + 1, ve);
                }
            } else if (!is_string_or_null(v)) {
                return 0;
            }
        } else if (ieq(kbuf, kn, "exp") || ieq(kbuf, kn, "nbf") || ieq(kbuf, kn, "iat")) {
            bool isexp = ieq(kbuf, kn, "exp"), isnbf = ieq(kbuf, kn, "nbf");
            if (*v == 'n') { if (isexp) have_exp = false; if (isnbf) have_nbf = false; continue; }
            int64_t t;
            if (!numeric_date(v, ve, &t)) return 0;
            if (isexp) { exp = t; have_exp = true; }
            else if (isnbf) { nbf = t; have_nbf = true; }
        }
    }
    if (!have_exp || !(now_s < exp)) return 0;
    if (have_nbf && now_s < nbf) return 0;
    if (sub_n == UINT32_MAX || !parse_u63(sub, sub_n, &uid) || uid <= 0) return 0;
    return uid;
}

/* The header path for anything but the canonical header: a JSON object whose "alg" is exactly "HS256". */
static bool header_slow(const char *h, uint32_t hn) {
    uint8_t buf[512];
    uint32_t n;
    if (hn > 680 || !b64url_decode(buf, &n, h, hn)) return false;
    const char *e = (const char *)buf + n, *p = skip_ws((const char *)buf, e);
    if (p >= e || *p != '{') return false;
    const char *end = json_scan(p, e);
    if (!end || skip_ws(end, e) != e) return false; /* json.Unmarshal: nothing may follow */
    p++;
    if (*skip_ws(p, e) == '}') return false;
    char kbuf[64], alg[64];
    bool ok = false;
    const char *v, *ve;
    uint32_t kn;
    while (next_member(&p, e, kbuf, sizeof kbuf, &kn, &v, &ve)) {
        if (kn != 3 || memcmp(kbuf, "alg", 3) != 0) continue;   /* map keys match exactly */
        ok = false;
        if (*v == '"' && 3 * (uint32_t)(ve - v - 2) + 4 <= sizeof alg) {
            uint32_t an = json_unescape(alg, v + 1, ve - 1);
            ok = an == 5 && memcmp(alg, "HS256", 5) == 0;
        }
    }
    return ok;
}

static bool digits(const char **pp, const char *e, int64_t *v) {
    const char *p = *pp, *s = p;
    while (p < e && *p >= '0' && *p <= '9') p++;
    if (!parse_u63(s, (uint32_t)(p - s), v)) return false;
    *pp = p;
    return true;
}

static inline bool lit(const char **pp, const char *e, const char *s, size_t n) {
    if ((size_t)(e - *pp) < n || memcmp(*pp, s, n) != 0) return false;
    *pp += n;
    return true;
}
#define LIT(pp, e, s) lit(pp, e, s, sizeof(s) - 1)

int64_t jwt_verify(const char *tok, uint32_t n, int64_t now_ms) {
    const char *e = tok + n;
    const char *d1 = memchr(tok, '.', n);
    if (!d1) return 0;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(e - d1 - 1));
    if (!d2 || memchr(d2 + 1, '.', (size_t)(e - d2 - 1))) return 0;
    uint32_t hn = (uint32_t)(d1 - tok), pn = (uint32_t)(d2 - d1 - 1), sn = (uint32_t)(e - d2 - 1);
    if (!(hn == HDR_N && memcmp(tok, HDR, HDR_N) == 0) && !header_slow(tok, hn)) return 0;
    /* signature */
    uint8_t sig[48], mac[32];
    uint32_t sigl;
    if (sn != 43 || !b64url_decode(sig, &sigl, d2 + 1, sn) || sigl != 32) return 0;
    hmac_sha256(mac, &key, tok, (size_t)(d2 - tok));
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(sig[i] ^ mac[i]);
    if (diff) return 0;
    /* claims */
    uint8_t claims[1024];
    uint32_t cn;
    if (pn > 1360 || !b64url_decode(claims, &cn, d1 + 1, pn)) return 0;
    int64_t now_s = now_ms >= 0 ? now_ms / 1000 : -((-now_ms + 999) / 1000);
    /* fast path: {"sub":"N","iat":N,"exp":N} exactly, as issued here and minted by the load generator */
    const char *p = (const char *)claims, *ce = p + cn;
    int64_t uid, iat, exp;
    if (LIT(&p, ce, "{\"sub\":\"") && digits(&p, ce, &uid) && LIT(&p, ce, "\",\"iat\":") && digits(&p, ce, &iat) &&
        LIT(&p, ce, ",\"exp\":") && digits(&p, ce, &exp) && p + 1 == ce && *p == '}')
        return uid > 0 && now_s < exp ? uid : 0;
    return claims_slow((const char *)claims, cn, now_s);
}

uint32_t jwt_issue(char *dst, int64_t uid, int64_t now_s) {
    char claims[96];
    uint32_t c = 0;
    memcpy(claims, "{\"sub\":\"", 8); c = 8;
    c += fmt_i64(claims + c, uid);
    memcpy(claims + c, "\",\"iat\":", 8); c += 8;
    c += fmt_i64(claims + c, now_s);
    memcpy(claims + c, ",\"exp\":", 7); c += 7;
    c += fmt_i64(claims + c, now_s + TTL_S);
    claims[c++] = '}';
    uint32_t o = HDR_N;
    memcpy(dst, HDR, HDR_N);
    dst[o++] = '.';
    o += b64url_encode(dst + o, (const uint8_t *)claims, c);
    uint8_t mac[32];
    hmac_sha256(mac, &key, dst, o);
    dst[o++] = '.';
    o += b64url_encode(dst + o, mac, 32);
    return o;
}
