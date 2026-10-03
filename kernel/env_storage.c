/*
 * env_storage.c — POSIX-Environments v0.2 Phase P1b: the environment's
 * durable block store. See env_storage.h for the full design writeup
 * (why a dedicated LBA region rather than a stream, the first-touch quota
 * model, the keys, and why an unattached region is RAM by design).
 *
 * Dependencies are deliberately the host-testable set: storage_quota.h
 * (accounting only), persist.h (the directory's writer), nvme_io.h (the
 * device), kernel_io.h (the two logging functions), cap.h + partition.h
 * (caller gating for the syscall wrappers). The host test stubs the NVMe
 * layer exactly the way tests/rowstore_io_stubs.c already stubs it for the
 * row-store: a RAM disk behind the same entry points.
 */
#include "env_storage.h"
#include "storage_quota.h"
#include "persist.h"
#include "../drivers/nvme_io.h"
#include "kernel_io.h"

/* The syscall wrappers live in cap.c beside sys_sls_alloc_region (the
 * region-family precedent); this file stays inside the host-testable
 * dependency set, and the wrappers gate the caller the way cap.c always
 * does — cap_current_pid(), never a request field. */

/* The live directory. persist.c owns the region (p_region_specs) and loads
 * this array at boot through the same trusted-checksum path every other
 * persisted array uses; every mutation below marks the region pending. */
struct EnvStorageEntry env_storage_table[ENV_STORAGE_MAX];

/* The persisted entry size is pinned here, where the directory's writer and
 * the boot restore both mean it: persist.c's p_region_specs checksums
 * `sizeof(env_storage_table)` as a compile-time constant, so an entry that
 * grew would silently truncate into the next frame (or checksum a span the
 * writer no longer fills). 32 B = the struct's natural x86-64 layout with
 * no padding to lose. */
_Static_assert(sizeof(struct EnvStorageEntry) == 32,
               "EnvStorageEntry must stay 32 bytes (persist checksum span)");

/* Which slots have had their persisted charged_pages re-charged to the
 * partition THIS boot. RAM-only on purpose: the usage counter is a
 * per-boot array too, and the persisted value is the occupancy to
 * re-charge, not the fact that it was done. */
static uint8_t es_boot_charged[ENV_STORAGE_MAX];

/* ─── small helpers (no libc here; every module carries its own, as
 * env_ckpt.c's ec_memset and persist.c's p_memcpy do) ──────────────────── */
static void es_memset(void* dst, uint8_t v, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

static int es_io_ready(void) {
    /* Same posture stream.c's own skip rule has: without an I/O queue
     * (NVMe MMIO above 4 GiB, host tests) there is no device to talk to. */
    return io_sq != 0;
}

/* Every mutation persists the directory through persist.c's writer (which
 * folds into a running defer batch the way persist_partitions() does). */
static void es_mark_pending(void) { persist_env_storage(); }

/* The boot boundary. The kernel relies on BSS zeroing for this at power-on;
 * persist_restore_all() calls it explicitly before loading the directory so
 * the reboot seam is a line of code a guard can pin rather than an accident
 * of the loader, and so host tests can cross the same boundary honestly. */
void env_storage_boot_reset(void) {
    for (uint32_t i = 0; i < ENV_STORAGE_MAX; i++) es_boot_charged[i] = 0;
}

static struct EnvStorageEntry* es_find_key(uint32_t partition_id, uint32_t index) {
    for (uint32_t i = 0; i < ENV_STORAGE_MAX; i++) {
        struct EnvStorageEntry* e = &env_storage_table[i];
        if ((e->flags & ENV_STORAGE_F_VALID) &&
            e->partition_id == partition_id && e->index == index)
            return e;
    }
    return 0;
}

static struct EnvStorageEntry* es_find_base(uint64_t region_base) {
    if (region_base == 0) return 0;
    for (uint32_t i = 0; i < ENV_STORAGE_MAX; i++) {
        struct EnvStorageEntry* e = &env_storage_table[i];
        if ((e->flags & ENV_STORAGE_F_VALID) && e->region_base == region_base)
            return e;
    }
    return 0;
}

static uint32_t es_slot_of(const struct EnvStorageEntry* e) {
    return (uint32_t)(e - env_storage_table);
}

/* Charge pages [e->charged_pages, need) to e->partition_id. On denial every
 * page THIS call charged is released again (the watermark must only ever
 * advance over pages that are actually charged) and 1 is returned; on
 * success 0 and the watermark already advanced. Denial therefore happens
 * before any NVMe byte moves — storage_quota.h's own contract. */
static int es_charge_to(struct EnvStorageEntry* e, uint32_t need) {
    uint32_t got = 0;
    for (uint32_t p = e->charged_pages; p < need; p++) {
        if (storage_page_reserve(e->partition_id)) {
            if (got) storage_page_release(e->partition_id, got);
            kernel_serial_printf(
                "[ENV-STORAGE] quota denied partition=%u pages_wanted=%llu "
                "pages_charged=%llu (the storage quota's own refusal, not a "
                "frame-pool exhaustion)\n",
                (unsigned)e->partition_id,
                (unsigned long long)need,
                (unsigned long long)e->charged_pages);
            return 1;
        }
        got++;
    }
    e->charged_pages = need;
    if (got) es_boot_charged[es_slot_of(e)] = 1;
    return 0;
}

/* ─── attach ─────────────────────────────────────────────────────────────── */
uint64_t env_storage_attach(uint32_t partition_id, uint32_t index,
                            uint64_t region_base, uint64_t region_bytes) {
    if (region_base == 0 || region_bytes == 0 ||
        (region_bytes % ENV_STORAGE_PAGE_BYTES) != 0 ||
        region_bytes > ENV_STORAGE_EXTENT_BYTES) {
        kernel_serial_printf(
            "[ENV-STORAGE] attach refused: region bytes=%llu does not fit "
            "one %u-byte extent\n",
            (unsigned long long)region_bytes,
            (unsigned)ENV_STORAGE_EXTENT_BYTES);
        return ENV_STORAGE_ERR_INVAL;
    }

    struct EnvStorageEntry* e = es_find_key(partition_id, index);
    if (e) {
        /* Re-attach — the post-reboot path: same identity, a fresh frame
         * region. Validate the identity is the one this entry records
         * before adopting the new placement, and re-charge what the store
         * already occupies (refusing before anything is wired if the
         * partition's quota no longer covers it). */
        uint32_t s = es_slot_of(e);
        if (e->region_bytes != region_bytes) {
            kernel_serial_printf(
                "[ENV-STORAGE] attach refused: partition=%u index=%u store "
                "is %llu bytes, this create asked for %llu\n",
                (unsigned)partition_id, (unsigned)index,
                (unsigned long long)e->region_bytes,
                (unsigned long long)region_bytes);
            return ENV_STORAGE_ERR_ID;
        }
        if (!es_boot_charged[s]) {
            /* Re-charge what the store already occupies: the usage counter
             * is per-boot, the watermark is persisted, and a partition
             * whose quota no longer covers the store it already holds
             * refuses the create before anything is wired — refusal over
             * partial application, exactly like the attach that follows. */
            uint32_t persisted = e->charged_pages;
            e->charged_pages = 0;
            if (es_charge_to(e, persisted)) {
                e->charged_pages = persisted;  /* leave the record as it was */
                return ENV_STORAGE_ERR_QUOTA;
            }
            es_boot_charged[s] = 1;
        }
        e->region_base  = region_base;
        e->region_bytes = (uint32_t)region_bytes;
        es_mark_pending();
        kernel_serial_printf(
            "[ENV-STORAGE] attach partition=%u index=%u base=%llu bytes=%llu "
            "slot=%ld durable=%u charged=%llu (reattach)\n",
            (unsigned)partition_id, (unsigned)index,
            (unsigned long long)region_base,
            (unsigned long long)region_bytes,
            (long)e->extent_slot,
            (unsigned)((e->flags & ENV_STORAGE_F_DURABLE) ? 1u : 0u),
            (unsigned long long)e->charged_pages);
        return ENV_STORAGE_OK;
    }

    /* First create for this identity: find the entry slot (the extent slot
     * is the same number — one store, one extent, 1:1). */
    uint32_t s = ENV_STORAGE_MAX;
    for (uint32_t i = 0; i < ENV_STORAGE_MAX; i++) {
        if (!(env_storage_table[i].flags & ENV_STORAGE_F_VALID)) { s = i; break; }
    }
    if (s == ENV_STORAGE_MAX) {
        kernel_serial_printf(
            "[ENV-STORAGE] attach refused: all %u environment stores are in "
            "use (partition=%u index=%u)\n",
            (unsigned)ENV_STORAGE_MAX, (unsigned)partition_id, (unsigned)index);
        return ENV_STORAGE_ERR_NOSLOT;
    }

    struct EnvStorageEntry* ne = &env_storage_table[s];

    /* No device this boot — NVMe's MMIO never came up (the same posture
     * stream.c's cold-start skip has). The store still exists and is still
     * metered; it is just RAM-backed, and the serial line says so
     * (durable=0, slot=-1). Refusing here would refuse ENVIRONMENT
     * CREATION on every such boot — the E5/E6 boots, and any node whose
     * BAR landed below 4 GiB — which is a regression of the environment,
     * not of durability: restore and write-through already branch on the
     * durable flag. */
    if (!es_io_ready()) {
        es_memset(ne, 0, (uint32_t)sizeof(*ne));
        ne->partition_id = partition_id;
        ne->index        = index;
        ne->region_base  = region_base;
        ne->region_bytes = (uint32_t)region_bytes;
        ne->charged_pages = 0;
        ne->extent_slot  = -1;
        ne->flags        = ENV_STORAGE_F_VALID;
        es_boot_charged[s] = 1;   /* 0 pages charged: nothing to re-charge */
        es_mark_pending();
        kernel_serial_printf(
            "[ENV-STORAGE] attach partition=%u index=%u base=%llu bytes=%llu "
            "slot=-1 durable=0 charged=0 (no NVMe queue — RAM-backed)\n",
            (unsigned)partition_id, (unsigned)index,
            (unsigned long long)region_base,
            (unsigned long long)region_bytes);
        return ENV_STORAGE_OK;
    }

    /* The extent must exist and be ZERO before any sidecar can read a byte
     * of it: a recycled slot would otherwise hand the new environment the
     * previous tenant's data. Fail closed HERE — with a device present but
     * unreachable, "durable" that never reaches media is the lie this
     * phase exists to not tell. */
    for (uint32_t pg = 0; pg < ENV_STORAGE_PAGES; pg++) {
        /* 4096-aligned like every buffer that reaches nvme_*_sync:
         * nvme_buf_aligned() rejects a misaligned one with 0xFC, which
         * would make every attach refuse (the failure the live boot saw
         * before this attribute was here). */
        static const uint8_t __attribute__((aligned(4096))) zero_page[4096];
        if (nvme_write_sync(ENV_STORAGE_SLOT_LBA(s) + (uint64_t)pg * 8u,
                            zero_page) != 0) {
            kernel_serial_printf(
                "[ENV-STORAGE] attach refused: zeroing extent slot=%u page=%u "
                "failed\n", (unsigned)s, (unsigned)pg);
            return ENV_STORAGE_ERR_IO;
        }
    }

    es_memset(ne, 0, (uint32_t)sizeof(*ne));
    ne->partition_id = partition_id;
    ne->index        = index;
    ne->region_base  = region_base;
    ne->region_bytes = (uint32_t)region_bytes;
    ne->charged_pages = 0;
    ne->extent_slot  = (int32_t)s;
    ne->flags        = ENV_STORAGE_F_VALID | ENV_STORAGE_F_DURABLE;
    es_boot_charged[s] = 1;   /* 0 pages charged: nothing to re-charge */
    es_mark_pending();

    kernel_serial_printf(
        "[ENV-STORAGE] attach partition=%u index=%u base=%llu bytes=%llu "
        "slot=%ld durable=1 charged=0 (new store)\n",
        (unsigned)partition_id, (unsigned)index,
        (unsigned long long)region_base,
        (unsigned long long)region_bytes, (long)s);
    return ENV_STORAGE_OK;
}

/* ─── restore (the ramdisk server's RD_INFO step) ────────────────────────── */
uint64_t env_storage_restore(uint64_t region_base) {
    struct EnvStorageEntry* e = es_find_base(region_base);
    if (!e) {
        /* Unattached region — the system ramdisk, and the honest answer
         * for anything that never earned a store. Nothing to restore. */
        return ENV_STORAGE_OK;
    }
    if (!(e->flags & ENV_STORAGE_F_DURABLE) || !es_io_ready()) {
        /* RAM-backed by design (the §5 `ram-backed` tooth's shape): the
         * region's bytes start where the frame pool left them. */
        kernel_serial_printf(
            "[ENV-STORAGE] restore slot=%ld: RAM-backed, nothing to load\n",
            (long)e->extent_slot);
        return ENV_STORAGE_OK;
    }

    uint32_t pages = e->region_bytes / ENV_STORAGE_PAGE_BYTES;
    uint32_t pg = 0;
    while (pg < pages) {
        uint32_t n = pages - pg;
        if (n > 32) n = 32;  /* NVME_MAX_PAGES_PER_XFER */
        if (nvme_read_pages_sync(ENV_STORAGE_SLOT_LBA((uint32_t)e->extent_slot) +
                                 (uint64_t)pg * 8u,
                                 (void*)(region_base + (uint64_t)pg * 4096u),
                                 n) != 0) {
            kernel_serial_printf(
                "[ENV-STORAGE] restore failed: slot=%ld page=%u (device "
                "error) — the environment mounts an empty store rather than "
                "a half-loaded one only if the mount itself refuses; retry "
                "the boot\n", (long)e->extent_slot, (unsigned)pg);
            return ENV_STORAGE_ERR_IO;
        }
        pg += n;
    }
    kernel_serial_printf(
        "[ENV-STORAGE] restore partition=%u index=%u slot=%ld pages=%u "
        "loaded from LBA %llu\n",
        (unsigned)e->partition_id, (unsigned)e->index, (long)e->extent_slot,
        (unsigned)pages,
        (unsigned long long)ENV_STORAGE_SLOT_LBA((uint32_t)e->extent_slot));
    return ENV_STORAGE_OK;
}

/* ─── write-through ──────────────────────────────────────────────────────── */
uint64_t env_storage_write(uint64_t region_base, uint32_t lba, uint32_t bytes) {
    struct EnvStorageEntry* e = es_find_base(region_base);
    if (!e) return ENV_STORAGE_ERR_NOENT;   /* unattached: RAM by design */

    if (bytes == 0 ||
        ((uint64_t)lba * 512ull + bytes) > (uint64_t)e->region_bytes) {
        return ENV_STORAGE_ERR_INVAL;
    }

    uint32_t first_page = lba / 8u;                       /* 8 sectors/page */
    uint32_t last_byte  = lba * 512u + bytes - 1u;
    uint32_t last_page  = last_byte / 4096u;
    uint32_t need       = last_page + 1u;                 /* first-touch high-water */

    uint32_t saved = e->charged_pages;
    if (need > saved) {
        if (es_charge_to(e, need)) {
            /* Refused: the pages this call would have charged are released
             * again inside es_charge_to, and the RAM copy the ramdisk
             * server just made must not outlive the refusal either — revert
             * the touched pages from their persisted image so a refused
             * write is invisible to a subsequent read. */
            struct EnvStorageEntry* d = e;
            if ((d->flags & ENV_STORAGE_F_DURABLE) && es_io_ready()) {
                uint32_t pg = first_page;
                while (pg <= last_page) {
                    uint32_t n = last_page - pg + 1;
                    if (n > 32) n = 32;
                    if (nvme_read_pages_sync(
                            ENV_STORAGE_SLOT_LBA((uint32_t)d->extent_slot) +
                            (uint64_t)pg * 8u,
                            (void*)(region_base + (uint64_t)pg * 4096u), n) != 0)
                        break;
                    pg += n;
                }
            }
            return ENV_STORAGE_ERR_QUOTA;
        }
    }

    if ((e->flags & ENV_STORAGE_F_DURABLE) && es_io_ready()) {
        uint32_t pg = first_page;
        while (pg <= last_page) {
            uint32_t n = last_page - pg + 1;
            if (n > 32) n = 32;
            if (nvme_write_pages_sync(
                    ENV_STORAGE_SLOT_LBA((uint32_t)e->extent_slot) +
                    (uint64_t)pg * 8u,
                    (void*)(region_base + (uint64_t)pg * 4096u), n) != 0) {
                kernel_serial_printf(
                    "[ENV-STORAGE] write failed: slot=%ld page=%u (device "
                    "error); the pages stay charged — occupancy was "
                    "claimed\n", (long)e->extent_slot, (unsigned)pg);
                es_mark_pending();
                return ENV_STORAGE_ERR_IO;
            }
            pg += n;
        }
    }

    if (e->charged_pages != saved) es_mark_pending();
    return ENV_STORAGE_OK;
}

/* ─── release (destroy) ──────────────────────────────────────────────────── */
uint64_t env_storage_release(uint64_t region_base) {
    struct EnvStorageEntry* e = es_find_base(region_base);
    if (!e) return ENV_STORAGE_ERR_NOENT;

    uint32_t s = es_slot_of(e);
    uint32_t charged = e->charged_pages;
    if (charged) storage_page_release(e->partition_id, charged);
    kernel_serial_printf(
        "[ENV-STORAGE] release partition=%u index=%u slot=%ld charged=%llu "
        "handed back\n",
        (unsigned)e->partition_id, (unsigned)e->index, (long)e->extent_slot,
        (unsigned long long)charged);
    es_memset(e, 0, (uint32_t)sizeof(*e));
    e->extent_slot = -1;
    es_boot_charged[s] = 0;
    es_mark_pending();
    return ENV_STORAGE_OK;
}

/* ─── the descriptor's half ──────────────────────────────────────────────── */
int env_storage_extent_of(uint32_t partition_id, uint32_t index,
                          uint64_t* out_lba, uint64_t* out_sectors,
                          uint32_t* out_bytes) {
    /* Out params are written unconditionally: a caller that only checks the
     * return code can never be handed a stale extent from a previous query
     * (the descriptor's "0 until then" is enforced here, not by the caller). */
    if (out_lba)     *out_lba     = 0;
    if (out_sectors) *out_sectors = 0;
    if (out_bytes)   *out_bytes   = 0;
    struct EnvStorageEntry* e = es_find_key(partition_id, index);
    if (!e || !(e->flags & ENV_STORAGE_F_DURABLE)) return 1;
    if (out_lba)     *out_lba     = ENV_STORAGE_SLOT_LBA((uint32_t)e->extent_slot);
    if (out_sectors) *out_sectors = ENV_STORAGE_EXTENT_SECTORS;
    if (out_bytes)   *out_bytes   = e->region_bytes;
    return 0;
}

/* The partition that owns a region's store, or 0xFFFFFFFF when the region
 * is not attached — the capability gate's lookup (a caller naming another
 * environment's region must be refused by the wrapper, not by this file). */
uint32_t env_storage_owner(uint64_t region_base) {
    struct EnvStorageEntry* e = es_find_base(region_base);
    return e ? e->partition_id : 0xFFFFFFFFu;
}
