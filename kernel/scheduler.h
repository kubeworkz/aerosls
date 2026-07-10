#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>

#define MAX_TASKS 64

enum TaskState {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED
};

struct TaskContext {
    // Layout must perfectly match the push/pop sequence in the interrupt stub
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss; 
};

struct Task {
    uint32_t id;
    enum TaskState state;
    uint64_t blocked_on_vaddr; // The SLS address this thread is waiting for
    uint64_t rsp;               // Saved stack pointer value
    uint8_t  stack[4096];       // Dedicated 4KB kernel stack space
};

void init_scheduler(void);
void kernel_yield_scheduler(void);
void block_thread_on_object(uint32_t thread_id, uint64_t vaddr);
void wakeup_threads_blocked_on_object(uint64_t vaddr);
uint32_t kernel_get_current_thread_id(void);

#endif