/*
 * dspp.c — Multi-Node Partition Scaling Roadmap Phase 5: real DSPP
 * page-routing logic. See dspp.h's own header comment for the design
 * writeup and the honest "no RX dispatcher wired up yet" caveat this
 * phase's own findings addendum in the roadmap doc also names.
 */
#include "dspp.h"
#include "consensus.h"
#include "../kernel/object_catalog.h"
#include "../kernel/partition.h"
#include "../kernel/kernel_io.h"

uint32_t dspp_resolve_partition_id(uint64_t system_object_id) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && object_catalog[i].object_id == system_object_id)
            return object_catalog[i].partition_id;
    }
    return 0;   /* not found -- honest "no object, no partition" default (0 == PARTITION_SYSTEM) */
}

int dspp_page_read_allowed(uint32_t partition_id) {
    return partition_is_local(partition_id);
}

int dspp_page_write_allowed(uint32_t partition_id) {
    return partition_is_local(partition_id) && partition_holds_write_lease(partition_id);
}

int process_dspp_page_packet(struct DSPPFullPagePacket* packet) {
    uint32_t partition_id = dspp_resolve_partition_id(packet->header.system_object_id);

    if (packet->header.opcode == DSPP_PAGE_READ_REQ) {
        if (!dspp_page_read_allowed(partition_id)) {
            kernel_serial_printf(
                "[DSPP] READ_REQ denied for object %llu (partition %u not local).\n",
                (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
            return 0;
        }
        /* Real implementation: resolve system_object_id/virtual_address to
         * a physical frame and transmit a DSPP_PAGE_READ_ACK carrying it.
         * No such object-to-physical-frame resolution plumbing exists
         * anywhere in this codebase yet -- a separate, larger integration
         * this phase does not attempt. This phase closes the routing/
         * gating gap (should this request even be serviced here), not the
         * page-move plumbing gap (how the bytes would actually get onto
         * the wire). */
        kernel_serial_printf(
            "[DSPP] READ_REQ for object %llu (partition %u, local) -- allowed.\n",
            (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
        return 1;
    }

    if (packet->header.opcode == DSPP_PAGE_WRITE_REQ) {
        if (!dspp_page_write_allowed(partition_id)) {
            kernel_serial_printf(
                "[DSPP] WRITE_REQ denied for object %llu (partition %u: not local and/or no write lease held).\n",
                (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
            return 0;
        }
        kernel_serial_printf(
            "[DSPP] WRITE_REQ for object %llu (partition %u, local + write lease held) -- allowed.\n",
            (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
        return 1;
    }

    return 0;   /* not a page-routing opcode this function handles */
}
