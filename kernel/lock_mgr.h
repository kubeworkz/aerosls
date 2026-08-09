#ifndef LOCK_MGR_H
#define LOCK_MGR_H

#include <stdint.h>
#include "object_catalog.h"

// ─── AeroSLS Row-Level Lock Manager ─────────────────────────────────────────
//
// Implements write-intent locking at the record (key) level, giving
// Read-Committed isolation:
//
//   • Every INSERT / UPDATE / DELETE acquires an EXCLUSIVE lock on
//     (object_id, key) before staging to the WAL.
//   • A second transaction trying to write the same key is immediately
//     rejected with an error (no blocking / waiting in a bare-metal kernel).
//   • On COMMIT or ROLLBACK all locks held by that transaction are released.
//   • Read paths are not locked (readers never block writers, writers never
//     block readers).  Full Repeatable-Read requires cursor support (DB5).
//
// This matches IBM i's default isolation level for DML: *CS (Cursor Stability),
// which upgrades to *ALL (Serializable) when you want full protection.
// ─────────────────────────────────────────────────────────────────────────────

// ─── Limits ───────────────────────────────────────────────────────────────────
#define LOCK_MAX_ENTRIES   128   // max concurrent row locks across all transactions

// ─── Lock type ────────────────────────────────────────────────────────────────
typedef enum {
    LOCK_SHARED    = 0,   // S — read intent (reserved for future cursor support)
    LOCK_EXCLUSIVE = 1,   // X — write intent (INSERT / UPDATE / DELETE)
} LockType;

// ─── One lock entry ───────────────────────────────────────────────────────────
struct LockEntry {
    uint64_t tx_id;                     // transaction that holds this lock
    uint64_t object_id;                 // FNV-1a hash of the locked object
    char     key[RECORD_KEY_LEN];       // locked record key ("" = object-level)
    uint8_t  type;                      // LockType
    uint8_t  active;                    // 1 = held, 0 = free slot
};

// ─── Globals ──────────────────────────────────────────────────────────────────
extern struct LockEntry lock_table[LOCK_MAX_ENTRIES];
extern uint32_t         lock_entry_count;   // total ever acquired (not current)

// ─── Public API ───────────────────────────────────────────────────────────────

// Initialise the lock table (called from kernel_main).
void lock_mgr_init(void);

// Attempt to acquire an exclusive lock on (object_id, key) for tx_id.
// Returns 0 on success, or a non-zero conflict code if another transaction
// already holds an exclusive lock on the same key.
//
// Conflict code:  1 = key locked by another TX  (caller should return error)
//                 2 = lock table full
int  lock_acquire(uint64_t tx_id, uint64_t object_id, const char* key,
                  LockType type);

// Release all locks held by tx_id (called on commit or rollback).
void lock_release_tx(uint64_t tx_id);

// Return 1 if tx_id already holds any lock on (object_id, key).
int  lock_owned(uint64_t tx_id, uint64_t object_id, const char* key);

// Serialise the current lock table as JSON into buf[max].
int  lock_to_json(char* buf, int max);

#endif /* LOCK_MGR_H */
