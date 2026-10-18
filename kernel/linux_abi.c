/*
 * linux_abi.c — Phase E7, increment 1: the three contracts as code.
 *
 * Pure functions only: no state, no allocation, no trap path, no sidecar.
 * Each one is a total function whose every input is either enumerated by
 * the census artifact or refused by name — the same "no silent default"
 * discipline the rest of this tree applies to on-disk formats. See
 * kernel/linux_abi.h for why this increment lands before any mechanism.
 */
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
    case CAP_EPERM:                  return -LINUX_EPERM;
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
    default:               return 0;   /* no unique preimage (or not an error) */
    }
}

/* ─── The syscall classification table (§5.6) ──────────────────────────────
 * Built FROM tools/linux_syscall_census.txt: 27 rows, sorted by number.
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
    {   9, LSC_MEM,    0 },   /* mmap — §5.4 budget-backed */
    {  10, LSC_MEM,    LSC_F_REFUSES_WX }, /* mprotect — W|X refused (§5.4) */
    {  11, LSC_MEM,    0 },   /* munmap */
    {  12, LSC_MEM,    0 },   /* brk — §5.4 bump pointer in the budget */
    {  13, LSC_SIG,    0 },   /* rt_sigaction — recorded, never delivered */
    {  14, LSC_SIG,    0 },   /* rt_sigprocmask — recorded, never delivered */
    {  59, LSC_SPAWN,  0 },   /* execve — the spawn path */
    {  72, LSC_FD,     0 },   /* fcntl */
    {  74, LSC_FD,     0 },   /* fsync */
    {  77, LSC_FD,     0 },   /* ftruncate */
    {  82, LSC_FD,     0 },   /* rename — the .img.partial -> final swap */
    {  83, LSC_FD,     0 },   /* mkdir */
    {  87, LSC_FD,     0 },   /* unlink */
    { 131, LSC_SIG,    0 },   /* sigaltstack — recorded, never delivered */
    { 158, LSC_MSR,    0 },   /* arch_prctl(ARCH_SET_FS) — §5.3's one MSR */
    { 217, LSC_FD,     0 },   /* getdents64 */
    { 218, LSC_LOCAL,  0 },   /* set_tid_address — single-threaded: stored and ignored */
    { 228, LSC_LOCAL,  0 },   /* clock_gettime — vDSO-backstopped (§5.5): a real trap here */
    { 231, LSC_EXIT,   0 },   /* exit_group — the sidecar reaps, frames return */
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
    if (num != LINUX_WRITE || !args) return p;   /* nothing to attach */

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
#define LAB_PTE_USER     0x4ULL
#define LAB_PTE_PS       0x80ULL
#define LAB_PTE_FRAME    0x000FFFFFFFFFF000ULL

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
 * frame read-back). Read-ONLY by construction: every level is checked
 * before the next pointer is followed, and nothing anywhere writes a
 * table or creates a missing one — the allocating walker (arch/x86/
 * walk_page_tables_x86.c) is exactly what these must NOT be: over a
 * tenant-supplied address its table allocation is a memory lever, not a
 * read. The USER bit is checked at every level, matching what a ring-3
 * load would require.
 *
 * Frame -> pointer is the identity map's own claim (boot.asm: "the
 * physical addr = virtual addr"). */
static int lab_walk_leaf(uint64_t root, uint64_t va, uint64_t *leaf_out)
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
        if (!(e & LAB_PTE_USER)) return -LINUX_EFAULT;
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
        if (lab_walk_leaf(root, va, &leaf) != 0) return -LINUX_EFAULT;
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

/* ─── E7 increment 6 (the mem class, §5.4) — the budget span ──────────────
 * The two decisions the resume arm's brk apply makes, and the one read it
 * needs, as the pure functions the host test drives. Nothing below reads
 * kernel state: the verdict is arithmetic over the stamped span and the
 * staged request, the frame reader is a walk of a root the CALLER supplies
 * (the task's PML4 in production, a fake table in the test — the same fake
 * tables the copy's cases already build). */
int linux_mprotect_refuses_wx(uint64_t prot)
{
    return (prot & LINUX_PROT_WRITE) && (prot & LINUX_PROT_EXEC);
}

struct LinuxMemBrkVerdict linux_mem_brk_verdict(uint64_t heap_base,
                                                uint64_t heap_limit,
                                                uint64_t brk_cur,
                                                uint64_t req, uint64_t rax)
{
    struct LinuxMemBrkVerdict v;
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
        req >= heap_base && req <= heap_limit) {
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

uint64_t linux_user_page_frame(uint64_t root, uint64_t va)
{
    uint64_t leaf = 0;
    if (lab_walk_leaf(root, va, &leaf) != 0) return 0;
    return leaf & LAB_PTE_FRAME;
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
        if ((((uintptr_t)region + c_off) % 16u) == 0) break;
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
            argv_out[i] = (uint64_t)(uintptr_t)w;
            s = p->argv[i];
            while (*s) *w++ = (uint8_t)*s++;
            *w++ = 0;
        }
        argv_out[n_argv] = 0;
        for (i = 0; i < n_envp; i++) {
            envp_out[i] = (uint64_t)(uintptr_t)w;
            s = p->envp[i];
            while (*s) *w++ = (uint8_t)*s++;
            *w++ = 0;
        }
        envp_out[n_envp] = 0;
        execfn_va = (uint64_t)(uintptr_t)w;
        s = p->execfn;
        while (*s) *w++ = (uint8_t)*s++;
        *w++ = 0;
        platform_va = (uint64_t)(uintptr_t)w;
        s = platform;
        while (*s) *w++ = (uint8_t)*s++;
        *w++ = 0;
        random_va = (uint64_t)(uintptr_t)w;
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

    return (uint64_t)(uintptr_t)(region + argc_off);
}
