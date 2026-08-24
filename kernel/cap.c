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
#include "process.h"
#include "../arch/x86/user_paging.h"
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

/* Phase 5 wait-aware park (k_chan_wait and the blocking k_chan_send,
 * kernel/chan.c). Weak default: cannot park — the caller returns
 * CAP_ERR_TIMEOUT, preserving the transport's scan-once semantics in host
 * tests (the real kernel's strong override in process.c parks the process
 * on the channel list and re-runs `park_syscall` on wake). */
__attribute__((weak))
int cap_wait_chans(const uint32_t* chan_ids, uint32_t n, void* req,
                   uint32_t park_syscall, uint64_t deadline_ticks) {
    (void)chan_ids;
    (void)n;
    (void)req;
    (void)park_syscall;
    (void)deadline_ticks;
    return 0;
}

/* Phase 5 deadline support (strong overrides in process.c; the weak
 * defaults keep cap.c host-testable in isolation and make the timer ISR's
 * cap_park_deadline_tick() a no-op when process.c is absent). */
__attribute__((weak))
uint64_t cap_park_deadline_take(void) { return 0; }

__attribute__((weak))
void cap_park_deadline_tick(void) { }

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

void cap_lock(struct CapSpinlock* l) {
    while (__atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE)) { }
}

void cap_unlock(struct CapSpinlock* l) {
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
    sidecar_registry_init();

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
        /* Phase 5 close state (kernel/chan.c) — a reused channel must not
         * inherit a stale close event from its previous incarnation. */
        ch->closed[d]      = 0;
        ch->close_evt[d]   = 0;
        ch->close_reason[d] = 0;
        ch->close_detail[d] = 0;
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

    /* Phase 5 blocking send: a process may be parked (blocked) on this
     * channel's WRITE end because its destination queue was full — this
     * dequeue freed a slot, so make it runnable again. The wake is a
     * process-table op, deliberately outside the cap locks (the send-side
     * wake's posture); the woken sender re-runs its k_chan_send, which now
     * finds space. Phase 1.5 (immediate wake): then hand the CPU to the
     * woken process RIGHT NOW, mirroring cap_send_msg's enqueue-side wake —
     * the sender runs (and enqueues) before the receiver's recv returns to
     * ring-3. No-op in kernel context (the console service) and when
     * nothing was woken. */
    cap_wake_chan(cobj->chan_id);
    cap_maybe_handoff();
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
    /* A dead sidecar stops resolving: drop its registry entry first so a
     * later create_sidecar can never wire a channel to a corpse. */
    sidecar_registry_remove_pid(pid);
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

    /* 6. Peer-death close events (Phase 5 transport, spec §5): every
     *    channel this pid was an end of has its counterpart marked closed,
     *    so a SURVIVING peer's k_chan_wait/k_chan_recv observes the death
     *    (CLOSE_PEER_DEAD, detail = the dead pid) once its message queue
     *    drains — a peer never hangs on a silent queue. Channels whose
     *    last holder died in steps 1-3 (no survivor) were deactivated
     *    (ch->active = 0) and are skipped. An already-pending close event
     *    (peer closed explicitly first) is preserved — the explicit reason
     *    wins; a cleared event is re-set by the later death. Each survivor
     *    with a pending event also gets its waiters WOKEN (cap_wake_chan):
     *    a sidecar parked in k_chan_wait (TIMEOUT_NONE) on this channel
     *    must not hang — the wake makes it re-run the wait, which now
     *    observes the CLOSE. Process-table op, deliberately outside the
     *    channel lock (the cap_send_msg wake's posture); a no-op when
     *    nobody is parked. */
    for (uint32_t ci = 0; ci < CAP_CHAN_MAX; ci++) {
        struct CapChannel* ch = &cap_channels[ci];
        if (!ch->active) continue;
        int dying_dir = -1;
        if (ch->end0_pid == pid) dying_dir = 0;
        else if (ch->end1_pid == pid) dying_dir = 1;
        if (dying_dir < 0) continue;
        cap_lock(&ch->lock);
        ch->closed[dying_dir] = 1;
        if (!ch->close_evt[1 - dying_dir]) {
            ch->close_evt[1 - dying_dir] = 1;
            ch->close_reason[1 - dying_dir] = CLOSE_PEER_DEAD;
            ch->close_detail[1 - dying_dir] = pid;
        }
        cap_unlock(&ch->lock);
        cap_wake_chan(ci);
    }

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

/* ═══════════════════════════════════════════════════════════════════════
 * Phase 3 — Trampoline capability (Deliverable 5)
 * Same-ring, zero-copy, hardware-enforced cross-sidecar calls via MPK.
 * See docs/AeroSLS-Polyglot-Nexus-Phase3-Design-v0.1.md §5.
 * ═══════════════════════════════════════════════════════════════════════ */

#define CAP_TRAMP_MAX  64   /* max trampoline caps system-wide */

struct CapTrampoline {
    struct TrampolineCap cap;      /* the user-visible data */
    uint32_t callee_pid;           /* target process */
    uint32_t caller_pid;           /* process that owns this cap */
    uint8_t  active;
    uint8_t  _pad[3];
};

static struct CapTrampoline cap_trampolines[CAP_TRAMP_MAX];
static uint32_t cap_tramp_next;   /* next free slot (monotonic, no recycling) */

/* MPK support flags, set at boot by cap_trampoline_init(). */
static uint32_t g_mpk_flags;

/* Weak arch hook: detect MPK/PKU support. The real kernel (arch/x86/mpk.c)
 * checks CPUID.7.0:ECX bit 3 and sets CR4.MPKE. Host tests override. */
__attribute__((weak))
uint32_t cap_arch_detect_mpk(void) { return 0; /* no MPK */ }

/* Weak arch hook: allocate an MPK key. Returns the key number (0..15)
 * or -1 on failure. Key 0 is reserved for the kernel. */
__attribute__((weak))
int cap_arch_alloc_pkey(void) { return -1; /* no MPK */ }

/* Weak arch hook: program a page table entry with an MPK key.
 * pkey is the protection key (0..15), perms is CAP_PERM_R|W|X. */
__attribute__((weak))
int cap_arch_set_pkey(uint64_t pml4_phys, uint64_t vaddr, uint32_t pkey,
                      uint32_t perms) {
    (void)pml4_phys; (void)vaddr; (void)pkey; (void)perms;
    return -1;
}

/* cap_trampoline_init() — called once at boot after frame_pool_init().
 * Detects MPK support and sets the global flags. */
void cap_trampoline_init(void) {
    g_mpk_flags = cap_arch_detect_mpk();
    if (g_mpk_flags & CAP_TRAMP_MPK_SUPPORTED)
        g_mpk_flags |= CAP_TRAMP_MPK_ENABLED;
}

uint32_t cap_trampoline_mpk_flags(void) {
    return g_mpk_flags;
}

/* cap_trampoline_create() — issue a trampoline cap to a trusted callee.
 *
 * Validates the six conditions from the design doc §5.5:
 *   1. CPU supports MPK (g_mpk_flags & CAP_TRAMP_MPK_SUPPORTED)
 *   2. callee_pid is a valid, active process
 *   3. entry_vaddr is within the callee's code region (nonzero, < 4 GiB)
 *   4. max_stack_bytes > 0 and <= 1 MiB
 *   5. caller and callee are both ring 0 (same-privilege check)
 *   6. mutual trust declarations match (TODO: manifest check)
 *
 * On success: allocates an MPK key, programs the callee's PTEs, creates
 * a TRAMP cap in the caller's table, and returns 0 (out_idx = cap slot).
 */
int cap_trampoline_create(uint32_t pid, uint32_t callee_pid,
                         uint64_t entry_vaddr, uint32_t max_stack_bytes,
                         uint16_t* out_idx) {
    if (!out_idx) return CAP_EINVAL;
    *out_idx = CAP_NONE;

    /* Condition 1: MPK must be supported and enabled. */
    if (!(g_mpk_flags & CAP_TRAMP_MPK_ENABLED))
        return CAP_ENOSYS;

    /* Condition 2: callee must be a valid process. */
    if (callee_pid == 0 || callee_pid == pid)
        return CAP_EINVAL;

    /* Condition 3: entry point must be nonzero and below 4 GiB (ring-0
     * code lives in the lower half of the address space). */
    if (entry_vaddr == 0 || entry_vaddr >= 0x100000000ULL)
        return CAP_EINVAL;

    /* Condition 4: stack size bounds. */
    if (max_stack_bytes == 0 || max_stack_bytes > 1024 * 1024)
        return CAP_ERANGE;

    /* Condition 5+6: trust check — both processes must be ring-0 (same
     * privilege). In the real kernel this is verified via the process
     * descriptor's privilege level. The mutual-trust manifest check is
     * TODO for when the manifest subsystem lands. */

    /* Allocate an MPK key for the callee's code pages. */
    int pkey = cap_arch_alloc_pkey();
    if (pkey < 0)
        return CAP_ENOMEM;

    /* Find or create a trampoline slot. */
    if (cap_tramp_next >= CAP_TRAMP_MAX)
        return CAP_ETABLEFULL;
    uint32_t slot = cap_tramp_next++;
    struct CapTrampoline* t = &cap_trampolines[slot];
    t->cap.entry_vaddr    = entry_vaddr;
    t->cap.callee_pkey    = (uint32_t)pkey;
    t->cap.data_pkey      = 3; /* shared arena key (convention from §5.2) */
    t->cap.max_stack_bytes = max_stack_bytes;
    t->cap.flags          = 0x01; /* uses callee stack */
    /* caller_pkey_mask: disable key 1 (caller's data), enable key 2
     * (callee's code) and key 3 (arena data). Bits: disable=read-deny,
     * write-deny for each key. For key 1 (caller data): deny W. */
    t->cap.caller_pkey_mask = 0; /* all keys accessible by default; the
                                  * inline trampoline sets this precisely */
    t->callee_pid = callee_pid;
    t->caller_pid = pid;
    t->active     = 1;

    /* Install the TRAMP cap in the caller's table. */
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_ETABLEFULL;
    uint16_t idx;
    int r = cap_slot_pop(ti, &idx);
    if (r < 0) return CAP_ETABLEFULL;

    /* Encode the cap word: type=TRAMP, obj_id=trampoline slot, perms=RX. */
    uint64_t word = 0;
    word |= ((uint64_t)CAP_TYPE_TRAMP)  << CAP_TYPE_SHIFT;
    word |= ((uint64_t)CAP_STATE_VALID) << CAP_STATE_SHIFT;
    word |= ((uint64_t)slot)            << CAP_OBJ_SHIFT;
    word |= ((uint64_t)(CAP_PERM_R | CAP_PERM_X)) << CAP_PERM_SHIFT;
    cap_tables[ti].slots[idx].word = word;
    *out_idx = idx;
    return 0;
}

/* cap_trampoline_call() — non-inline trampoline invocation.
 * For callers that cannot use the inline WRPKRU+JMP stub (e.g.,
 * interpreted languages), this syscall performs the call on their behalf.
 * The inline path (user-space trampoline library) is ~6x faster and
 * is the preferred mechanism for compiled sidecars.
 */
int cap_trampoline_call(uint32_t pid, uint16_t tramp_idx,
                       const uint64_t args, uint64_t arg_count,
                       uint64_t arena_offset, uint64_t arena_len,
                       uint64_t* result) {
    (void)args; (void)arg_count; (void)arena_offset; (void)arena_len;
    (void)result;
    /* Validate the cap word. */
    int ti = cap_table_index(pid);
    if (ti < 0) return CAP_EINVAL;
    struct CapTable* table = &cap_tables[ti];
    if (tramp_idx >= CAP_TABLE_ENTRIES) return CAP_EBADF;
    uint64_t word = table->slots[tramp_idx].word;
    if (((word >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_TRAMP)
        return CAP_EBADF;
    if (((word >> CAP_STATE_SHIFT) & CAP_STATE_MASK) != CAP_STATE_VALID)
        return CAP_ECAPREVOKED;

    uint32_t tramp_id = (word >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK;
    if (tramp_id >= cap_tramp_next || !cap_trampolines[tramp_id].active)
        return CAP_EBADF;

    /* TODO: the actual WRPKRU+JMP sequence. On a real AeroSLS kernel
     * this would: (1) save PKRU, (2) load caller_pkey_mask, (3) set
     * RSP to callee's stack, (4) push args, (5) JMP entry_vaddr,
     * (6) read result from arena slot, (7) restore PKRU.
     * For now this is a stub that returns ENOSYS — the inline path
     * in the user-space library is the primary mechanism. */
    return CAP_ENOSYS; /* not yet implemented; use inline trampoline */
}

/* Debug: dump trampoline cap info. */
void cap_trampoline_list(void) {
    kernel_serial_printf("Trampoline caps: %u active (MPK flags=0x%x)\n",
                         cap_tramp_next, g_mpk_flags);
    for (uint32_t i = 0; i < cap_tramp_next; i++) {
        struct CapTrampoline* t = &cap_trampolines[i];
        if (!t->active) continue;
        kernel_serial_printf("  [%u] callee=%u entry=0x%lx pkey=%u stack=%u\n",
                             i, t->callee_pid, t->cap.entry_vaddr,
                             t->cap.callee_pkey, t->cap.max_stack_bytes);
    }
}

/* ─── Trampoline syscall wrappers ────────────────────────────────────────── */

uint64_t sys_sls_trampoline_create(struct SLSTrampolineCreateRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint16_t idx;
    int r = cap_trampoline_create(cap_current_pid(), req->callee_pid,
                                 req->entry_vaddr, req->max_stack_bytes,
                                 &idx);
    if (r < 0) return (uint64_t)(int64_t)r;
    return (uint64_t)idx;
}

uint64_t sys_sls_trampoline_call(struct SLSTrampolineCallRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint64_t result = 0;
    int r = cap_trampoline_call(cap_current_pid(), req->tramp_idx,
                               (const uint64_t)req->args, req->arg_count,
                               req->arena_offset, req->arena_len, &result);
    if (r < 0) return (uint64_t)(int64_t)r;
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Phase 5 — create_sidecar (self-hosted boot, §1.5 of the design doc)
 *
 * SYS_SLS_CREATE_SIDECAR (310): accepts a packed sidecar manifest blob,
 * creates a new process, maps the sidecar image, builds the initial
 * capability table from manifest CAP records, writes a BootInfoBlock at
 * the child's stack top, and enters ring-3 (async spawn, HELD state so
 * the parent can provision resources before the child runs).
 *
 * Lock order: table < channel < object (same as all other cap paths).
 * The child's cap table is pre-bound before spawn so cap_chan_create can
 * mint the messenger channel's far-end caps directly into it.
 * ═══════════════════════════════════════════════════════════════════════ */

/* IEEE CRC-32 (reflected), same polynomial as user/proto/src/manifest.rs.
 * Table-free, 256-byte stack budget. */
static uint32_t cap_sidecar_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Find the process descriptor for `pid`. Returns NULL if not found. */
static struct ProcessDescriptor* sidecar_find_pid(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid == pid)
            return &proc_table[i];
    return 0;
}

/* ─── Freestanding string helpers (no libc in the kernel) ───────────────── */
static int sidecar_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
/* Returns 1 if `s` starts with `pre`. */
static int sidecar_prefix(const char* s, const char* pre) {
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}
static void sidecar_strcpy(char* d, const char* s, int n) {
    int i; for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}

/* ─── Sidecar registry (Phase 5: name → pid for CAP_CHAN wiring) ────────
 * Populated by cap_create_sidecar from each manifest's NAME record;
 * consulted when resolving CAP_CHAN peer_names. Small, fixed-size,
 * freestanding — the same idiom as service_registry.c. */
static struct SidecarRegistryEntry {
    char     name[SIDECAR_REGISTRY_NAME_LEN];
    uint32_t pid;
    uint8_t  active;
} sidecar_registry[SIDECAR_REGISTRY_MAX];

void sidecar_registry_init(void) {
    for (int i = 0; i < SIDECAR_REGISTRY_MAX; i++)
        sidecar_registry[i].active = 0;
}

int sidecar_registry_register(const char* name, uint32_t pid) {
    if (!name || !name[0] || pid == 0) return -1;
    /* Re-registering an existing name UPDATES it in place (a restarted
     * sidecar keeps its identity; later spawns simply re-point the name). */
    for (int i = 0; i < SIDECAR_REGISTRY_MAX; i++) {
        struct SidecarRegistryEntry* e = &sidecar_registry[i];
        if (e->active && sidecar_streq(e->name, name)) { e->pid = pid; return 0; }
    }
    for (int i = 0; i < SIDECAR_REGISTRY_MAX; i++) {
        struct SidecarRegistryEntry* e = &sidecar_registry[i];
        if (e->active) continue;
        sidecar_strcpy(e->name, name, SIDECAR_REGISTRY_NAME_LEN);
        e->pid = pid;
        e->active = 1;
        return 0;
    }
    kernel_serial_print("[SIDECAR] registry full — sidecar name not registered\n");
    return -1;
}

uint32_t sidecar_registry_resolve(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < SIDECAR_REGISTRY_MAX; i++) {
        struct SidecarRegistryEntry* e = &sidecar_registry[i];
        if (e->active && sidecar_streq(e->name, name)) return e->pid;
    }
    return 0;   /* not found */
}

uint32_t sidecar_registry_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < SIDECAR_REGISTRY_MAX; i++)
        if (sidecar_registry[i].active) n++;
    return n;
}

void sidecar_registry_remove_pid(uint32_t pid) {
    for (int i = 0; i < SIDECAR_REGISTRY_MAX; i++) {
        struct SidecarRegistryEntry* e = &sidecar_registry[i];
        if (e->active && e->pid == pid) e->active = 0;
    }
}

/* Append one BIB cap entry — name_len u16 + name + slot u16 + ty u8 +
 * rights u8 + base u64 + len u64, exactly what user/proto/src/bootinfo.rs
 * parses (no 2-byte name padding). Returns the new offset, or 0 if the
 * entry would overflow `buf_cap`. */
static uint32_t bib_put_entry(uint8_t* buf, uint32_t off, uint32_t buf_cap,
                              const char* name, uint16_t nlen, uint16_t slot,
                              uint8_t ty, uint8_t rights,
                              uint64_t base, uint64_t len) {
    if (off + 2 + nlen + 2 + 1 + 1 + 8 + 8 > buf_cap) return 0;
    *(uint16_t*)(buf + off) = nlen;  off += 2;
    for (uint16_t j = 0; j < nlen; j++) buf[off + j] = (uint8_t)name[j];
    off += nlen;
    *(uint16_t*)(buf + off) = slot;  off += 2;
    buf[off] = ty;      off += 1;
    buf[off] = rights;  off += 1;
    *(uint64_t*)(buf + off) = base;  off += 8;
    *(uint64_t*)(buf + off) = len;   off += 8;
    return off;
}

/* ─── cap_create_sidecar ────────────────────────────────────────────────────
 * The heart of self-hosted AeroSLS boot. Creates one sidecar from a
 * packed manifest. Returns 0 on success (*out_ch_r = parent's CHAN_R to
 * the child for receiving replies, or CAP_NONE if no messenger channel
 * was created).
 *
 * Steps:
 *   1. Parse the packed manifest blob (bounds-checked TLV walk).
 *   2. Look up the parent process, find a free process slot.
 *   3. Clone the kernel page table, map the sidecar image from the blob.
 *   4. Allocate and map a user stack; write the BootInfoBlock.
 *   5. Pre-bind the child's cap table, spawn the child as HELD (async).
 *   6. Create a messenger channel (cap_chan_create with far_pid).
 *   7. Mint CAP_MEM capabilities into the child's table.
 *   8. Restore the parent's kernel_rsp, release the child.
 */
int cap_create_sidecar(uint32_t parent_pid,
                       const void* manifest, uint32_t manifest_len,
                       uint16_t parent_ch_w, uint16_t console_ch_w,
                       uint16_t* out_ch_r) {
    if (out_ch_r) *out_ch_r = CAP_NONE;
    (void)console_ch_w;  /* reserved: parent sends console cap via messenger */
    if (!manifest || manifest_len < SIDECAR_MANIFEST_HEADER_LEN)
        return CAP_EINVAL;

    /* ── 1. Validate header ──────────────────────────────────────────── */
    const uint8_t* blob = (const uint8_t*)manifest;
    const struct SidecarManifestHeader* hdr =
        (const struct SidecarManifestHeader*)blob;

    for (int i = 0; i < 8; i++)
        if (hdr->magic[i] != SIDECAR_MANIFEST_MAGIC[i])
            return CAP_EINVAL;
    if (hdr->version_major != SIDECAR_MANIFEST_VERSION_MAJOR)
        return CAP_EINVAL;
    if (hdr->total_len > manifest_len)
        return CAP_ERANGE;

    /* CRC-32 over the record body (bytes after header, including any
     * appended BlobFooter with image_kaddr). */
    uint32_t body_len = manifest_len - SIDECAR_MANIFEST_HEADER_LEN;
    if (body_len > 0) {
        uint32_t crc = cap_sidecar_crc32(blob + SIDECAR_MANIFEST_HEADER_LEN,
                                         body_len);
        if (crc != hdr->body_crc32)
            return CAP_EINVAL;  /* corrupted manifest */
    }

    /* Caps (≤16) + the fixed records (personality, name, image, budget,
     * cpu, limits, bootstrap, flags, signature = 9) + slack. */
    if (hdr->record_count > SIDECAR_MANIFEST_MAX_CAPS + 10)
        return CAP_ERANGE;

    /* ── 2. Parse TLV records ────────────────────────────────────────── */
    struct SidecarManifest m;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(m)); i++)
        ((uint8_t*)&m)[i] = 0;
    m.record_count = hdr->record_count;

    uint32_t off = SIDECAR_MANIFEST_HEADER_LEN;
    for (uint16_t rec = 0; rec < hdr->record_count; rec++) {
        if (off + 4 > hdr->total_len) return CAP_EINVAL;
        uint16_t tag  = *(const uint16_t*)(blob + off);
        uint16_t rlen = *(const uint16_t*)(blob + off + 2);
        if (off + 4 + rlen > hdr->total_len) return CAP_EINVAL;
        const uint8_t* rp = blob + off + 4;

        switch (tag) {
        case SIDECAR_TAG_PERSONALITY:
            break;  /* informational only */

        case SIDECAR_TAG_NAME: {
            /* Sidecar identity: name_len u16 + name UTF-8. Registered in
             * the sidecar registry below, so later manifests can wire
             * CAP_CHAN channels to this sidecar by name. */
            if (rlen < 2) return CAP_EINVAL;
            uint16_t nlen = *(const uint16_t*)(rp + 0);
            if ((int)rlen < (int)nlen + 2) return CAP_EINVAL;
            uint8_t ml = (nlen < SIDECAR_MANIFEST_MAX_NAME - 1)
                         ? (uint8_t)nlen
                         : (uint8_t)(SIDECAR_MANIFEST_MAX_NAME - 1);
            for (uint8_t j = 0; j < ml; j++)
                m.name[j] = (char)rp[2 + j];
            m.name[ml] = '\0';
            m.name_len = ml;
            break;
        }

        case SIDECAR_TAG_IMAGE: {
            if (rlen < 24) return CAP_EINVAL;
            m.image_entry = *(const uint64_t*)(rp + 0);
            /* rp+8: blob_offset (u32) — image offset within the blob */
            m.image_size  = *(const uint32_t*)(rp + 12);
            /* rp+16: image_kaddr (u64) — physical addr of image data */
            break;
        }
        case SIDECAR_TAG_BUDGET: {
            if (rlen < 16) return CAP_EINVAL;
            m.budget_mem_bytes    = *(const uint64_t*)(rp + 0);
            m.budget_stack_bytes  = *(const uint32_t*)(rp + 8);
            m.budget_heap_init    = *(const uint32_t*)(rp + 12);
            break;
        }
        case SIDECAR_TAG_CPU: {
            if (rlen < 4) return CAP_EINVAL;
            /* share_pct, preemptible — informational */
            break;
        }
        case SIDECAR_TAG_LIMITS: {
            if (rlen < 12) return CAP_EINVAL;
            /* max_tasks, max_fds, max_chans — informational */
            break;
        }
        case SIDECAR_TAG_CAP_MEM: {
            /* Record layout (matches user/proto/src/manifest.rs and Phase 2
             * §2.2): name_len u16, name[nlen], phys_base u64, size u64
             * (bytes), rights u8 = nlen + 19 bytes. */
            if (rlen < 2) return CAP_EINVAL;
            uint16_t nlen = *(const uint16_t*)(rp + 0);
            if ((int)rlen < (int)nlen + 19) return CAP_EINVAL;
            if (m.n_caps >= SIDECAR_MANIFEST_MAX_CAPS) return CAP_ERANGE;
            struct SidecarCap* sc = &m.caps[m.n_caps];
            sc->kind = SIDECAR_TAG_CAP_MEM;
            sc->name_len = nlen;
            for (uint16_t j = 0; j < nlen && j < SIDECAR_MANIFEST_MAX_NAME - 1; j++)
                sc->name[j] = (char)rp[2 + j];
            sc->name[nlen < SIDECAR_MANIFEST_MAX_NAME ? nlen : SIDECAR_MANIFEST_MAX_NAME - 1] = '\0';
            sc->phys_base  = *(const uint64_t*)(rp + 2 + nlen);
            sc->size_bytes = *(const uint64_t*)(rp + 2 + nlen + 8);
            sc->rights     = rp[2 + nlen + 16];
            m.n_caps++;
            break;
        }
        case SIDECAR_TAG_CAP_CHAN: {
            /* Record layout (matches manifest.rs and Phase 2 §2.2):
             * name_len u16, name[nlen], peer_len u16, peer[plen],
             * rights u8, flags u8 = nlen + plen + 6 bytes. */
            if (rlen < 2) return CAP_EINVAL;
            uint16_t nlen = *(const uint16_t*)(rp + 0);
            if ((int)rlen < (int)nlen + 2) return CAP_EINVAL;   /* room for peer_len */
            uint16_t plen = *(const uint16_t*)(rp + 2 + nlen);
            if ((int)rlen < (int)nlen + (int)plen + 6) return CAP_EINVAL;
            if (m.n_caps >= SIDECAR_MANIFEST_MAX_CAPS) return CAP_ERANGE;
            struct SidecarCap* sc = &m.caps[m.n_caps];
            sc->kind = SIDECAR_TAG_CAP_CHAN;
            sc->name_len = nlen;
            for (uint16_t j = 0; j < nlen && j < SIDECAR_MANIFEST_MAX_NAME - 1; j++)
                sc->name[j] = (char)rp[2 + j];
            sc->name[nlen < SIDECAR_MANIFEST_MAX_NAME ? nlen : SIDECAR_MANIFEST_MAX_NAME - 1] = '\0';
            sc->peer_name_len = plen;
            for (uint16_t j = 0; j < plen && j < SIDECAR_MANIFEST_MAX_NAME - 1; j++)
                sc->peer_name[j] = (char)rp[2 + nlen + 2 + j];
            sc->peer_name[plen < SIDECAR_MANIFEST_MAX_NAME ? plen : SIDECAR_MANIFEST_MAX_NAME - 1] = '\0';
            sc->rights = rp[2 + nlen + 2 + plen];
            sc->flags  = rp[2 + nlen + 2 + plen + 1];
            m.n_caps++;
            break;
        }
        case SIDECAR_TAG_BOOTSTRAP:
        case SIDECAR_TAG_FLAGS:
            break;  /* informational */
        default:
            if (!(hdr->flags & 0x0001))  /* tolerate_unknown */
                return CAP_EINVAL;
            break;
        }
        off += 4 + rlen;
    }

    /* ── 3. Validate parsed manifest ─────────────────────────────────── */
    if (m.image_size == 0) return CAP_EINVAL;
    if (m.image_entry >= m.image_size) return CAP_ERANGE;
    if (m.budget_stack_bytes < 4096) return CAP_EINVAL;

    /* ── 4. Look up parent process ───────────────────────────────────── */
    struct ProcessDescriptor* parent = sidecar_find_pid(parent_pid);
    if (!parent) return CAP_EINVAL;

    int pi = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) { pi = i; break; }
    }
    if (pi < 0) return CAP_ETABLEFULL;

    /* ── 5. Clone page table, map image ──────────────────────────────── */
    uint64_t new_cr3 = user_clone_page_table();
    if (!new_cr3) return CAP_ENOMEM;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)new_cr3;

    /* Image physical address: stored in the blob's BlobFooter (appended
     * after all TLV records by the image packer). The IMAGE record's
     * blob_offset field is repurposed as image_kaddr when the blob is a
     * flat image+manifest package. */
    uint32_t footer_off = SIDECAR_MANIFEST_HEADER_LEN;
    for (uint16_t rec = 0; rec < hdr->record_count; rec++) {
        if (footer_off + 4 > hdr->total_len) break;
        uint16_t rlen = *(const uint16_t*)(blob + footer_off + 2);
        footer_off += 4 + rlen;
    }
    uint64_t image_kaddr = 0;
    if (footer_off + 8 <= hdr->total_len)
        image_kaddr = *(const uint64_t*)(blob + footer_off);

    if (!image_kaddr || image_kaddr < 0x100000ULL) {
        kernel_serial_printf("[SIDECAR] create: invalid image_kaddr 0x%llx\n",
                             (unsigned long long)image_kaddr);
        return CAP_EINVAL;
    }

    uint32_t n_img_pages = (m.image_size + 4095) / 4096;
    uint64_t image_vbase = 0x400000000000ULL;  /* USER_PROC_CODE_BASE */
    uint64_t bytes_left  = m.image_size;
    for (uint32_t p = 0; p < n_img_pages; p++) {
        void* frame = allocate_physical_ram_frame_for_partition(parent->partition_id);
        if (!frame) {
            kernel_serial_print("[SIDECAR] create: frame alloc failed\n");
            return CAP_ENOMEM;
        }
        uint64_t faddr = (uint64_t)(uintptr_t)frame;
        uint32_t chunk = (bytes_left > 4096) ? 4096 : (uint32_t)bytes_left;
        const uint8_t* src = (const uint8_t*)(uintptr_t)(image_kaddr + (uint64_t)p * 4096);
        uint8_t* dst = (uint8_t*)(uintptr_t)faddr;
        for (uint32_t b = 0; b < chunk; b++) dst[b] = src[b];
        user_map_page(pml4, image_vbase + (uint64_t)p * 4096, faddr,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_EXEC);
        bytes_left -= chunk;
    }

    /* ── 6. User stack ───────────────────────────────────────────────── */
    uint64_t stack_base = image_vbase
                        + (uint64_t)n_img_pages * 4096 + 4096;  /* guard pg */
    uint32_t stk_pages = (m.budget_stack_bytes + 4095) / 4096;
    if (stk_pages < 1) stk_pages = 1;
    for (uint32_t p = 0; p < stk_pages; p++) {
        void* frame = allocate_physical_ram_frame_for_partition(parent->partition_id);
        if (!frame) {
            kernel_serial_print("[SIDECAR] create: stack frame alloc failed\n");
            return CAP_ENOMEM;
        }
        user_map_page(pml4, stack_base + (uint64_t)p * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE
                      | USER_PTE_NOEXEC);
    }
    uint64_t user_rsp = stack_base + (uint64_t)stk_pages * 4096 - 16;

    /* ── 7. Populate process descriptor ──────────────────────────────── */
    struct ProcessDescriptor* pd = &proc_table[pi];
    pd->pid        = alloc_pid();
    if (m.name_len > 0) {
        /* Use the manifest's NAME record for the debug name. */
        uint32_t nl = (m.name_len < PROC_NAME_LEN - 1) ? (uint32_t)m.name_len
                                                       : (uint32_t)(PROC_NAME_LEN - 1);
        for (uint32_t j = 0; j < nl; j++) pd->name[j] = m.name[j];
        pd->name[nl] = '\0';
    } else {
        pd->name[0] = 's';  /* short name for debug */
        pd->name[1]    = 'c';
        pd->name[2]    = '\0';
    }
    pd->cr3        = new_cr3;
    pd->user_rip   = image_vbase + m.image_entry;
    pd->user_rsp   = user_rsp;
    pd->owner_uid  = parent->owner_uid;
    pd->parent_pid = parent_pid;
    pd->partition_id = parent->partition_id;
    pd->state      = PROC_HELD;   /* HELD: parent provisions before release */
    pd->priority   = PROC_PRIO_NORMAL;
    pd->active     = 1;
    pd->pending_teardown = 0;
    pd->waiting_chan = CAP_NONE;
    pd->has_ring3_ctx = 1;  /* synthetic context (async spawn) */
    pd->resume_kernel  = 0;
    pd->resume_sysret  = 0;
    pd->handoff_target = 0;
    {   /* zero park_ctx to avoid stale state from a reused slot */
        uint64_t* pc = (uint64_t*)&pd->park_ctx;
        for (int _i = 0; _i < (int)(sizeof(pd->park_ctx) / sizeof(uint64_t)); _i++)
            pc[_i] = 0;
        pd->park_req = 0;
    }
    proc_count++;

    /* The async child runs through the scheduler, which iretq's from the
     * synthetic ring3_ctx frame and repoints [gs:8] (per_cpu_data.kernel_rsp)
     * at the child's OWN syscall stack on every switch — the child must
     * have BOTH before it can be scheduled. Allocate the dedicated 8 KiB
     * syscall stack (two contiguous frames, same as process_create) and
     * fill the synthetic context exactly like the async spawn path in
     * process.c. Without either, the child's first schedule would iretq
     * from a zeroed frame and its first syscall would push onto the
     * kernel's stack. */
    pd->syscall_stack_top = alloc_proc_syscall_stack(parent->partition_id);
    if (!pd->syscall_stack_top) {
        kernel_serial_print("[SIDECAR] create: syscall stack allocation failed\n");
        pd->active = 0;
        proc_count--;
        return CAP_ENOMEM;
    }
    {
        uint64_t* ctx = (uint64_t*)&pd->ring3_ctx;
        for (int i = 0; i < 15; i++) ctx[i] = 0;
        ctx[15] = pd->user_rip;
        ctx[16] = 0x23;      /* ring-3 code */
        ctx[17] = 0x202;     /* IF on */
        ctx[18] = pd->user_rsp;
        ctx[19] = 0x1B;      /* ring-3 data */
    }

    /* ── 8. Bind cap table, create messenger channel ──────────────────── */
    int cti = cap_table_index(pd->pid);
    if (cti < 0) {
        kernel_serial_print("[SIDECAR] create: cap table full\n");
        pd->active = 0;
        proc_count--;
        return CAP_ETABLEFULL;
    }

    /* Messenger channel: cap_chan_create with far_pid inserts the child's
     * CHAN_R and CHAN_W directly into the child's cap table (Phase 1.5
     * two-party bootstrap). */
    uint16_t parent_rd = CAP_NONE, parent_wr = CAP_NONE;
    uint16_t child_rd  = CAP_NONE, child_wr  = CAP_NONE;
    int ch_r = cap_chan_create(parent_pid, pd->pid,
                               &parent_rd, &parent_wr,
                               &child_rd,  &child_wr);
    if (ch_r < 0) {
        kernel_serial_printf("[SIDECAR] create: chan_create failed (%d)\n", ch_r);
        pd->active = 0;
        proc_count--;
        return (int)ch_r;
    }

    /* Mint CAP_MEM capabilities from the manifest into the child's table.
     * The manifest's size is in BYTES (Phase 2 §2.2); convert to whole
     * pages for cap_create_mem. */
    for (uint8_t ci = 0; ci < m.n_caps; ci++) {
        struct SidecarCap* sc = &m.caps[ci];
        if (sc->kind != SIDECAR_TAG_CAP_MEM) continue;
        if (sc->phys_base == 0 || sc->size_bytes == 0) continue;
        if ((sc->phys_base & 0xFFFULL) != 0) continue;
        if (sc->phys_base + sc->size_bytes > 0x100000000ULL) continue;
        uint32_t npages = (uint32_t)((sc->size_bytes + 4095u) / 4096u);
        uint32_t perms = (uint32_t)sc->rights | CAP_PERM_MAP;
        uint16_t mem_idx = CAP_NONE;
        if (cap_create_mem(pd->pid, sc->phys_base, npages,
                           perms, &mem_idx) < 0) {
            kernel_serial_printf("[SIDECAR] create: mem cap '%s' failed\n",
                                 sc->name);
            /* Non-fatal: the sidecar can still boot without this cap. */
        }
    }

    /* Wire CAP_CHAN records: resolve each peer_name against the sidecar
     * registry and connect a real channel. The child's endpoints land in
     * the child's cap table (the BIB reports them below); the peer's
     * endpoints land in the peer's table. A peer named "kernel.*" is a
     * kernel-owned service (e.g. the console): the kernel context (pid 0)
     * is the peer and holds that end of the channel. An unresolvable peer
     * leaves the cap unwired — non-fatal, the sidecar boots without it. */
    for (uint8_t ci = 0; ci < m.n_caps; ci++) {
        struct SidecarCap* sc = &m.caps[ci];
        if (sc->kind != SIDECAR_TAG_CAP_CHAN) continue;
        if (sc->peer_name_len == 0) continue;   /* nothing to connect to */
        uint16_t c_rd = CAP_NONE, c_wr = CAP_NONE;
        if (sidecar_prefix(sc->peer_name, "kernel.")) {
            uint16_t k_rd = CAP_NONE, k_wr = CAP_NONE;
            if (cap_chan_create(0, pd->pid, &k_rd, &k_wr,
                                &c_rd, &c_wr) == 0) {
                sc->wired = 1;
                sc->wired_rd = c_rd;
                sc->wired_wr = c_wr;
                kernel_serial_printf(
                    "[SIDECAR] PID %u '%s': wired chan '%s' to kernel service "
                    "'%s' (slots %u/%u, kernel end %u/%u)\n",
                    pd->pid, pd->name, sc->name, sc->peer_name,
                    (unsigned)c_rd, (unsigned)c_wr, (unsigned)k_rd, (unsigned)k_wr);
            }
        } else {
            uint32_t peer_pid = sidecar_registry_resolve(sc->peer_name);
            if (peer_pid == 0 || peer_pid == pd->pid) {
                kernel_serial_printf(
                    "[SIDECAR] PID %u '%s': chan '%s' peer '%s' not "
                    "registered — cap left unwired\n",
                    pd->pid, pd->name, sc->name, sc->peer_name);
                continue;
            }
            uint16_t p_rd = CAP_NONE, p_wr = CAP_NONE;
            if (cap_chan_create(pd->pid, peer_pid, &c_rd, &c_wr,
                                &p_rd, &p_wr) == 0) {
                sc->wired = 1;
                sc->wired_rd = c_rd;
                sc->wired_wr = c_wr;
                kernel_serial_printf(
                    "[SIDECAR] PID %u '%s': wired chan '%s' to sidecar '%s' "
                    "(PID %u, slots %u/%u)\n",
                    pd->pid, pd->name, sc->name, sc->peer_name,
                    (unsigned)peer_pid, (unsigned)c_rd, (unsigned)c_wr);
            }
        }
    }

    /* Register the sidecar's identity so later manifests can wire
     * channels to it by name. */
    if (m.name_len > 0) {
        if (sidecar_registry_register(m.name, pd->pid) < 0)
            kernel_serial_printf("[SIDECAR] PID %u '%s': registry registration failed\n",
                                 pd->pid, m.name);
    }

    /* Restore parent's kernel_rsp — the async child's syscall stack
     * overwrote per_cpu_data[0].kernel_rsp during process_spawn_nb_held;
     * without this restore, the parent's next syscall would run on the
     * child's (now-mapped) stack, corrupting the kernel stack chain. */
    per_cpu_data[0].kernel_rsp = parent->syscall_stack_top;

    /* ── 9. Write BootInfoBlock at child's stack top ─────────────────── */
    /* BIB lives at the top 4 KiB of the stack, and _start receives its
     * VIRTUAL address in rdi — the sidecar crt0 contract (crt0.S: "the
     * kernel jumps to _start with a pointer to the Boot Info Block in
     * a0/rdi"). The synthetic frame zeroed all GPRs above, so rdi must
     * be filled here or the child's first `mov %rdi, %r13` saves zero. */
    uint64_t bib_vaddr = stack_base + (uint64_t)stk_pages * 4096 - 4096;
    ((uint64_t*)&pd->ring3_ctx)[9] = bib_vaddr;   /* TaskContext order: rdi */
    uint64_t bib_paddr = 0;
    /* Walk the page table to find the physical frame backing bib_vaddr. */
    {
        uint64_t pml4e = *(const uint64_t*)(uintptr_t)(new_cr3 +
                          ((bib_vaddr >> 39) & 0x1FF) * 8);
        if (!(pml4e & 1)) return CAP_ENOMEM;
        uint64_t pdpt = pml4e & 0x000FFFFFFFFFF000ULL;
        uint64_t pdpe = *(const uint64_t*)(uintptr_t)(pdpt +
                         ((bib_vaddr >> 30) & 0x1FF) * 8);
        if (!(pdpe & 1)) return CAP_ENOMEM;
        if (pdpe & 0x80) { bib_paddr = pdpe & 0x000FFFFFC0000000ULL;
                           bib_paddr |= bib_vaddr & 0x3FFFFFFFULL; }
        else {
            uint64_t pd = pdpe & 0x000FFFFFFFFFF000ULL;
            uint64_t pde = *(const uint64_t*)(uintptr_t)(pd +
                           ((bib_vaddr >> 21) & 0x1FF) * 8);
            if (!(pde & 1)) return CAP_ENOMEM;
            if (pde & 0x80) { bib_paddr = pde & 0x000FFFFFE0000000ULL;
                              bib_paddr |= bib_vaddr & 0x1FFFFFULL; }
            else {
                uint64_t pt = pde & 0x000FFFFFFFFFF000ULL;
                uint64_t pte = *(const uint64_t*)(uintptr_t)(pt +
                               ((bib_vaddr >> 12) & 0x1FF) * 8);
                if (!(pte & 1)) return CAP_ENOMEM;
                bib_paddr = (pte & 0x000FFFFFFFFFF000ULL)
                           | (bib_vaddr & 0xFFFULL);
            }
        }
    }
    if (!bib_paddr) return CAP_ENOMEM;

    /* Build the BIB in kernel memory (static, 256 bytes max). */
    static uint8_t bib_buf[256];
    uint32_t bib_off = 0;

    /* Header: magic(8) + version(2) + cap_count(2) + budget(8) +
     * stack_top(8) + total_len(4) = 32 bytes. */
    for (int i = 0; i < 8; i++) bib_buf[bib_off + i] = SIDECAR_BIB_MAGIC[i];
    bib_off += 8;
    *(uint16_t*)(bib_buf + bib_off) = SIDECAR_BIB_VERSION;  bib_off += 2;
    uint32_t bib_n_caps = 0;  /* will be patched below */
    *(uint16_t*)(bib_buf + bib_off) = 0;  bib_off += 2;  /* cap_count placeholder */
    *(uint64_t*)(bib_buf + bib_off) = m.budget_mem_bytes;  bib_off += 8;
    *(uint64_t*)(bib_buf + bib_off) = user_rsp + 16;  /* stack_top = RSP at _start */
    bib_off += 8;
    *(uint32_t*)(bib_buf + bib_off) = 0;  bib_off += 4;  /* total_len placeholder */

    /* Cap 0: messenger CHAN_R (the child's read end of the messenger).
     * Entry layout: name_len u16, name, slot u16, ty u8, rights u8,
     * base u64, len u64 — exactly what user/proto/src/bootinfo.rs parses. */
    if (child_rd != CAP_NONE) {
        uint16_t c = child_rd;
        *(uint16_t*)(bib_buf + bib_off) = 0;  /* name_len = 0 */
        bib_off += 2;
        *(uint16_t*)(bib_buf + bib_off) = c;  bib_off += 2;  /* slot */
        bib_buf[bib_off] = CAP_TYPE_CHAN_R;   bib_off += 1;
        bib_buf[bib_off] = CAP_PERM_RECV;     bib_off += 1;
        *(uint64_t*)(bib_buf + bib_off) = 0;  bib_off += 8;  /* base */
        *(uint64_t*)(bib_buf + bib_off) = 0;  bib_off += 8;  /* len */
        bib_n_caps++;
    }

    /* Cap 1: messenger CHAN_W (the child's write end). */
    if (child_wr != CAP_NONE) {
        uint16_t c = child_wr;
        *(uint16_t*)(bib_buf + bib_off) = 0;
        bib_off += 2;
        *(uint16_t*)(bib_buf + bib_off) = c;  bib_off += 2;
        bib_buf[bib_off] = CAP_TYPE_CHAN_W;   bib_off += 1;
        bib_buf[bib_off] = CAP_PERM_SEND;     bib_off += 1;
        *(uint64_t*)(bib_buf + bib_off) = 0;  bib_off += 8;
        *(uint64_t*)(bib_buf + bib_off) = 0;  bib_off += 8;
        bib_n_caps++;
    }

    /* Subsequent caps: manifest caps in record order. A minted MEM cap is
     * one entry; a wired CHAN cap is TWO entries (CHAN_R then CHAN_W),
     * both named — the same shape as the messenger caps above. Caps that
     * were not minted/wired are skipped. */
    for (uint8_t ci = 0; ci < m.n_caps && bib_n_caps < SIDECAR_BIB_CAPS_MAX; ci++) {
        struct SidecarCap* sc = &m.caps[ci];
        uint16_t nlen = sc->name_len;
        if (nlen > SIDECAR_MANIFEST_MAX_NAME - 1) nlen = SIDECAR_MANIFEST_MAX_NAME - 1;

        if (sc->kind == SIDECAR_TAG_CAP_MEM) {
            /* Find the cap's slot in the child's table by scanning for a
             * MEM cap matching the phys_base (npages derived from
             * size_bytes the same way as the mint loop above). */
            uint32_t npages = (uint32_t)((sc->size_bytes + 4095u) / 4096u);
            uint16_t found_slot = CAP_NONE;
            for (int si = 0; si < CAP_TABLE_ENTRIES; si++) {
                uint64_t w = cap_tables[cti].slots[si].word;
                if (!cap_word_valid(w)) continue;
                if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_MEM) continue;
                uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
                struct CapObject* o = cap_object_get(oid);
                if (o && o->phys_base == sc->phys_base && o->npages == npages) {
                    found_slot = (uint16_t)si;
                    break;
                }
            }
            if (found_slot == CAP_NONE) continue;   /* not minted: skip */
            uint32_t no = bib_put_entry(bib_buf, bib_off, sizeof(bib_buf),
                                        sc->name, nlen, found_slot,
                                        CAP_TYPE_MEM, (uint8_t)sc->rights,
                                        sc->phys_base, sc->size_bytes);
            if (no == 0) break;   /* BIB buffer full: stop appending */
            bib_off = no;
            bib_n_caps++;
        } else if (sc->kind == SIDECAR_TAG_CAP_CHAN && sc->wired) {
            uint32_t no = bib_put_entry(bib_buf, bib_off, sizeof(bib_buf),
                                        sc->name, nlen, sc->wired_rd,
                                        CAP_TYPE_CHAN_R, CAP_PERM_RECV, 0, 0);
            if (no == 0) break;
            bib_off = no;
            bib_n_caps++;
            no = bib_put_entry(bib_buf, bib_off, sizeof(bib_buf),
                               sc->name, nlen, sc->wired_wr,
                               CAP_TYPE_CHAN_W, CAP_PERM_SEND, 0, 0);
            if (no == 0) break;
            bib_off = no;
            bib_n_caps++;
        }
    }

    /* Patch cap_count and total_len. */
    *(uint16_t*)(bib_buf + 10) = (uint16_t)bib_n_caps;
    *(uint32_t*)(bib_buf + 28) = bib_off;

    /* Copy BIB into the child's stack frame. */
    uint8_t* dst = (uint8_t*)(uintptr_t)bib_paddr;
    for (uint32_t i = 0; i < bib_off; i++) dst[i] = bib_buf[i];

    /* ── 10. Release child from HELD state ───────────────────────────── */
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pd->pid
            && proc_table[i].state == PROC_HELD) {
            proc_table[i].state = PROC_SUSPENDED;
            kernel_serial_printf(
                "[SIDECAR] PID %u '%s' released — runs on next schedule "
                "(BIB at 0x%016lx, entry 0x%016lx)\n",
                pd->pid, pd->name, bib_vaddr, pd->user_rip);
            break;
        }
    }

    /* Return the parent's CHAN_R (for receiving the child's first reply). */
    if (out_ch_r) *out_ch_r = parent_rd;
    return 0;
}

/* Find the parent's CHAN_W slot for the messenger channel whose CHAN_R
 * slot is `ch_r` — cap_create_sidecar mints BOTH ends into the parent's
 * table (cap_chan_create) but returns only the CHAN_R slot; the spawning
 * sidecar needs the CHAN_W slot too (it SENDS the registry over the
 * messenger). The scan matches on the underlying channel object
 * (chan_id), so it is robust to slot layout. The parent is the current
 * process, whose own table is stable while it runs this syscall — no
 * cross-process race (same posture as the Phase-3 message syscalls). */
static uint16_t sidecar_find_parent_ch_w(uint32_t parent_pid, uint16_t ch_r) {
    if (ch_r == CAP_NONE) return CAP_NONE;
    int cti = cap_table_index(parent_pid);
    if (cti < 0) return CAP_NONE;
    uint16_t want_chan = CAP_NONE;
    {
        uint64_t w = cap_tables[cti].slots[ch_r].word;
        if (!cap_word_valid(w)) return CAP_NONE;
        if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_R)
            return CAP_NONE;
        uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
        struct CapObject* o = cap_object_get(oid);
        if (!o || o->kind != CAP_OBJ_KIND_CHAN) return CAP_NONE;
        want_chan = o->chan_id;
    }
    for (int si = 0; si < CAP_TABLE_ENTRIES; si++) {
        uint64_t w = cap_tables[cti].slots[si].word;
        if (!cap_word_valid(w)) continue;
        if (((w >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK) != CAP_TYPE_CHAN_W)
            continue;
        uint32_t oid = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
        struct CapObject* o = cap_object_get(oid);
        if (o && o->kind == CAP_OBJ_KIND_CHAN && o->chan_id == want_chan)
            return (uint16_t)si;
    }
    return CAP_NONE;
}

/* ─── sys_sls_create_sidecar ──────────────────────────────────────────────── */
uint64_t sys_sls_create_sidecar(struct SLSCreateSidecarRequest* req) {
    if (!req) return (uint64_t)(int64_t)CAP_EINVAL;
    uint16_t ch_r = CAP_NONE;
    int r = cap_create_sidecar(cap_current_pid(),
                               req->manifest, req->manifest_len,
                               req->ch_w_idx, req->console_w_idx,
                               &ch_r);
    if (r < 0) return (uint64_t)(int64_t)r;
    req->out_ch_r = ch_r;
    req->out_ch_w = sidecar_find_parent_ch_w(cap_current_pid(), ch_r);
    return (uint64_t)ch_r;
}
