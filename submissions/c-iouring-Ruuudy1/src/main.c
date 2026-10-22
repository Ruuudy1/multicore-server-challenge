/* main.c: configuration, startup and signals.
 *
 * Startup: bcrypt self-test (refuses to start on a mismatch) and lane calibration, Postgres connections (every
 * statement prepared and its row layout checked), then the event loops, the bcrypt workers and the optional
 * stats thread. Only DATABASE_URL, JWT_SECRET, HOST and PORT are set on the benchmark box; every other variable
 * exists for local sweeps, and its default is the tuned value. */
#include "loop.h"

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

struct config cfg;

static int env_int(const char *name, int def, int lo, int hi) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end;
    long x = strtol(v, &end, 10);
    if (*end || x < lo || x > hi) die("%s must be an integer in %d..%d, got %s", name, lo, hi, v);
    return (int)x;
}

static void load_config(void) {
    cfg.database_url = getenv("DATABASE_URL");
    cfg.jwt_secret = getenv("JWT_SECRET");
    if (!cfg.database_url || !*cfg.database_url || !cfg.jwt_secret || !*cfg.jwt_secret) die("DATABASE_URL and JWT_SECRET are required");
    cfg.host = getenv("HOST") && *getenv("HOST") ? getenv("HOST") : "127.0.0.1";
    cfg.port = getenv("PORT") && *getenv("PORT") ? getenv("PORT") : "3000";
    /* One loop: with E8 it held 21,000 users against 19,000 for two loops (README); one loop thread is 78% busy at
     * 37,000 req/s, so it has headroom to about 48,000 req/s. LOOPS=2 for more. */
    cfg.loops = env_int("LOOPS", 1, 1, 32);
    cfg.pg_read_conns = env_int("PG_READ_CONNS", 8, 1, 94);
    cfg.pg_write_conns = env_int("PG_WRITE_CONNS", 2, 1, 32);
    if (cfg.pg_read_conns < cfg.loops) cfg.pg_read_conns = cfg.loops;
    if (cfg.pg_write_conns < cfg.loops) cfg.pg_write_conns = cfg.loops;
    if (cfg.pg_read_conns + cfg.pg_write_conns > 95) die("at most 95 Postgres connections (PG_READ_CONNS + PG_WRITE_CONNS)");
    cfg.pg_depth = env_int("PG_DEPTH", 2, 1, MAX_DEPTH);
    cfg.read_batch = env_int("READ_BATCH", 24, 1, 1024);
    cfg.write_batch = env_int("WRITE_BATCH", 128, 1, 4096);
    cfg.uring_busy_cqe = env_int("URING_BUSY_CQE", 16, 1, 1 << 20);
    cfg.uring_min_cqe = env_int("URING_MIN_CQE", 32, 1, 4096);
    cfg.uring_wait_us = env_int("URING_WAIT_US", 250, 0, 1000000);
    cfg.bcrypt_threads = env_int("BCRYPT_THREADS", 2, 1, 64);
    const char *lanes = getenv("BCRYPT_LANES");
    cfg.bcrypt_lanes = !lanes || !*lanes || !strcmp(lanes, "auto") ? 0 : env_int("BCRYPT_LANES", 0, 1, 8);
    cfg.bcrypt_wait_ms = env_int("BCRYPT_WAIT_MS", 250, 0, 60000);
    cfg.bcrypt_nice = env_int("BCRYPT_NICE", 5, -20, 19);
    static const char *const kinds[K_KINDS] = {"LIST_SHAPE_GLOBAL", "LIST_SHAPE_TAG", "LIST_SHAPE_AUTHOR", "LIST_SHAPE_FAVORITED", "LIST_SHAPE_FEED"};
    static const char *const shapes[] = {"v1", "v2", "v1d", "v2c", "v2t"}; /* SHAPE_* order */
    for (int k = 0; k < K_KINDS; k++) {
        const char *v = getenv(kinds[k]);
        cfg.list_shape[k] = SHAPE_V2T; /* E1 + E8: v2 lists with tag names by key probes (README) */
        if (!v || !*v) continue;
        int s = -1;
        for (int i = 0; i < (int)ARRAY_LEN(shapes); i++) if (!strcmp(v, shapes[i])) s = i;
        if (s < 0) die("%s must be v1, v2, v1d, v2c or v2t", kinds[k]);
        cfg.list_shape[k] = s;
    }
    cfg.e2_batch = env_int("E2_BATCH", 0, 0, 1);
    cfg.e2_min = env_int("E2_MIN", 2, 2, 1024);
    cfg.gzip_min = env_int("GZIP_MIN", 0, 0, 1 << 30);
    cfg.gzip_level = env_int("GZIP_LEVEL", 1, 1, 12);
#ifndef WITH_GZIP
    if (cfg.gzip_min) { log_msg("GZIP_MIN ignored: built without gzip (build with GZIP=1)"); cfg.gzip_min = 0; }
#endif
    const char *snap = getenv("READ_SNAPSHOT");
    cfg.read_snapshot = snap && !strcmp(snap, "batch");
    if (snap && *snap && strcmp(snap, "batch") && strcmp(snap, "statement")) die("READ_SNAPSHOT must be statement or batch");
    cfg.stats = env_int("STATS", 0, 0, 1);
    cfg.max_header = 16 * 1024;
    cfg.max_body = (uint32_t)env_int("MAX_BODY", 1 << 20, 1024, 1 << 28);
}

static void raise_nofile(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

int main(int argc, char **argv) {
    sha256_setup();
    if (argc > 1 && !strcmp(argv[1], "--bcrypt-selftest")) {
        bool ok = bcrypt_selftest();
        printf("bcrypt self-test: %s; best interleave: %d lanes\n", ok ? "ok" : "FAILED", bcrypt_calibrate());
        return ok ? 0 : 1;
    }
    signal(SIGPIPE, SIG_IGN);
    load_config();
    raise_nofile();
    jwt_setup(cfg.jwt_secret);
    if (!bcrypt_selftest()) die("bcrypt self-test failed: refusing to start");
    int lanes = cfg.bcrypt_lanes ? cfg.bcrypt_lanes : bcrypt_calibrate();
    if (!pg_parse_url(cfg.database_url, &pgconf)) die("DATABASE_URL must be postgres://user:password@host:port/dbname");

    /* block the signals we handle before starting threads, so only this thread receives them */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    nloops = cfg.loops;
    loops = calloc((size_t)nloops, sizeof *loops);
    if (!loops) die("out of memory");
    for (int i = 0; i < nloops; i++) loops[i] = loop_create(i);
    for (int i = 0; i < nloops; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, loop_run, loops[i])) die("pthread_create loop");
        pthread_detach(t);
    }
    for (int i = 0; i < nloops; i++) while (!__atomic_load_n(&loops[i]->ready, __ATOMIC_ACQUIRE)) usleep(1000);
    bcrypt_start(cfg.bcrypt_threads, lanes, cfg.bcrypt_wait_ms, cfg.bcrypt_nice);
    if (cfg.stats) stats_start();
    log_msg("conduit (C + io_uring) on %s:%s: %d loops, %d+%d postgres connections, depth %d, read batch %d, "
            "write batch %d, bcrypt %d threads x %d lanes, SHA-NI %s", cfg.host, cfg.port, cfg.loops, cfg.pg_read_conns,
            cfg.pg_write_conns, cfg.pg_depth, cfg.read_batch, cfg.write_batch, cfg.bcrypt_threads, lanes, sha256_has_ni ? "yes" : "no");
    int sig;
    sigwait(&sigs, &sig);
    log_msg("signal %d: exiting", sig);
    return 0;
}
