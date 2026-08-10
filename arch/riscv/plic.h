#ifndef PLIC_H
#define PLIC_H

#include <stdint.h>

/* Phase 9i: the QEMU virt PLIC driver — the device-driven interrupt path
 * for the S-mode kernel. init_riscv_plic() is called at boot (S-mode
 * build only) to route the 16550 UART's interrupt line (source 10) to
 * this hart's S-mode context; the trap handler (sbi.c's
 * handle_riscv_supervisor_interrupt) claims and completes through the
 * same driver. See AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9i. */
void init_riscv_plic(uint32_t target_hart_id);
uint32_t plic_claim_interrupt(uint32_t target_hart_id);
void plic_complete_interrupt(uint32_t target_hart_id, uint32_t irq);

#endif
