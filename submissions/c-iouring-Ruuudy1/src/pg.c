/* pg.c: Postgres connection setup, done with blocking I/O (at startup, and on the connector thread for
 * reconnects): DATABASE_URL, the startup packet with our session settings, SCRAM-SHA-256 (or cleartext/trust),
 * then Parse + Describe of every statement, checking each result layout against the catalog. */
#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct pgconf pgconf;

/* Sent in the startup packet: generic plans every time, no JIT or parallel plans, and a guard against bitmap
 * scans (they cannot stop early); statement and lock timeouts so nothing hangs. */
static const char *const SESSION[][2] = {
    {"plan_cache_mode", "force_generic_plan"}, {"jit", "off"}, {"max_parallel_workers_per_gather", "0"},
    {"enable_bitmapscan", "off"}, {"statement_timeout", "9s"}, {"lock_timeout", "5s"},
    {"client_encoding", "UTF8"}, {"application_name", "conduit-c-iouring"},
};

/* ---------- DATABASE_URL ---------- */

static void url_copy(char *dst, size_t cap, const char *s, size_t n) {
    uint32_t dn = 0;
    char tmp[512];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    if (!pct_decode(tmp, &dn, s, (uint32_t)n, false)) { memcpy(tmp, s, n); dn = (uint32_t)n; }
    if (dn >= cap) dn = (uint32_t)cap - 1;
    memcpy(dst, tmp, dn);
    dst[dn] = 0;
}

bool pg_parse_url(const char *url, struct pgconf *pc) {
    memset(pc, 0, sizeof *pc);
    strcpy(pc->host, "127.0.0.1");
    strcpy(pc->port, "5432");
    const char *p;
    if (!strncmp(url, "postgres://", 11)) p = url + 11;
    else if (!strncmp(url, "postgresql://", 13)) p = url + 13;
    else return false;
    const char *end = p + strcspn(p, "/?");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        url_copy(pc->user, sizeof pc->user, p, (size_t)((colon ? colon : at) - p));
        if (colon) url_copy(pc->password, sizeof pc->password, colon + 1, (size_t)(at - colon - 1));
        p = at + 1;
    }
    const char *hs = p, *he = end, *port = NULL;
    if (*hs == '[') {
        const char *rb = memchr(hs, ']', (size_t)(end - hs));
        if (!rb) return false;
        if (rb + 1 < end && rb[1] == ':') port = rb + 2;
        hs++;
        he = rb;
    } else {
        const char *colon = memchr(hs, ':', (size_t)(end - hs));
        if (colon) { port = colon + 1; he = colon; }
    }
    if (he > hs) url_copy(pc->host, sizeof pc->host, hs, (size_t)(he - hs));
    if (port && end > port) url_copy(pc->port, sizeof pc->port, port, (size_t)(end - port));
    p = end;
    if (*p == '/') {
        const char *ds = ++p;
        p += strcspn(p, "?");
        url_copy(pc->dbname, sizeof pc->dbname, ds, (size_t)(p - ds));
    }
    if (*p == '?') {
        p++;
        while (*p) {
            size_t n = strcspn(p, "&");
            const char *eq = memchr(p, '=', n);
            if (eq) {
                size_t kn = (size_t)(eq - p), vn = n - kn - 1;
                const char *v = eq + 1;
                if (kn == 4 && !memcmp(p, "host", 4)) url_copy(pc->host, sizeof pc->host, v, vn);
                else if (kn == 4 && !memcmp(p, "port", 4)) url_copy(pc->port, sizeof pc->port, v, vn);
                else if (kn == 4 && !memcmp(p, "user", 4)) url_copy(pc->user, sizeof pc->user, v, vn);
                else if (kn == 8 && !memcmp(p, "password", 8)) url_copy(pc->password, sizeof pc->password, v, vn);
                else if (kn == 6 && !memcmp(p, "dbname", 6)) url_copy(pc->dbname, sizeof pc->dbname, v, vn);
            }
            p += n;
            if (*p == '&') p++;
        }
    }
    if (!pc->user[0]) {
        const char *u = getenv("PGUSER");
        if (!u) u = getenv("USER");
        snprintf(pc->user, sizeof pc->user, "%s", u ? u : "postgres");
    }
    if (!pc->dbname[0]) snprintf(pc->dbname, sizeof pc->dbname, "%s", pc->user);
    pc->unix_socket = pc->host[0] == '/';
    return true;
}

/* ---------- blocking message I/O ---------- */

struct bconn { int fd; char *buf; uint32_t len, pos, cap; char *err; size_t errn; };

static bool send_all(struct bconn *b, const void *p, size_t n) {
    const char *s = p;
    while (n) {
        ssize_t w = send(b->fd, s, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            snprintf(b->err, b->errn, "send: %s", strerror(errno));
            return false;
        }
        s += w;
        n -= (size_t)w;
    }
    return true;
}

/* Reads one message; *body points into the buffer until the next call. */
static bool read_msg(struct bconn *b, char *type, const char **body, uint32_t *n) {
    for (;;) {
        if (b->len - b->pos >= 5) {
            uint32_t ml = rd32(b->buf + b->pos + 1);
            if (ml < 4 || ml > (1u << 30)) { snprintf(b->err, b->errn, "bad message length"); return false; }
            if (b->len - b->pos >= 1 + ml) {
                *type = b->buf[b->pos];
                *body = b->buf + b->pos + 5;
                *n = ml - 4;
                b->pos += 1 + ml;
                return true;
            }
            if (1 + ml > b->cap) {
                uint32_t cap = b->cap;
                while (cap < 1 + ml) cap *= 2;
                char *nb = malloc(cap);
                if (!nb) die("out of memory");
                memcpy(nb, b->buf + b->pos, b->len - b->pos);
                free(b->buf);
                b->buf = nb;
                b->cap = cap;
                b->len -= b->pos;
                b->pos = 0;
            }
        }
        if (b->pos) {
            memmove(b->buf, b->buf + b->pos, b->len - b->pos);
            b->len -= b->pos;
            b->pos = 0;
        }
        struct pollfd pfd = {.fd = b->fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 15000);
        if (pr == 0) { snprintf(b->err, b->errn, "timeout waiting for the server"); return false; }
        ssize_t r = recv(b->fd, b->buf + b->len, b->cap - b->len, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { snprintf(b->err, b->errn, "connection closed%s%s", r < 0 ? ": " : "", r < 0 ? strerror(errno) : ""); return false; }
        b->len += (uint32_t)r;
    }
}

static void error_text(struct bconn *b, const char *body, uint32_t n) {
    const char *sev = "", *code = "", *msg = "";
    for (uint32_t i = 0; i < n && body[i];) {
        char f = body[i++];
        const char *v = body + i;
        i += (uint32_t)strnlen(v, n - i) + 1;
        if (f == 'S') sev = v;
        else if (f == 'C') code = v;
        else if (f == 'M') msg = v;
    }
    snprintf(b->err, b->errn, "%s %s: %s", sev, code, msg);
}

typedef struct { char *p; uint32_t len, cap; } wbuf;
static void wb_put(wbuf *w, const void *s, uint32_t n) {
    if (w->len + n > w->cap) {
        while (w->len + n > w->cap) w->cap = w->cap ? w->cap * 2 : 4096;
        w->p = realloc(w->p, w->cap);
        if (!w->p) die("out of memory");
    }
    memcpy(w->p + w->len, s, n);
    w->len += n;
}
static void wb_str(wbuf *w, const char *s) { wb_put(w, s, (uint32_t)strlen(s) + 1); }
static void wb_u32(wbuf *w, uint32_t v) { char b[4]; wr32(b, v); wb_put(w, b, 4); }
static void wb_u16(wbuf *w, uint16_t v) { char b[2]; wr16(b, v); wb_put(w, b, 2); }
static uint32_t wb_begin(wbuf *w, char type) { wb_put(w, &type, 1); uint32_t at = w->len; wb_u32(w, 0); return at; }
static void wb_end(wbuf *w, uint32_t at) { wr32(w->p + at, w->len - at); }

/* ---------- SCRAM-SHA-256 ---------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t b64_encode(char *dst, const uint8_t *s, uint32_t n) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        dst[o++] = B64[v >> 18];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        dst[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    dst[o] = 0;
    return o;
}

static int b64_decode(uint8_t *dst, uint32_t cap, const char *s, uint32_t n) {
    uint32_t o = 0, acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == '=') break;
        const char *p = strchr(B64, s[i]);
        if (!p || !s[i]) return -1;
        acc = acc << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            dst[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

static const char *attr(const char *msg, uint32_t n, char key, uint32_t *vn) {
    for (uint32_t i = 0; i + 1 < n;) {
        uint32_t e = i;
        while (e < n && msg[e] != ',') e++;
        if (msg[i] == key && msg[i + 1] == '=') { *vn = e - i - 2; return msg + i + 2; }
        i = e + 1;
    }
    return NULL;
}

static bool scram(struct bconn *b, const struct pgconf *pc, const char *mechs, uint32_t mn) {
    bool have = false;
    for (uint32_t i = 0; i < mn && mechs[i];) {
        if (!strcmp(mechs + i, "SCRAM-SHA-256")) have = true;
        i += (uint32_t)strlen(mechs + i) + 1;
    }
    if (!have) { snprintf(b->err, b->errn, "server offers no SCRAM-SHA-256"); return false; }
    uint8_t rnd[18];
    fill_random(rnd, sizeof rnd);
    char nonce[32], first_bare[64];
    b64_encode(nonce, rnd, sizeof rnd);
    int fbn = snprintf(first_bare, sizeof first_bare, "n=,r=%s", nonce);
    wbuf w = {0};
    uint32_t at = wb_begin(&w, 'p');
    wb_str(&w, "SCRAM-SHA-256");
    wb_u32(&w, (uint32_t)fbn + 3);
    wb_put(&w, "n,,", 3);
    wb_put(&w, first_bare, (uint32_t)fbn);
    wb_end(&w, at);
    bool ok = send_all(b, w.p, w.len);
    free(w.p);
    if (!ok) return false;
    char t;
    const char *body;
    uint32_t n;
    if (!read_msg(b, &t, &body, &n)) return false;
    if (t == 'E') { error_text(b, body, n); return false; }
    if (t != 'R' || n < 4 || rd32(body) != 11) { snprintf(b->err, b->errn, "unexpected SASL message"); return false; }
    char server_first[512];
    uint32_t sfn = n - 4 < sizeof server_first - 1 ? n - 4 : (uint32_t)sizeof server_first - 1;
    memcpy(server_first, body + 4, sfn);
    server_first[sfn] = 0;
    uint32_t rn = 0, sn = 0, in = 0;
    const char *r = attr(server_first, sfn, 'r', &rn), *s = attr(server_first, sfn, 's', &sn), *it = attr(server_first, sfn, 'i', &in);
    if (!r || !s || !it || rn < strlen(nonce) || memcmp(r, nonce, strlen(nonce)) != 0) {
        snprintf(b->err, b->errn, "bad SCRAM server-first message");
        return false;
    }
    uint8_t salt[128];
    int saltn = b64_decode(salt, sizeof salt, s, sn);
    int iters = atoi(it);
    if (saltn <= 0 || iters <= 0) { snprintf(b->err, b->errn, "bad SCRAM salt or iterations"); return false; }
    uint8_t salted[32], client_key[32], stored_key[32], client_sig[32], proof[32], server_key[32], server_sig[32];
    pbkdf2_sha256(salted, pc->password, strlen(pc->password), salt, (size_t)saltn, (uint32_t)iters);
    hmac_key k;
    hmac_init(&k, salted, 32);
    hmac_sha256(client_key, &k, "Client Key", 10);
    hmac_sha256(server_key, &k, "Server Key", 10);
    sha256(stored_key, client_key, 32);
    char final_wo[600], auth_msg[1400];
    int fwn = snprintf(final_wo, sizeof final_wo, "c=biws,r=%.*s", (int)rn, r);
    int amn = snprintf(auth_msg, sizeof auth_msg, "%s,%s,%s", first_bare, server_first, final_wo);
    hmac_init(&k, stored_key, 32);
    hmac_sha256(client_sig, &k, auth_msg, (size_t)amn);
    for (int i = 0; i < 32; i++) proof[i] = client_key[i] ^ client_sig[i];
    hmac_init(&k, server_key, 32);
    hmac_sha256(server_sig, &k, auth_msg, (size_t)amn);
    char proof64[64], final[700];
    b64_encode(proof64, proof, 32);
    int fn = snprintf(final, sizeof final, "%.*s,p=%s", fwn, final_wo, proof64);
    memset(&w, 0, sizeof w);
    at = wb_begin(&w, 'p');
    wb_put(&w, final, (uint32_t)fn);
    wb_end(&w, at);
    ok = send_all(b, w.p, w.len);
    free(w.p);
    if (!ok || !read_msg(b, &t, &body, &n)) return false;
    if (t == 'E') { error_text(b, body, n); return false; }
    if (t != 'R' || n < 4 || rd32(body) != 12) { snprintf(b->err, b->errn, "unexpected SASL final message"); return false; }
    uint32_t vn;
    const char *v = attr(body + 4, n - 4, 'v', &vn);
    uint8_t sig[64];
    if (!v || b64_decode(sig, sizeof sig, v, vn) != 32 || memcmp(sig, server_sig, 32) != 0) {
        snprintf(b->err, b->errn, "SCRAM server signature mismatch");
        return false;
    }
    return true;
}

/* ---------- connect ---------- */

static int open_socket(const struct pgconf *pc, char *err, size_t errn) {
    if (pc->unix_socket) {
        struct sockaddr_un sa = {.sun_family = AF_UNIX};
        snprintf(sa.sun_path, sizeof sa.sun_path, "%.80s/.s.PGSQL.%.10s", pc->host, pc->port);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
            snprintf(err, errn, "connect %s: %s", sa.sun_path, strerror(errno));
            if (fd >= 0) close(fd);
            return -1;
        }
        return fd;
    }
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res, *ai;
    int rc = getaddrinfo(pc->host, pc->port, &hints, &res);
    if (rc) { snprintf(err, errn, "resolve %s: %s", pc->host, gai_strerror(rc)); return -1; }
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        snprintf(err, errn, "connect %s:%s: %s", pc->host, pc->port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

int pg_connect(const struct pgconf *pc, bool writer, char *err, size_t errn) {
    int fd = open_socket(pc, err, errn);
    if (fd < 0) return -1;
    struct bconn b = {.fd = fd, .cap = 1 << 16, .err = err, .errn = errn};
    b.buf = malloc(b.cap);
    if (!b.buf) die("out of memory");
    wbuf w = {0};
    wb_u32(&w, 0);
    wb_u32(&w, 196608); /* protocol 3.0 */
    wb_str(&w, "user"); wb_str(&w, pc->user);
    wb_str(&w, "database"); wb_str(&w, pc->dbname);
    for (size_t i = 0; i < ARRAY_LEN(SESSION); i++) { wb_str(&w, SESSION[i][0]); wb_str(&w, SESSION[i][1]); }
    if (!writer && cfg.read_snapshot) {
        /* experiment E3: one snapshot per read batch (read-only repeatable read never fails to serialize) */
        wb_str(&w, "default_transaction_isolation");
        wb_str(&w, "repeatable read");
    }
    wb_put(&w, "", 1);
    wr32(w.p, w.len);
    bool ok = send_all(&b, w.p, w.len);
    free(w.p);
    char t;
    const char *body;
    uint32_t n;
    bool utf8 = false, intdt = false;
    while (ok) {
        if (!(ok = read_msg(&b, &t, &body, &n))) break;
        if (t == 'E') { error_text(&b, body, n); ok = false; break; }
        if (t == 'R') {
            uint32_t code = n >= 4 ? rd32(body) : 99;
            if (code == 0) continue;
            if (code == 10) { ok = scram(&b, pc, body + 4, n - 4); continue; }
            if (code == 3) {
                memset(&w, 0, sizeof w);
                uint32_t at = wb_begin(&w, 'p');
                wb_str(&w, pc->password);
                wb_end(&w, at);
                ok = send_all(&b, w.p, w.len);
                free(w.p);
                continue;
            }
            snprintf(err, errn, "unsupported authentication method %u (use scram-sha-256)", code);
            ok = false;
            break;
        }
        if (t == 'S') {
            if (n >= 16 && !strcmp(body, "server_encoding")) utf8 = !strcmp(body + 16, "UTF8");
            if (n >= 18 && !strcmp(body, "integer_datetimes")) intdt = !strcmp(body + 18, "on");
            if (n >= 16 && !strcmp(body, "client_encoding") && strcmp(body + 16, "UTF8")) { snprintf(err, errn, "client_encoding is not UTF8"); ok = false; }
        }
        if (t == 'Z') break;
    }
    if (ok && !intdt) { snprintf(err, errn, "integer_datetimes is off"); ok = false; }
    (void)utf8; /* the client encoding is UTF8, so text arrives as UTF-8 whatever the server encoding */
    if (ok) {
        /* prepare every statement and describe its result columns */
        memset(&w, 0, sizeof w);
        for (int s = 0; s < S_COUNT; s++) {
            char name[16];
            snprintf(name, sizeof name, "s%d", s);
            uint32_t at = wb_begin(&w, 'P');
            wb_str(&w, name);
            wb_str(&w, STMTS[s].sql);
            wb_u16(&w, STMTS[s].nparams);
            for (int i = 0; i < STMTS[s].nparams; i++) wb_u32(&w, STMTS[s].ptypes[i]);
            wb_end(&w, at);
            at = wb_begin(&w, 'D');
            wb_put(&w, "S", 1);
            wb_str(&w, name);
            wb_end(&w, at);
        }
        uint32_t at = wb_begin(&w, 'S');
        wb_end(&w, at);
        ok = send_all(&b, w.p, w.len);
        free(w.p);
        int s = 0;
        while (ok) {
            if (!(ok = read_msg(&b, &t, &body, &n))) break;
            if (t == 'E') { error_text(&b, body, n); log_msg("prepare %s failed: %s", s < S_COUNT ? STMTS[s].name : "?", err); ok = false; break; }
            if (t == 'T') {
                if (s >= S_COUNT) { snprintf(err, errn, "unexpected row description"); ok = false; break; }
                uint16_t nc = rd16(body);
                const struct stmt *st = &STMTS[s];
                if (nc != st->ncols) { snprintf(err, errn, "statement %s returns %u columns, expected %u", st->name, nc, st->ncols); ok = false; break; }
                const char *p = body + 2;
                for (uint16_t c = 0; c < nc; c++) {
                    p += strlen(p) + 1;
                    uint32_t oid = rd32(p + 6);
                    if (oid != st->ctypes[c]) {
                        snprintf(err, errn, "statement %s column %u has type %u, expected %u", st->name, c, oid, st->ctypes[c]);
                        ok = false;
                        break;
                    }
                    p += 18;
                }
                s++;
            } else if (t == 'n') {
                if (s < S_COUNT && STMTS[s].ncols) { snprintf(err, errn, "statement %s returns no rows", STMTS[s].name); ok = false; break; }
                s++;
            } else if (t == 'Z') {
                break;
            }
        }
        if (ok && s != S_COUNT) { snprintf(err, errn, "prepared %d of %d statements", s, S_COUNT); ok = false; }
    }
    free(b.buf);
    if (!ok) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* ---------- reconnects ---------- */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct pgc *q[256];
    int n;
    bool started;
} rc = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

struct pgmsg { struct pgc *c; int fd; };

static void *connector(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "pg-connect");
    for (;;) {
        pthread_mutex_lock(&rc.mu);
        while (!rc.n) pthread_cond_wait(&rc.cv, &rc.mu);
        struct pgc *c = rc.q[--rc.n];
        pthread_mutex_unlock(&rc.mu);
        uint32_t backoff = 50;
        for (;;) {
            char err[256];
            int fd = pg_connect(&pgconf, c->writer, err, sizeof err);
            if (fd >= 0) {
                struct pgmsg *m = malloc(sizeof *m);
                if (!m) die("out of memory");
                m->c = c;
                m->fd = fd;
                loop_post(c->loop, (void *)((uintptr_t)m | 1));
                break;
            }
            log_msg("postgres reconnect failed (retry in %u ms): %s", backoff, err);
            usleep(backoff * 1000);
            backoff = backoff * 2 > 2000 ? 2000 : backoff * 2;
        }
    }
    return NULL;
}

void pg_reconnect_async(struct pgc *c) {
    pthread_mutex_lock(&rc.mu);
    if (!rc.started) {
        pthread_t t;
        if (pthread_create(&t, NULL, connector, NULL)) die("pthread_create connector");
        pthread_detach(t);
        rc.started = true;
    }
    if (rc.n < (int)ARRAY_LEN(rc.q)) rc.q[rc.n++] = c;
    pthread_mutex_unlock(&rc.mu);
    pthread_cond_signal(&rc.cv);
}
