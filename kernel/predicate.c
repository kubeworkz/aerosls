/*
 * predicate.c — Phase 18 (relational layer) predicate evaluation (WHERE
 * engine). See predicate.h for the full design writeup.
 */
#include "predicate.h"
#include "object_catalog.h"
#include <stddef.h>

// ─── String / parsing helpers (no libc — each kernel source file keeps its
// own small copies, matching this codebase's established convention: rs_*
// in rowstore.c, ri_* in row_index.c, pe_* here). ─────────────────────────
static uint32_t pe_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int pe_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void pe_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static int pe_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}
static int pe_parse_f64(const char* s, double* out) {
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
static int pe_parse_bool(const char* s, uint8_t* out) {
    if (pe_streq(s, "true") || pe_streq(s, "1"))  { *out = 1; return 0; }
    if (pe_streq(s, "false") || pe_streq(s, "0")) { *out = 0; return 0; }
    return 1;
}
// Lexicographic byte compare -- returns <0, 0, >0, same contract as strcmp.
static int pe_strcmp(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// ─── Lifecycle / builders ────────────────────────────────────────────────
void predicate_init(struct Predicate* p) {
    p->node_count = 0;
    p->root = PREDICATE_INVALID_NODE;
}

uint32_t predicate_add_comparison(struct Predicate* p, const char* column_name,
                                  PredicateCompareOp op, const char* literal) {
    if (!p || p->node_count >= PREDICATE_MAX_NODES) return PREDICATE_INVALID_NODE;
    struct PredicateNode* n = &p->nodes[p->node_count];
    n->kind = PRED_NODE_COMPARISON;
    pe_strcpy(n->column_name, column_name ? column_name : "", RECORD_KEY_LEN);
    n->op = op;
    pe_strcpy(n->literal, literal ? literal : "", RECORD_VAL_LEN);
    n->left = PREDICATE_INVALID_NODE;
    n->right = PREDICATE_INVALID_NODE;
    return p->node_count++;
}

static uint32_t add_logical(struct Predicate* p, PredicateNodeKind kind, uint32_t left, uint32_t right) {
    if (!p || p->node_count >= PREDICATE_MAX_NODES) return PREDICATE_INVALID_NODE;
    if (left >= p->node_count || right >= p->node_count) return PREDICATE_INVALID_NODE;
    struct PredicateNode* n = &p->nodes[p->node_count];
    n->kind = kind;
    n->column_name[0] = '\0';
    n->literal[0] = '\0';
    n->left = left;
    n->right = right;
    return p->node_count++;
}

uint32_t predicate_add_and(struct Predicate* p, uint32_t left, uint32_t right) {
    return add_logical(p, PRED_NODE_AND, left, right);
}
uint32_t predicate_add_or(struct Predicate* p, uint32_t left, uint32_t right) {
    return add_logical(p, PRED_NODE_OR, left, right);
}

// ─── Type-aware comparison ───────────────────────────────────────────────
// Resolves column_name against layout, parses both the row's stored text
// value and the predicate's literal text according to the column's
// SLSFieldType, and returns a signed comparison (<0/0/>0), or 1 via *fail
// if the column doesn't exist or either value fails to parse -- the caller
// (eval_comparison below) treats *fail as "this comparison is false."
static int compare_typed(const struct RowTableLayout* layout, const struct RowValues* row,
                         const char* column_name, const char* literal, int* fail) {
    *fail = 0;
    uint32_t col = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < layout->column_count; i++) {
        if (pe_streq(layout->column_names[i], column_name)) { col = i; break; }
    }
    if (col == 0xFFFFFFFFu || col >= row->count) { *fail = 1; return 0; }

    const char* row_text = row->values[col];
    switch (layout->column_types[col]) {
        case FIELD_TYPE_UINT64: {
            uint64_t a, b;
            if (pe_parse_u64(row_text, &a) || pe_parse_u64(literal, &b)) { *fail = 1; return 0; }
            return (a > b) - (a < b);
        }
        case FIELD_TYPE_FLOAT: {
            double a, b;
            if (pe_parse_f64(row_text, &a) || pe_parse_f64(literal, &b)) { *fail = 1; return 0; }
            return (a > b) - (a < b);
        }
        case FIELD_TYPE_BOOL: {
            uint8_t a, b;
            if (pe_parse_bool(row_text, &a) || pe_parse_bool(literal, &b)) { *fail = 1; return 0; }
            return (int)a - (int)b;
        }
        case FIELD_TYPE_STRING:
        default: {
            uint32_t ignore = pe_strlen(row_text); (void)ignore;
            return pe_strcmp(row_text, literal);
        }
    }
}

static int eval_comparison(const struct PredicateNode* n, const struct RowTableLayout* layout,
                           const struct RowValues* row) {
    int fail = 0;
    int cmp = compare_typed(layout, row, n->column_name, n->literal, &fail);
    if (fail) return 0;   // unresolvable column / unparseable literal -- fail closed
    switch (n->op) {
        case PRED_OP_EQ: return cmp == 0;
        case PRED_OP_NE: return cmp != 0;
        case PRED_OP_LT: return cmp <  0;
        case PRED_OP_GT: return cmp >  0;
        case PRED_OP_LE: return cmp <= 0;
        case PRED_OP_GE: return cmp >= 0;
        default:         return 0;
    }
}

static int eval_node(const struct Predicate* p, uint32_t idx, const struct RowTableLayout* layout,
                     const struct RowValues* row) {
    if (idx >= p->node_count) return 0;   // malformed tree -- fail closed, not a crash
    const struct PredicateNode* n = &p->nodes[idx];
    switch (n->kind) {
        case PRED_NODE_COMPARISON: return eval_comparison(n, layout, row);
        case PRED_NODE_AND:        return eval_node(p, n->left, layout, row) && eval_node(p, n->right, layout, row);
        case PRED_NODE_OR:         return eval_node(p, n->left, layout, row) || eval_node(p, n->right, layout, row);
        default:                   return 0;
    }
}

int predicate_eval(const struct Predicate* p, const struct RowTableLayout* layout,
                   const struct RowValues* row) {
    if (!p || p->root == PREDICATE_INVALID_NODE) return 1;   // no predicate -- matches every row
    if (!layout || !row) return 0;
    return eval_node(p, p->root, layout, row);
}

// ─── Table-scan-with-predicate ──────────────────────────────────────────────
static int find_table_catalog_index(const char* table_name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (pe_streq(object_catalog[i].name, table_name)) return (int)i;
    }
    return -1;
}

struct pred_scan_ctx {
    const struct Predicate*      pred;
    const struct RowTableLayout* layout;
    RowScanCb                    user_cb;
    void*                        user_ctx;
    uint32_t                     matched;
};

static void pred_scan_trampoline(struct RowId id, const struct RowValues* values, void* ctxp) {
    struct pred_scan_ctx* ctx = (struct pred_scan_ctx*)ctxp;
    if (!predicate_eval(ctx->pred, ctx->layout, values)) return;
    ctx->matched++;
    if (ctx->user_cb) ctx->user_cb(id, values, ctx->user_ctx);
}

uint32_t predicate_table_scan(uint32_t caller_uid, const char* table_name,
                              const struct Predicate* pred, RowScanCb cb, void* ctx) {
    int tidx = find_table_catalog_index(table_name);
    if (tidx < 0) return 0;
    struct pred_scan_ctx pctx = { pred, &table_headers[tidx].layout, cb, ctx, 0 };
    rowstore_table_scan(caller_uid, table_name, pred_scan_trampoline, &pctx);
    return pctx.matched;
}
