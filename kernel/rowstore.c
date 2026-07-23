/*
 * rowstore.c — Phase 16 (relational layer) row-set storage engine.
 * See rowstore.h for the full design writeup.
 */
#include "rowstore.h"
#include "object_catalog.h"
#include "frame_pool.h"
#include "storage_quota.h"    // Storage Isolation Roadmap Phase 1: per-partition on-disk page quota
#include "kernel_io.h"
#include "persist.h"
#include "row_index.h"
#include "row_constraint.h"   // Phase 5 (SQL Feature-Parity Roadmap, DDL): rowstore_drop_table() cleanup
#include "row_journal.h"      // Phase 5: rowstore_drop_table() cleanup
#include "../drivers/nvme_io.h"
#include "../user/permissions.h"
#include <stddef.h>

struct RowTableHeader table_headers[ROWSTORE_MAX_TABLES];
uint32_t              rowstore_next_free_page_id = 0;

// RAM cache of loaded pages — NULL = not loaded. Not persisted directly;
// pages restore lazily on first access, mirroring stream.c's frames[].
static uint8_t* row_pages[ROWSTORE_MAX_PAGES];

// ─── String / parsing helpers (no libc, same discipline as every other
// kernel source file) ───────────────────────────────────────────────────────
static uint32_t rs_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int rs_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void rs_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static void rs_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void rs_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}

// Strict unsigned-decimal parse — rejects empty strings, non-digit chars,
// and a leading sign (UINT64 columns are unsigned; use FLOAT for signed
// values in this first cut, same "narrow on purpose" posture as everything
// else new in this phase).
static int rs_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

// Minimal signed decimal float parse: [-]digits[.digits]. No exponents, no
// inf/nan — deliberately narrow, matching this phase's stated non-goal of a
// general expression language (that's Phase 18).
static int rs_parse_f64(const char* s, double* out) {
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

static int rs_parse_bool(const char* s, uint8_t* out) {
    if (rs_streq(s, "true") || rs_streq(s, "1"))  { *out = 1; return 0; }
    if (rs_streq(s, "false") || rs_streq(s, "0")) { *out = 0; return 0; }
    return 1;
}

static void rs_u64_to_str(uint64_t v, char* buf, uint32_t max) {
    char tmp[24]; int l = 0;
    if (!v) tmp[l++] = '0';
    else while (v) { tmp[l++] = (char)('0' + v % 10); v /= 10; }
    uint32_t n = 0;
    for (int i = l - 1; i >= 0 && n < max - 1; i--) buf[n++] = tmp[i];
    buf[n] = '\0';
}

// Fixed 6-decimal-place formatter — enough to round-trip a value inserted
// via rs_parse_f64 for this phase's purposes; not a general float printer.
static void rs_f64_to_str(double v, char* buf, uint32_t max) {
    uint32_t n = 0;
    if (v < 0) { if (n < max - 1) buf[n++] = '-'; v = -v; }
    uint64_t ip = (uint64_t)v;
    double frac = v - (double)ip;
    char ipbuf[24];
    rs_u64_to_str(ip, ipbuf, sizeof(ipbuf));
    for (uint32_t i = 0; ipbuf[i] && n < max - 1; i++) buf[n++] = ipbuf[i];
    if (n < max - 1) buf[n++] = '.';
    for (int d = 0; d < 6 && n < max - 1; d++) {
        frac *= 10.0;
        int digit = (int)frac;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        buf[n++] = (char)('0' + digit);
        frac -= (double)digit;
    }
    buf[n] = '\0';
}

// ─── Column width / layout ──────────────────────────────────────────────────
static uint32_t column_width(SLSFieldType t) {
    switch (t) {
        case FIELD_TYPE_UINT64: return 8;
        case FIELD_TYPE_FLOAT:  return 8;
        case FIELD_TYPE_BOOL:   return 1;
        // Phase 4: BLOB is stored exactly like STRING in this first cut --
        // same inline slot width, no separate large-object path. See
        // rowstore.h's Phase 4 note.
        case FIELD_TYPE_STRING: case FIELD_TYPE_BLOB: default: return ROWSTORE_STRING_LEN;
    }
}

static int compute_layout(struct SLSObjectSchema* schema, struct RowTableLayout* out) {
    uint32_t active_count = 0;
    for (uint32_t i = 0; i < SCHEMA_MAX_FIELDS; i++)
        if (schema->fields[i].active) active_count++;
    if (active_count == 0 || active_count > ROWSTORE_MAX_COLUMNS) return 1;

    rs_memset(out, 0, sizeof(*out));
    uint32_t col = 0;
    // Phase 4: byte 0 of every row slot is the tombstone/active flag, bytes
    // 1-2 are the null_mask (little-endian uint16_t) -- see rowstore.h's
    // Phase 4 note. Column data starts at byte 3, not byte 1.
    uint32_t offset = 3;
    for (uint32_t i = 0; i < SCHEMA_MAX_FIELDS; i++) {
        if (!schema->fields[i].active) continue;
        out->column_types[col] = schema->fields[i].type;
        rs_strcpy(out->column_names[col], schema->fields[i].key, RECORD_KEY_LEN);
        out->column_offset[col] = offset;
        offset += column_width(schema->fields[i].type);
        col++;
    }
    out->column_count = col;
    out->row_width = offset;
    out->rows_per_page = (ROWSTORE_PAGE_SIZE - 4) / out->row_width;   // -4: page's next_page_id header
    if (out->rows_per_page == 0) return 1;   // row too wide to fit even one per page
    return 0;
}

// ─── Row (de)serialization: text (API boundary) <-> fixed-width binary ─────
static int serialize_row(const struct RowTableLayout* layout,
                         const struct RowValues* values, uint8_t* out /* row_width bytes */) {
    out[0] = 1;   // active
    // Phase 4: persist the null_mask into bytes 1-2 (little-endian), same
    // convention rs_memcpy-of-a-fixed-width-value already uses elsewhere in
    // this file for UINT64/FLOAT columns.
    out[1] = (uint8_t)(values->null_mask & 0xFF);
    out[2] = (uint8_t)((values->null_mask >> 8) & 0xFF);
    for (uint32_t c = 0; c < layout->column_count; c++) {
        uint8_t* dst = out + layout->column_offset[c];
        // Phase 4: a NULL column's text is never parsed -- it may be
        // anything (typically empty) and is meaningless once null_mask
        // says so. Zero the slot instead so a stale/garbage byte pattern
        // never survives into a future read that (incorrectly) ignores
        // the mask.
        if (values->null_mask & (1u << c)) {
            rs_memset(dst, 0, column_width(layout->column_types[c]));
            continue;
        }
        const char* text = values->values[c];
        switch (layout->column_types[c]) {
            case FIELD_TYPE_UINT64: {
                uint64_t v; if (rs_parse_u64(text, &v)) return 1;
                rs_memcpy(dst, &v, 8); break;
            }
            case FIELD_TYPE_FLOAT: {
                double v; if (rs_parse_f64(text, &v)) return 1;
                rs_memcpy(dst, &v, 8); break;
            }
            case FIELD_TYPE_BOOL: {
                uint8_t v; if (rs_parse_bool(text, &v)) return 1;
                dst[0] = v; break;
            }
            case FIELD_TYPE_STRING:
            case FIELD_TYPE_BLOB:
            default: {
                uint32_t len = rs_strlen(text);
                if (len >= ROWSTORE_STRING_LEN) return 1;   // reject, never silently truncate
                rs_memset(dst, 0, ROWSTORE_STRING_LEN);
                rs_memcpy(dst, text, len);
                break;
            }
        }
    }
    return 0;
}

static void deserialize_row(const struct RowTableLayout* layout,
                            const uint8_t* row, struct RowValues* out) {
    out->count = layout->column_count;
    // Phase 4: read the null_mask back out of bytes 1-2.
    out->null_mask = (uint16_t)row[1] | ((uint16_t)row[2] << 8);
    for (uint32_t c = 0; c < layout->column_count; c++) {
        const uint8_t* src = row + layout->column_offset[c];
        char* dst = out->values[c];
        // Phase 4: a NULL column's underlying bytes were zeroed at write
        // time and are never type-decoded -- decoding a zeroed UINT64/FLOAT
        // slot would misleadingly print "0"/"0.0", indistinguishable from a
        // real stored zero. Leave the text empty; callers must check
        // null_mask, not infer NULL-ness from the text.
        if (out->null_mask & (1u << c)) { dst[0] = '\0'; continue; }
        switch (layout->column_types[c]) {
            case FIELD_TYPE_UINT64: {
                uint64_t v; rs_memcpy(&v, src, 8); rs_u64_to_str(v, dst, RECORD_VAL_LEN); break;
            }
            case FIELD_TYPE_FLOAT: {
                double v; rs_memcpy(&v, src, 8); rs_f64_to_str(v, dst, RECORD_VAL_LEN); break;
            }
            case FIELD_TYPE_BOOL: {
                rs_strcpy(dst, src[0] ? "true" : "false", RECORD_VAL_LEN); break;
            }
            case FIELD_TYPE_STRING:
            case FIELD_TYPE_BLOB:
            default: {
                rs_strcpy(dst, (const char*)src, RECORD_VAL_LEN); break;
            }
        }
    }
}

// ─── Page pool ───────────────────────────────────────────────────────────────
// Loads page_id into RAM (allocating a fresh frame first if needed),
// lazily reading it from NVMe if it's a previously-allocated page this
// boot hasn't touched yet — mirrors stream_lazy_load_frame() exactly.
static uint8_t* rowstore_load_page(uint32_t page_id) {
    if (page_id >= ROWSTORE_MAX_PAGES) return 0;
    if (row_pages[page_id]) return row_pages[page_id];

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) return 0;

    if (page_id < rowstore_next_free_page_id) {
        // Previously allocated (this boot or a prior one) -- may have real
        // data on NVMe. Errors are swallowed the same way persist.c's
        // persist_read_array() does: the caller sees whatever's in the
        // zeroed frame, detectable as "no valid rows" rather than a crash.
        rs_memset(frame, 0, ROWSTORE_PAGE_SIZE);
        nvme_read_sync(ROWSTORE_LBA_BASE + (uint64_t)page_id * 8, frame);
    } else {
        rs_memset(frame, 0, ROWSTORE_PAGE_SIZE);
        uint32_t invalid = ROWSTORE_INVALID_PAGE;
        rs_memcpy(frame, &invalid, 4);
    }
    row_pages[page_id] = frame;
    return frame;
}

static void rowstore_flush_page(uint32_t page_id) {
    if (page_id >= ROWSTORE_MAX_PAGES || !row_pages[page_id]) return;
    nvme_write_sync(ROWSTORE_LBA_BASE + (uint64_t)page_id * 8, row_pages[page_id]);
}

// Allocates a brand-new page from the shared bump-allocator pool. No
// reclaim in this first cut -- see rowstore.h's design comment.
//
// Storage Isolation Roadmap Phase 1: now quota-checked via storage_quota.c's
// combined rowstore+vecstore per-partition page budget. The check happens
// FIRST, before the cursor advances or any RAM frame is touched -- same
// "denial before any side effect" posture as allocate_physical_ram_frame_
// for_partition(). partition_id comes from the caller (the owning object_
// catalog entry), not resolved here, matching how every other quota-checked
// allocator in this codebase takes the id as a parameter rather than
// re-deriving it.
static uint32_t rowstore_alloc_page(uint32_t partition_id) {
    if (rowstore_next_free_page_id >= ROWSTORE_MAX_PAGES) return ROWSTORE_INVALID_PAGE;
    if (storage_page_reserve(partition_id)) return ROWSTORE_INVALID_PAGE;   // over quota -- fail cleanly, cursor untouched
    uint32_t id = rowstore_next_free_page_id;

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) {
        storage_page_release(partition_id, 1);   // roll back the reservation -- this page never actually happened
        return ROWSTORE_INVALID_PAGE;   // don't advance the cursor on failure
    }
    rs_memset(frame, 0, ROWSTORE_PAGE_SIZE);
    uint32_t invalid = ROWSTORE_INVALID_PAGE;
    rs_memcpy(frame, &invalid, 4);
    row_pages[id] = frame;

    rowstore_next_free_page_id++;
    return id;
}

// ─── Table lookup ────────────────────────────────────────────────────────────
static int find_active_table(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (rs_streq(object_catalog[i].name, name)) return (int)i;
    }
    return -1;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────
void rowstore_init(void) {
    rs_memset(table_headers, 0, sizeof(table_headers));
    rs_memset(row_pages, 0, sizeof(row_pages));
    rowstore_next_free_page_id = 0;
    kernel_serial_print("[ROWSTORE] Row-set storage engine initialised.\n");
}

int rowstore_create_table(const char* table_name) {
    int idx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (rs_streq(object_catalog[i].name, table_name)) { idx = (int)i; break; }
    }
    if (idx < 0) return 1;
    if (object_catalog[idx].uses_rowstore) return 1;

    struct RowTableLayout layout;
    if (compute_layout(&object_schemas[idx], &layout)) return 1;

    struct RowTableHeader* h = &table_headers[idx];
    rs_memset(h, 0, sizeof(*h));
    h->object_id         = object_catalog[idx].object_id;
    h->active             = 1;
    h->layout             = layout;
    h->first_page_id       = ROWSTORE_INVALID_PAGE;
    h->last_page_id        = ROWSTORE_INVALID_PAGE;

    object_catalog[idx].uses_rowstore = 1;

    persist_rowstore_headers();
    persist_catalog();   // uses_rowstore lives on SLSObjectEntry -- a separate array/persist call

    kernel_serial_printf(
        "[ROWSTORE] '%s' enabled: %u column(s), row_width=%u, %u row(s)/page.\n",
        table_name, layout.column_count, layout.row_width, layout.rows_per_page);
    return 0;
}

// ─── Phase B: live reachability adapter ─────────────────────────────────────
uint64_t sys_sls_rowstore_create_table(struct SLSRowstoreCreateTableRequest* req) {
    if (!req) return 1;
    req->status = rowstore_create_table(req->table_name);
    return (uint64_t)req->status;
}

// Phase 5 (SQL Feature-Parity Roadmap, DDL): drops a row-set table -- real
// cleanup, not just sys_sls_vfree()'s bare catalog deactivation.
// sys_sls_vfree() alone (object_catalog.c) only clears object_catalog[]
// .active/object_records[] -- confirmed by direct read, not assumed -- it
// has zero awareness of table_headers[], row_indexes[], row_constraints[],
// or row_journal_attachments[], every one of which keys off this table's
// object_id/name and would otherwise dangle forever after a bare vfree.
// This function is the one real teardown path: deactivates every
// row_indexes[]/row_constraints[]/row_journal_attachments[] entry that
// references this table, deactivates table_headers[idx] itself, then
// calls sys_sls_vfree() for the catalog-level cleanup it already does
// correctly (vecstore notification, .active=0, persist_catalog()).
//
// Old page data is abandoned, not reclaimed -- matching this whole
// subsystem's pre-existing "no reclaim in first cut" posture
// (rowstore_alloc_page() has never freed a page; this isn't a new gap
// DROP TABLE introduces).
//
// Deliberately does NOT check whether another table's REFERENCE
// constraint still points at this one (unlike row_constraint_check_
// delete()'s own RESTRICT semantics for individual row deletes) -- named
// explicitly as an out-of-scope gap, not a silent one: dropping a table
// still referenced by another table's FK leaves that FK's ref_table_
// object_id dangling. A future phase could add the same RESTRICT check
// row deletes already have.
//
// Returns 0 on success. Non-zero: 1 = table not found / not a row-set
// table. 2 = permission denied (caller_uid needs PERM_WRITE).
int rowstore_drop_table(uint32_t caller_uid, const char* table_name) {
    int idx = find_active_table(table_name);
    if (idx < 0) return 1;
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return 2;

    uint64_t table_object_id = object_catalog[idx].object_id;

    for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
        if (row_indexes[i].active && row_indexes[i].table_object_id == table_object_id)
            row_indexes[i].active = 0;
    }
    for (uint32_t i = 0; i < ROW_CONSTRAINT_MAX; i++) {
        if (row_constraints[i].active && row_constraints[i].table_object_id == table_object_id)
            row_constraints[i].active = 0;
    }
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++) {
        if (row_journal_attachments[i].active && rs_streq(row_journal_attachments[i].table_name, table_name))
            row_journal_attachments[i].active = 0;
    }

    table_headers[idx].active = 0;
    object_catalog[idx].uses_rowstore = 0;

    persist_row_index_defs();
    persist_row_constraints();
    persist_row_journal();
    persist_rowstore_headers();

    kernel_serial_printf("[ROWSTORE] '%s' dropped: table_headers/row_indexes/row_constraints/"
                         "row_journal_attachments cleaned up.\n", table_name);
    sys_sls_vfree(table_name);   // clears object_catalog[idx].active, persists catalog
    return 0;
}

// ROWSTORE_ADD_COLUMN_MAX_ROWS: a real, named bound on how many rows
// rowstore_add_column() can migrate in one call -- see its own header
// comment for why a full rewrite is unavoidable and why this codebase's
// freestanding, no-dynamic-allocation kernel needs a fixed-size scratch
// buffer rather than an unbounded one. Generous for this codebase's own
// test/dev scale (existing host tests never exceed a few dozen rows per
// table) but a real limit, not silently unbounded -- matching this whole
// project's "narrow on purpose, name it" posture (row_index.h's own
// BTREE_MAX_DUPES_PER_KEY is the same kind of named ceiling).
#define ROWSTORE_ADD_COLUMN_MAX_ROWS 4096

// Phase 5 (SQL Feature-Parity Roadmap, DDL): adds a new column to an
// existing row-set table, live. A real storage-layout migration, not a
// cheap metadata-only change: row_width changes, and rowstore's page
// format packs exactly rows_per_page rows of exactly row_width bytes each
// per page (confirmed by direct read of rowstore_row_insert()'s slot
// arithmetic) -- there is no way to widen a row in place. Every existing
// row is re-read under the OLD layout, given a real NULL (Phase 4
// null_mask bit set) for the new column, and rewritten into freshly
// allocated pages at the NEW width. This also means every row's RowId
// (page_id, slot_index) changes -- so any row_index defined on this table
// is transparently rebuilt afterward (deactivate the old definition, then
// call row_index_create() again with the same name/table/column, which
// re-derives the whole B-tree from the migrated data) rather than trying
// to incrementally patch stale (page_id, slot_index) references. This
// reuses the exact rebuild-not-patch pattern persist.c's own boot-restore
// path already established for indexes (see persist_restore_all()'s
// row-index restore block).
//
// Old pages are abandoned, not freed, matching rowstore_drop_table()'s own
// note just above -- this subsystem has no page-free/reuse mechanism at
// all yet. This means ADD COLUMN leaks the old page allocation
// permanently; named explicitly as a real, deliberate scope cut, not an
// oversight. A future page-reclamation phase (mirroring frame_pool.c's
// own Phase 13 physical memory quota work) would need to land before this
// stops being acceptable for anything beyond dev/test scale.
//
// Returns: 0 success. 1 = table not found/not a row-set table. 2 =
// permission denied. 3 = column name already exists on this table. 4 =
// column_count already at ROWSTORE_MAX_COLUMNS, or the underlying
// object_schemas[] field_count already at SCHEMA_MAX_FIELDS. 6 = page
// pool exhausted during migration. 7 = row count exceeds
// ROWSTORE_ADD_COLUMN_MAX_ROWS.
static struct RowValues g_add_column_scratch[ROWSTORE_ADD_COLUMN_MAX_ROWS];
static uint32_t         g_add_column_scratch_count;
static uint8_t          g_add_column_scratch_overflow;

static void add_column_collect_cb(struct RowId id, const struct RowValues* values, void* ctx) {
    (void)id; (void)ctx;
    if (g_add_column_scratch_count >= ROWSTORE_ADD_COLUMN_MAX_ROWS) {
        g_add_column_scratch_overflow = 1;
        return;
    }
    g_add_column_scratch[g_add_column_scratch_count++] = *values;
}

int rowstore_add_column(uint32_t caller_uid, const char* table_name,
                        const char* column_name, SLSFieldType column_type) {
    int idx = find_active_table(table_name);
    if (idx < 0) return 1;
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return 2;

    struct RowTableLayout* old_layout = &table_headers[idx].layout;
    if (old_layout->column_count >= ROWSTORE_MAX_COLUMNS) return 4;
    for (uint32_t c = 0; c < old_layout->column_count; c++)
        if (rs_streq(old_layout->column_names[c], column_name)) return 3;

    struct SLSObjectSchema* schema = &object_schemas[idx];
    uint32_t free_field = SCHEMA_MAX_FIELDS;
    for (uint32_t i = 0; i < SCHEMA_MAX_FIELDS; i++) {
        if (!schema->fields[i].active) { if (free_field == SCHEMA_MAX_FIELDS) free_field = i; continue; }
        if (rs_streq(schema->fields[i].key, column_name)) return 3;
    }
    if (free_field == SCHEMA_MAX_FIELDS) return 4;

    // 1. Collect every existing row under the OLD layout, before anything
    //    about the table's schema/layout changes.
    g_add_column_scratch_count = 0;
    g_add_column_scratch_overflow = 0;
    rowstore_table_scan(0, table_name, add_column_collect_cb, 0);
    if (g_add_column_scratch_overflow) return 7;

    // 2. Add the new field to object_schemas[idx] directly -- bypassing
    //    sys_sls_schema_set()'s own "row-set tables are frozen post-
    //    promotion" guard on purpose: that guard exists because schema_set
    //    has no idea how to migrate live row data, which is exactly the
    //    gap this function closes. Direct object_schemas[] mutation from
    //    inside rowstore.c matches this file's own existing precedent
    //    (rowstore_create_table() above reads object_schemas[idx] directly
    //    too, just never wrote to it before now).
    rs_strcpy(schema->fields[free_field].key, column_name, RECORD_KEY_LEN);
    schema->fields[free_field].type = column_type;
    schema->fields[free_field].active = 1;
    if (schema->field_count <= free_field) schema->field_count = free_field + 1;

    // 3. Recompute the layout from the now-extended schema.
    struct RowTableLayout new_layout;
    if (compute_layout(schema, &new_layout)) {
        // Roll back the schema field on failure -- leave the table exactly
        // as it was, not half-migrated.
        schema->fields[free_field].active = 0;
        return 4;
    }
    uint32_t new_col = new_layout.column_count - 1;   // compute_layout() appends in schema-array order

    // 4. Rewrite every collected row into a fresh page chain under the new
    //    layout -- a scratch header, not table_headers[idx] itself, so the
    //    live table stays queryable under its OLD layout/pages until the
    //    swap at the very end.
    struct RowTableHeader migrate_hdr;
    rs_memset(&migrate_hdr, 0, sizeof(migrate_hdr));
    migrate_hdr.object_id     = object_catalog[idx].object_id;
    migrate_hdr.active        = 1;
    migrate_hdr.layout        = new_layout;
    migrate_hdr.first_page_id = ROWSTORE_INVALID_PAGE;
    migrate_hdr.last_page_id  = ROWSTORE_INVALID_PAGE;

    for (uint32_t i = 0; i < g_add_column_scratch_count; i++) {
        struct RowValues v = g_add_column_scratch[i];
        v.count = new_layout.column_count;
        v.values[new_col][0] = '\0';
        v.null_mask |= (uint16_t)(1u << new_col);   // existing rows get a real NULL for the new column

        uint8_t row_buf[ROWSTORE_MAX_ROW_BYTES];
        if (serialize_row(&new_layout, &v, row_buf)) continue;   // shouldn't happen -- old values were already valid

        if (migrate_hdr.first_page_id == ROWSTORE_INVALID_PAGE ||
            migrate_hdr.rows_in_last_page >= migrate_hdr.layout.rows_per_page) {
            uint32_t new_page = rowstore_alloc_page(object_catalog[idx].partition_id);
            if (new_page == ROWSTORE_INVALID_PAGE) return 6;
            if (migrate_hdr.first_page_id == ROWSTORE_INVALID_PAGE) {
                migrate_hdr.first_page_id = new_page;
            } else {
                uint8_t* prev = rowstore_load_page(migrate_hdr.last_page_id);
                if (prev) { rs_memcpy(prev, &new_page, 4); rowstore_flush_page(migrate_hdr.last_page_id); }
            }
            migrate_hdr.last_page_id = new_page;
            migrate_hdr.rows_in_last_page = 0;
            migrate_hdr.page_count++;
        }
        uint8_t* page = rowstore_load_page(migrate_hdr.last_page_id);
        if (!page) return 6;
        uint32_t slot = migrate_hdr.rows_in_last_page;
        uint8_t* slot_ptr = page + 4 + slot * migrate_hdr.layout.row_width;
        rs_memcpy(slot_ptr, row_buf, migrate_hdr.layout.row_width);
        rowstore_flush_page(migrate_hdr.last_page_id);
        migrate_hdr.rows_in_last_page++;
        migrate_hdr.row_count++;
    }

    // 5. Swap: the live table now points at the new layout/page chain.
    table_headers[idx] = migrate_hdr;
    persist_rowstore_headers();

    kernel_serial_printf(
        "[ROWSTORE] '%s': column '%s' added, %u row(s) migrated to row_width=%u.\n",
        table_name, column_name, migrate_hdr.row_count, new_layout.row_width);

    // 6. Rebuild any row_index defined on this table -- every row's RowId
    //    just changed. Capture (name, column) pairs first (row_index_drop()
    //    inside the loop would otherwise mutate row_indexes[] while a
    //    second pass over it is still in flight).
    char rebuild_name[ROW_INDEX_MAX][OBJECT_NAME_LEN];
    char rebuild_col[ROW_INDEX_MAX][RECORD_KEY_LEN];
    uint32_t rebuild_count = 0;
    for (uint32_t i = 0; i < ROW_INDEX_MAX && rebuild_count < ROW_INDEX_MAX; i++) {
        if (!row_indexes[i].active || row_indexes[i].table_object_id != migrate_hdr.object_id) continue;
        rs_strcpy(rebuild_name[rebuild_count], row_indexes[i].index_name, OBJECT_NAME_LEN);
        uint32_t ci = row_indexes[i].column_index;
        rs_strcpy(rebuild_col[rebuild_count], ci < new_layout.column_count ? new_layout.column_names[ci] : "", RECORD_KEY_LEN);
        rebuild_count++;
    }
    for (uint32_t i = 0; i < rebuild_count; i++) {
        row_index_drop(0, rebuild_name[i]);
        row_index_create(0, rebuild_name[i], table_name, rebuild_col[i]);
    }

    return 0;
}

// ─── Row CRUD ────────────────────────────────────────────────────────────────
int rowstore_row_insert(uint32_t caller_uid, const char* table_name,
                        const struct RowValues* values, struct RowId* out_id) {
    if (!table_name || !values) return 5;
    int idx = find_active_table(table_name);
    if (idx < 0) return 1;
    struct RowTableHeader* h = &table_headers[idx];
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return 2;
    if (values->count != h->layout.column_count) return 4;

    uint8_t row_buf[ROWSTORE_MAX_ROW_BYTES];
    if (serialize_row(&h->layout, values, row_buf)) return 5;

    if (h->first_page_id == ROWSTORE_INVALID_PAGE ||
        h->rows_in_last_page >= h->layout.rows_per_page) {
        uint32_t new_page = rowstore_alloc_page(object_catalog[idx].partition_id);
        if (new_page == ROWSTORE_INVALID_PAGE) return 6;

        if (h->first_page_id == ROWSTORE_INVALID_PAGE) {
            h->first_page_id = new_page;
        } else {
            uint8_t* prev = rowstore_load_page(h->last_page_id);
            if (prev) { rs_memcpy(prev, &new_page, 4); rowstore_flush_page(h->last_page_id); }
        }
        h->last_page_id = new_page;
        h->rows_in_last_page = 0;
        h->page_count++;
    }

    uint8_t* page = rowstore_load_page(h->last_page_id);
    if (!page) return 6;
    uint32_t slot = h->rows_in_last_page;
    uint8_t* slot_ptr = page + 4 + slot * h->layout.row_width;
    rs_memcpy(slot_ptr, row_buf, h->layout.row_width);
    rowstore_flush_page(h->last_page_id);

    h->rows_in_last_page++;
    h->row_count++;

    struct RowId new_id = { h->last_page_id, slot };
    if (out_id) *out_id = new_id;
    persist_rowstore_headers();

    // Phase 17: auto-maintain any B-tree indexes defined on this table. A
    // no-op (single loop, zero iterations) for every table with no indexes
    // -- see row_index.h for why this lives here rather than being an
    // opt-in step a caller has to remember.
    row_index_notify_insert(object_catalog[idx].object_id, new_id, values, &h->layout);
    return 0;
}

int rowstore_row_get(uint32_t caller_uid, const char* table_name,
                     struct RowId id, struct RowValues* out) {
    int idx = find_active_table(table_name);
    if (idx < 0) return 1;
    struct RowTableHeader* h = &table_headers[idx];
    if (!catalog_check_access(caller_uid, table_name, PERM_READ)) return 2;
    if (id.page_id >= rowstore_next_free_page_id) return 3;
    if (id.slot_index >= h->layout.rows_per_page) return 3;

    uint8_t* page = rowstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->layout.row_width;
    if (!slot[0]) return 3;   // tombstoned / never written

    if (out) deserialize_row(&h->layout, slot, out);
    return 0;
}

int rowstore_row_update(uint32_t caller_uid, const char* table_name,
                        struct RowId id, const struct RowValues* values) {
    if (!values) return 5;
    int idx = find_active_table(table_name);
    if (idx < 0) return 1;
    struct RowTableHeader* h = &table_headers[idx];
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return 2;
    if (values->count != h->layout.column_count) return 4;
    if (id.page_id >= rowstore_next_free_page_id) return 3;
    if (id.slot_index >= h->layout.rows_per_page) return 3;

    uint8_t* page = rowstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->layout.row_width;
    if (!slot[0]) return 3;

    uint8_t row_buf[ROWSTORE_MAX_ROW_BYTES];
    if (serialize_row(&h->layout, values, row_buf)) return 5;

    // Phase 17: capture the pre-update values before overwriting the slot,
    // so the index-maintenance hook can remove the OLD indexed key as well
    // as add the new one -- the legacy in-place overwrite below destroys
    // this information, so it must be read first.
    struct RowValues old_values;
    deserialize_row(&h->layout, slot, &old_values);

    rs_memcpy(slot, row_buf, h->layout.row_width);
    rowstore_flush_page(id.page_id);

    row_index_notify_update(object_catalog[idx].object_id, id, &old_values, values, &h->layout);
    return 0;
}

int rowstore_row_delete(uint32_t caller_uid, const char* table_name, struct RowId id) {
    int idx = find_active_table(table_name);
    if (idx < 0) return 1;
    struct RowTableHeader* h = &table_headers[idx];
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return 2;
    if (id.page_id >= rowstore_next_free_page_id) return 3;
    if (id.slot_index >= h->layout.rows_per_page) return 3;

    uint8_t* page = rowstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->layout.row_width;
    if (!slot[0]) return 3;   // already deleted / never written

    // Phase 17: capture the values being removed before tombstoning, so the
    // index-maintenance hook knows which indexed key(s) to remove this
    // row_id from.
    struct RowValues old_values;
    deserialize_row(&h->layout, slot, &old_values);

    slot[0] = 0;   // tombstone -- slot is NOT reclaimed for reuse in this first cut
    rowstore_flush_page(id.page_id);
    h->row_count--;
    persist_rowstore_headers();

    row_index_notify_delete(object_catalog[idx].object_id, id, &old_values, &h->layout);
    return 0;
}

// ─── Scan ────────────────────────────────────────────────────────────────────
uint32_t rowstore_table_scan(uint32_t caller_uid, const char* table_name,
                             RowScanCb cb, void* ctx) {
    int idx = find_active_table(table_name);
    if (idx < 0) return 0;
    struct RowTableHeader* h = &table_headers[idx];
    if (!catalog_check_access(caller_uid, table_name, PERM_READ)) return 0;

    uint32_t visited = 0;
    uint32_t page_id = h->first_page_id;
    while (page_id != ROWSTORE_INVALID_PAGE) {
        uint8_t* page = rowstore_load_page(page_id);
        if (!page) break;
        uint32_t next_page; rs_memcpy(&next_page, page, 4);
        uint32_t limit = (page_id == h->last_page_id) ? h->rows_in_last_page : h->layout.rows_per_page;

        for (uint32_t s = 0; s < limit; s++) {
            uint8_t* slot = page + 4 + s * h->layout.row_width;
            if (!slot[0]) continue;
            struct RowValues vals;
            deserialize_row(&h->layout, slot, &vals);
            struct RowId id = { page_id, s };
            if (cb) cb(id, &vals, ctx);
            visited++;
        }
        page_id = next_page;
    }
    return visited;
}
