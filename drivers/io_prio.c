#include "io_prio.h"
#include "nvme.h"

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

// Thread-safe wrapper using simple atomic bit operations to append requests
void enqueue_prioritized_io(struct NVMeCmd cmd, uint64_t vaddr, enum IOPriority priority) {
    uint32_t current_thread = kernel_get_current_thread_id();
    
    // Acquire slot using atomic compare-and-swap loop to preserve multi-core execution safety
    while (1) {
        uint32_t t_tail = io_broker.tail[priority];
        uint32_t next_tail = (t_tail + 1) % PRIO_QUEUE_DEPTH;
        
        if (io_broker.count[priority] >= PRIO_QUEUE_DEPTH) {
            kernel_panic("SLS I/O Traffic Controller Queue Saturated.");
        }

        if (__sync_bool_compare_and_swap(&io_broker.tail[priority], t_tail, next_tail)) {
            struct PrioritizedCmd* p_cmd = &io_broker.queues[priority][t_tail];
            p_cmd->command = cmd;
            p_cmd->faulting_vaddr = vaddr;
            p_cmd->thread_id = current_thread;
            p_cmd->is_active = 1;
            
            __sync_fetch_and_add(&io_broker.count[priority], 1);
            break;
        }
    }
}

// Invoked regularly by the scheduler daemon or hardware completion interrupts
void dispatch_pending_ios_to_nvme(void) {
    struct NVMeCmd* io_sq = (struct NVMeCmd*)nvme_ctrl.mmio_base + 0x1000;
    uint32_t io_sq_tail = 0;
    
    // Strict Priority Strictures: Only drain lower lanes if higher lanes are completely empty
    for (int prio = PRIO_HIGH; prio < PRIO_COUNT; prio++) {
        while (io_broker.count[prio] > 0) {
            uint32_t h_idx = io_broker.head[prio];
            struct PrioritizedCmd* p_cmd = &io_broker.queues[prio][h_idx];
            
            // Map the request down to the actual physical NVMe hardware submission slots
            io_sq[io_sq_tail] = p_cmd->command;
            
            // Propagate thread-blocking constraints out to matching token arrays
            block_thread_on_storage_token(p_cmd->thread_id, p_cmd->command.command_id, p_cmd->faulting_vaddr);
            
            io_sq_tail = (io_sq_tail + 1) % PRIO_QUEUE_DEPTH;
            
            // Clean out software scheduling queue slot metadata
            p_cmd->is_active = 0;
            io_broker.head[prio] = (h_idx + 1) % PRIO_QUEUE_DEPTH;
            __sync_fetch_and_sub(&io_broker.count[prio], 1);

            // Ring the actual NVMe hardware controller doorway bell
            uint64_t io_sq_doorbell = nvme_ctrl.mmio_base + 0x1000 + (2 * nvme_ctrl.stride);
            *(volatile uint32_t*)io_sq_doorbell = io_sq_tail;
        }
    }
}