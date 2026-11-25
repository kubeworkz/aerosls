#ifndef LINUX_ABI_H
#define LINUX_ABI_H

/*
 * linux_abi.h — Phase E7 (AeroSLS-Linux-ABI-Shim-Design-v0.1.md), increment 1:
 * the three pure-function contracts the shim is built on.
 *
 * ─── Why this file exists, and why it is first ─────────────────────────────
 * E7's in-scope list (§9) starts with machinery that traps, forwards and
 * parks — but three of its pieces are pure functions with no dependencies
 * on the trap path, the sidecar, or a boot: the errno map (§5.7), the
 * syscall classification table built from the census artifact (§5.6), and
 * the initial stack/auxv builder (§5.2). This repo lands record layers
 * before mechanism (P1a's first increment was the checkpoint record for
 * the same reason), and these are E7's record layer: the contracts every
 * later increment consumes, each asserted by a host test with no boot, no
 * cargo and no built kernel.
 *
 * ─── What is deliberately NOT here ────────────────────────────────────────
 * The personality field, the trap-path branch, the six-argument capture,
 * the request struct and the describe-and-refuse branch (steps 1-4) are
 * increment 2; the channel delivery, the park/resume and the reply's trip
 * through the errno map (steps 5-7) and the sidecar core's first answers
 * are increment 3 — the three pure functions gained here for it (apply,
 * in-flight state, channel shape) are what that increment's host teeth
 * bite on, instead of a boot. fs_base/TLS (§5.3) is increment 4, because
 * it is the one piece that touches the MSR and the context switch.
 * The fd class (§5.6's read/write row — "the kernel copy plus the
 * sidecar's fd table") is increment 5: its PLAN and its COPY are pure
 * over a page-table root, so the byte crossing gets host teeth without a
 * boot, and brk/mmap budget policy (§5.4) moves to increment 6. This file
 * carries no mechanism, so a bug here can only be a wrong number or a
 * malformed image — both of which a host test names, and neither of which
 * can reach a running system.
 *
 * ─── Freestanding rule ────────────────────────────────────────────────────
 * The kernel build is freestanding: no <errno.h>, no libc. Linux's errno
 * numbers are ABI (they are what a static musl binary was compiled
 * against), so they are owned here as literals with their origin noted —
 * the same discipline kernel/cap.h applies to CAP_E*. The mapping between
 * the two families is a total function: every code in either direction is
 * enumerated, including the CAP_NONE sentinel, which must be translated
 * and never leaked as a return value to a Linux task (§5.7).
 */
#include <stdint.h>
#include <stddef.h>

/* ─── Linux x86-64 errno numbers (owned values; <errno.h> is not includable)
 * Only the codes the two families can produce are defined. glibc/musl see
 * these as negative rax values in [-4095, -1]. ───────────────────────────── */
#define LINUX_EPERM        1
#define LINUX_ENOENT       2
#define LINUX_ESRCH        3
#define LINUX_EINTR        4
#define LINUX_EIO          5
#define LINUX_EBADF        9
#define LINUX_EAGAIN      11
#define LINUX_ENOMEM      12
#define LINUX_EBUSY       16
#define LINUX_EINVAL      22
#define LINUX_ENFILE      23
#define LINUX_ENOSPC      28
#define LINUX_ERANGE      34
#define LINUX_ENOSYS      38
#define LINUX_EPROTO      71
#define LINUX_ENOBUFS    105
#define LINUX_ETIMEDOUT  110
#define LINUX_EALREADY   114
/* The file class's own errnos (E7 step 4): the VFS failures §5.7's map
 * must be able to name — each is the twin of a cap.h CAP_E* code added
 * with this increment, and of the sidecar's vfs::Errno code() values
 * (user/vfs/src/errno.rs pins the Linux numbering on the Rust side). */
#define LINUX_EACCES      13
#define LINUX_EEXIST      17
#define LINUX_ENOTDIR     20
#define LINUX_EISDIR      21
#define LINUX_ESPIPE      29
#define LINUX_EROFS       30
#define LINUX_ENAMETOOLONG 36
#define LINUX_ENOTEMPTY   39

/* ─── The syscall classification table (§5.6) ──────────────────────────────
 * The census artifact (tools/linux_syscall_census.txt) is the specification:
 * 27 numbers the designated first user actually calls. Every one gets a
 * class naming WHERE its semantics live, and — this is the number-space
 * property that makes D1 structural — a Linux number that is NOT in the
 * table is LSC_UNKNOWN, and a Linux-personality task's dispatch may only
 * refuse it. The SLS numbers this candidate's ABI overlaps (105, 160, 320;
 * §2.2) are therefore UNKNOWN in this table by construction: no entry, no
 * semantics, no path to a SYS_ arm. tests/linux_syscall_table_host_test.c
 * asserts every census row has a non-UNKNOWN class and that 105 is UNKNOWN. */
enum LinuxSyscallClass {
    LSC_UNKNOWN = 0,   /* not in the census: refused by name, never dispatched */
    LSC_FD,            /* sidecar fd table + path resolution (Phase 2 §3-§4) */
    LSC_MEM,           /* §5.4 budget-backed memory: brk/mmap/munmap/mprotect */
    LSC_SIG,           /* recorded at the sidecar, and delivered ONLY as
                        * tkill's default action (§10.13's step 2); a user
                        * HANDLER is RECORDED (the full 32-byte act) but
                        * never invoked — delivery is refused by name (no
                        * signal frames/rt_sigreturn in v1)                 */
    LSC_MSR,           /* §5.3 the one kernel-side semantic: arch_prctl(SET_FS) */
    LSC_SPAWN,         /* execve: the spawn path (the sidecar's to decide) */
    LSC_THREAD,        /* clone: kernel-local, no sidecar request (I2)      */
    LSC_TASK,          /* getpid/gettid/exit: kernel-local identity and
                        * thread lifecycle (I2)                            */
    LSC_FUTEX,         /* futex: kernel-local wait/wake over the group's
                        * own word (I3)                                    */
    LSC_LOCAL,         /* sidecar-local: no fd, no memory, no kernel state */
    LSC_EXIT,          /* exit_group: the sidecar reaps, frames return */
};

/* Per-entry flags. The one that exists today is a §5.4 contract: the shim's
 * mprotect MUST refuse simultaneous write+execute, and H2's Node exclusion
 * is that refusal rather than a restatement of it. Making it a table flag
 * means the property is assertable against the census's own mprotect row
 * instead of living in prose beside the dispatch. */
#define LSC_F_REFUSES_WX  0x1u

struct LinuxSyscallEntry {
    uint64_t             num;
    enum LinuxSyscallClass cls;
    uint32_t             flags;
};

/* The table itself, so the host test (and later the dispatch) read ONE
 * artifact. Sorted by number; length exposed for bounds checks. */
extern const struct LinuxSyscallEntry linux_syscall_table[];
extern const size_t linux_syscall_table_len;

/* Class of a Linux syscall number; LSC_UNKNOWN for anything not in the
 * census. Total function: the answer for every uint64_t is defined. */
enum LinuxSyscallClass linux_syscall_class(uint64_t num);

/* Flags of a Linux syscall number, or 0 when the number is unknown. */
uint32_t linux_syscall_flags(uint64_t num);

/* Class name for logs and refusal lines ("fd", "mem", ..., "unknown").
 * Never NULL. */
const char *linux_syscall_class_name(enum LinuxSyscallClass cls);

/* ─── E7 §5.3 (the plan's increment 4) — arch_prctl's contracts ─────────
 * The one Linux semantic the KERNEL unavoidably owns: the design's split is
 * "the sidecar decides WHETHER the task may, the kernel does the write",
 * so both halves of that sentence get a pure function — host-testable
 * instead of boot-testable. The sidecar re-implements the same bound
 * literally (user/sidecar/src/linux.rs's 158 arm, commented as the
 * mirror) because the Rust side cannot include this header; the kernel
 * re-checks the address AT THE WRITE regardless of what the grant said,
 * because the wrmsr is the dangerous half. */
#define LINUX_CLONE           56u            /* E7 TG-1 I2: kernel-local clone */
#define LINUX_GETPID          39u            /* E7 TG-1 I2: getpid — tgid */
#define LINUX_EXIT            60u            /* E7 TG-1 I2: exit — thread-local */
#define LINUX_GETTID          186u           /* E7 TG-1 I2: gettid — tid */
#define LINUX_ARCH_PRCTL      158u           /* the census row (§5.6) */
#define LINUX_ARCH_SET_FS     0x1002u        /* cmd word in args[0]    */
#define LINUX_ARCH_SET_GS     0x1001u        /* no GSR in this kernel:
                                                the sidecar always
                                                answers it ENOSYS       */
#define LINUX_ARCH_GET_FS     0x1003u        /* the value lives in the
                                                kernel's field — this
                                                sidecar cannot answer
                                                (not in the census)    */
#define LINUX_USER_ADDR_MAX   0x00007FFFFFFFFFFFULL

/* ─── E7 increment 19: the time source across the seam ────────────────────
 * clock_gettime (228) is in the census but vDSO-backstopped on Linux; this
 * kernel ships no vDSO, so the call really traps here. The reply is one
 * Linux `struct timespec` — two little-endian i64s, seconds then
 * nanoseconds — written back to args[1]. NULL is -EFAULT at the FIRST trap
 * (the same span rule every reply plan uses), before any channel post. The
 * sidecar owns the value (a pinned epoch, linux.rs's 228 arm); the kernel
 * owns only where the sixteen bytes land. */
#define LINUX_CLOCK_GETTIME   228u
#define LINUX_TIMESPEC_BYTES  16u

/* Is this address legal as a task's FS base? User-canonical — bits 63..47
 * all zero, one unsigned compare for a 48-bit user VA. 0 is legal
 * (refusing it would invent a rule no Linux kernel has). The kernel
 * applies this BEFORE every wrmsr, so a grant that somehow waved a
 * non-canonical value through still ends as -EINVAL in rax instead of a
 * garbage MSR. */
int linux_fs_addr_is_user_canonical(uint64_t addr);

/* Was this trap an ARCH_SET_FS? (num, args[0]) — ONE predicate used at
 * BOTH ends of the stage/apply seam (first trap stages, resume arm
 * applies), so the two ends can never disagree about what got staged. */
int linux_arch_prctl_is_set_fs(uint64_t num, uint64_t cmd);

/* ─── E7 increment 5 (the fd class) — the kernel copy ─────────────────────
 * The one fd-class byte crossing §5.6's row names: write(2)'s buffer lives
 * in the CHILD's address space (a separate cr3 by §3.5), so the sidecar —
 * which owns the fd table and every policy decision — cannot read it. The
 * kernel copies it out into the forwarded message, exactly as increment 4
 * taught the kernel to perform the one MSR write: mechanism here, policy
 * (which fd, what it means) always in the sidecar (§3.3 step 6).
 *
 * Two pieces, both pure enough for host teeth without a boot:
 *   - the PLAN — how many bytes cross, from where, or why not. It never
 *     reads kernel state; given the same words every caller gets the same
 *     answer. count == 0 plans nothing (Linux does not validate a buffer
 *     it is not asked to touch), a span that leaves the user-canonical
 *     range refuses -EFAULT BEFORE the post (a first-trap refusal, §3.3),
 *     and a count above LINUX_FD_WRITE_CHUNK clamps — a short write is
 *     POSIX-legal, and the clamp rides IN the description (args[2]) so
 *     header, payload and the count the task will see are one truth.
 *   - the COPY — a read-only walk of the task's OWN page tables: present,
 *     user, never a huge page, every level. The USER bit is the security
 *     boundary of this copy: it reads exactly what ring-3 could read
 *     itself, so forwarding the bytes through the sidecar exposes nothing
 *     the task did not already hold — and a missing mapping is -EFAULT
 *     into rax, never a page fault in ring 0 over a tenant's pointer.
 *
 * The identity map (boot.asm, 0–4 GiB, "physical addr = virtual addr") is
 * what lets a translated frame be read directly; the walk itself starts
 * from the task's own PML4 physical address (ProcessDescriptor.cr3), so
 * it is correct whichever cr3 the trap path is running on. */
#define LINUX_WRITE           1u           /* the census's write row (§5.6) */
#define LINUX_EFAULT          14           /* Linux's own number: a bad
                                            * buffer address. Returned by
                                            * the plan/copy paths directly —
                                            * it is the task's truth about
                                            * the task's own pointer, not a
                                            * CAP-family code, so it never
                                            * crosses the errno map (§5.7). */
#define LINUX_FD_WRITE_CHUNK  1024u        /* bytes that cross per write
                                            * trap: ≤ CAP_MSG_MAX_PAYLOAD
                                            * − sizeof(request) (the guard's
                                            * clause M does the arithmetic
                                            * against cap.h's literal), and a
                                            * legal short write either way. */

struct LinuxFdWritePlan {
    uint64_t src;      /* child VA the copy reads from (args[1])          */
    uint32_t len;      /* bytes to attach: min(count, CHUNK); 0 = none     */
    int32_t  refuse;   /* 0 = proceed; else the Linux errno to return
                        * BEFORE any post (first-trap refusal discipline)  */
};

/* Plan one write's or poll's byte crossing. For a number that is neither
 * LINUX_WRITE nor LINUX_POLL the plan is empty (no attach, no refusal) —
 * the rest of the fd class still forwards as the plain 72-byte
 * description it was. Pure: the host test drives every branch, including
 * count==0 ignoring a non-canonical buffer, nfds==0 planning nothing, the
 * poll cap and the wrap-around span. */
struct LinuxFdWritePlan linux_fd_write_plan(uint64_t num,
                                            const uint64_t args[6]);

/* Copy n bytes from a task's user VA into dst, walking root's page tables
 * read-only. Returns 0, or -LINUX_EFAULT — never a fault: every level is
 * checked before the bytes are read, and a span crossing a page boundary
 * validates BOTH pages. n == 0 succeeds without reading anything (root
 * may be 0). dst must hold n bytes; the caller owns it. */
int linux_user_copy_out(uint8_t *dst, uint64_t root, uint64_t va, uint32_t n);

/* ─── E7 increment 24 — poll (7): readiness and waiting ────────────────────
 * The firstuser's LAST ENOSYS answer. Rust std's lang_start_internal polls
 * fds {0,1,2} before it installs any signal handler, and an errno outside
 * its tolerated set aborts the task. A timeout is now part of the request,
 * so the KERNEL copies the pollfd array out and the reply's rewritten
 * array back in; the SIDECAR owns readiness, the pending wait, and the
 * timeout decision. The array is capped at LINUX_POLL_MAX_FDS so one call
 * cannot make either side's frame unbounded. A NULL array with nfds > 0,
 * a non-canonical span, or nfds above the cap is a first-trap refusal. */
#define LINUX_POLL  7u             /* the census's poll row (§5.6)       */
#define LINUX_POLL_MAX_FDS 16u
#define LINUX_POLLFD_BYTES 8u       /* struct pollfd: fd, events, revents  */
#define LINUX_POLL_PAYLOAD_BYTES (LINUX_POLL_MAX_FDS * LINUX_POLLFD_BYTES)


/* ─── E7 increment 6 (the mem class, §5.4) — the budget span ──────────────
 * §5.4's row is "budget-backed memory", and its first half is brk: a bump
 * pointer inside a reserved span. The split is the design's own — the
 * sidecar decides WHETHER the break moves (the budget is its policy: it
 * authored the manifest's BUDGET record), the kernel provides the frames —
 * and it is increment 4's posture one level up: a page-table write instead
 * of an MSR write, so the same three rules hold. The span is stamped at
 * create (kernel/cap.c, beside the personality stamp); the grant is
 * re-checked AT THE MAP against the span AND against the request the FIRST
 * trap staged, because a reply is never trusted about the address it asks
 * the kernel to map.
 *
 * Three pieces, all pure enough for host teeth without a boot:
 *   - the W|X refusal: simultaneous write+execute is refused by the shim
 *     itself (§5.4; the census's mprotect row carries LSC_F_REFUSES_WX,
 *     and H2's Node exclusion IS this refusal). It is a property of the
 *     REQUEST — no object, no policy — so the first trap produces it
 *     before the post, exactly like the write plan's -EFAULT, and the
 *     sidecar carries the same refusal as its own arm so neither end
 *     alone is load-bearing.
 *   - the brk verdict: what the resume arm must do with a reply, over the
 *     kernel's own span, the staged request and the reply's rax. Linux's
 *     brk failure shape is the load-bearing convention here: a denied brk
 *     returns the CURRENT break, not -ENOMEM — libc compares the return
 *     against the request — so "the break did not move" is a success
 *     answer with a value, never an errno, and a grant must echo the
 *     request exactly (the increment-5 one-truth rule, on an address) and
 *     sit inside the stamped span before a single page is mapped. The
 *     mapped range is [align_up(cur), align_up(new)): the page CONTAINING
 *     the old break was mapped by whatever moved the break there, so the
 *     grant can never re-map a live page (which would leak the old frame
 *     and reset the bytes on it).
 *   - the page-frame reader: the leaf frame behind a user mapping, walked
 *     read-only under the copy's own three boundaries (present, USER at
 *     every level, no huge page). The resume arm needs it for the UNWIND:
 *     a grant whose frames run out mid-batch must return the frames it
 *     already mapped, and reading the leaf back is the only honest way to
 *     learn which ones those are — deliberately NOT walk_page_tables,
 *     which allocates a missing table (over any address, which is how a
 *     read becomes a memory lever). */
#define LINUX_BRK        12u           /* the census's brk row (§5.6)      */
#define LINUX_MPROTECT   10u           /* the census's mprotect row        */
#define LINUX_MMAP        9u           /* the census's mmap row — the      */
#define LINUX_MUNMAP     11u           /* anonymous-map half landed at     */
                                       /* increment 16; its munmap seam at */
                                       /* increment 25 (the memory class's */
                                       /* last one)                        */
#define LINUX_PROT_NONE  0x0u          /* Linux mman.h PROT_* bits — ABI  */
#define LINUX_PROT_READ  0x1u          /* (the sidecar's twin repeats them */
#define LINUX_PROT_WRITE 0x2u          /*  as literals; guard N3 pins it)  */
#define LINUX_PROT_EXEC  0x4u

/* The mmap flag bits this shim's v1 knows by name (Linux mman.h). The
 * allowlist is closed: anything outside it is refused by name at BOTH
 * ends, with the flags on the answer line. FIXED is honored; NORESERVE
 * and STACK are documented no-op hints on x86-64 — accepting and
 * ignoring them IS Linux's behavior, not a shortcut. ANONYMOUS+PRIVATE
 * is the only served class: the measured allocator's arena shapes. */
#define LINUX_MAP_SHARED    0x0001u
#define LINUX_MAP_PRIVATE   0x0002u
#define LINUX_MAP_FIXED     0x0010u
#define LINUX_MAP_ANONYMOUS 0x0020u
#define LINUX_MAP_NORESERVE 0x1000u  /* hint, ignored (documented no-op) */
#define LINUX_MAP_STACK     0x20000u /* hint, ignored (documented no-op) */

/* The closed allowlist: PRIVATE|ANON|FIXED|NORESERVE|STACK. A request
 * whose flags leave bits outside this mask is a shape the shim does not
 * serve — refused ENOSYS by name, the flags on the line. */
#define LINUX_MAP_V1_ALLOW (LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | \
                            LINUX_MAP_FIXED   | LINUX_MAP_NORESERVE | \
                            LINUX_MAP_STACK)

/* 1 iff this mprotect must be refused before the post: prot has BOTH the
 * write and the execute bit. Total over every prot word. Also the mmap
 * first-trap's W|X rule (increment 16): the same §5.4 refusal over a
 * prot word that arrives with a mapping ask. */
int linux_mprotect_refuses_wx(uint64_t prot);

/* The request a first trap stages for a granted mmap — the whole ask,
 * not just args[0]: the verdict at the reply re-checks the grant
 * against THESE words (what the FIRST trap saw), never against the
 * reply's. This is brk's staged-request rule (§5.4) over four words
 * instead of one. */
struct LinuxMmapReq {
    uint64_t addr;   /* args[0] as captured — the FIXED address, or 0  */
    uint64_t len;    /* args[1] as captured — bytes, not pages          */
    uint64_t prot;   /* args[2] as captured — PROT_* word               */
    uint64_t flags;  /* args[3] as captured — MAP_* word                */
};

/* The first-trap refusals for mmap (increment 16): pure predicates over
 * the request words — properties of the REQUEST, not of any object, so
 * the channel is never posted for a shape that cannot be answered (the
 * mprotect/poll posture). Returns 0 (proceed to describe-and-post) or a
 * NEGATIVE Linux errno — the copy plans' convention, so the call site
 * maps it straight into rax: -EPERM for W|X, -EINVAL for a malformed
 * word (zero length, unknown prot bit, MAP_FIXED on an unaligned
 * address), -ENOSYS for a shape not served (file-backed, shared, EXEC,
 * unknown flag bits), -ENOMEM for a FIXED address outside the stamped
 * span — the budget shape §5.4 names. span_base/span_limit are the
 * descriptor's OWN stamped numbers (for a task without a heap span
 * they are 0 and only the non-span rules apply). */
int linux_mem_mmap_refusal(const struct LinuxMmapReq *req,
                           uint64_t span_base, uint64_t span_limit);

struct LinuxMemMmapVerdict {
    int      malformed;    /* 1: the reply was neither this request's
                            * grant nor its refusal — nothing maps, the
                            * caller logs it and answers ENOMEM        */
    int      replace;      /* 1: MAP_FIXED — every present page in the
                            * range is unmapped first (frame returned),
                            * then the grant installs per prot        */
    int      prot_write;   /* 1: map the granted pages RW-NX; 0: RO-NX */
    uint64_t map_va;       /* the range's first page: the one to back
                            * (map_pages > 0) or reserve (PROT_NONE)   */
    uint64_t range_pages;  /* pages in the granted range — the replace
                            * walk covers exactly these               */
    uint64_t map_pages;    /* pages to back with zero-filled frames — 0
                            * for PROT_NONE (reservation only: every
                            * access faults, which is PROT_NONE's whole
                            * observable contract); always <= range_pages */
    uint64_t new_mmap_top; /* the kernel-side cursor after this grant: */
                            /* descends for non-FIXED, unchanged for   */
                            /* FIXED (last map wins, as Linux itself)  */
};

/* The verdict at the reply (increment 16): re-check a granted mmap's
 * answer against the kernel's OWN stamped span and OWN two cursors
 * (brk_cur, mmap_top) — the staged request is what the first trap saw,
 * rax is what the reply delivered. FIXED must echo the staged address
 * exactly; non-FIXED must be page-aligned and land strictly above the
 * break and at or below the cursor. A refusal reply (rax < 0) maps
 * nothing and is not malformed — its errno rides the §5.7 map as
 * usual. Pure: the host test drives every branch, including each way a
 * grant can be a lie the kernel must refuse. */
struct LinuxMemMmapVerdict
linux_mem_mmap_verdict(uint64_t heap_base, uint64_t heap_limit,
                       uint64_t brk_cur, uint64_t mmap_top,
                       const struct LinuxMmapReq *req, uint64_t rax);

struct LinuxMemBrkVerdict {
    int      malformed;  /* 1: the reply was neither this request's grant
                          * nor its refusal — nothing maps, the break
                          * stays, the caller logs it                   */
    uint64_t map_va;     /* first page to back (valid iff map_pages > 0) */
    uint32_t map_pages;  /* fresh frames to map (0 = the break did not
                          * move: denial echo or a no-op ask)           */
    uint64_t new_cur;    /* the break the kernel records on success      */
};

/* Verdict on a brk reply. heap_base/heap_limit/brk_cur are the kernel's
 * OWN stamped state (never the reply's), mmap_top is the anonymous-map
 * cursor (increment 16): brk's ceiling is min(heap_limit, mmap_top),
 * because a page a granted mmap backed is a page a brk growth may never
 * map over (a re-map leaks the frame and zeros live bytes). req is the
 * break the first trap staged (args[0] as captured), rax is what the
 * reply delivered. Pure: the host test drives every branch, including
 * each way a grant can be a lie the kernel must refuse. */
struct LinuxMemBrkVerdict linux_mem_brk_verdict(uint64_t heap_base,
                                                uint64_t heap_limit,
                                                uint64_t mmap_top,
                                                uint64_t brk_cur,
                                                uint64_t req, uint64_t rax);

/* ─── E7 increment 25: munmap, the memory class's last seam ─────────────
 * The request is two words, but the decisions are the same both-ends
 * pattern brk/mmap use. The FIRST trap refuses a shape no page-table
 * walk can honestly serve — a zero or overflowed length, an unaligned
 * address, a range outside the stamped span, or one that starts below
 * the current break (v1 keeps the break's pages the brk seam's). The
 * REPLY verdict re-checks a success (rax 0; a negative errno unmaps
 * nothing) against the kernel's OWN span and break, then names the
 * exact page range the apply walks. The sidecar answers WHETHER; the
 * kernel's page tables are the truth about which pages are present, so
 * a double unmap is success and no sidecar mapping ledger is needed.
 * The two cursors never move: a freed virtual range is not re-handed by
 * a later mmap in v1 (frames return; the range waits for teardown). */
struct LinuxMunmapReq {
    uint64_t addr;   /* args[0] as captured — the page-aligned start     */
    uint64_t len;    /* args[1] as captured — bytes, rounded up to pages */
};

int linux_mem_munmap_refusal(const struct LinuxMunmapReq *req,
                             uint64_t span_base, uint64_t span_limit,
                             uint64_t brk_cur);

struct LinuxMemMunmapVerdict {
    int      malformed;    /* 1: a success reply that is not this
                            * request's page range — unmap nothing      */
    uint64_t unmap_va;     /* the first page to walk (valid iff > 0)    */
    uint64_t unmap_pages;  /* pages in the rounded range — absent pages
                            * are skipped by the apply, not an error    */
};

struct LinuxMemMunmapVerdict
linux_mem_munmap_verdict(uint64_t heap_base, uint64_t heap_limit,
                         uint64_t brk_cur,
                         const struct LinuxMunmapReq *req, uint64_t rax);

/* ─── E7 §10.15 — the soft-protected transition ──────────────────────────
 * The first trap validates the three request words against the task's own
 * image, stack and heap windows. The sidecar answers the non-W|X policy;
 * the kernel's page-table prewalk is the truth about which pages exist.
 * No frame is allocated or freed, and every page is validated before the
 * first protection bit changes. */
struct LinuxMprotectReq {
    uint64_t addr;   /* args[0] — page-aligned start */
    uint64_t len;    /* args[1] — bytes, rounded up */
    uint64_t prot;   /* args[2] — PROT_* word */
};

int linux_mem_mprotect_refusal(const struct LinuxMprotectReq *req,
                               uint64_t image_lo, uint64_t image_hi,
                               uint64_t stack_lo, uint64_t stack_hi,
                               uint64_t heap_lo, uint64_t heap_hi);

struct LinuxMemMprotectVerdict {
    int      malformed; /* 1: a success reply for a request the first
                         * trap would refuse, or a non-zero success */
    uint64_t va;        /* first page the apply may protect */
    uint64_t pages;     /* pages in the rounded range */
};

struct LinuxMemMprotectVerdict
linux_mem_mprotect_verdict(uint64_t image_lo, uint64_t image_hi,
                           uint64_t stack_lo, uint64_t stack_hi,
                           uint64_t heap_lo, uint64_t heap_hi,
                           const struct LinuxMprotectReq *req, uint64_t rax);

/* The 4 KiB frame behind a user mapping, walked read-only. The OWNERSHIP
 * walk accepts a PRESENT soft-protected leaf (U=0 plus the software bit)
 * so munmap/brk/mmap unwind can return its frame; the copy walkers do
 * not. 0 when any level refuses (absent, non-owner, huge). The unwind's
 * read-back — and only that: a kernel address or a missing page reads as
 * 0, so a walker refusal can never free a frame that was not mapped by
 * this path. */
uint64_t linux_user_page_frame(uint64_t root, uint64_t va);

/* ─── E7 increment 10 (the signal seam, §10.13's step 2) — the sig class's byte crossing ──
 * rt_sigaction's `act` and rt_sigprocmask's `set` live in the CHILD's
 * address space, so they cross exactly as write(2)'s buffer does: the
 * KERNEL copies them OUT into the forwarded message (the same read-only
 * walker, the same USER-bit boundary, the same first-trap -EFAULT), and
 * the SIDECAR owns every semantic decision over the bytes — which
 * disposition changed, whether a mask block happened, whether the answer
 * is a delivery (its 13/14/200 arms). This function is the mechanism's
 * half of that split and nothing more.
 *
 * Two refusals live HERE, both at the FIRST trap and before any channel,
 * because both are places the request as asked cannot honestly be served:
 *   - args[3] (sigsetsize) != 8 — the kernel-ABI sigset this seam crosses
 *     is 8 bytes; a caller claiming another size would get its bytes
 *     silently reinterpreted. Linux's own number answers this: EINVAL.
 *   - args[1] (act/set) spans the copy-IN (increment 12): a non-canonical
 *     or max-crossing span is -EFAULT at the first trap, same as ever.
 * The OUT-POINTER (args[2] != 0) is NO LONGER refused here: increment 12
 * landed §10.10's copy-IN gap, and the out-pointer's span is planned by
 * linux_fd_reply_plan below — one plan per number, both computed before
 * the describe, both first-trap.
 * A NULL act/set with a NULL out-pointer is a no-op the sidecar answers
 * 0 — nothing to copy, nothing to ask. Pure: the host test drives every
 * branch (tests/linux_personality_host_test.c). */
#define LINUX_RT_SIGACTION   13u          /* the census's sigaction row   */
#define LINUX_RT_SIGPROCMASK 14u          /* the census's sigprocmask row */
#define LINUX_SIGACT_BYTES   32u          /* kernel-ABI struct sigaction:
                                           * sa_handler, sa_flags, sa_mask,
                                           * sa_restorer — four u64s       */
#define LINUX_SIGSET_BYTES   8u           /* the sigset the shim crosses  */
#define LINUX_SIGALTSTACK   131u          /* the census's sigaltstack row */
/* E7 increment 28: tkill — the number whose reply can BE a delivery. It is
 * named here (and only here) because the kernel must state which number a
 * 80-byte reply belongs to; the classification table still decides the
 * class, and nothing here adds a row to it. */
#define LINUX_TKILL         200u
#define LINUX_STACK_T_BYTES  24u          /* kernel-ABI struct stack_t:
                                           * ss_sp u64, ss_flags i32 + pad,
                                           * ss_size u64                    */
#define LINUX_SS_DISABLE     2u           /* stack_t.ss_flags: no altstack */

struct LinuxSigCopyPlan {
    uint64_t src;    /* child VA to copy from (args[1]) — 0 = nothing   */
    uint32_t len;    /* bytes to attach: 32 (act) or 8 (set); 0 = none;
                      * sigaltstack installs attach nothing           */
    int32_t  refuse; /* 0 = proceed; else the Linux errno to return
                      * BEFORE any post (first-trap refusal discipline)  */
};

/* Plan one sig-class byte crossing: EINVAL for a foreign sigsetsize,
 * -EFAULT for a span ring 3 could not read, else the act/set bytes (or
 * none). Total: any other number plans nothing and refuses nothing. */
struct LinuxSigCopyPlan linux_sig_copy_plan(uint64_t num,
                                            const uint64_t args[6]);

/* ─── E7 increment 12 (the reply payload, §10.13's step 3) — the copy-IN ───
 * The reply half of the fd class's byte crossing: increment 5 taught the
 * forward to carry bytes OUT of the child, this makes the reply carry
 * bytes BACK. §10.10's named gap — an oldact/oldset out-pointer the
 * caller would otherwise read stack garbage from — closes here.
 *
 * The split is the same split, mirrored. The FIRST trap validates the
 * CHILD's destination span and stages it (kernel/process.h's
 * reply_stage_* beside fs_base_stage): the copy-in aims where the first
 * trap said, never where the reply says — park_req cannot carry it (§10.7)
 * and a stale one must not aim a payload at a reused slot's successor
 * (cleared at the four places the contract names: top of every first-
 * trap block, consume at the deliver, refusal, slot reuse). The DELIVER
 * (kernel/chan.c) accepts exactly three shapes and copies only within
 * the staged clamp. The SIDECAR owns the answer's bytes (its 13/14/131 arms)
 * and its own bound over them (service() — payload length must equal the
 * forwarded out-pointer's clamp). Neither end alone is load-bearing.
 *
 * linux_user_copy_in is linux_user_copy_out's mirror over the same
 * four-level walk, and STRICTER where it must be: every page present,
 * USER and WRITABLE, in two phases (validate the whole span, then
 * write), allocating nothing. Writable, not merely user: copy-out only
 * needs readable, but copy-in must never write a byte into a page the
 * child itself could not have written — or the sidecar would launder
 * writes through the child's read-only pages. That asymmetry is the new
 * security property, and the refusals-before-mutation shape (a bad page
 * ANYWHERE in the span leaves every byte untouched) is its host test. */#define LINUX_READ  0u           /* the census's read row (§5.6) — the
                                 * count-truth number step 4 lands on
                                 * this same machinery                */

/* ─── E7 step 4 (the file class): the census's thirteen numbers ───────────
 * The numbers themselves, so the plan/attach/shape contracts below name
 * their own family instead of repeating literals (the sidecar mirrors
 * these as its own consts — guard clause P pins both halves). LSC_FD
 * rows in linux_syscall_table[] already classify every one of them, so
 * routing is the table's; what is NEW here is what each number's first
 * trap attaches and what its reply may carry back. */
#define LINUX_OPEN        2u
#define LINUX_OPENAT      257u   /* E7 TG-1 I4: openat — the gate's measured
                                    * path open. Path in args[1] (args[0] is
                                    * the dirfd); v1 serves AT_FDCWD only. */
#define LINUX_CLOSE       3u
#define LINUX_STAT        4u
#define LINUX_LSTAT       6u
#define LINUX_LSEEK       8u
#define LINUX_FCNTL       72u
#define LINUX_FSYNC       74u
#define LINUX_FTRUNCATE   77u
#define LINUX_RENAME      82u
#define LINUX_MKDIR       83u
#define LINUX_UNLINK      87u
#define LINUX_GETDENTS64  217u

/* Path bytes: the twin of FD_PAYLOAD_CAP, and the shim's NAMED
 * divergence from Linux's PATH_MAX 4096 — a longer path is refused by
 * name (first-trap -ENAMETOOLONG via linux_fd_path_plan), never
 * truncated, and the trigger to raise it is raising this cap and both
 * recv buffers together (§5.6's trigger rule). rename carries TWO paths
 * back to back in one payload, so the attach ceiling is two caps. */
#define LINUX_PATH_CAP        1024u
#define LINUX_FWD_ATTACH_CAP  (2u * (LINUX_PATH_CAP + 1u))

/* What a staged reply payload IS — the apply/deliver's dispatch over the
 * three reply shapes this increment adds to increment 12's copy-IN.
 * RAW is the existing rule (the payload bytes land at the staged
 * destination verbatim — read's data and the sig class's structs);
 * STAT serializes the sidecar's compact Stat into a full Linux
 * `struct stat`; DENTS packs the sidecar's name list into
 * `linux_dirent64` records the KERNEL lays out (d_reclen math stays
 * kernel-side — the sidecar never sees a Linux layout). */
#define LINUX_STAGE_NONE  0u
#define LINUX_STAGE_RAW   1u
#define LINUX_STAGE_STAT  2u
#define LINUX_STAGE_DENTS 3u
/* E7 increment 28 (P1-C's wire-up): tkill's reply is not owed to a pointer
 * in the task — it is the kernel's INSTRUCTION to build a frame. There is
 * no user destination (dst stays 0); the payload lands in the park-context
 * bridge below, and the write it causes goes through linux_sig_deliver. */
#define LINUX_STAGE_SIGFRAME 4u

/* `struct stat`, musl x86-64 (144 bytes): field offsets the golden test
 * pins and the probe reads back on-target (§5.6). The sidecar's payload
 * is a COMPACT Stat (mode u16, uid u16, gid u16, size u64, mtime u32 —
 * 24 bytes, zero-padded) and linux_stat_bytes below is the only place
 * the two layouts meet. */
#define LINUX_STAT_BYTES         144u
#define LINUX_STAT_PAYLOAD_BYTES 24u
#define LINUX_STAT_OFF_MODE      24u   /* u32 st_mode */
#define LINUX_STAT_OFF_UID       28u   /* u32 st_uid  */
#define LINUX_STAT_OFF_GID       32u   /* u32 st_gid  */
#define LINUX_STAT_OFF_SIZE      48u   /* i64 st_size */
#define LINUX_STAT_OFF_MTIME     88u   /* i64 st_mtim.tv_sec; nsec at +96 */

/* getdents64's two bounds: the sidecar may attach at most
 * LINUX_DENTS_PAYLOAD_CAP bytes of name list, and the KERNEL packs at
 * most LINUX_DIRENT_USER_CAP bytes of records into the child's buffer
 * (a short getdents is POSIX-legal — the caller loops; the census asks
 * for 2048). `struct linux_dirent64` is musl's: d_ino u64, d_off i64,
 * d_reclen u16 (8-aligned: align8(19 + strlen + 1)), d_type u8, d_name. */
#define LINUX_DENTS_PAYLOAD_CAP 1024u
#define LINUX_DIRENT_USER_CAP   2048u
#define LINUX_DIRENT_MIN_RECLEN 24u   /* "." — the smallest record */

struct LinuxFdReplyPlan {
    uint64_t dst;      /* child VA to copy the reply payload into
                        * (args[2] for the sig class, args[1] for read,
                        * stat/lstat and getdents64)                    */
    uint32_t len;      /* the payload CLAMP: bytes the sidecar may
                        * attach behind the reply header (32/8 for the
                        * sig class; LINUX_STAT_PAYLOAD_BYTES for stat;
                        * LINUX_DENTS_PAYLOAD_CAP for getdents64; for
                        * read, min(count, FD_PAYLOAD_CAP)); 0 = no
                        * payload is owed back                          */
    uint32_t aux;      /* the CHILD-side clamp: bytes the deliver may
                        * write at dst (LINUX_STAT_BYTES for stat — the
                        * serialized form is larger than the payload;
                        * min(count, LINUX_DIRENT_USER_CAP) for
                        * getdents64; == len for the RAW family)        */
    uint8_t  kind;     /* LINUX_STAGE_*: what the deliver does with the
                        * payload before the copy-in                   */
    int32_t  refuse;   /* 0 = proceed; else the Linux errno to return
                        * BEFORE any post (first-trap refusal discipline)  */
};

/* The path plan (the forward's second shape): open/stat/lstat/unlink/
 * mkdir carry ONE pathname out of the child (args[0]), rename TWO
 * (args[0], args[1]). The plan is a pure CLASSIFIER — which registers
 * name paths — because measuring a string touches the child's bytes and
 * needs a cr3. linux_path_measure (below) does the measuring at the
 * attach, still first-trap: a span ring 3 could not read is
 * -LINUX_EFAULT, a string with no NUL within LINUX_PATH_CAP is
 * -LINUX_ENAMETOOLONG — both before any post. Total over (num, args). */
struct LinuxFdPathPlan {
    uint64_t src;      /* args[0] — the first path's child VA (0 = none) */
    uint64_t src2;     /* args[1] — rename's second path (0 otherwise)   */
    uint8_t  two;      /* 1 = this number carries two paths              */
    int32_t  refuse;   /* reserved: the classifier touches no bytes, so
                        * today always 0 — the refusals are measure's   */
};

struct LinuxFdPathPlan linux_fd_path_plan(uint64_t num,
                                          const uint64_t args[6]);

/* Measure one path string in the child's memory (the attach's own
 * walker rules — present, user, never faulting): returns 0 with the
 * length INCLUDING the NUL, -LINUX_EFAULT for a span that cannot be
 * read, -LINUX_ENAMETOOLONG when no NUL arrives within `cap`. A local
 * byte, no buffer — the frame cost is a word regardless of cap. */
int linux_path_measure(uint64_t root, uint64_t va, uint32_t cap,
                       uint32_t *out_len);

/* Plan one reply payload's destination span — the write plan's twin over
 * the IN direction. For a copy-IN number with a NULL out-pointer the
 * plan is empty (no stage, no refusal: nothing is owed back); with a
 * pointer, the (dst, len) span must be user-canonical and inside the
 * user range — the same (span_end - 1) > MAX shape the write plan's
 * host tests pin — else -LINUX_EFAULT at the first trap. Pure: any other
 * number plans nothing and refuses nothing (total). */
struct LinuxFdReplyPlan linux_fd_reply_plan(uint64_t num,
                                            const uint64_t args[6]);

/* Copy n bytes into a task's user VA from src, walking root's page
 * tables. Two phases: EVERY page of the span is first validated (present,
 * USER, writable, no huge page, level by level), then the bytes are
 * written — a refusal in phase one mutates NOTHING (the host test pins
 * bytes-unchanged). Returns 0 or -LINUX_EFAULT, never a fault: the
 * identity map is what lets a translated frame be written directly, and
 * n == 0 succeeds without touching anything (root may be 0). src must
 * hold n bytes; the caller owns it. */
int linux_user_copy_in(const uint8_t *src, uint64_t root, uint64_t va,
                       uint32_t n);

/* Is this reply payload's `ret` the right shape for `num`? The one-truth
 * rule applied to the IN direction: a count-truth number (LINUX_READ —
 * read's return IS the byte count) must echo n in ret, every other
 * copy-IN number this seam knows (rt_sigaction/rt_sigprocmask) returns
 * 0 on success, so its ret must be 0. A payload whose ret contradicts
 * the number is -EPROTO at the deliver, before anything is copied — the
 * same 'header, payload and rax are one truth' rule increment 5 set on
 * the OUT side. Total over (num, ret, n); a number with no reply plan
 * never reaches it (the deliver refuses by stage first). A future
 * copy-IN number whose Linux return is neither 0 nor its count grows
 * this predicate with its own arm — never a default that guesses. */
int linux_reply_shape_ok(uint64_t num, uint64_t ret, uint32_t n);

/* May a SUCCESS reply for `num` carry ZERO payload bytes? Only for the
 * count-truth numbers, whose zero IS the answer: read at end-of-file and
 * getdents64 at end-of-directory. A staged request for any other number
 * owes a fixed struct, so a 0-byte success there is the protocol break
 * chan.c's shape-1 rule refuses. Total over num. */
int linux_reply_zero_ok(uint64_t num);

/* Serialize the sidecar's compact Stat payload (LINUX_STAT_PAYLOAD_BYTES,
 * little-endian: mode u16 @0, uid u16 @2, gid u16 @4, size u64 @8,
 * mtime u32 @16) into musl's 144-byte `struct stat` — the one place the
 * two layouts meet. Fields the VFS does not carry are filled HONESTLY,
 * never invented: st_ino 0 (the vfs::Stat carries no inode), st_nlink 1,
 * st_dev/st_rdev 0, st_blksize 4096, st_blocks derived from size,
 * atim/ctim echoing mtime (one timestamp is the truth; two more would
 * be fabrications). dst must hold LINUX_STAT_BYTES; src must hold
 * LINUX_STAT_PAYLOAD_BYTES. The deliver calls it; the golden host test
 * pins every offset. */
void linux_stat_bytes(const uint8_t *src, uint8_t *dst);

/* Pack the sidecar's compact directory-entry list into the child's
 * `linux_dirent64` stream. The payload (n bytes from src) is:
 *   u32 count; then per entry: u64 ino, u8 ty, u16 name_len, name bytes.
 * Records are laid out HERE — d_reclen = align8(19 + name_len + 1),
 * d_off the entry's own cookie (index + 1) — starting at the entry the
 * staged cursor (dir_pos) names, and packing stops before the first
 * record that would exceed count_cap bytes (aux: the child's buffer
 * size). Returns the byte count packed into dst and writes the number
 * of entries CONSUMED to *fitted (what the cursor must advance by —
 * only what was packed); -1 on a malformed payload (count/name_len
 * crossing n). A listing with nothing left to give packs 0 bytes, 0
 * entries — getdents64's end-of-directory. */
int linux_dirent_pack(const uint8_t *src, uint32_t n, uint32_t count_cap,
                      uint64_t dir_pos, uint8_t *dst, uint32_t *fitted);

/* ─── The errno map (§5.7) ─────────────────────────────────────────────────
 * Two families cross into the Linux task as negative rax: the Phase-3
 * CAP_E* syscalls return (-1)..(-12), and the Phase-5 k_chan_ path returns
 * positive CAP_ERR_* codes that the forward/reply path may surface. Both
 * are total-function inputs here, and the CAP_NONE (0xFFFF) out-param
 * sentinel is translated to -EIO: a sentinel is a kernel-side answer that
 * was not an answer, and leaking it as a return value would put a value
 * no errno can produce into a tenant binary's error path (§5.7).
 *
 * The reverse map is defined ONLY where a Linux errno has exactly one
 * preimage; where several CAP codes collapse onto one errno (ENOMEM from
 * CAP_ENOMEM/CAP_ERR_BUDGET/CAP_ERR_NOMEM; EIO; EBUSY; EINVAL; EPERM),
 * the reverse returns 0 — "not defined" — rather than guessing which
 * kernel-side truth the caller meant. The host test round-trips exactly
 * the codes whose reverse IS defined. */
int32_t linux_errno_from_cap(int32_t cap_code);
int32_t cap_code_from_linux_errno(int32_t linux_errno); /* 0 = no unique preimage */

/* ─── The initial stack and auxv (§5.2) ───────────────────────────────────
 * A static musl/glibc start-up reads argc/argv/envp/auxv off the stack the
 * KERNEL builds; the native sidecar path enters _start with nothing, so
 * this is the one place the image contract becomes bytes. The required
 * AT_* set is §5.2's list, and its most important entry is absent:
 * AT_SYSINFO_EHDR is never written, because there is no vDSO here, which
 * is what makes clock_gettime (228) a real trap on AeroSLS and keeps the
 * census's vDSO declaration (§5.5) honest at the source rather than at
 * the trace. */

/* Owned AT_* values (linux/auxvec.h is not includable; these are ABI). */
#define LINUX_AT_NULL         0
#define LINUX_AT_PHDR         3
#define LINUX_AT_PHENT        4
#define LINUX_AT_PHNUM        5
#define LINUX_AT_PAGESZ       6
#define LINUX_AT_BASE         7
#define LINUX_AT_ENTRY        9
#define LINUX_AT_PLATFORM    15
#define LINUX_AT_HWCAP       16
#define LINUX_AT_CLKTCK      17
#define LINUX_AT_SECURE      23
#define LINUX_AT_RANDOM      25
#define LINUX_AT_HWCAP2      26
#define LINUX_AT_EXECFN      31
/* LINUX_AT_SYSINFO_EHDR (33) is deliberately not written; see §5.2. */

#define LINUX_AT_RANDOM_BYTES 16
#define LINUX_PAGE_SIZE       4096u
#define LINUX_CLKTCK          100u
#define LINUX_ELF64_PHENT     56u   /* sizeof(Elf64_Phdr); AT_PHENT's value */

struct LinuxInitStackParams {
    uint64_t       region_vaddr;  /* E7 increment 7: the vaddr this region is
                                   * mapped at in the CHILD's page table.
                                   * Every in-region POINTER this builder
                                   * writes (the argv/envp slots and the
                                   * AT_RANDOM/AT_EXECFN/AT_PLATFORM values)
                                   * and the returned %rsp are offsets into
                                   * the region translated to this base — a
                                   * Linux child dereferences them in its OWN
                                   * address space, where the region lives at
                                   * stack_base, not at the host/phys address
                                   * the builder writes through. A host test
                                   * passes the region's own address
                                   * (identity) and gets byte-identical
                                   * images; the create passes the stack's
                                   * user vaddr. The untranslated form ran
                                   * the .flat probe (which never reads %rsp)
                                   * and was caught by increment 7's
                                   * measurement of the real candidate. */
    uint64_t       entry_vaddr;   /* AT_ENTRY: e_entry */
    uint64_t       phdr_vaddr;    /* AT_PHDR: vaddr of the program headers */
    uint16_t       phent;         /* AT_PHENT: LINUX_ELF64_PHENT */
    uint16_t       phnum;         /* AT_PHNUM */
    const char *const *argv;      /* NULL-terminated array of NUL-terminated strings */
    const char *const *envp;      /* NULL-terminated array of NUL-terminated strings */
    const uint8_t *at_random;     /* LINUX_AT_RANDOM_BYTES bytes from kernel/entropy.c;
                                   * required — absent, stack canaries are worthless */
    const char    *execfn;        /* AT_EXECFN: the pathname string, NUL-terminated */
};

/*
 * Build the entry stack image into [region, region + region_bytes) and
 * return the entry %rsp (16-byte aligned per the x86-64 psABI), or 0 if
 * the image would not fit. argv/envp/execfn/platform strings and the
 * AT_RANDOM bytes are copied into the region; the returned rsp points at
 * argc, above which sit the argv pointers, a NULL, the envp pointers, a
 * NULL, and the auxv pairs ending in AT_NULL. The caller owns the region
 * (it will be a partition-charged stack allocation, §5.2); this function
 * owns its layout.
 */
uint64_t linux_initial_stack_build(uint8_t *region, size_t region_bytes,
                                   const struct LinuxInitStackParams *p);

/* ─── Q2: the stack high-water scan (E7's gate measurement) ───────────────
 * The gate profile fills the unused span below the entry %rsp with
 * LINUX_STACK_POISON at create, then scans it at the stop and at teardown:
 * the leading run still holding the poison is the headroom the workload
 * left. Pure over the bytes, so the host test can bite without a boot; the
 * kernel passes the stack's contiguous physical base, which is what makes
 * the region scannable at all (§5.2's single-span requirement, used for a
 * measurement rather than only for the initial image). */
#define LINUX_STACK_POISON  0xA5u
size_t linux_stack_hwm_headroom(const uint8_t *region, size_t bytes,
                                uint8_t poison);

/* ─── The personality (E7 increment 2, §3.3/§3.5) ─────────────────────────
 * A task's personality decides what its `rax` MEANS at the trap, and it is
 * explicit at spawn — never inferred from bytes (§5.1) — so the values
 * live here, with the rest of E7's contracts, and struct ProcessDescriptor
 * carries the field (kernel/process.h). PROC_PERSONALITY_SLS is 0: a
 * zeroed slot — every slot at boot, every slot before its stamp — is the
 * SLS personality, so the safe default is the one BSS already provides
 * and a stale slot is the failure proc_clear_resume_state() refuses. */
#define PROC_PERSONALITY_SLS    0u   /* the existing SLS ring-3 ABI (default) */
#define PROC_PERSONALITY_LINUX  1u   /* Linux x86-64 ABI: rax = Linux number */

/* 1 iff a task with this personality takes the Linux branch (describe and
 * forward) rather than the number chains. The whole number-space safety
 * property (§2.2) rests on this predicate being consulted before any
 * `case`/`cmp`: a Linux task's rax = 105 is setuid, not SYS_SLS_ALLOCATE. */
int linux_personality_intercepts(uint32_t personality);

/* ─── The forwarded request (§3.4) ────────────────────────────────────────
 * A DESCRIPTION, not an interpretation: the kernel knows the Linux
 * argument POSITIONS (ABI, not policy) and nothing about what `num` means.
 * Fixed-size so it crosses a channel unchanged; the six values are the
 * ones the trap path captured to the per-CPU sibling block before any
 * call (arch/x86/syscall.asm's linux_args[6], pinned at [gs:0x50..0x80)). */
struct SLSLinuxSyscallRequest {
    uint64_t num;        /* the Linux syscall number, as it arrived in rax */
    uint64_t args[6];    /* rdi, rsi, rdx, r10, r8, r9 — capture order */
    uint64_t ret;        /* the sidecar's return value — filled by the reply
                          * (linux_syscall_apply_reply, increment 3) */
    int32_t  status;     /* the sidecar's CAP_ERR_* or CAP_E* status, mapped
                          * to a Linux errno on the return path (§5.7); 0 = the
                          * reply carries a real return value in ret */
};

/* Build the request from the captured values. Pure: ret/status are zeroed,
 * so what a caller reads before an answer exists is defined, not stale. */
void linux_syscall_describe(struct SLSLinuxSyscallRequest *req,
                            uint64_t num, const uint64_t args[6]);

/* ─── The park resume (E7 increment 3, §3.3 steps 5-7) ──────────────────────
 * A forwarded syscall's reply can only be read by the park's resume re-run,
 * so "a forward is in flight" is a first-class state, not an inference:
 * park_syscall == SYS_LINUX_TRAP_RESUME is armed BEFORE the post (it is also
 * what stops cap_maybe_handoff from sysret-returning the half-finished
 * syscall — kernel/process.c) and cleared the moment the reply is applied
 * (or refused). A ring-3 trap carrying the sentinel number while NOT in
 * flight is a stray and is refused by name — it never parks, because there
 * is no reply to wait for and a parked stray would wedge into a task slot.
 *
 * The value is an ASCII "LINR" in a u32 (park_syscall is uint32_t): far
 * outside Linux's errno/number space, outside every SYS_SLS_* number (the
 * guard's M1 clause proves that against the whole number space), and outside
 * the census's 27 — linux_syscall_class() answers UNKNOWN for it, so it can
 * never be mistaken for a real syscall's description. */
#define SYS_LINUX_TRAP_RESUME  0x4C494E52u /* "LINR" */
/* E7 TG-1 I4: park_syscall while a thread waits for its GROUP's forward
 * lease. The waiter is not owed a reply yet: the owner's release grants the
 * lease and restarts the caller's `syscall` instruction (rip -= 2), so the
 * first trap runs again with the same registers and now owns the lease.
 * Distinct from LINR because cap_maybe_handoff and the resume arm must not
 * treat this park as an in-flight forward. */
#define SYS_LINUX_LEASE_RESUME 0x4C4C5345u /* "LLSE" */

/* 1 iff a trap at the resume sentinel may be treated as the resume re-run of
 * an in-flight forward: the personality takes the branch AND the arm is set.
 * Refusing anything else is what keeps the deliver loop from being entered
 * by a stray raw trap (which would then poll a channel nothing is owed on). */
int linux_forward_resume_is_live(uint32_t personality, uint32_t park_syscall);

/* §3.5's capability posture as a pure rule: a Linux task's table holds the
 * channel to its environment's sidecar and nothing else — exactly ONE
 * sendable CHAN_W and exactly ONE recv-able CHAN_R. Counted over usable
 * handles (SEND / RECV permissions, same requirements cap_send_msg and
 * cap_recv_msg enforce); any other count — a missing channel, two
 * candidates — is refused rather than guessed between. cap_linux_find_channels
 * (kernel/cap.c) scans the live table against this rule. */
int linux_channel_shape_ok(int n_chan_w, int n_chan_r);

/* Apply a sidecar reply to the in-flight request and return the Linux rax:
 * the payload is THIS struct with num echoed and ret/status filled. Pure and
 * total — refuses a wrong size (-EPROTO: the reply is not an answer, so
 * nothing in it may be trusted), enforces the num echo (-EPROTO: a reply for
 * a different syscall must never produce this one's rax), passes ret through
 * on status 0, and maps any nonzero status through the §5.7 errno map (so a
 * CAP code reaches the task as a Linux errno and never as itself; the
 * total map also covers whatever out-of-family value a buggy sidecar wrote).
 * The caller's `req` holds the EXPECTED num on entry; on refusal its contents
 * are the (rejected) reply's — the caller returns the errno and never reads
 * req again, which is what makes the byte copy safe to do before the check. */
uint64_t linux_syscall_apply_reply(struct SLSLinuxSyscallRequest *req,
                                   const void *payload, uint64_t payload_len);

/* ─── E7 increment 26 (P1-C, "the frame") — the signal frame's ABI ────────
 * Increment 10's seam records handler dispositions and refuses to run them:
 * there is no frame to run a handler IN and no rt_sigreturn to come back
 * through. This increment lands the frame itself as a PURE function — the
 * bytes and the two decisions (deliver? interrupt?) — with the user-stack
 * write, the return-to-user hook and the sidecar's EINTR answer left to
 * increment 27. The split is the repo's: a wrong number here can only be
 * caught by a host test, so the numbers live where a host test can see
 * them, and no delivery path exists yet that could disagree.
 *
 * The layout is the x86-64 kernel's own `struct rt_sigframe`
 * (arch/x86/include/asm/ucontext.h + asm/sigcontext.h), because the thing
 * that reads it is the GUEST's libc handler trampoline, not this kernel:
 *
 *   0    pretcode        the restorer the caller installed (sa_restorer)
 *                        — a handler with no restorer cannot return, and a
 *                        frame built with a zero here would `ret` to NULL
 *   8    uc_flags
 *   16   uc_link
 *   24   uc_stack.ss_sp / 32 .ss_flags (+pad) / 40 .ss_size
 *   48   uc_mcontext     struct sigcontext, 256 bytes (see below)
 *   304  uc_sigmask      the kernel-ABI sigset: 8 bytes, 64 signals
 *   312  siginfo         128 bytes; si_signo/errno/code, then si_addr at 16
 *   440  (end — no fpstate; see the named limit below)
 *
 * `struct sigcontext`, at MCONTEXT:
 *   +0    r8 r9 r10 r11 r12 r13 r14 r15     (the kernel's order, not the
 *   +64   rdi rsi rbp rbx rdx rax rcx rsp rip   SYSCALL capture's order)
 *   +136  eflags
 *   +144  cs (u16), gs, fs, pad0
 *   +152  err, trapno, oldmask, cr2
 *   +184  fpstate pointer (0 here)
 *   +192  reserved1[8]
 *
 * ─── What this increment deliberately does NOT carry ─────────────────────
 *   - **No fpstate.** The pointer is NULL and rt_sigreturn does not touch
 *     the FPU, so a handler that clobbers xmm keeps its clobbering. That is
 *     a divergence from Linux, named here so nobody reads the frame as
 *     complete: Go's async preemption (P1-D) needs a real fpstate and the
 *     `save`/`restore` around the handler before its gate can be green.
 *   - **No delivery.** `linux_sig_frame_build` writes bytes into a caller
 *     buffer; nothing here writes a user stack, touches CR2, or hooks
 *     return-to-user. That hook, the rt_sigreturn TABLE row (15) and the
 *     park-interrupt are increment 27 — and the table row waits for a real
 *     census run, because §5.6's rule is that the table is BUILT from the
 *     trace, never from a number somebody believed.
 *
 * The EINTR half is a predicate, not a path: `linux_sig_park_is_eintr`
 * answers Linux's own question (is this signal, for this disposition,
 * allowed to interrupt a blocked syscall?) so increment 27's park flag has
 * one named answer to consult instead of an inline condition.
 *
 * Pure: no allocation, no user-memory access, no globals — the host test
 * drives every branch (tests/linux_sigframe_host_test.c), and the guard's
 * clause P pins the layout literals it holds. */
#define LINUX_RT_SIGRETURN      15u   /* named here; the TABLE row is
                                       * increment 27's, after the census */

/* The frame, at the offsets above. Pinned twice: as these literals (read by
 * the host test) and as _Static_asserts in linux_abi.c (so a compiler that
 * disagrees stops the build, not the boot). */
#define LINUX_SIGFRAME_PRETCODE    0u
#define LINUX_SIGFRAME_UC         8u
#define LINUX_SIGFRAME_UC_FLAGS   8u
#define LINUX_SIGFRAME_UC_LINK    16u
#define LINUX_SIGFRAME_UC_STACK   24u
#define LINUX_SIGFRAME_MCONTEXT   48u
#define LINUX_SIGFRAME_SIGMASK   304u
#define LINUX_SIGFRAME_INFO      312u
#define LINUX_SIGFRAME_SIZE      440u
#define LINUX_SIGFRAME_MCONTEXT_BYTES 256u
#define LINUX_SIGINFO_BYTES      128u

/* sigcontext-internal offsets, relative to LINUX_SIGFRAME_MCONTEXT. */
#define LINUX_SIGCTX_EFLAGS      136u
#define LINUX_SIGCTX_CS          144u
#define LINUX_SIGCTX_GS          146u
#define LINUX_SIGCTX_FS          148u
#define LINUX_SIGCTX_ERR         152u
#define LINUX_SIGCTX_TRAPNO      160u
#define LINUX_SIGCTX_OLDMASK     168u
#define LINUX_SIGCTX_CR2         176u
#define LINUX_SIGCTX_FPSTATE     184u
#define LINUX_SIGCTX_RESERVED    192u

/* The ring-3 code selector a frame's cs must carry: a frame whose cs is
 * anything else was not built for a user handler, so restore refuses it. */
#define LINUX_SIGFRAME_USER_CS   0x33u

/* The fault-class signal numbers (Linux x86-64 UAPI) and the two si_codes a
 * #PF picks between: an access to an unmapped page is MAPERR, an access that
 * protection refused is ACCERR — the error code's P bit is the whole
 * difference, and a crash reporter that reads si_code distinguishes them. */
#define LINUX_SIGILL       4u
#define LINUX_SIGBUS       7u
#define LINUX_SIGFPE       8u
#define LINUX_SIGSEGV      11u
#define LINUX_SEGV_MAPERR  1u
#define LINUX_SEGV_ACCERR  2u

/* sa_flags bits this increment reasons about. Values are Linux's. */
#define LINUX_SA_SIGINFO   0x00000004ull
#define LINUX_SA_ONSTACK   0x08000000ull
#define LINUX_SA_RESTART   0x10000000ull
#define LINUX_SA_NODEFER   0x40000000ull
#define LINUX_SS_ONSTACK   1u

/* The interrupted context as the frame carries it: the kernel's gregs order.
 * Named indexes, because "which slot is rip" is the kind of fact a wrong
 * number in an array literal gets wrong silently. */
enum LinuxSigReg {
    LINUX_SIGREG_R8 = 0, LINUX_SIGREG_R9, LINUX_SIGREG_R10, LINUX_SIGREG_R11,
    LINUX_SIGREG_R12, LINUX_SIGREG_R13, LINUX_SIGREG_R14, LINUX_SIGREG_R15,
    LINUX_SIGREG_RDI, LINUX_SIGREG_RSI, LINUX_SIGREG_RBP, LINUX_SIGREG_RBX,
    LINUX_SIGREG_RDX, LINUX_SIGREG_RAX, LINUX_SIGREG_RCX, LINUX_SIGREG_RSP,
    LINUX_SIGREG_RIP,
    LINUX_SIGREG_COUNT
};

struct LinuxSigRegs {
    uint64_t r[LINUX_SIGREG_COUNT];   /* gregs, in the kernel's order */
    uint64_t eflags;
};

/* What a delivery has to decide before a frame exists. `handler`/`restorer`
 * are act words 0 and 3 verbatim; `act_*` are words 1 and 2. */
struct LinuxSigFrameSpec {
    uint32_t signo;        /* 1..=64 */
    uint32_t code;         /* si_code (SI_USER 0, SEGV_MAPERR 1, ...) */
    uint64_t addr;         /* si_addr; 0 when the signal carries none */
    uint64_t handler;      /* sa_handler — an address, never 0/1 here */
    uint64_t restorer;     /* sa_restorer — the frame's pretcode */
    uint64_t act_flags;    /* sa_flags */
    uint64_t act_mask;     /* sa_mask, added to the handler's mask */
    uint64_t blocked;      /* the thread's own mask before delivery */
    uint64_t user_rsp;     /* the interrupted rsp */
    uint64_t alt_sp;       /* sigaltstack.ss_sp, 0 = none installed */
    uint64_t alt_size;     /* sigaltstack.ss_size */
    uint32_t alt_flags;    /* sigaltstack.ss_flags */
    uint32_t siginfo;      /* 1 = SA_SIGINFO: the handler takes 3 args */
    struct LinuxSigRegs regs;   /* the interrupted context */
};

/* Build the frame for `spec` into `buf` (the caller's user-stack staging
 * buffer — this function cannot touch a user page). On success returns 0,
 * writes the handler's ENTRY context to `entry` (rsp aims at the frame,
 * rip at the handler, rdi/rsi/rdx per SA_SIGINFO) and the mask to install
 * to `entry_mask`. Refusals are -LINUX_EINVAL, all first-trap style: a cap
 * below LINUX_SIGFRAME_SIZE, a signo outside 1..64, a handler that is
 * SIG_DFL/SIG_IGN (no frame can run those), a zero restorer (nothing to
 * return through), or a stack with no room for the frame. */
int linux_sig_frame_build(uint8_t *buf, uint32_t cap,
                          const struct LinuxSigFrameSpec *spec,
                          struct LinuxSigRegs *entry, uint64_t *entry_mask);

/* The inverse: read a built frame back out. Refuses -LINUX_EINVAL on a short
 * buffer, a NULL argument, or a cs that is not LINUX_SIGFRAME_USER_CS — the
 * one field a forged frame cannot get right without building it. `mask` may
 * be NULL (a caller that only wants the registers). */
int linux_sig_frame_restore(const uint8_t *buf, uint32_t len,
                            struct LinuxSigRegs *regs, uint64_t *mask);

/* Linux's default action per signal, so "terminate" is a table rather than
 * an else-branch: 0..=64 total, 0 for a number outside the range (no
 * action exists to take). */
enum LinuxSigDefault {
    LINUX_SIGDEF_NONE = 0,      /* not a signal number              */
    LINUX_SIGDEF_IGNORE,        /* discarded                        */
    LINUX_SIGDEF_TERMINATE,     /* kills the group                  */
    LINUX_SIGDEF_CORE,          /* kills, and Linux would core-dump */
    LINUX_SIGDEF_STOP,          /* stops                            */
    LINUX_SIGDEF_CONTINUE       /* SIGCONT resumes a stopped task   */
};
int linux_sig_default_action(uint32_t signo);

/* Does this signal need a frame? 1 only for an unmasked number with a real
 * handler installed: SIG_DFL/SIG_IGN take the action/ignore path, a blocked
 * number stays pending, and a number outside 1..=64 is never delivered. */
int linux_sig_delivery_needed(uint64_t blocked, uint32_t signo,
                              uint64_t handler);

/* Linux's EINTR rule, as one predicate: an installed handler, not blocked,
 * and no SA_RESTART means a blocked syscall returns -EINTR instead of being
 * restarted. Increment 27's park flag consults this; the sidecar's answer
 * and this predicate must agree, which is why it is not an inline test. */
int linux_sig_park_is_eintr(uint32_t signo, uint64_t blocked,
                            uint64_t handler, uint64_t act_flags);

/* ─── E7 increment 27 (P1-C, "the delivery") — the hook's contract ────────
 * Increment 26 landed the frame as bytes. This is the half that puts those
 * bytes where a handler can run them, and it keeps exactly the split §3.3
 * built the seam on:
 *
 *   the SIDECAR decides  (which signal, which handler, which flags — its
 *                        SigState is the disposition authority, and the
 *                        reply carries the words verbatim)
 *   the KERNEL performs (the frame is built here, written into the task's
 *                        own stack through the validated writable copy —
 *                        the same walker the fd class uses — and the
 *                        handler's entry context is handed back)
 *
 * Nothing here reads the guest's memory for a decision: the request is a
 * fixed 88-byte struct of act words and stack bounds, refused by length or
 * by a handler word that is not an address. The copy is two-phase, so a
 * stack the task made read-only refuses with -EFAULT and leaves every byte
 * of the guest's stack untouched — a signal must never be the thing that
 * faults in ring 0.
 *
 * Still absent, deliberately: rt_sigreturn's table row (waiting on a real
 * census trace) and any fpstate (increment 26's named limit stands). */

/* The sidecar's answer to "run this handler": eleven words, little-endian
 * on the wire, all of them act/stack facts this kernel re-checks. */
struct LinuxSigDeliverReq {
    uint32_t signo;      /* 1..=64 */
    uint32_t code;       /* si_code the sidecar chose (SI_USER 0, ...) */
    uint64_t addr;       /* si_addr for a non-fault signal, 0 normally */
    uint64_t handler;    /* sa_handler */
    uint64_t restorer;   /* sa_restorer — the frame's pretcode */
    uint64_t act_flags;  /* sa_flags */
    uint64_t act_mask;   /* sa_mask */
    uint64_t blocked;    /* the thread's mask, as the sidecar knows it */
    uint64_t alt_sp;     /* sigaltstack.ss_sp, 0 = none */
    uint64_t alt_size;
    uint32_t alt_flags;
    uint32_t siginfo;    /* 1 = SA_SIGINFO */
};
#define LINUX_SIGDELIVER_BYTES 80u   /* 2 x u32 + 8 x u64 + 2 x u32 */
_Static_assert(sizeof(struct LinuxSigDeliverReq) == LINUX_SIGDELIVER_BYTES,
               "the request's wire length is its struct length, so a decoder on one side and a struct on the other cannot differ by padding");

/* Read the request out of a reply payload. Refuses -LINUX_EINVAL on a length
 * that is not exactly LINUX_SIGDELIVER_BYTES (a truncated or padded answer
 * is not an answer) and on a handler/restorer pair that could not run. */
int linux_sig_deliver_req_parse(const uint8_t *bytes, uint32_t len,
                                struct LinuxSigDeliverReq *out);

/* Linux's si_addr for a fault, from the two numbers the trap path has:
 *   SIGSEGV / SIGBUS  -> CR2   (the faulting ADDRESS)
 *   SIGFPE  / SIGILL  -> RIP   (the faulting INSTRUCTION; CR2 is stale)
 *   anything else     -> no address (returns 0, and si_addr is then the
 *                        sidecar's own `addr` for a software-sent signal)
 * Returns 1 when it wrote an address, 0 when the signal carries none — so
 * a NULL-deref SIGSEGV's cr2 == 0 is still "the address" rather than being
 * mistaken for "no answer". A crash reporter that reads si_addr is the
 * reason this is its own function rather than a field filled in by habit. */
int linux_sig_si_addr(uint32_t signo, uint64_t cr2, uint64_t rip, uint64_t *out);

/* THE HOOK'S CORE (no registers are touched here — the caller applies the
 * returned context when it next returns to ring 3):
 *   build the frame from `req` + the interrupted `ctx`
 *   write it to the task's stack at the rsp the builder chose, through
 *          linux_user_copy_in against `root` (the task's own cr3)
 *   return the handler's entry context and the mask to install
 * Refusals, all before any byte is written: a frame the builder refuses
 * (-EINVAL), or a stack span ring 3 could not write (-EFAULT). */
int linux_sig_deliver(uint64_t root, const struct LinuxSigDeliverReq *req,
                      const struct LinuxSigRegs *ctx, uint64_t cr2,
                      struct LinuxSigRegs *entry, uint64_t *entry_mask);

/* ─── E7 increment 31 (P1-C): the FAULT's own delivery ─────────────────────
 * A ring-3 #PF is the one signal source with no syscall behind it: by the
 * time it fires, the thread is not parked on a forward, so the sidecar —
 * which owns the dispositions — cannot be asked before the thread must
 * either run a handler or die. The kernel therefore keeps its OWN copy of
 * the act for the four fault signals (SIGILL 4, SIGBUS 7, SIGFPE 8,
 * SIGSEGV 11), recorded from the same 32 bytes the guest installs at
 * rt_sigaction's first trap, and builds the frame itself. The sidecar stays
 * the authority for every signal a syscall can reach; this copy is exactly
 * one class narrower, and both are written from one struct.
 *
 * What the copy does NOT cover, by name: the mask (the kernel's
 * linux_sig_mask tracks DELIVERIES, so a guest that blocks SIGSEGV after a
 * delivery is not seen here), the altstack (v1 records none), and the
 * sidecar's own bookkeeping — a fault-delivered handler cannot return
 * through rt_sigreturn, because the sidecar never entered it (§10.26). A
 * handler that longjmp's or exits is the shape this serves.
 */
#define LINUX_FAULT_SIG_COUNT 4u

struct LinuxFaultAct {
    uint64_t handler;    /* act word 0, as installed (0 = SIG_DFL, 1 = IGN) */
    uint64_t flags;      /* word 1                                   */
    uint64_t mask;       /* word 2                                   */
    uint64_t restorer;   /* word 3                                   */
};

/* Index of `signo` in the fault table, or -1 when it is not a fault class:
 * a signal with no trap behind it must not be deliverable from one. */
int linux_fault_act_index(uint32_t signo);

/* Record one act from the 32 bytes the guest installed (kernel-ABI order:
 * handler, flags, restorer, mask). Returns 1 when `signo` is a fault class
 * and the slot was written, 0 otherwise — a number outside the table is not
 * an error, it is simply not this path's business. */
int linux_fault_act_record(struct LinuxFaultAct *slots, uint32_t signo,
                           const uint8_t act[LINUX_SIGACT_BYTES]);

/* Decide and build, touching no user page: on 1, `frame` holds the
 * rt_sigframe to copy to entry->r[LINUX_SIGREG_RSP], `entry` is the
 * handler's context (the fault stub loads it and iretq's) and `entry_mask`
 * the mask to install. On 0 there is no handler to run — SIG_DFL/SIG_IGN, a
 * blocked signal, or no restorer — and the caller keeps the attributed
 * kill. Negative is a builder refusal, treated the same way: no frame, no
 * delivery. `cr2` is what the #PF captured; si_addr comes from
 * linux_sig_si_addr, so SIGSEGV names the ADDRESS and SIGILL the
 * instruction. */
int linux_fault_sig_plan(const struct LinuxFaultAct *act, uint64_t blocked,
                         uint32_t signo, uint32_t code, uint64_t cr2,
                         const struct LinuxSigRegs *ctx,
                         uint8_t frame[LINUX_SIGFRAME_SIZE],
                         struct LinuxSigRegs *entry, uint64_t *entry_mask);

/* ─── E7 increment 28 (P1-C's wire-up) — the park-context bridge ───────────
 * A delivery replaces the RETURN of the parked forward, and the only place
 * the kernel keeps a return target is the park context: process.c's
 * CapParkCtx, whose word layout cap_recv_resume's asm reads by offset
 * (0 = r11/user RFLAGS, 1 = rcx/user RIP, 2..7 = r15..rbp, 8 = user RSP,
 * 9..14 = the six Linux argument registers). These two functions are the
 * whole bridge between that layout and the frame builder's register file,
 * and they are pure arrays-in/arrays-out so the host test can drive them:
 * a wrong offset here is a handler entered with the syscall's rsp or the
 * caller's rip, which no boot line would attribute to this file. */
#define LINUX_PARK_WORDS 16u   /* CapParkCtx is 128 bytes: 8 saved regs,
                                * user RSP, the six Linux arguments and
                                * (increment 29) the sysret's RAX — every
                                * word is named by an asm offset */

/* Read the INTERRUPTED context out of a park context: what the handler's
 * frame records and what rt_sigreturn will one day restore. */
int linux_sig_park_read(const uint64_t *w, uint32_t words,
                        struct LinuxSigRegs *ctx);

/* Write the HANDLER's entry context into the park context: the return
 * path rebuilds its frame from these words, so a correct install IS the
 * jump into the handler. */
int linux_sig_park_install(uint64_t *w, uint32_t words,
                           const struct LinuxSigRegs *entry);

/* E7 increment 29b: the return trip's GATE. A forwarded 15 comes back as an
 * ordinary reply, but its answer is not a return value — the task resumes in
 * the frame, so the kernel must decide "did the sidecar actually restore the
 * mask it saved at the delivery?" before touching a byte of the guest's
 * stack. That decision is this predicate, and it is NOT "the rax is not an
 * error": the sidecar's 15 arm answers status 0 exactly when it put
 * `saved_mask` back and status != 0 (E_INVAL) when no handler was entered, so
 * the status is the truth and the rax is a consequence of it. `rax` joins the
 * arguments only because a reply that never APPLIED (a length that is not the
 * request struct) leaves `status` at describe's zero — the -EPROTO the apply
 * answered with is what says the field is not the sidecar's.
 *
 * 1 = restore the frame; 0 = leave it alone and answer the syscall with the
 * errno the §5.7 map already produced. Pure: the host test drives it and
 * guard clause P16 pins the shape chan.c calls it in. */
int linux_sig_return_reply_ok(uint64_t num, uint64_t rax, int32_t status);

/* ─── E7 increment 32: the fault frame's return gate ─────────────────────
 * rt_sigreturn's gate above asks whether the SIDECAR consented, because for
 * a tkill-delivered frame the sidecar is the half that entered the handler
 * and moved the mask. A fault-delivered handler has no such half: the trap
 * entered it, the kernel built and installed the frame, and the sidecar's
 * `handler_active` was never raised — so its 15 arm answers EINVAL, which is
 * correct for its own model (it moved no mask, so it has none to put back)
 * and would otherwise be the last word on a return the kernel owes.
 *
 * This names the ONE case where that refusal is not the task's answer:
 *   - a frame IS installed (sig_active) — otherwise there is nothing to
 *     restore and EINVAL is exactly right (the live probe's
 *     SIGRET-NOHAND-OK arm depends on this);
 *   - that frame came from the TRAP (fault_active) — a sidecar-delivered
 *     frame keeps the gate above, so a refusal there still fails closed;
 *   - the answer really was a refusal (status != 0) — a consent, a
 *     -EPROTO, or any other errno is not a fault return and falls through
 *     to the same path it always took;
 *   - and the errno the task would have heard is exactly -EINVAL, the
 *     sidecar's "no handler was entered", never a different refusal
 *     (a -ENOSYS or an -EPERM stays the task's answer by name).
 *
 * Pure and parameterised by the two flags rather than by the descriptor, so
 * the host test can drive every corner of it without a process. */
int linux_sig_fault_return_ok(int sig_active, int fault_active, uint64_t rax,
                              int32_t status);

#endif /* LINUX_ABI_H */
