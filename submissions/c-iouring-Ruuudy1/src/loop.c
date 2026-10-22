/* loop.c: one io_uring per event-loop thread. Multishot accept into the ring's registered file table, multishot
 * receive with buffers from a provided-buffer ring (idle connections own no buffer), one send per response, and
 * completion batching: after a busy pass the loop waits for URING_MIN_CQE completions or URING_WAIT_US.
 * Fallbacks: plain descriptors when file registration fails, multishot poll + recv() when buffer rings do. */
#include "loop.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif

#define BGID 7
#define MAX_INPUT_SLACK (64 * 1024)

struct loop **loops;
int nloops;

static int make_listener(void) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE}, *res;
    int rc = getaddrinfo(cfg.host, cfg.port, &hints, &res);
    if (rc) die("listen address %s:%s: %s", cfg.host, cfg.port, gai_strerror(rc));
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) die("socket: %s", strerror(errno));
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* accepted sockets inherit it */
    if (bind(fd, res->ai_addr, res->ai_addrlen) < 0) die("bind %s:%s: %s", cfg.host, cfg.port, strerror(errno));
    if (listen(fd, 65535) < 0) die("listen: %s", strerror(errno));
    freeaddrinfo(res);
    return fd;
}

struct loop *loop_create(int idx) {
    struct loop *l = calloc(1, sizeof *l);
    if (!l) die("out of memory");
    l->idx = idx;
    l->lfd = make_listener();
    l->efd = eventfd(0, EFD_CLOEXEC);
    if (l->efd < 0) die("eventfd: %s", strerror(errno));
    pthread_mutex_init(&l->post_mu, NULL);
    sched_init(l);
    return l;
}

int64_t loop_now_ms(struct loop *l) { return l->now_ms; }

/* ---------- submission ---------- */

struct io_uring_sqe *get_sqe(struct loop *l) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&l->ring);
    while (!sqe) {
        io_uring_submit(&l->ring);
        sqe = io_uring_get_sqe(&l->ring);
    }
    return sqe;
}

static inline void sqe_fd(struct loop *l, struct io_uring_sqe *sqe, uint32_t slot) {
    if (l->fixed) sqe->flags |= IOSQE_FIXED_FILE;
    (void)slot;
}

static void arm_accept(struct loop *l) {
    struct io_uring_sqe *sqe = get_sqe(l);
    if (l->fixed) io_uring_prep_multishot_accept_direct(sqe, l->lfd, NULL, NULL, SOCK_NONBLOCK);
    else io_uring_prep_multishot_accept(sqe, l->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, UD(UD_ACCEPT, 0, 0));
    l->accept_armed = true;
}

static void arm_recv(struct loop *l, struct conn *c, uint32_t slot) {
    struct io_uring_sqe *sqe = get_sqe(l);
    if (l->bufring) {
        io_uring_prep_recv_multishot(sqe, (int)slot, NULL, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = BGID;
        sqe_fd(l, sqe, slot);
        io_uring_sqe_set_data64(sqe, UD(UD_RECV, c->gen, slot));
    } else {
        io_uring_prep_poll_multishot(sqe, (int)slot, POLLIN | POLLRDHUP);
        io_uring_sqe_set_data64(sqe, UD(UD_POLL, c->gen, slot));
    }
    c->flags |= C_RECV;
}

static void arm_eventfd(struct loop *l) {
    struct io_uring_sqe *sqe = get_sqe(l);
    io_uring_prep_read(sqe, l->efd, &l->efd_val, sizeof l->efd_val, 0);
    io_uring_sqe_set_data64(sqe, UD(UD_EVENTFD, 0, 0));
}

static inline char *buf_addr(struct loop *l, uint32_t bid) { return l->bufs + (size_t)bid * l->bufsz; }

static inline void buf_recycle(struct loop *l, uint32_t bid) {
    io_uring_buf_ring_add(l->br, buf_addr(l, bid), l->bufsz, (unsigned short)bid, (int)l->bmask, (int)l->br_pending++);
}

/* ---------- requests ---------- */

struct req *req_alloc(struct loop *l) {
    struct req *r = l->free_reqs;
    if (!r) {
        struct req *chunk = aligned_alloc(64, sizeof(struct req) * 64);
        if (!chunk) die("out of memory");
        for (int i = 0; i < 64; i++) { chunk[i].next = l->free_reqs; l->free_reqs = &chunk[i]; }
        r = l->free_reqs;
    }
    l->free_reqs = r->next;
    memset(r, 0, offsetof(struct req, a_inline));
    r->loop = l;
    return r;
}

void req_free(struct req *r) {
    struct loop *l = r->loop;
    ob_free(&r->out);
    arena_reset(r);
    r->next = l->free_reqs;
    l->free_reqs = r;
}

/* ---------- connections ---------- */

static void conn_finish_close(struct loop *l, struct conn *c, uint32_t slot) {
    if (c->flags & (C_RECV | C_SENDING)) return; /* wait for the outstanding operations */
    struct io_uring_sqe *sqe = get_sqe(l);
    if (l->fixed) io_uring_prep_close_direct(sqe, slot);
    else io_uring_prep_close(sqe, (int)slot);
    io_uring_sqe_set_data64(sqe, UD(UD_CLOSE, c->gen, slot));
    if (c->in) pool_free(c->in, c->in_cap);
    c->in = NULL;
    c->in_len = c->in_cap = 0;
    c->flags = 0;
    c->gen++;
    l->st.closes++;
    l->st.open_conns--;
}

static void conn_close(struct loop *l, struct conn *c) {
    uint32_t slot = (uint32_t)(c - l->conns);
    if (!(c->flags & C_OPEN)) return;
    c->flags |= C_CLOSING;
    if (c->req && !(c->flags & C_SENDING)) {
        c->req->flags |= RQ_ORPHAN; /* still waiting for Postgres or bcrypt: freed when that finishes */
        c->req = NULL;
    }
    if ((c->flags & C_RECV) && !(c->flags & C_CANCELED)) {
        struct io_uring_sqe *sqe = get_sqe(l);
        io_uring_prep_cancel64(sqe, UD(l->bufring ? UD_RECV : UD_POLL, c->gen, slot), 0);
        io_uring_sqe_set_data64(sqe, UD(UD_CANCEL, c->gen, slot));
        c->flags |= C_CANCELED;
    }
    conn_finish_close(l, c, slot);
}

static void conn_send(struct loop *l, struct conn *c, struct req *r) {
    uint32_t slot = (uint32_t)(c - l->conns);
    uint32_t end = (r->flags & RQ_HEAD) ? HDR_RESERVE : r->out.len;
    uint32_t start = r->hdr_off + c->sent;
    struct io_uring_sqe *sqe = get_sqe(l);
    io_uring_prep_send(sqe, (int)slot, r->out.p + start, end - start, MSG_NOSIGNAL | MSG_WAITALL);
    sqe_fd(l, sqe, slot);
    io_uring_sqe_set_data64(sqe, UD(UD_SEND, c->gen, slot));
    c->flags |= C_SENDING;
}

void req_complete(struct req *r) {
    struct loop *l = r->loop;
    if (r->flags & RQ_ORPHAN) { req_free(r); return; }
    struct conn *c = &l->conns[r->slot];
    if (c->gen != r->gen || c->req != r || !(c->flags & C_OPEN)) { req_free(r); return; }
    int64_t lat = l->now_ms - r->t0_ms;
    l->st.lat_sum_ms += (uint64_t)(lat > 0 ? lat : 0);
    if ((uint64_t)lat > l->st.lat_max_ms) l->st.lat_max_ms = (uint64_t)lat;
    if (c->flags & C_SENDING) { r->flags |= RQ_PENDING; return; } /* a 100 Continue is still going out */
    c->sent = 0;
    conn_send(l, c, r);
}

static bool in_append(struct loop *l, struct conn *c, const char *data, uint32_t n) {
    if (c->in_len + n > c->in_cap) {
        uint64_t want = (uint64_t)c->in_len + n;
        if (want > (uint64_t)cfg.max_header + cfg.max_body + MAX_INPUT_SLACK) return false;
        uint32_t cap = c->in_cap ? c->in_cap : 4096;
        while (cap < want) cap *= 2;
        char *p = pool_alloc(&cap);
        if (c->in_len) memcpy(p, c->in, c->in_len);
        if (c->in) pool_free(c->in, c->in_cap);
        c->in = p;
        c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, data, n);
    c->in_len += n;
    (void)l;
    return true;
}

static void reply_and_close(struct loop *l, struct conn *c, int status, const char *msg) {
    struct req *r = req_alloc(l);
    r->slot = (uint32_t)(c - l->conns);
    r->gen = c->gen;
    r->t0_ms = l->now_ms;
    r->flags |= RQ_CLOSE;
    c->req = r;
    resp_error(r, status, msg);
    req_complete(r);
}

static void send_continue(struct loop *l, struct conn *c) {
    static const char CONT[] = "HTTP/1.1 100 Continue\r\n\r\n";
    uint32_t slot = (uint32_t)(c - l->conns);
    struct io_uring_sqe *sqe = get_sqe(l);
    io_uring_prep_send(sqe, (int)slot, CONT, sizeof CONT - 1, MSG_NOSIGNAL | MSG_WAITALL);
    sqe_fd(l, sqe, slot);
    io_uring_sqe_set_data64(sqe, UD(UD_CONTINUE, c->gen, slot));
    c->flags |= C_SENDING | C_CONTINUE;
}

/* Parses one request from data. Returns the bytes it used, 0 if the request is incomplete. Everything the
 * request needs is copied into its arena, so the caller can reuse data afterwards. */
static uint32_t conn_process(struct loop *l, struct conn *c, const char *data, uint32_t n) {
    struct http_req h;
    int rc = http_parse(data, n, &h, cfg.max_header);
    if (rc == 0) return 0;
    if (rc < 0) {
        if (rc == -2) reply_and_close(l, c, 431, "request headers too large");
        else if (rc == -3) reply_and_close(l, c, 501, "unsupported transfer encoding");
        else reply_and_close(l, c, 400, "bad request");
        return n;
    }
    const char *body = data + h.header_n;
    uint32_t body_n = 0, total = h.header_n;
    char *decoded = NULL;
    uint32_t dcap = 0;
    if (h.chunked) {
        dcap = n - h.header_n + 1;
        decoded = pool_alloc(&dcap);
        uint32_t used;
        int cr = http_chunked(data + h.header_n, n - h.header_n, decoded, &body_n, &used, cfg.max_body);
        if (cr <= 0) {
            pool_free(decoded, dcap);
            if (cr < 0) { reply_and_close(l, c, 400, "bad chunked body"); return n; }
            if (h.expect_continue && !(c->flags & C_CONTINUE)) send_continue(l, c);
            return 0;
        }
        body = decoded;
        total += used;
    } else if (h.content_length > 0) {
        if ((uint64_t)h.content_length > cfg.max_body) { reply_and_close(l, c, 413, "request body too large"); return n; }
        if (n - h.header_n < (uint64_t)h.content_length) {
            if (h.expect_continue && !(c->flags & C_CONTINUE)) send_continue(l, c);
            return 0;
        }
        body_n = (uint32_t)h.content_length;
        total += body_n;
    }
    c->flags &= (uint16_t)~C_CONTINUE;
    struct req *r = req_alloc(l);
    r->slot = (uint32_t)(c - l->conns);
    r->gen = c->gen;
    r->t0_ms = l->now_ms;
    if (!h.keepalive) r->flags |= RQ_CLOSE;
    else if (h.minor == 0) r->flags |= RQ_KEEPALIVE10;
    if (h.gzip_ok) r->flags |= RQ_GZIP;
    c->req = r;
    l->st.requests++;
    api_dispatch(r, &h, body, body_n);
    if (decoded) pool_free(decoded, dcap);
    return total;
}

void conn_try_input(struct loop *l, struct conn *c) {
    while (!c->req && c->in_len && (c->flags & C_OPEN) && !(c->flags & C_CLOSING)) {
        uint32_t used = conn_process(l, c, c->in, c->in_len);
        if (!used) break;
        if (used >= c->in_len) c->in_len = 0;
        else {
            memmove(c->in, c->in + used, c->in_len - used);
            c->in_len -= used;
        }
    }
    if (!c->in_len && c->in) {
        pool_free(c->in, c->in_cap);
        c->in = NULL;
        c->in_cap = 0;
    }
}

static void on_data(struct loop *l, struct conn *c, const char *data, uint32_t n) {
    if (c->req || c->in_len) {
        if (!in_append(l, c, data, n)) { conn_close(l, c); return; }
        if (!c->req) conn_try_input(l, c);
        return;
    }
    uint32_t used = 0;
    while (used < n && !c->req && !(c->flags & C_CLOSING)) {
        uint32_t u = conn_process(l, c, data + used, n - used);
        if (!u) break;
        used += u;
    }
    if (used < n && (c->flags & C_OPEN) && !(c->flags & C_CLOSING) && !in_append(l, c, data + used, n - used)) conn_close(l, c);
}

/* ---------- completions ---------- */

static void on_accept(struct loop *l, struct io_uring_cqe *cqe) {
    if (!(cqe->flags & IORING_CQE_F_MORE)) l->accept_armed = false;
    if (cqe->res < 0) {
        if (cqe->res == -ENFILE || cqe->res == -EMFILE || cqe->res == -ENOMEM || cqe->res == -ENOBUFS) {
            /* out of descriptors or slots: pause accepting for a moment instead of spinning */
            l->accept_retry_at = clock_ms() + 100;
            if (l->accept_armed) {
                struct io_uring_sqe *sqe = get_sqe(l);
                io_uring_prep_cancel64(sqe, UD(UD_ACCEPT, 0, 0), 0);
                io_uring_sqe_set_data64(sqe, UD(UD_CANCEL, 0, 0));
            }
        } else if (cqe->res != -ECANCELED) {
            log_msg("accept: %s", strerror(-cqe->res));
        }
        return;
    }
    uint32_t slot = (uint32_t)cqe->res;
    if (slot >= l->maxconns) {
        struct io_uring_sqe *sqe = get_sqe(l);
        if (l->fixed) io_uring_prep_close_direct(sqe, slot);
        else io_uring_prep_close(sqe, (int)slot);
        io_uring_sqe_set_data64(sqe, UD(UD_CLOSE, 0, slot));
        return;
    }
    struct conn *c = &l->conns[slot];
    uint32_t gen = c->gen + 1;
    memset(c, 0, sizeof *c);
    c->gen = gen;
    c->flags = C_OPEN;
    l->st.accepts++;
    l->st.open_conns++;
    arm_recv(l, c, slot);
}

static void on_recv(struct loop *l, struct io_uring_cqe *cqe) {
    uint32_t slot = UD_SLOT(cqe->user_data);
    bool has_buf = cqe->flags & IORING_CQE_F_BUFFER;
    uint32_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    struct conn *c = slot < l->maxconns ? &l->conns[slot] : NULL;
    if (!c || (c->gen & 0xffffff) != UD_GEN(cqe->user_data) || !(c->flags & C_OPEN)) {
        if (has_buf) buf_recycle(l, bid);
        return;
    }
    bool more = cqe->flags & IORING_CQE_F_MORE;
    if (!more) c->flags &= (uint16_t)~C_RECV;
    if (cqe->res > 0) {
        if (has_buf) {
            if (!(c->flags & C_CLOSING)) on_data(l, c, buf_addr(l, bid), (uint32_t)cqe->res);
            buf_recycle(l, bid);
        }
        if (!more && (c->flags & C_OPEN) && !(c->flags & C_CLOSING)) arm_recv(l, c, slot);
    } else if (cqe->res == -ENOBUFS && !(c->flags & C_CLOSING)) {
        if (l->nrearm == l->rearm_cap) {
            l->rearm_cap = l->rearm_cap ? l->rearm_cap * 2 : 256;
            l->rearm = realloc(l->rearm, sizeof(uint32_t) * l->rearm_cap);
            if (!l->rearm) die("out of memory");
        }
        l->rearm[l->nrearm++] = slot;
        c->flags |= C_REARM;
    } else if (!more) {
        conn_close(l, c); /* EOF, reset, or our cancel */
    }
    if (c->flags & C_CLOSING) conn_finish_close(l, c, slot);
}

static void on_poll(struct loop *l, struct io_uring_cqe *cqe) {
    uint32_t slot = UD_SLOT(cqe->user_data);
    struct conn *c = slot < l->maxconns ? &l->conns[slot] : NULL;
    if (!c || (c->gen & 0xffffff) != UD_GEN(cqe->user_data) || !(c->flags & C_OPEN)) return;
    bool more = cqe->flags & IORING_CQE_F_MORE;
    if (!more) c->flags &= (uint16_t)~C_RECV;
    if (cqe->res < 0) { conn_close(l, c); return; }
    for (;;) {
        if (c->flags & C_CLOSING) break;
        ssize_t r = recv((int)slot, l->scratch, sizeof l->scratch, MSG_DONTWAIT);
        if (r > 0) { on_data(l, c, l->scratch, (uint32_t)r); if ((size_t)r < sizeof l->scratch) break; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        conn_close(l, c);
        break;
    }
    if (!more && (c->flags & C_OPEN) && !(c->flags & C_CLOSING)) arm_recv(l, c, slot);
    if (c->flags & C_CLOSING) conn_finish_close(l, c, slot);
}

static void on_send(struct loop *l, struct io_uring_cqe *cqe) {
    uint32_t slot = UD_SLOT(cqe->user_data);
    struct conn *c = slot < l->maxconns ? &l->conns[slot] : NULL;
    if (!c || (c->gen & 0xffffff) != UD_GEN(cqe->user_data)) return;
    c->flags &= (uint16_t)~C_SENDING;
    struct req *r = c->req;
    if (!r) { if (c->flags & C_CLOSING) conn_finish_close(l, c, slot); return; }
    if (cqe->res < 0) {
        c->req = NULL;
        req_free(r);
        conn_close(l, c);
        return;
    }
    uint32_t end = (r->flags & RQ_HEAD) ? HDR_RESERVE : r->out.len;
    c->sent += (uint32_t)cqe->res;
    if (r->hdr_off + c->sent < end && !(c->flags & C_CLOSING)) { conn_send(l, c, r); return; }
    bool close_after = r->flags & RQ_CLOSE;
    c->req = NULL;
    c->sent = 0;
    req_free(r);
    l->st.responses++;
    if (close_after || (c->flags & C_CLOSING)) { conn_close(l, c); return; }
    if (c->in_len) conn_try_input(l, c);
}

static void on_continue(struct loop *l, struct io_uring_cqe *cqe) {
    uint32_t slot = UD_SLOT(cqe->user_data);
    struct conn *c = slot < l->maxconns ? &l->conns[slot] : NULL;
    if (!c || (c->gen & 0xffffff) != UD_GEN(cqe->user_data)) return;
    c->flags &= (uint16_t)~C_SENDING;
    if (cqe->res < 0 || (c->flags & C_CLOSING)) {
        if (c->req) { c->req->flags |= RQ_ORPHAN; if (c->req->flags & RQ_PENDING) req_free(c->req); c->req = NULL; }
        conn_close(l, c);
        return;
    }
    if (c->req && (c->req->flags & RQ_PENDING)) {
        c->req->flags &= (uint16_t)~RQ_PENDING;
        c->sent = 0;
        conn_send(l, c, c->req);
    }
}

/* A pointer posted by another thread: a finished bcrypt job, or a re-established Postgres connection. */
static void on_posted(struct loop *l, uintptr_t v) {
    if (v & 1) {
        struct { struct pgc *c; int fd; } *m = (void *)(v & ~(uintptr_t)1);
        sched_reconnected(l, m->c, m->fd);
        free(m);
        return;
    }
    struct bjob *j = (struct bjob *)v;
    struct req *r = (struct req *)((char *)j - offsetof(struct req, bj));
    if (r->flags & RQ_ORPHAN) { req_free(r); return; }
    api_bcrypt_done(r);
}

static void on_eventfd(struct loop *l) {
    pthread_mutex_lock(&l->post_mu);
    uint32_t n = l->nposts;
    uintptr_t local[256], *list = n <= 256 ? local : malloc(sizeof(uintptr_t) * n);
    if (!list) die("out of memory");
    memcpy(list, l->posts, sizeof(uintptr_t) * n);
    l->nposts = 0;
    pthread_mutex_unlock(&l->post_mu);
    for (uint32_t i = 0; i < n; i++) on_posted(l, list[i]);
    if (list != local) free(list);
    arm_eventfd(l);
}

static void handle(struct loop *l, struct io_uring_cqe *cqe) {
    switch (UD_TYPE(cqe->user_data)) {
    case UD_ACCEPT: on_accept(l, cqe); break;
    case UD_RECV: on_recv(l, cqe); break;
    case UD_POLL: on_poll(l, cqe); break;
    case UD_SEND: on_send(l, cqe); break;
    case UD_CONTINUE: on_continue(l, cqe); break;
    case UD_PG_RECV: case UD_PG_SEND: sched_cqe(l, cqe); break;
    case UD_MSG: on_posted(l, (uintptr_t)UD_PTR(cqe->user_data)); break;
    case UD_EVENTFD: on_eventfd(l); break;
    default: break; /* close and cancel completions */
    }
}

/* ---------- cross-thread posts ---------- */

static __thread struct io_uring *tl_ring;
static __thread bool tl_ring_failed;

void loop_post(struct loop *l, void *ptr) {
    uint64_t data = UD(UD_MSG, 0, 0) | ((uintptr_t)ptr & 0x00ffffffffffffffULL);
    if (!tl_ring && !tl_ring_failed) {
        tl_ring = malloc(sizeof *tl_ring);
        if (!tl_ring || io_uring_queue_init(8, tl_ring, 0) < 0) { free(tl_ring); tl_ring = NULL; tl_ring_failed = true; }
    }
    if (tl_ring) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(tl_ring);
        if (sqe) {
            io_uring_prep_msg_ring(sqe, l->ring_fd, 0, data, 0);
            struct io_uring_cqe *cqe;
            if (io_uring_submit_and_wait(tl_ring, 1) >= 0 && io_uring_wait_cqe(tl_ring, &cqe) == 0) {
                int res = cqe->res;
                io_uring_cqe_seen(tl_ring, cqe);
                if (res >= 0) return;
            }
        }
        io_uring_queue_exit(tl_ring);
        free(tl_ring);
        tl_ring = NULL;
        tl_ring_failed = true;
        log_msg("IORING_OP_MSG_RING unavailable; using an eventfd");
    }
    pthread_mutex_lock(&l->post_mu);
    if (l->nposts == l->posts_cap) {
        l->posts_cap = l->posts_cap ? l->posts_cap * 2 : 64;
        l->posts = realloc(l->posts, sizeof(uintptr_t) * l->posts_cap);
        if (!l->posts) die("out of memory");
    }
    l->posts[l->nposts++] = (uintptr_t)ptr;
    pthread_mutex_unlock(&l->post_mu);
    uint64_t one = 1;
    ssize_t w = write(l->efd, &one, sizeof one);
    (void)w;
}

/* ---------- setup and the loop ---------- */

static void ring_setup(struct loop *l) {
    static const unsigned flags[] = {
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL,
        IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SUBMIT_ALL,
        0,
    };
    int rc = -1;
    for (size_t i = 0; i < ARRAY_LEN(flags); i++) {
        struct io_uring_params p = {0};
        p.flags = flags[i] | IORING_SETUP_CQSIZE;
        p.cq_entries = 16384;
        rc = io_uring_queue_init_params(4096, &l->ring, &p);
        if (rc == 0) {
            if (i) log_msg("loop %d: io_uring without some setup flags (fallback %zu)", l->idx, i);
            break;
        }
    }
    if (rc) die("io_uring_queue_init: %s", strerror(-rc));
    l->ring_fd = l->ring.ring_fd;

    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    uint32_t nfiles = rl.rlim_cur > (1u << 20) ? (1u << 20) : (uint32_t)rl.rlim_cur;
    l->fixed = getenv("NO_FIXED_FILES") == NULL && io_uring_register_files_sparse(&l->ring, nfiles) == 0;

    l->nbufs = 4096;
    l->bufsz = 4096;
    l->bmask = (uint32_t)io_uring_buf_ring_mask(l->nbufs);
    int err = 0;
    l->br = getenv("NO_BUF_RING") ? NULL : io_uring_setup_buf_ring(&l->ring, l->nbufs, BGID, 0, &err);
    if (l->br) {
        if (posix_memalign((void **)&l->bufs, 4096, (size_t)l->nbufs * l->bufsz)) die("out of memory");
        for (uint32_t i = 0; i < l->nbufs; i++) io_uring_buf_ring_add(l->br, buf_addr(l, i), l->bufsz, (unsigned short)i, (int)l->bmask, (int)i);
        io_uring_buf_ring_advance(l->br, (int)l->nbufs);
        l->bufring = true;
    } else {
        /* poll + recv() needs real descriptors */
        if (l->fixed) io_uring_unregister_files(&l->ring);
        l->fixed = false;
        log_msg("loop %d: no provided buffer rings (%s); using multishot poll + recv", l->idx, strerror(-err));
    }
    l->maxconns = l->fixed ? nfiles : (uint32_t)(rl.rlim_cur > (1u << 22) ? (1u << 22) : rl.rlim_cur);
    l->conns = mmap(NULL, sizeof(struct conn) * (size_t)l->maxconns, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (l->conns == MAP_FAILED) die("mmap connections: %s", strerror(errno));
}

void *loop_run(void *arg) {
    struct loop *l = arg;
    char name[16];
    snprintf(name, sizeof name, "loop-%d", l->idx);
    pthread_setname_np(pthread_self(), name);
    ring_setup(l);
    l->now_ms = realtime_ms();
    arm_accept(l);
    arm_eventfd(l);
    sched_start(l);
    __atomic_store_n(&l->ready, 1, __ATOMIC_RELEASE);
    log_msg("loop %d: %s descriptors, %s, %d postgres connections", l->idx, l->fixed ? "registered" : "plain",
            l->bufring ? "provided buffer ring" : "poll + recv", l->npg);
    unsigned busy = 0;
    for (;;) {
        sched_pump(l);
        if (l->br_pending) {
            io_uring_buf_ring_advance(l->br, (int)l->br_pending);
            l->br_pending = 0;
        }
        for (uint32_t i = 0; i < l->nrearm; i++) {
            struct conn *c = &l->conns[l->rearm[i]];
            if ((c->flags & (C_OPEN | C_REARM | C_RECV | C_CLOSING)) == (C_OPEN | C_REARM)) {
                c->flags &= (uint16_t)~C_REARM;
                arm_recv(l, c, l->rearm[i]);
            }
        }
        l->nrearm = 0;
        if (!l->accept_armed && clock_ms() >= l->accept_retry_at) arm_accept(l);
        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts;
        unsigned wait_nr;
        if (busy >= (unsigned)cfg.uring_busy_cqe) {
            wait_nr = (unsigned)cfg.uring_min_cqe;
            ts.tv_sec = 0;
            ts.tv_nsec = (long long)cfg.uring_wait_us * 1000;
        } else {
            wait_nr = 1;
            ts.tv_sec = l->accept_armed ? 1 : 0;
            ts.tv_nsec = l->accept_armed ? 0 : 100 * 1000 * 1000;
        }
        io_uring_submit_and_wait_timeout(&l->ring, &cqe, wait_nr, &ts, NULL);
        l->now_ms = realtime_ms();
        unsigned head, n = 0;
        io_uring_for_each_cqe(&l->ring, head, cqe) {
            handle(l, cqe);
            n++;
        }
        io_uring_cq_advance(&l->ring, n);
        busy = n;
        l->st.cqes += n;
        l->st.waits++;
    }
    return NULL;
}
