/* api_profiles.c: profiles and follows. */
#include "conduit.h"

void render_profile(obuf_t *b, str_t username, str_t bio, str_t image, bool following) {
    OB_LIT(b, "{\"username\":");
    ob_json_str(b, username.p, username.n);
    OB_LIT(b, ",\"bio\":");
    ob_json_nstr(b, bio.p, bio.p ? (int32_t)bio.n : -1);
    OB_LIT(b, ",\"image\":");
    ob_json_nstr(b, image.p, image.p ? (int32_t)image.n : -1);
    OB_LIT(b, ",\"following\":");
    ob_bool(b, following);
    ob_ch(b, '}');
}

static void profile_out(struct req *r, struct pgres *res, bool following) {
    resp_begin(r, res->bytes + 256);
    OB_LIT(&r->out, "{\"profile\":");
    render_profile(&r->out, pv_str(res, 0, 0), pv_str(res, 0, 1), pv_str(res, 0, 2), following);
    ob_ch(&r->out, '}');
    resp_finish(r, 200);
}

static void profile_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "profile not found"); return; }
    profile_out(r, res, pv_bool(res, 0, 3));
}

void api_profile_get(struct req *r, str_t username) {
    prm_i64(r, 0, r->viewer);
    prm_str(r, 1, username);
    db_read(r, S_P_GET, 2, profile_done);
}

static void follow_done(struct req *r, struct pgres *res) {
    if (!res->nrows) { resp_error(r, 404, "profile not found"); return; }
    profile_out(r, res, r->route == R_FOLLOW);
}

void api_follow(struct req *r, str_t username, bool follow) {
    prm_i64(r, 0, r->viewer);
    prm_str(r, 1, username);
    db_write(r, follow ? S_P_FOLLOW : S_P_UNFOLLOW, 2, follow_done, false);
}
