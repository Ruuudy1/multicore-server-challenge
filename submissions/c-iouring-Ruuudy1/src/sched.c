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

static char stmt_names[S_COUNT][16];

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

/* Item 2: the shared statement answering several requests of a v2 list statement of one kind (not the feed), or -1. */
static int shared_list_of(int s) {
    static const int bases[] = {S_L2_GLOBAL, S_L2C_GLOBAL, S_L2T_GLOBAL};
    for (size_t i = 0; i < ARRAY_LEN(bases); i++)
        if (s >= bases[i] && s < bases[i] + K_FEED) return S_LS_GLOBAL + (s - bases[i]);
    return -1;
}

/* E2 and item 2: one execution of the batched form of the leader's statement; parameter i becomes the array of
 * every member's parameter i (the element types are the single statement's). */
static int batched_of(int s) {
    switch (s) {
    case S_A_GET: return S_A_GET_B;
    case S_C_LIST: return S_C_LIST_B;
    case S_P_GET: return S_P_GET_B;
    case S_U_GET: return S_U_GET_B;
    default: return shared_list_of(s);
    }
}

static void enc_group(struct pgc *c, struct req *leader) {
    const struct stmt *st = &STMTS[leader->stmt];
    const char *name = stmt_names[batched_of(leader->stmt)];
    uint32_t nl = (uint32_t)strlen(name) + 1, need = 1 + 4 + 1 + nl + 8 + 2 + 4 + 10;
    for (int i = 0; i < st->nparams; i++) {
        need += 4 + 20;
        for (struct req *m = leader; m; m = m->gnext) need += 4 + (m->prm[i].len > 0 ? (uint32_t)m->prm[i].len : 0);
    }
    sb_need(c, need);
    char *p = c->sbuf + c->slen, *start = p;
    *p++ = 'B';
    char *lenp = p;
    p += 4;
    *p++ = 0;
    memcpy(p, name, nl); p += nl;
    wr16(p, 1); wr16(p + 2, 1); p += 4;
    wr16(p, st->nparams); p += 2;
    for (int i = 0; i < st->nparams; i++) {
        char *plen = p;
        p += 4;
        bool nulls = false;
        for (struct req *m = leader; m; m = m->gnext) if (m->prm[i].len < 0) nulls = true;
        wr32(p, 1); wr32(p + 4, nulls); wr32(p + 8, st->ptypes[i]); wr32(p + 12, leader->gsize); wr32(p + 16, 1);
        p += 20;
        for (struct req *m = leader; m; m = m->gnext) {
            int32_t len = m->prm[i].len;
            wr32(p, (uint32_t)len);
            p += 4;
            if (len > 0) { memcpy(p, m->prm[i].p, (size_t)len); p += len; }
        }
        wr32(plen, (uint32_t)(p - plen - 4));
    }
    wr16(p, 1); wr16(p + 2, 1); p += 4;
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

/* E2: links the requests of one batchable statement in take[] behind the first of them (at most 255 per group). */
static void group_reads(struct req **take, int nt) {
    static const int singles[] = {S_A_GET, S_C_LIST, S_P_GET, S_U_GET};
    for (size_t s = 0; s < ARRAY_LEN(singles); s++) {
        int count = 0;
        for (int i = 0; i < nt; i++) if (take[i] && take[i]->stmt == singles[s]) count++;
        if (count < cfg.e2_min) continue;
        struct req *leader = NULL, *last = NULL;
        for (int i = 0; i < nt; i++) {
            struct req *r = take[i];
            if (!r || r->stmt != singles[s]) continue;
            r->gnext = NULL;
            if (!leader || leader->gsize == 255) {
                leader = last = r;
                r->flags |= RQ_GROUP;
                r->gsize = 1;
                continue;
            }
            last->gnext = r;
            last = r;
            leader->gsize++;
            take[i] = NULL; /* answered through its leader */
        }
    }
    /* a group of one is a plain execution */
    for (int i = 0; i < nt; i++) if (take[i] && (take[i]->flags & RQ_GROUP) && take[i]->gsize == 1) take[i]->flags &= (uint16_t)~RQ_GROUP;
}

/* Items 1 and 2: requests one execution answers together, by family: every tag list request (0), and the v2 list
 * requests of one kind (1 + kind). -1: not shared. */
enum { SHARE_FAMILIES = 1 + K_FEED, GATHER_SCAN = 512 };
static int share_family(const struct req *r) {
    if (r->stmt == S_T_LIST) return cfg.tag_share ? 0 : -1;
    int s = cfg.list_share ? shared_list_of(r->stmt) : -1;
    return s < 0 ? -1 : 1 + s - S_LS_GLOBAL;
}

/* Item 3: a batch holding a shareable request also takes the queued requests of the same family from further back
 * in the FIFO (looking at most GATHER_SCAN deep), so its shared executions answer more requests at once. */
static int gather(struct loop *l, struct req **take, int nt, int cap) {
    uint32_t want = 0;
    for (int i = 0; i < nt; i++) {
        int f = share_family(take[i]);
        if (f >= 0) want |= 1u << f;
    }
    struct req *prev = NULL, *next;
    int scanned = 0;
    for (struct req *r = l->rq_head; r && want && nt < cap && scanned < GATHER_SCAN; r = next, scanned++) {
        next = r->next;
        int f = share_family(r);
        if (f < 0 || !(want >> f & 1) || (r->flags & RQ_ORPHAN)) { prev = r; continue; }
        if (prev) prev->next = next;
        else l->rq_head = next;
        if (l->rq_tail == r) l->rq_tail = prev;
        l->rq_len--;
        r->next = NULL;
        take[nt++] = r;
    }
    return nt;
}

/* Links each family's requests in take[] behind a leader, at most 255 per execution. A list family needs SHARE_MIN
 * requests (a shared execution of one costs more than the plain statement); tag lists need two. */
static void share_groups(struct req **take, int nt) {
    for (int f = 0; f < SHARE_FAMILIES; f++) {
        int count = 0;
        for (int i = 0; i < nt; i++) if (take[i] && share_family(take[i]) == f) count++;
        if (count < (f == 0 ? 2 : cfg.share_min)) continue;
        struct req *leader = NULL, *last = NULL;
        for (int i = 0; i < nt; i++) {
            struct req *r = take[i];
            if (!r || share_family(r) != f) continue;
            r->gnext = NULL;
            if (!leader || leader->gsize == 255) {
                leader = last = r;
                r->flags |= RQ_GROUP;
                r->gsize = 1;
                continue;
            }
            last->gnext = r;
            last = r;
            leader->gsize++;
            take[i] = NULL; /* answered through its leader */
        }
    }
}

static void send_reads(struct loop *l, struct pgc *c) {
    struct batch *b = &c->batches[(c->bhead + c->bcount) % MAX_DEPTH];
    b->n = b->done = 0;
    b->err = false;
    struct req *take[1024];
    int nt = 0;
    while (nt < cfg.read_batch && l->rq_head) {
        struct req *r = q_pop(&l->rq_head, &l->rq_tail);
        l->rq_len--;
        if (r->flags & RQ_ORPHAN) { req_free(r); continue; } /* the client left before we asked */
        take[nt++] = r;
    }
    if (!nt) return;
    if (cfg.gather && (cfg.tag_share || cfg.list_share)) nt = gather(l, take, nt, MIN(cfg.read_batch + cfg.gather, (int)ARRAY_LEN(take)));
    if (cfg.e2_batch) group_reads(take, nt);
    if (cfg.tag_share || cfg.list_share) share_groups(take, nt);
    for (int i = 0; i < nt; i++) {
        struct req *r = take[i];
        if (!r) continue;
        if (r->flags & RQ_GROUP) {
            if (r->stmt == S_T_LIST) enc_job(c, r); /* item 1: the plain statement; every member gets its rows */
            else enc_group(c, r);
            l->st.grouped += r->gsize;
        } else {
            enc_job(c, r);
        }
        jobs_append(c, r);
        b->n++;
    }
    enc_sync(c);
    c->bcount++;
    c->batches_sent++;
    c->jobs_sent += (uint64_t)nt;
    l->st.pg_batches++;
    l->st.pg_jobs += (uint64_t)nt;
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

/* Answers every request of a grouped execution with what its own statement would have returned, so its usual
 * renderer runs unchanged. E2: only its own rows (ord = its position), minus the ord column. Item 1 (tag lists):
 * the execution's rows, the same for every member. Item 2 (lists): its own v2 rows rebuilt by share_view. */
static void dispatch_group(struct pgc *c, struct req *leader, const struct pgres *res, const char *sqlstate) {
    bool tags = leader->stmt == S_T_LIST, lists = shared_list_of(leader->stmt) >= 0;
    uint16_t nc = STMTS[leader->stmt].ncols;
    if (!sqlstate && !tags && res->nrows && res->ncols != STMTS[batched_of(leader->stmt)].ncols) sqlstate = "XX000";
    if (lists && !sqlstate) share_index(&c->six, res);
    uint32_t need = res->nrows * nc + 1;
    if (!tags && !lists && need > c->gvals_cap) {
        c->gvals_cap = need * 2;
        c->gvals = realloc(c->gvals, sizeof(struct pgval) * c->gvals_cap);
        if (!c->gvals) die("out of memory");
    }
    int64_t k = 1;
    for (struct req *m = leader, *next; m; m = next, k++) {
        next = m->gnext;
        m->gnext = NULL;
        m->flags &= (uint16_t)~RQ_GROUP;
        if (m->flags & RQ_ORPHAN) { req_free(m); continue; }
        struct pgres own;
        if (sqlstate) {
            log_msg("statement %s failed: %s", STMTS[tags ? m->stmt : batched_of(m->stmt)].name, sqlstate);
            resp_error(m, 500, "internal error");
        } else if (tags) {
            own = *res;
            m->done(m, &own);
        } else if (lists) {
            if (share_view(&c->six, res, k, &own)) {
                m->done(m, &own);
            } else {
                log_msg("statement %s: no rows for request %lld", STMTS[batched_of(m->stmt)].name, (long long)k);
                resp_error(m, 500, "internal error");
            }
        } else {
            uint32_t rows = 0;
            for (uint32_t i = 0; i < res->nrows; i++) {
                if (pv_i64(res, i, 0) != k) continue;
                memcpy(&c->gvals[rows * nc], &res->v[i * res->ncols + 1], sizeof(struct pgval) * nc);
                rows++;
            }
            struct pgres mine = {.base = res->base, .v = c->gvals, .bytes = res->bytes, .nrows = rows, .ncols = nc};
            mine.sqlstate[0] = 0;
            m->done(m, &mine);
        }
        if (m->flags & RQ_READY) req_complete(m);
    }
}

/* Puts reads back at the head of the FIFO in order, unpacking E2 groups into their requests. */
static void requeue_reads(struct loop *l, struct req *first) {
    struct req *head = NULL, *tail = NULL;
    uint32_t n = 0;
    for (struct req *r = first, *next; r; r = next) {
        next = r->next;
        bool group = r->flags & RQ_GROUP;
        r->flags &= (uint16_t)~RQ_GROUP;
        for (struct req *g = r, *gn; g; g = gn) {
            gn = group ? g->gnext : NULL;
            g->gnext = g->next = NULL;
            if (tail) tail->next = g;
            else head = g;
            tail = g;
            n++;
        }
    }
    if (head) {
        q_prepend(&l->rq_head, &l->rq_tail, head, tail);
        l->rq_len += n;
    }
}

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
    if (!c->writer && (r->flags & RQ_GROUP)) {
        if (sqlstate) b->err = true;
        dispatch_group(c, r, &res, sqlstate);
    } else if (!c->writer) {
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
        requeue_reads(l, first); /* skipped after a failed statement: ask again first */
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
            if (b->n > 1) log_msg("write group of %u rolled back: retrying each write alone", b->n);
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
        requeue_reads(l, c->jhead); /* reads are safe to ask again */
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
