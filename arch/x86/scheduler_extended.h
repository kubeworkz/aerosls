#include <stdint.h>

enum TaskState {
    TASK_READY = 0,
    TASK_RUNNING = 1,
    TASK_BLOCKED = 2,
    TASK_TERMINATED = 3
};

struct ExtendedTask {
    uint32_t id;
    enum TaskState state;
    uint64_t rsp; // Standard saved kernel stack layout reference
    
    // Forced 64-byte alignment is strictly mandatory for the XSAVE target memory area
    __attribute__((aligned(64))) uint8_t avx512_state_buffer[2688];
};