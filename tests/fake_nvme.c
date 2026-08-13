/* fake_nvme.c — see fake_nvme.h. */

#include "fake_nvme.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* nvme_io.h's externs. Non-NULL means "the I/O queue is up", which is what
 * every guarded call site tests before touching the disk. */
void *io_sq = (void *)1;
void *io_cq = (void *)1;

/* Open addressing, power-of-two capacity, linear probe. 4096 frames is 16 MiB
 * of host memory and far more than any test here touches; overflowing it is
 * reported as a contract violation rather than silently wrapping, because a
 * silently-wrapping store would make a test pass by losing the write it was
 * checking for. */
#define CAP 4096u
struct slot { uint64_t lba; int used; unsigned char data[FAKE_NVME_FRAME]; };
static struct slot g_slots[CAP];
static unsigned    g_used;

static unsigned long g_ops, g_reads, g_writes, g_violations;
static unsigned long g_fail_at;   static int      g_fail_persistent;
static unsigned long g_tear_at;   static unsigned g_tear_sectors;

static void violation(const char *what, uint64_t lba)
{
    g_violations++;
    fprintf(stderr, "fake_nvme: CONTRACT VIOLATION: %s (lba %llu)\n",
            what, (unsigned long long)lba);
}

static unsigned hash_lba(uint64_t lba)
{
    /* Frames are 8 sectors apart, so the low three bits of an LBA carry no
     * information and hashing on them would put every frame in one bucket. */
    uint64_t h = (lba / FAKE_NVME_SECTORS) * 0x9E3779B97F4A7C15ull;
    return (unsigned)((h >> 32) & (CAP - 1u));
}

static struct slot *find(uint64_t lba, int create)
{
    unsigned i = hash_lba(lba);
    for (unsigned n = 0; n < CAP; n++) {
        struct slot *s = &g_slots[(i + n) & (CAP - 1u)];
        if (s->used && s->lba == lba) { return s; }
        if (!s->used) {
            if (!create) { return 0; }
            if (g_used + 1 >= CAP) { violation("frame store full", lba); return 0; }
            s->used = 1; s->lba = lba; memset(s->data, 0, FAKE_NVME_FRAME);
            g_used++;
            return s;
        }
    }
    violation("frame store full", lba);
    return 0;
}

void fake_nvme_reset(void)
{
    memset(g_slots, 0, sizeof g_slots);
    g_used = 0;
    g_ops = g_reads = g_writes = g_violations = 0;
    g_fail_at = 0; g_fail_persistent = 0;
    g_tear_at = 0; g_tear_sectors = 0;
}

void fake_nvme_fail_at(unsigned long op, int persistent)
{
    g_fail_at = op; g_fail_persistent = persistent;
}

void fake_nvme_tear_at(unsigned long op, unsigned sectors)
{
    g_tear_at = op; g_tear_sectors = sectors;
}

unsigned long fake_nvme_ops(void)        { return g_ops; }
unsigned long fake_nvme_reads(void)      { return g_reads; }
unsigned long fake_nvme_writes(void)     { return g_writes; }
unsigned long fake_nvme_violations(void) { return g_violations; }

/* drivers/nvme_io.h: "buf must be 4-KiB page-aligned". NVMe requires every PRP
 * entry after the first to have a zero offset, so an unaligned buffer cannot
 * be described by a PRP list at all -- the real driver refuses it. A mock that
 * accepted it would let a bug through to hardware, where it is far more
 * expensive to find. */
static int aligned(const void *buf, uint64_t lba, const char *dir)
{
    if (((uintptr_t)buf & (FAKE_NVME_FRAME - 1u)) != 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "unaligned buffer passed to %s", dir);
        violation(msg, lba);
        return 0;
    }
    return 1;
}

static int should_fail(void)
{
    if (g_fail_at == 0) { return 0; }
    if (g_ops == g_fail_at) { return 1; }
    return g_fail_persistent && g_ops > g_fail_at;
}

int nvme_read_sync(uint64_t slba, void *buf)
{
    struct slot *s;
    g_ops++; g_reads++;
    if (!aligned(buf, slba, "nvme_read_sync")) { return 1; }
    if (should_fail()) { return 1; }
    s = find(slba, 0);
    /* A frame never written reads as zeroes -- what a fresh image gives. */
    if (!s) { memset(buf, 0, FAKE_NVME_FRAME); return 0; }
    memcpy(buf, s->data, FAKE_NVME_FRAME);
    return 0;
}

int nvme_write_sync(uint64_t slba, const void *buf)
{
    struct slot *s;
    g_ops++; g_writes++;
    if (!aligned(buf, slba, "nvme_write_sync")) { return 1; }
    if (should_fail()) { return 1; }
    s = find(slba, 1);
    if (!s) { return 1; }

    if (g_tear_at != 0 && g_ops == g_tear_at) {
        /* Power lost partway through the command: some sectors landed, the
         * rest hold whatever was there before. Reported as SUCCESS, because
         * that is what makes torn writes hard -- the caller is told the write
         * completed. */
        unsigned n = g_tear_sectors > FAKE_NVME_SECTORS
                         ? FAKE_NVME_SECTORS : g_tear_sectors;
        memcpy(s->data, buf, (size_t)n * (FAKE_NVME_FRAME / FAKE_NVME_SECTORS));
        return 0;
    }

    memcpy(s->data, buf, FAKE_NVME_FRAME);
    return 0;
}

int fake_nvme_peek(uint64_t lba, unsigned char out[FAKE_NVME_FRAME])
{
    struct slot *s = find(lba, 0);
    if (!s) { memset(out, 0, FAKE_NVME_FRAME); return 0; }
    memcpy(out, s->data, FAKE_NVME_FRAME);
    return 1;
}

int fake_nvme_frame_equals(uint64_t lba, const unsigned char *expect, size_t len)
{
    struct slot *s = find(lba, 0);
    if (!s || len > FAKE_NVME_FRAME) { return 0; }
    return memcmp(s->data, expect, len) == 0;
}
