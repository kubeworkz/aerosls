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

// ─── Multi-page transfers ────────────────────────────────────────────────────
// Every transfer above is exactly one 4 KiB page, because each builds a
// command with prp1 alone and NLB=7. A PRP1-only command structurally cannot
// span more than one page, so moving N pages costs N separate synchronous
// submit-and-poll round trips at queue depth 1. That is the dominant cost on
// every sequential path in this codebase: persist_write_array() walks whole
// arrays a frame at a time (persist_records() alone is 323 commands),
// stream_write_chunk()'s is_last flush walks up to STREAM_MAX_FRAMES (16,384)
// frames, and stream_relocate_partition() pays three commands per page.
// See docs/AeroSLS-Persist-Write-Amplification-Scoping-v0.1.md.
//
// The pair below issue ONE command for up to NVME_MAX_PAGES_PER_XFER pages by
// building a real PRP list. Callers with more pages than that must chunk;
// the cap is deliberately conservative (see below).
//
// Requirements on buf, both of which the caller must guarantee:
//   * 4 KiB aligned -- NVMe requires every PRP entry after the first to have
//     a zero offset, so an unaligned buffer cannot be described by a PRP list
//     at all. (nvme_read_sync/nvme_write_sync above already carried the same
//     alignment requirement for their single page.)
//   * physically contiguous for the whole page_count -- this kernel is
//     identity-mapped below 4 GiB (see nvme_io.c's own note on the MMIO BAR),
//     so a contiguous C array satisfies this; scattered frame-pool frames do
//     NOT and must not be passed as one call.
//
// page_count of 0 is a no-op returning success. page_count above
// NVME_MAX_PAGES_PER_XFER is rejected rather than silently truncated.
// Returns 0 on success, non-zero on NVMe status error, matching the
// single-page functions' existing contract.
//
// Cap rationale: the NVMe Maximum Data Transfer Size (MDTS) is reported in
// Identify Controller, which this driver never reads (nvme_admin.c issues no
// Identify command). 128 KiB is below the smallest MDTS in practical use and
// well under the 512-entry limit of a single 4 KiB PRP list page, so it is
// safe without that query. Raising it should be gated on actually reading
// MDTS first, not guessed.
#define NVME_PAGE_SIZE            4096u
#define NVME_SECTORS_PER_PAGE     8u
#define NVME_MAX_PAGES_PER_XFER   32u    /* 128 KiB */

int nvme_read_pages_sync(uint64_t slba, void* buf, uint32_t page_count);
int nvme_write_pages_sync(uint64_t slba, const void* buf, uint32_t page_count);

// Pure PRP-address arithmetic, exposed for host testing.
//
// Split out deliberately: nvme_io.c's submit path does raw MMIO doorbell
// writes and polls a hardware completion queue, so it cannot be host-tested
// at all (no test in this suite links this file). The PRP construction is the
// part where a mistake silently corrupts unrelated memory -- a wrong list
// entry makes the controller DMA a page somewhere it was never told to -- so
// it is separated out here as pure arithmetic over caller-supplied addresses
// and covered by tests/nvme_prp_host_test.c. The untestable remainder stays
// as thin as it already was.
//
// Fills *out_prp1/*out_prp2 and, when page_count > 2, populates prp_list_page
// with (page_count - 1) entries. prp_list_page may be NULL when page_count
// <= 2 (no list is needed). Returns 0 on success, 1 on invalid arguments
// (page_count 0 or above the cap, unaligned buf, or a needed-but-NULL list).
int nvme_build_prp(uint64_t buf_phys, uint32_t page_count,
                   uint64_t* prp_list_page,
                   uint64_t* out_prp1, uint64_t* out_prp2);

#endif /* NVME_IO_H */
