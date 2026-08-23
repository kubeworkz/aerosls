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

/* Phase 4: device driver SDK types ────────────────────────────────────── */
#define CAP_TYPE_IO_PORT    5   /* Port I/O (x86 in/out; RV: MMIO via PMP/IOMMU) */
#define CAP_TYPE_IRQ        6   /* Hardware interrupt delivery */
#define CAP_TYPE_DMA_MEM    7   /* Physically pinned memory for DMA */
#define CAP_TYPE_BUS_ACCESS 8   /* PCIe config space / bus topology access */

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

/* Phase 4 permission bits (shared encoding; meaning depends on cap type) ── */
#define CAP_PERM_IO_READ    0x01  /* IO_PORT: allow reads */
#define CAP_PERM_IO_WRITE   0x02  /* IO_PORT: allow writes */
#define CAP_PERM_IO_PF      0x04  /* IO_PORT: allow BAR sizing */
#define CAP_PERM_IRQ_LISTEN 0x01  /* IRQ: receive interrupt messages */
#define CAP_PERM_IRQ_ACK    0x02  /* IRQ: acknowledge (EOI) the interrupt */
#define CAP_PERM_IRQ_MASK   0x04  /* IRQ: mask/unmask the interrupt */
#define CAP_PERM_DMA_SHARE  0x10  /* DMA_MEM: allow sharing to other sidecars */
#define CAP_PERM_DMA_MAP    0x20  /* DMA_MEM: allow IOMMU mapping */
#define CAP_PERM_BUS_ENUM   0x01  /* BUS_ACCESS: enumerate devices */
#define CAP_PERM_BUS_CFG_R  0x02  /* BUS_ACCESS: read config space */
#define CAP_PERM_BUS_CFG_W  0x04  /* BUS_ACCESS: write config space */

#define CAP_NONE 0xFFFF   /* "no capability" sentinel (slot index / syscall result) */

/* ─── Limits ──────────────────────────────────────────────────────────────── */
#define CAP_TABLE_MAX        16    /* == PROC_MAX; one table per process + kernel ctx */
#define CAP_TABLE_ENTRIES    512   /* fd-like index space per process */
#define CAP_MAP_MAX          64    /* map records per process */
#define CAP_OBJECT_MAX       1024  /* object table; monotonic, never recycled (Phase 1) */
#define CAP_HOLDER_MAX       8192  /* holder-pool nodes (slots + queued caps) */
#define CAP_CHAN_MAX         64    /* channel objects */
#define CHAN_QUEUE_DEPTH     16    /* messages per directional queue */

/* ─── Phase 3 message transport (Polyglot Nexus, docs/AeroSLS-Polyglot-
 * Nexus-Phase3-Design-v0.1.md §2.3) ────────────────────────────────────────
 * A channel message may carry a payload (the IDL envelope: opcode + args,
 * opaque to the kernel) plus up to CAP_MSG_MAX_CAPS moved MEM caps. The
 * payload is staged in a fixed pool (CAP_MSG_PAYLOAD_POOL ×
 * CAP_MSG_MAX_PAYLOAD, 8 × 4 KiB = 32 KiB static) — same fixed-pool style
 * as the holder pool; a send whose payload would exceed the pool returns
 * CAP_ENOSPC (fail-before-mutate, nothing queued). The 4 KiB payload bound
 * is exactly the AeroIDL compiler's own payload ceiling, so every
 * generated stub fits. Cap descriptors travel with the message so the
 * receiver learns the byte-level view (offset/len/rights/flags) the IDL
 * attached to each moved cap. */
#define CAP_MSG_MAX_CAPS      4
#define CAP_MSG_MAX_PAYLOAD   4096
#define CAP_MSG_PAYLOAD_POOL  8

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
    uint8_t  kind;            /* CAP_OBJ_KIND_* */
    uint8_t  revoking;        /* set at the revoke linearization point */
    uint8_t  active;
    uint8_t  _pad;
    uint16_t holder_head;     /* first holder node, 0xFFFF = none */
    uint32_t refcount;        /* == # VALID holders */
    uint32_t max_perms;       /* object-wide permission ceiling */
    uint64_t phys_base;       /* MEM: physical address of page 0 */
    uint32_t npages;          /* MEM: object length in pages */
    uint16_t chan_id;         /* CHAN: index into cap_channels[] */

    /* ── Phase 4: device driver SDK fields ────────────────────────────────
     * Union-style layout: each kind uses different fields. Because objects
     * are monotonic (never recycled, Phase 1 invariant §3), a dead MEM
     * object's phys_base never aliases a live IO_PORT — the kind discriminant
     * is authoritative and validated on every use. */
    union {
        struct {                /* IO_PORT */
            uint64_t io_phys_base;  /* physical base (MMIO on RV, port on x86) */
            uint32_t io_length;     /* length in bytes or port count */
            uint8_t  io_width;      /* 1, 2, or 4 bytes per access */
            uint8_t  io_flags;      /* bit0=UNCACHEABLE, bit1=READ_ONLY */
            uint16_t _io_pad;
        } io;
        struct {                /* IRQ */
            uint32_t irq_number;        /* PLIC source ID or MSI vector */
            uint8_t  irq_trigger;       /* 0=level-low, 1=edge-rising, 2=level-high */
            uint8_t  irq_polarity;      /* reserved, must be 0 */
            uint16_t irq_affinity_cpu;  /* target CPU (0xFFFF = any) */
            uint32_t irq_chan_id;        /* channel id for IRQ message delivery */
            uint32_t irq_masked;        /* 1 = masked, 0 = enabled */
            uint64_t irq_coalesce_us;   /* min interval between IRQ messages */
            uint64_t irq_last_us;       /* timestamp of last delivered IRQ */
            uint32_t irq_coalesce_count; /* pending coalesced interrupt count */
            uint32_t irq_sequence;       /* monotonic message sequence number */
            uint32_t irq_dropped;        /* messages dropped (queue full) */
        } irq;
        struct {                /* DMA_MEM — extends MEM (phys_base/npages) */
            uint32_t dma_iommu_domain;  /* IOMMU domain id (0 = no IOMMU) */
            uint32_t dma_device_id;     /* owning device's object id */
            uint8_t  dma_flags;         /* bit0=COHERENT, bit1=RO_DEV, bit2=WO_DEV */
            uint8_t  _dma_pad[3];
            uint32_t dma_shared_count;  /* number of MEM caps referencing this */
        } dma;
        struct {                /* BUS_ACCESS */
            uint8_t  bus_root;           /* starting bus number */
            uint8_t  bus_max;            /* ending bus number */
            uint8_t  bus_flags;          /* bit0=ENUM, bit1=CFG_R, bit2=CFG_W */
            uint8_t  _bus_pad;
            uint32_t bus_domain;         /* IOMMU domain for bus-mastering devices */
        } bus;
    } u;
    uint8_t  _pad2[4];
};

#define CAP_OBJ_KIND_MEM      1
#define CAP_OBJ_KIND_CHAN     2
#define CAP_OBJ_KIND_IO_PORT  5
#define CAP_OBJ_KIND_IRQ      6
#define CAP_OBJ_KIND_DMA_MEM  7
#define CAP_OBJ_KIND_BUS      8

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

/* ─── Phase 4: device driver SDK syscalls (307-313 — next free after
 * SYS_SLS_TRAMPOLINE_CALL = 306; confirmed via grep across every kernel
 * header defining SYS_SLS_*) ────────────────────────────────────────── */
#define SYS_SLS_CAP_CREATE_IO_PORT   307
#define SYS_SLS_CAP_CREATE_IRQ       308
#define SYS_SLS_IRQ_MASK             309
#define SYS_SLS_IRQ_UNMASK           310
#define SYS_SLS_IRQ_ACK              311
#define SYS_SLS_CAP_CREATE_DMA_MEM   312
#define SYS_SLS_CAP_CREATE_BUS       313

/* ─── Phase 4: DMA buffer allocator syscalls (314-320 — next free after
 * SYS_SLS_CAP_CREATE_BUS = 313; confirmed via grep across every kernel
 * header defining SYS_SLS_*) ────────────────────────────────────────── */
#define SYS_SLS_DMA_ALLOC          314
#define SYS_SLS_DMA_FREE           315
#define SYS_SLS_DMA_SHARE          316
#define SYS_SLS_DMA_PIN            317
#define SYS_SLS_DMA_UNPIN          318
#define SYS_SLS_DMA_IOMMU_MAP      319
#define SYS_SLS_DMA_IOMMU_UNMAP    320

/* ─── Phase 4: IRQ message format ─────────────────────────────────────────
 * Hardware interrupts are translated into fixed-format channel messages.
 * The kernel writes these into the IRQ channel; the driver reads them via
 * cap_recv_msg(). Carries data only — no capability transfers. */
struct IRQMessage {
    uint32_t irq_number;        /* interrupt source (PLIC source ID or MSI vector) */
    uint32_t timestamp_lo;      /* low 32 bits of hardware cycle counter */
    uint32_t timestamp_hi;      /* high 32 bits */
    uint16_t sequence;           /* monotonic per-IRQ-cap */
    uint8_t  priority;           /* 0=lowest..255=highest */
    uint8_t  flags;              /* bit0=COALESCED, bit1=SHARED_IRQ, bit7=OVERFLOW */
    uint32_t coalesce_count;     /* if COALESCED: how many interrupts merged */
    uint32_t device_status;      /* driver-specific (optional, read from device) */
};

#define CAP_IRQ_COALESCED  0x01
#define CAP_IRQ_SHARED     0x02
#define CAP_IRQ_OVERFLOW   0x80  /* channel queue full, messages dropped */

/* ─── Phase 4: request structs ──────────────────────────────────────────── */
struct SLSCapCreateIOPortRequest {
    uint64_t phys_base;       /* physical address (or port number on x86) */
    uint32_t length;          /* length in bytes (or port count) */
    uint8_t  width;           /* 1, 2, or 4 bytes per access */
    uint8_t  flags;           /* IO_FLAGS_* */
    uint8_t  perm;            /* CAP_PERM_IO_READ | CAP_PERM_IO_WRITE */
    uint8_t  _pad;
};

struct SLSCapCreateIRQRequest {
    uint32_t irq_number;       /* PLIC source or MSI vector */
    uint8_t  trigger;          /* edge/level, high/low */
    uint8_t  perm;             /* CAP_PERM_IRQ_LISTEN | ACK | MASK */
    uint16_t _pad;
    uint64_t coalesce_us;      /* 0 = no coalescing */
};

struct SLSIRQMaskRequest {
    uint16_t irq_cap_idx;      /* IRQ capability slot */
    uint8_t  masked;            /* 1 = mask, 0 = unmask */
    uint8_t  _pad;
};

struct SLSIRQAckRequest {
    uint16_t irq_cap_idx;      /* IRQ capability slot */
    uint8_t  _pad[6];
};

struct SLSCapCreateDMAMemRequest {
    uint32_t npages;          /* number of 4K pages */
    uint32_t align_pages;     /* minimum alignment in pages (1=4K, 16=64K, 512=2M) */
    uint8_t  flags;           /* DMA_BUF_FLAG_* */
    uint8_t  _pad[3];
    uint32_t out_cap_idx;     /* [out] MEM cap index in caller's table */
    uint32_t out_dma_buf_id;  /* [out] DMA buffer id */
};

struct SLSCapCreateBusRequest {
    uint8_t  bus_root;        /* starting bus number */
    uint8_t  bus_max;         /* ending bus number */
    uint8_t  perm;            /* CAP_PERM_BUS_ENUM | CFG_R | CFG_W */
    uint8_t  _pad;
};

/* ─── Trampoline capability (Deliverable 5, same-ring trampoline) ──────
 * A TRAMP cap grants the caller permission to directly branch to a
 * pre-verified entry point in the callee's address space, with hardware-
 * enforced memory protection (Intel MPK / AMD PKU). The kernel issues
 * TRAMP caps only when both sidecars run in ring 0, declare mutual
 * trust, and the CPU supports MPK. See docs/AeroSLS-Polyglot-Nexus-
 * Phase3-Design-v0.1.md §5 for the full rationale. ────────────────── */
#define CAP_TYPE_TRAMP  4

/* Phase 4: IO_PORT flags (passed via SLSCapCreateIOPortRequest.flags) */
#define CAP_IO_FLAG_UNCACHEABLE  0x01  /* map as UC (MTRR/MATR) */
#define CAP_IO_FLAG_READ_ONLY    0x02  /* device read-only region */
#define CAP_IO_FLAG_BE_MEM       0x04  /* big-endian MMIO */

/* Phase 4: DMA buffer flags (passed via SLSCapCreateDMAMemRequest.flags) */
#define CAP_DMA_FLAG_COHERENT    0x01  /* cache-coherent DMA */
#define CAP_DMA_FLAG_RO_DEVICE   0x02  /* device reads only */
#define CAP_DMA_FLAG_WO_DEVICE   0x04  /* device writes only */

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
uint64_t sys_sls_cap_send_msg(struct SLSCapSendMsgRequest* req);
uint64_t sys_sls_cap_recv_msg(struct SLSCapRecvMsgRequest* req);
uint64_t sys_sls_cap_arena_free(struct SLSCapArenaFreeRequest* req);

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

/* ─── Phase 4: device driver SDK public API ────────────────────────────────
 * Create capabilities for hardware access. All return 0 on success with
 * *out_idx set to the new cap slot, or a negative CAP_E* on failure.
 * `pid` is the caller. */
int cap_create_io_port(uint32_t pid, uint64_t phys_base, uint32_t length,
                       uint8_t width, uint8_t flags, uint8_t perm,
                       uint16_t* out_idx);
int cap_create_irq(uint32_t pid, uint32_t irq_number, uint8_t trigger,
                   uint8_t perm, uint64_t coalesce_us, uint16_t* out_idx);
int cap_create_dma_mem(uint32_t pid, uint32_t npages, uint32_t align_pages,
                       uint8_t flags, uint16_t* out_cap_idx,
                       uint32_t* out_dma_buf_id);
int cap_create_bus(uint32_t pid, uint8_t bus_root, uint8_t bus_max,
                   uint8_t perm, uint16_t* out_idx);

/* IRQ control: mask/unmask at the interrupt controller, acknowledge (EOI). */
int cap_irq_mask(uint32_t pid, uint16_t irq_cap_idx, uint8_t masked);
int cap_irq_ack(uint32_t pid, uint16_t irq_cap_idx);

/* Weak arch hooks for interrupt controller operations.
 * RISC-V: PLIC claim/complete register. x86: APIC EOI register.
 * cap.c ships weak defaults that are no-ops; the real kernel provides
 * strong overrides. */
void cap_irq_mask_source(uint32_t irq_number, uint8_t masked);
void cap_irq_eoi(uint32_t irq_number);

/* IOMMU hooks (weak defaults in cap.c, strong in kernel/iommu.c). */
void cap_iommu_map(uint32_t domain_id, uint64_t phys_base, uint32_t npages);
void cap_iommu_unmap(uint32_t domain_id, uint64_t phys_base, uint32_t npages);

/* DMA buffer pool hooks (weak defaults in cap.c, strong in kernel/dma.c). */
int  cap_dma_pin(uint64_t phys_base, uint32_t npages);
void cap_dma_unpin(uint64_t phys_base, uint32_t npages);

/* Debug: dump IRQ cap info. */
void cap_irq_list(void);

/* ─── Phase 4: syscall wrappers ──────────────────────────────────────────── */
uint64_t sys_sls_cap_create_io_port(struct SLSCapCreateIOPortRequest* req);
uint64_t sys_sls_cap_create_irq(struct SLSCapCreateIRQRequest* req);
uint64_t sys_sls_irq_mask(struct SLSIRQMaskRequest* req);
uint64_t sys_sls_irq_ack(struct SLSIRQAckRequest* req);
uint64_t sys_sls_cap_create_dma_mem(struct SLSCapCreateDMAMemRequest* req);
uint64_t sys_sls_cap_create_bus(struct SLSCapCreateBusRequest* req);

#endif /* CAP_H */
