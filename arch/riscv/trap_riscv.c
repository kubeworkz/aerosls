/*
 * trap_riscv.c — see trap_riscv.h for the full design rationale. Gap
 * Remediation SIMI Phase 9 (sub-phases 9c/9d/9f/9g).
 *
 * SIMI_HOST_TEST (host-side test build only): this file is also compiled
 * for the x86 host by tools/simi/rv_syscall_exit_test.c, which stubs
 * sbi_putchar/sbi_system_reset (and, for the trap-routing twin,
 * handle_riscv_supervisor_interrupt) and drives riscv_syscall_dispatch()'s
 * exit branches and riscv_trap_dispatch_common()'s routing with injected
 * cause/tval values. Under that macro the RISC-V-only pieces are swapped
 * for host behavior: the CSR-armed functions (riscv_trap_init and the
 * riscv_trap_dispatch/riscv_trap_dispatch_m wrappers that READ scause/
 * mcause) are compiled out entirely -- x86 has no such CSRs -- while the
 * routing core they share, riscv_trap_dispatch_common(), is compiled in
 * (it takes its cause/tval as plain arguments, so the host test can
 * inject them), and rv_halt() exits the process instead of wfi-spinning
 * (see rv_halt below).
 */
#include "trap_riscv.h"
#include "sbi.h"
#include <stddef.h>
#if defined(SIMI_HOST_TEST)
#include <stdlib.h>   /* exit() -- host-side test build only, see rv_halt() */
#endif

extern void handle_riscv_supervisor_interrupt(uint64_t scause, uint64_t stval);
/* Deliberately NOT calling kernel/process.c's process_exit() here -- it
 * assumes a live ProcessDescriptor entered via the x86-only
 * kernel_enter_ring3() mechanism (swapgs, per-CPU GS-relative state),
 * none of which exists on this build. See riscv_syscall_dispatch()'s own
 * comment below. */

/* ─── Layout guards ─────────────────────────────────────────────────────
 * trap_riscv.S hardcodes every one of these as a literal byte offset
 * (asm can't #include this struct). These _Static_asserts are the only
 * thing standing between "the struct changed" and "the assembly silently
 * saves/restores the wrong register at the wrong offset" -- exactly the
 * class of bug this project has caught before by actually running code
 * (see the Phase 11 callee-saved-register corruption bug, or the RV64
 * JMPR host-address bug from Phase 14 Part E) rather than by review
 * alone. There is no way to run this file's own assembly in this sandbox
 * (see trap_riscv.h's header comment and AeroSLS-SIMI-ISA-v0.1.md §16
 * Phase 9 for why), so these compile-time checks are the strongest
 * verification available for this particular file today -- real, but
 * weaker than the execution-based verification every other file in this
 * project gets, and that gap is stated here plainly rather than left
 * implicit. */
_Static_assert(offsetof(struct RvPerHartData, trap_frame) == 0,
                "trap_riscv.S assumes trap_frame is RvPerHartData's first member");
_Static_assert(offsetof(struct RvPerHartData, kernel_sp) == 528,
                "trap_riscv.S hardcodes offset 528 for kernel_sp (Design B FP region: 66*8)");
_Static_assert(TF_SP == 1, "trap_riscv.S hardcodes offset 8 for trap_frame.sp");
_Static_assert(TF_SEPC == 31, "trap_riscv.S hardcodes offset 248 for trap_frame.sepc");
_Static_assert(TF_A7 == 16, "riscv_syscall_dispatch reads a7 at trap_frame[16] (offset 128)");
_Static_assert(TF_A0 == 9, "riscv_syscall_dispatch reads/writes a0 at trap_frame[9] (offset 72)");
/* Design B FP region (ISA doc §16 Phase 16 audit addendum): pin the
 * reserved slots so the future save path (an FS lazy-save scause=2
 * handler, or an eager entry save) and this header can never drift: the
 * GPR+sepc slots occupy 0..248, f0-f31 256..504, fcsr 512, sfs 520.
 * The entry assembly does not touch the FP region yet (it only saves
 * GPRs), so these asserts document the reservation, not current code. */
_Static_assert(TF_F0 == 32, "TF_F0 must be the first slot after TF_SEPC (offset 256)");
_Static_assert(TF_F31 == 63, "TF_F31 must end the f-register block (offset 504)");
_Static_assert(TF_FCSR == 64, "TF_FCSR at offset 512 (fcsr is 32-bit, full slot)");
_Static_assert(TF_SFS == 65, "TF_SFS at offset 520 (saved sstatus.FS field)");
_Static_assert(sizeof(struct RvPerHartData) == 1608,
                "1608 = 66*8 (trap_frame, GPR+sepc + Design B FP region) + 8 "
                "(kernel_sp) + 4*33*8 (fp_save, Design B part 2/3: RV_FP_OWNERS=4 "
                "owners' f0-f31+fcsr) + 8 (fp_owner) + 8 (fp_current) -- if "
                "this changes, trap_riscv.S's hardcoded offsets need updating "
                "too");
_Static_assert(offsetof(struct RvPerHartData, fp_save) == 536,
                "fp_save must sit right after kernel_sp (offset 536)");

/* ─── Local no-libc helper (mirrors kernel/simi_x86.c's own convention) ── */
static void rv_print_str(const char* s) {
    while (*s) sbi_putchar(*s++);
}
static void rv_print_udec(uint64_t v) {
    char buf[20];
    int i = 0;
    if (v == 0) { sbi_putchar('0'); return; }
    while (v > 0 && i < 20) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) sbi_putchar(buf[--i]);
}

/* Design B part 3: how many FS lazy-saves have fired since boot. Only
 * touched from trap context (single hart, interrupts disabled inside
 * the handler), so a plain global is fine. The N-task ready queue
 * asserts its delta across the demo (exactly one lazy-save per
 * task-slice, since each switch disarms FP). */
uint64_t g_fp_lazy_count;

/* Deliberate terminal halt for the syscall/unhandled paths (and, since
 * Phase 9i's command loop, the shell's `exit` command -- arch/riscv/
 * sbi.c, which includes this header). Real kernel: wfi-spin forever,
 * never returning. Host-side test build (SIMI_HOST_TEST): exit the
 * process cleanly instead, so the harness observes "dispatch never
 * returned" as the process terminating from inside the handler rather
 * than falling through to main. */
void rv_halt(void) {
#if defined(SIMI_HOST_TEST)
    exit(0);
#else
    while (1) { __asm__ volatile("wfi"); }
#endif
}

#if !defined(SIMI_HOST_TEST)
void riscv_trap_init(struct RvPerHartData* phd, uint64_t kernel_stack_top) {
    phd->kernel_sp = kernel_stack_top;
    for (int i = 0; i < TF_COUNT; i++) phd->trap_frame[i] = 0;

    /* sscratch <- &RvPerHartData. Must happen before the trap vector is
     * armed -- the very first trap after the csrw below will immediately
     * swap sp with whatever sscratch currently holds, so sscratch has to
     * already be correct by then, not "correct eventually." */
    __asm__ volatile("csrw sscratch, %0" : : "r"(phd) : "memory");

    /* Phase 9g: arm the vector for the build's mode -- the variant is
     * baked in at compile time (-DRISCV_MMODE), not detected at runtime:
     * reading mstatus (the only CSR that would reveal the privilege
     * mode) traps as an illegal instruction from S-mode (see §16 Phase
     * 9g). Under OpenSBI (S-mode) exceptions go to stvec; as a direct
     * M-mode payload they always go to mtvec, and arming only stvec
     * would leave every trap jumping to address 0. The M-mode entry
     * reads mepc/mcause/mtval and returns via mret (trap_riscv.S). */
#if defined(RISCV_MMODE)
    extern void riscv_trap_entry_m(void);
    __asm__ volatile("csrw mtvec, %0" : : "r"(&riscv_trap_entry_m) : "memory");
    rv_print_str("[TRAP] mtvec + sscratch armed for this hart (direct M-mode boot).\n");
#else
    extern void riscv_trap_entry(void);
    __asm__ volatile("csrw stvec, %0" : : "r"(&riscv_trap_entry) : "memory");
    rv_print_str("[TRAP] stvec + sscratch armed for this hart.\n");
#endif
}
#endif /* !SIMI_HOST_TEST: CSR-armed riscv_trap_init only -- riscv_syscall_dispatch below is ALWAYS compiled, because the host test drives it */

/* Gap Remediation SIMI Phase 9 (sub-phases 9d/9f): the minimal,
 * headless-only syscall surface. Only SYS_SLS_EXIT is wired -- there is
 * no process table, no scheduler, and no user/kernel privilege
 * separation on RISC-V yet (paging is still Bare everywhere, see
 * user_paging_riscv.h), so kernel/process.c's real process_exit() (which
 * assumes a live ProcessDescriptor entered via kernel_enter_ring3(), an
 * x86-only mechanism this phase does not port -- see §16 Phase 9's
 * findings) has nothing valid to act on here. Since 9f the exit is a
 * REAL machine shutdown through OpenSBI's SBI_SRST extension (an ecall
 * to M-mode OpenSBI powers the machine off -- on QEMU the process exits
 * rc=0), which is the honest, correct behavior for "a bare translated
 * SIMI program finished running with nothing else going on," not a
 * stand-in for real process teardown. Reached either from an ecall
 * (exception 9, only once/if OpenSBI ever delegates it) or from the
 * ebreak trigger (exception 3, delegated today -- see
 * riscv_trap_dispatch()'s comment and trap_riscv.h). */
void riscv_syscall_dispatch(struct RvPerHartData* phd) {
    uint64_t num = phd->trap_frame[TF_A7];
    uint64_t arg0 = phd->trap_frame[TF_A0];

    if (num == RV_SYS_EXIT) {
        rv_print_str("[SYSCALL] SYS_SLS_EXIT, code=");
        rv_print_udec(arg0);
#if defined(RISCV_MMODE)
        /* Direct M-mode boot: there is no firmware, so no SBI_SRST
         * exists to power the machine off -- the honest terminal state
         * is a reported, deliberate halt (QEMU is killed by the CI
         * timeout, rc=124). */
        rv_print_str(" -- direct M-mode boot: no firmware to power off -- halting hart.\n");
#else
        rv_print_str(" -- powering off via OpenSBI SBI_SRST (SHUTDOWN)...\n");
        sbi_system_reset();
        /* Only reached if the firmware lacks the SRST extension (or the
         * reset failed) -- sbi_system_reset() documents that callers
         * must not assume the machine is gone just because it returned. */
        rv_print_str("[SYSCALL] SBI_SRST unsupported or failed -- halting hart instead.\n");
#endif
        rv_halt();
    }

    rv_print_str("[SYSCALL] unimplemented syscall number ");
    rv_print_udec(num);
    rv_print_str(" -- halting hart.\n");
    rv_halt();
}

/* Shared routing for both privilege modes; the caller reads the
 * mode-appropriate cause/tval CSRs (scause/stval in S-mode,
 * mcause/mtval in M-mode) and hands them in. The unhandled-exception
 * message below calls the value "scause" for brevity; in M-mode it is
 * the mcause value (same encoding). NOT static: the CSR-armed wrappers
 * below call it in the kernel, and the SIMI_HOST_TEST build drives it
 * directly with injected cause/tval (tools/simi/rv_syscall_exit_test.c,
 * -DSIMI_TEST_TRAP) -- see trap_riscv.h. */
void riscv_trap_dispatch_common(struct RvPerHartData* phd,
                                uint64_t scause, uint64_t stval) {
    int is_interrupt = (int)((scause >> 63) & 1);
    uint64_t code = scause & 0x7FFFFFFFFFFFFFFFULL;

    if (is_interrupt) {
        /* Confirmed-dangling-symbol fix: this is the first real call site
         * for handle_riscv_supervisor_interrupt() anywhere in this
         * project -- see this file's header comment / trap_riscv.h. */
        handle_riscv_supervisor_interrupt(scause, stval);
        return;
    }

    if (code == 9) {   /* Environment call from S-mode */
        riscv_syscall_dispatch(phd);
        /* riscv_syscall_dispatch() never returns today (every path halts
         * the hart or powers the machine off) -- but advance sepc past
         * the ecall regardless, so a future syscall that DOES return
         * doesn't re-trap on the same ecall instruction forever. ecall is
         * always a 4-byte instruction (RISC-V has no compressed-C
         * encoding for it), so +4 is exact, not a heuristic. */
        phd->trap_frame[TF_SEPC] += 4;
        return;
    }

    if (code == 3 && phd->trap_frame[TF_A7] == RV_SYS_EXIT) {
        /* S-mode syscall trigger (Phase 9f): OpenSBI's default MEDELEG
         * does not delegate exception 9 (ecall from S-mode), so an ecall
         * carrying the syscall ABI never reaches our stvec handler -- it
         * bounces as an SBI call. ebreak (exception 3) IS delegated, so
         * the kernel's syscall convention is carried through ebreak
         * instead: a7 = syscall number, a0 = argument -- the same ABI
         * the ecall path above uses (see trap_riscv.h). The SIMI boot
         * smoke issues exactly this (kernel/kernel_riscv.c), and
         * riscv_syscall_dispatch() powers the machine off via SBI_SRST,
         * so this is the smoke's terminal event. If dispatch ever
         * returns, advance past the ebreak -- +4 is exact ONLY because
         * the trigger site pins the 4-byte (non-compressed) ebreak via
         * .option norvc (kernel/kernel_riscv.c; the C extension would
         * otherwise compress `ebreak` to 2 bytes and +4 would land
         * mid-instruction -- a latent bug the bare-metal trap-entry
         * fixture exposed by executing the restore path, ISA doc §16
         * Phase 9h) -- and continue. */
        riscv_syscall_dispatch(phd);
        phd->trap_frame[TF_SEPC] += 4;
        return;
    }

#if !defined(SIMI_HOST_TEST)
    /* Design A + Design B part 2 (ISA doc §16 Phase 16 audit addendum):
     * an illegal instruction. Kernel-only: the branch reads/writes
     * sstatus (a CSR the x86 host twin cannot express) and calls the
     * FP save/load asm helpers (trap_riscv.S, never built on host), so
     * under SIMI_HOST_TEST code==2 falls through to the unhandled
     * branch below -- which is exactly what rv-trap-test's "unhandled"
     * mode pins (scause=2 + stval=0x1234 -> the [TRAP] unhandled
     * exception message). The lazy-save itself is verified by the
     * kernel float smoke, not the host twin.
     *
     * stval for cause 2 holds the FAULTING INSTRUCTION BITS, not an
     * address (RISC-V priv spec; the boot spikes confirmed QEMU puts
     * e.g. 0xF2028553, the full fmv.d.x word, in mtval), so the opcode
     * is decoded directly from stval. Two classes:
         *
         * 1. FP-family opcode (opcodes 0x07/0x27 load/store-FP, the four
         *    FMA opcodes 0x43/0x47/0x4B/0x4F, OP-FP 0x53) WITH FS=Off: a
         *    disabled-FP access -- the Design B part 2 LAZY-SAVE. The FP
         *    registers still hold the previous owner's live state (the
         *    owner switch only cleared FS, it never saved), so: enable
         *    FP (FS=Dirty), save the live owner's state into its
         *    fp_save row, load the new owner's saved state, fold
         *    fp_current, and return WITHOUT advancing sepc -- the
         *    trapping instruction re-executes, now enabled. The lazy
         *    save happens exactly once per owner switch (after the
         *    first re-execution FS stays Dirty). This is the FS lazy-
         *    save twin lazy_vector.c's header always claimed existed.
         * 2. Anything else (genuinely malformed opcode, or an FP
         *    instruction with FS already enabled): Design A's loud halt
         *    with the FP-free diagnostic -- no safe recovery exists
         *    (the arm64 FPEN trap is the same shape: loud halt, never
         *    silent corruption). */
    if (code == 2) {
        uint32_t insn = (uint32_t)stval;
        unsigned q = insn & 3;   /* compressed-instruction quadrant */
        int is_fp;
        if (q == 3) {
            /* Full 32-bit instruction: the RISC-V opcode field (bits 6:0)
             * is authoritative. FP family = load/store-FP (0x07/0x27),
             * the four FMA opcodes (0x43/0x47/0x4B/0x4F), OP-FP (0x53). */
            unsigned op = insn & 0x7f;
            is_fp = (op == 0x07 || op == 0x27 || op == 0x43 || op == 0x47 ||
                     op == 0x4b || op == 0x4f || op == 0x53);
        } else if (q == 1) {
            is_fp = 0;   /* quadrant 1 is all integer/control (c.addi..c.bnez) */
        } else {
            /* Compressed FP load/store: quadrant 0 (c.fld funct3=001,
             * c.fsd funct3=101) and quadrant 2 (c.fldsp 001, c.fsdsp
             * 101). The low bits of a compressed word are NOT an RV64
             * opcode field — the task round-robin's prologue fsd traps
             * as c.fsd and this quadrant decode is what keeps it a
             * lazy-save instead of a halt (the census's compressed-
             * encoding lesson, on the decode side this time). RV64 has
             * no c.flw/c.fsw (RV32-only), so funct3 011 is not FP. */
            unsigned f3 = (insn >> 13) & 7;
            is_fp = (f3 == 0x01 || f3 == 0x05);
        }
        uint64_t sstatus_v;
        __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_v));
        uint64_t fs = (sstatus_v >> 13) & 3;
        if (is_fp && fs == 0) {
            uint64_t live = phd->fp_current;   /* full owner id, 0..RV_FP_OWNERS-1 */
            uint64_t next = phd->fp_owner;     /* (the &1 two-owner masks are gone -- part 3) */
            __asm__ volatile("csrw sstatus, %0"
                              : : "r"(sstatus_v | (3ULL << 13)) : "memory");
            fp_save_all(&phd->fp_save[live][0]);
            fp_load_all(&phd->fp_save[next][0]);
            phd->fp_current = next;
            g_fp_lazy_count++;
            /* Short form on purpose: this line prints on EVERY slice's
             * first fadd.d (once per preemption), and at the demo's
             * fast cadence the ~80-char tail was a meaningful chunk of
             * the slice budget (~0.5ms at 115200 baud vs ~0.2ms now) —
             * the cadence stress tightens the print path itself. The
             * "[FP] lazy-save:" prefix is what the CI count asserts;
             * the semantics (owner swap, sepc unchanged, re-execute)
             * are documented in the code and the ISA doc. */
            rv_print_str("[FP] lazy-save: owner ");
            rv_print_udec(live);
            rv_print_str(" -> ");
            rv_print_udec(next);
            rv_print_str("\n");
            return;   /* sepc unchanged: re-execute the trapping FP insn */
        }
        rv_print_str("[TRAP] illegal instruction (scause=2), sstatus.FS=");
        rv_print_udec(fs);
        rv_print_str(" -- an FP/vector access with FS=Off traps here. The RV64 kernel's FP use is limited to the fp_save_all/fp_load_all plumbing (rv64_fp_census gate); halting hart.\n");
        rv_halt();
        return;
    }
#endif /* !SIMI_HOST_TEST: the scause=2 special-case is kernel-only */

    /* Unhandled exception (page fault, misaligned access, ecall from an
     * unexpected mode, ...). No crash-safe recovery exists for these yet
     * -- print diagnostics and halt rather than `sret` back into the
     * same faulting instruction forever. */
    rv_print_str("[TRAP] unhandled exception, scause=");
    rv_print_udec(scause);
    rv_print_str(", stval=");
    rv_print_udec(stval);
    rv_print_str(", sepc=");
    rv_print_udec(phd->trap_frame[TF_SEPC]);
    rv_print_str(" -- halting hart.\n");
    rv_halt();
}

#if !defined(SIMI_HOST_TEST)
void riscv_trap_dispatch(struct RvPerHartData* phd) {
    uint64_t scause, stval;
    __asm__ volatile("csrr %0, scause" : "=r"(scause));
    __asm__ volatile("csrr %0, stval"  : "=r"(stval));
    riscv_trap_dispatch_common(phd, scause, stval);
}

/* Phase 9g: M-mode twin -- exceptions taken in M-mode populate mcause/
 * mtval, not scause/stval. Called by riscv_trap_entry_m (trap_riscv.S). */
void riscv_trap_dispatch_m(struct RvPerHartData* phd) {
    uint64_t mcause, mtval;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    __asm__ volatile("csrr %0, mtval"  : "=r"(mtval));
    riscv_trap_dispatch_common(phd, mcause, mtval);
}
#endif /* !SIMI_HOST_TEST: CSR-armed trap wrappers above */
