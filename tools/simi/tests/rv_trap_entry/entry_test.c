/* entry_test.c — Phase 9h (ISA doc §16): the C half of the trap-entry
 * fixture (tools/simi/tests/rv_trap_entry/). Provides the fixture's
 * RvPerHartData, the pre-trap observation globals, the console, and —
 * most importantly — the verification STUB the real entry calls:
 * riscv_trap_dispatch_m() in the default (M-mode) build,
 * riscv_trap_dispatch() when compiled -DFIXTURE_SMODE. Same source,
 * four builds — exactly the kernel's own RISCV_MMODE pattern.
 *
 * The stub runs INSIDE the trap (the entry switched sp to
 * phd->kernel_sp before calling it, a0 = &phd) and checks the
 * just-saved frame against the distinctive register values entry_test.S
 * loaded before the ebreak — proving the entry's save offsets — plus
 * sp and the exception PC. It then advances the exception PC past the
 * ebreak and RETURNS, which is the one thing the real kernel's
 * dispatch never does: every kernel trap either halts (unhandled
 * exception, M-mode exit) or powers the machine off (S-mode exit), so
 * the entry's restore+return half (the `ld` sequence, the PC restore,
 * the sscratch re-arm, the sp restore, sret/mret) has never executed
 * until this fixture.
 *
 * The real kernel's dispatcher is deliberately NOT linked; the entry's
 * `call` resolves to this stub. The S-mode build is booted under
 * OpenSBI (stvec/sepc/sret, payload at 0x80200000, SBI_DBCN console);
 * the M-mode build is a direct -bios none payload (mtvec/mepc/mret at
 * 0x80000000, raw 16550 console).
 *
 * Compiled freestanding with -msmall-data-limit=0 -mno-relax (see the
 * Makefile target): the driver pins gp/tp to junk values across the
 * trap, so no compiled code may address data through gp, and nothing
 * may be relaxed into gp-relative form.
 */
#include <stdint.h>
#include "../../../../arch/riscv/trap_riscv.h"

/* The fixture's per-hart data. The entry (trap_riscv.S) swaps sscratch
 * into sp and saves the frame here; kernel_sp (offset 256) is set by
 * _start in entry_test.S. Every one of the 32 frame slots is written by
 * the entry itself, so the fixture needs no .bss zeroing for it. */
struct RvPerHartData g_phd;

/* What the trap must observe, recorded by _start right before the
 * ebreak (entry_test.S). */
uint64_t g_expected_sepc;
uint64_t g_pre_trap_sp;

/* The post-mret register dump (x1..x31, sp excluded): captured by the
 * driver IMMEDIATELY after mret, before any check clobbers a register,
 * so the C side can report exactly which one failed to round-trip.
 * Index order = the capture stores in entry_test.S: 0=ra, 1=gp, 2=tp,
 * 3=t0, 4=t1, 5=t2, 6=s0, 7=s1, 8=a0, ... 26=t3, 27=t4, 28=t5, 29=t6
 * (x2/sp omitted -- it is not pinable). Two slots are EXPECTED to
 * mismatch the constants and are skipped: index 3 (t0 -- the entry's
 * sp-restore scratch, left holding the pre-trap sp) and index 28 (t5 --
 * the capture pointer). */
uint64_t g_reg_dump[30];

/* Console. The M-mode build (no firmware) writes the 16550 UART at the
 * QEMU virt MMIO base directly — the same console the M-mode kernel
 * uses (arch/riscv/sbi.c's RISCV_MMODE block). The S-mode build runs
 * under OpenSBI, where the legacy console-putchar ecall was REMOVED
 * (>= 0.9, silently dropped — a mute fixture would look like a hang),
 * so it prints through the Debug Console extension (SBI_DBCN) exactly
 * like the S-mode kernel's sbi_putchar does. */
#ifdef FIXTURE_SMODE
static void uart_putchar(char c) {
    /* SBI_DBCN write, spec-correct layout (SBI v2.0 4.2): a0=num_bytes,
     * a1=base_lo, a2=base_hi=0, a3=count_lo, a4=count_hi=0. The kernel
     * used to pass the count pointer in a2 (base_hi) -- DBCN always
     * errored and printing rode on the legacy fallback; this fixture
     * exposed it (ISA doc §16 Phase 9h) and arch/riscv/sbi.c now uses
     * this same layout. Legacy putchar kept as a fallback, mirroring
     * the kernel. */
    char buf = c;
    unsigned long written = 0;
    register unsigned long a0 __asm__("a0") = 1;
    register unsigned long a1 __asm__("a1") = (unsigned long)&buf;
    register unsigned long a2 __asm__("a2") = 0;             /* base_hi */
    register unsigned long a3 __asm__("a3") = (unsigned long)&written;
    register unsigned long a4 __asm__("a4") = 0;             /* count_hi */
    register unsigned long a6 __asm__("a6") = 0;             /* DBCN_WRITE */
    register unsigned long a7 __asm__("a7") = 0x4442434EUL;  /* SBI_EXT_DBCN */
    __asm__ volatile("ecall"
                     : "+r"(a0), "+r"(a1)
                     : "r"(a2), "r"(a3), "r"(a4), "r"(a6), "r"(a7)
                     : "memory");
    /* Only the return is trusted: OpenSBI v1.3 emits the byte with
     * error 0 but does not reliably write the count back to a3, so
     * checking written would double-print via the fallback. */
    if (a0 == 0) return;
    /* Legacy console putchar (SBI v0.1) fallback. */
    __asm__ volatile("mv a0, %0\n\tli a7, 1\n\tecall"
                     : : "r"((unsigned long)(unsigned char)c)
                     : "a0", "a7", "memory");
}
#else
#define VIRT_UART_BASE 0x10000000UL
static void uart_putchar(char c) {
    volatile uint8_t* lsr = (volatile uint8_t*)(VIRT_UART_BASE + 5);
    volatile uint8_t* thr = (volatile uint8_t*)(VIRT_UART_BASE + 0);
    while ((*lsr & 0x20) == 0) { }   /* LSR bit 5: THR empty — wait for it */
    *thr = (uint8_t)c;
}
#endif

void uart_print(const char* s) {
    while (*s) uart_putchar(*s++);
}

#ifdef FIXTURE_SMODE_INTR
#define FIXTURE_MODE_TAG " (S-mode, interrupt)"
#elif defined(FIXTURE_SMODE_TIMER)
#define FIXTURE_MODE_TAG " (S-mode, timer)"
#elif defined(FIXTURE_SMODE)
#define FIXTURE_MODE_TAG " (S-mode)"
#else
#define FIXTURE_MODE_TAG ""
#endif

static void uart_udec(uint64_t v) {
    char buf[20];
    int i = 0;
    if (v == 0) { uart_putchar('0'); return; }
    while (v > 0 && i < 20) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) uart_putchar(buf[--i]);
}

static void halt_forever(void) {
    for (;;) { __asm__ volatile("wfi"); }
}

/* Prints the fail-path register dump (see g_reg_dump) and halts. The
 * expected value for the dump slot holding TF index i is 0x1000+i (the
 * constants entry_test.S loaded before the ebreak). */
void dump_and_halt(void) {
    static const uint64_t exp[30] = {
        0x1000, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008,
        0x1009, 0x100A, 0x100B, 0x100C, 0x100D, 0x100E, 0x100F, 0x1010,
        0x1011, 0x1012, 0x1013, 0x1014, 0x1015, 0x1016, 0x1017, 0x1018,
        0x1019, 0x101A, 0x101B, 0x101C, 0x101D, 0x101E
    };
    static const char* names[30] = {
        "ra", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
        "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9",
        "s10", "s11", "t3", "t4", "t5", "t6"
    };
    uart_print("[FIXTURE] FAIL: register round-trip dump (x1..x31, sp omitted)\n");
    for (int i = 0; i < 30; i++) {
        if (i == 3) continue;    /* t0: the entry's sp-restore scratch, left holding the pre-trap sp */
        if (i == 28) continue;   /* t5: the capture pointer, not its post-mret value */
        if (g_reg_dump[i] != exp[i]) {
            uart_print("  ");
            uart_print(names[i]);
            uart_print(": got ");
            uart_udec(g_reg_dump[i]);
            uart_print(" want ");
            uart_udec(exp[i]);
            uart_print("\n");
        }
    }
    /* Also show the frame slots for the two mismatching registers, to
     * distinguish a broken restore (frame intact) from a corrupted
     * frame (frame modified between save and restore). */
    uart_print("[FIXTURE] frame slots: tp(3)=");
    uart_udec(g_phd.trap_frame[3]);
    uart_print(" t4(28)=");
    uart_udec(g_phd.trap_frame[28]);
    uart_print(" sepc(31)=");
    uart_udec(g_phd.trap_frame[31]);
    uart_print(" sp(1)=");
    uart_udec(g_phd.trap_frame[1]);
    uart_print("\n");
    halt_forever();
}

/* The verification stub the entry calls with a0 = &phd. Runs on the
 * fixture's kernel stack (phd->kernel_sp = _stack_top, set by _start).
 * The real entry's `call` resolves to whichever name the build's mode
 * picks: riscv_trap_dispatch under FIXTURE_SMODE (sepc/sret), else
 * riscv_trap_dispatch_m (mepc/mret). */
#ifdef FIXTURE_SMODE
#define FIXTURE_DISPATCH_MAIN  riscv_trap_dispatch
#define FIXTURE_DISPATCH_OTHER riscv_trap_dispatch_m
#else
#define FIXTURE_DISPATCH_MAIN  riscv_trap_dispatch_m
#define FIXTURE_DISPATCH_OTHER riscv_trap_dispatch
#endif

void FIXTURE_DISPATCH_MAIN(struct RvPerHartData* phd) {
    /* TF index i (i != TF_SP, i != TF_SEPC) must hold 0x1000+i — the
     * distinctive value entry_test.S loaded before the ebreak. */
    static const uint64_t exp[TF_COUNT] = {
        0x1000, 0,        0x1002, 0x1003, 0x1004, 0x1005, 0x1006,
        0x1007, 0x1008,   0x1009, 0x100A, 0x100B, 0x100C, 0x100D,
        0x100E, 0x100F,   0x1010, 0x1011, 0x1012, 0x1013, 0x1014,
        0x1015, 0x1016,   0x1017, 0x1018, 0x1019, 0x101A, 0x101B,
        0x101C, 0x101D,   0x101E, 0
    };
    for (int i = 0; i < TF_COUNT; i++) {
        if (i == TF_SP || i == TF_SEPC) continue;
        if (phd->trap_frame[i] != exp[i]) {
            uart_print("[FIXTURE] FAIL: dispatcher frame slot ");
            uart_udec((uint64_t)i);
            uart_print(": got ");
            uart_udec(phd->trap_frame[i]);
            uart_print(" want ");
            uart_udec(exp[i]);
            uart_print("\n");
            halt_forever();
        }
    }
    if (phd->trap_frame[TF_SP] != g_pre_trap_sp) {
        uart_print("[FIXTURE] FAIL: dispatcher sp check\n");
        halt_forever();
    }
    if (phd->trap_frame[TF_SEPC] != g_expected_sepc) {
        uart_print("[FIXTURE] FAIL: dispatcher sepc check\n");
        halt_forever();
    }

    uart_print("[FIXTURE] dispatcher" FIXTURE_MODE_TAG ": frame save verified\n");

#if defined(FIXTURE_SMODE_TIMER)
    /* Timer build: acknowledge by moving mtimecmp to the far future —
     * SBI_SET_TIMER with 2^64-1 (mtime on the QEMU virt machine runs
     * at 10 MHz, so that is effectively never). Timer interrupts are
     * LEVEL-SENSITIVE: STIP stays pending until mtimecmp > mtime, and
     * sret restores SIE from SPIE (re-enabling interrupts), so a
     * still-pending STIP would re-trap immediately after the return.
     * Setting mtimecmp to the future clears STIP — the device-driven
     * counterpart of the SI build's csrc sip. The trap is NOT
     * advanced: interrupts leave sepc untouched, exactly like the SI
     * twin. */
    __asm__ volatile(
        "li a0, -1\n\t"
        "li a1, -1\n\t"
        "li a6, 0\n\t"
        "li a7, 0\n\t"
        "ecall"
        ::: "a0", "a1", "a6", "a7", "memory");
#elif defined(FIXTURE_SMODE_INTR)
    /* Interrupt build: interrupts do NOT advance the exception PC, so
     * the entry must resume at the SAME instruction (intr_site) — the
     * whole point of this variant is proving the restore path with an
     * un-advanced sepc. Also acknowledge the software interrupt (clear
     * SSIP): sret restores SIE from SPIE (re-enabling interrupts), so
     * a still-pending SSIP would re-trap immediately after the return. */
    __asm__ volatile("csrc sip, 2");
#else
    /* Advance the exception PC past the ebreak — the contract the
     * kernel's own dispatch keeps (see riscv_trap_dispatch_common's
     * code==3/9 branches) but never exercises, because it never
     * returns. The entry restores this value into the exception-PC CSR
     * and sret/mret resumes at ebreak+4, letting the fixture's
     * post-trap checks run. */
    phd->trap_frame[TF_SEPC] += 4;
#endif
}

/* The OTHER entry (the one this build does NOT arm) sits in the SAME
 * .text section as the armed one (trap_riscv.S emits both), so
 * --gc-sections keeps both; this stub satisfies the link. The fixture
 * only arms one vector, so it is never called. */
void FIXTURE_DISPATCH_OTHER(struct RvPerHartData* phd) {
    (void)phd;
#ifdef FIXTURE_SMODE
    uart_print("[FIXTURE] FAIL: M-mode dispatcher called (mtvec was never armed)\n");
#else
    uart_print("[FIXTURE] FAIL: S-mode dispatcher called (stvec was never armed)\n");
#endif
    halt_forever();
}
