#include "cursor.h"
#include "index_mgr.h"
#include "kernel_io.h"

// ─── Globals ──────────────────────────────────────────────────────────────────
struct SLSCursor cursor_table[CURSOR_MAX];
uint32_t         cursor_next_id = 1;

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void cs_cpy(char* dst, const char* src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int cs_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// Suffix match: "alice_dept" matches field suffix "dept"
static int cs_field_match(const char* key, const char* field) {
    if (!field || !field[0]) return 1;   // empty field = match all
    if (cs_eq(key, field)) return 1;
    int klen = 0; while (key[klen])   klen++;
    int flen = 0; while (field[flen]) flen++;
    if (klen > flen + 1 && key[klen - flen - 1] == '_')
        return cs_eq(key + klen - flen, field);
    return 0;
}

// JSON string escape — write a quoted, escaped string into buf at pos n
static int cs_jstr(char* buf, int max, int n, const char* s) {
    if (n < max - 1) buf[n++] = '"';
    while (*s && n < max - 3) {
        if (*s == '"' || *s == '\\') { buf[n++] = '\\'; }
        buf[n++] = *s++;
    }
    if (n < max - 1) buf[n++] = '"';
    return n;
}

// ─── cursor_mgr_init ─────────────────────────────────────────────────────────
void cursor_mgr_init(void) {
    for (int i = 0; i < CURSOR_MAX; i++) cursor_table[i].active = 0;
    cursor_next_id = 1;
    kernel_serial_print("[CURSOR] Server-side cursor engine initialised.\n");
}

// ─── cursor_open ─────────────────────────────────────────────────────────────
uint32_t cursor_open(const char* table_name,
                     const char* where_field,
                     const char* where_value,
                     const char* order_index)
{
    // Verify table exists
    int found = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && cs_eq(object_catalog[i].name, table_name)) {
            found = 1; break;
        }
    }
    if (!found) {
        kernel_serial_print("[CURSOR] open: table not found\n");
        return 0;
    }

    // Find a free slot
    struct SLSCursor* c = 0;
    for (int i = 0; i < CURSOR_MAX; i++) {
        if (!cursor_table[i].active) { c = &cursor_table[i]; break; }
    }
    if (!c) {
        kernel_serial_print("[CURSOR] max cursors open\n");
        return 0;
    }

    c->cursor_id = cursor_next_id++;
    cs_cpy(c->table_name,  table_name,   (int)sizeof(c->table_name));
    cs_cpy(c->where_field, where_field ? where_field : "",
           (int)sizeof(c->where_field));
    cs_cpy(c->where_value, where_value ? where_value : "",
           (int)sizeof(c->where_value));
    cs_cpy(c->order_index, order_index ? order_index : "",
           (int)sizeof(c->order_index));
    c->position  = 0;
    c->index_pos = 0;
    c->done      = 0;
    c->active    = 1;
    // Phase 19: a reused slot may have last been a row-set cursor -- reset
    // its mode flag explicitly, the same "slot reuse leaves stale fields"
    // lesson Phase 8's partition_id bug and Phase 14's frame-usage reset
    // both taught this project to watch for. result_rows[]/result_count
    // don't need clearing: is_rowset==0 makes them permanently unreachable
    // through this cursor from here on.
    c->is_rowset = 0;

    kernel_serial_print("[CURSOR] opened #");
    kernel_serial_print_hex64(c->cursor_id);
    kernel_serial_print(" on ");
    kernel_serial_print(table_name);
    kernel_serial_print("\n");
    return c->cursor_id;
}

// ─── cursor_close ────────────────────────────────────────────────────────────
int cursor_close(uint32_t cursor_id) {
    for (int i = 0; i < CURSOR_MAX; i++) {
        if (cursor_table[i].active && cursor_table[i].cursor_id == cursor_id) {
            cursor_table[i].active = 0;
            kernel_serial_print("[CURSOR] closed\n");
            return 0;
        }
    }
    return 1;
}

// ─── cursor_fetch ────────────────────────────────────────────────────────────
int cursor_fetch(uint32_t cursor_id, uint32_t max_rows,
                 char* buf, int max_buf)
{
    // Find the cursor
    struct SLSCursor* c = 0;
    for (int i = 0; i < CURSOR_MAX; i++) {
        if (cursor_table[i].active && cursor_table[i].cursor_id == cursor_id) {
            c = &cursor_table[i]; break;
        }
    }

    // Build the JSON result header
    int n = 0;
    #define JW(s) do { const char* _s=(s); while(*_s && n<max_buf-2) buf[n++]=*_s++; } while(0)
    #define JWC(c_) do { if (n<max_buf-2) buf[n++]=(c_); } while(0)
    #define JWU(v) do { \
        char _t[12]; int _l=0; uint32_t _v=(uint32_t)(v); \
        if(!_v){_t[_l++]='0';}else{while(_v){_t[_l++]=(char)('0'+_v%10);_v/=10;}} \
        for(int _i=_l-1;_i>=0&&n<max_buf-2;_i--) buf[n++]=_t[_i]; \
    } while(0)

    JW("{\"id\":"); JWU(cursor_id);

    if (!c) {
        JW(",\"error\":\"cursor not found\",\"rows\":[],\"done\":true}");
        if (n < max_buf) buf[n] = '\0';
        return n;
    }
    if (c->done) {
        JW(",\"rows\":[],\"done\":true}");
        if (n < max_buf) buf[n] = '\0';
        return n;
    }

    JW(",\"rows\":[");

    uint32_t fetched = 0;
    int      first   = 1;

    // ── Ordered scan via index ────────────────────────────────────────────────
    if (c->order_index[0]) {
        struct SLSIndex* ix = 0;
        for (int i = 0; i < INDEX_MAX; i++) {
            // find matching index (extern from index_mgr)
            extern struct SLSIndex index_store[];
            if (index_store[i].active &&
                cs_eq(index_store[i].index_name, c->order_index)) {
                ix = &index_store[i]; break;
            }
        }
        if (ix) {
            // Find the parent table's object record
            struct SLSObjectRecord* rec = 0;
            for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
                if (object_catalog[oi].active &&
                    cs_eq(object_catalog[oi].name, c->table_name)) {
                    rec = &object_records[oi]; break;
                }
            }
            // Iterate index entries from index_pos
            while (c->index_pos < ix->count && fetched < max_rows) {
                struct IndexEntry* ie = &ix->entries[c->index_pos];
                if (!ie->active) { c->index_pos++; continue; }

                // Apply value filter (where_value)
                if (c->where_value[0] && !cs_eq(ie->value, c->where_value)) {
                    c->index_pos++; continue;
                }

                // Emit row using the rec_key to look up the full value
                if (!first) JWC(',');
                first = 0;
                JW("{\"key\":");
                n = cs_jstr(buf, max_buf, n, ie->rec_key);
                JW(",\"value\":");
                n = cs_jstr(buf, max_buf, n, ie->value);
                JWC('}');

                c->index_pos++;
                fetched++;
            }
            if (c->index_pos >= ix->count) c->done = 1;
        } else {
            c->done = 1;  // index not found
        }
    }
    // ── Natural order scan ────────────────────────────────────────────────────
    else {
        for (uint32_t oi = 0; oi < object_catalog_count && fetched < max_rows; oi++) {
            if (!object_catalog[oi].active) continue;
            if (!cs_eq(object_catalog[oi].name, c->table_name)) continue;

            struct SLSObjectRecord* rec = &object_records[oi];
            uint32_t fi = c->position;

            for (; fi < RECORD_MAX_FIELDS && fetched < max_rows; fi++) {
                if (!rec->fields[fi].active) continue;

                // Apply where_field filter (suffix match on key)
                if (!cs_field_match(rec->fields[fi].key, c->where_field))
                    continue;

                // Apply where_value filter
                if (c->where_value[0] &&
                    !cs_eq(rec->fields[fi].value, c->where_value))
                    continue;

                if (!first) JWC(',');
                first = 0;
                JW("{\"key\":");
                n = cs_jstr(buf, max_buf, n, rec->fields[fi].key);
                JW(",\"value\":");
                n = cs_jstr(buf, max_buf, n, rec->fields[fi].value);
                JWC('}');
                fetched++;
            }

            c->position = fi;
            if (fi >= RECORD_MAX_FIELDS) c->done = 1;
            break;
        }
        if (fetched == 0 && !c->done) c->done = 1;  // nothing left
    }

    JW("],\"fetched\":");
    JWU(fetched);
    JW(",\"done\":");
    JW(c->done ? "true" : "false");
    JWC('}');
    if (n < max_buf) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWU
    return n;
}

// ─── cursors_to_json ─────────────────────────────────────────────────────────
int cursors_to_json(char* buf, int max) {
    int n = 0;
    #define JW(s) do { const char* _s=(s); while(*_s && n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c_) do { if (n<max-2) buf[n++]=(c_); } while(0)
    #define JWU(v) do { \
        char _t[12]; int _l=0; uint32_t _v=(uint32_t)(v); \
        if(!_v){_t[_l++]='0';}else{while(_v){_t[_l++]=(char)('0'+_v%10);_v/=10;}} \
        for(int _i=_l-1;_i>=0&&n<max-2;_i--) buf[n++]=_t[_i]; \
    } while(0)

    JWC('[');
    int first = 1;
    for (int i = 0; i < CURSOR_MAX; i++) {
        struct SLSCursor* c = &cursor_table[i];
        if (!c->active) continue;
        if (!first) JWC(',');
        first = 0;
        JW("{\"id\":"); JWU(c->cursor_id);
        JW(",\"table\":\""); JW(c->table_name); JWC('"');
        if (c->where_field[0]) {
            JW(",\"where\":\""); JW(c->where_field); JWC('"');
            if (c->where_value[0]) {
                JW(",\"eq\":\""); JW(c->where_value); JWC('"');
            }
        }
        if (c->order_index[0]) {
            JW(",\"order\":\""); JW(c->order_index); JWC('"');
        }
        JW(",\"pos\":"); JWU(c->position);
        JW(",\"done\":"); JW(c->done ? "true" : "false");
        JWC('}');
    }
    JWC(']');
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWU
    return n;
}

// ─── Phase 19 (relational layer) row-set mode ────────────────────────────
// See cursor.h's struct comment: shares the slot pool and the opaque
// cursor_id/position/done pagination shape with the legacy path above, but
// touches none of the legacy path's fields or logic.
uint32_t cursor_open_rowset(const char* table_name,
                            const struct RowValues* rows, uint32_t row_count) {
    struct SLSCursor* c = 0;
    for (int i = 0; i < CURSOR_MAX; i++) {
        if (!cursor_table[i].active) { c = &cursor_table[i]; break; }
    }
    if (!c) {
        kernel_serial_print("[CURSOR] max cursors open (rowset)\n");
        return 0;
    }

    c->cursor_id = cursor_next_id++;
    cs_cpy(c->table_name, table_name ? table_name : "", (int)sizeof(c->table_name));
    // Legacy-path fields don't apply to a row-set cursor -- reset rather
    // than leave a prior slot occupant's values visible to cursors_to_json.
    c->where_field[0] = '\0';
    c->where_value[0] = '\0';
    c->order_index[0] = '\0';
    c->position  = 0;
    c->index_pos = 0;

    uint32_t n = row_count;
    if (n > CURSOR_MAX_ROWSET_ROWS) n = CURSOR_MAX_ROWSET_ROWS;   // documented cap -- see cursor.h
    for (uint32_t i = 0; i < n; i++) c->result_rows[i] = rows[i];
    c->result_count = n;
    c->result_pos   = 0;

    c->is_rowset = 1;
    c->done      = (n == 0) ? 1 : 0;
    c->active    = 1;

    kernel_serial_print("[CURSOR] opened rowset cursor #");
    kernel_serial_print_hex64(c->cursor_id);
    kernel_serial_print(" on ");
    kernel_serial_print(c->table_name);
    kernel_serial_print("\n");
    return c->cursor_id;
}

uint32_t cursor_fetch_rows(uint32_t cursor_id, uint32_t max_rows, RowScanCb cb, void* ctx) {
    struct SLSCursor* c = 0;
    for (int i = 0; i < CURSOR_MAX; i++) {
        if (cursor_table[i].active && cursor_table[i].cursor_id == cursor_id) {
            c = &cursor_table[i]; break;
        }
    }
    if (!c || !c->is_rowset) return 0;
    if (c->done) return 0;

    struct RowId zero_id = { 0, 0 };   // materialized rows carry no identity -- see cursor.h's note
    uint32_t fetched = 0;
    while (c->result_pos < c->result_count && fetched < max_rows) {
        if (cb) cb(zero_id, &c->result_rows[c->result_pos], ctx);
        c->result_pos++;
        fetched++;
    }
    if (c->result_pos >= c->result_count) c->done = 1;
    return fetched;
}
