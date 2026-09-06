#include <stdint.h>
#include <stddef.h>
#include "../arch/x86/lapic.h"
#include "microkernel.h"
#include "smp.h"
#include "console_service.h"  /* deferred uniprocessor console drain */

extern void* allocate_physical_ram_frame(void);
extern void ap_kernel_main(void);
extern void flush_daemon_tick(void);
extern void qemu_sls_pgo_scan_tick(void);
extern void kernel_sleep_ticks(uint32_t ticks);
extern void kernel_serial_print(const char* s);
extern void kernel_serial_printf(const char* fmt, ...);
extern volatile uint64_t kernel_tick_counter;

extern uint8_t trampoline_start;
extern uint8_t trampoline_end;
extern uint64_t gdt_ptr;

// Global tracking structure used for multicore handshakes
volatile uint32_t ap_bootstrap_lock = 0;

int boot_application_processors(uint8_t target_apic_id) {
    // 1. Copy our flat assembly binary payload to physical target location 0x08000
    uint8_t* dest = (uint8_t*)0x08000;
    uint8_t* src  = &trampoline_start;
    size_t size   = &trampoline_end - &trampoline_start;
    for (size_t i = 0; i < size; i++) dest[i] = src[i];

    // 2. Populate communication variables used by the trampoline script
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    *(volatile uint32_t*)(0x07000) = (uint32_t)current_cr3;

    // The trampoline does `lgdt [0x07010]` which reads the struct CONTENTS
    // at 0x07010, NOT a pointer to the struct.  Copy the 10-byte GDTPointer
    // struct directly into the handshake area.
    {
        volatile uint8_t* dst = (volatile uint8_t*)0x07010;
        const uint8_t* src = (const uint8_t*)&gdt_ptr;
        for (int gi = 0; gi < 10; gi++) dst[gi] = src[gi];
    }

    *(volatile uint64_t*)(0x07020) = (uint64_t)ap_kernel_main;

    // Allocate an isolated 4KB stack space for the incoming AP thread
    void* ap_stack = allocate_physical_ram_frame();
    *(volatile uint64_t*)(0x07030) = (uint64_t)ap_stack + 4096; // Stack grows downwards

    // Set lock token to intercept the incoming core bootup completion loop
    ap_bootstrap_lock = 0;

    // 3. Issue the INIT IPI command sequence via the Interrupt Command Registers (ICR)
    // Select targeted APIC ID, specify Init delivery mode (0x500), assert edge trigger
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,  0x00004500);

    kernel_sleep_ticks(10); // Wait 10ms for hardware initialization loops

    // 4. Issue the STARTUP IPI (SIPI) command sequence
    // Vector 0x08 maps directly down to address location: 0x08 * 4096 = 0x08000
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW,  0x00004608); 

    /* Wait for the AP to enter ap_kernel_main and set the flag -- but with
     * a deadline. This was an unbounded `while (ap_bootstrap_lock == 0)`
     * spin, which meant that booting with `-smp 1` (no APIC id 1 to answer
     * the SIPI) hung the kernel here forever, silently, before any of the
     * subsystems below had started. A machine with one CPU is a supported
     * configuration; failing to find a second one is not an error. */
    uint64_t deadline = kernel_tick_counter + AP_BOOT_TIMEOUT_TICKS;
    while (ap_bootstrap_lock == 0) {
        if (kernel_tick_counter >= deadline) {
            kernel_serial_printf(
                "[SMP] no AP answered at APIC id %u within %u ticks -- running "
                "UNIPROCESSOR; the BSP will drive the service loop itself.\n",
                (unsigned)target_apic_id, (unsigned)AP_BOOT_TIMEOUT_TICKS);
            return 0;
        }
        __asm__ volatile("pause");
    }
    kernel_serial_printf("[SMP] AP at APIC id %u online.\n",
                         (unsigned)target_apic_id);
    return 1;
}

int smp_ap_online(void) {
    return __atomic_load_n(&ap_bootstrap_lock, __ATOMIC_SEQ_CST) != 0;
}

void smp_uniprocessor_tick(void) {
    /* Checked live, not latched at boot: if an AP turns up late -- past the
     * timeout -- it resumes ownership of this work and the BSP stops, so
     * the two can never both be driving it. */
    if (smp_ap_online()) return;

    static uint64_t next_due = 0;
    if (kernel_tick_counter < next_due) return;
    next_due = kernel_tick_counter + SMP_UNI_TICK_INTERVAL;

    flush_daemon_tick();
    microkernel_service_poll();
    qemu_sls_pgo_scan_tick();
    /* Uniprocessor fallback for the console drain: the timer ISR only
     * latches console_service_irq_defer()'s pending flag (running the
     * drain inside the IRQ deadlocks on cap spinlocks — see
     * console_service.h). This is the process-context consumer; on SMP
     * boots the AP's microkernel_service_poll already covers the drain. */
    console_service_deferred_tick();
}

// Executed concurrently by Core 1 and Core 2 when they leave the trampoline
void ap_kernel_main(void) {
    // Reload local core segment references
    init_local_apic_registers();

    // Atomically signal the BSP that this core has initialized successfully
    __atomic_store_n(&ap_bootstrap_lock, 1, __ATOMIC_SEQ_CST);

    // Combined loop: dirty-page flush + microkernel service bus poll
    while (1) {
        flush_daemon_tick();
        microkernel_service_poll();
        qemu_sls_pgo_scan_tick();
        kernel_sleep_ticks(10);
    }
}