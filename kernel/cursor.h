#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>
#include "object_catalog.h"
#include "rowstore.h"

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

// Phase 19 (relational layer): a row-set cursor's result set is pre-
// materialized (already WHERE-filtered/ORDER BY-sorted/LIMIT-truncated by
// the SQL executor before the cursor is even opened — see sql_exec.h),
// then just paginated out via cursor_fetch_rows() the same "position +
// fetch-N-at-a-time + done flag" shape cursor_fetch() already uses for the
// legacy path below. A first-cut, fixed-capacity ceiling, matching this
// whole roadmap's "documented cap, not silent/unbounded" posture elsewhere
// (row_index.h's BTREE_MAX_DUPES_PER_KEY, rowstore.h's ROWSTORE_MAX_PAGES).
#define CURSOR_MAX_ROWSET_ROWS  256

// ─── One cursor ───────────────────────────────────────────────────────────────
// Two modes, sharing one slot pool and the same opaque cursor_id/position/
// done pagination shape, but otherwise independent: is_rowset==0 (the
// default — every field below this line is zero for a legacy cursor,
// backward compatible by construction, same posture every phase since 8
// has used for new struct fields) keeps the ENTIRE legacy field-iteration
// path above exactly as it always was; is_rowset==1 is Phase 19's new
// row-set mode, used only by cursor_open_rowset()/cursor_fetch_rows()
// below. Neither mode's fields are touched by the other's functions.
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

    // ── Phase 19 row-set mode fields (zero-default; unused when is_rowset==0) ──
    uint8_t          is_rowset;
    struct RowValues result_rows[CURSOR_MAX_ROWSET_ROWS];
    uint32_t         result_count;
    uint32_t         result_pos;
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

// ─── Phase 19 row-set mode ────────────────────────────────────────────────
// Opens a cursor over an ALREADY-COMPUTED, already-ordered/limited result
// set (the SQL executor in sql_exec.c does the WHERE/ORDER BY/LIMIT work
// before calling this — this function only materializes and paginates,
// exactly mirroring cursor_open()'s "find a free slot, stamp it, return
// the id" shape). row_count is clamped to CURSOR_MAX_ROWSET_ROWS if the
// caller passes more (documented cap, not a silent truncation the caller
// can't detect — the return value reflects how many rows the cursor
// actually holds). Returns the cursor_id (>0) or 0 if no free slot.
uint32_t cursor_open_rowset(const char* table_name,
                            const struct RowValues* rows, uint32_t row_count);

// Fetch up to max_rows rows from a row-set cursor via a typed callback
// (RowScanCb, reused unmodified from rowstore.h — no new callback type
// invented), the same "position advances, done flips once exhausted"
// pagination shape cursor_fetch() already uses. The RowId passed to cb()
// is always {0,0} for row-set-cursor rows -- the executor's already-
// materialized result rows are values-only (see cursor.h's struct comment
// and sql_exec.h's own note on why projection stops at values, not
// identity). Returns the number of rows delivered this call (0 once done,
// or if cursor_id doesn't name an active row-set cursor).
uint32_t cursor_fetch_rows(uint32_t cursor_id, uint32_t max_rows, RowScanCb cb, void* ctx);

#endif /* CURSOR_H */
