#ifndef MQT_H
#define MQT_H

#include <stdint.h>
#include "object_catalog.h"
#include "aggregate.h"

// ─── AeroSLS Materialized Query Tables (MQTs) ────────────────────────────────
//
// An MQT is a derived DB_TABLE whose content is a pre-computed aggregate
// query against a base table.  The kernel auto-refreshes it after every
// committed DML operation on the base table.
//
// IBM i equivalent: CREATE TABLE … AS (SELECT … GROUP BY …) DATA DEFERRED
// (also called "summary tables" or "derived tables").
//
// Stored results:
//   Non-grouped: one record  key="result"        value="<aggregate>"
//                            key="count"          value="<count>"
//                            key="refreshed_tick" value="<kernel_tick>"
//   Grouped:     per group   key="<group_value>"  value="<aggregate>"
//                            key="<gv>_count"     value="<count>"
//
// The MQT result object is a normal DB_TABLE — readable via `select`,
// indexable via DB3, and queryable via DB5/DB6 cursor/aggregate.
//
// Auto-refresh is triggered after every successful INSERT, UPDATE, or DELETE
// that commits (or direct writes) to the base table.  Each refresh clears
// the old results and writes fresh ones.
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define MQT_MAX  8

// ─── One MQT definition ───────────────────────────────────────────────────────
struct SLSMQT {
    char    mqt_name[OBJECT_NAME_LEN];    // name of the result table object
    char    base_table[OBJECT_NAME_LEN];  // table being aggregated
    uint8_t fn;                           // AggFunc (from aggregate.h)
    char    agg_field[RECORD_KEY_LEN];    // field to aggregate
    char    where_field[RECORD_KEY_LEN];  // optional WHERE filter field
    char    where_eq[RECORD_VAL_LEN];     // optional WHERE filter value
    char    group_field[RECORD_KEY_LEN];  // GROUP BY field (empty = no group)
    uint8_t active;
};

// ─── Globals ──────────────────────────────────────────────────────────────────
extern struct SLSMQT mqt_table[MQT_MAX];
extern uint32_t      mqt_count;

// ─── Lifecycle ────────────────────────────────────────────────────────────────
void mqt_init(void);

// Create an MQT: allocates a result table object, runs initial refresh.
// Returns 0 on success.
int  mqt_create(const char* mqt_name,
                const char* base_table,
                uint8_t     fn,
                const char* agg_field,
                const char* where_field,
                const char* where_eq,
                const char* group_field);

// Drop an MQT (also frees the result table object).
int  mqt_drop(const char* mqt_name);

// Refresh one MQT by name.  Clears the result table and re-runs the query.
void mqt_refresh(const char* mqt_name);

// Auto-refresh all MQTs whose base_table matches.
// Called by the DML hooks in object_catalog.c after every write.
void mqt_refresh_for_table(const char* table_name);

// ─── Serialise ────────────────────────────────────────────────────────────────
int  mqts_to_json(char* buf, int max);

#endif /* MQT_H */
