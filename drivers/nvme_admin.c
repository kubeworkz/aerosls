#include "nvme.h"
#include "nvme_admin.h"

#define mmio_write32(addr, val) (*(volatile uint32_t*)(addr) = (val))

static uint16_t admin_sq_tail = 0;
static uint16_t admin_cq_head = 0;
static uint16_t global_cmd_id = 0;
static uint16_t current_phase_tag = 1; // NVMe starts tracking completions with Phase = 1

// Submit an Admin command and synchronously block until the controller executes it
struct NVMeCqe nvme_submit_admin_cmd(struct NVMeCmd cmd) {
    struct NVMeCmd* asq = (struct NVMeCmd*)nvme_ctrl.admin_sq_virt;
    struct NVMeCqe* acq = (struct NVMeCqe*)nvme_ctrl.admin_cq_virt;

    cmd.command_id = global_cmd_id++;
    
    // Copy 64 bytes into the current tail index of the Admin Submission Queue ring
    asq[admin_sq_tail] = cmd;
    
    // Advance tail pointer wrapping at queue boundaries
    admin_sq_tail = (admin_sq_tail + 1) % ADMIN_QUEUE_SIZE;

    // Ring Admin Doorbell (Doorbell 0 is fixed at base + 0x1000)
    uint64_t sq_doorbell_addr = nvme_ctrl.mmio_base + 0x1000;
    mmio_write32(sq_doorbell_addr, admin_sq_tail);

    // Poll the Completion Queue slot for execution confirmation
    volatile struct NVMeCqe* cqe = &acq[admin_cq_head];
    
    // The Phase Tag bit (Bit 0 of status field) flips when the hardware populates a slot
    while ((cqe->status & 0x01) != current_phase_tag) {
        __asm__ volatile("pause");
    }

    struct NVMeCqe local_copy = *cqe;

    // Advance completion head index pointer
    admin_cq_head = (admin_cq_head + 1) % ADMIN_QUEUE_SIZE;
    if (admin_cq_head == 0) {
        // When completion queue wraps, expected hardware Phase Tag toggles
        current_phase_tag = current_phase_tag ^ 1; 
    }

    // Ring Completion Queue Doorbell to inform the device we processed the entry
    uint64_t cq_doorbell_addr = nvme_ctrl.mmio_base + 0x1000 + nvme_ctrl.stride;
    mmio_write32(cq_doorbell_addr, admin_cq_head);

    return local_copy;
}