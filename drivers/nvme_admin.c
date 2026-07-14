#include "nvme.h"
#include "nvme_admin.h"
#include "../kernel/kernel_io.h"

#define mmio_write32(addr, val) (*(volatile uint32_t*)(addr) = (val))

// Timeout: ~500 ms at 1 GHz (500 million iterations)
#define NVME_ADMIN_TIMEOUT  500000000UL

static uint16_t admin_sq_tail = 0;
static uint16_t admin_cq_head = 0;
static uint16_t global_cmd_id = 0;
static uint16_t current_phase_tag = 1;

// Submit an Admin command and synchronously poll for completion.
// Returns a zero-status CQE on timeout (safe for callers to check).
struct NVMeCqe nvme_submit_admin_cmd(struct NVMeCmd cmd) {
    struct NVMeCmd* asq = (struct NVMeCmd*)nvme_ctrl.admin_sq_virt;
    struct NVMeCqe* acq = (struct NVMeCqe*)nvme_ctrl.admin_cq_virt;

    cmd.command_id = global_cmd_id++;

    asq[admin_sq_tail] = cmd;
    admin_sq_tail = (admin_sq_tail + 1) % ADMIN_QUEUE_SIZE;

    // Ring Admin SQ doorbell (doorbell 0 at mmio_base + 0x1000)
    uint64_t sq_doorbell_addr = nvme_ctrl.mmio_base + 0x1000;
    mmio_write32(sq_doorbell_addr, admin_sq_tail);

    // Poll with timeout — disable interrupts during the spin to avoid
    // timer ISR re-entrancy while the controller is processing the command.
    __asm__ volatile("cli");
    volatile struct NVMeCqe* cqe = &acq[admin_cq_head];
    uint64_t deadline = NVME_ADMIN_TIMEOUT;
    while (((cqe->status) & 0x01) != current_phase_tag) {
        __asm__ volatile("pause");
        if (--deadline == 0) {
            __asm__ volatile("sti");
            kernel_serial_printf(
                "[NVME] admin cmd 0x%x timeout (sq_tail=%u cq_head=%u phase=%u)\n",
                (unsigned)cmd.opcode, admin_sq_tail, admin_cq_head,
                (unsigned)current_phase_tag);
            struct NVMeCqe zero = {0, 0, 0, 0};
            return zero;
        }
    }
    __asm__ volatile("sti");

    struct NVMeCqe local_copy = *cqe;

    admin_cq_head = (admin_cq_head + 1) % ADMIN_QUEUE_SIZE;
    if (admin_cq_head == 0) current_phase_tag ^= 1;

    // Ring Admin CQ doorbell (doorbell 1 at mmio_base + 0x1000 + stride)
    uint64_t cq_doorbell_addr = nvme_ctrl.mmio_base + 0x1000 + nvme_ctrl.stride;
    mmio_write32(cq_doorbell_addr, admin_cq_head);

    return local_copy;
}