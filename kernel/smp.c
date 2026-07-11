#include <stdint.h>
#include <stddef.h>
#include "../arch/x86/lapic.h"

extern void* allocate_physical_ram_frame(void);
extern void ap_kernel_main(void); // Defined in our C main code tree
extern void sls_flush_daemon_loop(void);
extern void kernel_sleep_ticks(uint32_t ticks);

extern uint8_t trampoline_start;
extern uint8_t trampoline_end;
extern uint64_t gdt_ptr;

// Global tracking structure used for multicore handshakes
volatile uint32_t ap_bootstrap_lock = 0;

void boot_application_processors(uint8_t target_apic_id) {
    // 1. Copy our flat assembly binary payload to physical target location 0x08000
    uint8_t* dest = (uint8_t*)0x08000;
    uint8_t* src  = &trampoline_start;
    size_t size   = &trampoline_end - &trampoline_start;
    for (size_t i = 0; i < size; i++) dest[i] = src[i];

    // 2. Populate communication variables used by the trampoline script
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    *(volatile uint32_t*)(0x07000) = (uint32_t)current_cr3;
    *(volatile uint64_t*)(0x07010) = (uint64_t)&gdt_ptr; // Passes structural pointer base
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

    // Wait for the AP to safely enter ap_kernel_main and clear the lock flag
    while (ap_bootstrap_lock == 0) {
        __asm__ volatile("pause");
    }
}

// Executed concurrently by Core 1 and Core 2 when they leave the trampoline
void ap_kernel_main(void) {
    // Reload local core segment references
    init_local_apic_registers(); 
    
    // Atomically signal the BSP that this core has initialized successfully
    __atomic_store_n(&ap_bootstrap_lock, 1, __ATOMIC_SEQ_CST);

    // Enter our thread scheduler round-robin execution processing loops
    sls_flush_daemon_loop();
}