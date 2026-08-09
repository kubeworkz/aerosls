#ifndef INDEX_MGR_H
#define INDEX_MGR_H

#include <stdint.h>
#include "object_catalog.h"

// ─── AeroSLS Secondary Index Manager ─────────────────────────────────────────
//
// Each DB_INDEX object is a sorted array of (indexed_value → record_slot)
// pairs maintained over a DB_TABLE's records.
//
// Access pattern vs linear scan:
//   sys_sls_select (no index): O(n) — scans all 32 fields
//   index_lookup              : O(log n) — binary search on sorted array
//   index_range_scan          : O(k) — iterate from the first match
//
// Indexes are automatically maintained:
//   sys_sls_insert → index_on_insert()
//   sys_sls_update → index_on_update()
//   sys_sls_delete → index_on_delete()
//
// IBM i equivalent: *KEYFLD logical file / keyed access path (CRTLF).
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define INDEX_MAX          16    // max index objects
#define INDEX_KEY_LEN      64    // max indexed value length
#define INDEX_ENTRIES_MAX  32    // = RECORD_MAX_FIELDS per table

// ─── One entry in the sorted index array ─────────────────────────────────────
// Sorted ascending by `value` (the indexed field's value).
struct IndexEntry {
    char     value[INDEX_KEY_LEN];   // the value of the indexed field in this row
    char     rec_key[RECORD_KEY_LEN];// the record's own key (e.g. "alice_name")
    uint8_t  active;
};

// ─── One index object ─────────────────────────────────────────────────────────
struct SLSIndex {
    char           index_name[OBJECT_NAME_LEN];  // name of the DB_INDEX object
    char           table_name[OBJECT_NAME_LEN];  // parent DB_TABLE
    char           field_name[RECORD_KEY_LEN];   // field whose VALUE is indexed
    struct IndexEntry entries[INDEX_ENTRIES_MAX]; // sorted by entries[i].value
    uint32_t       count;    // active entries
    uint8_t        active;   // 1 = valid index
};

// ─── Globals ──────────────────────────────────────────────────────────────────
extern struct SLSIndex index_store[INDEX_MAX];
extern uint32_t        index_count;

// ─── Lifecycle ───────────────────────────────────────────────────────────────
void     index_mgr_init(void);

// Create a new index (builds from existing records if any).
// Returns 0 on success, non-zero on error.
int      index_create(const char* index_name,
                      const char* table_name,
                      const char* field_name);

// Drop an index by name.
int      index_drop(const char* index_name);

// ─── Auto-maintenance hooks (called by object_catalog DML) ───────────────────
// Called after a successful INSERT on table_name with the new field key + value.
void     index_on_insert(const char* table_name,
                         const char* rec_key,
                         const char* field_name,
                         const char* value);

// Called before an UPDATE; old_value is the previous value, new_value is next.
void     index_on_update(const char* table_name,
                         const char* rec_key,
                         const char* field_name,
                         const char* old_value,
                         const char* new_value);

// Called after a DELETE; the record key and its indexed values are removed.
void     index_on_delete(const char* table_name, const char* rec_key);

// ─── Query ────────────────────────────────────────────────────────────────────
// Exact-match lookup: fills rec_key_out with the matching record key.
// Returns 1 on hit, 0 on miss.
int      index_lookup(const char* index_name,
                      const char* value,
                      char*       rec_key_out);

// Range scan starting from `start_value` (inclusive).
// Calls callback(rec_key, value) for each matching entry until it returns 0.
typedef int (*IndexScanCb)(const char* rec_key, const char* value);
void     index_range_scan(const char* index_name,
                          const char* start_value,
                          IndexScanCb cb);

// ─── Serialise ────────────────────────────────────────────────────────────────
// Dump all entries of one index as a JSON array into buf[max].
int      index_to_json(const char* index_name, char* buf, int max);

// List all index objects as JSON.
int      indexes_to_json(char* buf, int max);

// Rebuild an index from the current records of its parent table.
void     index_rebuild(const char* index_name);

#endif /* INDEX_MGR_H */
