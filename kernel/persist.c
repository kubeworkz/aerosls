// kernel/persist.c — L1/L2 kernel state persistence (Aurora-style snapshots)
//
// Strategy: write-on-mutation snapshots of all volatile kernel struct arrays to
// fixed NVMe LBA regions.  On boot, persist_restore_all() reads them back before
// stream_init() runs, making DB tables, records, schemas, and program binaries
// survive kernel reboots — the same guarantee streams already have.
//
// Inspired by Aurora SLS (rcslab/aurora): incremental checkpoints to NVMe.
// Simplified for AeroSLS: full-array writes (structs are small) rather than
// page-dirty tracking.  Trade-off: a few extra NVMe frames written per mutation;
// acceptable for a research/demo OS.

#include "persist.h"
#include "object_catalog.h"
#include "loader.h"
#include "kernel_io.h"
#include "partition.h"
#include "rowstore.h"
#include "row_index.h"       // Gap Remediation Phase D
#include "row_constraint.h"  // Gap Remediation Phase D
#include "row_journal.h"     // Gap Remediation Phase D
#include "vecstore.h"        // Gap Remediation Phase D
#include "vec_index.h"       // Gap Remediation Phase D
#include "mvcc.h"            // Gap Remediation Phase D -- mvcc_bootstrap_from_rowstore()
#include "database.h"        // Database Gap Analysis §1 -- databases[]/database_grants[]/database_next_id
#include "view.h"            // Query-Surface Roadmap Phase 5 -- views[]
#include "tenant.h"
#include "service_registry.h"   // Orchestration Plan Phase 4 -- services_registry[]           // Multitenant Isolation Gap Analysis §5 item 1 -- tenants[]/tenant_next_id
#include "../drivers/nvme_io.h"

// ─── 4 KiB DMA staging buffer (page-aligned for NVMe PRP) ────────────────────
// The NVMe driver passes this address as the PRP1 DMA buffer.  Physical alignment
// to 4096 is required; the linker places page-aligned BSS objects correctly.
static uint8_t __attribute__((aligned(4096))) p_buf[4096];

// ─── Internal helpers ─────────────────────────────────────────────────────────
static void p_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void p_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}
static int p_memcmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (n--) { if (*x != *y) return 1; x++; y++; }
    return 0;
}

// ─── Region checksums (crash/torn-write detection) ───────────────────────────
// FNV-1a, 64-bit. Hand-rolled and local to this file, matching this codebase's
// established per-file helper convention (p_memcpy/p_memset above, tn_* in
// tenant.c, db_* in database.c) rather than reaching into transaction.c's
// static CRC32. Table-free and trivially auditable; this detects accidental
// corruption from an interrupted write, not adversarial tampering, so a
// non-cryptographic checksum is the right tool.
#define P_FNV_OFFSET 1469598103934665603ULL
#define P_FNV_PRIME  1099511628211ULL

/* Running region-write state -- see the crash-consistency block below for
 * what these mean and why the header is written last. */
static uint64_t p_region_hdr_lba   = 0;
static uint64_t p_region_hdr_magic = 0;
static uint32_t p_region_v0 = 0, p_region_v1 = 0, p_region_v2 = 0;
static uint64_t p_region_csum      = 0;
static int      p_region_open      = 0;

static uint64_t p_csum_fold(uint64_t h, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n--) { h ^= (uint64_t)(*p++); h *= P_FNV_PRIME; }
    return h;
}

// Gap fix: every function below used to call nvme_write_sync()/
// nvme_read_sync() unconditionally, with no check that the NVMe I/O queue
// actually came up this boot. stream.c has always guarded its own NVMe call
// with `(io_sq && io_cq) ? nvme_read_sync(...) : -1` for exactly this reason
// (see nvme_io.h's own comment: io_sq/io_cq are NULL whenever nvme_io_init()
// was never called, e.g. kernel.c's boot sequence skips it entirely when the
// controller's MMIO BAR lands above the 4 GiB identity map) -- but every
// persist_*() function in this file (persist_catalog(), persist_vecstore_
// headers(), persist_rowstore_headers(), persist_partitions(), and everything
// else routing through persist_write_array()/persist_read_array()/stage_hdr()
// below) never had that same guard. Concretely: on a boot where NVMe is
// unavailable, io_sq/io_cq are NULL, so nvme_io_submit_sync() dereferenced
// those NULLs and rang a "doorbell" at a bogus address derived from a
// zeroed/never-set nvme_ctrl.mmio_base -- nothing real ever acknowledges it,
// so the completion poll either (a) spun until NVME_IO_TIMEOUT (this
// session's other fix, which stopped it from hanging the single-threaded
// HTTP loop forever but still burned the full timeout for nothing), or (b)
// by sheer luck read a few bytes of unrelated low memory that happened to
// already satisfy the phase-tag check, silently "succeeding" without ever
// writing anything real -- which is why persistence looked flaky (worked
// once, hung the next time) rather than consistently broken. Guarding here,
// once, at the three shared low-level helpers, protects every persist_*()
// caller in one place instead of needing the same fix repeated at each of
// their call sites.
static int persist_nvme_available(void) { return io_sq && io_cq; }

// ─── Multi-page batching staging buffer ──────────────────────────────────────
// Both array helpers below used to issue one 4 KiB NVMe command per frame,
// each preceded by a full-page memset and a memcpy through the single p_buf --
// so persist_records() alone was 323 synchronous submit-and-poll round trips
// plus 323 page-sized memsets. nvme_write_pages_sync()/nvme_read_pages_sync()
// (drivers/nvme_io.c) now move up to NVME_MAX_PAGES_PER_XFER pages per
// command via a real PRP list, cutting that to ceil(322/32) + 1 = 12.
//
// Why stage through a buffer at all rather than DMA straight out of the
// caller's array: a PRP list requires the whole transfer to start 4 KiB
// aligned, and none of the persisted arrays (object_records[], databases[],
// ...) carry an alignment attribute -- they are ordinary globals. Copying
// into this page-aligned, physically contiguous scratch buffer satisfies
// that requirement without touching a single array declaration across the
// codebase. The copy is one large sequential memcpy per batch, replacing the
// per-frame memset+memcpy pair it supersedes, so it is strictly less CPU work
// than before, not more.
static uint8_t __attribute__((aligned(4096)))
       p_batch[NVME_MAX_PAGES_PER_XFER * NVME_PAGE_SIZE];

// Write `total_bytes` from `src` to successive 4-KiB NVMe frames starting at
// `lba`.  Each frame is 8 NVMe 512-byte sectors. Full pages go out in
// multi-page batches; only a trailing partial frame still needs the
// zero-padded single-page path (the on-disk image must have that tail
// zero-filled rather than carrying whatever followed the array in memory).
static void persist_write_array(const void* src, uint32_t total_bytes, uint64_t lba) {
    // Fold before the availability check so the checksum reflects the region's
    // logical content regardless of whether NVMe is up, keeping multi-array
    // regions consistent.
    if (p_region_open) p_region_csum = p_csum_fold(p_region_csum, src, total_bytes);
    if (!persist_nvme_available()) return;
    const uint8_t* p = (const uint8_t*)src;

    uint32_t full_pages = total_bytes / NVME_PAGE_SIZE;
    uint32_t tail_bytes = total_bytes % NVME_PAGE_SIZE;

    while (full_pages > 0) {
        uint32_t batch = full_pages < NVME_MAX_PAGES_PER_XFER
                       ? full_pages : NVME_MAX_PAGES_PER_XFER;
        uint32_t bytes = batch * NVME_PAGE_SIZE;
        p_memcpy(p_batch, p, bytes);
        nvme_write_pages_sync(lba, p_batch, batch);
        p          += bytes;
        lba        += (uint64_t)batch * NVME_SECTORS_PER_PAGE;
        full_pages -= batch;
    }

    if (tail_bytes > 0) {
        p_memset(p_buf, 0, NVME_PAGE_SIZE);
        p_memcpy(p_buf, p, tail_bytes);
        nvme_write_sync(lba, p_buf);
    }
}

// ─── Shadow-compare writes (write only the frames that actually changed) ─────
// persist_write_array() above still writes every frame of its region. For
// object_records[] that is 1.26 MiB to change as little as one 321-byte
// SLSRecordField -- roughly 4,100x write amplification (see the scoping doc).
// Multi-page batching cut the COMMAND count for that; it did not reduce the
// BYTES, which is what costs SSD wear and memory bandwidth.
//
// ─── Why a shadow copy rather than dirty marks at the mutation sites ────
// The scoping doc originally proposed having each mutation site mark the byte
// range it touched, and named the risk plainly: a MISSED mark means a change
// that lives in RAM, is never written, and silently vanishes on reboot --
// invisible until someone notices absent data, and easy to reintroduce later
// when a new mutation site is added and the mark is forgotten.
//
// Comparing against a shadow copy removes that entire failure class by
// construction. Dirtiness is DERIVED FROM THE BYTES, not asserted by a
// caller, so there is no mark to forget: any mutation, from any call site,
// present or future, is detected. It also needs zero call-site changes.
//
// The cost is one shadow buffer per covered region plus a sequential compare
// pass per write. That compare is memory-bandwidth work measured in
// microseconds against NVMe round trips measured in tens of microseconds
// each, and Option A's batching already made these calls infrequent -- so it
// is a clearly favourable trade, and a much safer one than the alternative.
//
// Correctness rests on one invariant: the shadow must equal what is actually
// on disk for the region. That holds because PERSIST_REC_ENT_LBA has exactly
// one writer (this function) and one reader (persist_restore_all()), verified
// by grep before this was built. If a second writer to a shadowed region is
// ever added, it MUST call persist_shadow_invalidate() or the shadow becomes
// stale-optimistic and real changes will be skipped. The verify mode below
// exists to catch exactly that class of mistake in testing.
//
// Scoped deliberately to object_records[] for now: at 322 frames it is both
// the largest region and the only one on the per-mutation hot path. The other
// thirteen are 1-89 frames and keep the unconditional whole-region write. The
// helper below is region-agnostic, so opting another region in is adding a
// shadow buffer and a flag, not new logic.
static uint8_t p_shadow_records[sizeof(object_records)];
static int     p_shadow_records_valid = 0;

// The other two per-mutation hot regions. Both are written on every row
// insert/delete or journaled mutation (kernel/rowstore.c:664/747,
// kernel/row_journal.c:119/149/161), so a single SQL row insert into a
// journaled table paid 40 + 35 = 75 whole-region frames before this.
//
// The remaining ELEVEN persisted regions are deliberately NOT shadowed.
// They are administrative -- written on create-database, create-view,
// create-tenant, schema-set, program-upload -- not per mutation, and range
// from 1 to 89 frames. Shadowing all of them would cost roughly 1.4 MiB of
// additional BSS to optimise operations a human performs a handful of times
// per boot. Deciding not to is the point: the mechanism is region-agnostic
// and opting one in later is adding a buffer and a flag, so this is a
// reversible judgement about which regions are hot, not a limitation.
static uint8_t p_shadow_rowstore[sizeof(table_headers)];
static int     p_shadow_rowstore_valid = 0;
static uint8_t p_shadow_rowjournal[sizeof(row_journal_buffer)];
static int     p_shadow_rowjournal_valid = 0;

// Diagnostics/tests: how many 4 KiB frames the last shadow-compared write
// actually put on the wire.
static uint32_t p_last_frames_written = 0;
uint32_t persist_last_frames_written(void) { return p_last_frames_written; }

// Verify mode: after a shadow-compared write, read the ENTIRE region back and
// compare it against memory, so a skipped-but-needed frame fails loudly here
// instead of silently surviving until the next boot. Off by default (it costs
// a full-region read per write); intended for tests and for bring-up after
// any change to a shadowed region's write path.
static int p_verify_mode = 0;
void persist_verify_set(int on) { p_verify_mode = on ? 1 : 0; }
int  persist_verify_get(void)   { return p_verify_mode; }

// Force the next write of every shadowed region to be a full write. Called
// after restore-from-disk paths and available as an escape hatch whenever the
// on-disk image may no longer match the shadow (format-version mismatch, a
// newly added second writer, a failed write).
void persist_shadow_invalidate(void) {
    p_shadow_records_valid    = 0;
    p_shadow_rowstore_valid   = 0;
    p_shadow_rowjournal_valid = 0;
}

// Reads the whole region back off NVMe and compares against memory. Returns
// the number of differing frames (0 == disk and memory agree).
static uint32_t persist_verify_region(const void* src, uint32_t total_bytes, uint64_t lba) {
    const uint8_t* p = (const uint8_t*)src;
    uint32_t frames = (total_bytes + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
    uint32_t bad = 0;
    for (uint32_t f = 0; f < frames; f++) {
        if (nvme_read_sync(lba + (uint64_t)f * NVME_SECTORS_PER_PAGE, p_buf) != 0) { bad++; continue; }
        uint32_t off  = f * NVME_PAGE_SIZE;
        uint32_t span = (total_bytes - off) < NVME_PAGE_SIZE ? (total_bytes - off) : NVME_PAGE_SIZE;
        if (p_memcmp(p_buf, p + off, span) != 0) bad++;
    }
    return bad;
}

// Writes only the frames whose contents differ from `shadow`, coalescing
// CONSECUTIVE dirty frames into single multi-page commands (so this composes
// with the batching above rather than undoing it). Updates the shadow to
// match what was written.
static void persist_write_array_diffed(const void* src, uint32_t total_bytes, uint64_t lba,
                                       uint8_t* shadow, int* shadow_valid) {
    // Checksum covers the region's whole logical content, not just the frames
    // this call happens to write -- shadow-compare skips clean frames, but the
    // on-disk image still contains all of it.
    if (p_region_open) p_region_csum = p_csum_fold(p_region_csum, src, total_bytes);
    if (!persist_nvme_available()) return;

    // Cold start, or the shadow was explicitly invalidated: the on-disk image
    // may be absent, stale, or from another kernel build, so nothing can be
    // safely skipped. Write everything, then the shadow is trustworthy.
    if (!*shadow_valid) {
        int reopen = p_region_open;
        p_region_open = 0;              /* already folded above -- don't double-count */
        persist_write_array(src, total_bytes, lba);
        p_region_open = reopen;
        p_memcpy(shadow, src, total_bytes);
        *shadow_valid = 1;
        p_last_frames_written = (total_bytes + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;
        return;
    }

    const uint8_t* p = (const uint8_t*)src;
    uint32_t full_pages = total_bytes / NVME_PAGE_SIZE;
    uint32_t tail_bytes = total_bytes % NVME_PAGE_SIZE;
    uint32_t written    = 0;

    uint32_t run_start = 0;   // first page index of the current dirty run
    uint32_t run_len   = 0;

    for (uint32_t pg = 0; pg <= full_pages; pg++) {
        int dirty = 0;
        if (pg < full_pages) {
            dirty = p_memcmp(p + (uint32_t)pg * NVME_PAGE_SIZE,
                             shadow + (uint32_t)pg * NVME_PAGE_SIZE,
                             NVME_PAGE_SIZE) != 0;
        }
        // pg == full_pages is a sentinel pass that flushes any trailing run.
        if (dirty) {
            if (run_len == 0) run_start = pg;
            run_len++;
            if (run_len < NVME_MAX_PAGES_PER_XFER) continue;
        } else if (run_len == 0) {
            continue;
        }
        // Flush the accumulated run.
        uint32_t bytes = run_len * NVME_PAGE_SIZE;
        p_memcpy(p_batch, p + run_start * NVME_PAGE_SIZE, bytes);
        nvme_write_pages_sync(lba + (uint64_t)run_start * NVME_SECTORS_PER_PAGE,
                              p_batch, run_len);
        written += run_len;
        run_len  = 0;
    }

    // Trailing partial frame: compare only the live bytes, but write the
    // zero-padded full frame, matching persist_write_array()'s own contract
    // that the on-disk tail is zero-filled rather than carrying whatever
    // followed the array in memory.
    if (tail_bytes > 0) {
        uint32_t off = full_pages * NVME_PAGE_SIZE;
        if (p_memcmp(p + off, shadow + off, tail_bytes) != 0) {
            p_memset(p_buf, 0, NVME_PAGE_SIZE);
            p_memcpy(p_buf, p + off, tail_bytes);
            nvme_write_sync(lba + (uint64_t)full_pages * NVME_SECTORS_PER_PAGE, p_buf);
            written++;
        }
    }

    // Shadow now mirrors the on-disk image.
    p_memcpy(shadow, src, total_bytes);
    p_last_frames_written = written;

    if (p_verify_mode) {
        uint32_t bad = persist_verify_region(src, total_bytes, lba);
        if (bad != 0) {
            kernel_serial_printf(
                "[PERSIST] VERIFY FAILED: %u frame(s) on disk differ from memory at LBA %lu "
                "-- a needed write was skipped. Forcing a full rewrite.\n",
                (unsigned)bad, (unsigned long)lba);
            *shadow_valid = 0;               /* next write repairs it in full */
            int reopen2 = p_region_open;
            p_region_open = 0;           /* already folded above */
            persist_write_array(src, total_bytes, lba);
            p_region_open = reopen2;
            p_memcpy(shadow, src, total_bytes);
            *shadow_valid = 1;
        }
    }
}

// Read back `total_bytes` into `dst` from NVMe.  Stops silently on NVMe error
// (the caller will detect corruption via the magic-number mismatch on next boot).
static void persist_read_array(void* dst, uint32_t total_bytes, uint64_t lba) {
    if (!persist_nvme_available()) return;
    uint8_t* d = (uint8_t*)dst;

    uint32_t full_pages = total_bytes / NVME_PAGE_SIZE;
    uint32_t tail_bytes = total_bytes % NVME_PAGE_SIZE;

    while (full_pages > 0) {
        uint32_t batch = full_pages < NVME_MAX_PAGES_PER_XFER
                       ? full_pages : NVME_MAX_PAGES_PER_XFER;
        uint32_t bytes = batch * NVME_PAGE_SIZE;
        if (nvme_read_pages_sync(lba, p_batch, batch) != 0) return;
        p_memcpy(d, p_batch, bytes);
        d          += bytes;
        lba        += (uint64_t)batch * NVME_SECTORS_PER_PAGE;
        full_pages -= batch;
    }

    if (tail_bytes > 0) {
        if (nvme_read_sync(lba, p_buf) != 0) return;
        p_memcpy(d, p_buf, tail_bytes);
    }
}

// ─── Crash consistency: checksummed header, written AFTER its data ───────────
// The original order was header first, then data, with the restore side
// checking only the magic value and the recorded size. A crash partway
// through a multi-frame data write therefore left a VALID header pointing at
// a half-old, half-new array, and the next boot accepted it silently. There
// was no checksum, no generation counter, and no torn-write detection
// anywhere in the persistence layer. See §3 of
// docs/AeroSLS-Persist-Write-Amplification-Scoping-v0.1.md.
//
// The fix is two changes that only work together:
//
//   1. The header is now written LAST, and carries a checksum of the region's
//      data. A crash before the header lands leaves the PREVIOUS header, whose
//      checksum will not match the partially-rewritten data -- so the torn
//      state is detected at restore instead of trusted.
//   2. An NVM Flush separates the two, so the data is durable on media before
//      the header that validates it becomes durable. Without the barrier the
//      controller could commit them in the opposite order and reintroduce
//      exactly the window being closed.
//
// This fails CLOSED: a detected mismatch means that region cold-starts rather
// than loading torn data. It is detection, not recovery -- recovering the
// previous good copy would need A/B double-buffering of every region, which
// the current LBA layout has nowhere near enough slack for (78 frames spare
// against 679 in use).
//
// stage_hdr() records what the header will say and resets the running
// checksum; persist_write_array*() folds each array it writes into that
// checksum; persist_region_commit() performs the barrier-header-barrier
// sequence. Every persist_*() below therefore reads: stage, write arrays,
// commit.

// Byte offset of the checksum within the 4 KiB header frame. Chosen past the
// existing magic(8) + v0/v1/v2(12) = 20 bytes, so this is a purely ADDITIVE
// format change: a header written before this existed has zeros here, which
// the restore side treats as "no checksum recorded" and accepts exactly as it
// did before. One-way upgrade, same accepted trade-off this codebase's other
// one-way format changes documented (see PERSIST_STORAGE_ISOLATION_PHASE3_MARK
// in persist.h).
#define P_HDR_CSUM_OFF 24

static void stage_hdr(uint64_t lba, uint64_t magic,
                      uint32_t v0, uint32_t v1, uint32_t v2) {
    p_region_hdr_lba   = lba;
    p_region_hdr_magic = magic;
    p_region_v0 = v0; p_region_v1 = v1; p_region_v2 = v2;
    p_region_csum = P_FNV_OFFSET;
    p_region_open = 1;
}

static void persist_region_commit(void) {
    if (!p_region_open) return;
    p_region_open = 0;
    if (!persist_nvme_available()) return;

    // Barrier 1: every data frame of this region reaches media before the
    // header that vouches for it is even submitted.
    nvme_flush_sync();

    uint64_t csum = p_region_csum;
    if (csum == 0) csum = 1;   /* 0 is the "no checksum recorded" sentinel */

    p_memset(p_buf, 0, 4096);
    p_memcpy(p_buf +  0, &p_region_hdr_magic, 8);
    p_memcpy(p_buf +  8, &p_region_v0, 4);
    p_memcpy(p_buf + 12, &p_region_v1, 4);
    p_memcpy(p_buf + 16, &p_region_v2, 4);
    p_memcpy(p_buf + P_HDR_CSUM_OFF, &csum, 8);
    nvme_write_sync(p_region_hdr_lba, p_buf);

    // Barrier 2: the region is genuinely durable when this returns, rather
    // than merely acknowledged by a volatile controller cache.
    nvme_flush_sync();
}


// ─── Restore-side verification: check BEFORE loading ─────────────────────────
// persist_read_array() loads straight into the live arrays, so verifying after
// the fact would already have polluted kernel state with torn data. Instead
// every region is scanned once up front: its data is streamed off disk and
// checksummed WITHOUT being retained anywhere, and only regions whose checksum
// matches their header are then allowed to load.
//
// The span table below must list exactly the arrays each persist_*() writes,
// in the same order, because the write side folds them into the checksum in
// that order. It was extracted mechanically from those functions rather than
// transcribed by hand; a mismatch would surface immediately as every region
// failing verification on the first boot after a write.
#define P_MAX_SPANS 3
struct PersistRegionSpec {
    uint64_t hdr_lba;
    uint64_t magic;
    int      nspans;
    struct { uint64_t lba; uint32_t bytes; } spans[P_MAX_SPANS];
};

static const struct PersistRegionSpec p_region_specs[] = {
    { PERSIST_CAT_HDR_LBA, PERSIST_MAGIC_CAT,
      2, { { PERSIST_CAT_ENT_LBA, (uint32_t)sizeof(object_catalog) }, { PERSIST_ROLE_ENT_LBA, (uint32_t)sizeof(role_table) } } },
    { PERSIST_REC_HDR_LBA, PERSIST_MAGIC_REC,
      1, { { PERSIST_REC_ENT_LBA, (uint32_t)sizeof(object_records) } } },
    { PERSIST_SCH_HDR_LBA, PERSIST_MAGIC_SCH,
      1, { { PERSIST_SCH_ENT_LBA, (uint32_t)sizeof(object_schemas) } } },
    { PERSIST_PROG_HDR_LBA, PERSIST_MAGIC_PROG,
      1, { { PERSIST_PROG_DAT_LBA, (uint32_t)sizeof(service_binaries) } } },
    { PERSIST_PART_HDR_LBA, PERSIST_MAGIC_PART,
      3, { { PERSIST_PART_ENT_LBA, (uint32_t)sizeof(partition_table) }, { PERSIST_PART_ASSIGN_LBA, (uint32_t)sizeof(partition_assign_table) }, { PERSIST_PART_OWNER_LBA, (uint32_t)sizeof(partition_owner_table) } } },
    { PERSIST_ROWSTORE_HDR_LBA, PERSIST_MAGIC_ROWSTORE,
      2, { { PERSIST_ROWSTORE_ENT_LBA, (uint32_t)sizeof(table_headers) }, { PERSIST_ROWSTORE_PARTCURSOR_LBA, (uint32_t)sizeof(rowstore_partition_cursor) } } },
    { PERSIST_ROW_CONSTRAINT_HDR_LBA, PERSIST_MAGIC_ROW_CONSTRAINT,
      1, { { PERSIST_ROW_CONSTRAINT_ENT_LBA, (uint32_t)sizeof(row_constraints) } } },
    { PERSIST_ROW_INDEX_HDR_LBA, PERSIST_MAGIC_ROW_INDEX,
      1, { { PERSIST_ROW_INDEX_ENT_LBA, (uint32_t)sizeof(row_indexes) } } },
    { PERSIST_VECSTORE_HDR_LBA, PERSIST_MAGIC_VECSTORE,
      2, { { PERSIST_VECSTORE_ENT_LBA, (uint32_t)sizeof(vector_collections) }, { PERSIST_VECSTORE_PARTCURSOR_LBA, (uint32_t)sizeof(vecstore_partition_cursor) } } },
    { PERSIST_VEC_INDEX_HDR_LBA, PERSIST_MAGIC_VEC_INDEX,
      1, { { PERSIST_VEC_INDEX_ENT_LBA, (uint32_t)sizeof(vec_indexes) } } },
    { PERSIST_ROW_JOURNAL_HDR_LBA, PERSIST_MAGIC_ROW_JOURNAL,
      2, { { PERSIST_ROW_JOURNAL_ENT_LBA, (uint32_t)sizeof(row_journal_buffer) }, { PERSIST_ROW_JOURNAL_ATTACH_LBA, (uint32_t)sizeof(row_journal_attachments) } } },
    { PERSIST_DATABASE_HDR_LBA, PERSIST_MAGIC_DATABASE,
      2, { { PERSIST_DATABASE_ENT_LBA, (uint32_t)sizeof(databases) }, { PERSIST_DATABASE_GRANT_LBA, (uint32_t)sizeof(database_grants) } } },
    { PERSIST_VIEW_HDR_LBA, PERSIST_MAGIC_VIEW,
      1, { { PERSIST_VIEW_ENT_LBA, (uint32_t)sizeof(views) } } },
    { PERSIST_TENANT_HDR_LBA, PERSIST_MAGIC_TENANT,
      1, { { PERSIST_TENANT_ENT_LBA, (uint32_t)sizeof(tenants) } } },
    { PERSIST_SERVICE_HDR_LBA, PERSIST_MAGIC_SERVICE,
      1, { { PERSIST_SERVICE_ENT_LBA, (uint32_t)sizeof(services_registry) } } },
};
#define P_REGION_COUNT ((int)(sizeof(p_region_specs)/sizeof(p_region_specs[0])))

// Bit i set == region i's on-disk image was verified (or predates checksums).
static uint32_t p_trusted_mask = 0;
static int      p_scan_done    = 0;

// Dedicated scratch frame for the scan. Deliberately NOT p_buf: each restore
// block reads its header into p_buf and then reads fields out of it AFTER
// calling persist_region_trusted(), so a scan sharing p_buf would clobber the
// caller's header mid-block and make every region look size-mismatched. (That
// is not hypothetical -- it is exactly what the first run of
// tests/persist_crash_consistency_host_test.c caught.)
static uint8_t __attribute__((aligned(4096))) p_scan_buf[4096];

// Streams one span off disk, folding it into the running checksum without
// retaining it. Returns 0 on success, 1 if any frame could not be read.
static int p_csum_span_from_disk(uint64_t lba, uint32_t bytes, uint64_t* h) {
    uint32_t rem = bytes;
    while (rem > 0) {
        uint32_t chunk = rem < NVME_PAGE_SIZE ? rem : NVME_PAGE_SIZE;
        if (nvme_read_sync(lba, p_scan_buf) != 0) return 1;
        *h = p_csum_fold(*h, p_scan_buf, chunk);
        rem -= chunk;
        lba += NVME_SECTORS_PER_PAGE;
    }
    return 0;
}

// Scans every region once, recording which are safe to load.
static void persist_scan_regions(void) {
    p_trusted_mask = 0;
    p_scan_done    = 1;
    if (!persist_nvme_available()) return;

    for (int i = 0; i < P_REGION_COUNT; i++) {
        const struct PersistRegionSpec* r = &p_region_specs[i];
        if (nvme_read_sync(r->hdr_lba, p_scan_buf) != 0) continue;

        uint64_t magic = 0, stored = 0;
        p_memcpy(&magic, p_scan_buf, 8);
        if (magic != r->magic) continue;      /* absent or foreign -- cold start regardless */
        p_memcpy(&stored, p_scan_buf + P_HDR_CSUM_OFF, 8);

        if (stored == 0) {
            /* Header predates checksums (one-way, additive format change).
             * Accepted exactly as before -- rejecting it would discard every
             * image written by an older build. Such an image simply has no
             * torn-write protection until it is next rewritten. */
            p_trusted_mask |= (1u << i);
            continue;
        }

        uint64_t h = P_FNV_OFFSET;
        int io_ok = 1;
        for (int sp = 0; sp < r->nspans && io_ok; sp++)
            if (p_csum_span_from_disk(r->spans[sp].lba, r->spans[sp].bytes, &h) != 0) io_ok = 0;
        if (h == 0) h = 1;                    /* mirrors the write side's sentinel guard */

        if (io_ok && h == stored) {
            p_trusted_mask |= (1u << i);
        } else {
            kernel_serial_printf(
                "[PERSIST] Region at LBA %lu FAILED checksum -- torn or interrupted write "
                "detected; cold-starting this region rather than loading corrupt state.\n",
                (unsigned long)r->hdr_lba);
        }
    }
}

// True if this region's on-disk image may be loaded. Keyed on the magic value
// so each restore block needs exactly one extra condition.
static int persist_region_trusted(uint64_t magic) {
    if (!p_scan_done) persist_scan_regions();
    for (int i = 0; i < P_REGION_COUNT; i++)
        if (p_region_specs[i].magic == magic) return (p_trusted_mask & (1u << i)) != 0;
    return 1;   /* unknown region: unchanged behaviour */
}

// ─── Gap Remediation Phase D: HNSW backfill helper ───────────────────────────
// vec_index_create() never backfills an already-populated collection (see
// vec_index.h's own point 7) -- restoring a persisted HNSW index definition
// needs exactly that backfill, once, right after creation. This callback
// (used only by persist_restore_all()'s vec-index restore block) feeds every
// already-restored entry in the collection through vec_index_notify_
// insert(), the same auto-maintenance entry point vecstore_insert() calls
// on every live write -- reused here rather than duplicated.
struct persist_vec_backfill_ctx { const char* collection_name; };
static void persist_vec_backfill_cb(struct VecId id, uint64_t external_id,
                                    const struct VecValues* values, void* ctxp) {
    struct persist_vec_backfill_ctx* ctx = (struct persist_vec_backfill_ctx*)ctxp;
    vec_index_notify_insert(0, ctx->collection_name, id, external_id, values);
}

// ─── Deferred / batched persistence ──────────────────────────────────────────
// Every persist_*() below rewrites its whole region -- persist_records() alone
// costs 323 synchronous 4 KiB NVMe commands (1.26 MiB) per call. That is
// tolerable once per operation, but sys_sls_tx_commit() (kernel/transaction.c)
// applies its staged WAL entries by calling sys_sls_update() in a loop, and
// each of those lands on the direct-write path and calls persist_records()
// again -- so committing N operations wrote N * 1.26 MiB where one write of
// the final state would have been equivalent.
//
// This is a batching bracket, not a new persistence policy: between
// persist_defer_begin() and the matching persist_defer_end(), a persist_*()
// call records that its region needs writing and returns; persist_defer_end()
// then performs exactly one real write per distinct region that was touched.
// The end state on disk is byte-for-byte what the un-batched sequence would
// have produced, because every persist_*() writes the *current* contents of
// its array rather than a delta -- writing it once at the end of a batch and
// writing it after every step differ only in how many times the same final
// bytes are written. Nothing is skipped, and no durability window is
// introduced beyond the duration of the bracket itself.
//
// Nestable (depth-counted) so a caller inside an already-deferred region does
// not flush early. Unbalanced persist_defer_end() calls are ignored rather
// than underflowing the depth.
//
// Deliberately NOT wired into flush_daemon_tick(): that runs on Core 1
// (kernel/smp.c's ap_kernel_main()), while every persist_*() caller today --
// both http_server_run() and sls_shell_loop(), see kernel/kernel.c's own
// "8. HTTP server" block -- runs on the BSP. persist.c's p_buf is a single
// shared static staging buffer with no lock, so moving persist writes onto
// the AP would race the BSP's own persist calls and tear the staging buffer.
// A cross-core deferred flush needs locking this kernel does not have; the
// batching bracket below gets the large win (N->1 per transaction) without
// touching the concurrency model at all. The same manual batching idea is
// already precedented in kernel/object_catalog.c's vfree-partition loop
// ("batched into one persist_catalog() call at the end instead of one per").
#define PERSIST_PEND_CAT           (1u <<  0)
#define PERSIST_PEND_REC           (1u <<  1)
#define PERSIST_PEND_SCH           (1u <<  2)
#define PERSIST_PEND_PROG          (1u <<  3)
#define PERSIST_PEND_PART          (1u <<  4)
#define PERSIST_PEND_ROWSTORE      (1u <<  5)
#define PERSIST_PEND_ROWCONSTRAINT (1u <<  6)
#define PERSIST_PEND_ROWINDEX      (1u <<  7)
#define PERSIST_PEND_VECSTORE      (1u <<  8)
#define PERSIST_PEND_VECINDEX      (1u <<  9)
#define PERSIST_PEND_ROWJOURNAL    (1u << 10)
#define PERSIST_PEND_DATABASE      (1u << 11)
#define PERSIST_PEND_VIEW          (1u << 12)
#define PERSIST_PEND_TENANT        (1u << 13)
#define PERSIST_PEND_SERVICE       (1u << 14)   /* Orchestration Plan Phase 4 */

static uint32_t persist_defer_depth   = 0;
static uint32_t persist_pending_mask  = 0;

// Returns 1 if the caller should defer (and records the region as pending),
// 0 if it should write immediately. Every persist_*() calls this first.
static int persist_defer_note(uint32_t region_bit) {
    if (persist_defer_depth == 0) return 0;
    persist_pending_mask |= region_bit;
    return 1;
}

void persist_defer_begin(void) { persist_defer_depth++; }

int persist_defer_active(void) { return persist_defer_depth != 0; }

uint32_t persist_defer_pending_mask(void) { return persist_pending_mask; }

void persist_defer_end(void) {
    if (persist_defer_depth == 0) return;      /* unbalanced -- ignore, don't underflow */
    if (--persist_defer_depth != 0) return;    /* still inside an outer bracket */

    uint32_t pend = persist_pending_mask;
    persist_pending_mask = 0;                  /* cleared first: the calls below re-enter
                                                * persist_*() with depth now 0, so they
                                                * write for real rather than re-marking */
    if (pend & PERSIST_PEND_CAT)           persist_catalog();
    if (pend & PERSIST_PEND_REC)           persist_records();
    if (pend & PERSIST_PEND_SCH)           persist_schemas();
    if (pend & PERSIST_PEND_PROG)          persist_programs();
    if (pend & PERSIST_PEND_PART)          persist_partitions();
    if (pend & PERSIST_PEND_ROWSTORE)      persist_rowstore_headers();
    if (pend & PERSIST_PEND_ROWCONSTRAINT) persist_row_constraints();
    if (pend & PERSIST_PEND_ROWINDEX)      persist_row_index_defs();
    if (pend & PERSIST_PEND_VECSTORE)      persist_vecstore_headers();
    if (pend & PERSIST_PEND_VECINDEX)      persist_vec_index_defs();
    if (pend & PERSIST_PEND_ROWJOURNAL)    persist_row_journal();
    if (pend & PERSIST_PEND_DATABASE)      persist_databases();
    if (pend & PERSIST_PEND_VIEW)          persist_views();
    if (pend & PERSIST_PEND_TENANT)        persist_tenants();
    if (pend & PERSIST_PEND_SERVICE)       persist_services();
}

// ─── persist_catalog ─────────────────────────────────────────────────────────
// Writes object_catalog[] and role_table[].  Called after valloc/vfree/role_set.
void persist_catalog(void) {
    if (persist_defer_note(PERSIST_PEND_CAT)) return;
    if (!io_sq || !io_cq) return;
    uint32_t cat_bytes  = (uint32_t)sizeof(object_catalog);
    uint32_t role_bytes = (uint32_t)sizeof(role_table);
    stage_hdr(PERSIST_CAT_HDR_LBA, PERSIST_MAGIC_CAT,
              object_catalog_count, cat_bytes, role_bytes);
    persist_write_array(object_catalog, cat_bytes,  PERSIST_CAT_ENT_LBA);
    persist_write_array(role_table,     role_bytes, PERSIST_ROLE_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Catalog snapshot written.\n");
}

// ─── persist_records ─────────────────────────────────────────────────────────
// Writes the full object_records[] array.  Called after direct insert/update/delete.
//
// Cost, corrected: this comment previously read "~232 KiB (57 NVMe frames) --
// acceptable for a research kernel." That figure was stale by 5.6x; it
// described an older, narrower struct SLSRecordField and was never updated
// when the field widened. The real cost is sizeof(object_records) =
// 128 * 10,284 B = 1,316,352 B = 322 data frames + 1 header frame = 323
// separate synchronous 4 KiB NVMe commands, per call -- and this is called
// once per direct insert/update/delete, to change as little as one 321-byte
// SLSRecordField. persist.h's own LBA layout table (322 frames) is the
// accurate reference and always was; only this comment disagreed with it.
// See docs/AeroSLS-Persist-Write-Amplification-Scoping-v0.1.md.
void persist_records(void) {
    if (persist_defer_note(PERSIST_PEND_REC)) return;
    if (!io_sq || !io_cq) return;
    uint32_t rec_bytes = (uint32_t)sizeof(object_records);
    stage_hdr(PERSIST_REC_HDR_LBA, PERSIST_MAGIC_REC, rec_bytes, 0, 0);
    // Shadow-compared: writes only the frames whose bytes actually changed
    // since the last write. See persist_write_array_diffed() for why this is
    // a shadow copy rather than caller-supplied dirty marks.
    persist_write_array_diffed(object_records, rec_bytes, PERSIST_REC_ENT_LBA,
                               p_shadow_records, &p_shadow_records_valid);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Records snapshot written.\n");
}

// ─── persist_schemas ─────────────────────────────────────────────────────────
void persist_schemas(void) {
    if (persist_defer_note(PERSIST_PEND_SCH)) return;
    if (!io_sq || !io_cq) return;
    uint32_t sch_bytes = (uint32_t)sizeof(object_schemas);
    stage_hdr(PERSIST_SCH_HDR_LBA, PERSIST_MAGIC_SCH, sch_bytes, 0, 0);
    persist_write_array(object_schemas, sch_bytes, PERSIST_SCH_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Schemas snapshot written.\n");
}

// ─── persist_programs ────────────────────────────────────────────────────────
// Writes service_binaries[] (name, object_id, raw binary data, size, flags).
// Called after the final upload chunk (is_last=1) so only complete binaries
// are snapshotted.
void persist_programs(void) {
    if (persist_defer_note(PERSIST_PEND_PROG)) return;
    if (!io_sq || !io_cq) return;
    uint32_t prog_bytes = (uint32_t)sizeof(service_binaries);
    stage_hdr(PERSIST_PROG_HDR_LBA, PERSIST_MAGIC_PROG, prog_bytes, 0, 0);
    persist_write_array(service_binaries, prog_bytes, PERSIST_PROG_DAT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Programs snapshot written.\n");
}

// ─── persist_partitions ───────────────────────────────────────────────────────
// Phase 10 (LPAR persistence). Writes partition_table[] and
// partition_assign_table[] together in one header, same "two arrays, one
// magic-tagged header" shape persist_catalog() already uses for
// object_catalog[]/role_table[] — partition_table[]/partition_assign_table[]
// are that same pair's sibling (defined-partitions table + uid-assignment
// table), so the persistence shape matches on purpose.
//
// Multi-Node Partition Scaling Roadmap, Phase 2: also writes
// partition_owner_table[] as a third array under the same header, using
// stage_hdr()'s previously-unused v2 size slot (Phase 10 always passed 0
// there -- see the restore block's own comment on why this is safe for
// old snapshots). Ownership needs to survive a reboot the same way
// partition identity itself does: partition_table[]/partition_assign_
// table[] are restored directly into the arrays by persist_restore_all(),
// bypassing partition_create() entirely, so if ownership were only ever
// stamped inside partition_create() (as Phase 2's own scope note first
// suggested), every restored partition would come back with NO ownership
// row at all after a reboot -- a real, silent regression for the one
// property this whole roadmap exists to make durable.
void persist_partitions(void) {
    if (persist_defer_note(PERSIST_PEND_PART)) return;
    if (!io_sq || !io_cq) return;
    uint32_t part_bytes   = (uint32_t)sizeof(partition_table);
    uint32_t assign_bytes = (uint32_t)sizeof(partition_assign_table);
    uint32_t owner_bytes  = (uint32_t)sizeof(partition_owner_table);
    stage_hdr(PERSIST_PART_HDR_LBA, PERSIST_MAGIC_PART,
              part_bytes, assign_bytes, owner_bytes);
    persist_write_array(partition_table,        part_bytes,   PERSIST_PART_ENT_LBA);
    persist_write_array(partition_assign_table, assign_bytes, PERSIST_PART_ASSIGN_LBA);
    persist_write_array(partition_owner_table,  owner_bytes,  PERSIST_PART_OWNER_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Partitions snapshot written.\n");
}

// ─── persist_rowstore_headers ─────────────────────────────────────────────────
// Phase 16 (relational layer). Writes table_headers[] plus the page-pool
// high-water mark (rowstore_next_free_page_id, stashed in the header
// frame's v1 slot — same "steal a uint32 slot in stage_hdr()" trick this
// file has no dedicated pattern for otherwise). Row PAGE data is NOT
// written here — see persist.h's comment on this function.
//
// Storage Isolation Roadmap Phase 3: also writes rowstore_partition_cursor[]
// (the real per-partition sub-range state) to its own new LBA region, and
// stashes PERSIST_STORAGE_ISOLATION_PHASE3_MARK in the header's v2 slot
// (previously always 0) so a restore can tell this snapshot includes it —
// see persist.h's LBA layout comment for the full reasoning.
void persist_rowstore_headers(void) {
    if (persist_defer_note(PERSIST_PEND_ROWSTORE)) return;
    if (!io_sq || !io_cq) return;
    uint32_t hdr_bytes = (uint32_t)sizeof(table_headers);
    stage_hdr(PERSIST_ROWSTORE_HDR_LBA, PERSIST_MAGIC_ROWSTORE,
              hdr_bytes, rowstore_next_free_page_id, PERSIST_STORAGE_ISOLATION_PHASE3_MARK);
    persist_write_array_diffed(table_headers, hdr_bytes, PERSIST_ROWSTORE_ENT_LBA,
                               p_shadow_rowstore, &p_shadow_rowstore_valid);
    persist_write_array(rowstore_partition_cursor, (uint32_t)sizeof(rowstore_partition_cursor),
                        PERSIST_ROWSTORE_PARTCURSOR_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Row-store table headers snapshot written.\n");
}

// ─── Gap Remediation Phase D ─────────────────────────────────────────────────

// persist_row_constraints — pure definitions, no derived runtime state (see
// persist.h's own comment). Called after every successful row_constraint_
// add_unique/_not_null/_range/_reference().
void persist_row_constraints(void) {
    if (persist_defer_note(PERSIST_PEND_ROWCONSTRAINT)) return;
    if (!io_sq || !io_cq) return;
    uint32_t bytes = (uint32_t)sizeof(row_constraints);
    stage_hdr(PERSIST_ROW_CONSTRAINT_HDR_LBA, PERSIST_MAGIC_ROW_CONSTRAINT,
              row_constraint_count, bytes, 0);
    persist_write_array(row_constraints, bytes, PERSIST_ROW_CONSTRAINT_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Row constraints snapshot written.\n");
}

// persist_row_index_defs — snapshots the live row_indexes[] array as-is
// (root_node/entry_count included), but persist_restore_all()'s own restore
// block only trusts index_name/table_object_id/column_index from it -- the
// B-tree itself is rebuilt fresh via row_index_create(), not replayed from
// stale node-pool indices (see row_index_create()'s own node pool, which is
// never persisted at all -- see PERSIST_ROW_INDEX_HDR_LBA's own comment in
// persist.h). Called after every successful row_index_create().
void persist_row_index_defs(void) {
    if (persist_defer_note(PERSIST_PEND_ROWINDEX)) return;
    if (!io_sq || !io_cq) return;
    uint32_t bytes = (uint32_t)sizeof(row_indexes);
    stage_hdr(PERSIST_ROW_INDEX_HDR_LBA, PERSIST_MAGIC_ROW_INDEX, bytes, 0, 0);
    persist_write_array(row_indexes, bytes, PERSIST_ROW_INDEX_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Row index definitions snapshot written.\n");
}

// persist_vecstore_headers — mirrors persist_rowstore_headers()'s exact
// shape (header + one small struct array; bulk page data is a separate,
// directly-managed NVMe region -- see VECSTORE_LBA_BASE, vecstore.h).
// Called after every successful vecstore_create_collection() / vecstore_
// insert() / vecstore_delete().
// Storage Isolation Roadmap Phase 3: also writes vecstore_partition_cursor[]
// and stashes the same format-version marker in the header's v2 slot --
// mirrors persist_rowstore_headers()'s own Phase 3 addition exactly.
void persist_vecstore_headers(void) {
    if (persist_defer_note(PERSIST_PEND_VECSTORE)) return;
    if (!io_sq || !io_cq) return;
    uint32_t hdr_bytes = (uint32_t)sizeof(vector_collections);
    stage_hdr(PERSIST_VECSTORE_HDR_LBA, PERSIST_MAGIC_VECSTORE,
              hdr_bytes, vecstore_next_free_page_id, PERSIST_STORAGE_ISOLATION_PHASE3_MARK);
    persist_write_array(vector_collections, hdr_bytes, PERSIST_VECSTORE_ENT_LBA);
    persist_write_array(vecstore_partition_cursor, (uint32_t)sizeof(vecstore_partition_cursor),
                        PERSIST_VECSTORE_PARTCURSOR_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Vecstore collection headers snapshot written.\n");
}

// persist_vec_index_defs — same "snapshot as-is, restore trusts only the
// definitional fields" relationship persist_row_index_defs() has with
// row_index_create() above, mirrored here for vec_index_create(). Called
// after every successful vec_index_create().
void persist_vec_index_defs(void) {
    if (persist_defer_note(PERSIST_PEND_VECINDEX)) return;
    if (!io_sq || !io_cq) return;
    uint32_t bytes = (uint32_t)sizeof(vec_indexes);
    stage_hdr(PERSIST_VEC_INDEX_HDR_LBA, PERSIST_MAGIC_VEC_INDEX, bytes, 0, 0);
    persist_write_array(vec_indexes, bytes, PERSIST_VEC_INDEX_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Vec index definitions snapshot written.\n");
}

// persist_row_journal — direct, full-array-rewrite-per-mutation persistence
// (matching persist_records()'s own established trade-off for a small-
// enough struct array), NOT rebuild-on-boot: an audit trail is inherently
// historical, no current-state scan can regenerate it. Called after every
// row_journal_notify_insert/update/delete() and row_journal_commit_tx()/
// _rollback_tx().
void persist_row_journal(void) {
    if (persist_defer_note(PERSIST_PEND_ROWJOURNAL)) return;
    if (!io_sq || !io_cq) return;
    uint32_t buf_bytes    = (uint32_t)sizeof(row_journal_buffer);
    uint32_t attach_bytes = (uint32_t)sizeof(row_journal_attachments);
    stage_hdr(PERSIST_ROW_JOURNAL_HDR_LBA, PERSIST_MAGIC_ROW_JOURNAL,
              buf_bytes, row_journal_entry_count, attach_bytes);
    persist_write_array_diffed(row_journal_buffer, buf_bytes, PERSIST_ROW_JOURNAL_ENT_LBA,
                               p_shadow_rowjournal, &p_shadow_rowjournal_valid);
    persist_write_array(row_journal_attachments, attach_bytes, PERSIST_ROW_JOURNAL_ATTACH_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Row journal snapshot written.\n");
}

// ─── Database Gap Analysis §1 ───────────────────────────────────────────────

// persist_databases — databases[] + database_grants[] + database_next_id.
// Pure definitions, direct restore. database_next_id rides in the header's
// third field (the same header-carried-scalar trick persist_row_
// constraints() uses for row_constraint_count) -- restoring it is the
// load-bearing half of this function: without it a fresh boot's bump
// allocator re-issues ids that stale persisted database_id tags (block 1's
// catalog restore keeps those alive) still reference -- the silent-
// reattachment failure the Namespace roadmap's §1.2 never-reuse design
// exists to prevent, previously defeated by this exact persistence hole.
void persist_databases(void) {
    if (persist_defer_note(PERSIST_PEND_DATABASE)) return;
    if (!io_sq || !io_cq) return;
    uint32_t db_bytes    = (uint32_t)sizeof(databases);
    uint32_t grant_bytes = (uint32_t)sizeof(database_grants);
    stage_hdr(PERSIST_DATABASE_HDR_LBA, PERSIST_MAGIC_DATABASE,
              db_bytes, grant_bytes, database_next_id);
    persist_write_array(databases,       db_bytes,    PERSIST_DATABASE_ENT_LBA);
    persist_write_array(database_grants, grant_bytes, PERSIST_DATABASE_GRANT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Databases snapshot written.\n");
}

// persist_views — Query-Surface Roadmap Phase 5. views[] is pure
// definitions with no derived state at all (no bump-allocated id like
// databases[]'s database_next_id, no rebuild-on-boot step like row_index/
// vec_index) -- the header carries just the array's own byte size.
void persist_views(void) {
    if (persist_defer_note(PERSIST_PEND_VIEW)) return;
    if (!io_sq || !io_cq) return;
    uint32_t view_bytes = (uint32_t)sizeof(views);
    stage_hdr(PERSIST_VIEW_HDR_LBA, PERSIST_MAGIC_VIEW, view_bytes, 0, 0);
    persist_write_array(views, view_bytes, PERSIST_VIEW_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Views snapshot written.\n");
}

// ─── Multitenant Isolation Gap Analysis §5 item 1 ───────────────────────────

// persist_tenants — tenants[] + tenant_next_id. Pure definitions, direct
// restore, same shape as persist_databases() -- tenant_next_id rides in
// the header's third field for the identical reason database_next_id
// does there (see that function's own comment): a fresh boot's bump
// allocator must not re-issue an id a stale persisted tenant_id
// reference still holds.
void persist_tenants(void) {
    if (persist_defer_note(PERSIST_PEND_TENANT)) return;
    if (!io_sq || !io_cq) return;
    uint32_t tenant_bytes = (uint32_t)sizeof(tenants);
    stage_hdr(PERSIST_TENANT_HDR_LBA, PERSIST_MAGIC_TENANT,
              tenant_bytes, 0, tenant_next_id);
    persist_write_array(tenants, tenant_bytes, PERSIST_TENANT_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Tenants snapshot written.\n");
}

// persist_services — services_registry[] (Orchestration Plan Phase 4).
// Pure definitions, direct restore, same shape as persist_tenants(). No
// header-carried scalar: registrations are keyed by NAME, not by a bump-
// allocated id, so there is no next_id whose reuse would alias a stale
// reference -- the reason persist_tenants()/persist_databases() need that
// third header field does not arise here.
void persist_services(void) {
    if (persist_defer_note(PERSIST_PEND_SERVICE)) return;
    if (!io_sq || !io_cq) return;
    uint32_t svc_bytes = (uint32_t)sizeof(services_registry);
    stage_hdr(PERSIST_SERVICE_HDR_LBA, PERSIST_MAGIC_SERVICE, svc_bytes, 0, 0);
    persist_write_array(services_registry, svc_bytes, PERSIST_SERVICE_ENT_LBA);
    persist_region_commit();
    kernel_serial_print("[PERSIST] Service registry snapshot written.\n");
}

// ─── persist_restore_all ─────────────────────────────────────────────────────
// Called once at boot (kernel.c step 7b), before stream_init().
// Each region is independently checked: a missing or mismatched magic causes
// a cold start for that subsystem while others may still restore successfully.
// Struct-size validation catches format changes between kernel builds.
void persist_restore_all(void) {
    // Fresh scan per call: the trusted mask describes the CURRENT on-disk image,
    // and a caller may legitimately restore more than once (host tests do).
    p_scan_done = 0;
    persist_scan_regions();
    if (!io_sq || !io_cq) return;

    // ── 1. Catalog + role table ───────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_CAT_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_CAT && persist_region_trusted(PERSIST_MAGIC_CAT)) {
            uint32_t cat_count, cat_bytes, role_bytes;
            p_memcpy(&cat_count,  p_buf +  8, 4);
            p_memcpy(&cat_bytes,  p_buf + 12, 4);
            p_memcpy(&role_bytes, p_buf + 16, 4);
            if (cat_bytes  == (uint32_t)sizeof(object_catalog) &&
                role_bytes == (uint32_t)sizeof(role_table)) {
                persist_read_array(object_catalog, cat_bytes,  PERSIST_CAT_ENT_LBA);
                persist_read_array(role_table,     role_bytes, PERSIST_ROLE_ENT_LBA);
                object_catalog_count = cat_count;
                catalog_after_restore();
                kernel_serial_printf("[PERSIST] Catalog restored: %u entries.\n",
                                     cat_count);
            } else {
                kernel_serial_print("[PERSIST] Catalog: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Catalog: no snapshot — cold start.\n");
        }
    }

    // ── 2. Records ───────────────────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_REC_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_REC && persist_region_trusted(PERSIST_MAGIC_REC)) {
            uint32_t rec_bytes;
            p_memcpy(&rec_bytes, p_buf + 8, 4);
            if (rec_bytes == (uint32_t)sizeof(object_records)) {
                persist_read_array(object_records, rec_bytes, PERSIST_REC_ENT_LBA);
                // Seed the shadow: at this exact point the on-disk image and
                // memory are identical by construction (we just read one into
                // the other), which is precisely the invariant the shadow
                // encodes. Doing this lets the first post-boot write diff
                // properly instead of rewriting all 322 frames. Deliberately
                // NOT done on the size-mismatch/cold-start branches below --
                // there the shadow stays invalid, so the first write is a
                // full one, which is the correct conservative behaviour when
                // the on-disk contents are unknown or from another build.
                p_memcpy(p_shadow_records, object_records, rec_bytes);
                p_shadow_records_valid = 1;
                kernel_serial_print("[PERSIST] Records restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Records: struct size mismatch — cold start.\n");
            }
        }
    }

    // ── 3. Schemas ───────────────────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_SCH_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_SCH && persist_region_trusted(PERSIST_MAGIC_SCH)) {
            uint32_t sch_bytes;
            p_memcpy(&sch_bytes, p_buf + 8, 4);
            if (sch_bytes == (uint32_t)sizeof(object_schemas)) {
                persist_read_array(object_schemas, sch_bytes, PERSIST_SCH_ENT_LBA);
                kernel_serial_print("[PERSIST] Schemas restored from NVMe.\n");
            } else {
                kernel_serial_print("[PERSIST] Schemas: struct size mismatch — cold start.\n");
            }
        }
    }

    // ── 4. Program binaries ──────────────────────────────────────────────────
    if (nvme_read_sync(PERSIST_PROG_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_PROG && persist_region_trusted(PERSIST_MAGIC_PROG)) {
            uint32_t prog_bytes;
            p_memcpy(&prog_bytes, p_buf + 8, 4);
            if (prog_bytes == (uint32_t)sizeof(service_binaries)) {
                persist_read_array(service_binaries, prog_bytes, PERSIST_PROG_DAT_LBA);
                int cnt = 0;
                for (int i = 0; i < MAX_SERVICE_BINARIES; i++)
                    if (service_binaries[i].active) cnt++;
                kernel_serial_printf("[PERSIST] Programs restored: %d binaries.\n", cnt);
            } else {
                kernel_serial_print("[PERSIST] Programs: struct size mismatch — cold start.\n");
            }
        }
    }

    // ── 5. Partitions (Phase 10) ─────────────────────────────────────────────
    // Runs after partition_init() (kernel.c step 4c-bis) has already set up
    // the default single-partition state — a valid snapshot here simply
    // overwrites those defaults, same relationship persist_catalog()'s
    // restore has with object_catalog[]'s BSS-zeroed starting state.
    if (nvme_read_sync(PERSIST_PART_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_PART && persist_region_trusted(PERSIST_MAGIC_PART)) {
            uint32_t part_bytes, assign_bytes, owner_bytes;
            p_memcpy(&part_bytes,   p_buf +  8, 4);
            p_memcpy(&assign_bytes, p_buf + 12, 4);
            p_memcpy(&owner_bytes,  p_buf + 16, 4);   // Phase 2 (Multi-Node) -- 0 on any snapshot written before this phase
            if (part_bytes   == (uint32_t)sizeof(partition_table) &&
                assign_bytes == (uint32_t)sizeof(partition_assign_table)) {
                persist_read_array(partition_table,        part_bytes,   PERSIST_PART_ENT_LBA);
                persist_read_array(partition_assign_table, assign_bytes, PERSIST_PART_ASSIGN_LBA);
                kernel_serial_print("[PERSIST] Partitions restored from NVMe.\n");

                // Phase 2 (Multi-Node Partition Scaling Roadmap): ownership
                // rows are only restorable from a snapshot that was itself
                // written by Phase-2-or-later code (owner_bytes matches the
                // current struct's real size). A snapshot written before
                // this phase has owner_bytes==0 (stage_hdr()'s v2 slot was
                // always passed 0 previously) -- there's no valid data at
                // PERSIST_PART_OWNER_LBA to read in that case, so the
                // owner table is deliberately left at whatever
                // partition_init() already set for it (every partition
                // just-restored above then reads as owned by whichever
                // node this boot is, via partition_get_owner_node()'s own
                // "no row -> 0" fallback) rather than reading garbage.
                if (owner_bytes == (uint32_t)sizeof(partition_owner_table)) {
                    persist_read_array(partition_owner_table, owner_bytes, PERSIST_PART_OWNER_LBA);
                    kernel_serial_print("[PERSIST] Partition ownership restored from NVMe.\n");
                } else {
                    kernel_serial_print(
                        "[PERSIST] Partition ownership: snapshot predates Phase 2 (or size "
                        "mismatch) -- ownership left at boot defaults.\n");
                }
            } else {
                kernel_serial_print("[PERSIST] Partitions: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Partitions: no snapshot — cold start.\n");
        }
    }

    // ── 6. Row-store table headers (Phase 16) ────────────────────────────────
    // Runs after rowstore_init() (kernel.c, alongside mqt_init()/agent_init())
    // has already zeroed table_headers[] and reset the page-pool cursor — a
    // valid snapshot here overwrites those cold-start defaults, same
    // relationship every other restore block has with its subsystem's
    // BSS-zeroed/explicitly-reset starting state. Row PAGE data itself is
    // NOT restored here — pages restore lazily, one at a time, on first
    // access via rowstore_load_page(), exactly like stream.c's frames[].
    if (nvme_read_sync(PERSIST_ROWSTORE_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROWSTORE && persist_region_trusted(PERSIST_MAGIC_ROWSTORE)) {
            uint32_t hdr_bytes, next_page, part_mark;
            p_memcpy(&hdr_bytes, p_buf +  8, 4);
            p_memcpy(&next_page, p_buf + 12, 4);
            p_memcpy(&part_mark, p_buf + 16, 4);
            if (hdr_bytes == (uint32_t)sizeof(table_headers)) {
                persist_read_array(table_headers, hdr_bytes, PERSIST_ROWSTORE_ENT_LBA);
                rowstore_next_free_page_id = next_page;
                // Storage Isolation Roadmap Phase 3: only trust the per-
                // partition cursor array on a snapshot actually written by
                // Phase-3-aware code -- an older snapshot's page ownership
                // predates sub-ranges entirely and cannot be reattributed
                // (one-way format change, see persist.h's LBA layout
                // comment). rowstore_init() has already left every
                // partition's cursor at its own cold-start sub-range start,
                // which is the correct, safe fallback here.
                if (part_mark == PERSIST_STORAGE_ISOLATION_PHASE3_MARK) {
                    persist_read_array(rowstore_partition_cursor, (uint32_t)sizeof(rowstore_partition_cursor),
                                       PERSIST_ROWSTORE_PARTCURSOR_LBA);
                    kernel_serial_print("[PERSIST] Row-store table headers + per-partition page cursors restored from NVMe.\n");
                } else {
                    kernel_serial_print(
                        "[PERSIST] Row-store table headers restored from NVMe (pre-Phase-3 snapshot -- "
                        "per-partition page cursors reset to cold-start sub-range starts).\n");
                }
            } else {
                kernel_serial_print("[PERSIST] Row-store: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row-store: no snapshot — cold start.\n");
        }
    }

    // ── 6b. MVCC bootstrap (Gap Remediation Phase D) ─────────────────────────
    // Not a restore block itself (mvcc.c persists nothing -- see mvcc.h's own
    // "persistence out of scope" stance, still true for the mechanism, just
    // no longer true for the CONSEQUENCE of skipping it -- see mvcc_
    // bootstrap_from_rowstore()'s own header comment for the full story).
    // Registers one synthetic, always-visible MvccVersion per physical row
    // block 6 just restored, unconditionally (not gated by whether a
    // catalog/rowstore snapshot actually existed -- calling this against an
    // empty table_headers[]/object_catalog[] on a genuine cold start is a
    // correct no-op, not a special case to guard against). Must run after
    // block 6 (needs restored row data) and before any real transaction
    // begins -- every block below this point that touches MVCC-visible data
    // (row constraints' runtime checks, if exercised) depends on it.
    mvcc_bootstrap_from_rowstore();

    // ── 7. Row constraints (Gap Remediation Phase D) ────────────────────────
    // Pure definitions, no derived runtime state (see row_constraint.h's own
    // header comment: enforcement always consults mvcc_table_scan() live,
    // nothing is cached) -- direct restore, same shape as every simple
    // struct-array block above.
    if (nvme_read_sync(PERSIST_ROW_CONSTRAINT_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROW_CONSTRAINT && persist_region_trusted(PERSIST_MAGIC_ROW_CONSTRAINT)) {
            uint32_t count, bytes;
            p_memcpy(&count, p_buf +  8, 4);
            p_memcpy(&bytes, p_buf + 12, 4);
            if (bytes == (uint32_t)sizeof(row_constraints)) {
                persist_read_array(row_constraints, bytes, PERSIST_ROW_CONSTRAINT_ENT_LBA);
                row_constraint_count = count;
                kernel_serial_printf("[PERSIST] Row constraints restored: %u defined.\n", count);
            } else {
                kernel_serial_print("[PERSIST] Row constraints: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row constraints: no snapshot — cold start.\n");
        }
    }

    // ── 8. Row-set B-tree indexes (Gap Remediation Phase D): rebuild-on-
    // boot ────────────────────────────────────────────────────────────────
    // Restored definitions are staged in a local buffer, then fed one at a
    // time through the real row_index_create() -- which re-derives the
    // actual B-tree by scanning already-restored row data (blocks 1 and 6
    // above, both of which run earlier in this function) -- rather than
    // being loaded directly into the live row_indexes[]/node pool (whose
    // node-pool indices were never persisted at all and would be meaningless
    // after a fresh boot's node pool starts empty again). caller_uid=0 here
    // uses the existing, already-established "kernel role always passes
    // catalog_check_access()" convention (object_catalog.c's catalog_get_
    // role()) -- not a new convention invented for this restore path.
    if (nvme_read_sync(PERSIST_ROW_INDEX_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROW_INDEX && persist_region_trusted(PERSIST_MAGIC_ROW_INDEX)) {
            uint32_t bytes;
            p_memcpy(&bytes, p_buf + 8, 4);
            if (bytes == (uint32_t)sizeof(row_indexes)) {
                struct RowIndex staged[ROW_INDEX_MAX];
                persist_read_array(staged, bytes, PERSIST_ROW_INDEX_ENT_LBA);
                uint32_t defined = 0, rebuilt = 0;
                for (uint32_t i = 0; i < ROW_INDEX_MAX; i++) {
                    if (!staged[i].active) continue;
                    defined++;
                    int tidx = -1;
                    for (uint32_t c = 0; c < object_catalog_count; c++) {
                        if (object_catalog[c].active &&
                            object_catalog[c].object_id == staged[i].table_object_id) { tidx = (int)c; break; }
                    }
                    if (tidx < 0) continue;   // parent table no longer exists -- can't rebuild, skip
                    if (staged[i].column_index >= table_headers[tidx].layout.column_count) continue;
                    const char* col_name = table_headers[tidx].layout.column_names[staged[i].column_index];
                    if (row_index_create(0, staged[i].index_name, object_catalog[tidx].name, col_name) == 0)
                        rebuilt++;
                }
                kernel_serial_printf("[PERSIST] Row indexes rebuilt from NVMe-persisted definitions: %u of %u.\n",
                                     rebuilt, defined);
            } else {
                kernel_serial_print("[PERSIST] Row indexes: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row indexes: no snapshot — cold start.\n");
        }
    }

    // ── 9. Vecstore collection headers (Gap Remediation Phase D) ────────────
    // Mirrors block 6's rowstore restore exactly. Bulk page data is NOT
    // restored here -- it restores lazily, one page at a time, on first
    // access via vecstore_load_page(), exactly like rowstore's own row pages.
    if (nvme_read_sync(PERSIST_VECSTORE_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_VECSTORE && persist_region_trusted(PERSIST_MAGIC_VECSTORE)) {
            uint32_t hdr_bytes, next_page, part_mark;
            p_memcpy(&hdr_bytes, p_buf +  8, 4);
            p_memcpy(&next_page, p_buf + 12, 4);
            p_memcpy(&part_mark, p_buf + 16, 4);
            if (hdr_bytes == (uint32_t)sizeof(vector_collections)) {
                persist_read_array(vector_collections, hdr_bytes, PERSIST_VECSTORE_ENT_LBA);
                vecstore_next_free_page_id = next_page;
                // Storage Isolation Roadmap Phase 3: same format-version
                // gate as the rowstore restore block above -- see its
                // comment for the full reasoning.
                if (part_mark == PERSIST_STORAGE_ISOLATION_PHASE3_MARK) {
                    persist_read_array(vecstore_partition_cursor, (uint32_t)sizeof(vecstore_partition_cursor),
                                       PERSIST_VECSTORE_PARTCURSOR_LBA);
                    kernel_serial_print("[PERSIST] Vecstore collection headers + per-partition page cursors restored from NVMe.\n");
                } else {
                    kernel_serial_print(
                        "[PERSIST] Vecstore collection headers restored from NVMe (pre-Phase-3 snapshot -- "
                        "per-partition page cursors reset to cold-start sub-range starts).\n");
                }
            } else {
                kernel_serial_print("[PERSIST] Vecstore: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Vecstore: no snapshot — cold start.\n");
        }
    }

    // ── 10. HNSW vector indexes (Gap Remediation Phase D): rebuild-on-boot,
    // same shape as block 8 ──────────────────────────────────────────────────
    // Restored definitions feed vec_index_create(), then a one-time backfill
    // scan over the now-restored collection (vecstore_collection_scan(),
    // which lazily loads whatever pages it visits via block 9's restored
    // vecstore_next_free_page_id) rebuilds the actual graph by feeding every
    // entry through vec_index_notify_insert() -- the exact backfill helper
    // vec_index.h's own header comment (point 7) named as "straightforward
    // to add later ... if a real caller needs it." This restore path is that
    // real caller; see persist_vec_backfill_cb() above.
    if (nvme_read_sync(PERSIST_VEC_INDEX_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_VEC_INDEX && persist_region_trusted(PERSIST_MAGIC_VEC_INDEX)) {
            uint32_t bytes;
            p_memcpy(&bytes, p_buf + 8, 4);
            if (bytes == (uint32_t)sizeof(vec_indexes)) {
                struct VecIndex staged[VEC_INDEX_MAX];
                persist_read_array(staged, bytes, PERSIST_VEC_INDEX_ENT_LBA);
                uint32_t defined = 0, rebuilt = 0;
                for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
                    if (!staged[i].active) continue;
                    defined++;
                    if (vec_index_create(0, staged[i].index_name, staged[i].collection_name, staged[i].metric) != 0)
                        continue;
                    struct persist_vec_backfill_ctx ctx = { staged[i].collection_name };
                    vecstore_collection_scan(0, staged[i].collection_name, persist_vec_backfill_cb, &ctx);
                    rebuilt++;
                }
                kernel_serial_printf("[PERSIST] HNSW vector indexes rebuilt from NVMe-persisted definitions: %u of %u.\n",
                                     rebuilt, defined);
            } else {
                kernel_serial_print("[PERSIST] Vec indexes: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Vec indexes: no snapshot — cold start.\n");
        }
    }

    // ── 11. Row journal (Gap Remediation Phase D) ───────────────────────────
    // Direct restore -- an audit trail is inherently historical, not a
    // derived structure; see persist_row_journal()'s own header comment.
    if (nvme_read_sync(PERSIST_ROW_JOURNAL_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_ROW_JOURNAL && persist_region_trusted(PERSIST_MAGIC_ROW_JOURNAL)) {
            uint32_t buf_bytes, entry_count, attach_bytes;
            p_memcpy(&buf_bytes,    p_buf +  8, 4);
            p_memcpy(&entry_count,  p_buf + 12, 4);
            p_memcpy(&attach_bytes, p_buf + 16, 4);
            if (buf_bytes    == (uint32_t)sizeof(row_journal_buffer) &&
                attach_bytes == (uint32_t)sizeof(row_journal_attachments)) {
                persist_read_array(row_journal_buffer, buf_bytes, PERSIST_ROW_JOURNAL_ENT_LBA);
                persist_read_array(row_journal_attachments, attach_bytes, PERSIST_ROW_JOURNAL_ATTACH_LBA);
                row_journal_entry_count = entry_count;
                uint32_t acount = 0;
                for (uint32_t i = 0; i < ROW_JOURNAL_MAX_ATTACHMENTS; i++)
                    if (row_journal_attachments[i].active) acount++;
                row_journal_attachment_count = acount;
                kernel_serial_printf("[PERSIST] Row journal restored: %u entries (seq), %u attachment(s).\n",
                                     entry_count, acount);
            } else {
                kernel_serial_print("[PERSIST] Row journal: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Row journal: no snapshot — cold start.\n");
        }
    }

    // ── 12. Databases (Database Gap Analysis §1) ────────────────────────────
    // Direct restore -- pure definitions, no derived state. Restoring
    // database_next_id from the header is the load-bearing part (see
    // persist_databases()'s own comment). database_grant_count is a
    // high-water mark recomputed from the restored array's active flags,
    // the same derived-not-stored treatment row_journal_attachment_count
    // gets in block 11 above. On a size-mismatched or absent snapshot this
    // cold-starts with database_next_id at its compile-time initial value
    // (1) -- exactly the pre-Gap-1 behavior, no worse, and the mismatch is
    // logged rather than silent.
    if (nvme_read_sync(PERSIST_DATABASE_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_DATABASE && persist_region_trusted(PERSIST_MAGIC_DATABASE)) {
            uint32_t db_bytes, grant_bytes, next_id;
            p_memcpy(&db_bytes,    p_buf +  8, 4);
            p_memcpy(&grant_bytes, p_buf + 12, 4);
            p_memcpy(&next_id,     p_buf + 16, 4);
            if (db_bytes    == (uint32_t)sizeof(databases) &&
                grant_bytes == (uint32_t)sizeof(database_grants)) {
                persist_read_array(databases,       db_bytes,    PERSIST_DATABASE_ENT_LBA);
                persist_read_array(database_grants, grant_bytes, PERSIST_DATABASE_GRANT_LBA);
                database_next_id = next_id;
                uint32_t gcount = 0, dcount = 0;
                for (uint32_t i = 0; i < DATABASE_GRANT_MAX; i++)
                    if (database_grants[i].active && (i + 1) > gcount) gcount = i + 1;
                database_grant_count = gcount;
                for (uint32_t i = 0; i < DATABASE_MAX; i++)
                    if (databases[i].active) dcount++;
                kernel_serial_printf("[PERSIST] Databases restored: %u defined, next_id=%u.\n",
                                     dcount, next_id);
            } else {
                kernel_serial_print("[PERSIST] Databases: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Databases: no snapshot — cold start.\n");
        }
    }

    // ── 13. Views (Query-Surface Roadmap Phase 5) ───────────────────────────
    // Direct restore -- pure definitions, no derived state at all (unlike
    // block 12's database_next_id/database_grant_count, there's nothing
    // else to recompute here). On a size-mismatched or absent snapshot this
    // cold-starts with an empty views[] -- logged rather than silent.
    if (nvme_read_sync(PERSIST_VIEW_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_VIEW && persist_region_trusted(PERSIST_MAGIC_VIEW)) {
            uint32_t view_bytes;
            p_memcpy(&view_bytes, p_buf + 8, 4);
            if (view_bytes == (uint32_t)sizeof(views)) {
                persist_read_array(views, view_bytes, PERSIST_VIEW_ENT_LBA);
                uint32_t vcount = 0;
                for (uint32_t i = 0; i < VIEW_MAX; i++)
                    if (views[i].active) vcount++;
                kernel_serial_printf("[PERSIST] Views restored: %u defined.\n", vcount);
            } else {
                kernel_serial_print("[PERSIST] Views: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Views: no snapshot — cold start.\n");
        }
    }

    // ── 14. Tenants (Multitenant Isolation Gap Analysis §5 item 1) ─────────
    // Direct restore -- pure definitions, no derived state beyond
    // tenant_next_id itself (the load-bearing part, same reason as block
    // 12's database_next_id: a fresh boot's bump allocator must not
    // re-issue an id a stale persisted tenant_id reference still holds).
    // On a size-mismatched or absent snapshot this cold-starts with an
    // empty tenants[] and tenant_next_id at its compile-time initial
    // value (1) -- logged rather than silent.
    if (nvme_read_sync(PERSIST_TENANT_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_TENANT && persist_region_trusted(PERSIST_MAGIC_TENANT)) {
            uint32_t tenant_bytes, next_id;
            p_memcpy(&tenant_bytes, p_buf +  8, 4);
            p_memcpy(&next_id,      p_buf + 16, 4);
            if (tenant_bytes == (uint32_t)sizeof(tenants)) {
                persist_read_array(tenants, tenant_bytes, PERSIST_TENANT_ENT_LBA);
                tenant_next_id = next_id;
                uint32_t tcount = 0;
                for (uint32_t i = 0; i < TENANT_MAX; i++)
                    if (tenants[i].active) tcount++;
                kernel_serial_printf("[PERSIST] Tenants restored: %u defined, next_id=%u.\n",
                                     tcount, next_id);
            } else {
                kernel_serial_print("[PERSIST] Tenants: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Tenants: no snapshot — cold start.\n");
        }
    }

    // ── 15. Service registry (Orchestration Plan Phase 4) ──────────────────
    // Direct restore -- pure definitions, and notably NO derived state to
    // rebuild: a registration stores name -> partition, and the node id is
    // recomputed from partition_owner_table[] on every resolve (see
    // service_registry.h). So a restored registry is correct the instant it
    // loads, even if the partition it points at moved to a different node
    // while this machine was down -- there is no stale cached location to
    // invalidate, because none was ever stored.
    if (nvme_read_sync(PERSIST_SERVICE_HDR_LBA, p_buf) == 0) {
        uint64_t magic = 0;
        p_memcpy(&magic, p_buf, 8);
        if (magic == PERSIST_MAGIC_SERVICE && persist_region_trusted(PERSIST_MAGIC_SERVICE)) {
            uint32_t svc_bytes;
            p_memcpy(&svc_bytes, p_buf + 8, 4);
            if (svc_bytes == (uint32_t)sizeof(services_registry)) {
                persist_read_array(services_registry, svc_bytes, PERSIST_SERVICE_ENT_LBA);
                uint32_t scount = 0;
                for (uint32_t i = 0; i < SERVICE_MAX; i++)
                    if (services_registry[i].active) scount++;
                kernel_serial_printf("[PERSIST] Service registry restored: %u registration(s).\n",
                                     scount);
            } else {
                kernel_serial_print("[PERSIST] Service registry: struct size mismatch — cold start.\n");
            }
        } else {
            kernel_serial_print("[PERSIST] Service registry: no snapshot — cold start.\n");
        }
    }
}