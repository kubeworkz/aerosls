#ifndef AGGREGATE_H
#define AGGREGATE_H

#include <stdint.h>
#include "object_catalog.h"

// ─── AeroSLS Aggregate Query Engine ──────────────────────────────────────────
//
// Implements analytics queries over DB_TABLE records in a single in-memory
// pass.  IBM i equivalent: embedded SQL SELECT with aggregates.
//
// Supported functions:
//   COUNT  — number of matching rows
//   SUM    — sum of numeric field values
//   AVG    — arithmetic mean (SUM / COUNT)
//   MIN    — minimum numeric value
//   MAX    — maximum numeric value
//   NONE   — no aggregate; return rows sorted by order_field (ORDER BY only)
//
// Optional clauses:
//   WHERE  where_field suffix-match + where_value exact match
//   GROUP BY  group_field suffix-match → one result per distinct group value
//   HAVING    minimum count per group (applied after GROUP BY)
//   ORDER BY  result rows sorted ASC or DESC by a field value
//
// All operates on at most RECORD_MAX_FIELDS (32) rows per table — O(n²) sorts
// are fast enough.  The engine is intentionally self-contained (no cursors
// needed) for ad-hoc analytics queries.
//
// IBM i equivalents:
//   OPNQRYF KEYFLD(*NONE) QRYSLT('DEPT *EQ ''Engineering''')
//   SELECT COUNT(*), SUM(SALARY) FROM STAFF WHERE DEPT = 'Engineering'
//   SELECT DEPT, COUNT(*) FROM STAFF GROUP BY DEPT HAVING COUNT(*) > 1
// ─────────────────────────────────────────────────────────────────────────────

// ─── Aggregate function ───────────────────────────────────────────────────────
typedef enum {
    AGG_COUNT = 0,
    AGG_SUM   = 1,
    AGG_AVG   = 2,
    AGG_MIN   = 3,
    AGG_MAX   = 4,
    AGG_NONE  = 5,   // ORDER BY only — return all matching rows sorted
} AggFunc;

// ─── Limits ───────────────────────────────────────────────────────────────────
#define AGG_MAX_GROUPS   32   // max GROUP BY buckets
#define AGG_MAX_ROWS     32   // max rows returned for ORDER BY / AGG_NONE

// ─── One GROUP BY bucket ─────────────────────────────────────────────────────
struct AggGroup {
    char    group_key[64];    // distinct value of the group_by field
    int64_t count;
    int64_t sum;
    int64_t min_val;
    int64_t max_val;
    uint8_t has_data;         // at least one row contributed
    uint8_t active;
};

// ─── One row result (for AGG_NONE / ORDER BY) ─────────────────────────────────
struct AggRow {
    char key[RECORD_KEY_LEN];
    char value[RECORD_VAL_LEN];
    char sort_key[RECORD_VAL_LEN];   // value of order_field for this row
};

// ─── Query descriptor ─────────────────────────────────────────────────────────
struct AggQuery {
    char    table[OBJECT_NAME_LEN];
    uint8_t fn;                      // AggFunc
    char    agg_field[RECORD_KEY_LEN];   // field to aggregate (suffix match)
    char    where_field[RECORD_KEY_LEN]; // filter field suffix (empty = all)
    char    where_eq[RECORD_VAL_LEN];    // required value (empty = any)
    char    group_field[RECORD_KEY_LEN]; // GROUP BY field suffix (empty = none)
    int64_t having_min_count;            // HAVING COUNT >= N  (0 = disabled)
    char    order_field[RECORD_KEY_LEN]; // ORDER BY field suffix (empty = none)
    uint8_t order_desc;                  // 1 = DESC, 0 = ASC
};

// ─── Public API ───────────────────────────────────────────────────────────────

// Execute an aggregate query.  Serialises results as JSON into buf[max].
// Returns bytes written.
int aggregate_exec(const struct AggQuery* q, char* buf, int max);

#endif /* AGGREGATE_H */
