/* entry_test.c — Phase 9h (ISA doc §16): the C half of the bare-metal
 * trap-entry fixture (tools/simi/tests/rv_trap_entry/). Provides the
 * fixture's RvPerHartData, the pre-trap observation globals, the 16550
 * UART console, and — most importantly — riscv_trap_dispatch_m(), the
 * verification STUB the real entry calls.
 *
 * The stub runs INSIDE the trap (the entry switched sp to
 * phd->kernel_sp before calling it, a0 = &phd) and checks the
 * just-saved frame against the distinctive register values entry_test.S
 * loaded before the ebreak — proving the entry's save offsets — plus
 * sp and mepc. It then advances the exception PC past the ebreak and
 * RETURNS, which is the one thing the real kernel's dispatch never does:
 * every kernel trap either halts (unhandled exception, M-mode exit) or
 * powers the machine off (S-mode exit), so the entry's restore+return
 * half (the `ld` sequence, the mepc restore, the sscratch re-arm, the
 * sp restore, mret) has never executed until this fixture.
 *
 * The real kernel's riscv_trap_dispatch_m is deliberately NOT linked;
 * the entry's `call riscv_trap_dispatch_m` resolves to this stub.
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

/* 16550 UART at the QEMU virt machine's MMIO base — the same console
 * the M-mode kernel uses (arch/riscv/sbi.c's RISCV_MMODE block). */
#define VIRT_UART_BASE 0x10000000UL

static void uart_putchar(char c) {
    volatile uint8_t* lsr = (volatile uint8_t*)(VIRT_UART_BASE + 5);
    volatile uint8_t* thr = (volatile uint8_t*)(VIRT_UART_BASE + 0);
    while ((*lsr & 0x20) == 0) { }   /* LSR bit 5: THR empty — wait for it */
    *thr = (uint8_t)c;
}

void uart_print(const char* s) {
    while (*s) uart_putchar(*s++);
}

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
 * fixture's kernel stack (phd->kernel_sp = _stack_top, set by _start). */
void riscv_trap_dispatch_m(struct RvPerHartData* phd) {
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

    uart_print("[FIXTURE] dispatcher: frame save verified\n");

    /* Advance the exception PC past the ebreak — the contract the
     * kernel's own dispatch keeps (see riscv_trap_dispatch_common's
     * code==3/9 branches) but never exercises, because it never
     * returns. The entry restores this value into mepc and mret resumes
     * at ebreak+4, letting the fixture's post-trap checks run. */
    phd->trap_frame[TF_SEPC] += 4;
}

/* The S-mode entry (riscv_trap_entry, stvec) sits in the SAME .text
 * section as the M-mode one (trap_riscv.S), so --gc-sections keeps
 * both; this stub satisfies the link. The fixture only arms mtvec
 * (entry_m), so this is never called. */
void riscv_trap_dispatch(struct RvPerHartData* phd) {
    (void)phd;
    uart_print("[FIXTURE] FAIL: S-mode dispatcher called (stvec was never armed)\n");
    halt_forever();
}
