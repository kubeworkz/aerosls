/*
 * dma.h — Phase 4: DMA buffer allocator.
 *
 * Provides physically contiguous, pinned memory for device DMA. Each
 * device gets its own IOMMU domain; the kernel programs IOMMU page tables
 * so a device can only DMA to its own buffers.
 *
 * Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §4.
 *
 * The DMA pool is a separate physically contiguous region carved from
 * frame_pool_reserve_contiguous() at boot, distinct from the cap arena.
 * Allocation is bitmap-based first-fit with alignment support.
 *
 * The cap layer's cap_create_dma_mem() delegates to dma_alloc() for the
 * physical pages, then upgrades the resulting MEM cap to DMA_MEM and
 * programs IOMMU mappings. Sidecars can also use the direct syscalls
 * (SYS_SLS_DMA_ALLOC / DMA_FREE / DMA_SHARE) for buffer management
 * without going through the capability creation path.
 */
#ifndef DMA_H
#define DMA_H

#include <stdint.h>

/* ─── Limits ────────────────────────────────────────────────────────────── */
#define DMA_POOL_SIZE       (32u * 1024u * 1024u)  /* 32 MiB default pool */
#define DMA_POOL_FRAMES     (DMA_POOL_SIZE / 4096u) /* 8192 frames */
#define DMA_BUFFER_MAX      1024                     /* buffer descriptors */
#define DMA_MAX_SHARES      16                       /* max MEM caps per buffer */
#define DMA_DOMAIN_MAX      64                       /* IOMMU domains */

/* ─── Buffer flags ──────────────────────────────────────────────────────── */
#define DMA_FLAG_COHERENT   0x01   /* cache-coherent DMA */
#define DMA_FLAG_RO_DEVICE  0x02   /* device reads only */
#define DMA_FLAG_WO_DEVICE  0x04   /* device writes only */

/* ─── Alignment presets ─────────────────────────────────────────────────── */
#define DMA_ALIGN_4K        1      /* 4 KiB (1 page) */
#define DMA_ALIGN_16K       4      /* 16 KiB */
#define DMA_ALIGN_64K       16     /* 64 KiB (e1000 descriptor rings) */
#define DMA_ALIGN_2M        512    /* 2 MiB (huge page) */

/* ─── Buffer descriptor ───────────────────────────────────────────────────
 * One per allocated DMA buffer. Monotonically assigned; ids are never
 * recycled (same discipline as CapObject ids in cap.c). */
struct DMABufferDesc {
    uint32_t frame_index;       /* base frame index within DMA pool */
    uint32_t npages;            /* number of contiguous 4K pages */
    uint32_t iommu_domain;      /* owning IOMMU domain id */
    uint16_t owner_pid;         /* owning sidecar pid (0 = kernel) */
    uint8_t  flags;             /* DMA_FLAG_* */
    uint8_t  pinned;            /* 1 = pages are pinned (non-reclaimable) */
    uint32_t shared_count;      /* number of MEM caps referencing this */
    uint32_t cap_obj_ids[DMA_MAX_SHARES]; /* object ids of shared MEM caps */
};

/* ─── IOMMU domain ──────────────────────────────────────────────────────── */
struct IOMMUDomain {
    uint32_t device_id;         /* PCI device this domain serves */
    uint32_t map_count;         /* active IOMMU mappings */
    uint8_t  active;
    uint8_t  _pad[3];
};

/* ─── DMA pool state ────────────────────────────────────────────────────── */
struct DMAPool {
    uint64_t base_phys;                        /* physical base address */
    uint32_t total_frames;                     /* total frames in pool */
    uint32_t free_frames;                      /* currently free */
    uint32_t alloc_count;                      /* monotonic allocation counter */
    uint8_t  bitmap[(DMA_POOL_FRAMES + 7) / 8]; /* allocation bitmap */
    struct DMABufferDesc buffers[DMA_BUFFER_MAX];
    uint32_t buffer_free_head;                 /* freelist head (index) */
    struct IOMMUDomain domains[DMA_DOMAIN_MAX];
    uint32_t domain_count;
};

/* ─── Public API ────────────────────────────────────────────────────────── */

/* Boot-time init: carve DMA pool from frame pool, set up freelist. */
void dma_init(void);

/* Allocate a contiguous DMA buffer. Returns buffer id (>= 0) or -1.
 * The buffer is NOT pinned by default — call dma_pin() after allocation
 * if the buffer will be used for DMA. Alignment is in pages (1 = 4K). */
int dma_alloc(uint32_t npages, uint32_t align_pages, uint8_t flags);

/* Free a DMA buffer. Unpins if pinned, unmaps from IOMMU, revokes all
 * outstanding MEM caps, returns frames to the pool. */
int dma_free(uint32_t buf_id);

/* Pin (make non-reclaimable) or unpin a DMA buffer. Pinning is required
 * before the buffer can be used for DMA — the IOMMU programs physical
 * addresses that must remain stable. */
int dma_pin(uint32_t buf_id);
int dma_unpin(uint32_t buf_id);

/* Get the physical base address of a DMA buffer. Returns 0 on error. */
uint64_t dma_phys(uint32_t buf_id);

/* Get the buffer descriptor. Returns NULL on error. */
const struct DMABufferDesc* dma_desc(uint32_t buf_id);

/* Create a MEM cap referencing this DMA buffer in the given pid's table.
 * The cap is added to the buffer's shared_count and tracked for cleanup.
 * Returns the cap slot index, or CAP_NONE (0xFFFF) on error. */
uint16_t dma_share(uint32_t buf_id, uint32_t target_pid, uint8_t rights);

/* Revoke all outstanding MEM caps for a buffer (called on dma_free). */
void dma_revoke_caps(uint32_t buf_id);

/* ─── IOMMU domain management ──────────────────────────────────────────── */

/* Create an IOMMU domain for a device. Returns domain id, or -1. */
int dma_iommu_create_domain(uint32_t device_id);

/* Destroy an IOMMU domain (unmaps all pages). */
void dma_iommu_destroy_domain(uint32_t domain_id);

/* Map/unmap a buffer's physical pages into an IOMMU domain.
 * Called automatically by dma_alloc/dma_free, but also available
 * for explicit remapping (e.g., sharing a buffer with another device). */
void dma_iommu_map(uint32_t domain_id, uint64_t phys_base, uint32_t npages);
void dma_iommu_unmap(uint32_t domain_id, uint64_t phys_base, uint32_t npages);

/* Bind a buffer to an IOMMU domain (update the domain_id field). */
int dma_iommu_bind(uint32_t buf_id, uint32_t domain_id);

/* ─── Debug ─────────────────────────────────────────────────────────────── */

/* Dump pool state to serial. */
void dma_pool_list(void);

/* Pool statistics. */
uint32_t dma_pool_free_frames(void);
uint32_t dma_pool_total_frames(void);
uint32_t dma_buffer_count(void);

/* ─── Weak arch hooks ─────────────────────────────────────────────────────
 * The real IOMMU driver provides strong overrides. cap.c's weak defaults
 * are no-ops; dma.c's weak defaults delegate to cap.c's (which are also
 * no-ops until the IOMMU driver lands). */

/* Syscall numbers 314-320 are defined in kernel/cap.h to avoid
 * macro redefinition. The request structs and wrappers live here. */

struct SLSDMAAllocRequest {
    uint32_t npages;            /* number of 4K pages */
    uint32_t align_pages;       /* minimum alignment in pages */
    uint8_t  flags;             /* DMA_FLAG_* */
    uint8_t  _pad[3];
    uint32_t out_buf_id;        /* [out] buffer id */
};

struct SLSDMAFreeRequest {
    uint32_t buf_id;            /* buffer id to free */
    uint8_t  _pad[4];
};

struct SLSDMAShareRequest {
    uint32_t buf_id;            /* buffer to share */
    uint32_t target_pid;        /* sidecar to share with */
    uint8_t  rights;            /* CAP_PERM_R | CAP_PERM_W */
    uint8_t  _pad[3];
    uint16_t out_cap_idx;       /* [out] MEM cap slot in target's table */
    uint8_t  _pad2[2];
};

struct SLSDMAPinRequest {
    uint32_t buf_id;
    uint8_t  _pad[4];
};

struct SLSDMAIOMMUMapRequest {
    uint32_t buf_id;
    uint32_t domain_id;
};

/* Thin syscall wrappers (do_syscall ABI: uint64_t return). */
uint64_t sys_sls_dma_alloc(struct SLSDMAAllocRequest* req);
uint64_t sys_sls_dma_free(struct SLSDMAFreeRequest* req);
uint64_t sys_sls_dma_share(struct SLSDMAShareRequest* req);
uint64_t sys_sls_dma_pin(struct SLSDMAPinRequest* req);
uint64_t sys_sls_dma_unpin(struct SLSDMAPinRequest* req);
uint64_t sys_sls_dma_iommu_map(struct SLSDMAIOMMUMapRequest* req);
uint64_t sys_sls_dma_iommu_unmap(struct SLSDMAIOMMUMapRequest* req);

#endif /* DMA_H */
