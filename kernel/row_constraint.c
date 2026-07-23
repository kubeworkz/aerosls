/*
 * row_constraint.c — Phase 23 (relational layer) constraint enforcement.
 * See row_constraint.h for the full design writeup.
 */
#include "row_constraint.h"
#include "object_catalog.h"
#include "mvcc.h"
#include "persist.h"   // Gap Remediation Phase D -- persist_row_constraints()
#include <stddef.h>

// ─── String / parsing helpers (no libc -- each kernel source file keeps its
// own small copies, matching this codebase's established convention: rs_*
// in rowstore.c, ri_* in row_index.c, pe_* in predicate.c, se_*/mv_*
// elsewhere, rc_* here). ────────────────────────────────────────────────────
// Phase 4 (SQL Feature-Parity Roadmap): rc_strlen() (an `strlen(val) == 0`
// empty-string-as-NULL convention helper) is no longer used anywhere in
// this file -- every NULL check below now reads the real null_mask flag
// instead (see row_constraint_check_write()'s Phase 4 note). Removed
// rather than left as dead code.
static int rc_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void rc_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static int rc_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}
static int rc_parse_f64(const char* s, double* out) {
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
static int rc_strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// -1 if not found / not a row-set table.
static int rc_find_table_index(const char* table_name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (rc_streq(object_catalog[i].name, table_name)) return (int)i;
    }
    return -1;
}
static uint32_t rc_find_column_index(const struct RowTableLayout* layout, const char* name) {
    for (uint32_t i = 0; i < layout->column_count; i++)
        if (rc_streq(layout->column_names[i], name)) return i;
    return 0xFFFFFFFFu;
}

// Type-aware "is value in [min, max]" check, matching predicate.c's own
// compare_typed()'s per-SLSFieldType shape (a separate small copy, not a
// shared function -- this codebase's established per-file convention).
static int rc_in_range(SLSFieldType type, const char* value, const char* lo, const char* hi, int* parse_fail) {
    *parse_fail = 0;
    switch (type) {
        case FIELD_TYPE_UINT64: {
            uint64_t v, a, b;
            if (rc_parse_u64(value, &v) || rc_parse_u64(lo, &a) || rc_parse_u64(hi, &b)) { *parse_fail = 1; return 0; }
            return v >= a && v <= b;
        }
        case FIELD_TYPE_FLOAT: {
            double v, a, b;
            if (rc_parse_f64(value, &v) || rc_parse_f64(lo, &a) || rc_parse_f64(hi, &b)) { *parse_fail = 1; return 0; }
            return v >= a && v <= b;
        }
        case FIELD_TYPE_STRING:
        default:
            return rc_strcmp(value, lo) >= 0 && rc_strcmp(value, hi) <= 0;
    }
}

struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
uint32_t                row_constraint_count;

void row_constraint_init(void) {
    for (uint32_t i = 0; i < ROW_CONSTRAINT_MAX; i++) row_constraints[i].active = 0;
    row_constraint_count = 0;
}

// ─── Registration ─────────────────────────────────────────────────────────
static RowConstraintResult rc_add(RowConstraintKind kind, const char* table_name, const char* column_name,
                                  const char* lo, const char* hi,
                                  const char* ref_table_name, const char* ref_column_name,
                                  RowOnDeleteAction on_delete_action) {
    int tidx = rc_find_table_index(table_name);
    if (tidx < 0) return ROW_CONSTRAINT_ERR_TABLE_NOT_FOUND;
    const struct RowTableLayout* layout = &table_headers[tidx].layout;
    uint32_t col = rc_find_column_index(layout, column_name);
    if (col == 0xFFFFFFFFu) return ROW_CONSTRAINT_ERR_COLUMN_NOT_FOUND;

    if (row_constraint_count >= ROW_CONSTRAINT_MAX) return ROW_CONSTRAINT_ERR_POOL_FULL;

    uint64_t ref_table_object_id = 0;
    uint32_t ref_col = 0;
    if (kind == ROW_CONSTRAINT_REFERENCE) {
        int ridx = rc_find_table_index(ref_table_name);
        if (ridx < 0) return ROW_CONSTRAINT_ERR_REF_TABLE_NOT_FOUND;
        const struct RowTableLayout* rlayout = &table_headers[ridx].layout;
        ref_col = rc_find_column_index(rlayout, ref_column_name);
        if (ref_col == 0xFFFFFFFFu) return ROW_CONSTRAINT_ERR_REF_COLUMN_NOT_FOUND;
        if (rlayout->column_types[ref_col] != layout->column_types[col]) return ROW_CONSTRAINT_ERR_TYPE_MISMATCH;
        ref_table_object_id = object_catalog[ridx].object_id;
    }
    if (kind == ROW_CONSTRAINT_RANGE) {
        int fail = 0;
        // Validate min/max parse against the column's type up front --
        // rc_in_range()'s own parse_fail path exists for the runtime check,
        // but a constraint that can never be satisfied because its own
        // bounds don't parse is a registration-time mistake, not a
        // per-row one, and deserves to fail loud right away.
        char dummy[2] = "0";
        rc_in_range(layout->column_types[col], dummy, lo, hi, &fail);
        if (fail) return ROW_CONSTRAINT_ERR_RANGE_INVALID;
    }

    struct RowConstraintDef* c = &row_constraints[row_constraint_count++];
    c->active = 1;
    c->kind = kind;
    c->table_object_id = object_catalog[tidx].object_id;
    rc_strcpy(c->table_name, table_name, OBJECT_NAME_LEN);
    c->column_index = col;
    c->literal_min[0] = '\0';
    c->literal_max[0] = '\0';
    if (lo) rc_strcpy(c->literal_min, lo, RECORD_VAL_LEN);
    if (hi) rc_strcpy(c->literal_max, hi, RECORD_VAL_LEN);
    c->ref_table_object_id = ref_table_object_id;
    c->ref_table_name[0] = '\0';
    if (ref_table_name) rc_strcpy(c->ref_table_name, ref_table_name, OBJECT_NAME_LEN);
    c->ref_column_index = ref_col;
    c->on_delete_action = (uint8_t)on_delete_action;   // Cascading phase -- 0 (RESTRICT) for every non-REFERENCE kind

    persist_row_constraints();   // Gap Remediation Phase D
    return ROW_CONSTRAINT_OK;
}

RowConstraintResult row_constraint_add_unique(const char* table_name, const char* column_name) {
    return rc_add(ROW_CONSTRAINT_UNIQUE, table_name, column_name, NULL, NULL, NULL, NULL, ROW_ONDELETE_RESTRICT);
}
RowConstraintResult row_constraint_add_not_null(const char* table_name, const char* column_name) {
    return rc_add(ROW_CONSTRAINT_NOT_NULL, table_name, column_name, NULL, NULL, NULL, NULL, ROW_ONDELETE_RESTRICT);
}
RowConstraintResult row_constraint_add_range(const char* table_name, const char* column_name,
                                             const char* min_literal, const char* max_literal) {
    return rc_add(ROW_CONSTRAINT_RANGE, table_name, column_name, min_literal, max_literal, NULL, NULL, ROW_ONDELETE_RESTRICT);
}
RowConstraintResult row_constraint_add_reference(const char* table_name, const char* column_name,
                                                 const char* ref_table_name, const char* ref_column_name) {
    // Cascading phase: kept as a thin RESTRICT wrapper -- see the header's
    // own comment on why this survives instead of being replaced.
    return rc_add(ROW_CONSTRAINT_REFERENCE, table_name, column_name, NULL, NULL, ref_table_name, ref_column_name, ROW_ONDELETE_RESTRICT);
}
RowConstraintResult row_constraint_add_reference_action(const char* table_name, const char* column_name,
                                                        const char* ref_table_name, const char* ref_column_name,
                                                        RowOnDeleteAction on_delete_action) {
    return rc_add(ROW_CONSTRAINT_REFERENCE, table_name, column_name, NULL, NULL, ref_table_name, ref_column_name, on_delete_action);
}

// ─── Runtime checks ───────────────────────────────────────────────────────
struct rc_unique_ctx { uint32_t col; const char* candidate; uint64_t exclude_logical_id; int found; };
static void rc_unique_scan_cb(struct MvccRowId id, const struct RowValues* v, void* ctxp) {
    struct rc_unique_ctx* ctx = (struct rc_unique_ctx*)ctxp;
    if (ctx->found) return;
    if (id.logical_id == ctx->exclude_logical_id) return;
    // Phase 4 (SQL Feature-Parity Roadmap): a NULL in the scanned row can
    // never be a duplicate of anything (standard SQL UNIQUE semantics) --
    // without this check, a NULL column's now-empty text would incorrectly
    // collide with a real empty-STRING candidate value.
    if (v->null_mask & (1u << ctx->col)) return;
    if (rc_strcmp(v->values[ctx->col], ctx->candidate) == 0) ctx->found = 1;
}

struct rc_ref_ctx { uint32_t col; const char* candidate; int found; };
static void rc_ref_scan_cb(struct MvccRowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct rc_ref_ctx* ctx = (struct rc_ref_ctx*)ctxp;
    if (ctx->found) return;
    // Phase 4: a NULL column in the scanned row can never satisfy a
    // REFERENCE match either.
    if (v->null_mask & (1u << ctx->col)) return;
    if (rc_strcmp(v->values[ctx->col], ctx->candidate) == 0) ctx->found = 1;
}

RowConstraintResult row_constraint_check_write(uint64_t txn_id, uint32_t caller_uid,
                                               uint64_t table_object_id,
                                               const struct RowValues* values,
                                               uint64_t exclude_logical_id) {
    for (uint32_t i = 0; i < row_constraint_count; i++) {
        struct RowConstraintDef* c = &row_constraints[i];
        if (!c->active || c->table_object_id != table_object_id) continue;
        if (c->column_index >= values->count) continue;   // defensive -- shouldn't happen, column count is schema-fixed
        const char* val = values->values[c->column_index];
        // Phase 4 (SQL Feature-Parity Roadmap): the real null_mask flag,
        // replacing this function's old `rc_strlen(val) == 0` convention
        // (which couldn't distinguish NULL from a real empty STRING value).
        int is_null = (values->null_mask & (1u << c->column_index)) ? 1 : 0;

        switch (c->kind) {
            case ROW_CONSTRAINT_NOT_NULL:
                if (is_null) return ROW_CONSTRAINT_VIOLATION_NOT_NULL;
                break;

            case ROW_CONSTRAINT_RANGE: {
                // A NULL value trivially satisfies RANGE -- standard SQL
                // constraint semantics (a constraint's condition is only
                // evaluated against a present value; NULL makes it
                // vacuously true, not violated).
                if (is_null) break;
                int fail = 0;
                int tidx = rc_find_table_index(c->table_name);
                SLSFieldType t = (tidx >= 0) ? table_headers[tidx].layout.column_types[c->column_index] : FIELD_TYPE_STRING;
                if (!rc_in_range(t, val, c->literal_min, c->literal_max, &fail) || fail)
                    return ROW_CONSTRAINT_VIOLATION_RANGE;
                break;
            }

            case ROW_CONSTRAINT_UNIQUE: {
                // A real NULL is never compared for uniqueness -- matches
                // standard SQL UNIQUE semantics (multiple NULLs are allowed
                // even in a UNIQUE column). Phase 4: this now correctly
                // treats a real empty STRING value as a comparable, non-
                // exempt candidate -- the old `strlen(val) == 0` check
                // conflated the two, a genuine behavior fix as a side
                // effect of no longer doing so.
                if (is_null) break;
                struct rc_unique_ctx ctx = { c->column_index, val, exclude_logical_id, 0 };
                mvcc_table_scan(txn_id, caller_uid, c->table_name, rc_unique_scan_cb, &ctx);
                if (ctx.found) return ROW_CONSTRAINT_VIOLATION_UNIQUE;
                break;
            }

            case ROW_CONSTRAINT_REFERENCE: {
                if (is_null) break;   // a NULL FK column is not required to reference anything
                struct rc_ref_ctx ctx = { c->ref_column_index, val, 0 };
                mvcc_table_scan(txn_id, caller_uid, c->ref_table_name, rc_ref_scan_cb, &ctx);
                if (!ctx.found) return ROW_CONSTRAINT_VIOLATION_REFERENCE;
                break;
            }
        }
    }
    return ROW_CONSTRAINT_OK;
}

// ─── Cascading phase: collect-then-act child row gathering ────────────────
// Collects the logical ids of every child row whose FK column matches the
// parent's value -- deliberately does NOT mutate anything during the scan
// (a SET NULL's mvcc_row_update() appends to the very mvcc_versions[]
// array mvcc_table_scan() iterates; see the header's cascading section).
struct rc_collect_ctx {
    uint32_t col;
    const char* candidate;
    uint64_t ids[ROW_CASCADE_MAX_ROWS];
    uint32_t count;
    int overflow;
};
static void rc_collect_scan_cb(struct MvccRowId id, const struct RowValues* v, void* ctxp) {
    struct rc_collect_ctx* ctx = (struct rc_collect_ctx*)ctxp;
    if (ctx->overflow) return;
    if (v->null_mask & (1u << ctx->col)) return;   // NULL FK never matches (Phase 4 semantics)
    if (rc_strcmp(v->values[ctx->col], ctx->candidate) != 0) return;
    if (ctx->count >= ROW_CASCADE_MAX_ROWS) { ctx->overflow = 1; return; }
    ctx->ids[ctx->count++] = id.logical_id;
}

// Depth guard for CASCADE chains: mvcc_row_delete() on a child row re-enters
// row_constraint_check_delete() for the child's own table, which is exactly
// how multi-level FK chains cascade correctly -- but also exactly how a
// circular FK chain (A references B references A) would recurse forever.
// A plain static counter (not per-transaction) is sufficient: this whole
// engine is single-threaded per call path (no concurrent SQL execution --
// sql_exec.c's own static-scratch design already relies on this), so the
// counter can never be racing another in-flight delete's.
static uint32_t rc_cascade_depth = 0;

RowConstraintResult row_constraint_check_delete(uint64_t txn_id, uint32_t caller_uid,
                                                const char* table_name, uint64_t table_object_id,
                                                struct RowId physical_id) {
    // Cheap common case: no REFERENCE constraint anywhere targets this
    // table, so there is nothing that could ever be "still referencing"
    // the row about to be deleted -- skip fetching its values entirely.
    int any_ref = 0;
    for (uint32_t i = 0; i < row_constraint_count; i++) {
        if (row_constraints[i].active && row_constraints[i].kind == ROW_CONSTRAINT_REFERENCE &&
            row_constraints[i].ref_table_object_id == table_object_id) { any_ref = 1; break; }
    }
    if (!any_ref) return ROW_CONSTRAINT_OK;

    struct RowValues values;
    if (rowstore_row_get(caller_uid, table_name, physical_id, &values) != 0) return ROW_CONSTRAINT_OK;   // row already gone -- nothing to protect

    for (uint32_t i = 0; i < row_constraint_count; i++) {
        struct RowConstraintDef* c = &row_constraints[i];
        if (!c->active || c->kind != ROW_CONSTRAINT_REFERENCE || c->ref_table_object_id != table_object_id) continue;
        if (c->ref_column_index >= values.count) continue;
        const char* ref_val = values.values[c->ref_column_index];
        if (values.null_mask & (1u << c->ref_column_index)) continue;   // Phase 4: real null_mask, not strlen==0

        // ── RESTRICT (the unchanged pre-cascading path, zero default) ──
        if (c->on_delete_action == ROW_ONDELETE_RESTRICT) {
            struct rc_ref_ctx ctx = { c->column_index, ref_val, 0 };
            mvcc_table_scan(txn_id, caller_uid, c->table_name, rc_ref_scan_cb, &ctx);
            if (ctx.found) return ROW_CONSTRAINT_VIOLATION_REFERENCED;
            continue;
        }

        // ── CASCADE / SET NULL: collect the child rows first, then act ──
        struct rc_collect_ctx cctx;
        cctx.col = c->column_index;
        cctx.candidate = ref_val;
        cctx.count = 0;
        cctx.overflow = 0;
        mvcc_table_scan(txn_id, caller_uid, c->table_name, rc_collect_scan_cb, &cctx);
        if (cctx.overflow) return ROW_CONSTRAINT_CASCADE_FAILED;   // > ROW_CASCADE_MAX_ROWS children -- fail whole delete cleanly
        if (cctx.count == 0) continue;                              // nothing references this row via this constraint

        if (c->on_delete_action == ROW_ONDELETE_CASCADE) {
            if (rc_cascade_depth >= ROW_CASCADE_MAX_DEPTH) return ROW_CONSTRAINT_CASCADE_FAILED;
            rc_cascade_depth++;
            for (uint32_t k = 0; k < cctx.count; k++) {
                struct MvccRowId child = { cctx.ids[k] };
                // The REAL delete path -- the child's own constraints
                // (including further cascades) run exactly as if the child
                // were deleted directly. MVCC_ERR_ROW_NOT_VISIBLE is
                // tolerated: an earlier iteration (or an earlier
                // constraint's cascade) may have already deleted this
                // child within this same transaction.
                MvccError de = mvcc_row_delete(txn_id, caller_uid, c->table_name, child);
                if (de != MVCC_OK && de != MVCC_ERR_ROW_NOT_VISIBLE) {
                    rc_cascade_depth--;
                    return ROW_CONSTRAINT_CASCADE_FAILED;
                }
            }
            rc_cascade_depth--;
            continue;
        }

        // ── SET NULL ──
        for (uint32_t k = 0; k < cctx.count; k++) {
            struct MvccRowId child = { cctx.ids[k] };
            struct RowValues child_vals;
            MvccError ge = mvcc_row_get(txn_id, caller_uid, c->table_name, child, &child_vals);
            if (ge == MVCC_ERR_ROW_NOT_VISIBLE) continue;   // already gone this txn -- nothing to null out
            if (ge != MVCC_OK) return ROW_CONSTRAINT_CASCADE_FAILED;
            child_vals.null_mask |= (1u << c->column_index);   // real NULL, Phase 4 semantics
            child_vals.values[c->column_index][0] = '\0';
            // The REAL update path -- runs row_constraint_check_write() on
            // the child as usual, so SET NULL against a column that also
            // carries NOT NULL correctly fails the whole delete (standard
            // SQL behavior), for free.
            MvccError ue = mvcc_row_update(txn_id, caller_uid, c->table_name, child, &child_vals);
            if (ue != MVCC_OK) return ROW_CONSTRAINT_CASCADE_FAILED;
        }
    }
    return ROW_CONSTRAINT_OK;
}
