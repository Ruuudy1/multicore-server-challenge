/* loop.h: event-loop internals shared by loop.c, sched.c and stats.c. */
#pragma once
#include "conduit.h"

#include <liburing.h>
#include <pthread.h>

/* user_data: [type:8][gen:24][slot:32], or [type:8][pointer:56] for posted pointers */
enum { UD_ACCEPT = 1, UD_RECV, UD_SEND, UD_CLOSE, UD_CANCEL, UD_PG_RECV, UD_PG_SEND, UD_MSG, UD_EVENTFD, UD_POLL, UD_CONTINUE };
#define UD(type, gen, slot) ((uint64_t)(type) << 56 | ((uint64_t)(gen) & 0xffffff) << 32 | (uint32_t)(slot))
#define UD_TYPE(u) ((int)((u) >> 56))
#define UD_GEN(u) ((uint32_t)((u) >> 32) & 0xffffff)
#define UD_SLOT(u) ((uint32_t)(u))
#define UD_PTR(u) ((void *)(uintptr_t)((u) & 0x00ffffffffffffffULL))

/* client connection: 64 bytes, indexed by registered-file slot (or fd in fallback mode) */
enum { C_OPEN = 1, C_RECV = 2, C_SENDING = 4, C_CLOSING = 8, C_CANCELED = 16, C_CONTINUE = 32, C_REARM = 64 };
struct conn {
    uint32_t gen;
    uint16_t flags;
    uint16_t pad;
    uint32_t in_len, in_cap;
    char *in;               /* buffered input (partial or pipelined requests) */
    struct req *req;        /* request being served */
    uint32_t sent;          /* bytes of the response already sent */
    uint32_t pad2;
    uint64_t pad3[3];
};

#define MAX_DEPTH 16
struct batch { uint16_t n, done; bool err; };

/* Postgres connection owned by a loop */
struct pgc {
    struct loop *loop;
    int idx, fd;
    bool writer, up, sending, recving, connecting;
    char *sbuf; uint32_t slen, scap, soff;
    char *rbuf; uint32_t rlen, rcap;
    struct req *jhead, *jtail;      /* sent, not yet answered, in order */
    uint32_t njobs;
    struct batch batches[MAX_DEPTH];
    uint32_t bhead, bcount;
    struct req *chead, *ctail;      /* write group: answered, waiting for the commit */
    bool group_retry;               /* the group failed: retry its writes alone */
    char *rows; uint32_t rows_len, rows_cap;   /* current job's rows */
    struct pgval *vals; uint32_t nvals, vals_cap;
    struct pgval *gvals; uint32_t gvals_cap;   /* E2: one request's view of a grouped execution */
    uint32_t nrows; uint16_t ncols;
    char sqlstate[6];
    uint32_t gen;                   /* bumped on reconnect: completions of the old socket are ignored */
    uint64_t batches_sent, jobs_sent;
};

struct posted { struct posted *next; };

struct loop {
    int idx;
    struct io_uring ring;
    int ring_fd;
    volatile int ready;
    int lfd;
    bool fixed, bufring, accept_armed, efd_armed;
    int64_t accept_retry_at;
    struct io_uring_buf_ring *br;
    char *bufs;
    uint32_t nbufs, bufsz, bmask, br_pending;
    struct conn *conns;
    uint32_t maxconns;
    int64_t now_ms;
    /* scheduler */
    struct req *rq_head, *rq_tail; uint32_t rq_len;
    struct req *wq_head, *wq_tail; uint32_t wq_len;
    struct pgc *pg;
    int npg;
    /* request slab */
    struct req *free_reqs;
    /* cross-thread posts (eventfd fallback for MSG_RING) */
    pthread_mutex_t post_mu;
    uintptr_t *posts;
    uint32_t nposts, posts_cap;
    int efd;
    uint64_t efd_val;
    /* connections whose multishot receive ran out of buffers */
    uint32_t *rearm;
    uint32_t nrearm, rearm_cap;
    struct lstats st;
    char scratch[65536];
};

struct io_uring_sqe *get_sqe(struct loop *l);
void conn_try_input(struct loop *l, struct conn *c);

/* sched.c */
void sched_init(struct loop *l);
void sched_start(struct loop *l);    /* loop thread: arm receives on the Postgres connections */
void sched_pump(struct loop *l);     /* send queued work to idle Postgres connections */
void sched_cqe(struct loop *l, struct io_uring_cqe *cqe);
void sched_reconnected(struct loop *l, struct pgc *c, int fd);
void sched_tick(struct loop *l);

/* pg.c: blocking connection setup (startup, SCRAM, Parse + Describe of every statement) */
struct pgconf { char host[256], port[16], user[128], password[256], dbname[128]; bool unix_socket; };
bool pg_parse_url(const char *url, struct pgconf *pc);
int pg_connect(const struct pgconf *pc, bool writer, char *err, size_t errn);
extern struct pgconf pgconf;
void pg_reconnect_async(struct pgc *c);   /* connector thread; posts the new fd back to c->loop */
