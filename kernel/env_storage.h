#ifndef ENV_STORAGE_H
#define ENV_STORAGE_H

#include <stdint.h>
#include "partition.h"

/*
 * env_storage.h — POSIX-Environments Roadmap v0.2, Phase P1b:
 * the environment's durable block store.
 *
 * ─── What this is ───────────────────────────────────────────────────────────
 * An environment's private block device (the `storage` cap the ramdisk
 * server serves RD_READ/RD_WRITE from) used to be *only* the 1 MiB frame
 * region init allocated with alloc_region_in(RD_STORAGE_FRAMES, ...): RAM
 * that a checkpoint had to carry and that was gone the moment nothing
 * restored it. This module is the durable half. The frame region stays —
 * it is the mapped view RD_MAP hands the POSIX sidecar and the cache the
 * ramdisk server serves reads from — but it stops being the authoritative
 * copy:
 *
 *   - init ATTACHes the region when an environment is created, before the
 *     ramdisk server is spawned. The kernel records the region under the
 *     environment's identity (partition_id, index), allocates a 1 MiB NVMe
 *     extent for it from a fixed band, and zeroes the extent once.
 *   - the ramdisk server RESTOREs at its RD_INFO handshake: the extent's
 *     bytes are read back into the frame region, so a rebooted environment
 *     mounts the filesystem it wrote before the reboot.
 *   - every RD_WRITE is WRITE-through: the kernel charges the partition's
 *     storage quota for the extent's first-touched pages and writes the
 *     touched frame pages to NVMe, refusing the whole call (and reverting
 *     the frame pages from their last persisted image) when the partition
 *     has no quota left — the quota's own denial, not a frame-pool
 *     exhaustion (v0.2 §5's verification plan demands exactly that
 *     distinction).
 *   - the environment descriptor NAMES it: env_ckpt_register_from() stamps
 *     state_lba/state_sectors/state_bytes from this table, so a restore
 *     reattaches to the same store and a migration (P3) has an extent to
 *     carry. That is why P1b depends on P1a (§5's fourth bullet).
 *   - destroy RELEASEs: the extent returns to the band and the charged
 *     pages are handed back to the partition's quota.
 *
 * ─── Why a dedicated LBA region and not a stream (§11 Q1) ──────────────────
 * §11 left this open and asked for it to be settled on the facts. The
 * facts: stream.c is STREAM_MAX (8) FIXED 64 MiB slots — half a gigabyte
 * per environment would-be — and storage_quota.h deliberately excludes
 * streams from the per-partition page quota ("a per-partition quota on top
 * of an 8-slot total would be redundant"). P1b needs the opposite of both:
 * a store sized to the environment's actual 1 MiB region, charged to the
 * tenant. A dedicated band of per-environment extents with its own small
 * directory is the smaller, honest shape; it also keeps the quota story in
 * one place (storage_page_reserve, the same primitive rowstore/vecstore
 * use). Migration gains nothing from being a stream today — P3 moves
 * partitions and names this extent in the descriptor regardless.
 *
 * ─── The quota model, and what it deliberately does not do ────────────────
 * Pages are charged on FIRST TOUCH (a high-water mark: pages [0,
 * charged_pages) of the extent are charged to the partition), mirroring
 * rowstore.c's Phase 1 posture: denial happens before any side effect,
 * there is no reclaim path (rowstore/vecstore have none either), and a
 * freed file's pages stay charged until the environment is destroyed and
 * env_storage_release() hands the whole count back. charged_pages lives in
 * the persisted directory, so a rebooted environment is re-charged for what
 * it already occupies instead of starting metered-empty — an attach that
 * cannot re-charge is refused with the quota error before anything is
 * created (refusal over partial application).
 *
 * ─── Keys ──────────────────────────────────────────────────────────────────
 * The table is keyed by (partition_id, index) — the environment's identity,
 * which is stable across a reboot — because region_base is NOT: init
 * allocates a fresh frame region every boot. region_base is carried in the
 * entry as the current placement and is the lookup key for restore/write/
 * release, which the ramdisk server can only name from its own cap; attach
 * keeps the two in step (it runs before the server exists).
 *
 * ─── Unattached regions are RAM, by design ────────────────────────────────
 * The system rootfs ramdisk (instance 0, storage rights 0x1) is not an
 * environment and never attaches; its writes answer NOENT and the server
 * serves them from RAM exactly as before. Durability and quota are
 * properties an attached region HAS, not that every region has — so
 * "attach happened" is a source clause the guard pins rather than an
 * assumption buried in the write path.
 */

/* One store per environment; ENV_CKPT_MAX is the same number (8) because
 * the descriptor and the store are per-environment resources. */
#define ENV_STORAGE_MAX          8u

/* The extent: exactly the tenant ramdisk's region (RD_STORAGE_FRAMES 256 ×
 * 4 KiB), so region page i maps to extent page i with no translation. */
#define ENV_STORAGE_PAGE_BYTES   4096u
#define ENV_STORAGE_PAGES        256u
#define ENV_STORAGE_EXTENT_BYTES (ENV_STORAGE_PAGES * ENV_STORAGE_PAGE_BYTES) /* 1 MiB */
#define ENV_STORAGE_EXTENT_SECTORS (ENV_STORAGE_EXTENT_BYTES / 512u)          /* 2048 */

/* ─── Where the bytes live on NVMe ────────────────────────────────────────
 * The data band starts exactly where stream data ends and stops long before
 * the row-store pool begins:
 *
 *   STREAM_DATA_LBA_BASE 65536 + 8 slots × 131072 sectors = 1 114 112
 *   (nothing else is mapped between there and ROWSTORE_LBA_BASE 2 000 000)
 *
 * 8 extents × 2048 sectors = 16 384 sectors (8 MiB) — 1114112..1130496.
 * Derived here from the stream.h constants rather than trusting a
 * transcription (persist_lba_layout_host_test.c's own lesson). */
#define ENV_STORAGE_DATA_LBA_BASE 1114112ULL
#define ENV_STORAGE_SLOT_LBA(slot) (ENV_STORAGE_DATA_LBA_BASE + \
    ((uint64_t)(slot) * (uint64_t)ENV_STORAGE_EXTENT_SECTORS))

/* ─── The persisted directory ─────────────────────────────────────────────
 * Two frames in the small-record band: the P1a region's entries end at
 * 7712, the 1-frame safety gap every boundary in persist.h carries lands
 * on 7720, the header takes 7720..7728, the 8 × 32 B entries take
 * 7728..7736, and STREAM_DIR_LBA 8192 is still 456 sectors away. */
#define PERSIST_ENVSTOR_HDR_LBA  7720ULL
#define PERSIST_ENVSTOR_ENT_LBA  7728ULL
#define ENV_STORAGE_REC_VERSION  1u   /* bumped by the WRITER on layout change */

/* ─── The table ─────────────────────────────────────────────────────────── */
#define ENV_STORAGE_F_VALID      0x0001u
#define ENV_STORAGE_F_DURABLE    0x0002u  /* an extent is allocated; without it
                                           * the region is RAM-backed (the §5
                                           * `ram-backed` tooth's shape) */

struct EnvStorageEntry {
    uint32_t partition_id;   /* the tenant charged for the pages — set once   */
    uint32_t index;          /* the environment's index within that partition */
    uint64_t region_base;    /* this boot's frame-region base (the IO key)    */
    uint32_t region_bytes;   /* the region's size; must fit one extent        */
    uint32_t charged_pages;  /* first-touch high-water mark (persisted)       */
    int32_t  extent_slot;    /* [0, ENV_STORAGE_MAX) when durable, -1 if not  */
    uint32_t flags;          /* ENV_STORAGE_F_*                               */
};
extern struct EnvStorageEntry env_storage_table[ENV_STORAGE_MAX];

/* ─── Status codes (the syscall return values) ────────────────────────────
 * Named rather than boolean so a refusal can be told apart by a guard the
 * same way EnvCkptRefusal's codes can. QUOTA is its own code: it is the
 * one the roadmap's verification plan insists be distinguishable from a
 * frame-pool exhaustion. */
#define ENV_STORAGE_OK        0u
#define ENV_STORAGE_ERR_QUOTA 1u  /* storage_page_reserve refused a page      */
#define ENV_STORAGE_ERR_NOSLOT 2u /* ENV_STORAGE_MAX environments already     */
#define ENV_STORAGE_ERR_NOENT 3u  /* no entry keys this region (unattached)   */
#define ENV_STORAGE_ERR_ID    4u  /* (partition,index) collision on re-attach */
#define ENV_STORAGE_ERR_INVAL 5u  /* region too big / misaligned / bad args   */
#define ENV_STORAGE_ERR_IO    6u  /* an NVMe command failed                   */

/* ─── Syscalls (323-326; 322 is the highest existing SYS_SLS_*) ─────────── */
#define SYS_SLS_ENV_STORAGE_ATTACH   323
#define SYS_SLS_ENV_STORAGE_RESTORE  324
#define SYS_SLS_ENV_STORAGE_WRITE    325
#define SYS_SLS_ENV_STORAGE_RELEASE  326

struct SLSEnvStorageAttachRequest {
    uint32_t partition_id;
    uint32_t index;
    uint64_t region_base;   /* the frame region init just allocated          */
    uint64_t region_bytes;  /* RD_STORAGE_BYTES                              */
};

struct SLSEnvStorageRegionRequest {
    uint64_t region_base;
};

struct SLSEnvStorageWriteRequest {
    uint64_t region_base;
    uint32_t lba;           /* first 512 B sector, region-relative           */
    uint32_t bytes;         /* length of the just-copied write               */
};

uint64_t sys_sls_env_storage_attach(struct SLSEnvStorageAttachRequest* req);
uint64_t sys_sls_env_storage_restore(struct SLSEnvStorageRegionRequest* req);
uint64_t sys_sls_env_storage_write(struct SLSEnvStorageWriteRequest* req);
uint64_t sys_sls_env_storage_release(struct SLSEnvStorageRegionRequest* req);

/* ─── Kernel-internal API (also the host test's surface) ────────────────── */

/* Record the region under (partition_id, index): find or create the entry,
 * allocate and zero an extent on first create, and re-charge the persisted
 * charged_pages to the partition (refusing with ERR_QUOTA before anything
 * is created if it no longer fits). Returns ENV_STORAGE_*. */
uint64_t env_storage_attach(uint32_t partition_id, uint32_t index,
                            uint64_t region_base, uint64_t region_bytes);

/* Read the extent back into the frame region (the ramdisk server's RD_INFO
 * step). A RAM-backed entry or a missing entry is a successful no-op —
 * there is nothing durable to restore — so one code path serves both the
 * system ramdisk and the §5 `ram-backed` tooth. */
uint64_t env_storage_restore(uint64_t region_base);

/* Write-through: charge first-touch pages, persist the touched frame pages
 * to the extent, and — on a quota refusal — revert the frame pages from
 * their persisted image so a refused write leaves no trace in RAM either.
 * NOENT (unattached region) is success-without-durability, by design. */
uint64_t env_storage_write(uint64_t region_base, uint32_t lba, uint32_t bytes);

/* Destroy-side release: hand the charged pages back to the partition,
 * return the extent to the band, clear the entry, persist the directory. */
uint64_t env_storage_release(uint64_t region_base);

/* The boot boundary: clears this-boot-only state (which stores have
 * re-charged their persisted occupancy). persist_restore_all() calls it
 * before loading the directory; the kernel's power-on BSS zeroing would do
 * the same, but an explicit line is a seam a guard can pin. */
void env_storage_boot_reset(void);

/* The descriptor's half: env_ckpt_register_from() looks the entry up by
 * (partition_id, index) and stamps state_lba/state_sectors/state_bytes from
 * it. Returns 0 when the region is attached and durable, 1 when there is
 * nothing to name (the fields stay 0 — "0 until then"). */
int env_storage_extent_of(uint32_t partition_id, uint32_t index,
                          uint64_t* out_lba, uint64_t* out_sectors,
                          uint32_t* out_bytes);

/* The partition that owns a region's store, or 0xFFFFFFFF when the region
 * is not attached. The syscall wrappers (cap.c) gate on this so one
 * environment's ramdisk server can never name another's region — the
 * trusted-lookup half of the same discipline sys_sls_alloc_region uses. */
uint32_t env_storage_owner(uint64_t region_base);

#endif /* ENV_STORAGE_H */
