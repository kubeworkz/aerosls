#include "scheduler_lazy.h"
#include <stddef.h>

extern struct ExtendedTask* kernel_get_current_task_struct(void);
#include "lapic.h"

void handle_device_not_available_fault(void) {
    // 1. Instantly clear the TS bit so the kernel itself can execute saving commands safely
    // clts instruction clears Bit 3 of CR0 atomically
    __asm__ volatile("clts");

    struct ExtendedTask* current_task = kernel_get_current_task_struct();
    
    // If the current running task already physically owns the FPU registers, we are done
    if (fpu_hardware_owner == current_task) {
        lapic_write(LAPIC_REG_EOI, 0);
        return;
    }

    uint32_t mask_low = 0xFFFFFFFF;
    uint32_t mask_high = 0xFFFFFFFF;

    // 2. If a separate task owns the hardware registers, save its active state to its buffer
    if (fpu_hardware_owner != NULL) {
        uint64_t state_buf_addr = (uint64_t)&fpu_hardware_owner->avx512_state_buffer;
        
        // Save the old owner's AVX-512 register values to their isolated memory block
        __asm__ volatile("xsave (%0)" : : "r"(state_buf_addr), "a"(mask_low), "d"(mask_high) : "memory");
    }

    // 3. SECURE RESTORE: Load the current thread's vector states into the active ZMM registers
    uint64_t current_buf_addr = (uint64_t)&current_task->avx512_state_buffer;
    
    // Atomically populate zmm0-zmm31 with this thread's unique historical parameters
    __asm__ volatile("xrstor (%0)" : : "r"(current_buf_addr), "a"(mask_low), "d"(mask_high) : "memory");

    // 4. Update global ownership tracker to point to the current thread
    fpu_hardware_owner = current_task;

    // Acknowledge LAPIC interrupt delivery lines
    lapic_write(LAPIC_REG_EOI, 0);
    
    // The CPU automatically restarts the exact vector instruction that caused the trap, 
    // now executing with perfectly restored and completely secure hardware register values.
}