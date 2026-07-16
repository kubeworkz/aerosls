#ifndef PERSIST_H
#define PERSIST_H

#include <stdint.h>

// ─── NVMe Snapshot LBA Layout ─────────────────────────────────────────────────
//
// All regions sit below STREAM_DIR_LBA (8192), avoiding any collision with the
// stream subsystem.  Each "frame" = 4 KiB = 8 × 512-byte NVMe sectors.
//
//  LBA 1024  PERSIST_CAT_HDR_LBA   1 frame  — catalog + role header
//  LBA 1032  PERSIST_CAT_ENT_LBA   2 frames — object_catalog[64]
//  LBA 1048  PERSIST_ROLE_ENT_LBA  1 frame  — role_table[64]
//
//  LBA 2048  PERSIST_REC_HDR_LBA   1 frame  — records header
//  LBA 2056  PERSIST_REC_ENT_LBA  57 frames — object_records[64]  (~232 KiB)
//
//  LBA 2568  PERSIST_SCH_HDR_LBA   1 frame  — schemas header
//  LBA 2576  PERSIST_SCH_ENT_LBA  29 frames — object_schemas[64]  (~116 KiB)
//
//  LBA 4096  PERSIST_PROG_HDR_LBA  1 frame  — programs header
//  LBA 4104  PERSIST_PROG_DAT_LBA 17 frames — service_binaries[4] (~66 KiB)

#define PERSIST_CAT_HDR_LBA   1024ULL
#define PERSIST_CAT_ENT_LBA   1032ULL
#define PERSIST_ROLE_ENT_LBA  1048ULL

#define PERSIST_REC_HDR_LBA   2048ULL
#define PERSIST_REC_ENT_LBA   2056ULL

#define PERSIST_SCH_HDR_LBA   2568ULL
#define PERSIST_SCH_ENT_LBA   2576ULL

#define PERSIST_PROG_HDR_LBA  4096ULL
#define PERSIST_PROG_DAT_LBA  4104ULL

// ─── Snapshot magic values ────────────────────────────────────────────────────
// Distinct per-subsystem so a stale/partial write on one region is detectable.
#define PERSIST_MAGIC_CAT   0xCAFE000000000001ULL
#define PERSIST_MAGIC_REC   0xCAFE000000000002ULL
#define PERSIST_MAGIC_SCH   0xCAFE000000000003ULL
#define PERSIST_MAGIC_PROG  0xCAFE000000000004ULL

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

#endif /* PERSIST_H */
