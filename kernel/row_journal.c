/*
 * row_journal.c — Phase 23 (relational layer) row-set audit trail. See
 * row_journal.h for the full design writeup.
 */
#include "row_journal.h"
#include "kernel_io.h"
#include "persist.h"   // Gap Remediation Phase D -- persist_row_journal()
#include <stddef.h>

// ─── String helpers (no libc -- rj_* here, matching this codebase's
// established per-file convention). ─────────────────────────────────────
static int rj_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void rj_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static void rj_append(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}
static void rj_append_json_str(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 2) {
        if (*s == '"' || *s == '\\') { if (*pos < max - 2) buf[(*pos)++] = '\\'; }
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}
// Minimal unsigned-decimal formatter (no libc snprintf in a freestanding
// kernel file, matching every other subsystem's own hand-rolled parsing).
static void rj_append_u64(char* buf, int* pos, int max, uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 24) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos < max - 1) buf[(*pos)++] = tmp[--n];
    buf[*pos] = '\0';
}

struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
uint32_t                    row_journal_entry_count;
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
uint32_t                    row_journal_attachment_count;

void row_journal_init(void) {
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ENTRIES; i++) row_journal_buffer[i].active = 0;
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++) row_journal_attachments[i].active = 0;
    row_journal_entry_count = 0;
    row_journal_attachment_count = 0;
}

// Returns the journal_name attached to table_name, or NULL -- mirrors
// journal_for_object()'s own contract.
static const char* rj_journal_for_table(const char* table_name) {
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++) {
        if (row_journal_attachments[i].active && rj_streq(row_journal_attachments[i].table_name, table_name))
            return row_journal_attachments[i].journal_name;
    }
    return NULL;
}

int row_journal_attach(const char* journal_name, const char* table_name) {
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++) {
        if (row_journal_attachments[i].active && rj_streq(row_journal_attachments[i].table_name, table_name))
            return 1;   // already attached to some journal (possibly a different one) -- one journal per table, matching journal.c
    }
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++) {
        if (!row_journal_attachments[i].active) {
            row_journal_attachments[i].active = 1;
            rj_strcpy(row_journal_attachments[i].journal_name, journal_name, ROW_JOURNAL_NAME_LEN);
            rj_strcpy(row_journal_attachments[i].table_name, table_name, OBJECT_NAME_LEN);
            row_journal_attachment_count++;
            persist_row_journal();   // Gap Remediation Phase D
            return 0;
        }
    }
    return 1;   // attachment table full
}

int row_journal_detach(const char* journal_name, const char* table_name) {
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++) {
        if (row_journal_attachments[i].active &&
            rj_streq(row_journal_attachments[i].journal_name, journal_name) &&
            rj_streq(row_journal_attachments[i].table_name, table_name)) {
            row_journal_attachments[i].active = 0;
            persist_row_journal();   // Gap Remediation Phase D
            return 0;
        }
    }
    return 1;
}

static struct RowJournalEntry* rj_alloc_entry(void) {
    // Bump allocator with wraparound (matching journal.c's own "wraps"
    // note on journal_entry_count) -- once the small ring fills, the
    // oldest entry is silently overwritten rather than rejecting new
    // writes, the same tradeoff journal.c's own ring buffer already makes.
    struct RowJournalEntry* e = &row_journal_buffer[row_journal_entry_count % ROW_JOURNAL_MAX_ENTRIES];
    row_journal_entry_count++;
    return e;
}

static void rj_write(uint64_t txn_id, const char* journal_name, const char* table_name, uint64_t table_object_id,
                     uint64_t logical_id, JournalEntryType type,
                     const struct RowValues* before, const struct RowValues* after) {
    struct RowJournalEntry* e = rj_alloc_entry();
    e->active = 1;
    e->seq = row_journal_entry_count;
    e->tx_id = txn_id;
    e->table_object_id = table_object_id;
    rj_strcpy(e->journal_name, journal_name, ROW_JOURNAL_NAME_LEN);
    rj_strcpy(e->table_name, table_name, OBJECT_NAME_LEN);
    e->logical_id = logical_id;
    e->type = type;
    e->committed = 0;   // pending until row_journal_commit_tx()
    if (before) e->before = *before; else e->before.count = 0;
    if (after)  e->after  = *after;  else e->after.count  = 0;

    persist_row_journal();   // Gap Remediation Phase D -- covers all three
                               // notify_insert/update/delete() callers, which
                               // all funnel through this one function
}

void row_journal_notify_insert(uint64_t txn_id, const char* table_name, uint64_t table_object_id,
                               uint64_t logical_id, const struct RowValues* after) {
    const char* jn = rj_journal_for_table(table_name);
    if (!jn) return;
    rj_write(txn_id, jn, table_name, table_object_id, logical_id, JENT_PT, NULL, after);
}
void row_journal_notify_update(uint64_t txn_id, const char* table_name, uint64_t table_object_id,
                               uint64_t logical_id, const struct RowValues* before, const struct RowValues* after) {
    const char* jn = rj_journal_for_table(table_name);
    if (!jn) return;
    rj_write(txn_id, jn, table_name, table_object_id, logical_id, JENT_UB, before, NULL);
    rj_write(txn_id, jn, table_name, table_object_id, logical_id, JENT_UP, NULL, after);
}
void row_journal_notify_delete(uint64_t txn_id, const char* table_name, uint64_t table_object_id,
                               uint64_t logical_id, const struct RowValues* before) {
    const char* jn = rj_journal_for_table(table_name);
    if (!jn) return;
    rj_write(txn_id, jn, table_name, table_object_id, logical_id, JENT_DL, before, NULL);
}

void row_journal_commit_tx(uint64_t txn_id) {
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ENTRIES; i++) {
        if (row_journal_buffer[i].active && row_journal_buffer[i].tx_id == txn_id && !row_journal_buffer[i].committed)
            row_journal_buffer[i].committed = 1;
    }
    persist_row_journal();   // Gap Remediation Phase D
}
void row_journal_rollback_tx(uint64_t txn_id) {
    // Real garbage -- these entries described a mutation that never took
    // effect. Freeing the slots (rather than keeping a JENT_RB-style
    // permanent record, journal.c's own approach) is a deliberate
    // simplification given this ring's much smaller capacity -- see
    // row_journal.h's header comment.
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ENTRIES; i++) {
        if (row_journal_buffer[i].active && row_journal_buffer[i].tx_id == txn_id && !row_journal_buffer[i].committed)
            row_journal_buffer[i].active = 0;
    }
    persist_row_journal();   // Gap Remediation Phase D
}

// Formats one RowValues as "v0|v1|v2" for dump/JSON output -- a plain,
// simple representation, not attempting a full re-typed reconstruction
// (the table's schema, not this journal, is authoritative for types).
//
// Bug fixed during Phase 23 verification: when rv->count == 0 (the
// EXPECTED, common case -- an INSERT's before-image, a DELETE's
// after-image, an UPDATE's UB entry's after-image, etc. are all
// legitimately empty), the loop below never runs and rj_append() is never
// called, so buf was left completely untouched -- an uninitialized
// caller-owned stack array, NOT a valid empty C string. Every caller then
// fed that buffer into rj_append_json_str() as a NUL-terminated string,
// reading past the array into whatever adjacent stack memory happened to
// come before a stray zero byte -- confirmed by a host test scenario
// (row_constraint_journal_host_test.c's own s12) that found a rolled-back
// row's data leaking into an unrelated, unconnected entry's "before"
// field. Explicitly null-terminating buf[*pos] up front, before the loop,
// guarantees a real empty string for the count==0 case without changing
// behavior for count > 0 (the loop's own rj_append() calls keep
// re-terminating as they go, same as before).
static void rj_format_row(char* buf, int* pos, int max, const struct RowValues* rv) {
    if (max > 0) buf[*pos] = '\0';
    for (uint32_t i = 0; i < rv->count; i++) {
        if (i > 0) rj_append(buf, pos, max, "|");
        rj_append(buf, pos, max, rv->values[i]);
    }
}

void row_journal_dump(const char* journal_name, uint64_t since_seq) {
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ENTRIES; i++) {
        struct RowJournalEntry* e = &row_journal_buffer[i];
        if (!e->active) continue;
        if (!rj_streq(e->journal_name, journal_name)) continue;
        if (since_seq > 0 && e->seq < since_seq) continue;
        char before[600], after[600];
        int bp = 0, ap = 0;
        rj_format_row(before, &bp, sizeof(before), &e->before);
        rj_format_row(after, &ap, sizeof(after), &e->after);
        kernel_serial_printf("[ROWJRN] seq=%llu tx=%llu %s.%llu type=%s committed=%u before=[%s] after=[%s]\n",
                             (unsigned long long)e->seq, (unsigned long long)e->tx_id, e->table_name,
                             (unsigned long long)e->logical_id, jent_type_name(e->type), e->committed, before, after);
    }
}

int row_journal_to_json(const char* journal_name, uint64_t since_seq, char* buf, int max) {
    int pos = 0;
    rj_append(buf, &pos, max, "[");
    int first = 1;
    for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ENTRIES; i++) {
        struct RowJournalEntry* e = &row_journal_buffer[i];
        if (!e->active) continue;
        if (!rj_streq(e->journal_name, journal_name)) continue;
        if (since_seq > 0 && e->seq < since_seq) continue;
        if (!first) rj_append(buf, &pos, max, ",");
        first = 0;
        rj_append(buf, &pos, max, "{\"seq\":");
        rj_append_u64(buf, &pos, max, e->seq);
        rj_append(buf, &pos, max, ",\"tx_id\":");
        rj_append_u64(buf, &pos, max, e->tx_id);
        rj_append(buf, &pos, max, ",\"table\":\"");
        rj_append_json_str(buf, &pos, max, e->table_name);
        rj_append(buf, &pos, max, "\",\"logical_id\":");
        rj_append_u64(buf, &pos, max, e->logical_id);
        rj_append(buf, &pos, max, ",\"type\":\"");
        rj_append_json_str(buf, &pos, max, jent_type_name(e->type));
        rj_append(buf, &pos, max, "\",\"committed\":");
        rj_append(buf, &pos, max, e->committed ? "true" : "false");
        rj_append(buf, &pos, max, ",\"before\":\"");
        {
            char tmp[600]; int tp = 0;
            rj_format_row(tmp, &tp, sizeof(tmp), &e->before);
            rj_append_json_str(buf, &pos, max, tmp);
        }
        rj_append(buf, &pos, max, "\",\"after\":\"");
        {
            char tmp[600]; int tp = 0;
            rj_format_row(tmp, &tp, sizeof(tmp), &e->after);
            rj_append_json_str(buf, &pos, max, tmp);
        }
        rj_append(buf, &pos, max, "\"}");
    }
    rj_append(buf, &pos, max, "]");
    return pos;
}
