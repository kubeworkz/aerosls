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
#include "row_constraint.h"   // Phase 5 (SQL Feature-Parity Roadmap, DDL): inline column constraints
#include "../user/permissions.h"   // Phase 8 follow-on (schema export/import): PERM_READ
#include "database.h"   // Database Namespace & Access Roadmap Phase 2: CREATE/DROP DATABASE, IN DATABASE tagging
#include "persist.h"    // Database Gap Analysis §2.3: persist_catalog() after ALTER TABLE ... SET DATABASE retags
#include "view.h"       // Query-Surface Roadmap Phase 5: CREATE/DROP VIEW, FROM-a-view resolution
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
// Query-Surface Roadmap Phase 7: needed by substitute_outer_refs() below.
static uint32_t se_strlen(const char* s) {
    uint32_t n = 0; while (s[n]) n++; return n;
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

// ─── Query-Surface Roadmap Phase 3: depth-indexed scratch banking ─────────
// This whole file has always been "non-reentrant for now" (see the note on
// g_stmt_scratch below) -- every per-query static scratch buffer is a
// single instance, safe only because nothing ever recursively calls back
// into dispatch_stmt() while an outer call is still mid-use of its own
// buffers. Phase 4's UNION right branch, and Phase 5/6's view/CTE
// expansion, are all about to need exactly that recursive call (run a
// nested SELECT to a materialized result while the outer statement's own
// materialization is still needed for the merge/substitution step) -- this
// section is the enabler, landed on its own with zero observable behavior
// change so those phases have somewhere real to plug into.
//
// The fix is depth-indexed banking, not a bigger single buffer: g_exec_depth
// tracks which of exactly 2 banked slots is "live" for whoever is currently
// executing (0 = the top-level statement, 1 = one nested statement inside
// it). Each banked buffer keeps its original name as an object-like macro
// expanding to `_bank[g_exec_depth]` -- every existing reference throughout
// this file (there are dozens, across exec_select/join_materialize/
// exec_select_group/dispatch_stmt) keeps working completely unchanged,
// because `g_select_scratch[i]` textually becomes
// `g_select_scratch_bank[g_exec_depth][i]`, still a valid expression. This
// is a deliberate, narrow use of macro-shadowing specifically because
// rewriting every call site to thread a bank index through dozens of
// function signatures would be a much larger, much riskier diff for the
// exact same result.
//
// Banked here: g_stmt_scratch, g_select_scratch, g_join_scratch_b,
// g_agg_buckets, g_join_probe_pred -- the scope the roadmap names
// explicitly. Deliberately NOT banked: g_subquery_stmt_scratch/
// g_subquery_result_text (Phase 7's subquery machinery resolves fully at
// the top of dispatch_stmt(), before any nested exec runs -- there is no
// window where it's live across a recursive dispatch_stmt() call, so
// banking it would add BSS cost for a hazard that doesn't exist); and
// g_join_matched_ids (Query-Surface Phase 2's RIGHT/FULL anti-pass set,
// entirely populated and consumed within one synchronous join step of
// join_materialize() -- a JOIN's ON/WHERE clauses never themselves invoke
// nested statement dispatch, so it's never live across a recursive call
// either, same reasoning as the subquery scratch).
//
// sql_exec_depth_enter()/_leave() are the pair a future nested caller
// (Phase 4's UNION, Phase 5/6's view/CTE expansion) must use symmetrically
// around its own recursive dispatch_stmt() call -- enter before, leave
// after, exactly like the internal reentrancy test below does manually
// (Phase 3 adds no real nested call site of its own; that's Phase 4+).
// SQL_EXEC_MAX_DEPTH banked slots means depths 0 and 1 are valid; a would-be
// depth-2 call fails loud via SQL_ERR_NESTING_TOO_DEEP rather than silently
// wrapping around and reusing depth-1's still-in-progress buffers out from
// under it -- the same "denial looks like absence" bug class this project
// names every time a near-miss of it is found, avoided here by construction
// before any real caller exists to trigger it.
#define SQL_EXEC_MAX_DEPTH 2
static uint32_t g_exec_depth = 0;

// Returns 1 and increments g_exec_depth on success; returns 0 (out->error/
// error_msg already set to SQL_ERR_NESTING_TOO_DEEP) if no banked slot is
// left. Callers MUST pair a successful enter with exactly one
// sql_exec_depth_leave() call, including on every early-return error path
// after entering -- same discipline this file already applies to
// txn_id/cursor lifetimes elsewhere.
static int sql_exec_depth_enter(struct SqlResult* out) {
    if (g_exec_depth + 1 >= SQL_EXEC_MAX_DEPTH) {
        out->error = SQL_ERR_NESTING_TOO_DEEP;
        se_strcpy(out->error_msg, "SQL statement nesting too deep (max 1 level of nested SELECT)", SQL_ERR_MSG_LEN);
        return 0;
    }
    g_exec_depth++;
    return 1;
}
static void sql_exec_depth_leave(void) {
    g_exec_depth--;
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
    // Phase 3 (SQL Feature-Parity Roadmap): an arithmetic-comparison leaf
    // has no bare column_name (see predicate.h) -- validate each operand
    // that IS a column reference instead; a numeric-literal operand needs
    // no column lookup at all.
    if (n->kind == PRED_NODE_ARITH_COMPARISON) {
        if (n->arith_op1.is_column && find_column_index(layout, n->arith_op1.text) == 0xFFFFFFFFu) return 0;
        if (n->arith_op != PRED_ARITH_NONE && n->arith_op2.is_column && find_column_index(layout, n->arith_op2.text) == 0xFFFFFFFFu) return 0;
        return 1;
    }
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
        case MVCC_ERR_CONSTRAINT_REFERENCED:
        case MVCC_ERR_CASCADE_FAILED:         return SQL_ERR_CONSTRAINT_VIOLATION;   // Cascading phase -- same collapse
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
// Query-Surface Roadmap Phase 3: banked (see the depth-indexed scratch
// banking note above) so a nested SELECT at depth 1 gets its own
// materialization buffer instead of overwriting the outer statement's.
static struct RowValues g_select_scratch_bank[SQL_EXEC_MAX_DEPTH][CURSOR_MAX_ROWSET_ROWS];
#define g_select_scratch (g_select_scratch_bank[g_exec_depth])

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
// Query-Surface Roadmap Phase 3: banked, same reasoning as
// g_select_scratch above.
static struct sql_agg_bucket g_agg_buckets_bank[SQL_EXEC_MAX_DEPTH][SQL_MAX_GROUPS];
#define g_agg_buckets (g_agg_buckets_bank[g_exec_depth])

static void exec_select_join(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);
static void exec_select_group(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);
static void exec_select_set_op(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);
static void exec_select_view(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out, int view_idx);
static void exec_select_cte(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);
static void exec_select_correlated(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out);

static void exec_select(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    // Query-Surface Roadmap Phase 7: correlated subqueries. Checked FIRST,
    // ahead of the CTE check below (and everything after it), since a
    // correlated subquery needs the narrowest possible query shape (its
    // own dedicated per-outer-row path, bypassing set-op/aggregate/JOIN/
    // CTE dispatch entirely) -- combining it with ANY of those four is a
    // real, named v1 scope cut, given a LOUD, distinctly-worded rejection
    // here rather than silently reusing the older "unresolved subquery
    // marker fails closed to false" fallback a non-correlated subquery
    // under JOIN/GROUP BY still gets (see predicate.h's Phase 7 notes).
    if (s->has_where && predicate_has_correlated_subquery(&s->where)) {
        if (s->has_join || s->has_aggregates || s->has_set_op || s->has_cte) {
            out->error = SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED;
            se_strcpy(out->error_msg, "a correlated subquery cannot be combined with JOIN, aggregates, a set operator, or a CTE (v1 scope cut) -- only a plain SELECT ... FROM <table> WHERE ... is supported", SQL_ERR_MSG_LEN);
            return;
        }
        exec_select_correlated(txn_id, caller_uid, s, out);
        return;
    }

    // Query-Surface Roadmap Phase 6: WITH ... AS (...) CTE resolution --
    // checked FIRST, ahead of set-op/aggregate/JOIN dispatch below AND
    // ahead of find_table_catalog_index()/view_find_index() further down,
    // because a CTE must shadow a same-named real TABLE or VIEW (standard
    // SQL scoping) -- the OPPOSITE precedence from Phase 5's views, which
    // only ever win as a fallback once a real table lookup has already
    // failed (see view.h's own namespace-collision note). v1 is exactly
    // as narrow as views: a CTE may only be the sole FROM source of a
    // plain query, never combined with a JOIN/aggregates/a set operator
    // in the SAME statement -- caught here, in ONE place, before any of
    // those three dispatch away below, which is why (unlike views) there
    // is no separate "aggregates over a CTE" gap to name: this check
    // already covers it.
    if (s->has_cte && se_streq(s->table_name, s->cte_name)) {
        if (s->has_join || s->has_aggregates || s->has_set_op) {
            out->error = SQL_ERR_CTE_SCOPE_UNSUPPORTED;
            se_strcpy(out->error_msg, "a CTE cannot be combined with JOIN, aggregates, or a set operator (v1 scope cut) -- only a plain SELECT ... FROM <cte-name> [WHERE/ORDER BY/LIMIT] is supported", SQL_ERR_MSG_LEN);
            return;
        }
        exec_select_cte(txn_id, caller_uid, s, out);
        return;
    }

    // Query-Surface Roadmap Phase 4: checked first, ahead of aggregates/
    // JOIN -- a set-op statement's LEFT branch (this same struct, minus
    // has_set_op/has_order_by/has_limit) can itself be a JOIN or aggregate
    // query, so exec_select_set_op() re-enters exec_select() for that left
    // branch rather than duplicating this dispatch.
    if (s->has_set_op) { exec_select_set_op(txn_id, caller_uid, s, out); return; }
    if (s->has_aggregates) { exec_select_group(txn_id, caller_uid, s, out); return; }
    if (s->has_join) { exec_select_join(txn_id, caller_uid, s, out); return; }

    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) {
        // Query-Surface Roadmap Phase 5: a real TABLE always wins this
        // resolution when both a table and a view somehow share a name
        // (see view.h's own namespace-collision note) -- the view registry
        // is only ever consulted here, as a fallback, never ahead of
        // find_table_catalog_index(). has_join/has_aggregates/has_set_op
        // were already checked and dispatched away above, so this is
        // exactly the "FROM v [WHERE...] [ORDER BY...] [LIMIT n]" v1 shape
        // the roadmap doc specs -- a view referenced from a JOIN or under
        // aggregates never reaches this fallback at all (see
        // join_materialize()'s own explicit SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED
        // check, and exec_select_group()'s FROM-table resolution, which
        // has no such fallback and so still reports plain TABLE_NOT_FOUND
        // for a view name under GROUP BY -- a real, named gap: "SELECT ...
        // FROM view GROUP BY ..." isn't supported, only plain queries are).
        int vidx = view_find_index(s->table_name);
        if (vidx >= 0) { exec_select_view(txn_id, caller_uid, s, out, vidx); return; }
        out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return;
    }
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

// ─── Query-Surface Roadmap Phase 4: UNION / UNION ALL / INTERSECT / EXCEPT ─
// Not banked (unlike g_select_scratch/g_agg_buckets/etc. above): a set-op
// statement can never be reached from within its own right branch's
// execution, because the parser rejects a second top-level set-op keyword
// while capturing the first one's raw text (see sql_parser.c's
// parse_select_body()) -- so no input could ever reach exec time with a
// right branch that itself triggers exec_select_set_op() again. Exactly
// the same "provably never live across a nested call" reasoning that
// already justifies leaving g_subquery_stmt_scratch and g_join_matched_ids
// unbanked (see the depth-banking note above).
static struct RowValues g_setop_left_scratch[CURSOR_MAX_ROWSET_ROWS];
static struct RowValues g_setop_right_scratch[CURSOR_MAX_ROWSET_ROWS];
static struct RowValues g_setop_merge_scratch[CURSOR_MAX_ROWSET_ROWS];

struct setop_row_ctx {
    struct RowValues* out;
    uint32_t           count;   // total rows scanned so far (may exceed max)
    uint32_t           max;
};
static void setop_row_collect_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct setop_row_ctx* ctx = (struct setop_row_ctx*)ctxp;
    if (ctx->count < ctx->max) ctx->out[ctx->count] = *v;
    ctx->count++;
}

// Row equality for dedup/membership (UNION/INTERSECT/EXCEPT), NOT the same
// thing as WHERE's own NULL != NULL predicate semantics -- two NULLs at the
// same column position count as equal here, matching every other engine's
// UNION/INTERSECT/EXCEPT convention that treats a row's NULLs as a
// comparable part of its identity for set-membership purposes even though
// a predicate's NULL is never "true" against anything, including itself.
static int setop_rows_equal(const struct RowValues* a, const struct RowValues* b) {
    if (a->count != b->count) return 0;
    for (uint32_t i = 0; i < a->count; i++) {
        int a_null = (a->null_mask & (1u << i)) != 0;
        int b_null = (b->null_mask & (1u << i)) != 0;
        if (a_null != b_null) return 0;
        if (!a_null && !se_streq(a->values[i], b->values[i])) return 0;
    }
    return 1;
}

// Runs the LEFT branch (this same statement, minus set-op/ORDER BY/LIMIT --
// those apply to the merged result only), then the RIGHT branch (the raw
// text captured at parse time, re-parsed and dispatched as an entirely
// ordinary NESTED statement at depth 1 -- Query-Surface Roadmap Phase 3's
// banking is exactly what makes this safe: the left branch's own
// g_select_scratch/g_join_scratch_b/g_agg_buckets/g_join_probe_pred at
// depth 0 are untouched by whatever the right branch's own JOIN/aggregate/
// WHERE logic does at depth 1, even though both branches may use every one
// of those buffers internally). Column-count mismatch between branches is
// a loud error; column NAMES (and, in this v1, effective ORDER BY
// comparison behavior -- see below) follow the LEFT branch, standard SQL
// posture. Both branches, and the final merge, are capped at
// CURSOR_MAX_ROWSET_ROWS (256) -- the same "documented cap, not silent/
// unbounded" posture this file already applies to every other rowset.
static void exec_select_set_op(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    struct SqlSelectStmt left = *s;
    left.has_set_op   = 0;
    left.has_order_by = 0;
    left.has_limit    = 0;

    struct SqlResult left_out;
    se_memset(&left_out, 0, sizeof(left_out));
    exec_select(txn_id, caller_uid, &left, &left_out);
    if (left_out.error != SQL_ERR_NONE) { *out = left_out; return; }

    struct setop_row_ctx lctx;
    lctx.out = g_setop_left_scratch; lctx.count = 0; lctx.max = CURSOR_MAX_ROWSET_ROWS;
    cursor_fetch_rows(left_out.cursor_id, CURSOR_MAX_ROWSET_ROWS, setop_row_collect_cb, &lctx);
    uint32_t left_n = lctx.count < CURSOR_MAX_ROWSET_ROWS ? lctx.count : CURSOR_MAX_ROWSET_ROWS;
    uint8_t  truncated = left_out.truncated;
    uint32_t left_column_count = left_out.column_count;
    char left_columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    for (uint32_t i = 0; i < left_column_count; i++) se_strcpy(left_columns[i], left_out.columns[i], RECORD_KEY_LEN);
    cursor_close(left_out.cursor_id);

    if (!sql_exec_depth_enter(out)) return;
    struct SqlResult right_out;
    sql_execute_tx(txn_id, caller_uid, s->set_op_rhs_text, &right_out);
    sql_exec_depth_leave();

    if (right_out.error != SQL_ERR_NONE) { *out = right_out; return; }
    if (right_out.kind != SQL_STMT_SELECT) {
        // Shouldn't happen -- parse-time validation (sql_parser.c) already
        // confirmed the captured text is a plain SELECT; fail closed if it
        // ever does (e.g. a table dropped and namesake DDL text persisted
        // some other way between parse and exec, though nothing in this
        // engine actually allows that today).
        out->error = SQL_ERR_INTERNAL;
        se_strcpy(out->error_msg, "internal error: set operator's right branch did not re-parse as a SELECT", SQL_ERR_MSG_LEN);
        cursor_close(right_out.cursor_id);
        return;
    }
    if (right_out.column_count != left_column_count) {
        out->error = SQL_ERR_COLUMN_COUNT_MISMATCH;
        se_strcpy(out->error_msg, "the two sides of a set operator must select the same number of columns", SQL_ERR_MSG_LEN);
        cursor_close(right_out.cursor_id);
        return;
    }

    struct setop_row_ctx rctx;
    rctx.out = g_setop_right_scratch; rctx.count = 0; rctx.max = CURSOR_MAX_ROWSET_ROWS;
    cursor_fetch_rows(right_out.cursor_id, CURSOR_MAX_ROWSET_ROWS, setop_row_collect_cb, &rctx);
    uint32_t right_n = rctx.count < CURSOR_MAX_ROWSET_ROWS ? rctx.count : CURSOR_MAX_ROWSET_ROWS;
    if (right_out.truncated) truncated = 1;
    cursor_close(right_out.cursor_id);

    // Merge per the operator's own membership rule. O(n^2) row/membership
    // compare, honest at the 256-row cap -- same posture as UNION's own
    // dedup the roadmap doc names explicitly.
    uint32_t merged_n = 0;
    if (s->set_op == SQL_SETOP_UNION_ALL) {
        for (uint32_t i = 0; i < left_n && merged_n < CURSOR_MAX_ROWSET_ROWS; i++) g_setop_merge_scratch[merged_n++] = g_setop_left_scratch[i];
        for (uint32_t i = 0; i < right_n && merged_n < CURSOR_MAX_ROWSET_ROWS; i++) g_setop_merge_scratch[merged_n++] = g_setop_right_scratch[i];
        if (left_n + right_n > CURSOR_MAX_ROWSET_ROWS) truncated = 1;
    } else if (s->set_op == SQL_SETOP_UNION) {
        for (uint32_t i = 0; i < left_n && merged_n < CURSOR_MAX_ROWSET_ROWS; i++) {
            int dup = 0;
            for (uint32_t j = 0; j < merged_n; j++) if (setop_rows_equal(&g_setop_left_scratch[i], &g_setop_merge_scratch[j])) { dup = 1; break; }
            if (!dup) g_setop_merge_scratch[merged_n++] = g_setop_left_scratch[i];
        }
        for (uint32_t i = 0; i < right_n && merged_n < CURSOR_MAX_ROWSET_ROWS; i++) {
            int dup = 0;
            for (uint32_t j = 0; j < merged_n; j++) if (setop_rows_equal(&g_setop_right_scratch[i], &g_setop_merge_scratch[j])) { dup = 1; break; }
            if (!dup) g_setop_merge_scratch[merged_n++] = g_setop_right_scratch[i];
        }
    } else if (s->set_op == SQL_SETOP_INTERSECT) {
        for (uint32_t i = 0; i < left_n && merged_n < CURSOR_MAX_ROWSET_ROWS; i++) {
            int found = 0;
            for (uint32_t j = 0; j < right_n; j++) if (setop_rows_equal(&g_setop_left_scratch[i], &g_setop_right_scratch[j])) { found = 1; break; }
            if (!found) continue;
            int dup = 0;
            for (uint32_t k = 0; k < merged_n; k++) if (setop_rows_equal(&g_setop_left_scratch[i], &g_setop_merge_scratch[k])) { dup = 1; break; }
            if (!dup) g_setop_merge_scratch[merged_n++] = g_setop_left_scratch[i];
        }
    } else {   // SQL_SETOP_EXCEPT
        for (uint32_t i = 0; i < left_n && merged_n < CURSOR_MAX_ROWSET_ROWS; i++) {
            int found = 0;
            for (uint32_t j = 0; j < right_n; j++) if (setop_rows_equal(&g_setop_left_scratch[i], &g_setop_right_scratch[j])) { found = 1; break; }
            if (found) continue;
            int dup = 0;
            for (uint32_t k = 0; k < merged_n; k++) if (setop_rows_equal(&g_setop_left_scratch[i], &g_setop_merge_scratch[k])) { dup = 1; break; }
            if (!dup) g_setop_merge_scratch[merged_n++] = g_setop_left_scratch[i];
        }
    }

    // ORDER BY / LIMIT apply to the merged result only -- s's own fields,
    // never either branch's (sql_parser.h's Phase 4 note on why the parser
    // only lets a trailing ORDER BY/LIMIT land here). Column NAMES follow
    // the left branch per the roadmap's own scope; v1 also compares as
    // plain text regardless of either branch's declared column type -- a
    // real, named simplification (not an oversight), since a merged
    // column's "true" type has no single answer once JOINs/aggregates on
    // either side are in play.
    if (s->has_order_by) {
        uint32_t order_col = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < left_column_count; i++) {
            if (se_streq(left_columns[i], s->order_by)) { order_col = i; break; }
        }
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in the set operator's result (column names follow the left branch)", SQL_ERR_MSG_LEN);
            return;
        }
        for (uint32_t i = 1; i < merged_n; i++) {
            struct RowValues key = g_setop_merge_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = se_strcmp(g_setop_merge_scratch[j].values[order_col], key.values[order_col]);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_setop_merge_scratch[j + 1] = g_setop_merge_scratch[j];
                j--;
            }
            g_setop_merge_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < merged_n) merged_n = s->limit;

    uint32_t cid = cursor_open_rowset(s->table_name, g_setop_merge_scratch, merged_n);

    out->error         = SQL_ERR_NONE;
    out->cursor_id     = cid;
    out->row_count     = merged_n;
    out->truncated     = truncated;
    out->column_count  = left_column_count;
    for (uint32_t i = 0; i < left_column_count; i++) se_strcpy(out->columns[i], left_columns[i], RECORD_KEY_LEN);
}

// ─── Query-Surface Roadmap Phase 5: CREATE VIEW / DROP VIEW (query side) ───
// Not banked, for the same "provably never live across a nested call"
// reasoning as g_setop_left_scratch/_right_scratch/_merge_scratch just
// above: every write into g_view_scratch happens strictly AFTER the one
// nested sql_execute_tx() call this function makes has already returned
// (sql_exec_depth_leave() runs first), and nothing after that point in
// this function recurses into dispatch_stmt() again -- predicate_eval()/
// compare_rows_by_column() are pure functions, not nested statement
// dispatch.
static struct RowValues g_view_scratch[CURSOR_MAX_ROWSET_ROWS];

// Executes a view's own stored SELECT at depth+1 (Query-Surface Roadmap
// Phase 3's banking), then applies THIS statement's own outer
// projection/WHERE/ORDER BY/LIMIT over the materialized result -- the
// "capture-and-reparse-at-exec, compose the outer clauses on top" shape
// the roadmap doc specs for views. `view_idx` is the caller's
// already-resolved views[] index (exec_select() looked it up via
// view_find_index() before calling this, so this function never repeats
// that lookup).
//
// Permission: invoker's-rights -- the view's own stored text runs under
// caller_uid (the CALLER, not views[view_idx].owner_uid, which is never
// even read here) -- see view.h's own header comment for why this is the
// simpler, safer first cut.
//
// Views of views: NOT specially detected here -- caught for free by the
// SAME sql_exec_depth_enter() call below. This function's own nested
// sql_execute_tx() runs the view's body one level deeper than whatever
// depth exec_select_view() itself was called at; if that body's own FROM
// also names a view, resolving IT recurses into exec_select_view() again,
// which needs one level deeper still -- SQL_EXEC_MAX_DEPTH=2 (depths 0
// and 1 only) refuses that second enter with SQL_ERR_NESTING_TOO_DEEP, a
// real, loud, named rejection, not a bespoke "views of views" check.
//
// Column TYPES for the outer WHERE/ORDER BY are a real, named
// simplification: the inner SqlResult only reports column NAMES
// (out->columns[]), never types (see sql_exec.h's own SqlResult comment),
// so the synthetic outer layout built below types every column as
// FIELD_TYPE_STRING -- an outer `WHERE age > 30` over a view compares
// TEXT lexicographically, not numerically. Equality/IN/LIKE/IS NULL and
// column-name validation are all unaffected (STRING comparison already IS
// se_strcmp() for those, see compare_typed() above). Exactly the same
// simplification Query-Surface Roadmap Phase 4's own set-op ORDER BY
// already accepted, for the same underlying reason: a materialized,
// re-executed result has no single "true" per-column type once JOINs/
// aggregates/set-ops/views are in play -- named here, not silently
// glossed over.
static void exec_select_view(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out, int view_idx) {
    char view_sql[VIEW_SQL_TEXT_LEN];
    se_strcpy(view_sql, views[view_idx].sql_text, VIEW_SQL_TEXT_LEN);

    if (!sql_exec_depth_enter(out)) return;
    struct SqlResult view_out;
    se_memset(&view_out, 0, sizeof(view_out));
    sql_execute_tx(txn_id, caller_uid, view_sql, &view_out);
    sql_exec_depth_leave();

    if (view_out.error != SQL_ERR_NONE) {
        // The view's own text failed at THIS exec time -- e.g. a table it
        // references was dropped after CREATE VIEW's own parse-time check
        // passed. Propagate the REAL underlying error rather than
        // wrapping it in a generic one, the same "denial looks like
        // absence" bug class this project avoids everywhere else
        // (Query-Surface Roadmap Phase 4's own set-op right-branch
        // propagation is the direct precedent) -- this is exactly the
        // roadmap doc's own "view text that no longer parses ... failing
        // loud at query time" verification case.
        *out = view_out;
        return;
    }
    if (view_out.kind != SQL_STMT_SELECT) {
        // Shouldn't happen -- CREATE VIEW's own parse-time validation
        // already confirmed this text parses as a SELECT; fail closed if
        // it ever does anyway, same posture as exec_select_set_op()'s
        // identical check.
        out->error = SQL_ERR_INTERNAL;
        se_strcpy(out->error_msg, "internal error: view definition did not re-parse as a SELECT", SQL_ERR_MSG_LEN);
        cursor_close(view_out.cursor_id);
        return;
    }

    // Reuses the same generic row-collection callback exec_select_set_op()
    // defined above (struct setop_row_ctx/setop_row_collect_cb) -- it's a
    // plain "copy fetched rows into a caller-supplied buffer" helper with
    // nothing set-op-specific about it.
    struct setop_row_ctx ctx;
    ctx.out = g_view_scratch; ctx.count = 0; ctx.max = CURSOR_MAX_ROWSET_ROWS;
    cursor_fetch_rows(view_out.cursor_id, CURSOR_MAX_ROWSET_ROWS, setop_row_collect_cb, &ctx);
    uint32_t n = ctx.count < CURSOR_MAX_ROWSET_ROWS ? ctx.count : CURSOR_MAX_ROWSET_ROWS;
    uint8_t truncated = (uint8_t)(view_out.truncated || (ctx.count > CURSOR_MAX_ROWSET_ROWS));
    uint32_t view_column_count = view_out.column_count;
    char view_columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    for (uint32_t i = 0; i < view_column_count; i++) se_strcpy(view_columns[i], view_out.columns[i], RECORD_KEY_LEN);
    cursor_close(view_out.cursor_id);

    // Synthetic ALL-STRING layout -- see this function's own header
    // comment above for why. column_offset/row_width/rows_per_page are
    // left zeroed: nothing below reads them (only column_count/
    // column_types/column_names are used by find_column_index()/
    // predicate_columns_valid()/predicate_eval()/compare_rows_by_column()
    // -- the storage-layout fields are irrelevant to a result set that's
    // never written back to a table, the same reasoning join_materialize()'s
    // own synthetic `running` layout already relies on).
    struct RowTableLayout layout;
    se_memset(&layout, 0, sizeof(layout));
    layout.column_count = view_column_count;
    for (uint32_t i = 0; i < view_column_count; i++) {
        layout.column_types[i] = FIELD_TYPE_STRING;
        se_strcpy(layout.column_names[i], view_columns[i], RECORD_KEY_LEN);
    }

    if (!s->select_all) {
        for (uint32_t i = 0; i < s->column_count; i++) {
            if (find_column_index(&layout, s->columns[i]) == 0xFFFFFFFFu) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "SELECT column not found in view", SQL_ERR_MSG_LEN);
                return;
            }
        }
    }
    uint32_t order_col = 0xFFFFFFFFu;
    if (s->has_order_by) {
        order_col = find_column_index(&layout, s->order_by);
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in view", SQL_ERR_MSG_LEN);
            return;
        }
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, &layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    // Outer WHERE: filter the already-materialized rows in place --
    // there's no real table to scan (mvcc_find_matching_rows() doesn't
    // apply here), just predicate_eval() per already-fetched row. This is
    // genuinely new to views: neither a plain SELECT nor a set operator
    // has an "outer WHERE over an already-materialized result" step
    // today.
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s->has_where && !predicate_eval(&s->where, &layout, &g_view_scratch[i])) continue;
        if (m != i) g_view_scratch[m] = g_view_scratch[i];
        m++;
    }
    n = m;

    if (s->has_order_by) {
        // Insertion sort, same shape exec_select()'s own ORDER BY uses.
        for (uint32_t i = 1; i < n; i++) {
            struct RowValues key = g_view_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(&layout, order_col, &g_view_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_view_scratch[j + 1] = g_view_scratch[j];
                j--;
            }
            g_view_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < n) n = s->limit;

    uint32_t cid = cursor_open_rowset(s->table_name, g_view_scratch, n);

    out->error        = SQL_ERR_NONE;
    out->cursor_id     = cid;
    out->row_count     = n;
    out->truncated     = truncated;
    if (s->select_all) {
        out->column_count = view_column_count;
        for (uint32_t i = 0; i < view_column_count; i++) se_strcpy(out->columns[i], view_columns[i], RECORD_KEY_LEN);
    } else {
        out->column_count = s->column_count;
        for (uint32_t i = 0; i < s->column_count; i++) se_strcpy(out->columns[i], s->columns[i], RECORD_KEY_LEN);
    }
}

// ─── Query-Surface Roadmap Phase 6: WITH <name> AS (<select...>) ───────────
// Not banked, same "provably never live across a nested call" reasoning as
// g_view_scratch above.
static struct RowValues g_cte_scratch[CURSOR_MAX_ROWSET_ROWS];

// Executes a CTE's own captured body at depth+1, then applies THIS
// statement's own outer projection/WHERE/ORDER BY/LIMIT over the
// materialized result -- structurally identical to exec_select_view()
// above (same synthetic all-STRING RowTableLayout simplification for the
// outer WHERE/ORDER BY, same real-error propagation instead of a wrapped
// generic one -- see that function's own header comment for the reasoning
// behind both, not repeated here). The one real difference: there is no
// registry lookup (no views[]/view_idx) -- s->cte_text is already this
// statement's own captured body, read directly off `s`.
//
// CTE-over-a-view depth budget: a CTE body that itself references a view
// resolves the SAME SQL_EXEC_MAX_DEPTH=2 guard exec_select_view() uses for
// views-of-views -- this function enters depth+1 to run cte_text; if
// THAT body's own FROM names a view, resolving it enters depth+1 again,
// one level deeper than the 2-deep budget allows, so it fails loud with
// SQL_ERR_NESTING_TOO_DEEP, not a bespoke check here.
//
// Non-recursive by construction, not by a dedicated check: a CTE body
// that names its OWN cte_name (`WITH x AS (SELECT * FROM x) SELECT * FROM
// x`) re-parses via an entirely fresh, independent sql_execute_tx() call
// at exec time -- that fresh parse has no memory of being "inside" a CTE
// named x, so its own `FROM x` resolves via the ordinary find_table_
// catalog_index()/view_find_index() path, finds neither (a CTE is never
// registered anywhere global -- see sql_parser.h's own Phase 6 note), and
// fails loud with a plain SQL_ERR_TABLE_NOT_FOUND rather than looping. A
// CTE referencing WITH again inside its own captured text at PARSE time
// is refused earlier still, by the SAME g_cte_validating reentrancy guard
// sql_parser.c's parse_with_select() uses.
static void exec_select_cte(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    char cte_sql[SQL_CTE_TEXT_LEN];
    se_strcpy(cte_sql, s->cte_text, SQL_CTE_TEXT_LEN);

    if (!sql_exec_depth_enter(out)) return;
    struct SqlResult cte_out;
    se_memset(&cte_out, 0, sizeof(cte_out));
    sql_execute_tx(txn_id, caller_uid, cte_sql, &cte_out);
    sql_exec_depth_leave();

    if (cte_out.error != SQL_ERR_NONE) {
        // The CTE's own body failed at THIS exec time (e.g. a table it
        // references was dropped between statements). Propagate the REAL
        // underlying error rather than wrapping it in a generic one, same
        // "denial looks like absence" avoidance as exec_select_view()'s
        // identical propagation.
        *out = cte_out;
        return;
    }
    if (cte_out.kind != SQL_STMT_SELECT) {
        // Shouldn't happen -- parse-time eager validation already
        // confirmed cte_text parses as a SELECT; fail closed if it ever
        // does anyway, same posture as exec_select_view()'s identical check.
        out->error = SQL_ERR_INTERNAL;
        se_strcpy(out->error_msg, "internal error: CTE body did not re-parse as a SELECT", SQL_ERR_MSG_LEN);
        cursor_close(cte_out.cursor_id);
        return;
    }

    // Reuses the same generic row-collection callback exec_select_set_op()/
    // exec_select_view() already use -- nothing CTE-specific about it.
    struct setop_row_ctx ctx;
    ctx.out = g_cte_scratch; ctx.count = 0; ctx.max = CURSOR_MAX_ROWSET_ROWS;
    cursor_fetch_rows(cte_out.cursor_id, CURSOR_MAX_ROWSET_ROWS, setop_row_collect_cb, &ctx);
    uint32_t n = ctx.count < CURSOR_MAX_ROWSET_ROWS ? ctx.count : CURSOR_MAX_ROWSET_ROWS;
    uint8_t truncated = (uint8_t)(cte_out.truncated || (ctx.count > CURSOR_MAX_ROWSET_ROWS));
    uint32_t cte_column_count = cte_out.column_count;
    char cte_columns[ROWSTORE_MAX_COLUMNS][RECORD_KEY_LEN];
    for (uint32_t i = 0; i < cte_column_count; i++) se_strcpy(cte_columns[i], cte_out.columns[i], RECORD_KEY_LEN);
    cursor_close(cte_out.cursor_id);

    // Synthetic ALL-STRING layout -- see exec_select_view()'s own header
    // comment for why (the inner SqlResult reports column NAMES only,
    // never types).
    struct RowTableLayout layout;
    se_memset(&layout, 0, sizeof(layout));
    layout.column_count = cte_column_count;
    for (uint32_t i = 0; i < cte_column_count; i++) {
        layout.column_types[i] = FIELD_TYPE_STRING;
        se_strcpy(layout.column_names[i], cte_columns[i], RECORD_KEY_LEN);
    }

    if (!s->select_all) {
        for (uint32_t i = 0; i < s->column_count; i++) {
            if (find_column_index(&layout, s->columns[i]) == 0xFFFFFFFFu) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "SELECT column not found in CTE", SQL_ERR_MSG_LEN);
                return;
            }
        }
    }
    uint32_t order_col = 0xFFFFFFFFu;
    if (s->has_order_by) {
        order_col = find_column_index(&layout, s->order_by);
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in CTE", SQL_ERR_MSG_LEN);
            return;
        }
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, &layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    // Outer WHERE: filter the already-materialized rows in place, same
    // shape as exec_select_view()'s identical step.
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s->has_where && !predicate_eval(&s->where, &layout, &g_cte_scratch[i])) continue;
        if (m != i) g_cte_scratch[m] = g_cte_scratch[i];
        m++;
    }
    n = m;

    if (s->has_order_by) {
        // Insertion sort, same shape exec_select()/exec_select_view() use.
        for (uint32_t i = 1; i < n; i++) {
            struct RowValues key = g_cte_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(&layout, order_col, &g_cte_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_cte_scratch[j + 1] = g_cte_scratch[j];
                j--;
            }
            g_cte_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < n) n = s->limit;

    uint32_t cid = cursor_open_rowset(s->table_name, g_cte_scratch, n);

    out->error        = SQL_ERR_NONE;
    out->cursor_id    = cid;
    out->row_count    = n;
    out->truncated    = truncated;
    if (s->select_all) {
        out->column_count = cte_column_count;
        for (uint32_t i = 0; i < cte_column_count; i++) se_strcpy(out->columns[i], cte_columns[i], RECORD_KEY_LEN);
    } else {
        out->column_count = s->column_count;
        for (uint32_t i = 0; i < s->column_count; i++) se_strcpy(out->columns[i], s->columns[i], RECORD_KEY_LEN);
    }
}

// ─── Phase 20/Phase 2 (SQL Feature-Parity Roadmap): N-way JOIN + aliasing +
// LEFT JOIN -- see sql_exec.h's header comment for the full design writeup.
static void build_qualified_name(char* out, uint32_t max, const char* table, const char* col) {
    uint32_t i = 0;
    for (; table[i] && i < max - 1; i++) out[i] = table[i];
    if (i < max - 1) out[i++] = '.';
    for (uint32_t j = 0; col[j] && i < max - 1; j++) out[i++] = col[j];
    out[i] = '\0';
}

// A table's qualifying identity throughout a statement: its alias if AS
// gave it one, else its own real name -- Phase 20's original "qualifier
// must be the table's own name" rule, generalized by one word.
static const char* join_display_name(const char* table, const char* alias) {
    return alias[0] ? alias : table;
}

// Query-Surface Roadmap Phase 2: an unmatched side's columns are now
// padded with REAL Phase-4 NULLs (null_mask bits + empty text), replacing
// the original per-type sentinels ("0"/"false"/"") that predated real
// NULL and were indistinguishable from genuine values -- the correction
// that makes IS NULL find padded rows, which FULL OUTER's whole purpose
// (surfacing the unmatched) depends on. A deliberate behavior change to
// existing LEFT JOIN output, named plainly: padded columns stop
// masquerading as zeros/empty strings.
static void fill_join_null_pad(struct RowValues* row, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        row->values[start + i][0] = '\0';
        row->null_mask |= (1u << (start + i));
    }
}

// The ON clause's temporary single-comparison probe predicate (built once
// per outer row, per JOIN step) is a struct Predicate -- several KB (32
// nodes x a 256-byte literal each), same sizing concern as g_stmt_scratch
// above. Reused/reinitialized per call via predicate_init(), safe since
// execution is single-threaded/non-reentrant throughout this file already
// (see the notes above).
// Query-Surface Roadmap Phase 3: banked, same reasoning as g_select_scratch
// above.
static struct Predicate g_join_probe_pred_bank[SQL_EXEC_MAX_DEPTH];
#define g_join_probe_pred (g_join_probe_pred_bank[g_exec_depth])

// Phase 2: the chain's second scratch buffer -- ping-pongs against
// g_select_scratch across JOIN steps (step 0 reads the FROM table's rows
// out of g_select_scratch and writes into this one, step 1 reads this one
// and writes back into g_select_scratch, and so on), so no step ever reads
// and writes the same buffer at once. Static for the same reason every
// other per-query scratch buffer in this file is (see the notes above).
// Query-Surface Roadmap Phase 3: banked, same reasoning as g_select_scratch
// above.
static struct RowValues g_join_scratch_b_bank[SQL_EXEC_MAX_DEPTH][CURSOR_MAX_ROWSET_ROWS];
#define g_join_scratch_b (g_join_scratch_b_bank[g_exec_depth])

struct join_from_collect_ctx {
    struct RowValues* out;
    uint32_t           count;   // total rows scanned so far (may exceed CURSOR_MAX_ROWSET_ROWS)
    uint32_t           max;
};
static void join_from_collect_cb(struct MvccRowId id, const struct RowValues* values, void* ctxp) {
    (void)id;
    struct join_from_collect_ctx* ctx = (struct join_from_collect_ctx*)ctxp;
    if (ctx->count < ctx->max) ctx->out[ctx->count] = *values;
    ctx->count++;
}

// Query-Surface Roadmap Phase 2: copies src's first src_count columns into
// dst starting at dst_start, carrying each source column's real null_mask
// bit along with its text -- the piece the original sentinel-based padding
// never needed, since sentinels made every combined column "non-NULL" by
// construction. Without this, a genuinely-NULL source column (e.g. a
// nullable field that was never a join key) would silently read back as
// non-NULL empty text in the combined row.
static void copy_row_segment(struct RowValues* dst, uint32_t dst_start, const struct RowValues* src, uint32_t src_count) {
    for (uint32_t c = 0; c < src_count; c++) {
        se_strcpy(dst->values[dst_start + c], src->values[c], RECORD_VAL_LEN);
        if (src->null_mask & (1u << c)) dst->null_mask |= (1u << (dst_start + c));
    }
}

// Query-Surface Roadmap Phase 2: RIGHT/FULL OUTER's anti-pass needs to know
// which of the newly-joined table's rows were matched by ANY outer row
// during the main nested-loop probe below, so it can emit the rest with the
// accumulated (outer) side NULL-padded. This is a matched-set rather than a
// literal operand swap: `cur` is a synthetic in-memory row set (not a real
// table) once a step is past the first JOIN, so it can't be probed via
// mvcc_find_matching_rows the way jc->table can -- scanning jc->table once
// more and checking membership here is the cheapest way to get the same
// "right side preserved" result with the existing per-step machinery.
static int join_id_matched(const struct MvccRowId* ids, uint32_t n, struct MvccRowId id) {
    for (uint32_t i = 0; i < n; i++) if (ids[i].logical_id == id.logical_id) return 1;
    return 0;
}

struct join_right_anti_ctx {
    struct RowValues*         out;
    uint32_t*                 n_io;
    uint32_t                  max;
    uint32_t                  running_col_count;
    uint32_t                  new_col_count;
    const struct MvccRowId*   matched_ids;
    uint32_t                  matched_count;
    uint8_t*                  truncated;
};
static void join_right_anti_cb(struct MvccRowId id, const struct RowValues* values, void* ctxp) {
    struct join_right_anti_ctx* ctx = (struct join_right_anti_ctx*)ctxp;
    if (join_id_matched(ctx->matched_ids, ctx->matched_count, id)) return;
    struct RowValues combined;
    se_memset(&combined, 0, sizeof(combined));
    combined.count = ctx->running_col_count + ctx->new_col_count;
    fill_join_null_pad(&combined, 0, ctx->running_col_count);
    copy_row_segment(&combined, ctx->running_col_count, values, ctx->new_col_count);
    if (*ctx->n_io < ctx->max) ctx->out[*ctx->n_io] = combined;
    else *ctx->truncated = 1;
    (*ctx->n_io)++;
}

// Query-Surface Roadmap Phase 2: scratch set of the newly-joined table's
// row ids matched by at least one outer row this step -- static for the
// same single-threaded/non-reentrant reason every other per-query scratch
// buffer in this file is (see the notes above), reset (via matched_count)
// at the top of each RIGHT/FULL step rather than re-declared.
static struct MvccRowId g_join_matched_ids[CURSOR_MAX_ROWSET_ROWS];

// Query-Surface Roadmap Phase 1: the join chain's materialization stage,
// factored out of exec_select_join() so exec_select_group() can feed its
// aggregate bucketing from a JOINed row source too -- the refactor that
// retires SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED. Runs the FROM scan + every
// nested-loop JOIN step, leaving the combined rows in g_select_scratch
// [0..*n_out) under the fully qualified *running_out layout. Returns 0 on
// success; on failure fills out->error/error_msg and returns 1. WHERE is
// deliberately NOT applied here -- both callers apply it against the
// combined layout afterward, preserving the established "WHERE applies
// once, at the very end" rule.
static int join_materialize(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s,
                            struct SqlResult* out, struct RowTableLayout* running_out,
                            uint32_t* n_out, uint8_t* truncated_out) {
    int tidx0 = find_table_catalog_index(s->table_name);
    if (tidx0 < 0) {
        // Query-Surface Roadmap Phase 5: a view name referenced as the
        // FROM source of a JOIN chain is a real, named v1 rejection (see
        // view.h's own header comment) -- distinct wording from plain
        // "table not found" so the caller knows exactly why, matching
        // this file's established convention for every other named scope
        // cut (SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED etc.).
        if (view_find_index(s->table_name) >= 0) {
            out->error = SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED;
            se_strcpy(out->error_msg, "a view cannot be the FROM source of a JOIN (v1 scope cut)", SQL_ERR_MSG_LEN);
            return 1;
        }
        out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "FROM table not found", SQL_ERR_MSG_LEN); return 1;
    }
    const struct RowTableLayout* layout0 = &table_headers[tidx0].layout;
    const char* disp0 = join_display_name(s->table_name, s->table_alias);

    // Running combined layout, grown by one table's worth of qualified
    // columns per JOIN step -- starts as just the FROM table's own columns,
    // qualified by its display name (alias if given, else its real name).
    struct RowTableLayout running;
    se_memset(&running, 0, sizeof(running));
    running.column_count = layout0->column_count;
    for (uint32_t c = 0; c < layout0->column_count; c++) {
        build_qualified_name(running.column_names[c], RECORD_KEY_LEN, disp0, layout0->column_names[c]);
        running.column_types[c] = layout0->column_types[c];
    }

    // Tracks each table's display name -> real (unqualified) layout, in
    // chain order, so an ON clause's "outer" side (referencing some earlier
    // table) can be validated in the same TWO separate steps Phase 20
    // originally used for its fixed pair: first "does this qualifier name a
    // table in the chain at all" (SQL_ERR_JOIN_INVALID if not), then "does
    // that table actually have this column" (SQL_ERR_COLUMN_NOT_FOUND if
    // not). Collapsing these into a single find_column_index() against
    // `running`'s own flat qualified namespace would misreport a
    // wrong-column-name typo as an unresolved-qualifier error instead -- a
    // real distinction worth keeping, caught by the Phase 20 regression
    // test still needing to pass unchanged after this generalization.
    struct join_chain_entry { const char* display; const struct RowTableLayout* layout; };
    struct join_chain_entry chain[SQL_MAX_JOINS + 1];
    uint32_t chain_count = 1;
    chain[0].display = disp0;
    chain[0].layout  = layout0;

    // Step 0: materialize the FROM table's own rows into buffer A
    // (g_select_scratch) via a plain snapshot-consistent scan -- no WHERE
    // pushdown here either, matching every JOIN step's own "WHERE applies
    // once, at the very end" rule (see sql_exec.h).
    struct RowValues* cur = g_select_scratch;
    struct RowValues* nxt = g_join_scratch_b;
    struct join_from_collect_ctx fc;
    fc.out = cur; fc.count = 0; fc.max = CURSOR_MAX_ROWSET_ROWS;
    mvcc_table_scan(txn_id, caller_uid, s->table_name, join_from_collect_cb, &fc);
    uint32_t cur_n = fc.count < CURSOR_MAX_ROWSET_ROWS ? fc.count : CURSOR_MAX_ROWSET_ROWS;
    uint8_t truncated = (fc.count > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;

    for (uint32_t ji = 0; ji < s->join_count; ji++) {
        const struct SqlJoinClause* jc = &s->joins[ji];

        int tidx = find_table_catalog_index(jc->table);
        if (tidx < 0) {
            // Query-Surface Roadmap Phase 6: a CTE named on the JOIN side
            // is the same v1 rejection as a view on the JOIN side, but
            // only needs checking HERE, not mirrored at the FROM-source
            // check above the way the view check is: exec_select() itself
            // already rejects a CTE-matching FROM table_name combined
            // with has_join before join_materialize() is ever called (see
            // exec_select()'s own top-of-function check), so only a JOIN
            // clause's OWN table name can still reach this point unrejected.
            if (s->has_cte && se_streq(jc->table, s->cte_name)) {
                out->error = SQL_ERR_CTE_SCOPE_UNSUPPORTED;
                se_strcpy(out->error_msg, "a CTE cannot be JOIN'd (v1 scope cut)", SQL_ERR_MSG_LEN);
                return 1;
            }
            // Same v1 rejection as the FROM-source check above, for a
            // view named on the JOIN side instead.
            if (view_find_index(jc->table) >= 0) {
                out->error = SQL_ERR_VIEW_IN_JOIN_UNSUPPORTED;
                se_strcpy(out->error_msg, "a view cannot be JOIN'd (v1 scope cut)", SQL_ERR_MSG_LEN);
                return 1;
            }
            out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "joined table not found", SQL_ERR_MSG_LEN); return 1;
        }
        const struct RowTableLayout* layout_new = &table_headers[tidx].layout;
        const char* disp_new = join_display_name(jc->table, jc->alias);

        if (running.column_count + layout_new->column_count > ROWSTORE_MAX_COLUMNS) {
            out->error = SQL_ERR_JOIN_TOO_WIDE;
            se_strcpy(out->error_msg, "joined tables have too many combined columns", SQL_ERR_MSG_LEN);
            return 1;
        }

        // Resolve the ON clause's two "qualifier.column" halves: one side
        // must be this NEW table's own display name (its own column), the
        // other side must resolve against `running` -- the accumulated set
        // of every table already earlier in the chain. This generalizes
        // Phase 20's original "matches exactly one of the two known table
        // names" check from a fixed pair to however many display names
        // have accumulated so far, with zero new resolution machinery:
        // `running`'s own column_names[] already IS that accumulated
        // qualifier set.
        int new_is_left;
        if (se_streq(jc->on_left_qualifier, disp_new)) {
            new_is_left = 1;
        } else if (se_streq(jc->on_right_qualifier, disp_new)) {
            new_is_left = 0;
        } else {
            out->error = SQL_ERR_JOIN_INVALID;
            se_strcpy(out->error_msg, "ON clause does not reference the newly joined table (use its alias if it has one)", SQL_ERR_MSG_LEN);
            return 1;
        }
        const char* new_col_name    = new_is_left ? jc->on_left_col : jc->on_right_col;
        const char* outer_qualifier = new_is_left ? jc->on_right_qualifier : jc->on_left_qualifier;
        const char* outer_col_name  = new_is_left ? jc->on_right_col : jc->on_left_col;

        uint32_t new_col = find_column_index(layout_new, new_col_name);
        if (new_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ON clause column not found in the joined table", SQL_ERR_MSG_LEN);
            return 1;
        }

        // Two-phase check on the outer side, matching the new side's own
        // shape above: first find WHICH earlier table outer_qualifier
        // names (SQL_ERR_JOIN_INVALID if none), then check that table's
        // OWN real layout for outer_col_name (SQL_ERR_COLUMN_NOT_FOUND if
        // absent) -- see the chain[] comment above for why this can't just
        // be one find_column_index() call against `running`.
        const struct RowTableLayout* outer_layout = 0;
        for (uint32_t ct = 0; ct < chain_count; ct++) {
            if (se_streq(chain[ct].display, outer_qualifier)) { outer_layout = chain[ct].layout; break; }
        }
        if (!outer_layout) {
            out->error = SQL_ERR_JOIN_INVALID;
            se_strcpy(out->error_msg, "ON clause's other side does not resolve to a table already in the FROM/JOIN chain", SQL_ERR_MSG_LEN);
            return 1;
        }
        if (find_column_index(outer_layout, outer_col_name) == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ON clause column not found in the corresponding table", SQL_ERR_MSG_LEN);
            return 1;
        }
        char outer_qualified[RECORD_KEY_LEN];
        build_qualified_name(outer_qualified, RECORD_KEY_LEN, outer_qualifier, outer_col_name);
        uint32_t outer_col = find_column_index(&running, outer_qualified);
        // outer_col is now guaranteed found: outer_qualifier matched a known
        // chain table above and outer_col_name exists in that table's real
        // layout, so running's qualified name for it must be present too.

        // Build this step's new running layout up front: every column
        // `running` already had, then the new table's own columns
        // qualified by its display name -- same "query-time-only
        // construct, never persisted or paged" property every layout in
        // this file has (see the Phase 20 note above).
        struct RowTableLayout next_layout;
        se_memset(&next_layout, 0, sizeof(next_layout));
        next_layout.column_count = running.column_count + layout_new->column_count;
        for (uint32_t c = 0; c < running.column_count; c++) {
            se_strcpy(next_layout.column_names[c], running.column_names[c], RECORD_KEY_LEN);
            next_layout.column_types[c] = running.column_types[c];
        }
        for (uint32_t c = 0; c < layout_new->column_count; c++) {
            build_qualified_name(next_layout.column_names[running.column_count + c], RECORD_KEY_LEN, disp_new, layout_new->column_names[c]);
            next_layout.column_types[running.column_count + c] = layout_new->column_types[c];
        }

        // Nested-loop probe: for every row accumulated so far, find the new
        // table's matching row(s) via the SAME mvcc_find_matching_rows()
        // every other path uses (Phase 22 note: no index-assisted short cut
        // survives MVCC routing here either -- see sql_exec.h). A LEFT JOIN
        // with zero matches still emits the outer row once, right side
        // sentinel-filled; an INNER JOIN with zero matches drops it, same
        // as Phase 20's original behavior.
        // Query-Surface Roadmap Phase 2: RIGHT/FULL need every unmatched row
        // of jc->table preserved (accumulated side NULL-padded), which the
        // main outer-driven loop below can't produce on its own -- it only
        // ever sees jc->table rows that DID match something. need_right_pass
        // records which jc->table rows matched as the loop runs, then an
        // anti-pass after the loop emits everything that never did.
        uint8_t need_right_pass = (jc->type == SQL_JOIN_RIGHT || jc->type == SQL_JOIN_FULL);
        uint32_t matched_count = 0;

        uint32_t next_n = 0;
        for (uint32_t r = 0; r < cur_n; r++) {
            struct MvccRowId matches[CURSOR_MAX_ROWSET_ROWS];
            uint32_t take_m = 0;

            // Standard SQL NULL semantics: a NULL join key never equals
            // anything, not even another NULL -- an outer row whose join
            // column is genuinely NULL never probe-matches, same as if the
            // probe ran and found zero rows. Skipping the probe entirely
            // also avoids querying jc->table with a meaningless empty-text
            // literal for what NULL's placeholder value[] content happens
            // to be.
            uint8_t outer_key_is_null = (cur[r].null_mask & (1u << outer_col)) != 0;
            if (!outer_key_is_null) {
                predicate_init(&g_join_probe_pred);
                uint32_t probe_root = predicate_add_comparison(&g_join_probe_pred,
                    layout_new->column_names[new_col], PRED_OP_EQ, cur[r].values[outer_col]);
                if (probe_root != PREDICATE_INVALID_NODE) {   // shouldn't happen (one fresh node), fail closed if it ever does
                    g_join_probe_pred.root = probe_root;
                    uint32_t total_m = mvcc_find_matching_rows(txn_id, caller_uid, jc->table,
                                                                &g_join_probe_pred, layout_new, matches, CURSOR_MAX_ROWSET_ROWS);
                    take_m = total_m < CURSOR_MAX_ROWSET_ROWS ? total_m : CURSOR_MAX_ROWSET_ROWS;
                }
            }

            if (need_right_pass) {
                for (uint32_t m = 0; m < take_m; m++) {
                    if (matched_count < CURSOR_MAX_ROWSET_ROWS && !join_id_matched(g_join_matched_ids, matched_count, matches[m]))
                        g_join_matched_ids[matched_count++] = matches[m];
                }
            }

            if (take_m == 0) {
                // LEFT and FULL both preserve an unmatched outer row (right
                // side NULL-padded); RIGHT drops it here -- its unmatched
                // rows come from jc->table instead, via the anti-pass below.
                if (jc->type == SQL_JOIN_LEFT || jc->type == SQL_JOIN_FULL) {
                    struct RowValues combined;
                    se_memset(&combined, 0, sizeof(combined));
                    combined.count = next_layout.column_count;
                    copy_row_segment(&combined, 0, &cur[r], running.column_count);
                    fill_join_null_pad(&combined, running.column_count, layout_new->column_count);
                    if (next_n < CURSOR_MAX_ROWSET_ROWS) nxt[next_n] = combined;
                    next_n++;
                }
                continue;
            }
            for (uint32_t m = 0; m < take_m; m++) {
                struct RowValues row_new;
                if (mvcc_row_get(txn_id, caller_uid, jc->table, matches[m], &row_new) != MVCC_OK) continue;

                struct RowValues combined;
                se_memset(&combined, 0, sizeof(combined));
                combined.count = next_layout.column_count;
                copy_row_segment(&combined, 0, &cur[r], running.column_count);
                copy_row_segment(&combined, running.column_count, &row_new, layout_new->column_count);

                if (next_n < CURSOR_MAX_ROWSET_ROWS) nxt[next_n] = combined;
                next_n++;
            }
        }

        if (need_right_pass) {
            struct join_right_anti_ctx actx;
            actx.out = nxt;
            actx.n_io = &next_n;
            actx.max = CURSOR_MAX_ROWSET_ROWS;
            actx.running_col_count = running.column_count;
            actx.new_col_count = layout_new->column_count;
            actx.matched_ids = g_join_matched_ids;
            actx.matched_count = matched_count;
            actx.truncated = &truncated;
            mvcc_table_scan(txn_id, caller_uid, jc->table, join_right_anti_cb, &actx);
        }
        if (next_n > CURSOR_MAX_ROWSET_ROWS) truncated = 1;

        struct RowValues* tmp = cur; cur = nxt; nxt = tmp;   // ping-pong buffers
        cur_n = next_n < CURSOR_MAX_ROWSET_ROWS ? next_n : CURSOR_MAX_ROWSET_ROWS;
        running = next_layout;

        chain[chain_count].display = disp_new;
        chain[chain_count].layout  = layout_new;
        chain_count++;
    }

    // Every downstream step (WHERE filtering, ORDER BY's insertion sort,
    // cursor_open_rowset()) operates on g_select_scratch by this file's own
    // established convention -- copy back if the chain's final buffer
    // landed in g_join_scratch_b instead (an even vs. odd number of JOIN
    // steps).
    if (cur != g_select_scratch) {
        for (uint32_t i = 0; i < cur_n; i++) g_select_scratch[i] = cur[i];
        cur = g_select_scratch;
    }

    *running_out   = running;
    *n_out          = cur_n;
    *truncated_out = truncated;
    return 0;
}

static void exec_select_join(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    struct RowTableLayout running;
    uint32_t cur_n = 0;
    uint8_t truncated = 0;
    if (join_materialize(txn_id, caller_uid, s, out, &running, &cur_n, &truncated)) return;

    if (!s->select_all) {
        for (uint32_t i = 0; i < s->column_count; i++) {
            if (find_column_index(&running, s->columns[i]) == 0xFFFFFFFFu) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "SELECT column not found in the joined result (use table.column or alias.column)", SQL_ERR_MSG_LEN);
                return;
            }
        }
    }
    uint32_t order_col = 0xFFFFFFFFu;
    if (s->has_order_by) {
        order_col = find_column_index(&running, s->order_by);
        if (order_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "ORDER BY column not found in the joined result (use table.column or alias.column)", SQL_ERR_MSG_LEN);
            return;
        }
    }
    if (s->has_where && !predicate_columns_valid(&s->where, s->where.root, &running)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column (use table.column or alias.column)", SQL_ERR_MSG_LEN);
        return;
    }

    // WHERE filtering happens exactly once, here, against the FULLY
    // combined chain -- not per JOIN step. No predicate pushdown, the same
    // named non-goal Phase 20 already carried (see sql_exec.h).
    if (s->has_where) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < cur_n; i++) {
            if (predicate_eval(&s->where, &running, &g_select_scratch[i])) {
                if (kept != i) g_select_scratch[kept] = g_select_scratch[i];
                kept++;
            }
        }
        cur_n = kept;
    }

    if (s->has_order_by) {
        for (uint32_t i = 1; i < cur_n; i++) {
            struct RowValues key = g_select_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(&running, order_col, &g_select_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_select_scratch[j + 1] = g_select_scratch[j];
                j--;
            }
            g_select_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < cur_n) cur_n = s->limit;

    // table_name here is metadata-only (cursor.h's struct comment) -- a
    // joined cursor's rows span every table in the chain, so this just
    // records where the query started, not an authoritative single-table
    // identity.
    uint32_t cid = cursor_open_rowset(s->table_name, g_select_scratch, cur_n);

    out->error     = SQL_ERR_NONE;
    out->cursor_id = cid;
    out->row_count = cur_n;
    out->truncated = truncated;
    if (s->select_all) {
        out->column_count = running.column_count;
        for (uint32_t i = 0; i < running.column_count; i++)
            se_strcpy(out->columns[i], running.column_names[i], RECORD_KEY_LEN);
    } else {
        out->column_count = s->column_count;
        for (uint32_t i = 0; i < s->column_count; i++)
            se_strcpy(out->columns[i], s->columns[i], RECORD_KEY_LEN);
    }
}

// ─── Phase 1 (SQL Feature-Parity Roadmap): GROUP BY / HAVING / aggregates ──
// Originally single-table only; Query-Surface Roadmap Phase 1 added the
// JOIN-sourced mode (see the comment inside), retiring that scope cut.
// Validates everything up-front against the SOURCE layout (the table's
// own, or the join's combined qualified one) and a synthetic per-group
// RESULT layout (built from the SELECT list, before any row is scanned --
// same "validate cheaply first, scan expensively second" discipline
// exec_select()/exec_select_join() already follow), then does one
// snapshot-consistent scan, bucketing matched rows by the GROUP BY column
// (or a single implicit bucket for a bare aggregate with no GROUP BY at
// all, e.g. `SELECT COUNT(*) FROM t`), then formats each surviving bucket
// (after HAVING) into a real RowValues row so ORDER BY/LIMIT/cursor_open_
// rowset() all work completely unchanged against the grouped result.
static void exec_select_group(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    // ── Query-Surface Roadmap Phase 1: the aggregate path now has TWO row
    // sources instead of rejecting JOIN outright (the old SQL_ERR_GROUP_BY_
    // JOIN_UNSUPPORTED, retired here). Single-table mode keeps the exact
    // original shape: validate against the table's own layout, then one
    // WHERE-pushed mvcc scan feeds the bucketing. Join mode first
    // materializes the combined rows via join_materialize() (the same
    // pipeline exec_select_join() itself runs, factored out), applies WHERE
    // against the combined qualified layout afterward (the join path's own
    // "WHERE once, at the very end" rule), and then the IDENTICAL bucketing
    // /HAVING/format/ORDER BY code runs against those rows -- one aggregate
    // implementation, two sources, not a second copy. ──────────────────────
    struct RowTableLayout join_layout;
    const struct RowTableLayout* layout;
    uint32_t src_n = 0;
    uint8_t src_truncated = 0;
    const int from_join = s->has_join ? 1 : 0;

    if (from_join) {
        if (join_materialize(txn_id, caller_uid, s, out, &join_layout, &src_n, &src_truncated)) return;
        layout = &join_layout;
        // WHERE: validated and applied against the COMBINED rows, before
        // bucketing -- same semantics as the single-table path's pushed-
        // down WHERE (rows are filtered before aggregation either way).
        if (s->has_where) {
            if (!predicate_columns_valid(&s->where, s->where.root, layout)) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "WHERE clause references an unknown column (use table.column or alias.column)", SQL_ERR_MSG_LEN);
                return;
            }
            uint32_t kept = 0;
            for (uint32_t i = 0; i < src_n; i++) {
                if (predicate_eval(&s->where, layout, &g_select_scratch[i])) {
                    if (kept != i) g_select_scratch[kept] = g_select_scratch[i];
                    kept++;
                }
            }
            src_n = kept;
        }
    } else {
        int tidx = find_table_catalog_index(s->table_name);
        if (tidx < 0) { out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return; }
        layout = &table_headers[tidx].layout;
    }

    uint32_t group_col = 0xFFFFFFFFu;
    if (s->has_group_by) {
        group_col = find_column_index(layout, s->group_by);
        if (group_col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, from_join
                      ? "GROUP BY column not found in the joined result (use table.column or alias.column)"
                      : "GROUP BY column not found in table", SQL_ERR_MSG_LEN);
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
            se_strcpy(out->error_msg, from_join
                      ? "aggregate function argument column not found in the joined result (use table.column or alias.column)"
                      : "aggregate function argument column not found in table", SQL_ERR_MSG_LEN);
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
    // Join mode already validated AND applied WHERE against the combined
    // rows up top -- only the single-table path validates + pushes it into
    // the scan here.
    if (!from_join && s->has_where && !predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    // ── Row source + bucket. Single-table: WHERE-filtered snapshot scan
    // (same mvcc_find_matching_rows() every other exec_* uses). Join: the
    // combined rows already materialized (and WHERE-filtered) in
    // g_select_scratch[0..src_n) by the block up top. ─────────────────────
    struct MvccRowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t n;
    uint8_t row_truncated;
    if (from_join) {
        n = src_n;
        row_truncated = src_truncated;
    } else {
        uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, s->table_name,
                                                 s->has_where ? &s->where : NULL, layout,
                                                 ids, CURSOR_MAX_ROWSET_ROWS);
        n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;
        row_truncated = (total > CURSOR_MAX_ROWSET_ROWS) ? 1 : 0;
    }

    for (uint32_t gi = 0; gi < SQL_MAX_GROUPS; gi++) g_agg_buckets[gi].active = 0;
    uint32_t group_count = 0;
    uint8_t group_overflow = 0;

    // Join mode reads source rows straight out of g_select_scratch --
    // safe even though the format stage below ALSO writes g_select_scratch,
    // because bucketing fully completes (all state accumulated into
    // g_agg_buckets[]) before the first formatted group row is written.
    for (uint32_t i = 0; i < n; i++) {
        struct RowValues rv;
        if (from_join) {
            rv = g_select_scratch[i];
        } else if (mvcc_row_get(txn_id, caller_uid, s->table_name, ids[i], &rv) != MVCC_OK) {
            continue;
        }

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

// Phase 6 (SQL Feature-Parity Roadmap): builds one row's RowValues from a
// single VALUES tuple's raw text/is_null arrays (shared col_idx[]/ccount
// resolved once by the caller, since every tuple in one INSERT statement
// uses the SAME column list). Any table column NOT present in col_idx[]
// (partial-column INSERT) is left NULL -- values is zero-initialized and
// then only named columns are overwritten, so an unseen column's null_mask
// bit is set explicitly here rather than relying on zero-init alone, since
// a real "no value provided" column must be indistinguishable from an
// explicit `NULL` literal (both are the real Phase 4 null_mask bit).
static void build_insert_row_values(const struct RowTableLayout* layout, const uint32_t* col_idx, uint32_t ccount,
                                    const char values_text[][RECORD_VAL_LEN], const uint8_t* is_null,
                                    struct RowValues* out_values) {
    se_memset(out_values, 0, sizeof(*out_values));
    out_values->count = layout->column_count;
    uint8_t seen[ROWSTORE_MAX_COLUMNS];
    se_memset(seen, 0, sizeof(seen));

    for (uint32_t i = 0; i < ccount; i++) {
        uint32_t col = col_idx[i];
        // Phase 4 (SQL Feature-Parity Roadmap): a NULL value sets the real
        // null_mask bit instead of storing empty text -- see rowstore.h's
        // Phase 4 note.
        if (is_null[i]) {
            out_values->null_mask |= (uint16_t)(1u << col);
            out_values->values[col][0] = '\0';
        } else {
            se_strcpy(out_values->values[col], values_text[i], RECORD_VAL_LEN);
        }
        seen[col] = 1;
    }
    // Phase 6 (SQL Feature-Parity Roadmap): any table column not named in
    // this INSERT's column list is a real NULL, not zero-filled/corrupted --
    // see sql_parser.h's Phase 6 note on why this needed no new parser field.
    for (uint32_t c = 0; c < layout->column_count; c++) {
        if (!seen[c]) out_values->null_mask |= (uint16_t)(1u << c);
    }
}

static void exec_insert(uint64_t txn_id, uint32_t caller_uid, const struct SqlInsertStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) {
        // Query-Surface Roadmap Phase 5: INSERT/UPDATE/DELETE through a
        // view is a real, named v1 rejection (views are read-only) -- see
        // view.h's own header comment.
        if (view_find_index(s->table_name) >= 0) {
            out->error = SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED;
            se_strcpy(out->error_msg, "cannot INSERT into a view (views are read-only in v1)", SQL_ERR_MSG_LEN);
            return;
        }
        out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return;
    }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;

    // Phase 6 (SQL Feature-Parity Roadmap): the column list may now name
    // FEWER columns than the table has (partial-column INSERT) -- it may
    // never name MORE (that's always a real error, matching every column
    // count check elsewhere in this codebase). Resolved once, shared by
    // every VALUES tuple in this statement.
    if (s->count > layout->column_count) {
        out->error = SQL_ERR_COLUMN_COUNT_MISMATCH;
        se_strcpy(out->error_msg, "INSERT names more columns than the table has", SQL_ERR_MSG_LEN);
        return;
    }
    uint32_t col_idx[ROWSTORE_MAX_COLUMNS];
    for (uint32_t i = 0; i < s->count; i++) {
        uint32_t col = find_column_index(layout, s->columns[i]);
        if (col == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "INSERT column not found in table", SQL_ERR_MSG_LEN);
            return;
        }
        col_idx[i] = col;
    }

    // Phase 6 (SQL Feature-Parity Roadmap): row 0 plus any extra_values[]
    // tuples (multi-row INSERT) -- looped against the SAME already-open
    // txn_id, so a mid-loop failure lets sql_execute()'s existing
    // begin/commit/rollback wrapping (Phase 22) roll back every row this
    // statement already inserted, for free -- see sql_parser.h's Phase 6
    // note on why no new transaction plumbing was needed here.
    uint32_t total_rows = 1 + s->extra_row_count;
    uint32_t inserted    = 0;
    struct MvccRowId last_id;
    se_memset(&last_id, 0, sizeof(last_id));

    for (uint32_t r = 0; r < total_rows; r++) {
        struct RowValues values;
        if (r == 0) {
            build_insert_row_values(layout, col_idx, s->count, s->values, s->is_null, &values);
        } else {
            build_insert_row_values(layout, col_idx, s->count, s->extra_values[r - 1], s->extra_is_null[r - 1], &values);
        }

        struct MvccRowId new_id;
        MvccError rc = mvcc_row_insert(txn_id, caller_uid, s->table_name, &values, &new_id);
        if (rc != MVCC_OK) {
            out->error = map_mvcc_err(rc);
            se_strcpy(out->error_msg, out->error == SQL_ERR_CONSTRAINT_VIOLATION
                      ? "INSERT violates a UNIQUE, NOT NULL, RANGE, or REFERENCE constraint"
                      : "INSERT rejected by the row store", SQL_ERR_MSG_LEN);
            out->affected_rows = inserted;
            return;
        }
        inserted++;
        last_id = new_id;
    }

    out->error         = SQL_ERR_NONE;
    out->affected_rows = inserted;
    out->inserted_id   = last_id;   // last row's id -- see sql_exec.h's Phase 6 note on multi-row inserted_id semantics
}

static void exec_update(uint64_t txn_id, uint32_t caller_uid, const struct SqlUpdateStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) {
        // Query-Surface Roadmap Phase 5: same v1 rejection as exec_insert().
        if (view_find_index(s->table_name) >= 0) {
            out->error = SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED;
            se_strcpy(out->error_msg, "cannot UPDATE a view (views are read-only in v1)", SQL_ERR_MSG_LEN);
            return;
        }
        out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return;
    }
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
    // Phase 3 (SQL Feature-Parity Roadmap): validate every arithmetic SET
    // value's column operand(s) up front -- same "validate cheaply first,
    // scan expensively second" discipline every other exec_* function in
    // this file already follows for WHERE/ORDER BY/SELECT-list columns.
    for (uint32_t i = 0; i < s->set_count; i++) {
        if (!s->set_is_arith[i]) continue;
        if (s->set_arith_op1[i].is_column && find_column_index(layout, s->set_arith_op1[i].text) == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "SET arithmetic expression references an unknown column", SQL_ERR_MSG_LEN);
            return;
        }
        if (s->set_arith_op[i] != PRED_ARITH_NONE && s->set_arith_op2[i].is_column &&
            find_column_index(layout, s->set_arith_op2[i].text) == 0xFFFFFFFFu) {
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "SET arithmetic expression references an unknown column", SQL_ERR_MSG_LEN);
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

        // Phase 3 (SQL Feature-Parity Roadmap): compute every SET value into
        // a scratch array FIRST, from the pristine pre-UPDATE `rv` snapshot,
        // before applying ANY of them. This matters when a statement has
        // multiple SET assignments and one arithmetic expression references
        // a column another assignment is also setting (e.g. `SET a = a + 1,
        // b = a`) -- standard SQL semantics require `b` to see `a`'s value
        // as it was BEFORE this statement, not an implementation-order-
        // dependent intermediate value. Evaluating against `rv` here (never
        // against a partially-mutated copy) guarantees that.
        char new_values[ROWSTORE_MAX_COLUMNS][RECORD_VAL_LEN];
        int  bad_arith = 0;
        for (uint32_t j = 0; j < s->set_count; j++) {
            if (!s->set_is_arith[j]) {
                se_strcpy(new_values[j], s->set_values[j], RECORD_VAL_LEN);
                continue;
            }
            double result;
            if (predicate_eval_arith(layout, &rv, s->set_arith_op1[j], s->set_arith_op[j],
                                      s->set_arith_op2[j], &result) != 0) {
                bad_arith = 1;
                break;
            }
            se_fmt_f64(new_values[j], RECORD_VAL_LEN, result);
        }
        if (bad_arith) {
            out->error = SQL_ERR_VALUE_INVALID;
            se_strcpy(out->error_msg, "SET arithmetic expression could not be evaluated for a matched row "
                      "(non-numeric operand or division by zero)", SQL_ERR_MSG_LEN);
            out->affected_rows = 0;
            return;
        }
        for (uint32_t j = 0; j < s->set_count; j++) {
            se_strcpy(rv.values[set_cols[j]], new_values[j], RECORD_VAL_LEN);
            // Phase 4 (SQL Feature-Parity Roadmap): SET col = NULL sets the
            // real null_mask bit; any OTHER SET value (literal or
            // arithmetic) clears it -- a column being freshly written a
            // real value is no longer NULL even if it was before this
            // statement.
            if (s->set_is_null[j]) rv.null_mask |= (uint16_t)(1u << set_cols[j]);
            else                   rv.null_mask &= (uint16_t)~(1u << set_cols[j]);
        }
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
    if (tidx < 0) {
        // Query-Surface Roadmap Phase 5: same v1 rejection as exec_insert().
        if (view_find_index(s->table_name) >= 0) {
            out->error = SQL_ERR_DML_THROUGH_VIEW_UNSUPPORTED;
            se_strcpy(out->error_msg, "cannot DELETE from a view (views are read-only in v1)", SQL_ERR_MSG_LEN);
            return;
        }
        out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return;
    }
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

// ── Phase 5 (SQL Feature-Parity Roadmap): DDL executor dispatch. ───────────
// None of these five take a txn_id -- DDL isn't a row-data mutation mvcc.c
// wraps, it's a catalog/schema/index/constraint change that calls straight
// into object_catalog.c/rowstore.c/row_index.c/row_constraint.c, exactly
// like the pre-existing direct-API/HTTP path already does (see rowstore.c's
// own rowstore_create_table()/rowstore_drop_table()/rowstore_add_column()
// header comments). dispatch_stmt() still requires an active txn_id before
// reaching any of these (see its own header comment) purely for uniformity
// with every other statement kind, not because DDL itself is transactional.

// CREATE TABLE chains the same three-step flow the pre-existing HTTP routes
// already expose separately (POST /api/valloc -> POST /api/schema -> POST
// /api/tables, see net/http.c's api_table_create_post()/api_schema_set_
// post() header comments) into one SQL statement, then registers any
// inline column constraints (NOT NULL/UNIQUE/REFERENCES) via row_
// constraint_add_*() in column-definition order. If a later step fails
// (schema_set, promotion, or a constraint registration), earlier steps are
// NOT rolled back -- the table may be left partially defined (e.g. valloc'd
// but not yet promoted, or promoted but missing a later inline constraint).
// This matches this codebase's existing, established behavior for the
// exact same multi-step flow over HTTP (api_schema_set_post()'s own header
// comment: "Stops at the first column that fails ... rather than silently
// applying a partial schema") -- not a new limitation introduced here, and
// named explicitly rather than silently differing between the two paths.
static void exec_create_table(uint32_t caller_uid, struct SqlCreateTableStmt* s, struct SqlResult* out) {
    if (s->column_count == 0) {
        out->error = SQL_ERR_DDL_FAILED;
        se_strcpy(out->error_msg, "CREATE TABLE requires at least one column", SQL_ERR_MSG_LEN);
        return;
    }

    // Database Namespace & Access Roadmap Phase 2: resolve the optional "IN
    // DATABASE <name>" clause to a real database_id BEFORE valloc'ing
    // anything -- same posture as REFERENCES validation (sql_parser.h's own
    // has_database/database_name comment): resolution happens at EXEC time,
    // not parse time, and a bad name must fail cleanly with nothing
    // partially created, not stamp a 0/NONE tag silently.
    uint32_t resolved_database_id = 0;
    if (s->has_database) {
        resolved_database_id = database_find_id(s->database_name);
        if (resolved_database_id == 0) {
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE TABLE: IN DATABASE names a database that does not exist", SQL_ERR_MSG_LEN);
            return;
        }
    }

    struct SLSVallocRequest vreq;
    se_memset(&vreq, 0, sizeof(vreq));
    se_strcpy(vreq.name, s->table_name, OBJECT_NAME_LEN);
    vreq.type         = OBJ_TYPE_DB_TABLE;
    vreq.size_pages   = 1;
    vreq.owner_uid    = caller_uid;
    vreq.perm_mask    = 0;   // owner_uid always has full access -- catalog_check_access()
    vreq.partition_id = 0;
    vreq.database_id  = resolved_database_id;   // 0 (NONE) unless IN DATABASE was present and resolved above
    uint64_t obj_id = sys_sls_valloc(&vreq);
    if (!obj_id) {
        out->error = SQL_ERR_DDL_FAILED;
        se_strcpy(out->error_msg, "CREATE TABLE failed: table name already exists or catalog is full", SQL_ERR_MSG_LEN);
        return;
    }

    for (uint32_t i = 0; i < s->column_count; i++) {
        struct SLSSchemaRequest sreq;
        se_strcpy(sreq.object_name, s->table_name, OBJECT_NAME_LEN);
        se_strcpy(sreq.key, s->columns[i].name, RECORD_KEY_LEN);
        sreq.type = s->columns[i].type;
        if (sys_sls_schema_set(&sreq) != 0) {
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE TABLE failed while defining a column (schema_set)", SQL_ERR_MSG_LEN);
            return;
        }
    }

    if (rowstore_create_table(s->table_name) != 0) {
        out->error = SQL_ERR_DDL_FAILED;
        se_strcpy(out->error_msg, "CREATE TABLE failed while promoting to a row-set table", SQL_ERR_MSG_LEN);
        return;
    }

    for (uint32_t i = 0; i < s->column_count; i++) {
        struct SqlColumnDef* c = &s->columns[i];
        if (c->not_null && row_constraint_add_not_null(s->table_name, c->name) != ROW_CONSTRAINT_OK) {
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE TABLE: NOT NULL constraint registration failed", SQL_ERR_MSG_LEN);
            return;
        }
        if (c->is_unique && row_constraint_add_unique(s->table_name, c->name) != ROW_CONSTRAINT_OK) {
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE TABLE: UNIQUE constraint registration failed", SQL_ERR_MSG_LEN);
            return;
        }
        if (c->has_range &&
            row_constraint_add_range(s->table_name, c->name, c->range_min, c->range_max) != ROW_CONSTRAINT_OK) {
            // Database Gap Analysis §2.7 -- registration-time validation
            // (unparseable bounds vs. the column's type, ERR_RANGE_INVALID)
            // happens inside add_range() itself, surfacing here.
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE TABLE: CHECK (BETWEEN) constraint registration failed (bounds may not parse against the column's type)", SQL_ERR_MSG_LEN);
            return;
        }
        if (c->has_reference &&
            row_constraint_add_reference_action(s->table_name, c->name, c->ref_table, c->ref_column,
                                                (RowOnDeleteAction)c->on_delete_action) != ROW_CONSTRAINT_OK) {
            // Cascading phase: carries the parsed ON DELETE action through
            // (0/absent == RESTRICT, identical to the old add_reference()).
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE TABLE: REFERENCES constraint registration failed", SQL_ERR_MSG_LEN);
            return;
        }
    }

    out->error = SQL_ERR_NONE;
}

static void exec_alter_table(uint32_t caller_uid, struct SqlAlterTableStmt* s, struct SqlResult* out) {
    // ── Database Gap Analysis §2.3: ALTER TABLE ... SET DATABASE ────────
    // A pure catalog-metadata retag -- no storage is touched, no rows
    // move. Gated through catalog_check_access() with PERM_WRITE (the
    // established per-object choke point) rather than a bespoke owner
    // check. The new tag takes effect on the next access check
    // immediately (catalog_check_access() reads database_id live), so a
    // table moved into a granted database becomes reachable to that
    // database's grantees with no further step -- and §1.6's "empty a
    // database" friction (drop tables outright) is finally closed with a
    // reassignment path.
    if (s->alter_kind == 1) {
        uint32_t new_db_id = 0;
        if (!s->database_none) {
            new_db_id = database_find_id(s->database_name);
            if (new_db_id == 0) {
                out->error = SQL_ERR_DDL_FAILED;
                se_strcpy(out->error_msg, "ALTER TABLE SET DATABASE: database not found", SQL_ERR_MSG_LEN);
                return;
            }
        }
        int tidx = -1;
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            if (object_catalog[i].active && object_catalog[i].uses_rowstore &&
                se_streq(object_catalog[i].name, s->table_name)) { tidx = (int)i; break; }
        }
        if (tidx < 0) {
            out->error = SQL_ERR_TABLE_NOT_FOUND;
            se_strcpy(out->error_msg, "ALTER TABLE SET DATABASE: table not found or not a row-set table", SQL_ERR_MSG_LEN);
            return;
        }
        if (!catalog_check_access(caller_uid, s->table_name, PERM_WRITE)) {
            out->error = SQL_ERR_PERMISSION_DENIED;
            se_strcpy(out->error_msg, "ALTER TABLE SET DATABASE: permission denied", SQL_ERR_MSG_LEN);
            return;
        }
        object_catalog[tidx].database_id = new_db_id;
        persist_catalog();   // the tag lives on the catalog entry -- catalog snapshot, not the database one
        out->error = SQL_ERR_NONE;
        return;
    }

    int rc = rowstore_add_column(caller_uid, s->table_name, s->column_name, s->column_type);
    switch (rc) {
        case 0: {
            // rowstore_add_column() gave every row a brand-new physical
            // RowId (fresh pages, new layout) -- mvcc.c's own per-row
            // version cache (mvcc_versions[]) still points at the old,
            // now-abandoned physical locations unless explicitly rebuilt
            // here. See mvcc.h's own header comment on mvcc_rebuild_
            // versions_for_table() for why this lives here (sql_exec.c)
            // rather than inside rowstore.c itself.
            int tidx = -1;
            for (uint32_t i = 0; i < object_catalog_count; i++) {
                if (object_catalog[i].active && se_streq(object_catalog[i].name, s->table_name)) { tidx = (int)i; break; }
            }
            if (tidx >= 0) mvcc_rebuild_versions_for_table(object_catalog[tidx].object_id, s->table_name);
            out->error = SQL_ERR_NONE;
            return;
        }
        case 1:
            out->error = SQL_ERR_TABLE_NOT_FOUND;
            se_strcpy(out->error_msg, "ALTER TABLE: table not found or not a row-set table", SQL_ERR_MSG_LEN);
            return;
        case 2:
            out->error = SQL_ERR_PERMISSION_DENIED;
            se_strcpy(out->error_msg, "ALTER TABLE: permission denied", SQL_ERR_MSG_LEN);
            return;
        case 3:
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "ALTER TABLE: column name already exists", SQL_ERR_MSG_LEN);
            return;
        case 4:
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "ALTER TABLE: column/schema capacity exhausted", SQL_ERR_MSG_LEN);
            return;
        case 6:
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "ALTER TABLE: page pool exhausted during migration", SQL_ERR_MSG_LEN);
            return;
        case 7:
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "ALTER TABLE: too many existing rows to migrate in one call", SQL_ERR_MSG_LEN);
            return;
        default:
            out->error = SQL_ERR_INTERNAL;
            se_strcpy(out->error_msg, "ALTER TABLE: unexpected error", SQL_ERR_MSG_LEN);
            return;
    }
}

// The real drop-one-table sequence (object_id capture -> rowstore_drop_
// table() -> mvcc_notify_table_dropped()), factored out of exec_drop_
// table() so DROP DATABASE ... CASCADE (Cascading phase, below) drops each
// child table through the exact same path -- including the Phase 8
// ghost-row fix -- rather than a second, drift-prone copy. Returns
// rowstore_drop_table()'s own rc (0 ok, 2 permission, else not-found).
static int drop_one_table_by_name(uint32_t caller_uid, const char* table_name) {
    // Capture the real object_id before rowstore_drop_table() deactivates
    // the catalog entry -- needed below to clean up mvcc.c's own per-row
    // version cache. See mvcc.h's mvcc_notify_table_dropped() header
    // comment for the full "why this matters" writeup (short version:
    // table_object_id is a deterministic hash of the table's NAME, so a
    // future CREATE TABLE reusing this name would otherwise resurrect
    // these rows as ghost data).
    uint64_t table_object_id = 0;
    int found_before_drop = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && se_streq(object_catalog[i].name, table_name)) {
            table_object_id = object_catalog[i].object_id;
            found_before_drop = 1;
            break;
        }
    }

    int rc = rowstore_drop_table(caller_uid, table_name);
    if (rc == 0 && found_before_drop) mvcc_notify_table_dropped(table_object_id);
    return rc;
}

static void exec_drop_table(uint32_t caller_uid, struct SqlDropTableStmt* s, struct SqlResult* out) {
    int rc = drop_one_table_by_name(caller_uid, s->table_name);
    if (rc == 0) {
        out->error = SQL_ERR_NONE;
        return;
    }
    if (rc == 2) {
        out->error = SQL_ERR_PERMISSION_DENIED;
        se_strcpy(out->error_msg, "DROP TABLE: permission denied", SQL_ERR_MSG_LEN);
        return;
    }
    out->error = SQL_ERR_TABLE_NOT_FOUND;
    se_strcpy(out->error_msg, "DROP TABLE: table not found or not a row-set table", SQL_ERR_MSG_LEN);
}

static void exec_create_index(uint32_t caller_uid, struct SqlCreateIndexStmt* s, struct SqlResult* out) {
    int rc = row_index_create(caller_uid, s->index_name, s->table_name, s->column_name);
    switch (rc) {
        case 0:
            out->error = SQL_ERR_NONE;
            return;
        case 1:
            out->error = SQL_ERR_TABLE_NOT_FOUND;
            se_strcpy(out->error_msg, "CREATE INDEX: table not found or not a row-set table", SQL_ERR_MSG_LEN);
            return;
        case 2:
            out->error = SQL_ERR_PERMISSION_DENIED;
            se_strcpy(out->error_msg, "CREATE INDEX: permission denied", SQL_ERR_MSG_LEN);
            return;
        case 3:
            out->error = SQL_ERR_COLUMN_NOT_FOUND;
            se_strcpy(out->error_msg, "CREATE INDEX: column not found", SQL_ERR_MSG_LEN);
            return;
        case 4:
            out->error = SQL_ERR_DDL_FAILED;
            se_strcpy(out->error_msg, "CREATE INDEX: index name already used or index capacity exhausted", SQL_ERR_MSG_LEN);
            return;
        default:
            out->error = SQL_ERR_NONE;   // row_index_create()'s own rc==5 case is a logged warning, not a failure -- index was still created
            return;
    }
}

static void exec_drop_index(uint32_t caller_uid, struct SqlDropIndexStmt* s, struct SqlResult* out) {
    int rc = row_index_drop(caller_uid, s->index_name);
    if (rc == 0) { out->error = SQL_ERR_NONE; return; }
    if (rc == 2) {
        out->error = SQL_ERR_PERMISSION_DENIED;
        se_strcpy(out->error_msg, "DROP INDEX: permission denied", SQL_ERR_MSG_LEN);
        return;
    }
    out->error = SQL_ERR_DDL_FAILED;
    se_strcpy(out->error_msg, "DROP INDEX: index not found", SQL_ERR_MSG_LEN);
}

// ─── Database Namespace & Access Roadmap Phase 2 ───────────────────────────
// Thin wrappers straight into kernel/database.c's own lifecycle -- mirrors
// exec_drop_table()/exec_drop_index()'s own shape exactly. No SQL GRANT
// exists anywhere in this codebase (roadmap doc §1.5), so these two are the
// entire SQL-reachable surface for databases; grants stay Terminal/HTTP-only
// (Phase 3, not yet built).
static void exec_create_database(uint32_t caller_uid, struct SqlCreateDatabaseStmt* s, struct SqlResult* out) {
    int rc = database_create(caller_uid, s->database_name);
    if (rc == 0) { out->error = SQL_ERR_NONE; return; }
    out->error = SQL_ERR_DDL_FAILED;
    se_strcpy(out->error_msg, "CREATE DATABASE failed: bad/empty/too-long name, duplicate name, or database table is full", SQL_ERR_MSG_LEN);
}

static void exec_drop_database(uint32_t caller_uid, struct SqlDropDatabaseStmt* s, struct SqlResult* out) {
    int rc = database_drop(caller_uid, s->database_name);

    // ── Cascading phase: DROP DATABASE ... CASCADE ──────────────────────
    // Implemented HERE, at the executor layer, as drop-the-children-then-
    // retry -- deliberately NOT by adding a cascade flag inside
    // database_drop() itself. rc==3 ("still has tables") can only be
    // returned AFTER database_drop()'s own permission gate passed (rc==2
    // fires first), so retry-after-emptying reuses that gate and the
    // emptiness check completely unchanged -- no second copy of either.
    // Each child table is dropped through drop_one_table_by_name() (the
    // exact same path a direct DROP TABLE takes, including the Phase 8
    // mvcc ghost-row fix), which enforces the caller's own per-table
    // permission: a database owner who lacks rights on some table inside
    // it gets a partial-failure error, with already-dropped siblings NOT
    // resurrected -- DDL is not transactional anywhere in this engine
    // (CREATE TABLE's own multi-step sequence has the same property),
    // named honestly rather than papered over.
    if (rc == 3 && s->cascade) {
        uint32_t db_id = database_find_id(s->database_name);
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            if (!object_catalog[i].active || object_catalog[i].database_id != db_id) continue;
            int drc = drop_one_table_by_name(caller_uid, object_catalog[i].name);
            if (drc != 0) {
                out->error = drc == 2 ? SQL_ERR_PERMISSION_DENIED : SQL_ERR_DDL_FAILED;
                se_strcpy(out->error_msg,
                          drc == 2
                              ? "DROP DATABASE CASCADE: permission denied on a table inside the database (tables dropped before this one stay dropped -- DDL is not transactional)"
                              : "DROP DATABASE CASCADE: failed dropping a table inside the database (a non-row-set object may be tagged with this database id)",
                          SQL_ERR_MSG_LEN);
                return;
            }
            // drop_one_table_by_name() deactivated this catalog slot in
            // place (object_catalog[] is a fixed array, never compacted),
            // so continuing the same index scan is safe -- no restart
            // needed, matching catalog_vfree_partition()'s own in-place
            // deactivation-during-scan pattern.
        }
        rc = database_drop(caller_uid, s->database_name);   // now-empty retry through the same gate
    }

    if (rc == 0) { out->error = SQL_ERR_NONE; return; }
    if (rc == 2) {
        out->error = SQL_ERR_PERMISSION_DENIED;
        se_strcpy(out->error_msg, "DROP DATABASE: permission denied", SQL_ERR_MSG_LEN);
        return;
    }
    if (rc == 3) {
        out->error = SQL_ERR_DDL_FAILED;
        se_strcpy(out->error_msg, "DROP DATABASE: still has one or more tables tagged with it -- drop or reassign them first, or use DROP DATABASE <name> CASCADE", SQL_ERR_MSG_LEN);
        return;
    }
    out->error = SQL_ERR_DDL_FAILED;
    se_strcpy(out->error_msg, "DROP DATABASE: database not found", SQL_ERR_MSG_LEN);
}

// ─── Query-Surface Roadmap Phase 5: CREATE VIEW / DROP VIEW (DDL side) ─────
// Thin wrappers straight into kernel/view.c's own lifecycle -- mirrors
// exec_create_database()/exec_drop_database()'s own shape exactly. The
// captured sql_text has ALREADY been validated as a parseable SELECT by
// sql_parser.c's own g_view_skip_scratch/g_view_validating guard at parse
// time (see sql_parser.h's SqlCreateViewStmt comment); view_create() below
// re-checks name/text-length/namespace-collision constraints only, not
// grammar.
static void exec_create_view(uint32_t caller_uid, struct SqlCreateViewStmt* s, struct SqlResult* out) {
    int rc = view_create(caller_uid, s->view_name, s->sql_text);
    if (rc == 0) { out->error = SQL_ERR_NONE; return; }
    out->error = SQL_ERR_DDL_FAILED;
    se_strcpy(out->error_msg, "CREATE VIEW failed: bad/empty/too-long name, a table already has this name, duplicate view name, or the view table is full", SQL_ERR_MSG_LEN);
}

static void exec_drop_view(uint32_t caller_uid, struct SqlDropViewStmt* s, struct SqlResult* out) {
    int rc = view_drop(caller_uid, s->view_name);
    if (rc == 0) { out->error = SQL_ERR_NONE; return; }
    if (rc == 2) {
        out->error = SQL_ERR_PERMISSION_DENIED;
        se_strcpy(out->error_msg, "DROP VIEW: permission denied", SQL_ERR_MSG_LEN);
        return;
    }
    out->error = SQL_ERR_DDL_FAILED;
    se_strcpy(out->error_msg, "DROP VIEW: view not found", SQL_ERR_MSG_LEN);
}

// ─── Phase 7 (SQL Feature-Parity Roadmap): non-correlated subquery
// resolution -- see predicate.h's own Phase 7 note for the full design.
// This is the piece that turns a parsed-but-unresolved `uses_subquery`
// marker node into an ordinary, already-resolved comparison node before
// predicate_eval() ever sees it for real row filtering. ────────────────────

// Dedicated EXEC-TIME-ONLY scratch for re-parsing an embedded subquery's
// raw text -- deliberately separate from g_stmt_scratch below. Reusing
// g_stmt_scratch here would be a real bug, not just untidy: g_stmt_scratch
// holds the OUTER statement's own still-in-progress parsed fields (e.g. its
// WHERE predicate, which is exactly what's being resolved right now) for
// the whole rest of dispatch_stmt()'s call -- overwriting it mid-resolution
// would corrupt the outer statement out from under itself. Also distinct
// from sql_parser.c's own g_subquery_skip_scratch (parse-time-only shape
// validation, a different translation unit, never linked to this one's
// lifetime).
static struct SqlStatement g_subquery_stmt_scratch;

// Raw-text result values extracted from one subquery's single projected
// column, up to CURSOR_MAX_ROWSET_ROWS of them (matching the cap every
// other row-materializing path in this file already uses). Static, not a
// stack local, for the same reason g_select_scratch is: this-sized a
// buffer doesn't belong on a freestanding kernel's stack.
static char g_subquery_result_text[CURSOR_MAX_ROWSET_ROWS][RECORD_VAL_LEN];

// Re-parses and executes an already parse-time-validated (single-column,
// non-JOIN, non-aggregate) embedded subquery's raw text, extracting its
// projected column's values into g_subquery_result_text[0..*out_count).
// Deliberately does NOT go through exec_select()/cursor_open_rowset(): a
// subquery result is consumed immediately and entirely by
// resolve_predicate_subqueries() below, so materializing it as a real
// cursor would burn one of only CURSOR_MAX==8 simultaneously-open-cursor
// slots for no reason (and risk the exact "silent pool exhaustion" bug
// Phase 6 hit once already) -- calling mvcc_find_matching_rows()/
// mvcc_row_get() directly, the same two calls exec_select() itself uses
// internally, gets the same rows with no cursor involved at all.
// Returns 1 on success (0 rows is a legitimate, successful result -- see
// the IN-with-empty-set and scalar-empty-comparison handling in
// resolve_predicate_subqueries()), 0 on a genuine execution error
// (out->error/error_msg already set) -- expected only for a reason that
// couldn't have been caught at parse-time validation, e.g. the subquery's
// table having been DROPped between parse time and now.
static int exec_subquery_column(uint64_t txn_id, uint32_t caller_uid, const char* raw_text,
                                 struct SqlResult* out, uint32_t* out_count) {
    char perr[SQL_ERR_MSG_LEN];
    if (sql_parse(raw_text, &g_subquery_stmt_scratch, perr, SQL_ERR_MSG_LEN) != 0 ||
        g_subquery_stmt_scratch.kind != SQL_STMT_SELECT) {
        out->error = SQL_ERR_INTERNAL;
        se_strcpy(out->error_msg, "internal error: subquery failed to re-parse at exec time", SQL_ERR_MSG_LEN);
        return 0;
    }
    struct SqlSelectStmt* sq = &g_subquery_stmt_scratch.u.select;

    int tidx = find_table_catalog_index(sq->table_name);
    if (tidx < 0) {
        out->error = SQL_ERR_TABLE_NOT_FOUND;
        se_strcpy(out->error_msg, "subquery references a table that no longer exists", SQL_ERR_MSG_LEN);
        return 0;
    }
    const struct RowTableLayout* layout = &table_headers[tidx].layout;
    uint32_t col_idx = find_column_index(layout, sq->columns[0]);
    if (col_idx == 0xFFFFFFFFu) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "subquery's selected column no longer exists in its table", SQL_ERR_MSG_LEN);
        return 0;
    }
    if (sq->has_where && !predicate_columns_valid(&sq->where, sq->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "subquery's WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return 0;
    }

    struct MvccRowId ids[CURSOR_MAX_ROWSET_ROWS];
    uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, sq->table_name,
                                             sq->has_where ? &sq->where : NULL, layout,
                                             ids, CURSOR_MAX_ROWSET_ROWS);
    uint32_t n = total < CURSOR_MAX_ROWSET_ROWS ? total : CURSOR_MAX_ROWSET_ROWS;

    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct RowValues row;
        if (mvcc_row_get(txn_id, caller_uid, sq->table_name, ids[i], &row) != MVCC_OK) continue;
        se_strcpy(g_subquery_result_text[count], row.values[col_idx], RECORD_VAL_LEN);
        count++;
    }
    *out_count = count;
    return 1;
}

// ─── Query-Surface Roadmap Phase 7: correlated-subquery textual splice ─────
// See predicate.h's own Phase 7 addendum for the full design. These two
// helpers turn "OUTER.<column>" occurrences in a subquery's raw text into
// that column's actual value from the CURRENT outer row, producing text
// that is then handed to exec_subquery_column() completely unchanged --
// one execution path serves both correlated and non-correlated subqueries.

// Formats one outer-row value for splicing into subquery text: STRING/BLOB
// values are single-quoted with embedded quotes doubled (matching this
// codebase's own string-literal lexing convention in sql_parser.c's
// lex_next()); every other type is copied unquoted, matching how a numeric/
// bool literal is normally written. Returns the number of bytes written
// (excluding the terminator).
static uint32_t format_value_for_splice(char* out, uint32_t max, SLSFieldType type, const char* value) {
    uint32_t oi = 0;
    if (type == FIELD_TYPE_STRING || type == FIELD_TYPE_BLOB) {
        if (oi < max - 1) out[oi++] = '\'';
        for (uint32_t i = 0; value[i] && oi < max - 2; i++) {
            if (value[i] == '\'' && oi < max - 2) out[oi++] = '\'';
            out[oi++] = value[i];
        }
        if (oi < max - 1) out[oi++] = '\'';
    } else {
        for (uint32_t i = 0; value[i] && oi < max - 1; i++) out[oi++] = value[i];
    }
    out[oi] = '\0';
    return oi;
}

// A plain char classifier, matching sql_parser.c's own identifier-character
// rule -- used only for the token-boundary check after a splice below (this
// file has no lexer of its own to reuse).
static int se_char_is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Copies subq_text into out_text, replacing every token-boundary-checked
// "OUTER.<ident>" occurrence with that column's value from outer_row (per
// outer_layout's own column type). Returns 1 on success; 0 with out->error/
// error_msg already set on an unknown OUTER column (SQL_ERR_COLUMN_NOT_
// FOUND -- a real, named error, not a silent empty match) or an output
// buffer overflow (SQL_ERR_INTERNAL). An "OUTER." not followed by an
// identifier is copied through unchanged -- predicate.c's own detection
// pass already confirmed a bare "OUTER." exists somewhere in this text (or
// this function would never be called), so a malformed one here just falls
// through to the re-parse below failing as an ordinary syntax error.
static int substitute_outer_refs(const char* subq_text, const struct RowTableLayout* outer_layout,
                                 const struct RowValues* outer_row, char* out_text, uint32_t out_max,
                                 struct SqlResult* out) {
    uint32_t len = se_strlen(subq_text);
    uint32_t si = 0, oi = 0;
    while (si < len) {
        int boundary_ok = (si == 0) || !se_char_is_ident(subq_text[si - 1]);
        int is_outer = boundary_ok && si + 6 <= len &&
            (subq_text[si]=='o'||subq_text[si]=='O') && (subq_text[si+1]=='u'||subq_text[si+1]=='U') &&
            (subq_text[si+2]=='t'||subq_text[si+2]=='T') && (subq_text[si+3]=='e'||subq_text[si+3]=='E') &&
            (subq_text[si+4]=='r'||subq_text[si+4]=='R') && subq_text[si+5]=='.';
        if (is_outer) {
            uint32_t j = si + 6;
            uint32_t col_start = j;
            while (j < len && se_char_is_ident(subq_text[j])) j++;
            if (j == col_start) {
                if (oi >= out_max - 1) { out->error = SQL_ERR_INTERNAL; se_strcpy(out->error_msg, "correlated subquery substitution too long", SQL_ERR_MSG_LEN); return 0; }
                out_text[oi++] = subq_text[si++];
                continue;
            }
            char col_name[RECORD_KEY_LEN];
            uint32_t clen = j - col_start; if (clen >= RECORD_KEY_LEN) clen = RECORD_KEY_LEN - 1;
            for (uint32_t k = 0; k < clen; k++) col_name[k] = subq_text[col_start + k];
            col_name[clen] = '\0';

            uint32_t cidx = find_column_index(outer_layout, col_name);
            if (cidx == 0xFFFFFFFFu) {
                out->error = SQL_ERR_COLUMN_NOT_FOUND;
                se_strcpy(out->error_msg, "correlated subquery references an unknown outer column (OUTER.<name>)", SQL_ERR_MSG_LEN);
                return 0;
            }
            char formatted[RECORD_VAL_LEN * 2 + 4];
            uint32_t flen = format_value_for_splice(formatted, sizeof(formatted), outer_layout->column_types[cidx], outer_row->values[cidx]);
            if (oi + flen >= out_max) {
                out->error = SQL_ERR_INTERNAL;
                se_strcpy(out->error_msg, "correlated subquery substitution too long", SQL_ERR_MSG_LEN);
                return 0;
            }
            for (uint32_t k = 0; k < flen; k++) out_text[oi++] = formatted[k];
            si = j;
            continue;
        }
        if (oi >= out_max - 1) { out->error = SQL_ERR_INTERNAL; se_strcpy(out->error_msg, "correlated subquery substitution too long", SQL_ERR_MSG_LEN); return 0; }
        out_text[oi++] = subq_text[si++];
    }
    out_text[oi] = '\0';
    return 1;
}

// Resolves every uses_subquery marker node in `pred` IN PLACE, before the
// caller's own row scan runs. A scalar comparison ("= (SELECT ...)", "> ...
// etc) gets its node's literal filled in directly; `IN (SELECT ...)`
// desugars into the same OR-chain-of-EQ shape the parser already builds
// for a literal IN-list, reusing predicate_add_comparison()/
// predicate_add_or() and copying the final chain's root node CONTENT back
// into the marker node's own slot index (pred->nodes[i] = pred->nodes[acc])
// so every existing AND/OR parent reference to node i stays valid with no
// new "reparent" primitive needed -- the freshly-allocated `acc` slot is
// simply abandoned, this file's usual bump-allocated-pool convention.
// Returns 1 on success (including "no subqueries present" -- the common,
// byte-for-byte-free case, and "subquery legitimately returned 0 rows"),
// 0 on a genuine resolution error (out->error/error_msg already set; the
// caller should abort the whole statement without scanning any rows).
//
// Query-Surface Roadmap Phase 7: outer_layout/outer_row are new, additive
// parameters -- NULL/NULL (the byte-for-byte pre-Phase-7 call shape) from
// dispatch_stmt()'s own top-level, once-only call site means "not running
// per outer row yet": a CORRELATED node (pred->subquery_is_correlated[...])
// is deliberately SKIPPED (left unresolved) in that case rather than
// erroring, since it genuinely cannot be resolved without a specific row --
// exec_select_correlated() below is what supplies a real outer_row, calling
// this SAME function once per candidate row against a fresh copy of the
// predicate. A non-correlated node is completely unaffected by any of
// this -- resolved exactly as before regardless of whether outer_row is
// NULL, so this refactor changes zero behavior for every pre-Phase-7 query.
static int resolve_predicate_subqueries(uint64_t txn_id, uint32_t caller_uid, struct Predicate* pred,
                                        const struct RowTableLayout* outer_layout, const struct RowValues* outer_row,
                                        struct SqlResult* out) {
    if (pred->subquery_count == 0) return 1;
    for (uint32_t i = 0; i < pred->node_count; i++) {
        struct PredicateNode* n = &pred->nodes[i];
        if (n->kind != PRED_NODE_COMPARISON || !n->uses_subquery) continue;
        if (n->subquery_index >= pred->subquery_count) {
            out->error = SQL_ERR_INTERNAL;
            se_strcpy(out->error_msg, "internal error: invalid subquery index", SQL_ERR_MSG_LEN);
            return 0;
        }

        int correlated = pred->subquery_is_correlated[n->subquery_index];
        if (correlated && !outer_row) continue;   // deferred to the per-outer-row pass (see comment above)

        const char* text_to_run = pred->subqueries[n->subquery_index];
        char subst[PREDICATE_SUBQUERY_TEXT_LEN];
        if (correlated) {
            if (!substitute_outer_refs(text_to_run, outer_layout, outer_row, subst, PREDICATE_SUBQUERY_TEXT_LEN, out)) return 0;
            text_to_run = subst;
        }

        uint32_t rcount = 0;
        if (!exec_subquery_column(txn_id, caller_uid, text_to_run, out, &rcount)) return 0;

        if (n->op == PRED_OP_IN_SUBQUERY) {
            if (rcount == 0) {
                // Standard SQL: IN against an empty set is always false --
                // no value can ever equal "nothing".
                n->op = PRED_OP_FALSE;
                n->uses_subquery = 0;
                continue;
            }
            char col_name[RECORD_KEY_LEN];
            se_strcpy(col_name, n->column_name, RECORD_KEY_LEN);
            uint32_t acc = PREDICATE_INVALID_NODE;
            for (uint32_t r = 0; r < rcount; r++) {
                uint32_t eqn = predicate_add_comparison(pred, col_name, PRED_OP_EQ, g_subquery_result_text[r]);
                if (eqn == PREDICATE_INVALID_NODE) {
                    out->error = SQL_ERR_INTERNAL;
                    se_strcpy(out->error_msg, "IN (SELECT ...) result too large (predicate node pool exhausted)", SQL_ERR_MSG_LEN);
                    return 0;
                }
                acc = (acc == PREDICATE_INVALID_NODE) ? eqn : predicate_add_or(pred, acc, eqn);
                if (acc == PREDICATE_INVALID_NODE) {
                    out->error = SQL_ERR_INTERNAL;
                    se_strcpy(out->error_msg, "IN (SELECT ...) result too large (predicate node pool exhausted)", SQL_ERR_MSG_LEN);
                    return 0;
                }
            }
            // n may be a stale pointer after the predicate_add_* calls grew
            // pred->node_count -- pred->nodes[] itself never moves (fixed
            // array), so re-deref by index rather than trust the pointer.
            pred->nodes[i] = pred->nodes[acc];
        } else {
            // Scalar comparison.
            if (rcount > 1) {
                out->error = SQL_ERR_VALUE_INVALID;
                se_strcpy(out->error_msg, "subquery returned more than one row for a scalar comparison", SQL_ERR_MSG_LEN);
                return 0;
            }
            if (rcount == 0) {
                // Comparing against an empty scalar subquery result is
                // UNKNOWN in standard SQL, which WHERE treats as false --
                // the same simplification this codebase's NULL handling
                // already makes elsewhere (predicate.h Phase 4).
                pred->nodes[i].op = PRED_OP_FALSE;
                pred->nodes[i].uses_subquery = 0;
            } else {
                se_strcpy(pred->nodes[i].literal, g_subquery_result_text[0], RECORD_VAL_LEN);
                pred->nodes[i].uses_subquery = 0;
            }
        }
    }
    return 1;
}

// ─── Query-Surface Roadmap Phase 7: correlated subqueries (query side) ─────
// Not banked, same "provably never live across a nested call" reasoning as
// g_view_scratch/g_cte_scratch above -- exec_subquery_column() (the only
// thing resolve_predicate_subqueries() calls per candidate row) is a leaf
// call that never recurses into dispatch_stmt()/sql_execute_tx() at all
// (see its own header comment: it calls mvcc_find_matching_rows()/
// mvcc_row_get() directly), so correlated-subquery resolution never
// interacts with Phase 3's depth banking at all.
static struct RowValues g_correlated_scratch[CURSOR_MAX_ROWSET_ROWS];
static struct Predicate g_correlated_pred_scratch;

// A real, named ceiling on how many outer-table rows get tested against a
// correlated subquery, matching the roadmap doc's own "capped at a small
// outer-row budget (e.g. first 64 rows, loud truncation)" v1 posture: a
// correlated predicate can't be pushed down into mvcc_find_matching_rows()
// the way an ordinary WHERE is (there IS no single fixed comparison value
// until a specific outer row's own columns are spliced in), so this bounds
// the cost at O(budget x subquery-cost) rather than O(all-rows x
// subquery-cost) -- a real, honest v1 limitation, not silently unbounded.
#define SQL_CORRELATED_MAX_OUTER_ROWS 64

// Executes a plain (no JOIN/aggregates/set operator/CTE -- exec_select()'s
// own top-of-function check already rejected any of those combined with a
// correlated marker before this function is ever called) SELECT whose
// WHERE contains at least one correlated subquery. Fetches up to
// SQL_CORRELATED_MAX_OUTER_ROWS candidate rows with NO predicate pushed
// down, then re-resolves a FRESH COPY of the WHERE predicate against EACH
// candidate row in turn (g_correlated_pred_scratch = s->where, then
// resolve_predicate_subqueries(..., layout, &row, ...) -- the SAME function
// dispatch_stmt()'s own once-only call uses, just now supplied a real
// outer_row so a correlated marker actually gets spliced-and-executed
// instead of skipped), then predicate_eval()s the resolved copy against
// that row. A fresh copy is required every iteration because resolving
// mutates the predicate in place (see resolve_predicate_subqueries()'s own
// header comment) -- reusing s->where directly across rows would corrupt
// it after the first one.
static void exec_select_correlated(uint64_t txn_id, uint32_t caller_uid, const struct SqlSelectStmt* s, struct SqlResult* out) {
    int tidx = find_table_catalog_index(s->table_name);
    if (tidx < 0) {
        out->error = SQL_ERR_TABLE_NOT_FOUND; se_strcpy(out->error_msg, "table not found", SQL_ERR_MSG_LEN); return;
    }
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
    if (!predicate_columns_valid(&s->where, s->where.root, layout)) {
        out->error = SQL_ERR_COLUMN_NOT_FOUND;
        se_strcpy(out->error_msg, "WHERE clause references an unknown column", SQL_ERR_MSG_LEN);
        return;
    }

    struct MvccRowId ids[SQL_CORRELATED_MAX_OUTER_ROWS];
    uint32_t total = mvcc_find_matching_rows(txn_id, caller_uid, s->table_name, NULL, layout,
                                             ids, SQL_CORRELATED_MAX_OUTER_ROWS);
    uint32_t candidate_n = total < SQL_CORRELATED_MAX_OUTER_ROWS ? total : SQL_CORRELATED_MAX_OUTER_ROWS;
    uint8_t truncated = (total > SQL_CORRELATED_MAX_OUTER_ROWS) ? 1 : 0;

    uint32_t n = 0;
    for (uint32_t i = 0; i < candidate_n && n < CURSOR_MAX_ROWSET_ROWS; i++) {
        struct RowValues row;
        if (mvcc_row_get(txn_id, caller_uid, s->table_name, ids[i], &row) != MVCC_OK) continue;

        g_correlated_pred_scratch = s->where;
        if (!resolve_predicate_subqueries(txn_id, caller_uid, &g_correlated_pred_scratch, layout, &row, out)) return;

        if (predicate_eval(&g_correlated_pred_scratch, layout, &row)) {
            g_correlated_scratch[n] = row;
            n++;
        }
    }

    if (s->has_order_by) {
        // Insertion sort, same shape exec_select()'s own ORDER BY uses.
        for (uint32_t i = 1; i < n; i++) {
            struct RowValues key = g_correlated_scratch[i];
            int j = (int)i - 1;
            while (j >= 0) {
                int cmp = compare_rows_by_column(layout, order_col, &g_correlated_scratch[j], &key);
                int should_move = s->order_desc ? (cmp < 0) : (cmp > 0);
                if (!should_move) break;
                g_correlated_scratch[j + 1] = g_correlated_scratch[j];
                j--;
            }
            g_correlated_scratch[j + 1] = key;
        }
    }

    if (s->has_limit && s->limit < n) n = s->limit;

    uint32_t cid = cursor_open_rowset(s->table_name, g_correlated_scratch, n);

    out->error       = SQL_ERR_NONE;
    out->cursor_id   = cid;
    out->row_count   = n;
    out->truncated   = truncated;
    if (s->select_all) {
        out->column_count = layout->column_count;
        for (uint32_t i = 0; i < layout->column_count; i++) se_strcpy(out->columns[i], layout->column_names[i], RECORD_KEY_LEN);
    } else {
        out->column_count = s->column_count;
        for (uint32_t i = 0; i < s->column_count; i++) se_strcpy(out->columns[i], s->columns[i], RECORD_KEY_LEN);
    }
}

// See exec_select's own comment above on why this is static scratch, not a
// stack-local: struct SqlStatement embeds a struct Predicate (32 nodes x a
// 256-byte literal each), several KB on its own -- too large to put on a
// freestanding kernel's stack repeatedly, same reasoning, same tradeoff.
// Query-Surface Roadmap Phase 3: banked -- this is the one that matters
// most. sql_execute_tx() parses into g_stmt_scratch and hands dispatch_stmt()
// a POINTER into it; every exec_* function reads through that pointer for
// the statement's whole execution. A future nested sql_execute_tx() call
// (made from inside an exec_* function at depth 0, e.g. Phase 4's UNION
// running its right branch) reparsing into an unbanked single instance
// would corrupt the outer statement's own still-in-use fields out from
// under its own live pointer -- banked, the nested call's parse lands in
// bank[1] while the outer's pointer keeps referencing bank[0] untouched.
static struct SqlStatement g_stmt_scratch_bank[SQL_EXEC_MAX_DEPTH];
#define g_stmt_scratch (g_stmt_scratch_bank[g_exec_depth])

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
    // Phase 7 (SQL Feature-Parity Roadmap): resolve any embedded subqueries
    // in a PLAIN (non-JOIN, non-aggregate) SELECT/UPDATE/DELETE's WHERE
    // clause before that statement's own row scan runs -- see predicate.h's
    // Phase 7 note and resolve_predicate_subqueries()'s own comment above
    // for why exec_select_join()/exec_select_group()/HAVING are deliberately
    // excluded here (an unresolved marker there fails closed instead).
    //
    // Query-Surface Roadmap Phase 7: outer_layout/outer_row are passed NULL/
    // NULL at this top-level, once-only call site -- a CORRELATED marker is
    // deliberately left unresolved here (see resolve_predicate_subqueries()'s
    // own updated comment) rather than erroring, since exec_select() (for
    // SELECT) or the loud rejections just below (for UPDATE/DELETE) are what
    // decide what to do with it next.
    if (stmt->kind == SQL_STMT_SELECT && stmt->u.select.has_where &&
        !stmt->u.select.has_join && !stmt->u.select.has_aggregates) {
        if (!resolve_predicate_subqueries(txn_id, caller_uid, &stmt->u.select.where, NULL, NULL, out)) return;
    } else if (stmt->kind == SQL_STMT_UPDATE && stmt->u.update.has_where) {
        if (predicate_has_correlated_subquery(&stmt->u.update.where)) {
            out->error = SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED;
            se_strcpy(out->error_msg, "a correlated subquery is not supported in UPDATE ... WHERE (v1 scope cut -- SELECT only)", SQL_ERR_MSG_LEN);
            return;
        }
        if (!resolve_predicate_subqueries(txn_id, caller_uid, &stmt->u.update.where, NULL, NULL, out)) return;
    } else if (stmt->kind == SQL_STMT_DELETE && stmt->u.del.has_where) {
        if (predicate_has_correlated_subquery(&stmt->u.del.where)) {
            out->error = SQL_ERR_CORRELATED_SUBQUERY_UNSUPPORTED;
            se_strcpy(out->error_msg, "a correlated subquery is not supported in DELETE ... WHERE (v1 scope cut -- SELECT only)", SQL_ERR_MSG_LEN);
            return;
        }
        if (!resolve_predicate_subqueries(txn_id, caller_uid, &stmt->u.del.where, NULL, NULL, out)) return;
    }

    switch (stmt->kind) {
        case SQL_STMT_SELECT: exec_select(txn_id, caller_uid, &stmt->u.select, out); break;
        case SQL_STMT_INSERT: exec_insert(txn_id, caller_uid, &stmt->u.insert, out); break;
        case SQL_STMT_UPDATE: exec_update(txn_id, caller_uid, &stmt->u.update, out); break;
        case SQL_STMT_DELETE: exec_delete(txn_id, caller_uid, &stmt->u.del,    out); break;
        // Phase 5 (SQL Feature-Parity Roadmap): DDL -- (void)txn_id, none of
        // these five touch mvcc.c (see the header comment right above
        // exec_create_table() for why).
        case SQL_STMT_CREATE_TABLE: exec_create_table(caller_uid, &stmt->u.create_table, out); break;
        case SQL_STMT_ALTER_TABLE:  exec_alter_table(caller_uid, &stmt->u.alter_table,   out); break;
        case SQL_STMT_DROP_TABLE:   exec_drop_table(caller_uid, &stmt->u.drop_table,     out); break;
        case SQL_STMT_CREATE_INDEX: exec_create_index(caller_uid, &stmt->u.create_index, out); break;
        case SQL_STMT_DROP_INDEX:   exec_drop_index(caller_uid, &stmt->u.drop_index,     out); break;
        // Database Namespace & Access Roadmap Phase 2 -- see the header
        // comment right above exec_create_database() for why these are
        // thin wrappers, and sql_parser.h's SqlCreateDatabaseStmt/
        // SqlDropDatabaseStmt comments for why there's no ALTER DATABASE.
        case SQL_STMT_CREATE_DATABASE: exec_create_database(caller_uid, &stmt->u.create_database, out); break;
        case SQL_STMT_DROP_DATABASE:   exec_drop_database(caller_uid, &stmt->u.drop_database,     out); break;
        // Query-Surface Roadmap Phase 5 -- see the header comment right
        // above exec_create_view() for why these are thin wrappers, and
        // exec_select()'s own view-resolution fallback for the query side.
        case SQL_STMT_CREATE_VIEW: exec_create_view(caller_uid, &stmt->u.create_view, out); break;
        case SQL_STMT_DROP_VIEW:   exec_drop_view(caller_uid, &stmt->u.drop_view,     out); break;
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

// Query-Surface Roadmap Phase 3: internal-only proof of the depth-indexed
// banking above, called by tests/sql_exec_depth_phase3_host_test.c. NOT
// part of the public SQL surface -- no shell/HTTP/syscall entry point
// reaches this, and it exists purely so a host test can drive real
// nesting through the actual static internals (g_stmt_scratch,
// dispatch_stmt(), g_exec_depth) that a black-box caller of sql_execute()/
// sql_execute_tx() alone could never reach or corrupt on purpose.
//
// Parses `outer_sql` into depth 0's g_stmt_scratch bank and snapshots two
// representative fields (kind, FROM table name) BEFORE any nested work --
// then, still at depth 0 with the outer statement parsed but not yet
// dispatched, enters depth 1 and runs `inner_sql` to full completion via
// an entirely ordinary sql_execute_tx() call (its own parse, its own
// dispatch, its own cursor) -- exactly the shape a future Phase 4 UNION
// right branch or Phase 5/6 view/CTE expansion will use. Leaves depth 1,
// re-checks the outer snapshot survived byte-for-byte, and only THEN
// dispatches the outer statement for real -- so the caller's `outer_out`
// reflects the outer statement's true, uncorrupted result, not just a
// "didn't crash" signal.
//
// Returns 1 if the outer statement's own execution succeeded (SQL_ERR_NONE)
// AND its pre/post-nesting snapshot matched; 0 otherwise, with the specific
// failure already recorded in whichever of outer_out/inner_out caused it
// (parse failure, depth-enter failure, snapshot mismatch, or the outer
// statement's own dispatch error).
int sql_exec_test_phase3_nesting(uint64_t txn_id, uint32_t caller_uid,
                                  const char* outer_sql, const char* inner_sql,
                                  struct SqlResult* outer_out, struct SqlResult* inner_out) {
    se_memset(outer_out, 0, sizeof(*outer_out));
    se_memset(inner_out, 0, sizeof(*inner_out));

    char err[SQL_ERR_MSG_LEN];
    if (sql_parse(outer_sql, &g_stmt_scratch, err, sizeof(err)) != 0) {
        outer_out->kind  = SQL_STMT_INVALID;
        outer_out->error = SQL_ERR_PARSE;
        se_strcpy(outer_out->error_msg, err, SQL_ERR_MSG_LEN);
        return 0;
    }
    SqlStmtKind outer_kind_before = g_stmt_scratch.kind;
    char outer_table_before[OBJECT_NAME_LEN];
    se_strcpy(outer_table_before, g_stmt_scratch.u.select.table_name, OBJECT_NAME_LEN);

    if (!sql_exec_depth_enter(outer_out)) return 0;
    sql_execute_tx(txn_id, caller_uid, inner_sql, inner_out);
    sql_exec_depth_leave();

    // Back at depth 0 -- g_stmt_scratch macro-resolves to bank[0] again.
    // Prove it's untouched before trusting it for the real dispatch below.
    if (g_stmt_scratch.kind != outer_kind_before ||
        !se_streq(g_stmt_scratch.u.select.table_name, outer_table_before)) {
        outer_out->error = SQL_ERR_INTERNAL;
        se_strcpy(outer_out->error_msg,
                  "Phase 3 banking failed: outer g_stmt_scratch was corrupted by the nested call",
                  SQL_ERR_MSG_LEN);
        return 0;
    }

    dispatch_stmt(txn_id, caller_uid, &g_stmt_scratch, outer_out);
    return outer_out->error == SQL_ERR_NONE;
}

// Companion to sql_exec_test_phase3_nesting() above: proves the fail-loud
// path directly -- entering a second nested level (depth 1 -> 2) must be
// rejected with SQL_ERR_NESTING_TOO_DEEP, not silently wrap around and
// reuse bank[1] while it's still the live "depth 1" bank. Always leaves
// depth back at 0 before returning, regardless of outcome, so it's safe to
// call from a test without leaking depth state into later checks.
int sql_exec_test_phase3_depth_exceeded(struct SqlResult* scratch_out) {
    struct SqlResult r1, r2;
    se_memset(&r1, 0, sizeof(r1));
    se_memset(&r2, 0, sizeof(r2));
    int enter1 = sql_exec_depth_enter(&r1);   // depth 0 -> 1, should succeed
    int enter2 = sql_exec_depth_enter(&r2);   // depth 1 -> 2, should fail
    if (enter2) sql_exec_depth_leave();       // unwind if it unexpectedly succeeded
    if (enter1) sql_exec_depth_leave();       // unwind the first enter
    if (scratch_out) *scratch_out = r2;
    return enter1 && !enter2 && r2.error == SQL_ERR_NESTING_TOO_DEEP;
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

// ─── Schema import/export (SQL Feature-Parity Roadmap follow-on) ───────────
// See sql_exec.h's own header comment for the full design writeup: this
// mirrors SQLite's own "schema is just SQL text" convention (there is no
// separate schema FILE format even in real SQLite -- CREATE TABLE/CREATE
// INDEX text stored in sqlite_master, dumped/reloaded as plain SQL) rather
// than inventing a new structured format, and it reuses this file's own
// sql_execute()/sql_parse() for both directions instead of adding a new
// executor path.

// Bounded append: writes src onto the end of buf[0..*pos), bounded by max
// (always leaves room for the NUL this file's other buffer-building
// functions -- e.g. build_insert_row_values() -- rely on being present).
// Returns 0 the moment there's no room left, so a caller chaining several
// appends together can just OR the results and bail out on the first 0
// without needing to separately re-check *pos itself.
static int se_append(char* buf, uint32_t* pos, uint32_t max, const char* src) {
    uint32_t i = 0;
    while (src[i]) {
        if (*pos + 1 >= max) return 0;
        buf[(*pos)++] = src[i++];
    }
    buf[*pos] = '\0';
    return 1;
}

// Reconstructs ONE table's CREATE TABLE statement (inline NOT NULL/UNIQUE/
// REFERENCES/CHECK-BETWEEN per column, cross-referenced from
// row_constraints[] by table_object_id + column_index) into stmt_out.
// Database Gap Analysis §2.7: RANGE constraints now export as real,
// reconstructable CHECK (col BETWEEN lo AND hi) clauses -- the old
// out_has_range/"-- note:" lossy-export plumbing is gone. Returns the
// statement's text length (including the trailing ';'), or 0 if it didn't
// fit in stmt_max at all.
static uint32_t se_build_create_table(uint32_t catalog_idx, char* stmt_out, uint32_t stmt_max) {
    struct SLSObjectEntry* e = &object_catalog[catalog_idx];
    struct RowTableLayout* layout = &table_headers[catalog_idx].layout;
    uint64_t table_oid = e->object_id;
    uint32_t pos = 0;

    if (!se_append(stmt_out, &pos, stmt_max, "CREATE TABLE ")) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, e->name)) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, " (")) return 0;

    for (uint32_t c = 0; c < layout->column_count; c++) {
        if (c && !se_append(stmt_out, &pos, stmt_max, ", ")) return 0;
        if (!se_append(stmt_out, &pos, stmt_max, layout->column_names[c])) return 0;
        if (!se_append(stmt_out, &pos, stmt_max, " ")) return 0;
        if (!se_append(stmt_out, &pos, stmt_max, field_type_name(layout->column_types[c]))) return 0;

        for (uint32_t k = 0; k < row_constraint_count; k++) {
            struct RowConstraintDef* rc = &row_constraints[k];
            if (!rc->active || rc->table_object_id != table_oid || rc->column_index != c) continue;
            if (rc->kind == ROW_CONSTRAINT_NOT_NULL) {
                if (!se_append(stmt_out, &pos, stmt_max, " NOT NULL")) return 0;
            } else if (rc->kind == ROW_CONSTRAINT_UNIQUE) {
                if (!se_append(stmt_out, &pos, stmt_max, " UNIQUE")) return 0;
            } else if (rc->kind == ROW_CONSTRAINT_REFERENCE) {
                if (!se_append(stmt_out, &pos, stmt_max, " REFERENCES ")) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, rc->ref_table_name)) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, "(")) return 0;
                // ref_column_index indexes the REFERENCED table's own
                // layout, not this table's -- look it up by name rather
                // than assuming any relationship to `c`.
                int ref_idx = find_table_catalog_index(rc->ref_table_name);
                if (ref_idx >= 0 && rc->ref_column_index < table_headers[ref_idx].layout.column_count) {
                    if (!se_append(stmt_out, &pos, stmt_max, table_headers[ref_idx].layout.column_names[rc->ref_column_index])) return 0;
                }
                if (!se_append(stmt_out, &pos, stmt_max, ")")) return 0;
                // Cascading phase: emit the ON DELETE action so a schema
                // export/import round-trip preserves it. RESTRICT (the
                // default) is deliberately NOT emitted -- a plain
                // REFERENCES clause already means RESTRICT on re-import,
                // and omitting it keeps every pre-cascading export
                // byte-for-byte identical.
                if (rc->on_delete_action == ROW_ONDELETE_CASCADE) {
                    if (!se_append(stmt_out, &pos, stmt_max, " ON DELETE CASCADE")) return 0;
                } else if (rc->on_delete_action == ROW_ONDELETE_SET_NULL) {
                    if (!se_append(stmt_out, &pos, stmt_max, " ON DELETE SET NULL")) return 0;
                }
            } else if (rc->kind == ROW_CONSTRAINT_RANGE) {
                // Database Gap Analysis §2.7: RANGE now has real SQL syntax
                // (CHECK ... BETWEEN), so it exports as a reconstructable
                // clause instead of the old lossy "-- note:" -- closing the
                // round-trip loss this function's own header used to name.
                // STRING bounds are re-quoted (the lexer strips quotes at
                // parse time); numeric bounds emit bare.
                int is_str = (layout->column_types[c] == FIELD_TYPE_STRING);
                if (!se_append(stmt_out, &pos, stmt_max, " CHECK (")) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, layout->column_names[c])) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, " BETWEEN ")) return 0;
                if (is_str && !se_append(stmt_out, &pos, stmt_max, "'")) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, rc->literal_min)) return 0;
                if (is_str && !se_append(stmt_out, &pos, stmt_max, "'")) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, " AND ")) return 0;
                if (is_str && !se_append(stmt_out, &pos, stmt_max, "'")) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, rc->literal_max)) return 0;
                if (is_str && !se_append(stmt_out, &pos, stmt_max, "'")) return 0;
                if (!se_append(stmt_out, &pos, stmt_max, ")")) return 0;
            }
        }
    }
    if (!se_append(stmt_out, &pos, stmt_max, ");")) return 0;
    return pos;
}

// Reconstructs one CREATE INDEX statement for row_indexes[idx_slot] (must
// already be known active and belong to table_headers[catalog_idx]).
// Returns the statement's text length, or 0 if it didn't fit or the
// index's own column_index is stale (shouldn't happen -- guarded anyway).
static uint32_t se_build_create_index(uint32_t idx_slot, uint32_t catalog_idx, char* stmt_out, uint32_t stmt_max) {
    struct RowIndex* ri = &row_indexes[idx_slot];
    struct RowTableLayout* layout = &table_headers[catalog_idx].layout;
    uint32_t pos = 0;
    if (ri->column_index >= layout->column_count) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, "CREATE INDEX ")) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, ri->index_name)) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, " ON ")) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, object_catalog[catalog_idx].name)) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, "(")) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, layout->column_names[ri->column_index])) return 0;
    if (!se_append(stmt_out, &pos, stmt_max, ");")) return 0;
    return pos;
}

uint32_t sql_schema_export(uint32_t caller_uid, char* out, uint32_t max) {
    if (!out || max == 0) return 0;
    uint32_t pos = 0;
    out[0] = '\0';
    int stop = 0;

    for (uint32_t i = 0; i < object_catalog_count && !stop; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !e->uses_rowstore) continue;
        // Schema-only export still needs a real gate: without this, any
        // caller could enumerate every table's column names/types/
        // constraints regardless of catalog_check_access() -- the same
        // PERM_READ a SELECT against the table's own rows would require.
        if (!catalog_check_access(caller_uid, e->name, PERM_READ)) continue;

        char stmt[640];   // comfortably above SQL_MAX_TEXT_LEN=512 so the length check below is the real gate, not this buffer
        uint32_t slen = se_build_create_table(i, stmt, sizeof(stmt));
        if (slen == 0 || slen > SQL_MAX_TEXT_LEN) {
            // Emitted as a visible comment, not silently dropped -- see
            // sql_exec.h's own header note on why a too-long CREATE TABLE
            // is never truncated into invalid SQL instead.
            if (!se_append(out, &pos, max, "-- skipped table '") ||
                !se_append(out, &pos, max, e->name) ||
                !se_append(out, &pos, max, "': reconstructed CREATE TABLE would exceed SQL_MAX_TEXT_LEN\n")) { stop = 1; break; }
            continue;
        }
        if (!se_append(out, &pos, max, stmt) || !se_append(out, &pos, max, "\n")) { stop = 1; break; }
        // Database Gap Analysis §2.7: the old `has_range`/"-- note:" lossy-
        // export block is gone -- RANGE constraints now export inline as
        // real CHECK (col BETWEEN lo AND hi) clauses inside `stmt` itself.

        for (uint32_t k = 0; k < ROW_INDEX_MAX; k++) {
            if (!row_indexes[k].active || row_indexes[k].table_object_id != e->object_id) continue;
            char istmt[256];
            uint32_t ilen = se_build_create_index(k, i, istmt, sizeof(istmt));
            if (ilen == 0) continue;   // stale slot, shouldn't happen -- skip quietly rather than fail the whole export
            if (!se_append(out, &pos, max, istmt) || !se_append(out, &pos, max, "\n")) { stop = 1; break; }
        }
    }
    return pos;
}

void sql_schema_import(uint32_t caller_uid, const char* sql_text, struct SqlSchemaImportResult* out) {
    se_memset(out, 0, sizeof(*out));
    if (!out) return;
    if (!sql_text) return;

    uint32_t len = 0;
    while (sql_text[len]) len++;

    uint32_t i = 0;
    while (i < len) {
        // Skip leading whitespace and empty (';;'-style) segments, AND any
        // `-- ...` comment lines, so `offset` always points at the
        // statement's own first real character. This matters specifically
        // because sql_schema_export() emits `-- skipped:` (the `-- note:`
        // RANGE case became a real CHECK clause in Gap Analysis §2.7)
        // comment lines with no trailing ';' of their own (they're
        // documentation, not statements) -- without stripping them here
        // first, a comment line would otherwise get glued onto the FRONT
        // of the next real statement's text (there's nothing else to stop
        // at until that statement's own ';'), corrupting it. This lexer
        // has no comment syntax at all (see sql_parser.c), so that glued
        // text would fail to parse -- comment-skipping has to happen here,
        // one level up, before any text ever reaches sql_execute().
        for (;;) {
            while (i < len && (sql_text[i] == ' ' || sql_text[i] == '\t' ||
                               sql_text[i] == '\n' || sql_text[i] == '\r' || sql_text[i] == ';')) i++;
            if (i + 1 < len && sql_text[i] == '-' && sql_text[i + 1] == '-') {
                while (i < len && sql_text[i] != '\n') i++;
                continue;   // loop back to strip whitespace/';' and any further comment lines
            }
            break;
        }
        if (i >= len) break;

        uint32_t start = i;
        uint8_t in_quote = 0;
        while (i < len) {
            char c = sql_text[i];
            if (c == '\'') in_quote = (uint8_t)!in_quote;
            else if (c == ';' && !in_quote) break;
            i++;
        }
        uint32_t stmt_len = i - start;
        if (i < len) i++;   // consume the ';' itself; running off the end with no trailing ';' is fine too

        if (stmt_len == 0) continue;

        out->total++;
        if (out->total > SQL_SCHEMA_IMPORT_MAX_STMTS) continue;   // still counted in ->total so a caller can detect truncation; just not attempted

        char stmt_buf[SQL_MAX_TEXT_LEN + 8];
        uint32_t copy_len = stmt_len < sizeof(stmt_buf) - 1 ? stmt_len : sizeof(stmt_buf) - 1;
        for (uint32_t j = 0; j < copy_len; j++) stmt_buf[j] = sql_text[start + j];
        stmt_buf[copy_len] = '\0';

        struct SqlSchemaImportStmtResult* sr = &out->stmts[out->total - 1];
        sr->offset = start;
        struct SqlResult r;
        int rc = sql_execute(caller_uid, stmt_buf, &r);
        if (rc == 0) {
            sr->ok = 1;
            out->succeeded++;
            // A stray SELECT inside an otherwise-DDL import batch would
            // otherwise leak one of only CURSOR_MAX==8 open-cursor slots
            // for a result nobody asked to read back -- same fail-safe
            // posture Phase 7's exec_subquery_column() already applies.
            if (r.kind == SQL_STMT_SELECT) cursor_close(r.cursor_id);
        } else {
            sr->ok = 0;
            sr->error = r.error;
            se_strcpy(sr->error_msg, r.error_msg, SQL_ERR_MSG_LEN);
            out->failed++;
        }
    }
}

uint64_t sys_sls_schema_export(struct SLSSchemaExportRequest* req) {
    if (!req) return 1;
    req->bytes_written = sql_schema_export(req->caller_uid, req->sql_out, sizeof(req->sql_out));
    return 0;
}

uint64_t sys_sls_schema_import(struct SLSSchemaImportRequest* req) {
    if (!req) return 1;
    sql_schema_import(req->caller_uid, req->sql_text, &req->result);
    return req->result.failed > 0 ? 1 : 0;
}
