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
//  LBA 5816  PERSIST_PART_OWNER_LBA  1 frame — partition_owner_table[16]  (~192 B)
//            (Multi-Node Partition Scaling Roadmap Phase 2 -- fits exactly in
//            the 1-frame gap this region already had before the next region's
//            header at 5824, no shift of anything downstream needed)
//  (end LBA 5824)
//
//  LBA 5824  PERSIST_ROWSTORE_HDR_LBA  1 frame  — row-store header (Phase 16)
//  LBA 5832  PERSIST_ROWSTORE_ENT_LBA ~38 frames — table_headers[128] (~150 KiB)
//  (end LBA ~6144 — comfortably clear of STREAM_DIR_LBA 8192)
//
//  Row PAGE data itself (the bulk, sparse, growing part of Phase 16) does
//  NOT live in this small-struct-array region — it has its own dedicated
//  region at ROWSTORE_LBA_BASE (rowstore.h), the same separation stream.c's
//  STREAM_DATA_LBA_BASE already has from this file's regions.
//
//  Gap Remediation Phase D (docs/AeroSLS-Gap-Remediation-Roadmap-v0.1.md):
//  persistence for the RDBMS/Vector Store subsystems that were RAM-only
//  through Phase 24/Phase 6. Exact frame counts computed from real
//  `sizeof()`, not estimated (see Phase D's own findings addendum) —
//  RowConstraintDef=680B*64=42.5KiB, RowIndex definitions=96B*16=1.5KiB,
//  VecCollectionHeader=48B*128=6KiB, VecIndex definitions=156B*16=2.4KiB,
//  RowJournalEntry=8336B*16=130.25KiB, RowJournalAttachment=97B*16=1.5KiB.
//
//  LBA 6160  PERSIST_ROW_CONSTRAINT_HDR_LBA  1 frame  — row_constraints[] header
//  LBA 6168  PERSIST_ROW_CONSTRAINT_ENT_LBA 11 frames — row_constraints[64] (~42.5 KiB)
//            direct restore, no rebuild — pure definitions, no derived state (see header comment)
//  (end LBA 6256)
//
//  LBA 6272  PERSIST_ROW_INDEX_HDR_LBA  1 frame — row-index definitions header
//  LBA 6280  PERSIST_ROW_INDEX_ENT_LBA  1 frame — row_indexes[16] definitions (~1.5 KiB)
//            rebuild-on-boot — restored definitions feed row_index_create(),
//            which re-derives the actual B-tree from already-restored row data
//  (end LBA 6288)
//
//  LBA 6304  PERSIST_VECSTORE_HDR_LBA  1 frame — vector_collections[] header
//  LBA 6312  PERSIST_VECSTORE_ENT_LBA  2 frames — vector_collections[128] (~6 KiB)
//            direct restore (mirrors PERSIST_ROWSTORE_HDR/ENT_LBA's own shape
//            exactly) — bulk page data restores lazily, see VECSTORE_LBA_BASE
//            (vecstore.h)
//  (end LBA 6328)
//
//  LBA 6344  PERSIST_VEC_INDEX_HDR_LBA  1 frame — HNSW index definitions header
//  LBA 6352  PERSIST_VEC_INDEX_ENT_LBA  1 frame — vec_indexes[16] definitions (~2.4 KiB)
//            rebuild-on-boot — restored definitions feed vec_index_create()
//            + a backfill scan over the now-restored vecstore collection
//  (end LBA 6360)
//
//  LBA 6376  PERSIST_ROW_JOURNAL_HDR_LBA     1 frame  — row journal header
//  LBA 6384  PERSIST_ROW_JOURNAL_ENT_LBA    33 frames — row_journal_buffer[16] (~130 KiB)
//  LBA 6664  PERSIST_ROW_JOURNAL_ATTACH_LBA  1 frame  — row_journal_attachments[16] (~1.5 KiB)
//            direct restore — an audit trail is inherently historical, not a
//            derived structure that can be rebuilt from current row state
//            (see Phase D's own findings addendum)
//  (end LBA 6672)
//
//  Database Gap Analysis §1 (docs/AeroSLS-Database-Gap-Analysis-v0.1.md):
//  databases[]/database_grants[]/database_next_id were the one database-
//  layer state cluster with zero persistence — tagged tables' database_id
//  survived reboot (catalog persistence, block 1) while the databases they
//  point at vanished AND database_next_id reset to 1, silently re-issuing
//  ids that stale persisted tags still reference — defeating §1.2's own
//  never-reuse-ids design through the persistence hole. Frame counts from
//  real sizeof(): SLSDatabaseEntry=44B*32=1.4KiB, SLSDatabaseGrant=340B*64
//  =21.25KiB. database_next_id rides in the header's third uint32 field
//  (the same header-carried-scalar trick persist_row_constraints() already
//  uses for row_constraint_count).
//
//  LBA 6688  PERSIST_DATABASE_HDR_LBA    1 frame  — databases header (+ next_id)
//  LBA 6696  PERSIST_DATABASE_ENT_LBA    1 frame  — databases[32] (~1.4 KiB)
//  LBA 6704  PERSIST_DATABASE_GRANT_LBA  6 frames — database_grants[64] (~21.25 KiB)
//            direct restore — pure definitions, no derived state
//  (end LBA 6752 — comfortably clear of STREAM_DIR_LBA 8192)

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
#define PERSIST_PART_OWNER_LBA  5816ULL   /* Multi-Node Partition Scaling Roadmap Phase 2 */

#define PERSIST_ROWSTORE_HDR_LBA 5824ULL
#define PERSIST_ROWSTORE_ENT_LBA 5832ULL

// ─── Gap Remediation Phase D ────────────────────────────────────────────────
#define PERSIST_ROW_CONSTRAINT_HDR_LBA 6160ULL
#define PERSIST_ROW_CONSTRAINT_ENT_LBA 6168ULL

#define PERSIST_ROW_INDEX_HDR_LBA 6272ULL
#define PERSIST_ROW_INDEX_ENT_LBA 6280ULL

#define PERSIST_VECSTORE_HDR_LBA 6304ULL
#define PERSIST_VECSTORE_ENT_LBA 6312ULL

#define PERSIST_VEC_INDEX_HDR_LBA 6344ULL
#define PERSIST_VEC_INDEX_ENT_LBA 6352ULL

#define PERSIST_ROW_JOURNAL_HDR_LBA    6376ULL
#define PERSIST_ROW_JOURNAL_ENT_LBA    6384ULL
#define PERSIST_ROW_JOURNAL_ATTACH_LBA 6664ULL

// ─── Database Gap Analysis §1 ───────────────────────────────────────────────
#define PERSIST_DATABASE_HDR_LBA   6688ULL
#define PERSIST_DATABASE_ENT_LBA   6696ULL
#define PERSIST_DATABASE_GRANT_LBA 6704ULL

// ─── Snapshot magic values ────────────────────────────────────────────────────
// Distinct per-subsystem so a stale/partial write on one region is detectable.
#define PERSIST_MAGIC_CAT            0xCAFE000000000001ULL
#define PERSIST_MAGIC_REC            0xCAFE000000000002ULL
#define PERSIST_MAGIC_SCH            0xCAFE000000000003ULL
#define PERSIST_MAGIC_PROG           0xCAFE000000000004ULL
#define PERSIST_MAGIC_PART           0xCAFE000000000005ULL
#define PERSIST_MAGIC_ROWSTORE       0xCAFE000000000006ULL
#define PERSIST_MAGIC_ROW_CONSTRAINT 0xCAFE000000000007ULL
#define PERSIST_MAGIC_ROW_INDEX      0xCAFE000000000008ULL
#define PERSIST_MAGIC_VECSTORE       0xCAFE000000000009ULL
#define PERSIST_MAGIC_VEC_INDEX      0xCAFE00000000000AULL
#define PERSIST_MAGIC_ROW_JOURNAL    0xCAFE00000000000BULL
#define PERSIST_MAGIC_DATABASE       0xCAFE00000000000CULL   /* Database Gap Analysis §1 */

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

// ─── Gap Remediation Phase D ────────────────────────────────────────────────
// See docs/AeroSLS-Gap-Remediation-Roadmap-v0.1.md's Phase D findings
// addendum for the full design rationale (direct-persist vs. rebuild-on-
// boot, per subsystem).

// Snapshot row_constraints[] → NVMe. Call after every successful
// row_constraint_add_unique/_not_null/_range/_reference(). Pure definitions,
// no derived runtime state — direct restore, no rebuild step needed.
void persist_row_constraints(void);

// Snapshot row_indexes[] → NVMe. Call after every successful
// row_index_create(). NOTE: restored definitions are NOT loaded directly
// into the live row_indexes[]/B-tree node pool — persist_restore_all()
// feeds them through the real row_index_create() again, which re-derives
// the actual B-tree from already-restored row data (rebuild-on-boot; see
// row_index.h's own "persistence explicitly out of scope" comment, now
// superseded by Phase D the same way vecstore.h's was).
void persist_row_index_defs(void);

// Snapshot vector_collections[] → NVMe. Call after every successful
// vecstore_create_collection() / vecstore_insert() / vecstore_delete().
// Mirrors persist_rowstore_headers()'s exact shape — bulk page data is NOT
// written here, see VECSTORE_LBA_BASE (vecstore.h).
void persist_vecstore_headers(void);

// Snapshot vec_indexes[] → NVMe. Call after every successful
// vec_index_create(). Same rebuild-on-boot relationship persist_row_index_
// defs() has with row_index_create() — restored definitions feed
// vec_index_create() again, then a backfill scan over the now-restored
// vecstore collection rebuilds the actual HNSW graph (vec_index_create()
// itself never backfills — see vec_index.h's own point 7 — so
// persist_restore_all() does the one-time backfill scan itself).
void persist_vec_index_defs(void);

// Snapshot row_journal_buffer[] + row_journal_attachments[] → NVMe. Call
// after every row_journal_notify_insert/update/delete() and every
// row_journal_commit_tx()/_rollback_tx() — an audit trail is inherently
// historical, not a derived structure that can be rebuilt from current row
// state, so (unlike row_index/vec_index above) this persists directly,
// full-array-rewrite-per-mutation, matching persist_records()'s own
// established trade-off for a small-enough struct array.
void persist_row_journal(void);

// Snapshot databases[] + database_grants[] + database_next_id → NVMe.
// Database Gap Analysis §1: call after every successful database_create()/
// database_drop()/database_grant_uid()/database_grant_group(). Pure
// definitions, no derived state — direct restore. database_next_id rides
// in the header (see the LBA layout comment above); restoring it is the
// load-bearing part — without it, a fresh boot's bump allocator re-issues
// ids that stale persisted database_id tags still reference, the silent-
// reattachment failure §1.2 (Database Namespace roadmap) exists to prevent.
// database_grant_count is deliberately NOT persisted — it's a high-water
// mark recomputed from the restored array's active flags at restore time,
// the same derived-not-stored treatment row_journal_attachment_count
// already gets.
void persist_databases(void);

#endif /* PERSIST_H */
