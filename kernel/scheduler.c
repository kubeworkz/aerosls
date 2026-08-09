#include "scheduler.h"

static struct Task task_table[MAX_TASKS];
static uint32_t current_task_idx = 0;
static uint32_t total_tasks = 0;

void init_scheduler(void) {
    task_table[0].id    = 0;
    task_table[0].state = TASK_RUNNING;
    total_tasks         = 1;
    current_task_idx    = 0;
}

// Register a new kernel-mode thread in the task table.
// The thread's entry point fn() will be called by the AP service poll loop.
uint32_t spawn_kernel_thread(void (*fn)(void), uint32_t task_id) {
    (void)fn; // fn is invoked directly by the service loop, not via context switch
    if (total_tasks >= MAX_TASKS) return 0;
    uint32_t slot = total_tasks++;
    task_table[slot].id               = task_id;
    task_table[slot].state            = TASK_READY;
    task_table[slot].blocked_on_vaddr = 0;
    task_table[slot].rsp              = 0;
    return task_id;
}

uint32_t kernel_get_current_thread_id(void) {
    return task_table[current_task_idx].id;
}

void block_thread_on_object(uint32_t thread_id, uint64_t vaddr) {
    for (int i = 0; i < total_tasks; i++) {
        if (task_table[i].id == thread_id) {
            task_table[i].state = TASK_BLOCKED;
            task_table[i].blocked_on_vaddr = vaddr;
            break;
        }
    }
}

void wakeup_threads_blocked_on_object(uint64_t vaddr) {
    for (int i = 0; i < total_tasks; i++) {
        if (task_table[i].state == TASK_BLOCKED && task_table[i].blocked_on_vaddr == vaddr) {
            task_table[i].state = TASK_READY;
            task_table[i].blocked_on_vaddr = 0;
        }
    }
}

// Selects the next eligible thread via Round-Robin rotation
uint64_t schedule_next(uint64_t old_rsp) {
    // Save current thread's stack location pointer
    task_table[current_task_idx].rsp = old_rsp;
    if (task_table[current_task_idx].state == TASK_RUNNING) {
        task_table[current_task_idx].state = TASK_READY;
    }

    // Find next TASK_READY slot
    uint32_t next_idx = current_task_idx;
    while (1) {
        next_idx = (next_idx + 1) % total_tasks;
        if (task_table[next_idx].state == TASK_READY) {
            break;
        }
    }

    current_task_idx = next_idx;
    task_table[current_task_idx].state = TASK_RUNNING;
    
    return task_table[current_task_idx].rsp;
}