#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>
#include "object_catalog.h"

// ─── AeroSLS Server-Side Cursor Engine ───────────────────────────────────────
//
// A cursor is a named, stateful iterator over a DB_TABLE's records.
// It holds position across multiple FETCH calls so large result sets
// can be paginated without re-scanning from the beginning each time.
//
// IBM i equivalent: DECLARE CURSOR / OPEN / FETCH / CLOSE (RPG CHAIN/READE).
//
// Cursor lifecycle:
//   POST /api/cursor/open  → cursor_id   (like DECLARE CURSOR + OPEN)
//   GET  /api/cursor/fetch?id=N&n=20     (like FETCH NEXT N ROWS)
//   DELETE /api/cursor/<id>              (like CLOSE)
//
// Filtering (WHERE equivalent):
//   where_field + where_value — only fields whose key suffix matches
//   where_field AND whose value matches where_value are returned.
//   Leave where_value empty to iterate ALL fields with that suffix.
//
// Ordering:
//   order_field — if non-empty, results come from the matching index
//   (sorted ascending).  If empty, records are returned in insertion order.
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define CURSOR_MAX  8    // max simultaneously open cursors

// ─── One cursor ───────────────────────────────────────────────────────────────
struct SLSCursor {
    uint32_t cursor_id;                    // opaque handle returned to client
    char     table_name[OBJECT_NAME_LEN];  // table being scanned
    char     where_field[RECORD_KEY_LEN];  // field suffix filter (empty = all)
    char     where_value[RECORD_VAL_LEN];  // value filter (empty = no value check)
    char     order_index[OBJECT_NAME_LEN]; // index name for ordered scan (empty = natural)
    uint32_t position;                     // next field slot to examine (0-based)
    uint32_t index_pos;                    // position within the index (ordered scan)
    uint8_t  done;     // 1 = exhausted, no more rows
    uint8_t  active;
};

// ─── Globals ──────────────────────────────────────────────────────────────────
extern struct SLSCursor cursor_table[CURSOR_MAX];
extern uint32_t         cursor_next_id;   // monotonic ID generator

// ─── Lifecycle ────────────────────────────────────────────────────────────────
void     cursor_mgr_init(void);

// Open a cursor.  Returns the cursor_id (>0) or 0 on error.
uint32_t cursor_open(const char* table_name,
                     const char* where_field,
                     const char* where_value,
                     const char* order_index);

// Close and free a cursor by ID.
int      cursor_close(uint32_t cursor_id);

// ─── Fetch ────────────────────────────────────────────────────────────────────
// Fetch up to max_rows records into buf[max_buf] as a JSON object:
//   {"id":N,"rows":[{"key":"...","value":"..."},...], "done":true/false}
// Returns bytes written.
int      cursor_fetch(uint32_t cursor_id, uint32_t max_rows,
                      char* buf, int max_buf);

// ─── Serialise ────────────────────────────────────────────────────────────────
// List all open cursors as JSON.
int      cursors_to_json(char* buf, int max);

#endif /* CURSOR_H */
