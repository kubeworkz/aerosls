/*
 * cap.c — AeroSLS Seed Kernel capability layer (Phase 1).
 *
 * Design: docs/AeroSLS-Capability-SDK-Phase1-Seed-Kernel-Design-v0.1.md.
 *
 * What this file proves (the Seed Kernel's acceptance test):
 *   create → transfer → map → write/read → revoke, with refcount and
 *   arena ownership returning to zero — no leaks, no aliasing, no forged
 *   capability ever accepted.
 *
 * Concurrency model (lock order: table < channel < object):
 *   - cap_send:  sender table → channel → object
 *   - cap_recv:  receiver's OWN table → channel → object
 *   - cap_map:   own table → object
 *   - cap_revoke: object (snapshot + set revoking, stamp queued words,
 *                 unlink ALL holders, release) → per-holder table locks one
 *                 at a time (never two at once, never under the object
 *                 lock) → object again for the final refcount check/free
 *   - No blocking while holding a lock; locked sections are validation +
 *     word/queue writes only.
 *
 * Safety invariants (double-free / use-after-free exclusion):
 *   1. A slot is FREE (type NONE), VALID, or IN_TRANSIT; only its table's
 *      lock changes it.
 *   2. refcount == number of VALID holders (VALID slots + VALID queued
 *      words), mutated only under the object lock.
 *   3. Object structs and object ids are MONOTONIC and never recycled in
 *      Phase 1: a stale word can never alias a recycled object, because the
 *      object it names either still exists (validated via revoking/active)
 *      or is a dead id whose struct is retained and inactive. Object slots
 *      are therefore never freed back to a freelist; the 1024-object
 *      ceiling is documented Phase-1 scope (Phase 2 adds reclamation).
 *   4. Revocation sets obj->revoking under the object lock and stamps
 *      queued words REVOKED under that same lock. Every path that could
 *      create a NEW reference to the object (send, recv install, map)
 *      checks revoking / the stamp under the object lock, so between the
 *      linearization point and the final free no new reference can exist.
 *
 * Implementation deltas from the v0.1 design doc (recorded in the doc's
 * own addendum): chan_create mints four caps (both ends), the receiver's
 * slot is installed at cap_recv time rather than pre-installed by the
 * sender (single-install protocol), and blocking recv is a user-space SDK
 * concern in Phase 1 (the kernel recv is non-blocking). Phase 1.5 adds
 * REAL kernel-side blocking: an empty recv with block=1 hands off to
 * process.c's cap_wait_chan() (weak hook here), which parks the process
 * mid-syscall; a later send wakes it. The weak default keeps the Phase-1
 * behavior (CAP_EAGAIN) so host tests run without the scheduler.
 */
#include "cap.h"
#include "kernel_io.h"
#include "frame_pool.h"
#include <stddef.h>

/* ─── Phase 1.5: blocking-recv park/wake (weak defaults) ─────────────────────
 * Strong overrides live in process.c (real kernel) and can be overridden
 * by host tests. cap.c additionally stashes the recv request pointer here
 * before calling cap_recv() so the park path can hand it to
 * cap_wait_chan() — the resume re-runs the same syscall with the same
 * user-space request struct. Single CPU, one syscall at a time: a plain
 * static is safe. */
static void* g_recv_park_req = 0;

__attribute__((weak))
int cap_wait_chan(uint32_t chan_id, void* recv_req) {
    (void)chan_id;
    (void)recv_req;
    return 0;   /* cannot park: caller returns CAP_EAGAIN (Phase-1 behavior) */
}

__attribute__((weak))
void cap_wake_chan(uint32_t chan_id) { (void)chan_id; }

/* Phase 1.5 (immediate wake): weak default — the real kernel (process.c)
 * hands the CPU to the just-woken process right after cap_send's wake, so
 * the receiver runs before the sender's send returns to ring-3. The weak
 * default is a no-op: host tests (and the kernel shell, which sends in
 * kernel context with no process to hand off FROM) keep the Phase-1.5
 * "wake on send, next schedule" behavior. */
__attribute__((weak))
void cap_maybe_handoff(void) { }

/* ─── Static state ─────────────────────────────────────────────────────────── */

#define CAP_FREELIST_END 0xFFFF

/* Provided by arch/x86/linker.ld — the end of the loaded kernel image,
 * used by cap_create_mem() to refuse ranges overlapping the kernel. */
extern char _kernel_image_end[];

struct CapTable cap_tables[CAP_TABLE_MAX];

/* pid → table binding. cap_table_pid[0] is permanently 0 (the kernel /
 * shell context table); real pids start at 100 in this kernel so there is
 * no collision. Tables bind lazily on first use by a pid. */
static uint32_t cap_table_pid[CAP_TABLE_MAX];

struct CapObject cap_objects[CAP_OBJECT_MAX];
static uint32_t  cap_obj_next;          /* next monotonic object id (= struct index) */

struct CapHolder cap_holders[CAP_HOLDER_MAX];
static uint16_t  cap_holder_free_head;  /* freelist chained via .next */

struct CapChannel cap_channels[CAP_CHAN_MAX];
static uint32_t   cap_chan_next;        /* next monotonic channel slot */

/* Shared-memory arena: a physically contiguous pool carved from the frame
 * pool at boot. One frame, one object — no aliasing is possible. */
static uint64_t cap_arena_base = 0;
static uint8_t  cap_arena_bitmap[(CAP_ARENA_FRAMES + 7) / 8];
static uint32_t cap_arena_owner[CAP_ARENA_FRAMES];   /* owning object id, 0 = free */
static uint32_t cap_arena_free_count = 0;

/* Phase 3 message payload staging pool. Fixed-size buffers, chained through
 * a parallel `next` array (the payload bytes themselves are arbitrary data,
 * so the freelist cannot chain through them like the holder pool does).
 * Single CPU, one syscall at a time: a plain static freelist is safe. */
static uint8_t  cap_msg_payload[CAP_MSG_PAYLOAD_POOL][CAP_MSG_MAX_PAYLOAD];
static uint16_t cap_msg_payload_next[CAP_MSG_PAYLOAD_POOL];
static uint16_t cap_msg_payload_free_head = CAP_FREELIST_END;

static void cap_msg_payload_init(void) {
    cap_msg_payload_free_head = 0;
    for (int i = 0; i < CAP_MSG_PAYLOAD_POOL - 1; i++)
        cap_msg_payload_next[i] = (uint16_t)(i + 1);
    cap_msg_payload_next[CAP_MSG_PAYLOAD_POOL - 1] = CAP_FREELIST_END;
}

static int cap_msg_payload_alloc(uint16_t* out_idx) {
    if (cap_msg_payload_free_head == CAP_FREELIST_END) return -1;
    uint16_t i = cap_msg_payload_free_head;
    cap_msg_payload_free_head = cap_msg_payload_next[i];
    *out_idx = i;
    return 0;
}

static void cap_msg_payload_free(uint16_t idx) {
    if (idx == CAP_NONE || idx >= CAP_MSG_PAYLOAD_POOL) return;
    cap_msg_payload_next[idx] = cap_msg_payload_free_head;
    cap_msg_payload_free_head = idx;
}

/* ─── Spinlock ─────────────────────────────────────────────────────────────── */

void cap_lock_init(struct CapSpinlock* l) { l->v = 0; }

static void cap_lock(struct CapSpinlock* l) {
    while (__atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE)) { }
}

static void cap_unlock(struct CapSpinlock* l) {
    __atomic_store_n(&l->v, 0u, __ATOMIC_RELEASE);
}

/* ─── Capability word helpers ──────────────────────────────────────────────── */

static uint64_t cap_word_make(uint32_t type, uint32_t obj, uint32_t perms,
                              uint32_t off, uint32_t len) {
    return (((uint64_t)type  & CAP_TYPE_MASK)  << CAP_TYPE_SHIFT) |
           (((uint64_t)obj   & CAP_OBJ_MASK)   << CAP_OBJ_SHIFT)  |
           (((uint64_t)perms & CAP_PERM_MASK)  << CAP_PERM_SHIFT) |
           (((uint64_t)off   & CAP_OFF_MASK)   << CAP_OFF_SHIFT)  |
           (((uint64_t)len   & CAP_LEN_MASK)   << CAP_LEN_SHIFT);
}

/* 1 if the word is a usable capability: real type, VALID state, reserved
 * bits zero (the forgery tag check). */
static int cap_word_valid(uint64_t w) {
    uint32_t type = (uint32_t)((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK);
    uint32_t state = (uint32_t)((w >> CAP_STATE_SHIFT) & CAP_STATE_MASK);
    if (type == CAP_TYPE_NONE) return 0;
    if (state != CAP_STATE_VALID) return 0;
    if (w & CAP_RSVD_MASK) return 0;
    return 1;
}

static uint64_t cap_word_stamp_revoked(uint64_t w) {
    return (w & ~(CAP_STATE_MASK << CAP_STATE_SHIFT)) |
           ((uint64_t)CAP_STATE_REVOKED << CAP_STATE_SHIFT);
}

/* ─── Table lookup / slot freelist ─────────────────────────────────────────── */

/* Returns the table index for pid, binding lazily. Table 0 is the kernel
 * context (pid 0). -1 on table exhaustion (never, with PROC_MAX processes
 * and CAP_TABLE_MAX == PROC_MAX + kernel slot). */
static int cap_table_index(uint32_t pid) {
    if (pid == 0) return 0;
    for (int i = 0; i < CAP_TABLE_MAX; i++)
        if (cap_table_pid[i] == pid) return i;
    for (int i = 1; i < CAP_TABLE_MAX; i++) {
        if (cap_table_pid[i] == 0) {
            cap_table_pid[i] = pid;
            cap_tables[i].pid = pid;
            return i;
        }
    }
    return -1;
}

/* Pure lookup — never binds. Phase 2 teardown must NOT create a table for
 * a pid that never used capabilities (that would transiently steal one of
 * the 15 process table slots for a dead pid, then unbind it). */
static int cap_table_find(uint32_t pid) {
    if (pid == 0) return 0;   /* kernel context table — never torn down */
    for (int i = 0; i < CAP_TABLE_MAX; i++)
        if (cap_table_pid[i] == pid) return i;
    return -1;
}

static void cap_table_freelist_init(uint32_t ti) {
    struct CapTable* t = &cap_tables[ti];
    t->freelist_head = CAP_FREELIST_END;
    for (int i = CAP_TABLE_ENTRIES - 1; i >= 0; i--) {
        t->slots[i].word =
            ((uint64_t)((t->freelist_head == CAP_FREELIST_END)
                        ? CAP_FREELIST_END : t->freelist_head)) << CAP_OBJ_SHIFT;
        t->freelist_head = (uint16_t)i;
    }
}

static void cap_slot_push(uint32_t ti, uint16_t idx) {
    struct CapTable* t = &cap_tables[ti];
    t->slots[idx].word = ((uint64_t)((t->freelist_head == CAP_FREELIST_END)
                                     ? CAP_FREELIST_END : t->freelist_head))
                         << CAP_OBJ_SHIFT;
    t->freelist_head = idx;
}

static int cap_slot_pop(uint32_t ti, uint16_t* out) {
    struct CapTable* t = &cap_tables[ti];
    if (t->freelist_head == CAP_FREELIST_END) return -1;
    uint16_t idx = t->freelist_head;
    uint32_t next = (uint32_t)((t->slots[idx].word >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    t->freelist_head = (next == CAP_FREELIST_END) ? CAP_FREELIST_END : (uint16_t)next;
    t->slots[idx].word = 0;
    *out = idx;
    return 0;
}

/* ─── Object / holder / channel allocation ─────────────────────────────────── */

static struct CapObject* cap_object_get(uint32_t obj_id) {
    if (obj_id >= CAP_OBJECT_MAX) return 0;
    struct CapObject* o = &cap_objects[obj_id];
    if (!o->active || o->id != obj_id) return 0;
    return o;
}

static int cap_object_alloc(uint8_t kind, uint32_t* out_id) {
    if (cap_obj_next >= CAP_OBJECT_MAX) return -1;   /* Phase 1: no reclamation */
    uint32_t id = cap_obj_next++;
    struct CapObject* o = &cap_objects[id];
    cap_lock_init(&o->lock);
    o->id = id;
    o->kind = kind;
    o->revoking = 0;
    o->active = 1;
    o->holder_head = CAP_FREELIST_END;
    o->refcount = 0;
    o->max_perms = 0;
    o->phys_base = 0;
    o->npages = 0;
    o->chan_id = 0;
    *out_id = id;
    return 0;
}

static void cap_object_destroy(uint32_t obj_id) {
    struct CapObject* o = &cap_objects[obj_id];
    o->active = 0;
    o->revoking = 1;
    o->holder_head = CAP_FREELIST_END;
    o->refcount = 0;
}

static int cap_holder_alloc(uint32_t obj_id, uint8_t kind, uint16_t pid,
                            uint16_t ref, uint8_t qdir, uint16_t* out) {
    if (cap_holder_free_head == CAP_FREELIST_END) return -1;
    uint16_t h = cap_holder_free_head;
    struct CapHolder* node = &cap_holders[h];
    cap_holder_free_head = node->next;
    node->obj_id = obj_id;
    node->kind = kind;
    node->pid = pid;
    node->ref = ref;
    node->qdir = qdir;
    node->active = 1;
    node->next = CAP_FREELIST_END;
    node->prev = CAP_FREELIST_END;
    *out = h;
    return 0;
}

static void cap_holder_free(uint16_t h) {
    struct CapHolder* node = &cap_holders[h];
    node->active = 0;
    node->next = cap_holder_free_head;
    cap_holder_free_head = h;
}

/* Link a PRE-ALLOCATED holder node (cap_holder_alloc) onto the object's
 * list. Cannot fail — used by the transfer paths, which pre-reserve the
 * node so a pool-exhaustion can never leave a half-moved holder. */
static void cap_holder_link(uint16_t h, struct CapObject* o) {
    struct CapHolder* node = &cap_holders[h];
    node->next = o->holder_head;
    if (o->holder_head != CAP_FREELIST_END)
        cap_holders[o->holder_head].prev = h;
    o->holder_head = h;
    o->refcount++;
}

static int cap_holder_insert(uint32_t obj_id, uint8_t kind, uint16_t pid,
                             uint16_t ref, uint8_t qdir) {
    struct CapObject* o = &cap_objects[obj_id];
    uint16_t h;
    if (cap_holder_alloc(obj_id, kind, pid, ref, qdir, &h)) return -1;
    cap_holder_link(h, o);
    return 0;
}

static void cap_holder_remove(uint32_t obj_id, uint8_t kind, uint16_t pid,
                              uint16_t ref) {
    struct CapObject* o = &cap_objects[obj_id];
    uint16_t h = o->holder_head;
    while (h != CAP_FREELIST_END) {
        struct CapHolder* node = &cap_holders[h];
        if (node->kind == kind && node->pid == pid && node->ref == ref) {
            if (node->prev != CAP_FREELIST_END)
                cap_holders[node->prev].next = node->next;
            else
                o->holder_head = node->next;
            if (node->next != CAP_FREELIST_END)
                cap_holders[node->next].prev = node->prev;
            cap_holder_free(h);
            if (o->refcount > 0) o->refcount--;
            return;
        }
        h = node->next;
    }
    /* Not found: invariant violation. Phase 1 is single-threaded enough
     * that this is a programming error, not a race; stay silent rather
     * than corrupt anything further (the caller still holds its locks). */
}

/* ─── Arena ────────────────────────────────────────────────────────────────── */

static void cap_arena_init(void) {
    /* Carve CAP_ARENA_SIZE of physically contiguous free RAM, 2 MiB-aligned
     * (512 frames) so a later phase can use huge pages. The frames are
     * marked reserved in the frame-pool bitmap AND folded into
     * reserved_below, so no partition reclamation can ever free them. */
    cap_arena_base = frame_pool_reserve_contiguous(CAP_ARENA_FRAMES, 512);
    if (cap_arena_base == 0) {
        kernel_serial_print("[CAP] *** arena carve failed: no contiguous "
                            "64 MiB run; cap_arena_alloc() disabled. ***\n");
        return;
    }
    cap_arena_free_count = CAP_ARENA_FRAMES;
    kernel_serial_printf(
        "[CAP] shared-memory arena: %u MiB physically contiguous at 0x%llx "
        "(%u frames).\n",
        (unsigned)(CAP_ARENA_SIZE >> 20), (unsigned long long)cap_arena_base,
        (unsigned)CAP_ARENA_FRAMES);
}

static int cap_arena_frame_free(uint32_t f) {
    return (cap_arena_bitmap[f >> 3] & (1u << (f & 7))) == 0;
}

/* Phase 2 teardown: is paddr inside the shared-memory arena? Exposed so the
 * per-process page-table teardown (user_destroy_page_table) can refuse to
 * free arena frames a stale cap-map PTE might still point at. Precise where
 * frame_pool_frame_is_machine_owned() is not: the arena carve folds into
 * the reserved_below watermark, which also covers every allocatable frame
 * BELOW the arena (the pool hands those out to processes) — so that check
 * wrongly reports live process frames as machine state. The arena itself is
 * the only boot-reserved region that can appear in a user slot. */
int cap_frame_in_arena(uint64_t paddr) {
    return cap_arena_base != 0 && paddr >= cap_arena_base &&
           paddr < cap_arena_base + CAP_ARENA_SIZE;
}

static void cap_arena_frame_mark(uint32_t f, uint32_t owner) {
    cap_arena_bitmap[f >> 3] |= (1u << (f & 7));
    cap_arena_owner[f] = owner;
}

static void cap_arena_frame_clear(uint32_t f) {
    cap_arena_bitmap[f >> 3] &= ~(1u << (f & 7));
    cap_arena_owner[f] = 0;
    cap_arena_free_count++;
}

/* First-fit run of npages consecutive free frames; marks them owned and
 * returns the frame index, or 0xFFFFFFFF on failure (nothing mutated). */
static uint32_t cap_arena_alloc_run(uint32_t npages, uint32_t owner) {
    if (npages == 0 || npages > CAP_ARENA_FRAMES) return 0xFFFFFFFFu;
    for (uint32_t start = 0; start + npages <= CAP_ARENA_FRAMES; start++) {
        int ok = 1;
        for (uint32_t i = 0; i < npages; i++) {
            if (!cap_arena_frame_free(start + i)) { ok = 0; break; }
        }
        if (!ok) continue;
        for (uint32_t i = 0; i < npages; i++)
            cap_arena_frame_mark(start + i, owner);
        cap_arena_free_count -= npages;
        return start;
    }
    return 0xFFFFFFFFu;
}

/* ─── Mapping helpers ──────────────────────────────────────────────────────── */

/* Unmap every mapping of obj_id in table ti (used by revoke). */
static void cap_table_unmap_object(uint32_t ti, uint32_t obj_id) {
    struct CapTable* t = &cap_tables[ti];
    uint64_t cr3 = cap_proc_cr3(t->pid);
    for (int i = 0; i < CAP_MAP_MAX; i++) {
        struct CapMap* m = &t->maps[i];
        if (!m->active || m->obj_id != obj_id) continue;
        for (uint32_t p = 0; p < m->npages; p++)
            cap_arch_unmap_page(cr3, m->vaddr + (uint64_t)p * 4096u);
        m->active = 0;
    }
    if (cr3) cap_arch_tlb_flush();
}

static int cap_maps_overlap(const struct CapTable* t, uint32_t except_obj,
                            uint64_t vaddr, uint32_t npages) {
    uint64_t lo = vaddr;
    uint64_t hi = vaddr + (uint64_t)npages * 4096u;
    for (int i = 0; i < CAP_MAP_MAX; i++) {
        const struct CapMap* m = &t->maps[i];
        if (!m->active || m->obj_id == except_obj) continue;
        uint64_t mlo = m->vaddr;
        uint64_t mhi = m->vaddr + (uint64_t)m->npages * 4096u;
        if (lo < mhi && mlo < hi) return 1;
    }
    return 0;
}

/* ─── Weak hooks (strong definitions: kernel/process.c, arch/x86/user_paging.c) ─ */

__attribute__((weak)) uint32_t cap_current_pid(void) { return 0; }
__attribute__((weak)) uint64_t cap_proc_cr3(uint32_t pid) { (void)pid; return 0; }

/* Phase 1.5 (immediate wake): release a HELD process just before a blocking
 * recv parks, so the released process runs while the receiver is parked —
 * with NO user-mode window between the release and the park (a timer tick
 * there would schedule the child first and the send would find nobody
 * parked). Strong override in kernel/process.c calls process_release(); the
 * weak default lets cap.c host-test in isolation. Errors are ignored: the
 * re-run of a parked recv re-releases an already-released pid (harmless). */
__attribute__((weak)) int cap_release_pid(uint32_t pid) {
    (void)pid;
    return 0;
}

__attribute__((weak)) int cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr,
                                            uint64_t paddr, uint32_t cap_perms) {
    (void)pml4_phys; (void)vaddr; (void)paddr; (void)cap_perms;
    return (int)CAP_ENOSYS;
}

__attribute__((weak)) int cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    (void)pml4_phys; (void)vaddr;
    return (int)CAP_ENOSYS;
}

__attribute__((weak)) void cap_arch_tlb_flush(void) { }

/* ─── Init ─────────────────────────────────────────────────────────────────── */

void cap_init(void) {
    for (int i = 0; i < CAP_TABLE_MAX; i++) {
        cap_lock_init(&cap_tables[i].lock);
        cap_table_pid[i] = 0;
        cap_tables[i].pid = 0;
        cap_table_freelist_init(i);
    }
    for (int i = 0; i < CAP_HOLDER_MAX; i++) {
        cap_holders[i].active = 0;
        cap_holders[i].next = (uint16_t)(i + 1);
    }
    cap_holder_free_head = 0;
    cap_holders[CAP_HOLDER_MAX - 1].next = CAP_FREELIST_END;

    cap_obj_next = 0;
    cap_chan_next = 0;
    cap_arena_init();
    cap_msg_payload_init();

    kernel_serial_print("[CAP] Seed kernel capability layer online: "
                        "512-slot tables, 1024-object ceiling, holders, "
                        "channels, arena, message transport, syscalls 289-304.\n");
}

/* ─── cap_create_mem ───────────────────────────────────────────────────────── */
/* Wraps an EXISTING physical range in a MEM capability. The arena is the
 * preferred source (cap_arena_alloc); this path exists for ranges the
 * kernel knows about and is deliberately strict: page-aligned, within the
 * identity map, disjoint from the kernel image, from the arena, and from
 * every existing MEM object (no aliasing). */
int cap_create_mem(uint32_t pid, uint64_t phys_base, uint32_t npages,
                   uint32_t perm, uint16_t* out_idx) {
    if (out_idx) *out_idx = CAP_NONE;
    if (!out_idx || npages == 0 || npages > CAP_VIEW_MAX_PAGES)
        return CAP_EINVAL;
    if (perm == 0 || (perm & ~(CAP_PERM_R | CAP_PERM_W | CAP_PERM_X | CAP_PERM_MAP)))
        return CAP_EINVAL;
    if ((phys_base & 0xFFFULL) != 0) return CAP_EINVAL;
    uint64_t end = phys_base + (uint64_t)npages * 4096u;
    if (end <= phys_base || end > 0x100000000ULL) return CAP_ERANGE;   /* 4 GiB identity map */
    /* Must not touch machine-reserved low memory or the kernel image
     * [0x100000, _kernel_image_end) — both are machine state. */
    uint64_t kimg_end = (uint64_t)(uintptr_t)_kernel_image_end;
    if (phys_base < 0x100000ULL ||
        (phys_base < kimg_end && end > 0x100000ULL))
        return CAP_ECONFLICT;
    /* Must not overlap the arena (arena memory is managed by arena_alloc). */
    if (cap_arena_base != 0 &&
        phys_base < cap_arena_base + CAP_ARENA_SIZE && end > cap_arena_base)
        return CAP_ECONFLICT;
    /* Must not overlap any existing MEM object. */
    for (uint32_t i = 0; i < CAP_OBJECT_MAX; i++) {
        const struct CapObject* o = &cap_objects[i];
        if (!o->active || o->kind != CAP_OBJ_KIND_MEM) continue;
        uint64_t oend = o->phys_base + (uint64_t)o->npages * 4096u;
        if (phys_base < oend && end > o->phys_base) return CAP_ECONFLICT;
    }

    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;

    uint32_t obj_id;
    if (cap_object_alloc(CAP_OBJ_KIND_MEM, &obj_id)) return CAP_ENOMEM;
    struct CapObject* o = &cap_objects[obj_id];
    o->max_perms = perm;
    o->phys_base = phys_base;
    o->npages = npages;

    cap_lock(&cap_tables[ti].lock);
    uint16_t idx;
    if (cap_slot_pop(ti, &idx)) {
        cap_unlock(&cap_tables[ti].lock);
        cap_object_destroy(obj_id);
        return CAP_ETABLEFULL;
    }
    cap_lock(&o->lock);
    if (cap_holder_insert(obj_id, HOLDER_SLOT, (uint16_t)pid, idx, 0)) {
        cap_slot_push(ti, idx);
        cap_unlock(&o->lock);
        cap_unlock(&cap_tables[ti].lock);
        cap_object_destroy(obj_id);
        return CAP_ENOMEM;
    }
    cap_tables[ti].slots[idx].word =
        cap_word_make(CAP_TYPE_MEM, obj_id, perm, 0, npages);
    cap_unlock(&o->lock);
    cap_unlock(&cap_tables[ti].lock);

    *out_idx = idx;
    return 0;
}

/* ─── cap_arena_alloc ──────────────────────────────────────────────────────── */
/* The arena path: allocates a contiguous run of physically contiguous pages
 * from the shared-memory arena and mints a MEM cap over the whole run in
 * one atomic step — there is never a window where the frames exist but no
 * object owns them. */
int cap_arena_alloc(uint32_t pid, uint32_t npages, uint32_t perm,
                    uint16_t* out_idx) {
    if (out_idx) *out_idx = CAP_NONE;
    if (!out_idx || npages == 0 || npages > CAP_VIEW_MAX_PAGES)
        return CAP_EINVAL;
    if (perm == 0 || (perm & ~(CAP_PERM_R | CAP_PERM_W | CAP_PERM_X | CAP_PERM_MAP)))
        return CAP_EINVAL;
    if (cap_arena_base == 0) return CAP_ENOMEM;

    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;

    uint32_t obj_id;
    if (cap_object_alloc(CAP_OBJ_KIND_MEM, &obj_id)) return CAP_ENOMEM;
    struct CapObject* o = &cap_objects[obj_id];

    uint32_t start = cap_arena_alloc_run(npages, obj_id);
    if (start == 0xFFFFFFFFu) {
        cap_object_destroy(obj_id);
        return CAP_ENOSPC;
    }
    o->max_perms = perm;
    o->phys_base = cap_arena_base + (uint64_t)start * 4096u;
    o->npages = npages;

    cap_lock(&cap_tables[ti].lock);
    uint16_t idx;
    if (cap_slot_pop(ti, &idx)) {
        cap_unlock(&cap_tables[ti].lock);
        for (uint32_t i = 0; i < npages; i++)
            cap_arena_frame_clear(start + i);
        cap_object_destroy(obj_id);
        return CAP_ETABLEFULL;
    }
    cap_lock(&o->lock);
    if (cap_holder_insert(obj_id, HOLDER_SLOT, (uint16_t)pid, idx, 0)) {
        cap_slot_push(ti, idx);
        cap_unlock(&o->lock);
        cap_unlock(&cap_tables[ti].lock);
        for (uint32_t i = 0; i < npages; i++)
            cap_arena_frame_clear(start + i);
        cap_object_destroy(obj_id);
        return CAP_ENOMEM;
    }
    cap_tables[ti].slots[idx].word =
        cap_word_make(CAP_TYPE_MEM, obj_id, perm, 0, npages);
    cap_unlock(&o->lock);
    cap_unlock(&cap_tables[ti].lock);

    *out_idx = idx;
    return 0;
}

/* ─── cap_chan_create ──────────────────────────────────────────────────────── */
/* Provisions BOTH endpoints of a new bidirectional channel. With far_pid == 0
 * all four caps land in the caller's table (handoff by transfer over an
 * existing channel); with far_pid nonzero the far end's caps are minted
 * directly into that process's table — the concrete Phase-1 bootstrap, the
 * kernel doing what socket-pair creation does in a Unix kernel.
 *
 * Ordering matters for SMP: holders are inserted (under the object lock)
 * BEFORE any cap word is published to any table, and table locks are taken
 * one at a time (never two at once). A failed slot allocation rolls back
 * everything — no orphaned holders, no partially-minted channel. */
int cap_chan_create(uint32_t pid, uint32_t far_pid,
                    uint16_t* out_rd, uint16_t* out_wr,
                    uint16_t* out_far_rd, uint16_t* out_far_wr) {
    if (out_rd) *out_rd = CAP_NONE;
    if (out_wr) *out_wr = CAP_NONE;
    if (out_far_rd) *out_far_rd = CAP_NONE;
    if (out_far_wr) *out_far_wr = CAP_NONE;
    if (!out_rd || !out_wr || !out_far_rd || !out_far_wr) return CAP_EINVAL;

    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    int fti = (far_pid != 0 && far_pid != pid) ? cap_table_index(far_pid) : -1;
    if (far_pid != 0 && far_pid != pid && fti < 0) return CAP_ETABLEFULL;

    if (cap_chan_next >= CAP_CHAN_MAX) return CAP_ENOMEM;
    uint32_t chan_id = cap_chan_next++;
    struct CapChannel* ch = &cap_channels[chan_id];
    cap_lock_init(&ch->lock);
    ch->end0_pid = (uint16_t)pid;
    ch->active = 1;
    for (int d = 0; d < 2; d++) {
        ch->qhead[d] = 0;
        ch->qtail[d] = 0;
        ch->qdepth[d] = 0;
    }

    uint32_t obj_id;
    if (cap_object_alloc(CAP_OBJ_KIND_CHAN, &obj_id)) {
        ch->active = 0;
        return CAP_ENOMEM;
    }
    struct CapObject* o = &cap_objects[obj_id];
    o->chan_id = (uint16_t)chan_id;

    uint16_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
    uint16_t far_holder_pid = (far_pid != 0) ? (uint16_t)far_pid : (uint16_t)pid;

    /* Pre-allocate all four holder nodes BEFORE anything is visible, so
     * pool exhaustion can never leave a half-provisioned channel. The ref
     * fields are placeholders now and are updated in place once the real
     * slot indices are known (no remove/re-insert churn). */
    uint16_t h1 = CAP_FREELIST_END, h2 = CAP_FREELIST_END;
    uint16_t h3 = CAP_FREELIST_END, h4 = CAP_FREELIST_END;
    cap_lock(&o->lock);
    if (cap_holder_alloc(obj_id, HOLDER_SLOT, (uint16_t)pid, 0, 0, &h1) ||
        cap_holder_alloc(obj_id, HOLDER_SLOT, (uint16_t)pid, 1, 0, &h2) ||
        cap_holder_alloc(obj_id, HOLDER_SLOT, far_holder_pid, 2, 0, &h3) ||
        cap_holder_alloc(obj_id, HOLDER_SLOT, far_holder_pid, 3, 0, &h4)) {
        if (h1 != CAP_FREELIST_END) cap_holder_free(h1);
        if (h2 != CAP_FREELIST_END) cap_holder_free(h2);
        if (h3 != CAP_FREELIST_END) cap_holder_free(h3);
        if (h4 != CAP_FREELIST_END) cap_holder_free(h4);
        cap_unlock(&o->lock);
        cap_object_destroy(obj_id);
        ch->active = 0;
        return CAP_ENOMEM;
    }
    cap_holder_link(h1, o);
    cap_holder_link(h2, o);
    cap_holder_link(h3, o);
    cap_holder_link(h4, o);
    cap_unlock(&o->lock);

    /* Slots + words: far table first, then caller table — one table lock
     * at a time. Holder refs above are placeholder indices 0..3; they are
     * REPLACED below once the real slot indices are known. */
    int fail = 0;
    if (fti >= 0) {
        cap_lock(&cap_tables[fti].lock);
        if (cap_slot_pop(fti, &frd)) fail = 1;
        if (!fail && cap_slot_pop(fti, &fwr)) fail = 1;
        if (fail) {
            if (frd != CAP_NONE) cap_slot_push(fti, frd);
            cap_unlock(&cap_tables[fti].lock);
        } else {
            cap_tables[fti].slots[frd].word =
                cap_word_make(CAP_TYPE_CHAN_R, obj_id, CAP_PERM_RECV, 0, 1);
            cap_tables[fti].slots[fwr].word =
                cap_word_make(CAP_TYPE_CHAN_W, obj_id, CAP_PERM_SEND, 0, 1);
            cap_unlock(&cap_tables[fti].lock);
        }
    } else {
        /* far caps stay in the caller's table too */
        frd = CAP_NONE; fwr = CAP_NONE;
    }

    if (!fail) {
        cap_lock(&cap_tables[ti].lock);
        if (cap_slot_pop(ti, &rd)) fail = 1;
        if (!fail && cap_slot_pop(ti, &wr)) fail = 1;
        if (!fail && fti < 0) {
            if (cap_slot_pop(ti, &frd)) fail = 1;
            if (!fail && cap_slot_pop(ti, &fwr)) fail = 1;
        }
        if (fail) {
            if (rd != CAP_NONE) cap_slot_push(ti, rd);
            if (wr != CAP_NONE) cap_slot_push(ti, wr);
            if (fti < 0) {
                if (frd != CAP_NONE) cap_slot_push(ti, frd);
                if (fwr != CAP_NONE) cap_slot_push(ti, fwr);
            }
            cap_unlock(&cap_tables[ti].lock);
        } else {
            cap_tables[ti].slots[rd].word =
                cap_word_make(CAP_TYPE_CHAN_R, obj_id, CAP_PERM_RECV, 0, 1);
            cap_tables[ti].slots[wr].word =
                cap_word_make(CAP_TYPE_CHAN_W, obj_id, CAP_PERM_SEND, 0, 1);
            if (fti < 0) {
                cap_tables[ti].slots[frd].word =
                    cap_word_make(CAP_TYPE_CHAN_R, obj_id, CAP_PERM_RECV, 0, 1);
                cap_tables[ti].slots[fwr].word =
                    cap_word_make(CAP_TYPE_CHAN_W, obj_id, CAP_PERM_SEND, 0, 1);
            }
            cap_unlock(&cap_tables[ti].lock);
        }
    }

    if (fail) {
        /* Roll back: remove the placeholder holders, free the channel. */
        cap_lock(&o->lock);
        cap_holder_remove(obj_id, HOLDER_SLOT, (uint16_t)pid, 0);
        cap_holder_remove(obj_id, HOLDER_SLOT, (uint16_t)pid, 1);
        cap_holder_remove(obj_id, HOLDER_SLOT, far_holder_pid, 2);
        cap_holder_remove(obj_id, HOLDER_SLOT, far_holder_pid, 3);
        cap_unlock(&o->lock);
        cap_object_destroy(obj_id);
        ch->active = 0;
        return CAP_ETABLEFULL;
    }

    /* Stitch the real slot indices into the pre-allocated holder nodes. */
    cap_lock(&o->lock);
    cap_holders[h1].ref = rd;
    cap_holders[h2].ref = wr;
    cap_holders[h3].ref = frd;
    cap_holders[h4].ref = fwr;
    cap_unlock(&o->lock);

    *out_rd = rd;
    *out_wr = wr;
    *out_far_rd = frd;
    *out_far_wr = fwr;
    return 0;
}

/* Free a dead object's machine resources (arena frames / channel slot);
 * defined after cap_revoke. Forward-declared here because cap_recv's
 * defensive multi-cap drain and cap_arena_free both call it. */
static uint32_t cap_object_free_resources(uint32_t obj_id);

/* ─── cap_send ─────────────────────────────────────────────────────────────── */
/* MOVE a capability (or send a plain message) into the channel's directional
 * queue, atomically with respect to revocation:
 *   - under the sender's table lock: validate the CHAN_W and the payload cap;
 *   - under the channel lock: queue-space check (fail before any mutation);
 *   - under the object lock: check revoking, then move the holder from the
 *     sender's slot to the queue entry (refcount unchanged — the queued cap
 *     IS the holder);
 *   - publish the message, release locks. Queue-full / table-full leave
 *     everything unchanged: a failed send never loses a capability.
 */
int cap_send(uint32_t pid, uint16_t ch_w_idx, uint16_t cap_idx, uint64_t cookie) {
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);

    uint64_t w = t->slots[ch_w_idx].word;
    if (!cap_word_valid(w) ||
        ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_W ||
        !((w >> CAP_PERM_SHIFT) & CAP_PERM_SEND)) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    uint32_t chan_obj = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* cobj = cap_object_get(chan_obj);
    if (!cobj || cobj->kind != CAP_OBJ_KIND_CHAN) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    struct CapChannel* ch = &cap_channels[cobj->chan_id];
    if (!ch->active) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    /* My end → which queue? end0's CHAN_W feeds q[1]; anyone else (the far
     * end, which owns the end-1 write cap) feeds q[0]. */
    int dest = (pid == ch->end0_pid) ? 1 : 0;

    uint64_t payload = 0;
    struct CapObject* obj = 0;
    if (cap_idx != CAP_NONE) {
        uint64_t pw = t->slots[cap_idx].word;
        if (!cap_word_valid(pw)) {
            cap_unlock(&t->lock);
            return CAP_EINVAL;   /* free slot, IN_TRANSIT, REVOKED, or forged */
        }
        payload = pw;
        obj = cap_object_get((uint32_t)((pw >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK));
        if (!obj) {
            cap_unlock(&t->lock);
            return CAP_EINVAL;
        }
    }

    cap_lock(&ch->lock);
    if (ch->qdepth[dest] >= CHAN_QUEUE_DEPTH) {
        cap_unlock(&ch->lock);
        cap_unlock(&t->lock);
        return CAP_EAGAIN;
    }
    /* qtail is the write cursor, qhead the read cursor — a plain fixed-size
     * ring (same shape as ipc.c's queues). */
    uint32_t entry = ch->qtail[dest] % CHAN_QUEUE_DEPTH;
    /* Zero the whole entry: ring slots are reused, and cap_drain_queue /
     * cap_revoke scan cap_word[] for nonzero words, so a stale word from a
     * previous multi-cap message must never survive into a new message. */
    for (int i = 0; i < CAP_MSG_MAX_CAPS; i++) {
        ch->q[dest][entry].cap_word[i] = 0;
        ch->q[dest][entry].cap_off[i] = 0;
        ch->q[dest][entry].cap_len[i] = 0;
        ch->q[dest][entry].cap_rights[i] = 0;
        ch->q[dest][entry].cap_flags[i] = 0;
    }

    if (obj) {
        cap_lock(&obj->lock);
        if (obj->revoking) {
            /* Revoke won the race: the send aborts, the sender keeps its
             * cap (slot untouched), and revoke will zero it in its own
             * holder walk. */
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ECAPREVOKED;
        }
        /* Pre-reserve the queue holder node BEFORE mutating anything, so
         * pool exhaustion can never leave a half-moved holder (slot freed
         * but no queue holder to account for the cap). ref = entry*MAX + 0
         * keeps the encoding uniform with cap_send_msg's per-cap holders. */
        uint16_t hq;
        if (cap_holder_alloc(obj->id, HOLDER_QUEUE, (uint16_t)cobj->chan_id,
                             (uint16_t)(entry * CAP_MSG_MAX_CAPS), (uint8_t)dest, &hq)) {
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ENOMEM;   /* nothing mutated: sender keeps its cap */
        }
        /* Holder move: sender slot → queue entry. refcount unchanged. The holder's
     * `ref` encodes (entry, cap index) as entry*CAP_MSG_MAX_CAPS + i so
     * multi-cap messages can carry several holders per ring entry — see
     * cap_send_msg. A single-cap legacy message is just the i=0 case. */
        cap_holder_remove(obj->id, HOLDER_SLOT, (uint16_t)pid, cap_idx);
        t->slots[cap_idx].word = 0;
        cap_slot_push(ti, cap_idx);
        cap_holder_link(hq, obj);
        ch->q[dest][entry].cap_word[0] = payload;
        ch->q[dest][entry].n_caps = 1;
        cap_unlock(&obj->lock);
    } else {
        ch->q[dest][entry].n_caps = 0;
        ch->q[dest][entry].cap_word[0] = 0;
    }

    ch->q[dest][entry].cookie = cookie;
    ch->q[dest][entry].payload_len = 0;
    ch->q[dest][entry].payload_idx = CAP_NONE;
    ch->q[dest][entry].flags = 0;
    ch->qtail[dest]++;
    ch->qdepth[dest]++;
    cap_unlock(&ch->lock);
    cap_unlock(&t->lock);

    /* Phase 1.5: a process may be parked (blocked) on this channel's read
     * end waiting for exactly this message — make it runnable again. The
     * wake is a process-table op, deliberately outside the cap locks.
     * Phase 1.5 (immediate wake): then hand the CPU to the woken process
     * RIGHT NOW (seL4-style direct handoff) — if the sender is a Ring-3
     * process, it yields mid-syscall and the receiver runs before the
     * sender's send even returns to ring-3. No-op in kernel context (the
     * shell) and when nothing was woken. */
    cap_wake_chan(cobj->chan_id);
    cap_maybe_handoff();
    return 0;
}

/* ─── cap_msg_drain_tail ──────────────────────────────────────────────────── */
/* Drain the caps AFTER index 0 of a message whose first cap is being
 * skipped (stale word for a dead object, or revoked — in both cases cap 0's
 * holder was already accounted by destroy/revoke). Each remaining LIVE cap's
 * HOLDER_QUEUE node is removed from its object, the object is freed at
 * refcount 0 (its arena frames return), and the word is zeroed, so a
 * multi-cap message can never leave holders behind when it is dequeued.
 * Caller holds the table + channel locks; per-object locks are taken inside
 * (order table < channel < object, same as cap_recv). */
static void cap_msg_drain_tail(struct CapChannel* ch, uint16_t chan_id,
                               uint32_t entry, struct ChanMsg* m) {
    for (int i = 1; i < m->n_caps; i++) {
        if (m->cap_word[i] == 0) continue;
        uint32_t xid = (uint32_t)((m->cap_word[i] >> CAP_OBJ_SHIFT) &
                                  CAP_OBJ_MASK);
        struct CapObject* xo = cap_object_get(xid);
        if (xo) {
            cap_lock(&xo->lock);
            cap_holder_remove(xo->id, HOLDER_QUEUE, chan_id,
                              (uint16_t)(entry * CAP_MSG_MAX_CAPS + i));
            if (xo->refcount == 0) {
                cap_object_free_resources(xo->id);
                cap_object_destroy(xo->id);
            }
            cap_unlock(&xo->lock);
        }
        m->cap_word[i] = 0;
    }
}

/* ─── cap_recv ─────────────────────────────────────────────────────────────── */
/* Dequeue one message from the caller's read queue. If it carries a cap,
 * the holder moves from the queue entry to a fresh slot in the CALLER's
 * table (refcount unchanged). The queued word is re-read UNDER the object
 * lock, which is what serializes us against revoke's REVOKED stamp: a
 * revoked-in-flight cap is drained without being installed, exactly as if
 * it had been revoked before the send. Phase 1 recv is non-blocking: an
 * empty queue returns CAP_EAGAIN and the SDK's cap_recv_blocking() wrapper
 * retries in user space. */
int cap_recv(uint32_t pid, uint16_t ch_r_idx, int block,
             uint64_t* cookie_out, uint16_t* out_cap) {
    if (out_cap) *out_cap = CAP_NONE;
    if (!cookie_out || !out_cap) return CAP_EINVAL;

    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);

    uint64_t w = t->slots[ch_r_idx].word;
    if (!cap_word_valid(w) ||
        ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_R ||
        !((w >> CAP_PERM_SHIFT) & CAP_PERM_RECV)) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    uint32_t chan_obj = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* cobj = cap_object_get(chan_obj);
    if (!cobj || cobj->kind != CAP_OBJ_KIND_CHAN) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    struct CapChannel* ch = &cap_channels[cobj->chan_id];
    if (!ch->active) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    int dir = (pid == ch->end0_pid) ? 0 : 1;

    cap_lock(&ch->lock);
    if (ch->qdepth[dir] == 0) {
        cap_unlock(&ch->lock);
        cap_unlock(&t->lock);
        if (block) {
            /* Phase 1.5: park the calling process on this channel. Returns
             * only if it could NOT park (no runnable process) — then the
             * caller gets CAP_EAGAIN and the SDK retries. When it parks,
             * this call chain is abandoned and the resume re-runs the
             * syscall on wake (see process.c's cap_recv_resume). */
            cap_wait_chan(cobj->chan_id, g_recv_park_req);
        }
        return CAP_EAGAIN;
    }
    uint32_t entry = ch->qhead[dir] % CHAN_QUEUE_DEPTH;
    struct ChanMsg* m = &ch->q[dir][entry];

    if (m->n_caps > 0) {
        uint32_t obj_id = (uint32_t)((m->cap_word[0] >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
        struct CapObject* obj = cap_object_get(obj_id);
        if (!obj) {
            /* Stale word for a dead object: cap 0's holder is already gone
             * (the object was destroyed); drain any LIVE extra caps — a
             * multi-cap message must never leave holders behind — then
             * skip the message (defensive). */
            cap_msg_drain_tail(ch, cobj->chan_id, entry, m);
            m->n_caps = 0;
            m->payload_len = 0;
            cap_msg_payload_free(m->payload_idx);
            m->payload_idx = CAP_NONE;
            *cookie_out = m->cookie;
            ch->qhead[dir]++;
            ch->qdepth[dir]--;
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ECAPREVOKED;
        }
        cap_lock(&obj->lock);
        if (((m->cap_word[0] >> CAP_STATE_SHIFT) & CAP_STATE_MASK) == CAP_STATE_REVOKED ||
            obj->revoking) {
            /* Revoked in flight: cap 0's refcount was already accounted by
             * revoke (its holder is gone); drain any LIVE extra caps, then
             * skip the message — no double-count. */
            cap_msg_drain_tail(ch, cobj->chan_id, entry, m);
            m->n_caps = 0;
            m->payload_len = 0;
            cap_msg_payload_free(m->payload_idx);
            m->payload_idx = CAP_NONE;
            *cookie_out = m->cookie;
            ch->qhead[dir]++;
            ch->qdepth[dir]--;
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ECAPREVOKED;
        }
        uint16_t nslot;
        if (cap_slot_pop(ti, &nslot)) {
            /* Table full: the message STAYS queued; retry later. */
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ETABLEFULL;
        }
        /* Pre-reserve the slot holder node before the move, same
         * fail-before-mutate discipline as cap_send. */
        uint16_t hs;
        if (cap_holder_alloc(obj->id, HOLDER_SLOT, (uint16_t)pid, nslot, 0, &hs)) {
            cap_slot_push(ti, nslot);
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ENOMEM;   /* message STAYS queued */
        }
        cap_holder_remove(obj->id, HOLDER_QUEUE, (uint16_t)cobj->chan_id,
                          (uint16_t)(entry * CAP_MSG_MAX_CAPS));
        cap_holder_link(hs, obj);
        t->slots[nslot].word = m->cap_word[0];
        m->cap_word[0] = 0;
        *out_cap = nslot;
        cap_unlock(&obj->lock);

        /* Defensive: a legacy single-cap recv on a multi-cap message. The
         * Phase-3 path uses cap_recv_msg (which installs all caps); this
         * branch exists so a mismatch can never leak holders. Drain the
         * extras exactly as revoke would — remove each queue holder, free
         * the object at refcount 0 — without installing them. */
        cap_msg_drain_tail(ch, cobj->chan_id, entry, m);
    } else {
        *out_cap = CAP_NONE;
    }

    *cookie_out = m->cookie;
    m->payload_len = 0;
    cap_msg_payload_free(m->payload_idx);
    m->payload_idx = CAP_NONE;
    ch->qhead[dir]++;
    ch->qdepth[dir]--;
    cap_unlock(&ch->lock);
    cap_unlock(&t->lock);
    return 0;
}

/* ─── Phase 3 message transport ───────────────────────────────────────────────
 * cap_send_msg / cap_recv_msg — the SEND_CAP / RECV_CAP of the Polyglot
 * Nexus design (§2.2-2.3): a channel message carries a staged payload (the
 * IDL envelope, opaque to the kernel) plus up to CAP_MSG_MAX_CAPS moved MEM
 * caps, each with the byte-level descriptor (offset/len/rights/flags) the
 * IDL attached. The cap movement reuses the exact single-cap holder
 * machinery of cap_send/cap_recv, one holder per cap, ref encoded as
 * entry*CAP_MSG_MAX_CAPS + i so revoke can stamp the right word.
 *
 * Lock order (table < channel < object) and fail-before-mutate discipline
 * are identical to the single-cap path. */

/* cap_arena_free needs cap_object_free_resources (defined after cap_revoke);
 * forward-declared here so this section stands on its own. */
static uint32_t cap_object_free_resources(uint32_t obj_id);

/* ─── cap_send_msg ─────────────────────────────────────────────────────────── */
int cap_send_msg(uint32_t pid, uint16_t ch_w_idx, const void* payload,
                 uint32_t payload_len, const struct SLSCapDesc* descs,
                 uint16_t n_caps, uint32_t tag, uint32_t flags) {
    if (n_caps > CAP_MSG_MAX_CAPS) return CAP_ERANGE;
    if (payload_len > CAP_MSG_MAX_PAYLOAD) return CAP_ERANGE;
    if (payload_len > 0 && !payload) return CAP_EINVAL;

    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);

    uint64_t w = t->slots[ch_w_idx].word;
    if (!cap_word_valid(w) ||
        ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_W ||
        !((w >> CAP_PERM_SHIFT) & CAP_PERM_SEND)) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    uint32_t chan_obj = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* cobj = cap_object_get(chan_obj);
    if (!cobj || cobj->kind != CAP_OBJ_KIND_CHAN) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    struct CapChannel* ch = &cap_channels[cobj->chan_id];
    if (!ch->active) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    int dest = (pid == ch->end0_pid) ? 1 : 0;

    /* Validate every descriptor up front: each names a valid MEM cap in the
     * sender's table. The words are snapshotted here; the holder moves below
     * re-validate under the object lock (revoke race, same as cap_send). */
    uint64_t words[CAP_MSG_MAX_CAPS];
    struct CapObject* objs[CAP_MSG_MAX_CAPS];
    for (int i = 0; i < n_caps; i++) {
        uint16_t slot = descs[i].slot;
        uint64_t pw = t->slots[slot].word;
        if (!cap_word_valid(pw) ||
            ((pw >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_MEM) {
            cap_unlock(&t->lock);
            return CAP_EINVAL;
        }
        words[i] = pw;
        objs[i] = cap_object_get((uint32_t)((pw >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK));
        if (!objs[i]) {
            cap_unlock(&t->lock);
            return CAP_EINVAL;
        }
    }

    /* Stage the payload BEFORE taking the channel lock: pool exhaustion must
     * fail without touching the queue. */
    uint16_t pidx = CAP_NONE;
    if (payload_len > 0) {
        if (cap_msg_payload_alloc(&pidx)) {
            cap_unlock(&t->lock);
            return CAP_ENOSPC;
        }
        for (uint32_t i = 0; i < payload_len; i++)
            cap_msg_payload[pidx][i] = ((const uint8_t*)payload)[i];
    }

    cap_lock(&ch->lock);
    if (ch->qdepth[dest] >= CHAN_QUEUE_DEPTH) {
        cap_unlock(&ch->lock);
        if (pidx != CAP_NONE) cap_msg_payload_free(pidx);
        cap_unlock(&t->lock);
        return CAP_EAGAIN;
    }
    uint32_t entry = ch->qtail[dest] % CHAN_QUEUE_DEPTH;
    struct ChanMsg* m = &ch->q[dest][entry];
    /* Zero the whole entry first (ring slots are reused; drain/revoke scan
     * cap_word[] for nonzero words). */
    for (int i = 0; i < CAP_MSG_MAX_CAPS; i++) {
        m->cap_word[i] = 0;
        m->cap_off[i] = 0;
        m->cap_len[i] = 0;
        m->cap_rights[i] = 0;
        m->cap_flags[i] = 0;
    }

    /* Move each cap: check revoking under the object lock, pre-reserve the
     * queue holder, move slot → queue. A revoke race on cap i aborts the
     * whole send — earlier caps are moved BACK so nothing is half-sent. */
    int moved = 0;
    int fail = 0;
    uint16_t hq[CAP_MSG_MAX_CAPS];
    for (int i = 0; i < n_caps; i++) {
        struct CapObject* obj = objs[i];
        cap_lock(&obj->lock);
        if (obj->revoking) {
            cap_unlock(&obj->lock);
            fail = 1;
            break;
        }
        uint16_t slot = descs[i].slot;
        if (cap_holder_alloc(obj->id, HOLDER_QUEUE, (uint16_t)cobj->chan_id,
                             (uint16_t)(entry * CAP_MSG_MAX_CAPS + i),
                             (uint8_t)dest, &hq[i])) {
            cap_unlock(&obj->lock);
            fail = 1;
            break;
        }
        cap_holder_remove(obj->id, HOLDER_SLOT, (uint16_t)pid, slot);
        t->slots[slot].word = 0;
        cap_slot_push(ti, slot);
        cap_holder_link(hq[i], obj);
        m->cap_word[i] = words[i];
        m->cap_off[i] = descs[i].offset;
        m->cap_len[i] = descs[i].len;
        m->cap_rights[i] = descs[i].rights;
        m->cap_flags[i] = descs[i].flags;
        cap_unlock(&obj->lock);
        moved++;
    }

    if (fail) {
        /* Roll back: move moved caps back to fresh slots in the sender's
         * table, free staged payload, leave the queue untouched. */
        for (int i = 0; i < moved; i++) {
            struct CapObject* obj = objs[i];
            cap_lock(&obj->lock);
            uint16_t slot;
            if (cap_slot_pop(ti, &slot) == 0) {
                cap_holder_remove(obj->id, HOLDER_QUEUE,
                                  (uint16_t)cobj->chan_id,
                                  (uint16_t)(entry * CAP_MSG_MAX_CAPS + i));
                uint16_t hs;
                if (cap_holder_alloc(obj->id, HOLDER_SLOT, (uint16_t)pid,
                                     slot, 0, &hs) == 0) {
                    cap_holder_link(hs, obj);
                    t->slots[slot].word = words[i];
                } else {
                    /* holder pool exhausted mid-rollback: never lose the
                     * slot (a leaked free slot breaks the freelist) */
                    cap_slot_push(ti, slot);
                }
            }
            m->cap_word[i] = 0;
            cap_unlock(&obj->lock);
        }
        m->n_caps = 0;
        cap_unlock(&ch->lock);
        if (pidx != CAP_NONE) cap_msg_payload_free(pidx);
        cap_unlock(&t->lock);
        return CAP_ECAPREVOKED;
    }

    m->n_caps = (uint8_t)n_caps;
    m->cookie = tag;
    m->flags = (uint8_t)flags;
    m->payload_len = (uint16_t)payload_len;
    m->payload_idx = pidx;
    ch->qtail[dest]++;
    ch->qdepth[dest]++;
    cap_unlock(&ch->lock);
    cap_unlock(&t->lock);

    cap_wake_chan(cobj->chan_id);
    cap_maybe_handoff();
    return 0;
}

/* ─── cap_recv_msg ─────────────────────────────────────────────────────────── */
int cap_recv_msg(uint32_t pid, uint16_t ch_r_idx,
                 void* buf, uint32_t buf_len, uint32_t* out_payload_len,
                 uint16_t max_caps, struct SLSCapDesc* out_caps,
                 uint16_t* out_n_caps, uint32_t* out_tag, uint32_t* out_flags) {
    if (max_caps > CAP_MSG_MAX_CAPS) max_caps = CAP_MSG_MAX_CAPS;
    if (out_n_caps) *out_n_caps = 0;
    if (out_payload_len) *out_payload_len = 0;
    if (out_tag) *out_tag = 0;
    if (out_flags) *out_flags = 0;

    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);

    uint64_t w = t->slots[ch_r_idx].word;
    if (!cap_word_valid(w) ||
        ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_R ||
        !((w >> CAP_PERM_SHIFT) & CAP_PERM_RECV)) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    uint32_t chan_obj = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* cobj = cap_object_get(chan_obj);
    if (!cobj || cobj->kind != CAP_OBJ_KIND_CHAN) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    struct CapChannel* ch = &cap_channels[cobj->chan_id];
    if (!ch->active) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    int dir = (pid == ch->end0_pid) ? 0 : 1;

    cap_lock(&ch->lock);
    if (ch->qdepth[dir] == 0) {
        cap_unlock(&ch->lock);
        cap_unlock(&t->lock);
        return CAP_EAGAIN;
    }
    uint32_t entry = ch->qhead[dir] % CHAN_QUEUE_DEPTH;
    struct ChanMsg* m = &ch->q[dir][entry];

    uint16_t n_installed = 0;
    for (int i = 0; i < m->n_caps && i < max_caps; i++) {
        if (m->cap_word[i] == 0) continue;  /* installed by an earlier
                                             * partial recv (ETABLEFULL /
                                             * ENOMEM retry): the word was
                                             * zeroed; the holder already
                                             * moved. obj_id 0 is a REAL
                                             * object here, so a zeroed word
                                             * must never reach the install. */
        uint32_t obj_id =
            (uint32_t)((m->cap_word[i] >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
        struct CapObject* obj = cap_object_get(obj_id);
        if (!obj) continue;   /* defensive: stale word, skip */
        cap_lock(&obj->lock);
        if (((m->cap_word[i] >> CAP_STATE_SHIFT) & CAP_STATE_MASK)
                == CAP_STATE_REVOKED || obj->revoking) {
            /* Revoked in flight: revoke already accounted the holder; drain
             * this word without installing (no double-count). */
            m->cap_word[i] = 0;
            cap_unlock(&obj->lock);
            continue;
        }
        uint16_t nslot;
        if (cap_slot_pop(ti, &nslot)) {
            /* Table full: the message STAYS queued; caps already installed
             * this call are left installed (they were moved out of the
             * queue); retry drains the remainder. */
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ETABLEFULL;
        }
        uint16_t hs;
        if (cap_holder_alloc(obj->id, HOLDER_SLOT, (uint16_t)pid, nslot, 0,
                             &hs)) {
            cap_slot_push(ti, nslot);
            cap_unlock(&obj->lock);
            cap_unlock(&ch->lock);
            cap_unlock(&t->lock);
            return CAP_ENOMEM;
        }
        cap_holder_remove(obj->id, HOLDER_QUEUE, (uint16_t)cobj->chan_id,
                          (uint16_t)(entry * CAP_MSG_MAX_CAPS + i));
        cap_holder_link(hs, obj);
        t->slots[nslot].word = m->cap_word[i];
        if (out_caps && n_installed < max_caps) {
            out_caps[n_installed].slot = nslot;
            out_caps[n_installed].offset = m->cap_off[i];
            out_caps[n_installed].len = m->cap_len[i];
            out_caps[n_installed].rights = m->cap_rights[i];
            out_caps[n_installed].flags = m->cap_flags[i];
        }
        m->cap_word[i] = 0;
        n_installed++;
        cap_unlock(&obj->lock);
    }

    /* Drain any caps beyond max_caps (or skipped as revoked): remove each
     * leftover queue holder, freeing the object at refcount 0 — a message
     * must never leave holders behind when it is dequeued. */
    for (int i = 0; i < m->n_caps; i++) {
        if (m->cap_word[i] == 0) continue;   /* installed or revoked above */
        uint32_t xid = (uint32_t)((m->cap_word[i] >> CAP_OBJ_SHIFT) &
                                  CAP_OBJ_MASK);
        struct CapObject* xo = cap_object_get(xid);
        if (xo) {
            cap_lock(&xo->lock);
            cap_holder_remove(xo->id, HOLDER_QUEUE, (uint16_t)cobj->chan_id,
                              (uint16_t)(entry * CAP_MSG_MAX_CAPS + i));
            if (xo->refcount == 0) {
                cap_object_free_resources(xo->id);
                cap_object_destroy(xo->id);
            }
            cap_unlock(&xo->lock);
        }
        m->cap_word[i] = 0;
    }

    /* Copy the payload out and release the staging buffer. */
    uint32_t copy_len = m->payload_len;
    if (copy_len > buf_len) copy_len = buf_len;
    if (copy_len > 0 && buf) {
        for (uint32_t i = 0; i < copy_len; i++)
            ((uint8_t*)buf)[i] = cap_msg_payload[m->payload_idx][i];
    }
    if (out_payload_len) *out_payload_len = m->payload_len;
    if (out_tag) *out_tag = (uint32_t)m->cookie;
    if (out_flags) *out_flags = m->flags;
    if (out_n_caps) *out_n_caps = n_installed;
    cap_msg_payload_free(m->payload_idx);
    m->payload_idx = CAP_NONE;
    m->payload_len = 0;
    m->n_caps = 0;
    m->flags = 0;
    ch->qhead[dir]++;
    ch->qdepth[dir]--;
    cap_unlock(&ch->lock);
    cap_unlock(&t->lock);
    return 0;
}

/* ─── cap_arena_free ───────────────────────────────────────────────────────── */
/* Drop ONE MEM reference: remove the slot holder (the object's arena frames
 * return at refcount 0). Unlike cap_revoke — which nukes every holder of the
 * object including queued copies — this only drops the caller's own slot, so
 * a cap that was also sent on a channel survives. Lock order table < object,
 * same as cap_revoke's slot-zeroing phase. */
int cap_arena_free(uint32_t pid, uint16_t cap_idx) {
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);
    uint64_t w = t->slots[cap_idx].word;
    if (!cap_word_valid(w) ||
        ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_MEM) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* obj = cap_object_get(obj_id);
    if (!obj) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }

    cap_lock(&obj->lock);
    if (obj->revoking) {
        /* Revoke in progress: its holder walk owns this slot; leave it. */
        cap_unlock(&obj->lock);
        cap_unlock(&t->lock);
        return CAP_ECAPREVOKED;
    }
    cap_holder_remove(obj_id, HOLDER_SLOT, (uint16_t)pid, cap_idx);
    uint32_t freed = 0;
    if (obj->refcount == 0) {
        freed = cap_object_free_resources(obj_id);
        cap_object_destroy(obj_id);
    }
    cap_unlock(&obj->lock);

    t->slots[cap_idx].word = 0;
    cap_slot_push(ti, cap_idx);
    cap_unlock(&t->lock);

    (void)freed;
    return 0;
}

/* Defined below (after cap_revoke, which uses it): free a dead object's
 * machine resources (arena frames / channel slot). Forward-declared so the
 * revoke finalize can call it. */
static uint32_t cap_object_free_resources(uint32_t obj_id);

/* ─── cap_revoke ───────────────────────────────────────────────────────────── */
/* Immediate, total revocation. Linearization point: obj->revoking set under
 * the object lock. After that, under the same lock, every queued word is
 * stamped REVOKED (so no recv can install it) and every holder is unlinked
 * and counted. Then, OUTSIDE the object lock, each holder's slot is zeroed
 * and that process's mappings of the object are torn down (one table lock
 * at a time). Finally, back under the object lock, refcount is reduced by
 * the number of holders; at zero the object is destroyed and its arena
 * frames returned. Because ids/structs are never recycled, a stale slot
 * word can never alias a new object; because every creation path checks
 * revoking / the stamp under the object lock, no new reference can exist
 * between the linearization point and the free. */
int cap_revoke(uint32_t pid, uint16_t cap_idx) {
    (void)pid;
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);
    uint64_t w = t->slots[cap_idx].word;
    if (!cap_word_valid(w)) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* obj = cap_object_get(obj_id);
    if (!obj) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    cap_unlock(&t->lock);   /* the walk below needs other tables; hold nothing */

    cap_lock(&obj->lock);
    if (obj->revoking) {
        cap_unlock(&obj->lock);
        return CAP_EALREADY;
    }
    obj->revoking = 1;      /* ← linearization point */

    /* Collect holders into a STATIC scratch buffer, not a local: at
     * CAP_HOLDER_MAX entries a local would be a 16 KiB frame, which this
     * kernel treats as corruption risk (see the Makefile's
     * -Wframe-larger-than note and tests/stack_frame_budget_check.sh).
     * Single-writer by construction: revoke serializes on the object
     * lock, so no two revokes can be in this walk at once. */
    static uint16_t holders[CAP_HOLDER_MAX];
    uint32_t n = 0;
    uint16_t h = obj->holder_head;
    while (h != CAP_FREELIST_END && n < CAP_HOLDER_MAX) {
        holders[n++] = h;
        h = cap_holders[h].next;
    }
    obj->holder_head = CAP_FREELIST_END;
    for (uint32_t i = 0; i < n; i++) {
        struct CapHolder* node = &cap_holders[holders[i]];
        node->next = CAP_FREELIST_END;
        node->prev = CAP_FREELIST_END;
        if (node->kind == HOLDER_QUEUE) {
            struct CapChannel* ch = &cap_channels[node->pid];
            /* ref encodes (ring entry, cap index): entry*MAX + i. */
            uint32_t entry = node->ref / CAP_MSG_MAX_CAPS;
            uint32_t cap_i = node->ref % CAP_MSG_MAX_CAPS;
            ch->q[node->qdir][entry].cap_word[cap_i] =
                cap_word_stamp_revoked(
                    ch->q[node->qdir][entry].cap_word[cap_i]);
        }
    }
    cap_unlock(&obj->lock);

    /* Zero slots + tear down mappings, one table at a time. */
    for (uint32_t i = 0; i < n; i++) {
        struct CapHolder* node = &cap_holders[holders[i]];
        if (node->kind != HOLDER_SLOT) continue;
        int hti = cap_table_index(node->pid);
        if (hti < 0) continue;
        struct CapTable* ht = &cap_tables[hti];
        cap_lock(&ht->lock);
        ht->slots[node->ref].word = 0;
        cap_slot_push(hti, node->ref);
        cap_unlock(&ht->lock);
    }

    /* Mappings outlive the cap that created them (a process can map, then
     * send the cap away, and still hold the PTE). Revocation must therefore
     * tear down the object's PTEs in EVERY table, not just the current
     * holders' — the map registry is keyed by object id precisely so this
     * scan can find them. 16 tables x 64 map records is cheap. */
    for (int ti = 0; ti < CAP_TABLE_MAX; ti++) {
        cap_lock(&cap_tables[ti].lock);
        cap_table_unmap_object((uint32_t)ti, obj_id);
        cap_unlock(&cap_tables[ti].lock);
    }

    /* Finalize under the object lock; free at refcount 0. */
    cap_lock(&obj->lock);
    if (obj->refcount >= n) obj->refcount -= n;
    else obj->refcount = 0;
    for (uint32_t i = 0; i < n; i++) cap_holder_free(holders[i]);
    if (obj->refcount == 0) {
        cap_object_free_resources(obj_id);
        cap_object_destroy(obj_id);
    }
    cap_unlock(&obj->lock);
    return 0;
}

/* ─── Phase 2 teardown ─────────────────────────────────────────────────────── */
/* Free the machine resources an object owns when its last holder dies:
 * MEM objects return their arena frames to the arena bitmap (refcount 0 is
 * the ONLY exit from arena ownership — every create path checks revoking /
 * holder existence under the object lock), CHAN objects deactivate their
 * channel. Returns the number of arena frames returned. The caller holds
 * the object lock. Shared by cap_revoke's finalize and cap_table_teardown. */
static uint32_t cap_object_free_resources(uint32_t obj_id) {
    struct CapObject* o = &cap_objects[obj_id];
    uint32_t freed = 0;
    if (o->kind == CAP_OBJ_KIND_MEM) {
        for (uint32_t f = 0; f < CAP_ARENA_FRAMES; f++) {
            if (cap_arena_owner[f] == obj_id) {
                cap_arena_frame_clear(f);
                freed++;
            }
        }
    } else if (o->kind == CAP_OBJ_KIND_CHAN) {
        struct CapChannel* ch = &cap_channels[o->chan_id];
        ch->active = 0;
    }
    return freed;
}

/* Drain every message in channel `ch`'s queue `dir` as if REVOKED: each
 * queued cap word's HOLDER_QUEUE node is removed from its object (freeing
 * the object and its arena frames at refcount 0), the message is cleared,
 * and qhead/qdepth advance. Used by teardown for queues nobody will ever
 * read again — the dying process's own read ends, and both queues of a
 * channel whose last holder just died. The caller holds the table lock
 * (lock order table < channel < object, same as cap_recv). */
static uint32_t cap_drain_queue(uint16_t chan_id, struct CapChannel* ch,
                                int dir, uint32_t* objs_destroyed) {
    uint32_t freed = 0;
    while (ch->qdepth[dir] > 0) {
        uint32_t entry = ch->qhead[dir] % CHAN_QUEUE_DEPTH;
        struct ChanMsg* m = &ch->q[dir][entry];
        for (int i = 0; i < CAP_MSG_MAX_CAPS; i++) {
            if (m->cap_word[i] == 0) continue;
            uint32_t obj_id =
                (uint32_t)((m->cap_word[i] >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
            struct CapObject* obj = cap_object_get(obj_id);
            if (obj) {
                /* The queued word is the cap's ONLY holder node here
                 * (HOLDER_QUEUE, keyed by the channel id + ring entry) —
                 * the dying process's read end was the only reader, so
                 * revoke it exactly as cap_revoke drains a stamped entry:
                 * remove the holder, free the object at refcount 0. */
                cap_lock(&obj->lock);
                cap_holder_remove(obj_id, HOLDER_QUEUE, chan_id,
                                  (uint16_t)(entry * CAP_MSG_MAX_CAPS + i));
                if (obj->refcount == 0) {
                    freed += cap_object_free_resources(obj_id);
                    cap_object_destroy(obj_id);
                    (*objs_destroyed)++;
                }
                cap_unlock(&obj->lock);
            }
            m->cap_word[i] = 0;
        }
        m->n_caps = 0;
        m->payload_len = 0;
        cap_msg_payload_free(m->payload_idx);
        m->payload_idx = CAP_NONE;
        ch->qhead[dir]++;
        ch->qdepth[dir]--;
    }
    return freed;
}

/* ─── cap_table_teardown (Phase 2) ───────────────────────────────────────────
 * Total reclamation of ONE process's capability footprint, called from
 * process_exit()/process_kill(). Drops every slot holder (destroying
 * objects at refcount 0 — returning arena frames / deactivating channels),
 * drains every channel queue nobody can ever read again, unmaps the
 * process's cap-derived PTEs, and UNBINDS the pid→table slot so a later
 * process reuses it (without this, repeated spawn/exit cycles exhaust the
 * 15 process tables and every cap syscall of a new process returns
 * CAP_ETABLEFULL — the leak the milestone exists to close).
 *
 * Ordering (all under the table lock; one channel/object lock at a time,
 * lock order table < channel < object, matching cap_recv):
 *   1. PRE-DRAIN the read queue of every channel this process held a CHAN_R
 *      on — caps queued for a dead reader can never be recv'd, so they are
 *      revoked here (holders removed, objects freed at 0).
 *   2. DROP every slot holder; zero + free the slot. Objects destroyed at
 *      refcount 0. A CHAN object destroyed here may still hold messages in
 *      ITS other queue (sent by this very process to itself in the
 *      far_pid=0 shape) — recorded for step 3.
 *   3. POST-DRAIN both queues of every channel destroyed in step 2 (all
 *      holders gone, so no live reader can ever drain them).
 *   4. UNMAP the process's cap-derived mappings (arena PTEs — the frames
 *      themselves are already freed with their objects in steps 1-3).
 *   5. UNBIND the table and rebuild its freelist.
 *
 * Called with the process marked ZOMBIE but still active, so
 * cap_proc_cr3(pid) resolves its PML4 for step 4. No-op when the pid never
 * used capabilities (cap_table_find never binds).
 *
 * Concurrency: teardown runs inside a syscall (single CPU, IF=0) — the
 * timer cannot preempt it, and the lock discipline mirrors the normal cap
 * paths, so the whole thing is atomic with respect to the rest of the
 * kernel. Static scratch arrays, single-writer by construction, same
 * discipline as cap_revoke's holder snapshot. */
void cap_table_teardown(uint32_t pid) {
    int ti = cap_table_find(pid);
    if (ti < 0) return;   /* never used capabilities: nothing to reclaim */
    struct CapTable* t = &cap_tables[ti];

    static uint16_t slots_buf[CAP_TABLE_ENTRIES];   /* valid slot indices   */
    static uint32_t chan_buf[CAP_CHAN_MAX];         /* channels to pre-drain */
    static uint32_t dead_buf[CAP_CHAN_MAX];         /* channels to post-drain */
    uint32_t n_slots = 0, n_chans = 0, n_dead = 0;
    uint32_t objs_destroyed = 0;
    uint32_t arena_freed = 0;

    cap_lock(&t->lock);

    /* 0. Snapshot the process's valid slots; collect distinct channels it
     *    holds a CHAN_R on (for the pre-drain). */
    for (int i = 0; i < CAP_TABLE_ENTRIES; i++) {
        uint64_t w = t->slots[i].word;
        if (!cap_word_valid(w)) continue;
        slots_buf[n_slots++] = (uint16_t)i;
        if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) == CAP_TYPE_CHAN_R) {
            uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
            int seen = 0;
            for (uint32_t j = 0; j < n_chans; j++)
                if (chan_buf[j] == oid) { seen = 1; break; }
            if (!seen && n_chans < CAP_CHAN_MAX) chan_buf[n_chans++] = oid;
        }
    }

    /* 1. Pre-drain: the read queue of every channel this process held a
     *    CHAN_R on. Direction keyed on pid exactly like cap_recv
     *    (end0 reads q[0], the far end reads q[1]). */
    for (uint32_t j = 0; j < n_chans; j++) {
        struct CapObject* co = cap_object_get(chan_buf[j]);
        if (!co || co->kind != CAP_OBJ_KIND_CHAN) continue;
        struct CapChannel* ch = &cap_channels[co->chan_id];
        int dir = (pid == ch->end0_pid) ? 0 : 1;
        cap_lock(&ch->lock);
        arena_freed += cap_drain_queue(co->chan_id, ch, dir, &objs_destroyed);
        cap_unlock(&ch->lock);
    }

    /* 2. Drop every slot holder; destroy objects at refcount 0. */
    for (uint32_t k = 0; k < n_slots; k++) {
        uint16_t slot = slots_buf[k];
        uint64_t w = t->slots[slot].word;
        uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
        struct CapObject* obj = cap_object_get(obj_id);
        if (!obj) {   /* dead-object word (defensive): just free the slot */
            t->slots[slot].word = 0;
            cap_slot_push(ti, slot);
            continue;
        }
        cap_lock(&obj->lock);
        cap_holder_remove(obj_id, HOLDER_SLOT, (uint16_t)pid, slot);
        t->slots[slot].word = 0;
        cap_slot_push(ti, slot);
        if (obj->refcount == 0) {
            if (obj->kind == CAP_OBJ_KIND_CHAN && n_dead < CAP_CHAN_MAX)
                dead_buf[n_dead++] = obj->chan_id;
            arena_freed += cap_object_free_resources(obj_id);
            cap_object_destroy(obj_id);
            objs_destroyed++;
        }
        cap_unlock(&obj->lock);
    }

    /* 3. Post-drain: both queues of every channel whose LAST holder died in
     *    step 2 — with all four end caps gone, no reader exists, so any
     *    queued cap words (e.g. this process sending to itself in the
     *    far_pid=0 shape) would otherwise pin their objects forever. */
    for (uint32_t j = 0; j < n_dead; j++) {
        struct CapChannel* ch = &cap_channels[dead_buf[j]];
        cap_lock(&ch->lock);
        arena_freed += cap_drain_queue((uint16_t)dead_buf[j], ch, 0, &objs_destroyed);
        arena_freed += cap_drain_queue((uint16_t)dead_buf[j], ch, 1, &objs_destroyed);
        cap_unlock(&ch->lock);
    }

    /* 4. Unmap this process's cap-derived PTEs. The arena frames themselves
     *    are already returned (steps 1-3 destroyed their objects); this
     *    clears the dying address space's PTEs so nothing aliases the freed
     *    frames (belt-and-braces — the page tables are destroyed next, but
     *    the model must not depend on that). */
    uint64_t cr3 = cap_proc_cr3(pid);
    for (int i = 0; i < CAP_MAP_MAX; i++) {
        struct CapMap* m = &t->maps[i];
        if (!m->active) continue;
        for (uint32_t p = 0; p < m->npages; p++)
            cap_arch_unmap_page(cr3, m->vaddr + (uint64_t)p * 4096u);
        m->active = 0;
    }

    /* 5. Unbind: the pid→table slot is reusable by a later process. The
     *    freelist rebuild is redundant (every slot was just pushed) but
     *    makes the table provably clean for the next binder. */
    cap_table_pid[ti] = 0;
    cap_tables[ti].pid = 0;
    cap_table_freelist_init((uint32_t)ti);

    cap_unlock(&t->lock);

    kernel_serial_printf(
        "[TORE] PID %u teardown: %u cap(s) dropped, %u object(s) destroyed, "
        "%u arena frame(s) returned, table %d unbound\n",
        pid, n_slots, objs_destroyed, arena_freed, ti);
}

/* ─── cap_map / cap_unmap ──────────────────────────────────────────────────── */
int cap_map(uint32_t pid, uint16_t cap_idx, uint64_t vaddr, uint32_t flags) {
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);

    uint64_t w = t->slots[cap_idx].word;
    if (!cap_word_valid(w) ||
        ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_MEM) {
        cap_unlock(&t->lock);
        return CAP_EBADF;
    }
    uint32_t perms = (uint32_t)((w >> CAP_PERM_SHIFT) & CAP_PERM_MASK);
    if (!(perms & CAP_PERM_MAP)) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    if (flags == 0 || (flags & ~(CAP_PERM_R | CAP_PERM_W | CAP_PERM_X))) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    if (flags & ~perms) {   /* no permission escalation beyond the cap */
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    if ((vaddr & 0xFFFULL) != 0) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    uint32_t off = (uint32_t)((w >> CAP_OFF_SHIFT) & CAP_OFF_MASK);
    uint32_t len = (uint32_t)((w >> CAP_LEN_SHIFT) & CAP_LEN_MASK);
    uint64_t vhi = vaddr + (uint64_t)len * 4096u;
    if (len == 0 || vhi <= vaddr || vhi > 0x800000000000ULL) {
        cap_unlock(&t->lock);
        return CAP_ERANGE;   /* must fit in the 47-bit user half */
    }

    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    struct CapObject* obj = cap_object_get(obj_id);
    if (!obj || obj->kind != CAP_OBJ_KIND_MEM) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    if ((uint64_t)off + len > obj->npages) {
        cap_unlock(&t->lock);
        return CAP_ERANGE;
    }
    if (cap_maps_overlap(t, obj_id, vaddr, len)) {
        cap_unlock(&t->lock);
        return CAP_ECONFLICT;   /* different object already mapped here */
    }

    cap_lock(&obj->lock);
    if (obj->revoking) {
        cap_unlock(&obj->lock);
        cap_unlock(&t->lock);
        return CAP_ECAPREVOKED;
    }
    uint64_t cr3 = cap_proc_cr3(pid);
    if (cr3 == 0) {
        /* Kernel context (pid 0) has no user page table to map into. */
        cap_unlock(&obj->lock);
        cap_unlock(&t->lock);
        return CAP_ENOSYS;
    }
    for (uint32_t i = 0; i < len; i++) {
        int r = cap_arch_map_page(cr3, vaddr + (uint64_t)i * 4096u,
                                  obj->phys_base + (uint64_t)(off + i) * 4096u,
                                  flags);
        if (r < 0) {
            /* Roll back the pages already mapped this call. */
            for (uint32_t j = 0; j < i; j++)
                cap_arch_unmap_page(cr3, vaddr + (uint64_t)j * 4096u);
            cap_arch_tlb_flush();
            cap_unlock(&obj->lock);
            cap_unlock(&t->lock);
            return r;
        }
    }
    cap_arch_tlb_flush();
    cap_unlock(&obj->lock);

    /* Record the mapping (keyed by object, not slot: the cap may later
     * move; revocation tears the PTE down by object id). */
    int slot = -1;
    for (int i = 0; i < CAP_MAP_MAX; i++) {
        if (!t->maps[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        cap_unlock(&t->lock);
        return CAP_ETABLEFULL;
    }
    t->maps[slot].obj_id = obj_id;
    t->maps[slot].cap_slot = cap_idx;
    t->maps[slot].npages = (uint16_t)len;
    t->maps[slot].vaddr = vaddr;
    t->maps[slot].active = 1;

    cap_unlock(&t->lock);
    return 0;
}

int cap_unmap(uint32_t pid, uint16_t cap_idx, uint64_t vaddr) {
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    struct CapTable* t = &cap_tables[ti];

    cap_lock(&t->lock);
    uint64_t w = t->slots[cap_idx].word;
    uint32_t word_obj = (cap_word_valid(w) &&
                         ((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) == CAP_TYPE_MEM)
                        ? (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK) : 0;

    int found = -1;
    for (int i = 0; i < CAP_MAP_MAX; i++) {
        struct CapMap* m = &t->maps[i];
        if (!m->active || m->vaddr != vaddr) continue;
        if (m->cap_slot == cap_idx || (word_obj && m->obj_id == word_obj)) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        cap_unlock(&t->lock);
        return CAP_EINVAL;
    }
    struct CapMap* m = &t->maps[found];
    uint64_t cr3 = cap_proc_cr3(pid);
    for (uint32_t i = 0; i < m->npages; i++)
        cap_arch_unmap_page(cr3, m->vaddr + (uint64_t)i * 4096u);
    cap_arch_tlb_flush();
    m->active = 0;
    cap_unlock(&t->lock);
    return 0;
}

/* ─── Debug introspection ──────────────────────────────────────────────────── */

uint32_t cap_debug_objid(uint32_t pid, uint16_t cap_idx) {
    int ti = cap_table_index(pid);
    if (ti < 0) return 0xFFFFFFFFu;
    uint64_t w = cap_tables[ti].slots[cap_idx].word;
    if (!cap_word_valid(w)) return 0xFFFFFFFFu;
    return (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
}

uint32_t cap_debug_refcount(uint32_t obj_id) {
    struct CapObject* o = cap_object_get(obj_id);
    return o ? o->refcount : 0xFFFFFFFFu;
}

uint64_t cap_debug_obj_phys(uint32_t obj_id) {
    struct CapObject* o = cap_object_get(obj_id);
    if (!o || o->kind != CAP_OBJ_KIND_MEM) return 0;
    return o->phys_base;
}

uint32_t cap_object_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < CAP_OBJECT_MAX; i++)
        if (cap_objects[i].active) n++;
    return n;
}

uint32_t cap_arena_free_frames(void) {
    return cap_arena_free_count;
}

void cap_list(void) {
    kernel_serial_print("\n[CAP] Capability tables:\n");
    for (int ti = 0; ti < CAP_TABLE_MAX; ti++) {
        uint32_t live = 0;
        for (int i = 0; i < CAP_TABLE_ENTRIES; i++)
            if (cap_word_valid(cap_tables[ti].slots[i].word)) live++;
        if (live == 0) continue;
        kernel_serial_printf("  table %d (pid %u): %u live cap(s)\n",
                             ti, (unsigned)cap_tables[ti].pid, (unsigned)live);
        for (int i = 0; i < CAP_TABLE_ENTRIES; i++) {
            uint64_t w = cap_tables[ti].slots[i].word;
            if (!cap_word_valid(w)) continue;
            uint32_t type = (uint32_t)((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK);
            uint32_t obj = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
            uint32_t perms = (uint32_t)((w >> CAP_PERM_SHIFT) & CAP_PERM_MASK);
            kernel_serial_printf("    [%u] type=%u obj=%u perms=0x%02x\n",
                                 i, type, obj, perms);
        }
    }
    uint32_t live_objs = cap_object_count();
    uint32_t free_frames = cap_arena_free_frames();
    kernel_serial_printf(
        "[CAP] objects active=%u/%u  arena free=%u/%u frames\n",
        (unsigned)live_objs, (unsigned)CAP_OBJECT_MAX,
        (unsigned)free_frames, (unsigned)CAP_ARENA_FRAMES);
}

/* ─── Syscall wrappers (do_syscall ABI: uint64_t return) ───────────────────── */

uint64_t sys_sls_cap_create_mem(struct SLSCapCreateMemRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint16_t idx = CAP_NONE;
    int r = cap_create_mem(cap_current_pid(), req->phys_base, req->npages,
                           req->perm, &idx);
    if (r < 0) return (uint64_t)(int64_t)r;
    return (uint64_t)idx;
}

uint64_t sys_sls_cap_arena_alloc(struct SLSCapArenaAllocRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint16_t idx = CAP_NONE;
    int r = cap_arena_alloc(cap_current_pid(), req->npages, req->perm, &idx);
    if (r < 0) return (uint64_t)(int64_t)r;
    return (uint64_t)idx;
}

uint64_t sys_sls_chan_create(struct SLSCapChanCreateRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint16_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
    int r = cap_chan_create(cap_current_pid(), req->far_pid,
                            &rd, &wr, &frd, &fwr);
    if (r < 0) return (uint64_t)(int64_t)r;
    req->out_rd = rd;
    req->out_wr = wr;
    req->out_far_rd = frd;
    req->out_far_wr = fwr;
    return 0;
}

uint64_t sys_sls_cap_send(struct SLSCapSendRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    int r = cap_send(cap_current_pid(), req->ch_w_idx, req->cap_idx, req->cookie);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_cap_recv(struct SLSCapRecvRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    /* Phase 1.5 (immediate wake): release a HELD process before parking so
     * it runs while we are parked (atomic — no user-mode window for the
     * timer to schedule it before our park; the boot-check's overlap
     * ordering proof depends on this). Ignore errors: on a parked recv's
     * resume the request re-runs and the pid is already released. */
    if (req->release_pid) cap_release_pid(req->release_pid);
    uint16_t out = CAP_NONE;
    g_recv_park_req = req;   /* Phase 1.5: the resume re-runs THIS request */
    int r = cap_recv(cap_current_pid(), req->ch_r_idx, req->block,
                     &req->cookie, &out);
    g_recv_park_req = 0;
    if (r < 0) return (uint64_t)(int64_t)r;
    return (uint64_t)out;
}

uint64_t sys_sls_cap_revoke(struct SLSCapRevokeRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    int r = cap_revoke(cap_current_pid(), req->cap_idx);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_cap_map(struct SLSCapMapRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    int r = cap_map(cap_current_pid(), req->cap_idx, req->vaddr, req->flags);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_cap_unmap(struct SLSCapUnmapRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    int r = cap_unmap(cap_current_pid(), req->cap_idx, req->vaddr);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_cap_list(void) {
    cap_list();
    return 0;
}

uint64_t sys_sls_cap_send_msg(struct SLSCapSendMsgRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    int r = cap_send_msg(cap_current_pid(), req->ch_w_idx, req->payload,
                         req->payload_len, req->caps, req->n_caps,
                         req->tag, req->flags);
    return (uint64_t)(int64_t)r;
}

uint64_t sys_sls_cap_recv_msg(struct SLSCapRecvMsgRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint16_t out_n = 0;
    int r = cap_recv_msg(cap_current_pid(), req->ch_r_idx, req->buf,
                         req->buf_len, &req->out_payload_len,
                         req->max_caps, req->out_caps, &out_n,
                         &req->out_tag, &req->out_flags);
    if (r < 0) return (uint64_t)(int64_t)r;
    req->out_n_caps = out_n;
    return 0;
}

uint64_t sys_sls_cap_arena_free(struct SLSCapArenaFreeRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    int r = cap_arena_free(cap_current_pid(), req->cap_idx);
    return (uint64_t)(int64_t)r;
}
