#include "nvme.h"
#include "nvme_admin.h"
#include "../kernel/kernel_io.h"

extern void* allocate_physical_ram_frame(void);

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

// ─── Navigator-Parity Gap Roadmap Phase 2 ─────────────────────────────────────
static uint64_t cached_capacity_bytes = 0;

int nvme_identify_namespace(uint32_t nsid) {
    void* scratch = allocate_physical_ram_frame();
    if (!scratch) {
        kernel_serial_print("[NVME] Identify: OOM allocating scratch frame.\n");
        return 0;
    }
    uint32_t* zp = (uint32_t*)scratch;
    for (int i = 0; i < 4096 / 4; i++) zp[i] = 0;

    struct NVMeCmd cmd;
    uint32_t* cp = (uint32_t*)&cmd;
    for (int i = 0; i < 16; i++) cp[i] = 0;
    cmd.opcode = NVME_ADMIN_CMD_IDENTIFY;
    cmd.nsid   = nsid;
    cmd.prp1   = (uint64_t)(uintptr_t)scratch;
    cmd.cdw10  = 0x00000001u;   // CNS=1 (Identify Namespace, using nsid above)

    struct NVMeCqe cqe = nvme_submit_admin_cmd(cmd);
    if ((cqe.status >> 1) & 0xFF) {
        kernel_serial_printf("[NVME] Identify Namespace failed, status=0x%x\n",
                             (unsigned)(cqe.status >> 1) & 0xFF);
        return 0;
    }

    // Identify Namespace Data Structure: NSZE at byte offset 0 (8 bytes),
    // NCAP at byte offset 8 (8 bytes), both counts of logical blocks.
    uint64_t* fields = (uint64_t*)scratch;
    uint64_t nsze = fields[0];
    uint64_t ncap = fields[1];
    (void)nsze;   // total addressable LBAs; NCAP is the real usable capacity

    // Assumes 512-byte logical blocks -- see this function's own header
    // comment in nvme_admin.h for why the LBA Format table isn't parsed.
    cached_capacity_bytes = ncap * 512ULL;
    kernel_serial_printf("[NVME] Identify Namespace %u: NCAP=%llu blocks (~%llu MB assuming 512B LBA)\n",
                         (unsigned)nsid, (unsigned long long)ncap,
                         (unsigned long long)(cached_capacity_bytes / (1024 * 1024)));
    return 1;
}

uint64_t nvme_get_capacity_bytes(void) {
    return cached_capacity_bytes;
}