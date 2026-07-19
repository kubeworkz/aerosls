// kernel/persist.c — L1/L2 kernel state persistence (Aurora-style snapshots)
//
// Strategy: write-on-mutation snapshots of all volatile kernel struct arrays to
// fixed NVMe LBA regions.  On boot, persist_restore_all() reads them back before
// stream_init() runs, making DB tables, records, schemas, and program binaries
// survive kernel reboots — the same guarantee streams already have.
//
// Inspired by Aurora SLS (rcslab/aurora): incremental checkpoints to NVMe.
// Simplified for AeroSLS: full-array writes (structs are small) rather than
// page-dirty tracking.  Trade-off: a few extra NVMe frames written per mutation;
// acceptable for a research/demo OS.

#include "persist.h"
#include "object_catalog.h"
#include "loader.h"
#include "kernel_io.h"
#include "partition.h"
#include "rowstore.h"
#include "row_index.h"       // Gap Remediation Phase D
#include "row_constraint.h"  // Gap Remediation Phase D
#include "row_journal.h"     // Gap Remediation Phase D
#include "vecstore.h"        // Gap Remediation Phase D
#include "vec_index.h"       // Gap Remediation Phase D
#include "mvcc.h"            // Gap Remediation Phase D -- mvcc_bootstrap_from_rowstore()
#include "../drivers/nvme_io.h"

// ─── 4 KiB DMA staging buffer (page-aligned for NVMe PRP) ────────────────────
// The NVMe driver passes this address as the PRP1 DMA buffer.  Physical alignment
// to 4096 is required; the linker places page-aligned BSS objects correctly.
static uint8_t __attribute__((aligned(4096))) p_buf[4096];

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void p_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void p_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}

// Write `total_bytes` from `src` to successive 4-KiB NVMe frames starting at
// `lba`.  Each frame is 8 NVMe 512-byte sectors.
static void persist_write_array(const void* src, uint32_t total_bytes, uint64_t lba) {
    const uint8_t* p = (const uint8_t*)src;
    uint32_t rem = total_bytes;
    while (rem > 0) {
        uint32_t chunk = rem < 4096u ? rem : 4096u;
        p_memset(p_buf, 0, 4096);
        p_memcpy(p_buf, p, chunk);
        nvme_write_sync(lba, p_buf);
        p   += chunk;
        rem -= chunk;
        lba += 8;   // advance by 8 sectors (= 1 frame)
    }
}

// Read back `total_bytes` into `dst` from NVMe.  Stops silently on NVMe error
// (the caller will detect corruption via the magic-number mismatch on next boot).
static void persist_read_array(void* dst, uint32_t total_bytes, uint64_t lba) {
    uint8_t* d = (uint8_t*)dst;
    uint32_t rem = total_bytes;
    while (rem > 0) {
        if (nvme_read_sync(lba, p_buf) != 0) return;
        uint32_t chunk = rem < 4096u ? rem : 4096u;
        p_memcpy(d, p_buf, chunk);
        d   += chunk;
        rem -= chunk;
        lba += 8;
    }
}

// Write a 4-KiB header frame: 8-byte magic + three uint32_t fields.
static void write_hdr(uint64_t lba, uint64_t magic,
                      uint32_t v0, uint32_t v1, uint32_t v2) {
    p_memset(p_buf, 0, 4096);
    p_memcpy(p_buf +  0, &magic, 8);
    p_memcpy(p_buf +  8, &v0,   4);
    p_memcpy(p_buf + 12, &v1,   4);
    p_memcpy(p_buf + 16, &v2,   4);
    nvme_write_sync(lba, p_buf);
}

// ─── Gap Remediation Phase D: HNSW backfill helper ───────────────────────────
// vec_index_create() never backfills an already-populated collection (see
// vec_index.h's own point 7) -- restoring a persisted HNSW index definition
// needs exactly that backfill, once, right after creation. This callback
// (used only by persist_restore_all()'s vec-index restore block) feeds every
// already-restored entry in the collection through vec_index_notify_
// insert(), the same auto-maintenance entry point vecstore_insert() calls
// on every live write -- reused here rather than duplicated.
struct persist_vec_backfill_ctx { const char* collection_name; };
static void persist_vec_backfill_cb(struct VecId id, uint64_t external_id,
                                    const struct VecValues* values, void* ctxp) {
    struct persist_vec_backfill_ctx* ctx = (struct persist_vec_backfill_ctx*)ctxp;
    vec_index_notify_insert(0, ctx->collection_name, id, external_id, values);
}

// ─── persist_catalog ─────────────────────────────────────────────────────────
// Writes object_catalog[] and role_table[].  Called after valloc/vfree/role_set.
void persist_catalog(void) {
    if (!io_sq || !io_cq) return;
    uint32_t cat_bytes  = (uint32_t)sizeof(object_catalog);
    uint32_t role_bytes = (uint32_t)sizeof(role_table);
    write_hdr(PERSIST_CAT_HDR_LBA, PERSIST_MAGIC_CAT,
              object_catalog_count, cat_bytes, role_bytes);
    persist_write_array(object_catalog, cat_bytes,  PERSIST_CAT_ENT_LBA);
    persist_write_array(role_table,     role_bytes, PERSIST_ROLE_ENT_LBA);
    kernel_serial_print("[PERSIST] Catalog snapshot written.\n");
}

// ─── persist_records ─────────────────────────────────────────────────────────
// Writes the full object_records[] array.  Called after direct insert/update/delete.
// Note: writes ~232 KiB (57 NVMe frames) — acceptable for a research kernel.
void persist_records(void) {
    if (!io_sq || !io_cq) return;
    uint32_t rec_bytes = (uint32_t)sizeof(object_records);
    write_hdr(PERSIST_REC_HDR_LBA, PERSIST_MAGIC_REC, rec_bytes, 0, 0);
    persist_write_array(object_records, rec_bytes, PERSIST_REC_ENT_LBA);
    kernel_serial_print("[PERSIST] Records snapshot written.\n");
}

// ─── persist_schemas ─────────────────────────────────────────────────────────
void persist_schemas(void) {
    if (!io_sq || !io_cq) return;
    uint32_t sch_bytes = (uint32_t)sizeof(object_schemas);
    write_hdr(PERSIST_SCH_HDR_LBA, PERSIST_MAGIC_SCH, sch_bytes, 0, 0);
    persist_write_array(object_schemas, sch_bytes, PERSIST_SCH_ENT_LBA);
    kernel_serial_print("[PERSIST] Schemas snapshot written.\n");
}

// ─── persist_programs ────────────────────────────────────────────────────────
// Writes service_binaries[] (name, object_id, raw binary data, size, flags).
// Called after the final upload chunk (is_last=1) so only complete binaries
// are snapshotted.
void persist_programs(void) {
    if (!io_sq || !io_cq) return;
    uint32_t prog_bytes = (uint32_t)sizeof(service_binaries);
    write_hdr(PERSIST_PROG_HDR_LBA, PERSIST_MAGIC_PROG, prog_bytes, 0, 0);
    persist_write_array(service_binaries, prog_bytes, PERSIST_PROG_DAT_LBA);
    kernel_serial_print("[PERSIST] Programs snapshot written.\n");
}

// ─── persist_partitions ───────────────────────────────────────────────────────
// Phase 10 (LPAR persistence). Writes partition_table[] and
// partition_assign_table[] together in one header, same "two arrays, one
// magic-tagged header" shape persist_catalog() already uses for
// object_catalog[]/role_table[] — partition_table[]/partition_assign_table[]
// are that same pair's sibling (defined-partitions table + uid-assignment
// table), so the persistence shape matches on purpose.
void persist_partitions(void) {
    if (!io_sq || !io_cq) return;
    uint32_t part_bytes   = (uint32_t)sizeof(partition_table);
    uint32_t assign_bytes = (uint32_t)sizeof(partition_assign_table);
    write_hdr(PERSIST_PART_HDR_LBA, PERSIST_MAGIC_PART,
              part_bytes, assign_bytes, 0);
    persist_write_array(partition_table,        part_bytes,   PERSIST_PART_ENT_LBA);
    persist_write_array(partition_assign_table, assign_bytes, PERSIST_PART_ASSIGN_LBA);
    kernel_serial_print("[PERSIST] Partitions snapshot written.\n");
}

// ─── persist_rowstore_headers ─────────────────────────────────────────────────
// Phase 16 (relational layer). Writes table_headers[] plus the page-pool
// bump-allocator cursor (rowstore_next_free_page_id, stashed in the header
// frame's v1 slot — same "steal a uint32 slot in write_hdr()" trick this
// file has no dedicated pattern for otherwise). Row PAGE data is NOT
// written here — see persist.h's comment on this function.
void persist_rowstore_headers(void) {
    if (!io_sq || !io_cq) return;
    uint32_t hdr_bytes = (uint32_t)sizeof(table_headers);
    write_hdr(PERSIST_ROWSTORE_HDR_LBA, PERSIST_MAGIC_ROWSTORE,
              hdr_bytes, rowstore_next_free_page_id, 0);
    persist_write_array(table_headers, hdr_bytes, PERSIST_ROWSTORE_ENT_LBA);
    kernel_serial_print("[PERSIST] Row-store table headers snapshot written.\n");
}

// ─── Gap Remediation Phase D ─────────────────────────────────────────────────

// persist_row_constraints — pure definitions, no derived runtime state (see
// persist.h's own comment). Called after every successful row_constraint_
// add_unique/_not_null/_range/_reference().
void persist_row_constraints(void) {
    if (!io_sq || !io_cq) return;
    uint32_t bytes = (uint32_t)sizeof(row_constraints);
    write_hdr(PERSIST_ROW_CONSTRAINT_HDR_LBA, PERSIST_MAGIC_ROW_CONSTRAINT,
              row_constraint_count, bytes, 0);
    persist_write_array(row_constraints, bytes, PERSIST_ROW_CONSTRAINT_ENT_LBA);
    kernel_serial_print("[PERSIST] Row constraints snapshot written.\n");
}

// persist_row_index_defs — snapshots the live row_indexes[] array as-is
// (root_node/entry_count included), but persist_restore_all()'s own restore
// block only trusts index_name/table_object_id/column_index from it -- the
// B-tree itself is rebuilt fresh via row_index_create(), not replayed from
// stale node-pool indices (see row_index_create()'s own node pool, which is
// never persisted at all -- see PERSIST_ROW_INDEX_HDR_LBA's own comment in
// persist.h). Called after every successful row_index_create().
void persist_row_index_defs(void) {
    if (!io_sq || !io_cq) return;
    uint32_t bytes = (uint32_t)sizeof(row_indexes);
    write_hdr(PERSIST_ROW_INDEX_HDR_LBA, PERSIST_MAGIC_ROW_INDEX, bytes, 0, 0);
    persist_write_array(row_indexes, bytes, PERSIST_ROW_INDEX_ENT_LBA);
    kernel_serial_print("[PERSIST] Row index definitions snapshot written.\n");
}

// persist_vecstore_headers — mirrors persist_rowstore_headers()'s exact
// shape (header + one small struct array; bulk page data is a separate,
// directly-managed NVMe region -- see VECSTORE_LBA_BASE, vecstore.h).
// Called after every successful vecstore_create_collection() / vecstore_
// insert() / vecstore_delete().
void persist_vecstore_headers(void) {
    if (!io_sq || !io_cq) return;
    uint32_t hdr_bytes = (uint32_t)sizeof(vector_collections);
    write_hdr(PERSIST_VECSTORE_HDR_LBA, PERSIST_MAGIC_VECSTORE,
              hdr_bytes, vecstore_next_free_page_id, 0);
    persist_write_array(vector_collections, hdr_bytes, PERSIST_VECSTORE_ENT_LBA);
    kernel_serial_print("[PERSIST] Vecstore collection headers snapshot written.\n");
}

// persist_vec_index_defs — same "snapshot as-is, restore trusts only the
// definitional fields" relationship persist_row_index_defs() has with
// row_index_create() above, mirrored here for vec_index_create(). Called
// after every successful vec_index_create().
void persist_vec_index_defs(void) {
    if (!io_sq || !io_cq) return;
    uint32_t bytes = (uint32_t)sizeof(vec_indexes);
    write_hdr(PERSIST_VEC_INDEX_HDR_LBA, PERSIST_MAGIC_VEC_INDEX, bytes, 0, 0);
    persist_write_array(vec_indexes, bytes, PERSIST_VEC_INDEX_ENT_LBA);
    kernel_serial_print("[PERSIST] Vec index definitions snapshot written.\n");
}

// persist_row_journal — direct, full-array-rewrite-per-mutation persistence
// (matching persist_records()'s own established trade-off for a small-
// enough struct array), NOT rebuild-on-boot: an audit trail is inherently
// historical, no current-state scan can regenerate it. Called after every
// row_journal_notify_insert/update/delete() and row_journal_commit_tx()/
// _rollback_tx().
void persist_row_journal(void) {
    if (!io_sq || !io_cq) return;
    uint32_t buf_bytes    = (uint32_t)sizeof(row_journal_buffer);
    uint32_t attach_bytes = (uint32_t)sizeof(row_journal_attachments);
    write_hdr(PERSIST_ROW_JOURNAL_HDR_LBA, PERSIST_MAGIC_ROW_JOURNAL,
              buf_bytes, row_journal_entry_count, attach_bytes);
    persist_write_array(row_journal_buffer, buf_bytes, PERSIST_ROW_JOURNAL_ENT_LBA);
    persist_write_array(row_journal_attachments, attach_bytes, PERSIST_ROW_JOURNAL_ATTACH_LBA);
    kernel_serial_print("[PERSIST] Row journal snapshot written.\n");
}

// ─── persist_restore_all ─────────────────────────────────────────────────────
// Called once at boot (kernel.c step 7b), before stream_init().
// Each region is independently checked: a missing or mismatched magic causes
// a cold start for that subsystem while others may still restore successfully.
// Struct-size validation catches format changes between kernel builds.
void persist_restore_all(void) {
    if (!io_sq || !io_cq) return;

    // ── 1. Catalog + role table ───────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_CAT_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_CAT) {
            uint32_t cat_count, cat_bytes, role_bytes;
            p_memcpy(&cat_count,  p_buf +  8, 4);
            p_memcpy(&cat_bytes,  p_buf + 12, 4);
            p_memcpy(&role_bytes, p_buf + 16, 4);
            if (cat_bytes  == (uint32_t)sizeof(object_catalog) &&
                role_bytes == (uint32_t)sizeof(role_table)) {
                persist_read_array(object_catalog, cat_bytes,  PERSIST_CAT_ENT_LBA);
                persist_read_array(role_table,     role_bytes, PERSIST_ROLE_ENT_LBA);
                object_catalog_count = cat_count;
                catalog_after_restore();
                kernel_serial_printf("[PERSIST] Catalog restored: %u entries.\n",
                                     cat_count);
            } else {
                kernel_serial_print("[PERSIST] Catalog: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Catalog: no snapshot — cold start.\n");
        }
    }

    // ── 2. Records ───────────────────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_REC_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_REC) {
            uint32_t rec_bytes;
            p_memcpy(&rec_bytes, p_buf + 8, 4);
            if (rec_bytes == (uint32_t)sizeof(object_records)) {
                persist_read_array(object_records, rec_bytes, PERSIST_REC_ENT_LBA);
                kernel_serial_print("[PERSIST] Records restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Records: struct size mismatch — cold start.\n");
            }
        }
    }

    // ── 3. Schemas ───────────────────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_SCH_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_SCH) {
            uint32_t sch_bytes;
            p_memcpy(&sch_bytes, p_buf + 8, 4);
            if (sch_bytes == (uint32_t)sizeof(object_schemas)) {
                persist_read_array(object_schemas, sch_bytes, PERSIST_SCH_ENT_LBA);
                kernel_serial_print("[PERSIST] Schemas restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Schemas: struct size mismatch — cold start.\n");
            }
        }
    }

    // ── 4. Program binaries ──────────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_PROG_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_PROG) {
            uint32_t prog_bytes;
            p_memcpy(&prog_bytes, p_buf + 8, 4);
            if (prog_bytes == (uint32_t)sizeof(service_binaries)) {
                persist_read_array(service_binaries, prog_bytes, PERSIST_PROG_DAT_LBA);
                int cnt = 0;
                for (int i = 0; i < MAX_SERVICE_BINARIES; i++)
                    if (service_binaries[i].active) cnt++;
                kernel_serial_printf("[PERSIST] Programs restored: %d binaries.\n", cnt);
            } else {
                kernel_serial_print("[PERSIST] Programs: struct size mismatch — cold start.\n");
            }
        }
    }

    // ── 5. Partitions (Phase 10) ─────────────────────────────────────────────
    // Runs after partition_init() (kernel.c step 4c-bis) has already set up
    // the default single-partition state — a valid snapshot here simply
    // overwrites those defaults, same relationship persist_catalog()'s
    // restore has with object_catalog[]'s BSS-zeroed starting state.
    if (nvme_read_sync(PERSIST_PART_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_PART) {
            uint32_t part_bytes, assign_bytes;
            p_memcpy(&part_bytes,   p_buf +  8, 4);
            p_memcpy(&assign_bytes, p_buf + 12, 4);
            if (part_bytes   == (uint32_t)sizeof(partition_table) &&
                assign_bytes == (uint32_t)sizeof(partition_assign_table)) {
                persist_read_array(partition_table,        part_bytes,   PERSIST_PART_ENT_LBA);
                persist_read_array(partition_assign_table, assign_bytes, PERSIST_PART_ASSIGN_LBA);
                kernel_serial_print("[PERSIST] Partitions restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Partitions: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Partitions: no snapshot — cold start.\n");
        }
    }

    // ── 6. Row-store table headers (Phase 16) ────────────────────────────────
    // Runs after rowstore_init() (kernel.c, alongside mqt_init()/agent_init())
    // has already zeroed table_headers[] and reset the page-pool cursor — a
    // valid snapshot here overwrites those cold-start defaults, same
    // relationship every other restore block has with its subsystem's
    // BSS-zeroed/explicitly-reset starting state. Row PAGE data itself is
    // NOT restored here — pages restore lazily, one at a time, on first
    // access via rowstore_load_page(), exactly like stream.c's frames[].
    if (nvme_read_sync(PERSIST_ROWSTORE_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROWSTORE) {
            uint32_t hdr_bytes, next_page;
            p_memcpy(&hdr_bytes, p_buf +  8, 4);
            p_memcpy(&next_page, p_buf + 12, 4);
            if (hdr_bytes == (uint32_t)sizeof(table_headers)) {
                persist_read_array(table_headers, hdr_bytes, PERSIST_ROWSTORE_ENT_LBA);
                rowstore_next_free_page_id = next_page;
                kernel_serial_print("[PERSIST] Row-store table headers restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Row-store: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row-store: no snapshot — cold start.\n");
        }
    }

    // ── 6b. MVCC bootstrap (Gap Remediation Phase D) ─────────────────────────
    // Not a restore block itself (mvcc.c persists nothing -- see mvcc.h's own
    // "persistence out of scope" stance, still true for the mechanism, just
    // no longer true for the CONSEQUENCE of skipping it -- see mvcc_
    // bootstrap_from_rowstore()'s own header comment for the full story).
    // Registers one synthetic, always-visible MvccVersion per physical row
    // block 6 just restored, unconditionally (not gated by whether a
    // catalog/rowstore snapshot actually existed -- calling this against an
    // empty table_headers[]/object_catalog[] on a genuine cold start is a
    // correct no-op, not a special case to guard against). Must run after
    // block 6 (needs restored row data) and before any real transaction
    // begins -- every block below this point that touches MVCC-visible data
    // (row constraints' runtime checks, if exercised) depends on it.
    mvcc_bootstrap_from_rowstore();

    // ── 7. Row constraints (Gap Remediation Phase D) ────────────────────────
    // Pure definitions, no derived runtime state (see row_constraint.h's own
    // header comment: enforcement always consults mvcc_table_scan() live,
    // nothing is cached) -- direct restore, same shape as every simple
    // struct-array block above.
    if (nvme_read_sync(PERSIST_ROW_CONSTRAINT_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROW_CONSTRAINT) {
            uint32_t count, bytes;
            p_memcpy(&count, p_buf +  8, 4);
            p_memcpy(&bytes, p_buf + 12, 4);
            if (bytes == (uint32_t)sizeof(row_constraints)) {
                persist_read_array(row_constraints, bytes, PERSIST_ROW_CONSTRAINT_ENT_LBA);
                row_constraint_count = count;
                kernel_serial_printf("[PERSIST] Row constraints restored: %u defined.\n", count);
            } else {
                kernel_serial_print("[PERSIST] Row constraints: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row constraints: no snapshot — cold start.\n");
        }
    }

    // ── 8. Row-set B-tree indexes (Gap Remediation Phase D): rebuild-on-
    // boot ────────────────────────────────────────────────────────────────
    // Restored definitions are staged in a local buffer, then fed one at a
    // time through the real row_index_create() -- which re-derives the
    // actual B-tree by scanning already-restored row data (blocks 1 and 6
    // above, both of which run earlier in this function) -- rather than
    // being loaded directly into the live row_indexes[]/node pool (whose
    // node-pool indices were never persisted at all and would be meaningless
    // after a fresh boot's node pool starts empty again). caller_uid=0 here
    // uses the existing, already-established "kernel role always passes
    // catalog_check_access()" convention (object_catalog.c's catalog_get_
    // role()) -- not a new convention invented for this restore path.
    if (nvme_read_sync(PERSIST_ROW_INDEX_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROW_INDEX) {
            uint32_t bytes;
            p_memcpy(&bytes, p_buf + 8, 4);
            if (bytes == (uint32_t)sizeof(row_indexes)) {
                struct RowIndex staged[ROW_INDEX_MAX];
                persist_read_array(staged, bytes, PERSIST_ROW_INDEX_ENT_LBA);
                uint32_t defined = 0, rebuilt = 0;
                for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
                    if (!staged[i].active) continue;
                    defined++;
                    int tidx = -1;
                    for (uint32_t c = 0; c < object_catalog_count; c++) {
                        if (object_catalog[c].active &&
                            object_catalog[c].object_id == staged[i].table_object_id) { tidx = (int)c; break; }
                    }
                    if (tidx < 0) continue;   // parent table no longer exists -- can't rebuild, skip
                    if (staged[i].column_index >= table_headers[tidx].layout.column_count) continue;
                    const char* col_name = table_headers[tidx].layout.column_names[staged[i].column_index];
                    if (row_index_create(0, staged[i].index_name, object_catalog[tidx].name, col_name) == 0)
                        rebuilt++;
                }
                kernel_serial_printf("[PERSIST] Row indexes rebuilt from NVMe-persisted definitions: %u of %u.\n",
                                     rebuilt, defined);
            } else {
                kernel_serial_print("[PERSIST] Row indexes: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row indexes: no snapshot — cold start.\n");
        }
    }

    // ── 9. Vecstore collection headers (Gap Remediation Phase D) ────────────
    // Mirrors block 6's rowstore restore exactly. Bulk page data is NOT
    // restored here -- it restores lazily, one page at a time, on first
    // access via vecstore_load_page(), exactly like rowstore's own row pages.
    if (nvme_read_sync(PERSIST_VECSTORE_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_VECSTORE) {
            uint32_t hdr_bytes, next_page;
            p_memcpy(&hdr_bytes, p_buf +  8, 4);
            p_memcpy(&next_page, p_buf + 12, 4);
            if (hdr_bytes == (uint32_t)sizeof(vector_collections)) {
                persist_read_array(vector_collections, hdr_bytes, PERSIST_VECSTORE_ENT_LBA);
                vecstore_next_free_page_id = next_page;
                kernel_serial_print("[PERSIST] Vecstore collection headers restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Vecstore: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Vecstore: no snapshot — cold start.\n");
        }
    }

    // ── 10. HNSW vector indexes (Gap Remediation Phase D): rebuild-on-boot,
    // same shape as block 8 ──────────────────────────────────────────────────
    // Restored definitions feed vec_index_create(), then a one-time backfill
    // scan over the now-restored collection (vecstore_collection_scan(),
    // which lazily loads whatever pages it visits via block 9's restored
    // vecstore_next_free_page_id) rebuilds the actual graph by feeding every
    // entry through vec_index_notify_insert() -- the exact backfill helper
    // vec_index.h's own header comment (point 7) named as "straightforward
    // to add later ... if a real caller needs it." This restore path is that
    // real caller; see persist_vec_backfill_cb() above.
    if (nvme_read_sync(PERSIST_VEC_INDEX_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_VEC_INDEX) {
            uint32_t bytes;
            p_memcpy(&bytes, p_buf + 8, 4);
            if (bytes == (uint32_t)sizeof(vec_indexes)) {
                struct VecIndex staged[VEC_INDEX_MAX];
                persist_read_array(staged, bytes, PERSIST_VEC_INDEX_ENT_LBA);
                uint32_t defined = 0, rebuilt = 0;
                for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
                    if (!staged[i].active) continue;
                    defined++;
                    if (vec_index_create(0, staged[i].index_name, staged[i].collection_name, staged[i].metric) != 0)
                        continue;
                    struct persist_vec_backfill_ctx ctx = { staged[i].collection_name };
                    vecstore_collection_scan(0, staged[i].collection_name, persist_vec_backfill_cb, &ctx);
                    rebuilt++;
                }
                kernel_serial_printf("[PERSIST] HNSW vector indexes rebuilt from NVMe-persisted definitions: %u of %u.\n",
                                     rebuilt, defined);
            } else {
                kernel_serial_print("[PERSIST] Vec indexes: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Vec indexes: no snapshot — cold start.\n");
        }
    }

    // ── 11. Row journal (Gap Remediation Phase D) ───────────────────────────
    // Direct restore -- an audit trail is inherently historical, not a
    // derived structure; see persist_row_journal()'s own header comment.
    if (nvme_read_sync(PERSIST_ROW_JOURNAL_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROW_JOURNAL) {
            uint32_t buf_bytes, entry_count, attach_bytes;
            p_memcpy(&buf_bytes,    p_buf +  8, 4);
            p_memcpy(&entry_count,  p_buf + 12, 4);
            p_memcpy(&attach_bytes, p_buf + 16, 4);
            if (buf_bytes    == (uint32_t)sizeof(row_journal_buffer) &&
                attach_bytes == (uint32_t)sizeof(row_journal_attachments)) {
                persist_read_array(row_journal_buffer, buf_bytes, PERSIST_ROW_JOURNAL_ENT_LBA);
                persist_read_array(row_journal_attachments, attach_bytes, PERSIST_ROW_JOURNAL_ATTACH_LBA);
                row_journal_entry_count = entry_count;
                uint32_t acount = 0;
                for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++)
                    if (row_journal_attachments[i].active) acount++;
                row_journal_attachment_count = acount;
                kernel_serial_printf("[PERSIST] Row journal restored: %u entries (seq), %u attachment(s).\n",
                                     entry_count, acount);
            } else {
                kernel_serial_print("[PERSIST] Row journal: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row journal: no snapshot — cold start.\n");
        }
    }
}
