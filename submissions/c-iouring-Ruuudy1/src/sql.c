/* sql.c: the statement catalog. The SQL text and parameter types come from sql_gen.h (generated from
 * tools/sqlcheck/statements.py); the result column types are what the renderers decode, and pg.c checks them
 * against Postgres's RowDescription at startup. */
#include "conduit.h"
#include "sql_gen.h"

#define T OID_TEXT
#define I8 OID_INT8
#define I4 OID_INT4
#define B OID_BOOL
#define TS OID_TIMESTAMPTZ
#define TA OID_TEXT_ARRAY
#define IA OID_INT8_ARRAY

#define ENTRY(id, cols...) \
    [S_##id] = {.name = #id, .sql = SQL_##id, .nparams = SQL_##id##_NPARAMS, .ptypes = SQL_##id##_PTYPES, \
                .ncols = sizeof((uint32_t[]){cols}) / sizeof(uint32_t), .ctypes = {cols}}

/* article columns as A_GET and the favorite toggles return them */
#define ARTICLE_COLS T, T, T, TS, TS, TA, I8, B, T, T, T, B
#define LIST1_COLS I8, I8, I8, T, T, T, TS, TS, TA, I8, B, T, T, T, B
#define LIST2_COLS I4, I8, I8, T, T, T, TS, TS, T, T, T, I8, IA, IA, IA, IA, IA, TA

const struct stmt STMTS[S_COUNT] = {
    ENTRY(U_GET, T, T, T, T),
    ENTRY(U_LOGIN, I8, T, T, T, T, T),
    ENTRY(P_GET, T, T, T, B),
    ENTRY(T_LIST, TA),
    ENTRY(A_GET, ARTICLE_COLS),
    ENTRY(A_GET_SLUG, I8, T, T, T, T, TS, TS, TA, I8, B, T, T, T, B),
    ENTRY(C_LIST, I4, I8, T, TS, TS, I8, T, T, T, IA),
    ENTRY(L1_GLOBAL, LIST1_COLS), ENTRY(L1_TAG, LIST1_COLS), ENTRY(L1_AUTHOR, LIST1_COLS),
    ENTRY(L1_FAVORITED, LIST1_COLS), ENTRY(L1_FEED, LIST1_COLS),
    ENTRY(L2_GLOBAL, LIST2_COLS), ENTRY(L2_TAG, LIST2_COLS), ENTRY(L2_AUTHOR, LIST2_COLS),
    ENTRY(L2_FAVORITED, LIST2_COLS), ENTRY(L2_FEED, LIST2_COLS),
    ENTRY(L1D_GLOBAL, LIST1_COLS), ENTRY(L1D_TAG, LIST1_COLS), ENTRY(L1D_AUTHOR, LIST1_COLS),
    ENTRY(L1D_FAVORITED, LIST1_COLS), ENTRY(L1D_FEED, LIST1_COLS),
    ENTRY(L2C_GLOBAL, LIST2_COLS), ENTRY(L2C_TAG, LIST2_COLS), ENTRY(L2C_AUTHOR, LIST2_COLS),
    ENTRY(L2C_FAVORITED, LIST2_COLS), ENTRY(L2C_FEED, LIST2_COLS),
    ENTRY(L2T_GLOBAL, LIST2_COLS), ENTRY(L2T_TAG, LIST2_COLS), ENTRY(L2T_AUTHOR, LIST2_COLS),
    ENTRY(L2T_FAVORITED, LIST2_COLS), ENTRY(L2T_FEED, LIST2_COLS),
    /* E2: the element's position (ord), then exactly the columns of the single-request statement */
    ENTRY(A_GET_B, I8, ARTICLE_COLS),
    ENTRY(C_LIST_B, I8, I4, I8, T, TS, TS, I8, T, T, T, IA),
    ENTRY(P_GET_B, I8, T, T, T, B),
    ENTRY(U_GET_B, I8, T, T, T, T),
    /* item 2: the v2 columns, then the request's position (ord) and its page's ids */
    ENTRY(LS_GLOBAL, LIST2_COLS, I8, IA), ENTRY(LS_TAG, LIST2_COLS, I8, IA), ENTRY(LS_AUTHOR, LIST2_COLS, I8, IA),
    ENTRY(LS_FAVORITED, LIST2_COLS, I8, IA),
    ENTRY(U_REGISTER, I8, T, T, T, T),
    ENTRY(U_UPDATE, I8, T, T, T, T),
    ENTRY(P_FOLLOW, T, T, T),
    ENTRY(P_UNFOLLOW, T, T, T),
    ENTRY(C_ADD, I8, T, TS, TS, T, T, T),
    ENTRY(C_DEL, I8),
    ENTRY(F_ADD, ARTICLE_COLS),
    ENTRY(F_DEL, ARTICLE_COLS),
    ENTRY(A_ADD, T, T, T, T, TS, TS, TA, I8, B, T, T, T, B),
    ENTRY(A_PUT, I8, T, T, T, T, TS, TS, TA, I8, B, T, T, T, B),
    ENTRY(A_DEL, I8),
};
