/*
 * cap.h — AeroSLS Seed Kernel capability layer (Phase 1).
 *
 * Implements docs/AeroSLS-Capability-SDK-Phase1-Seed-Kernel-Design-v0.1.md:
 * per-process capability tables with fd-like small-integer indices, three
 * capability types (CAP_MEM, CAP_CHAN_R, CAP_CHAN_W), kernel-managed
 * bidirectional channels that transfer capabilities atomically, a shared-
 * memory arena, and immediate revocation with a total post-return guarantee.
 *
 * Self-contained by design: this header pulls in only <stdint.h>, so both
 * kernel/process.c and arch/x86/user_paging.c can include it without
 * dragging the rest of the kernel along, and host tests can link cap.c
 * against stubs (see tests/cap_lifecycle_host_test.c).
 */
#ifndef CAP_H
#define CAP_H

#include <stdint.h>

/* ─── Capability word (64 bits) ─────────────────────────────────────────────
 *   63    61 60    57 56          40 39       32 31          16 15          4 3  0
 * +--------+--------+--------------+----------+--------------+-------------+-----+
 * | type   | state  | object id    | perms    | offset (pg)  | len (pg)    |rsrvd|
 * +--------+--------+--------------+----------+--------------+-------------+-----+
 * type 0 (NONE) marks a free slot. A capability word is validated on every
 * use; the reserved bits must be zero, which is the software "tag" that
 * makes forging a word from scratch detectable (same idea as tools/simi's
 * capability-tag regions, enforced here by kernel validation). */
#define CAP_TYPE_SHIFT   61
#define CAP_TYPE_MASK    0x7ULL
#define CAP_STATE_SHIFT  57
#define CAP_STATE_MASK   0xFULL
#define CAP_OBJ_SHIFT    40
#define CAP_OBJ_MASK     0x1FFFFULL
#define CAP_PERM_SHIFT   32
#define CAP_PERM_MASK    0xFFULL
#define CAP_OFF_SHIFT    16
#define CAP_OFF_MASK     0xFFFFULL
#define CAP_LEN_SHIFT    4
#define CAP_LEN_MASK     0xFFFULL
#define CAP_RSVD_MASK    0xFULL

#define CAP_TYPE_NONE    0
#define CAP_TYPE_MEM     1
#define CAP_TYPE_CHAN_R  2
#define CAP_TYPE_CHAN_W  3

#define CAP_STATE_VALID       0
#define CAP_STATE_IN_TRANSIT  1   /* reserved for Phase-2 blocked send */
#define CAP_STATE_REVOKED     2   /* stamped into queued words by revoke */

/* MEM permissions. CHAN caps reuse the same bits with different meaning:
 * bit 0 = SEND (CHAN_W), bit 1 = RECV (CHAN_R) — see chan_create(). */
#define CAP_PERM_R     0x01
#define CAP_PERM_W     0x02
#define CAP_PERM_X     0x04
#define CAP_PERM_MAP   0x08
#define CAP_PERM_SEND  0x01
#define CAP_PERM_RECV  0x02

#define CAP_NONE 0xFFFF   /* "no capability" sentinel (slot index / syscall result) */

/* ─── Limits ──────────────────────────────────────────────────────────────── */
#define CAP_TABLE_MAX        16    /* == PROC_MAX; one table per process + kernel ctx */
#define CAP_TABLE_ENTRIES    512   /* fd-like index space per process */
#define CAP_MAP_MAX          64    /* map records per process */
#define CAP_OBJECT_MAX       1024  /* object table; monotonic, never recycled (Phase 1) */
#define CAP_HOLDER_MAX       8192  /* holder-pool nodes (slots + queued caps) */
#define CAP_CHAN_MAX         64    /* channel objects */
#define CHAN_QUEUE_DEPTH     16    /* messages per directional queue */

#define CAP_ARENA_SIZE       (64u * 1024u * 1024u)
#define CAP_ARENA_FRAMES     (CAP_ARENA_SIZE / 4096u)
#define CAP_VIEW_MAX_PAGES   4096  /* 12-bit len field in the cap word */

/* ─── Errors (errno-style negative codes, widened through do_syscall's ABI) ─ */
#define CAP_EINVAL      (-1)
#define CAP_EBADF       (-2)
#define CAP_EAGAIN      (-3)
#define CAP_ETABLEFULL  (-4)
#define CAP_ECAPREVOKED (-5)
#define CAP_EALREADY    (-6)
#define CAP_ENOMEM      (-7)
#define CAP_ENOSPC      (-8)
#define CAP_ERANGE      (-9)
#define CAP_ENOSYS      (-10)
#define CAP_ECONFLICT   (-11)

/* ─── Data structures ─────────────────────────────────────────────────────── */

struct CapSpinlock {
    volatile uint32_t v;
};

struct ChanMsg {              /* 24 bytes — fixed, preallocated in the channel */
    uint64_t cookie;          /* user-defined tag */
    uint64_t cap_word;        /* the MOVED capability word, or 0 when !has_cap */
    uint8_t  has_cap;
    uint8_t  _pad[7];
};

/* One directional queue per end. q[0] carries messages bound FOR end0,
 * q[1] for end1. end0's CHAN_W enqueues q[1]; end1's CHAN_W enqueues q[0]. */
struct CapChannel {
    struct CapSpinlock lock;
    uint16_t end0_pid;        /* creator; end1 is whoever owns the far-end caps */
    uint16_t _pad0;
    uint32_t qhead[2];
    uint32_t qtail[2];        /* monotonically growing; entries are qtail%DEPTH */
    uint32_t qdepth[2];
    struct ChanMsg q[2][CHAN_QUEUE_DEPTH];
    uint8_t  active;
    uint8_t  _pad[3];
};

/* Holder-pool node. Every place that can reach an object (a VALID slot in a
 * process table, or a VALID cap word sitting in a channel queue) has exactly
 * one holder node; obj->refcount == number of VALID holders. The doubly
 * linked list per object is the MDB-style holder walk revocation needs. */
#define HOLDER_SLOT  0
#define HOLDER_QUEUE 1

struct CapHolder {
    uint32_t obj_id;
    uint16_t next;            /* pool index, 0xFFFF = none */
    uint16_t prev;
    uint16_t pid;             /* SLOT: owning pid; QUEUE: channel slot id */
    uint16_t ref;             /* SLOT: slot index; QUEUE: queue entry index */
    uint8_t  kind;
    uint8_t  qdir;            /* QUEUE: 0/1 */
    uint8_t  active;
    uint8_t  _pad[1];
};                            /* 16 bytes */

struct CapObject {
    struct CapSpinlock lock;
    uint32_t id;              /* == index into cap_objects[] (Phase 1: monotonic) */
    uint8_t  kind;            /* CAP_TYPE_MEM / CAP_TYPE_CHAN_R? no: CAP_MEM or CHAN */
    uint8_t  revoking;        /* set at the revoke linearization point */
    uint8_t  active;
    uint8_t  _pad;
    uint16_t holder_head;     /* first holder node, 0xFFFF = none */
    uint32_t refcount;        /* == # VALID holders */
    uint32_t max_perms;       /* object-wide permission ceiling */
    uint64_t phys_base;       /* MEM: physical address of page 0 */
    uint32_t npages;          /* MEM: object length in pages */
    uint16_t chan_id;         /* CHAN: index into cap_channels[] */
    uint8_t  _pad2[6];
};

#define CAP_OBJ_KIND_MEM   1
#define CAP_OBJ_KIND_CHAN  2

struct CapSlot {
    uint64_t word;            /* the capability word; type NONE == free */
};

struct CapMap {               /* one record per mapping derived from a cap */
    uint32_t obj_id;          /* keyed by OBJECT, not slot: slots get reused */
    uint16_t cap_slot;
    uint16_t npages;
    uint64_t vaddr;
    uint8_t  active;
    uint8_t  _pad[7];
};

struct CapTable {
    struct CapSpinlock lock;
    uint32_t pid;             /* pid bound to this table (0 = kernel context) */
    uint16_t freelist_head;   /* CAP_FREELIST_END when empty */
    uint16_t _pad;
    struct CapSlot slots[CAP_TABLE_ENTRIES];   /* 512 * 8 B = 4 KiB */
    struct CapMap  maps[CAP_MAP_MAX];          /* 64 * 24 B */
};

extern struct CapTable cap_tables[CAP_TABLE_MAX];   /* defined in cap.c */

/* ─── Syscall numbers (289-297 — next free range after SYS_SLS_RECONCILE_
 * ENABLE = 288; confirmed via grep across every kernel header defining
 * SYS_SLS_* before picking these, per this repo's convention) ──────────── */
#define SYS_SLS_CAP_CREATE_MEM   289
#define SYS_SLS_CAP_ARENA_ALLOC  290
#define SYS_SLS_CHAN_CREATE      291
#define SYS_SLS_CAP_SEND         292
#define SYS_SLS_CAP_RECV         293
#define SYS_SLS_CAP_REVOKE       294
#define SYS_SLS_CAP_MAP          295
#define SYS_SLS_CAP_UNMAP        296
#define SYS_SLS_CAP_LIST         297

/* ─── Request structs (single opaque arg through do_syscall, repo ABI) ───── */
struct SLSCapCreateMemRequest {
    uint64_t phys_base;
    uint32_t npages;
    uint32_t perm;
};

struct SLSCapArenaAllocRequest {
    uint32_t npages;
    uint32_t perm;
};

/* chan_create provisions BOTH endpoints. With far_pid == 0 the far end's
 * two caps are minted into the CALLER's table (endpoint handoff by
 * capability transfer over an already-established channel); with far_pid
 * nonzero the far end's caps are minted directly into that process's table
 * (kernel-mediated provisioning, socket-pair style — the concrete Phase-1
 * bootstrap). Either way, four caps exist; the out_* fields report their
 * indices. */
struct SLSCapChanCreateRequest {
    uint32_t far_pid;         /* 0 = mint far caps into the caller's table */
    uint16_t out_rd;          /* [out] caller's read end  (CHAN_R) */
    uint16_t out_wr;          /* [out] caller's write end (CHAN_W) */
    uint16_t out_far_rd;      /* [out] far end's read end  (CHAN_R) */
    uint16_t out_far_wr;      /* [out] far end's write end (CHAN_W) */
    uint8_t  _pad[6];
};

struct SLSCapSendRequest {
    uint16_t ch_w_idx;        /* caller's CHAN_W slot */
    uint16_t cap_idx;         /* cap to MOVE, or CAP_NONE for a plain message */
    uint8_t  _pad[4];
    uint64_t cookie;
};

struct SLSCapRecvRequest {
    uint16_t ch_r_idx;        /* caller's CHAN_R slot */
    uint8_t  block;           /* 1 = park the process mid-syscall when empty */
    uint8_t  _pad[1];
    uint32_t release_pid;     /* Phase 1.5 (immediate wake): best-effort
                               * process_release() BEFORE parking — the
                               * released (HELD) process then runs while we
                               * are parked, with no user-mode window between
                               * the release and the park for a timer to
                               * schedule it first. 0 = none. */
    uint64_t cookie;          /* [out] */
};

struct SLSCapRevokeRequest {
    uint16_t cap_idx;
    uint8_t  _pad[6];
};

struct SLSCapMapRequest {
    uint16_t cap_idx;
    uint8_t  _pad[6];
    uint64_t vaddr;
    uint32_t flags;           /* CAP_PERM_R|W|X applied to the mapping */
    uint8_t  _pad2[4];
};

struct SLSCapUnmapRequest {
    uint16_t cap_idx;
    uint8_t  _pad[6];
    uint64_t vaddr;
};

/* ─── Public API ──────────────────────────────────────────────────────────── */

void cap_init(void);          /* boot: arena carve, table/holder/object setup */

/* Phase 2 teardown: reclaim EVERYTHING pid's capability table owns — drop
 * all its slot holders (destroying objects at refcount 0, which returns
 * arena frames / deactivates channels), drain channel queues bound for its
 * read ends (revoking caps nobody will ever recv), unmap its cap-derived
 * PTEs, and UNBIND the table so the pid→table slot is reusable by a later
 * process. Called from process_exit()/process_kill() AFTER the process is
 * marked ZOMBIE but BEFORE it is marked inactive (cap_proc_cr3() needs
 * active=1 to find its PML4 for the unmap walk). No-op when pid never used
 * capabilities (no table was ever bound). */
void cap_table_teardown(uint32_t pid);

/* Core lifecycle. All return 0 on success (or a cap index — see each
 * comment), a negative CAP_E* on failure. `pid` is the caller. */
int cap_create_mem(uint32_t pid, uint64_t phys_base, uint32_t npages,
                   uint32_t perm, uint16_t* out_idx);      /* returns 0; out_idx = cap index */
int cap_arena_alloc(uint32_t pid, uint32_t npages, uint32_t perm,
                    uint16_t* out_idx);                    /* returns 0; out_idx = cap index */
int cap_chan_create(uint32_t pid, uint32_t far_pid,
                    uint16_t* out_rd, uint16_t* out_wr,
                    uint16_t* out_far_rd, uint16_t* out_far_wr);
int cap_send(uint32_t pid, uint16_t ch_w_idx, uint16_t cap_idx, uint64_t cookie);
int cap_recv(uint32_t pid, uint16_t ch_r_idx, int block,
             uint64_t* cookie_out, uint16_t* out_cap);     /* returns 0; out_cap = index or CAP_NONE */
int cap_revoke(uint32_t pid, uint16_t cap_idx);            /* total: no cap survives */
int cap_map(uint32_t pid, uint16_t cap_idx, uint64_t vaddr, uint32_t flags);
int cap_unmap(uint32_t pid, uint16_t cap_idx, uint64_t vaddr);
void cap_list(void);          /* serial introspection (SYS_SLS_CAP_LIST) */

/* Debug introspection for the lifecycle host test / shell. */
uint32_t cap_debug_objid(uint32_t pid, uint16_t cap_idx);
uint32_t cap_debug_refcount(uint32_t obj_id);
uint64_t cap_debug_obj_phys(uint32_t obj_id);  /* MEM object's physical base (0 if not MEM) */
uint32_t cap_object_count(void);          /* active objects */
uint32_t cap_arena_free_frames(void);
int cap_frame_in_arena(uint64_t paddr);   /* Phase 2: is paddr inside the cap arena? */

/* ─── Weak hooks, overridden by the real kernel (process.c) and by host
 * tests. cap.c ships weak defaults so it can be host-tested in isolation. */
uint32_t cap_current_pid(void);
uint64_t cap_proc_cr3(uint32_t pid);

/* Phase 1.5 — blocking cap_recv park/wake. cap.c ships weak defaults:
 * cap_wait_chan() returns 0 ("could not park") so an empty blocking recv
 * degrades to CAP_EAGAIN exactly like Phase 1; cap_wake_chan() is a no-op.
 * process.c overrides both with the real scheduler integration (park the
 * current process on the channel and switch away; wake it via the kernel
 * resume path). chan_id is the channel's object id (cap_objects[] index).
 * cap_maybe_handoff() is called by cap_send right after a successful wake:
 * the weak default is a no-op ("wake on send, next schedule" — the
 * Phase-1.5 behavior); process.c's strong override hands the CPU to the
 * just-woken process immediately (the receiver runs before the sender's
 * send returns to ring-3). */
int  cap_wait_chan(uint32_t chan_id, void* recv_req);
void cap_wake_chan(uint32_t chan_id);
void cap_maybe_handoff(void);

/* ─── Arch hooks, overridden per-architecture (arch/x86/user_paging.c).
 * cap.c ships weak stubs so the host test can fake a page table. */
int  cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                       uint32_t cap_perms);
int  cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr);
void cap_arch_tlb_flush(void);

/* Syscall wrappers (called from syscall_dispatch.c). */
uint64_t sys_sls_cap_create_mem(struct SLSCapCreateMemRequest* req);
uint64_t sys_sls_cap_arena_alloc(struct SLSCapArenaAllocRequest* req);
uint64_t sys_sls_chan_create(struct SLSCapChanCreateRequest* req);
uint64_t sys_sls_cap_send(struct SLSCapSendRequest* req);
uint64_t sys_sls_cap_recv(struct SLSCapRecvRequest* req);
uint64_t sys_sls_cap_revoke(struct SLSCapRevokeRequest* req);
uint64_t sys_sls_cap_map(struct SLSCapMapRequest* req);
uint64_t sys_sls_cap_unmap(struct SLSCapUnmapRequest* req);
uint64_t sys_sls_cap_list(void);

#endif /* CAP_H */
