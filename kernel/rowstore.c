/*
 * rowstore.c — Phase 16 (relational layer) row-set storage engine.
 * See rowstore.h for the full design writeup.
 */
#include "rowstore.h"
#include "object_catalog.h"
#include "frame_pool.h"
#include "kernel_io.h"
#include "persist.h"
#include "row_index.h"
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
static uint32_t rowstore_alloc_page(void) {
    if (rowstore_next_free_page_id >= ROWSTORE_MAX_PAGES) return ROWSTORE_INVALID_PAGE;
    uint32_t id = rowstore_next_free_page_id;

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) return ROWSTORE_INVALID_PAGE;   // don't advance the cursor on failure
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
        uint32_t new_page = rowstore_alloc_page();
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
