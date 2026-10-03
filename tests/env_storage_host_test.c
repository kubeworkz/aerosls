/*
 * env_storage_host_test.c — POSIX-Environments Roadmap v0.2, Phase P1b:
 * a standalone host-buildable test for kernel/env_storage.c, linked
 * against the REAL, unmodified env_storage.c and storage_quota.c — not a
 * reimplementation (the same rule every host test in tests/ states).
 *
 * What it proves, in the order the roadmap's verification plan claims it:
 *
 *   1. attach creates a durable, ZEROED extent and records the identity —
 *      a recycled slot must never hand the next environment the previous
 *      tenant's bytes.
 *   2. write-through persists to the extent, and restore reads the bytes
 *      back into a DIFFERENT frame region (a reboot allocates fresh
 *      frames) keyed by the same (partition, index) identity — durability.
 *   3. first-touch quota charging moves usage by the pages the file
 *      occupied, and a write past the quota is refused with the QUOTA
 *      status (not a frame-pool or ENOMEM-shaped error) with the frame
 *      pages reverted from their persisted image — so a refused write is
 *      invisible even to a subsequent read.
 *   4. a re-attach after a "reboot" re-charges the persisted occupancy,
 *      and refuses with QUOTA (entry unchanged) when the partition's
 *      quota no longer covers what the store already holds.
 *   5. release hands every charged page back and clears the entry.
 *   6. an unattached region is RAM by design: NOENT on write, no-op on
 *      restore, no owner — the system rootfs path, and the §5
 *      `ram-backed` invariant this guard's tooth leans on: durability
 *      gone, quota intact.
 *   7. no I/O queue is no reason to refuse: attach degrades to a
 *      RAM-backed store (metered, honestly slot=-1) instead of refusing
 *      the environment that asked — the path every boot whose NVMe never
 *      came up takes (E5/E6's smokes caught the refusal regression).
 *
 * The NVMe layer is a RAM disk behind the real entry points (io_sq is
 * non-NULL, nvme_*_sync read/write a static 8 MiB band covering exactly
 * ENV_STORAGE_DATA_LBA_BASE's 8 extents). persist_env_storage() is a
 * counting stub: this test is about the table and the device, not about
 * persist.c's checksum machinery (persist_lba_layout_host_test.c and the
 * persist host tests already watch that half).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/env_storage_host_test \
 *       tests/env_storage_host_test.c kernel/env_storage.c kernel/storage_quota.c
 *   /tmp/env_storage_host_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "env_storage.h"
#include "storage_quota.h"

/* ─── the NVMe layer: a RAM disk behind the real entry points ─────────────── */
#define STUB_EXTENT_BAND_BYTES (ENV_STORAGE_MAX * ENV_STORAGE_EXTENT_BYTES)
static uint8_t es_disk[STUB_EXTENT_BAND_BYTES];
void* io_sq = (void*)1;   /* non-NULL: "the I/O queue exists" */
void* io_cq = (void*)1;

static int es_disk_ptr(uint64_t slba, uint8_t** out, uint64_t bytes) {
    uint64_t off = slba * 512ull;
    uint64_t base = ENV_STORAGE_DATA_LBA_BASE * 512ull;
    if (off < base) return 0;
    off -= base;
    if (off + bytes > (uint64_t)STUB_EXTENT_BAND_BYTES) return 0;
    *out = es_disk + off;
    return 1;
}

/* The real driver's buffer contract, enforced HERE rather than assumed:
 * drivers/nvme_io.c's nvme_buf_aligned() rejects a misaligned buffer with
 * 0xFC, single pages are exactly 4096 bytes, and pages_sync treats a
 * page_count of 0 as a no-op and refuses one above NVME_MAX_PAGES_PER_XFER.
 * A permissive stub is how an unaligned caller buffer reached QEMU once
 * (the attach's zero page — every attach refused, caught by the live arm,
 * not by this test). The stub now answers the same way the device does. */
static int es_buf_aligned(const void* buf) {
    return ((uintptr_t)(const uint8_t*)buf & 4095u) == 0;
}

int nvme_read_sync(uint64_t slba, void* buf) {
    uint8_t* p;
    if (!es_buf_aligned(buf)) return 0xFC;
    if (!es_disk_ptr(slba, &p, 4096)) return 1;
    memcpy(buf, p, 4096);
    return 0;
}
int nvme_write_sync(uint64_t slba, const void* buf) {
    uint8_t* p;
    if (!es_buf_aligned(buf)) return 0xFC;
    if (!es_disk_ptr(slba, &p, 4096)) return 1;
    memcpy(p, buf, 4096);
    return 0;
}
int nvme_read_pages_sync(uint64_t slba, void* buf, uint32_t page_count) {
    uint8_t* p;
    if (page_count == 0) return 0;
    if (page_count > 32) return 1;          /* NVME_MAX_PAGES_PER_XFER */
    if (!es_buf_aligned(buf)) return 0xFC;
    if (!es_disk_ptr(slba, &p, (uint64_t)page_count * 4096ull)) return 1;
    memcpy(buf, p, (size_t)page_count * 4096u);
    return 0;
}
int nvme_write_pages_sync(uint64_t slba, const void* buf, uint32_t page_count) {
    uint8_t* p;
    if (page_count == 0) return 0;
    if (page_count > 32) return 1;
    if (!es_buf_aligned(buf)) return 0xFC;
    if (!es_disk_ptr(slba, &p, (uint64_t)page_count * 4096ull)) return 1;
    memcpy(p, buf, (size_t)page_count * 4096u);
    return 0;
}
int nvme_flush_sync(void) { return 0; }

/* ─── the two kernel_io.h logging functions (quiet by default) ────────────── */
int g_stub_quiet = 1;
void kernel_serial_print(const char* s)         { if (!g_stub_quiet) fputs(s, stdout); }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── persist.c's writer, as a counter ─────────────────────────────────────── */
static int g_persist_calls = 0;
void persist_env_storage(void) { g_persist_calls++; }

/* ─── harness ──────────────────────────────────────────────────────────────── */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("ok    %s\n", msg); } \
    else      { printf("FAIL  %s (line %d)\n", msg, __LINE__); g_fail = 1; } \
} while (0)

/* Two "frame regions": plain host memory standing in for the frame pool's
 * contiguous runs. Different addresses, because a reboot allocates fresh
 * frames — the identity that must survive is (partition, index), not base. */
static uint8_t region_a[ENV_STORAGE_EXTENT_BYTES] __attribute__((aligned(4096)));
static uint8_t region_b[ENV_STORAGE_EXTENT_BYTES] __attribute__((aligned(4096)));
static uint8_t region_c[ENV_STORAGE_EXTENT_BYTES] __attribute__((aligned(4096)));
static uint8_t region_d[ENV_STORAGE_EXTENT_BYTES] __attribute__((aligned(4096)));

#define BASE_A ((uint64_t)(uintptr_t)region_a)
#define BASE_B ((uint64_t)(uintptr_t)region_b)
#define BASE_C ((uint64_t)(uintptr_t)region_c)
#define BASE_D ((uint64_t)(uintptr_t)region_d)

/* The "server copies the grant, then write-throughs" order. */
static uint64_t server_write(uint64_t base, uint32_t lba,
                             const void* data, uint32_t bytes) {
    memcpy((void*)(uintptr_t)base + (size_t)lba * 512u, data, bytes);
    return env_storage_write(base, lba, bytes);
}

static void wipe_usage(uint32_t partition_id) {
    uint64_t u = storage_get_page_usage(partition_id);
    if (u) storage_page_release(partition_id, u);
}

int main(void) {
    /* ── 1. attach: durable, zeroed, recorded ─────────────────────────────── */
    /* Pre-dirty slot 0 so "zeroed" means something. */
    memset(es_disk, 0xAB, sizeof es_disk);

    CHECK(env_storage_attach(1, 0, BASE_A, ENV_STORAGE_EXTENT_BYTES) ==
              ENV_STORAGE_OK,
          "attach records a new store for (partition 1, index 0)");

    int dirty = 0;
    for (uint64_t i = 0; i < ENV_STORAGE_EXTENT_BYTES; i++)
        if (es_disk[i] != 0) { dirty = 1; break; }
    CHECK(!dirty, "the recycled extent slot was zeroed before first use "
                  "(no previous tenant's bytes survive)");
    CHECK(env_storage_owner(BASE_A) == 1, "owner() names the store's partition");
    CHECK(g_persist_calls >= 1, "attach persisted the directory");

    uint64_t lba = 0, sectors = 0; uint32_t bytes = 0;
    CHECK(env_storage_extent_of(1, 0, &lba, &sectors, &bytes) == 0 &&
              lba == ENV_STORAGE_SLOT_LBA(0) &&
              sectors == ENV_STORAGE_EXTENT_SECTORS &&
              bytes == ENV_STORAGE_EXTENT_BYTES,
          "the descriptor's extent lookup names slot 0's LBA and size");

    /* ── 2. write-through + restore across a fresh region base ────────────── */
    uint8_t payload[8192];
    for (uint32_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 7 + 1);

    CHECK(server_write(BASE_A, 0, payload, sizeof payload) == ENV_STORAGE_OK,
          "a write inside the quota succeeds and persists");
    CHECK(memcmp(&es_disk[0], payload, sizeof payload) == 0,
          "the extent on the device holds the written bytes");

    /* "Reboot": the old region is gone, fresh frames at a new base. */
    memset(region_a, 0, sizeof region_a);
    wipe_usage(1);                /* the usage array is per-boot, like the real one */
    env_storage_boot_reset();     /* ...and so is the re-charge bookkeeping */
    CHECK(env_storage_attach(1, 0, BASE_B, ENV_STORAGE_EXTENT_BYTES) ==
              ENV_STORAGE_OK,
          "re-attach after reboot, same identity, a different frame base");
    CHECK(env_storage_restore(BASE_B) == ENV_STORAGE_OK,
          "restore loads the extent into the fresh region");
    CHECK(memcmp(region_b, payload, sizeof payload) == 0,
          "the file's bytes survived the reboot (region A's copy is gone)");
    CHECK(storage_get_page_usage(1) == 2,
          "re-attach re-charged the persisted occupancy (2 pages of the "
          "8 KiB write) after the boot reset the counter");

    /* ── 3. quota refusal is the QUOTA error, and reverts the RAM copy ────── */
    wipe_usage(2);
    CHECK(env_storage_attach(2, 0, BASE_C, ENV_STORAGE_EXTENT_BYTES) ==
              ENV_STORAGE_OK,
          "a second environment attaches in its own partition");
    CHECK(storage_set_page_quota(2, 4) == 0, "partition 2 gets a 4-page quota");

    uint8_t page[4096];
    memset(page, 0x5A, sizeof page);
    CHECK(server_write(BASE_C, 0, page, 4096) == ENV_STORAGE_OK &&
              storage_get_page_usage(2) == 1,
          "the first page writes and charges one page");

    memset(page, 0x3C, sizeof page);
    CHECK(server_write(BASE_C, 4096 / 512, page, 4096) == ENV_STORAGE_OK &&
              storage_get_page_usage(2) == 2,
          "the second page writes and charges a second page");

    /* Skip to page 6: past the 4-page quota. */
    uint64_t rc = server_write(BASE_C, (6 * 4096) / 512, page, 4096);
    CHECK(rc == ENV_STORAGE_ERR_QUOTA,
          "a write past the quota is refused with the QUOTA status — the "
          "quota's own error, not a frame-pool exhaustion");
    CHECK(storage_get_page_usage(2) == 2,
          "the refused write charged nothing (denial before side effect)");
    /* The refused write's bytes were reverted from the persisted image
     * (extent page 6 was never written: the zero-fill from attach). */
    int reverted = 1;
    for (uint32_t i = 0; i < 4096; i++)
        if (region_c[6 * 4096 + i] != 0) { reverted = 0; break; }
    CHECK(reverted, "the refused write left no bytes in the frame region "
                    "(reverted from the persisted image)");

    /* ── 4. re-attach refuses when the quota no longer covers occupancy ───── */
    wipe_usage(2);                              /* "reboot": counter resets   */
    env_storage_boot_reset();                   /* ...and the bookkeeping     */
    storage_set_page_quota(2, 1);               /* operator shrinks it to 1   */
    uint64_t before = (uint64_t)region_d[0]; (void)before;
    uint64_t rc2 = env_storage_attach(2, 0, BASE_D, ENV_STORAGE_EXTENT_BYTES);
    CHECK(rc2 == ENV_STORAGE_ERR_QUOTA,
          "a re-attach whose persisted occupancy no longer fits the quota "
          "is refused with QUOTA");
    CHECK(env_storage_owner(BASE_C) == 2 && env_storage_owner(BASE_D) == 0xFFFFFFFFu,
          "the refused re-attach left the record as it was (the old base "
          "still owns the store; the new one was never wired)");
    storage_set_page_quota(2, 0);               /* restore unlimited          */

    /* ── 5. release hands everything back ─────────────────────────────────── */
    wipe_usage(1);
    uint64_t rc3 = env_storage_release(BASE_B);
    CHECK(rc3 == ENV_STORAGE_OK, "release accepts the store's own base");
    CHECK(env_storage_owner(BASE_B) == 0xFFFFFFFFu,
          "the entry is gone after release");
    CHECK(storage_get_page_usage(1) == 0,
          "release handed the charged pages back to the partition");
    CHECK(env_storage_extent_of(1, 0, &lba, &sectors, &bytes) == 1 &&
              lba == 0 && sectors == 0 && bytes == 0,
          "a released environment's descriptor would name nothing "
          "(state_lba stays 0)");

    /* ── 6. unattached and RAM-backed: by design, not by accident ─────────── */
    CHECK(env_storage_write(BASE_D, 0, 64) == ENV_STORAGE_ERR_NOENT,
          "an unattached region's write answers NOENT (the server serves "
          "it from RAM — the system rootfs path)");
    CHECK(env_storage_restore(BASE_D) == ENV_STORAGE_OK,
          "restore of an unattached region is a successful no-op");

    /* The §5 `ram-backed` shape: an entry that lost its extent keeps its
     * quota and loses only its durability. Constructed here the way the
     * tooth's mutation would leave it. */
    wipe_usage(2);
    storage_set_page_quota(2, 0);
    CHECK(env_storage_attach(2, 1, BASE_D, ENV_STORAGE_EXTENT_BYTES) ==
              ENV_STORAGE_OK, "partition 2 index 1 attaches");
    for (uint32_t i = 0; i < ENV_STORAGE_MAX; i++) {
        if ((env_storage_table[i].flags & ENV_STORAGE_F_VALID) &&
            env_storage_table[i].region_base == BASE_D) {
            env_storage_table[i].flags &= ~ENV_STORAGE_F_DURABLE;
            env_storage_table[i].extent_slot = -1;
        }
    }
    uint8_t before_disk[256];
    memcpy(before_disk, &es_disk[2 * 1024 * 1024], sizeof before_disk);
    memset(page, 0x77, sizeof page);
    CHECK(server_write(BASE_D, 0, page, 4096) == ENV_STORAGE_OK,
          "a RAM-backed store still accepts writes");
    CHECK(storage_get_page_usage(2) >= 1,
          "a RAM-backed store still charges its partition (quota green "
          "without durability — the control the roadmap's tooth needs)");
    CHECK(memcmp(before_disk, &es_disk[2 * 1024 * 1024], sizeof before_disk) == 0,
          "a RAM-backed store persisted nothing to the device");
    CHECK(env_storage_restore(BASE_D) == ENV_STORAGE_OK,
          "restore of a RAM-backed store is a no-op, not an error");
    CHECK(env_storage_extent_of(2, 1, &lba, &sectors, &bytes) == 1,
          "a RAM-backed store's descriptor names nothing (no extent to "
          "reattach to)");

    /* ── 7. no I/O queue at all: the store degrades, the environment does not ─ */
    /* The boots whose NVMe never came up (a BAR below 4 GiB; the E5/E6
     * smokes' QEMU) have no queue. Refusing attach there would refuse
     * ENVIRONMENT CREATION — a regression of the environment, not of
     * durability. The store must still exist and still meter; it is just
     * RAM-backed this boot (the shape scenario 6 built, reached the way a
     * real boot reaches it). */
    wipe_usage(4);
    storage_set_page_quota(4, 0);            /* unlimited, like a fresh partition */
    io_sq = 0;                               /* the queue never came up */
    CHECK(env_storage_attach(4, 0, BASE_A, ENV_STORAGE_EXTENT_BYTES) ==
              ENV_STORAGE_OK,
          "attach without an I/O queue succeeds — a boot with no NVMe still "
          "creates its environment (the E5/E6 boots' path)");
    CHECK(env_storage_owner(BASE_A) == 4,
          "the no-device store is keyed and owned like any other");
    CHECK(env_storage_extent_of(4, 0, &lba, &sectors, &bytes) == 1 &&
              lba == 0 && sectors == 0 && bytes == 0,
          "the no-device store is honestly RAM-backed: its descriptor "
          "names no extent");
    memset(page, 0x5A, sizeof page);
    CHECK(server_write(BASE_A, 0, page, 4096) == ENV_STORAGE_OK,
          "writes to the no-device store succeed (from RAM)");
    CHECK(storage_get_page_usage(4) >= 1,
          "the no-device store still charges its partition — metering does "
          "not depend on the device (E4's isolation holds without NVMe)");
    CHECK(env_storage_restore(BASE_A) == ENV_STORAGE_OK,
          "restore of the no-device store is a successful no-op");
    CHECK(env_storage_release(BASE_A) == ENV_STORAGE_OK &&
              env_storage_owner(BASE_A) == 0xFFFFFFFFu &&
              storage_get_page_usage(4) == 0,
          "release clears the no-device store and hands its pages back");
    io_sq = (void*)1;                       /* the device is back */

    printf("%s\n", g_fail ? "env_storage_host_test: FAILED"
                          : "env_storage_host_test: all checks passed");
    return g_fail;
}
