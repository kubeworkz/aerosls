#ifndef ISR_STUBS_H
#define ISR_STUBS_H

/*
 * Assembly-defined ISR entry stubs (arch/x86/interrupt.asm).
 *
 * Each stub saves caller-saved registers, dispatches to the corresponding
 * C handler, restores registers, and executes iretq.  Declaring them here
 * gives the C compiler and language-server a single authoritative source
 * of truth for the assembly/C interface boundary.
 */

extern void isr14_stub(void);   /* Page Fault (Exception 14)          */
extern void isr32_stub(void);   /* Timer IRQ0 — LAPIC / PIT (Vector 32) */
extern void isr6_stub(void);    /* Invalid Opcode (#UD, Exception  6)  */
extern void isr7_stub(void);    /* Device Not Available (#NM, Exception 7) —
                                  * lazy FPU/AVX-512 trap, Gap Remediation
                                  * SIMI Phase 10. Dispatches to
                                  * handle_device_not_available_fault()
                                  * (arch/x86/lazy_fpu.c), NOT the generic
                                  * handle_ring3_fault() path every other
                                  * stub here uses — see isr7_stub's own
                                  * comment in interrupt.asm. */
extern void isr11_stub(void);   /* Segment Not Present (#NP, Exc 11)   */
extern void isr12_stub(void);   /* Stack-Segment Fault (#SS, Exc 12)   */
extern void isr13_stub(void);   /* General Protection Fault (#GP, Exc 13) */

#endif /* ISR_STUBS_H */
