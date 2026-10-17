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
 * the request struct and the forwarding path (§3.3's steps 1-7) are
 * increment 2. fs_base/TLS (§5.3) is increment 3, because it is the one
 * piece that touches the MSR and the context switch. brk/mmap budget
 * policy (§5.4) is increment 4. This file carries no mechanism, so a bug
 * here can only be a wrong number or a malformed image — both of which a
 * host test names, and neither of which can reach a running system.
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
    LSC_SIG,           /* accepted and recorded; never delivered (§5.6) */
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

#endif /* LINUX_ABI_H */
