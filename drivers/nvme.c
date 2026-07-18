#include "nvme.h"

struct nvme_controller nvme_ctrl;

// Macro reads/writes memory-mapped IO registers safely
#define mmio_read32(addr)  (*(volatile uint32_t*)(addr))
#define mmio_write32(addr, val) (*(volatile uint32_t*)(addr) = (val))
#define mmio_write64(addr, val) (*(volatile uint64_t*)(addr) = (val))

extern void* allocate_physical_ram_frame(void);

int init_nvme_controller(uint64_t bar0_phys) {
    nvme_ctrl.mmio_base = bar0_phys;

    uint64_t cap = *(volatile uint64_t*)(nvme_ctrl.mmio_base + NVME_REG_CAP);
    
    // Bits 32-35 of CAP dictate the doorbell register stride factor (4 << stride) bytes
    nvme_ctrl.stride = 4 << ((cap >> 32) & 0xF);

    // 1. If controller is currently active, shut it down to apply admin queue structures
    uint32_t cc = mmio_read32(nvme_ctrl.mmio_base + NVME_REG_CC);
    if (cc & (1 << 0)) { // Bit 0 is Controller Enable (EN)
        cc &= ~(1 << 0); // Clear Enable bit
        mmio_write32(nvme_ctrl.mmio_base + NVME_REG_CC, cc);
    }

    // Wait for Controller Status (CSTS) Ready bit (RDY) to pull low (0 = Disabled)
    while (mmio_read32(nvme_ctrl.mmio_base + NVME_REG_CSTS) & (1 << 0)) {
        __asm__ volatile("pause");
    }

    // 2. Allocate memory blocks for the Admin Queues
    // Admin Submission Queue takes 64-byte command entries
    // Admin Completion Queue takes 16-byte response entries
    nvme_ctrl.admin_sq_virt = allocate_physical_ram_frame();
    nvme_ctrl.admin_cq_virt = allocate_physical_ram_frame();
    if (!nvme_ctrl.admin_sq_virt || !nvme_ctrl.admin_cq_virt) return 0;

    // Clear memory frames to prevent processing stale data artifacts
    for (int i = 0; i < 1024; i++) {
        ((uint32_t*)nvme_ctrl.admin_sq_virt)[i] = 0;
        ((uint32_t*)nvme_ctrl.admin_cq_virt)[i] = 0;
    }

    // 3. Program Queue Length Sizes into Admin Queue Attributes (AQA)
    // Format: Bits 0-15: ASQS (0-indexed count), Bits 16-31: ACQS (0-indexed count)
    uint32_t aqa = ((ADMIN_QUEUE_SIZE - 1) << 16) | (ADMIN_QUEUE_SIZE - 1);
    mmio_write32(nvme_ctrl.mmio_base + NVME_REG_AQA, aqa);

    // Program base memory addresses into controller
    mmio_write64(nvme_ctrl.mmio_base + NVME_REG_ASQ, (uint64_t)nvme_ctrl.admin_sq_virt);
    mmio_write64(nvme_ctrl.mmio_base + NVME_REG_ACQ, (uint64_t)nvme_ctrl.admin_cq_virt);

    // 4. Formulate enabling parameters inside Controller Configuration (CC)
    // Select I/O Command Set (Bits 4-6 = 000 for NVM), Page Size (Bits 7-10 = 0 for 4KB)
    // Submission Entry Size (Bits 16-19 = 6 for 64 bytes), Completion Entry Size (Bits 20-23 = 4 for 16 bytes)
    cc = (4 << 20) | (6 << 16) | (0 << 7) | (0 << 4) | (1 << 0);
    mmio_write32(nvme_ctrl.mmio_base + NVME_REG_CC, cc);

    // Wait for CSTS Ready (RDY) to flag high (1 = Fully booted and processing)
    while (!(mmio_read32(nvme_ctrl.mmio_base + NVME_REG_CSTS) & (1 << 0))) {
        __asm__ volatile("pause");
    }

    return 1; // NVMe Device Ready for Admin commands
}