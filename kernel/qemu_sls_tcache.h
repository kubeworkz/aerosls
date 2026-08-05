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

#define QEMU_TCACHE_MAGIC        0xCAFE000000000020ULL


/* Matches QEMU_GUEST_RAM_PAGES — one gen counter per guest 4 KiB page. */
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

/* Bump gen counter for gpa's page; immediately persists the affected NVMe page. */
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

/*
 * Copy freshly generated host code into the persistent buffer and record it.
 *
 * Gate 2 of the Phase 2 plan. TCG generates into its OWN buffer
 * (sls_code_buffer, 32 MiB, sls/sls-runtime.c); this copies the emitted bytes
 * into qemu_sls_codebuf (4 MiB, persisted) and inserts the descriptor, so the
 * two allocators stay independent.
 *
 * Copy rather than redirecting TCG's allocator at this buffer: that would put
 * TCG's region manager and this cursor in charge of the same memory, and being
 * wrong about which owns what means executing the wrong bytes -- with cached
 * host code, silently. One memcpy per translated block is a cheap price for
 * keeping the two apart, and translation already costs ~85% of a launch.
 *
 * Returns a pointer INTO the persistent buffer -- the address the code will
 * occupy on every subsequent boot, which is why the buffer is .bss at a
 * linker-fixed VA and why the identity stamp must match before any of it is
 * trusted. NULL if the buffer or the table is full.
 */
void *qemu_sls_tcache_store(uint64_t guest_pc, const void *code,
                            uint32_t code_len, uint64_t gpa,
                            uint32_t insn_count);

/* Update a TB's code location after PGO re-translation; resets exec_count. */
void qemu_sls_tcache_update_tb(uint64_t guest_pc,
    uint32_t new_code_offset, uint32_t new_code_len);

/* Mark code-buffer pages covering [code_offset, code_offset+len) dirty.
 * Call after the TCG backend writes new code so sync() persists only changed pages. */
void qemu_sls_tcache_mark_code_dirty(uint32_t code_offset, uint32_t len);

#endif /* QEMU_SLS_TCACHE_H */
