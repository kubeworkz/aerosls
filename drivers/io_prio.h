#ifndef IO_PRIO_H
#define IO_PRIO_H

#include "nvme_admin.h"

enum IOPriority {
    PRIO_HIGH = 0, // Interactive shell page faults (Stalled users)
    PRIO_MED  = 1, // Explicit application sync fences
    PRIO_LOW  = 2, // Background clock evictions and live defragmentation
    PRIO_COUNT = 3
};

struct PrioritizedCmd {
    struct NVMeCmd command;
    uint64_t       faulting_vaddr;
    uint32_t       thread_id;
    uint8_t        is_active;
};

#define PRIO_QUEUE_DEPTH 64

struct PriorityScheduler {
    struct PrioritizedCmd queues[PRIO_COUNT][PRIO_QUEUE_DEPTH];
    uint32_t head[PRIO_COUNT];
    uint32_t tail[PRIO_COUNT];
    uint32_t count[PRIO_COUNT];
};

static struct PriorityScheduler io_broker = {0};

void enqueue_prioritized_io(struct NVMeCmd cmd, uint64_t vaddr, enum IOPriority priority);
void dispatch_pending_ios_to_nvme(void);

#endif