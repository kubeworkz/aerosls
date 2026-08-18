/* user/libaerocap/aerocap.h — AeroSLS Capability SDK (Seed Kernel Phase 1)
 *
 * The Ring-3 skin over the SYS_SLS_CAP_* syscalls (289-297). Hides the raw
 * do_syscall ABI — request structs, error widening, and the syscall-number
 * constants — behind named wrappers, exactly as the design doc §6.1's
 * "one C wrapper per syscall plus a header" describes. A program that wants
 * to hand capabilities around includes this header and calls cap_create_mem,
 * cap_send, cap_recv, cap_map, cap_revoke, etc.; it never sees a request
 * struct or a raw syscall number.
 *
 * Conventions (matching kernel/cap.h, whose constants these MUST mirror —
 * kept in sync by hand, same posture as libsls/sls.h's SLS_SYS_* numbers):
 *   cap_t        = int32_t, fd-like small-integer capability index
 *   CAP_NONE     = 0xFFFF,  "no capability" sentinel (plain-message recv)
 *   return value = 0 on success (out-params filled), negative CAP_E* on error
 *
 * Every request struct below is byte-for-byte identical to the kernel's
 * SLSCap*Request (kernel/cap.h). tests/aerocap_abi_host_test.c pins that
 * identity with sizeof/offsetof comparisons against the kernel header, so a
 * drift on either side fails CI instead of corrupting a syscall.
 *
 * The wrappers call the kernel through libsls's _sls_syscall() (syscall
 * instruction). The kernel resolves the CALLING process from its own
 * scheduler state (cap_current_pid()), so no pid argument appears here —
 * unlike the kernel-side cap_* functions, which take pid explicitly.
 *
 * Ring-3 caveat (Phase 1, same as the kernel-shell demo): channel direction
 * is keyed on pid (end0 sends to q[1], the far end to q[0]), so a SINGLE
 * process holding both endpoint pairs (far_pid=0) cannot round-trip a cap
 * through one channel — sends always land in q[1], recvs always read q[0].
 * The two-party pattern (A creates, mints far caps into B's table via
 * far_pid=B, B recvs on its own read end) is the real acceptance scenario;
 * a single-process program can still exercise every wrapper, including a
 * real cap_map that installs a user PTE in ITS OWN address space.
 *
 * Build:
 *   make user-programs          (USER_CFLAGS gains -Iuser/libaerocap)
 */

#ifndef AEROCAP_H
#define AEROCAP_H

#include <sls.h>   /* _sls_syscall, sls_memset */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capability index type ─────────────────────────────────────────────────── */
typedef int32_t cap_t;

/* ── Syscall numbers (MUST match kernel/cap.h SYS_SLS_CAP_*) ───────────────── */
#define SLS_SYS_CAP_CREATE_MEM   289
#define SLS_SYS_CAP_ARENA_ALLOC  290
#define SLS_SYS_CHAN_CREATE      291
#define SLS_SYS_CAP_SEND         292
#define SLS_SYS_CAP_RECV         293
#define SLS_SYS_CAP_REVOKE       294
#define SLS_SYS_CAP_MAP          295
#define SLS_SYS_CAP_UNMAP        296
#define SLS_SYS_CAP_LIST         297

/* ── Sentinel / limits (MUST match kernel/cap.h) ───────────────────────────── */
#define CAP_NONE 0xFFFF   /* "no capability" sentinel (slot / plain message) */

/* ── Permission bits (MUST match kernel/cap.h) ──────────────────────────────── */
#define CAP_PERM_R   0x01   /* MEM: readable mapping   / CHAN: can send  */
#define CAP_PERM_W   0x02   /* MEM: writable mapping   / CHAN: can recv  */
#define CAP_PERM_X   0x04   /* MEM: executable mapping                    */
#define CAP_PERM_MAP 0x08   /* MEM: holder may map the range              */

/* ── Error codes (MUST match kernel/cap.h CAP_E*, widened int64) ───────────── */
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

/* ── Request structs — layout MUST be identical to kernel/cap.h ────────────── */
struct sls_cap_create_mem_req {
    uint64_t phys_base;
    uint32_t npages;
    uint32_t perm;
};

struct sls_cap_arena_alloc_req {
    uint32_t npages;
    uint32_t perm;
};

/* far_pid == 0 mints the far end's caps into the CALLER's table; far_pid
 * nonzero mints them directly into that process's table (socket-pair style). */
struct sls_cap_chan_create_req {
    uint32_t far_pid;
    uint16_t out_rd;          /* [out] caller's read end  (CHAN_R) */
    uint16_t out_wr;          /* [out] caller's write end (CHAN_W) */
    uint16_t out_far_rd;      /* [out] far end's read end  (CHAN_R) */
    uint16_t out_far_wr;      /* [out] far end's write end (CHAN_W) */
    uint8_t  _pad[6];
};

struct sls_cap_send_req {
    uint16_t ch_w_idx;        /* caller's CHAN_W slot */
    uint16_t cap_idx;         /* cap to MOVE, or CAP_NONE for a plain message */
    uint8_t  _pad[4];
    uint64_t cookie;
};

struct sls_cap_recv_req {
    uint16_t ch_r_idx;        /* caller's CHAN_R slot */
    uint8_t  block;           /* 1 = park the process mid-syscall when empty */
    uint8_t  _pad[1];
    uint32_t release_pid;     /* Phase 1.5: release this HELD pid before park */
    uint64_t cookie;          /* [out] */
};

struct sls_cap_revoke_req {
    uint16_t cap_idx;
    uint8_t  _pad[6];
};

struct sls_cap_map_req {
    uint16_t cap_idx;
    uint8_t  _pad[6];
    uint64_t vaddr;
    uint32_t flags;           /* CAP_PERM_R|W|X applied to the mapping */
    uint8_t  _pad2[4];
};

struct sls_cap_unmap_req {
    uint16_t cap_idx;
    uint8_t  _pad[6];
    uint64_t vaddr;
};

/* ── Wrappers ──────────────────────────────────────────────────────────────── */

/* Wrap an existing physical range in a MEM capability. Returns 0 on success
 * with *out_idx set, or a negative CAP_E*. */
static inline int cap_create_mem(uint64_t phys_base, uint32_t npages,
                                 uint32_t perm, cap_t* out_idx) {
    struct sls_cap_create_mem_req req;
    sls_memset(&req, 0, sizeof(req));
    req.phys_base = phys_base;
    req.npages    = npages;
    req.perm      = perm;
    int64_t r = (int64_t)_sls_syscall(SLS_SYS_CAP_CREATE_MEM, &req);
    if (r < 0) return (int)r;
    if (out_idx) *out_idx = (cap_t)r;
    return 0;
}

/* Allocate npages from the kernel's shared-memory arena. Returns 0 on
 * success with *out_idx set, or a negative CAP_E*. */
static inline int cap_arena_alloc(uint32_t npages, uint32_t perm,
                                  cap_t* out_idx) {
    struct sls_cap_arena_alloc_req req;
    sls_memset(&req, 0, sizeof(req));
    req.npages = npages;
    req.perm   = perm;
    int64_t r = (int64_t)_sls_syscall(SLS_SYS_CAP_ARENA_ALLOC, &req);
    if (r < 0) return (int)r;
    if (out_idx) *out_idx = (cap_t)r;
    return 0;
}

/* Create a bidirectional channel, provisioning BOTH endpoints. Returns 0 on
 * success with the four cap indices filled, or a negative CAP_E*. */
static inline int cap_chan_create(uint32_t far_pid,
                                  cap_t* out_rd, cap_t* out_wr,
                                  cap_t* out_far_rd, cap_t* out_far_wr) {
    struct sls_cap_chan_create_req req;
    sls_memset(&req, 0, sizeof(req));
    req.far_pid = far_pid;
    int64_t r = (int64_t)_sls_syscall(SLS_SYS_CHAN_CREATE, &req);
    if (r < 0) return (int)r;
    if (out_rd)     *out_rd     = (cap_t)req.out_rd;
    if (out_wr)     *out_wr     = (cap_t)req.out_wr;
    if (out_far_rd) *out_far_rd = (cap_t)req.out_far_rd;
    if (out_far_wr) *out_far_wr = (cap_t)req.out_far_wr;
    return 0;
}

/* Send a message over a CHAN_W. cap_idx == CAP_NONE sends a plain message;
 * otherwise the capability MOVES from the caller's table into the channel.
 * Returns 0 on success, or a negative CAP_E*. */
static inline int cap_send(cap_t ch_w_idx, cap_t cap_idx, uint64_t cookie) {
    struct sls_cap_send_req req;
    sls_memset(&req, 0, sizeof(req));
    req.ch_w_idx = (uint16_t)ch_w_idx;
    req.cap_idx  = (uint16_t)cap_idx;
    req.cookie   = cookie;
    return (int)(int64_t)_sls_syscall(SLS_SYS_CAP_SEND, &req);
}

/* Receive a message from a CHAN_R. On success (0) *cookie_out holds the
 * sender's cookie and *out_cap the received capability index — CAP_NONE for
 * a plain message. Returns a negative CAP_E* on error (CAP_EAGAIN when the
 * queue is empty: Phase-1 recv is non-blocking; retry in user space). */
static inline int cap_recv(cap_t ch_r_idx, int block,
                           uint64_t* cookie_out, cap_t* out_cap) {
    struct sls_cap_recv_req req;
    sls_memset(&req, 0, sizeof(req));
    req.ch_r_idx = (uint16_t)ch_r_idx;
    req.block    = block ? 1 : 0;
    int64_t r = (int64_t)_sls_syscall(SLS_SYS_CAP_RECV, &req);
    if (r < 0) return (int)r;
    if (cookie_out) *cookie_out = req.cookie;
    if (out_cap)    *out_cap    = (cap_t)r;
    return 0;
}

/* Blocking recv that FIRST releases a HELD process — the release and the
 * park happen inside ONE syscall, so no user-mode instruction runs between
 * them and a timer tick cannot schedule the released process before we park
 * (the Phase 1.5 overlap test's immediacy proof depends on this). The
 * released process then runs LIVE while we are parked in the recv. */
static inline int cap_recv_release(cap_t ch_r_idx, int block,
                                   uint32_t release_pid,
                                   uint64_t* cookie_out, cap_t* out_cap) {
    struct sls_cap_recv_req req;
    sls_memset(&req, 0, sizeof(req));
    req.ch_r_idx   = (uint16_t)ch_r_idx;
    req.block      = block ? 1 : 0;
    req.release_pid = release_pid;
    int64_t r = (int64_t)_sls_syscall(SLS_SYS_CAP_RECV, &req);
    if (r < 0) return (int)r;
    if (cookie_out) *cookie_out = req.cookie;
    if (out_cap)    *out_cap    = (cap_t)r;
    return 0;
}

/* Immediately revoke a capability: the cap word is invalidated and its
 * object is destroyed when no holder remains (a total post-return
 * guarantee — no cap survives). Returns 0, or a negative CAP_E*. */
static inline int cap_revoke(cap_t cap_idx) {
    struct sls_cap_revoke_req req;
    sls_memset(&req, 0, sizeof(req));
    req.cap_idx = (uint16_t)cap_idx;
    return (int)(int64_t)_sls_syscall(SLS_SYS_CAP_REVOKE, &req);
}

/* Map a MEM capability's physical range into the CALLING process's address
 * space at vaddr (4 KiB-aligned) with the given CAP_PERM_R|W|X. Returns 0,
 * or a negative CAP_E*. */
static inline int cap_map(cap_t cap_idx, uint64_t vaddr, uint32_t flags) {
    struct sls_cap_map_req req;
    sls_memset(&req, 0, sizeof(req));
    req.cap_idx = (uint16_t)cap_idx;
    req.vaddr   = vaddr;
    req.flags   = flags;
    return (int)(int64_t)_sls_syscall(SLS_SYS_CAP_MAP, &req);
}

/* Tear down a mapping previously created by cap_map(). Returns 0, or a
 * negative CAP_E*. */
static inline int cap_unmap(cap_t cap_idx, uint64_t vaddr) {
    struct sls_cap_unmap_req req;
    sls_memset(&req, 0, sizeof(req));
    req.cap_idx = (uint16_t)cap_idx;
    req.vaddr   = vaddr;
    return (int)(int64_t)_sls_syscall(SLS_SYS_CAP_UNMAP, &req);
}

/* Dump all capability tables to the kernel serial log (debug, mirrors
 * proc list). No return value. */
static inline void cap_list(void) {
    _sls_syscall(SLS_SYS_CAP_LIST, 0);
}

#ifdef __cplusplus
}
#endif

#endif /* AEROCAP_H */
