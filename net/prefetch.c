#include "prefetch.h"
#include "dspp.h"
#include "io_prio.h"
#include "e1000.h"

extern uint64_t* walk_page_tables(uint64_t virtual_address);

// Evaluates adjacent virtual address space steps on demand
void issue_speculative_prefetch(uint64_t object_id, uint64_t current_fault_vaddr) {
    // Speculatively check the next immediate linear memory page (N + 1 page = +4096 bytes)
    uint64_t next_page_vaddr = current_fault_vaddr + 4096;
    
    uint64_t* pte = walk_page_tables(next_page_vaddr);
    if (pte && !(*pte & (1ULL << 0))) { // Entry exists but Present Bit is 0 (Not loaded in RAM)
        
        // Append the prefetch request into the background pipeline array
        uint32_t t = pf_pipeline.tail;
        if (!pf_pipeline.requests[t].active) {
            pf_pipeline.requests[t].system_object_id = object_id;
            pf_pipeline.requests[t].target_vaddr = next_page_vaddr;
            pf_pipeline.requests[t].active = 1;
            pf_pipeline.tail = (t + 1) % PREFETCH_QUEUE_DEPTH;
        }
    }
}

// Background Task Executor loop deployed permanently on CPU Core 3
void prefetch_worker_kernel_thread(void) {
    while (1) {
        uint32_t h = pf_pipeline.head;
        
        if (pf_pipeline.requests[h].active) {
            struct PrefetchRequest* req = &pf_pipeline.requests[h];
            
            // Build a non-blocking DSPP packet to extract data out of remote node memory
            struct DSPPFullPagePacket pf_packet;
            pf_packet.header.magic = DSPP_MAGIC;
            pf_packet.header.system_object_id = req->system_object_id;
            pf_packet.header.virtual_address = req->target_vaddr;
            pf_packet.header.opcode = DSPP_PAGE_READ_REQ; // Remote Cache Read Request
            pf_packet.header.node_source_id = 1;
            pf_packet.header.transaction_id = 0x999;     // Token marker identifying prefetch frames

            // Send packet onto the network. We do NOT block the scheduler here.
            // Core 3 passes the packet and instantly loops to handle other system operations.
            e1000_transmit_packet(&pf_packet, sizeof(struct DSPPFullPagePacket));

            req->active = 0; // Free the software queue slot
            pf_pipeline.head = (h + 1) % PREFETCH_QUEUE_DEPTH;
        }
        
        __asm__ volatile("pause");
    }
}