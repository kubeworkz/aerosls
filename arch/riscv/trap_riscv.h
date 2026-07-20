/*
 * trap_riscv.h — Gap Remediation SIMI Phase 9 (sub-phase 9c): real RISC-V
 * trap entry/exit and a per-hart-data mechanism. See
 * AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9 for the full scope statement.
 *
 * This is a genuinely RISC-V-native design, not a transliteration of
 * x86's swapgs/MSR-based syscall gate (arch/x86/user_paging.c's
 * syscall_gate_init/PerCPUData) — RISC-V has no equivalent instruction or
 * mechanism, so the natural analogue is the standard `sscratch`-swap
 * trap-entry idiom every real RISC-V supervisor kernel uses: `sscratch`
 * holds a pointer to this hart's RvPerHartData for the ENTIRE time
 * outside of trap handling; `csrrw sp, sscratch, sp` at trap entry
 * atomically swaps the live `sp` into `sscratch` and loads
 * `sscratch`'s old value (the RvPerHartData pointer) into `sp`, giving
 * the entry stub real, valid memory to save every other GPR into before
 * anything else touches them.
 *
 * Confirmed bug this fixes in passing: `arch/riscv/sbi.c`'s
 * `handle_riscv_supervisor_interrupt()` carries a comment claiming it is
 * "registered inside stvec" — a direct grep of every `.c`/`.S` file in
 * this tree before this phase found no `csrw stvec` anywhere at all.
 * `stvec` was never actually written; that handler was dead code, only
 * reachable if something set the trap vector, which nothing did.
 * `riscv_trap_init()` below is the first real `csrw stvec` in this
 * project, and `riscv_trap_dispatch()` (trap_riscv.c) is what now
 * actually calls `handle_riscv_supervisor_interrupt()` for external-
 * interrupt causes, closing that gap for real rather than leaving the
 * stale comment as the only evidence anyone intended to.
 *
 * Layout note: `trap_frame[]`'s 32 slots and `RvPerHartData`'s overall
 * shape are mirrored EXACTLY as hardcoded byte offsets in
 * trap_riscv.S (0, 8, 16, ... 248 for the frame, 256 for kernel_sp) —
 * assembly can't #include this struct, so trap_riscv.c carries
 * compile-time `_Static_assert`s pinning every offset this header
 * implies against what the assembly actually uses, specifically so the
 * two can never silently drift apart undetected.
 */
#ifndef TRAP_RISCV_H
#define TRAP_RISCV_H

#include <stdint.h>

/* Index into trap_frame[] for each general-purpose register — x0 (zero)
 * is never saved (hardwired zero, saving it would be pointless), so
 * these indices skip it entirely: index 0 is x1 (ra), not x0. */
enum {
    TF_RA = 0, TF_SP, TF_GP, TF_TP,
    TF_T0, TF_T1, TF_T2,
    TF_S0, TF_S1,
    TF_A0, TF_A1, TF_A2, TF_A3, TF_A4, TF_A5, TF_A6, TF_A7,
    TF_S2, TF_S3, TF_S4, TF_S5, TF_S6, TF_S7, TF_S8, TF_S9, TF_S10, TF_S11,
    TF_T3, TF_T4, TF_T5, TF_T6,
    TF_SEPC,
    TF_COUNT   /* = 32 */
};

struct RvPerHartData {
    uint64_t trap_frame[TF_COUNT];   /* offsets 0..248, see enum above */
    uint64_t kernel_sp;              /* offset 256: top of this hart's
                                       * dedicated trap-handling stack --
                                       * the C dispatcher runs on THIS
                                       * stack, not on trap_frame[] itself
                                       * (32*8=256 bytes is nowhere near
                                       * enough room for a real C call
                                       * stack). */
};

/* Real assembly trap entry point, installed into stvec by
 * riscv_trap_init() below. Never called directly from C. */
void riscv_trap_entry(void);

/* Called once per hart during boot, before anything that might trap
 * (an ecall, or an external interrupt if sstatus.SIE ever gets set).
 * kernel_stack_top must point to the top (highest address) of a real,
 * dedicated, already-allocated stack region for this hart's trap
 * handling -- distinct from whatever stack kernel_riscv_main() itself is
 * already running on. */
void riscv_trap_init(struct RvPerHartData* phd, uint64_t kernel_stack_top);

/* The real trap dispatcher, called from riscv_trap_entry (trap_riscv.S)
 * once every GPR is safely saved. Reads scause/stval itself; routes
 * external-interrupt causes to the existing
 * handle_riscv_supervisor_interrupt() (arch/riscv/sbi.c), and
 * environment-call-from-S-mode causes to riscv_syscall_dispatch()
 * (trap_riscv.c) -- see that function's own comment for why S-mode, not
 * U-mode. */
void riscv_trap_dispatch(struct RvPerHartData* phd);

/* Gap Remediation SIMI Phase 9 (sub-phase 9d): minimal ecall-based
 * syscall surface. a7 = syscall number, a0 = single argument -- the
 * smallest convention that can express SYS_SLS_EXIT (the only syscall
 * wired today; see trap_riscv.c). NOT the real AeroSLS x86 syscall ABI
 * (kernel/process.c's SYSCALL-based dispatch, register-for-argument
 * marshaling, or the full syscall table) -- a deliberately narrower,
 * headless-only surface, per this project's own established "host
 * toolchain only until it can actually be verified" scoping discipline
 * (see §16 Phase 9's sub-phase 9d design text). */
#define RV_SYS_EXIT 164   /* matches kernel/process.h's SYS_SLS_EXIT numeric value */

void riscv_syscall_dispatch(struct RvPerHartData* phd);

#endif /* TRAP_RISCV_H */
