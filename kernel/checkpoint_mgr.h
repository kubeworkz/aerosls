#ifndef CHECKPOINT_MGR_H
#define CHECKPOINT_MGR_H

#include <stdint.h>

struct IPCMessage;  /* forward declaration — defined in ipc.h */

/*
 * checkpoint_mgr.h — coordinated full-system checkpoint via IPC
 * (Core Backup Strategies, Step 1).
 *
 * Wraps the existing per-subsystem persist_*() calls into a single atomic
 * batch that writes all kernel state to NVMe as one checkpoint, records a
 * sequence number and timestamp, and reports status via the IPC bus.
 *
 * This module does NOT duplicate persist.c's storage logic. It orchestrates
 * calling every persist_*() function inside a persist_defer_begin/end
 * bracket, then writes a small checkpoint header to its own dedicated LBA
 * so a restore can enumerate what snapshots exist and when they were taken.
 */

// ─── Checkpoint Header (persisted to NVMe) ────────────────────────────────────
#define CKPT_MAGIC          0x434B5054UL   /* "CKPT" */
#define CKPT_VERSION        1
#define CKPT_MAX_HISTORY    8

// NVMe LBA region (after PERSIST_WORKLOAD_ENT, before STREAM_DIR_LBA)
#define PERSIST_CKPT_HDR_LBA  7656ULL
#define PERSIST_CKPT_ENT_LBA  7664ULL
#define PERSIST_CKPT_MAGIC_NV 0xCAFE000000000011ULL

struct CkptHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t sequence;
    uint64_t timestamp_tsc;
    uint32_t num_objects;
    uint32_t num_partitions;
    uint32_t num_streams;
    uint32_t node_id;
    uint8_t  status;         /* 0=incomplete, 1=committed */
    uint8_t  _pad[7];
};

// ─── Checkpoint Status (returned via IPC payload[0]) ──────────────────────────
#define CKPT_STATUS_OK             0
#define CKPT_STATUS_BUSY           1   /* checkpoint already in progress */
#define CKPT_STATUS_NO_NVME        2   /* NVMe not available */
#define CKPT_STATUS_WRITE_FAILED   3

// ─── Public API ───────────────────────────────────────────────────────────────
void     checkpoint_mgr_init(void);
int      checkpoint_trigger(void);         /* returns CKPT_STATUS_* */
uint64_t checkpoint_last_sequence(void);
uint64_t checkpoint_last_timestamp(void);
uint32_t checkpoint_count(void);

// IPC handler (called from microkernel_service_poll via the service table)
void     checkpoint_mgr_handler(struct IPCMessage* msg);

#endif /* CHECKPOINT_MGR_H */
