/*
 * qemu_sls_vm.h — Phase 4: VM snapshot and zero-copy DMA integration.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 4.
 *
 * The guest architectural state fits in one NVMe page (4 KiB) so save/restore
 * is a single synchronous NVMe write/read — no separate WAL needed.
 *
 * NVMe layout:
 *   LBA 20000  QEMU_VM_STATE_LBA   1 frame — QemuVMState (4 KiB)
 *              (clear of the Phase 2 code buffer which ends at LBA ~18968)
 */
#ifndef QEMU_SLS_VM_H
#define QEMU_SLS_VM_H

#include <stdint.h>

#define QEMU_VM_STATE_LBA    20000ULL
#define QEMU_VM_STATE_MAGIC  0xCAFE000000000030ULL

/*
 * Guest architectural state snapshot.  Sized to exactly one NVMe page so a
 * single nvme_write_sync() / nvme_read_sync() covers the complete struct.
 * Fields after _meta_pad are written by snapshot_save(), not by the caller.
 */
typedef struct {
    /* x86-64 general-purpose registers */
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
    /* guest control registers */
    uint64_t cr0, cr2, cr3, cr4;
    /* QEMU-SLS Phase 1: shadow PT and guest page table root */
    uint64_t shadow_cr3;
    uint64_t guest_cr3;
    /* Phase 2: tcache state at snapshot time */
    uint32_t tcache_codebuf_used;
    uint32_t _meta_pad;
    /* snapshot metadata — set by snapshot_save() */
    uint64_t magic;
    uint64_t sequence;
    uint64_t timestamp_tick;
    /* pad to exactly one NVMe page */
    uint8_t  _pad[4096 - 224];
} QemuVMState;

/* One-time boot check; logs whether a valid snapshot exists on NVMe. */
void qemu_sls_vm_init(void);

/*
 * Persist the guest architectural state.  Fills magic/sequence/timestamp_tick
 * automatically; the caller supplies the rest.  Also calls qemu_sls_tcache_sync()
 * for a consistent point-in-time checkpoint.
 * Returns 0 on success, -1 if NVMe is unavailable or the write fails.
 */
int  qemu_sls_snapshot_save(const QemuVMState *state);

/*
 * Restore the most recent snapshot into *state.
 * Returns 0 on success, -1 if no valid snapshot exists.
 */
int  qemu_sls_snapshot_restore(QemuVMState *state);

/* 1 if a valid snapshot exists on NVMe, 0 otherwise. */
int  qemu_sls_snapshot_exists(void);

#endif /* QEMU_SLS_VM_H */
