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
                        * HANDLER is still never invoked — installation is
                        * refused by name (no signal frames in v1)          */
    LSC_MSR,           /* §5.3 the one kernel-side semantic: arch_prctl(SET_FS) */
    LSC_SPAWN,         /* execve: the spawn path (the sidecar's to decide) */
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

/* Plan one write's byte crossing. For any number other than LINUX_WRITE
 * the plan is empty (no attach, no refusal) — the rest of the fd class
 * still forwards as the plain 72-byte description it was. Pure: the host
 * test drives every branch, including count==0 ignoring a non-canonical
 * buffer and the wrap-around span. */
struct LinuxFdWritePlan linux_fd_write_plan(uint64_t num,
                                            const uint64_t args[6]);

/* Copy n bytes from a task's user VA into dst, walking root's page tables
 * read-only. Returns 0, or -LINUX_EFAULT — never a fault: every level is
 * checked before the bytes are read, and a span crossing a page boundary
 * validates BOTH pages. n == 0 succeeds without reading anything (root
 * may be 0). dst must hold n bytes; the caller owns it. */
int linux_user_copy_out(uint8_t *dst, uint64_t root, uint64_t va, uint32_t n);

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
#define LINUX_MMAP        9u           /* named: the mem class's NEXT      */
#define LINUX_MUNMAP     11u           /* increment, not landed here       */
#define LINUX_PROT_WRITE 0x2u          /* Linux mman.h PROT_* bits — ABI  */
#define LINUX_PROT_EXEC  0x4u          /* (the sidecar's twin repeats them */
                                       /*  as literals; guard N3 pins it)  */

/* 1 iff this mprotect must be refused before the post: prot has BOTH the
 * write and the execute bit. Total over every prot word. */
int linux_mprotect_refuses_wx(uint64_t prot);

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
 * OWN stamped state (never the reply's), req is the break the first trap
 * staged (args[0] as captured), rax is what the reply delivered. Pure:
 * the host test drives every branch, including each way a grant can be a
 * lie the kernel must refuse. */
struct LinuxMemBrkVerdict linux_mem_brk_verdict(uint64_t heap_base,
                                                uint64_t heap_limit,
                                                uint64_t brk_cur,
                                                uint64_t req, uint64_t rax);

/* The 4 KiB frame behind a user mapping, walked read-only under the
 * copy's boundaries; 0 when any level refuses (absent, supervisor, huge).
 * The unwind's read-back — and only that: a kernel address or a missing
 * page reads as 0, so a walker refusal can never free a frame that was
 * not mapped by this path. */
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
 *   - args[2] (oldact/oldset) != 0 — writing the OLD disposition BACK
 *     into the child is §10.10's copy-IN gap (increment 8's seam), which
 *     does not exist yet. Answering 0 while the caller's out-pointer
 *     keeps stack garbage it believes is an answer is exactly the silent
 *     lie the refusals-by-name discipline exists to prevent; ENOSYS with
 *     the call site's logged line is the honest shape, trigger: land the
 *     reply payload's copy-IN and this refusal goes with it.
 * A NULL act/set with a NULL out-pointer is a no-op the sidecar answers
 * 0 — nothing to copy, nothing to ask. Pure: the host test drives every
 * branch (tests/linux_personality_host_test.c). */
#define LINUX_RT_SIGACTION   13u          /* the census's sigaction row   */
#define LINUX_RT_SIGPROCMASK 14u          /* the census's sigprocmask row */
#define LINUX_SIGACT_BYTES   32u          /* kernel-ABI struct sigaction:
                                           * sa_handler, sa_flags, sa_mask,
                                           * sa_restorer — four u64s       */
#define LINUX_SIGSET_BYTES   8u           /* the sigset the shim crosses  */

struct LinuxSigCopyPlan {
    uint64_t src;    /* child VA to copy from (args[1]) — 0 = nothing   */
    uint32_t len;    /* bytes to attach: 32 (act) or 8 (set); 0 = none   */
    int32_t  refuse; /* 0 = proceed; else the Linux errno to return
                      * BEFORE any post (first-trap refusal discipline)  */
};

/* Plan one sig-class byte crossing: EINVAL for a foreign sigsetsize, 
 * ENOSYS for an out-pointer this kernel cannot write back, -EFAULT for a
 * span ring 3 could not read, else the act/set bytes (or none). Total:
 * any other number plans nothing and refuses nothing. */
struct LinuxSigCopyPlan linux_sig_copy_plan(uint64_t num,
                                            const uint64_t args[6]);

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

#endif /* LINUX_ABI_H */
