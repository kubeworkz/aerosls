/*
 * irq_bind_host_test.c — Driver SDK ABI v0.1 §4.3: CAP_TYPE_IRQ with
 * SYS_IRQ_BIND (316), driven against the REAL kernel/cap.c and
 * kernel/chan.c (not a reimplementation).
 *
 * The test mints single-use IRQ cap words directly into a fake process's
 * cap table (IRQ caps come from the manifest `devices:` section at
 * create_sidecar, which is M2 work), then drives k_irq_bind through the
 * REAL channel creation, holder bookkeeping, registry install, and
 * single-use stamp. The ISR side is exercised by calling cap_irq_notify
 * directly (the arch ISR stub's job is simply to invoke it). Since the ISR
 * deferral, cap_irq_notify only self-masks + latches the pending bit + EOIs
 * — it must never take a cap lock. Delivery (the lock-taking enqueue +
 * wake) happens in process context via cap_irq_drain_pending, so the test
 * calls the drainer after each notify to prove the full latch → enqueue →
 * wake → k_chan_recv path the driver sees. cap_wake_chan and cap_irq_eoi
 * are overridden to record.
 *
 * Scenarios:
 *   1. bind a vector: returns a CHAN_R slot; k_cap_info shows CHAN_R;
 *      the IRQ cap word is stamped REVOKED (single-use)
 *   2. second bind of the same slot → CAP_ERR_REVOKED
 *   3. second IRQ cap with the SAME vector → CAP_ERR_STATE (already bound)
 *   4. ISR path: cap_irq_notify latches, cap_irq_drain_pending delivers;
 *      k_chan_recv yields
 *      tag = vector, payload byte = vector; queue empties after one recv
 *   5. notify on an unbound vector is a silent no-op (no wake, no crash)
 *   6. rights: IRQ cap without CAP_PERM_BIND → CAP_ERR_RIGHTS
 *   7. type: MEM cap in the slot → CAP_ERR_TYPE; revoked slot →
 *      CAP_ERR_REVOKED; bad slot → CAP_ERR_RANGE
 *   8. vector out of range (>= 256) → CAP_ERR_RANGE; budget > depth →
 *      CAP_ERR_RANGE
 *   9. syscall wrapper: NULL → CAP_ERR_PROTO; success fills out_chan_r
 *  10. teardown releases the vector: after cap_table_teardown(pid), a
 *  11. SYS_IRQ_UNBIND (317): bind a vector, unbind it by its CHAN_R slot
 *      - the registry is disarmed (notify no longer wakes), the driver's
 *      channel is closed (recv fails CAP_ERR_STATE), and the SAME vector
 *      binds again with a fresh IRQ cap
 *  12. unbind errors: bad slot CAP_ERR_RANGE, revoked slot
 *      CAP_ERR_REVOKED, non-CHAN_R cap CAP_ERR_TYPE, non-IRQ channel
 *      CAP_ERR_STATE (a plain chan_create channel), double-unbind
 *      CAP_ERR_STATE once already released, NULL request CAP_ERR_PROTO
 *      fresh IRQ cap on the same vector binds again
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/irq_bind_host_test \
 *       tests/irq_bind_host_test.c kernel/cap.c kernel/chan.c kernel/frame_pool.c
 *   /tmp/irq_bind_host_test
 */
#include "kernel/cap.h"
#include "tests/process_host_stubs.h"   /* stack_bottom/stack_top (frame_pool_init reservation bounds) */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ─── Link stubs ──────────────────────────────────────────────────────────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_putchar(char c) { (void)c; }

volatile uint64_t kernel_tick_counter = 0;

/* Link stubs for cap.c's sidecar-spawn path (not exercised here). */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;
uint32_t alloc_pid(void) { return 903; }
uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    (void)partition_id;
    return 0x400000007000ULL;
}
uint64_t user_clone_page_table(void) { return 0x4000; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages,
                      uint64_t flags) {
    (void)pml4; (void)phys; (void)npages; (void)flags;
    return 0;
}
void cap_arch_tlb_flush(void) { }
int cap_arch_map_page(uint64_t pml4, uint64_t v, uint64_t p, uint32_t perms) {
    (void)pml4; (void)v; (void)p; (void)perms; return 0;
}
int cap_arch_unmap_page(uint64_t pml4, uint64_t v) {
    (void)pml4; (void)v; return 0;
}

/* ─── Strong overrides of cap.c's weak hooks ─────────────────────────────── */
static uint32_t g_cur_pid = 0;
uint32_t cap_current_pid(void) { return g_cur_pid; }

static int g_wakes = 0;
static uint32_t g_wake_chan = 0;
void cap_wake_chan(uint32_t chan_id) {
    g_wakes++;
    g_wake_chan = chan_id;
}

static int g_eois = 0;
static uint32_t g_eoi_vector = 0;
void cap_irq_eoi(uint32_t vector) {
    g_eois++;
    g_eoi_vector = vector;
}

/* cap_table_index (kernel/cap.c): not declared in cap.h (internal only). */
int cap_table_index(uint32_t pid);
int  k_irq_unbind(uint32_t pid, uint16_t chan_r, uint16_t* out_vector);
int  k_irq_mask(uint32_t pid, uint16_t chan_r, uint8_t mask);

/* ─── Cap word construction ──────────────────────────────────────────────── */
static uint64_t irq_word(uint32_t vector, uint8_t perm) {
    return ((uint64_t)CAP_TYPE_IRQ << CAP_TYPE_SHIFT) |
           ((uint64_t)vector     << CAP_OBJ_SHIFT) |
           ((uint64_t)perm       << CAP_PERM_SHIFT);
}

static void mint_word(uint32_t pid, uint16_t slot, uint64_t word) {
    int ti = cap_table_index(pid);
    if (ti < 0) { fprintf(stderr, "FATAL: no cap table for pid %u\n", pid); }
    cap_tables[ti].slots[slot].word = word;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
    printf("ok: %s\n", msg); \
} while (0)

int main(void) {
    cap_init();
    const uint32_t P = 904;
    g_cur_pid = P;
    g_wakes = g_eois = 0;

    /* ── 1. bind a vector ─────────────────────────────────────────────── */
    uint16_t irq1 = 4;                       /* slot 4 holds the IRQ cap */
    mint_word(P, irq1, irq_word(33 /*vector*/, CAP_PERM_BIND));
    uint16_t chan_r = CAP_NONE;
    CHECK(k_irq_bind(P, irq1, 0, &chan_r) == CAP_ERR_OK, "bind vector 33 succeeds");
    CHECK(chan_r != CAP_NONE && chan_r != irq1, "bind returned a fresh CHAN_R slot");

    struct SLSCapInfoOut info;
    memset(&info, 0, sizeof(info));
    CHECK(k_cap_info(P, chan_r, &info) == CAP_ERR_OK, "k_cap_info on the new slot ok");
    CHECK(info.ty == CAP_TYPE_CHAN_R && (info.rights & CAP_PERM_RECV),
          "new slot is a CHAN_R with RECV right");
    CHECK(k_cap_info(P, irq1, &info) == CAP_ERR_REVOKED,
          "IRQ cap now introspects as REVOKED (single-use stamp)");

    /* ── 2. single-use: the IRQ cap is stamped REVOKED ────────────────── */
    uint16_t again = CAP_NONE;
    CHECK(k_irq_bind(P, irq1, 0, &again) == CAP_ERR_REVOKED,
          "second bind of the same slot is CAP_ERR_REVOKED (single-use)");

    /* ── 3. same vector on a DIFFERENT cap is already bound ───────────── */
    uint16_t irq2 = 5;
    mint_word(P, irq2, irq_word(33, CAP_PERM_BIND));
    uint16_t dup = CAP_NONE;
    CHECK(k_irq_bind(P, irq2, 0, &dup) == CAP_ERR_STATE,
          "second cap on the same vector is CAP_ERR_STATE");

    /* ── 4. ISR path: notify → latch, drain → wake → recv ─────────────── */
    g_wakes = 0;
    cap_irq_notify(33);
    CHECK(g_wakes == 0, "notify only latches (no wake from ISR context)");
    CHECK(g_eois == 1 && g_eoi_vector == 33, "EOI ran for the fired vector");
    cap_irq_drain_pending();
    CHECK(g_wakes == 1, "drain delivered the latched notify (one wake)");

    uint8_t buf[16];
    struct SLSChanRecvOut out;
    memset(&out, 0, sizeof(out));
    CHECK(k_chan_recv(P, chan_r, buf, sizeof(buf), NULL, 0, &out) == CAP_ERR_OK,
          "k_chan_recv drains the notification");
    CHECK(out.kind == CH_KIND_MSG, "notification arrives as a message");
    CHECK(out.tag == 33, "message tag is the vector number");
    CHECK(out.len == 1 && buf[0] == 33, "payload is the one-byte vector");

    /* a second recv finds nothing (the transport reports the empty state
     * so the driver parks with k_chan_wait — the documented pattern) */
    CHECK(k_chan_recv(P, chan_r, buf, sizeof(buf), NULL, 0, &out) == CAP_ERR_STATE,
          "queue is empty after one recv (CAP_ERR_STATE → driver parks)");

    /* ── 5. notify on an unbound vector is a silent no-op ─────────────── */
    g_wakes = 0;
    cap_irq_notify(77);
    cap_irq_drain_pending();
    CHECK(g_wakes == 0, "notify on an unbound vector does not wake");
    CHECK(g_eois == 2, "unbound notify still EOIs (harmless)");

    /* ── 6. rights ────────────────────────────────────────────────────── */
    uint16_t no_perm = 6;
    mint_word(P, no_perm, irq_word(34, 0));
    uint16_t r6 = CAP_NONE;
    CHECK(k_irq_bind(P, no_perm, 0, &r6) == CAP_ERR_RIGHTS,
          "IRQ cap without bind perm is CAP_ERR_RIGHTS");

    /* ── 7. type / revoked / slot errors ──────────────────────────────── */
    uint16_t mem_slot = 7;
    mint_word(P, mem_slot, ((uint64_t)CAP_TYPE_MEM << CAP_TYPE_SHIFT) |
                           ((uint64_t)1 << CAP_OBJ_SHIFT) |
                           ((uint64_t)(CAP_PERM_R | CAP_PERM_W) << CAP_PERM_SHIFT));
    uint16_t r7 = CAP_NONE;
    CHECK(k_irq_bind(P, mem_slot, 0, &r7) == CAP_ERR_TYPE,
          "MEM cap in the slot is CAP_ERR_TYPE");
    uint16_t dead = 8;
    cap_tables[cap_table_index(P)].slots[dead].word = 0;
    uint16_t r8 = CAP_NONE;
    CHECK(k_irq_bind(P, dead, 0, &r8) == CAP_ERR_REVOKED,
          "empty (free) slot is CAP_ERR_REVOKED");
    uint16_t r9 = CAP_NONE;
    CHECK(k_irq_bind(P, 3000, 0, &r9) == CAP_ERR_RANGE,
          "slot beyond CAP_TABLE_ENTRIES is CAP_ERR_RANGE");
    CHECK(k_irq_bind(P, irq1, 0, NULL) == CAP_ERR_PROTO,
          "NULL out pointer is CAP_ERR_PROTO");

    /* ── 8. vector / budget range ─────────────────────────────────────── */
    uint16_t far_vec = 9;
    mint_word(P, far_vec, irq_word(999 /* >= 256 */, CAP_PERM_BIND));
    uint16_t r10 = CAP_NONE;
    CHECK(k_irq_bind(P, far_vec, 0, &r10) == CAP_ERR_RANGE,
          "vector beyond 255 is CAP_ERR_RANGE");
    uint16_t ok_vec = 10;
    mint_word(P, ok_vec, irq_word(35, CAP_PERM_BIND));
    uint16_t r11 = CAP_NONE;
    CHECK(k_irq_bind(P, ok_vec, CHAN_QUEUE_DEPTH + 1, &r11) == CAP_ERR_RANGE,
          "budget beyond CHAN_QUEUE_DEPTH is CAP_ERR_RANGE");

    /* ── 9. syscall wrapper ───────────────────────────────────────────── */
    CHECK(sys_sls_irq_bind(NULL) == CAP_ERR_PROTO, "NULL request is CAP_ERR_PROTO");
    struct SLSIrqBindRequest req;
    memset(&req, 0, sizeof(req));
    req.slot = ok_vec;
    CHECK(sys_sls_irq_bind(&req) == CAP_ERR_OK, "sys_sls_irq_bind wrapper ok");
    CHECK(req.out_chan_r != CAP_NONE, "wrapper fills out_chan_r");
    CHECK(k_cap_info(P, req.out_chan_r, &info) == CAP_ERR_OK &&
          info.ty == CAP_TYPE_CHAN_R, "wrapper's returned slot is a CHAN_R");

    /* ── 10. teardown releases the vector ─────────────────────────────── */
    cap_table_teardown(P);
    uint16_t rebind = 11;
    mint_word(P, rebind, irq_word(33 /* the first vector again */, CAP_PERM_BIND));
    uint16_t r12 = CAP_NONE;
    CHECK(k_irq_bind(P, rebind, 0, &r12) == CAP_ERR_OK,
          "vector 33 is free for rebind after teardown");
    CHECK(r12 != CAP_NONE, "rebind returned a channel");

    /* ------------------------------------------ 11. SYS_IRQ_UNBIND (317): release without dying */
    uint16_t ub_irq = 12;
    mint_word(P, ub_irq, irq_word(40 /* vector for the unbind test */, CAP_PERM_BIND));
    uint16_t ub_chan = CAP_NONE;
    CHECK(k_irq_bind(P, ub_irq, 0, &ub_chan) == CAP_ERR_OK, "bind vector 40");
    CHECK(ub_chan != CAP_NONE, "vector 40 has a CHAN_R slot");

    /* SYS_IRQ_MASK (318): arm/disarm a bound vector without unbinding. */
    CHECK(k_irq_mask(P, ub_chan, 1) == CAP_ERR_OK, "mask a bound vector");
    CHECK(k_irq_mask(P, ub_chan, 0) == CAP_ERR_OK, "re-arm a bound vector");
    CHECK(k_irq_mask(P, 0xFFFF, 1) == CAP_ERR_RANGE, "mask: bad slot is RANGE");

    uint16_t freed_vector = CAP_NONE;
    CHECK(k_irq_unbind(P, ub_chan, &freed_vector) == CAP_ERR_OK,
          "unbind by CHAN_R slot succeeds");
    CHECK(freed_vector == 40, "unbind reports the freed vector");

    /* registry disarmed: notify no longer wakes or enqueues */
    g_wakes = 0;
    cap_irq_notify(40);
    cap_irq_drain_pending();
    CHECK(g_wakes == 0, "notify after unbind does not wake");

    /* driver's endpoint closed: recv fails CAP_ERR_STATE */
    struct SLSChanRecvOut out2;
    memset(&out2, 0, sizeof(out2));
    CHECK(k_chan_recv(P, ub_chan, buf, sizeof(buf), NULL, 0, &out2) == CAP_ERR_STATE,
          "driver's recv on the unbound channel is CAP_ERR_STATE");

    /* vector free for rebind with a fresh IRQ cap */
    uint16_t rebind2 = 13;
    mint_word(P, rebind2, irq_word(40, CAP_PERM_BIND));
    uint16_t r13 = CAP_NONE;
    CHECK(k_irq_bind(P, rebind2, 0, &r13) == CAP_ERR_OK,
          "vector 40 is free for rebind after unbind");
    CHECK(r13 != CAP_NONE, "rebind returned a channel");

    /* ------------------------------------------ 12. unbind errors */
    CHECK(k_irq_unbind(P, 3000, NULL) == CAP_ERR_RANGE,
          "unbind bad slot is CAP_ERR_RANGE");
    uint16_t dead2 = 14;
    cap_tables[cap_table_index(P)].slots[dead2].word = 0;
    CHECK(k_irq_unbind(P, dead2, NULL) == CAP_ERR_REVOKED,
          "unbind revoked slot is CAP_ERR_REVOKED");
    uint16_t mem2 = 15;
    mint_word(P, mem2, ((uint64_t)CAP_TYPE_MEM << CAP_TYPE_SHIFT) |
                       ((uint64_t)1 << CAP_OBJ_SHIFT) |
                       ((uint64_t)CAP_PERM_R << CAP_PERM_SHIFT));
    CHECK(k_irq_unbind(P, mem2, NULL) == CAP_ERR_TYPE,
          "unbind on a MEM cap is CAP_ERR_TYPE");

    /* a plain (non-IRQ) channel is CAP_ERR_STATE */
    uint16_t p_ch_w = CAP_NONE, p_ch_r = CAP_NONE;
    uint16_t p_fr = CAP_NONE, p_fw = CAP_NONE;
    CHECK(cap_chan_create(P, 0, &p_ch_r, &p_ch_w, &p_fr, &p_fw) == 0,
          "create a plain channel pair for the negative test");
    CHECK(p_ch_r != CAP_NONE, "plain channel's CHAN_R slot exists");
    CHECK(k_irq_unbind(P, p_ch_r, NULL) == CAP_ERR_STATE,
          "unbind on a non-IRQ channel is CAP_ERR_STATE");
    CHECK(k_irq_mask(P, p_ch_r, 1) == CAP_ERR_STATE,
          "mask: plain channel is CAP_ERR_STATE");

    /* the already-unbound channel: unbind again */
    CHECK(k_irq_unbind(P, ub_chan, NULL) == CAP_ERR_STATE,
          "double-unbind of the same channel is CAP_ERR_STATE");
    CHECK(sys_sls_irq_unbind(NULL) == CAP_ERR_PROTO,
          "NULL unbind request is CAP_ERR_PROTO");
    struct SLSIrqUnbindRequest ureq;
    memset(&ureq, 0, sizeof(ureq));
    ureq.chan_r = r13;   /* the vector-40 rebind channel */
    CHECK(sys_sls_irq_unbind(&ureq) == CAP_ERR_OK,
          "sys_sls_irq_unbind wrapper ok");



    printf("all irq-bind checks passed\n");
    return 0;
}