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
#include "process.h"
#include "loader.h"
#include "../kernel/webapp.h"
#include "../kernel/auth.h"
#include "../arch/x86/user_paging.h"
#include "../net/net.h"
#include "../net/e1000.h"
#include "../net/http.h"

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

    // ── 4b. SYSCALL/SYSRET gate ───────────────────────────────────────────
    syscall_gate_init();   // configure STAR/LSTAR/SFMASK/EFER, per-CPU GS

    // ── 4c. Process manager ────────────────────────────────────────────
    process_init();

    // ── 4d. Service binary loader ───────────────────────────────────────────
    loader_init();

    // ── 4e. Web asset store ────────────────────────────────────────────────
    webapp_init();   // installs built-in Navigator welcome page

    // ── 4f. Token authentication registry ─────────────────────────────────
    auth_init();     // registers 4 demo tokens; prints them to serial log

    // ── 5. Microkernel (IPC + 5 services + tier manager) ──────────────────
    microkernel_init();

    // ── 6. Boot Application Processor (Core 1) ─────────────────────────
    boot_application_processors(1);

    // ── 7. Network stack  ───────────────────────────────────────────────────────
    // e1000_mmio_base is discovered by the PCI scan in a real boot;
    // for QEMU user-mode networking the default BAR0 is 0xFEBC0000.
    // Leave init conditional so the kernel boots without a NIC too.
    if (e1000_mmio_base) {
        e1000_init(e1000_mmio_base);
        net_init();   // also sends gratuitous ARP
        kernel_serial_print("[NET] e1000 RX/TX rings online.\n");
    } else {
        kernel_serial_print("[NET] No e1000 base address — network disabled.\n"
                            "      Set e1000_mmio_base before boot for HTTP API.\n");
    }

    kernel_serial_print(
        "----------------------------------------------------------------------------------\n"
        "[SLS] System ready. Launching secure shell...\n"
        "----------------------------------------------------------------------------------\n");

    // ── 8. HTTP server runs on Core 1 alongside the flush/service loop ─────────
    // In the current build the BSP runs the shell; Core 1 already runs
    // microkernel_service_poll() from smp.c.  Kick the HTTP server on the BSP
    // as a foreground loop only if the NIC is present; otherwise fall through
    // to the shell.
    if (e1000_mmio_base) {
        http_server_run();  // does not return — serves REST API on port 3000
    }

    // ── Shell (does not return) ─────────────────────────────────────────────
    sls_shell_loop();
}