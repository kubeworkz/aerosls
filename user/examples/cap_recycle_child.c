/* user/examples/cap_recycle_child.c — Phase 2 teardown verification, CHILD side
 *
 * Spawned by cap_recycle (the parent) via SYS_SLS_PROGRAM_SPAWN_NB_HELD —
 * the GATED non-blocking spawn. While we are HELD, the parent provisions a
 * channel with far_pid = our pid, so our far-end caps are already minted in
 * OUR table at frd=0, fwr=1 when we first run. We then:
 *
 *   1. cap_arena_alloc(1) — a MEM cap we KEEP in our table (slot 2). We exit
 *      WITHOUT revoking it: Phase-2 teardown must drop the holder, destroy
 *      the object, and return its arena frame at process_exit time.
 *   2. cap_arena_alloc(1) — a second MEM cap (slot 3) we MOVE to the parent
 *      via cap_send on our far write end (slot 1). The parent recv's it and
 *      revokes it on the normal path.
 *   3. exit — our far-end channel caps (0, 1) are also reclaimed by
 *      teardown: the channel object's refcount drops 4 -> 2 (the parent
 *      still holds its own ends), and our cap-table binding is UNBOUND so
 *      the next cycle's child can bind it again.
 *
 * The parent's 20-cycle loop is what proves reclamation: with teardown
 * broken, the cap tables exhaust after ~15 children (CAP_TABLE_MAX = 16,
 * one slot reserved for the kernel) and cycle 16's chan_create / child
 * allocations fail.
 */

#include <sls.h>
#include <aerocap.h>

static void put_u32(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[11] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sls_puts(&buf[i]);
}

int main(void) {
    /* Far end provisioned by the parent while we were HELD: frd=0, fwr=1.
     * The parent releases us inside its blocking recv, so we run LIVE while
     * it is parked — our send wakes it (send-side handoff). */
    cap_t keep = CAP_NONE, send = CAP_NONE;

    /* 1. The KEPT cap — reclaimed by Phase-2 teardown at our exit. */
    int rc = cap_arena_alloc(1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP, &keep);
    if (rc != 0 || keep == CAP_NONE) {
        sls_puts("[rc2] arena_alloc(keep) FAILED\n");
        return 1;
    }
    /* 2. The SENT cap — moved to the parent (normal recv/revoke path). */
    rc = cap_arena_alloc(1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP, &send);
    if (rc != 0 || send == CAP_NONE) {
        sls_puts("[rc2] arena_alloc(send) FAILED\n");
        return 1;
    }
    rc = cap_send(1 /* fwr */, send, 0x51 /* 'Q' */);
    if (rc != 0) {
        sls_puts("[rc2] send FAILED (rc=");
        put_u32((uint32_t)(-rc));
        sls_puts(")\n");
        return 1;
    }
    /* 3. Exit WITHOUT revoking anything — keep, send's slot (now free), and
     *    the far-end channel caps are all reclaimed by teardown. */
    return 0;
}
