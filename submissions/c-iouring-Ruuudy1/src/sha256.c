/* sha256.c: SHA-256 (SHA-NI when available, portable otherwise), HMAC-SHA256 with precomputed midstates, and
 * PBKDF2-HMAC-SHA256 for SCRAM. */
#include "conduit.h"

#include <cpuid.h>
#include <immintrin.h>

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static const uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

bool sha256_has_ni;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void blocks_portable(uint32_t h[8], const uint8_t *p, size_t nblocks) {
    while (nblocks--) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = rd32(p + 4 * i);
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
            uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        p += 64;
    }
}

/* Intel SHA extensions: 4 rounds per group, message schedule with sha256msg1/msg2. */
__attribute__((target("sha,sse4.1,ssse3")))
static void blocks_ni(uint32_t h[8], const uint8_t *p, size_t nblocks) {
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    __m128i tmp = _mm_loadu_si128((const __m128i *)&h[0]);
    __m128i s1 = _mm_loadu_si128((const __m128i *)&h[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xB1);            /* CDAB */
    s1 = _mm_shuffle_epi32(s1, 0x1B);              /* EFGH */
    __m128i s0 = _mm_alignr_epi8(tmp, s1, 8);      /* ABEF */
    s1 = _mm_blend_epi16(s1, tmp, 0xF0);           /* CDGH */
    while (nblocks--) {
        __m128i abef = s0, cdgh = s1, m[4], msg;
#pragma GCC unroll 16
        for (int g = 0; g < 16; g++) {
            if (g < 4) m[g] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * g)), MASK);
            msg = _mm_add_epi32(m[g & 3], _mm_loadu_si128((const __m128i *)&K256[4 * g]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, msg);
            if (g >= 3 && g <= 14) {
                __m128i t = _mm_alignr_epi8(m[g & 3], m[(g + 3) & 3], 4);
                m[(g + 1) & 3] = _mm_sha256msg2_epu32(_mm_add_epi32(m[(g + 1) & 3], t), m[g & 3]);
            }
            msg = _mm_shuffle_epi32(msg, 0x0E);
            s0 = _mm_sha256rnds2_epu32(s0, s1, msg);
            if (g >= 1 && g <= 12) m[(g + 3) & 3] = _mm_sha256msg1_epu32(m[(g + 3) & 3], m[g & 3]);
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
        p += 64;
    }
    tmp = _mm_shuffle_epi32(s0, 0x1B);             /* FEBA */
    s1 = _mm_shuffle_epi32(s1, 0xB1);              /* DCHG */
    s0 = _mm_blend_epi16(tmp, s1, 0xF0);           /* DCBA */
    s1 = _mm_alignr_epi8(s1, tmp, 8);              /* ABEF -> HGFE */
    _mm_storeu_si128((__m128i *)&h[0], s0);
    _mm_storeu_si128((__m128i *)&h[4], s1);
}

void sha256_setup(void) {
    unsigned a, b, c, d;
    sha256_has_ni = false;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 29))) {
        unsigned a1, b1, c1, d1;
        if (__get_cpuid(1, &a1, &b1, &c1, &d1) && (c1 & (1u << 19)) && (c1 & (1u << 9))) sha256_has_ni = true;
    }
    if (getenv("SHA256_PORTABLE")) sha256_has_ni = false;
}

void sha256_blocks(uint32_t h[8], const uint8_t *data, size_t nblocks) {
    if (sha256_has_ni) blocks_ni(h, data, nblocks);
    else blocks_portable(h, data, nblocks);
}

/* Continues from state h after `done` bytes (a multiple of 64), hashes msg and finishes. */
static void sha256_tail(uint8_t out[32], uint32_t h[8], uint64_t done, const uint8_t *msg, size_t n) {
    size_t full = n / 64;
    if (full) sha256_blocks(h, msg, full);
    uint8_t blk[128];
    size_t rem = n - full * 64;
    memcpy(blk, msg + full * 64, rem);
    blk[rem] = 0x80;
    size_t tot = rem + 1 + 8 <= 64 ? 64 : 128;
    memset(blk + rem + 1, 0, tot - rem - 1 - 8);
    wr64(blk + tot - 8, (done + n) * 8);
    sha256_blocks(h, blk, tot / 64);
    for (int i = 0; i < 8; i++) wr32(out + 4 * i, h[i]);
}

void sha256(uint8_t out[32], const void *data, size_t n) {
    uint32_t h[8];
    memcpy(h, IV, sizeof h);
    sha256_tail(out, h, 0, data, n);
}

void hmac_init(hmac_key *k, const void *key, size_t n) {
    uint8_t kb[64] = {0}, pad[64];
    if (n > 64) sha256(kb, key, n);
    else memcpy(kb, key, n);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    memcpy(k->inner, IV, sizeof IV);
    sha256_blocks(k->inner, pad, 1);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
    memcpy(k->outer, IV, sizeof IV);
    sha256_blocks(k->outer, pad, 1);
}

void hmac_sha256(uint8_t out[32], const hmac_key *k, const void *msg, size_t n) {
    uint32_t h[8];
    uint8_t inner[32];
    memcpy(h, k->inner, sizeof h);
    sha256_tail(inner, h, 64, msg, n);
    memcpy(h, k->outer, sizeof h);
    sha256_tail(out, h, 64, inner, 32);
}

void pbkdf2_sha256(uint8_t out[32], const void *pw, size_t pwn, const void *salt, size_t saltn, uint32_t iters) {
    hmac_key k;
    hmac_init(&k, pw, pwn);
    uint8_t *s1 = malloc(saltn + 4);
    if (!s1) die("out of memory");
    memcpy(s1, salt, saltn);
    wr32(s1 + saltn, 1);
    uint8_t u[32], t[32];
    hmac_sha256(u, &k, s1, saltn + 4);
    free(s1);
    memcpy(t, u, 32);
    /* U_i = HMAC(U_{i-1}): a 32-byte message is one padded block for the inner and one for the outer hash. */
    uint8_t blk[64] = {0};
    blk[32] = 0x80;
    wr64(blk + 56, (64 + 32) * 8);
    for (uint32_t i = 1; i < iters; i++) {
        uint32_t h[8];
        memcpy(blk, u, 32);
        memcpy(h, k.inner, sizeof h);
        sha256_blocks(h, blk, 1);
        for (int j = 0; j < 8; j++) wr32(blk + 4 * j, h[j]);
        memcpy(h, k.outer, sizeof h);
        sha256_blocks(h, blk, 1);
        for (int j = 0; j < 8; j++) wr32(u + 4 * j, h[j]);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out, t, 32);
}
