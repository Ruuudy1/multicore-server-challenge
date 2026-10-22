/* stats.c: STATS=1 prints one line per loop per second (requests, completions per wake-up, Postgres batch sizes,
 * queue lengths, server-side latency). The counters are plain per-loop fields; the reads here are racy on
 * purpose and only for display. */
#include "loop.h"

#include <unistd.h>

static void *stats_thread(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "stats");
    struct lstats *prev = calloc((size_t)nloops, sizeof *prev);
    if (!prev) return NULL;
    for (;;) {
        sleep(1);
        for (int i = 0; i < nloops; i++) {
            struct lstats s = loops[i]->st, *p = &prev[i];
            uint64_t req = s.requests - p->requests, resp = s.responses - p->responses, waits = s.waits - p->waits;
            uint64_t rb = s.pg_batches - p->pg_batches, rj = s.pg_jobs - p->pg_jobs;
            uint64_t wb = s.pg_wbatches - p->pg_wbatches, wj = s.pg_wjobs - p->pg_wjobs;
            uint64_t lat = s.lat_sum_ms - p->lat_sum_ms;
            log_msg("loop %d: %llu req/s %llu resp/s, %u open, %.1f cqe/wake, reads %llu in %llu batches (%.1f, %llu grouped), "
                    "writes %llu in %llu groups (%.1f), fifo %u, wq %u, mean %.1f ms, max %llu ms",
                    i, (unsigned long long)req, (unsigned long long)resp, s.open_conns,
                    waits ? (double)(s.cqes - p->cqes) / (double)waits : 0.0, (unsigned long long)rj, (unsigned long long)rb,
                    rb ? (double)rj / (double)rb : 0.0, (unsigned long long)(s.grouped - p->grouped),
                    (unsigned long long)wj, (unsigned long long)wb, wb ? (double)wj / (double)wb : 0.0,
                    s.fifo_len, s.wq_len, resp ? (double)lat / (double)resp : 0.0, (unsigned long long)s.lat_max_ms);
            loops[i]->st.lat_max_ms = 0;
            *p = s;
        }
    }
    return NULL;
}

void stats_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, stats_thread, NULL) == 0) pthread_detach(t);
}
