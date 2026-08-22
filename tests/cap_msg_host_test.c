/*
 * cap_msg_host_test.c — Seed Kernel Phase 3 verification: the Polyglot Nexus
 * message transport (SEND_CAP / RECV_CAP / ARENA_FREE), driven against the
 * REAL kernel/cap.c and kernel/frame_pool.c (not a reimplementation).
 *
 * docs/AeroSLS-Polyglot-Nexus-Phase3-Design-v0.1.md §2.2-2.3. This test
 * proves the pieces the generated AeroIDL stubs (tools/aeroidl --target c,
 * user/libaerocap/aerosls_cap.h) depend on:
 *   1. a message carries a payload (opaque bytes) plus up to
 *      CAP_MSG_MAX_CAPS MOVED MEM caps, each with its byte-level descriptor
 *   2. payload + caps survive the queue; the receiver installs caps into
 *      fresh slots and can map + read the arena data the sender wrote
 *   3. a reply message travels back with its own payload
 *   4. arena_free drops ONE reference (frames return at refcount 0), the
 *      callee half of the IDL ownership handoff
 *   5. hygiene: object count and arena free-frames return to baseline
 *      (no leaks through the payload pool or the multi-cap holders)
 *
 * Same harness as tests/cap_lifecycle_host_test.c: fake pids (A=100, B=200)
 * switched between calls, a fake page table, and a physical-memory mirror.
 * The weak hooks (cap_current_pid / cap_proc_cr3 / cap_arch_*) are overridden
 * exactly as that test does; everything else is the real kernel code.
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/cap_msg_host_test \
 *       tests/cap_msg_host_test.c kernel/cap.c kernel/frame_pool.c
 *   /tmp/cap_msg_host_test
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs (see cap_lifecycle_host_test.c for the same two) ───────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }

/* ─── Strong overrides of cap.c's weak hooks ─────────────────────────────── */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

static uint64_t g_fake_pml4 = 0x1000;   /* any nonzero "cr3" */
uint64_t cap_proc_cr3(uint32_t pid) { return pid ? g_fake_pml4 : 0; }

#define FAKE_PT_ENTRIES (1u << 20)
static uint64_t g_fake_pt[FAKE_PT_ENTRIES];
#define FAKE_MEM_FRAMES (1u << 16)
static uint8_t g_mem[(size_t)FAKE_MEM_FRAMES * 4096u];

int cap_arch_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr,
                      uint32_t cap_perms) {
    if (!pml4_phys) return -1;
    uint64_t idx = vaddr >> 12;
    if (idx >= FAKE_PT_ENTRIES) return -1;
    if ((paddr >> 12) >= FAKE_MEM_FRAMES) return -1;
    uint64_t flags = 1;
    if (cap_perms & CAP_PERM_W) flags |= 2;
    flags |= 4;
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

int main(void) {
    cap_init();
    const uint32_t A = 100, B = 200;
    uint32_t start_objs = cap_object_count();
    uint32_t start_frames = cap_arena_free_frames();

    /* ── 1. channel bootstrap ──────────────────────────────────────────── */
    g_cur_pid = A;
    uint16_t rd, wr, frd, fwr;
    CHECK(cap_chan_create(A, B, &rd, &wr, &frd, &fwr) == 0,
          "chan_create provisions both endpoints");

    /* ── 2. sender allocates an arena cap, writes payload data ─────────── */
    struct SLSCapDesc descs[CAP_MSG_MAX_CAPS];
    uint16_t mem_a;
    g_cur_pid = A;
    CHECK(cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP,
                          &mem_a) == 0,
          "A allocates a 4 KiB arena cap");
    uint32_t mem_obj = cap_debug_objid(A, mem_a);
    CHECK(cap_map(A, mem_a, 0x40000000ULL, CAP_PERM_R | CAP_PERM_W) == 0,
          "A maps the arena page");
    memcpy(mem_at_paddr(pt_paddr(0x40000000ULL)),
           "PolyglotNexus", 14);            /* A WRITES */

    /* ── 3. SEND_CAP: payload + one moved cap ──────────────────────────── */
    uint8_t req_payload[16];
    for (int i = 0; i < 16; i++) req_payload[i] = (uint8_t)(0xA0 + i);
    descs[0].slot   = mem_a;
    descs[0].offset = 0;
    descs[0].len    = 14;
    descs[0].rights = CAP_PERM_R;
    descs[0].flags  = 1;   /* arena */

    g_cur_pid = A;
    CHECK(cap_send_msg(A, wr, req_payload, sizeof(req_payload), descs, 1,
                       0xCAFE, 0) == 0,
          "A sends a message with payload + 1 MEM cap");
    CHECK(cap_debug_objid(A, mem_a) == 0xFFFFFFFFu,
          "sender's slot freed by the cap move");
    CHECK(cap_debug_refcount(mem_obj) == 1,
          "queued cap keeps refcount 1 (queue entry is the holder)");

    /* ── 4. RECV_CAP: payload + cap installed into a fresh slot ────────── */
    uint8_t rcv_payload[16];
    struct SLSCapDesc got_caps[CAP_MSG_MAX_CAPS];
    uint16_t got_n = 0;
    uint32_t got_tag = 0, got_flags = 0, got_plen = 0;

    g_cur_pid = B;
    CHECK(cap_recv_msg(B, frd, rcv_payload, sizeof(rcv_payload), &got_plen,
                       CAP_MSG_MAX_CAPS, got_caps, &got_n,
                       &got_tag, &got_flags) == 0,
          "B receives the message");
    CHECK(got_plen == sizeof(req_payload), "payload length delivered");
    CHECK(memcmp(rcv_payload, req_payload, sizeof(req_payload)) == 0,
          "payload bytes survive the queue");
    CHECK(got_tag == 0xCAFE, "request tag echoed");
    CHECK(got_n == 1 && got_caps[0].slot != CAP_NONE,
          "one cap installed into B's table");
    CHECK(cap_debug_objid(B, got_caps[0].slot) == mem_obj,
          "installed cap names the same object");
    CHECK(cap_debug_refcount(mem_obj) == 1, "refcount 1 after delivery");
    CHECK(got_caps[0].len == 14 && got_caps[0].flags == 1,
          "descriptor metadata (len/flags) travels with the cap");

    /* B maps and READS the arena data the sender wrote — the zero-copy
     * payload handoff of the design: data lives in the shared arena, only
     * the cap (and a small envelope) crosses the channel. */
    CHECK(cap_map(B, got_caps[0].slot, 0x41000000ULL,
                  CAP_PERM_R | CAP_PERM_W) == 0,
          "B maps the received cap");
    CHECK(memcmp(mem_at_paddr(pt_paddr(0x41000000ULL)),
                 "PolyglotNexus", 14) == 0,
          "★ B reads the arena data A wrote (zero-copy)");

    /* ── 5. reply: B sends its own payload back on the same channel ────── */
    uint8_t reply_payload[8];
    for (int i = 0; i < 8; i++) reply_payload[i] = (uint8_t)(0x50 + i);
    g_cur_pid = B;
    CHECK(cap_send_msg(B, fwr, reply_payload, sizeof(reply_payload), NULL, 0,
                       0xBEEF, 0) == 0,
          "B sends a reply (payload only)");
    CHECK(cap_debug_refcount(mem_obj) == 1,
          "reply with no caps leaves B's cap untouched");

    g_cur_pid = A;
    uint8_t got_reply[8];
    uint32_t reply_len = 0, reply_tag = 0;
    uint16_t reply_caps = 0;
    CHECK(cap_recv_msg(A, rd, got_reply, sizeof(got_reply), &reply_len,
                       CAP_MSG_MAX_CAPS, NULL, &reply_caps,
                       &reply_tag, NULL) == 0,
          "A receives the reply");
    CHECK(memcmp(got_reply, reply_payload, sizeof(reply_payload)) == 0,
          "reply payload bytes survive");
    CHECK(reply_tag == 0xBEEF, "reply tag echoed");
    CHECK(reply_caps == 0, "reply carried no caps");

    /* ── 6. ARENA_FREE: drop one reference, frames return at refcount 0 ── */
    g_cur_pid = B;
    CHECK(cap_arena_free(B, got_caps[0].slot) == 0, "B frees the received cap");
    CHECK(cap_debug_objid(B, got_caps[0].slot) == 0xFFFFFFFFu,
          "freed slot is reusable");
    CHECK(cap_debug_refcount(mem_obj) == 0xFFFFFFFFu,
          "object destroyed at refcount 0");
    CHECK(cap_arena_free_frames() == start_frames,
          "arena frames returned to the pool");

    /* ── 7. multi-cap message + revoke-in-flight drain ─────────────────── */
    uint16_t m2a, m2b;
    g_cur_pid = A;
    CHECK(cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP,
                          &m2a) == 0 &&
          cap_arena_alloc(A, 1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP,
                          &m2b) == 0,
          "A allocates two more arena caps");
    uint32_t obj_a = cap_debug_objid(A, m2a);
    uint32_t obj_b = cap_debug_objid(A, m2b);
    descs[0].slot = m2a; descs[0].offset = 0; descs[0].len = 16;
    descs[0].rights = CAP_PERM_R; descs[0].flags = 1;
    descs[1].slot = m2b; descs[1].offset = 0; descs[1].len = 32;
    descs[1].rights = CAP_PERM_W; descs[1].flags = 2;
    CHECK(cap_send_msg(A, wr, req_payload, 4, descs, 2, 0x77, 0) == 0,
          "A sends a message with TWO caps");
    CHECK(cap_debug_refcount(obj_a) == 1 && cap_debug_refcount(obj_b) == 1,
          "both objects held only by the queue");

    g_cur_pid = B;
    uint16_t got2 = 0;
    struct SLSCapDesc got2c[CAP_MSG_MAX_CAPS];
    CHECK(cap_recv_msg(B, frd, rcv_payload, sizeof(rcv_payload), &got_plen,
                       CAP_MSG_MAX_CAPS, got2c, &got2, &got_tag, NULL) == 0,
          "B receives the two-cap message");
    CHECK(got2 == 2, "both caps installed");
    CHECK(cap_debug_objid(B, got2c[0].slot) == obj_a &&
          cap_debug_objid(B, got2c[1].slot) == obj_b,
          "each installed cap names its object");
    CHECK(got2c[0].len == 16 && got2c[1].len == 32,
          "per-cap descriptor lengths preserved");

    /* B drops one; the other survives → refcounts 0 and 1. */
    CHECK(cap_arena_free(B, got2c[0].slot) == 0, "B frees cap 0");
    CHECK(cap_debug_refcount(obj_a) == 0xFFFFFFFFu, "object 0 destroyed");
    CHECK(cap_debug_refcount(obj_b) == 1, "object 1 survives (refcount 1)");
    CHECK(cap_arena_free(B, got2c[1].slot) == 0, "B frees cap 1");
    CHECK(cap_debug_refcount(obj_b) == 0xFFFFFFFFu, "object 1 destroyed");

    /* ── 8. hygiene: everything back to baseline ───────────────────────── */
    CHECK(cap_object_count() == start_objs + 1,   /* the channel object */
          "object count back to baseline (+ the channel)");
    CHECK(cap_arena_free_frames() == start_frames,
          "all arena frames returned (no payload-pool or holder leaks)");

    printf("\n%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
