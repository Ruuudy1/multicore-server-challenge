/* sched.c: the per-loop scheduler and the Postgres wire protocol on the event loop.
 *
 * Reads: one FIFO per loop. A read connection with fewer than PG_DEPTH batches in flight takes up to READ_BATCH
 * jobs, encodes them back to back (Bind + Execute each) with one Sync, and sends them in one write. Each read is
 * answered at its own CommandComplete. If a statement fails, Postgres skips the rest of its batch: those jobs
 * go back to the head of the FIFO.
 *
 * Writes: one write connection per loop with group commit. While a group's commit is in flight, writes queue up;
 * the next group (up to WRITE_BATCH) is one implicit transaction with one Sync, and its responses wait for
 * ReadyForQuery. If anything in a group fails, the group rolled back and each of its writes is retried alone.
 * User writes always run alone (a unique violation there is an expected 422). */
#include "loop.h"

#include <errno.h>
#include <unistd.h>

/* ---------- queues ---------- */

static inline void q_push(struct req **h, struct req **t, struct req *r) {
    r->next = NULL;
    if (*t) (*t)->next = r;
    else *h = r;
    *t = r;
}

static inline struct req *q_pop(struct req **h, struct req **t) {
    struct req *r = *h;
    if (r) {
        *h = r->next;
        if (!*h) *t = NULL;
        r->next = NULL;
    }
    return r;
}

/* Prepends the chain [first .. last] (already linked) to a queue. */
static inline void q_prepend(struct req **h, struct req **t, struct req *first, struct req *last) {
    last->next = *h;
    *h = first;
    if (!*t) *t = last;
}

void db_read(struct req *r, int stmt, int nparams, pg_done_fn done) {
    struct loop *l = r->loop;
    r->stmt = (uint8_t)stmt;
    r->nparams = (uint8_t)nparams;
    r->done = done;
    q_push(&l->rq_head, &l->rq_tail, r);
    l->rq_len++;
}

void db_write(struct req *r, int stmt, int nparams, pg_done_fn done, bool alone) {
    struct loop *l = r->loop;
    r->stmt = (uint8_t)stmt;
    r->nparams = (uint8_t)nparams;
    r->done = done;
    if (alone) r->flags |= RQ_ALONE;
    q_push(&l->wq_head, &l->wq_tail, r);
    l->wq_len++;
}

/* ---------- connections ---------- */

static char stmt_names[S_COUNT][8];

static void pgc_alloc(struct pgc *c) {
    c->scap = 64 * 1024;
    c->sbuf = malloc(c->scap);
    c->rcap = 256 * 1024;
    c->rbuf = malloc(c->rcap);
    c->rows_cap = 64 * 1024;
    c->rows = malloc(c->rows_cap);
    c->vals_cap = 4096;
    c->vals = malloc(sizeof(struct pgval) * c->vals_cap);
    if (!c->sbuf || !c->rbuf || !c->rows || !c->vals) die("out of memory");
}

void sched_init(struct loop *l) {
    for (int s = 0; s < S_COUNT; s++) snprintf(stmt_names[s], sizeof stmt_names[s], "s%d", s);
    /* split the connection budget over the loops; every loop gets at least one of each */
    int nr = cfg.pg_read_conns / cfg.loops + (l->idx < cfg.pg_read_conns % cfg.loops);
    int nw = cfg.pg_write_conns / cfg.loops + (l->idx < cfg.pg_write_conns % cfg.loops);
    if (nr < 1) nr = 1;
    if (nw < 1) nw = 1;
    l->npg = nr + nw;
    l->pg = calloc((size_t)l->npg, sizeof(struct pgc));
    if (!l->pg) die("out of memory");
    for (int i = 0; i < l->npg; i++) {
        struct pgc *c = &l->pg[i];
        c->loop = l;
        c->idx = i;
        c->writer = i >= nr;
        pgc_alloc(c);
        char err[512];
        int64_t deadline = clock_ms() + 50000;
        for (;;) {
            c->fd = pg_connect(&pgconf, c->writer, err, sizeof err);
            if (c->fd >= 0) break;
            if (clock_ms() > deadline) die("postgres: %s", err);
            log_msg("postgres not ready (%s), retrying", err);
            usleep(200 * 1000);
        }
        c->up = true;
    }
}

static void pgc_arm_recv(struct loop *l, struct pgc *c) {
    if (c->rcap - c->rlen < 16 * 1024) {
        c->rcap *= 2;
        c->rbuf = realloc(c->rbuf, c->rcap);
        if (!c->rbuf) die("out of memory");
    }
    struct io_uring_sqe *sqe = get_sqe(l);
    io_uring_prep_recv(sqe, c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, 0);
    io_uring_sqe_set_data64(sqe, UD(UD_PG_RECV, c->gen, c->idx));
    c->recving = true;
}

void sched_start(struct loop *l) {
    for (int i = 0; i < l->npg; i++) if (l->pg[i].up) pgc_arm_recv(l, &l->pg[i]);
}

static void pgc_send(struct loop *l, struct pgc *c) {
    struct io_uring_sqe *sqe = get_sqe(l);
    io_uring_prep_send(sqe, c->fd, c->sbuf + c->soff, c->slen - c->soff, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, UD(UD_PG_SEND, c->gen, c->idx));
    c->sending = true;
}

/* ---------- encoding ---------- */

static inline void sb_need(struct pgc *c, uint32_t n) {
    if (c->slen + n > c->scap) {
        while (c->slen + n > c->scap) c->scap *= 2;
        c->sbuf = realloc(c->sbuf, c->scap);
        if (!c->sbuf) die("out of memory");
    }
}

static void enc_job(struct pgc *c, struct req *r) {
    const char *name = stmt_names[r->stmt];
    uint32_t nl = (uint32_t)strlen(name) + 1, need = 1 + 4 + 1 + nl + 2 + 2 + 2 + 2 + 2 + 10;
    for (int i = 0; i < r->nparams; i++) need += 4 + (r->prm[i].len > 0 ? (uint32_t)r->prm[i].len : 0);
    sb_need(c, need);
    char *p = c->sbuf + c->slen, *start = p;
    *p++ = 'B';
    char *lenp = p;
    p += 4;
    *p++ = 0;                       /* unnamed portal */
    memcpy(p, name, nl); p += nl;
    wr16(p, 1); wr16(p + 2, 1); p += 4;           /* all parameters binary */
    wr16(p, r->nparams); p += 2;
    for (int i = 0; i < r->nparams; i++) {
        int32_t len = r->prm[i].len;
        wr32(p, (uint32_t)len);
        p += 4;
        if (len > 0) { memcpy(p, r->prm[i].p, (size_t)len); p += len; }
    }
    wr16(p, 1); wr16(p + 2, 1); p += 4;           /* all results binary */
    wr32(lenp, (uint32_t)(p - lenp));
    *p++ = 'E';
    wr32(p, 9); p += 4;
    *p++ = 0;
    wr32(p, 0); p += 4;
    c->slen += (uint32_t)(p - start);
}

static void enc_sync(struct pgc *c) {
    sb_need(c, 5);
    char *p = c->sbuf + c->slen;
    p[0] = 'S';
    wr32(p + 1, 4);
    c->slen += 5;
}

/* ---------- dispatch ---------- */

static inline bool user_write(int stmt) { return stmt == S_U_REGISTER || stmt == S_U_UPDATE; }

static void jobs_append(struct pgc *c, struct req *r) {
    r->next = NULL;
    if (c->jtail) c->jtail->next = r;
    else c->jhead = r;
    c->jtail = r;
    c->njobs++;
}

static void send_reads(struct loop *l, struct pgc *c) {
    struct batch *b = &c->batches[(c->bhead + c->bcount) % MAX_DEPTH];
    b->n = b->done = 0;
    b->err = false;
    while (b->n < cfg.read_batch && l->rq_head) {
        struct req *r = q_pop(&l->rq_head, &l->rq_tail);
        l->rq_len--;
        if (r->flags & RQ_ORPHAN) { req_free(r); continue; } /* the client left before we asked */
        enc_job(c, r);
        jobs_append(c, r);
        b->n++;
    }
    if (!b->n) return;
    enc_sync(c);
    c->bcount++;
    c->batches_sent++;
    c->jobs_sent += b->n;
    l->st.pg_batches++;
    l->st.pg_jobs += b->n;
    pgc_send(l, c);
}

static void send_writes(struct loop *l, struct pgc *c) {
    struct batch *b = &c->batches[(c->bhead + c->bcount) % MAX_DEPTH];
    b->n = b->done = 0;
    b->err = false;
    struct req *prev = NULL, *r = l->wq_head;
    while (r && b->n < cfg.write_batch) {
        struct req *next = r->next;
        bool alone = (r->flags & RQ_ALONE) || user_write(r->stmt);
        if (r->flags & RQ_ORPHAN || !alone || b->n == 0) {
            /* unlink r */
            if (prev) prev->next = next;
            else l->wq_head = next;
            if (l->wq_tail == r) l->wq_tail = prev;
            l->wq_len--;
            if (r->flags & RQ_ORPHAN) { req_free(r); r = next; continue; }
            enc_job(c, r);
            jobs_append(c, r);
            b->n++;
            if (alone) break; /* a group of one */
        } else {
            prev = r;
        }
        r = next;
    }
    if (!b->n) return;
    enc_sync(c);
    c->bcount++;
    c->batches_sent++;
    c->jobs_sent += b->n;
    l->st.pg_wbatches++;
    l->st.pg_wjobs += b->n;
    pgc_send(l, c);
}

void sched_pump(struct loop *l) {
    while (l->rq_head) {
        struct pgc *best = NULL;
        for (int i = 0; i < l->npg; i++) {
            struct pgc *c = &l->pg[i];
            if (c->writer || !c->up || c->sending || c->bcount >= (uint32_t)cfg.pg_depth) continue;
            if (!best || c->njobs < best->njobs) best = c;
        }
        if (!best) break;
        send_reads(l, best);
    }
    for (int i = 0; i < l->npg && l->wq_head; i++) {
        struct pgc *c = &l->pg[i];
        if (c->writer && c->up && !c->sending && c->bcount == 0) send_writes(l, c);
    }
    l->st.fifo_len = l->rq_len;
    l->st.wq_len = l->wq_len;
}

/* ---------- results ---------- */

static void res_view(struct pgc *c, struct req *r, struct pgres *res, const char *sqlstate) {
    res->base = c->rows;
    res->v = c->vals;
    res->bytes = c->rows_len;
    res->nrows = c->nrows;
    res->ncols = c->nrows ? c->ncols : STMTS[r->stmt].ncols;
    if (sqlstate) memcpy(res->sqlstate, sqlstate, 6);
    else res->sqlstate[0] = 0;
}

static void rows_reset(struct pgc *c) { c->rows_len = c->nvals = c->nrows = 0; }

static void commit_append(struct pgc *c, struct req *r) {
    r->next = NULL;
    if (c->ctail) c->ctail->next = r;
    else c->chead = r;
    c->ctail = r;
}

static bool retryable(const char *s) { return !strcmp(s, "40P01") || !strcmp(s, "40001"); }

/* A statement finished (CommandComplete, or ErrorResponse when sqlstate is set). */
static void job_done(struct loop *l, struct pgc *c, const char *sqlstate) {
    struct batch *b = &c->batches[c->bhead];
    struct req *r = c->jhead;
    if (!c->bcount || !r || b->done >= b->n) {
        /* an error at the Sync (e.g. the commit) belongs to the batch, not to a statement */
        if (sqlstate && c->bcount) b->err = true;
        rows_reset(c);
        return;
    }
    c->jhead = r->next;
    if (!c->jhead) c->jtail = NULL;
    r->next = NULL;
    c->njobs--;
    b->done++;
    struct pgres res;
    res_view(c, r, &res, sqlstate);
    if (!c->writer) {
        if (sqlstate) b->err = true;
        if (r->flags & RQ_ORPHAN) req_free(r);
        else {
            if (sqlstate) {
                log_msg("statement %s failed: %s", STMTS[r->stmt].name, sqlstate);
                resp_error(r, 500, "internal error");
            } else {
                r->done(r, &res);
            }
            if (r->flags & RQ_READY) req_complete(r);
        }
    } else {
        if (sqlstate) {
            b->err = true;
            if (b->n == 1 && retryable(sqlstate) && r->retries == 0) {
                r->retries++;
                r->flags |= RQ_ALONE;
                r->status = -1; /* retry after the rollback */
            } else if (b->n == 1) {
                if (!strcmp(sqlstate, "23505") && user_write(r->stmt)) {
                    resp_error(r, 422, "username or email is already taken");
                } else {
                    log_msg("write %s failed: %s", STMTS[r->stmt].name, sqlstate);
                    resp_error(r, 500, "internal error");
                }
            }
        } else {
            r->done(r, &res);
        }
        commit_append(c, r);
    }
    rows_reset(c);
}

/* ReadyForQuery: the oldest batch is over (and for a write group, committed or rolled back). */
static void batch_done(struct loop *l, struct pgc *c) {
    if (!c->bcount) return;
    struct batch *b = &c->batches[c->bhead];
    uint32_t skipped = b->n - b->done;
    struct req *first = NULL, *last = NULL;
    for (uint32_t i = 0; i < skipped && c->jhead; i++) {
        struct req *r = c->jhead;
        c->jhead = r->next;
        if (!c->jhead) c->jtail = NULL;
        c->njobs--;
        r->next = NULL;
        if (last) last->next = r;
        else first = r;
        last = r;
    }
    if (!c->writer) {
        if (first) { /* skipped after a failed statement: ask again first */
            uint32_t n = 0;
            for (struct req *r = first; r; r = r->next) n++;
            q_prepend(&l->rq_head, &l->rq_tail, first, last);
            l->rq_len += n;
        }
    } else {
        struct req *g = c->chead, *gl = c->ctail;
        c->chead = c->ctail = NULL;
        if (!b->err) {
            while (g) { struct req *next = g->next; g->next = NULL; req_complete(g); g = next; }
        } else if (b->n == 1 && g && g->status != -1) {
            g->next = NULL;
            req_complete(g); /* the write's own error response */
        } else {
            /* the group rolled back: retry every write alone, in order */
            if (gl) { gl->next = first; if (!first) last = gl; first = g; }
            uint32_t n = 0;
            for (struct req *r = first; r; r = r->next) {
                n++;
                resp_reset(r);
                r->flags |= RQ_ALONE;
                r->status = 0;
            }
            if (first) {
                q_prepend(&l->wq_head, &l->wq_tail, first, last);
                l->wq_len += n;
            }
        }
    }
    c->bhead = (c->bhead + 1) % MAX_DEPTH;
    c->bcount--;
}

static void row_add(struct pgc *c, const char *body, uint32_t n) {
    if (c->rows_len + n > c->rows_cap) {
        while (c->rows_len + n > c->rows_cap) c->rows_cap *= 2;
        c->rows = realloc(c->rows, c->rows_cap);
        if (!c->rows) die("out of memory");
    }
    uint32_t base = c->rows_len;
    memcpy(c->rows + base, body, n);
    c->rows_len += n;
    uint16_t nc = rd16(body);
    if (c->nvals + nc > c->vals_cap) {
        while (c->nvals + nc > c->vals_cap) c->vals_cap *= 2;
        c->vals = realloc(c->vals, sizeof(struct pgval) * c->vals_cap);
        if (!c->vals) die("out of memory");
    }
    uint32_t p = 2;
    for (uint16_t i = 0; i < nc && p + 4 <= n; i++) {
        int32_t len = (int32_t)rd32(body + p);
        p += 4;
        c->vals[c->nvals++] = (struct pgval){base + p, len};
        if (len > 0) p += (uint32_t)len;
    }
    c->ncols = nc;
    c->nrows++;
}

static void pgc_fail(struct loop *l, struct pgc *c, const char *why);

static void parse(struct loop *l, struct pgc *c) {
    uint32_t pos = 0;
    while (c->up && c->rlen - pos >= 5) {
        char t = c->rbuf[pos];
        uint32_t ml = rd32(c->rbuf + pos + 1);
        if (ml < 4) { pgc_fail(l, c, "bad message length"); return; }
        if (c->rlen - pos < 1 + ml) break;
        const char *body = c->rbuf + pos + 5;
        uint32_t n = ml - 4;
        switch (t) {
        case 'D': row_add(c, body, n); break;
        case 'C': case 'I': job_done(l, c, NULL); break;
        case 'E': {
            char code[6] = "XX000", msg[160] = "";
            bool fatal = false;
            for (uint32_t i = 0; i < n && body[i];) {
                char f = body[i++];
                const char *v = body + i;
                size_t vl = strnlen(v, n - i);
                if (f == 'C' && vl == 5) memcpy(code, v, 5);
                if (f == 'S' && (!strncmp(v, "FATAL", 5) || !strncmp(v, "PANIC", 5))) fatal = true;
                if (f == 'M') snprintf(msg, sizeof msg, "%.*s", (int)vl, v);
                i += (uint32_t)vl + 1;
            }
            if (fatal) {
                /* the backend is going away: a lost connection, not a failed statement (reads are asked again) */
                char why[200];
                snprintf(why, sizeof why, "%s %s", code, msg);
                pgc_fail(l, c, why);
                return;
            }
            job_done(l, c, code);
            break;
        }
        case 'Z': batch_done(l, c); break;
        default: break; /* BindComplete, notices, parameter status */
        }
        pos += 1 + ml;
    }
    if (!c->up) return;
    if (pos) {
        memmove(c->rbuf, c->rbuf + pos, c->rlen - pos);
        c->rlen -= pos;
    }
    if (c->rlen >= 5) {
        uint32_t need = 1 + rd32(c->rbuf + 1) + 16 * 1024;
        if (need > c->rcap) {
            while (need > c->rcap) c->rcap *= 2;
            c->rbuf = realloc(c->rbuf, c->rcap);
            if (!c->rbuf) die("out of memory");
        }
    }
}

static void pgc_fail(struct loop *l, struct pgc *c, const char *why) {
    if (!c->up) return;
    log_msg("postgres connection %d.%d lost: %s", l->idx, c->idx, why);
    c->up = false;
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    c->fd = -1;
    c->gen++;
    if (!c->writer) {
        /* reads are safe to ask again */
        if (c->jhead) {
            uint32_t n = c->njobs;
            q_prepend(&l->rq_head, &l->rq_tail, c->jhead, c->jtail);
            l->rq_len += n;
        }
    } else {
        /* the commit of anything sent is uncertain: never retried */
        struct req *lists[2] = {c->chead, c->jhead};
        for (int k = 0; k < 2; k++) {
            for (struct req *r = lists[k]; r;) {
                struct req *next = r->next;
                r->next = NULL;
                resp_reset(r);
                resp_error(r, 503, "database connection lost");
                req_complete(r);
                r = next;
            }
        }
    }
    c->jhead = c->jtail = c->chead = c->ctail = NULL;
    c->njobs = c->bhead = c->bcount = 0;
    c->slen = c->soff = c->rlen = 0;
    c->sending = c->recving = false;
    rows_reset(c);
    c->connecting = true;
    pg_reconnect_async(c);
}

void sched_reconnected(struct loop *l, struct pgc *c, int fd) {
    c->fd = fd;
    c->up = true;
    c->connecting = false;
    log_msg("postgres connection %d.%d restored", l->idx, c->idx);
    pgc_arm_recv(l, c);
}

void sched_cqe(struct loop *l, struct io_uring_cqe *cqe) {
    uint64_t ud = cqe->user_data;
    uint32_t idx = UD_SLOT(ud);
    if (idx >= (uint32_t)l->npg) return;
    struct pgc *c = &l->pg[idx];
    if (UD_GEN(ud) != (c->gen & 0xffffff) || !c->up) return; /* an old socket's completion */
    if (UD_TYPE(ud) == UD_PG_SEND) {
        if (cqe->res < 0) { pgc_fail(l, c, strerror(-cqe->res)); return; }
        c->soff += (uint32_t)cqe->res;
        if (c->soff < c->slen) { pgc_send(l, c); return; }
        c->slen = c->soff = 0;
        c->sending = false;
    } else {
        c->recving = false;
        if (cqe->res <= 0) { pgc_fail(l, c, cqe->res ? strerror(-cqe->res) : "closed by the server"); return; }
        c->rlen += (uint32_t)cqe->res;
        parse(l, c);
        if (c->up) pgc_arm_recv(l, c);
    }
}

void sched_tick(struct loop *l) { (void)l; }
