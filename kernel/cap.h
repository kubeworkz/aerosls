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
/* Cache-hint bits ride the map-perms word into cap_arch_map_page for DEV
 * mappings only (never stored in a cap word; the arch hook translates
 * them to PTE PWT/PCD). */
#define CAP_PERM_DEV_WC   0x10
#define CAP_PERM_DEV_UC   0x20
#define CAP_PERM_BIND     0x01   /* CAP_TYPE_IRQ: the one right — bind the vector
                                  * (bit position collides with PERM_R only by
                                  * naming; an IRQ cap's PERM field MEANS bind) */
#define CAP_PERM_SEND  0x01
#define CAP_PERM_RECV  0x02

/* ─── Phase 5 channel transport constants (the kabi.rs k_chan_* contract,
 * docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md §3/§5/§7). The
 * kernel's k_chan_* functions return these POSITIVE CAP_ERR_* codes — NOT
 * the negative CAP_E* codes of the Phase-3 syscalls; 0 = CAP_ERR_OK. */
#define CAP_ERR_OK         0
#define CAP_ERR_NOTFOUND   1
#define CAP_ERR_REVOKED    2
#define CAP_ERR_RIGHTS     3
#define CAP_ERR_RANGE      4
#define CAP_ERR_BUDGET     5
#define CAP_ERR_SPACE      6
#define CAP_ERR_TARGET     7
#define CAP_ERR_STATE      8
#define CAP_ERR_TYPE       9
#define CAP_ERR_PROTO      10
#define CAP_ERR_NOMEM      11
#define CAP_ERR_BUFSZ      12
#define CAP_ERR_TIMEOUT    13

/* Envelope kinds (transport spec §5) and send flags (§3.4). CH_KIND_MSG is
 * what k_chan_send produces; k_chan_recv/k_chan_wait report the head's
 * kind. F_REPLY is accepted but window bookkeeping is deferred (Phase-1.5
 * convention: the transport does not yet enforce the request/reply window). */
#define CH_KIND_MSG         0
#define CH_KIND_CLOSE       1
#define CH_KIND_NEW_CHANNEL 2
#define CH_KIND_NONE        3
#define CH_F_REPLY          0x0001
#define CH_F_NO_REPLY       0x0002

/* Close-reason registry (capability-layer spec §6.2, transport spec §5).
 * k_chan_close takes one; cap_table_teardown emits CLOSE_PEER_DEAD with
 * the dead pid as detail. */
#define CLOSE_PEER        0x0001
#define CLOSE_PEER_DEAD   0x0002
#define CLOSE_REVOKED     0x0003
#define CLOSE_PROTO       0x0004
#define CLOSE_ADMIN       0x0005

/* Block forever (real kernel) — mirrored from kabi.rs TIMEOUT_NONE. */
#define CH_TIMEOUT_NONE     0xFFFFFFFFFFFFFFFFULL

#define CAP_NONE 0xFFFF   /* "no capability" sentinel (slot index / syscall result) */

/* ─── Limits ──────────────────────────────────────────────────────────────── */
#define CAP_TABLE_MAX        16    /* == PROC_MAX; one table per process + kernel ctx */
#define CAP_TABLE_ENTRIES    512   /* fd-like index space per process */
#define CAP_MAP_MAX          64    /* map records per process */
#define CAP_OBJECT_MAX       1024  /* object table; monotonic, never recycled (Phase 1) */
#define CAP_HOLDER_MAX       8192  /* holder-pool nodes (slots + queued caps) */
#define CAP_CHAN_MAX         64    /* channel objects */
#define CHAN_QUEUE_DEPTH     16    /* messages per directional queue */
#define CHAN_WAIT_MAX_CHANS  8     /* endpoints one k_chan_wait may poll/park on */

/* ─── Phase 3 message transport (Polyglot Nexus, docs/AeroSLS-Polyglot-
 * Nexus-Phase3-Design-v0.1.md §2.3) ────────────────────────────────────────
 * A channel message may carry a payload (the IDL envelope: opcode + args,
 * opaque to the kernel) plus up to CAP_MSG_MAX_CAPS moved MEM caps. The
 * payload is staged in a fixed pool (CAP_MSG_PAYLOAD_POOL ×
 * CAP_MSG_MAX_PAYLOAD — 32 × 4 KiB = 128 KiB static) — same fixed-pool
 * style as the holder pool; a send whose payload would exceed the pool
 * returns CAP_ENOSPC (fail-before-mutate, nothing queued). The 4 KiB
 * payload bound is exactly the AeroIDL compiler's own payload ceiling, so
 * every generated stub fits. Cap descriptors travel with the message so
 * the receiver learns the byte-level view (offset/len/rights/flags) the
 * IDL attached to each moved cap.
 *
 * Why 32, not the original 8 (Phase 5 QEMU boot finding): the pool is
 * GLOBAL — every channel's queued messages draw from it — but
 * CHAN_QUEUE_DEPTH (16) is per-channel. A sidecar booting with a burst of
 * console log messages (each a small payload) can fill 8 slots before the
 * console service's next drain tick, and a concurrent critical send (the
 * init→DM device-registry handshake) then fails CAP_ENOSPC/NOMEM — the
 * exact failure seen under QEMU ("failed to send device registry: kernel
 * error 11"). 32 = 2 × queue depth gives one channel's full burst plus a
 * second channel's worth of headroom; the console drain returns slots
 * within a tick, so steady state needs far fewer. */
#define CAP_MSG_MAX_CAPS      4
#define CAP_MSG_MAX_PAYLOAD   4096
#define CAP_MSG_PAYLOAD_POOL  32

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

struct ChanMsg {              /* fixed, preallocated in the channel */
    uint64_t cookie;          /* user-defined tag (request id for Phase 3) */
    uint16_t payload_len;     /* bytes of staged payload, 0 = none */
    uint8_t  n_caps;          /* 0..CAP_MSG_MAX_CAPS moved caps */
    uint8_t  flags;           /* bit0 = NO_REPLY (async) */
    uint16_t payload_idx;     /* pool buffer index, CAP_NONE = no payload */
    uint8_t  _pad[1];
    uint64_t cap_word[CAP_MSG_MAX_CAPS];   /* MOVED cap words (0 = unused slot) */
    uint32_t cap_off[CAP_MSG_MAX_CAPS];    /* byte offset into the arena region */
    uint32_t cap_len[CAP_MSG_MAX_CAPS];    /* bytes granted */
    uint8_t  cap_rights[CAP_MSG_MAX_CAPS]; /* R/W (never X over channels) */
    uint8_t  cap_flags[CAP_MSG_MAX_CAPS];  /* borrowed/arena/owned */
};

/* One directional queue per end. q[0] carries messages bound FOR end0,
 * q[1] for end1. end0's CHAN_W enqueues q[1]; end1's CHAN_W enqueues q[0]. */
struct CapChannel {
    struct CapSpinlock lock;
    uint16_t end0_pid;        /* creator */
    uint16_t end1_pid;        /* pid the far-end caps were minted for at creation */
                               /* (the peer; may drift if far-end caps are later
                                * forwarded — the peer-death walk then misses the
                                * close rather than falsely closing an unrelated
                                * channel) */
    uint32_t qhead[2];
    uint32_t qtail[2];        /* monotonically growing; entries are qtail%DEPTH */
    uint32_t qdepth[2];
    struct ChanMsg q[2][CHAN_QUEUE_DEPTH];
    /* Phase 5 close state (kernel/chan.c). closed[d] = the owner of end d
     * closed its endpoint (idempotent; its further sends/recvs fail with
     * CAP_ERR_STATE). close_evt[d] = a CLOSE event awaits end d's owner
     * (the PEER closed): delivered by k_chan_recv after the message queue
     * drains, exactly once. Zeroed in cap_chan_create. */
    uint8_t  closed[2];
    uint8_t  close_evt[2];
    uint16_t close_reason[2];
    uint32_t close_detail[2];
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
#define CAP_OBJ_KIND_DEV   3   /* device MMIO region (CAP_TYPE_DEV); not arena-backed */

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
extern struct CapObject cap_objects[CAP_OBJECT_MAX];    /* defined in cap.c */
extern struct CapChannel cap_channels[CAP_CHAN_MAX];    /* defined in cap.c */

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

/* ─── Phase 3 message syscalls (302-304 — 298-301 are taken by Phase 1.5
 * GETPPID / PROGRAM_SPAWN_NB / YIELD / PROGRAM_SPAWN_NB_HELD; the design
 * doc's Appendix C numbers 298-300 collided with those and are superseded
 * here, recorded in the doc addendum) ─────────────────────────────────── */
#define SYS_SLS_CAP_SEND_MSG    302
#define SYS_SLS_CAP_RECV_MSG    303
#define SYS_SLS_CAP_ARENA_FREE  304

/* ─── Phase 3 trampoline capability syscalls (305-306 — next free after
 * SYS_SLS_CAP_ARENA_FREE = 304; confirmed via grep across every kernel
 * header defining SYS_SLS_*) ────────────────────────────────────────── */
#define SYS_SLS_TRAMPOLINE_CREATE  305
#define SYS_SLS_TRAMPOLINE_CALL    306

/* ─── Trampoline capability (Deliverable 5, same-ring trampoline) ──────
 * A TRAMP cap grants the caller permission to directly branch to a
 * pre-verified entry point in the callee's address space, with hardware-
 * enforced memory protection (Intel MPK / AMD PKU). The kernel issues
 * TRAMP caps only when both sidecars run in ring 0, declare mutual
 * trust, and the CPU supports MPK. See docs/AeroSLS-Polyglot-Nexus-
 * Phase3-Design-v0.1.md §5 for the full rationale. ────────────────── */
#define CAP_TYPE_TRAMP  4
#define CAP_TYPE_DEV    5   /* driver SDK ABI v0.1 §3.1: device MMIO region
                             * (OBJ = CapObject region id, OFF = byte offset,
                             * LEN = 4 KiB pages, PERM = R/W) */
#define CAP_TYPE_IO     6   /* driver SDK ABI v0.1 §3.2: I/O port range
                             * (OBJ = port base, LEN = port count, PERM = R/W) */
#define CAP_TYPE_IRQ    7   /* driver SDK ABI v0.1 §3.3: single-use device
                             * interrupt (OBJ = vector number, PERM = bind) */

struct TrampolineCap {
    uint64_t entry_vaddr;       /* callee's verified entry point */
    uint64_t stack_vaddr;       /* callee's pre-allocated call stack */
    uint32_t callee_pkey;       /* MPK protection key for callee code pages */
    uint32_t caller_pkey_mask;  /* PKRU value for the caller during call */
    uint32_t data_pkey;         /* shared arena data key (RW for both) */
    uint32_t flags;             /* bit0 = uses callee stack */
    uint64_t max_stack_bytes;   /* stack guard bound */
};

/* ─── MPK support flags (set at boot by kernel, queried by user) ─────── */
#define CAP_TRAMP_MPK_SUPPORTED    0x01  /* CPU supports Intel MPK / AMD PKU */
#define CAP_TRAMP_MPK_ENABLED      0x02  /* kernel has enabled MPK subsystem */

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

/* Phase 3: cap descriptor passed over a channel — the IDL layer's view of
 * a moved MEM cap (matches aerosls_cap_desc_t in the Ring-3 SDK). slot is
 * the SENDER's table slot on send; on recv the kernel replaces it with the
 * receiver's NEW slot index. offset/len are the byte-level region view the
 * IDL attached; the cap word itself remains page-granular. */
struct SLSCapDesc {
    uint16_t slot;            /* sender's cap-table slot (u16 — MEM tables) */
    uint32_t offset;          /* byte offset into the arena region */
    uint32_t len;             /* bytes granted (≤ region − offset) */
    uint8_t  rights;          /* R=0x1 W=0x2 (never X) */
    uint8_t  flags;           /* borrowed/arena/owned (wire metadata) */
};

struct SLSCapSendMsgRequest {
    uint16_t ch_w_idx;        /* caller's CHAN_W slot */
    uint16_t n_caps;          /* 0..CAP_MSG_MAX_CAPS */
    uint8_t  _pad[4];
    uint32_t tag;             /* request id, echoed by the receiver */
    uint32_t flags;           /* bit0 = NO_REPLY (async) */
    uint32_t payload_len;     /* ≤ CAP_MSG_MAX_PAYLOAD */
    uint8_t  _pad2[4];
    void*    payload;         /* user buffer the kernel copies payload from */
    struct SLSCapDesc caps[CAP_MSG_MAX_CAPS];   /* sender's descriptors */
};

struct SLSCapRecvMsgRequest {
    uint16_t ch_r_idx;        /* caller's CHAN_R slot */
    uint8_t  block;           /* accepted for ABI symmetry; kernel-side park
                               * is Phase-1.5 territory (single-cap path), so
                               * recv_msg always returns CAP_EAGAIN when empty
                               * and the Ring-3 SDK retries (Phase-1 pattern) */
    uint8_t  _pad[1];
    uint16_t max_caps;        /* capacity of out_caps (≤ CAP_MSG_MAX_CAPS) */
    uint8_t  _pad2[2];
    void*    buf;             /* user buffer the kernel copies payload out to */
    uint32_t buf_len;         /* capacity of buf */
    uint8_t  _pad3[4];
    uint32_t out_tag;         /* [out] request id echoed */
    uint32_t out_flags;       /* [out] message flags */
    uint32_t out_payload_len; /* [out] payload bytes copied to buf */
    uint16_t out_n_caps;      /* [out] caps installed (≤ max_caps) */
    uint8_t  _pad4[2];
    struct SLSCapDesc out_caps[CAP_MSG_MAX_CAPS];  /* [out] installed caps */
};

struct SLSCapArenaFreeRequest {
    uint16_t cap_idx;         /* MEM cap to drop (refcount decrement) */
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

/* ─── Phase 5 channel transport request structs (syscalls 311-315) ────────
 * Layouts mirror kabi.rs's WaitOut/RecvOut/CapRefOut/CapInfoOut and the
 * extern "C" k_chan_* signatures exactly (little-endian, repr(C) on the
 * sidecar side). `handle`/`chan` fields are the CALLER's cap-table slots. */

/* One granted cap written by k_chan_recv (kabi CapRefOut). */
struct SLSChanCapRef {
    uint32_t handle;          /* receiver-table slot of the minted cap */
    uint8_t  rights;
    uint8_t  flags;
    uint16_t pad;
    uint64_t base;            /* sender base + offset */
    uint64_t len;
};

/* k_chan_recv's out block (kabi RecvOut). */
struct SLSChanRecvOut {
    uint16_t kind;            /* CH_KIND_MSG | CH_KIND_CLOSE */
    uint16_t flags;           /* CH_F_REPLY | CH_F_NO_REPLY */
    uint32_t tag;             /* request id echoed */
    uint32_t len;             /* payload bytes copied into buf */
    uint32_t n_caps;          /* granted caps written into slots[] */
    uint32_t needed;          /* on CAP_ERR_BUFSZ: (caps_needed << 16) | payload_needed */
};

struct SLSChanWaitRequest {
    uint16_t chans[CHAN_WAIT_MAX_CHANS]; /* caller's CHAN_R slots to poll */
    uint16_t n_chans;                    /* 1..CHAN_WAIT_MAX_CHANS */
    uint8_t  _pad[4];
    uint64_t timeout_ns;      /* CH_TIMEOUT_NONE = block forever: the caller
                               * parks on the listed channels (cap_wait_chans,
                               * process.c) and a later enqueue/close/death
                               * wakes it to re-run the wait. A finite timeout
                               * parks WITH a deadline: the timer ISR wakes it
                               * when the deadline passes and the re-run then
                               * returns CAP_ERR_TIMEOUT (granularity = one
                               * ~10 ms tick, KERNEL_TICK_NS) */
    uint32_t out_idx;         /* [out] index into chans[] */
    uint16_t out_kind;        /* [out] CH_KIND_* of the head entry */
    uint8_t  _pad2[2];
};

struct SLSChanRecvRequest {
    uint16_t chan;            /* caller's CHAN_R slot */
    uint8_t  _pad[6];
    void*    buf;             /* user buffer for the payload / close body */
    uint32_t buf_len;
    uint32_t n_slots;         /* capacity of slots[] */
    struct SLSChanCapRef slots[8];
    struct SLSChanRecvOut out;   /* [out] */
};

struct SLSChanSendRequest {
    uint16_t chan;            /* caller's CHAN_W slot */
    uint8_t  _pad[2];
    uint32_t tag;             /* request id (echoed in replies) */
    uint16_t flags;           /* CH_F_REPLY | CH_F_NO_REPLY */
    uint8_t  _pad2[2];
    uint32_t payload_len;     /* ≤ CAP_MSG_MAX_PAYLOAD */
    uint8_t  _pad3[4];
    void*    payload;         /* user buffer the kernel copies from */
    struct SLSCapDesc caps[CAP_MSG_MAX_CAPS];
    uint16_t n_caps;          /* 0..CAP_MSG_MAX_CAPS */
    uint8_t  _pad4[6];
    uint64_t timeout_ns;      /* 0 = block until enqueued: a queue-full send
                               * parks the caller (cap_wait_chans,
                               * park_syscall = SYS_SLS_CHAN_SEND); a recv
                               * freeing a slot wakes it to re-run the send.
                               * Nonzero = block with a deadline: the timer
                               * ISR wakes the parked sender when the
                               * deadline passes and the re-run then returns
                               * CAP_ERR_TIMEOUT (granularity = one ~10 ms
                               * tick, KERNEL_TICK_NS) */
};

struct SLSChanCloseRequest {
    uint16_t chan;            /* caller's CHAN_R or CHAN_W slot */
    uint8_t  _pad[2];
    uint16_t reason;          /* CLOSE_PEER | CLOSE_PEER_DEAD | ... */
    uint8_t  _pad2[2];
    uint32_t detail;
};

/* Driver SDK ABI v0.1 §4.2 — port I/O request structs. `index` is
 * relative to the cap's port base (OBJ field); `index + size <= LEN`
 * (the cap's port count) or CAP_ERR_RANGE. size is 1|2|4. */
struct SLSIoInRequest {
    uint16_t slot;            /* caller's CAP_TYPE_IO slot */
    uint16_t index;           /* port offset relative to base */
    uint8_t  size;            /* 1 | 2 | 4 */
    uint8_t  _pad[3];
    uint32_t value;           /* [out] the value read */
};

struct SLSIoOutRequest {
    uint16_t slot;            /* caller's CAP_TYPE_IO slot */
    uint16_t index;           /* port offset relative to base */
    uint8_t  size;            /* 1 | 2 | 4 */
    uint8_t  _pad[3];
    uint32_t value;           /* the value to write */
};

/* Driver SDK ABI v0.1 §4.1 — map a CAP_TYPE_DEV cap into the caller's
 * address space. The kernel validates the cap, allocates a window (hint
 * honored if free, else a free window is found), maps the physical range
 * with the cap's R/W perms, and records the mapping — a second call with
 * the same cap returns the same vaddr. Unmapping happens on revoke /
 * teardown (cap_table_unmap_object keys by obj id). flags:
 * DEV_MMAP_WC (bit0, write-combining), DEV_MMAP_UNCACHED (bit1). */
#define DEV_MMAP_WC        0x01
#define DEV_MMAP_UNCACHED  0x02

struct SLSDevMmapRequest {
    uint16_t slot;            /* caller's CAP_TYPE_DEV slot */
    uint8_t  _pad[2];
    uint32_t flags;           /* DEV_MMAP_WC | DEV_MMAP_UNCACHED */
    uint64_t vaddr_hint;      /* 0 = kernel picks a window */
    uint64_t out_vaddr;       /* [out] the mapped window (CAP_NONE on error) */
};

/* Driver SDK ABI v0.1 §4.3 — bind a CAP_TYPE_IRQ cap to a notification
 * channel. The kernel creates a channel pair (it holds the CHAN_W end,
 * the caller receives the CHAN_R end in a new slot), registers the vector
 * in the kernel IRQ table so the ISR path (cap_irq_notify) can enqueue,
 * and stamps the IRQ cap REVOKED (single-use). budget = max queued
 * notifications (0 = CHAN_QUEUE_DEPTH); the ISR drops rather than blocks. */
#define CAP_IRQ_VECTORS   256

struct SLSIrqBindRequest {
    uint16_t slot;            /* caller's CAP_TYPE_IRQ slot */
    uint8_t  _pad[2];
    uint32_t budget;          /* 0 = default (CHAN_QUEUE_DEPTH) */
    uint16_t out_chan_r;      /* [out] caller's new CHAN_R slot */
    uint8_t  _pad2[6];
};

/* Driver SDK ABI v0.1 §4.4 — release a bound IRQ vector without dying:
 * disarms the registry entry (the ISR stops enqueuing), closes the
 * channel pair (blocked receivers get the existing CLOSE event / wake),
 * and frees the vector for rebind. Takes the caller's CHAN_R slot from a
 * prior successful bind. */
struct SLSIrqUnbindRequest {
    uint16_t chan_r;          /* caller's CHAN_R slot from k_irq_bind */
    uint8_t  _pad[6];
};

/* k_cap_info's out block (kabi CapInfoOut). */
struct SLSCapInfoOut {
    uint16_t ty;              /* CAP_TYPE_MEM | CAP_TYPE_CHAN_R | ... */
    uint16_t rights;
    uint16_t flags;
    uint16_t pad;
    uint64_t base;
    uint64_t len;
};

struct SLSCapInfoRequest {
    uint16_t handle;          /* caller's cap slot */
    uint8_t  _pad[6];
    struct SLSCapInfoOut out; /* [out] */
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

/* Phase 3 message transport. cap_send_msg stages the payload into the pool
 * and MOVES up to CAP_MSG_MAX_CAPS MEM caps into the queue; cap_recv_msg
 * installs them into fresh slots in the receiver's table and copies the
 * payload out. cap_arena_free drops ONE MEM reference (removing the slot
 * holder; the object's arena frames return at refcount 0) — the single-
 * reference analogue of cap_revoke, which nukes every holder of an object. */
int cap_send_msg(uint32_t pid, uint16_t ch_w_idx, const void* payload,
                 uint32_t payload_len, const struct SLSCapDesc* descs,
                 uint16_t n_caps, uint32_t tag, uint32_t flags);
int cap_recv_msg(uint32_t pid, uint16_t ch_r_idx,
                 void* buf, uint32_t buf_len, uint32_t* out_payload_len,
                 uint16_t max_caps, struct SLSCapDesc* out_caps,
                 uint16_t* out_n_caps, uint32_t* out_tag, uint32_t* out_flags);
int cap_arena_free(uint32_t pid, uint16_t cap_idx);

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
/* Phase 5 wait-aware park: like cap_wait_chan but parks on a LIST of
 * channels (the k_chan_wait and the blocking k_chan_send paths,
 * kernel/chan.c). `chan_ids` are cap_channels[] indexes; the wake
 * (cap_wake_chan on ANY listed channel, or the deadline tick) resumes the
 * process and re-runs the syscall `park_syscall` (SYS_SLS_CHAN_WAIT or
 * SYS_SLS_CHAN_SEND) with the saved request pointer. `deadline_ticks` is
 * the ABSOLUTE kernel tick count by which the park must resolve, or 0 =
 * block forever (CH_TIMEOUT_NONE / send timeout 0); a finite deadline is
 * met by cap_park_deadline_tick (timer ISR, ~10 ms granularity) waking the
 * park, whose re-run then returns CAP_ERR_TIMEOUT. Weak default returns 0
 * ("could not park") so the caller degrades to CAP_ERR_TIMEOUT exactly
 * like the Phase-1.5 recv weak default. */
int  cap_wait_chans(const uint32_t* chan_ids, uint32_t n, void* req,
                    uint32_t park_syscall, uint64_t deadline_ticks);
void cap_wake_chan(uint32_t chan_id);
/* Phase 5 deadline support (see cap.c's weak defaults and process.c's
 * strong overrides): cap_park_deadline_take() returns the current
 * process's stored absolute park deadline (0 = none/forever) and CLEARS
 * it — the resume re-run of a woken wait/send takes the ORIGINAL deadline
 * so re-parks never extend it. cap_park_deadline_tick() (called from the
 * timer ISR) wakes every parked process whose deadline has passed. */
uint64_t cap_park_deadline_take(void);
void     cap_park_deadline_tick(void);
void cap_maybe_handoff(void);

/* ─── Arch hooks, overridden per-architecture (arch/x86/user_paging.c).
 * cap.c ships weak stubs so the host test can fake a page table. */
int  cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                       uint32_t cap_perms);
int  cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr);
void cap_arch_tlb_flush(void);
/* Phase 5 sidecar MEM-cap grant mapping: after a MEM cap is moved across a
 * channel (cap_recv_msg), make the region Ring-3-accessible in the
 * receiver's address space (arch/x86/user_paging.c user_map_identity). */
int  cap_arch_identity_map_user(uint64_t pml4_phys, uint64_t phys,
                                uint32_t npages, uint32_t cap_perms);

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
uint64_t sys_sls_cap_send_msg(struct SLSCapSendMsgRequest* req);
uint64_t sys_sls_cap_recv_msg(struct SLSCapRecvMsgRequest* req);
uint64_t sys_sls_cap_arena_free(struct SLSCapArenaFreeRequest* req);

/* Phase 5 channel transport (kernel/chan.c) — return POSITIVE CAP_ERR_*. */
uint64_t sys_sls_chan_wait(struct SLSChanWaitRequest* req);
uint64_t sys_sls_chan_recv(struct SLSChanRecvRequest* req);
uint64_t sys_sls_chan_send(struct SLSChanSendRequest* req);
uint64_t sys_sls_chan_close(struct SLSChanCloseRequest* req);
uint64_t sys_sls_cap_info(struct SLSCapInfoRequest* req);
uint64_t sys_sls_io_in(struct SLSIoInRequest* req);
uint64_t sys_sls_io_out(struct SLSIoOutRequest* req);
uint64_t sys_sls_dev_mmap(struct SLSDevMmapRequest* req);
uint64_t sys_sls_irq_bind(struct SLSIrqBindRequest* req);
uint64_t sys_sls_irq_unbind(struct SLSIrqUnbindRequest* req);

/* Driver SDK ABI v0.1 §4.2 — kabi-level port I/O (called by the syscall
 * wrappers in chan.c; k_cap_info-style: resolve the caller's slot, check
 * type/rights/range, execute the port access). */
int  k_io_in(uint32_t pid, uint16_t slot, uint16_t index, uint8_t size,
             uint32_t* out_val);
int  k_io_out(uint32_t pid, uint16_t slot, uint16_t index, uint8_t size,
              uint32_t val);
int  k_dev_mmap(uint32_t pid, uint16_t slot, uint32_t flags,
                uint64_t vaddr_hint, uint64_t* out_vaddr);
int  k_irq_bind(uint32_t pid, uint16_t slot, uint32_t budget,
                uint16_t* out_chan_r);
int  k_irq_unbind(uint32_t pid, uint16_t chan_r, uint16_t* out_vector);

/* IRQ registry + ISR path (driver SDK ABI v0.1 §4.3). cap_irq_notify is
 * what the arch ISR stub calls on fire: look up the vector, enqueue a
 * one-byte notification (payload = vector, tag = vector) on the kernel-
 * held CHAN_W end, wake, EOI. Weak arch hooks: cap_irq_eoi is a no-op
 * unless an arch layer overrides it (LAPIC/PIC). */
void cap_irq_notify(uint32_t vector);
void cap_irq_eoi(uint32_t vector) __attribute__((weak));

/* Port I/O hooks — weak in cap.c (no-op), strong in arch/x86/user_paging.c
 * (real in/out instructions); host tests override with a fake port map. */
uint32_t cap_io_read(uint16_t port, uint8_t size);
void     cap_io_write(uint16_t port, uint8_t size, uint32_t val);

/* Channel lock (defined in cap.c; chan.c peeks channel queues under it). */
void cap_lock(struct CapSpinlock* l);
void cap_unlock(struct CapSpinlock* l);

/* ─── Phase 5: create_sidecar (self-hosted boot) ────────────────────────
 * SYS_SLS_CREATE_SIDECAR (310) — next free after TRAMPOLINE_CALL (306);
 * confirmed via grep across every kernel header defining SYS_SLS_*.
 *
 * The syscall accepts a packed sidecar manifest blob, creates a new
 * process, maps the sidecar image, builds the initial capability table
 * from the manifest's CAP records, writes a BootInfoBlock at the top of
 * the child's stack, and enters ring-3 at the image entry point. On
 * success the caller's messenger caps to the child are returned through
 * the request's OUT fields (out_ch_r = parent CHAN_R for receiving the
 * child's replies, out_ch_w = parent CHAN_W for sending to the child; the
 * syscall's return value is also the CHAN_R slot). The child's BIB lists
 * the child's ends as caps #0 (CHAN_R) and #1 (CHAN_W).
 *
 * Packed manifest wire format (matches user/proto/src/manifest.rs):
 *   Header (24 bytes):
 *     magic[8]="AERSLSM1", version_major u16=1, version_minor u16=0,
 *     record_count u16, flags u16, total_len u32, body_crc32 u32
 *   Records (each: tag u16, len u16, payload[len]):
 *     TAG_PERSONALITY 0x0001 — name_len u16, name UTF-8
 *     TAG_IMAGE       0x0002 — entry_offset u64, image_size u32, _pad u32
 *     TAG_BUDGET      0x0003 — mem_bytes u64, stack_bytes u32, heap_init u32
 *     TAG_CPU         0x0004 — share_pct u16, preemptible u8, _pad u8
 *     TAG_LIMITS      0x0005 — max_tasks u16, max_fds u16, max_chans u16,
 *                              max_open_files u16, chan_qdepth u16, _pad u6
 *     TAG_CAP_MEM     0x0006 — name_len u16, name[], phys_base u64,
 *                              size u64 (bytes), rights u8
 *     TAG_CAP_CHAN    0x0007 — name_len u16, name[], peer_len u16,
 *                              peer[], rights u8, flags u8
 *     TAG_BOOTSTRAP   0x0008 — cons_name_len u16, cons_name[],
 *                              debug_name_len u16, debug_name[], log_level u8
 *     TAG_FLAGS       0x0009 — flags_value u32
 *     TAG_NAME        0x000A — name_len u16, name UTF-8 (sidecar identity;
 *                              registered in the sidecar registry so peers
 *                              can wire CAP_CHAN channels to it)
 *
 * BootInfoBlock wire format (filled by the kernel, read by sidecar _start):
 *   Header (32 bytes):
 *     magic[8]="AERSLSB1", version u16=1, cap_count u16,
 *     budget_bytes u64, stack_top u64, total_len u32
 *   Cap entries (each): name_len u16, name[], slot u16, ty u8,
 *                       rights u8, base u64, len u64
 */
#define SYS_SLS_CREATE_SIDECAR 310

/* ─── Phase 5 channel transport syscalls (311-315 — next free after
 * SYS_SLS_CREATE_SIDECAR = 310; confirmed via grep across every kernel
 * header defining SYS_SLS_*) — the kabi.rs k_chan_* contract over
 * cap_send_msg/cap_recv_msg (kernel/chan.c). These return the transport's
 * POSITIVE CAP_ERR_* codes, not the negative CAP_E* codes of the
 * Phase-3 syscalls. ─────────────────────────────────────────────────── */
#define SYS_SLS_CHAN_WAIT     311
#define SYS_SLS_CHAN_RECV     312
#define SYS_SLS_CHAN_SEND     313
#define SYS_SLS_CHAN_CLOSE    314
#define SYS_SLS_CAP_INFO      315
/* driver SDK ABI v0.1 §4.2 — SYS_IO_IN/SYS_IO_OUT. The ABI doc's
 * originally-proposed 302/303 collided with the Phase-3
 * SYS_SLS_CAP_SEND_MSG/RECV_MSG, so these take the next free pair
 * (307/308; 306 is TRAMPOLINE_CALL, 310 is CREATE_SIDECAR). */
#define SYS_SLS_IO_IN        307
#define SYS_SLS_IO_OUT       308
#define SYS_SLS_DEV_MMAP     309
#define SYS_SLS_IRQ_BIND     316
#define SYS_SLS_IRQ_UNBIND   317

/* Manifest record tags */
#define SIDECAR_MANIFEST_MAGIC         "AERSLSM1"
#define SIDECAR_MANIFEST_VERSION_MAJOR 1
#define SIDECAR_MANIFEST_HEADER_LEN    24

#define SIDECAR_TAG_PERSONALITY  0x0001
#define SIDECAR_TAG_IMAGE        0x0002
#define SIDECAR_TAG_BUDGET       0x0003
#define SIDECAR_TAG_CPU          0x0004
#define SIDECAR_TAG_LIMITS       0x0005
#define SIDECAR_TAG_CAP_MEM      0x0006
#define SIDECAR_TAG_CAP_CHAN     0x0007
#define SIDECAR_TAG_BOOTSTRAP    0x0008
#define SIDECAR_TAG_FLAGS        0x0009
#define SIDECAR_TAG_NAME         0x000A

#define SIDECAR_MANIFEST_MAX_CAPS  16
#define SIDECAR_MANIFEST_MAX_NAME  64

struct SidecarManifestHeader {
    uint8_t  magic[8];
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t record_count;
    uint16_t flags;
    uint32_t total_len;
    uint32_t body_crc32;
} __attribute__((packed));

struct SidecarManifestRecord {
    uint16_t tag;
    uint16_t len;
    /* payload[len] follows */
} __attribute__((packed));

/* Parsed cap record (CAP_MEM or CAP_CHAN from the manifest). Field widths
 * and order mirror the wire layout exactly (name_len u16, peer_len u16,
 * size in BYTES). `wired_rd`/`wired_wr`/`wired` are set by the CHAN wiring
 * pass inside cap_create_sidecar (the child's endpoint slots once the
 * channel to the peer exists). */
struct SidecarCap {
    char     name[SIDECAR_MANIFEST_MAX_NAME];
    uint16_t name_len;          /* wire: u16 */
    uint8_t  kind;              /* SIDECAR_TAG_CAP_MEM or SIDECAR_TAG_CAP_CHAN */
    uint8_t  rights;            /* CAP_PERM_R|W bits */
    uint8_t  flags;             /* CHAN: per-cap flags; MEM: unused */
    uint64_t phys_base;         /* MEM: physical base address */
    uint64_t size_bytes;        /* MEM: size in BYTES (not pages) */
    char     peer_name[SIDECAR_MANIFEST_MAX_NAME]; /* CHAN: peer name */
    uint16_t peer_name_len;     /* CHAN: length of peer_name (wire: u16) */
    uint8_t  wired;             /* CHAN: channel created (child end minted) */
    uint16_t wired_rd;          /* CHAN: child's CHAN_R slot in its table */
    uint16_t wired_wr;          /* CHAN: child's CHAN_W slot in its table */
};

/* Parsed sidecar manifest (stack-allocated, no heap). */
struct SidecarManifest {
    uint16_t flags;
    uint16_t record_count;
    /* NAME record (sidecar identity) */
    char     name[SIDECAR_MANIFEST_MAX_NAME];
    uint8_t  name_len;
    /* IMAGE record */
    uint64_t image_entry;       /* entry point offset */
    uint32_t image_size;        /* image size in bytes */
    /* BUDGET record */
    uint64_t budget_mem_bytes;
    uint32_t budget_stack_bytes;
    uint32_t budget_heap_init;
    /* CAP records */
    struct SidecarCap caps[SIDECAR_MANIFEST_MAX_CAPS];
    uint8_t  n_caps;
};

/* BootInfoBlock — written by the kernel at the child's stack top.
 * Read by sidecar _start (user/proto/src/bootinfo.rs defines the
 * Rust parser for this exact wire format). */
#define SIDECAR_BIB_MAGIC     "AERSLSB1"
#define SIDECAR_BIB_VERSION   1
#define SIDECAR_BIB_CAPS_MAX  16

struct SidecarBib {
    uint8_t  magic[8];
    uint16_t version;
    uint16_t cap_count;
    uint64_t budget_bytes;
    uint64_t stack_top;
    uint32_t total_len;
} __attribute__((packed));

struct SidecarBibCap {
    uint16_t name_len;
    /* name[name_len] follows, UNPADDED — bootinfo.rs reads the slot field
     * immediately after the name bytes. */
    uint16_t slot;
    uint8_t  ty;                /* 1=MEM, 2=CHAN_R, 3=CHAN_W */
    uint8_t  rights;
    uint64_t base;
    uint64_t len;               /* MEM: byte size (not pages) */
} __attribute__((packed));

struct SLSCreateSidecarRequest {
    const void*  manifest;     /* pointer to packed manifest blob */
    uint32_t     manifest_len; /* blob size in bytes */
    uint8_t      _pad[4];
    uint16_t     ch_w_idx;     /* parent's CHAN_W to the child, or CAP_NONE */
    uint16_t     console_w_idx; /* console CHAN_W, or CAP_NONE */
    uint16_t     out_ch_r;     /* OUT: parent's messenger CHAN_R slot */
    uint16_t     out_ch_w;     /* OUT: parent's messenger CHAN_W slot */
};

int cap_create_sidecar(uint32_t parent_pid,
                       const void* manifest, uint32_t manifest_len,
                       uint16_t parent_ch_w, uint16_t console_ch_w,
                       uint16_t* out_ch_r);
uint64_t sys_sls_create_sidecar(struct SLSCreateSidecarRequest* req);

/* ─── Sidecar registry (Phase 5: name → pid for CAP_CHAN wiring) ────────
 * cap_create_sidecar registers each new sidecar under its manifest's
 * NAME record, and resolves CAP_CHAN peer_names against this table when
 * connecting channels. A peer named "kernel.*" is a kernel-owned service
 * (e.g. "kernel.debug.console"): the kernel context (pid 0) is the peer.
 * Registrations are dropped when the sidecar's cap table is torn down. */
#define SIDECAR_REGISTRY_MAX      16
#define SIDECAR_REGISTRY_NAME_LEN 64

void     sidecar_registry_init(void);
int      sidecar_registry_register(const char* name, uint32_t pid);
uint32_t sidecar_registry_resolve(const char* name);  /* pid, or 0 */
uint32_t sidecar_registry_count(void);
void     sidecar_registry_remove_pid(uint32_t pid);

/* ─── Phase 3 trampoline syscall wrappers ─────────────────────────────── */
struct SLSTrampolineCreateRequest {
    uint32_t callee_pid;        /* target sidecar pid */
    uint64_t entry_vaddr;       /* callee's verified entry point */
    uint32_t max_stack_bytes;   /* stack guard bound */
    uint32_t _pad;
};

struct SLSTrampolineCallRequest {
    uint16_t tramp_idx;         /* caller's TRAMP slot */
    uint8_t  _pad[6];
    uint64_t args[4];           /* up to 4 register-width arguments */
    uint64_t arg_count;         /* number of valid args */
    uint64_t arena_offset;      /* byte offset into shared arena for large data */
    uint64_t arena_len;         /* bytes of arena data (0 = none) */
};

/* Trampoline capabilities are issued when two sidecars run in the same
 * ring, declare mutual trust, and the CPU supports MPK. The kernel maps
 * the callee's code pages with a dedicated MPK key and programs the shared
 * arena with a data key. The caller uses WRPKRU to swap access rights
 * and JMP to the callee's entry — no syscall, no message copy. */
int cap_trampoline_create(uint32_t pid, uint32_t callee_pid,
                         uint64_t entry_vaddr, uint32_t max_stack_bytes,
                         uint16_t* out_idx);
int cap_trampoline_call(uint32_t pid, uint16_t tramp_idx,
                       const uint64_t args, uint64_t arg_count,
                       uint64_t arena_offset, uint64_t arena_len,
                       uint64_t* result);

/* MPK support detection. Returns CAP_TRAMP_MPK_* flags. Called at boot
 * and by user-space to decide whether to use the trampoline path or
 * fall back to channels. */
uint32_t cap_trampoline_mpk_flags(void);

uint64_t sys_sls_trampoline_create(struct SLSTrampolineCreateRequest* req);
uint64_t sys_sls_trampoline_call(struct SLSTrampolineCallRequest* req);

#endif /* CAP_H */
