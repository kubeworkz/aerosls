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

/* Gap Remediation SIMI Phase 9 (sub-phase 9d): translate the embedded
 * rv64_boot_smoke.simi program and call it directly as a real function —
 * the exact same verification idea tools/simi/simi_riscv_verify.c already
 * uses on the host side (a plain C function-pointer call into freshly
 * emitted machine code), just now running inside the real freestanding
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
    int64_t result = fn();

    rv_boot_print("[SIMI] entry returned (real machine code executed) -- issuing "
                  "SYS_SLS_EXIT via ecall for real, through the new trap path...\n");

    /* Real ecall round trip: a0 = exit code, a7 = syscall number. Traps
     * into riscv_trap_entry (arch/riscv/trap_riscv.S), which dispatches
     * to riscv_syscall_dispatch() (arch/riscv/trap_riscv.c) -- that
     * function halts the hart itself (see its own comment for why: no
     * process table exists to return control to), so this call never
     * returns. */
    register uint64_t a0 __asm__("a0") = (uint64_t)result;
    register uint64_t a7 __asm__("a7") = RV_SYS_EXIT;
    __asm__ volatile("ecall" : : "r"(a0), "r"(a7) : "memory");

    /* Unreachable in practice (riscv_syscall_dispatch halts), but stated
     * explicitly rather than left as fallthrough into whatever code
     * happens to follow. */
    rv_boot_print("[SIMI] unexpected: ecall returned control.\n");
}

void kernel_riscv_main(unsigned long hart_id, unsigned long fdt) {
    (void)fdt;
    const char* msg = "AeroSLS RISC-V Supervisor Node Kernel Online!";
    for(int i = 0; msg[i] != '\0'; i++) sbi_putchar(msg[i]);
    sbi_putchar('\n');

    if (hart_id == 0) {
        uint64_t trap_stack_top =
            (uint64_t)(uintptr_t)&g_hart0_trap_stack[sizeof(g_hart0_trap_stack) - 16];
        riscv_trap_init(&g_hart0_data, trap_stack_top);

        rv64_boot_smoke_test();
    }

    while(1) { asm volatile("wfi"); }
}

void ap_riscv_kernel_main(void) { while(1); }
