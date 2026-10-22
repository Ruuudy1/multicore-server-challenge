/* libFuzzer target for the request parsers: HTTP request head, chunked bodies, query strings, the JSON
 * wrapper parser, JWT verification and slugify. Run with tests/fuzz.sh. */
#include "../src/conduit.h"

struct config cfg;
struct loop **loops;
int nloops;
void loop_post(struct loop *l, void *p) { (void)l; (void)p; }

static struct req *R;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    sha256_setup();
    jwt_setup("multicore-server-challenge");
    R = calloc(1, sizeof *R);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1 << 20) return 0;
    const char *s = (const char *)data;
    uint32_t n = (uint32_t)size;
    struct http_req h;
    int rc = http_parse(s, n, &h, 16384);
    if (rc == 1) {
        if (h.target_n) {
            const char *q = memchr(h.target, '?', h.target_n);
            if (q) {
                str_t v;
                query_get(R, q + 1, (uint32_t)(h.target + h.target_n - q - 1), "limit", &v);
                query_get(R, q + 1, (uint32_t)(h.target + h.target_n - q - 1), "tag", &v);
            }
        }
        if (h.auth_present && h.auth_n > 6) jwt_verify(h.auth + 6, h.auth_n - 6, 1790000000000LL);
        char *dst = malloc(n + 1);
        uint32_t dn, used;
        http_chunked(s + h.header_n, n - h.header_n, dst, &dn, &used, 1 << 20);
        free(dst);
    }
    struct jfield f[4] = {{.name = "title"}, {.name = "description"}, {.name = "body"}, {.name = "tagList"}};
    if (json_wrapper(R, s, n, "article", f, 4) && f[3].type == JF_ARRAY) {
        str_t *items;
        uint32_t count;
        json_string_array(R, f[3].raw, f[3].raw_n, &items, &count);
    }
    char *slug = malloc(n + 1);
    slugify(slug, s, n);
    free(slug);
    jwt_verify(s, n, 1790000000000LL);
    arena_reset(R);
    return 0;
}
