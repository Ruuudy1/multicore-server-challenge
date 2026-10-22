/* Times cost-10 bcrypt: one hash at a time (bcrypt_hash_sync) against the calibrated interleaved widths.
 *   gcc -O3 -march=native -pthread tests/bcrypt_bench.c src/bcrypt.c src/util.c src/sha256.c -lm && ./a.out */
#include "../src/conduit.h"

#include <time.h>

struct config cfg;
struct loop **loops;
int nloops;
void loop_post(struct loop *l, void *p) { (void)l; (void)p; }

static double cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int main(void) {
    uint8_t salt[16] = {7};
    char h[64];
    double best = 1e9;
    for (int i = 0; i < 5; i++) {
        double t0 = cpu_ms();
        bcrypt_hash_sync(h, "password123", 11, 10, salt);
        double t = cpu_ms() - t0;
        if (t < best) best = t;
    }
    printf("one hash at a time: %.1f ms of CPU per cost-10 hash\n", best);
    bcrypt_calibrate();
    return 0;
}
