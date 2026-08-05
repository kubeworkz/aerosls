/*
 * qemu_sls_tcache.c — Phase 2 persistent translation cache for QEMU-SLS.
 * See docs/AeroSLS-QEMU-SLS-Viability-Analysis.md §Phase 2.
 *
 * The code-gen buffer (codebuf_storage) lives in .bss at a linker-assigned VA
 * that is identical every boot, so absolute addresses embedded in translated
 * code remain valid after restore.  Gen counters and the TB descriptor table
 * are both persisted synchronously, making stale-translation detection correct
 * across reboots without a boot-epoch counter.
 */

#include "qemu_sls_tcache.h"
#include "checkpoint_delta.h"
#include "kernel_io.h"
#include "../drivers/nvme_io.h"
#include <stddef.h>
#include <stdint.h>

/* ─── static storage ────────────────────────────────────────────────────── */

uint32_t qemu_sls_page_gen[QEMU_TCACHE_GEN_PAGES]
    __attribute__((aligned(4096)));

static QemuTBDesc tb_table[QEMU_TCACHE_MAX_TBS]
    __attribute__((aligned(4096)));

static uint8_t codebuf_storage[QEMU_TCACHE_CODEBUF_SIZE]
    __attribute__((aligned(4096)));

uint8_t  *qemu_sls_codebuf      = codebuf_storage;
uint32_t  qemu_sls_codebuf_used = 0;

static int initialized;

/* Scratch frame for single-page NVMe header reads/writes. */
static uint8_t io_scratch[4096] __attribute__((aligned(4096)));
/* One bit per code-buffer page; set by mark_code_dirty, cleared after adaptive sync. */
#define CODEBUF_DIRTY_WORDS ((QEMU_TCACHE_CODEBUF_PAGES + 63) / 64)
static uint64_t codebuf_dirty_map[CODEBUF_DIRTY_WORDS];
/* ─── internal helpers ──────────────────────────────────────────────────── */

/* Fibonacci multiplicative hash — good distribution for page-aligned PCs. */
static uint32_t hash_pc(uint64_t pc) {
    return (uint32_t)((pc * 0x9e3779b97f4a7c15ULL) >> 52)
           & (QEMU_TCACHE_MAX_TBS - 1);
}

static void write_gen_page(uint32_t nvme_pg) {
    nvme_write_sync(QEMU_TCACHE_GEN_DAT_LBA + nvme_pg * NVME_SECTORS_PER_PAGE,
                    qemu_sls_page_gen + nvme_pg * (NVME_PAGE_SIZE / sizeof(uint32_t)));
}
/* ─── qemu_sls_tcache_mark_code_dirty ──────────────────────────────────────── */

void qemu_sls_tcache_mark_code_dirty(uint32_t code_offset, uint32_t len) {
    if (!len) return;
    uint32_t first = code_offset / NVME_PAGE_SIZE;
    uint32_t last  = (code_offset + len - 1) / NVME_PAGE_SIZE;
    if (last >= QEMU_TCACHE_CODEBUF_PAGES) last = QEMU_TCACHE_CODEBUF_PAGES - 1;
    for (uint32_t p = first; p <= last; p++)
        codebuf_dirty_map[p / 64] |= 1ULL << (p % 64);
}
/* ─── qemu_sls_tcache_init ──────────────────────────────────────────────── */

int qemu_sls_tcache_init(void) {
    if (!io_sq || !io_cq) {
        for (uint32_t i = 0; i < QEMU_TCACHE_GEN_PAGES; i++)
            qemu_sls_page_gen[i] = 1;
        initialized = 1;
        kernel_serial_print("[QEMU-SLS TCACHE] NVMe unavailable — cold start\n");
        return 0;
    }

    if (nvme_read_sync(QEMU_TCACHE_HDR_LBA, io_scratch) != 0 ||
        *(const uint64_t *)io_scratch != QEMU_TCACHE_MAGIC) {
        for (uint32_t i = 0; i < QEMU_TCACHE_GEN_PAGES; i++)
            qemu_sls_page_gen[i] = 1;
        initialized = 1;
        kernel_serial_print("[QEMU-SLS TCACHE] no snapshot — cold start\n");
        return 0;
    }

    /* ─── Identity, checked before a single cached byte is restored ────────
     * The magic above only proves SOME build wrote this. What follows it is
     * host machine code, so a cache from a different compiler or different
     * code-generation flags would be executed as though it were ours -- with
     * no fault and no diagnostic. Discard and cold-start on any mismatch: a
     * needless recompile costs one round; a wrongly-accepted cache costs
     * arbitrary behaviour. See the note in qemu_sls_tcache.h. */
    {
        uint64_t want = qemu_tcache_identity();
        uint64_t got  = *(const uint64_t *)(io_scratch + 16);
        if (got != want) {
            for (uint32_t i = 0; i < QEMU_TCACHE_GEN_PAGES; i++)
                qemu_sls_page_gen[i] = 1;
            initialized = 1;
            kernel_serial_printf(
                "[QEMU-SLS TCACHE] snapshot is from a DIFFERENT BUILD "
                "(identity 0x%016lx, this build 0x%016lx) — discarded, cold start.\n"
                "[QEMU-SLS TCACHE] It holds host machine code; running it would "
                "execute instructions built against different assumptions.\n",
                got, want);
            return 0;
        }
    }

    uint32_t saved_code_used = *(const uint32_t *)(io_scratch + 12);

    /* Restore gen counters: 64 pages in 2 batches. */
    nvme_read_pages_sync(QEMU_TCACHE_GEN_DAT_LBA,
                         qemu_sls_page_gen, 32);
    nvme_read_pages_sync(QEMU_TCACHE_GEN_DAT_LBA + 32 * NVME_SECTORS_PER_PAGE,
                         (uint8_t *)qemu_sls_page_gen + 32 * NVME_PAGE_SIZE, 32);

    /* Restore TB table: 32 pages. */
    nvme_read_pages_sync(QEMU_TCACHE_TB_DAT_LBA,
                         tb_table, sizeof(tb_table) / NVME_PAGE_SIZE);

    /* Restore code buffer: 1024 pages in 32-page batches. */
    for (uint32_t b = 0; b < QEMU_TCACHE_CODEBUF_PAGES;
         b += NVME_MAX_PAGES_PER_XFER) {
        nvme_read_pages_sync(
            QEMU_TCACHE_CODE_DAT_LBA + (uint64_t)b * NVME_SECTORS_PER_PAGE,
            codebuf_storage + (uint64_t)b * NVME_PAGE_SIZE,
            NVME_MAX_PAGES_PER_XFER);
    }
    qemu_sls_codebuf_used = saved_code_used;

    initialized = 1;
    kernel_serial_printf(
        "[QEMU-SLS TCACHE] warm start — codebuf_used=%u, codebuf=0x%016lx\n",
        saved_code_used, (uint64_t)(uintptr_t)codebuf_storage);
    return 0;
}

/* ─── qemu_sls_tcache_flush_page ─────────────────────────────────────────── */

void qemu_sls_tcache_flush_page(uint64_t gpa) {
    uint32_t page = (uint32_t)(gpa / NVME_PAGE_SIZE);
    if (page >= QEMU_TCACHE_GEN_PAGES) return;
    qemu_sls_page_gen[page]++;
    /* Synchronously persist the one NVMe page covering this counter. */
    if (io_sq && io_cq)
        write_gen_page(page / (NVME_PAGE_SIZE / sizeof(uint32_t)));
}

/* ─── qemu_sls_tcache_flush_all ──────────────────────────────────────────── */

void qemu_sls_tcache_flush_all(void) {
    for (uint32_t i = 0; i < QEMU_TCACHE_GEN_PAGES; i++)
        qemu_sls_page_gen[i]++;
    /* Full gen array sync is left to the next qemu_sls_tcache_sync() call. */
}

/* ─── qemu_sls_tcache_lookup ─────────────────────────────────────────────── */

void *qemu_sls_tcache_lookup(uint64_t guest_pc, uint32_t *code_len_out,
                             uint32_t *insn_count_out) {
    if (!initialized || !guest_pc) return NULL;
    uint32_t slot = hash_pc(guest_pc);
    for (uint32_t i = 0; i < QEMU_TCACHE_MAX_TBS; i++) {
        QemuTBDesc *d = &tb_table[(slot + i) & (QEMU_TCACHE_MAX_TBS - 1)];
        if (!d->guest_pc) return NULL;
        if (d->guest_pc != guest_pc) continue;
        /* Stale translation: source page was written after this TB was compiled. */
        if (d->guest_page < QEMU_TCACHE_GEN_PAGES &&
            qemu_sls_page_gen[d->guest_page] != d->gen_expected)
            return NULL;
        d->exec_count++;
        if (code_len_out)   *code_len_out   = d->code_len;
        if (insn_count_out) *insn_count_out = d->insn_count;
        return codebuf_storage + d->code_offset;
    }
    return NULL;
}

/* ─── qemu_sls_tcache_insert ─────────────────────────────────────────────── */

/* ─── qemu_sls_tcache_store ─────────────────────────────────────────────── */

void *qemu_sls_tcache_store(uint64_t guest_pc, const void *code,
                            uint32_t code_len, uint64_t gpa,
                            uint32_t insn_count) {
    /* ─── The one path that used to fail silently ──────────────────────────
     * Two loud failure messages below and a bare `return 0` here meant a cache
     * that stored nothing looked identical to one that was never called. On
     * the first hardware run every block was a miss, checkpoint reported
     * "0 TBs, 0 code bytes", and neither failure message appeared -- leaving
     * no way to tell which of four conditions had declined the store.
     *
     * guest_pc == 0 is the interesting one and is NOT a bug in the caller: the
     * benchmark guest is loaded at GPA 0, so its first block legitimately
     * starts at guest_pc 0 -- which qemu_sls_tcache_insert() uses as its
     * empty-slot sentinel. That block can never be cached until the table
     * distinguishes "empty" from "guest_pc 0" with a separate valid flag. */
    if (!initialized || !guest_pc || !code || !code_len) {
        kernel_serial_printf(
            "[QEMU-SLS TCACHE] store declined at guest_pc=0x%016lx: %s\n",
            guest_pc,
            !initialized ? "tcache not initialised" :
            !guest_pc    ? "guest_pc is 0, which the TB table uses as its "
                           "empty-slot marker -- this block cannot be cached" :
            !code        ? "code pointer is NULL" :
                           "code_len is 0");
        return 0;
    }

    /* 16-byte alignment: generated blocks are entered by an indirect call, and
     * an unaligned entry point costs a fetch penalty on every execution of a
     * block that will be re-executed for the life of the cache. */
    uint32_t off = (qemu_sls_codebuf_used + 15u) & ~15u;

    if ((uint64_t)off + code_len > QEMU_TCACHE_CODEBUF_SIZE) {
        /* Full. Refuse rather than wrap: wrapping would overwrite code that
         * live TB descriptors still point at, and the next lookup would return
         * a valid-looking pointer into the middle of someone else's block.
         * Declining to cache costs a re-translation; wrapping costs a jump
         * into arbitrary bytes. */
        kernel_serial_printf(
            "[QEMU-SLS TCACHE] code buffer full (%u of %u bytes) -- not caching "
            "the block at guest_pc=0x%016lx. Translation still works; it will "
            "just not persist.\n",
            qemu_sls_codebuf_used, (unsigned)QEMU_TCACHE_CODEBUF_SIZE, guest_pc);
        return 0;
    }

    uint8_t *dst = qemu_sls_codebuf + off;
    const uint8_t *src = (const uint8_t *)code;
    for (uint32_t i = 0; i < code_len; i++) dst[i] = src[i];

    if (qemu_sls_tcache_insert(guest_pc, off, code_len, gpa, insn_count) != 0) {
        /* Table full. The bytes are already copied, but without a descriptor
         * nothing can ever find them, so the space is wasted rather than
         * dangerous. The cursor is NOT advanced, so the next store reuses it. */
        kernel_serial_printf(
            "[QEMU-SLS TCACHE] TB table full (%u entries) -- block at "
            "guest_pc=0x%016lx not cached.\n",
            (unsigned)QEMU_TCACHE_MAX_TBS, guest_pc);
        return 0;
    }

    qemu_sls_codebuf_used = off + code_len;
    qemu_sls_tcache_mark_code_dirty(off, code_len);
    return dst;
}

int qemu_sls_tcache_insert(uint64_t guest_pc, uint32_t code_offset,
                            uint32_t code_len, uint64_t gpa,
                            uint32_t insn_count) {
    if (!initialized || !guest_pc) return -1;
    uint32_t guest_page = (uint32_t)(gpa / NVME_PAGE_SIZE);
    uint32_t gen = (guest_page < QEMU_TCACHE_GEN_PAGES)
                   ? qemu_sls_page_gen[guest_page] : 0;
    uint32_t slot = hash_pc(guest_pc);
    for (uint32_t i = 0; i < QEMU_TCACHE_MAX_TBS; i++) {
        QemuTBDesc *d = &tb_table[(slot + i) & (QEMU_TCACHE_MAX_TBS - 1)];
        if (!d->guest_pc || d->guest_pc == guest_pc) {
            d->guest_pc     = guest_pc;
            d->code_offset  = code_offset;
            d->code_len     = code_len;
            d->gen_expected = gen;
            d->exec_count   = 0;
            d->guest_page   = guest_page;
            d->insn_count   = insn_count;
            ckpt_mark_dirty(CKPT_REGION_TCACHE);
            return 0;
        }
    }
    return -1;  /* table full */
}

/* ─── qemu_sls_tcache_foreach_hot ──────────────────────────────────────────── */

void qemu_sls_tcache_foreach_hot(uint32_t threshold,
    void (*cb)(uint64_t guest_pc, uint32_t code_offset,
               uint32_t code_len, uint32_t exec_count)) {
    for (uint32_t i = 0; i < QEMU_TCACHE_MAX_TBS; i++) {
        QemuTBDesc *d = &tb_table[i];
        if (d->guest_pc && d->exec_count >= threshold)
            cb(d->guest_pc, d->code_offset, d->code_len, d->exec_count);
    }
}

/* ─── qemu_sls_tcache_update_tb ─────────────────────────────────────────────── */

void qemu_sls_tcache_update_tb(uint64_t guest_pc,
    uint32_t new_code_offset, uint32_t new_code_len) {
    if (!guest_pc) return;
    uint32_t slot = hash_pc(guest_pc);
    for (uint32_t i = 0; i < QEMU_TCACHE_MAX_TBS; i++) {
        QemuTBDesc *d = &tb_table[(slot + i) & (QEMU_TCACHE_MAX_TBS - 1)];
        if (!d->guest_pc) return;
        if (d->guest_pc != guest_pc) continue;
        d->code_offset = new_code_offset;
        d->code_len    = new_code_len;
        d->exec_count  = 0;
        return;
    }
}

void qemu_sls_tcache_sync(void) {
    if (!initialized || !io_sq || !io_cq) return;

    /* Count live TBs for the log line. */
    uint32_t tb_count = 0;
    for (uint32_t i = 0; i < QEMU_TCACHE_MAX_TBS; i++)
        if (tb_table[i].guest_pc) tb_count++;

    /* Header. */
    for (uint32_t i = 0; i < 4096; i++) io_scratch[i] = 0;
    *(uint64_t *)io_scratch        = QEMU_TCACHE_MAGIC;
    *(uint32_t *)(io_scratch +  8) = tb_count;
    *(uint32_t *)(io_scratch + 12) = qemu_sls_codebuf_used;
    /* Identity, so the next boot can tell OUR cache from a different build's.
     * Written on every sync rather than once, because the value is derived at
     * compile time and a header written by an older binary would otherwise
     * carry an older stamp into a newer file. */
    *(uint64_t *)(io_scratch + 16) = qemu_tcache_identity();
    *(uint32_t *)(io_scratch + 24) = QEMU_TCACHE_CODEBUF_SIZE;
    *(uint32_t *)(io_scratch + 28) = QEMU_TCACHE_MAX_TBS;
    nvme_write_sync(QEMU_TCACHE_HDR_LBA, io_scratch);

    /* Gen counters: 64 pages in 2 batches. */
    nvme_write_pages_sync(QEMU_TCACHE_GEN_DAT_LBA,
                          qemu_sls_page_gen, 32);
    nvme_write_pages_sync(QEMU_TCACHE_GEN_DAT_LBA + 32 * NVME_SECTORS_PER_PAGE,
                          (const uint8_t *)qemu_sls_page_gen + 32 * NVME_PAGE_SIZE,
                          32);

    /* TB table: 32 pages. */
    nvme_write_pages_sync(QEMU_TCACHE_TB_DAT_LBA,
                          tb_table, sizeof(tb_table) / NVME_PAGE_SIZE);

    /* Code buffer: adaptive — only write dirty 4 KiB pages (Phase 4). */
    for (uint32_t p = 0; p < QEMU_TCACHE_CODEBUF_PAGES; p++) {
        if (!(codebuf_dirty_map[p / 64] & (1ULL << (p % 64)))) continue;
        nvme_write_sync(QEMU_TCACHE_CODE_DAT_LBA + (uint64_t)p * NVME_SECTORS_PER_PAGE,
                        codebuf_storage + (uint64_t)p * NVME_PAGE_SIZE);
        codebuf_dirty_map[p / 64] &= ~(1ULL << (p % 64));
    }

    nvme_flush_sync();
    kernel_serial_printf(
        "[QEMU-SLS TCACHE] synced: %u TBs, %u code bytes\n",
        tb_count, qemu_sls_codebuf_used);
}
