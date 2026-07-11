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

#endif /* ISR_STUBS_H */
