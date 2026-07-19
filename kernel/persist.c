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
}
