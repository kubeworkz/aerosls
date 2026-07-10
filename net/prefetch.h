#ifndef PREFETCH_H
#define PREFETCH_H

#include <stdint.h>

struct PrefetchRequest {
    uint64_t system_object_id;
    uint64_t target_vaddr;
    uint32_t active;
};

#define PREFETCH_QUEUE_DEPTH 32
struct PrefetchPipeline {
    struct PrefetchRequest requests[PREFETCH_QUEUE_DEPTH];
    uint32_t head;
    uint32_t tail;
};

static struct PrefetchPipeline pf_pipeline = {0};

void issue_speculative_prefetch(uint64_t object_id, uint64_t current_fault_vaddr);
void prefetch_worker_kernel_thread(void);

#endif