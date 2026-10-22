/* util.c: logging, the per-thread buffer pool, JSON output helpers, number and time formatting, base64url,
 * slugify, percent-decoding and Postgres binary arrays. */
#include "conduit.h"

#include <emmintrin.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* ---------- logging ---------- */

void log_msg(const char *fmt, ...) {
    char buf[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    ssize_t w = write(2, buf, (size_t)n);
    (void)w;
}

/* ---------- buffer pool ---------- */

#define POOL_MIN_SHIFT 12   /* 4 KiB */
#define POOL_CLASSES 11     /* up to 4 MiB */
#define POOL_KEEP(c) ((c) < 9 ? 512u >> (c) : 1u) /* free buffers kept per class: about 2 MiB each */

struct pool_node { struct pool_node *next; };
static __thread struct { struct pool_node *head[POOL_CLASSES]; uint32_t count[POOL_CLASSES]; } pool;

static int pool_class(uint32_t cap) {
    int c = 0;
    while (c < POOL_CLASSES && (1u << (POOL_MIN_SHIFT + c)) < cap) c++;
    return c;
}

char *pool_alloc(uint32_t *cap) {
    int c = pool_class(*cap);
    if (c >= POOL_CLASSES) {
        char *p = malloc(*cap);
        if (!p) die("out of memory (%u bytes)", *cap);
        return p;
    }
    *cap = 1u << (POOL_MIN_SHIFT + c);
    struct pool_node *n = pool.head[c];
    if (n) {
        pool.head[c] = n->next;
        pool.count[c]--;
        return (char *)n;
    }
    char *p = malloc(*cap);
    if (!p) die("out of memory (%u bytes)", *cap);
    return p;
}

void pool_free(char *p, uint32_t cap) {
    if (!p) return;
    int c = pool_class(cap);
    if (c >= POOL_CLASSES || (1u << (POOL_MIN_SHIFT + c)) != cap || pool.count[c] >= POOL_KEEP(c)) {
        free(p);
        return;
    }
    struct pool_node *n = (struct pool_node *)p;
    n->next = pool.head[c];
    pool.head[c] = n;
    pool.count[c]++;
}

void ob_init(obuf_t *b, uint32_t cap) {
    b->cap = cap;
    b->p = pool_alloc(&b->cap);
    b->len = 0;
}

void ob_free(obuf_t *b) {
    if (b->p) pool_free(b->p, b->cap);
    b->p = NULL;
    b->len = b->cap = 0;
}

void ob_grow(obuf_t *b, uint32_t need) {
    uint64_t want = (uint64_t)b->len + need;
    uint64_t cap = b->cap ? b->cap : 4096;
    while (cap < want) cap *= 2;
    if (cap > 0x7fffffffu) die("response too large");
    uint32_t c = (uint32_t)cap;
    char *p = pool_alloc(&c);
    if (b->len) memcpy(p, b->p, b->len);
    if (b->p) pool_free(b->p, b->cap);
    b->p = p;
    b->cap = c;
}

/* ---------- request arena ---------- */

struct arena_chunk { struct arena_chunk *next; uint32_t cap, used; char data[] __attribute__((aligned(16))); };

char *arena_alloc(struct req *r, uint32_t n) {
    n = (n + 7) & ~7u;
    if (r->a_used + n <= ARENA_INLINE) {
        char *p = r->a_inline + r->a_used;
        r->a_used += n;
        return p;
    }
    struct arena_chunk *c = r->a_chunks;
    if (!c || c->used + n > c->cap - (uint32_t)sizeof *c) {
        uint32_t cap = c ? c->cap * 2 : 8192;
        while (cap - sizeof *c < n) cap *= 2;
        struct arena_chunk *nc = (struct arena_chunk *)pool_alloc(&cap);
        nc->next = c;
        nc->cap = cap;
        nc->used = 0;
        r->a_chunks = c = nc;
    }
    char *p = c->data + c->used;
    c->used += n;
    return p;
}

void arena_reset(struct req *r) {
    struct arena_chunk *c = r->a_chunks;
    while (c) {
        struct arena_chunk *next = c->next;
        pool_free((char *)c, c->cap);
        c = next;
    }
    r->a_chunks = NULL;
    r->a_used = 0;
}

/* ---------- JSON strings ---------- */

static const char HEX[] = "0123456789abcdef";

/* Escapes '"', '\\' and control characters; everything else (valid UTF-8 from Postgres) is copied. */
void ob_json_str(obuf_t *b, const char *s, uint32_t n) {
    ob_need(b, n + 2);
    char *d = b->p + b->len;
    *d++ = '"';
    uint32_t i = 0;
    const __m128i quote = _mm_set1_epi8('"'), bslash = _mm_set1_epi8('\\'), ctl = _mm_set1_epi8(0x1f);
    for (;;) {
        /* fast path: 16 bytes at a time while none needs escaping */
        while (i + 16 <= n) {
            __m128i x = _mm_loadu_si128((const __m128i *)(s + i));
            __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, quote), _mm_cmpeq_epi8(x, bslash)),
                                     _mm_cmpeq_epi8(_mm_max_epu8(x, ctl), ctl));
            int mask = _mm_movemask_epi8(m);
            if (mask) {
                int k = __builtin_ctz((unsigned)mask);
                memcpy(d, s + i, (size_t)k);
                d += k;
                i += (uint32_t)k;
                goto escape;
            }
            _mm_storeu_si128((__m128i *)d, x);
            d += 16;
            i += 16;
        }
        while (i < n) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\' || c < 0x20) goto escape;
            *d++ = (char)c;
            i++;
        }
        break;
    escape: {
            unsigned char c = (unsigned char)s[i++];
            /* worst case 6 bytes for this char; make room for it plus the rest unescaped */
            b->len = (uint32_t)(d - b->p);
            ob_need(b, 6 + (n - i) + 1);
            d = b->p + b->len;
            *d++ = '\\';
            switch (c) {
            case '"': *d++ = '"'; break;
            case '\\': *d++ = '\\'; break;
            case '\n': *d++ = 'n'; break;
            case '\r': *d++ = 'r'; break;
            case '\t': *d++ = 't'; break;
            case '\b': *d++ = 'b'; break;
            case '\f': *d++ = 'f'; break;
            default:
                *d++ = 'u'; *d++ = '0'; *d++ = '0'; *d++ = HEX[c >> 4]; *d++ = HEX[c & 15];
            }
        }
    }
    *d++ = '"';
    b->len = (uint32_t)(d - b->p);
}

void ob_json_nstr(obuf_t *b, const char *s, int32_t n) {
    if (n < 0) OB_LIT(b, "null");
    else ob_json_str(b, s, (uint32_t)n);
}

/* ---------- numbers ---------- */

uint32_t fmt_i64(char *dst, int64_t v) {
    char tmp[24];
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    int i = 24;
    do { tmp[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) tmp[--i] = '-';
    memcpy(dst, tmp + i, (size_t)(24 - i));
    return (uint32_t)(24 - i);
}

void ob_i64(obuf_t *b, int64_t v) {
    ob_need(b, 20);
    b->len += fmt_i64(b->p + b->len, v);
}

bool parse_u63(const char *s, uint32_t n, int64_t *out) {
    if (n == 0) return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned d = (unsigned char)s[i] - '0';
        if (d > 9) return false;
        if (v > (UINT64_C(0x7fffffffffffffff) - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = (int64_t)v;
    return true;
}

/* ---------- timestamps ---------- */

/* Days since 1970-01-01 to civil date (Howard Hinnant's algorithm). */
static void civil(int64_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

static inline void put2(char *d, unsigned v) { d[0] = (char)('0' + v / 10); d[1] = (char)('0' + v % 10); }

/* Postgres binary timestamptz: microseconds since 2000-01-01 UTC. Truncated (floored) to milliseconds. */
void ob_ts(obuf_t *b, int64_t pg_usec) {
    int64_t ms = pg_usec >= 0 ? pg_usec / 1000 : -((-pg_usec + 999) / 1000);
    ms += INT64_C(946684800000);
    int64_t secs = ms >= 0 ? ms / 1000 : -((-ms + 999) / 1000);
    unsigned msec = (unsigned)(ms - secs * 1000);
    int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    unsigned sod = (unsigned)(secs - days * 86400);
    int y; unsigned mo, d;
    civil(days, &y, &mo, &d);
    ob_need(b, 26);
    char *p = b->p + b->len;
    p[0] = '"';
    unsigned uy = (unsigned)(y < 0 ? 0 : y > 9999 ? 9999 : y);
    p[1] = (char)('0' + uy / 1000); p[2] = (char)('0' + uy / 100 % 10); p[3] = (char)('0' + uy / 10 % 10); p[4] = (char)('0' + uy % 10);
    p[5] = '-'; put2(p + 6, mo); p[8] = '-'; put2(p + 9, d);
    p[11] = 'T'; put2(p + 12, sod / 3600); p[14] = ':'; put2(p + 15, sod / 60 % 60); p[17] = ':'; put2(p + 18, sod % 60);
    p[20] = '.'; p[21] = (char)('0' + msec / 100); p[22] = (char)('0' + msec / 10 % 10); p[23] = (char)('0' + msec % 10);
    p[24] = 'Z'; p[25] = '"';
    b->len += 26;
}

/* ---------- base64url ---------- */

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

uint32_t b64url_encode(char *dst, const uint8_t *src, uint32_t n) {
    uint32_t i = 0, o = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8 | src[i + 2];
        dst[o++] = B64URL[v >> 18]; dst[o++] = B64URL[(v >> 12) & 63]; dst[o++] = B64URL[(v >> 6) & 63]; dst[o++] = B64URL[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        dst[o++] = B64URL[v >> 18]; dst[o++] = B64URL[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)src[i] << 16 | (uint32_t)src[i + 1] << 8;
        dst[o++] = B64URL[v >> 18]; dst[o++] = B64URL[(v >> 12) & 63]; dst[o++] = B64URL[(v >> 6) & 63];
    }
    return o;
}

static int8_t B64URL_DEC[256];
__attribute__((constructor)) static void b64url_table(void) {
    memset(B64URL_DEC, -1, sizeof B64URL_DEC);
    for (int i = 0; i < 64; i++) B64URL_DEC[(unsigned char)B64URL[i]] = (int8_t)i;
}

/* Like Go's base64.RawURLEncoding (not Strict): no padding, a final group of 2 or 3 chars, unused bits ignored. */
bool b64url_decode(uint8_t *dst, uint32_t *dn, const char *src, uint32_t n) {
    if (n % 4 == 1) return false;
    uint32_t o = 0, i = 0;
    for (; i + 4 <= n; i += 4) {
        int a = B64URL_DEC[(unsigned char)src[i]], b = B64URL_DEC[(unsigned char)src[i + 1]];
        int c = B64URL_DEC[(unsigned char)src[i + 2]], d = B64URL_DEC[(unsigned char)src[i + 3]];
        if ((a | b | c | d) < 0) return false;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        dst[o++] = (uint8_t)(v >> 16); dst[o++] = (uint8_t)(v >> 8); dst[o++] = (uint8_t)v;
    }
    if (n - i >= 2) {
        int a = B64URL_DEC[(unsigned char)src[i]], b = B64URL_DEC[(unsigned char)src[i + 1]];
        if ((a | b) < 0) return false;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12;
        if (n - i == 3) {
            int c = B64URL_DEC[(unsigned char)src[i + 2]];
            if (c < 0) return false;
            v |= (uint32_t)c << 6;
            dst[o++] = (uint8_t)(v >> 16); dst[o++] = (uint8_t)(v >> 8);
        } else {
            dst[o++] = (uint8_t)(v >> 16);
        }
    }
    *dn = o;
    return true;
}

/* ---------- slugify ---------- */

/* Byte-identical to trim(both '-' from lower(regexp_replace(x, '[^A-Za-z0-9]+', '-', 'g'))): runs of any other
 * byte (including every byte of a multi-byte character) become one '-', and leading/trailing runs vanish. */
uint32_t slugify(char *dst, const char *src, uint32_t n) {
    uint32_t o = 0;
    bool pending = false;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (alnum) {
            if (pending && o) dst[o++] = '-';
            dst[o++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            pending = false;
        } else {
            pending = true;
        }
    }
    return o;
}

/* ---------- percent-decoding ---------- */

static inline int hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool pct_decode(char *dst, uint32_t *dn, const char *src, uint32_t n, bool plus_space) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == '%') {
            if (i + 2 >= n) return false;
            int h = hexval((unsigned char)src[i + 1]), l = hexval((unsigned char)src[i + 2]);
            if (h < 0 || l < 0) return false;
            dst[o++] = (char)(h << 4 | l);
            i += 2;
        } else if (c == '+' && plus_space) {
            dst[o++] = ' ';
        } else {
            dst[o++] = c;
        }
    }
    *dn = o;
    return true;
}

/* ---------- random and clocks ---------- */

void fill_random(void *dst, size_t n) {
    uint8_t *p = dst;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            die("getrandom: %s", strerror(errno));
        }
        p += r;
        n -= (size_t)r;
    }
}

int64_t clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t realtime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- Postgres binary arrays ---------- */

/* Layout: int32 ndim, int32 has_nulls, int32 elemtype, then per dim (int32 size, int32 lbound), then per item
 * int32 length (-1 = NULL) and the bytes. */
bool pgarr_open(struct pgarr *a, const char *p, int32_t len) {
    a->n = 0;
    a->p = a->end = p;
    if (len < 0) return true;
    if (len < 12) return false;
    int32_t ndim = (int32_t)rd32(p);
    if (ndim == 0) return true;
    if (ndim != 1 || len < 20) return false;
    int32_t size = (int32_t)rd32(p + 12);
    if (size < 0) return false;
    a->n = (uint32_t)size;
    a->p = p + 20;
    a->end = p + len;
    return true;
}

bool pgarr_next(struct pgarr *a, const char **item, int32_t *len) {
    if (a->p + 4 > a->end) return false;
    int32_t n = (int32_t)rd32(a->p);
    a->p += 4;
    if (n < 0) { *item = NULL; *len = -1; return true; }
    if (a->p + n > a->end) return false;
    *item = a->p;
    *len = n;
    a->p += n;
    return true;
}

void ob_json_text_array(obuf_t *b, const char *p, int32_t len) {
    struct pgarr a;
    ob_ch(b, '[');
    if (pgarr_open(&a, p, len)) {
        for (uint32_t i = 0; i < a.n; i++) {
            const char *s;
            int32_t n;
            if (!pgarr_next(&a, &s, &n)) break;
            if (i) ob_ch(b, ',');
            ob_json_nstr(b, s, n);
        }
    }
    ob_ch(b, ']');
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

uint32_t pgarr_i64_sorted(const char *p, int32_t len, int64_t *buf, uint32_t cap, int64_t **out) {
    struct pgarr a;
    *out = buf;
    if (!pgarr_open(&a, p, len) || a.n == 0) return 0;
    int64_t *v = a.n <= cap ? buf : malloc(sizeof(int64_t) * a.n);
    if (!v) die("out of memory");
    uint32_t k = 0;
    bool sorted = true;
    for (uint32_t i = 0; i < a.n; i++) {
        const char *s;
        int32_t n;
        if (!pgarr_next(&a, &s, &n)) break;
        if (n != 8) continue;
        v[k] = (int64_t)rd64(s);
        if (k && v[k] < v[k - 1]) sorted = false;
        k++;
    }
    if (!sorted) qsort(v, k, sizeof(int64_t), cmp_i64);
    *out = v;
    return k;
}

bool sorted_has(const int64_t *a, uint32_t n, int64_t x) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a[mid] < x) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && a[lo] == x;
}
