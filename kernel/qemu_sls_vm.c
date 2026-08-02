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
        /* vm_snap_buf is now populated by the NVMe read above. */
        kernel_serial_printf(
            "[QEMU-SLS VM] snapshot found: seq=%llu rip=0x%016lx\n",
            (unsigned long long)vm_snap_buf.sequence, vm_snap_buf.rip);
    } else {
        kernel_serial_print("[QEMU-SLS VM] no snapshot (cold start)\n");
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
