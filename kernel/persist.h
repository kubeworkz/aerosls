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
//  Multitenant Isolation Gap Analysis §5 item 9 / §7 item 8 (capacity
//  sizing, "medium ~256 tenant" target): PARTITION_MAX 16->256,
//  PARTITION_ASSIGN_MAX 64->4096, DATABASE_MAX 32->256, DATABASE_GRANT_MAX
//  64->1024, TENANT_MAX 32->256. Every region below whose governing
//  constant grew got its exact new frame count recomputed from real
//  `sizeof()` (not estimated -- verified with a standalone sizeof() probe
//  before writing this layout, the same discipline every prior addition to
//  this file used). This is also the pass that fixed a real, previously
//  latent bug this resize would otherwise have triggered: `persist_write_
//  array()` has no overlap/bounds check of its own, and several regions
//  below used to sit with EXACTLY ZERO slack between the end of one array
//  and the next region's header (e.g. partition_owner_table's old region
//  ended at LBA 5824 exactly where PERSIST_ROWSTORE_HDR_LBA began) -- fine
//  while sizes never changed, but a silent on-disk data-corruption time
//  bomb the moment any of those constants grew. Every region from
//  PERSIST_PART_HDR_LBA onward now carries a deliberate 1-frame safety gap
//  after its own content before the next region's header, so a future
//  resize of any one of these constants can no longer silently overwrite
//  its neighbor. Regions before this point (catalog/records/schemas/
//  programs) are untouched by this pass and keep their original LBAs
//  unchanged.
//
//  LBA 5792  PERSIST_PART_HDR_LBA    1 frame  — partitions header
//  LBA 5800  PERSIST_PART_ENT_LBA    3 frames — partition_table[256]        (10,240 B = 40 B * 256)
//  LBA 5824  PERSIST_PART_ASSIGN_LBA 12 frames — partition_assign_table[4096] (49,152 B = 12 B * 4096, exact)
//  LBA 5920  PERSIST_PART_OWNER_LBA  1 frame  — partition_owner_table[256]  (3,072 B = 12 B * 256)
//            (Multi-Node Partition Scaling Roadmap Phase 2's own region,
//            resized in place for the capacity-sizing pass above)
//  (end LBA 5928, +1 frame safety gap -> next region starts LBA 5936)
//
//  LBA 5936  PERSIST_ROWSTORE_HDR_LBA  1 frame  — row-store header (Phase 16)
//  LBA 5944  PERSIST_ROWSTORE_ENT_LBA 38 frames — table_headers[128] (153,600 B = 1,200 B * 128, exact sizeof(), not the old "~38" estimate)
//  (end LBA 6248, +1 frame safety gap -> next region starts LBA 6256)
//
//  Row PAGE data itself (the bulk, sparse, growing part of Phase 16) does
//  NOT live in this small-struct-array region — it has its own dedicated
//  region at ROWSTORE_LBA_BASE (rowstore.h), the same separation stream.c's
//  STREAM_DATA_LBA_BASE already has from this file's regions.
//
//  Gap Remediation Phase D (docs/AeroSLS-Gap-Remediation-Roadmap-v0.1.md):
//  persistence for the RDBMS/Vector Store subsystems that were RAM-only
//  through Phase 24/Phase 6. Frame counts (unaffected by the capacity-
//  sizing pass -- none of these governing constants changed): RowConstraintDef=680B*64=42.5KiB,
//  RowIndex definitions=96B*16=1.5KiB, VecCollectionHeader=48B*128=6KiB,
//  VecIndex definitions=156B*16=2.4KiB, RowJournalEntry=8344B*16=130.375KiB,
//  RowJournalAttachment=97B*16=1.5KiB.
//
//  LBA 6256  PERSIST_ROW_CONSTRAINT_HDR_LBA  1 frame  — row_constraints[] header
//  LBA 6264  PERSIST_ROW_CONSTRAINT_ENT_LBA 11 frames — row_constraints[64] (~42.5 KiB)
//            direct restore, no rebuild — pure definitions, no derived state (see header comment)
//  (end LBA 6352, +1 frame safety gap -> next region starts LBA 6360)
//
//  LBA 6360  PERSIST_ROW_INDEX_HDR_LBA  1 frame — row-index definitions header
//  LBA 6368  PERSIST_ROW_INDEX_ENT_LBA  1 frame — row_indexes[16] definitions (~1.5 KiB)
//            rebuild-on-boot — restored definitions feed row_index_create(),
//            which re-derives the actual B-tree from already-restored row data
//  (end LBA 6376, +1 frame safety gap -> next region starts LBA 6384)
//
//  LBA 6384  PERSIST_VECSTORE_HDR_LBA  1 frame — vector_collections[] header
//  LBA 6392  PERSIST_VECSTORE_ENT_LBA  2 frames — vector_collections[128] (~6 KiB)
//            direct restore (mirrors PERSIST_ROWSTORE_HDR/ENT_LBA's own shape
//            exactly) — bulk page data restores lazily, see VECSTORE_LBA_BASE
//            (vecstore.h)
//  (end LBA 6408, +1 frame safety gap -> next region starts LBA 6416)
//
//  LBA 6416  PERSIST_VEC_INDEX_HDR_LBA  1 frame — HNSW index definitions header
//  LBA 6424  PERSIST_VEC_INDEX_ENT_LBA  1 frame — vec_indexes[16] definitions (~2.4 KiB)
//            rebuild-on-boot — restored definitions feed vec_index_create()
//            + a backfill scan over the now-restored vecstore collection
//  (end LBA 6432, +1 frame safety gap -> next region starts LBA 6440)
//
//  LBA 6440  PERSIST_ROW_JOURNAL_HDR_LBA     1 frame  — row journal header
//  LBA 6448  PERSIST_ROW_JOURNAL_ENT_LBA    33 frames — row_journal_buffer[16] (~130.4 KiB)
//  LBA 6712  PERSIST_ROW_JOURNAL_ATTACH_LBA  1 frame  — row_journal_attachments[16] (~1.5 KiB)
//            direct restore — an audit trail is inherently historical, not a
//            derived structure that can be rebuilt from current row state
//            (see Phase D's own findings addendum)
//  (end LBA 6720, +1 frame safety gap -> next region starts LBA 6728)
//
//  Database Gap Analysis §1 (docs/AeroSLS-Database-Gap-Analysis-v0.1.md):
//  databases[]/database_grants[]/database_next_id were the one database-
//  layer state cluster with zero persistence — tagged tables' database_id
//  survived reboot (catalog persistence, block 1) while the databases they
//  point at vanished AND database_next_id reset to 1, silently re-issuing
//  ids that stale persisted tags still reference — defeating §1.2's own
//  never-reuse-ids design through the persistence hole. database_next_id
//  rides in the header's third uint32 field (the same header-carried-scalar
//  trick persist_row_constraints() already uses for row_constraint_count).
//  Frame counts recomputed for the capacity-sizing pass (DATABASE_MAX
//  32->256, DATABASE_GRANT_MAX 64->1024): SLSDatabaseEntry=44B*256=11KiB,
//  SLSDatabaseGrant=340B*1024=332.5KiB exactly (85 frames, the single
//  largest region in this file after object_records).
//
//  LBA 6728  PERSIST_DATABASE_HDR_LBA    1 frame  — databases header (+ next_id)
//  LBA 6736  PERSIST_DATABASE_ENT_LBA    3 frames — databases[256] (11,264 B = 44 B * 256)
//  LBA 6760  PERSIST_DATABASE_GRANT_LBA  85 frames — database_grants[1024] (348,160 B = 340 B * 1024, exact)
//            direct restore — pure definitions, no derived state
//  (end LBA 7440, +1 frame safety gap -> next region starts LBA 7448)
//
//  Query-Surface Roadmap Phase 5 (kernel/view.h): views[] had zero
//  persistence at all -- a CREATE VIEW survived only until the next
//  reboot. Pure definitions, no derived state (unlike row_index/vec_index,
//  there's nothing to rebuild -- a view's own stored SELECT text is
//  re-parsed fresh on every query anyway, see sql_exec.c's
//  exec_select_view()), so this is a direct restore, mirroring
//  persist_databases()'s own shape one region down. Frame count from real
//  sizeof() (unaffected by the capacity-sizing pass): SLSViewDef=584B*16=9.125KiB -> 3 frames.
//
//  LBA 7448  PERSIST_VIEW_HDR_LBA  1 frame  — views header
//  LBA 7456  PERSIST_VIEW_ENT_LBA  3 frames — views[16] (~9.1 KiB)
//            direct restore — pure definitions, no derived state
//  (end LBA 7480, +1 frame safety gap -> next region starts LBA 7488)
//
//  Multitenant Isolation Gap Analysis §5 item 1 / §7 item 2: tenants[] --
//  the identity that unifies partition_id and database_id. Pure
//  definitions, direct restore, same shape as persist_databases() one
//  region up -- tenant_next_id rides in the header's third field exactly
//  like database_next_id does, for the identical reason (a fresh boot's
//  bump allocator must not re-issue an id a stale persisted tenant_id
//  reference still holds). Frame count recomputed for the capacity-sizing
//  pass (TENANT_MAX 32->256): SLSTenantEntry=52B*256=13KiB exactly -> 4 frames.
//
//  LBA 7488  PERSIST_TENANT_HDR_LBA  1 frame — tenants header (+ next_id)
//  LBA 7496  PERSIST_TENANT_ENT_LBA  4 frames — tenants[256] (13,312 B = 52 B * 256)
//  (end LBA 7528, +1 frame safety gap -> next region starts LBA 7536)
//
//  Storage Isolation Roadmap Phase 3: real per-partition page sub-ranges.
//  rowstore_partition_cursor[]/vecstore_partition_cursor[] (kernel/
//  rowstore.c, kernel/vecstore.c) are the one genuinely new piece of state
//  this phase needs to survive a reboot correctly -- without it, a restored
//  boot would forget how far each partition's own sub-range had already
//  been consumed and could re-issue an already-live page_id. Deliberately
//  reuses PERSIST_ROWSTORE_HDR/VECSTORE_HDR's own existing header frame
//  (the v2 field, previously always 0) as a one-way format-version marker
//  rather than adding two more header frames -- PERSIST_STORAGE_ISOLATION_
//  PHASE3_MARK in that field means "this snapshot was written by Phase-3-
//  aware code and the array at the LBA below is real"; its absence (any
//  snapshot from before this phase) means restore correctly falls back to
//  each partition's cold-start sub-range start instead of trusting
//  whatever raw bytes happen to sit at a LBA nothing ever wrote before.
//  One-way format change, no migration path for a pre-Phase-3 image's
//  row/vector page ownership -- same accepted trade-off this codebase's
//  own prior one-way format changes documented explicitly (e.g. SQL
//  Feature-Parity Roadmap Phase 4's row-width change). Size unaffected by
//  the capacity-sizing pass despite PARTITION_MAX growing (256 * 4 B =
//  1,024 B either way rounds up to 1 frame), but the LBA moved along with
//  everything else above.
//
//  LBA 7536  PERSIST_ROWSTORE_PARTCURSOR_LBA  1 frame — rowstore_partition_cursor[256] (1,024 B)
//  (end LBA 7544, +1 frame safety gap -> next region starts LBA 7552)
//  LBA 7552  PERSIST_VECSTORE_PARTCURSOR_LBA  1 frame — vecstore_partition_cursor[256] (1,024 B)
//  (end LBA 7560, +1 frame safety gap -> end of file's persisted regions at LBA 7568,
//  comfortably clear of STREAM_DIR_LBA 8192 -- 624 sectors / 78 frames of slack remain)

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
#define PERSIST_PART_ASSIGN_LBA         5824ULL
#define PERSIST_PART_OWNER_LBA          5920ULL   /* Multi-Node Partition Scaling Roadmap Phase 2 */

#define PERSIST_ROWSTORE_HDR_LBA        5936ULL
#define PERSIST_ROWSTORE_ENT_LBA        5944ULL

// ─── Gap Remediation Phase D ────────────────────────────────────────────────
#define PERSIST_ROW_CONSTRAINT_HDR_LBA  6256ULL
#define PERSIST_ROW_CONSTRAINT_ENT_LBA  6264ULL

#define PERSIST_ROW_INDEX_HDR_LBA       6360ULL
#define PERSIST_ROW_INDEX_ENT_LBA       6368ULL

#define PERSIST_VECSTORE_HDR_LBA        6384ULL
#define PERSIST_VECSTORE_ENT_LBA        6392ULL

#define PERSIST_VEC_INDEX_HDR_LBA       6416ULL
#define PERSIST_VEC_INDEX_ENT_LBA       6424ULL

#define PERSIST_ROW_JOURNAL_HDR_LBA     6440ULL
#define PERSIST_ROW_JOURNAL_ENT_LBA     6448ULL
#define PERSIST_ROW_JOURNAL_ATTACH_LBA  6712ULL

// ─── Database Gap Analysis §1 ───────────────────────────────────────────────
#define PERSIST_DATABASE_HDR_LBA        6728ULL
#define PERSIST_DATABASE_ENT_LBA        6736ULL
#define PERSIST_DATABASE_GRANT_LBA      6760ULL

// ─── Query-Surface Roadmap Phase 5 ──────────────────────────────────────────
#define PERSIST_VIEW_HDR_LBA            7448ULL
#define PERSIST_VIEW_ENT_LBA            7456ULL

#define PERSIST_TENANT_HDR_LBA          7488ULL
#define PERSIST_TENANT_ENT_LBA          7496ULL

// ─── Storage Isolation Roadmap Phase 3 ──────────────────────────────────────
#define PERSIST_ROWSTORE_PARTCURSOR_LBA 7536ULL
#define PERSIST_VECSTORE_PARTCURSOR_LBA 7552ULL

// ─── Orchestration Plan Phase 4 (service registry) ──────────────────────────
// services_registry[] is 64 x 80 = 5120 B = 2 frames = 16 sectors, so the
// entry array spans 7584..7600. Placed after the last in-use region
// (VECSTORE_PARTCURSOR ends at 7560) with this layout's 1-frame safety gap
// on each side. tests/persist_lba_layout_host_test.c enforces both the gap
// and the no-overlap invariant -- see that file for why this is checked by
// a test rather than trusted to arithmetic done once.
#define PERSIST_SERVICE_HDR_LBA         7568ULL
#define PERSIST_SERVICE_ENT_LBA         7584ULL

// ─── Orchestration Plan Phase 5 (declarative workloads) ─────────────────────
// workloads[] is 32 x 148 = 4736 B = 2 frames = 16 sectors, spanning
// 7624..7640. The SERVICE entry array ends at 7600; 1-frame gap each side,
// enforced by tests/persist_lba_layout_host_test.c.
#define PERSIST_WORKLOAD_HDR_LBA        7608ULL
#define PERSIST_WORKLOAD_ENT_LBA        7624ULL

// ─── TLS trust anchor ────────────────────────────────────────────────────────
// One frame at LBA 7680, after checkpoint_mgr's entries.
//
//   PERSIST_CKPT_ENT_LBA 7664 + SECTORS_PER_FRAME 8  -> ends 7672
//   +1 frame safety gap, matching every other boundary in this file
//   PERSIST_TLS_LBA      7680 + 8                    -> ends 7688
//   STREAM_DIR_LBA       8192                        -> 504 sectors still free
//
// Holds the node's CA certificate and CA private key, and nothing else. The
// LEAF certificate and its key are deliberately NOT stored: they are
// regenerated every boot and signed by this CA, so the only long-lived secret
// on disk is the one that has to be. A browser that has imported this CA stays
// trusting across reboots, which is the entire point -- three re-imports in one
// afternoon is what made this necessary.
//
// §6.1 applies to exactly one thing now: these bytes. The CA key must be
// provably absent from checkpoint_mgr's walk, and a guard test is what makes
// that more than an intention.
#define PERSIST_TLS_LBA                 7680ULL
#define PERSIST_TLS_MAGIC               0x534C53544C533031ULL   /* "SLSTLS01" */

// One-way format-version marker, written into PERSIST_ROWSTORE_HDR_LBA's/
// PERSIST_VECSTORE_HDR_LBA's own header frame (the v2 field, previously
// always 0) -- see the LBA layout comment above for the full reasoning.
#define PERSIST_STORAGE_ISOLATION_PHASE3_MARK 0x50334C42UL   /* "P3LB" */

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
#define PERSIST_MAGIC_VIEW           0xCAFE00000000000DULL   /* Query-Surface Roadmap Phase 5 */
#define PERSIST_MAGIC_TENANT         0xCAFE00000000000EULL   /* Multitenant Isolation Gap Analysis §5 item 1 */
#define PERSIST_MAGIC_SERVICE        0xCAFE00000000000FULL   /* Orchestration Plan Phase 4 (service registry) */
#define PERSIST_MAGIC_WORKLOAD       0xCAFE000000000010ULL   /* Orchestration Plan Phase 5 (declarative workloads) */

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

// Snapshot views[] → NVMe. Query-Surface Roadmap Phase 5: call after every
// successful view_create()/view_drop(). Pure definitions, direct restore --
// no bump-allocated id and no derived state to recompute (unlike
// persist_databases()'s database_next_id), so the header carries only the
// array's own byte size, nothing else.
void persist_views(void);

// Snapshot tenants[] + tenant_next_id → NVMe. Multitenant Isolation Gap
// Analysis §5 item 1: call after every successful tenant_create(). Pure
// definitions, no derived state — direct restore, same shape as
// persist_databases(). tenant_next_id rides in the header for the same
// reason database_next_id does there — see the LBA layout comment above.
void persist_tenants(void);
void persist_services(void);   /* Orchestration Plan Phase 4 -- services_registry[] */
void persist_workloads(void);  /* Orchestration Plan Phase 5 -- workloads[] */

/* ─── Deferred / batched persistence ──────────────────────────────────────
 * Every persist_*() above rewrites its entire region: persist_records()
 * alone is 323 synchronous 4 KiB NVMe commands (1.26 MiB) per call, and it
 * is called once per direct insert/update/delete. sys_sls_tx_commit()
 * (kernel/transaction.c) applies its staged WAL entries by calling
 * sys_sls_update() in a loop -- each iteration taking the direct-write path
 * and paying that cost again -- so an N-operation commit wrote N * 1.26 MiB
 * to reach a final state that one write would have produced.
 *
 * persist_defer_begin() / persist_defer_end() bracket a batch. Inside the
 * bracket a persist_*() call marks its region pending and returns; the
 * matching persist_defer_end() issues exactly one real write per distinct
 * region touched. This is safe to reason about because persist_*() always
 * writes the CURRENT contents of its array rather than a delta -- so
 * writing once at the end of a batch and writing after every step produce
 * byte-identical results on disk. Nothing is dropped and no durability
 * window is added beyond the bracket's own duration.
 *
 * Nestable: brackets are depth-counted, so an inner bracket does not flush
 * early. An unbalanced persist_defer_end() is ignored rather than
 * underflowing.
 *
 * Scope note, deliberate: this is NOT wired into flush_daemon_tick(). That
 * runs on Core 1 (kernel/smp.c), whereas every persist_*() caller today
 * runs on the BSP (kernel/kernel.c runs both http_server_run() and
 * sls_shell_loop() there). persist.c's p_buf DMA staging buffer is a single
 * shared static with no lock, so moving persist writes onto the AP would
 * race the BSP's and tear the buffer. Cross-core deferred flushing needs
 * locking this kernel does not have; see
 * docs/AeroSLS-Persist-Write-Amplification-Scoping-v0.1.md.
 */
void     persist_defer_begin(void);
void     persist_defer_end(void);

/* Introspection, for tests and diagnostics. */
int      persist_defer_active(void);
uint32_t persist_defer_pending_mask(void);

/* ─── Shadow-compare writes ───────────────────────────────────────────────
 * Batching (above) reduced how many COMMANDS a snapshot costs; it did not
 * reduce the BYTES. persist_records() still rewrote all 1.26 MiB of
 * object_records[] to change as little as one 321-byte field. It now writes
 * only the 4 KiB frames whose contents actually differ from the previous
 * write, coalescing consecutive dirty frames into multi-page commands.
 *
 * Dirtiness is derived by comparing against a shadow copy, NOT from marks
 * supplied by mutation sites. That is the load-bearing design choice: a
 * missed mark would mean a change that lives in RAM, is never written, and
 * silently vanishes on reboot -- invisible until someone notices absent
 * data. Deriving it from the bytes makes that failure impossible and needs
 * no cooperation from any current or future call site.
 *
 * The invariant this rests on is that the shadow matches what is on disk.
 * Today that holds because each shadowed region has exactly one writer.
 * ANY new writer to a shadowed region must call persist_shadow_invalidate(),
 * or real changes will be silently skipped.
 *
 * Currently applied to object_records[] only -- the largest region (322
 * frames) and the only one on the per-mutation hot path.
 */

/* Forces the next write of every shadowed region to be a full write. Call
 * after any path that changes a shadowed region's on-disk image outside
 * persist_*(), or when the image may be from a different kernel build. */
void     persist_shadow_invalidate(void);

/* Verify mode: after each shadow-compared write, read the whole region back
 * and compare against memory, logging loudly and self-repairing with a full
 * rewrite on divergence. Off by default (costs a full-region read per
 * write). Intended for tests and for bring-up after changing a shadowed
 * region's write path -- it turns "a needed write was skipped" from a
 * silent, reboot-surviving data loss into an immediate, visible failure. */
void     persist_verify_set(int on);
int      persist_verify_get(void);

/* Frames actually put on the wire by the last shadow-compared write. */
uint32_t persist_last_frames_written(void);

#endif /* PERSIST_H */
