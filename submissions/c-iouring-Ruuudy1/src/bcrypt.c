/* bcrypt.c: cost-10 bcrypt ($2a$) with N independent hashes computed in lockstep, so the dependent S-box loads
 * of one hash overlap with the others' (the algorithm is unchanged: every hash is a full EksBlowfish run).
 * The key schedule follows golang.org/x/crypto/bcrypt, which the reference server uses: the password plus a
 * trailing NUL, repeated to 72 bytes. Workers batch jobs from one queue and post results to the owning loop. */
#include "conduit.h"
#include "bf_init.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAXL 8
#define PS_WORDS (18 + 1024)

typedef struct { uint32_t PS[PS_WORDS]; } bf_state; /* P[18] then S[4][256] */

static const uint32_t MAGIC[6] = {0x4F727068, 0x65616E42, 0x65686F6C, 0x64657253, 0x63727944, 0x6F756274}; /* OrpheanBeholderScryDoubt */
static const char BC64[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

#define BF_F(st, x) ((((st).PS[18 + ((x) >> 24)] + (st).PS[18 + 256 + (((x) >> 16) & 0xff)]) ^ \
                      (st).PS[18 + 512 + (((x) >> 8) & 0xff)]) + (st).PS[18 + 768 + ((x) & 0xff)])

static inline __attribute__((always_inline)) void bf_enc(const int n, bf_state *st, uint32_t *L, uint32_t *R) {
#pragma GCC unroll 8
    for (int l = 0; l < n; l++) L[l] ^= st[l].PS[0];
#pragma GCC unroll 8
    for (int i = 1; i <= 16; i += 2) {
#pragma GCC unroll 8
        for (int l = 0; l < n; l++) R[l] ^= BF_F(st[l], L[l]) ^ st[l].PS[i];
#pragma GCC unroll 8
        for (int l = 0; l < n; l++) L[l] ^= BF_F(st[l], R[l]) ^ st[l].PS[i + 1];
    }
#pragma GCC unroll 8
    for (int l = 0; l < n; l++) { uint32_t t = R[l]; R[l] = L[l]; L[l] = t ^ st[l].PS[17]; }
}

/* ExpandKey with a zero salt: re-encrypt the whole state, chaining through P and the S-boxes. */
static inline __attribute__((always_inline)) void bf_body(const int n, bf_state *st) {
    uint32_t L[MAXL] = {0}, R[MAXL] = {0};
    for (int k = 0; k < PS_WORDS; k += 2) {
        bf_enc(n, st, L, R);
#pragma GCC unroll 8
        for (int l = 0; l < n; l++) { st[l].PS[k] = L[l]; st[l].PS[k + 1] = R[l]; }
    }
}

static inline __attribute__((always_inline)) void eks(const int n, bf_state *st, const uint32_t (*key)[18],
                                                      const uint32_t (*salt)[4], uint32_t rounds, uint32_t (*out)[6]) {
    for (int l = 0; l < n; l++) {
        for (int i = 0; i < 18; i++) st[l].PS[i] = BF_INIT_P[i] ^ key[l][i];
        memcpy(st[l].PS + 18, BF_INIT_S, sizeof BF_INIT_S);
    }
    uint32_t L[MAXL] = {0}, R[MAXL] = {0};
    for (int k = 0; k < PS_WORDS; k += 2) {
        int s = (k & 2) ? 2 : 0; /* salt words are consumed cyclically, two per block */
#pragma GCC unroll 8
        for (int l = 0; l < n; l++) { L[l] ^= salt[l][s]; R[l] ^= salt[l][s + 1]; }
        bf_enc(n, st, L, R);
#pragma GCC unroll 8
        for (int l = 0; l < n; l++) { st[l].PS[k] = L[l]; st[l].PS[k + 1] = R[l]; }
    }
    for (uint32_t r = 0; r < rounds; r++) {
        for (int l = 0; l < n; l++) for (int i = 0; i < 18; i++) st[l].PS[i] ^= key[l][i];
        bf_body(n, st);
        for (int l = 0; l < n; l++) for (int i = 0; i < 18; i++) st[l].PS[i] ^= salt[l][i & 3];
        bf_body(n, st);
    }
    for (int b = 0; b < 3; b++) {
#pragma GCC unroll 8
        for (int l = 0; l < n; l++) { L[l] = MAGIC[2 * b]; R[l] = MAGIC[2 * b + 1]; }
        for (int i = 0; i < 64; i++) bf_enc(n, st, L, R);
        for (int l = 0; l < n; l++) { out[l][2 * b] = L[l]; out[l][2 * b + 1] = R[l]; }
    }
}

#define EKS_N(N) \
    static __attribute__((noinline)) void eks_##N(bf_state *st, const uint32_t (*key)[18], const uint32_t (*salt)[4], \
                                                  uint32_t rounds, uint32_t (*out)[6]) { eks(N, st, key, salt, rounds, out); }
EKS_N(1) EKS_N(2) EKS_N(3) EKS_N(4) EKS_N(5) EKS_N(6) EKS_N(7) EKS_N(8)
typedef void (*eks_fn)(bf_state *, const uint32_t (*)[18], const uint32_t (*)[4], uint32_t, uint32_t (*)[6]);
static const eks_fn EKS[MAXL + 1] = {NULL, eks_1, eks_2, eks_3, eks_4, eks_5, eks_6, eks_7, eks_8};

/* ---------- encoding ---------- */

static void bc64_encode(char *dst, const uint8_t *src, int n) {
    int i = 0, o = 0;
    while (i < n) {
        uint32_t c1 = src[i++];
        dst[o++] = BC64[c1 >> 2];
        c1 = (c1 & 3) << 4;
        if (i >= n) { dst[o++] = BC64[c1]; break; }
        uint32_t c2 = src[i++];
        c1 |= c2 >> 4;
        dst[o++] = BC64[c1];
        c1 = (c2 & 0x0f) << 2;
        if (i >= n) { dst[o++] = BC64[c1]; break; }
        c2 = src[i++];
        c1 |= c2 >> 6;
        dst[o++] = BC64[c1];
        dst[o++] = BC64[c2 & 0x3f];
    }
}

static int bc64_val(char c) {
    const char *p = c ? strchr(BC64, c) : NULL;
    return p ? (int)(p - BC64) : -1;
}

/* Decodes n output bytes from bcrypt base64 (unused trailing bits ignored, as Go's non-strict decoder does). */
static bool bc64_decode(uint8_t *dst, int n, const char *src) {
    int o = 0, i = 0;
    while (o < n) {
        int c1 = bc64_val(src[i++]), c2 = bc64_val(src[i++]);
        if (c1 < 0 || c2 < 0) return false;
        dst[o++] = (uint8_t)(c1 << 2 | (c2 & 0x30) >> 4);
        if (o >= n) break;
        int c3 = bc64_val(src[i++]);
        if (c3 < 0) return false;
        dst[o++] = (uint8_t)((c2 & 0x0f) << 4 | (c3 & 0x3c) >> 2);
        if (o >= n) break;
        int c4 = bc64_val(src[i++]);
        if (c4 < 0) return false;
        dst[o++] = (uint8_t)((c3 & 0x03) << 6 | c4);
    }
    return true;
}

void bcrypt_set_key(struct bjob *j, const char *pw, uint32_t n) {
    uint32_t k = n < 72 ? n : 72;
    memcpy(j->key, pw, k);
    if (n < 72) j->key[k++] = 0; /* the trailing NUL is part of the key */
    j->keylen = (uint8_t)k;
}

static void key_words(uint32_t w[18], const uint8_t *k, uint32_t klen) {
    uint32_t j = 0;
    for (int i = 0; i < 18; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4; b++) { v = v << 8 | k[j]; j = j + 1 == klen ? 0 : j + 1; }
        w[i] = v;
    }
}

/* A parsed "$2?$NN$<22 salt><31 hash>". */
struct parsed { int cost; uint32_t salt[4]; uint8_t raw_salt[16]; char salt_chars[22]; char hash_chars[31]; char minor; };

static bool parse_hash(const char *h, struct parsed *p) {
    size_t n = strlen(h);
    if (n < 59 || h[0] != '$' || h[1] != '2') return false;
    size_t i = 3;
    p->minor = 0;
    if (h[2] != '$') { p->minor = h[2]; if (h[3] != '$') return false; i = 4; }
    if (n != i + 3 + 53 || h[i] < '0' || h[i] > '9' || h[i + 1] < '0' || h[i + 1] > '9' || h[i + 2] != '$') return false;
    p->cost = (h[i] - '0') * 10 + (h[i + 1] - '0');
    if (p->cost < 4 || p->cost > 31) return false;
    const char *s = h + i + 3;
    if (!bc64_decode(p->raw_salt, 16, s)) return false;
    for (int k = 0; k < 4; k++) p->salt[k] = rd32(p->raw_salt + 4 * k);
    memcpy(p->salt_chars, s, 22);
    memcpy(p->hash_chars, s + 22, 31);
    for (int k = 0; k < 31; k++) if (bc64_val(p->hash_chars[k]) < 0) return false;
    return true;
}

/* Runs up to MAXL hashes with the same cost in lockstep. */
static void run_lanes(int n, const uint32_t (*key)[18], const uint32_t (*salt)[4], int cost, uint32_t (*out)[6]) {
    static __thread bf_state *st;
    if (!st && posix_memalign((void **)&st, 64, sizeof(bf_state) * MAXL)) die("out of memory");
    EKS[n](st, key, salt, 1u << cost, out);
}

static void encode_out(char hash31[31], const uint32_t out[6]) {
    uint8_t raw[24];
    for (int k = 0; k < 6; k++) wr32(raw + 4 * k, out[k]);
    char tmp[33];
    bc64_encode(tmp, raw, 23);
    memcpy(hash31, tmp, 31);
}

/* ---------- synchronous API ---------- */

bool bcrypt_hash_sync(char out[64], const char *pw, uint32_t n, int cost, const uint8_t salt16[16]) {
    struct bjob j;
    bcrypt_set_key(&j, pw, n);
    uint32_t key[1][18], salt[1][4], o[1][6];
    key_words(key[0], j.key, j.keylen);
    for (int k = 0; k < 4; k++) salt[0][k] = rd32(salt16 + 4 * k);
    run_lanes(1, (const uint32_t (*)[18])key, (const uint32_t (*)[4])salt, cost, o);
    memcpy(out, "$2a$", 4);
    out[4] = (char)('0' + cost / 10);
    out[5] = (char)('0' + cost % 10);
    out[6] = '$';
    bc64_encode(out + 7, salt16, 16);
    encode_out(out + 29, o[0]);
    out[60] = 0;
    return true;
}

bool bcrypt_verify_sync(const char *pw, uint32_t n, const char *hash) {
    struct parsed p;
    if (!parse_hash(hash, &p)) return false;
    struct bjob j;
    bcrypt_set_key(&j, pw, n);
    uint32_t key[1][18], o[1][6];
    key_words(key[0], j.key, j.keylen);
    run_lanes(1, (const uint32_t (*)[18])key, (const uint32_t (*)[4])&p.salt, p.cost, o);
    char h[31];
    encode_out(h, o[0]);
    uint8_t d = 0;
    for (int k = 0; k < 31; k++) d |= (uint8_t)(h[k] ^ p.hash_chars[k]);
    return d == 0;
}

/* ---------- self-test and calibration ---------- */

/* Vectors: the seed's hash of "password123", hashes made by golang.org/x/crypto/bcrypt, and Openwall's
 * crypt_blowfish test vectors (all $2a$). */
static const struct { const char *pw, *hash; } VECTORS[] = {
    {"password123", "$2a$10$ZSfEoPvlcsj2XfvsSeK25em4NZ9Jm7ntGqXuLgQfHEeZPYB8899wq"},
    {"U*U", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.E5YPO9kmyuRGyh0XouQYb4YMJKvyOeW"},
    {"U*U*", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.VGOzA784oUp/Z0DY336zx7pLYAy0lwK"},
    {"U*U*U", "$2a$05$XXXXXXXXXXXXXXXXXXXXXOAcXxm9kjPGEMsLznoKqmqw7tc8WCx4a"},
    {"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789chars after 72 are ignored",
     "$2a$05$abcdefghijklmnopqrstuu5s2v8.iXieOjg/.AySBTTZIIVFJeBui"},
    {"", "$2a$05$CCCCCCCCCCCCCCCCCCCCC.7uG0VCzI2bS7j6ymqJi9CdcdxiRTWNy"},
#include "bcrypt_vectors.h"
};

bool bcrypt_selftest(void) {
    for (size_t i = 0; i < ARRAY_LEN(VECTORS); i++) {
        if (!bcrypt_verify_sync(VECTORS[i].pw, (uint32_t)strlen(VECTORS[i].pw), VECTORS[i].hash)) {
            log_msg("bcrypt self-test: vector %zu does not verify", i);
            return false;
        }
        struct parsed p;
        parse_hash(VECTORS[i].hash, &p);
        char h[64];
        bcrypt_hash_sync(h, VECTORS[i].pw, (uint32_t)strlen(VECTORS[i].pw), p.cost, p.raw_salt);
        if (memcmp(h + 7, VECTORS[i].hash + 7, 53) != 0) {
            log_msg("bcrypt self-test: vector %zu re-hashes to %s", i, h);
            return false;
        }
    }
    /* every interleave width must agree with the single-lane result */
    uint32_t key[MAXL][18], salt[MAXL][4], ref[MAXL][6], out[MAXL][6];
    for (int l = 0; l < MAXL; l++) {
        uint8_t raw[16];
        char pw[24];
        snprintf(pw, sizeof pw, "lane-%d-pw", l * 7 + 3);
        struct bjob j;
        bcrypt_set_key(&j, pw, (uint32_t)strlen(pw));
        key_words(key[l], j.key, j.keylen);
        for (int k = 0; k < 16; k++) raw[k] = (uint8_t)(l * 31 + k * 7);
        for (int k = 0; k < 4; k++) salt[l][k] = rd32(raw + 4 * k);
        run_lanes(1, (const uint32_t (*)[18])key[l], (const uint32_t (*)[4])salt[l], 4, &ref[l]);
    }
    for (int n = 2; n <= MAXL; n++) {
        run_lanes(n, (const uint32_t (*)[18])key, (const uint32_t (*)[4])salt, 4, out);
        if (memcmp(out, ref, sizeof(out[0]) * (size_t)n) != 0) {
            log_msg("bcrypt self-test: %d-lane result differs", n);
            return false;
        }
    }
    return true;
}

static double thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int bcrypt_calibrate(void) {
    /* 6 lanes keep 25 KB of S-boxes, which fits a 32 KB L1D; 8 lanes (34 KB) only win on an idle core with a
     * larger L1 and lose under load, when Postgres shares the core (or its SMT sibling). */
    static const int cand[] = {3, 4, 6};
    uint32_t key[MAXL][18] = {{0}}, salt[MAXL][4] = {{0}}, out[MAXL][6];
    for (int l = 0; l < MAXL; l++) { key[l][0] = (uint32_t)l; salt[l][1] = (uint32_t)l * 3; }
    int best = 4;
    double best_ns = 1e300;
    for (size_t c = 0; c < ARRAY_LEN(cand); c++) {
        int n = cand[c];
        double min = 1e300;
        for (int rep = 0; rep < 3; rep++) {
            double t0 = thread_cpu_ns();
            run_lanes(n, (const uint32_t (*)[18])key, (const uint32_t (*)[4])salt, 7, out);
            double per = (thread_cpu_ns() - t0) / n;
            if (per < min) min = per;
        }
        log_msg("bcrypt calibration: %d lanes, %.2f ms per cost-10 hash", n, min * 8 / 1e6);
        /* prefer fewer lanes unless more are at least 3% cheaper per hash (less waiting to fill a batch) */
        if (min < best_ns * 0.97) { best_ns = min; best = n; }
    }
    return best;
}

/* ---------- workers ---------- */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    struct bjob *head, *tail;
    uint32_t len;
    int lanes, wait_ms, nice;
} q = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

void bcrypt_submit(struct bjob *j) {
    j->next = NULL;
    j->enq_ms = clock_ms();
    pthread_mutex_lock(&q.mu);
    if (q.tail) q.tail->next = j;
    else q.head = j;
    q.tail = j;
    q.len++;
    pthread_mutex_unlock(&q.mu);
    pthread_cond_broadcast(&q.cv);
}

static void run_batch(struct bjob **jobs, int n) {
    /* jobs with the same cost run in lockstep; malformed stored hashes fail without hashing */
    uint32_t key[MAXL][18], salt[MAXL][4], out[MAXL][6];
    struct parsed p[MAXL];
    int cost[MAXL], idx[MAXL], done = 0;
    for (int i = 0; i < n; i++) {
        struct bjob *j = jobs[i];
        key_words(key[i], j->key, j->keylen);
        if (j->op == BJ_VERIFY) {
            if (!parse_hash(j->hash, &p[i])) { cost[i] = -1; continue; }
            memcpy(salt[i], p[i].salt, sizeof salt[i]);
            cost[i] = p[i].cost;
        } else {
            uint8_t raw[16];
            fill_random(raw, 16);
            memcpy(p[i].raw_salt, raw, 16);
            for (int k = 0; k < 4; k++) salt[i][k] = rd32(raw + 4 * k);
            cost[i] = 10;
        }
    }
    bool used[MAXL] = {false};
    while (done < n) {
        int c = -2, k = 0;
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            if (c == -2) c = cost[i];
            if (cost[i] == c) idx[k++] = i;
        }
        for (int i = 0; i < k; i++) used[idx[i]] = true;
        done += k;
        if (c < 0) {
            for (int i = 0; i < k; i++) jobs[idx[i]]->ok = 0;
            continue;
        }
        uint32_t kk[MAXL][18], ss[MAXL][4], oo[MAXL][6];
        for (int i = 0; i < k; i++) { memcpy(kk[i], key[idx[i]], sizeof kk[i]); memcpy(ss[i], salt[idx[i]], sizeof ss[i]); }
        run_lanes(k, (const uint32_t (*)[18])kk, (const uint32_t (*)[4])ss, c, oo);
        for (int i = 0; i < k; i++) memcpy(out[idx[i]], oo[i], sizeof out[0]);
    }
    for (int i = 0; i < n; i++) {
        struct bjob *j = jobs[i];
        if (cost[i] < 0) continue;
        char h[31];
        encode_out(h, out[i]);
        if (j->op == BJ_VERIFY) {
            uint8_t d = 0;
            for (int k = 0; k < 31; k++) d |= (uint8_t)(h[k] ^ p[i].hash_chars[k]);
            j->ok = d == 0;
        } else {
            memcpy(j->hash, "$2a$10$", 7);
            bc64_encode(j->hash + 7, p[i].raw_salt, 16);
            memcpy(j->hash + 29, h, 31);
            j->hash[60] = 0;
            j->ok = 1;
        }
    }
}

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_BATCH, &sp);
    setpriority(PRIO_PROCESS, (id_t)gettid(), q.nice);
    char name[16];
    snprintf(name, sizeof name, "bcrypt-%d", id);
    pthread_setname_np(pthread_self(), name);
    /* the first worker fills a batch for up to wait_ms; the others only take full batches or older jobs */
    int64_t wait = (int64_t)q.wait_ms * (id == 0 ? 1 : 2);
    struct bjob *jobs[MAXL];
    for (;;) {
        pthread_mutex_lock(&q.mu);
        int n = 0;
        for (;;) {
            if (q.len >= (uint32_t)q.lanes) break;
            if (q.len) {
                int64_t due = q.head->enq_ms + wait, now = clock_ms();
                if (now >= due) break;
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                int64_t ns = ts.tv_nsec + (due - now) * 1000000;
                ts.tv_sec += ns / 1000000000;
                ts.tv_nsec = ns % 1000000000;
                pthread_cond_timedwait(&q.cv, &q.mu, &ts);
            } else {
                pthread_cond_wait(&q.cv, &q.mu);
            }
        }
        while (n < q.lanes && q.head) {
            jobs[n++] = q.head;
            q.head = q.head->next;
            q.len--;
        }
        if (!q.head) q.tail = NULL;
        pthread_mutex_unlock(&q.mu);
        run_batch(jobs, n);
        for (int i = 0; i < n; i++) loop_post(jobs[i]->loop, jobs[i]);
    }
    return NULL;
}

void bcrypt_start(int threads, int lanes, int wait_ms, int nice) {
    q.lanes = lanes < 1 ? 1 : lanes > MAXL ? MAXL : lanes;
    q.wait_ms = wait_ms;
    q.nice = nice;
    for (int i = 0; i < threads; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, (void *)(intptr_t)i)) die("pthread_create bcrypt");
        pthread_detach(t);
    }
}
