#include <stdint.h>
/*
 * qemu_sls_vm.c — Phase 4 VM snapshot for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 4.
 */

#include "qemu_sls_vm.h"
#include "qemu_sls_tcache.h"
#include "kernel_io.h"
#include "timer.h"
#include "../drivers/nvme_io.h"

static QemuVMState vm_snap_buf __attribute__((aligned(4096)));
static uint64_t    snap_sequence;

/* ─── qemu_sls_snapshot_exists ──────────────────────────────────────────── */

int qemu_sls_snapshot_exists(void) {
    if (!io_sq || !io_cq) return 0;
    if (nvme_read_sync(QEMU_VM_STATE_LBA, &vm_snap_buf) != 0) return 0;
    return vm_snap_buf.magic == QEMU_VM_STATE_MAGIC;
}

/* ─── qemu_sls_vm_init ───────────────────────────────────────────────────── */

void qemu_sls_vm_init(void) {
    if (qemu_sls_snapshot_exists()) {
        /* vm_snap_buf is now populated by the NVMe read above.
         *
         * Reaching here would be genuinely surprising: qemu_sls_snapshot_save()
         * has no callers, so nothing writes LBA 20000. A valid magic here means
         * either that a writer was added without updating this note, or that
         * something else is writing into the VM-state LBA -- and the second
         * would be worth chasing immediately. */
        kernel_serial_printf(
            "[QEMU-SLS VM] snapshot found: seq=%llu rip=0x%016lx\n"
            "[QEMU-SLS VM] NOTE: nothing in this tree calls "
            "qemu_sls_snapshot_save(). Investigate what wrote LBA 20000.\n",
            (unsigned long long)vm_snap_buf.sequence, vm_snap_buf.rip);
    } else {
        /* The only outcome this line has ever had. Says why, so it is not
         * mistaken for a report about a subsystem that is doing something --
         * see the status note in qemu_sls_vm.h. */
        kernel_serial_print(
            "[QEMU-SLS VM] no snapshot -- Phase 4 save/restore has no callers "
            "(scaffolding, awaiting a launcher resume path)\n");
    }
}

/* ─── qemu_sls_snapshot_save ─────────────────────────────────────────────── */

int qemu_sls_snapshot_save(const QemuVMState *state) {
    if (!state || !io_sq || !io_cq) return -1;

    vm_snap_buf                = *state;
    vm_snap_buf.magic          = QEMU_VM_STATE_MAGIC;
    vm_snap_buf.sequence       = snap_sequence++;
    vm_snap_buf.timestamp_tick = kernel_tick_counter;

    if (nvme_write_sync(QEMU_VM_STATE_LBA, &vm_snap_buf) != 0) return -1;
    nvme_flush_sync();

    /* Consistent checkpoint: persist tcache alongside the CPU state. */
    qemu_sls_tcache_sync();

    kernel_serial_printf(
        "[QEMU-SLS VM] snapshot saved: seq=%llu rip=0x%016lx\n",
        (unsigned long long)vm_snap_buf.sequence, vm_snap_buf.rip);
    return 0;
}

/* ─── qemu_sls_snapshot_restore ─────────────────────────────────────────── */

int qemu_sls_snapshot_restore(QemuVMState *state) {
    if (!state || !io_sq || !io_cq) return -1;
    if (nvme_read_sync(QEMU_VM_STATE_LBA, &vm_snap_buf) != 0) return -1;
    if (vm_snap_buf.magic != QEMU_VM_STATE_MAGIC) return -1;
    *state = vm_snap_buf;
    kernel_serial_printf(
        "[QEMU-SLS VM] snapshot restored: seq=%llu rip=0x%016lx\n",
        (unsigned long long)vm_snap_buf.sequence, vm_snap_buf.rip);
    return 0;
}

/* See qemu_sls_mmu.h. Defined here rather than in qemu_sls_mmu.c so that file
 * stays free of privileged instructions and remains host-testable. */
void qemu_sls_invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)va) : "memory");
}
