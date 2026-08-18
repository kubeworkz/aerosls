/* user/examples/cap_peer.c — two-party capability handoff, CHILD side
 *
 * Spawned by cap_two_party (the parent) via SYS_SLS_PROGRAM_SPAWN. This is
 * the far_pid half of the Phase-1 acceptance pattern that a single-process
 * demo cannot exercise: a REAL second Ring-3 process mints a channel with
 * far_pid = its parent's pid, so the far-end caps land directly in the
 * PARENT's capability table:
 *
 *   1. sls_getppid()      — resolve the spawner (kernel recorded it at spawn)
 *   2. cap_chan_create(far_pid = parent) — our end: rd=0, wr=1 (fresh table);
 *                           far end minted into the parent's table: frd=0,
 *                           fwr=1 (also fresh)
 *   3. cap_arena_alloc    — a 4 KiB MEM cap (slot 2 in our table)
 *   4. cap_map            — a real user PTE in THIS process's page table
 *   5. write "Hello"      — through the mapping
 *   6. cap_send(wr, mem)  — MOVE the MEM cap to the parent over the channel
 *                           (pid-keyed direction: creator end0 -> end1)
 *   7. exit               — the kernel returns control to the blocked parent
 *
 * The parent resumes, recvs the cap from its own table, maps the SAME
 * physical page in ITS address space, and reads "Hello" back.
 *
 * Build:
 *   make user-programs
 *   python3 utils/program_upload.py --file user/examples/cap_peer.bin \
 *                                   --name cap_peer
 */

#include <sls.h>
#include <aerocap.h>

/* Free 4 KiB-aligned user vaddr for the demo mapping — 1 TiB (PML4 slot 2).
 * MUST NOT sit in PML4 slot 0: the kernel's supervisor-only identity map is
 * cloned into every user PML4 there, and the CPU checks U/S at every walk
 * level, so a user leaf PTE in slot 0 still faults (see cap_roundtrip.c). */
#define DEMO_VA 0x10000000000ULL

/* Tiny decimal printer (libsls has no printf). */
static void put_u32(uint32_t v) {
    char buf[12];
    int i = 11;
    buf[11] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    sls_puts(&buf[i]);
}

int main(void) {
    sls_puts("[peer] ring-3 capability peer starting\n");

    /* 1. Who spawned me? The kernel recorded it at spawn time. */
    uint32_t parent_pid = sls_getppid();
    if (parent_pid == 0) {
        sls_puts("[peer] getppid -> 0 (kernel-context spawn; expected a parent)\n");
        return 1;
    }
    sls_puts("[peer] getppid -> ");
    put_u32(parent_pid);
    sls_puts("\n");

    /* 2. Channel: far end minted directly into the PARENT's table. */
    cap_t rd = CAP_NONE, wr = CAP_NONE, frd = CAP_NONE, fwr = CAP_NONE;
    int rc = cap_chan_create(parent_pid, &rd, &wr, &frd, &fwr);
    if (rc != 0 || rd == CAP_NONE || wr == CAP_NONE) {
        sls_puts("[peer] chan_create FAILED\n");
        return 1;
    }
    sls_puts("[peer] chan_create ok (far_pid=parent): mine rd=");
    put_u32((uint32_t)rd);
    sls_puts(" wr=");
    put_u32((uint32_t)wr);
    sls_puts("  far frd=");
    put_u32((uint32_t)frd);
    sls_puts(" fwr=");
    put_u32((uint32_t)fwr);
    sls_puts("\n");

    /* 3. Arena MEM cap. */
    cap_t mem = CAP_NONE;
    rc = cap_arena_alloc(1, CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP, &mem);
    if (rc != 0 || mem == CAP_NONE) {
        sls_puts("[peer] arena_alloc FAILED\n");
        return 1;
    }
    sls_puts("[peer] arena_alloc ok, MEM cap=");
    put_u32((uint32_t)mem);
    sls_puts("\n");

    /* 4+5. Map in THIS process's address space and write "Hello". */
    rc = cap_map(mem, DEMO_VA, CAP_PERM_R | CAP_PERM_W);
    if (rc != 0) {
        sls_puts("[peer] map FAILED\n");
        return 1;
    }
    sls_puts("[peer] map ok (user PTE in child's page table)\n");
    volatile char* p = (volatile char*)(uintptr_t)DEMO_VA;
    const char hello[] = "Hello";
    for (int i = 0; i < 5; i++) p[i] = hello[i];

    /* 6. Move the MEM cap to the parent over the channel. */
    rc = cap_send(wr, mem, 0x5EEDF00DULL);
    if (rc != 0) {
        sls_puts("[peer] send FAILED\n");
        return 1;
    }
    sls_puts("[peer] send ok — wrote 'Hello' and moved the MEM cap to the parent\n");
    sls_puts("[peer] exiting (kernel resumes the blocked parent)\n");
    return 0;
}
