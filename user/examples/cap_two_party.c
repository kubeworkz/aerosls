/* user/examples/cap_two_party.c — two-party capability handoff, PARENT side
 *
 * The Phase-1 acceptance pattern with a REAL second Ring-3 process. This
 * kernel's spawn is synchronous (program_spawn() blocks in kernel_enter_ring3
 * until the child exits — one Ring-3 process runs at a time, timer-preempted),
 * so the exchange is turn-based: the child runs to completion and we resume.
 * The capability handoff is still fully real — kernel-mediated, across two
 * distinct address spaces and two distinct capability tables:
 *
 *   1. sls_program_spawn("cap_peer") — blocks until the child exits, then
 *      returns its pid. While the child ran it minted a channel with
 *      far_pid = our pid, so OUR table now holds the far end: frd=0, fwr=1
 *      (our table was fresh; freelist pops 0,1 in order).
 *   2. cap_recv(slot 0 = frd) — pops the MEM cap the child sent; it lands in
 *      our table at the next free slot (2).
 *   3. cap_map + read — the SAME physical arena page the child wrote, mapped
 *      into OUR address space: "Hello" must come back.
 *   4. cap_revoke — teardown: our far caps (0,1) and the MEM cap; the arena
 *      frame returns to the pool.
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/cap_two_party.bin \
 *                                   --name cap_two_party
 *   python3 utils/program_upload.py --file user/examples/cap_peer.bin \
 *                                   --name cap_peer
 *   (spawn cap_two_party via POST /api/program/spawn)
 */

#include <sls.h>
#include <aerocap.h>

/* Same free user-half vaddr as cap_peer — different page tables, no clash. */
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
    sls_puts("[roundtrip] two-party capability handoff (parent) starting\n");
    int fail = 0;

    /* 1. Spawn the child — blocks until it exits, then returns its pid. */
    uint64_t peer_pid = sls_program_spawn("cap_peer");
    if (peer_pid == 0) {
        sls_puts("[roundtrip] spawn cap_peer FAILED\n");
        return 1;
    }
    sls_puts("[roundtrip] child ran and exited; pid=");
    put_u32((uint32_t)peer_pid);
    sls_puts("\n");

    /* 2. The child minted the far end into OUR table: frd=0, fwr=1.
     *    Recv on slot 0 pops the transferred MEM cap into slot 2. */
    uint64_t cookie = 0;
    cap_t mem = CAP_NONE;
    int rc = cap_recv(0 /* frd */, 0, &cookie, &mem);
    if (rc != 0 || mem == CAP_NONE) {
        sls_puts("[roundtrip] recv FAILED (no cap from child)\n");
        return 1;
    }
    sls_puts("[roundtrip] recv ok — MEM cap arrived in our table, slot=");
    put_u32((uint32_t)mem);
    sls_puts(" cookie=");
    put_u32((uint32_t)cookie);
    sls_puts("\n");

    /* 3. Map the SAME physical page in OUR address space and read "Hello". */
    rc = cap_map(mem, DEMO_VA, CAP_PERM_R | CAP_PERM_W);
    if (rc != 0) {
        sls_puts("[roundtrip] map FAILED\n");
        return 1;
    }
    volatile char* p = (volatile char*)(uintptr_t)DEMO_VA;
    const char hello[] = "Hello";
    int match = 1;
    for (int i = 0; i < 5; i++) if (p[i] != hello[i]) match = 0;
    if (match)
        sls_puts("[roundtrip] mapped + read 'Hello' back from the child: PASS\n");
    else {
        sls_puts("[roundtrip] mapped + read 'Hello' back from the child: FAIL\n");
        fail = 1;
    }

    /* 4. Teardown: our far caps (0,1) + the transferred MEM cap. The arena
     *    frame returns to the pool. (The child's channel caps persist in its
     *    table — Phase 1 tables are process-lifetime; reclaim is Phase 2.) */
    cap_revoke(0);
    cap_revoke(1);
    rc = cap_revoke(mem);
    if (rc != 0) { sls_puts("[roundtrip] revoke FAILED\n"); fail = 1; }
    else         sls_puts("[roundtrip] revoke ok — channel end + MEM cap torn down\n");

    cap_list();

    sls_puts(fail ? "[roundtrip] done (FAILURES)\n" : "[roundtrip] done\n");
    return fail;
}
