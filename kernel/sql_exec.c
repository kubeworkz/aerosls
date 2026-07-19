/*
 * sql_exec.c — Phase 19 (relational layer) planner + executor. See
 * sql_exec.h for the full design writeup.
 */
#include "sql_exec.h"
#include "object_catalog.h"
#include "row_index.h"
#include "predicate.h"
#include "cursor.h"
#include <stddef.h>

// ─── String / parsing helpers (no libc — each kernel source file keeps its
// own small copies; se_* here, matching rs_*/ri_*/pe_*/sq_* elsewhere). ────
static void se_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static int se_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void se_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}
static int se_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}
static int se_parse_f64(const char* s, double* out) {
    if (!s || !s[0]) return 1;
    uint32_t i = 0;
    int neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    if (!s[i]) return 1;
    double v = 0.0;
    int saw_digit = 0;
    for (; s[i] >= '0' && s[i] <= '9'; i++) { v = v * 10.0 + (double)(s[i] - '0'); saw_digit = 1; }
    if (s[i] == '.') {
        i++;
        double frac = 0.1;
        for (; s[i] >= '0' && s[i] <= '9'; i++) { v += (double)(s[i] - '0') * frac; frac *= 0.1; saw_digit = 1; }
    }
    if (s[i] != '\0' || !saw_digit) return 1;
    *out = neg ? -v : v;
    return 0;
}
static int se_parse_bool(const char* s, uint8_t* out) {
    if (se_streq(s, "true") || se_streq(s, "1"))  { *out = 1; return 0; }
    if (se_streq(s, "false") || se_streq(s, "0")) { *out = 0; return 0; }
    return 1;
}
static int se_strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// ─── Table / column lookup ───────────────────────────────────────────────
static int find_table_catalog_index(const char* table_name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (se_streq(object_catalog[i].name, table_name)) return (int)i;
    }
    return -1;
}
static uint32_t find_column_index(const struct RowTableLayout* layout, const char* name) {
    for (uint32_t i = 0; i < layout->column_count; i++)
        if (se_streq(layout->column_names[i], name)) return i;
    return 0xFFFFFFFFu;
}

// Type-aware comparison between two rows' values for one column -- used by
// ORDER BY. Unparseable values compare equal (0) rather than crashing or
// producing an undefined order beyond "stays roughly where it was."
static int compare_rows_by_column(const struct RowTableLayout* layout, uint32_t col,
                                  const struct RowValues* a, const struct RowValues* b) {
    const char* ta = a->values[col];
    const char* tb = b->values[col];
    switch (layout->column_types[col]) {
        case FIELD_TYPE_UINT64: {
            uint64_t va, vb;
            if (se_parse_u64(ta, &va) || se_parse_u64(tb, &vb)) return 0;
            return (va > vb) - (va < vb);
        }
        case FIELD_TYPE_FLOAT: {
            double va, vb;
            if (se_parse_f64(ta, &va) || se_parse_f64(tb, &vb)) return 0;
            return (va > vb) - (va < vb);
        }
        case FIELD_TYPE_BOOL: {
            uint8_t va, vb;
            if (se_parse_bool(ta, &va) || se_parse_bool(tb, &vb)) return 0;
            return (int)va - (int)vb;
        }
        case FIELD_TYPE_STRING:
        default:
            return se_strcmp(ta, tb);
    }
}

// Walks a predicate tree confirming every comparison's column exists in
// layout -- used to reject a WHERE clause naming an unknown column up
// front (SQL_ERR_COLUMN_NOT_FOUND) rather than letting it silently match
// zero rows via predicate_eval()'s own fail-closed-per-row behavior. The
// two failure modes look similar ("this WHERE matches nothing") but a
// typo'd column name is a malformed QUERY, not a legitimately-empty
// RESULT, and deserves a clear error the same way exec_select() already
// gives one for an unknown SELECT/ORDER BY column -- this makes WHERE
// consistent with that, across SELECT/UPDATE/DELETE alike.
static int predicate_columns_valid(const struct Predicate* pred, uint32_t idx,
                                   const struct RowTableLayout* layout) {
    if (idx >= pred->node_count) return 0;
    const struct PredicateNode* n = &pred->nodes[idx];
    if (n->kind == PRED_NODE_COMPARISON) return find_column_index(layout, n->column_name) != 0xFFFFFFFFu;
    return predicate_columns_valid(pred, n->left, layout) && predicate_columns_valid(pred, n->right, layout);
}

// ─── Planner + candidate row collection ─────────────────────────────────────
struct sql_collect_ctx { struct RowId* ids; uint32_t count; uint32_t max; };
static void sql_collect_cb(struct RowId id, const struct RowValues* values, void* ctxp) {
    (void)values;
    struct sql_collect_ctx* ctx = (struct sql_collect_ctx*)ctxp;
    if (ctx->count < ctx->max) ctx->ids[ctx->count] = id;
    ctx->count++;
}

// Returns the TRUE total match count (may exceed max_ids -- caller detects
// truncation the same way row_index_lookup()/_range_scan() already do).
static uint32_t sql_find_matching_rows(uint32_t caller_uid, int tidx, const char* table_name,
                                       const struct Predicate* pred,
                                       struct RowId* out_ids, uint32_t max_ids) {
    const struct RowTableLayout* layout = &table_headers[tidx].layout;

    // Trivial planner: only a single top-level comparison (not AND/OR-
    // wrapped) against an indexed, non-"!="-compared column is eligible.
    // See sql_exec.h's header comment for why the index path is always
    // re-checked against the full predicate rather than trusted outright.
    if (pred && pred->root != PREDICATE_INVALID_NODE) {
        const struct PredicateNode* n = &pred->nodes[pred->root];
        if (n->kind == PRED_NODE_COMPARISON && n->op != PRED_OP_NE) {
            uint32_t col = find_column_index(layout, n->column_name);
            if (col != 0xFFFFFFFFu) {
                char idx_name[OBJECT_NAME_LEN];
                if (row_index_find_for_column(object_catalog[tidx].object_id, col, idx_name)) {
                    struct RowId candidates[CURSOR_MAX_ROWSET_ROWS];
                    uint32_t cand_n;
                    if (n->op == PRED_OP_EQ) {
                        cand_n = row_index_lookup(caller_uid, idx_name, n->literal, candidates, CURSOR_MAX_ROWSET_ROWS);
                    } else {
                        const char* lo = (n->op == PRED_OP_GT || n->op == PRED_OP_GE) ? n->literal : NULL;
                        const char* hi = (n->op == PRED_OP_LT || n->op == PRED_OP_LE) ? n->literal : NULL;
                        cand_n = row_index_range_scan(caller_uid, idx_name, lo, hi, candidates, CURSOR_MAX_ROWSET_ROWS);
                    }
                    uint32_t take = cand_n < CURSOR_MAX_ROWSET_ROWS ? cand_n : CURSOR_MAX_ROWSET_ROWS;
                    uint32_t found = 0;
                    for (uint32_t i = 0; i < take; i++) {
                        struct RowValues rv;
                        if (rowstore_row_get(caller_uid, table_name, candidates[i], &rv) != 0) continue;
                        if (!predicate_eval(pred, layout, &rv)) continue;
                        if (found < max_ids) out_ids[found] = candidates[i];
                        found++;
                    }
                    return found;
                }
            }
        }
    }

    // Fallback: full predicate_table_scan() (Phase 18) -- also the path
    // for "no WHERE at all" (pred == NULL), matching predicate_table_scan's
    // own "NULL predicate visits every row" convention.
    struct sql_collect_ctx ctx = { out_ids, 0, max_ids };
    predicate_table_scan(caller_uid, table_name, pred, sql_collect_cb, &ctx);
    return ctx.count;
}

// ─── Statement execution ─────────────────────────────────────────────────
// A SELECT's materialized row buffer: CURSOR_MAX_ROWSET_ROWS entries of
// struct RowValues (~4 KiB each) is far too large to put on a freestanding
// kernel's stack (~1 MiB total) -- static scratch instead, the same
// "non-reentrant for now" tradeoff sql_execute()'s own struct SqlStatement
// makes below. This is a real, documented consequence of Phase 21 (real
// concurrency control) not existing yet, not an oversight: nothing in this
// project's row-set path is safe under concurrent execution today.
static struct RowValues g_select_scratch[CURSOR_MAX_ROWSET_ROWS];

static void exec_select_join(uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);

static void exec_select(uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    if (s->has_join) { exec_select_join(caller_uid, s, out); return; }

    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;

    if (!s->select_all) {
        for (uint32_t i = 0; i < s->column_count; i++) {
            if (find_column_index(layout, s->columns[i]) == 0xFFFFFFFFu) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "SELECT column not found in table", SQL_ERR_MSG_LEN);
                return;
            }
        }
    }
    uint32_t order_col = 0xFFFFFFFFu;
    if (s->has_order_by) {
        order_col = find_column_index(layout, s->order_by);
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in table", SQL_ERR_MSG_LEN);
            return;
        }
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    struct RowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = sql_find_matching_rows(caller_uid, tidx, s->table_name,
                                            s->has_where ? &s->where : NULL,
                                            ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;
    out->truncated = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;

    for (uint32_t i = 0; i < n; i++) {
        if (rowstore_row_get(caller_uid, s->table_name, ids[i], &g_select_scratch[i]) != 0) {
            se_memset(&g_select_scratch[i], 0, sizeof(g_select_scratch[i]));
        }
    }

    if (s->has_order_by) {
        // Insertion sort: O(n^2), bounded by CURSOR_MAX_ROWSET_ROWS -- fine
        // for a first cut with no cost model, matching this whole phase's
        // "trivial planner" posture applied to sorting too.
        for (uint32_t i = 1; i < n; i++) {
            struct RowValues key = g_select_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(layout, order_col, &g_select_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_select_scratch[j + 1] = g_select_scratch[j];
                j--;
            }
            g_select_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < n) n = s->limit;

    uint32_t cid = cursor_open_rowset(s->table_name, g_select_scratch, n);

    out->error      = SQL_ERR_NONE;
    out->cursor_id  = cid;
    out->row_count  = n;
    if (s->select_all) {
        out->column_count = layout->column_count;
        for (uint32_t i = 0; i < layout->column_count; i++)
            se_strcpy(out->columns[i], layout->column_names[i], RECORD_KEY_LEN);
    } else {
        out->column_count = s->column_count;
        for (uint32_t i = 0; i < s->column_count; i++)
            se_strcpy(out->columns[i], s->columns[i], RECORD_KEY_LEN);
    }
}

// ─── Phase 20: two-table JOIN ────────────────────────────────────────────
static void build_qualified_name(char* out, uint32_t max, const char* table, const char* col) {
    uint32_t i = 0;
    for (; table[i] && i < max - 1; i++) out[i] = table[i];
    if (i < max - 1) out[i++] = '.';
    for (uint32_t j = 0; col[j] && i < max - 1; j++) out[i++] = col[j];
    out[i] = '\0';
}

// The ON clause's temporary single-comparison probe predicate (built once
// per outer row of A, inside join_outer_cb below) is a struct Predicate --
// several KB (32 nodes x a 256-byte literal each), same sizing concern as
// g_stmt_scratch above. Reused/reinitialized per call via predicate_init(),
// safe since execution is single-threaded/non-reentrant throughout this
// file already (see the notes above).
static struct Predicate g_join_probe_pred;

struct join_ctx {
    uint32_t                     caller_uid;
    int                          tidx_b;
    const char*                  table_b_name;
    uint32_t                     join_col_a;
    uint32_t                     join_col_b;
    const struct RowTableLayout* layout_a;
    const struct RowTableLayout* layout_b;
    const struct RowTableLayout* combined_layout;
    const struct Predicate*      where;   // NULL if no WHERE clause
    uint32_t                     count;   // total combined rows matched so far (may exceed CURSOR_MAX_ROWSET_ROWS)
};

// Called once per row of table A (the outer loop). Probes table B for
// matching rows using the SAME sql_find_matching_rows() the single-table
// path uses -- this is what makes "indexed nested-loop join when B has an
// index on the join column" fall out for free: sql_find_matching_rows()
// already prefers an index for a single EQ comparison, and a join-column
// probe is exactly that shape.
static void join_outer_cb(struct RowId id_a, const struct RowValues* row_a, void* ctxp) {
    (void)id_a;
    struct join_ctx* ctx = (struct join_ctx*)ctxp;

    predicate_init(&g_join_probe_pred);
    uint32_t probe_root = predicate_add_comparison(&g_join_probe_pred,
        ctx->layout_b->column_names[ctx->join_col_b], PRED_OP_EQ, row_a->values[ctx->join_col_a]);
    if (probe_root == PREDICATE_INVALID_NODE) return;   // shouldn't happen (one fresh node), fail closed if it ever does
    g_join_probe_pred.root = probe_root;

    struct RowId matches[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total_b = sql_find_matching_rows(ctx->caller_uid, ctx->tidx_b, ctx->table_b_name,
                                              &g_join_probe_pred, matches, CURSOR_MAX_ROWSET_ROWS);
    uint32_t take_b = total_b < CURSOR_MAX_ROWSET_ROWS ? total_b : CURSOR_MAX_ROWSET_ROWS;

    for (uint32_t i = 0; i < take_b; i++) {
        struct RowValues row_b;
        if (rowstore_row_get(ctx->caller_uid, ctx->table_b_name, matches[i], &row_b) != 0) continue;

        struct RowValues combined;
        se_memset(&combined, 0, sizeof(combined));
        combined.count = ctx->combined_layout->column_count;
        for (uint32_t c = 0; c < ctx->layout_a->column_count; c++)
            se_strcpy(combined.values[c], row_a->values[c], RECORD_VAL_LEN);
        for (uint32_t c = 0; c < ctx->layout_b->column_count; c++)
            se_strcpy(combined.values[ctx->layout_a->column_count + c], row_b.values[c], RECORD_VAL_LEN);

        if (ctx->where && !predicate_eval(ctx->where, ctx->combined_layout, &combined)) continue;

        if (ctx->count < CURSOR_MAX_ROWSET_ROWS) g_select_scratch[ctx->count] = combined;
        ctx->count++;
    }
}

static void exec_select_join(uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    int tidx_a = find_table_catalog_index(s->table_name);
    if (tidx_a < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "left table not found", SQL_ERR_MSG_LEN); return; }
    int tidx_b = find_table_catalog_index(s->join_table);
    if (tidx_b < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "joined table not found", SQL_ERR_MSG_LEN); return; }

    const struct RowTableLayout* layout_a = &table_headers[tidx_a].layout;
    const struct RowTableLayout* layout_b = &table_headers[tidx_b].layout;

    if (layout_a->column_count + layout_b->column_count > ROWSTORE_MAX_COLUMNS) {
        out->error = SQL_ERR_JOIN_TOO_WIDE;
        se_strcpy(out->error_msg, "joined tables have too many combined columns", SQL_ERR_MSG_LEN);
        return;
    }

    // Resolve the ON clause's two "qualifier.column" halves to A/B -- the
    // parser captured them in whichever order the user wrote them (see
    // sql_parser.h), so this accepts either "A.x = B.y" or "B.y = A.x".
    int a_is_left;
    if (se_streq(s->join_left_qualifier, s->table_name) && se_streq(s->join_right_qualifier, s->join_table)) {
        a_is_left = 1;
    } else if (se_streq(s->join_left_qualifier, s->join_table) && se_streq(s->join_right_qualifier, s->table_name)) {
        a_is_left = 0;
    } else {
        out->error = SQL_ERR_JOIN_INVALID;
        se_strcpy(out->error_msg, "ON clause qualifiers don't match the two joined tables", SQL_ERR_MSG_LEN);
        return;
    }
    const char* col_a_name = a_is_left ? s->join_left_col : s->join_right_col;
    const char* col_b_name = a_is_left ? s->join_right_col : s->join_left_col;
    uint32_t join_col_a = find_column_index(layout_a, col_a_name);
    uint32_t join_col_b = find_column_index(layout_b, col_b_name);
    if (join_col_a == 0xFFFFFFFFu || join_col_b == 0xFFFFFFFFu) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "ON clause column not found in the corresponding table", SQL_ERR_MSG_LEN);
        return;
    }

    // Build the combined synthetic layout: every column from A then every
    // column from B, renamed "tablename.column". row_width/rows_per_page
    // are left zero -- this layout is a query-time-only construct (never
    // persisted or paged), and neither field is read by any of
    // predicate_eval()/find_column_index()/compare_rows_by_column(), the
    // only consumers of a layout in this file.
    struct RowTableLayout combined;
    se_memset(&combined, 0, sizeof(combined));
    combined.column_count = layout_a->column_count + layout_b->column_count;
    for (uint32_t c = 0; c < layout_a->column_count; c++) {
        build_qualified_name(combined.column_names[c], RECORD_KEY_LEN, s->table_name, layout_a->column_names[c]);
        combined.column_types[c] = layout_a->column_types[c];
    }
    for (uint32_t c = 0; c < layout_b->column_count; c++) {
        build_qualified_name(combined.column_names[layout_a->column_count + c], RECORD_KEY_LEN, s->join_table, layout_b->column_names[c]);
        combined.column_types[layout_a->column_count + c] = layout_b->column_types[c];
    }

    if (!s->select_all) {
        for (uint32_t i = 0; i < s->column_count; i++) {
            if (find_column_index(&combined, s->columns[i]) == 0xFFFFFFFFu) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "SELECT column not found in the joined result (use table.column)", SQL_ERR_MSG_LEN);
                return;
            }
        }
    }
    uint32_t order_col = 0xFFFFFFFFu;
    if (s->has_order_by) {
        order_col = find_column_index(&combined, s->order_by);
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in the joined result (use table.column)", SQL_ERR_MSG_LEN);
            return;
        }
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, &combined)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column (use table.column)", SQL_ERR_MSG_LEN);
        return;
    }

    struct join_ctx ctx;
    ctx.caller_uid      = caller_uid;
    ctx.tidx_b           = tidx_b;
    ctx.table_b_name     = s->join_table;
    ctx.join_col_a       = join_col_a;
    ctx.join_col_b       = join_col_b;
    ctx.layout_a         = layout_a;
    ctx.layout_b         = layout_b;
    ctx.combined_layout  = &combined;
    ctx.where            = s->has_where ? &s->where : NULL;
    ctx.count            = 0;

    // Nested-loop join: A is always the outer scan (the FROM table, no
    // cost-based side selection -- see sql_exec.h). No WHERE pushdown into
    // either side's scan before joining -- a real, named non-goal (query-
    // optimizer territory, out of scope per this roadmap's own Phase 22
    // framing), not an oversight.
    rowstore_table_scan(caller_uid, s->table_name, join_outer_cb, &ctx);

    uint32_t n = ctx.count < CURSOR_MAX_ROWSET_ROWS ? ctx.count : CURSOR_MAX_ROWSET_ROWS;
    out->truncated = (ctx.count > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;

    if (s->has_order_by) {
        for (uint32_t i = 1; i < n; i++) {
            struct RowValues key = g_select_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(&combined, order_col, &g_select_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_select_scratch[j + 1] = g_select_scratch[j];
                j--;
            }
            g_select_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < n) n = s->limit;

    // table_name here is metadata-only (cursor.h's struct comment) -- a
    // joined cursor's rows span two tables, so this just records where the
    // query started, not an authoritative single-table identity.
    uint32_t cid = cursor_open_rowset(s->table_name, g_select_scratch, n);

    out->error     = SQL_ERR_NONE;
    out->cursor_id = cid;
    out->row_count = n;
    if (s->select_all) {
        out->column_count = combined.column_count;
        for (uint32_t i = 0; i < combined.column_count; i++)
            se_strcpy(out->columns[i], combined.column_names[i], RECORD_KEY_LEN);
    } else {
        out->column_count = s->column_count;
        for (uint32_t i = 0; i < s->column_count; i++)
            se_strcpy(out->columns[i], s->columns[i], RECORD_KEY_LEN);
    }
}

static void exec_insert(uint32_t caller_uid, const struct SqlInsertStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;

    if (s->count != layout->column_count) {
        out->error = SQL_ERR_COLUMN_COUNT_MISMATCH;
        se_strcpy(out->error_msg, "INSERT must specify every column (no NULL/default support yet)", SQL_ERR_MSG_LEN);
        return;
    }

    struct RowValues values;
    se_memset(&values, 0, sizeof(values));
    values.count = layout->column_count;
    uint8_t seen[ROWSTORE_MAX_COLUMNS];
    se_memset(seen, 0, sizeof(seen));

    for (uint32_t i = 0; i < s->count; i++) {
        uint32_t col = find_column_index(layout, s->columns[i]);
        if (col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "INSERT column not found in table", SQL_ERR_MSG_LEN);
            return;
        }
        se_strcpy(values.values[col], s->values[i], RECORD_VAL_LEN);
        seen[col] = 1;
    }
    for (uint32_t c = 0; c < layout->column_count; c++) {
        if (!seen[c]) {
            out->error = SQL_ERR_COLUMN_COUNT_MISMATCH;
            se_strcpy(out->error_msg, "INSERT is missing a required column", SQL_ERR_MSG_LEN);
            return;
        }
    }

    struct RowId new_id;
    int rc = rowstore_row_insert(caller_uid, s->table_name, &values, &new_id);
    if (rc != 0) {
        out->error = (rc == 2) ? SQL_ERR_PERMISSION_DENIED : SQL_ERR_VALUE_INVALID;
        se_strcpy(out->error_msg, "INSERT rejected by the row store", SQL_ERR_MSG_LEN);
        return;
    }
    out->error         = SQL_ERR_NONE;
    out->affected_rows = 1;
    out->inserted_id   = new_id;
}

static void exec_update(uint32_t caller_uid, const struct SqlUpdateStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;

    uint32_t set_cols[ROWSTORE_MAX_COLUMNS];
    for (uint32_t i = 0; i < s->set_count; i++) {
        uint32_t col = find_column_index(layout, s->set_columns[i]);
        if (col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "SET column not found in table", SQL_ERR_MSG_LEN);
            return;
        }
        set_cols[i] = col;
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    struct RowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = sql_find_matching_rows(caller_uid, tidx, s->table_name,
                                            s->has_where ? &s->where : NULL,
                                            ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;

    uint32_t affected = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct RowValues rv;
        if (rowstore_row_get(caller_uid, s->table_name, ids[i], &rv) != 0) continue;
        for (uint32_t j = 0; j < s->set_count; j++)
            se_strcpy(rv.values[set_cols[j]], s->set_values[j], RECORD_VAL_LEN);
        if (rowstore_row_update(caller_uid, s->table_name, ids[i], &rv) == 0) affected++;
    }
    out->error         = SQL_ERR_NONE;
    out->affected_rows = affected;
    out->truncated     = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;
}

static void exec_delete(uint32_t caller_uid, const struct SqlDeleteStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    struct RowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = sql_find_matching_rows(caller_uid, tidx, s->table_name,
                                            s->has_where ? &s->where : NULL,
                                            ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;

    uint32_t affected = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (rowstore_row_delete(caller_uid, s->table_name, ids[i]) == 0) affected++;
    }
    out->error         = SQL_ERR_NONE;
    out->affected_rows = affected;
    out->truncated     = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;
}

// See exec_select's own comment above on why this is static scratch, not a
// stack-local: struct SqlStatement embeds a struct Predicate (32 nodes x a
// 256-byte literal each), several KB on its own -- too large to put on a
// freestanding kernel's stack repeatedly, same reasoning, same tradeoff.
static struct SqlStatement g_stmt_scratch;

int sql_execute(uint32_t caller_uid, const char* sql_text, struct SqlResult* out) {
    se_memset(out, 0, sizeof(*out));

    char err[SQL_ERR_MSG_LEN];
    if (sql_parse(sql_text, &g_stmt_scratch, err, sizeof(err)) != 0) {
        out->kind  = SQL_STMT_INVALID;
        out->error = SQL_ERR_PARSE;
        se_strcpy(out->error_msg, err, SQL_ERR_MSG_LEN);
        return 1;
    }

    out->kind = g_stmt_scratch.kind;
    switch (g_stmt_scratch.kind) {
        case SQL_STMT_SELECT: exec_select(caller_uid, &g_stmt_scratch.u.select, out); break;
        case SQL_STMT_INSERT: exec_insert(caller_uid, &g_stmt_scratch.u.insert, out); break;
        case SQL_STMT_UPDATE: exec_update(caller_uid, &g_stmt_scratch.u.update, out); break;
        case SQL_STMT_DELETE: exec_delete(caller_uid, &g_stmt_scratch.u.del,    out); break;
        default:
            out->error = SQL_ERR_INTERNAL;
            se_strcpy(out->error_msg, "unhandled statement kind", SQL_ERR_MSG_LEN);
            break;
    }
    return out->error == SQL_ERR_NONE ? 0 : 1;
}
