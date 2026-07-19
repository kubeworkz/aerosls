/*
 * row_journal.h — Phase 23 (relational layer): audit trail / change-data-
 * capture for row-set tables. See docs/AeroSLS-RDBMS-Roadmap-v0.1.md §10
 * for scope and the "Findings addendum" for what's built here.
 *
 * ─── Why this is a new, parallel subsystem, not an extension of
 * journal.c ────────────────────────────────────────────────────────────────
 * `journal.c`'s `struct JournalEntry` records ONE FIELD's change per
 * entry -- `before_image[64]`/`after_image[64]` are 64-byte strings,
 * sized for the legacy KV model's one-record-one-field-at-a-time shape
 * (`journal_write(object_name, field_key, before_value, after_value,
 * ...)`). A row-set row can have up to `ROWSTORE_MAX_COLUMNS` (16)
 * columns of up to `RECORD_VAL_LEN` (256) bytes each -- a full
 * `struct RowValues` is over 4 KiB, more than 60x too large for
 * `journal.c`'s existing per-field string fields. This is the exact same
 * "hard wall, not a tuning problem" discovery Phase 16 made about
 * `RECORD_MAX_FIELDS`/`SLSObjectRecord` needing a real new storage shape
 * rather than a bigger version of the old one -- confirmed here for
 * `journal.c`'s entry shape specifically, not assumed. `JournalEntryType`
 * (journal.h) is reused as-is, though -- a plain, dependency-free enum
 * (PT/UP/UB/DL/CM/RB) whose vocabulary is identical for a row-set
 * mutation, no reason to invent a second one.
 *
 * ─── Design, mirroring journal.c's own shape everywhere it still fits ────
 * `row_journal_attach()`/`_detach()` mirror `journal_attach()`/`_detach()`
 * exactly (a named journal, a table opted in by name, silently-ignored
 * writes for anything not attached). Entries carry full `struct
 * RowValues` before/after images (not 64-byte strings) and are keyed by
 * `struct MvccRowId.logical_id` (not a field-name string) -- the two
 * genuine shape changes this phase needed, everything else (seq/
 * timestamp/tx_id/committed flag, attach-by-name, dump/JSON-serialize)
 * follows `journal.c`'s own precedent directly.
 *
 * ─── Wired automatically from mvcc.c, not opt-in per caller ─────────────
 * Matching `row_constraint.h`'s identical reasoning: `mvcc_row_insert()`/
 * `_update()`/`_delete()` call `row_journal_notify_insert/update/delete()`
 * automatically after each mutation is staged, and `mvcc_commit()`/
 * `mvcc_rollback()` call `row_journal_commit_tx()`/`_rollback_tx()`
 * automatically -- entries move from pending to committed (or are marked
 * rolled back) in lockstep with the real transaction outcome, the same
 * `committed` bit `journal.c`'s own entries already carry, now correct
 * for row-set tables too.
 *
 * ─── First-cut capacity, a deliberate, smaller ring than journal.c's ────
 * `ROW_JOURNAL_MAX_ENTRIES` is much smaller than `journal.c`'s own
 * `JOURNAL_MAX_ENTRIES` (256) precisely because each entry is roughly 30x
 * larger (two full `RowValues` vs. two 64-byte strings) -- a smaller ring
 * is the honest capacity trade-off for the same BSS budget, not free
 * lunch, matching this whole roadmap's "documented cap, not silent"
 * posture (`journal.h`'s own header comment already states its entry
 * count "≈ 72 KiB BSS" explicitly; this file's entries are sized and
 * commented the same way for the same reason).
 */
#ifndef ROW_JOURNAL_H
#define ROW_JOURNAL_H

#include <stdint.h>
#include "rowstore.h"
#include "mvcc.h"
#include "journal.h"   // reuses JournalEntryType / jent_type_name() as-is -- see header comment

// ─── Limits ─────────────────────────────────────────────────────────────────
#define ROW_JOURNAL_MAX_ENTRIES      16   // ~8.3 KiB/entry (two struct RowValues) * 16 ~= 133 KiB BSS -- see header comment
#define ROW_JOURNAL_MAX_ATTACHMENTS  16
#define ROW_JOURNAL_NAME_LEN         32

struct RowJournalEntry {
    uint64_t          seq;                              // global monotonic sequence number
    uint64_t          tx_id;                             // parent transaction (mvcc.h's own txn_id)
    uint64_t          table_object_id;
    char               journal_name[ROW_JOURNAL_NAME_LEN];
    char               table_name[OBJECT_NAME_LEN];
    uint64_t          logical_id;                        // struct MvccRowId.logical_id of the row that changed
    JournalEntryType   type;                              // JENT_PT/UP/UB/DL (journal.h) -- CM/RB not used per-entry,
                                                           // see row_journal_commit_tx()/_rollback_tx() instead
    uint8_t            committed;                         // 1 = committed, 0 = pending (mirrors journal.c's own flag)
    uint8_t            active;                            // 0 = free/reclaimed slot
    struct RowValues   before;                            // empty (count=0) for an INSERT's PT entry
    struct RowValues   after;                             // empty (count=0) for a DELETE's DL entry
};

struct RowJournalAttachment {
    char    journal_name[ROW_JOURNAL_NAME_LEN];
    char    table_name[OBJECT_NAME_LEN];
    uint8_t active;
};

extern struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
extern uint32_t                    row_journal_entry_count;   // total ever written (wraps, matching journal.c)
extern struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
extern uint32_t                    row_journal_attachment_count;

void row_journal_init(void);

// Attach/detach a row-set table to/from a named journal -- mirrors
// journal_attach()/journal_detach() exactly. Returns 0 on success.
int row_journal_attach(const char* journal_name, const char* table_name);
int row_journal_detach(const char* journal_name, const char* table_name);

// Called automatically by mvcc.c -- silently a no-op if table_name isn't
// attached to any journal (matching journal_write()'s own "silently
// ignored if not attached" contract). before/after: pass NULL for
// whichever side doesn't apply (INSERT has no before, DELETE has no
// after) -- stored as an empty (count=0) RowValues either way.
void row_journal_notify_insert(uint64_t txn_id, const char* table_name, uint64_t table_object_id,
                               uint64_t logical_id, const struct RowValues* after);
void row_journal_notify_update(uint64_t txn_id, const char* table_name, uint64_t table_object_id,
                               uint64_t logical_id, const struct RowValues* before, const struct RowValues* after);
void row_journal_notify_delete(uint64_t txn_id, const char* table_name, uint64_t table_object_id,
                               uint64_t logical_id, const struct RowValues* before);

// Called automatically by mvcc_commit()/mvcc_rollback() (mvcc.c). Marks
// every pending entry for txn_id as committed=1, or removes every
// pending entry for txn_id entirely (rollback -- matching this whole
// roadmap's "aborted work is real garbage, reclaim it" posture, unlike
// journal.c's own JENT_RB-entry-kept-then-purged-later shape, a
// deliberate simplification since this ring is small enough that keeping
// a permanent rollback record isn't worth the capacity it costs here).
void row_journal_commit_tx(uint64_t txn_id);
void row_journal_rollback_tx(uint64_t txn_id);

// Print every entry for journal_name to the kernel serial port (matching
// journal_dump()'s own shape). since_seq > 0 filters to seq >= since_seq.
void row_journal_dump(const char* journal_name, uint64_t since_seq);

// JSON-serialize every entry for journal_name into buf[max] (matching
// journal_to_json()'s own contract). Returns the number of bytes written.
int row_journal_to_json(const char* journal_name, uint64_t since_seq, char* buf, int max);

#endif /* ROW_JOURNAL_H */
