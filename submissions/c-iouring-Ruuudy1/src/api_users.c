/* api_users.c: register, login, current user and user update. bcrypt runs on the worker threads; the request
 * waits (holding no event-loop time) and continues in api_bcrypt_done. */
#include "conduit.h"

static void put_user_fields(obuf_t *b, str_t email, str_t username, str_t bio, str_t image) {
    OB_LIT(b, "{\"user\":{\"email\":");
    ob_json_str(b, email.p, email.n);
    OB_LIT(b, ",\"username\":");
    ob_json_str(b, username.p, username.n);
    OB_LIT(b, ",\"bio\":");
    ob_json_nstr(b, bio.p, bio.p ? (int32_t)bio.n : -1);
    OB_LIT(b, ",\"image\":");
    ob_json_nstr(b, image.p, image.p ? (int32_t)image.n : -1);
    OB_LIT(b, ",\"token\":");
}

static void put_token(struct req *r, str_t token) {
    ob_json_str(&r->out, token.p, token.n);
    OB_LIT(&r->out, "}}");
}

static void put_fresh_token(struct req *r, int64_t uid) {
    char tok[JWT_MAX];
    uint32_t n = jwt_issue(tok, uid, loop_now_ms(r->loop) / 1000);
    put_token(r, (str_t){tok, n});
}

static void fail(struct req *r, int status, const char *msg) {
    resp_error(r, status, msg);
    req_complete(r);
}

/* ---------- GET /api/user ---------- */

static void user_get_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 401, "unauthorized"); return; }
    resp_begin(r, res->bytes + 512);
    put_user_fields(&r->out, pv_str(res, 0, 1), pv_str(res, 0, 0), pv_str(res, 0, 2), pv_str(res, 0, 3));
    put_token(r, r->token);
    resp_finish(r, 200);
}

void api_user_get(struct req *r) {
    prm_i64(r, 0, r->viewer);
    db_read(r, S_U_GET, 1, user_get_done);
}

/* ---------- POST /api/users/login ---------- */

static void login_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 401, "invalid email or password"); return; }
    str_t hash = pv_str(res, 0, 5);
    if (!hash.p || hash.n >= sizeof r->bj.hash) { resp_error(r, 401, "invalid email or password"); return; }
    memcpy(r->bj.hash, hash.p, hash.n);
    r->bj.hash[hash.n] = 0;
    r->aid = pv_i64(res, 0, 0); /* the user id, for the token */
    resp_begin(r, res->bytes + 512);
    put_user_fields(&r->out, pv_str(res, 0, 2), pv_str(res, 0, 1), pv_str(res, 0, 3), pv_str(res, 0, 4));
    r->bj.op = BJ_VERIFY;
    r->bj.loop = r->loop;
    bcrypt_submit(&r->bj); /* api_bcrypt_done finishes the response */
}

void api_login(struct req *r, const char *body, uint32_t n) {
    struct jfield f[2] = {{.name = "email"}, {.name = "password"}};
    if (!json_wrapper(r, body, n, "user", f, 2)) { fail(r, 422, "invalid body"); return; }
    if (f[0].type != JF_STRING || !f[0].s.n || f[1].type != JF_STRING || !f[1].s.n) {
        fail(r, 422, "email and password are required");
        return;
    }
    bcrypt_set_key(&r->bj, f[1].s.p, f[1].s.n);
    prm_str(r, 0, f[0].s);
    db_read(r, S_U_LOGIN, 1, login_done);
}

/* ---------- POST /api/users ---------- */

static void register_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 500, "internal error"); return; }
    resp_begin(r, res->bytes + 512);
    put_user_fields(&r->out, pv_str(res, 0, 2), pv_str(res, 0, 1), pv_str(res, 0, 3), pv_str(res, 0, 4));
    put_fresh_token(r, pv_i64(res, 0, 0));
    resp_finish(r, 201);
}

void api_register(struct req *r, const char *body, uint32_t n) {
    struct jfield f[3] = {{.name = "username"}, {.name = "email"}, {.name = "password"}};
    if (!json_wrapper(r, body, n, "user", f, 3)) { fail(r, 422, "invalid body"); return; }
    for (int i = 0; i < 3; i++) {
        if (f[i].type != JF_STRING || !f[i].s.n) { fail(r, 422, "username, email and password are required"); return; }
    }
    if (f[2].s.n > 72) { fail(r, 422, "password is too long"); return; } /* bcrypt's limit, as GenerateFromPassword */
    prm_str(r, 0, f[0].s);
    prm_str(r, 1, f[1].s);
    bcrypt_set_key(&r->bj, f[2].s.p, f[2].s.n);
    r->bj.op = BJ_HASH;
    r->bj.loop = r->loop;
    bcrypt_submit(&r->bj);
}

/* ---------- PUT /api/user ---------- */

static void update_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 401, "unauthorized"); return; }
    resp_begin(r, res->bytes + 512);
    put_user_fields(&r->out, pv_str(res, 0, 2), pv_str(res, 0, 1), pv_str(res, 0, 3), pv_str(res, 0, 4));
    put_token(r, r->token);
    resp_finish(r, 200);
}

void api_user_put(struct req *r, const char *body, uint32_t n) {
    struct jfield f[5] = {{.name = "email"}, {.name = "username"}, {.name = "password"}, {.name = "bio"}, {.name = "image"}};
    if (!json_wrapper(r, body, n, "user", f, 5)) { fail(r, 422, "invalid body"); return; }
    for (int i = 0; i < 5; i++) {
        bool bad = f[i].type == JF_OTHER || f[i].type == JF_ARRAY || (i < 3 && f[i].type == JF_STRING && !f[i].s.n);
        if (bad) { fail(r, 422, "invalid user fields"); return; }
    }
    prm_i64(r, 0, r->viewer);
    prm_str(r, 1, f[0].type == JF_STRING ? f[0].s : (str_t){NULL, 0});
    prm_str(r, 2, f[1].type == JF_STRING ? f[1].s : (str_t){NULL, 0});
    prm_null(r, 3);
    prm_str(r, 4, f[3].type == JF_STRING ? f[3].s : (str_t){NULL, 0});
    prm_str(r, 5, f[4].type == JF_STRING ? f[4].s : (str_t){NULL, 0});
    if (f[2].type == JF_STRING) {
        if (f[2].s.n > 72) { fail(r, 422, "password is too long"); return; }
        bcrypt_set_key(&r->bj, f[2].s.p, f[2].s.n);
        r->bj.op = BJ_HASH;
        r->bj.loop = r->loop;
        bcrypt_submit(&r->bj);
        return;
    }
    db_write(r, S_U_UPDATE, 6, update_done, true);
}

/* ---------- bcrypt results ---------- */

void api_bcrypt_done(struct req *r) {
    switch (r->route) {
    case R_LOGIN:
        if (!r->bj.ok) {
            resp_reset(r);
            resp_error(r, 401, "invalid email or password");
        } else {
            put_fresh_token(r, r->aid);
            resp_finish(r, 200);
        }
        req_complete(r);
        return;
    case R_REGISTER:
        prm_str(r, 2, (str_t){r->bj.hash, 60});
        db_write(r, S_U_REGISTER, 3, register_done, true);
        return;
    case R_USER_PUT:
        prm_str(r, 3, (str_t){r->bj.hash, 60});
        db_write(r, S_U_UPDATE, 6, update_done, true);
        return;
    default:
        resp_error(r, 500, "internal error");
        req_complete(r);
    }
}
