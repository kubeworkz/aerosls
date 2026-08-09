/*
 * kernel_riscv.c — AeroSLS RISC-V supervisor entry point.
 *
 * Gap Remediation SIMI Phase 9 (sub-phase 9d): extended from a 10-line
 * boot banner + wfi loop into a real, if deliberately minimal, boot-time
 * smoke test proving simi_riscv_translate()'s output is directly
 * callable as real machine code inside THIS freestanding kernel binary,
 * with a genuine ecall-based syscall round trip through the new
 * arch/riscv/trap_riscv.{S,c} trap-entry mechanism. See
 * AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9 for the full scope statement and
 * an honest accounting of what this is and is not (no process table, no
 * user/kernel privilege separation, no Sv39 paging enabled -- this all
 * still runs at the kernel's own S-mode privilege level, on the boot
 * stack's own physical memory, exactly like the banner-and-halt code it
 * replaces did).
 */
#include "../include/sls_mmu.h"
#include "simi_riscv.h"
#include "../arch/riscv/trap_riscv.h"
#include "rv64_boot_smoke_tmo.h"

extern void sbi_putchar(char c);

static void rv_boot_print(const char* s) {
    while (*s) sbi_putchar(*s++);
}
static void rv_boot_print_hex_rc(int rc) {
    /* rc is always small and non-negative (a TX_RV_ERR_* enum value) --
     * a single decimal digit is enough for every error code this
     * translator can return today. */
    sbi_putchar((char)('0' + (rc % 10)));
}

/* Dedicated trap-handling stack for hart 0 — deliberately separate from
 * bsp_stack_top (arch/riscv/boot_riscv.S), per trap_riscv.h's own
 * requirement that riscv_trap_init()'s kernel_stack_top not alias
 * whatever stack the caller is already running on. 8 KiB, matching the
 * x86 side's kernel_syscall_stack sizing (arch/x86/user_paging.c). */
static uint8_t g_hart0_trap_stack[8192] __attribute__((aligned(16)));
static struct RvPerHartData g_hart0_data;

/* Output buffer for the smoke-test translation. The embedded program is
 * tiny (80-byte .tmo, ~870 bytes of translated code per the host
 * toolchain's own simi-riscv-verify run — see rv64_boot_smoke_tmo.h) —
 * one page is generous headroom, matching this project's established
 * "static buffer, no general-purpose allocator" convention (kernel/
 * simi_translate.c's g_simi_code_buf, kernel/loader.c's ServiceBinary). */
#define RV64_SMOKE_CODE_BUF_SIZE 4096
static uint8_t g_smoke_code_buf[RV64_SMOKE_CODE_BUF_SIZE] __attribute__((aligned(16)));

/* Gap Remediation SIMI Phase 9 (sub-phases 9d/9f): translate the embedded
 * rv64_boot_smoke.simi program and call it directly as a real function —
 * the exact same verification idea tools/simi/simi_riscv_verify.c already
 * uses on the host side (a call into freshly emitted machine code, with
 * the result read from t0), just now running inside the real freestanding
 * kernel binary instead of a host test harness. scratch_ptr/rt_resolve_fn/
 * rt_objsize_fn/rt_objtype_fn are all 0 -- this program never touches r7,
 * r6, RESOLVE, OBJSIZE, or OBJTYPE, so nothing dereferences them. */
static void rv64_boot_smoke_test(void) {
    rv_boot_print("[SIMI] RV64 boot smoke test: translating rv64_boot_smoke.tmo...\n");

    uint32_t len = 0, entry_off = 0;
    int rc = simi_riscv_translate(g_rv64_boot_smoke_tmo, g_rv64_boot_smoke_tmo_len,
                                   g_smoke_code_buf, RV64_SMOKE_CODE_BUF_SIZE,
                                   "main", 0, 0, 0, 0, &len, &entry_off);
    if (rc != TX_RV_OK) {
        rv_boot_print("[SIMI] translate FAILED, rc=");
        rv_boot_print_hex_rc(rc);
        rv_boot_print("\n");
        return;
    }

    rv_boot_print("[SIMI] translated OK, calling entry directly...\n");

    typedef int64_t (*SimiEntryFn)(void);
    SimiEntryFn fn = (SimiEntryFn)(uintptr_t)(g_smoke_code_buf + entry_off);

    /* The translated program's result rides in t0 (x5), NOT a0: that is
     * the RV translator's documented return convention (simi_riscv.c's
     * OP_RET loads r0 into t0 -- "r0 is the return-value register" --
     * and the trampoline's epilogue returns with t0 untouched; the host
     * verifier reads the same register, simi_riscv_verify.c's cpu.x[5]).
     * A plain C call would read a0, which the trampoline never sets, so
     * the call happens in inline asm with ra as the link register (the
     * trampoline saves/restores its incoming ra itself) and t0 declared
     * live so the compiler cannot reuse it; the value is copied out of
     * t0 immediately, before any intervening C call can clobber it. */
    register uint64_t result __asm__("t0");
    __asm__ volatile(
        "jalr ra, 0(%[addr])\n"
        : "+r"(result)
        : [addr] "r"((unsigned long)(uintptr_t)fn)
        : "ra", "memory");
    uint64_t code = result;   /* captured from t0 before anything can clobber it */

    rv_boot_print("[SIMI] entry returned (real machine code executed) -- issuing "
                  "RV_SYS_EXIT (a7=164, a0=code) via ebreak, the delegated "
                  "syscall trigger...\n");

    /* The SIMI syscall fixture on real hardware (Phase 9f): the
     * translated program's return value (42) becomes the RV_SYS_EXIT
     * syscall ABI -- a7 = 164 (matching kernel/process.h's SYS_SLS_EXIT),
     * a0 = exit code -- and the syscall is triggered through ebreak
     * (exception 3), the one exception OpenSBI's default MEDELEG
     * delegates to S-mode. (Exception 9, ecall from S-mode, is NOT
     * delegated -- an ecall carrying this ABI bounces as a failed SBI
     * call and never reaches our stvec handler.) riscv_trap_entry
     * (arch/riscv/trap_riscv.S) saves every GPR including a7/a0,
     * riscv_trap_dispatch() (arch/riscv/trap_riscv.c) routes scause=3
     * with a7==RV_SYS_EXIT to riscv_syscall_dispatch(), which reports
     * the exit code and powers the machine off via OpenSBI's SBI_SRST
     * extension -- control never returns, and QEMU exits rc=0. This is
     * the same trap path the pre-9f ebreak smoke exercised, now carrying
     * the real syscall instead of halting on an unhandled exception. */
    __asm__ volatile(
        "mv a0, %0\n"
        "li a7, %1\n"
        "ebreak\n"
        : : "r"(code), "i"(RV_SYS_EXIT) : "a0", "a7", "memory");

    /* Unreachable in practice (the syscall powers the machine off), but
     * stated explicitly rather than left as fallthrough into whatever
     * code happens to follow. */
    rv_boot_print("[SIMI] unexpected: RV_SYS_EXIT returned control.\n");
}

void kernel_riscv_main(unsigned long hart_id, unsigned long fdt) {
    (void)hart_id;
    (void)fdt;
    const char* msg = "AeroSLS RISC-V Supervisor Node Kernel Online!";
    for(int i = 0; msg[i] != '\0'; i++) sbi_putchar(msg[i]);
    sbi_putchar('\n');

    /* OpenSBI (fw_dynamic) delivers the payload to exactly one hart -- the
     * boot hart -- whose id is NOT necessarily 0 (default: the last hart,
     * see boot_riscv.S). The former `hart_id == 0` gate never fired under
     * `-smp > 1`, so the trap path and the SIMI smoke never ran; the
     * delivered hart IS the boot hart, so run the boot sequence directly. */
    uint64_t trap_stack_top =
        (uint64_t)(uintptr_t)&g_hart0_trap_stack[sizeof(g_hart0_trap_stack) - 16];
    riscv_trap_init(&g_hart0_data, trap_stack_top);

    rv64_boot_smoke_test();

    while(1) { asm volatile("wfi"); }
}
