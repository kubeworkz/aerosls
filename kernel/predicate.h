/*
 * predicate.h — Phase 18 (relational layer): predicate evaluation (WHERE
 * engine) for row-set tables (kernel/rowstore.h, Phase 16). See
 * docs/AeroSLS-RDBMS-Roadmap-v0.1.md §5 for the scoped design and the
 * "Findings addendum" for what's built here.
 *
 * Filtering before this phase was exactly one shape: cursor.c's
 * where_field/where_value, single-field string equality against the legacy
 * object_records[] path. This gives row-set tables a real WHERE clause:
 * comparison operators (=, !=, <, >, <=, >=), AND/OR combinators, and
 * type-aware comparison against each column's SLSFieldType — comparing
 * "100" and "9" as raw strings gives the wrong answer for a UINT64 column,
 * which this phase exists specifically to avoid.
 *
 * ─── Representation ──────────────────────────────────────────────────────
 * A predicate is a small, fixed-capacity, array-backed tree (PREDICATE_
 * MAX_NODES=32, matching legacy index_mgr.c's own INDEX_ENTRIES_MAX=32
 * sizing precedent) -- not a pointer-based AST with heap allocation, same
 * "no dynamic allocation, fixed arrays, index-addressed nodes" discipline
 * kernel/row_index.c (Phase 17) and every other kernel subsystem in this
 * project already follows. Two node kinds: PRED_NODE_COMPARISON (a leaf:
 * column name, operator, literal text) and PRED_NODE_AND/PRED_NODE_OR (two
 * child node indices). There is no parser yet (that's Phase 19) -- callers
 * build a Predicate directly via predicate_add_comparison()/_add_and()/
 * _add_or(), threading the returned indices together and setting
 * Predicate.root themselves, the same "caller assembles it from small
 * primitives" shape row_index.c's own build helpers use.
 *
 * Explicitly OUT OF SCOPE this phase (see the roadmap doc): arithmetic
 * expressions, string functions, subqueries, LIKE/pattern matching, IN
 * lists -- deliberately narrow, matching this whole roadmap's stated
 * non-goal of a full SQL expression language. An index-assisted scan path
 * (using a Phase 17 B-tree index when a predicate's column/operator match
 * one) is also deliberately deferred -- there is no query planner yet to
 * decide table-scan vs. index-scan; that's Phase 19's job, once it exists
 * to make the choice. This phase only proves the predicate itself is
 * correct and that a full-table scan can be filtered by one.
 */
#ifndef PREDICATE_H
#define PREDICATE_H

#include <stdint.h>
#include "rowstore.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define PREDICATE_MAX_NODES     32
#define PREDICATE_INVALID_NODE  0xFFFFFFFFu

typedef enum {
    PRED_OP_EQ = 0,
    PRED_OP_NE,
    PRED_OP_LT,
    PRED_OP_GT,
    PRED_OP_LE,
    PRED_OP_GE,
} PredicateCompareOp;

typedef enum {
    PRED_NODE_COMPARISON = 0,
    PRED_NODE_AND,
    PRED_NODE_OR,
} PredicateNodeKind;

// ─── One node — either a typed comparison leaf or an AND/OR combinator. ────
struct PredicateNode {
    PredicateNodeKind kind;
    // meaningful when kind == PRED_NODE_COMPARISON
    char               column_name[RECORD_KEY_LEN];
    PredicateCompareOp op;
    char               literal[RECORD_VAL_LEN];   // text, matching RowValues' own
                                                    // "everything is text at the API
                                                    // boundary" convention
    // meaningful when kind == PRED_NODE_AND / PRED_NODE_OR
    uint32_t left;
    uint32_t right;
};

// ─── A predicate: a small fixed pool of nodes plus a root index. An empty
// predicate (root == PREDICATE_INVALID_NODE, the state predicate_init()
// leaves it in) evaluates to true for every row -- the standard "no WHERE
// clause matches everything" SQL semantic, and the useful default for
// predicate_table_scan() below. ───────────────────────────────────────────
struct Predicate {
    struct PredicateNode nodes[PREDICATE_MAX_NODES];
    uint32_t             node_count;
    uint32_t             root;
};

void predicate_init(struct Predicate* p);

// Appends a comparison leaf; returns its node index, or PREDICATE_INVALID_
// NODE if the node pool (PREDICATE_MAX_NODES) is exhausted. Does not touch
// p->root -- the caller wires nodes together and sets p->root explicitly.
uint32_t predicate_add_comparison(struct Predicate* p, const char* column_name,
                                  PredicateCompareOp op, const char* literal);

// Appends an AND/OR combinator over two already-added node indices; returns
// its node index, or PREDICATE_INVALID_NODE if exhausted or either child
// index is out of range.
uint32_t predicate_add_and(struct Predicate* p, uint32_t left, uint32_t right);
uint32_t predicate_add_or(struct Predicate* p, uint32_t left, uint32_t right);

// ─── Evaluation ─────────────────────────────────────────────────────────────
// Evaluates p against one row, given its table's layout (for column name ->
// index/SLSFieldType resolution). A NULL predicate, or one whose root is
// PREDICATE_INVALID_NODE, evaluates to true (matches every row). A
// comparison against a column name not present in layout, or a literal
// that fails to parse against that column's type, evaluates to FALSE for
// that comparison (fail closed -- "denial looks like absence," the same
// posture this project has used since Phase 7's capability checks) rather
// than crashing or silently matching. Pure function: no I/O, no globals.
int predicate_eval(const struct Predicate* p, const struct RowTableLayout* layout,
                   const struct RowValues* row);

// ─── Table-scan-with-predicate ──────────────────────────────────────────────
// Thin wrapper over rowstore_table_scan() (Phase 16): scans table_name,
// invoking cb() only for rows that pass pred (or every row, if pred is
// NULL/empty). Reuses rowstore_table_scan()'s own catalog_check_access()
// gate unmodified -- no new permission-check code path, same "one choke
// point" precedent access control has followed in this project since
// Phase 7. Returns the number of MATCHING rows (cb invocations), not the
// number scanned -- 0 if the table doesn't exist, isn't a row-set table,
// or access is denied, same as rowstore_table_scan()'s own return
// convention in those cases.
uint32_t predicate_table_scan(uint32_t caller_uid, const char* table_name,
                              const struct Predicate* pred, RowScanCb cb, void* ctx);

#endif /* PREDICATE_H */
