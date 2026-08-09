#ifndef NVME_H
#define NVME_H

#include <stdint.h>

// NVMe MMIO register offsets
#define NVME_REG_CAP  0x0000   // Controller Capabilities (64-bit)
#define NVME_REG_CC   0x0014   // Controller Configuration (32-bit)
#define NVME_REG_CSTS 0x001C   // Controller Status (32-bit)
#define NVME_REG_AQA  0x0024   // Admin Queue Attributes (32-bit)
#define NVME_REG_ASQ  0x0028   // Admin Submission Queue Base Address (64-bit)
#define NVME_REG_ACQ  0x0030   // Admin Completion Queue Base Address (64-bit)

// Number of entries in admin submission/completion queues
#define ADMIN_QUEUE_SIZE 64

// NVMe controller state
struct nvme_controller {
    uint64_t mmio_base;      // Physical base address of BAR0 MMIO region
    uint32_t stride;         // Doorbell register stride in bytes
    void*    admin_sq_virt;  // Admin Submission Queue virtual address
    void*    admin_cq_virt;  // Admin Completion Queue virtual address
};

extern struct nvme_controller nvme_ctrl;

// Initialize the NVMe controller at the given BAR0 physical address.
// Returns 1 on success, 0 on failure.
int init_nvme_controller(uint64_t bar0_phys);

#endif // NVME_H
