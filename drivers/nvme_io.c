#include "nvme_io.h"
#include "nvme.h"
#include "../kernel/kernel_io.h"

extern void* allocate_physical_ram_frame(void);

#define mmio_write32(a,v) (*(volatile uint32_t*)(a) = (v))
#define SECTORS_PER_FRAME 8u   /* 8 × 512 B = 4 KiB */

// Timeout for the I/O-queue completion poll below, same budget and pause()-
// loop shape as nvme_admin.c's nvme_submit_admin_cmd() (~500ms at 1GHz) --
// this file's own poll loop was missing that bound entirely (see the "Gap
// fix" comment on nvme_io_submit_sync() for how that surfaced: any command
// whose completion is never observed -- a missed doorbell, a real
// controller's completion timing differing from local QEMU testing, etc --
// spun this loop forever at 100% CPU with no way out. Because every
// nvme_write_sync()/nvme_read_sync() call in this codebase runs synchronously
// inside the single-threaded HTTP request path (net/http.c's
// http_server_run() loop), that infinite spin doesn't just fail one request
// -- it wedges the entire server, including completely unrelated GET
// requests like /api/health, since nothing else ever runs again. Unlike the
// admin path, this one deliberately does NOT cli/sti around the spin: the
// admin path's interrupt-disable is a boot-time-only safety measure against
// timer-ISR reentrancy during controller bring-up, and disabling interrupts
// on every single steady-state disk write would block unrelated IRQ
// handling (e.g. the network stack) for the duration -- worse than the
// problem being fixed.
#define NVME_IO_TIMEOUT 500000000UL

// ─── I/O queue state ─────────────────────────────────────────────────────────
void*          io_sq        = 0;
void*          io_cq        = 0;
static uint16_t        io_sq_tail   = 0;
static uint16_t        io_cq_head   = 0;
static uint16_t        io_cq_phase  = 1;    // phase tag starts at 1
static uint16_t        io_cmd_id    = 0x80; // use high range to avoid admin clash

// ─── Doorbell helpers ─────────────────────────────────────────────────────────
// Admin SQ = doorbell 0, Admin CQ = doorbell 1
// I/O SQ 1  = doorbell 2, I/O CQ 1  = doorbell 3
static inline uint64_t io_sq_doorbell(void) {
    return nvme_ctrl.mmio_base + 0x1000 + 2 * nvme_ctrl.stride;
}
static inline uint64_t io_cq_doorbell(void) {
    return nvme_ctrl.mmio_base + 0x1000 + 3 * nvme_ctrl.stride;
}

// ─── nvme_io_init ─────────────────────────────────────────────────────────────
int nvme_io_init(void) {
    // Allocate RAM frames for the I/O queues (4 KiB each, page-aligned)
    io_sq = allocate_physical_ram_frame();
    io_cq = allocate_physical_ram_frame();
    if (!io_sq || !io_cq) {
        kernel_serial_print("[NVME_IO] OOM: could not allocate I/O queue frames.\n");
        return 0;
    }
    // Zero both queues
    uint32_t* p = (uint32_t*)io_sq;
    for (int i = 0; i < 4096/4; i++) p[i] = 0;
    p = (uint32_t*)io_cq;
    for (int i = 0; i < 4096/4; i++) p[i] = 0;

    // --- Create I/O Completion Queue (admin opcode 0x05) ---
    struct NVMeCmd cq_cmd;
    uint32_t* cp = (uint32_t*)&cq_cmd;
    for (int i = 0; i < 16; i++) cp[i] = 0;
    cq_cmd.opcode = NVME_ADMIN_CMD_CREATE_CQ;
    cq_cmd.prp1   = (uint64_t)(uintptr_t)io_cq;
    cq_cmd.cdw10  = (uint32_t)(((NVME_IO_QUEUE_SIZE - 1) << 16) | NVME_IO_QUEUE_ID);
    cq_cmd.cdw11  = 0x00000001u;
    kernel_serial_printf("[NVME_IO] Creating CQ: prp1=0x%lx cdw10=0x%x\n",
                         cq_cmd.prp1, cq_cmd.cdw10);
    struct NVMeCqe cqe = nvme_submit_admin_cmd(cq_cmd);
    kernel_serial_printf("[NVME_IO] Create CQ done, status=0x%x\n",
                         (unsigned)(cqe.status >> 1) & 0xFF);
    if ((cqe.status >> 1) & 0xFF) return 0;

    // --- Create I/O Submission Queue (admin opcode 0x01) ---
    struct NVMeCmd sq_cmd;
    uint32_t* sp = (uint32_t*)&sq_cmd;
    for (int i = 0; i < 16; i++) sp[i] = 0;
    sq_cmd.opcode = NVME_ADMIN_CMD_CREATE_SQ;
    sq_cmd.prp1   = (uint64_t)(uintptr_t)io_sq;
    sq_cmd.cdw10  = (uint32_t)(((NVME_IO_QUEUE_SIZE - 1) << 16) | NVME_IO_QUEUE_ID);
    // cdw11: CQID (bits 31:16) | QPRIO=0 (bits 3:2) | PC=1 (bit 0)
    sq_cmd.cdw11  = (uint32_t)((NVME_IO_QUEUE_ID << 16) | 0x00000001u);
    cqe = nvme_submit_admin_cmd(sq_cmd);
    if ((cqe.status >> 1) & 0xFF) {
        kernel_serial_printf("[NVME_IO] Create SQ failed, status=0x%x\n",
                             (unsigned)(cqe.status >> 1) & 0xFF);
        return 0;
    }

    kernel_serial_print("[NVME_IO] I/O queue pair 1 ready (64 entries, 4-KiB PRP).\n");
    return 1;
}

// ─── submit one I/O command and poll for completion ───────────────────────────
// Gap fix: this poll loop used to be unbounded (`while (...) pause();` with
// no exit condition other than the completion actually showing up) -- see
// the NVME_IO_TIMEOUT comment above for the full story of how that produced
// the reported "kernel wedged at 100% CPU, every request including
// /api/health hangs" bug from a single, ordinary vector insert. Mirrors
// nvme_admin.c's nvme_submit_admin_cmd() timeout shape: on timeout, log it
// and return a nonzero (distinct, out-of-band 0xFF) status instead of
// hanging forever. Every existing caller of nvme_write_sync()/
// nvme_read_sync() already treats a nonzero return as failure and swallows
// it safely (vecstore.c/rowstore.c/stream.c all predate this fix and were
// written expecting the NVMe status-code convention "0 on success, non-zero
// on NVMe status error" documented in nvme_io.h -- a timeout is just another
// error under that same contract, not a new case callers need to learn).
static int nvme_io_submit_sync(struct NVMeCmd* cmd) {
    cmd->command_id = io_cmd_id++;

    // Place command in I/O SQ at current tail
    struct NVMeCmd* sq = (struct NVMeCmd*)io_sq;
    struct NVMeCqe* cq = (struct NVMeCqe*)io_cq;
    sq[io_sq_tail] = *cmd;
    io_sq_tail = (io_sq_tail + 1) % NVME_IO_QUEUE_SIZE;

    // Ring I/O SQ doorbell
    mmio_write32(io_sq_doorbell(), io_sq_tail);

    // Poll I/O CQ for completion (phase-tag protocol), bounded by
    // NVME_IO_TIMEOUT so a missed/delayed completion fails the request
    // instead of spinning the kernel forever.
    volatile struct NVMeCqe* cqe = &cq[io_cq_head];
    uint64_t deadline = NVME_IO_TIMEOUT;
    while (((cqe->status) & 0x1) != io_cq_phase) {
        __asm__ volatile("pause");
        if (--deadline == 0) {
            kernel_serial_printf(
                "[NVME_IO] I/O cmd 0x%x timeout (sq_tail=%u cq_head=%u phase=%u) -- "
                "failing request instead of hanging.\n",
                (unsigned)cmd->opcode, io_sq_tail, io_cq_head, (unsigned)io_cq_phase);
            return 0xFF;   // distinct out-of-band status -- never a real NVMe status-code byte
        }
    }

    uint16_t status = cqe->status;

    // Advance CQ head and ring CQ doorbell
    io_cq_head = (io_cq_head + 1) % NVME_IO_QUEUE_SIZE;
    if (io_cq_head == 0) io_cq_phase ^= 1;
    mmio_write32(io_cq_doorbell(), io_cq_head);

    // Status field bits 15:1 = Status Code; 0 = success
    return (int)((status >> 1) & 0xFF);
}

// ─── nvme_read_sync ───────────────────────────────────────────────────────────
int nvme_read_sync(uint64_t slba, void* buf) {
    struct NVMeCmd cmd;
    uint32_t* p = (uint32_t*)&cmd;
    for (int i = 0; i < 16; i++) p[i] = 0;
    cmd.opcode = NVME_NVM_READ;
    cmd.nsid   = NVME_NSID;
    cmd.prp1   = (uint64_t)(uintptr_t)buf;
    cmd.cdw10  = (uint32_t)(slba & 0xFFFFFFFFu);
    cmd.cdw11  = (uint32_t)(slba >> 32);
    cmd.cdw12  = SECTORS_PER_FRAME - 1;  // NLB (0-based: 7 = 8 sectors)
    return nvme_io_submit_sync(&cmd);
}

// ─── nvme_write_sync ──────────────────────────────────────────────────────────
int nvme_write_sync(uint64_t slba, const void* buf) {
    struct NVMeCmd cmd;
    uint32_t* p = (uint32_t*)&cmd;
    for (int i = 0; i < 16; i++) p[i] = 0;
    cmd.opcode = NVME_NVM_WRITE;
    cmd.nsid   = NVME_NSID;
    cmd.prp1   = (uint64_t)(uintptr_t)buf;
    cmd.cdw10  = (uint32_t)(slba & 0xFFFFFFFFu);
    cmd.cdw11  = (uint32_t)(slba >> 32);
    cmd.cdw12  = SECTORS_PER_FRAME - 1;  // NLB = 7 (0-based)
    return nvme_io_submit_sync(&cmd);
}
