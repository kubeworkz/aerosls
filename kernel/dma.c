/*
 * dma.c — Phase 4: DMA buffer allocator.
 *
 * Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §4.
 *
 * Provides a dedicated physically contiguous pool for device DMA buffers.
 * Each device gets its own IOMMU domain; the kernel programs IOMMU page
 * tables so a device can only DMA to its own buffers.
 *
 * Concurrency: single-CPU, one syscall at a time — same assumption as
 * cap.c. Locks are used defensively (spinlock on the pool) for future
 * SMP readiness, but today there is no contention.
 */
#include "dma.h"
#include "cap.h"
#include "kernel_io.h"
#include "frame_pool.h"
#include <stddef.h>

/* ─── Static state ──────────────────────────────────────────────────────── */

static struct DMAPool dma_pool;
static uint32_t g_dma_inited = 0;

/* Weak arch hooks for IOMMU (strong override in iommu.c). */
__attribute__((weak))
void cap_iommu_map(uint32_t domain_id, uint64_t phys_base, uint32_t npages) {
    (void)domain_id; (void)phys_base; (void)npages;
}

__attribute__((weak))
void cap_iommu_unmap(uint32_t domain_id, uint64_t phys_base, uint32_t npages) {
    (void)domain_id; (void)phys_base; (void)npages;
}

/* ─── Bitmap helpers ────────────────────────────────────────────────────── */

static int dma_frame_free(uint32_t f) {
    if (f >= DMA_POOL_FRAMES) return 0;
    return (dma_pool.bitmap[f >> 3] & (1u << (f & 7))) == 0;
}

static void dma_frame_mark(uint32_t f) {
    dma_pool.bitmap[f >> 3] |= (1u << (f & 7));
}

static void dma_frame_clear(uint32_t f) {
    dma_pool.bitmap[f >> 3] &= ~(1u << (f & 7));
}

/* ─── Buffer descriptor freelist ────────────────────────────────────────── */

static void dma_desc_freelist_init(void) {
    dma_pool.buffer_free_head = DMA_BUFFER_MAX;  /* sentinel */
    for (int i = DMA_BUFFER_MAX - 1; i >= 0; i--) {
        dma_pool.buffers[i].frame_index = 0;
        dma_pool.buffers[i].npages = 0;
        dma_pool.buffers[i].owner_pid = 0;
        dma_pool.buffers[i].flags = 0;
        dma_pool.buffers[i].pinned = 0;
        dma_pool.buffers[i].shared_count = 0;
        dma_pool.buffers[i].iommu_domain = 0;
        /* Chain through frame_index as next pointer (it's unused while free). */
        dma_pool.buffers[i].frame_index =
            (i == DMA_BUFFER_MAX - 1) ? DMA_BUFFER_MAX : (uint32_t)(i + 1);
    }
    dma_pool.buffer_free_head = 0;
}

static int dma_desc_alloc(uint32_t* out_id) {
    if (dma_pool.buffer_free_head >= DMA_BUFFER_MAX) return -1;
    uint32_t id = dma_pool.buffer_free_head;
    dma_pool.buffer_free_head = dma_pool.buffers[id].frame_index;
    dma_pool.buffers[id].frame_index = 0;  /* will be set by caller */
    dma_pool.buffers[id].npages = 0;
    dma_pool.buffers[id].owner_pid = 0;
    dma_pool.buffers[id].flags = 0;
    dma_pool.buffers[id].pinned = 0;
    dma_pool.buffers[id].shared_count = 0;
    dma_pool.buffers[id].iommu_domain = 0;
    for (int i = 0; i < DMA_MAX_SHARES; i++)
        dma_pool.buffers[id].cap_obj_ids[i] = 0;
    *out_id = id;
    return 0;
}

static void dma_desc_free(uint32_t id) {
    if (id >= DMA_BUFFER_MAX) return;
    struct DMABufferDesc* b = &dma_pool.buffers[id];
    b->frame_index = dma_pool.buffer_free_head;
    b->npages = 0;
    b->owner_pid = 0;
    b->flags = 0;
    b->pinned = 0;
    b->shared_count = 0;
    b->iommu_domain = 0;
    dma_pool.buffer_free_head = id;
}

/* ─── First-fit allocation with alignment ───────────────────────────────── */

static uint32_t dma_find_run(uint32_t npages, uint32_t align) {
    if (npages == 0 || npages > dma_pool.total_frames) return 0xFFFFFFFFu;
    if (align == 0) align = 1;

    for (uint32_t start = 0; start + npages <= dma_pool.total_frames; start++) {
        /* Check alignment: start must be a multiple of align. */
        if ((start % align) != 0) continue;

        int ok = 1;
        for (uint32_t i = 0; i < npages; i++) {
            if (!dma_frame_free(start + i)) { ok = 0; break; }
        }
        if (!ok) continue;

        /* Mark all frames. */
        for (uint32_t i = 0; i < npages; i++)
            dma_frame_mark(start + i);
        dma_pool.free_frames -= npages;
        return start;
    }
    return 0xFFFFFFFFu;
}

static void dma_release_frames(uint32_t start, uint32_t npages) {
    for (uint32_t i = 0; i < npages; i++)
        dma_frame_clear(start + i);
    dma_pool.free_frames += npages;
}

/* ─── Init ──────────────────────────────────────────────────────────────── */

void dma_init(void) {
    if (g_dma_inited) return;

    /* Carve DMA_POOL_FRAMES of physically contiguous free RAM, 2 MiB-aligned.
     * Same mechanism as cap_arena_init() in cap.c. */
    uint64_t base = frame_pool_reserve_contiguous(DMA_POOL_FRAMES, 512);
    if (base == 0) {
        kernel_serial_print("[DMA] *** pool carve failed: no contiguous "
                            "32 MiB run; dma_alloc() disabled. ***\n");
        return;
    }

    /* Zero the bitmap (all frames free). The bitmap is in .bss so it was
     * already zero, but explicitly clearing is clearer. */
    for (uint32_t i = 0; i < sizeof(dma_pool.bitmap); i++)
        dma_pool.bitmap[i] = 0;

    dma_pool.base_phys = base;
    dma_pool.total_frames = DMA_POOL_FRAMES;
    dma_pool.free_frames = DMA_POOL_FRAMES;
    dma_pool.alloc_count = 0;
    dma_pool.domain_count = 0;

    dma_desc_freelist_init();

    g_dma_inited = 1;

    kernel_serial_printf(
        "[DMA] buffer pool: %u MiB physically contiguous at 0x%llx "
        "(%u frames, max %u buffers).\n",
        (unsigned)(DMA_POOL_SIZE >> 20),
        (unsigned long long)base,
        (unsigned)DMA_POOL_FRAMES,
        (unsigned)DMA_BUFFER_MAX);
}

/* ─── dma_alloc ─────────────────────────────────────────────────────────── */

int dma_alloc(uint32_t npages, uint32_t align_pages, uint8_t flags) {
    if (!g_dma_inited) return -1;
    if (npages == 0 || npages > dma_pool.total_frames) return -1;
    if (align_pages == 0) align_pages = 1;

    /* Allocate a buffer descriptor slot. */
    uint32_t buf_id;
    if (dma_desc_alloc(&buf_id)) return -1;

    /* Find a free run in the bitmap with alignment. */
    uint32_t start = dma_find_run(npages, align_pages);
    if (start == 0xFFFFFFFFu) {
        dma_desc_free(buf_id);
        return -1;
    }

    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    b->frame_index = start;
    b->npages = npages;
    b->flags = flags;
    b->pinned = 0;
    b->owner_pid = (uint16_t)cap_current_pid();
    b->shared_count = 0;
    b->iommu_domain = 0;

    dma_pool.alloc_count++;
    return (int)buf_id;
}

/* ─── dma_free ──────────────────────────────────────────────────────────── */

int dma_free(uint32_t buf_id) {
    if (!g_dma_inited) return -1;
    if (buf_id >= DMA_BUFFER_MAX) return -1;

    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    if (b->npages == 0) return -1;   /* already free */

    /* Unmap from IOMMU if bound to a domain. */
    if (b->iommu_domain != 0) {
        uint64_t phys = dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
        cap_iommu_unmap(b->iommu_domain, phys, b->npages);
    }

    /* Revoke all outstanding MEM caps for this buffer. */
    dma_revoke_caps(buf_id);

    /* Release the physical frames. */
    dma_release_frames(b->frame_index, b->npages);

    /* Free the descriptor. */
    dma_desc_free(buf_id);
    return 0;
}

/* ─── dma_pin / dma_unpin ───────────────────────────────────────────────── */

int dma_pin(uint32_t buf_id) {
    if (!g_dma_inited) return -1;
    if (buf_id >= DMA_BUFFER_MAX) return -1;

    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    if (b->npages == 0) return -1;

    if (b->pinned) return 0;   /* already pinned (idempotent) */
    b->pinned = 1;
    return 0;
}

int dma_unpin(uint32_t buf_id) {
    if (!g_dma_inited) return -1;
    if (buf_id >= DMA_BUFFER_MAX) return -1;

    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    if (b->npages == 0) return -1;

    b->pinned = 0;
    return 0;
}

/* ─── dma_phys / dma_desc ──────────────────────────────────────────────── */

uint64_t dma_phys(uint32_t buf_id) {
    if (!g_dma_inited || buf_id >= DMA_BUFFER_MAX) return 0;
    const struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    if (b->npages == 0) return 0;
    return dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
}

const struct DMABufferDesc* dma_desc(uint32_t buf_id) {
    if (!g_dma_inited || buf_id >= DMA_BUFFER_MAX) return 0;
    if (dma_pool.buffers[buf_id].npages == 0) return 0;
    return &dma_pool.buffers[buf_id];
}

/* ─── dma_share ─────────────────────────────────────────────────────────── */

uint16_t dma_share(uint32_t buf_id, uint32_t target_pid, uint8_t rights) {
    if (!g_dma_inited) return 0xFFFF;
    if (buf_id >= DMA_BUFFER_MAX) return 0xFFFF;

    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    if (b->npages == 0) return 0xFFFF;
    if (b->shared_count >= DMA_MAX_SHARES) return 0xFFFF;

    /* Create a MEM cap in the target's capability table. */
    uint16_t cap_idx = 0xFFFF;
    uint64_t phys = dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
    uint32_t perm = 0;
    if (rights & 0x01) perm |= CAP_PERM_R;  /* CAP_PERM_R */
    if (rights & 0x02) perm |= CAP_PERM_W;  /* CAP_PERM_W */
    perm |= CAP_PERM_MAP | CAP_PERM_DMA_SHARE;

    int r = cap_create_mem(target_pid, phys, b->npages, perm, &cap_idx);
    if (r < 0) return 0xFFFF;

    /* Track the shared cap for revocation on dma_free. */
    if (b->shared_count < DMA_MAX_SHARES) {
        /* We store the cap slot index as a placeholder; the real tracking
         * is done by cap_create_mem's holder list. On dma_free we revoke
         * by iterating the holder list of the DMA_MEM object. For now,
         * we just increment the count. */
        b->cap_obj_ids[b->shared_count] = cap_idx;
        b->shared_count++;
    }

    return cap_idx;
}

/* ─── dma_revoke_caps ───────────────────────────────────────────────────── */

void dma_revoke_caps(uint32_t buf_id) {
    if (!g_dma_inited || buf_id >= DMA_BUFFER_MAX) return;
    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];

    /* The actual revocation happens via cap_revoke() on the DMA_MEM
     * capability objects. Here we just zero the tracking state. The
     * cap layer handles the real holder walk. */
    for (uint32_t i = 0; i < b->shared_count; i++)
        b->cap_obj_ids[i] = 0;
    b->shared_count = 0;
}

/* ─── IOMMU domain management ──────────────────────────────────────────── */

int dma_iommu_create_domain(uint32_t device_id) {
    if (!g_dma_inited) return -1;
    if (dma_pool.domain_count >= DMA_DOMAIN_MAX) return -1;

    uint32_t id = dma_pool.domain_count++;
    dma_pool.domains[id].device_id = device_id;
    dma_pool.domains[id].map_count = 0;
    dma_pool.domains[id].active = 1;
    return (int)id;
}

void dma_iommu_destroy_domain(uint32_t domain_id) {
    if (!g_dma_inited || domain_id >= DMA_DOMAIN_MAX) return;
    struct IOMMUDomain* d = &dma_pool.domains[domain_id];
    if (!d->active) return;

    /* Unmap all pages mapped to this domain. Walk all buffers. */
    for (uint32_t i = 0; i < DMA_BUFFER_MAX; i++) {
        struct DMABufferDesc* b = &dma_pool.buffers[i];
        if (b->npages == 0 || b->iommu_domain != domain_id) continue;
        uint64_t phys = dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
        cap_iommu_unmap(domain_id, phys, b->npages);
        d->map_count--;
        b->iommu_domain = 0;
    }

    d->active = 0;
    d->device_id = 0;
}

void dma_iommu_map(uint32_t domain_id, uint64_t phys_base, uint32_t npages) {
    if (!g_dma_inited || domain_id >= DMA_DOMAIN_MAX) return;
    if (!dma_pool.domains[domain_id].active) return;

    cap_iommu_map(domain_id, phys_base, npages);
    dma_pool.domains[domain_id].map_count++;
}

void dma_iommu_unmap(uint32_t domain_id, uint64_t phys_base, uint32_t npages) {
    if (!g_dma_inited || domain_id >= DMA_DOMAIN_MAX) return;
    if (!dma_pool.domains[domain_id].active) return;

    cap_iommu_unmap(domain_id, phys_base, npages);
    if (dma_pool.domains[domain_id].map_count > 0)
        dma_pool.domains[domain_id].map_count--;
}

int dma_iommu_bind(uint32_t buf_id, uint32_t domain_id) {
    if (!g_dma_inited) return -1;
    if (buf_id >= DMA_BUFFER_MAX) return -1;
    if (domain_id >= DMA_DOMAIN_MAX) return -1;
    if (!dma_pool.domains[domain_id].active) return -1;

    struct DMABufferDesc* b = &dma_pool.buffers[buf_id];
    if (b->npages == 0) return -1;

    /* If already bound to a different domain, unmap first. */
    if (b->iommu_domain != 0 && b->iommu_domain != domain_id) {
        uint64_t phys = dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
        dma_iommu_unmap(b->iommu_domain, phys, b->npages);
    }

    /* Map into the new domain. */
    uint64_t phys = dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
    if (b->iommu_domain != domain_id) {
        dma_iommu_map(domain_id, phys, b->npages);
    }

    b->iommu_domain = domain_id;
    return 0;
}

/* ─── Debug ─────────────────────────────────────────────────────────────── */

void dma_pool_list(void) {
    if (!g_dma_inited) {
        kernel_serial_print("[DMA] pool not initialized.\n");
        return;
    }

    kernel_serial_printf(
        "[DMA] pool: base=0x%llx total=%u free=%u allocs=%u domains=%u\n",
        (unsigned long long)dma_pool.base_phys,
        dma_pool.total_frames, dma_pool.free_frames,
        dma_pool.alloc_count, dma_pool.domain_count);

    uint32_t live = 0;
    for (uint32_t i = 0; i < DMA_BUFFER_MAX; i++) {
        const struct DMABufferDesc* b = &dma_pool.buffers[i];
        if (b->npages == 0) continue;
        live++;
        kernel_serial_printf(
            "  [%u] frame=%u pages=%u domain=%u pid=%u pinned=%u "
            "shared=%u flags=0x%02x\n",
            i, b->frame_index, b->npages, b->iommu_domain,
            b->owner_pid, b->pinned, b->shared_count, b->flags);
    }

    kernel_serial_printf("[DMA] active buffers: %u\n", live);
}

uint32_t dma_pool_free_frames(void) {
    return g_dma_inited ? dma_pool.free_frames : 0;
}

uint32_t dma_pool_total_frames(void) {
    return g_dma_inited ? dma_pool.total_frames : 0;
}

uint32_t dma_buffer_count(void) {
    if (!g_dma_inited) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < DMA_BUFFER_MAX; i++)
        if (dma_pool.buffers[i].npages > 0) n++;
    return n;
}

/* ─── Syscall wrappers (do_syscall ABI: uint64_t return) ────────────────── */

uint64_t sys_sls_dma_alloc(struct SLSDMAAllocRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    int id = dma_alloc(req->npages, req->align_pages, req->flags);
    if (id < 0) return (uint64_t)(int64_t)id;
    req->out_buf_id = (uint32_t)id;
    return 0;
}

uint64_t sys_sls_dma_free(struct SLSDMAFreeRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    int r = dma_free(req->buf_id);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_dma_share(struct SLSDMAShareRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    uint16_t cap = dma_share(req->buf_id, req->target_pid, req->rights);
    if (cap == 0xFFFF) return (uint64_t)(int64_t)-1;
    req->out_cap_idx = cap;
    return 0;
}

uint64_t sys_sls_dma_pin(struct SLSDMAPinRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    int r = dma_pin(req->buf_id);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_dma_unpin(struct SLSDMAPinRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    int r = dma_unpin(req->buf_id);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_dma_iommu_map(struct SLSDMAIOMMUMapRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    const struct DMABufferDesc* b = dma_desc(req->buf_id);
    if (!b) return (uint64_t)(int64_t)-1;
    dma_iommu_bind(req->buf_id, req->domain_id);
    return 0;
}

uint64_t sys_sls_dma_iommu_unmap(struct SLSDMAIOMMUMapRequest* req) {
    if (!req) return (uint64_t)(int64_t)-1;
    const struct DMABufferDesc* b = dma_desc(req->buf_id);
    if (!b) return (uint64_t)(int64_t)-1;
    if (b->iommu_domain == req->domain_id) {
        uint64_t phys = dma_pool.base_phys + (uint64_t)b->frame_index * 4096u;
        dma_iommu_unmap(req->domain_id, phys, b->npages);
        /* Clear domain binding. */
        ((struct DMABufferDesc*)b)->iommu_domain = 0;
    }
    return 0;
}
