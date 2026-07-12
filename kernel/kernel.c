// kernel.c — AeroSLS BSP entry point and system bootstrap

#include <stdint.h>
#include "kernel_io.h"
#include "scheduler.h"
#include "microkernel.h"
#include "../arch/x86/gdt.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "../arch/x86/isr_stubs.h"
#include "timer.h"

extern void sls_shell_loop(void);
extern void boot_application_processors(uint8_t apic_id);

#ifdef __cplusplus
extern "C"
#endif
void kernel_main(void) {

    // ── 1. Serial output — must be first so all subsequent prints are visible ──
    serial_init();
    kernel_serial_print(
        "[AEROSLS BOOT LOGGER V1.0.0 RUNNING]\n"
        "----------------------------------------------------------------------------------\n");

    // ── 2. CPU descriptor tables ───────────────────────────────────────────────
    kernel_serial_print("[BSP] Loading GDT and IDT...\n");
    init_gdt();
    init_idt();

    // ── 3. Local APIC + timer IRQ ──────────────────────────────────────────
    init_local_apic_registers();
    init_timer();
    kernel_serial_print("[BSP] LAPIC and IRQ0 timer online.\n");

    // ── 4. Scheduler ──────────────────────────────────────────────────────
    init_scheduler();
    kernel_serial_print("[BSP] Round-robin task scheduler initialised.\n");

    // ── 5. Microkernel (IPC + 5 services + tier manager) ──────────────────
    microkernel_init();

    // ── 6. Boot Application Processor (Core 1) ─────────────────────────
    boot_application_processors(1);

    kernel_serial_print(
        "----------------------------------------------------------------------------------\n"
        "[SLS] System ready. Launching secure shell...\n"
        "----------------------------------------------------------------------------------\n");

    // ── 7. Shell (does not return) ─────────────────────────────────────
    sls_shell_loop();
}