#ifndef PERSIST_H
#define PERSIST_H

#include <stdint.h>

// ─── NVMe Snapshot LBA Layout ─────────────────────────────────────────────────
//
// All regions sit below STREAM_DIR_LBA (8192), avoiding any collision with the
// stream subsystem.  Each "frame" = 4 KiB = 8 × 512-byte NVMe sectors.
//
// Expanded for CATALOG_MAX_OBJECTS=128, RECORD_KEY_LEN=64, RECORD_VAL_LEN=256:
//
//  LBA 1024  PERSIST_CAT_HDR_LBA   1 frame   — catalog header
//  LBA 1032  PERSIST_CAT_ENT_LBA   4 frames  — object_catalog[128]  (~14 KiB)
//  LBA 1064  PERSIST_ROLE_ENT_LBA  1 frame   — role_table[64]       (~768 B)
//
//  LBA 2048  PERSIST_REC_HDR_LBA   1 frame   — records header
//  LBA 2056  PERSIST_REC_ENT_LBA 322 frames  — object_records[128]  (~1.26 MiB)
//  (end LBA 4632)
//
//  LBA 4640  PERSIST_SCH_HDR_LBA   1 frame   — schemas header
//  LBA 4648  PERSIST_SCH_ENT_LBA  73 frames  — object_schemas[128]  (~290 KiB)
//  (end LBA 5232)
//
//  LBA 5248  PERSIST_PROG_HDR_LBA  1 frame   — programs header
//  LBA 5256  PERSIST_PROG_DAT_LBA 65 frames  — service_binaries[16] (~257 KiB)
//  (end LBA 5776)
//
//  LBA 5792  PERSIST_PART_HDR_LBA    1 frame — partitions header
//  LBA 5800  PERSIST_PART_ENT_LBA    1 frame — partition_table[16]        (~640 B)
//  LBA 5808  PERSIST_PART_ASSIGN_LBA 1 frame — partition_assign_table[64] (~768 B)
//  (end LBA 5816)
//
//  LBA 5824  PERSIST_ROWSTORE_HDR_LBA  1 frame  — row-store header (Phase 16)
//  LBA 5832  PERSIST_ROWSTORE_ENT_LBA ~38 frames — table_headers[128] (~150 KiB)
//  (end LBA ~6144 — comfortably clear of STREAM_DIR_LBA 8192)
//
//  Row PAGE data itself (the bulk, sparse, growing part of Phase 16) does
//  NOT live in this small-struct-array region — it has its own dedicated
//  region at ROWSTORE_LBA_BASE (rowstore.h), the same separation stream.c's
//  STREAM_DATA_LBA_BASE already has from this file's regions.

#define PERSIST_CAT_HDR_LBA   1024ULL
#define PERSIST_CAT_ENT_LBA   1032ULL
#define PERSIST_ROLE_ENT_LBA  1064ULL

#define PERSIST_REC_HDR_LBA   2048ULL
#define PERSIST_REC_ENT_LBA   2056ULL

#define PERSIST_SCH_HDR_LBA   4640ULL
#define PERSIST_SCH_ENT_LBA   4648ULL

#define PERSIST_PROG_HDR_LBA  5248ULL
#define PERSIST_PROG_DAT_LBA  5256ULL

#define PERSIST_PART_HDR_LBA    5792ULL
#define PERSIST_PART_ENT_LBA    5800ULL
#define PERSIST_PART_ASSIGN_LBA 5808ULL

#define PERSIST_ROWSTORE_HDR_LBA 5824ULL
#define PERSIST_ROWSTORE_ENT_LBA 5832ULL

// ─── Snapshot magic values ────────────────────────────────────────────────────
// Distinct per-subsystem so a stale/partial write on one region is detectable.
#define PERSIST_MAGIC_CAT       0xCAFE000000000001ULL
#define PERSIST_MAGIC_REC       0xCAFE000000000002ULL
#define PERSIST_MAGIC_SCH       0xCAFE000000000003ULL
#define PERSIST_MAGIC_PROG      0xCAFE000000000004ULL
#define PERSIST_MAGIC_PART      0xCAFE000000000005ULL
#define PERSIST_MAGIC_ROWSTORE  0xCAFE000000000006ULL

// ─── Public API ──────────────────────────────────────────────────────────────

// Called once at boot (kernel.c step 7b) BEFORE stream_init(), after
// nvme_io_init() succeeds.  Restores object_catalog[], role_table[],
// object_records[], object_schemas[], and service_binaries[] from NVMe.
// Safe to call when NVMe is unavailable — guards internally with io_sq/io_cq.
void persist_restore_all(void);

// Snapshot object_catalog[] + role_table[] → NVMe.
// Call after every successful sys_sls_valloc / sys_sls_vfree / sys_sls_role_set.
void persist_catalog(void);

// Snapshot object_records[] → NVMe.
// Call after every successful direct-write sys_sls_insert / update / delete.
// (WAL-committed paths trigger this indirectly via the direct-write sys_sls_update
//  calls made during tx_commit replay.)
void persist_records(void);

// Snapshot object_schemas[] → NVMe.
// Call after every successful sys_sls_schema_set.
void persist_schemas(void);

// Snapshot service_binaries[] → NVMe.
// Call after the final chunk (is_last=1) is written in sys_sls_upload_binary.
void persist_programs(void);

// Snapshot partition_table[] + partition_assign_table[] → NVMe. Phase 10
// (LPAR persistence). Call after every successful partition_create() /
// partition_assign_uid().
void persist_partitions(void);

// Snapshot table_headers[] (+ the row-page bump-allocator cursor) → NVMe.
// Phase 16 (relational layer). Call after every successful
// rowstore_create_table() / row insert / row delete (row_count and the
// page chain change). NOTE: this does NOT persist row page data itself —
// that's rowstore.c's own direct nvme_write_sync() per page, a separate
// mechanism for a separate (large, sparse) kind of data. See rowstore.h.
void persist_rowstore_headers(void);

#endif /* PERSIST_H */
