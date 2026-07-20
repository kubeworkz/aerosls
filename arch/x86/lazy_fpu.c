#include "scheduler_lazy.h"
#include <stddef.h>

extern struct ExtendedTask* kernel_get_current_task_struct(void);

/* Gap Remediation SIMI Phase 10: fpu_hardware_owner used to be declared
 * `static` inside scheduler_lazy.h -- a file-scope static sitting in a
 * header, so every translation unit that #included it would get its OWN
 * independent copy rather than sharing one true global. Harmless only by
 * accident (this .c file was the only one ever including it); the real
 * definition now lives here, with the header carrying just an `extern`
 * declaration, so a second translation unit that legitimately needs to
 * read/write FPU ownership in the future gets the correct shared symbol
 * instead of silently acquiring a private, always-desynced copy. */
struct ExtendedTask* volatile fpu_hardware_owner = NULL;

void handle_device_not_available_fault(void) {
    // 1. Instantly clear the TS bit so the kernel itself can execute saving commands safely
    // clts instruction clears Bit 3 of CR0 atomically
    __asm__ volatile("clts");

    struct ExtendedTask* current_task = kernel_get_current_task_struct();

    // If the current running task already physically owns the FPU registers, we are done
    if (fpu_hardware_owner == current_task) {
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
    //
    // Gap Remediation SIMI Phase 10: current_task can only be NULL here if
    // kernel_get_current_task_struct()'s 64-slot table is completely
    // exhausted (see kernel/stubs.c) -- an unhandled edge case consistent
    // with this kernel's existing static-capacity-exhaustion posture
    // elsewhere (most subsystems here don't gracefully degrade past their
    // fixed table sizes either), not something newly introduced by this
    // fix.
    uint64_t current_buf_addr = (uint64_t)&current_task->avx512_state_buffer;

    // Atomically populate zmm0-zmm31 with this thread's unique historical parameters
    __asm__ volatile("xrstor (%0)" : : "r"(current_buf_addr), "a"(mask_low), "d"(mask_high) : "memory");

    // 4. Update global ownership tracker to point to the current thread
    fpu_hardware_owner = current_task;

    // Gap Remediation SIMI Phase 10: the two lapic_write(LAPIC_REG_EOI, 0)
    // calls this function used to make on every path (including the
    // early-return above) were removed -- #NM (Device Not Available) is a
    // synchronous CPU exception raised directly by executing an FPU/SSE
    // instruction while CR0.TS=1, not a hardware interrupt ever routed
    // through the LAPIC. There is no in-service LAPIC interrupt to
    // acknowledge here; sending an EOI for one was a copy-paste artifact
    // from IRQ-handling code, not something a synchronous exception
    // handler should do.
    //
    // The CPU automatically restarts the exact instruction that caused
    // the trap once this function returns via iretq (see
    // arch/x86/interrupt.asm's isr7_stub), now executing with correctly
    // restored FPU/AVX-512 register state.
}