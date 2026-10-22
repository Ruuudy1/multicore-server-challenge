/* Unit tests for the pure modules: SHA-256/HMAC/PBKDF2, base64url, JSON, JWT, bcrypt, timestamps, slugify,
 * percent-decoding and the HTTP parser. Run with tests/run.sh. */
#include "../src/conduit.h"

#include <time.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct config cfg;
struct loop **loops;
int nloops;
void loop_post(struct loop *l, void *p) { (void)l; (void)p; }

static void hex(char *dst, const uint8_t *b, int n) { for (int i = 0; i < n; i++) sprintf(dst + 2 * i, "%02x", b[i]); }

static void test_sha(void) {
    struct { const char *msg, *want; } v[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (int pass = 0; pass < 2; pass++) {
        bool ni = sha256_has_ni;
        if (pass == 1) sha256_has_ni = false;
        for (size_t i = 0; i < ARRAY_LEN(v); i++) {
            uint8_t out[32];
            char h[65];
            sha256(out, v[i].msg, strlen(v[i].msg));
            hex(h, out, 32);
            CHECK(strcmp(h, v[i].want) == 0, "sha256(%s) ni=%d = %s", v[i].msg, sha256_has_ni, h);
        }
        sha256_has_ni = ni;
    }
    /* SHA-NI and portable agree on every length up to 300 */
    if (sha256_has_ni) {
        uint8_t buf[300];
        for (int i = 0; i < 300; i++) buf[i] = (uint8_t)(i * 37 + 11);
        for (int n = 0; n <= 300; n++) {
            uint8_t a[32], b[32];
            sha256(a, buf, (size_t)n);
            sha256_has_ni = false;
            sha256(b, buf, (size_t)n);
            sha256_has_ni = true;
            CHECK(memcmp(a, b, 32) == 0, "sha256 ni/portable differ at length %d", n);
        }
    }
    /* RFC 4231 */
    struct { uint8_t key[131]; int kn; const char *data, *want; } h[3] = {
        {{0}, 20, "Hi There", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {"Jefe", 4, "what do ya want for nothing?", "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {{0}, 131, "Test Using Larger Than Block-Size Key - Hash Key First", "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
    };
    memset(h[0].key, 0x0b, 20);
    memset(h[2].key, 0xaa, 131);
    for (int i = 0; i < 3; i++) {
        hmac_key k;
        uint8_t out[32];
        char x[65];
        hmac_init(&k, h[i].key, (size_t)h[i].kn);
        hmac_sha256(out, &k, h[i].data, strlen(h[i].data));
        hex(x, out, 32);
        CHECK(strcmp(x, h[i].want) == 0, "hmac case %d = %s", i, x);
    }
    /* RFC 7914 section 11 (first 32 bytes of each) */
    uint8_t dk[32];
    char x[65];
    pbkdf2_sha256(dk, "passwd", 6, "salt", 4, 1);
    hex(x, dk, 32);
    CHECK(strcmp(x, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc") == 0, "pbkdf2 c=1 = %s", x);
    pbkdf2_sha256(dk, "Password", 8, "NaCl", 4, 80000);
    hex(x, dk, 32);
    CHECK(strcmp(x, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56") == 0, "pbkdf2 c=80000 = %s", x);
}

static void test_b64(void) {
    const char *s[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *w[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        char e[16];
        uint32_t n = b64url_encode(e, (const uint8_t *)s[i], (uint32_t)strlen(s[i]));
        e[n] = 0;
        CHECK(strcmp(e, w[i]) == 0, "b64 encode %s = %s", s[i], e);
        uint8_t d[16];
        uint32_t dn;
        CHECK(b64url_decode(d, &dn, w[i], (uint32_t)strlen(w[i])) && dn == strlen(s[i]) && memcmp(d, s[i], dn) == 0, "b64 decode %s", w[i]);
    }
    uint8_t d[16];
    uint32_t dn;
    CHECK(!b64url_decode(d, &dn, "Zm9v=", 5), "padding rejected");
    CHECK(!b64url_decode(d, &dn, "Z", 1), "length 1 mod 4 rejected");
    CHECK(!b64url_decode(d, &dn, "Zm+v", 4), "standard alphabet rejected");
    CHECK(b64url_decode(d, &dn, "Zh", 2) && dn == 1 && d[0] == 'f', "trailing bits ignored (non-strict)");
}

static struct req *new_req(void) {
    struct req *r = calloc(1, sizeof *r);
    return r;
}

static void test_json(void) {
    struct req *r = new_req();
    struct jfield f[3] = {{.name = "username"}, {.name = "email"}, {.name = "tagList"}};
    const char *basic = "{\"user\":{\"username\":\"a\",\"email\":null}}";
    CHECK(json_wrapper(r, basic, (uint32_t)strlen(basic), "user", f, 3), "basic");
    CHECK(f[0].type == JF_STRING && f[0].s.n == 1 && f[0].s.p[0] == 'a', "username");
    CHECK(f[1].type == JF_NULL, "email null");
    CHECK(f[2].type == JF_ABSENT, "tagList absent");
    const char *bad[] = {"", "   ", "null", "[]", "{\"user\": {\"username\": \"u_x\",", "{\"user\":\"x\"}", "{\"user\":null}",
                         "{\"u\":{}}", "{\"user\":{}} x", "{\"user\":{\"a\":01}}", "{\"user\":{\"a\":\"\x01\"}}", "{\"user\":{\"a\":tru}}",
                         "{\"user\":{\"a\":\"\\x\"}}", "{\"user\":{},}", "{\"User\":{}}"};
    for (size_t i = 0; i < ARRAY_LEN(bad); i++)
        CHECK(!json_wrapper(r, bad[i], (uint32_t)strlen(bad[i]), "user", f, 3), "invalid body %zu accepted: %s", i, bad[i]);
    const char *good[] = {"{\"user\":{}}", " { \"user\" : { } } \n", "{\"x\":[1,2,{\"y\":-0.5e+3}],\"user\":{\"username\":5}}",
                          "{\"\\u0075ser\":{}}", "{\"user\":1,\"user\":{\"email\":\"last\"}}"};
    for (size_t i = 0; i < ARRAY_LEN(good); i++)
        CHECK(json_wrapper(r, good[i], (uint32_t)strlen(good[i]), "user", f, 3), "valid body %zu rejected: %s", i, good[i]);
    CHECK(json_wrapper(r, good[2], (uint32_t)strlen(good[2]), "user", f, 3) && f[0].type == JF_OTHER, "number is JF_OTHER");
    CHECK(json_wrapper(r, good[4], (uint32_t)strlen(good[4]), "user", f, 3) && f[1].type == JF_STRING && str_is(f[1].s, "last"), "last wrapper wins");
    const char *dup = "{\"user\":{\"email\":\"first\",\"email\":\"second\"}}";
    CHECK(json_wrapper(r, dup, (uint32_t)strlen(dup), "user", f, 3) && str_is(f[1].s, "second"), "last field wins");
    /* escapes, surrogates and invalid UTF-8 (Go replaces with U+FFFD) */
    const char *esc = "{\"user\":{\"username\":\"a\\\"b\\\\c\\/d\\n\\u00e9\\ud83d\\ude00\\ud800x\xff\"}}";
    CHECK(json_wrapper(r, esc, (uint32_t)strlen(esc), "user", f, 3) && f[0].type == JF_STRING, "escapes parse");
    const char want[] = "a\"b\\c/d\n\xc3\xa9\xf0\x9f\x98\x80\xef\xbf\xbdx\xef\xbf\xbd";
    CHECK(f[0].s.n == sizeof want - 1 && memcmp(f[0].s.p, want, sizeof want - 1) == 0, "escape decoding (%u bytes)", f[0].s.n);
    const char *tags = "{\"article\":{\"tagList\":[\"a\", \"b\" ,\"c\"]}}";
    struct jfield t[1] = {{.name = "tagList"}};
    CHECK(json_wrapper(r, tags, (uint32_t)strlen(tags), "article", t, 1) && t[0].type == JF_ARRAY, "tagList array");
    str_t *items;
    uint32_t count;
    CHECK(json_string_array(r, t[0].raw, t[0].raw_n, &items, &count) && count == 3 && str_is(items[2], "c"), "tagList items");
    const char *badtags[] = {"[\"ok\", \"\"]", "[1]", "[null]", "[[\"x\"]]"};
    for (size_t i = 0; i < ARRAY_LEN(badtags); i++)
        CHECK(!json_string_array(r, badtags[i], (uint32_t)strlen(badtags[i]), &items, &count), "bad tagList %zu", i);
    CHECK(json_string_array(r, "[]", 2, &items, &count) && count == 0, "empty tagList");
    /* deep nesting is limited like Go's 10000 */
    char *deep = malloc(30000);
    int k = 0;
    k += sprintf(deep, "{\"user\":{},\"x\":");
    for (int i = 0; i < 10001; i++) deep[k++] = '[';
    for (int i = 0; i < 10001; i++) deep[k++] = ']';
    deep[k++] = '}';
    CHECK(!json_wrapper(r, deep, (uint32_t)k, "user", f, 3), "nesting over 10000 rejected");
    free(deep);
    arena_reset(r);
    free(r);
}

static void test_jwt(void) {
    jwt_setup("multicore-server-challenge");
    char tok[JWT_MAX];
    int64_t now = 1790000000;
    uint32_t n = jwt_issue(tok, 42, now);
    tok[n] = 0;
    CHECK(jwt_verify(tok, n, now * 1000) == 42, "issued token verifies: %s", tok);
    CHECK(jwt_verify(tok, n, (now + 30 * 24 * 3600) * 1000) == 0, "expires at exp");
    CHECK(jwt_verify(tok, n, (now + 30 * 24 * 3600 - 1) * 1000 + 999) == 42, "valid just before exp");
    char bad[JWT_MAX];
    memcpy(bad, tok, n);
    bad[n - 1] = bad[n - 1] == 'A' ? 'B' : 'A';
    CHECK(jwt_verify(bad, n, now * 1000) == 0, "tampered signature rejected");
    /* golang-jwt MapClaims order (keys sorted): {"exp":..,"iat":..,"sub":".."} */
    const char *claims[] = {"{\"exp\":1800000000,\"iat\":1790000000,\"sub\":\"7\"}", "{\"sub\":\"7\",\"exp\":1800000000.9}",
                            "{\"SUB\":\"7\",\"EXP\":\"1800000000\"}", "{\"sub\":\"7\",\"exp\":1800000000,\"nbf\":1790000000,\"aud\":[\"a\"]}"};
    const char *rejected[] = {"{\"sub\":7,\"exp\":1800000000}", "{\"sub\":\"abc\",\"exp\":1800000000}", "{\"sub\":\"7\",\"iat\":1}",
                              "{\"sub\":\"7\",\"exp\":1700000000}", "{\"sub\":\"7\",\"exp\":1800000000,\"nbf\":1795000000}",
                              "{\"sub\":\"0\",\"exp\":1800000000}", "{\"sub\":\"7\",\"exp\":\"soon\"}", "{\"sub\":\"7\",\"exp\":1800000000,\"aud\":[1]}",
                              "{\"sub\":\"-7\",\"exp\":1800000000}", "[]"};
    hmac_key k;
    hmac_init(&k, "multicore-server-challenge", 26);
    for (int pass = 0; pass < 2; pass++) {
        const char **list = pass ? rejected : claims;
        size_t cnt = pass ? ARRAY_LEN(rejected) : ARRAY_LEN(claims);
        for (size_t i = 0; i < cnt; i++) {
            char t[512];
            uint32_t o = (uint32_t)sprintf(t, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.");
            o += b64url_encode(t + o, (const uint8_t *)list[i], (uint32_t)strlen(list[i]));
            uint8_t mac[32];
            hmac_sha256(mac, &k, t, o);
            t[o++] = '.';
            o += b64url_encode(t + o, mac, 32);
            int64_t uid = jwt_verify(t, o, (int64_t)1791000000 * 1000);
            if (pass) CHECK(uid == 0, "claims %s accepted", list[i]);
            else CHECK(uid == 7, "claims %s rejected", list[i]);
        }
    }
    /* header variants: alg none, other key order, extra fields */
    const char *hdrs[] = {"{\"typ\":\"JWT\",\"alg\":\"HS256\"}", "{\"alg\":\"HS256\",\"kid\":\"x\"}"};
    const char *badh[] = {"{\"alg\":\"none\",\"typ\":\"JWT\"}", "{\"alg\":\"HS384\"}", "{\"ALG\":\"HS256\"}", "{\"alg\":\"HS256\"} x", "{}"};
    for (int pass = 0; pass < 2; pass++) {
        const char **list = pass ? badh : hdrs;
        size_t cnt = pass ? ARRAY_LEN(badh) : ARRAY_LEN(hdrs);
        for (size_t i = 0; i < cnt; i++) {
            char t[512];
            uint32_t o = b64url_encode(t, (const uint8_t *)list[i], (uint32_t)strlen(list[i]));
            t[o++] = '.';
            const char *c = "{\"sub\":\"9\",\"iat\":1,\"exp\":1800000000}";
            o += b64url_encode(t + o, (const uint8_t *)c, (uint32_t)strlen(c));
            uint8_t mac[32];
            hmac_sha256(mac, &k, t, o);
            t[o++] = '.';
            o += b64url_encode(t + o, mac, 32);
            int64_t uid = jwt_verify(t, o, (int64_t)1791000000 * 1000);
            if (pass) CHECK(uid == 0, "header %s accepted", list[i]);
            else CHECK(uid == 9, "header %s rejected", list[i]);
        }
    }
    CHECK(jwt_verify("not.a.jwt", 9, now * 1000) == 0, "garbage");
    CHECK(jwt_verify("a.b", 3, now * 1000) == 0, "two parts");
    CHECK(jwt_verify("a.b.c.d", 7, now * 1000) == 0, "four parts");
}

static void test_bcrypt(void) {
    double t0 = (double)clock_ms();
    CHECK(bcrypt_selftest(), "bcrypt self-test");
    printf("bcrypt self-test took %.0f ms\n", (double)clock_ms() - t0);
    CHECK(bcrypt_verify_sync("password123", 11, "$2a$10$ZSfEoPvlcsj2XfvsSeK25em4NZ9Jm7ntGqXuLgQfHEeZPYB8899wq"), "seed hash");
    CHECK(!bcrypt_verify_sync("password124", 11, "$2a$10$ZSfEoPvlcsj2XfvsSeK25em4NZ9Jm7ntGqXuLgQfHEeZPYB8899wq"), "wrong password");
    CHECK(bcrypt_verify_sync("password123", 11, "$2b$10$ZSfEoPvlcsj2XfvsSeK25em4NZ9Jm7ntGqXuLgQfHEeZPYB8899wq"), "$2b$ prefix");
    CHECK(!bcrypt_verify_sync("password123", 11, "$2a$10$ZSfEoPvlcsj2XfvsSeK25em4NZ9Jm7ntGqXuLgQfHEeZPYB8899w"), "short hash");
    CHECK(!bcrypt_verify_sync("x", 1, "plaintext"), "garbage hash");
    uint8_t salt[16] = {1, 2, 3};
    char h[64];
    bcrypt_hash_sync(h, "hello", 5, 4, salt);
    CHECK(strlen(h) == 60 && bcrypt_verify_sync("hello", 5, h), "hash then verify: %s", h);
    int lanes = bcrypt_calibrate();
    printf("bcrypt calibration picked %d lanes\n", lanes);
}

static void test_misc(void) {
    obuf_t b;
    ob_init(&b, 64);
    struct { int64_t us; const char *want; } ts[] = {
        {0, "\"2000-01-01T00:00:00.000Z\""}, {999, "\"2000-01-01T00:00:00.000Z\""}, {1999, "\"2000-01-01T00:00:00.001Z\""},
        {-1, "\"1999-12-31T23:59:59.999Z\""}, {INT64_C(841536000000000), "\"2026-09-01T00:00:00.000Z\""},
        {INT64_C(762739200123456), "\"2024-03-03T00:00:00.123Z\""}, {INT64_C(951782400000000), "\"2030-02-28T00:00:00.000Z\""},
        {INT64_C(951868800000000), "\"2030-03-01T00:00:00.000Z\""}, {INT64_C(825681599999999), "\"2026-03-01T11:59:59.999Z\""},
    };
    for (size_t i = 0; i < ARRAY_LEN(ts); i++) {
        b.len = 0;
        ob_ts(&b, ts[i].us);
        CHECK(b.len == strlen(ts[i].want) && memcmp(b.p, ts[i].want, b.len) == 0, "ts %lld = %.*s", (long long)ts[i].us, (int)b.len, b.p);
    }
    b.len = 0;
    static const char raw[] = "a\"b\\c\n\x01\x1f\x7f\xc3\xa9 0123456789abcdef\"";
    ob_json_str(&b, raw, sizeof raw - 1);
    const char want[] = "\"a\\\"b\\\\c\\n\\u0001\\u001f\x7f\xc3\xa9 0123456789abcdef\\\"\"";
    CHECK(b.len == sizeof want - 1 && memcmp(b.p, want, b.len) == 0, "json escape: %.*s", (int)b.len, b.p);
    /* long strings exercise the SIMD path, with escapes at every alignment */
    for (int pos = 0; pos < 40; pos++) {
        char s[64];
        memset(s, 'x', 48);
        s[pos] = '"';
        b.len = 0;
        ob_json_str(&b, s, 48);
        CHECK(b.len == 51 && b.p[pos + 1] == '\\' && b.p[pos + 2] == '"', "escape at %d", pos);
    }
    ob_free(&b);
    struct { const char *in, *want; } sl[] = {
        {"Hello, World!", "hello-world"}, {"  --Leading and trailing--  ", "leading-and-trailing"},
        {"\xc3\x85ngstr\xc3\xb6m \xc3\xbc" "ber na\xc3\xafve", "ngstr-m-ber-na-ve"}, {"123 go", "123-go"}, {"!!!", ""}, {"", ""},
        {"A_B-C.D", "a-b-c-d"}, {"tab\tand\nnewline", "tab-and-newline"}, {"  Conformance -- Renamed Title!  ", "conformance-renamed-title"},
        {"Conformance: Hello, World! 2026", "conformance-hello-world-2026"},
    };
    for (size_t i = 0; i < ARRAY_LEN(sl); i++) {
        char d[128];
        uint32_t n = slugify(d, sl[i].in, (uint32_t)strlen(sl[i].in));
        CHECK(n == strlen(sl[i].want) && memcmp(d, sl[i].want, n) == 0, "slugify(%s) = %.*s", sl[i].in, (int)n, d);
    }
    int64_t v;
    CHECK(parse_u63("9223372036854775807", 19, &v) && v == INT64_MAX, "max int64");
    CHECK(!parse_u63("9223372036854775808", 19, &v), "overflow");
    CHECK(!parse_u63("99999999999999999999", 20, &v), "overflow 20 digits");
    CHECK(parse_u63("005", 3, &v) && v == 5, "leading zeros");
    CHECK(!parse_u63("", 0, &v) && !parse_u63("+5", 2, &v) && !parse_u63(" 5", 2, &v) && !parse_u63("1.5", 3, &v), "non-digits");
    char d[32];
    uint32_t dn;
    CHECK(pct_decode(d, &dn, "a%2Bb+c%20", 10, true) && dn == 6 && memcmp(d, "a+b c ", 6) == 0, "query decode");
    CHECK(pct_decode(d, &dn, "a+b", 3, false) && dn == 3 && memcmp(d, "a+b", 3) == 0, "path keeps +");
    CHECK(!pct_decode(d, &dn, "a%2", 3, true) && !pct_decode(d, &dn, "%zz", 3, true), "bad escapes");
}

static void test_http(void) {
    struct http_req h;
    const char *g = "GET /api/articles?limit=10 HTTP/1.1\r\nHost: x\r\nAuthorization:  Token abc  \r\n\r\n";
    CHECK(http_parse(g, (uint32_t)strlen(g), &h, 16384) == 1, "parse GET");
    CHECK(h.method == M_GET && h.minor == 1 && h.keepalive && h.content_length == -1, "GET fields");
    CHECK(h.target_n == 22 && memcmp(h.target, "/api/articles?limit=10", 22) == 0, "target");
    CHECK(h.auth_present && h.auth_n == 9 && memcmp(h.auth, "Token abc", 9) == 0, "auth trimmed: '%.*s'", (int)h.auth_n, h.auth);
    CHECK(h.header_n == strlen(g), "header length");
    CHECK(http_parse(g, (uint32_t)strlen(g) - 1, &h, 16384) == 0, "incomplete");
    const char *p = "POST /api/users HTTP/1.1\r\ncontent-length: 12\r\nCONNECTION: close\r\n\r\n";
    CHECK(http_parse(p, (uint32_t)strlen(p), &h, 16384) == 1 && h.method == M_POST && h.content_length == 12 && !h.keepalive, "POST");
    const char *c = "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    CHECK(http_parse(c, (uint32_t)strlen(c), &h, 16384) == 1 && h.chunked, "chunked");
    const char *ten = "GET / HTTP/1.0\r\n\r\n";
    CHECK(http_parse(ten, (uint32_t)strlen(ten), &h, 16384) == 1 && h.minor == 0 && !h.keepalive, "HTTP/1.0");
    const char *tenk = "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n";
    CHECK(http_parse(tenk, (uint32_t)strlen(tenk), &h, 16384) == 1 && h.keepalive, "HTTP/1.0 keep-alive");
    const char *bad[] = {"GET\r\n\r\n", "GET / HTTP/2.0\r\n\r\n", "GET / FOO\r\n\r\n", "GET /  HTTP/1.1\r\n\r\n",
                         "POST / HTTP/1.1\r\nContent-Length: x\r\n\r\n", "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"};
    for (size_t i = 0; i < ARRAY_LEN(bad); i++) CHECK(http_parse(bad[i], (uint32_t)strlen(bad[i]), &h, 16384) < 0, "bad request %zu accepted", i);
    char big[20000];
    memset(big, 'a', sizeof big);
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    CHECK(http_parse(big, sizeof big, &h, 16384) == -2, "headers too large");
    char out[64];
    uint32_t on, used;
    const char *ch = "4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nTrailer: x\r\n\r\nNEXT";
    CHECK(http_chunked(ch, (uint32_t)strlen(ch), out, &on, &used, 1000) == 1 && on == 9 && memcmp(out, "Wikipedia", 9) == 0 &&
          used == strlen(ch) - 4, "chunked decode (%u, %u)", on, used);
    CHECK(http_chunked(ch, 10, out, &on, &used, 1000) == 0, "chunked incomplete");
    CHECK(http_chunked("zz\r\n", 4, out, &on, &used, 1000) == -1, "chunked malformed");
}

int main(void) {
    sha256_setup();
    printf("SHA-NI: %s\n", sha256_has_ni ? "yes" : "no");
    test_sha();
    test_b64();
    test_json();
    test_jwt();
    test_misc();
    test_http();
    test_bcrypt();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
