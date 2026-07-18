#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stdint.h>
#include <stddef.h>

// ─── WAL On-Disk Sector ───────────────────────────────────────────────────────
// Matches the boot logger trace: Sector 2048 = Write-Ahead Journal
#define WAL_DISK_SECTOR_BASE   2048
#define WAL_MAX_ENTRIES        512     // reduced from 4096 for BSS budget
#define WAL_MAGIC              0x534C5357414C454EULL  // "SLSWALN"

// ─── WAL Entry States ─────────────────────────────────────────────────────────
typedef enum {
    WAL_STATE_PENDING   = 0,  // Staged but not committed
    WAL_STATE_COMMITTED = 1,  // Successfully written to storage
    WAL_STATE_ABORTED   = 2,  // Rolled back
} WALState;

// ─── WAL Entry ────────────────────────────────────────────────────────────────
struct WALEntry {
    uint64_t magic;           // WAL_MAGIC sanity marker
    uint32_t entry_id;        // Monotonic counter
    uint32_t crc32;           // CRC32 over all other fields
    uint64_t tx_id;           // Parent transaction ID
    uint64_t object_id;       // FNV-1a hash of target object name
    char     key[48];         // Record field key
    char     old_value[64];   // Value before this operation (for rollback)
    char     new_value[64];   // Proposed new value
    uint8_t  state;           // WALState
    uint8_t  _pad[7];
};

// ─── Transaction Context ──────────────────────────────────────────────────────
#define MAX_ACTIVE_TRANSACTIONS 64

struct TxContext {
    uint64_t tx_id;
    uint32_t thread_id;
    uint32_t wal_start;       // Index of first WAL entry for this tx
    uint32_t wal_count;       // Number of WAL entries staged
    uint8_t  active;
};

// ─── Syscall Numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_TX_BEGIN    120
#define SYS_SLS_TX_COMMIT   121
#define SYS_SLS_TX_ROLLBACK 122
#define SYS_SLS_TX_RECOVER  123

// ─── Public Transaction API ───────────────────────────────────────────────────
extern struct WALEntry wal_buffer[WAL_MAX_ENTRIES];
extern uint32_t        wal_entry_count;

// Returns new tx_id on success, 0 on error
uint64_t sys_sls_tx_begin(uint32_t thread_id);

// Commits all pending WAL entries for thread's active tx; returns 0 on success
uint64_t sys_sls_tx_commit(uint32_t thread_id);

// Rolls back all pending WAL entries for thread's active tx; returns 0 on success
uint64_t sys_sls_tx_rollback(uint32_t thread_id);

// Cold-boot WAL replay: redo COMMITTED entries, undo PENDING entries
void sys_sls_tx_recover(void);

// Stage a WAL entry for the current transaction (called by update/insert)
// Returns 0 on success
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id,
                   const char* key,
                   const char* old_value, const char* new_value);

// Print all WAL entries to the kernel serial port
void wal_dump(void);

// Get active tx_id for the given thread (0 if none)
uint64_t tx_get_active(uint32_t thread_id);

#endif /* TRANSACTION_H */
