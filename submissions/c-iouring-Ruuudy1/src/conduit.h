/* conduit.h: shared declarations for the C + io_uring Conduit server (see README.md for the design). */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---------- configuration (main.c) ---------- */

enum { K_GLOBAL, K_TAG, K_AUTHOR, K_FAVORITED, K_FEED, K_KINDS };
/* List SQL shapes: v1 per-row decoration (default), v2 header facts, and the E10/E7/E8 variants. */
enum { SHAPE_V1, SHAPE_V2, SHAPE_V1D, SHAPE_V2C, SHAPE_V2T };

struct config {
    const char *database_url, *jwt_secret, *host, *port;
    int loops, pg_read_conns, pg_write_conns, pg_depth, read_batch, write_batch;
    int uring_busy_cqe, uring_min_cqe, uring_wait_us;
    int bcrypt_threads, bcrypt_lanes, bcrypt_wait_ms, bcrypt_nice;
    int list_shape[K_KINDS]; /* SHAPE_*: the list SQL per kind */
    int read_snapshot;       /* 1: read connections use repeatable read (one snapshot per batch) */
    int e2_batch, e2_min;    /* E2: group concurrent point reads of one statement into one execution */
    int tag_share;           /* item 1: one execution of the tag list answers every tag list request in a batch */
    int list_share;          /* item 2: one execution answers a batch's v2 list requests of one kind */
    int share_min;           /* item 2: requests needed for a shared list execution (1 forces it, for tests) */
    int gather;              /* item 3: extra queued requests a batch may take to grow its shared executions */
    int gzip_min, gzip_level;/* E9: gzip JSON bodies of at least gzip_min bytes (0 = off; needs a GZIP=1 build) */
    int stats;
    uint32_t max_header, max_body;
};
extern struct config cfg;

/* ---------- logging (util.c) ---------- */

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define die(...) do { log_msg(__VA_ARGS__); exit(1); } while (0)

/* ---------- strings and byte order ---------- */

typedef struct { const char *p; uint32_t n; } str_t; /* p == NULL: absent / SQL NULL */

static inline bool str_is(str_t s, const char *lit) { size_t n = strlen(lit); return s.p && s.n == n && memcmp(s.p, lit, n) == 0; }
static inline uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return __builtin_bswap16(v); }
static inline uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline void wr16(void *p, uint16_t v) { v = __builtin_bswap16(v); memcpy(p, &v, 2); }
static inline void wr32(void *p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline void wr64(void *p, uint64_t v) { v = __builtin_bswap64(v); memcpy(p, &v, 8); }

/* ---------- buffer pool and output buffers (util.c) ---------- */

/* Thread-local size classes from 4 KiB to 4 MiB; larger requests fall through to malloc. */
char *pool_alloc(uint32_t *cap);
void pool_free(char *p, uint32_t cap);

typedef struct { char *p; uint32_t len, cap; } obuf_t;

void ob_init(obuf_t *b, uint32_t cap);
void ob_free(obuf_t *b);
void ob_grow(obuf_t *b, uint32_t need);
static inline void ob_need(obuf_t *b, uint32_t n) { if (unlikely(b->len + n > b->cap)) ob_grow(b, n); }
static inline void ob_raw(obuf_t *b, const void *s, uint32_t n) { ob_need(b, n); memcpy(b->p + b->len, s, n); b->len += n; }
static inline void ob_ch(obuf_t *b, char c) { ob_need(b, 1); b->p[b->len++] = c; }
#define OB_LIT(b, s) ob_raw((b), (s), sizeof(s) - 1)
void ob_json_str(obuf_t *b, const char *s, uint32_t n);  /* "..." with JSON escaping */
void ob_json_nstr(obuf_t *b, const char *s, int32_t n);  /* null when n < 0 */
void ob_i64(obuf_t *b, int64_t v);
void ob_ts(obuf_t *b, int64_t pg_usec);                  /* "2025-04-01T12:00:00.000Z", truncated to ms */
static inline void ob_bool(obuf_t *b, bool v) { if (v) OB_LIT(b, "true"); else OB_LIT(b, "false"); }

/* ---------- small utilities (util.c) ---------- */

bool parse_u63(const char *s, uint32_t n, int64_t *out);     /* ASCII digits only, fits in int64 */
uint32_t fmt_i64(char *dst, int64_t v);                      /* dst >= 20 bytes */
uint32_t b64url_encode(char *dst, const uint8_t *src, uint32_t n);
bool b64url_decode(uint8_t *dst, uint32_t *dn, const char *src, uint32_t n); /* unpadded, trailing bits ignored */
uint32_t slugify(char *dst, const char *src, uint32_t n);    /* dst >= n bytes */
/* Percent-decodes into dst (>= n bytes). plus_space: '+' is a space (query strings). False on a bad escape. */
bool pct_decode(char *dst, uint32_t *dn, const char *src, uint32_t n, bool plus_space);
void fill_random(void *dst, size_t n);
int64_t clock_ms(void);       /* CLOCK_MONOTONIC */
int64_t realtime_ms(void);    /* CLOCK_REALTIME_COARSE */

/* ---------- SHA-256, HMAC, PBKDF2 (sha256.c) ---------- */

void sha256_setup(void);                                     /* picks SHA-NI when the CPU has it */
extern bool sha256_has_ni;
void sha256_blocks(uint32_t h[8], const uint8_t *data, size_t nblocks);
void sha256(uint8_t out[32], const void *data, size_t n);
typedef struct { uint32_t inner[8], outer[8]; } hmac_key; /* midstates after the ipad and opad blocks */
void hmac_init(hmac_key *k, const void *key, size_t n);
void hmac_sha256(uint8_t out[32], const hmac_key *k, const void *msg, size_t n);
void pbkdf2_sha256(uint8_t out[32], const void *pw, size_t pwn, const void *salt, size_t saltn, uint32_t iters);

/* ---------- JWT HS256 (jwt.c) ---------- */

void jwt_setup(const char *secret);
/* Returns the user id (> 0) of a valid token, else 0. now_ms is the wall clock. */
int64_t jwt_verify(const char *tok, uint32_t n, int64_t now_ms);
#define JWT_MAX 256
uint32_t jwt_issue(char *dst, int64_t uid, int64_t now_s);   /* dst >= JWT_MAX */

/* ---------- request JSON (json.c) ---------- */

enum { JF_ABSENT, JF_NULL, JF_STRING, JF_ARRAY, JF_OTHER };
struct jfield {
    const char *name;
    uint8_t type;
    str_t s;              /* JF_STRING: decoded value (in the arena) */
    const char *raw;      /* JF_ARRAY: the raw value text */
    uint32_t raw_n;
};
struct req;
/* Validates the whole body as JSON, finds the object under `key` and fills fields (last duplicate wins).
 * False when the body is not valid JSON, not an object, or the wrapper is missing or not an object. */
bool json_wrapper(struct req *r, const char *body, uint32_t n, const char *key, struct jfield *f, int nf);
/* Decodes a JF_ARRAY of non-empty strings; false when any item is anything else. */
bool json_string_array(struct req *r, const char *raw, uint32_t n, str_t **items, uint32_t *count);
/* Validates one JSON value at p (leading whitespace skipped); returns its end, or NULL. */
const char *json_scan(const char *p, const char *e);
/* Decodes the content between the quotes of a validated string; d needs 3 * (e - p) + 4 bytes. */
uint32_t json_unescape(char *d, const char *p, const char *e);

/* ---------- statements (sql.c, sql_gen.h) ---------- */

enum stmt_id {
    S_U_GET, S_U_LOGIN, S_P_GET, S_T_LIST, S_A_GET, S_A_GET_SLUG, S_C_LIST,
    S_L1_GLOBAL, S_L1_TAG, S_L1_AUTHOR, S_L1_FAVORITED, S_L1_FEED,
    S_L2_GLOBAL, S_L2_TAG, S_L2_AUTHOR, S_L2_FAVORITED, S_L2_FEED,
    S_L1D_GLOBAL, S_L1D_TAG, S_L1D_AUTHOR, S_L1D_FAVORITED, S_L1D_FEED,   /* E10: v1, one users look-up per author */
    S_L2C_GLOBAL, S_L2C_TAG, S_L2C_AUTHOR, S_L2C_FAVORITED, S_L2C_FEED,   /* E7: v2, favorites counted in C */
    S_L2T_GLOBAL, S_L2T_TAG, S_L2T_AUTHOR, S_L2T_FAVORITED, S_L2T_FEED,   /* E8: v2, tag names by key probes */
    S_A_GET_B, S_C_LIST_B, S_P_GET_B, S_U_GET_B,                          /* E2: batched point reads */
    S_LS_GLOBAL, S_LS_TAG, S_LS_AUTHOR, S_LS_FAVORITED,                   /* item 2: shared lists */
    S_FIRST_WRITE,
    S_U_REGISTER = S_FIRST_WRITE, S_U_UPDATE, S_P_FOLLOW, S_P_UNFOLLOW, S_C_ADD, S_C_DEL, S_F_ADD, S_F_DEL,
    S_A_ADD, S_A_PUT, S_A_DEL,
    S_COUNT
};
#define MAX_PARAMS 8
#define MAX_COLS 20
struct stmt {
    const char *name, *sql;
    uint8_t nparams, ncols;
    uint32_t ptypes[MAX_PARAMS];
    uint32_t ctypes[MAX_COLS];
};
extern const struct stmt STMTS[S_COUNT];
static inline bool stmt_is_write(int s) { return s >= S_FIRST_WRITE; }

enum { OID_BOOL = 16, OID_INT8 = 20, OID_INT4 = 23, OID_TEXT = 25, OID_INT4_ARRAY = 1007, OID_INT8_ARRAY = 1016,
       OID_TEXT_ARRAY = 1009, OID_TIMESTAMPTZ = 1184 };

/* ---------- query results ---------- */

struct pgval { uint32_t off; int32_t len; }; /* len < 0: NULL */
struct pgres {
    const char *base;          /* row bytes */
    const struct pgval *v;     /* nrows * ncols values */
    uint32_t bytes;            /* size of the row data (for sizing the response) */
    uint32_t nrows;
    uint16_t ncols;
    char sqlstate[6];          /* "" when the statement succeeded */
};
static inline const struct pgval *pv(const struct pgres *r, uint32_t row, uint32_t col) { return &r->v[row * r->ncols + col]; }
static inline bool pv_null(const struct pgres *r, uint32_t row, uint32_t col) { return pv(r, row, col)->len < 0; }
static inline const char *pv_p(const struct pgres *r, uint32_t row, uint32_t col) { return r->base + pv(r, row, col)->off; }
static inline int32_t pv_n(const struct pgres *r, uint32_t row, uint32_t col) { return pv(r, row, col)->len; }
static inline int64_t pv_i64(const struct pgres *r, uint32_t row, uint32_t col) { return pv_null(r, row, col) ? 0 : (int64_t)rd64(pv_p(r, row, col)); }
static inline int32_t pv_i32(const struct pgres *r, uint32_t row, uint32_t col) { return pv_null(r, row, col) ? 0 : (int32_t)rd32(pv_p(r, row, col)); }
static inline bool pv_bool(const struct pgres *r, uint32_t row, uint32_t col) { return !pv_null(r, row, col) && pv_p(r, row, col)[0]; }
static inline str_t pv_str(const struct pgres *r, uint32_t row, uint32_t col) {
    const struct pgval *x = pv(r, row, col);
    return x->len < 0 ? (str_t){NULL, 0} : (str_t){r->base + x->off, (uint32_t)x->len};
}

/* One-dimensional binary arrays (int8[] and text[]). */
struct pgarr { const char *p, *end; uint32_t n; };
bool pgarr_open(struct pgarr *a, const char *p, int32_t len); /* NULL or empty: n = 0 */
bool pgarr_next(struct pgarr *a, const char **item, int32_t *len);
/* Writes a JSON array of strings from a text[] value. */
void ob_json_text_array(obuf_t *b, const char *p, int32_t len);
/* Reads an int8[] into a sorted array (malloc'd only when larger than the caller's buffer). */
uint32_t pgarr_i64_sorted(const char *p, int32_t len, int64_t *buf, uint32_t cap, int64_t **out);
bool sorted_has(const int64_t *a, uint32_t n, int64_t x);

/* ---------- bcrypt (bcrypt.c) ---------- */

enum { BJ_VERIFY, BJ_HASH };
struct loop;
struct bjob {
    struct bjob *next;
    struct loop *loop;          /* where the result goes */
    uint8_t op, ok, keylen;
    uint8_t key[72];            /* the password bytes bcrypt uses (with the trailing NUL when shorter than 72) */
    char hash[64];              /* VERIFY: stored hash; HASH: result ("$2a$10$..." + NUL) */
    int64_t enq_ms;
};
void bcrypt_set_key(struct bjob *j, const char *pw, uint32_t n);
bool bcrypt_selftest(void);
int bcrypt_calibrate(void);                     /* best interleave width for this CPU */
void bcrypt_start(int threads, int lanes, int wait_ms, int nice);
void bcrypt_submit(struct bjob *j);             /* thread-safe; the result is posted to j->loop */
/* Synchronous helpers (tests, tools). */
bool bcrypt_hash_sync(char out[64], const char *pw, uint32_t n, int cost, const uint8_t salt[16]);
bool bcrypt_verify_sync(const char *pw, uint32_t n, const char *hash);

/* ---------- requests ---------- */

enum route {
    R_NOT_FOUND, R_REGISTER, R_LOGIN, R_USER_GET, R_USER_PUT, R_PROFILE_GET, R_FOLLOW, R_UNFOLLOW,
    R_LIST, R_FEED, R_ARTICLE_POST, R_ARTICLE_GET, R_ARTICLE_PUT, R_ARTICLE_DEL, R_FAVORITE, R_UNFAVORITE,
    R_COMMENTS_GET, R_COMMENT_POST, R_COMMENT_DEL, R_TAGS, R_COUNT
};

struct pgparam { const char *p; int32_t len; };   /* len < 0: NULL */
struct req;
typedef void (*pg_done_fn)(struct req *r, struct pgres *res);

#define HDR_RESERVE 192   /* response header space in front of the body (the longest header is 179 bytes) */
#define ARENA_INLINE 1024

enum { RQ_HEAD = 1, RQ_CLOSE = 2, RQ_KEEPALIVE10 = 4, RQ_ORPHAN = 8, RQ_ALONE = 16, RQ_READY = 32, RQ_PENDING = 64,
       RQ_GROUP = 128, RQ_GZIP = 256 };

struct req {
    struct req *next;           /* scheduler queue link */
    struct loop *loop;
    struct req *gnext;          /* E2: the other requests answered by the same execution (RQ_GROUP leader) */
    uint32_t slot, gen;         /* owning client connection */
    uint8_t route, stmt, retries, gsize;
    uint16_t flags;
    uint8_t nparams, kind;
    int16_t status;
    uint16_t hdr_off;           /* where the response header starts in out.p */
    int64_t viewer;             /* token's user id, 0 when anonymous */
    int64_t aid;                /* article id from the slug suffix, 0 when it doesn't parse */
    int64_t t0_ms;              /* arrival (stats) */
    str_t slug, token, s1;      /* request slug, presented token, a route-specific string */
    pg_done_fn done;
    struct pgparam prm[MAX_PARAMS];
    uint8_t ibuf[MAX_PARAMS][8];
    obuf_t out;                 /* the response; the body starts at HDR_RESERVE */
    struct bjob bj;
    struct arena_chunk *a_chunks; /* overflow chunks (pool); pointers into the arena stay valid */
    uint32_t a_used;
    char a_inline[ARENA_INLINE] __attribute__((aligned(16)));
};

/* Per-request arena: 8-byte aligned allocations that live until the request is freed. */
char *arena_alloc(struct req *r, uint32_t n);
void arena_reset(struct req *r);
static inline str_t arena_dup(struct req *r, const char *s, uint32_t n) {
    char *d = arena_alloc(r, n);
    memcpy(d, s, n);
    return (str_t){d, n};
}

static inline void prm_i64(struct req *r, int i, int64_t v) { wr64(r->ibuf[i], (uint64_t)v); r->prm[i] = (struct pgparam){(const char *)r->ibuf[i], 8}; }
static inline void prm_i32(struct req *r, int i, int32_t v) { wr32(r->ibuf[i], (uint32_t)v); r->prm[i] = (struct pgparam){(const char *)r->ibuf[i], 4}; }
static inline void prm_str(struct req *r, int i, str_t s) { r->prm[i] = s.p ? (struct pgparam){s.p, (int32_t)s.n} : (struct pgparam){NULL, -1}; }
static inline void prm_null(struct req *r, int i) { r->prm[i] = (struct pgparam){NULL, -1}; }
void prm_text_array(struct req *r, int i, const str_t *items, uint32_t n);

/* Responses (api.c). The body is written into r->out after HDR_RESERVE; resp_finish adds the header. */
void resp_begin(struct req *r, uint32_t cap);
void resp_finish(struct req *r, int status);           /* JSON body already written */
void resp_error(struct req *r, int status, const char *msg);
void resp_no_content(struct req *r);                   /* 204 */
void resp_reset(struct req *r);

/* Database calls (sched.c): run statement r->stmt with r->prm, then call done on the loop thread. */
void db_read(struct req *r, int stmt, int nparams, pg_done_fn done);
void db_write(struct req *r, int stmt, int nparams, pg_done_fn done, bool alone);

/* Hands a finished response (RQ_READY) to the connection, or frees the request if the client is gone. */
void req_complete(struct req *r);
void req_free(struct req *r);

/* ---------- HTTP (http.c) ---------- */

enum { M_GET, M_POST, M_PUT, M_DELETE, M_HEAD, M_OTHER };
struct http_req {
    uint8_t method, minor;      /* HTTP/1.minor */
    bool keepalive, expect_continue, chunked, auth_present, gzip_ok;
    const char *target; uint32_t target_n;   /* request-target as sent */
    const char *auth; uint32_t auth_n;       /* Authorization value, OWS trimmed */
    int64_t content_length;                  /* -1 when absent */
    uint32_t header_n;                       /* through the blank line */
};
/* 1: headers complete; 0: need more bytes; -1: malformed (400); -2: headers too large (431); -3: not implemented (501). */
int http_parse(const char *buf, uint32_t n, struct http_req *h, uint32_t max_header);
/* Chunked body: 1 complete (body decoded in place into dst, *consumed bytes of src used), 0 need more, -1 malformed. */
int http_chunked(const char *src, uint32_t n, char *dst, uint32_t *dn, uint32_t *consumed, uint32_t max_body);

/* Routing and dispatch of a parsed request (api.c). */
struct route_args { str_t seg[2]; const char *q; uint32_t qn; };
int route_match(const struct http_req *h, struct req *r, struct route_args *a);
void api_dispatch(struct req *r, const struct http_req *h, const char *body, uint32_t body_n);

/* Query parameters with Go url.ParseQuery semantics (first value wins; decoded into the arena). */
bool query_get(struct req *r, const char *q, uint32_t qn, const char *key, str_t *out);

/* ---------- API handlers (api_*.c) ---------- */

void api_register(struct req *r, const char *body, uint32_t n);
void api_login(struct req *r, const char *body, uint32_t n);
void api_user_get(struct req *r);
void api_user_put(struct req *r, const char *body, uint32_t n);
void api_profile_get(struct req *r, str_t username);
void api_follow(struct req *r, str_t username, bool follow);
void api_list(struct req *r, const char *q, uint32_t qn, bool feed);
void api_article_get(struct req *r);
void api_article_post(struct req *r, const char *body, uint32_t n);
void api_article_put(struct req *r, const char *body, uint32_t n);
void api_article_del(struct req *r);
void api_favorite(struct req *r, bool add);
void api_comments_get(struct req *r);
void api_comment_post(struct req *r, const char *body, uint32_t n);
void api_comment_del(struct req *r, str_t id);
void api_tags(struct req *r);
void api_bcrypt_done(struct req *r);   /* a bcrypt job finished (loop thread) */

/* Shared renderers (api_articles.c). */
void render_profile(obuf_t *b, str_t username, str_t bio, str_t image, bool following);

/* Item 2: splitting a shared list execution into each request's own v2 result (api_articles.c). share_index runs
 * once per execution; share_view then gives request ord (its position, from 1) a view of the rows its own v2
 * statement would have returned, valid until the next share_view. */
struct share_ix {
    struct { int64_t id; uint32_t row; } *a;   /* article rows by id */
    uint32_t n, cap, facts;
    uint32_t hdr[256];                         /* header row of each request position */
    struct pgval *v; uint32_t vcap;            /* the current view */
};
void share_index(struct share_ix *x, const struct pgres *res);
bool share_view(struct share_ix *x, const struct pgres *res, int64_t ord, struct pgres *view);

/* ---------- event loop (loop.c) ---------- */

struct loop *loop_create(int idx);
void *loop_run(void *arg);
void loop_post(struct loop *l, void *ptr);    /* any thread: wake the loop with a bcrypt result */
int64_t loop_now_ms(struct loop *l);          /* wall clock, coarse, per pass */
struct req *req_alloc(struct loop *l);
extern struct loop **loops;
extern int nloops;

/* ---------- stats (stats.c) ---------- */

struct lstats {
    uint64_t requests, responses, accepts, closes, cqes, waits, pg_batches, pg_jobs, pg_wbatches, pg_wjobs;
    uint64_t lat_sum_ms, lat_max_ms, bcrypt_jobs, grouped;
    uint32_t open_conns, fifo_len, wq_len;
};
void stats_start(void);
