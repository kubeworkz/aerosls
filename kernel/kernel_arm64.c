/* kernel/kernel_arm64.c — M4a/M4b: the minimal arm64 kernel main.
 *
 * Gap Remediation SIMI Phase 10 / M4 (docs/AeroSLS-SIMI-ARM-Backend-Plan-
 * v0.1.md §6 M4, §10.187): the first bootable AArch64 kernel, mirroring
 * the RISC-V kernel's Phase 9 shape — a self-contained qemu -M virt
 * build whose boot prints a banner, translates and EXECUTES an embedded
 * .tmo through kernel/simi_arm.c (M4b), and ends in a clean power-off.
 * The arm64 analog of SBI_SRST is PSCI: qemu -M virt emulates PSCI 0.2
 * through its EL3 monitor, so `smc #0` with the SYSTEM_OFF function id
 * powers the machine off and QEMU exits rc=0.
 *
 * M4b's division of labor (the point of this milestone): the encoder
 * (kernel/simi_arm.c) is already proven by the qemu-aarch64 M3 leg and
 * is byte-identical to it (the re-diff tripwire §10.182); the kernel
 * leg's job is the LINK and the call. That link makes the §10.181
 * contract concrete: GCC 13 on AArch64 synthesizes memcpy/memset calls
 * from the M2 chain struct copies in simi_arm.c, and the kernel must
 * PROVIDE the freestanding {memcpy, memset} pair — defined below, the
 * exact symbols the undefined-symbol gate permits and no more.
 *
 * No libc, no FP/SIMD: compiled -mgeneral-regs-only, mirroring the x86
 * kernel's -mno-sse discipline. */
#include <stdint.h>

#include "arch/arm64/uart_pl011.h"
#include "arm64_boot_smoke_tmo.h"
#include "simi_arm.h"

/* PSCI 0.2 function ids (smc #0 to the virt EL3 monitor). */
#define PSCI_FN_SYSTEM_OFF 0x84000008UL

/* The freestanding {memcpy, memset} pair the M2 chain struct copies in
 * simi_arm.c synthesize (§10.181). Byte loops: GCC recognizes the
 * function names as builtins but cannot turn their own definitions into
 * recursive calls, so these are self-contained (verified: the linked
 * ELF has zero undefined symbols). */
void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *s, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

/* The emitted-code buffer (M4b): 4 KiB, mirroring the RV64 kernel's
 * RV64_SMOKE_CODE_BUF_SIZE. The smoke program emits ~1 KiB of native
 * A64 (the M2 register-allocator prologue/trampoline machinery), so
 * 4 KiB is ample; overflow would trip the translator's out_cap. */
#define ARM64_SMOKE_CODE_BUF_SIZE 4096
static uint8_t g_smoke_code_buf[ARM64_SMOKE_CODE_BUF_SIZE] __attribute__((aligned(16)));

static void print_u64(uint64_t v)
{
    /* Minimal hex print for the [SIMI] result line (no libc). */
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--) {
        unsigned nib = (unsigned)(v & 0xF);
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
        v >>= 4;
    }
    buf[16] = '\0';
    uart_puts("0x");
    uart_puts(buf);
}

/* M4b: translate the embedded arm64_boot_smoke.tmo and call the entry
 * as a real function. The translated program's result rides in t0 (x9),
 * NOT x0: X_T0 is simi_arm.c's primary working register and carries the
 * RET result; the trampoline is a normal callable A64 subroutine, so a
 * plain `blr` works and returns with x9 holding the result — exactly
 * what simi_arm_jit.c's `blr x0; mov x0, x9; ret` stub reads (M3,
 * §10.178). The call happens in inline asm with x9 declared live so the
 * compiler cannot reuse it; the value is copied out of x9 immediately,
 * before any intervening C call can clobber it — the RV64 kernel's t0
 * pattern (kernel_riscv.c rv64_boot_smoke_test), mirrored. */
static uint64_t arm64_boot_smoke_test(void)
{
    uart_puts("[SIMI] translating arm64_boot_smoke.tmo with kernel/simi_arm.c...\r\n");

    uint32_t len = 0, entry_off = 0;
    int rc = simi_arm_translate(g_arm64_boot_smoke_tmo, g_arm64_boot_smoke_tmo_len,
                                g_smoke_code_buf, ARM64_SMOKE_CODE_BUF_SIZE,
                                "main", 0, 0, 0, 0, &len, &entry_off);
    if (rc != TX_AR_OK) {
        uart_puts("[SIMI] translate FAILED, rc=");
        print_u64((uint64_t)(unsigned)rc);
        uart_puts(" (");
        uart_puts(simi_arm_strerror(rc));
        uart_puts(")\r\n");
        return 0;
    }

    uart_puts("[SIMI] translated OK, calling entry directly...\r\n");

    typedef int64_t (*SimiEntryFn)(void);
    SimiEntryFn fn = (SimiEntryFn)(uintptr_t)(g_smoke_code_buf + entry_off);

    register uint64_t result __asm__("x9");
    __asm__ volatile(
        "blr %[addr]\n"
        : "+r"(result)
        : [addr] "r"((unsigned long)(uintptr_t)fn)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
          "x10", "x11", "x12", "x29", "x30", "memory");
    uint64_t code = result;   /* captured from x9 before anything can clobber it */

    uart_puts("[SIMI] entry returned (real machine code executed) -- result=");
    print_u64(code);
    uart_puts(" (expected 0x2a = 42)\r\n");
    return code;
}

static void psci_system_off(void)
{
    register uint64_t x0 asm("x0") = PSCI_FN_SYSTEM_OFF;
    asm volatile("smc #0" : : "r"(x0) : "memory");
    /* Not reached when the monitor honors the call. */
    for (;;)
        ;
}

static void print_el(void)
{
    uint64_t el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("[M4] exception level: EL");
    uart_putc('0' + (char)((el >> 2) & 3));
    uart_puts("\r\n");
}

void kernel_arm64_main(void)
{
    uart_init();
    uart_puts("AeroSLS ARM64 Node Kernel Online!\r\n");
    uart_puts("[M4] minimal arm64 kernel (M4a+M4b) booted under qemu -M virt\r\n");
    print_el();
    arm64_boot_smoke_test();
    uart_puts("[M4] issuing PSCI SYSTEM_OFF\r\n");
    psci_system_off();
    /* PSCI should have powered us off; a return here is a bug. */
    for (;;)
        ;
}
