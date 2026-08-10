#ifndef SBI_H
#define SBI_H

#include <stdint.h>

// OpenSBI Extension ID (EID) and Function ID (FID) matching SBI v2.0 Specifications
#define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x01
#define SBI_EXT_0_1_CONSOLE_GETCHAR 0x02
#define SBI_EXT_DBCN                0x4442434E  // Debug Console Extension
#define SBI_DBCN_WRITE              0
#define SBI_DBCN_READ               1
#define SBI_EXT_SRST                0x53525354  // System Reset Extension ("SRST", SBI v0.3+)
#define SBI_SRST_RESET              0
#define SBI_SRST_RESET_TYPE_SHUTDOWN 0
#define SBI_SRST_RESET_REASON_NONE  0
/* Phase 9k: the timer's period, in `time` ticks. QEMU virt's timebase
 * is 10 MHz, so 10,000,000 ticks = 1 second. The timer is armed with
 * no firmware involvement in either mode: S-mode writes the stimecmp
 * CSR directly (the Sstc extension), M-mode programs the CLINT
 * mtimecmp MMIO — see arch/riscv/sbi.c's sbi_arm_timer. (The
 * SBI_SET_TIMER call shape was abandoned in S-mode: on this OpenSBI it
 * returns error 0 but never fires — documented at the csrw.)
 *
 * SBI_TIMER_TICKS_PER_SEC is the TIMEBASE constant — it converts the
 * `time` clock to wall-clock seconds and is used by the [TICK N Us]
 * uptime print, independent of the arm period. SBI_TIMER_TICK_PERIOD
 * is what each tick re-arms to: production builds tick once per
 * second; the echo builds tick every 100ms — the Phase 9n tick-vs-UART
 * contention probe, where the [TICK]/[SLICE] stream interleaves with
 * the [IRQ] tripwire 10x faster and the client asserts no protocol
 * bytes are lost and no re-arm is missed. */
#define SBI_TIMER_TICKS_PER_SEC     10000000UL
#if defined(KERNEL_UART_ECHO)
#define SBI_TIMER_TICK_PERIOD       1000000UL    /* Phase 9n: 100ms contention probe */
#else
#define SBI_TIMER_TICK_PERIOD       10000000UL   /* production: 1s */
#endif

struct SBIReturn {
    long error;
    long value;
};

// Raw architectural calling wrapper to pass parameters up to OpenSBI Machine Mode
static inline struct SBIReturn sbi_call(unsigned long ext, unsigned long fid, 
                                        unsigned long arg0, unsigned long arg1,
                                        unsigned long arg2) {
    struct SBIReturn ret;
    register unsigned long a0 __asm__("a0") = arg0;
    register unsigned long a1 __asm__("a1") = arg1;
    register unsigned long a2 __asm__("a2") = arg2;
    register unsigned long a7 __asm__("a7") = ext;
    register unsigned long a6 __asm__("a6") = fid;

    __asm__ volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a6), "r"(a7), "r"(a2)
        : "memory"
    );

    ret.error = a0;
    ret.value = a1;
    return ret;
}void sbi_putchar(char c);
int sbi_getchar(void);
void sbi_system_reset(void);
int sbi_arm_timer(uint64_t period_ticks);   /* Phase 9k: arm/re-arm the timer (stimecmp CSR in S-mode, CLINT mtimecmp in M-mode); always returns 0 */
void sbi_set_tick_period(uint64_t period_ticks);   /* Design B part 3: runtime tick-period override for the round-robin demo; 0 = the compile-time SBI_TIMER_TICK_PERIOD */

#endif
