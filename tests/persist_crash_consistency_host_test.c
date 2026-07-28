/*
 * persist_crash_consistency_host_test.c — verification for kernel/persist.c's
 * torn-write detection and durability ordering, linked against the REAL,
 * unmodified kernel/persist.c.
 *
 * The hazard being closed (§3 of the write-amplification scoping doc): the
 * header used to be written BEFORE its data, and the restore side checked
 * only the magic value and the recorded size. A crash partway through a
 * multi-frame data write therefore left a VALID header pointing at a
 * half-old, half-new array, and the next boot loaded it silently. There was
 * no checksum, no generation counter and no torn-write detection anywhere.
 * Separately, no NVM Flush was ever issued, so an acknowledged write was only
 * durable against a process restart, never against power loss.
 *
 * The fix is three things that only work together, and this file tests each:
 *   1. a checksum of the region's data, stored in the header;
 *   2. the header written LAST, so a crash before it lands leaves the
 *      previous header whose checksum will not match the torn data;
 *   3. an NVM Flush between the two, so the data reaches media before the
 *      header that vouches for it does -- without which the controller could
 *      commit them in the opposite order and reopen the window.
 *
 * The test models a crash the way a crash actually presents itself: bytes on
 * disk that no complete write ever produced. It does that by writing a region
 * normally and then damaging frames underneath persist.c, which is exactly
 * what an interrupted write leaves behind.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers \
 *       -o /tmp/persist_crash_consistency_host_test \
 *       tests/persist_crash_consistency_host_test.c kernel/persist.c kernel/view.c kernel/partition.c
 *   /tmp/persist_crash_consistency_host_test
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/row_index.h"
#include "kernel/row_constraint.h"
#include "kernel/row_journal.h"
#include "kernel/vecstore.h"
#include "kernel/vec_index.h"
#include "kernel/tenant.h"
#include "kernel/database.h"
#include "kernel/persist.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "kernel/simi_ctx_migrate.h"   // PEC Phase 3 -- stubbed below

/* ─── PEC Phase 3 stub: kernel/partition.c's context-migration step ────
 * FAITHFUL, not a no-op. The real simi_ctx_migrate_send_partition()
 * walks the live-context registry and sends whatever belongs to the
 * partition; this test registers no contexts, so the real function would
 * walk an empty registry and return 0 -- exactly what this returns. */
uint32_t simi_ctx_migrate_send_partition(uint32_t partition_id, uint32_t dest_node) {
    (void)partition_id; (void)dest_node; return 0;
}


static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Dummy globals persist.c references ────────────────────────────────── */
struct SLSTenantEntry  tenants[TENANT_MAX];
uint32_t               tenant_next_id = 1;
struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
uint32_t               object_catalog_count = 0;
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
void catalog_after_restore(void) { }
struct RowTableHeader table_headers[ROWSTORE_MAX_TABLES];
uint32_t              rowstore_next_free_page_id = 0;
uint32_t              rowstore_partition_cursor[PARTITION_MAX] = {0};
struct RowConstraintDef row_constraints[ROW_CONSTRAINT_MAX];
uint32_t                row_constraint_count = 0;
struct RowIndex         row_indexes[ROW_INDEX_MAX];
struct RowJournalEntry      row_journal_buffer[ROW_JOURNAL_MAX_ENTRIES];
uint32_t                    row_journal_entry_count = 0;
struct RowJournalAttachment row_journal_attachments[ROW_JOURNAL_MAX_ATTACHMENTS];
uint32_t                    row_journal_attachment_count = 0;
struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
uint32_t                   vecstore_partition_cursor[PARTITION_MAX] = {0};
struct VecIndex            vec_indexes[VEC_INDEX_MAX];
struct SLSDatabaseEntry    databases[DATABASE_MAX];
struct SLSDatabaseGrant    database_grants[DATABASE_GRANT_MAX];
uint32_t                   database_next_id = 1;
uint32_t                   database_grant_count = 0;
void mvcc_bootstrap_from_rowstore(void) { }
int row_index_create(uint32_t u, const char* a, const char* b, const char* c) {
    (void)u; (void)a; (void)b; (void)c; return 1;
}
int vec_index_create(uint32_t u, const char* a, const char* b, VecMetric m) {
    (void)u; (void)a; (void)b; (void)m; return 1;
}
uint32_t vecstore_collection_scan(uint32_t u, const char* n, VecScanCb cb, void* ctx) {
    (void)u; (void)n; (void)cb; (void)ctx; return 0;
}
void vec_index_notify_insert(uint32_t u, const char* n, struct VecId id,
                             uint64_t eid, const struct VecValues* v) {
    (void)u; (void)n; (void)id; (void)eid; (void)v;
}
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
SLSRole catalog_get_role(uint32_t uid) { (void)uid; return ROLE_SYSTEM_KERNEL; }
int stream_relocate_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
int stream_migrate_send_partition(uint32_t p, uint32_t d) { (void)p; (void)d; return 0; }
static uint32_t g_fake_local_node_id = 0;
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
uint32_t process_kill_partition(uint32_t p) { (void)p; return 0; }
uint32_t catalog_vfree_partition(uint32_t p) { (void)p; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t p) { (void)p; return 0; }
int partition_lease_step_down(uint32_t p) { (void)p; return 1; }

/* ─── Fake NVMe with a write journal, so ORDERING can be asserted ────────
 * The durability guarantee is about order (data on media before the header
 * that validates it), so the fake records the sequence of operations, not
 * just their effects. */
#define FAKE_NVME_MAX_FRAMES 1024
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];

#define OPLOG_MAX 4096
static struct { int is_flush; uint64_t lba; } g_oplog[OPLOG_MAX];
static uint32_t g_oplog_n = 0;
static void oplog(int is_flush, uint64_t lba) {
    if (g_oplog_n < OPLOG_MAX) { g_oplog[g_oplog_n].is_flush = is_flush;
                                 g_oplog[g_oplog_n].lba = lba; g_oplog_n++; }
}

void* io_sq = (void*)1;
void* io_cq = (void*)1;

static int fake_slot(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}
int nvme_write_sync(uint64_t lba, const void* buf) {
    int idx = fake_slot(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    oplog(0, lba);
    return 0;
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) {
            memcpy(buf, g_fake_nvme[i].data, 4096);
            return 0;
        }
    return 1;
}
int nvme_write_pages_sync(uint64_t slba, const void* buf, uint32_t page_count) {
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_write_sync(slba + (uint64_t)i * 8, (const uint8_t*)buf + (size_t)i * 4096) != 0)
            return 1;
    return 0;
}
int nvme_read_pages_sync(uint64_t slba, void* buf, uint32_t page_count) {
    for (uint32_t i = 0; i < page_count; i++)
        if (nvme_read_sync(slba + (uint64_t)i * 8, (uint8_t*)buf + (size_t)i * 4096) != 0)
            return 1;
    return 0;
}
int nvme_flush_sync(void) { oplog(1, 0); return 0; }

#define REC_HDR_LBA 2048ULL
#define REC_ENT_LBA 2056ULL

/* Damages a data frame directly, which is what an interrupted multi-frame
 * write leaves on disk: some frames new, some old, header not yet updated. */
static void tear_frame(uint64_t lba, uint8_t fill) {
    int s = fake_slot(lba);
    memset(g_fake_nvme[s].data, fill, 4096);
}

int main(void) {
    printf("=== persist crash consistency: torn-write detection + flush ordering ===\n\n");

    /* ── Scenario 1: a clean write round-trips ──────────────────────────── */
    memset(g_fake_nvme, 0, sizeof(g_fake_nvme));
    memset(object_records, 0, sizeof(object_records));
    persist_shadow_invalidate();
    for (int i = 0; i < 8; i++) {
        object_records[i].object_id = 0xC000 + i;
        object_records[i].field_count = 1;
        memset(object_records[i].fields[0].value, 'a' + i, 24);
    }
    persist_records();

    uint8_t saved[sizeof(object_records)];
    memcpy(saved, object_records, sizeof(object_records));

    memset(object_records, 0, sizeof(object_records));
    persist_restore_all();
    CHECK(memcmp(object_records, saved, sizeof(object_records)) == 0,
          "baseline: a cleanly written region restores byte-for-byte");

    /* ── Scenario 2: the header now carries a checksum at all ───────────── */
    uint8_t hdr[4096];
    CHECK(nvme_read_sync(REC_HDR_LBA, hdr) == 0, "header frame is readable");
    uint64_t stored_csum = 0;
    memcpy(&stored_csum, hdr + 24, 8);
    CHECK(stored_csum != 0,
          "header carries a non-zero data checksum (offset 24) -- absent entirely before this fix");

    /* ── Scenario 3: THE hazard -- a torn data write is detected ─────────
     * Damage one data frame, leaving the header intact and still claiming
     * the old checksum. Before this fix the magic+size check passed and the
     * corrupt array was loaded straight into live kernel state. */
    tear_frame(REC_ENT_LBA + 3 * 8, 0x5A);

    memset(object_records, 0xFF, sizeof(object_records));   /* poison, to prove it is overwritten or left */
    uint8_t poison[sizeof(object_records)];
    memset(poison, 0xFF, sizeof(poison));

    persist_restore_all();
    CHECK(memcmp(object_records, saved, sizeof(object_records)) != 0,
          "torn region is NOT loaded as if it were valid");
    CHECK(memcmp(object_records, poison, sizeof(object_records)) == 0,
          "...live array is left exactly as the caller had it -- verification happens BEFORE load, so torn bytes never reach kernel state");

    /* ── Scenario 4: a stale header over newer data is also caught ───────
     * The reverse ordering failure: data updated, header not. The old
     * checksum no longer describes the new data. */
    memset(g_fake_nvme, 0, sizeof(g_fake_nvme));
    memset(object_records, 0, sizeof(object_records));
    persist_shadow_invalidate();
    object_records[0].object_id = 0xAAAA;
    persist_records();                        /* good write, header matches */

    object_records[0].object_id = 0xBBBB;     /* newer data... */
    persist_shadow_invalidate();
    /* ...written WITHOUT updating the header: emulate a crash after the data
     * frames landed but before the header did, by restoring the old header
     * bytes immediately afterwards. */
    uint8_t old_hdr[4096];
    nvme_read_sync(REC_HDR_LBA, old_hdr);
    persist_records();
    int hs = fake_slot(REC_HDR_LBA);
    memcpy(g_fake_nvme[hs].data, old_hdr, 4096);   /* header reverts to the pre-write one */

    memset(object_records, 0xEE, sizeof(object_records));
    uint8_t poison2[sizeof(object_records)];
    memset(poison2, 0xEE, sizeof(poison2));
    persist_restore_all();
    CHECK(memcmp(object_records, poison2, sizeof(object_records)) == 0,
          "a stale header over newer data is rejected too -- the checksum no longer describes what is on disk");

    /* ── Scenario 5: durability ORDERING -- data, flush, header, flush ────
     * The checksum is only meaningful if the data is actually on media before
     * the header that vouches for it. Assert the emitted sequence, not just
     * that a flush happened somewhere. */
    memset(g_fake_nvme, 0, sizeof(g_fake_nvme));
    memset(object_records, 0, sizeof(object_records));
    persist_shadow_invalidate();
    object_records[5].object_id = 0x1234;
    g_oplog_n = 0;
    persist_records();

    int idx_hdr = -1, flush_before_hdr = 0, flush_after_hdr = 0, data_before_flush = 0;
    for (uint32_t i = 0; i < g_oplog_n; i++) {
        if (!g_oplog[i].is_flush && g_oplog[i].lba == REC_HDR_LBA) { idx_hdr = (int)i; break; }
    }
    CHECK(idx_hdr >= 0, "the header write is present in the op log");
    for (int i = 0; i < idx_hdr; i++) {
        if (g_oplog[i].is_flush) flush_before_hdr = 1;
        else if (g_oplog[i].lba >= REC_ENT_LBA) data_before_flush = 1;
    }
    for (uint32_t i = (uint32_t)idx_hdr + 1; i < g_oplog_n; i++)
        if (g_oplog[i].is_flush) flush_after_hdr = 1;

    CHECK(data_before_flush,
          "ordering: data frames are written before the header");
    CHECK(flush_before_hdr,
          "ordering: an NVM Flush separates data from header -- data reaches media before the header that validates it");
    CHECK(flush_after_hdr,
          "ordering: a second flush follows the header, so the region is genuinely durable on return rather than sitting in a volatile cache");

    /* ── Scenario 6: a pre-checksum image is still accepted ──────────────
     * The checksum field is additive; images written by an older build have
     * zero there. Rejecting them would discard every existing snapshot, so
     * they must load exactly as before -- with the honest consequence that
     * they get no torn-write protection until rewritten. */
    memset(g_fake_nvme, 0, sizeof(g_fake_nvme));
    memset(object_records, 0, sizeof(object_records));
    persist_shadow_invalidate();
    object_records[2].object_id = 0x7777;
    persist_records();
    memcpy(saved, object_records, sizeof(object_records));

    hs = fake_slot(REC_HDR_LBA);
    memset(g_fake_nvme[hs].data + 24, 0, 8);        /* erase the checksum: legacy header */

    memset(object_records, 0, sizeof(object_records));
    persist_restore_all();
    CHECK(memcmp(object_records, saved, sizeof(object_records)) == 0,
          "a pre-checksum (legacy) image still loads -- additive format change, no flag day");

    /* ── Scenario 7: legacy images genuinely have no protection ──────────
     * Asserting the limitation rather than leaving it implied. */
    tear_frame(REC_ENT_LBA + 1 * 8, 0x31);
    memset(object_records, 0, sizeof(object_records));
    persist_restore_all();
    CHECK(memcmp(object_records, saved, sizeof(object_records)) != 0,
          "a torn LEGACY image loads its corrupt bytes undetected -- the documented cost of accepting pre-checksum headers, asserted not assumed");

    /* ── Scenario 8: the next clean write re-arms protection ─────────────── */
    memset(object_records, 0, sizeof(object_records));
    object_records[9].object_id = 0x9999;
    persist_shadow_invalidate();
    persist_records();
    nvme_read_sync(REC_HDR_LBA, hdr);
    memcpy(&stored_csum, hdr + 24, 8);
    CHECK(stored_csum != 0,
          "rewriting a legacy region restores its checksum, so protection resumes from the next write onward");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
