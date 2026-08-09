#ifndef CONSTRAINT_H
#define CONSTRAINT_H

#include <stdint.h>
#include "object_catalog.h"

// ─── AeroSLS Data Constraint Engine ─────────────────────────────────────────
//
// Constraints are checked at the kernel DML boundary — before the WAL stage —
// so bad data never reaches persistent storage.
//
// Supported types (matches IBM i CHECK PENDING / column-level constraints):
//
//   UNIQUE     — no two fields with the same suffix in the table may share a value
//   NOT_NULL   — the field value must not be empty ("")
//   RANGE      — numeric value must satisfy min <= val <= max
//   REFERENCE  — value must exist as a record key in another table (foreign key)
//
// Field matching uses the same suffix rule as indexes: a constraint on field
// "dept" applies to "alice_dept", "bob_dept", etc.
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define CONSTRAINT_MAX  32

// ─── Constraint type ──────────────────────────────────────────────────────────
typedef enum {
    CTYPE_UNIQUE    = 0,   // value must be unique across the field in the table
    CTYPE_NOT_NULL  = 1,   // value must not be empty
    CTYPE_RANGE     = 2,   // numeric value must be in [range_min, range_max]
    CTYPE_REFERENCE = 3,   // value must match a key in ref_table
} ConstraintType;

// ─── One constraint definition ────────────────────────────────────────────────
struct SLSConstraint {
    char     table_name[OBJECT_NAME_LEN];  // table this constraint applies to
    char     field_name[RECORD_KEY_LEN];   // field suffix (e.g. "dept")
    uint8_t  type;                         // ConstraintType
    uint8_t  active;
    // RANGE parameters
    int64_t  range_min;
    int64_t  range_max;
    // REFERENCE parameter
    char     ref_table[OBJECT_NAME_LEN];   // referenced table for FK check
};

// ─── Globals ──────────────────────────────────────────────────────────────────
extern struct SLSConstraint constraint_table[CONSTRAINT_MAX];
extern uint32_t             constraint_count;

// ─── Lifecycle ────────────────────────────────────────────────────────────────
void constraint_init(void);

// Add a constraint.  For RANGE pass (int64_t)min and max in extra args.
// For REFERENCE pass ref_table name.  Returns 0 on success.
int  constraint_add_unique(const char* table, const char* field);
int  constraint_add_not_null(const char* table, const char* field);
int  constraint_add_range(const char* table, const char* field,
                          int64_t min, int64_t max);
int  constraint_add_reference(const char* table, const char* field,
                               const char* ref_table);

// Remove a specific constraint.  type=-1 removes all for (table, field).
int  constraint_remove(const char* table, const char* field, int type);

// ─── Enforcement ──────────────────────────────────────────────────────────────
// Check all active constraints for an INSERT.
// Returns 0 if the insert is allowed, or a non-zero violation code:
//   1 = UNIQUE violation
//   2 = NOT_NULL violation
//   3 = RANGE violation
//   4 = REFERENCE violation
int  constraint_check_insert(const char* table_name,
                              const char* key,
                              const char* value);

// Check for UPDATE (old_value → new_value).
int  constraint_check_update(const char* table_name,
                              const char* key,
                              const char* old_value,
                              const char* new_value);

// ─── Serialise ────────────────────────────────────────────────────────────────
int  constraints_to_json(const char* filter_table, char* buf, int max);

#endif /* CONSTRAINT_H */
