/*
 * sql_exec.c — Phase 19 (relational layer) planner + executor. See
 * sql_exec.h for the full design writeup.
 */
#include "sql_exec.h"
#include "object_catalog.h"
#include "predicate.h"
#include "cursor.h"
#include "mvcc.h"
#include "row_index.h"
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

// ─── Phase 1 (SQL Feature-Parity Roadmap): number-to-string formatting for
// aggregate results (COUNT/SUM/AVG) -- RowValues is text-in/text-out
// throughout this codebase (see rowstore.h), so a computed aggregate needs
// to become a string before it can be a "row" the rest of this file's
// existing machinery (predicate_eval for HAVING, compare_rows_by_column for
// ORDER BY, cursor_open_rowset for the result) already knows how to handle
// unchanged. ───────────────────────────────────────────────────────────────
static void se_fmt_u64(char* out, uint32_t max, uint64_t v) {
    char tmp[24]; int l = 0;
    if (!v) tmp[l++] = '0';
    else while (v) { tmp[l++] = (char)('0' + (v % 10)); v /= 10; }
    uint32_t i = 0;
    for (int k = l - 1; k >= 0 && i < max - 1; k--) out[i++] = tmp[k];
    out[i] = '\0';
}
// Fixed-point, up to 6 fractional digits, trailing zeros trimmed -- matches
// se_parse_f64()'s own simple decimal-only grammar (no exponents), so
// every value this writes is guaranteed re-parseable by that function.
static void se_fmt_f64(char* out, uint32_t max, double v) {
    int neg = v < 0.0;
    if (neg) v = -v;
    uint64_t ip = (uint64_t)v;
    double frac = v - (double)ip;
    uint64_t fp = (uint64_t)(frac * 1000000.0 + 0.5);
    if (fp >= 1000000ULL) { fp -= 1000000ULL; ip += 1; }
    uint32_t i = 0;
    if (neg && i < max - 1) out[i++] = '-';
    char ipbuf[24]; se_fmt_u64(ipbuf, sizeof(ipbuf), ip);
    for (uint32_t j = 0; ipbuf[j] && i < max - 1; j++) out[i++] = ipbuf[j];
    if (fp > 0) {
        char digits[6];
        uint64_t t = fp;
        for (int k = 5; k >= 0; k--) { digits[k] = (char)('0' + (t % 10)); t /= 10; }
        int end = 6;
        while (end > 1 && digits[end - 1] == '0') end--;   // trim trailing zeros, keep at least one digit
        if (i < max - 1) out[i++] = '.';
        for (int k = 0; k < end && i < max - 1; k++) out[i++] = digits[k];
    }
    out[i] = '\0';
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

// Type-aware comparison between two raw value strings of a given column
// type -- shared by compare_rows_by_column() (ORDER BY, below) and Phase 1
// (SQL Feature-Parity Roadmap)'s MIN/MAX aggregate tracking in
// exec_select_group(), which has no two full RowValues rows to hand it,
// only a "best value so far" string per group. Unparseable values compare
// equal (0) rather than crashing or producing an undefined order beyond
// "stays roughly where it was."
static int compare_typed(SLSFieldType type, const char* ta, const char* tb) {
    switch (type) {
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

// Type-aware comparison between two rows' values for one column -- used by
// ORDER BY.
static int compare_rows_by_column(const struct RowTableLayout* layout, uint32_t col,
                                  const struct RowValues* a, const struct RowValues* b) {
    return compare_typed(layout->column_types[col], a->values[col], b->values[col]);
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

// ─── Planner + candidate row collection (Phase 22: MVCC-routed; Phase 25:
// index-assisted equality lookups restored) ─────────────────────────────
// Phase 19/20's original Phase-17-index-assisted short cut stopped
// surviving Phase 22's MVCC routing: a B-tree index (row_index.h) only
// ever stores physical RowIds, and under MVCC a column value's physical
// row changes on every UPDATE, with no notion in the index of which
// version is visible to a given transaction's snapshot (see sql_exec.h's
// header comment for the original scope note this phase closes out).
//
// mvcc_resolve_physical() (mvcc.h, Phase 25) is the piece that makes an
// index hit trustworthy again: given a physical RowId, it reports whether
// that EXACT version is the one visible to this transaction's snapshot, in
// O(1) average (a hash lookup, not a scan). row_index_lookup_checked()
// (row_index.h, Phase 25) is the other half: it reports whether an index
// key's result can be trusted as COMPLETE, closing a second, independent
// hole discovered while building this -- a B-tree leaf's duplicate list
// (BTREE_MAX_DUPES_PER_KEY=16 entries per distinct value) fills up with
// every VERSION a value ever had under MVCC, not just current ones, so a
// hot value can silently exceed the cap with no signal available from the
// plain row_index_lookup() API. Both pieces together let ONE case be
// safely accelerated: a single top-level equality comparison against an
// indexed column. Every candidate the index returns is still rechecked
// against the full predicate before being trusted (the same "index
// narrows, recheck is the authority" shape this whole roadmap has used
// since Phase 19) -- this is a genuine algorithmic improvement (real
// O(1)-ish candidate resolution, not a fake one), not a relaxation of the
// correctness bar.
//
// Deliberately narrower than Phase 19's original design, which also
// index-assisted range comparisons (<, >, <=, >=) and let AND/OR combine
// with an index probe. Range comparisons are NOT restored here: the
// completeness problem above applies independently to EVERY distinct key a
// range touches, with no aggregate "was anything dropped in this range"
// signal available without a larger row_index.c change -- a real, named
// limitation, not an oversight (see the roadmap findings addendum). Ranges
// and AND/OR predicates always fall back to the full scan below, exactly
// as they did before this phase.
struct mvcc_collect_ctx {
    struct MvccRowId*            ids;
    uint32_t                     count;
    uint32_t                     max;
    const struct Predicate*      pred;
    const struct RowTableLayout* layout;
};
static void mvcc_collect_cb(struct MvccRowId id, const struct RowValues* values, void* ctxp) {
    struct mvcc_collect_ctx* ctx = (struct mvcc_collect_ctx*)ctxp;
    if (ctx->pred && !predicate_eval(ctx->pred, ctx->layout, values)) return;
    if (ctx->count < ctx->max) ctx->ids[ctx->count] = id;
    ctx->count++;
}

// Attempts the index-assisted fast path for a single top-level EQ
// comparison against an indexed column. Returns 1 (and fills *out_count,
// out_ids) if the index path was used and its result is provably complete;
// 0 if it declined for any reason (no such predicate shape, no index on
// that column, or the index result couldn't be proven complete) -- the
// caller falls back to the full scan exactly as before this phase.
static int try_index_assisted_eq(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                                 const struct Predicate* pred, const struct RowTableLayout* layout,
                                 struct MvccRowId* out_ids, uint32_t max_ids, uint32_t* out_count) {
    if (!pred || pred->root == PREDICATE_INVALID_NODE) return 0;
    const struct PredicateNode* n = &pred->nodes[pred->root];
    if (n->kind != PRED_NODE_COMPARISON || n->op != PRED_OP_EQ) return 0;

    uint32_t col = find_column_index(layout, n->column_name);
    if (col == 0xFFFFFFFFu) return 0;

    int cidx = find_table_catalog_index(table_name);
    if (cidx < 0) return 0;
    uint64_t table_object_id = object_catalog[cidx].object_id;

    char index_name[OBJECT_NAME_LEN];
    if (!row_index_find_for_column(table_object_id, col, index_name)) return 0;

    struct RowId candidates[BTREE_MAX_DUPES_PER_KEY];
    uint8_t complete = 0;
    uint32_t found = row_index_lookup_checked(caller_uid, index_name, n->literal,
                                              candidates, BTREE_MAX_DUPES_PER_KEY, &complete);
    if (!complete) return 0;   // this key's duplicate list was capped at some point --
                                 // can't prove the index result is the full match set

    uint32_t n_out = 0;
    for (uint32_t i = 0; i < found && n_out < max_ids; i++) {
        struct MvccRowId mid;
        struct RowValues vals;
        if (mvcc_resolve_physical(txn_id, caller_uid, table_name, candidates[i], &mid, &vals) != MVCC_OK)
            continue;   // this physical version is stale/superseded/not visible here -- an
                         // OLDER version of a row the index still remembers, not a current match
        if (!predicate_eval(pred, layout, &vals)) continue;   // authoritative recheck
        out_ids[n_out++] = mid;
    }
    *out_count = n_out;
    return 1;
}

// Returns the TRUE total match count (may exceed max_ids -- caller detects
// truncation the same way the pre-Phase-22 planner already did).
static uint32_t mvcc_find_matching_rows(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                                        const struct Predicate* pred, const struct RowTableLayout* layout,
                                        struct MvccRowId* out_ids, uint32_t max_ids) {
    uint32_t idx_count;
    if (try_index_assisted_eq(txn_id, caller_uid, table_name, pred, layout, out_ids, max_ids, &idx_count))
        return idx_count;

    struct mvcc_collect_ctx ctx = { out_ids, 0, max_ids, pred, layout };
    mvcc_table_scan(txn_id, caller_uid, table_name, mvcc_collect_cb, &ctx);
    return ctx.count;
}

// Maps an MvccError onto the closest SqlErrorCode -- shared by every
// exec_* function below so the mapping is defined exactly once.
static SqlErrorCode map_mvcc_err(MvccError e) {
    switch (e) {
        case MVCC_OK:                    return SQL_ERR_NONE;
        case MVCC_ERR_TABLE_NOT_FOUND:    return SQL_ERR_TABLE_NOT_FOUND;
        case MVCC_ERR_PERMISSION_DENIED:  return SQL_ERR_PERMISSION_DENIED;
        case MVCC_ERR_ROW_NOT_VISIBLE:    return SQL_ERR_ROW_NOT_FOUND;
        case MVCC_ERR_WRITE_CONFLICT:     return SQL_ERR_WRITE_CONFLICT;
        case MVCC_ERR_TXN_NOT_ACTIVE:     return SQL_ERR_TXN_NOT_ACTIVE;
        case MVCC_ERR_TXN_TABLE_FULL:     return SQL_ERR_TXN_UNAVAILABLE;
        case MVCC_ERR_VALUES_INVALID:     return SQL_ERR_VALUE_INVALID;
        case MVCC_ERR_VERSION_POOL_FULL:  return SQL_ERR_INTERNAL;
        // Phase 23: all five row_constraint.c violation kinds collapse to
        // one SqlErrorCode -- the caller_uid/table/column-level detail
        // lives in row_constraint.c's own return value, not in SQL's
        // error surface, matching this file's existing "one error per
        // subsystem checkpoint, not one per internal reason" posture
        // (e.g. every mvcc_table_scan() failure already collapses the
        // same way above).
        case MVCC_ERR_CONSTRAINT_UNIQUE:
        case MVCC_ERR_CONSTRAINT_NOT_NULL:
        case MVCC_ERR_CONSTRAINT_RANGE:
        case MVCC_ERR_CONSTRAINT_REFERENCE:
        case MVCC_ERR_CONSTRAINT_REFERENCED:  return SQL_ERR_CONSTRAINT_VIOLATION;
        default:                         return SQL_ERR_INTERNAL;
    }
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

// ─── Phase 1 (SQL Feature-Parity Roadmap): GROUP BY / aggregate buckets ────
// A fresh implementation over real RowValues rows -- NOT a call into
// kernel/aggregate.h's aggregate_exec(), which operates on the legacy
// object_records[] KV model exclusively and is structurally incompatible
// with row-set tables (see sql_parser.h's own header comment for the full
// rationale). SQL_MAX_GROUPS=64 matches kernel/aggregate.h's own
// AGG_MAX_GROUPS sizing precedent for the same kind of bucket table.
#define SQL_MAX_GROUPS 64
struct sql_agg_bucket {
    uint8_t  active;
    char     group_key[RECORD_VAL_LEN];       // meaningful only when the query has an explicit GROUP BY
    uint32_t count;
    // Parallel to the SELECT list (s->columns[]/agg_fn[]/agg_arg[]), NOT to
    // the source table's own columns -- one accumulator slot per
    // SELECT-list item, since a single query can aggregate more than one
    // column (`SELECT dept, SUM(salary), MAX(age) FROM ... GROUP BY dept`).
    double   sum[ROWSTORE_MAX_COLUMNS];
    uint8_t  has_data[ROWSTORE_MAX_COLUMNS];
    char     min_val[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
    char     max_val[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
};
static struct sql_agg_bucket g_agg_buckets[SQL_MAX_GROUPS];

static void exec_select_join(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);
static void exec_select_group(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);

static void exec_select(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    if (s->has_aggregates) { exec_select_group(txn_id, caller_uid, s, out); return; }
    if (s->has_join) { exec_select_join(txn_id, caller_uid, s, out); return; }

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

    struct MvccRowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, s->table_name,
                                             s->has_where ? &s->where : NULL, layout,
                                             ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;
    out->truncated = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;

    for (uint32_t i = 0; i < n; i++) {
        if (mvcc_row_get(txn_id, caller_uid, s->table_name, ids[i], &g_select_scratch[i]) != MVCC_OK) {
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
    uint64_t                     txn_id;
    uint32_t                     caller_uid;
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
// matching rows using the SAME mvcc_find_matching_rows() the single-table
// path uses. Phase 22 note: since that planner no longer takes an
// index-assisted short cut under MVCC routing (see sql_exec.h's header
// comment), the join probe is now always a full, snapshot-consistent scan
// of B per outer row too -- "indexed nested-loop join for free" no longer
// applies once a query runs through a real transaction. Correctness is
// unaffected; only this join's algorithmic complexity regressed, a real,
// named consequence of this phase, not a silent one.
static void join_outer_cb(struct MvccRowId id_a, const struct RowValues* row_a, void* ctxp) {
    (void)id_a;
    struct join_ctx* ctx = (struct join_ctx*)ctxp;

    predicate_init(&g_join_probe_pred);
    uint32_t probe_root = predicate_add_comparison(&g_join_probe_pred,
        ctx->layout_b->column_names[ctx->join_col_b], PRED_OP_EQ, row_a->values[ctx->join_col_a]);
    if (probe_root == PREDICATE_INVALID_NODE) return;   // shouldn't happen (one fresh node), fail closed if it ever does
    g_join_probe_pred.root = probe_root;

    struct MvccRowId matches[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total_b = mvcc_find_matching_rows(ctx->txn_id, ctx->caller_uid, ctx->table_b_name,
                                               &g_join_probe_pred, ctx->layout_b, matches, CURSOR_MAX_ROWSET_ROWS);
    uint32_t take_b = total_b < CURSOR_MAX_ROWSET_ROWS ? total_b : CURSOR_MAX_ROWSET_ROWS;

    for (uint32_t i = 0; i < take_b; i++) {
        struct RowValues row_b;
        if (mvcc_row_get(ctx->txn_id, ctx->caller_uid, ctx->table_b_name, matches[i], &row_b) != MVCC_OK) continue;

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

static void exec_select_join(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
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
    ctx.txn_id            = txn_id;
    ctx.caller_uid        = caller_uid;
    ctx.table_b_name      = s->join_table;
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
    // optimizer territory, out of scope per this roadmap's own Phase 25
    // framing), not an oversight. Phase 22: the outer scan is now
    // snapshot-consistent (mvcc_table_scan()), not a raw physical scan.
    mvcc_table_scan(txn_id, caller_uid, s->table_name, join_outer_cb, &ctx);

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

// ─── Phase 1 (SQL Feature-Parity Roadmap): GROUP BY / HAVING / aggregates ──
// Single table only (see sql_parser.h -- GROUP BY + JOIN in one statement
// is a real, named scope cut, not an oversight). Validates everything
// up-front against the SOURCE table layout and a synthetic per-group
// RESULT layout (built from the SELECT list, before any row is scanned --
// same "validate cheaply first, scan expensively second" discipline
// exec_select()/exec_select_join() already follow), then does one
// snapshot-consistent scan, bucketing matched rows by the GROUP BY column
// (or a single implicit bucket for a bare aggregate with no GROUP BY at
// all, e.g. `SELECT COUNT(*) FROM t`), then formats each surviving bucket
// (after HAVING) into a real RowValues row so ORDER BY/LIMIT/cursor_open_
// rowset() all work completely unchanged against the grouped result.
static void exec_select_group(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    if (s->has_join) {
        out->error = SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED;
        se_strcpy(out->error_msg, "GROUP BY/aggregate functions combined with JOIN are not supported yet", SQL_ERR_MSG_LEN);
        return;
    }

    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;

    uint32_t group_col = 0xFFFFFFFFu;
    if (s->has_group_by) {
        group_col = find_column_index(layout, s->group_by);
        if (group_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "GROUP BY column not found in table", SQL_ERR_MSG_LEN);
            return;
        }
    }

    // Validate every SELECT-list item and resolve each aggregate's argument
    // column against the SOURCE table layout (agg_col[i] is only meaningful
    // when agg_fn[i] != SQL_AGG_NONE and the argument isn't COUNT's "*").
    uint32_t agg_col[ROWSTORE_MAX_COLUMNS];
    for (uint32_t i = 0; i < s->column_count; i++) {
        if (s->agg_fn[i] == SQL_AGG_NONE) {
            // Standard SQL rule: an ordinary column in the SELECT list
            // under GROUP BY/aggregates must BE the GROUP BY column -- it
            // can't just silently take its first-seen value per group.
            if (!s->has_group_by || !se_streq(s->columns[i], s->group_by)) {
                out->error = SQL_ERR_GROUP_BY_COLUMN_INVALID;
                se_strcpy(out->error_msg, "SELECT column must be the GROUP BY column or an aggregate function", SQL_ERR_MSG_LEN);
                return;
            }
            continue;
        }
        if (s->agg_fn[i] == SQL_AGG_COUNT && se_streq(s->agg_arg[i], "*")) continue;
        uint32_t c = find_column_index(layout, s->agg_arg[i]);
        if (c == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "aggregate function argument column not found in table", SQL_ERR_MSG_LEN);
            return;
        }
        if ((s->agg_fn[i] == SQL_AGG_SUM || s->agg_fn[i] == SQL_AGG_AVG) &&
            layout->column_types[c] != FIELD_TYPE_UINT64 && layout->column_types[c] != FIELD_TYPE_FLOAT) {
            out->error = SQL_ERR_VALUE_INVALID;
            se_strcpy(out->error_msg, "SUM/AVG requires a UINT64 or FLOAT argument column", SQL_ERR_MSG_LEN);
            return;
        }
        agg_col[i] = c;
    }

    // Build the synthetic per-group RESULT layout -- purely from the parsed
    // statement + source layout types, no row data needed yet, matching
    // exec_select_join()'s own "combined" layout precedent (a query-time-
    // only construct, never persisted or paged, that every existing
    // predicate_eval()/find_column_index()/compare_rows_by_column() call
    // works against completely unchanged because the right names/types are
    // just baked into it up front).
    struct RowTableLayout group_layout;
    se_memset(&group_layout, 0, sizeof(group_layout));
    for (uint32_t i = 0; i < s->column_count; i++) {
        se_strcpy(group_layout.column_names[i], s->columns[i], RECORD_KEY_LEN);
        if (s->agg_fn[i] == SQL_AGG_NONE)            group_layout.column_types[i] = layout->column_types[group_col];
        else if (s->agg_fn[i] == SQL_AGG_COUNT)      group_layout.column_types[i] = FIELD_TYPE_UINT64;
        else if (s->agg_fn[i] == SQL_AGG_SUM ||
                 s->agg_fn[i] == SQL_AGG_AVG)         group_layout.column_types[i] = FIELD_TYPE_FLOAT;
        else /* MIN/MAX */                            group_layout.column_types[i] = layout->column_types[agg_col[i]];
    }
    group_layout.column_count = s->column_count;

    uint32_t order_col = 0xFFFFFFFFu;
    if (s->has_order_by) {
        order_col = find_column_index(&group_layout, s->order_by);
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in the grouped result (use the exact SELECT list label, e.g. COUNT(*))", SQL_ERR_MSG_LEN);
            return;
        }
    }
    if (s->has_having && !predicate_columns_valid(&s->having, s->having.root, &group_layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "HAVING clause references a column/aggregate not in the SELECT list", SQL_ERR_MSG_LEN);
        return;
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    // ── Scan (WHERE-filtered, snapshot-consistent -- same mvcc_find_
    // matching_rows() every other exec_* function uses) + bucket ──────────
    struct MvccRowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, s->table_name,
                                             s->has_where ? &s->where : NULL, layout,
                                             ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;
    uint8_t row_truncated = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;

    for (uint32_t gi = 0; gi < SQL_MAX_GROUPS; gi++) g_agg_buckets[gi].active = 0;
    uint32_t group_count = 0;
    uint8_t group_overflow = 0;

    for (uint32_t i = 0; i < n; i++) {
        struct RowValues rv;
        if (mvcc_row_get(txn_id, caller_uid, s->table_name, ids[i], &rv) != MVCC_OK) continue;

        const char* gk = s->has_group_by ? rv.values[group_col] : "";
        struct sql_agg_bucket* b = 0;
        for (uint32_t gi = 0; gi < group_count; gi++) {
            if (g_agg_buckets[gi].active && se_streq(g_agg_buckets[gi].group_key, gk)) { b = &g_agg_buckets[gi]; break; }
        }
        if (!b) {
            if (group_count >= SQL_MAX_GROUPS) { group_overflow = 1; continue; }
            b = &g_agg_buckets[group_count++];
            se_memset(b, 0, sizeof(*b));
            se_strcpy(b->group_key, gk, RECORD_VAL_LEN);
            b->active = 1;
        }

        b->count++;
        for (uint32_t ci = 0; ci < s->column_count; ci++) {
            if (s->agg_fn[ci] == SQL_AGG_NONE || s->agg_fn[ci] == SQL_AGG_COUNT) continue;   // COUNT already covered by b->count
            const char* valstr = rv.values[agg_col[ci]];
            if (s->agg_fn[ci] == SQL_AGG_MIN || s->agg_fn[ci] == SQL_AGG_MAX) {
                SLSFieldType t = layout->column_types[agg_col[ci]];
                if (!b->has_data[ci]) {
                    se_strcpy(b->min_val[ci], valstr, RECORD_VAL_LEN);
                    se_strcpy(b->max_val[ci], valstr, RECORD_VAL_LEN);
                } else {
                    if (compare_typed(t, valstr, b->min_val[ci]) < 0) se_strcpy(b->min_val[ci], valstr, RECORD_VAL_LEN);
                    if (compare_typed(t, valstr, b->max_val[ci]) > 0) se_strcpy(b->max_val[ci], valstr, RECORD_VAL_LEN);
                }
                b->has_data[ci] = 1;
                continue;
            }
            // SUM/AVG
            double v;
            if (se_parse_f64(valstr, &v)) continue;   // unparseable -- skip this value (fail-closed-per-value, not a whole-statement abort)
            b->sum[ci] += v;
            b->has_data[ci] = 1;
        }
    }

    // A bare aggregate with no GROUP BY still reports exactly one result
    // row even when zero source rows matched (`SELECT COUNT(*) FROM t
    // WHERE false` is 1 row with count=0 in standard SQL, not an empty
    // result set) -- GROUP BY with zero matches correctly reports zero
    // groups instead, the real distinction between "one implicit group
    // always exists" and "grouping happens over whatever rows existed."
    if (group_count == 0 && !s->has_group_by) {
        se_memset(&g_agg_buckets[0], 0, sizeof(g_agg_buckets[0]));
        g_agg_buckets[0].active = 1;
        group_count = 1;
    }

    // ── Format each surviving (post-HAVING) bucket into a real row ────────
    uint32_t out_n = 0;
    for (uint32_t gi = 0; gi < group_count; gi++) {
        struct sql_agg_bucket* b = &g_agg_buckets[gi];
        if (!b->active) continue;

        struct RowValues row;
        se_memset(&row, 0, sizeof(row));
        row.count = group_layout.column_count;
        for (uint32_t i = 0; i < s->column_count; i++) {
            switch (s->agg_fn[i]) {
                case SQL_AGG_NONE:  se_strcpy(row.values[i], b->group_key, RECORD_VAL_LEN); break;
                case SQL_AGG_COUNT: se_fmt_u64(row.values[i], RECORD_VAL_LEN, b->count); break;
                case SQL_AGG_SUM:   se_fmt_f64(row.values[i], RECORD_VAL_LEN, b->has_data[i] ? b->sum[i] : 0.0); break;
                case SQL_AGG_AVG:   se_fmt_f64(row.values[i], RECORD_VAL_LEN, (b->has_data[i] && b->count > 0) ? b->sum[i] / (double)b->count : 0.0); break;
                case SQL_AGG_MIN:   se_strcpy(row.values[i], b->has_data[i] ? b->min_val[i] : "", RECORD_VAL_LEN); break;
                case SQL_AGG_MAX:   se_strcpy(row.values[i], b->has_data[i] ? b->max_val[i] : "", RECORD_VAL_LEN); break;
            }
        }

        if (s->has_having && !predicate_eval(&s->having, &group_layout, &row)) continue;

        if (out_n < CURSOR_MAX_ROWSET_ROWS) g_select_scratch[out_n] = row;
        out_n++;
    }

    if (s->has_order_by) {
        for (uint32_t i = 1; i < out_n && i < CURSOR_MAX_ROWSET_ROWS; i++) {
            struct RowValues key = g_select_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(&group_layout, order_col, &g_select_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_select_scratch[j + 1] = g_select_scratch[j];
                j--;
            }
            g_select_scratch[j + 1] = key;
        }
    }

    uint32_t final_n = out_n < CURSOR_MAX_ROWSET_ROWS ? out_n : CURSOR_MAX_ROWSET_ROWS;
    if (s->has_limit && s->limit < final_n) final_n = s->limit;

    uint32_t cid = cursor_open_rowset(s->table_name, g_select_scratch, final_n);

    out->error      = SQL_ERR_NONE;
    out->cursor_id  = cid;
    out->row_count  = final_n;
    out->truncated  = (row_truncated || group_overflow || out_n > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;
    out->column_count = group_layout.column_count;
    for (uint32_t i = 0; i < group_layout.column_count; i++)
        se_strcpy(out->columns[i], group_layout.column_names[i], RECORD_KEY_LEN);
}

static void exec_insert(uint64_t txn_id, uint32_t caller_uid, const struct SqlInsertStmt* s, struct SqlResult* out) {
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

    struct MvccRowId new_id;
    MvccError rc = mvcc_row_insert(txn_id, caller_uid, s->table_name, &values, &new_id);
    if (rc != MVCC_OK) {
        out->error = map_mvcc_err(rc);
        se_strcpy(out->error_msg, out->error == SQL_ERR_CONSTRAINT_VIOLATION
                  ? "INSERT violates a UNIQUE, NOT NULL, RANGE, or REFERENCE constraint"
                  : "INSERT rejected by the row store", SQL_ERR_MSG_LEN);
        return;
    }
    out->error         = SQL_ERR_NONE;
    out->affected_rows = 1;
    out->inserted_id   = new_id;
}

static void exec_update(uint64_t txn_id, uint32_t caller_uid, const struct SqlUpdateStmt* s, struct SqlResult* out) {
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

    struct MvccRowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, s->table_name,
                                             s->has_where ? &s->where : NULL, layout,
                                             ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;

    // Statement-level atomicity (Phase 22): a write-write conflict on ANY
    // matched row aborts the WHOLE statement rather than reporting a
    // partial affected-row count -- under sql_execute()'s autocommit
    // wrapper the caller then rolls back the transaction, which correctly
    // undoes every row this loop already updated before hitting the
    // conflict, not just the row that conflicted. Matches this whole
    // roadmap's "fail cleanly, no partial effects" posture.
    uint32_t affected = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct RowValues rv;
        if (mvcc_row_get(txn_id, caller_uid, s->table_name, ids[i], &rv) != MVCC_OK) continue;
        for (uint32_t j = 0; j < s->set_count; j++)
            se_strcpy(rv.values[set_cols[j]], s->set_values[j], RECORD_VAL_LEN);
        MvccError rc = mvcc_row_update(txn_id, caller_uid, s->table_name, ids[i], &rv);
        // Phase 23 fix: this used to special-case only MVCC_ERR_WRITE_CONFLICT
        // and silently drop any other non-OK rc (constraint violations
        // included) without incrementing affected_rows OR setting out->error --
        // a fresh instance of this project's recurring "denial looks like
        // absence" class (see mvcc_txn_is_active()'s own Phase 22 fix). Any
        // non-OK rc now aborts the whole statement, matching the write-conflict
        // path's existing statement-level-atomicity contract.
        if (rc == MVCC_ERR_WRITE_CONFLICT) {
            out->error = SQL_ERR_WRITE_CONFLICT;
            se_strcpy(out->error_msg, "a row this UPDATE would affect was concurrently modified by another transaction", SQL_ERR_MSG_LEN);
            out->affected_rows = 0;
            return;
        }
        if (rc != MVCC_OK) {
            out->error = map_mvcc_err(rc);
            se_strcpy(out->error_msg, out->error == SQL_ERR_CONSTRAINT_VIOLATION
                      ? "UPDATE violates a UNIQUE, NOT NULL, RANGE, or REFERENCE constraint"
                      : "UPDATE rejected by the row store", SQL_ERR_MSG_LEN);
            out->affected_rows = 0;
            return;
        }
        affected++;
    }
    out->error         = SQL_ERR_NONE;
    out->affected_rows = affected;
    out->truncated     = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;
}

static void exec_delete(uint64_t txn_id, uint32_t caller_uid, const struct SqlDeleteStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    struct MvccRowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, s->table_name,
                                             s->has_where ? &s->where : NULL, layout,
                                             ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;

    // Statement-level atomicity -- same reasoning as exec_update() above.
    uint32_t affected = 0;
    for (uint32_t i = 0; i < n; i++) {
        MvccError rc = mvcc_row_delete(txn_id, caller_uid, s->table_name, ids[i]);
        // Same "any non-OK rc aborts the statement" fix as exec_update() above.
        if (rc == MVCC_ERR_WRITE_CONFLICT) {
            out->error = SQL_ERR_WRITE_CONFLICT;
            se_strcpy(out->error_msg, "a row this DELETE would affect was concurrently modified by another transaction", SQL_ERR_MSG_LEN);
            out->affected_rows = 0;
            return;
        }
        if (rc != MVCC_OK) {
            out->error = map_mvcc_err(rc);
            se_strcpy(out->error_msg, out->error == SQL_ERR_CONSTRAINT_VIOLATION
                      ? "DELETE violates a REFERENCE constraint (row is still referenced by another table)"
                      : "DELETE rejected by the row store", SQL_ERR_MSG_LEN);
            out->affected_rows = 0;
            return;
        }
        affected++;
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

// Runs ONE already-parsed statement against an already-open txn_id -- the
// shared core both sql_execute() (autocommit) and sql_execute_tx()
// (caller-managed transaction) dispatch through. Not itself public.
static void dispatch_stmt(uint64_t txn_id, uint32_t caller_uid, struct SqlStatement* stmt, struct SqlResult* out) {
    out->kind = stmt->kind;
    // mvcc_table_scan()/mvcc_row_get() etc. report "transaction inactive"
    // and "found nothing" identically (both return an empty/zero result,
    // matching their own documented contracts) -- without this check, a
    // SELECT against a bad txn_id would silently come back as an empty
    // result instead of a real error, the exact "denial looks like
    // absence" ambiguity Phase 19's predicate_columns_valid() was built to
    // avoid for unknown WHERE columns. Checked once, here, rather than
    // separately inside every exec_* function.
    if (!mvcc_txn_is_active(txn_id)) {
        out->error = SQL_ERR_TXN_NOT_ACTIVE;
        se_strcpy(out->error_msg, "transaction is not active", SQL_ERR_MSG_LEN);
        return;
    }
    switch (stmt->kind) {
        case SQL_STMT_SELECT: exec_select(txn_id, caller_uid, &stmt->u.select, out); break;
        case SQL_STMT_INSERT: exec_insert(txn_id, caller_uid, &stmt->u.insert, out); break;
        case SQL_STMT_UPDATE: exec_update(txn_id, caller_uid, &stmt->u.update, out); break;
        case SQL_STMT_DELETE: exec_delete(txn_id, caller_uid, &stmt->u.del,    out); break;
        default:
            out->error = SQL_ERR_INTERNAL;
            se_strcpy(out->error_msg, "unhandled statement kind", SQL_ERR_MSG_LEN);
            break;
    }
}

int sql_execute_tx(uint64_t txn_id, uint32_t caller_uid, const char* sql_text, struct SqlResult* out) {
    se_memset(out, 0, sizeof(*out));

    char err[SQL_ERR_MSG_LEN];
    if (sql_parse(sql_text, &g_stmt_scratch, err, sizeof(err)) != 0) {
        out->kind  = SQL_STMT_INVALID;
        out->error = SQL_ERR_PARSE;
        se_strcpy(out->error_msg, err, SQL_ERR_MSG_LEN);
        return 1;
    }

    dispatch_stmt(txn_id, caller_uid, &g_stmt_scratch, out);
    return out->error == SQL_ERR_NONE ? 0 : 1;
}

int sql_execute(uint32_t caller_uid, const char* sql_text, struct SqlResult* out) {
    se_memset(out, 0, sizeof(*out));

    uint64_t txn_id = mvcc_begin();
    if (txn_id == 0) {
        out->kind  = SQL_STMT_INVALID;
        out->error = SQL_ERR_TXN_UNAVAILABLE;
        se_strcpy(out->error_msg, "too many concurrently active transactions", SQL_ERR_MSG_LEN);
        return 1;
    }

    int rc = sql_execute_tx(txn_id, caller_uid, sql_text, out);
    if (out->error == SQL_ERR_NONE) {
        mvcc_commit(txn_id);
    } else {
        mvcc_rollback(txn_id, caller_uid);
    }
    return rc;
}

uint64_t sql_tx_begin(void) {
    return mvcc_begin();
}
int sql_tx_commit(uint64_t txn_id) {
    return mvcc_commit(txn_id) == MVCC_OK ? 0 : 1;
}
int sql_tx_rollback(uint64_t txn_id, uint32_t caller_uid) {
    return mvcc_rollback(txn_id, caller_uid) == MVCC_OK ? 0 : 1;
}

uint64_t sys_sls_sql_execute(struct SLSSqlRequest* req) {
    if (!req) return 1;
    return (uint64_t)sql_execute(req->caller_uid, req->sql_text, &req->result);
}
