#include "scheduler_lazy.h"
#include "fpu_enable.h"   /* the CR0/CR4 SSE bit arithmetic — pure, host-
                            * tested by tests/fpu_enable_host_test.c */
#include <stddef.h>

/* Declared here rather than via kernel_io.h, the pattern arch/x86's and
 * kernel/'s other early-init files already use (boot_params.c, smp.c). */
extern void kernel_serial_print(const char* s);

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

/* ─── SSE/FP enable for ring 3 — §10.13's SSE gate ───────────────────────
 * Measured (design doc §10.13, step-1 run): the designated first user got
 * past the loader and reached `main`, then died inside musl's `abort()` at
 * `pxor %xmm0,%xmm0` — rip=0x80525b0, `[FAULT] Ring-3 fault cs=0x23
 * error=0x0` — because boot.asm writes CR4.PAE and nothing else, so
 * CR4.OSFXSR (which makes SSE legal at all) and CR4.OSXMMEXCPT were never
 * written anywhere in the tree. Every real libc and every Rust binary uses
 * vector code, so this gate sat in front of §5.6 entirely.
 *
 * Order is load-bearing: CR4.OSFXSR before `ldmxcsr` (an SSE instruction,
 * itself #UD without it), CR0's EM/TS cleared before `fninit` (x87 #UD with
 * EM, #NM with TS). CPUID.1 gates the whole thing so a CPU without SSE gets
 * a refusal line instead of a #GP on the CR4 write — under -no-reboot that
 * would be a silent triple fault.
 *
 * What stays OUT, deliberately: CR0.TS is never armed here, so #NM never
 * fires and the lazy-FPU path below (xsave/xrstor, which would themselves
 * need CR4.OSXSAVE) remains dormant — it has no caller in the switch path
 * today (perform_lazy_context_switch() is referenced by nothing). The cost
 * of that choice is that XMM state is NOT saved per task across a switch;
 * it is named as a gap rather than papered over. */
void fpu_enable_sse(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint64_t cr0 = 0, cr4 = 0;
    static const uint32_t mxcsr_masked = FPU_SSE_MXCSR_DEFAULT;  /* all
                            * SIMD exceptions masked — the SDM reset value */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u) : "memory");
    (void)eax; (void)ebx; (void)ecx;
    if (!fpu_cpuid_has_sse(edx)) {
        kernel_serial_print("[FPU] CPUID.1:EDX reports no SSE/SSE2 — ring-3 "
                            "vector code stays unavailable (refused by name)\n");
        return;
    }
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 = fpu_cr4_enable_sse(cr4);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = fpu_cr0_enable_fpu(cr0);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("fninit");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr_masked) : "memory");
    kernel_serial_print("[FPU] SSE/SSE2 enabled for ring 3 — CR4.OSFXSR|"
                        "OSXMMEXCPT, CR0.MP; EM/TS clear, CR0.TS not armed\n");
}