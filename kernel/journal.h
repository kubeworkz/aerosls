#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>
#include "../kernel/object_catalog.h"

// ─── IBM i-style Journal for AeroSLS ─────────────────────────────────────────
//
// A Journal is a persistent, append-only record of every data change made to
// the tables attached to it.  Unlike the WAL (which is a crash-recovery ring
// buffer), a Journal:
//
//   • is indefinite — entries accumulate until explicitly purged
//   • records before-images AND after-images for every change
//   • is created and attached explicitly, per table (like IBM i STRJRNPF)
//   • drives audit trails, change-data capture, and point-in-time recovery
//
// Entry type codes map 1:1 to IBM i journal entry-type abbreviations:
//   PT — Put (INSERT)          UB — Update Before-image
//   UP — Update (after-image)  DL — Delete
//   CM — Commit                RB — Rollback
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define JOURNAL_MAX_ENTRIES      256   // in-memory ring; persisted to NVMe
#define JOURNAL_MAX_ATTACHMENTS   32   // max (journal, table) pairs
#define JOURNAL_DISK_SECTOR_BASE 4096  // NVMe LBA where journal starts

// ─── Entry type ───────────────────────────────────────────────────────────────
typedef enum {
    JENT_PT = 0,  // Put      — INSERT (no before-image)
    JENT_UP = 1,  // Update   — UPDATE after-image
    JENT_UB = 2,  // Update Before — UPDATE before-image
    JENT_DL = 3,  // Delete   — DELETE (no after-image)
    JENT_CM = 4,  // Commit   — transaction committed
    JENT_RB = 5,  // Rollback — transaction rolled back
} JournalEntryType;

static inline const char* jent_type_name(JournalEntryType t) {
    switch (t) {
        case JENT_PT: return "PT";
        case JENT_UP: return "UP";
        case JENT_UB: return "UB";
        case JENT_DL: return "DL";
        case JENT_CM: return "CM";
        case JENT_RB: return "RB";
        default:      return "??";
    }
}

// ─── One journal entry ────────────────────────────────────────────────────────
struct JournalEntry {
    uint64_t  seq;                    // global monotonic sequence number
    uint64_t  timestamp;              // kernel_tick_counter at write time
    uint64_t  tx_id;                  // parent transaction (0 = auto-commit)
    uint64_t  object_id;              // FNV-1a hash of the journaled table
    char      journal_name[32];       // owning journal object name
    char      object_name[32];        // table this entry belongs to
    char      key[48];                // record field key
    char      before_image[64];       // value before change (empty for PT)
    char      after_image[64];        // value after change (empty for DL/RB/CM)
    uint8_t   type;                   // JournalEntryType
    uint8_t   committed;              // 1=committed or auto-commit, 0=pending
    uint8_t   _pad[6];
};  // ≈ 288 bytes; 256 entries ≈ 72 KiB BSS

// ─── Journal attachment record ────────────────────────────────────────────────
// Tracks which tables are being journaled by which journal object.
struct JournalAttachment {
    char    journal_name[32];
    char    object_name[32];
    uint8_t active;
};

// ─── Globals ──────────────────────────────────────────────────────────────────
extern struct JournalEntry      journal_buffer[JOURNAL_MAX_ENTRIES];
extern uint32_t                 journal_entry_count;   // total ever written (wraps)
extern struct JournalAttachment journal_attachments[JOURNAL_MAX_ATTACHMENTS];
extern uint32_t                 journal_attachment_count;

// ─── Public API ───────────────────────────────────────────────────────────────

// Initialise journal subsystem (called from kernel_main).
void journal_init(void);

// Write a journal entry for a data-mutation event.
// Silently ignored if the object is not attached to any journal.
void journal_write(const char* object_name,
                   const char* key,
                   const char* before_image,
                   const char* after_image,
                   JournalEntryType type,
                   uint64_t tx_id);

// Mark all pending entries for tx_id as committed (called on tx commit).
void journal_commit_tx(uint64_t tx_id);

// Mark all pending entries for tx_id as rolled-back (called on tx rollback).
void journal_rollback_tx(uint64_t tx_id);

// Attach a table to a journal (creates journal object if it doesn't exist).
// Returns 0 on success, non-zero on error.
int  journal_attach(const char* journal_name, const char* object_name);

// Detach a table from a journal.
int  journal_detach(const char* journal_name, const char* object_name);

// Print all entries for a journal to the kernel serial port.
// If since_seq > 0, only entries with seq >= since_seq are printed.
void journal_dump(const char* journal_name, uint64_t since_seq);

// Remove rolled-back (RB) entries from a journal to reclaim space.
void journal_purge(const char* journal_name);

// JSON serialise all journal entries for a journal into buf[max].
int  journal_to_json(const char* journal_name, uint64_t since_seq,
                     char* buf, int max);

// Return the name of the journal attached to object_name, or NULL.
const char* journal_for_object(const char* object_name);

#endif /* JOURNAL_H */
