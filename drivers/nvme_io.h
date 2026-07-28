#ifndef NVME_IO_H
#define NVME_IO_H

#include <stdint.h>
#include "nvme_admin.h"

// ─── I/O queue constants ──────────────────────────────────────────────────────
#define NVME_IO_QUEUE_ID    1
#define NVME_IO_QUEUE_SIZE  64
#define NVME_NSID           1

// NVMe NVM command opcodes
#define NVME_NVM_FLUSH      0x00
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

// ─── Durability barrier ──────────────────────────────────────────────────────
// Issues an NVM Flush, committing the controller's volatile write cache to
// non-volatile media. Until this returns, a completed nvme_write_*_sync() is
// only durable against a process/kernel restart -- NOT against power loss,
// because the controller is free to have acknowledged the write while it
// still sits in a volatile cache.
//
// No flush command was issued anywhere in this codebase before this was
// added, so every "persisted" write carried that caveat silently. Callers
// that need a real ordering guarantee (write A must reach media before write
// B) must call this between them; see kernel/persist.c's region commit, which
// uses it to guarantee a region's data is durable before the header that
// validates it becomes durable.
//
// Returns 0 on success, non-zero on NVMe status error (including 0xFD if the
// I/O queue was never created, matching the multi-page functions above).
int nvme_flush_sync(void);

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

// ─── Scatter-gather transfers ────────────────────────────────────────────────
// The functions above require one physically contiguous buffer. Several
// callers do not have one: kernel/stream.c holds each 4 KiB page of a stream
// as a separately allocated frame-pool frame (`se->frames[]`), so flushing a
// stream meant one command per frame -- up to STREAM_MAX_FRAMES (16,384) of
// them for a single 64 MiB stream.
//
// No copying is needed to fix that. A PRP list is natively a scatter list:
// every entry is an independent page address, and nothing requires them to be
// consecutive. The gather variants below take an array of page pointers and
// describe them to the controller directly.
//
// The pages must each be 4 KiB aligned (frame-pool frames always are), but
// need no relationship to each other in memory. They DO map to a contiguous
// LBA range on disk -- one NVMe command writes one run of logical blocks --
// so callers with holes in their page array must issue one call per
// contiguous run.
//
// page_count of 0 is a no-op returning success; above NVME_MAX_PAGES_PER_XFER
// is rejected rather than truncated. Returns 0 on success.
int nvme_read_pages_gather_sync(uint64_t slba, void* const* pages, uint32_t page_count);
int nvme_write_pages_gather_sync(uint64_t slba, const void* const* pages, uint32_t page_count);

// Pure PRP arithmetic for the gather case, exposed for host testing for the
// same reason nvme_build_prp() is: a wrong entry makes the controller DMA a
// page it was never told to touch.
int nvme_build_prp_gather(const void* const* pages, uint32_t page_count,
                          uint64_t* prp_list_page,
                          uint64_t* out_prp1, uint64_t* out_prp2);

#endif /* NVME_IO_H */
