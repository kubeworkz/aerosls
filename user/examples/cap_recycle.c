/* user/examples/cap_recycle.c — Phase 2 teardown verification, PARENT side
 *
 * Proves that repeated spawn/exit cycles do NOT leak kernel memory:
 *
 *   for each of 20 cycles:
 *     1. sls_program_spawn_nb_held("cap_recycle_child") — the child is HELD
 *        (cannot run) until we provision it.
 *     2. cap_chan_create(far_pid = child) — our rd=0, wr=1; the child's far
 *        end is minted into ITS table at frd=0, fwr=1.
 *     3. cap_recv_release(rd, block=1, child) — release + park in ONE
 *        syscall; the child runs LIVE while we are parked, allocates two
 *        MEM caps, sends one, and exits WITHOUT revoking anything. Its exit
 *        runs Phase-2 teardown: the kept MEM object's arena frame is
 *        returned, the channel refcount drops 4 -> 2, its page tables and
 *        syscall stack are freed, and its cap table is UNBOUND.
 *     4. We resume inside the recv with the sent MEM cap (cookie 'Q'),
 *        revoke it (normal path), and revoke our channel ends — the channel
 *        dies with our last holder.
 *
 * 20 cycles > 15 process cap tables (CAP_TABLE_MAX = 16 minus the kernel
 * slot): without teardown's table unbinding, cycle 16's child cannot bind a
 * table and the cascade fails; without teardown's frame/arena reclamation,
 * the arena free count and frame-pool allocated count grow every cycle
 * (asserted by the boot check via `cap list` after the loop).
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/cap_recycle_child.bin \
 *                                   --name cap_recycle_child
 *   python3 utils/program_upload.py --file user/examples/cap_recycle.bin \
 *                                   --name cap_recycle
 *   (spawn cap_recycle via POST /api/program/spawn)
 */

#include <sls.h>
#include <aerocap.h>

#define CYCLES 20

static void put_u32(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[11] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sls_puts(&buf[i]);
}

int main(void) {
    sls_puts("[recycle] teardown recycle test starting (20 cycles)\n");
    int fail = 0;

    for (uint32_t cycle = 0; cycle < CYCLES; cycle++) {
        /* 1. GATED non-blocking spawn: the child cannot run until released. */
        uint64_t child_pid = sls_program_spawn_nb_held("cap_recycle_child");
        if (child_pid == 0) {
            sls_puts("[recycle] spawn FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            return 1;
        }

        /* 2. Provision the channel while the child is held. */
        cap_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
        int rc = cap_chan_create((uint32_t)child_pid, &rd, &wr, &frd, &fwr);
        if (rc != 0 || rd == CAP_NONE || wr == CAP_NONE) {
            sls_puts("[recycle] chan_create FAILED at cycle ");
            put_u32(cycle);
            sls_puts(" (rc=");
            put_u32((uint32_t)(-rc));
            sls_puts(")\n");
            return 1;
        }

        /* 3. Release + blocking recv in ONE syscall. The child runs while
         *    we are parked, sends the MEM cap + cookie 'Q', exits. */
        uint64_t cookie = 0;
        cap_t mem = CAP_NONE;
        rc = cap_recv_release(rd, 1 /* block */, (uint32_t)child_pid,
                              &cookie, &mem);
        if (rc != 0 || mem == CAP_NONE) {
            sls_puts("[recycle] recv FAILED at cycle ");
            put_u32(cycle);
            sls_puts(" (rc=");
            put_u32((uint32_t)(-rc));
            sls_puts(")\n");
            return 1;
        }
        if (cookie != 0x51) {
            sls_puts("[recycle] cookie mismatch at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            fail = 1;
        }

        /* 4. Normal-path teardown: the transferred MEM cap, then our
         *    channel ends (the channel object dies with our last holder —
         *    the child's ends died with the child at its exit). */
        rc = cap_revoke(mem);
        if (rc != 0) {
            sls_puts("[recycle] revoke(mem) FAILED at cycle ");
            put_u32(cycle);
            sls_puts("\n");
            fail = 1;
        }
        cap_revoke(rd);
        cap_revoke(wr);

        sls_puts("[recycle] cycle ");
        put_u32(cycle);
        sls_puts(" ok\n");
    }

    /* Yield FIRST so the last child (still suspended mid-send after the
     * final handoff) runs to its exit — its teardown reclaims its kept
     * MEM cap before the dump below. Without the yield, a timer tick may
     * not fire before cap_list, and the dump shows the last child's cap
     * still live (objects active=1, arena free=16383) — caught live. */
    sls_yield();

    /* Dump the kernel's cap tables so the boot check can assert the arena
     * is fully returned (objects active=0, arena free=16384/16384) after
     * 20 spawn/exit cycles — the leak the milestone exists to close. */
    cap_list();

    if (!fail) {
        sls_puts("[recycle] DONE: ");
        put_u32(CYCLES);
        sls_puts("/");
        put_u32(CYCLES);
        sls_puts(" cycles passed\n");
    } else {
        sls_puts("[recycle] done (FAILURES)\n");
    }
    return fail;
}
