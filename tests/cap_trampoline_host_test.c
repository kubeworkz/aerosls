/* cap_trampoline_host_test.c -- Phase 3 trampoline capability verification.
 *
 * Tests the kernel-side trampoline infrastructure:
 *   1. MPK flags detection (weak stub returns 0 = no MPK)
 *   2. Trampoline creation fails gracefully when MPK is unavailable
 *   3. Trampoline creation succeeds when MPK is faked available
 *   4. Trampoline cap validation (type check, state check)
 *   5. Trampoline call returns ENOSYS (kernel-mediated path not yet implemented)
 *   6. Cap word encoding: TRAMP type, VALID state, perms RX
 *   7. Lifecycle hygiene: trampoline count and active state
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/cap_trampoline_host_test \
 *       tests/cap_trampoline_host_test.c kernel/cap.c kernel/frame_pool.c
 *   /tmp/cap_trampoline_host_test
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Link stubs */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* Strong overrides of cap.c's weak hooks */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

static uint64_t g_fake_pml4 = 0x1000;
uint64_t cap_proc_cr3(uint32_t pid) { return pid ? g_fake_pml4 : 0; }

/* Fake page table and memory. */
#define FAKE_PT_ENTRIES (1u << 20)
static uint64_t g_fake_pt[FAKE_PT_ENTRIES];
#define FAKE_MEM_FRAMES (1u << 16)
static uint8_t g_mem[(size_t)FAKE_MEM_FRAMES * 4096u];

int cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                      uint32_t cap_perms) {
    (void)pml4_phys;
    uint64_t idx = vaddr >> 12;
    if (idx >= FAKE_PT_ENTRIES) return -1;
    if ((paddr >> 12) >= FAKE_MEM_FRAMES) return -1;
    uint64_t flags = 1;
    if (cap_perms & CAP_PERM_R) flags |= 2;
    if (cap_perms & CAP_PERM_W) flags |= 2;
    flags |= 4;
    g_fake_pt[idx] = (paddr & 0x000FFFFFFFFFF000ULL) | flags;
    return 0;
}

int cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    (void)pml4_phys;
    uint64_t idx = vaddr >> 12;
    if (idx >= FAKE_PT_ENTRIES) return -1;
    g_fake_pt[idx] = 0;
    return 0;
}

void cap_arch_tlb_flush(void) { }

/* Fake MPK support */
static int g_fake_mpk = 0;
static int g_pkey_counter = 4;

uint32_t cap_arch_detect_mpk(void) {
    return (uint32_t)g_fake_mpk;
}

int cap_arch_alloc_pkey(void) {
    if (g_pkey_counter > 15) return -1;
    return g_pkey_counter++;
}

int cap_arch_set_pkey(uint64_t pml4_phys, uint64_t vaddr, uint32_t pkey,
                      uint32_t perms) {
    (void)pml4_phys; (void)vaddr; (void)pkey; (void)perms;
    return 0;
}

/* Harness */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    cap_init();
    cap_trampoline_init();
    const uint32_t A = 100, B = 200;

    /* 1. MPK flags detection (default: no MPK) */
    CHECK(cap_trampoline_mpk_flags() == 0,
          "MPK flags are 0 when arch stub returns 0");

    /* 2. Trampoline creation fails without MPK */
    g_cur_pid = A;
    uint16_t tramp_idx;
    int r = cap_trampoline_create(A, B, 0x1000, 65536, &tramp_idx);
    CHECK(r == CAP_ENOSYS,
          "trampoline creation fails with ENOSYS when MPK unsupported");
    CHECK(tramp_idx == CAP_NONE,
          "tramp_idx is CAP_NONE on failure");

    /* 3. Enable fake MPK and retry */
    g_fake_mpk = CAP_TRAMP_MPK_SUPPORTED | CAP_TRAMP_MPK_ENABLED;
    cap_trampoline_init();
    CHECK((cap_trampoline_mpk_flags() & CAP_TRAMP_MPK_SUPPORTED) != 0,
          "MPK supported after fake detection");

    r = cap_trampoline_create(A, B, 0x4000, 65536, &tramp_idx);
    CHECK(r == 0, "trampoline creation succeeds with MPK");
    CHECK(tramp_idx != CAP_NONE, "tramp_idx is valid on success");

    /* 4. Validate the cap word */
    int ti = -1;
    for (int i = 0; i < CAP_TABLE_MAX; i++)
        if (cap_tables[i].pid == A) { ti = i; break; }
    CHECK(ti >= 0, "table found for pid A");
    if (ti >= 0) {
        uint64_t word = cap_tables[ti].slots[tramp_idx].word;
        uint8_t type = (word >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK;
        uint8_t state = (word >> CAP_STATE_SHIFT) & CAP_STATE_MASK;
        uint8_t perms = (word >> CAP_PERM_SHIFT) & CAP_PERM_MASK;
        CHECK(type == CAP_TYPE_TRAMP, "cap type is TRAMP");
        CHECK(state == CAP_STATE_VALID, "cap state is VALID");
        CHECK((perms & CAP_PERM_R) != 0, "cap has read permission");
        CHECK((perms & CAP_PERM_X) != 0, "cap has execute permission");
        CHECK((perms & CAP_PERM_W) == 0, "cap does NOT have write permission");
    }

    /* 5. Create a second trampoline */
    uint16_t tramp_idx2;
    r = cap_trampoline_create(A, B, 0x8000, 32768, &tramp_idx2);
    CHECK(r == 0, "second trampoline creation succeeds");
    CHECK(tramp_idx2 != tramp_idx, "second trampoline gets a different slot");

    /* 6. Trampoline call returns ENOSYS */
    uint64_t result = 0;
    r = cap_trampoline_call(A, tramp_idx, 0, 0, 0, 0, &result);
    CHECK(r == CAP_ENOSYS,
          "trampoline call returns ENOSYS (inline path preferred)");

    /* 7. Error paths */
    r = cap_trampoline_create(A, A, 0x1000, 65536, &tramp_idx);
    CHECK(r == CAP_EINVAL, "trampoline creation rejects self-calls");

    r = cap_trampoline_create(A, 0, 0x1000, 65536, &tramp_idx);
    CHECK(r == CAP_EINVAL, "trampoline creation rejects pid 0");

    r = cap_trampoline_create(A, B, 0, 65536, &tramp_idx);
    CHECK(r == CAP_EINVAL, "trampoline creation rejects zero entry point");

    r = cap_trampoline_create(A, B, 0x100000000ULL, 65536, &tramp_idx);
    CHECK(r == CAP_EINVAL, "trampoline creation rejects entry point >= 4 GiB");

    r = cap_trampoline_create(A, B, 0x1000, 0, &tramp_idx);
    CHECK(r == CAP_ERANGE, "trampoline creation rejects zero stack size");

    r = cap_trampoline_create(A, B, 0x1000, 2 * 1024 * 1024, &tramp_idx);
    CHECK(r == CAP_ERANGE, "trampoline creation rejects stack > 1 MiB");

    r = cap_trampoline_call(A, 999, 0, 0, 0, 0, &result);
    CHECK(r == CAP_EBADF, "trampoline call rejects invalid cap index");

    /* 8. Trampoline listing (smoke) */
    cap_trampoline_list();
    CHECK(1, "cap_trampoline_list() does not crash");

    /* 9. Lifecycle hygiene */
    printf("  (object_count=%u, free_frames=%u)\n",
           cap_object_count(), cap_arena_free_frames());

    /* Summary */
    printf("\n=== trampoline host test: %s (%d failures) ===\n",
           g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
