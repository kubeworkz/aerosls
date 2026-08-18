/* user/examples/cap_peer_ovl.c — Phase 1.5 overlap test, CHILD side
 *
 * Spawned by cap_overlap (the parent) via SYS_SLS_PROGRAM_SPAWN_NB_HELD —
 * the GATED non-blocking spawn: the parent's syscall returns immediately
 * and this process is queued with a synthetic ring-3 context in PROC_HELD
 * (non-schedulable). The PARENT provisions the channel (cap_chan_create
 * with far_pid = our pid) while we are held — so by the time we first run,
 * our far-end caps are already minted in OUR table at frd=0, fwr=1 — then
 * releases us and parks. We run LIVE while the parent is PARKED inside its
 * blocking cap_recv — the kernel parked the parent mid-syscall and
 * iretq'd into us. This is the first time two Ring-3 processes are
 * genuinely live at once:
 *
 *   1. sls_getppid()          — resolve the parent (recorded at spawn time)
 *   2. send on our far write end (slot 1) — the MEM cap MOVE that WAKES the
 *                              parked parent. Phase 1.5 IMMEDIATE WAKE: the
 *                              kernel hands the CPU to the parent RIGHT
 *                              HERE, so our send returns only AFTER the
 *                              parent recv'd the cap — the "resumed after
 *                              send" print below is our first line back.
 *   3. spin + "spin" prints   — prove we stay live (the parent is parked
 *                              in its second recv while we spin)
 *   4. plain "done" message   — the parent's second wake (handoff again)
 *   5. exit                   — the parent yields back to us first, so we
 *                              can exit; our exit hands the CPU to it.
 *
 * Layout: arena_alloc lands at slot 2 (our table held frd=0, fwr=1).
 */

#include <sls.h>
#include <aerocap.h>

/* Free 4 KiB-aligned user vaddr for the demo mapping — 1 TiB (PML4 slot 2).
 * MUST NOT sit in PML4 slot 0 (the kernel's supervisor-only identity map —
 * see cap_peer.c's comment). Different page table from the parent, so both
 * can map the same vaddr. */
#define DEMO_VA 0x10000000000ULL

static void put_u32(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[11] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sls_puts(&buf[i]);
}

static void spin(void) {
    volatile uint64_t k;
    for (k = 0; k < 4000000ULL; k++) __asm__ volatile("" ::: "memory");
}

int main(void) {
    sls_puts("[peer2] async child starting (live while parent is parked)\n");

    uint32_t parent_pid = sls_getppid();
    if (parent_pid == 0) {
        sls_puts("[peer2] getppid -> 0 (expected a live parent)\n");
        return 1;
    }
    sls_puts("[peer2] getppid -> ");
    put_u32(parent_pid);
    sls_puts(" (channel pre-provisioned by parent: our far end frd=0 fwr=1)\n");

    cap_t mem = CAP_NONE;
    int rc = cap_arena_alloc(1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP, &mem);
    if (rc != 0 || mem == CAP_NONE) {
        sls_puts("[peer2] arena_alloc FAILED\n");
        return 1;
    }
    rc = cap_map(mem, DEMO_VA, CAP_PERM_R | CAP_PERM_W);
    if (rc != 0) {
        sls_puts("[peer2] map FAILED\n");
        return 1;
    }
    volatile char* p = (volatile char*)(uintptr_t)DEMO_VA;
    const char hello[] = "Hello";
    for (int i = 0; i < 5; i++) p[i] = hello[i];

    /* Send on our far write end (slot 1, minted by the parent's
     * chan_create). This MOVE is what wakes the parked parent. */
    rc = cap_send(1 /* fwr */, mem, 0x5EEDF00DULL);
    if (rc != 0) {
        sls_puts("[peer2] send FAILED\n");
        return 1;
    }
    /* Immediate-wake proof: the parent already recv'd the MEM cap and is
     * parked in its SECOND recv before this line prints — the kernel
     * handed the CPU to it mid-send (send-side handoff), and our send
     * syscall only returned once the parent parked again. */
    sls_puts("[peer2] resumed after send — parent recv'd the MEM cap first\n");

    /* Stay live: the parent is parked in its second blocking recv while we
     * spin — prove we overlap with it. */
    for (int i = 0; i < 3; i++) {
        spin();
        sls_puts("[peer2] spin i=");
        put_u32((uint32_t)i);
        sls_puts("\n");
    }

    rc = cap_send(1 /* fwr */, CAP_NONE, 0xD0DEULL);
    if (rc != 0) {
        sls_puts("[peer2] done-send FAILED\n");
        return 1;
    }
    /* Handoff again: the parent recv'd the marker, tore down, and YIELDED
     * back to us before this line prints. */
    sls_puts("[peer2] resumed after 'done' send; exiting\n");
    return 0;
}
