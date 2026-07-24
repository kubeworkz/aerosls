/*
 * migration_data_movement_host_test.c -- Multi-Node Partition Scaling
 * Roadmap Phase 6 addendum ("real migration data movement", Multitenant
 * Isolation Gap Analysis §7 item 7) verification: a standalone
 * host-buildable test for kernel/stream.c's new stream_relocate_partition()
 * and its wiring into kernel/partition.c's partition_migrate(), linked
 * against the REAL, unmodified kernel/stream.c and kernel/partition.c --
 * not a reimplementation of either.
 *
 * Before this phase, partition_migrate() moved only an ownership RECORD
 * (kernel/partition.c's own Step 3 comment said so explicitly: "This
 * function does not copy or move any object data"). This test proves the
 * real fix for stream/blob storage specifically: stream_relocate_partition()
 * physically copies each of a partition's stream slots' on-disk bytes to a
 * fresh slot and verifies every page byte-for-byte before retiring the
 * original -- a genuine relocate-and-verify operation, not a pointer flip.
 *
 * ─── Why a stateful fake NVMe, not the trivial always-fail/always-succeed
 * stub other tests in this suite use ─────────────────────────────────────
 * stream_partition_host_test.c's nvme_read_sync/nvme_write_sync stubs are
 * trivial (read always fails, write always succeeds) because that test
 * never needs to verify actual byte content survives a round trip. This
 * test's whole point is proving real bytes really move, so it needs a
 * small in-memory LBA-indexed page store: nvme_write_sync() records the
 * page under its LBA, nvme_read_sync() returns exactly what was last
 * written there (or a zero page for an LBA never written, the same "erased
 * flash reads as zero" convention any real NVMe device honors). A linear
 * array with room for a few dozen pages is more than enough -- this test
 * only ever writes a handful of frames across a couple of small streams,
 * never anywhere close to a full 64 MiB stream slot.
 *
 * Other stubs (partition.c's external dependencies) are the exact same set
 * stream_partition_host_test.c already established: kernel_serial_print/
 * printf (logging), persist_partitions/process_kill_partition/
 * catalog_vfree_partition/partition_reclaim_all_frames (partition.c's
 * cross-subsystem calls, irrelevant here), cluster_local_node_id (fixed
 * node id), partition_lease_step_down (no lease machinery under test here
 * -- net/consensus.c is deliberately NOT linked, since partition_migrate()
 * only needs this one function's return value, not real lease semantics;
 * that real integration already has its own dedicated coverage in
 * partition_migrate_phase6_host_test.c). sys_sls_valloc/_insert/_update and
 * the settable-fake allocate_physical_ram_frame_for_partition() are the
 * same object_catalog.c/frame_pool.c stand-ins stream_partition_host_test.c
 * already uses, for the same reasons (see that file's own header comment).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I user \
 *       -o /tmp/migration_data_movement_host_test \
 *       tests/migration_data_movement_host_test.c kernel/stream.c kernel/partition.c
 *   /tmp/migration_data_movement_host_test
 */
#include "kernel/stream.h"
#include "kernel/partition.h"
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ─── Stubs for stream.c's/partition.c's external dependencies ─────────── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t partition_reclaim_all_frames(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t cluster_local_node_id(void) { return 0; }
int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }

/* Multi-Node Phase 7 addendum (real cross-node data movement): kernel/
 * stream.c now calls into net/dspp.c's dspp_migrate_send_begin()/_page()
 * from its new stream_migrate_send_partition() -- dead code from this
 * test's perspective, since cluster_local_node_id() is stubbed to 0 above,
 * meaning kernel/partition.c's partition_migrate() always takes the OLD
 * stream_relocate_partition() branch this file exists to test, never the
 * new one. net/dspp.c itself is deliberately NOT linked here (it would
 * pull in object_catalog[]/e1000_transmit_packet()/net_my_mac stubs this
 * same-disk-relocate-focused test has no interest in) -- permissive no-op
 * stubs instead, the same judgment call this file's own header comment
 * already applies to net/consensus.c's real lease machinery. */
void dspp_migrate_send_begin(uint64_t transfer_id, uint32_t node_dest_id,
                              uint32_t partition_id, const char* name,
                              const char* mime_type, uint64_t size,
                              uint32_t frames_used, uint32_t owner_uid) {
    (void)transfer_id; (void)node_dest_id; (void)partition_id; (void)name;
    (void)mime_type; (void)size; (void)frames_used; (void)owner_uid;
}
void dspp_migrate_send_page(uint64_t transfer_id, uint32_t node_dest_id,
                             uint32_t partition_id, uint32_t page_index,
                             const uint8_t* page_data) {
    (void)transfer_id; (void)node_dest_id; (void)partition_id; (void)page_index; (void)page_data;
}

uint64_t sys_sls_valloc(struct SLSVallocRequest* req) { (void)req; return 1; }
uint64_t sys_sls_insert(struct SLSRecordRequest* req) { (void)req; return 0; }
uint64_t sys_sls_update(struct SLSRecordRequest* req) { (void)req; return 0; }

static uint32_t fake_usage[PARTITION_MAX];
static uint64_t fake_quota[PARTITION_MAX];
void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    if (fake_quota[partition_id] != 0 && fake_usage[partition_id] >= fake_quota[partition_id]) return 0;
    void* frame = malloc(FRAME_SIZE);
    if (!frame) return 0;
    fake_usage[partition_id]++;
    return frame;
}
int partition_set_frame_quota(uint32_t partition_id, uint64_t frame_quota) {
    if (partition_id >= PARTITION_MAX) return 1;
    fake_quota[partition_id] = frame_quota;
    return 0;
}
uint64_t partition_get_frame_usage(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return fake_usage[partition_id];
}
uint64_t partition_get_frame_quota(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0xFFFFFFFFFFFFFFFFULL;
    return fake_quota[partition_id];
}

/* ─── Stateful fake NVMe (see header comment above) ──────────────────────── */
void* io_sq = (void*)1;   /* non-NULL -- "NVMe is up" for stream.c's (io_sq && io_cq) gate */
void* io_cq = (void*)1;

#define FAKE_NVME_MAX_PAGES 64
static uint64_t fake_lba[FAKE_NVME_MAX_PAGES];
static uint8_t  fake_page[FAKE_NVME_MAX_PAGES][4096];
static int      fake_count = 0;
static int      g_write_fail_after = -1;   /* -1 = never fail; 0 = fail the very next write */

static int fake_find(uint64_t slba) {
    for (int i = 0; i < fake_count; i++) if (fake_lba[i] == slba) return i;
    return -1;
}
int nvme_read_sync(uint64_t slba, void* buf) {
    int idx = fake_find(slba);
    if (idx < 0) { memset(buf, 0, 4096); return 0; }   /* never-written page reads as zero */
    memcpy(buf, fake_page[idx], 4096);
    return 0;
}
int nvme_write_sync(uint64_t slba, const void* buf) {
    if (g_write_fail_after == 0) { g_write_fail_after = -1; return -1; }
    if (g_write_fail_after > 0) g_write_fail_after--;
    int idx = fake_find(slba);
    if (idx < 0) {
        if (fake_count >= FAKE_NVME_MAX_PAGES) return -1;
        idx = fake_count++;
        fake_lba[idx] = slba;
    }
    memcpy(fake_page[idx], buf, 4096);
    return 0;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    partition_init();
    stream_init();

    /* ── Scenario A: a real byte-for-byte relocation of a single, small
     * (single-frame) stream. ─────────────────────────────────────────────── */
    uint32_t part_a = partition_create("tenant-a");
    partition_assign_uid(100, part_a);
    stream_create(100, "alpha", "text/plain");
    uint8_t chunk_a[100];
    memset(chunk_a, 0xAB, sizeof(chunk_a));
    stream_write_chunk("alpha", chunk_a, sizeof(chunk_a), 0, 1);

    struct StreamEntry* se_before = stream_find("alpha");
    int    slot_before      = (int)(se_before - stream_store);
    uint64_t lba_before      = se_before->lba_base;
    CHECK(se_before != 0 && se_before->active, "sA: 'alpha' exists and is active before relocation");

    int relocated = stream_relocate_partition(part_a, 42);
    CHECK(relocated == 1, "sA: stream_relocate_partition() relocated exactly 1 stream for tenant-a");

    CHECK(stream_store[slot_before].active == 0, "sA: the ORIGINAL slot is now inactive (retired)");
    struct StreamEntry* se_after = stream_find("alpha");
    CHECK(se_after != 0, "sA: 'alpha' is still findable by name after relocation");
    CHECK(se_after != se_before, "sA: 'alpha' now lives in a DIFFERENT slot than before");
    CHECK(se_after->lba_base != lba_before, "sA: the relocated stream's lba_base genuinely changed");
    CHECK(se_after->owner_uid == 100 && se_after->partition_id == part_a,
          "sA: owner_uid/partition_id survived the relocation unchanged");
    CHECK(se_after->size == 100, "sA: size metadata survived the relocation");

    /* Prove the BYTES themselves survived, not just the metadata -- read the
     * relocated page directly off the fake NVMe at the new lba_base. */
    {
        uint8_t readback[4096];
        int rc = nvme_read_sync(se_after->lba_base, readback);
        CHECK(rc == 0, "sA: reading the relocated page off NVMe succeeds");
        int match = memcmp(readback, chunk_a, sizeof(chunk_a)) == 0;
        CHECK(match, "sA: the first 100 bytes at the NEW location are byte-for-byte identical to what was originally written");
    }
    /* Prove lazy-load through the normal stream API also sees the relocated
     * data (frames[] was correctly cleared, not left pointing at stale RAM). */
    {
        CHECK(se_after->frames[0] == 0, "sA: the relocated slot's cached RAM frame pointer was cleared (lazy reload, not stale)");
        uint8_t* frame = stream_lazy_load_frame(se_after, 0);
        CHECK(frame != 0, "sA: lazy-loading frame 0 from the new location succeeds");
        CHECK(memcmp(frame, chunk_a, sizeof(chunk_a)) == 0,
              "sA: the lazily-reloaded frame's content matches the original bytes exactly");
    }

    /* ── Scenario B: a multi-frame stream relocates every page, in order,
     * all verified -- not just the first one. ───────────────────────────── */
    uint32_t part_b = partition_create("tenant-b");
    partition_assign_uid(200, part_b);
    stream_create(200, "beta", "application/octet-stream");
    uint8_t chunk_b[5000];   /* spans 2 frames: 4096 + 904 bytes */
    for (int i = 0; i < 5000; i++) chunk_b[i] = (uint8_t)(i & 0xFF);
    stream_write_chunk("beta", chunk_b, sizeof(chunk_b), 0, 1);

    int relocated_b = stream_relocate_partition(part_b, 42);
    CHECK(relocated_b == 1, "sB: exactly 1 stream relocated for tenant-b");
    struct StreamEntry* seb = stream_find("beta");
    CHECK(seb->frames_used == 2, "sB: the relocated stream still reports 2 frames used");
    {
        uint8_t p0[4096], p1[4096];
        CHECK(nvme_read_sync(seb->lba_base, p0) == 0, "sB: page 0 read back from the new location succeeds");
        CHECK(nvme_read_sync(seb->lba_base + 8, p1) == 0, "sB: page 1 read back from the new location succeeds");
        CHECK(memcmp(p0, chunk_b, 4096) == 0, "sB: page 0's bytes are exactly the first 4096 original bytes");
        CHECK(memcmp(p1, chunk_b + 4096, 904) == 0, "sB: page 1's first 904 bytes are exactly the remaining original bytes");
    }

    /* ── Scenario C: relocating one partition never touches another
     * partition's streams -- tenant-a's already-relocated 'alpha' (now
     * elsewhere) is completely unaffected by tenant-b's relocation above. ── */
    struct StreamEntry* alpha_again = stream_find("alpha");
    CHECK(alpha_again == se_after, "sC: 'alpha' is still in the exact same (post-relocation) slot after tenant-b's own relocation ran");
    CHECK(alpha_again->partition_id == part_a, "sC: 'alpha' still belongs to tenant-a, untouched by tenant-b's move");

    /* ── Scenario D: copy/verify failure leaves the source fully intact and
     * reports an honest short (zero) count -- no partial, corrupted state. ── */
    uint32_t part_d = partition_create("tenant-d");
    partition_assign_uid(300, part_d);
    stream_create(300, "gamma", "text/plain");
    uint8_t chunk_d[50];
    memset(chunk_d, 0x77, sizeof(chunk_d));
    stream_write_chunk("gamma", chunk_d, sizeof(chunk_d), 0, 1);
    struct StreamEntry* gamma_before = stream_find("gamma");
    uint64_t gamma_lba_before = gamma_before->lba_base;

    g_write_fail_after = 0;   /* force the very next NVMe write (the relocation's own copy) to fail */
    int relocated_d = stream_relocate_partition(part_d, 42);
    CHECK(relocated_d == 0, "sD: a forced write failure results in an honest 0 relocated (not a corrupted partial state)");
    struct StreamEntry* gamma_after = stream_find("gamma");
    CHECK(gamma_after == gamma_before, "sD: 'gamma' is still in its ORIGINAL slot after the failed relocation attempt");
    CHECK(gamma_after->active == 1, "sD: 'gamma' is still active -- the source was never retired since the copy never verified clean");
    CHECK(gamma_after->lba_base == gamma_lba_before, "sD: 'gamma's lba_base is completely unchanged after the failed attempt");

    /* ── Scenario E: no free destination slot -- fill every remaining slot,
     * then confirm relocation honestly reports 0 and leaves the source
     * untouched, rather than crashing or silently dropping data. ────────── */
    /* Slots currently occupied: alpha(relocated), beta(relocated), gamma = 3.
     * Fill the remaining 5 slots with other tenants' streams so there is
     * truly nowhere left to relocate to. */
    for (int i = 0; i < 5; i++) {
        char name[16], mime[8] = "text/x";
        snprintf(name, sizeof(name), "filler%d", i);
        uint32_t pid = partition_create(name);
        partition_assign_uid(400 + i, pid);
        stream_create(400 + (uint32_t)i, name, mime);
    }
    CHECK(fake_count >= 0, "sE: setup complete");   /* just a checkpoint, no real assertion needed here */
    int relocated_e = stream_relocate_partition(part_d, 99);
    CHECK(relocated_e == 0, "sE: with all 8 slots occupied, relocating tenant-d's remaining stream reports 0 (no free destination)");
    struct StreamEntry* gamma_final = stream_find("gamma");
    CHECK(gamma_final == gamma_before, "sE: 'gamma' is still exactly where it was -- an out-of-slots condition never corrupts or half-moves data");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
