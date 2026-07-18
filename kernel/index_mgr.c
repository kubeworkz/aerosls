#include "index_mgr.h"
#include "kernel_io.h"

// ─── Globals ──────────────────────────────────────────────────────────────────
struct SLSIndex index_store[INDEX_MAX];
uint32_t        index_count = 0;

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void ix_cpy(char* dst, const char* src, int n) {
    int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int ix_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int ix_cmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static struct SLSIndex* find_index(const char* name) {
    for (uint32_t i = 0; i < INDEX_MAX; i++)
        if (index_store[i].active && ix_eq(index_store[i].index_name, name))
            return &index_store[i];
    return 0;
}

static struct SLSIndex* find_index_for_table_field(const char* table,
                                                     const char* field) {
    for (uint32_t i = 0; i < INDEX_MAX; i++) {
        struct SLSIndex* ix = &index_store[i];
        if (ix->active && ix_eq(ix->table_name, table) &&
            ix_eq(ix->field_name, field))
            return ix;
    }
    return 0;
}

// Check if a record key matches the indexed field.
// Accepts: exact match ("dept") OR suffix match ("alice_dept" → field "dept").
static int key_matches_field(const char* key, const char* field) {
    if (ix_eq(key, field)) return 1;  // exact: key == "dept"
    // Suffix: key ends with "_dept"
    int klen = 0; while (key[klen])   klen++;
    int flen = 0; while (field[flen]) flen++;
    if (klen > flen + 1 && key[klen - flen - 1] == '_')
        return ix_eq(key + klen - flen, field);
    return 0;
}

// Insert a new entry into the sorted array (insertion sort by value).
static void ix_sorted_insert(struct SLSIndex* ix,
                              const char* value, const char* rec_key) {
    if (ix->count >= INDEX_ENTRIES_MAX) return;

    // Find insertion point (binary-ish, but with small n just scan)
    int pos = (int)ix->count;
    for (int i = 0; i < (int)ix->count; i++) {
        if (!ix->entries[i].active) continue;
        if (ix_cmp(value, ix->entries[i].value) <= 0) { pos = i; break; }
    }

    // Shift entries right to make room
    for (int i = (int)ix->count; i > pos; i--)
        ix->entries[i] = ix->entries[i - 1];

    ix_cpy(ix->entries[pos].value,   value,   (int)sizeof(ix->entries[pos].value));
    ix_cpy(ix->entries[pos].rec_key, rec_key, (int)sizeof(ix->entries[pos].rec_key));
    ix->entries[pos].active = 1;
    ix->count++;
}

// Remove an entry by rec_key (shift left to maintain sort order).
static void ix_remove_by_rec_key(struct SLSIndex* ix, const char* rec_key) {
    for (int i = 0; i < (int)ix->count; i++) {
        if (!ix->entries[i].active) continue;
        if (!ix_eq(ix->entries[i].rec_key, rec_key)) continue;
        // Shift left
        for (int j = i; j < (int)ix->count - 1; j++)
            ix->entries[j] = ix->entries[j + 1];
        ix->entries[ix->count - 1].active = 0;
        ix->count--;
        return;
    }
}

// ─── index_mgr_init ──────────────────────────────────────────────────────────
void index_mgr_init(void) {
    for (int i = 0; i < INDEX_MAX; i++) index_store[i].active = 0;
    index_count = 0;
    kernel_serial_print("[INDEX] Secondary index manager initialised.\n");
}

// ─── index_create ─────────────────────────────────────────────────────────────
int index_create(const char* index_name,
                 const char* table_name,
                 const char* field_name)
{
    // Reject duplicates
    if (find_index(index_name)) {
        kernel_serial_print("[INDEX] already exists\n");
        return 1;
    }
    if (index_count >= INDEX_MAX) {
        kernel_serial_print("[INDEX] max indexes reached\n");
        return 1;
    }

    // Find a free slot
    struct SLSIndex* ix = 0;
    for (int i = 0; i < INDEX_MAX; i++) {
        if (!index_store[i].active) { ix = &index_store[i]; break; }
    }
    if (!ix) return 1;

    ix_cpy(ix->index_name, index_name, (int)sizeof(ix->index_name));
    ix_cpy(ix->table_name, table_name, (int)sizeof(ix->table_name));
    ix_cpy(ix->field_name, field_name, (int)sizeof(ix->field_name));
    ix->count  = 0;
    ix->active = 1;
    index_count++;

    kernel_serial_print("[INDEX] created: ");
    kernel_serial_print(index_name);
    kernel_serial_print(" on ");
    kernel_serial_print(table_name);
    kernel_serial_print(".");
    kernel_serial_print(field_name);
    kernel_serial_print("\n");

    // Build from existing records
    index_rebuild(index_name);
    return 0;
}

// ─── index_drop ──────────────────────────────────────────────────────────────
int index_drop(const char* index_name) {
    struct SLSIndex* ix = find_index(index_name);
    if (!ix) return 1;
    ix->active = 0;
    ix->count  = 0;
    if (index_count) index_count--;
    kernel_serial_print("[INDEX] dropped: ");
    kernel_serial_print(index_name);
    kernel_serial_print("\n");
    return 0;
}

// ─── index_rebuild ────────────────────────────────────────────────────────────
// Re-scan the parent table and rebuild the index from scratch.
void index_rebuild(const char* index_name) {
    struct SLSIndex* ix = find_index(index_name);
    if (!ix) return;

    // Reset entries
    for (int i = 0; i < INDEX_ENTRIES_MAX; i++) ix->entries[i].active = 0;
    ix->count = 0;

    // Walk the object catalog to find the parent table
    for (uint32_t oi = 0; oi < object_catalog_count; oi++) {
        if (!object_catalog[oi].active) continue;
        if (!ix_eq(object_catalog[oi].name, ix->table_name)) continue;

        struct SLSObjectRecord* rec = &object_records[oi];
        for (uint32_t fi = 0; fi < RECORD_MAX_FIELDS; fi++) {
            if (!rec->fields[fi].active) continue;
            // The indexed field is the KEY of each record field (the field name)
            // We index by the field KEY matching the index's field_name, and the
            // VALUE is what gets sorted.
            if (key_matches_field(rec->fields[fi].key, ix->field_name))
                ix_sorted_insert(ix, rec->fields[fi].value,
                                    rec->fields[fi].key);
        }
        break;
    }
    kernel_serial_print("[INDEX] rebuilt: ");
    kernel_serial_print(index_name);
    kernel_serial_print("\n");
}

// ─── Auto-maintenance hooks ───────────────────────────────────────────────────
void index_on_insert(const char* table_name, const char* rec_key,
                     const char* field_name, const char* value)
{
    struct SLSIndex* ix = find_index_for_table_field(table_name, field_name);
    if (!ix) return;
    ix_sorted_insert(ix, value, rec_key);
}

void index_on_update(const char* table_name, const char* rec_key,
                     const char* field_name,
                     const char* old_value, const char* new_value)
{
    // Find all indexes affected by this field update
    for (int idx = 0; idx < INDEX_MAX; idx++) {
        struct SLSIndex* ix = &index_store[idx];
        if (!ix->active) continue;
        if (!ix_eq(ix->table_name, table_name)) continue;
        if (!key_matches_field(field_name, ix->field_name)) continue;

        // Remove old entry, insert updated value
        for (int i = 0; i < (int)ix->count; i++) {
            if (ix->entries[i].active &&
                ix_eq(ix->entries[i].rec_key, rec_key) &&
                ix_eq(ix->entries[i].value,   old_value)) {
                for (int j = i; j < (int)ix->count - 1; j++)
                    ix->entries[j] = ix->entries[j + 1];
                ix->entries[ix->count - 1].active = 0;
                ix->count--;
                break;
            }
        }
        ix_sorted_insert(ix, new_value, rec_key);
    }
    (void)old_value;
}

void index_on_delete(const char* table_name, const char* rec_key)
{
    // Remove from all indexes on this table
    for (int i = 0; i < INDEX_MAX; i++) {
        if (index_store[i].active &&
            ix_eq(index_store[i].table_name, table_name))
            ix_remove_by_rec_key(&index_store[i], rec_key);
    }
}

// ─── index_lookup ─────────────────────────────────────────────────────────────
int index_lookup(const char* index_name, const char* value,
                 char* rec_key_out)
{
    struct SLSIndex* ix = find_index(index_name);
    if (!ix || !ix->count) return 0;

    // Binary search on sorted entries
    int lo = 0, hi = (int)ix->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (!ix->entries[mid].active) { lo = mid + 1; continue; }
        int c = ix_cmp(value, ix->entries[mid].value);
        if (c == 0) {
            if (rec_key_out)
                ix_cpy(rec_key_out, ix->entries[mid].rec_key, RECORD_KEY_LEN);
            return 1;
        } else if (c < 0) hi = mid - 1;
        else               lo = mid + 1;
    }
    return 0;
}

// ─── index_range_scan ────────────────────────────────────────────────────────
void index_range_scan(const char* index_name,
                      const char* start_value,
                      IndexScanCb cb)
{
    struct SLSIndex* ix = find_index(index_name);
    if (!ix || !cb) return;

    // Find first entry >= start_value via binary search
    int lo = 0, hi = (int)ix->count - 1, start = (int)ix->count;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (!ix->entries[mid].active) { lo = mid + 1; continue; }
        if (ix_cmp(ix->entries[mid].value, start_value) < 0)
            lo = mid + 1;
        else
            { start = mid; hi = mid - 1; }
    }

    // Iterate from start
    for (int i = start; i < (int)ix->count; i++) {
        if (!ix->entries[i].active) continue;
        if (!cb(ix->entries[i].rec_key, ix->entries[i].value)) break;
    }
}

// ─── JSON serialisation ───────────────────────────────────────────────────────
int index_to_json(const char* index_name, char* buf, int max) {
    struct SLSIndex* ix = find_index(index_name);
    int n = 0;
    #define JW(s)  do { const char* _s=(s); while(*_s && n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c) do { if (n<max-2) buf[n++]=(c); } while(0)
    #define JWU(v) do { \
        char _t[12]; int _l=0; uint32_t _v=(v); \
        if(!_v){_t[_l++]='0';} else{while(_v){_t[_l++]=(char)('0'+_v%10);_v/=10;}} \
        for(int _i=_l-1;_i>=0&&n<max-2;_i--) buf[n++]=_t[_i]; \
    } while(0)

    if (!ix) { if (n < max) buf[n++] = '['; if (n < max) buf[n++] = ']'; if (n < max) buf[n] = '\0'; return n; }

    JW("{\"name\":\"");  JW(ix->index_name); JW("\",");
    JW("\"table\":\"");  JW(ix->table_name); JW("\",");
    JW("\"field\":\"");  JW(ix->field_name); JW("\",");
    JW("\"entries\":[");
    int first = 1;
    for (uint32_t i = 0; i < ix->count; i++) {
        if (!ix->entries[i].active) continue;
        if (!first) JWC(',');
        first = 0;
        JW("{\"value\":\""); JW(ix->entries[i].value); JW("\",");
        JW("\"key\":\"");    JW(ix->entries[i].rec_key); JW("\"}");
    }
    JW("]}");
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    #undef JWU
    return n;
}

int indexes_to_json(char* buf, int max) {
    int n = 0;
    #define JW(s) do { const char* _s=(s); while(*_s && n<max-2) buf[n++]=*_s++; } while(0)
    #define JWC(c) do { if (n<max-2) buf[n++]=(c); } while(0)

    JWC('[');
    int first = 1;
    for (int i = 0; i < INDEX_MAX; i++) {
        struct SLSIndex* ix = &index_store[i];
        if (!ix->active) continue;
        if (!first) JWC(',');
        first = 0;
        JW("{\"name\":\""); JW(ix->index_name); JW("\",");
        JW("\"table\":\""); JW(ix->table_name); JW("\",");
        JW("\"field\":\""); JW(ix->field_name); JW("\",");
        JW("\"entries\":"); 
        // write entry count as number
        char _t[12]; int _l=0; uint32_t _v=ix->count;
        if(!_v){_t[_l++]='0';} else{while(_v){_t[_l++]=(char)('0'+_v%10);_v/=10;}}
        for(int _i=_l-1;_i>=0&&n<max-2;_i--) buf[n++]=_t[_i];
        JWC('}');
    }
    JWC(']');
    if (n < max) buf[n] = '\0';

    #undef JW
    #undef JWC
    return n;
}
