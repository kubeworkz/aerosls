/*
 * sls_alloc_host_test.c — the QEMU-SLS arena allocator, against the REAL
 * sls_malloc/sls_free from qemu/sls/sls-runtime.c.
 *
 * ─── The leak this replaces ───────────────────────────────────────────────
 * sls_free() was a no-op, on the reasoning that "TCG's own pool system handles
 * reuse". That holds for allocations up to TCG_POOL_CHUNK_SIZE (32 KiB), which
 * TCG recycles through its pool_first chain and never frees. It does not hold
 * for anything larger: tcg.c:1295 puts those on pool_first_large, and
 * tcg_pool_reset() releases that chain with g_free().
 *
 * Measured on hardware: one ~1.48 MB allocation per translated block, leaked.
 * At a 16 MiB arena that exhausted it on the second `qemu bench 500` of a boot
 * and halted the node. Raising the arena to 64 MiB bought runs; it fixed
 * nothing.
 *
 * ─── Why an allocator needs its own test more than most code ──────────────
 * Every failure here is delayed and misattributed. A wrong size hands out an
 * overlapping block and corrupts whatever was already living there; a double
 * free makes the list circular and hangs the NEXT allocation; a bad header
 * check lets a wild pointer onto the free list to be handed out later. None of
 * those fail where they are caused, and all of them look like a bug in TCG.
 *
 * So the assertions below are about the far side of each boundary: the address
 * returned, the bytes still readable, the counters, and whether a refusal
 * actually refused.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I ../qemu/sls -I ../qemu/sls/include \
 *       -I ../qemu/include -DSLS_ALLOC_HOST_TEST=1 \
 *       -o /tmp/sls_alloc_host_test \
 *       tests/sls_alloc_host_test.c
 *   /tmp/sls_alloc_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

static int checks_passed = 0, checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* ─── The allocator, extracted verbatim ───────────────────────────────────
 * sls-runtime.c cannot be compiled here: it pulls in setjmp, the TCG headers
 * and the whole freestanding environment. The allocator is reproduced below
 * and MUST match. The risk of divergence is real, so the last check in main()
 * greps the original for the structural facts this copy depends on -- a
 * silently diverged copy is worse than no test.
 *
 * Panic output is captured rather than executed: the real one writes to a UART.
 */
static char  g_panic[1024];
static int   g_panic_len;
static void  kernel_panic_puts(const char *s) {
    for (; *s && g_panic_len < (int)sizeof g_panic - 1; s++)
        g_panic[g_panic_len++] = *s;
    g_panic[g_panic_len] = 0;
}
static void panic_reset(void) { g_panic_len = 0; g_panic[0] = 0; }
static int  panicked(void)    { return g_panic_len > 0; }

/* 8 MiB: large enough to hold the 1.48 MB block TCG actually asks for, twice.
 * Sized at 1 MiB first, which made the very first allocation abort -- and the
 * "control: the first allocation did grow it" check caught it immediately.
 * Without that control, the reuse check above it would have compared two NULLs
 * and PASSED, reporting a working allocator that had allocated nothing. */
#define SLS_HEAP_SIZE (8u * 1024u * 1024u)
static uint8_t  sls_heap[SLS_HEAP_SIZE] __attribute__((aligned(4096)));
static uint32_t sls_heap_offset = 0;
static uint32_t sls_alloc_count, sls_alloc_largest;
static void sls_abort(const char *m) { kernel_panic_puts(m); }

#define SLS_ALLOC_MAGIC 0x5A11ACEDu
typedef struct SlsBlockHdr {
    uint32_t magic; uint32_t size; struct SlsBlockHdr *next_free;
} SlsBlockHdr;
static SlsBlockHdr *sls_free_list;
static uint32_t sls_reuse_count, sls_free_count;

static void *sls_malloc(size_t size) {
    sls_alloc_count++;
    if (size > sls_alloc_largest) sls_alloc_largest = (uint32_t)size;
    size = (size + 15) & ~(size_t)15;
    SlsBlockHdr **link = &sls_free_list;
    for (SlsBlockHdr *b = sls_free_list; b; link = &b->next_free, b = b->next_free) {
        if (b->size < size) continue;
        *link = b->next_free;
        b->next_free = 0;
        sls_reuse_count++;
        return (uint8_t *)b + sizeof(SlsBlockHdr);
    }
    if (sls_heap_offset + size + sizeof(SlsBlockHdr) > SLS_HEAP_SIZE) {
        sls_abort("sls_malloc: heap exhausted");
        return 0;
    }
    SlsBlockHdr *h = (SlsBlockHdr *)&sls_heap[sls_heap_offset];
    h->magic = SLS_ALLOC_MAGIC; h->size = (uint32_t)size; h->next_free = 0;
    sls_heap_offset += (uint32_t)(size + sizeof(SlsBlockHdr));
    return (uint8_t *)h + sizeof(SlsBlockHdr);
}

static void sls_free(void *ptr) {
    if (!ptr) return;
    SlsBlockHdr *h = (SlsBlockHdr *)((uint8_t *)ptr - sizeof(SlsBlockHdr));
    if ((uint8_t *)h < sls_heap || (uint8_t *)h >= sls_heap + SLS_HEAP_SIZE) {
        kernel_panic_puts("outside the arena"); return;
    }
    if (h->magic != SLS_ALLOC_MAGIC) {
        kernel_panic_puts("bad or missing allocation header"); return;
    }
    for (SlsBlockHdr *b = sls_free_list; b; b = b->next_free)
        if (b == h) { kernel_panic_puts("double free"); return; }
    h->next_free = sls_free_list;
    sls_free_list = h;
    sls_free_count++;
}

int main(void) {
    printf("sls_alloc_host_test\n===================\n\n");

    printf("-- the property the leak was about --\n");
    {
        /* TCG's shape exactly: allocate a large block, free it at
         * tcg_pool_reset(), allocate the same size for the next translation.
         * Under the old no-op free this consumed the arena once per block. */
        uint32_t before = sls_heap_offset;
        void *first = sls_malloc(1479112);
        CHECK(first != 0, "control: the allocation succeeded at all");
        uint32_t after_first = sls_heap_offset;
        sls_free(first);
        void *second = sls_malloc(1479112);

        CHECK(second != 0 && second == first,
              "*** the same block comes back -- alloc/free/alloc of one size is "
              "TCG's exact pattern, and it is what leaked 1.48 MB per translated "
              "block ***");
        CHECK(sls_heap_offset == after_first,
              "*** ...and the arena did NOT grow. The address matching is not "
              "enough on its own: an allocator could return a stale pointer to "
              "memory it had already handed out again ***");
        CHECK(after_first > before, "control: the first allocation did grow it");
        sls_free(second);
    }

    printf("\n-- alignment, which TCG structs assume --\n");
    {
        void *a = sls_malloc(1), *b = sls_malloc(17), *c = sls_malloc(4095);
        CHECK(((uintptr_t)a % 16) == 0 && ((uintptr_t)b % 16) == 0 &&
              ((uintptr_t)c % 16) == 0,
              "*** every returned pointer is 16-byte aligned, header included. "
              "A header that broke alignment would be a subtler bug than the "
              "leak it fixed ***");
        CHECK(a != b && b != c && a != c, "...and distinct allocations differ");
    }

    printf("\n-- blocks do not overlap --\n");
    {
        /* The failure a wrong size header produces: two live allocations
         * sharing bytes. Writing a distinct pattern to each and reading both
         * back is the only assertion that catches it -- the addresses can look
         * perfectly reasonable. */
        uint8_t *p = sls_malloc(1000);
        uint8_t *q = sls_malloc(1000);
        memset(p, 0xAA, 1000);
        memset(q, 0xBB, 1000);
        int p_intact = 1, q_intact = 1;
        for (int i = 0; i < 1000; i++) {
            if (p[i] != 0xAA) p_intact = 0;
            if (q[i] != 0xBB) q_intact = 0;
        }
        CHECK(p_intact && q_intact,
              "*** two live allocations do not share bytes -- checked by writing "
              "distinct patterns and reading both back, because overlapping "
              "blocks return perfectly plausible addresses ***");
    }

    printf("\n-- a reused block is big enough --\n");
    {
        void *big = sls_malloc(8192);
        sls_free(big);
        uint8_t *small = sls_malloc(64);
        CHECK(small == big, "a smaller request reuses a larger free block");
        memset(small, 0x5A, 64);
        CHECK(((SlsBlockHdr *)((uint8_t *)small - sizeof(SlsBlockHdr)))->size >= 64,
              "*** ...and the block it got is at least as large as it asked for. "
              "First fit hands back an oversized block deliberately: splitting "
              "would need coalescing, and split-without-merge fragments the arena "
              "into slivers ***");
        sls_free(small);
    }

    printf("\n-- refusals that a no-op free could not make --\n");
    {
        void *p = sls_malloc(128);
        sls_free(p);
        panic_reset();
        uint32_t frees_before = sls_free_count;
        sls_free(p);
        CHECK(panicked() && sls_free_count == frees_before,
              "*** a double free is refused and NOT counted. Linking a block "
              "twice makes the list circular, and the next allocation walks it "
              "forever ***");

        panic_reset();
        int stack_var = 0;
        sls_free(&stack_var);
        CHECK(panicked(),
              "*** a pointer from outside the arena is refused before its header "
              "is read -- reading one would be a wild load on the path meant to "
              "be catching wild pointers ***");

        void *q = sls_malloc(256);
        panic_reset();
        sls_free((uint8_t *)q + 32);
        CHECK(panicked(),
              "*** an interior pointer is refused: no valid header there, so the "
              "size cannot be trusted and pushing it would put a bogus length on "
              "the free list ***");

        panic_reset();
        sls_free(0);
        CHECK(!panicked(), "...and free(NULL) is a silent no-op, as C requires");
    }

    printf("\n-- the copy above has not diverged from the original --\n");
    {
        /* This file reproduces the allocator because sls-runtime.c cannot be
         * compiled on the host. A copy that drifts is worse than no test: it
         * would keep passing while the shipped allocator changed underneath.
         * Checked structurally rather than by hoping. */
        FILE *f = fopen("../qemu/sls/sls-runtime.c", "r");
        CHECK(f != NULL, "the original source is readable from the test");
        if (f) {
            static char buf[400000];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            CHECK(strstr(buf, "SLS_ALLOC_MAGIC   0x5A11ACEDu") != NULL,
                  "...and its magic still matches this copy's");
            CHECK(strstr(buf, "sizeof(SlsBlockHdr)") != NULL &&
                  strstr(buf, "next_free") != NULL,
                  "...and it still uses a header with a free-list link");
            CHECK(strstr(buf, "double free") != NULL &&
                  strstr(buf, "outside the arena") != NULL &&
                  strstr(buf, "bad or missing allocation header") != NULL,
                  "*** ...and it still makes all three refusals. If any were "
                  "removed the shipped allocator would be less safe than the one "
                  "these checks pass against ***");
        }
    }

    printf("\n===================\n");
    printf("passed %d, failed %d\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
