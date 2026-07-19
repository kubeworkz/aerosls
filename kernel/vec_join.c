/*
 * vec_join.c — Vector Store Roadmap Phase 5 implementation. See
 * vec_join.h for the full design writeup.
 */
#include "vec_join.h"
#include "sql_exec.h"
#include "cursor.h"
#include <stddef.h>

// ─── String / parsing helpers (no libc -- each kernel source file keeps
// its own small copies; vj_* here, matching se_*/rs_*/ri_*/pe_*/sq_*
// elsewhere. vj_parse_u64() deliberately mirrors sql_exec.c's own
// se_parse_u64() rather than exporting/reusing that file's static
// function -- matching this codebase's established per-file convention,
// not an oversight.) ────────────────────────────────────────────────────
static int vj_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int vj_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}
static void vj_app(char* buf, uint32_t* pos, uint32_t max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}
static void vj_app_u64(char* buf, uint32_t* pos, uint32_t max, uint64_t v) {
    char digits[21];
    int n = 0;
    if (v == 0) { vj_app(buf, pos, max, "0"); return; }
    while (v > 0 && n < 20) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }
    char rev[21];
    for (int i = 0; i < n; i++) rev[i] = digits[n - 1 - i];
    rev[n] = '\0';
    vj_app(buf, pos, max, rev);
}

// ─── Row-correlation context ────────────────────────────────────────────
// id_col_idx is looked up ONCE per underlying query (from that query's own
// SqlResult.columns[]/column_count -- populated for SELECT * in table
// column order, confirmed by reading sql_exec.c's own select_all handling
// directly, not assumed), then reused for every row cursor_fetch_rows()
// delivers for that query.
struct vj_ctx {
    const struct VecMatch* matches;       // the FULL caller-supplied array, not just this batch --
    uint32_t                match_count;  // correlation is correct regardless of batching this way
    int32_t                 id_col_idx;   // -1 = id_column wasn't found in the result's own column list
    VecJoinRowCb             cb;
    void*                    user_ctx;
    uint32_t                 delivered;
};

static void vj_row_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct vj_ctx* ctx = (struct vj_ctx*)ctxp;
    if (ctx->id_col_idx < 0 || (uint32_t)ctx->id_col_idx >= v->count) return;

    uint64_t row_ext_id;
    if (vj_parse_u64(v->values[ctx->id_col_idx], &row_ext_id) != 0) {
        return;   // id_column's stored text isn't a plain unsigned integer -- can't
                  // correlate this row to any external_id; skip it cleanly rather
                  // than crash or guess, matching this codebase's own established
                  // "fail the one thing that's wrong, not the whole call" posture.
    }

    for (uint32_t i = 0; i < ctx->match_count; i++) {
        if (ctx->matches[i].external_id == row_ext_id) {
            ctx->cb(&ctx->matches[i], v, ctx->user_ctx);
            ctx->delivered++;
            // Deliberately NOT a `break` here -- see vec_join.h's own
            // header comment: every real (match, row) correlation is
            // delivered, including duplicates in either matches[] or
            // id_column's stored values, not just the first one found.
        }
    }
}

uint32_t vec_join_resolve(uint32_t caller_uid, const char* table_name,
                          const char* id_column, const struct VecMatch* matches,
                          uint32_t match_count, VecJoinRowCb cb, void* ctx) {
    if (!table_name || !id_column || !matches || !cb || match_count == 0) return 0;

    uint32_t total_delivered = 0;
    uint32_t i = 0;

    while (i < match_count) {
        char sql[SQL_MAX_TEXT_LEN];
        uint32_t pos = 0;
        sql[0] = '\0';
        vj_app(sql, &pos, sizeof(sql), "SELECT * FROM ");
        vj_app(sql, &pos, sizeof(sql), table_name);
        vj_app(sql, &pos, sizeof(sql), " WHERE ");

        uint32_t terms = 0;
        // Two independent limits, whichever is hit first ends this batch
        // (see vec_join.h's own header comment on why SQL_MAX_TEXT_LEN
        // alone isn't a safe assumption): VEC_JOIN_MAX_IDS_PER_QUERY (the
        // real predicate.h node-pool ceiling), and the SQL text buffer
        // itself actually having room for one more term.
        while (i < match_count && terms < VEC_JOIN_MAX_IDS_PER_QUERY) {
            char term[128];
            uint32_t tpos = 0;
            term[0] = '\0';
            if (terms > 0) vj_app(term, &tpos, sizeof(term), " OR ");
            vj_app(term, &tpos, sizeof(term), id_column);
            vj_app(term, &tpos, sizeof(term), " = ");
            vj_app_u64(term, &tpos, sizeof(term), matches[i].external_id);

            // Would appending this term overflow the SQL text buffer? Stop
            // the batch here rather than truncating a query mid-term -- a
            // silently truncated query is a WRONG RESULT (drops some
            // external_ids without saying so), which this project's own
            // "denial looks like absence" discipline treats as strictly
            // worse than issuing an additional, smaller query for the
            // remainder.
            if (pos + tpos >= sizeof(sql) - 1) break;

            vj_app(sql, &pos, sizeof(sql), term);
            terms++;
            i++;
        }

        if (terms == 0) {
            // Not even one term fits (an unrealistically long table/column
            // name) -- nothing more this function can safely do for this
            // particular match; skip it rather than loop forever on it.
            i++;
            continue;
        }

        struct SqlResult result;
        int rc = sql_execute(caller_uid, sql, &result);
        if (rc == 0 && result.kind == SQL_STMT_SELECT) {
            int32_t id_col_idx = -1;
            for (uint32_t c = 0; c < result.column_count; c++) {
                if (vj_streq(result.columns[c], id_column)) { id_col_idx = (int32_t)c; break; }
            }
            struct vj_ctx rctx = { matches, match_count, id_col_idx, cb, ctx, 0 };
            cursor_fetch_rows(result.cursor_id, result.row_count, vj_row_cb, &rctx);
            cursor_close(result.cursor_id);
            total_delivered += rctx.delivered;
        }
        // A query failure for one batch (bad table/column name, permission
        // denied, etc.) doesn't abort the whole resolve -- matches outside
        // this batch still get their own chance at the next iteration.
    }

    return total_delivered;
}

// ─── Gap Remediation Phase C: live reachability adapter ────────────────────
static void vjs_collect_cb(const struct VecMatch* match, const struct RowValues* row, void* ctxp) {
    struct SLSVecJoinRequest* req = (struct SLSVecJoinRequest*)ctxp;
    if (req->result_count < VEC_JOIN_MAX_RESULTS) {
        req->results[req->result_count].match = *match;
        req->results[req->result_count].row = *row;
    }
    req->result_count++;   // count past the cap too -- how truncation is detected
}

uint64_t sys_sls_vec_join(struct SLSVecJoinRequest* req) {
    if (!req) return 1;
    req->result_count = 0;
    uint32_t k = req->match_count;
    if (k > VEC_SEARCH_MAX_K) k = VEC_SEARCH_MAX_K;
    vec_join_resolve(req->caller_uid, req->table_name, req->id_column,
                     req->matches, k, vjs_collect_cb, req);
    req->truncated = (req->result_count > VEC_JOIN_MAX_RESULTS) ? 1 : 0;
    return 0;
}
