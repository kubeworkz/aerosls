/*
 * qemu_sls_tcache.h — Phase 2: persistent translation cache for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 2.
 *
 * NVMe layout (below STREAM_DIR_LBA 8192; last persist.h region ends ~7640):
 *
 *   LBA 10000  QEMU_TCACHE_HDR_LBA        1 frame — header (magic, tb_count, code_used)
 *   LBA 10008  QEMU_TCACHE_GEN_DAT_LBA   64 frames — page_gen[65536]  (256 KiB)
 *   LBA 10520  QEMU_TCACHE_TB_DAT_LBA    32 frames — tb_table[4096]   (128 KiB)
 *   LBA 10776  QEMU_TCACHE_CODE_DAT_LBA 1024 frames — codebuf[4 MiB]
 *              (end LBA 18968 — clear of STREAM_DATA_LBA_BASE 65536)
 */
#ifndef QEMU_SLS_TCACHE_H
#define QEMU_SLS_TCACHE_H

#include <stdint.h>

#define QEMU_TCACHE_HDR_LBA      10000ULL
#define QEMU_TCACHE_GEN_DAT_LBA  10008ULL
#define QEMU_TCACHE_TB_DAT_LBA   10520ULL
#define QEMU_TCACHE_CODE_DAT_LBA 10776ULL
/* Page-digest table. codebuf ends at 18968; QEMU_VM_STATE_LBA is 20000.
 * 4096 entries x 16 bytes = 64 KiB = 16 frames = 128 sectors: 19000..19128. */
#define QEMU_TCACHE_PDIG_DAT_LBA 19000ULL

#define QEMU_TCACHE_MAGIC        0xCAFE000000000020ULL


/* Matches QEMU_GUEST_RAM_PAGES — one gen counter per guest 4 KiB page. */
/* On-NVMe format version; folded into the identity stamp. v2 added the
 * page-digest table. Bump on any layout change. */
#define QEMU_TCACHE_FORMAT_VERSION 2U

/* One digest slot per TB slot: 4096 TBs can occupy at most 4096 distinct
 * guest pages, so the table can never be the thing that overflows first. */
#define QEMU_TCACHE_PDIG_ENTRIES 4096U

#define QEMU_TCACHE_GEN_PAGES    65536U

/* Open-addressing hash table size (power of 2). */
#define QEMU_TCACHE_MAX_TBS      4096U

/* Code-gen buffer; static .bss ensures a deterministic VA across reboots. */
#define QEMU_TCACHE_CODEBUF_SIZE (4U * 1024U * 1024U)
#define QEMU_TCACHE_CODEBUF_PAGES (QEMU_TCACHE_CODEBUF_SIZE / 4096U)

/* ─── Build identity: the magic is not enough ──────────────────────────────
 *
 * The magic proves the region was written by SOME build of this code. It does
 * not prove it was written by THIS one -- and what is stored here is HOST
 * MACHINE CODE. Restoring a cache produced by a different compiler, a
 * different TCG revision, or different code-generation flags hands the CPU
 * instructions built against assumptions that no longer hold. There is no
 * fault to catch it. It simply executes.
 *
 * That is the single most expensive failure shape this project has met: a
 * wrong state indistinguishable from a right one. Every other guard in the
 * QEMU-SLS layer exists to make some version of it loud, so this one is
 * checked before a single cached byte is ever executed.
 *
 * AEROSLS_BUILD_ID comes from the Makefile (git hash, or a timestamp when git
 * is unavailable). It is deliberately CONSERVATIVE: any rebuild invalidates
 * the cache, even one that could not have changed code generation. A needless
 * cold start costs one round of recompilation; a wrongly-accepted cache costs
 * arbitrary behaviour with no diagnostic.
 *
 * Header layout at QEMU_TCACHE_HDR_LBA:
 *   +0   u64  magic
 *   +8   u32  tb_count
 *   +12  u32  codebuf_used
 *   +16  u64  identity   (this)
 *   +24  u32  codebuf_size
 *   +28  u32  max_tbs
 */
#ifndef AEROSLS_BUILD_ID
#define AEROSLS_BUILD_ID "unknown-build"
#endif

/* FNV-1a over the build id, mixed with the structural constants that decide
 * whether cached code can be interpreted at all. Sizes are included because a
 * cache written when the table or buffer was a different size cannot be read
 * back correctly even from an identical compiler. */
/* Parameterised so the build id can be varied by a test.
 *
 * The reason is specific: with the id fixed at compile time, "the identity
 * responds to its input" is not observable, and mutation testing proved it --
 * an implementation that simply RETURNED the expected constant passed every
 * check, including determinism. A guard that cannot be shown to depend on what
 * it guards is not a guard. Taking the id as an argument makes the property
 * testable in the only way that means anything: call it twice with different
 * ids and require different answers. */
static inline uint64_t qemu_tcache_identity_of(const char *build_id) {
    uint64_t h = 1469598103934665603ULL;               /* FNV-1a offset basis */
    for (const char *p = build_id; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)QEMU_TCACHE_CODEBUF_SIZE << 1;
    h *= 1099511628211ULL;
    h ^= (uint64_t)QEMU_TCACHE_MAX_TBS << 3;
    h *= 1099511628211ULL;
    h ^= (uint64_t)sizeof(void *);                     /* host pointer width */
    h *= 1099511628211ULL;
    /* On-NVMe format version. Bumped when a region is added or its layout
     * changes -- the page-digest table at QEMU_TCACHE_PDIG_DAT_LBA was added
     * at v2. An older snapshot has no digest table, so those LBAs hold
     * whatever was there before; reading them as digests could produce a match
     * against uninitialised disk and skip a flush that was needed. Discarding
     * on the version alone is one cold start and removes the question. */
    h ^= (uint64_t)QEMU_TCACHE_FORMAT_VERSION << 5;
    h *= 1099511628211ULL;
    /* ─── softmmu side, because it changes every emitted load ──────────────
     * SLS_FORCE_SOFTMMU (see ../qemu/tcg/tcg-internal.h) flips guest memory
     * accesses between an inlined TLB lookup and a bare MOV -- 86 bytes of
     * host code per load against 16. The two builds emit different machine
     * code for the same guest instruction.
     *
     * Without this term they hash identically, because AEROSLS_BUILD_ID is
     * the git commit and the A/B is a flag change at one commit. A cache
     * written by the OFF build would then be accepted by the ON build and
     * jumped into: host machine code executed against the wrong memory-access
     * contract, with no fault to catch it. That is precisely the outcome the
     * identity stamp exists to make impossible, and the A/B knob is the one
     * change most likely to produce it.
     *
     * Adding this term changes the hash for every build once, discarding
     * existing caches on the next boot. One cold start, correctly taken.
     *
     * The two tags are far apart, and the multiply after them is not
     * decoration. Written first as constants differing in the low bit with no
     * multiply, the two sides landed ONE bit apart; adding the multiply took
     * that to 8, still poor. Two high-entropy tags differing in half their
     * bits, then mixed, put the sides 26 bits apart. tests/
     * tcache_identity_host_test.c argues exactly this point about build ids --
     * "a hash that changed a single bit would still differ, and would still be
     * a bad guard" -- and the same standard applies here. */
#ifdef SLS_FORCE_SOFTMMU
    h ^= 0x9E3779B97F4A7C15ULL;                        /* softmmu = ON  */
#else
    h ^= 0xC2B2AE3D27D4EB4FULL;                        /* softmmu = OFF */
#endif
    h *= 1099511628211ULL;
    return h;
}

static inline uint64_t qemu_tcache_identity(void) {
    return qemu_tcache_identity_of(AEROSLS_BUILD_ID);
}

/*
 * Translation block descriptor — 32 bytes so tb_table[4096] fits in 32 NVMe
 * pages.  exec_count is the Phase 3 hot-block profiling counter.
 */
typedef struct {
    uint64_t guest_pc;      /* primary key; 0 = empty slot */
    uint32_t code_offset;   /* byte offset into qemu_sls_codebuf */
    uint32_t code_len;
    uint32_t gen_expected;  /* page_gen value at translation time */
    uint32_t exec_count;    /* incremented per execution hit (Phase 3) */
    uint32_t guest_page;    /* gpa / 4096, for flush_page invalidation */
    /* Guest instructions this block covers. Was _pad. Needed because a cache
     * HIT skips translation entirely, and the launcher's instruction counter
     * would otherwise stop advancing -- making a warm run look like it executed
     * nothing. Struct stays 32 bytes, so the NVMe layout is unchanged. */
    uint32_t insn_count;
} QemuTBDesc;               /* 32 bytes */

/* Generation counter per guest code page — read in TCG-emitted prologues. */
extern uint32_t qemu_sls_page_gen[QEMU_TCACHE_GEN_PAGES];

/* Code-gen buffer base and current write cursor. */
extern uint8_t *qemu_sls_codebuf;
extern uint32_t qemu_sls_codebuf_used;

/*
 * Restore gen counters, TB table, and code buffer from NVMe if a valid
 * snapshot exists; otherwise cold-start (gen counters = 1, empty TB table).
 * Safe to call when NVMe is unavailable — guards on io_sq/io_cq.
 */
int  qemu_sls_tcache_init(void);

/* ─── Page digests: telling "rewritten" apart from "changed" ────────────────
 * The loader rewrites the guest image into guest RAM on every launch, because
 * guest RAM is not persisted while the translation cache is. It then flushed
 * the pages it wrote, which invalidated every restored TB -- measured, one
 * flush of page 0 per boot, generation climbing 2, 3, 4 while the restored
 * descriptors stayed one behind. A cache that survives NVMe and is then
 * destroyed by its own loader is a cache that never works across a reboot.
 *
 * The old predicate asked "do the bytes I am about to write differ from what
 * is in guest RAM?" On a fresh boot that is always yes, because the frames are
 * newly allocated. The predicate that matters is "do the bytes I am about to
 * write differ from the bytes these TBs were compiled from?" -- and after the
 * copy the page holds exactly those bytes, so the flush was firing on a page
 * that ended up correct.
 *
 * A digest of the written range answers the second question. The range, not
 * the whole page: allocate_physical_ram_frame() does not zero, so the tail of
 * the last page is recycled memory that differs between boots and would defeat
 * any full-page comparison.
 *
 * SAFETY -- the rule that makes this sound rather than merely convenient:
 * qemu_sls_tcache_flush_page() CLEARS the digest for the page it flushes. Any
 * invalidation from outside the loader (a guest store caught by
 * qemu_sls_mmu_shadow_fault(), DMA completion) therefore drops the digest, and
 * the next load cannot match against it. Only a page whose digest was recorded
 * by the loader, after its own flush, can ever be skipped. Loosening a
 * cache-validity check is the dangerous direction, and this is the invariant
 * that bounds it. */
int  qemu_sls_tcache_page_matches(uint64_t gpa, const void *bytes, uint32_t len);
void qemu_sls_tcache_record_page(uint64_t gpa, const void *bytes, uint32_t len);

/* Bump gen counter for gpa's page; immediately persists the affected NVMe page.
 * Also clears that page's digest -- see the note above. */
void qemu_sls_tcache_flush_page(uint64_t gpa);

/* Invalidate all TBs by incrementing every gen counter. */
void qemu_sls_tcache_flush_all(void);

/*
 * Look up the TB for guest_pc.  Returns a pointer into the code buffer on
 * hit (also fills *code_len_out), NULL on miss or gen mismatch.
 * Increments exec_count on each hit for Phase 3 profiling.
 */
void *qemu_sls_tcache_lookup(uint64_t guest_pc, uint32_t *code_len_out,
                             uint32_t *insn_count_out);

/*
 * Record a new TB.  code_offset is the byte offset already written into the
 * code buffer by the TCG backend; gpa is the guest physical address of the
 * source page.  Returns 0 on success, -1 if the table is full.
 */
int  qemu_sls_tcache_insert(uint64_t guest_pc, uint32_t code_offset,
                             uint32_t code_len, uint64_t gpa,
                             uint32_t insn_count);

/*
 * Write gen counters + TB table + code buffer to NVMe.
 * Call after a translation burst or on clean shutdown.
 */
void qemu_sls_tcache_sync(void);

/* Iterate every TB whose exec_count >= threshold; used by the Phase 3 scanner. */
void qemu_sls_tcache_foreach_hot(uint32_t threshold,
    void (*cb)(uint64_t guest_pc, uint32_t code_offset,
               uint32_t code_len, uint32_t exec_count));

/* ─── Generate-in-place: reserve, then commit ──────────────────────────────
 *
 * TCG output is POSITION-DEPENDENT. It carries relative calls and jumps -- to
 * helpers, to the epilogue, for exit_tb -- whose displacements are computed
 * against the address the block is generated at. Moving a block afterwards
 * breaks every one of them, which is why qemu_sls_tcache_store() below could
 * copy perfectly valid bytes and still produce a block that hung when run.
 * QEMU never relocates generated code, and neither can this.
 *
 * So the block must be generated AT its permanent address. That address is
 * needed before tcg_gen_code() runs, and the length only afterwards -- hence
 * two calls:
 *
 *     void *at = qemu_sls_tcache_reserve();      // where to generate
 *     tb->tc.ptr = at;
 *     int len = tcg_gen_code(...);               // emits AT that address
 *     qemu_sls_tcache_commit(pc, at, len, gpa, insns);
 *
 * reserve() returns NULL when too little room remains for a worst-case block,
 * and the caller then generates into TCG's own buffer as before -- correct,
 * just not cached. Nothing is recorded until commit(), so a failed or
 * abandoned translation leaves no descriptor pointing at half-written code.
 *
 * This is also why the buffer is .bss at a linker-fixed VA: a restored block
 * is only valid at precisely the address it was born at, which is exactly what
 * the build-identity stamp is there to guarantee.
 */
#define QEMU_TCACHE_MAX_TB_BYTES 65536U

void *qemu_sls_tcache_reserve(void);
int   qemu_sls_tcache_commit(uint64_t guest_pc, void *code_at,
                             uint32_t code_len, uint64_t gpa,
                             uint32_t insn_count);

/* Update a TB's code location after PGO re-translation; resets exec_count. */
void qemu_sls_tcache_update_tb(uint64_t guest_pc,
    uint32_t new_code_offset, uint32_t new_code_len);

/* Mark code-buffer pages covering [code_offset, code_offset+len) dirty.
 * Call after the TCG backend writes new code so sync() persists only changed pages. */
void qemu_sls_tcache_mark_code_dirty(uint32_t code_offset, uint32_t len);

#endif /* QEMU_SLS_TCACHE_H */
