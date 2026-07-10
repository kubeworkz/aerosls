#include "../include/sls_mmu.h"
    #define MAX_TASKS 64
    enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED };
    struct Task { uint32_t id; enum TaskState state; uint64_t blocked_on_vaddr; uint64_t rsp; };
    static struct Task task_table[MAX_TASKS];
    static uint32_t current_task_idx = 0;
    uint32_t kernel_get_current_thread_id(void) { return task_table[current_task_idx].id; 
    }