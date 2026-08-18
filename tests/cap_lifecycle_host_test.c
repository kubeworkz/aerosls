/*
 * cap_lifecycle_host_test.c — Seed Kernel Phase 1 verification: the full
 * capability lifecycle, driven against the REAL kernel/cap.c and
 * kernel/frame_pool.c (not a reimplementation).
 *
 * The test fakes the three things cap.c deliberately keeps behind weak
 * hooks: the "current pid" (two fake processes, A=100 and B=200, switched
 * between calls), the process's cr3 (a fake page table), and the arch
 * map/unmap/flush primitives (which record PTEs in the fake table and
 * mirror physical memory in a host buffer). Everything else — the cap
 * words, tables, holders, refcounts, arena bitmap, channels, queue
 * protocol, revocation — is the real kernel code.
 *
 * Scenarios:
 *   1. channel bootstrap (both endpoints provisioned, far end into B)
 *   2. create → transfer → map → write → return-trip → read (the design
 *      doc's §8 acceptance test, minus the scheduler: send is a move)
 *   3. permission-escalation refusal, forged-word rejection
 *   4. immediate, total revocation (PTEs torn down in BOTH processes,
 *      refcount 0, arena frames returned)
 *   5. queue-full / empty-queue / wrong-end error paths
 *   6. map-address conflicts and unmap
 *   7. cap_create_mem range validation (positive and negative)
 *   8. lifecycle hygiene: object count and arena free-frames return to
 *      their starting values (no leaks)
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/cap_lifecycle_host_test \
 *       tests/cap_lifecycle_host_test.c kernel/cap.c kernel/frame_pool.c
 *   /tmp/cap_lifecycle_host_test
 *
 * -no-pie is load-bearing: cap_create_mem() rejects ranges overlapping the
 * kernel image [0x100000, _kernel_image_end). In a PIE host binary that
 * symbol sits at a 47-bit address, so EVERY range below 4 GiB looks like it
 * overlaps the image; -no-pie puts it at ~0x40xxxx, below the 256 MiB range
 * the positive cap_create_mem() case uses. The real kernel's image end is
 * ~120 MiB, so the check is sane there either way.
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"   /* stack_bottom/stack_top (frame_pool_init reservation bounds) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs (see frame_pool_host_test.c for the same two) ───────────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
/* stack_bottom/stack_top come from tests/process_host_stubs.h (included
 * above) — the frame_pool_init() reservation bounds. That header's process
 * stubs (per_cpu_data, do_syscall, syscall_return_path, the teardown trio)
 * are inert in this TU: it links cap.c/frame_pool.c and never includes
 * process.c, so nothing here references them. */

/* ─── Strong overrides of cap.c's weak hooks ─────────────────────────────── */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

static uint64_t g_fake_pml4 = 0x1000;   /* any nonzero "cr3" */
uint64_t cap_proc_cr3(uint32_t pid) { return pid ? g_fake_pml4 : 0; }

/* Fake page table: one 64-bit PTE per (vaddr >> 12). */
#define FAKE_PT_ENTRIES (1u << 20)
static uint64_t g_fake_pt[FAKE_PT_ENTRIES];

/* Fake physical memory mirror, one 4 KiB frame per (paddr >> 12). The arena
 * in this host build starts at frame 512 (2 MiB, see cap_arena_init /
 * frame_pool_reserve_contiguous on an empty bitmap), so frame indices stay
 * well inside this buffer. */
#define FAKE_MEM_FRAMES (1u << 16)
static uint8_t g_mem[(size_t)FAKE_MEM_FRAMES * 4096u];

int cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                      uint32_t cap_perms) {
    if (!pml4_phys) return -1;
    uint64_t idx = vaddr >> 12;
    if (idx >= FAKE_PT_ENTRIES) return -1;
    if ((paddr >> 12) >= FAKE_MEM_FRAMES) return -1;
    uint64_t flags = 1;                       /* PRESENT */
    if (cap_perms & CAP_PERM_W) flags |= 2;   /* WRITE */
    flags |= 4;                               /* USER */
    g_fake_pt[idx] = (paddr & 0x000FFFFFFFFFF000ULL) | flags;
    return 0;
}

int cap_arch_unmap_page(uint64_t pml4_phys, uint64_t vaddr) {
    if (!pml4_phys) return -1;
    uint64_t idx = vaddr >> 12;
    if (idx >= FAKE_PT_ENTRIES) return -1;
    g_fake_pt[idx] = 0;
    return 0;
}

void cap_arch_tlb_flush(void) { }

static uint64_t pt_paddr(uint64_t vaddr) {
    return g_fake_pt[vaddr >> 12] & 0x000FFFFFFFFFF000ULL;
}
static uint8_t* mem_at_paddr(uint64_t paddr) {
    return &g_mem[(paddr >> 12) * 4096u];
}

/* ─── Harness ─────────────────────────────────────────────────────────────── */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

/* Table index for pid, mirroring cap.c's internal lookup via .pid. */
static int table_for_pid(uint32_t pid) {
    for (int i = 0; i < CAP_TABLE_MAX; i++)
        if (cap_tables[i].pid == pid) return i;
    return -1;
}

/* A free slot is one whose TYPE field is NONE — its word is NOT zero:
 * free slots chain the freelist through the object-id field, so "word == 0"
 * only ever means "last slot in the freelist". */
static uint16_t first_free_slot(uint32_t pid) {
    int ti = table_for_pid(pid);
    if (ti < 0) return CAP_NONE;
    for (int i = 0; i < CAP_TABLE_ENTRIES; i++)
        if (((cap_tables[ti].slots[i].word >> CAP_TYPE_SHIFT) & CAP_TYPE_MASK)
            == CAP_TYPE_NONE)
            return (uint16_t)i;
    return CAP_NONE;
}

int main(void) {
    cap_init();
    const uint32_t A = 100, B = 200;
    uint64_t cookie = 0;
    uint16_t got = CAP_NONE;

    CHECK(cap_arena_free_frames() == CAP_ARENA_FRAMES,
          "arena starts fully free");
    CHECK(cap_object_count() == 0, "no objects before any create");

    /* ── 1. channel bootstrap: both endpoints provisioned ──────────────── */
    g_cur_pid = A;
    uint16_t rd1, wr1, frd1, fwr1;
    CHECK(cap_chan_create(A, B, &rd1, &wr1, &frd1, &fwr1) == 0,
          "chan_create provisions both endpoints");
    CHECK(rd1 != CAP_NONE && wr1 != CAP_NONE && frd1 != CAP_NONE && fwr1 != CAP_NONE,
          "four endpoint caps minted");
    CHECK(cap_debug_objid(A, rd1) == cap_debug_objid(B, frd1),
          "both ends reference the same channel object");
    CHECK(cap_debug_refcount(cap_debug_objid(A, rd1)) == 4,
          "channel object starts with 4 holders");

    /* ── 2. the lifecycle: create → transfer → map → write → read ──────── */
    g_cur_pid = A;
    uint16_t mem_a;
    CHECK(cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP, &mem_a) == 0,
          "A allocates a 4 KiB MEM cap from the arena");
    uint32_t mem_obj = cap_debug_objid(A, mem_a);
    CHECK(mem_obj != 0xFFFFFFFFu, "MEM cap has a valid object id");
    CHECK(cap_debug_refcount(mem_obj) == 1, "fresh MEM object has refcount 1");

    CHECK(cap_send(A, wr1, mem_a, 0x1111) == 0, "A sends the MEM cap (move)");
    CHECK(cap_debug_objid(A, mem_a) == 0xFFFFFFFFu,
          "sender's slot is FREE after the move");
    CHECK(cap_debug_refcount(mem_obj) == 1,
          "queued cap keeps refcount 1 (the queue entry is the holder)");

    g_cur_pid = B;
    uint16_t mem_b;
    CHECK(cap_recv(B, frd1, 0, &cookie, &mem_b) == 0, "B receives the message");
    CHECK(cookie == 0x1111, "cookie travels with the cap");
    CHECK(mem_b != CAP_NONE, "B received a cap index");
    CHECK(cap_debug_objid(B, mem_b) == mem_obj, "B's cap names the same object");
    CHECK(cap_debug_refcount(mem_obj) == 1, "refcount 1 after delivery");

    /* ── 3. no permission escalation ────────────────────────────────────── */
    CHECK(cap_map(B, mem_b, 0x40000000ULL, CAP_PERM_W | CAP_PERM_X) == CAP_EINVAL,
          "map cannot escalate beyond the cap's perms (X not held)");
    CHECK(cap_map(B, mem_b, 0x40001000ULL, CAP_PERM_MAP) == CAP_EINVAL,
          "CAP_PERM_MAP alone is not a mapping permission");
    CHECK(cap_map(B, mem_b, 0x40001001ULL, CAP_PERM_R) == CAP_EINVAL,
          "misaligned vaddr rejected");

    CHECK(cap_map(B, mem_b, 0x40000000ULL, CAP_PERM_R | CAP_PERM_W) == 0,
          "B maps the page RW");
    uint64_t phys = pt_paddr(0x40000000ULL);
    CHECK(phys != 0, "PTE installed");
    memcpy(mem_at_paddr(phys), "Hello", 6);          /* B WRITES */

    CHECK(cap_send(B, fwr1, mem_b, 0x2222) == 0, "B sends the MEM cap back");
    CHECK(cap_debug_objid(B, mem_b) == 0xFFFFFFFFu, "B's slot freed on move");

    g_cur_pid = A;
    uint16_t mem_back;
    CHECK(cap_recv(A, rd1, 0, &cookie, &mem_back) == 0, "A receives the cap back");
    CHECK(cookie == 0x2222, "return-trip cookie correct");
    CHECK(cap_map(A, mem_back, 0x50000000ULL, CAP_PERM_R | CAP_PERM_W) == 0,
          "A maps the same physical page");
    CHECK(pt_paddr(0x50000000ULL) == phys, "A's PTE targets the same frame");
    CHECK(memcmp(mem_at_paddr(pt_paddr(0x50000000ULL)), "Hello", 6) == 0,
          "★ A READS what B wrote");

    /* Mappings persist across the cap move (seL4/EROS map-cap semantics:
     * only unmap or revoke tears them down). */
    CHECK(pt_paddr(0x40000000ULL) != 0,
          "B's mapping survives the move (until unmap/revoke)");

    /* ── 4. immediate, total revocation ─────────────────────────────────── */
    CHECK(cap_revoke(A, mem_back) == 0, "A revokes the MEM object");
    CHECK(cap_debug_refcount(mem_obj) == 0xFFFFFFFFu,
          "object destroyed at refcount 0");
    CHECK(pt_paddr(0x50000000ULL) == 0, "A's PTE torn down by revoke");
    CHECK(pt_paddr(0x40000000ULL) == 0, "B's PTE torn down by revoke");
    CHECK(cap_arena_free_frames() == CAP_ARENA_FRAMES, "arena frames returned");
    CHECK(cap_revoke(A, mem_back) == CAP_EINVAL,
          "revoking a freed slot fails cleanly (-EINVAL, not a crash)");

    /* ── 5. error paths ─────────────────────────────────────────────────── */
    g_cur_pid = B;
    CHECK(cap_recv(B, frd1, 1, &cookie, &got) == CAP_EAGAIN,
          "Phase-1 recv is non-blocking: empty queue → EAGAIN (block ignored)");
    CHECK(cap_send(B, frd1, CAP_NONE, 0) == CAP_EBADF,
          "a CHAN_R is not a write end (-EBADF)");
    CHECK(cap_send(B, fwr1, first_free_slot(B), 0) == CAP_EINVAL,
          "sending a free slot as payload is refused");

    /* A MEM cap is not a channel end, in either role. */
    g_cur_pid = A;
    {
        uint16_t tmp;
        CHECK(cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_MAP, &tmp) == 0,
              "temporary MEM cap for the misuse test");
        CHECK(cap_send(A, tmp, CAP_NONE, 0) == CAP_EBADF,
              "a MEM cap cannot be used as a write end");
        CHECK(cap_revoke(A, tmp) == 0, "revoke the temporary MEM cap");
    }

    /* Forged word: real type + object id, but reserved bits set. The slot's
     * original word is saved and restored — it was a freelist member, and
     * clobbering the chain would corrupt the table. */
    g_cur_pid = B;
    {
        int bti = table_for_pid(B);
        uint16_t fslot = first_free_slot(B);
        uint64_t saved = cap_tables[bti].slots[fslot].word;
        uint64_t forged =
            (1ULL << CAP_TYPE_SHIFT) | ((uint64_t)mem_obj << CAP_OBJ_SHIFT) |
            CAP_RSVD_MASK;
        cap_tables[bti].slots[fslot].word = forged;
        int r = cap_send(B, fwr1, fslot, 0);
        cap_tables[bti].slots[fslot].word = saved;
        CHECK(r == CAP_EINVAL,
              "forged word (reserved bits set) rejected on send");
    }

    /* Queue-full: 16 plain messages fill the A→B direction; the 17th fails
     * with nothing mutated. The read cursor is not at the ring start (the
     * earlier MEM transfer advanced it), so drain until empty and check the
     * SET of cookies rather than assuming ring position. */
    g_cur_pid = A;
    for (int i = 0; i < 16; i++)
        CHECK(cap_send(A, wr1, CAP_NONE, (uint64_t)(100 + i)) == 0,
              "plain message fills queue");
    CHECK(cap_send(A, wr1, CAP_NONE, 999) == CAP_EAGAIN,
          "queue full → EAGAIN, no message lost");
    {
        int seen[16] = {0};
        int count = 0;
        g_cur_pid = B;
        for (int i = 0; i < 16; i++) {
            got = CAP_NONE; cookie = 0;
            int r = cap_recv(B, frd1, 0, &cookie, &got);
            CHECK(r == 0 && got == CAP_NONE &&
                  cookie >= 100 && cookie <= 115 &&
                  !seen[cookie - 100],
                  "drained one queued message, no cap payload, no duplicates");
            if (r == 0 && got == CAP_NONE && cookie >= 100 && cookie <= 115)
                seen[cookie - 100] = 1, count++;
        }
        CHECK(count == 16, "all 16 queued messages drained exactly once");
        got = CAP_NONE;
        CHECK(cap_recv(B, frd1, 0, &cookie, &got) == CAP_EAGAIN,
              "queue empty again after draining");
    }

    /* ── 6. map-address conflicts and unmap ─────────────────────────────── */
    g_cur_pid = A;
    uint16_t mem2, mem3;
    CHECK(cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_MAP, &mem2) == 0,
          "A allocates a second MEM cap (R|MAP only)");
    CHECK(cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_MAP, &mem3) == 0,
          "A allocates a third MEM cap");
    CHECK(cap_map(A, mem3, 0x60000000ULL, CAP_PERM_R) == 0,
          "map mem3 at 0x60000000");
    CHECK(cap_map(A, mem2, 0x60000000ULL, CAP_PERM_R) == CAP_ECONFLICT,
          "different object at the same vaddr → conflict");
    CHECK(cap_unmap(A, mem3, 0x60000000ULL) == 0, "unmap mem3");
    CHECK(cap_map(A, mem2, 0x60000000ULL, CAP_PERM_R) == 0,
          "vaddr reusable after unmap");
    CHECK(cap_unmap(A, mem2, 0x60000000ULL) == 0, "unmap mem2");
    CHECK(cap_revoke(A, mem2) == 0, "revoke mem2");
    CHECK(cap_revoke(A, mem3) == 0, "revoke mem3");

    /* ── 7. cap_create_mem range validation ─────────────────────────────── */
    uint16_t out;
    CHECK(cap_create_mem(A, 0x1000, 1, CAP_PERM_R | CAP_PERM_MAP, &out) == CAP_ECONFLICT,
          "create_mem below 1 MiB (machine-reserved low memory) rejected");
    CHECK(cap_create_mem(A, 0x100000, 1, CAP_PERM_R | CAP_PERM_MAP, &out) == CAP_ECONFLICT,
          "create_mem overlapping the kernel image start rejected");
    CHECK(cap_create_mem(A, 0x200000, 1, CAP_PERM_R | CAP_PERM_MAP, &out) == CAP_ECONFLICT,
          "create_mem overlapping the arena rejected");
    CHECK(cap_create_mem(A, 0x10000000ULL /* 256 MiB, above arena+image */, 1,
                         CAP_PERM_R | CAP_PERM_MAP, &out) == 0 &&
          out != CAP_NONE,
          "create_mem on a legal high range succeeds");
    if (out != CAP_NONE)
        CHECK(cap_revoke(A, out) == 0, "revoke the create_mem cap");

    /* ── 8. lifecycle hygiene: everything back to baseline ──────────────── */
    CHECK(cap_object_count() == 1, "only the channel object remains (no leaks)");
    CHECK(cap_arena_free_frames() == CAP_ARENA_FRAMES,
          "arena is fully free again (no frame leaks)");
    CHECK(cap_debug_refcount(cap_debug_objid(A, rd1)) == 4,
          "channel object still has its 4 endpoint holders");

    if (g_fail == 0) printf("\nALL PASS\n");
    else             printf("\n%d FAILURE(S)\n", g_fail);
    return g_fail != 0;
}
