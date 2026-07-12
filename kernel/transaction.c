#include "transaction.h"
#include "object_catalog.h"
#include "journal.h"

// ─── WAL In-RAM Buffer ────────────────────────────────────────────────────────
struct WALEntry wal_buffer[WAL_MAX_ENTRIES];
uint32_t        wal_entry_count = 0;
static uint64_t wal_next_tx_id  = 1;

// ─── Active Transaction Contexts ─────────────────────────────────────────────
static struct TxContext tx_contexts[MAX_ACTIVE_TRANSACTIONS];

// ─── CRC32 ────────────────────────────────────────────────────────────────────
static uint32_t crc32_table[256];
static int      crc32_table_ready = 0;

static void init_crc32(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

static uint32_t crc32_compute(const void* data, size_t len) {
    if (!crc32_table_ready) init_crc32();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* b = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ b[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ─── String Helpers ───────────────────────────────────────────────────────────
static size_t tx_strlen(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static void tx_strncpy(char* d, const char* s, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

// ─── Context Lookup ───────────────────────────────────────────────────────────
static struct TxContext* find_ctx(uint32_t thread_id) {
    for (int i = 0; i < MAX_ACTIVE_TRANSACTIONS; i++) {
        if (tx_contexts[i].active && tx_contexts[i].thread_id == thread_id)
            return &tx_contexts[i];
    }
    return 0;
}

uint64_t tx_get_active(uint32_t thread_id) {
    struct TxContext* ctx = find_ctx(thread_id);
    return ctx ? ctx->tx_id : 0;
}

// ─── Phase 3: tx begin ────────────────────────────────────────────────────────
uint64_t sys_sls_tx_begin(uint32_t thread_id) {
    if (find_ctx(thread_id)) {
        kernel_serial_printf(
            "[WAL] tx begin: Thread %u already has an open transaction.\n",
            thread_id);
        return 0;
    }
    for (int i = 0; i < MAX_ACTIVE_TRANSACTIONS; i++) {
        if (!tx_contexts[i].active) {
            uint64_t new_tx_id = wal_next_tx_id++;
            tx_contexts[i].tx_id       = new_tx_id;
            tx_contexts[i].thread_id   = thread_id;
            tx_contexts[i].wal_start   = wal_entry_count;
            tx_contexts[i].wal_count   = 0;
            tx_contexts[i].active      = 1;
            kernel_serial_printf(
                "[WAL] Transaction %lu STARTED (thread=%u).\n",
                new_tx_id, thread_id);
            return new_tx_id;
        }
    }
    kernel_serial_print("[WAL] ERROR: Transaction context table full.\n");
    return 0;
}

// ─── Phase 3: tx commit ───────────────────────────────────────────────────────
uint64_t sys_sls_tx_commit(uint32_t thread_id) {
    struct TxContext* ctx = find_ctx(thread_id);
    if (!ctx) {
        kernel_serial_printf(
            "[WAL] tx commit: No active transaction for thread %u.\n",
            thread_id);
        return 1;
    }

    // Close the transaction context FIRST so that sys_sls_update calls made
    // during the apply loop below do not re-stage into the WAL.
    uint64_t committed_tx_id = ctx->tx_id;
    uint32_t wal_start       = ctx->wal_start;
    uint32_t wal_count       = ctx->wal_count;
    ctx->active = 0;

    uint32_t committed = 0;
    for (uint32_t i = wal_start; i < wal_start + wal_count; i++) {
        if (wal_buffer[i].state == WAL_STATE_PENDING) {
            wal_buffer[i].state = WAL_STATE_COMMITTED;

            // Apply the committed value to the live object record
            struct SLSRecordRequest req;
            // resolve object name from object_id via catalog
            const char* obj_name = "";
            for (uint32_t j = 0; j < object_catalog_count; j++) {
                if (object_catalog[j].active &&
                    object_catalog[j].object_id == wal_buffer[i].object_id) {
                    obj_name = object_catalog[j].name;
                    break;
                }
            }
            tx_strncpy(req.name,  obj_name,              OBJECT_NAME_LEN);
            tx_strncpy(req.key,   wal_buffer[i].key,     RECORD_KEY_LEN);
            tx_strncpy(req.value, wal_buffer[i].new_value, RECORD_VAL_LEN);
            sys_sls_update(&req);

            committed++;
        }
    }

    kernel_serial_printf(
        "[WAL] Transaction %lu COMMITTED. %u operation(s) applied.\n",
        committed_tx_id, committed);

    // Notify the journal subsystem so pending entries are marked committed
    journal_commit_tx(committed_tx_id);

    return 0;
}

// ─── Phase 3: tx rollback ─────────────────────────────────────────────────────
uint64_t sys_sls_tx_rollback(uint32_t thread_id) {
    struct TxContext* ctx = find_ctx(thread_id);
    if (!ctx) {
        kernel_serial_printf(
            "[WAL] tx rollback: No active transaction for thread %u.\n",
            thread_id);
        return 1;
    }

    uint32_t rolled = 0;
    for (uint32_t i = ctx->wal_start; i < ctx->wal_start + ctx->wal_count; i++) {
        if (wal_buffer[i].state == WAL_STATE_PENDING) {
            wal_buffer[i].state = WAL_STATE_ABORTED;
            rolled++;
        }
    }

    kernel_serial_printf(
        "[WAL] Transaction %lu ROLLED BACK. %u staged operation(s) discarded.\n",
        ctx->tx_id, rolled);

    // Notify the journal subsystem so pending entries are marked rolled-back
    journal_rollback_tx(ctx->tx_id);

    ctx->active = 0;
    return 0;
}

// ─── Phase 3: WAL recovery (cold boot) ───────────────────────────────────────
void sys_sls_tx_recover(void) {
    kernel_serial_print("[WAL] Starting WAL recovery scan...\n");
    uint32_t redone = 0, undone = 0;

    for (uint32_t i = 0; i < wal_entry_count; i++) {
        struct WALEntry* e = &wal_buffer[i];
        if (e->magic != WAL_MAGIC) continue;

        // Verify CRC
        uint32_t stored_crc = e->crc32;
        e->crc32 = 0;
        uint32_t computed = crc32_compute(e, sizeof(*e));
        e->crc32 = stored_crc;

        if (computed != stored_crc) {
            kernel_serial_printf(
                "[WAL] RECOVERY: Entry #%u CRC MISMATCH — skipping.\n",
                e->entry_id);
            continue;
        }

        if (e->state == WAL_STATE_COMMITTED) {
            // Redo: re-apply the committed value
            const char* obj_name = "";
            for (uint32_t j = 0; j < object_catalog_count; j++) {
                if (object_catalog[j].active &&
                    object_catalog[j].object_id == e->object_id) {
                    obj_name = object_catalog[j].name;
                    break;
                }
            }
            struct SLSRecordRequest req;
            tx_strncpy(req.name,  obj_name,  OBJECT_NAME_LEN);
            tx_strncpy(req.key,   e->key,    RECORD_KEY_LEN);
            tx_strncpy(req.value, e->new_value, RECORD_VAL_LEN);
            sys_sls_update(&req);
            redone++;
        } else if (e->state == WAL_STATE_PENDING) {
            // Undo: restore old value
            const char* obj_name = "";
            for (uint32_t j = 0; j < object_catalog_count; j++) {
                if (object_catalog[j].active &&
                    object_catalog[j].object_id == e->object_id) {
                    obj_name = object_catalog[j].name;
                    break;
                }
            }
            struct SLSRecordRequest req;
            tx_strncpy(req.name,  obj_name,  OBJECT_NAME_LEN);
            tx_strncpy(req.key,   e->key,    RECORD_KEY_LEN);
            tx_strncpy(req.value, e->old_value, RECORD_VAL_LEN);
            sys_sls_update(&req);
            e->state = WAL_STATE_ABORTED;
            undone++;
        }
    }

    kernel_serial_printf(
        "[WAL] Recovery complete. Redone: %u  Undone: %u\n", redone, undone);
}

// ─── Phase 3: stage a WAL entry ───────────────────────────────────────────────
uint64_t wal_stage(uint32_t thread_id, uint64_t object_id,
                   const char* key,
                   const char* old_value, const char* new_value) {
    struct TxContext* ctx = find_ctx(thread_id);
    if (!ctx) {
        // No open transaction — direct write, no WAL entry
        return 0;
    }
    if (wal_entry_count >= WAL_MAX_ENTRIES) {
        kernel_serial_print("[WAL] ERROR: WAL buffer full.\n");
        return 1;
    }

    struct WALEntry* e = &wal_buffer[wal_entry_count];
    e->magic     = WAL_MAGIC;
    e->entry_id  = wal_entry_count + 1;
    e->tx_id     = ctx->tx_id;
    e->object_id = object_id;
    tx_strncpy(e->key,       key,       48);
    tx_strncpy(e->old_value, old_value, 64);
    tx_strncpy(e->new_value, new_value, 64);
    e->state     = WAL_STATE_PENDING;
    e->crc32     = 0;
    e->crc32     = crc32_compute(e, sizeof(*e));

    wal_entry_count++;
    ctx->wal_count++;

    kernel_serial_printf(
        "[WAL] Staged entry #%u | tx=%lu | %s.%s = '%s' (was '%s')\n",
        e->entry_id, ctx->tx_id, "", key, new_value, old_value);
    return 0;
}

// ─── Phase 3: wal dump ────────────────────────────────────────────────────────
void wal_dump(void) {
    static const char* state_names[] = { "PENDING", "COMMITTED", "ABORTED" };
    kernel_serial_print(
        "\n[WAL] Write-Ahead Log  (CATALOG_CRC based)\n"
        " Entry  TxID  Object ID           Key                  Status\n"
        " -----  ----  ------------------  -------------------  ---------\n");

    if (wal_entry_count == 0) {
        kernel_serial_print(" (log is empty)\n");
    } else {
        for (uint32_t i = 0; i < wal_entry_count; i++) {
            struct WALEntry* e = &wal_buffer[i];
            const char* st = (e->state <= WAL_STATE_ABORTED)
                             ? state_names[e->state] : "?";
            kernel_serial_printf(
                " #%-5u %-5lu 0x%016lx  %-20s %s\n",
                e->entry_id, e->tx_id, e->object_id, e->key, st);
        }
    }
    kernel_serial_printf(" %u entry(ies) total.\n\n", wal_entry_count);
}
