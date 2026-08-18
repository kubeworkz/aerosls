/* user/examples/cap_roundtrip.c — Capability SDK (Phase 1) Ring-3 demo
 *
 * Runs the Seed Kernel capability lifecycle from USER SPACE through the
 * libaerocap SDK (user/libaerocap/aerocap.h) — no raw syscalls, no request
 * structs:
 *
 *   1. cap_chan_create  — both endpoints provisioned (4 caps)
 *   2. cap_arena_alloc  — a 4 KiB MEM cap from the shared-memory arena
 *   3. cap_map          — map it into THIS process's address space (a real
 *                         user PTE installed by the kernel's MMU hooks)
 *   4. write "Hello"    — through the mapped virtual address
 *   5. read it back     — from the mapped virtual address
 *   6. cap_send         — move the MEM cap over the channel (sender slot
 *                         freed; the far endpoint holds it)
 *   7. cap_revoke       — total teardown: object destroyed, arena frame back
 *   8. cap_list         — dump the tables so the serial log shows the
 *                         lifecycle closed with no leaks
 *
 * The map/write/read steps are the Ring-3 half of the acceptance scenario
 * that the kernel-shell `cap demo` cannot perform (the shell is kernel
 * context with no user CR3, so cap_map there returns CAP_ENOSYS). A real
 * user process HAS a page table, so cap_map installs an actual user PTE.
 *
 * Ring-3 channel caveat (Phase 1): direction is keyed on pid, so a single
 * process holding both endpoint pairs (far_pid=0) cannot round-trip a cap
 * through ONE channel — sends land in q[1], recvs read q[0]. The demo
 * therefore sends the cap A->B (queued for the far endpoint) and receives
 * the honest CAP_EAGAIN on the other direction's empty queue. The real
 * two-party pattern (A mints far caps into B's table via far_pid=B, B recvs
 * on its own read end) is the full acceptance test; the SDK supports it.
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/cap_roundtrip.bin \
 *                                   --name cap_roundtrip
 *   curl -X POST .../api/program/spawn -d '{"name":"cap_roundtrip"}'
 */

#include <sls.h>
#include <aerocap.h>

/* A free 4 KiB-aligned user virtual address for the demo mapping.
 *
 * MUST NOT sit in PML4 slot 0 (0-512 GiB): the kernel identity-maps that
 * slot with supervisor-only (U/S=0) entries, and user_clone_page_table()
 * copies it into every process's PML4, so a user leaf PTE installed there
 * is still denied by the supervisor walk (the CPU checks U/S at every
 * level) -- ring-3 access faults with #PF error=0x7 (present + write +
 * user). The process image lives at USER_PROC_CODE_BASE (64 TiB, PML4
 * slot 128) with its stack just above; 1 TiB (slot 2) is a free user-half
 * slot, clear of both, and cap_map requires the whole range to fit the
 * 47-bit user space. */
#define DEMO_VA 0x10000000000ULL  /* 1 TiB, PML4 slot 2 */

int main(void) {
    sls_puts("[cap] Ring-3 capability lifecycle demo\n");
    int fail = 0;

    /* 1. channel bootstrap: both endpoints provisioned */
    cap_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
    int rc = cap_chan_create(0, &rd, &wr, &frd, &fwr);
    if (rc != 0) { sls_puts("[cap] chan_create FAILED\n"); return 1; }
    sls_puts("[cap] chan_create ok (4 endpoint caps minted)\n");

    /* 2. arena alloc: 4 KiB MEM cap */
    cap_t mem = CAP_NONE;
    rc = cap_arena_alloc(1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP, &mem);
    if (rc != 0 || mem == CAP_NONE) { sls_puts("[cap] arena_alloc FAILED\n"); return 1; }
    sls_puts("[cap] arena_alloc ok (4 KiB MEM cap)\n");

    /* 3. map into THIS process's address space — a real user PTE */
    rc = cap_map(mem, DEMO_VA, CAP_PERM_R | CAP_PERM_W);
    if (rc != 0) { sls_puts("[cap] map FAILED\n"); return 1; }
    sls_puts("[cap] map ok (user PTE installed)\n");

    /* 4+5. write "Hello" through the mapping, read it back */
    volatile char* p = (volatile char*)(uintptr_t)DEMO_VA;
    const char hello[] = "Hello";
    for (int i = 0; i < 5; i++) p[i] = hello[i];
    int match = 1;
    for (int i = 0; i < 5; i++) if (p[i] != hello[i]) match = 0;
    if (match) sls_puts("[cap] write+read 'Hello' PASS (ring-3 user mapping)\n");
    else       { sls_puts("[cap] write+read 'Hello' FAIL\n"); fail = 1; }

    /* 6. send the MEM cap A->B (a move: slot freed, queued for far end) */
    rc = cap_send(wr, mem, 0xC0FFEE42ULL);
    if (rc != 0) { sls_puts("[cap] send FAILED\n"); return 1; }
    sls_puts("[cap] send ok (MEM cap moved to the channel queue)\n");

    /* Honest Phase-1 recv on the other direction: empty queue -> EAGAIN. */
    uint64_t cookie = 0;
    cap_t got = CAP_NONE;
    rc = cap_recv(rd, 0, &cookie, &got);
    if (rc == CAP_EAGAIN)
        sls_puts("[cap] recv -> CAP_EAGAIN (empty far queue; Phase-1 "
                 "non-blocking, expected in a single-process demo)\n");
    else
        sls_puts("[cap] recv: unexpected result\n");

    /* The MEM cap is now queued for the far endpoint, so there is nothing
     * left in THIS table to revoke. Create a second MEM cap and revoke it
     * to show total teardown returns the arena frame. */
    cap_t mem2 = CAP_NONE;
    rc = cap_arena_alloc(1, CAP_PERM_R | CAP_PERM_MAP, &mem2);
    if (rc != 0 || mem2 == CAP_NONE) { sls_puts("[cap] arena_alloc #2 FAILED\n"); return 1; }
    rc = cap_revoke(mem2);
    if (rc != 0) { sls_puts("[cap] revoke FAILED\n"); return 1; }
    sls_puts("[cap] revoke ok (MEM object destroyed, arena frame returned)\n");

    /* 8. dump tables: should show only the channel's four caps. */
    cap_list();

    sls_puts(fail ? "[cap] done (FAILURES)\n" : "[cap] done\n");
    return fail;
}
