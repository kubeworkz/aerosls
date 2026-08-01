/*
 * checkpoint_mgr.c — coordinated full-system checkpoint via IPC
 * (Core Backup Strategies, Step 1).
 *
 * Orchestrates all per-subsystem persist_*() calls into one atomic batch,
 * writes a checkpoint header to a dedicated NVMe region, and exposes
 * trigger/status/list operations via IPC port 0x1007.
 *
 * Freestanding: local helpers, no libc, per this codebase's convention.
 */
#include "checkpoint_mgr.h"
#include "ipc.h"
#include "persist.h"
#include "object_catalog.h"
#include "partition.h"
#include "stream.h"
#include "state_tree.h"
#include "checkpoint_delta.h"
#include "kernel_io.h"
#include "../kernel/dashboard.h"    /* read_tsc() */
#include "../drivers/nvme_io.h"
#include "../net/consensus.h"       /* cluster_local_node_id() */

/* ─── Local helpers ──────────────────────────────────────────────────── */
static void cm_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void cm_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}

/* ─── NVMe LBA Layout ────────────────────────────────────────────────────
 * Placed after PERSIST_WORKLOAD_ENT_LBA (7624) + 24 sectors (3 frames) = 7648,
 * with the standard 1-frame safety gap → header at 7656.
 * Entry array: 8 × CkptHeader (48 B each = 384 B total) → 1 frame.
 * End: 7672. Margin before STREAM_DIR_LBA (8192): 520 sectors / 65 frames. */
#define PERSIST_CKPT_MAGIC    PERSIST_CKPT_MAGIC_NV

/* ─── Global state ───────────────────────────────────────────────────── */
static struct CkptHeader ckpt_history[CKPT_MAX_HISTORY];
static uint64_t          ckpt_next_seq   = 1;
static uint32_t          ckpt_count      = 0;
static volatile uint8_t  ckpt_in_progress = 0;

/* Page-aligned DMA buffer for checkpoint header I/O */
static uint8_t __attribute__((aligned(4096))) ckpt_buf[4096];

/* State tree: built on each checkpoint, retained for diff on the next */
static struct StateTreeNode ckpt_tree[ST_MAX_NODES];
static uint32_t             ckpt_tree_count = 0;

/* ─── Count helpers ──────────────────────────────────────────────────── */
static uint32_t count_active_partitions(void) {
    uint32_t n = 0;
    for (int i = 0; i < PARTITION_MAX; i++)
        if (partition_table[i].active) n++;
    return n;
}

static uint32_t count_active_streams(void) {
    uint32_t n = 0;
    for (int i = 0; i < STREAM_MAX; i++)
        if (stream_store[i].active) n++;
    return n;
}

/* ─── NVMe I/O (guarded) ────────────────────────────────────────────── */
static int ckpt_nvme_available(void) { return io_sq && io_cq; }

static int ckpt_write_history(void) {
    if (!ckpt_nvme_available()) return CKPT_STATUS_NO_NVME;

    /* Write header frame: magic + seq counter */
    cm_memset(ckpt_buf, 0, 4096);
    uint64_t magic = PERSIST_CKPT_MAGIC;
    cm_memcpy(ckpt_buf, &magic, 8);
    cm_memcpy(ckpt_buf + 8, &ckpt_next_seq, 8);
    cm_memcpy(ckpt_buf + 16, &ckpt_count, 4);
    if (nvme_write_sync(PERSIST_CKPT_HDR_LBA, ckpt_buf) != 0)
        return CKPT_STATUS_WRITE_FAILED;

    /* Write entry array (all 8 slots fit in one 4 KiB frame) */
    cm_memset(ckpt_buf, 0, 4096);
    cm_memcpy(ckpt_buf, ckpt_history, sizeof(ckpt_history));
    if (nvme_write_sync(PERSIST_CKPT_ENT_LBA, ckpt_buf) != 0)
        return CKPT_STATUS_WRITE_FAILED;

    return CKPT_STATUS_OK;
}

static void ckpt_read_history(void) {
    if (!ckpt_nvme_available()) return;

    /* Read header */
    if (nvme_read_sync(PERSIST_CKPT_HDR_LBA, ckpt_buf) != 0) return;
    uint64_t magic = 0;
    cm_memcpy(&magic, ckpt_buf, 8);
    if (magic != PERSIST_CKPT_MAGIC) return;  /* no prior checkpoint data */

    cm_memcpy(&ckpt_next_seq, ckpt_buf + 8, 8);
    cm_memcpy(&ckpt_count, ckpt_buf + 16, 4);
    if (ckpt_count > CKPT_MAX_HISTORY) ckpt_count = CKPT_MAX_HISTORY;

    /* Read entry array */
    if (nvme_read_sync(PERSIST_CKPT_ENT_LBA, ckpt_buf) != 0) return;
    cm_memcpy(ckpt_history, ckpt_buf, sizeof(ckpt_history));

    kernel_serial_printf("[CKPT] Restored %u checkpoint record(s), next seq=%llu\n",
                         ckpt_count, ckpt_next_seq);
}

/* ─── Public API ─────────────────────────────────────────────────────── */

void checkpoint_mgr_init(void) {
    cm_memset(ckpt_history, 0, sizeof(ckpt_history));
    ckpt_next_seq = 1;
    ckpt_count = 0;
    ckpt_in_progress = 0;
    ckpt_read_history();
    kernel_serial_print("[CKPT] CheckpointMgr initialised.\n");
}

int checkpoint_trigger(void) {
    if (ckpt_in_progress) return CKPT_STATUS_BUSY;
    if (!ckpt_nvme_available()) return CKPT_STATUS_NO_NVME;

    ckpt_in_progress = 1;

    /* Decide full vs. incremental */
    int is_full = ckpt_should_force_full() || ckpt_count == 0;
    if (is_full) ckpt_mark_all_dirty();

    uint32_t dirty = ckpt_dirty_mask();

    /* Batch all persist calls — only dirty regions are written */
    persist_defer_begin();

    if (dirty & (1u << CKPT_REGION_CATALOG))     persist_catalog();
    if (dirty & (1u << CKPT_REGION_RECORDS))     persist_records();
    if (dirty & (1u << CKPT_REGION_SCHEMAS))     persist_schemas();
    if (dirty & (1u << CKPT_REGION_PROGRAMS))    persist_programs();
    if (dirty & (1u << CKPT_REGION_PARTITIONS))  persist_partitions();
    if (dirty & (1u << CKPT_REGION_ROWSTORE))    persist_rowstore_headers();
    if (dirty & (1u << CKPT_REGION_ROW_CONSTR))  persist_row_constraints();
    if (dirty & (1u << CKPT_REGION_ROW_INDEX))   persist_row_index_defs();
    if (dirty & (1u << CKPT_REGION_VECSTORE))    persist_vecstore_headers();
    if (dirty & (1u << CKPT_REGION_VEC_INDEX))   persist_vec_index_defs();
    if (dirty & (1u << CKPT_REGION_ROW_JOURNAL)) persist_row_journal();
    if (dirty & (1u << CKPT_REGION_DATABASES))   persist_databases();
    if (dirty & (1u << CKPT_REGION_VIEWS))       persist_views();
    if (dirty & (1u << CKPT_REGION_TENANTS))     persist_tenants();
    if (dirty & (1u << CKPT_REGION_SERVICES))    persist_services();
    if (dirty & (1u << CKPT_REGION_WORKLOADS))   persist_workloads();

    persist_defer_end();

    /* Build state tree snapshot (Step 2) */
    ckpt_tree_count = state_tree_build(ckpt_tree, ST_MAX_NODES);

    /* Record checkpoint metadata */
    uint32_t slot = (uint32_t)((ckpt_next_seq - 1) % CKPT_MAX_HISTORY);
    struct CkptHeader* h = &ckpt_history[slot];
    h->magic          = CKPT_MAGIC;
    h->version        = CKPT_VERSION;
    h->sequence       = ckpt_next_seq;
    h->timestamp_tsc  = read_tsc();
    h->num_objects    = object_catalog_count;
    h->num_partitions = count_active_partitions();
    h->num_streams    = count_active_streams();
    h->node_id        = cluster_local_node_id();
    h->status         = 1;  /* committed */

    ckpt_next_seq++;
    if (ckpt_count < CKPT_MAX_HISTORY) ckpt_count++;

    /* Persist the checkpoint history itself */
    int rc = ckpt_write_history();

    /* Mark tree clean now that everything is durable */
    state_tree_mark_clean(ckpt_tree, ckpt_tree_count, h->sequence);

    /* Clear dirty tracking; reset incremental counter on full checkpoints */
    ckpt_clear_all();
    if (is_full) ckpt_reset_incremental_count();

    ckpt_in_progress = 0;

    kernel_serial_printf("[CKPT] Checkpoint seq=%llu %s "
                         "(objects=%u, partitions=%u, streams=%u, tree_nodes=%u, dirty=0x%04x)\n",
                         h->sequence, is_full ? "FULL" : "INCR",
                         h->num_objects,
                         h->num_partitions, h->num_streams, ckpt_tree_count, dirty);
    return rc;
}

uint64_t checkpoint_last_sequence(void) {
    if (ckpt_count == 0) return 0;
    uint32_t slot = (uint32_t)((ckpt_next_seq - 2) % CKPT_MAX_HISTORY);
    return ckpt_history[slot].sequence;
}

uint64_t checkpoint_last_timestamp(void) {
    if (ckpt_count == 0) return 0;
    uint32_t slot = (uint32_t)((ckpt_next_seq - 2) % CKPT_MAX_HISTORY);
    return ckpt_history[slot].timestamp_tsc;
}

uint32_t checkpoint_count(void) { return ckpt_count; }

/* ─── IPC Handler ────────────────────────────────────────────────────── */
void checkpoint_mgr_handler(struct IPCMessage* msg) {
    switch (msg->opcode) {
    case CKPT_OP_TRIGGER: {
        int rc = checkpoint_trigger();
        kernel_serial_printf("[CKPT] IPC TRIGGER -> status=%d\n", rc);
        break;
    }
    case CKPT_OP_STATUS:
        kernel_serial_printf(
            "[CKPT] STATUS: seq=%llu  count=%u  in_progress=%u\n",
            checkpoint_last_sequence(), ckpt_count, ckpt_in_progress);
        break;
    case CKPT_OP_LIST:
        kernel_serial_printf("[CKPT] LIST: %u checkpoint(s) stored\n", ckpt_count);
        for (uint32_t i = 0; i < ckpt_count; i++) {
            struct CkptHeader* h = &ckpt_history[i];
            if (h->magic != CKPT_MAGIC) continue;
            kernel_serial_printf(
                "  [%u] seq=%llu  node=%u  objects=%u  partitions=%u  streams=%u\n",
                i, h->sequence, h->node_id,
                h->num_objects, h->num_partitions, h->num_streams);
        }
        break;
    case CKPT_OP_RESTORE:
        /* Restore is a no-op in this phase — persist_restore_all() at boot
         * already reads back from these exact NVMe regions. This opcode is
         * reserved for Step 5 (recovery scenarios) where a live node loads
         * a specific checkpoint from a peer. */
        kernel_serial_print("[CKPT] RESTORE: not yet implemented (Step 5)\n");
        break;
    case CKPT_OP_TREE: {
        static const char* type_names[] = {"ROOT","PART","OBJ","PROC","STRM"};
        kernel_serial_printf("[CKPT] State tree: %u node(s)\n", ckpt_tree_count);
        for (uint32_t i = 0; i < ckpt_tree_count; i++) {
            struct StateTreeNode* nd = &ckpt_tree[i];
            const char* tn = nd->type <= ST_NODE_STREAM ? type_names[nd->type] : "?";
            kernel_serial_printf("  [%u] %s  parent=%u  part=%u  eid=%llu  sz=%llu  dirty=%u\n",
                nd->id, tn, nd->parent_id, nd->partition_id,
                nd->entity_id, nd->size, nd->dirty);
        }
        break;
    }
    default:
        kernel_serial_printf("[CKPT] Unknown opcode: 0x%04x\n", msg->opcode);
    }
}
