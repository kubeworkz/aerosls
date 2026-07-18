#ifndef NVME_IO_H
#define NVME_IO_H

#include <stdint.h>
#include "nvme_admin.h"

// ─── I/O queue constants ──────────────────────────────────────────────────────
#define NVME_IO_QUEUE_ID    1
#define NVME_IO_QUEUE_SIZE  64
#define NVME_NSID           1

// NVMe NVM command opcodes
#define NVME_NVM_WRITE      0x01
#define NVME_NVM_READ       0x02

// ─── Public API ───────────────────────────────────────────────────────────────
// Set up I/O SQ and CQ via Create CQ / Create SQ admin commands.
// Must be called after init_nvme_controller() and before any read/write.
// Returns 1 on success, 0 on failure.
int  nvme_io_init(void);

// True after successful nvme_io_init(); used by stream.c to skip NVMe ops
// when the I/O queue was not initialised (e.g. NVMe MMIO above 4 GiB).
extern void* io_sq;
extern void* io_cq;

// Synchronous 4-KiB (8 sector) read: fills buf[4096] from NVMe LBA slba.
// buf must be 4-KiB page-aligned (frame-pool frames are always aligned).
// Returns 0 on success, non-zero on NVMe status error.
int  nvme_read_sync(uint64_t slba, void* buf);

// Synchronous 4-KiB (8 sector) write: writes buf[4096] to NVMe LBA slba.
// Returns 0 on success, non-zero on NVMe status error.
int  nvme_write_sync(uint64_t slba, const void* buf);

#endif /* NVME_IO_H */
