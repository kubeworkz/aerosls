#include <stdint.h>
#include <stddef.h>

#define SSTATUS_VS_MASK  (3ULL << 9)
#define SSTATUS_VS_OFF   (0ULL << 9)
#define SSTATUS_VS_DIRTY (3ULL << 9)

struct RISCVVectorTask {
    uint32_t id;
    uint64_t saved_sp;
    int      vector_used;
    // Buffer size depends on VLEN (e.g., 32 registers * 128-bit VLEN = 512 bytes)
    __attribute__((aligned(128))) uint8_t vector_state_buffer[512];
};

static struct RISCVVectorTask* volatile rvv_hardware_owner = NULL;

extern struct RISCVVectorTask* kernel_get_current_task(void);

// Invoked directly when an Illegal Instruction Exception (scause = 2) 
// is trapped because a thread attempted a vector operation while sstatus.VS was OFF
void handle_riscv_vector_disabled_trap(uint64_t* registers) {
    struct RISCVVectorTask* current = kernel_get_current_task();

    uint64_t sstatus_val;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_val));

    // 1. Temporarily turn on Supervisor Vector configurations so the kernel can execute vector saves
    sstatus_val &= ~SSTATUS_VS_MASK;
    sstatus_val |= (2ULL << 9); // Set VS = Initial/Clean to unlock vector assembly instructions
    __asm__ volatile("csrw sstatus, %0" : : "r"(sstatus_val));

    // 2. If a different thread holds dirty data in the hardware registers, save it out
    if (rvv_hardware_owner != NULL && rvv_hardware_owner != current) {
        uint64_t old_buf = (uint64_t)rvv_hardware_owner->vector_state_buffer;
        
        // Custom assembly helper to execute low-overhead stream writes (vse64.v)
        riscv_vector_save_state(old_buf);
        
        // Reset old owner's tracking status
        rvv_hardware_owner->vector_used = 1;
    }

    // 3. Load the current thread's historical encryption keys into v0-v31
    if (current->vector_used) {
        uint64_t new_buf = (uint64_t)current->vector_state_buffer;
        riscv_vector_load_state(new_buf);
    } else {
        // Zero-state initialization if the thread is using vector operations for the first time
        __asm__ volatile("vsetvli t0, zero, e8, m1, ta, ma"); // Set vector configuration bounds
        __asm__ volatile("vxor.vv v0, v0, v0");               // Clear out lanes safely
    }

    // 4. Update core trackers and hand permanent execution ownership over to this task
    rvv_hardware_owner = current;

    // Clear trap states: Leave sstatus.VS set to Clean/Dirty so the instruction can retry natively
    return;
}