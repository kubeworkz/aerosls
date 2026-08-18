/* user/examples/cap_overlap.c — Phase 1.5 live-overlap test, PARENT side
 *
 * Two Ring-3 processes genuinely LIVE at once for the first time:
 *
 *   1. sls_program_spawn_nb_held("cap_peer_ovl") — GATED non-blocking
 *      spawn: returns immediately with the child's pid; the child is queued
 *      with a synthetic ring-3 context in PROC_HELD — it CANNOT run until
 *      we release it. (A plain spawn_nb leaves a timer race: the child can
 *      run before our chan_create, shift its own far-end slots, and its
 *      hardcoded send index hits an empty slot.)
 *   2. cap_chan_create(far_pid = child) — WE provision the channel while
 *      the child is held. Our end: rd=0, wr=1; the child's far end is
 *      minted into ITS table: frd=0, fwr=1.
 *   3. cap_recv_release(0, block=1, child_pid) — the kernel releases the
 *      child AND parks us in ONE syscall (no user-mode window between them,
 *      so a timer tick cannot schedule the child before our park — a plain
 *      release-then-recv left that race and made the handoff-ordering proof
 *      flaky). We are still inside the recv syscall when the child runs —
 *      the scheduler iretq's into the child from our abandoned syscall
 *      stack. The child sends the MEM cap, which WAKES us.
 *   4. We resume INSIDE the recv (cap_recv_resume re-runs it), map the
 *      SAME physical page in our address space, and read "Hello" back.
 *   5. cap_recv(0, block=1) again — park for the child's "done" marker.
 *   6. sls_yield() immediately after recv#2 returns — hand the CPU back to
 *      the child (still suspended mid-send after its 'done' handoff) so it
 *      can exit cleanly; its async exit hands the CPU back to us and we
 *      resume from the yield. THEN print recv#2's result, tear down, and
 *      exit. (The yield must precede the serial prints: printing first
 *      would let the timer preempt us and the child would exit before the
 *      yield, turning it into a no-op.)
 *
 * Phase 1.5 IMMEDIATE WAKE: each child send WAKES us and the kernel hands
 * the CPU to us RIGHT THEN (send-side handoff) — we resume inside our recv
 * BEFORE the child's send syscall returns to ring-3. The transcript's
 * smoking gun: "[ovl] recv#1 ok" appears BEFORE "[peer2] resumed after
 * send" — the receiver ran first, no tick latency. The kernel's own
 * "[CAP] recv block: parked PID ..." / "[CAP] woken PID ..." /
 * "[CAP] send handoff: PID ..." lines tell the story, plus the child's
 * spin prints interleaving with our resumed prints when the timer
 * preempts it.
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/cap_peer_ovl.bin \
 *                                   --name cap_peer_ovl
 *   python3 utils/program_upload.py --file user/examples/cap_overlap.bin \
 *                                   --name cap_overlap
 *   (spawn cap_overlap via POST /api/program/spawn)
 */

#include <sls.h>
#include <aerocap.h>

/* Same free user-half vaddr as the child — different page tables, no clash. */
#define DEMO_VA 0x10000000000ULL

static void put_u32(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[11] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sls_puts(&buf[i]);
}

int main(void) {
    sls_puts("[ovl] live-overlap test starting\n");
    int fail = 0;

    /* 1. GATED non-blocking spawn: returns BEFORE the child has run — and
     * the child starts PROC_HELD, so it CANNOT run until we release it. A
     * plain spawn_nb leaves a timer race: a tick between its return and our
     * chan_create schedules the child first, its allocations shift the
     * far-end cap slots, and its hardcoded send index hits an empty slot
     * (caught live as a child EBADF cascade). */
    uint64_t child_pid = sls_program_spawn_nb_held("cap_peer_ovl");
    if (child_pid == 0) {
        sls_puts("[ovl] spawn_nb FAILED\n");
        return 1;
    }
    sls_puts("[ovl] spawn_nb returned immediately (held-spawn: child pid=");
    put_u32((uint32_t)child_pid);
    sls_puts(" — child cannot run yet)\n");

    /* 2. Provision the channel ourselves: our rd=0, wr=1; the child's far
     *    end (frd=0, fwr=1) is minted into the child's table by the kernel
     *    even though the child has not run yet. The child is HELD, so it
     *    cannot interfere with our provisioning (or shift its own slots). */
    cap_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
    int rc = cap_chan_create((uint32_t)child_pid, &rd, &wr, &frd, &fwr);
    if (rc != 0 || rd == CAP_NONE || wr == CAP_NONE) {
        sls_puts("[ovl] chan_create FAILED\n");
        return 1;
    }
    sls_puts("[ovl] chan_create ok (far_pid=child): mine rd=");
    put_u32((uint32_t)rd);
    sls_puts(" wr=");
    put_u32((uint32_t)wr);
    sls_puts("  far frd=");
    put_u32((uint32_t)frd);
    sls_puts(" fwr=");
    put_u32((uint32_t)fwr);
    sls_puts("\n");

    /* 2b+3. RELEASE + blocking recv in ONE syscall (cap_recv_release): the
     * channel is provisioned (the child's far-end caps are at frd=0/fwr=1,
     * whatever it allocates lands after). There is NO user-mode window
     * between the release and the park — a timer tick there would schedule
     * the child before we park, its send#1 would find nobody parked, and
     * the handoff-ordering proof would fail (caught live). The kernel
     * releases the child, parks us mid-syscall, and iretq's into the child
     * — which runs LIVE while we are parked, sends, and wakes us. */
    uint64_t cookie = 0;
    cap_t mem = CAP_NONE;
    rc = cap_recv_release(0 /* rd */, 1 /* block */,
                          (uint32_t)child_pid, &cookie, &mem);
    if (rc != 0 || mem == CAP_NONE) {
        sls_puts("[ovl] blocking recv FAILED (rc=");
        put_u32((uint32_t)(-rc));
        sls_puts(")\n");
        return 1;
    }
    sls_puts("[ovl] recv#1 ok — woken by child; MEM cap slot=");
    put_u32((uint32_t)mem);
    sls_puts("\n");

    /* 4. Map the SAME physical page in OUR address space and read "Hello". */
    rc = cap_map(mem, DEMO_VA, CAP_PERM_R | CAP_PERM_W);
    if (rc != 0) {
        sls_puts("[ovl] map FAILED\n");
        return 1;
    }
    volatile char* p = (volatile char*)(uintptr_t)DEMO_VA;
    const char hello[] = "Hello";
    int match = 1;
    for (int i = 0; i < 5; i++) if (p[i] != hello[i]) match = 0;
    if (match)
        sls_puts("[ovl] mapped + read 'Hello' back from the live child: PASS\n");
    else {
        sls_puts("[ovl] mapped + read 'Hello' back from the live child: FAIL\n");
        fail = 1;
    }

    /* 5. Park again for the child's 'done' marker (sent just before exit). */
    cookie = 0;
    cap_t m2 = CAP_NONE;
    rc = cap_recv(0, 1 /* block */, &cookie, &m2);
    if (rc != 0) {
        sls_puts("[ovl] recv#2 FAILED (rc=");
        put_u32((uint32_t)(-rc));
        sls_puts(")\n");
        return 1;
    }

    /* 5b. Phase 1.5 immediate wake: the child's 'done' send handed the CPU
     * to us, so the child is STILL SUSPENDED mid-send. Yield the CPU back
     * NOW — before any slow serial prints, so the child is guaranteed still
     * alive (printing first would let the timer preempt us and the child
     * would exit before our yield, making it a no-op). The child resumes,
     * prints, and exits; its async exit hands the CPU back to us and we
     * resume from this yield, then finish. */
    sls_yield();
    sls_puts("[ovl] resumed from yield (child exited)\n");

    sls_puts("[ovl] recv#2 ok — 'done' marker (cookie=");
    put_u32((uint32_t)cookie);
    sls_puts("), plain message as expected: ");
    sls_puts(m2 == CAP_NONE ? "PASS" : "FAIL");
    sls_puts("\n");
    if (m2 != CAP_NONE) fail = 1;

    /* 6. Teardown: our far caps (0,1) + the transferred MEM cap. */
    cap_revoke(0);
    cap_revoke(1);
    rc = cap_revoke(mem);
    if (rc != 0) { sls_puts("[ovl] revoke FAILED\n"); fail = 1; }
    else         sls_puts("[ovl] revoke ok — channel end + MEM cap torn down\n");

    /* 6b. Final summary print + exit (the yield in step 5b was the CPU
     * handback; the child has exited by now). */
    cap_list();

    sls_puts(fail ? "[ovl] done (FAILURES)\n" : "[ovl] done\n");
    return fail;
}
