/*
 * linux_abi.c — Phase E7, increment 1: the three contracts as code.
 *
 * Pure functions only: no state, no allocation, no trap path, no sidecar.
 * Each one is a total function whose every input is either enumerated by
 * the census artifact or refused by name — the same "no silent default"
 * discipline the rest of this tree applies to on-disk formats. See
 * kernel/linux_abi.h for why this increment lands before any mechanism.
 */
#include <stddef.h>  /* offsetof — a freestanding header, not libc: the frame's
                      * field offsets are asserted against a local mirror, so a
                      * compiler that disagrees stops the build (clause P) */
#include "linux_abi.h"
#include "cap.h"      /* CAP_E* (Phase-3 syscalls), CAP_ERR_* (Phase-5 k_chan), CAP_NONE */

/* ─── The errno map (§5.7) ─────────────────────────────────────────────────
 * Forward: CAP code -> Linux errno, total over both families and the
 * CAP_NONE sentinel. The load-bearing decision is the sentinel's: an
 * out-of-band value is a kernel answer that was not an answer, and the
 * only honest translation is the generic "the kernel did not produce a
 * result" — EIO — never a plausible errno a tenant binary might treat as
 * a retry or an absence. Values outside both families get the same
 * treatment for the same reason: they cannot have crossed here honestly. */
int32_t linux_errno_from_cap(int32_t cap_code)
{
    switch (cap_code) {
    case 0:                          return 0;                 /* success */
    /* ── Phase-5 k_chan_ family (positive; cap.h CAP_ERR_*) ──
     * CAP_ERR_OK is 0 and shares the success arm above. */
    case CAP_ERR_NOTFOUND:           return -LINUX_ENOENT;     /* object absent */
    case CAP_ERR_REVOKED:            return -LINUX_EIO;        /* died under us mid-op */
    case CAP_ERR_RIGHTS:             return -LINUX_EPERM;      /* rights check failed */
    case CAP_ERR_RANGE:              return -LINUX_ERANGE;
    case CAP_ERR_BUDGET:             return -LINUX_ENOMEM;     /* budget, not RAM: the
                                                                 * task's memory ask hit
                                                                 * the partition's wall */
    case CAP_ERR_SPACE:              return -LINUX_ENOBUFS;    /* channel full, not memory */
    case CAP_ERR_TARGET:             return -LINUX_ESRCH;      /* the peer is gone */
    case CAP_ERR_STATE:              return -LINUX_EBUSY;      /* wrong state for the op */
    case CAP_ERR_TYPE:               return -LINUX_EINVAL;     /* malformed object word */
    case CAP_ERR_PROTO:              return -LINUX_EPROTO;     /* conversation broke */
    case CAP_ERR_NOMEM:              return -LINUX_ENOMEM;
    case CAP_ERR_BUFSZ:              return -LINUX_EINVAL;     /* request won't fit the msg */
    case CAP_ERR_TIMEOUT:            return -LINUX_ETIMEDOUT;
    /* ── Phase-3 syscall family (negative; cap.h CAP_E*) ── */
    case CAP_EINVAL:                 return -LINUX_EINVAL;
    case CAP_EBADF:                  return -LINUX_EBADF;
    case CAP_EAGAIN:                 return -LINUX_EAGAIN;
    case CAP_ETABLEFULL:             return -LINUX_ENFILE;     /* a fixed KERNEL table:
                                                                 * system-scoped, not the
                                                                 * fd-scoped EMFILE */
    case CAP_ECAPREVOKED:            return -LINUX_EIO;        /* handle died under us */
    case CAP_EALREADY:               return -LINUX_EALREADY;
    case CAP_ENOMEM:                 return -LINUX_ENOMEM;
    case CAP_ENOSPC:                 return -LINUX_ENOSPC;
    case CAP_ERANGE:                 return -LINUX_ERANGE;
    case CAP_ENOSYS:                 return -LINUX_ENOSYS;
    case CAP_ECONFLICT:              return -LINUX_EBUSY;      /* state conflict */
    /* ── E7 file class (step 4): the VFS's own failures, each its own
     * code so a mkdir's EEXIST reaches the task as 17 and not as a
     * generic EIO the caller cannot branch on (cap.h's (-13)..(-20)). */
    case CAP_EEXIST:                 return -LINUX_EEXIST;
    case CAP_ENOTDIR:                return -LINUX_ENOTDIR;
    case CAP_EISDIR:                 return -LINUX_EISDIR;
    case CAP_EACCES:                 return -LINUX_EACCES;
    case CAP_EROFS:                  return -LINUX_EROFS;
    case CAP_ESPIPE:                 return -LINUX_ESPIPE;
    case CAP_ENOTEMPTY:              return -LINUX_ENOTEMPTY;
    case CAP_ENAMETOOLONG:           return -LINUX_ENAMETOOLONG;
    case CAP_EPERM:                  return -LINUX_EPERM;
    /* E7 increment 27: the park's wake. Its own code rather than a reuse of
     * EAGAIN, because a caller retries an EINTR and must not retry a
     * would-block: the two are different answers to different questions. */
    case CAP_EINTR:                  return -LINUX_EINTR;
    default:
        /* CAP_NONE (0xFFFF, the out-param sentinel) and anything else that
         * is not an enumerated code: translated, never leaked (§5.7). */
        return -LINUX_EIO;
    }
}

/* Reverse: Linux errno -> CAP code, defined ONLY where the preimage is
 * unique. Five errnos collapse two or three CAP codes onto one Linux value
 * (EPERM, ENOMEM, EINVAL, EIO, EBUSY); for those the reverse is 0 = not
 * defined, because picking one would invent a kernel-side truth the errno
 * does not carry. The host test round-trips exactly the defined ones. */
int32_t cap_code_from_linux_errno(int32_t linux_errno)
{
    switch (linux_errno) {
    case -LINUX_ENOENT:    return CAP_ERR_NOTFOUND;
    case -LINUX_ESRCH:     return CAP_ERR_TARGET;
    case -LINUX_EBADF:     return CAP_EBADF;
    case -LINUX_EAGAIN:    return CAP_EAGAIN;
    case -LINUX_ENFILE:    return CAP_ETABLEFULL;
    case -LINUX_EALREADY:  return CAP_EALREADY;
    case -LINUX_ENOSPC:    return CAP_ENOSPC;
    case -LINUX_ERANGE:    return CAP_ERANGE;
    case -LINUX_ENOSYS:    return CAP_ENOSYS;
    case -LINUX_ENOBUFS:   return CAP_ERR_SPACE;
    case -LINUX_EPROTO:    return CAP_ERR_PROTO;
    case -LINUX_ETIMEDOUT: return CAP_ERR_TIMEOUT;
    /* E7 file class: each new code has exactly ONE preimage, so each
     * reverse is defined — the round-trip the host test pins. */
    case -LINUX_EEXIST:    return CAP_EEXIST;
    case -LINUX_ENOTDIR:   return CAP_ENOTDIR;
    case -LINUX_EISDIR:    return CAP_EISDIR;
    case -LINUX_EACCES:    return CAP_EACCES;
    case -LINUX_EROFS:     return CAP_EROFS;
    case -LINUX_ESPIPE:    return CAP_ESPIPE;
    case -LINUX_ENOTEMPTY: return CAP_ENOTEMPTY;
    case -LINUX_ENAMETOOLONG: return CAP_ENAMETOOLONG;
    default:               return 0;   /* no unique preimage (or not an error) */
    }
}

/* ─── The syscall classification table (§5.6) ──────────────────────────────
 * Built FROM tools/linux_syscall_census.txt: 27 rows, plus the four
 * epoll/eventfd rows the Go gate's census requires
 * (tools/go_thread_gate_census.txt — TG-0); sorted by number.
 * The class names where the semantics live, per §5.6's destination table;
 * the flag pins §5.4's W^X refusal against the census's own mprotect row.
 *
 * The absence of a row is as load-bearing as any row: 105 (SLS
 * SYS_SLS_ALLOCATE / Linux setuid), 160 (SYS_SLS_PROC_CREATE) and 320
 * (SYS_SLS_ALLOC_REGION / kexec_file_load) have NO entry, so a
 * Linux-personality dispatch that consults this table refuses them by
 * name — the number-space property §2.2 requires, made structural. */
const struct LinuxSyscallEntry linux_syscall_table[] = {
    {   0, LSC_FD,     0 },   /* read */
    {   1, LSC_FD,     0 },   /* write */
    {   2, LSC_FD,     0 },   /* open */
    {   3, LSC_FD,     0 },   /* close */
    {   4, LSC_FD,     0 },   /* stat */
    {   6, LSC_FD,     0 },   /* lstat */
    {   7, LSC_FD,     0 },   /* poll */
    {   8, LSC_FD,     0 },   /* lseek */
    {   9, LSC_MEM,    0 },   /* mmap — §5.4 budget-backed; the anon-ymous  */
                              /* map half landed at increment 16            */
    {  10, LSC_MEM,    LSC_F_REFUSES_WX }, /* mprotect — W|X refused (§5.4) */
    {  11, LSC_MEM,    0 },   /* munmap */
    {  12, LSC_MEM,    0 },   /* brk — §5.4 bump pointer in the budget */
    {  13, LSC_SIG,    0 },   /* rt_sigaction — the whole act recorded; a handler is never invoked */
    {  14, LSC_SIG,    0 },   /* rt_sigprocmask — recorded; unblock delivers (step 2) */
    {  39, LSC_TASK,   0 },   /* getpid — the group's leader pid (TG-1 I2) */
    {  56, LSC_THREAD, 0 },   /* clone — the Go thread shape, kernel-local (I2) */
    {  59, LSC_SPAWN,  0 },   /* execve — the spawn path */
    {  60, LSC_TASK,   0 },   /* exit — thread-local teardown (TG-1 I2) */
    {  72, LSC_FD,     0 },   /* fcntl */
    {  74, LSC_FD,     0 },   /* fsync */
    {  77, LSC_FD,     0 },   /* ftruncate */
    {  82, LSC_FD,     0 },   /* rename — the .img.partial -> final swap */
    {  83, LSC_FD,     0 },   /* mkdir */
    {  87, LSC_FD,     0 },   /* unlink */
    { 131, LSC_SIG,    0 },   /* sigaltstack — recorded, never delivered */
    { 158, LSC_MSR,    0 },   /* arch_prctl(ARCH_SET_FS) — §5.3's one MSR */
    { 186, LSC_TASK,   0 },   /* gettid — this descriptor's pid (TG-1 I2) */
    { 202, LSC_FUTEX,  0 },   /* futex — private wait/wake, kernel-local (I3) */
    { 217, LSC_FD,     0 },   /* getdents64 */
    { 218, LSC_LOCAL,  0 },   /* set_tid_address — single-threaded: stored and ignored */
    { 228, LSC_LOCAL,  0 },   /* clock_gettime — vDSO-backstopped (§5.5): a real trap here */
    { 231, LSC_EXIT,   0 },   /* exit_group — the sidecar reaps, frames return */
    /* The Go gate's netpoll startup (TG-0): all four are required-by-runtime
     * rows, and the sidecar records the descriptors and epoll membership
     * with no event source behind them — epoll_pwait's zero-timeout probe
     * answers 0, a blocking wait is refused by name (linux.rs answer_event). */
    { 233, LSC_FD,     0 },   /* epoll_ctl */
    { 257, LSC_FD,     0 },   /* openat — the gate's measured path open (I4) */
    { 281, LSC_FD,     0 },   /* epoll_pwait — zero-timeout probe only */
    { 290, LSC_FD,     0 },   /* eventfd2 */
    { 291, LSC_FD,     0 },   /* epoll_create1 */
};
const size_t linux_syscall_table_len =
    sizeof(linux_syscall_table) / sizeof(linux_syscall_table[0]);

enum LinuxSyscallClass linux_syscall_class(uint64_t num)
{
    size_t i;
    for (i = 0; i < linux_syscall_table_len; i++) {
        if (linux_syscall_table[i].num == num)
            return linux_syscall_table[i].cls;
    }
    return LSC_UNKNOWN;   /* not in the census: refused by name, never dispatched */
}

uint32_t linux_syscall_flags(uint64_t num)
{
    size_t i;
    for (i = 0; i < linux_syscall_table_len; i++) {
        if (linux_syscall_table[i].num == num)
            return linux_syscall_table[i].flags;
    }
    return 0;
}

const char *linux_syscall_class_name(enum LinuxSyscallClass cls)
{
    switch (cls) {
    case LSC_FD:     return "fd";
    case LSC_MEM:    return "mem";
    case LSC_SIG:    return "sig";
    case LSC_MSR:    return "msr";
    case LSC_SPAWN:  return "spawn";
    case LSC_THREAD: return "thread";
    case LSC_TASK:   return "task";
    case LSC_FUTEX:  return "futex";
    case LSC_LOCAL:  return "local";
    case LSC_EXIT:   return "exit";
    case LSC_UNKNOWN:
    default:         return "unknown";
    }
}

/* ─── E7 §5.3 (the plan's increment 4) — arch_prctl, as two pure bounds ───
 * Named here beside their siblings so the host test bites WITHOUT a boot:
 * the canonicality of a proposed FS base (the kernel re-checks it at the
 * wrmsr, whatever the sidecar's grant said) and the stage/apply predicate
 * that both ends of the forward seam share. Nothing below reads kernel
 * state: given the same two words, every caller gets the same answer, which
 * is what makes "the sidecar decides, the kernel performs" assertable in
 * BOTH directions rather than observed in one boot. */
int linux_fs_addr_is_user_canonical(uint64_t addr)
{
    /* User-canonical on a 48-bit VA: bits 63..47 all zero — for an
     * unsigned value, exactly addr <= 0x0000_7FFF_FFFF_FFFF. (0 passes:
     * a degenerate but legal TLS base, and inventing a rejection here
     * would be a rule no Linux kernel has.) */
    return addr <= LINUX_USER_ADDR_MAX;
}

int linux_arch_prctl_is_set_fs(uint64_t num, uint64_t cmd)
{
    return num == LINUX_ARCH_PRCTL && cmd == LINUX_ARCH_SET_FS;
}

/* ─── E7 increment 5 (the fd class) — the plan, and the copy ───────────────
 * The two halves the header names, implemented as the pure functions the
 * host test drives. Neither reads kernel state: the plan is arithmetic
 * over the captured registers, the copy is a walk of a root the CALLER
 * supplies (the task's PML4 in production, a fake table in the test).
 *
 * The plan's one soft spot, chosen deliberately: count == 0 plans nothing
 * before canonicality is consulted. Linux does not fault a buffer it is
 * not asked to touch — write(fd, NULL, 0) is legal — and inventing an
 * -EFAULT there would be a rule no kernel the candidate was compiled
 * against enforces (same discipline as 0 passing the §5.3 bound). */
struct LinuxFdWritePlan linux_fd_write_plan(uint64_t num,
                                            const uint64_t args[6])
{
    struct LinuxFdWritePlan p;
    uint64_t count, span_end;

    p.src = 0;
    p.len = 0;
    p.refuse = 0;
    if (!args) return p;                         /* nothing to attach */

    /* poll(2): the array is the IN direction's byte crossing. The sidecar
     * holds the request pending until an fd is ready or the timeout fires,
     * and returns a rewritten array; this plan only carries the snapshot
     * out. A zero-nfds poll is a pure sleep and attaches nothing. */
    if (num == LINUX_POLL) {
        uint64_t nfds = args[1];
        if (nfds > LINUX_POLL_MAX_FDS) {
            p.refuse = -LINUX_EINVAL;
            return p;
        }
        if (nfds == 0) return p;
        if (args[0] == 0) {
            p.refuse = -LINUX_EFAULT;
            return p;
        }
        p.src = args[0];
        p.len = (uint32_t)(nfds * LINUX_POLLFD_BYTES);
        span_end = p.src + (uint64_t)p.len;
        if (!linux_fs_addr_is_user_canonical(p.src) || span_end < p.src ||
            (span_end - 1) > LINUX_USER_ADDR_MAX) {
            p.refuse = -LINUX_EFAULT;
        }
        return p;
    }

    if (num != LINUX_WRITE) return p;             /* nothing to attach */

    count = args[2];
    if (count == 0) return p;                    /* nothing to copy, and
                                                  * nothing to validate */

    p.src = args[1];
    p.len = count > LINUX_FD_WRITE_CHUNK
              ? (uint32_t)LINUX_FD_WRITE_CHUNK : (uint32_t)count;
    span_end = p.src + (uint64_t)p.len;          /* wrap = refuse below */
    if (!linux_fs_addr_is_user_canonical(p.src) || span_end < p.src ||
        (span_end - 1) > LINUX_USER_ADDR_MAX) {
        p.refuse = -LINUX_EFAULT;
    }
    return p;
}

/* Page-table flag bits, owned here as literals because linux/asm/page_64
 * types are not includable (the cap.h discipline for CAP_E*): present,
 * user, huge-page (PS at the PDPTE/PDE levels only — bit 7 of a 4 KiB
 * PTE is PAT, not size), and the 4 KiB frame mask. */
#define LAB_PTE_PRESENT  0x1ULL
#define LAB_PTE_RW       0x2ULL   /* E7 increment 12: the copy-IN's
                                  * writability boundary */
#define LAB_PTE_USER     0x4ULL
#define LAB_PTE_PS       0x80ULL
#define LAB_PTE_FRAME    0x000FFFFFFFFFF000ULL
#define LAB_PTE_SOFT     (1ULL << 9)  /* E7 §10.15: PRESENT, U=0,
                                       * soft-protected leaf */

/* The copy. Read-ONLY by construction: every level is checked before the
 * next pointer is followed, and nothing anywhere writes a table — the
 * walker this could have reused (arch/x86/walk_page_tables_x86.c)
 * ALLOCATES a missing table, which over a tenant-supplied address would be
 * a memory-exhaustion lever, not a copy. The USER bit is checked at every
 * level, matching what a ring-3 load would require: the copy can then
 * only read bytes this task could read itself, which is the whole
 * security argument for letting the sidecar see them.
 *
 * Frame -> pointer is the identity map's own claim (boot.asm: "the
 * physical addr = virtual addr"). x86-64 keeps the walk byte-oriented at
 * the end so the test's fake frames are ordinary aligned arrays. */
/* The one leaf walk both readers share (increment 5's copy, increment 6's
 * frame read-back, increment 12's copy-in). Read-ONLY by construction:
 * every level is checked before the next pointer is followed, and
 * nothing anywhere writes a table or creates a missing one — the
 * allocating walker (arch/x86/walk_page_tables_x86.c) is exactly what
 * these must NOT be: over a tenant-supplied address its table allocation
 * is a memory lever, not a read. The USER bit is checked at every level,
 * matching what a ring-3 load would require; require_rw adds the write
 * side of that same rule (only copy-in sets it), checked at EVERY level
 * too — effective writability is the conjunction of all four entries,
 * and a writable leaf under a read-only directory would let the copy-in
 * launder a write through a page the child itself could not write.
 *
 * Frame -> pointer is the identity map's own claim (boot.asm: "the
 * physical addr = virtual addr"). */
static int lab_walk_leaf(uint64_t root, uint64_t va, uint64_t *leaf_out,
                         int require_rw, int accept_soft)
{
    const uint64_t *tbl = (const uint64_t *)(uintptr_t)(root & LAB_PTE_FRAME);
    uint64_t leaf = 0;
    unsigned lvl;

    for (lvl = 0; lvl < 4; lvl++) {
        unsigned shift = 39u - 9u * lvl;
        uint64_t e = tbl[(va >> shift) & 0x1FF];
        if (!(e & LAB_PTE_PRESENT)) return -LINUX_EFAULT;
        if ((lvl == 1 || lvl == 2) && (e & LAB_PTE_PS))
            return -LINUX_EFAULT;   /* a huge page is not this shim's
                                     * user shape; refused by name */
        if (!(e & LAB_PTE_USER) &&
            !(lvl == 3 && accept_soft && (e & LAB_PTE_SOFT)))
            return -LINUX_EFAULT;
        if (require_rw && !(e & LAB_PTE_RW)) return -LINUX_EFAULT;
        leaf = e;
        if (lvl < 3) tbl = (const uint64_t *)(uintptr_t)(e & LAB_PTE_FRAME);
    }
    if (leaf_out) *leaf_out = leaf;
    return 0;
}

int linux_user_copy_out(uint8_t *dst, uint64_t root, uint64_t va, uint32_t n)
{
    if (!dst) return -LINUX_EFAULT;
    while (n) {
        uint64_t leaf = 0;
        if (lab_walk_leaf(root, va, &leaf, 0, 0) != 0) return -LINUX_EFAULT;
        {
            uint32_t off = (uint32_t)(va & (LINUX_PAGE_SIZE - 1u));
            uint32_t page_left = LINUX_PAGE_SIZE - off;
            uint32_t take = n < page_left ? n : page_left;
            const uint8_t *src =
                (const uint8_t *)(uintptr_t)((leaf & LAB_PTE_FRAME) + off);
            uint32_t i;
            for (i = 0; i < take; i++) dst[i] = src[i];
            dst += take;
            va  += take;
            n   -= take;
            /* root is NOT reassigned: the next iteration re-walks all
             * four levels from the PML4 for the next page — leaf-frame
             * reuse would index a PT as though it were a PML4. */
        }
    }
    return 0;
}

/* ─── E7 increment 10 (the signal seam, §10.13's step 2) — the act/set copy plan ──
 * The write plan's twin over the sig class: same walker, same boundaries,
 * same first-trap discipline, different bytes (a fixed-size struct rather
 * than a caller-chosen count — so there is no clamp to ride in the header:
 * BOTH ends derive the attach length from num and args[1] != 0, which is
 * what the sidecar's payload check mirrors). The refusal the header
 * names (sigsetsize) is decided here — pure over the six
 * captured words, so the host test drives it without a boot. The
 * OUT-POINTER's span is no longer this plan's business: increment 12
 * planned it as a reply payload (linux_fd_reply_plan below) instead of
 * refusing it ENOSYS. */
struct LinuxSigCopyPlan linux_sig_copy_plan(uint64_t num, const uint64_t args[6])
{
    struct LinuxSigCopyPlan p;
    uint64_t src, span_end;

    p.src = 0;
    p.len = 0;
    p.refuse = 0;
    /* sigaltstack installs do not need their stack_t copied IN: this v1
     * records no alternative stack to deliver on, and the query's old
     * stack_t is the reply plan's job (linux_fd_reply_plan). */
    if (num == LINUX_SIGALTSTACK)
        return p;
    if (num != LINUX_RT_SIGACTION && num != LINUX_RT_SIGPROCMASK)
        return p;   /* not a sig-class crossing */
    if (!args) return p;

    if (args[3] != LINUX_SIGSET_BYTES) {
        p.refuse = -LINUX_EINVAL;   /* Linux's own answer to a foreign
                                     * sigsetsize */
        return p;
    }

    src = args[1];
    if (src == 0) return p;         /* act/set NULL with no out-pointer:
                                     * a no-op, nothing to copy */
    p.src = src;
    p.len = (num == LINUX_RT_SIGACTION) ? LINUX_SIGACT_BYTES
                                        : LINUX_SIGSET_BYTES;
    span_end = p.src + (uint64_t)p.len;
    if (!linux_fs_addr_is_user_canonical(p.src) || span_end < p.src ||
        (span_end - 1) > LINUX_USER_ADDR_MAX) {
        p.refuse = -LINUX_EFAULT;
    }
    return p;
}

/* ─── E7 increment 11 (§10.13's step-2 coda) — poll (7): the waiting-poll refusal ──
 * The pure half both ends of the seam share (the header names the why):
 * 1 iff this poll would WAIT, refused at the first trap; a zero-timeout
 * probe falls to the sidecar's 0-events answer. Linux's timeout is an
 * int, so the low 32 bits decide with C's own truncation — -1 (forever)
 * and 5000 (ms) refuse, 0 and 0x1_00000000 (which truncates to 0) do
 * not. Total over (num, timeout): a number that is not poll never waits
 * as one. */
/* ─── E7 increment 6 (the mem class, §5.4) — the budget span ──────────────
 * The two decisions the resume arm's brk apply makes, and the one read it
 * needs, as the pure functions the host test drives. Nothing below reads
 * kernel state: the verdict is arithmetic over the stamped span and the
 * staged request, the frame reader is a walk of a root the CALLER supplies
 * (the task's PML4 in production, a fake table in the test — the same fake
 * tables the copy's cases already build). Increment 16 joins the section:
 * mmap's first-trap refusals and its verdict are the same discipline over
 * four staged words, with the span bounded from both ends by the two
 * cursors the map owns. */
int linux_mprotect_refuses_wx(uint64_t prot)
{
    return (prot & LINUX_PROT_WRITE) && (prot & LINUX_PROT_EXEC);
}

/* E7 §10.15: the first-trap and reply-recheck halves of a real mprotect.
 * The range is bounded by the task's OWN image/stack/heap windows; the
 * page tables are checked at the reply by the apply (a present 4 KiB leaf
 * at every page), because a window can contain a hole. Nothing here maps,
 * allocates or frees. */
static int linux_mem_range_in(uint64_t addr, uint64_t len_r,
                              uint64_t lo, uint64_t hi)
{
    return lo < hi && addr >= lo && addr <= hi &&
           len_r <= hi - addr;
}

int linux_mem_mprotect_refusal(const struct LinuxMprotectReq *req,
                               uint64_t image_lo, uint64_t image_hi,
                               uint64_t stack_lo, uint64_t stack_hi,
                               uint64_t heap_lo, uint64_t heap_hi)
{
    uint64_t len_r;

    if (!req) return -LINUX_EINVAL;
    if (req->len == 0) return -LINUX_EINVAL;
    if (req->addr & (LINUX_PAGE_SIZE - 1)) return -LINUX_EINVAL;
    if (req->len > UINT64_MAX - (LINUX_PAGE_SIZE - 1))
        return -LINUX_EINVAL;
    if (req->prot & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
                      LINUX_PROT_EXEC)) return -LINUX_EINVAL;
    if (linux_mprotect_refuses_wx(req->prot)) return -LINUX_EPERM;

    len_r = (req->len + LINUX_PAGE_SIZE - 1)
          & ~((uint64_t)LINUX_PAGE_SIZE - 1);
    if (!linux_mem_range_in(req->addr, len_r, image_lo, image_hi) &&
        !linux_mem_range_in(req->addr, len_r, stack_lo, stack_hi) &&
        !linux_mem_range_in(req->addr, len_r, heap_lo, heap_hi))
        return -LINUX_ENOMEM;
    return 0;
}

struct LinuxMemMprotectVerdict
linux_mem_mprotect_verdict(uint64_t image_lo, uint64_t image_hi,
                           uint64_t stack_lo, uint64_t stack_hi,
                           uint64_t heap_lo, uint64_t heap_hi,
                           const struct LinuxMprotectReq *req, uint64_t rax)
{
    struct LinuxMemMprotectVerdict v;
    uint64_t len_r;

    v.malformed = 0;
    v.va = 0;
    v.pages = 0;

    /* A negative answer is the sidecar's refusal: its errno rides the
     * §5.7 map and nothing changes. mprotect's success value is 0, so any
     * other non-negative reply is a lie the kernel must not act on. */
    if ((int64_t)rax < 0) return v;
    if (rax != 0) {
        v.malformed = 1;
        return v;
    }

    /* The ladder re-runs over the staged words: the verdict is total on
     * any input, including a success for a shape the first trap would
     * have refused. */
    if (linux_mem_mprotect_refusal(req, image_lo, image_hi,
                                   stack_lo, stack_hi,
                                   heap_lo, heap_hi) != 0) {
        v.malformed = 1;
        return v;
    }

    len_r = (req->len + LINUX_PAGE_SIZE - 1)
          & ~((uint64_t)LINUX_PAGE_SIZE - 1);
    v.va = req->addr;
    v.pages = len_r / LINUX_PAGE_SIZE;
    return v;
}

struct LinuxMemBrkVerdict linux_mem_brk_verdict(uint64_t heap_base,
                                                uint64_t heap_limit,
                                                uint64_t mmap_top,
                                                uint64_t brk_cur,
                                                uint64_t req, uint64_t rax)
{
    struct LinuxMemBrkVerdict v;
    /* brk's ceiling is the span's limit CAPPED by the anonymous-map
     * cursor (increment 16): a page a granted mmap backed — or reserved
     * — is a page a brk growth may never map over (a re-map leaks the
     * frame underneath and zeros the bytes on it). mmap_top == 0 means
     * no cursor was ever stamped: the span's own limit stands. */
    uint64_t ceiling = (mmap_top != 0 && mmap_top < heap_limit)
                     ? mmap_top : heap_limit;
    v.malformed = 0;
    v.map_va = 0;
    v.map_pages = 0;
    v.new_cur = brk_cur;

    /* The denial echo — and the no-op ask (req == brk_cur). Linux's brk
     * failure shape is the CURRENT break returned, never an errno: the
     * sidecar answers the unchanged break for an over-budget ask and for
     * a shrink (v1 grows only), and both arrive here as "the break did
     * not move" — legal, not malformed, nothing to map. */
    if (rax == brk_cur) return v;

    /* A grant. The reply must echo the request exactly (increment 5's
     * one-truth rule, on an address), and the value must sit inside the
     * span the create stamped: the kernel never maps a page because a
     * grant said so — the span is its own truth, whatever the policy
     * side believed. Below the current break is a "granted" shrink, and
     * rax is not even the request's echo — both are the malformed arm,
     * never a map. */
    if (rax == req && req > brk_cur &&
        req >= heap_base && req <= ceiling) {
        uint64_t va0 = (brk_cur + LINUX_PAGE_SIZE - 1)
                     & ~((uint64_t)LINUX_PAGE_SIZE - 1);
        uint64_t va1 = (rax + LINUX_PAGE_SIZE - 1)
                     & ~((uint64_t)LINUX_PAGE_SIZE - 1);
        /* align_UP both ends: the page CONTAINING the old break is
         * already mapped (whatever moved the break into it mapped its
         * page), so [va0, va1) covers exactly the pages no grant has
         * backed yet — a re-map would leak the frame underneath and
         * zero the bytes on it. rax > brk_cur makes va1 > va0, so a
         * granted growth always maps at least one page. */
        v.map_va = va0;
        v.map_pages = (uint32_t)((va1 - va0) / LINUX_PAGE_SIZE);
        v.new_cur = rax;
        return v;
    }

    v.malformed = 1;   /* neither the grant nor the refusal: nothing
                        * maps, the break stays, the caller says so */
    return v;
}

/* ─── E7 increment 16 — the anonymous-map seam ───────────────────────────
 * The two decisions the first trap makes about a mmap (refuse the shape,
 * or stage the whole request), and the one the reply makes (was the grant
 * a lie?), as the pure functions the host test drives. The apply — the
 * replace walk, the frame allocs, the invlpg — is the resume arm's, on
 * this side of the §5.4 line the brk apply already sits behind. */
int linux_mem_mmap_refusal(const struct LinuxMmapReq *req,
                           uint64_t span_base, uint64_t span_limit)
{
    /* 0 or a NEGATIVE Linux errno — the copy plans' convention, so the
     * call site maps it straight into rax. The ladder reads
     * malformed-word first, then un-served shapes, then the span — each
     * step is a property of the REQUEST words alone, so no channel is
     * ever posted for a shape that cannot be answered. */
    uint64_t len_r;

    if (req->len == 0) return -LINUX_EINVAL;
    if (req->len > UINT64_MAX - (LINUX_PAGE_SIZE - 1))
        return -LINUX_EINVAL;
    len_r = (req->len + LINUX_PAGE_SIZE - 1)
          & ~((uint64_t)LINUX_PAGE_SIZE - 1);

    if (req->prot & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
                      LINUX_PROT_EXEC)) return -LINUX_EINVAL;

    /* W|X first: mprotect's own §5.4 rule over a prot word (the twin at
     * both ends), so W|X reads EPERM wherever it arrives. */
    if (linux_mprotect_refuses_wx(req->prot)) return -LINUX_EPERM;
    if (req->prot & LINUX_PROT_EXEC) return -LINUX_ENOSYS;

    if (!(req->flags & LINUX_MAP_ANONYMOUS)) return -LINUX_ENOSYS;
    if (!(req->flags & LINUX_MAP_PRIVATE))   return -LINUX_ENOSYS;
    if (req->flags & LINUX_MAP_SHARED)       return -LINUX_ENOSYS;
    if (req->flags & ~LINUX_MAP_V1_ALLOW)    return -LINUX_ENOSYS;

    /* FIXED asks for an exact address; Linux requires it page-aligned
     * and the shim bounds it by the stamped span — outside the budget is
     * the budget shape §5.4 names, ENOMEM. Non-FIXED addresses are the
     * ledger's pick: no address rule here, the cursors decide at the
     * reply. */
    if (req->flags & LINUX_MAP_FIXED) {
        if (req->addr & (LINUX_PAGE_SIZE - 1)) return -LINUX_EINVAL;
        if (req->addr < span_base || req->addr > span_limit)
            return -LINUX_ENOMEM;
        if (len_r > span_limit - req->addr) return -LINUX_ENOMEM;
    }
    return 0;
}

struct LinuxMemMmapVerdict
linux_mem_mmap_verdict(uint64_t heap_base, uint64_t heap_limit,
                       uint64_t brk_cur, uint64_t mmap_top,
                       const struct LinuxMmapReq *req, uint64_t rax)
{
    struct LinuxMemMmapVerdict v;
    uint64_t len_r;
    int is_fixed;

    v.malformed = 0;
    v.replace = 0;
    v.prot_write = 0;
    v.map_va = 0;
    v.range_pages = 0;
    v.map_pages = 0;
    v.new_mmap_top = mmap_top;

    /* A refusal reply: its errno rides the §5.7 map as usual, nothing
     * maps, nothing is malformed. (MAP_FAILED's -1 arrives here too: it
     * is just a negative errno the sidecar chose.) */
    if ((int64_t)rax < 0) return v;

    /* The refusal ladder re-runs over the staged words: the verdict is
     * reached only through a request the first trap passed, but it is
     * driven standalone by the host test and must be total on any input
     * — a shape the trap would have refused maps nothing here either. */
    if (linux_mem_mmap_refusal(req, heap_base, heap_limit) != 0) {
        v.malformed = 1;
        return v;
    }

    len_r = (req->len + LINUX_PAGE_SIZE - 1)
          & ~((uint64_t)LINUX_PAGE_SIZE - 1);
    is_fixed = (req->flags & LINUX_MAP_FIXED) != 0;

    if (is_fixed) {
        /* The echo rule on an address (increment 5's one-truth rule,
         * over a mapping): a granted FIXED map returns exactly the
         * address asked. The span re-check is the kernel's OWN stamp —
         * the refusal ladder checked it at the first trap and the
         * stamped span cannot have moved, but the kernel never maps
         * because a grant said so, twice. */
        if (rax != req->addr || rax < heap_base || rax > heap_limit ||
            len_r > heap_limit - rax) {
            v.malformed = 1;
            return v;
        }
        v.replace = 1;              /* last map wins, as Linux itself */
    } else {
        /* The ledger's pick, re-checked against the kernel's OWN two
         * cursors: nonzero, page-aligned, inside the span, strictly at
         * or above the break (the page there is unmapped — brk maps
         * below its break), and the whole range at or below the
         * cursor — which then descends past it, so no later grant ever
         * re-hands this range. */
        if (rax == 0 || (rax & (LINUX_PAGE_SIZE - 1)) != 0 ||
            rax < heap_base || rax > heap_limit ||
            len_r > heap_limit - rax ||
            rax < brk_cur || rax > mmap_top ||
            len_r > mmap_top - rax) {
            v.malformed = 1;
            return v;
        }
    }

    v.map_va = rax;
    v.range_pages = len_r / LINUX_PAGE_SIZE;
    /* PROT_NONE backs no frames: a reservation whose every access
     * faults — PROT_NONE's whole observable contract, and the measured
     * allocator's guard-page shape. */
    v.map_pages = (req->prot == LINUX_PROT_NONE)
                ? 0 : v.range_pages;
    v.prot_write = (req->prot & LINUX_PROT_WRITE) ? 1 : 0;
    /* A non-fixed addr is a hint, not the anonymous-map cursor: Linux
     * leaves the cursor alone and keeps serving NULL maps elsewhere. The
     * Go gate depends on that (its arena hint 0xc000000000 is followed by
     * 32 MiB and small NULL maps). Only a NULL-address grant descends the
     * cursor; a FIXED map never does either. */
    v.new_mmap_top = (is_fixed || req->addr != 0) ? mmap_top : rax;
    return v;
}

/* ─── E7 increment 25 — munmap, the memory class's last seam ───────────── */
int linux_mem_munmap_refusal(const struct LinuxMunmapReq *req,
                             uint64_t span_base, uint64_t span_limit,
                             uint64_t brk_cur)
{
    uint64_t len_r;

    if (req->len == 0) return -LINUX_EINVAL;
    if (req->addr & (LINUX_PAGE_SIZE - 1)) return -LINUX_EINVAL;
    if (req->len > UINT64_MAX - (LINUX_PAGE_SIZE - 1))
        return -LINUX_EINVAL;
    len_r = (req->len + LINUX_PAGE_SIZE - 1)
          & ~((uint64_t)LINUX_PAGE_SIZE - 1);

    /* The span is the kernel's OWN budget, not a policy hint: a range
     * outside it would walk pages this task never owned. Starting below
     * the current break is v1's deliberate limit — those pages are the
     * brk seam's (munmap never rewinds either cursor). Linux's own
     * invalid-range answer is EINVAL; this is not a budget-exhaustion
     * ENOMEM because the request is malformed, not merely too large. */
    if (req->addr < span_base || req->addr > span_limit)
        return -LINUX_EINVAL;
    if (len_r > span_limit - req->addr)
        return -LINUX_EINVAL;
    if (req->addr < brk_cur)
        return -LINUX_EINVAL;
    return 0;
}

struct LinuxMemMunmapVerdict
linux_mem_munmap_verdict(uint64_t heap_base, uint64_t heap_limit,
                         uint64_t brk_cur,
                         const struct LinuxMunmapReq *req, uint64_t rax)
{
    struct LinuxMemMunmapVerdict v;
    uint64_t len_r;

    v.malformed = 0;
    v.unmap_va = 0;
    v.unmap_pages = 0;

    /* A negative reply is the sidecar's refusal: nothing unmaps, and its
     * errno rides the §5.7 map as usual. Munmap's success value is 0, so
     * any other non-negative answer is a lie the kernel must not act on
     * (the request's page range is not what the reply claimed). */
    if ((int64_t)rax < 0) return v;
    if (rax != 0) {
        v.malformed = 1;
        return v;
    }
    if (linux_mem_munmap_refusal(req, heap_base, heap_limit,
                                 brk_cur) != 0) {
        v.malformed = 1;
        return v;
    }

    len_r = (req->len + LINUX_PAGE_SIZE - 1)
          & ~((uint64_t)LINUX_PAGE_SIZE - 1);
    v.unmap_va = req->addr;
    v.unmap_pages = len_r / LINUX_PAGE_SIZE;
    return v;
}

uint64_t linux_user_page_frame(uint64_t root, uint64_t va)
{
    uint64_t leaf = 0;
    /* The OWNERSHIP read, not a copy: a soft-protected leaf is still this
     * task's frame and munmap/brk/map unwind must be able to return it.
     * linux_user_copy_out/in call the same walker with accept_soft=0 and
     * keep the U=1 boundary. */
    if (lab_walk_leaf(root, va, &leaf, 0, 1) != 0) return 0;
    return leaf & LAB_PTE_FRAME;
}

/* ─── E7 increment 12 (the reply payload, §10.13's step 3) — the copy-IN ───
 * The header names the split and the security property; these are its
 * three pure halves. Nothing below reads kernel state: the plan is
 * arithmetic over the six captured words, the copy walks a root the
 * CALLER supplies (the task's own PML4 in production, a fake table in
 * the host test), and the shape predicate is arithmetic over (num, ret,
 * n). */

/* The copy-IN's walk — write side. Phase one of copy_in's two phases:
 * validate EVERY page of the span before a single byte moves. */
static int lab_walk_leaf_rw(uint64_t root, uint64_t va, uint64_t *leaf_out)
{
    return lab_walk_leaf(root, va, leaf_out, 1, 0);
}

int linux_user_copy_in(const uint8_t *src, uint64_t root, uint64_t va,
                       uint32_t n)
{
    uint64_t v;
    uint32_t left;

    if (!src) return -LINUX_EFAULT;

    /* Phase 1 — validate the WHOLE span (present, USER, writable, no
     * huge page at any level). A refusal here mutates nothing: the
     * two-phase shape is what makes a read-only page anywhere in the
     * span leave every byte untouched, which the host test pins as
     * bytes-unchanged (refusals-before-mutation, §10.13's M4 family
     * extended to the IN direction). n == 0 returns before the walk. */
    v = va;
    left = n;
    while (left) {
        uint32_t off = (uint32_t)(v & (LINUX_PAGE_SIZE - 1u));
        uint32_t page_left = LINUX_PAGE_SIZE - off;
        uint32_t take = left < page_left ? left : page_left;
        if (lab_walk_leaf_rw(root, v, 0) != 0) return -LINUX_EFAULT;
        v += take;
        left -= take;
    }

    /* Phase 2 — write. Every page was validated above, and the task is
     * parked between its forward and its reply (no unmap can land
     * between the phases on this path), so a walk below cannot fail;
     * if one somehow did (a table changed under us), stop with -EFAULT
     * — the partial write would be a kernel-mutation bug, never a
     * tenant-controllable one, since phase 1 gates every page first. */
    v = va;
    left = n;
    while (left) {
        uint64_t leaf = 0;
        uint32_t off = (uint32_t)(v & (LINUX_PAGE_SIZE - 1u));
        uint32_t page_left = LINUX_PAGE_SIZE - off;
        uint32_t take = left < page_left ? left : page_left;
        uint32_t i;
        uint8_t *dst;
        if (lab_walk_leaf_rw(root, v, &leaf) != 0) return -LINUX_EFAULT;
        dst = (uint8_t *)(uintptr_t)((leaf & LAB_PTE_FRAME) + off);
        for (i = 0; i < take; i++) dst[i] = src[i];
        src += take;
        v += take;
        left -= take;
        /* root is NOT reassigned: the next iteration re-walks all four
         * levels from the PML4 (copy_out's own rule — a leaf frame is
         * not a table base). */
    }
    return 0;
}

struct LinuxFdReplyPlan linux_fd_reply_plan(uint64_t num, const uint64_t args[6])
{
    struct LinuxFdReplyPlan p;
    uint64_t span_bytes;

    p.dst = 0;
    p.len = 0;
    p.aux = 0;
    p.kind = LINUX_STAGE_NONE;
    p.refuse = 0;
    if (!args) return p;

    /* The sig class — increment 12's copy-IN, unchanged in shape, now
     * named as the RAW family (the payload lands verbatim). sigaltstack
     * is the class's third number: its query returns one 24-byte
     * struct stack_t at args[1], while an install (args[0] non-NULL)
     * needs no input copy because this v1 never delivers a signal on
     * an alternative stack. */
    if (num == LINUX_RT_SIGACTION || num == LINUX_RT_SIGPROCMASK ||
        num == LINUX_SIGALTSTACK) {
        uint64_t out = (num == LINUX_SIGALTSTACK) ? 1u : 2u;
        if (args[out] == 0) return p;   /* no out-pointer: nothing owed */
        p.dst  = args[out];
        p.len  = (num == LINUX_RT_SIGACTION) ? LINUX_SIGACT_BYTES
               : (num == LINUX_SIGALTSTACK) ? LINUX_STACK_T_BYTES
                                            : LINUX_SIGSET_BYTES;
        p.aux  = p.len;
        p.kind = LINUX_STAGE_RAW;
    } else if (num == LINUX_STAT || num == LINUX_LSTAT) {
        /* stat/lstat: the out-pointer is args[1] (statbuf), the payload
         * the COMPACT Stat (24 bytes — the sidecar never sees musl's
         * layout) and the child-side write the serialized 144 bytes.
         * A NULL statbuf is not refused here: the copy-in's page walk
         * fails it as -EFAULT at the deliver, the same posture write
         * and read take over a NULL buffer. */
        p.dst  = args[1];
        p.len  = LINUX_STAT_PAYLOAD_BYTES;
        p.aux  = LINUX_STAT_BYTES;
        p.kind = LINUX_STAGE_STAT;
    } else if (num == LINUX_GETDENTS64) {
        /* getdents64(fd, buf, count): a count too small for even the
         * "." record is EINVAL at the first trap (Linux's own rule —
         * the caller's buffer can never hold one record, so the call
         * cannot succeed); the child-side write is the count itself,
         * capped so one oversized ask cannot make the deliver's local
         * (or the caller's patience) unbounded. */
        if (args[2] < LINUX_DIRENT_MIN_RECLEN)
            p.refuse = -LINUX_EINVAL;
        p.dst  = args[1];
        p.len  = LINUX_DENTS_PAYLOAD_CAP;
        p.aux  = args[2] < LINUX_DIRENT_USER_CAP
                     ? (uint32_t)args[2] : LINUX_DIRENT_USER_CAP;
        p.kind = LINUX_STAGE_DENTS;
    } else if (num == LINUX_CLOCK_GETTIME) {
        /* clock_gettime(clockid, tp) — one struct timespec back. The
         * out-pointer is args[1] and the shape is fixed (16 bytes: tv_sec
         * i64, tv_nsec i64). NULL is -EFAULT at the first trap, before
         * any post; a non-canonical span falls to the shared check
         * below. The VALUE is the sidecar's (a pinned epoch); this plan
         * only says where the bytes land. */
        p.dst  = args[1];
        p.len  = LINUX_TIMESPEC_BYTES;
        p.aux  = LINUX_TIMESPEC_BYTES;
        p.kind = LINUX_STAGE_RAW;
        if (args[1] == 0) p.refuse = -LINUX_EFAULT;
    } else if (num == LINUX_POLL) {
        /* poll(2): the reply is the same array with revents rewritten. */
        if (args[1] > LINUX_POLL_MAX_FDS) {
            p.refuse = -LINUX_EINVAL;
            return p;
        }
        if (args[1] == 0) return p;
        if (args[0] == 0) {
            p.refuse = -LINUX_EFAULT;
            return p;
        }
        p.dst  = args[0];
        p.len  = (uint32_t)(args[1] * LINUX_POLLFD_BYTES);
        p.aux  = p.len;
        p.kind = LINUX_STAGE_RAW;
    } else if (num == LINUX_READ) {
        /* read(fd, buf, count) — count-truth's own rule, mirrored from
         * write: a count-0 read plans NOTHING before canonicality,
         * because inventing -EFAULT there would be a rule no kernel the
         * candidate was compiled against has (read(fd, NULL, 0) is a
         * legal no-op on Linux). The clamp rides IN the description as
         * write's does: header, payload and the count the task sees are
         * one truth. */
        if (args[2] == 0) return p;
        p.dst  = args[1];
        p.len  = (args[2] < LINUX_FD_WRITE_CHUNK) ? (uint32_t)args[2]
                                                   : LINUX_FD_WRITE_CHUNK;
        p.aux  = p.len;
        p.kind = LINUX_STAGE_RAW;
    } else if (num == LINUX_TKILL) {
        /* E7 increment 28: tkill's answer can BE a delivery — the sidecar's
         * 80-byte act request for a handler disposition. The plan stages a
         * bound with NO user address (dst 0): the bytes are not owed to a
         * pointer in the task, and the deliver's SIGFRAME arm must not copy
         * them anywhere. len is the exact size, so the kernel's clamp and
         * the sidecar's bound are the same number (the twin rule the other
         * numbers already obey). */
        p.dst  = 0;
        p.len  = LINUX_SIGDELIVER_BYTES;
        p.aux  = LINUX_SIGDELIVER_BYTES;
        p.kind = LINUX_STAGE_SIGFRAME;
    } else {
        return p;   /* not a reply-payload number (total function) */
    }

    /* The write plan's shape, over the IN direction: the WHOLE span the
     * deliver may write must sit in the user-canonical range or the
     * first trap says -EFAULT — before any post, so a bad destination is
     * a refusal the channel never sees (and the staged VA below can
     * never be one). Checked against AUX — the child-side bytes — which
     * for stat (144) and getdents (up to 2048) is larger than the
     * payload clamp. */
    span_bytes = p.aux;
    {
        uint64_t span_end = p.dst + span_bytes;
        if (!linux_fs_addr_is_user_canonical(p.dst) || span_end < p.dst ||
            (span_bytes && (span_end - 1) > LINUX_USER_ADDR_MAX)) {
            p.refuse = -LINUX_EFAULT;
        }
    }
    return p;
}

/* The path plan (step 4): open/stat/lstat/unlink/mkdir carry ONE
 * pathname out of the child, rename TWO. The plan stays a pure
 * CLASSIFIER over (num, args) — which register names a path, how many
 * of them — because measuring a string means touching the child's
 * bytes, and that needs the page walker and a cr3. The bytes are
 * measured below, at the attach, by linux_path_measure (first-trap
 * still: everything happens before the post). */
struct LinuxFdPathPlan linux_fd_path_plan(uint64_t num, const uint64_t args[6])
{
    struct LinuxFdPathPlan p;

    p.src = 0;
    p.src2 = 0;
    p.two = 0;
    p.refuse = 0;
    if (num != LINUX_RENAME && num != LINUX_OPEN && num != LINUX_OPENAT &&
        num != LINUX_STAT && num != LINUX_LSTAT && num != LINUX_UNLINK &&
        num != LINUX_MKDIR) {
        return p;   /* this number carries no path (total function) */
    }
    if (!args) return p;
    /* openat's args[0] is the dirfd; the pathname is args[1]. Every other
     * path number names its path in args[0]. */
    p.src  = (num == LINUX_OPENAT) ? args[1] : args[0];
    p.two  = (num == LINUX_RENAME);
    p.src2 = p.two ? args[1] : 0;
    return p;
}

/* Measure one path string in the child's memory: walk for the NUL
 * one byte at a time through the SAME copy-out walker every attach uses
 * (present, user, 4 KiB rules — it never faults over a tenant pointer),
 * and hand back the string's length INCLUDING the NUL. No buffer: the
 * byte is a local, so the frame cost is a word regardless of cap. A
 * span that cannot be read is -LINUX_EFAULT; no NUL within `cap` is
 * -LINUX_ENAMETOOLONG — the shim's named divergence from PATH_MAX,
 * refused by name rather than truncated, both before any post. */
int linux_path_measure(uint64_t root, uint64_t va, uint32_t cap,
                       uint32_t *out_len)
{
    uint32_t got = 0;
    while (got < cap) {
        uint8_t b = 0;
        if (linux_user_copy_out(&b, root, va + got, 1) != 0)
            return -LINUX_EFAULT;
        got++;
        if (b == 0) {
            if (out_len) *out_len = got;
            return 0;
        }
    }
    return -LINUX_ENAMETOOLONG;
}

/* LE readers over a channel payload (the deliver's idiom, repeated: the
 * sidecar's compact Stat is a byte stream, never aliased as a struct). */
static uint16_t lab_rd_u16(const uint8_t *b)
{
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint64_t lab_rd_u64(const uint8_t *b)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

static uint32_t lab_rd_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void lab_wr_u64(uint8_t *b, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) { b[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

void linux_stat_bytes(const uint8_t *src, uint8_t *dst)
{
    uint64_t size;
    uint32_t mtime;
    int i;

    for (i = 0; i < (int)LINUX_STAT_BYTES; i++) dst[i] = 0;
    size  = lab_rd_u64(src + 8);
    mtime = (uint32_t)lab_rd_u64(src + 16);   /* u32 at payload @16 */
    /* Fields the VFS carries: the truth, at musl's offsets. */
    lab_wr_u64(dst + 8, 0);                   /* st_ino: not carried → 0 */
    lab_wr_u64(dst + 16, 1);                  /* st_nlink: 1 (v1)        */
    {
        uint32_t mode = lab_rd_u16(src + 0);
        uint32_t uid  = lab_rd_u16(src + 2);
        uint32_t gid  = lab_rd_u16(src + 4);
        for (i = 0; i < 4; i++) {
            dst[LINUX_STAT_OFF_MODE + i] = (uint8_t)(mode >> (8 * i));
            dst[LINUX_STAT_OFF_UID  + i] = (uint8_t)(uid  >> (8 * i));
            dst[LINUX_STAT_OFF_GID  + i] = (uint8_t)(gid  >> (8 * i));
        }
    }
    lab_wr_u64(dst + LINUX_STAT_OFF_SIZE, size);
    lab_wr_u64(dst + 56, 4096);               /* st_blksize              */
    lab_wr_u64(dst + 64, (size + 511) / 512); /* st_blocks derived       */
    lab_wr_u64(dst + 72, mtime);              /* st_atim: echoes mtime   */
    lab_wr_u64(dst + LINUX_STAT_OFF_MTIME, mtime);
    lab_wr_u64(dst + 104, mtime);             /* st_ctim: echoes mtime   */
}

int linux_dirent_pack(const uint8_t *src, uint32_t n, uint32_t count_cap,
                      uint64_t dir_pos, uint8_t *dst, uint32_t *fitted)
{
    uint32_t count;
    uint32_t off = 4;
    uint32_t packed = 0;
    uint32_t i;

    if (fitted) *fitted = 0;
    if (n < 4) return -1;
    count = lab_rd_u32(src);                  /* u32 LE at @0 */
    for (i = 0; i < count; i++) {
        uint64_t ino;
        uint8_t ty;
        uint16_t name_len;
        uint16_t reclen;
        uint8_t *rec;
        uint32_t j;

        if (off + 11 > n) return -1;
        ino      = lab_rd_u64(src + off);
        ty       = src[off + 8];
        name_len = lab_rd_u16(src + off + 9);
        off += 11;
        if (off + name_len > n) return -1;
        if (name_len == 0 || name_len > 255) return -1;
        if (i < dir_pos) {                    /* already given out       */
            off += name_len;
            continue;
        }
        reclen = (uint16_t)((19u + name_len + 1u + 7u) & ~7u);
        if (packed + reclen > count_cap) break;  /* caller loops         */
        rec = dst + packed;
        lab_wr_u64(rec, ino);                 /* d_ino                   */
        lab_wr_u64(rec + 8, i + 1);           /* d_off: the entry's own
                                               * cookie (index + 1)      */
        for (j = 0; j < 2; j++)
            rec[16 + j] = (uint8_t)(reclen >> (8 * j));
        rec[18] = ty;                         /* d_type: DT_* pass-thru  */
        for (j = 0; j < name_len; j++) rec[19 + j] = src[off + j];
        rec[19 + name_len] = 0;
        for (j = 19u + name_len + 1u; j < reclen; j++) rec[j] = 0;
        packed += reclen;
        off += name_len;
        if (fitted) *fitted += 1;
    }
    return (int)packed;
}

/* Does a ZERO-byte success answer this number honestly? The shape-1 rule
 * in chan.c refuses a success on a request that staged an owed payload —
 * "answering 0 while the caller's out-pointer keeps stack garbage is the
 * silent lie the seam exists to prevent". But for a COUNT-TRUTH number the
 * count is the answer: read at end-of-file returns 0, and getdents64 at
 * end-of-directory packs 0 bytes. Refusing those would make EOF an error
 * (caught live: the gate's backup copy died on its last read with EPROTO).
 * Every other staged number owes a FIXED struct, so zero bytes there is
 * still the lie the rule names. Total; the twin of payload_bound's
 * per-number arm on the sidecar side. */
int linux_reply_zero_ok(uint64_t num)
{
    if (num == LINUX_READ || num == LINUX_GETDENTS64) return 1;
    /* sigaltstack(&new, NULL) is a legal install with no old-stack
     * out-pointer: the reply owes no struct and is STATUS_OK with zero
     * payload. When args[1] is non-NULL the sidecar returns the 24-byte
     * stack_t and this arm is not reachable for it (shape 2). */
    if (num == LINUX_SIGALTSTACK) return 1;
    return 0;
}

int linux_reply_shape_ok(uint64_t num, uint64_t ret, uint32_t n)
{
    if (num == LINUX_READ) return ret == (uint64_t)n;
    /* poll: n is the full pollfd array, ret is the ready count. */
    if (num == LINUX_POLL)
        return n % LINUX_POLLFD_BYTES == 0 && ret <= (uint64_t)(n / LINUX_POLLFD_BYTES);
    /* stat/lstat: success with EXACTLY the compact Stat — a payload the
     * serializer cannot parse is a protocol break, refused before any
     * byte is written (the twin of service()'s own bound). */
    if (num == LINUX_STAT || num == LINUX_LSTAT)
        return ret == 0 && n == LINUX_STAT_PAYLOAD_BYTES;
    /* getdents64: success; the byte count the child sees is the
     * KERNEL's packing result, never the sidecar's number — so ret is 0
     * here and the deliver overwrites rax with what was packed. */
    if (num == LINUX_GETDENTS64)
        return ret == 0 && n <= LINUX_DENTS_PAYLOAD_CAP;
    /* clock_gettime: success and exactly one timespec — a success with no
     * payload (or the wrong size) is refused before any byte is written. */
    if (num == LINUX_CLOCK_GETTIME)
        return ret == 0 && n == LINUX_TIMESPEC_BYTES;
    /* sigaltstack: success and exactly one stack_t back when the caller
     * asked for the old one. */
    if (num == LINUX_SIGALTSTACK)
        return ret == 0 && n == LINUX_STACK_T_BYTES;
    /* E7 increment 28: tkill's success that RUNS a handler carries exactly
     * the delivery request. A shorter payload is a request with a word the
     * sidecar never sent; a longer one is a message from a different
     * conversation — both refused before anything is built. */
    if (num == LINUX_TKILL)
        return ret == 0 && n == LINUX_SIGDELIVER_BYTES;
    return ret == 0;
}

/* ─── The personality (E7 increment 2) ───────────────────────────────────── */
int linux_personality_intercepts(uint32_t personality)
{
    return personality == PROC_PERSONALITY_LINUX;
}

void linux_syscall_describe(struct SLSLinuxSyscallRequest *req,
                            uint64_t num, const uint64_t args[6])
{
    size_t i;
    if (!req) return;
    req->num = num;
    for (i = 0; i < 6; i++)
        req->args[i] = args ? args[i] : 0;
    req->ret    = 0;   /* no answer exists until the sidecar gives one */
    req->status = 0;
}

/* ─── The park resume (E7 increment 3, §3.3 steps 5-7) ────────────────────────
 * The increment that stops describing and starts delivering. Three pure
 * functions, because every decision the forward path makes before it parks
 * ("is this resume ours?", "is this table shape ours?", "is this payload an
 * answer?") is a decision a source guard can only hold if the SOURCE holds
 * it as a named contract, and a host test can only bite if it is pure.
 *
 * in-flight: a forward is armed from BEFORE the post until the reply is
 * applied, and park_syscall is the arm (process.c reads the same value to
 * suppress the post's sysret handoff). A stray trap at the sentinel while
 * disarmed is refused — never parked — because no reply is owed; the
 * refusal is by name, the same honesty as the increment-2 branch refused
 * every number it could not interpret. Personality is an argument, not a
 * lookup: the truth table "unrecognised personality asserts nothing" is
 * host-assertable only if the value comes from outside. */
int linux_forward_resume_is_live(uint32_t personality, uint32_t park_syscall)
{
    return linux_personality_intercepts(personality) &&
           park_syscall == SYS_LINUX_TRAP_RESUME;
}

/* Channel shape (§3.5): exactly one sendable CHAN_W and exactly one
 * recv-able CHAN_R. Both — not "at least one": a second candidate means
 * the table does not carry the single channel §3.5 names, and guessing
 * which of two channels is the sidecar's would be a routing decision the
 * kernel is not allowed to make (it describes; it does not interpret). */
int linux_channel_shape_ok(int n_chan_w, int n_chan_r)
{
    return n_chan_w == 1 && n_chan_r == 1;
}

/* Apply the sidecar's reply (§3.3 step 7). The reply is the same struct
 * back — num echoed, ret/status filled — which makes "is this the answer
 * to THIS request" a single field compare rather than a protocol: the
 * channel is point-to-point and one-outstanding-per-task by construction
 * (the task is parked between post and reply), so echoed num mismatch can
 * only mean a misrouted or malformed reply, and both refuse with -EPROTO:
 * a reply that is not provably this request's answer must not put anything
 * into the task's rax (§5.7's rule restated one level up — not just "no
 * leak of CAP codes", but "no answer that was not an answer"). */
uint64_t linux_syscall_apply_reply(struct SLSLinuxSyscallRequest *req,
                                   const void *payload, uint64_t payload_len)
{
    const uint8_t *src = (const uint8_t *)payload;
    uint8_t *dst;
    uint64_t expect;
    uint64_t i;

    if (!req || !payload) return (uint64_t)(int64_t)(-LINUX_EPROTO);
    if (payload_len != sizeof *req)
        return (uint64_t)(int64_t)(-LINUX_EPROTO);   /* not a reply at all */

    expect = req->num;   /* what the FIRST trap described — the echo is
                          * checked against this, before trusting anything */
    dst = (uint8_t *)req;
    for (i = 0; i < sizeof *req; i++)
        dst[i] = src[i];   /* byte copy: channel payload, any alignment */

    if (req->num != expect)
        return (uint64_t)(int64_t)(-LINUX_EPROTO);   /* a different syscall's
                                                     * answer, or garbage */

    /* status 0 = the reply carries a real return value; nonzero = a CAP
     * code the §5.7 map turns into Linux's own numbering here, at the last
     * possible moment — a CAP-family negative never crosses into the task. */
    if (req->status == 0)
        return req->ret;
    return (uint64_t)(int64_t)linux_errno_from_cap(req->status);
}

/* ─── E7 increment 27 (P1-C, "the delivery") ──────────────────────────────
 * The frame builder's caller. Three pieces, each refusing before it mutates:
 *
 *   the REQUEST  — the sidecar's act words, decoded from a fixed 80-byte
 *                  reply payload. A length that is not exactly 80 is not a
 *                  truncated answer to be repaired; it is not an answer.
 *   the ADDRESS  — si_addr, from the numbers the trap path actually has.
 *                  CR2 for a fault that has an address, RIP for the two
 *                  that fault on an instruction, nothing for the rest.
 *   the WRITE    — build, then write the 440 bytes through
 *                  linux_user_copy_in against the task's own cr3. Two
 *                  phases, so a stack the task made read-only is -EFAULT
 *                  with the guest's memory untouched: a signal is the last
 *                  thing that may fault in ring 0.
 *
 * Registers are NOT applied here. The caller (the return-to-user path) owns
 * that, which is what keeps this half host-testable against fake tables. */
static uint64_t sig_get64(const uint8_t *p);
static uint32_t sig_get32(const uint8_t *p);

int linux_sig_deliver_req_parse(const uint8_t *bytes, uint32_t len,
                                struct LinuxSigDeliverReq *out)
{
    if (!bytes || !out) return -LINUX_EINVAL;
    if (len != LINUX_SIGDELIVER_BYTES) return -LINUX_EINVAL;

    out->signo     = sig_get32(bytes + 0);
    out->code      = sig_get32(bytes + 4);
    out->addr      = sig_get64(bytes + 8);
    out->handler   = sig_get64(bytes + 16);
    out->restorer  = sig_get64(bytes + 24);
    out->act_flags = sig_get64(bytes + 32);
    out->act_mask  = sig_get64(bytes + 40);
    out->blocked   = sig_get64(bytes + 48);
    out->alt_sp    = sig_get64(bytes + 56);
    out->alt_size  = sig_get64(bytes + 64);
    out->alt_flags = sig_get32(bytes + 72);
    out->siginfo   = sig_get32(bytes + 76);

    /* The frame builder repeats these two refusals; doing them here as well
     * means a decodable-but-unrunnable answer never reaches the write path
     * at all. */
    if (out->signo == 0u || out->signo > 64u) return -LINUX_EINVAL;
    if (out->handler <= 1u) return -LINUX_EINVAL;   /* SIG_DFL / SIG_IGN */
    if (out->restorer == 0u) return -LINUX_EINVAL;  /* a handler that cannot
                                                    * return is not a delivery */
    return 0;
}

int linux_sig_si_addr(uint32_t signo, uint64_t cr2, uint64_t rip, uint64_t *out)
{
    if (!out) return 0;
    switch (signo) {
    case 11u:                       /* SIGSEGV */
    case 7u:                        /* SIGBUS  */
        *out = cr2;                 /* cr2 == 0 is a real address here: the
                                     * NULL a segfault was taken on */
        return 1;
    case 8u:                        /* SIGFPE  */
    case 4u:                        /* SIGILL  */
        *out = rip;                 /* the faulting INSTRUCTION — CR2 is
                                     * stale or never loaded on this path */
        return 1;
    default:
        return 0;                   /* no address belongs to it; the caller
                                     * keeps the sidecar's own addr */
    }
}

/* ─── E7 increment 28: the park-context bridge ───────────────────────────
 * The offsets are CapParkCtx's, named by cap_recv_resume's asm (see the
 * header). Read and install are exact mirrors, so a round trip through
 * them is the identity on every word the return path reads. */
int linux_sig_park_read(const uint64_t *w, uint32_t words,
                        struct LinuxSigRegs *ctx)
{
    if (!w || !ctx) return -LINUX_EINVAL;
    if (words < LINUX_PARK_WORDS) return -LINUX_EINVAL;

    {
        uint32_t i;
        for (i = 0; i < (uint32_t)LINUX_SIGREG_COUNT; i++) ctx->r[i] = 0;
    }
    ctx->eflags = w[0];                        /* r11 */
    ctx->r[LINUX_SIGREG_RIP] = w[1];            /* rcx */
    ctx->r[LINUX_SIGREG_R15] = w[2];
    ctx->r[LINUX_SIGREG_R14] = w[3];
    ctx->r[LINUX_SIGREG_R13] = w[4];
    ctx->r[LINUX_SIGREG_R12] = w[5];
    ctx->r[LINUX_SIGREG_RBX] = w[6];
    ctx->r[LINUX_SIGREG_RBP] = w[7];
    ctx->r[LINUX_SIGREG_RSP] = w[8];            /* user_rsp */
    ctx->r[LINUX_SIGREG_RDI] = w[9];
    ctx->r[LINUX_SIGREG_RSI] = w[10];
    ctx->r[LINUX_SIGREG_RDX] = w[11];
    ctx->r[LINUX_SIGREG_R10] = w[12];
    ctx->r[LINUX_SIGREG_R8]  = w[13];
    ctx->r[LINUX_SIGREG_R9]  = w[14];
    return 0;
}

int linux_sig_park_install(uint64_t *w, uint32_t words,
                           const struct LinuxSigRegs *entry)
{
    if (!w || !entry) return -LINUX_EINVAL;
    if (words < LINUX_PARK_WORDS) return -LINUX_EINVAL;

    w[0]  = entry->eflags;
    w[1]  = entry->r[LINUX_SIGREG_RIP];
    w[2]  = entry->r[LINUX_SIGREG_R15];
    w[3]  = entry->r[LINUX_SIGREG_R14];
    w[4]  = entry->r[LINUX_SIGREG_R13];
    w[5]  = entry->r[LINUX_SIGREG_R12];
    w[6]  = entry->r[LINUX_SIGREG_RBX];
    w[7]  = entry->r[LINUX_SIGREG_RBP];
    w[8]  = entry->r[LINUX_SIGREG_RSP];
    w[9]  = entry->r[LINUX_SIGREG_RDI];
    w[10] = entry->r[LINUX_SIGREG_RSI];
    w[11] = entry->r[LINUX_SIGREG_RDX];
    w[12] = entry->r[LINUX_SIGREG_R10];
    w[13] = entry->r[LINUX_SIGREG_R8];
    w[14] = entry->r[LINUX_SIGREG_R9];
    return 0;
}

int linux_sig_deliver(uint64_t root, const struct LinuxSigDeliverReq *req,
                      const struct LinuxSigRegs *ctx, uint64_t cr2,
                      struct LinuxSigRegs *entry, uint64_t *entry_mask)
{
    uint8_t frame[LINUX_SIGFRAME_SIZE];
    struct LinuxSigFrameSpec spec;
    struct LinuxSigRegs entry_local;
    uint64_t mask_local = 0;
    uint64_t addr = 0;
    int r;

    if (!req || !ctx || !entry) return -LINUX_EINVAL;

    spec.signo     = req->signo;
    spec.code      = req->code;
    spec.addr      = linux_sig_si_addr(req->signo, cr2,
                                       ctx->r[LINUX_SIGREG_RIP], &addr)
                     ? addr : req->addr;
    spec.handler   = req->handler;
    spec.restorer  = req->restorer;
    spec.act_flags = req->act_flags;
    spec.act_mask  = req->act_mask;
    spec.blocked   = req->blocked;
    spec.user_rsp  = ctx->r[LINUX_SIGREG_RSP];
    spec.alt_sp    = req->alt_sp;
    spec.alt_size  = req->alt_size;
    spec.alt_flags = req->alt_flags;
    spec.siginfo   = req->siginfo;
    spec.regs      = *ctx;

    r = linux_sig_frame_build(frame, LINUX_SIGFRAME_SIZE, &spec,
                              &entry_local, &mask_local);
    if (r != 0) return r;           /* nothing written: the builder refuses
                                     * before its first store */

    r = linux_user_copy_in(frame, root, entry_local.r[LINUX_SIGREG_RSP],
                           LINUX_SIGFRAME_SIZE);
    if (r != 0) return r;           /* -EFAULT with the guest untouched:
                                     * phase 1 walks every page first */

    *entry = entry_local;
    if (entry_mask) *entry_mask = mask_local;
    return 0;
}

/* ─── The initial stack and auxv (§5.2) ────────────────────────────────────
 * Byte-exact layout, low address (returned rsp) upward — the canonical
 * process-entry image a static libc walks:
 *     [ argc ]                     <- returned rsp (16-byte aligned, psABI)
 *     [ argv pointers, NULL ]
 *     [ envp pointers, NULL ]
 *     [ auxv: 13 entries + AT_NULL, 16 bytes each ]
 *     [ pad to 8, plus a further 8 iff needed to keep rsp 16-aligned ]
 *     [ argv strings | env strings | execfn | "x86_64" | AT_RANDOM(16) ]
 * The strings ride at the TOP of the used span (the stack grows down
 * toward them); a libc reaches auxv by walking up from envp's NULL, so
 * the auxv must be the last thing below the strings — an inverted image
 * walks into whatever the caller left there.
 * The strings are copied (the source may be the sidecar's memory, which
 * will not exist in the restored world at the same address); AT_RANDOM
 * is copied, and its absence is refused outright rather than filled with
 * zeros — §5.2's rule that a stack without entropy makes canaries
 * worthless. */

static size_t lab_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void lab_memcpy(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

static void lab_memzero(uint8_t *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) dst[i] = 0;
}

/* auxv entry count: §5.2's required set, AT_SYSINFO_EHDR deliberately
 * absent, plus the AT_NULL terminator. */
#define LAB_AUX_ENTRIES 13
#define LAB_AUX_BYTES   ((LAB_AUX_ENTRIES + 1) * 16u)

uint64_t linux_initial_stack_build(uint8_t *region, size_t region_bytes,
                                   const struct LinuxInitStackParams *p)
{
    size_t n_argv = 0, n_envp = 0, i;
    size_t str_bytes, str_off, ptr_pad, aux_off, envp_off, argv_off, argc_off;
    uint8_t *w;
    const char *platform = "x86_64";

    if (!region || !p || !p->argv || !p->envp || !p->at_random || !p->execfn)
        return 0;

    while (p->argv[n_argv]) n_argv++;
    while (p->envp[n_envp]) n_envp++;

    /* ── sizes: refuse up front if the whole image cannot fit ── */
    str_bytes = 0;
    for (i = 0; i < n_argv; i++) str_bytes += lab_strlen(p->argv[i]) + 1;
    for (i = 0; i < n_envp; i++) str_bytes += lab_strlen(p->envp[i]) + 1;
    str_bytes += lab_strlen(p->execfn) + 1;
    str_bytes += lab_strlen(platform) + 1;
    str_bytes += LINUX_AT_RANDOM_BYTES;

    /* +16 covers the alignment pad the loop below may subtract; without it
     * an image that fits only to within a pad byte would wrap the offset. */
    if (str_bytes + LAB_AUX_BYTES + (n_envp + 1) * 8u
                             + (n_argv + 1) * 8u + 8u + 16u > region_bytes)
        return 0;   /* would not fit: refused, caller sizes the allocation */

    /* ── anchors, top-down: strings pinned at the region's top ── */
    str_off = region_bytes - str_bytes;
    /* pad sits between auxv and the strings; first make auxv 8-aligned ── */
    ptr_pad = (str_off - LAB_AUX_BYTES) % 8u;
    for (;;) {
        size_t a_off = str_off - LAB_AUX_BYTES - ptr_pad;
        size_t e_off = a_off - (n_envp + 1) * 8u;
        size_t v_off = e_off - (n_argv + 1) * 8u;
        size_t c_off = v_off - 8u;
        /* psABI: %rsp is 16-byte aligned at process entry — checked on the
         * ABSOLUTE address, since the region's own alignment is the
         * caller's to choose and an offset-only check would assume it. */
        if (((p->region_vaddr + (uint64_t)c_off) % 16u) == 0) break;
        ptr_pad += 8u;   /* one more 8-byte pad flips rsp's parity */
    }

    aux_off  = str_off - LAB_AUX_BYTES - ptr_pad;
    envp_off = aux_off - (n_envp + 1) * 8u;
    argv_off = envp_off - (n_argv + 1) * 8u;
    argc_off = argv_off - 8u;
    str_off  = region_bytes - str_bytes;

    lab_memzero(region + argc_off, region_bytes - argc_off);

    /* ── strings + AT_RANDOM ──────────────────────────────────────────────
     * Strings land at the TOP of the used span; the pointer arrays live at
     * known offsets below them, so each string is copied while its pointer
     * is written straight into its slot — no intermediate arrays, no 1 KiB
     * of kernel stack, and the caller's const argv/envp are never mutated. */
    w = region + str_off;
    {
        uint64_t execfn_va, platform_va, random_va;
        uint64_t *argv_out = (uint64_t *)(void *)(region + argv_off);
        uint64_t *envp_out = (uint64_t *)(void *)(region + envp_off);
        const char *s;

        for (i = 0; i < n_argv; i++) {
            argv_out[i] = p->region_vaddr + (uint64_t)(w - region);
            s = p->argv[i];
            while (*s) *w++ = (uint8_t)*s++;
            *w++ = 0;
        }
        argv_out[n_argv] = 0;
        for (i = 0; i < n_envp; i++) {
            envp_out[i] = p->region_vaddr + (uint64_t)(w - region);
            s = p->envp[i];
            while (*s) *w++ = (uint8_t)*s++;
            *w++ = 0;
        }
        envp_out[n_envp] = 0;
        execfn_va = p->region_vaddr + (uint64_t)(w - region);
        s = p->execfn;
        while (*s) *w++ = (uint8_t)*s++;
        *w++ = 0;
        platform_va = p->region_vaddr + (uint64_t)(w - region);
        s = platform;
        while (*s) *w++ = (uint8_t)*s++;
        *w++ = 0;
        random_va = p->region_vaddr + (uint64_t)(w - region);
        lab_memcpy(w, p->at_random, LINUX_AT_RANDOM_BYTES);
        w += LINUX_AT_RANDOM_BYTES;

        /* ── auxv, ascending type order, AT_NULL terminated ── */
        {
            uint64_t *aux = (uint64_t *)(void *)(region + aux_off);
            size_t a = 0;
#define AUX(t, v) do { aux[a++] = (uint64_t)(t); aux[a++] = (uint64_t)(v); } while (0)
            AUX(LINUX_AT_PHDR,     p->phdr_vaddr);
            AUX(LINUX_AT_PHENT,    p->phent);
            AUX(LINUX_AT_PHNUM,    p->phnum);
            AUX(LINUX_AT_PAGESZ,   LINUX_PAGE_SIZE);
            AUX(LINUX_AT_BASE,     0);            /* static image: no interp base */
            AUX(LINUX_AT_ENTRY,    p->entry_vaddr);
            AUX(LINUX_AT_PLATFORM, platform_va);
            AUX(LINUX_AT_HWCAP,    0);
            AUX(LINUX_AT_CLKTCK,   LINUX_CLKTCK);
            AUX(LINUX_AT_SECURE,   0);
            AUX(LINUX_AT_RANDOM,   random_va);
            AUX(LINUX_AT_HWCAP2,   0);
            AUX(LINUX_AT_EXECFN,   execfn_va);
            /* LINUX_AT_SYSINFO_EHDR is absent: there is no vDSO (§5.2/§5.5). */
            AUX(LINUX_AT_NULL,     0);
#undef AUX
        }

        /* ── argc ── (argv/envp were written as the strings were copied) */
        *(uint64_t *)(void *)(region + argc_off) = (uint64_t)n_argv;
    }

    return p->region_vaddr + (uint64_t)argc_off;
}

/* ─── Q2: the stack high-water scan ───────────────────────────────────────
 * The gate profile poisons the span below the entry %rsp at create and asks
 * this how much of it the workload never touched. The stack grows DOWN from
 * %rsp, so the touched bytes form a suffix of the span; the untouched prefix
 * is the headroom. A NULL region or zero span reports 0 ("nothing measured"),
 * which the caller must not read as "no headroom" — it checks the span first.
 */
size_t linux_stack_hwm_headroom(const uint8_t *region, size_t bytes,
                                uint8_t poison)
{
    size_t i = 0;
    if (!region) return 0;
    while (i < bytes && region[i] == poison) i++;
    return i;
}

/* ═══ E7 increment 26 (P1-C, "the frame") — the signal frame as bytes ════
 * The header carries the why and the layout table. This is the mechanism's
 * pure half: a byte writer, a byte reader and three predicates. No user
 * memory is touched (the caller hands us a staging buffer), no allocation,
 * no state — so the host test drives every branch and the guard's clause P
 * pins the numbers.
 *
 * The layout is pinned THREE ways, deliberately redundant: the macros in
 * the header (what the host test reads), a local mirror of the two kernel
 * structs (below) and _Static_asserts tying one to the other. A field the
 * compiler packs differently, or a number edited in one place, stops the
 * build instead of producing a frame a libc misreads at run time. */

/* The kernel's `struct sigcontext`, field for field (asm/sigcontext.h).
 * Only the offsets matter; the types exist so offsetof can speak. */
struct LinuxSigContextMirror {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, pad0;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved1[8];
};

/* The kernel's `struct ucontext` prefix, up to and including sigmask. */
struct LinuxUContextMirror {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct { uint64_t ss_sp; int32_t ss_flags; uint32_t pad; uint64_t ss_size; } uc_stack;
    struct LinuxSigContextMirror uc_mcontext;
    uint64_t uc_sigmask;         /* the kernel-ABI sigset: 8 bytes */
};

/* `struct rt_sigframe` as this increment writes it (pretcode, uc, info). */
struct LinuxRtSigframeMirror {
    uint64_t pretcode;
    struct LinuxUContextMirror uc;
    uint8_t info[128];
};

_Static_assert(sizeof(struct LinuxSigContextMirror) == LINUX_SIGFRAME_MCONTEXT_BYTES,
               "sigcontext is 256 bytes (asm/sigcontext.h)");
_Static_assert(offsetof(struct LinuxSigContextMirror, r8) == 0,
               "sigcontext's gregs start with r8, not rdi — the kernel order");
_Static_assert(offsetof(struct LinuxSigContextMirror, rip) == 128,
               "rip is greg 16, at 128 bytes into the sigcontext");
_Static_assert(offsetof(struct LinuxSigContextMirror, eflags) == LINUX_SIGCTX_EFLAGS,
               "eflags follows the 17 gregs");
_Static_assert(offsetof(struct LinuxSigContextMirror, cs) == LINUX_SIGCTX_CS,
               "cs/gs/fs are u16s after eflags");
_Static_assert(offsetof(struct LinuxSigContextMirror, err) == LINUX_SIGCTX_ERR,
               "err/trapno/oldmask/cr2 follow the selectors and their pad");
_Static_assert(offsetof(struct LinuxSigContextMirror, fpstate) == LINUX_SIGCTX_FPSTATE,
               "the fpstate pointer is the last named field before reserved1");
_Static_assert(offsetof(struct LinuxUContextMirror, uc_mcontext) ==
                   LINUX_SIGFRAME_MCONTEXT - LINUX_SIGFRAME_UC,
               "uc_mcontext sits 40 bytes into the ucontext");
_Static_assert(offsetof(struct LinuxUContextMirror, uc_sigmask) == 296,
               "uc_sigmask follows the 256-byte sigcontext, 40 bytes into the ucontext");
_Static_assert(offsetof(struct LinuxRtSigframeMirror, uc) == LINUX_SIGFRAME_UC,
               "pretcode is one pointer, then the ucontext");
_Static_assert(offsetof(struct LinuxRtSigframeMirror, info) == LINUX_SIGFRAME_INFO,
               "siginfo follows the ucontext");
_Static_assert(sizeof(struct LinuxRtSigframeMirror) == LINUX_SIGFRAME_SIZE,
               "the frame ends where the (absent) fpstate would begin");

/* Byte helpers: the kernel is freestanding and this file carries no libc
 * dependency, so the frame is written little-endian by hand. */
static void sig_put64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void sig_put32(uint8_t *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void sig_put16(uint8_t *p, uint16_t v)
{
    int i;
    for (i = 0; i < 2; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t sig_get64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint16_t sig_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t sig_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One delivery's stack choice, so the builder's two callers (build and the
 * caller deciding where the handler runs) cannot disagree. Linux's rule:
 * SA_ONSTACK with a usable altstack runs there and says SS_ONSTACK; without
 * one the handler runs on the interrupted stack. */
static uint64_t sig_handler_stack(const struct LinuxSigFrameSpec *spec,
                                  int *onstack)
{
    *onstack = 0;
    if (spec->alt_sp != 0 &&
        spec->alt_size >= (uint64_t)LINUX_SIGFRAME_SIZE + 8u &&
        (spec->alt_flags & (uint32_t)LINUX_SS_DISABLE) == 0 &&
        (spec->act_flags & LINUX_SA_ONSTACK) != 0) {
        *onstack = 1;
        return spec->alt_sp + spec->alt_size;
    }
    return spec->user_rsp;
}

int linux_sig_frame_build(uint8_t *buf, uint32_t cap,
                          const struct LinuxSigFrameSpec *spec,
                          struct LinuxSigRegs *entry, uint64_t *entry_mask)
{
    uint64_t top, rsp, mask;
    int onstack, i;

    if (buf == 0 || spec == 0 || entry == 0) return -LINUX_EINVAL;
    if (cap < LINUX_SIGFRAME_SIZE) return -LINUX_EINVAL;
    if (spec->signo == 0 || spec->signo > 64) return -LINUX_EINVAL;
    /* SIG_DFL (0) and SIG_IGN (1) are reserved words, never addresses: a
     * frame for one would jump to NULL or to 1. */
    if (spec->handler <= 1) return -LINUX_EINVAL;
    /* No restorer, no return: the handler's `ret` would aim at whatever the
     * frame's pretcode says, and Linux's fallback (the vDSO trampoline) does
     * not exist here. Refusing is the honest answer. */
    if (spec->restorer == 0) return -LINUX_EINVAL;

    top = sig_handler_stack(spec, &onstack);
    /* Room for the frame plus the 8-byte alignment step below, with a
     * non-wrapping subtraction. */
    if (top < (uint64_t)LINUX_SIGFRAME_SIZE + 24u) return -LINUX_EINVAL;
    /* Linux's alignment: (sp - size) & -16, then -8, so the handler is
     * entered with rsp % 16 == 8 — the ABI's "after a call" shape, which is
     * what a compiler's prologue assumes. */
    rsp = ((top - (uint64_t)LINUX_SIGFRAME_SIZE) & ~(uint64_t)0xF) - 8u;

    /* The mask Linux installs for the handler: the thread's own blocked set,
     * the action's mask, and the signal being delivered — unless SA_NODEFER
     * asked for it not to be blocked (a handler that must see recursion). */
    mask = spec->blocked | spec->act_mask;
    if ((spec->act_flags & LINUX_SA_NODEFER) == 0)
        mask |= (uint64_t)1u << (spec->signo - 1u);

    for (i = 0; i < (int)LINUX_SIGFRAME_SIZE; i++) buf[i] = 0;

    sig_put64(buf + LINUX_SIGFRAME_PRETCODE, spec->restorer);
    sig_put64(buf + LINUX_SIGFRAME_UC_FLAGS, 0);
    sig_put64(buf + LINUX_SIGFRAME_UC_LINK, 0);
    if (onstack) {
        sig_put64(buf + LINUX_SIGFRAME_UC_STACK + 0, spec->alt_sp);
        sig_put32(buf + LINUX_SIGFRAME_UC_STACK + 8, LINUX_SS_ONSTACK);
        sig_put64(buf + LINUX_SIGFRAME_UC_STACK + 16, spec->alt_size);
    }

    for (i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
        sig_put64(buf + LINUX_SIGFRAME_MCONTEXT + (uint64_t)i * 8u, spec->regs.r[i]);
    sig_put64(buf + LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_EFLAGS, spec->regs.eflags);
    sig_put16(buf + LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_CS, LINUX_SIGFRAME_USER_CS);
    /* fpstate stays NULL: this increment carries no FP state (header's named
     * limit). cr2/err/trapno/oldmask are 0 for the same reason — nothing
     * here faults, and a stale fault address in a SIGTERM frame is a lie. */

    sig_put64(buf + LINUX_SIGFRAME_SIGMASK, mask);

    sig_put32(buf + LINUX_SIGFRAME_INFO + 0, spec->signo);  /* si_signo */
    sig_put32(buf + LINUX_SIGFRAME_INFO + 4, 0);            /* si_errno */
    sig_put32(buf + LINUX_SIGFRAME_INFO + 8, spec->code);   /* si_code  */
    /* si_addr is the union's first member for the fault signals, 16 bytes
     * in. Written unconditionally: a 0 there is "no address", not stale. */
    sig_put64(buf + LINUX_SIGFRAME_INFO + 16, spec->addr);

    /* The handler's entry context: the interrupted registers, with rsp/rip
     * aimed at the frame and the handler, and the three arguments the ABI
     * names (signo; then siginfo and ucontext when SA_SIGINFO was set). */
    *entry = spec->regs;
    entry->r[LINUX_SIGREG_RSP] = rsp;
    entry->r[LINUX_SIGREG_RIP] = spec->handler;
    entry->r[LINUX_SIGREG_RDI] = spec->signo;
    if (spec->siginfo) {
        entry->r[LINUX_SIGREG_RSI] = rsp + LINUX_SIGFRAME_INFO;
        entry->r[LINUX_SIGREG_RDX] = rsp + LINUX_SIGFRAME_UC;
    }
    if (entry_mask) *entry_mask = mask;
    return 0;
}

int linux_sig_frame_restore(const uint8_t *buf, uint32_t len,
                            struct LinuxSigRegs *regs, uint64_t *mask)
{
    int i;

    if (buf == 0 || regs == 0) return -LINUX_EINVAL;
    if (len < LINUX_SIGFRAME_SIZE) return -LINUX_EINVAL;
    /* The frame must have been built for a USER handler. cs is the one field
     * a forged frame cannot pick correctly by accident, and a restore that
     * accepted, say, cs=0x10 would return to ring 0 with the guest's
     * registers. */
    if (sig_get16(buf + LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_CS) !=
        (uint16_t)LINUX_SIGFRAME_USER_CS)
        return -LINUX_EINVAL;

    for (i = 0; i < (int)LINUX_SIGREG_COUNT; i++)
        regs->r[i] = sig_get64(buf + LINUX_SIGFRAME_MCONTEXT + (uint64_t)i * 8u);
    regs->eflags = sig_get64(buf + LINUX_SIGFRAME_MCONTEXT + LINUX_SIGCTX_EFLAGS);
    if (mask) *mask = sig_get64(buf + LINUX_SIGFRAME_SIGMASK);
    return 0;
}

int linux_sig_default_action(uint32_t signo)
{
    switch (signo) {
    case 17:                    /* SIGCHLD  */
    case 23:                    /* SIGURG   */
    case 28:                    /* SIGWINCH */
        return LINUX_SIGDEF_IGNORE;
    case 18:                    /* SIGCONT  */
        return LINUX_SIGDEF_CONTINUE;
    case 19:                    /* SIGSTOP  */
    case 20:                    /* SIGTSTP  */
    case 21:                    /* SIGTTIN  */
    case 22:                    /* SIGTTOU  */
        return LINUX_SIGDEF_STOP;
    case 6:                     /* SIGABRT */
    case 7:                     /* SIGBUS  */
    case 8:                     /* SIGFPE  */
    case 4:                     /* SIGILL  */
    case 3:                     /* SIGQUIT */
    case 11:                    /* SIGSEGV */
    case 31:                    /* SIGSYS  */
    case 5:                     /* SIGTRAP */
    case 24:                    /* SIGXCPU */
    case 25:                    /* SIGXFSZ */
        return LINUX_SIGDEF_CORE;
    case 0:
        return LINUX_SIGDEF_NONE;
    default:
        /* 1..64 not named above terminate without a core (SIGHUP, SIGINT,
         * SIGKILL, SIGTERM, SIGUSR1/2, the real-time range, ...), which is
         * what Linux's table says for every one of them. */
        return (signo <= 64) ? LINUX_SIGDEF_TERMINATE : LINUX_SIGDEF_NONE;
    }
}

int linux_sig_delivery_needed(uint64_t blocked, uint32_t signo,
                              uint64_t handler)
{
    if (signo == 0 || signo > 64) return 0;
    if (blocked & ((uint64_t)1u << (signo - 1u))) return 0;
    if (handler <= 1) return 0;   /* SIG_DFL / SIG_IGN: no frame */
    return 1;
}

int linux_sig_park_is_eintr(uint32_t signo, uint64_t blocked,
                            uint64_t handler, uint64_t act_flags)
{
    if (signo == 0 || signo > 64) return 0;
    if (blocked & ((uint64_t)1u << (signo - 1u))) return 0;
    if (handler <= 1) return 0;
    if (act_flags & LINUX_SA_RESTART) return 0;
    return 1;
}
