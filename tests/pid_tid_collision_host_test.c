/*
 * pid_tid_collision_host_test.c — the two halves of the watchdog-respawn
 * PID/TID fix, driven against the REAL kernel code.
 *
 * The live failure this pins: on a single-vCPU boot the watchdog respawned
 * POSIX as PID 104 while a later Linux clone was handed TID 104 from a
 * separate sequence.  proc_group_cap_owner() then resolved 104 to the Linux
 * group's cap owner, so the sidecar's NET_SOCKET send landed in the thread
 * group's table and the network driver was never woken.
 *
 * This test has two independent sections:
 *
 *   1. linux_clone_thread() (kernel/process.c) takes its child's PID through
 *      alloc_pid(), the same monotonic sequence process creation uses.  A
 *      live descriptor already holding the next sequence value must be
 *      skipped, not reused as a TID.
 *
 *   2. cap_table_index() (kernel/cap.c) prefers a table already bound to the
 *      descriptor's OWN pid over Linux-group redirection.  A sidecar whose
 *      table was minted before a duplicate Linux thread appeared must keep
 *      using that table even though proc_group_cap_owner() now maps the
 *      duplicate pid to the thread group.
 *
 * Both properties are exercised through production functions: process.c is
 * included as source (the established host-test technique for its static
 * `next_pid`), and cap.c/linux_thread.c are linked for real.  The stubs
 * below are only the allocator/console edges those paths call.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -ffunction-sections -fdata-sections \
 *       -Wl,--gc-sections -no-pie -I . -I kernel \
 *       -o /tmp/pid_tid_collision_host_test \
 *       tests/pid_tid_collision_host_test.c kernel/cap.c kernel/linux_thread.c \
 *           tests/e7_group_stubs.c
 *   /tmp/pid_tid_collision_host_test
 *
 * Exit: 0 pass, 1 failure.
 */
#define _GNU_SOURCE   /* syscall(): ARCH_SET_GS for the %gs stand-in */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "kernel/process.h"
#include "kernel/linux_thread.h"
#include "kernel/linux_abi.h"
#include "tests/process_host_stubs.h"

/* ─── Link stubs for the paths this test does not drive ──────────────────── */
char _kernel_image_end[1];
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
volatile uint64_t kernel_tick_counter = 0;

/* linux_clone_thread() allocates the child's 8 KiB syscall stack through
 * process.c's alloc_proc_syscall_stack(), which normally calls the frame
 * pool.  Return one fixed, non-zero run; the stack's contents are irrelevant
 * here, only that the clone gets a valid top and proceeds to join the group. */
uint64_t allocate_contiguous_frames_for_partition(uint32_t partition_id,
                                                  uint64_t nframes,
                                                  uint64_t align_frames) {
    (void)partition_id; (void)nframes; (void)align_frames;
    return 0x700000;
}

/* linux_thread.c's clone validator uses the same user-canonical bound as the
 * live kernel.  The real implementation lives in linux_abi.c, whose wider
 * dependency graph is not needed by these two paths. */
int linux_fs_addr_is_user_canonical(uint64_t addr) {
    return addr < 0x0000800000000000ULL;
}

/* cap_table_index() is internal to cap.c and not in cap.h. */
int cap_table_index(uint32_t pid);

/* Pull in process.c itself: this gives the test the REAL alloc_pid(),
 * linux_clone_thread(), proc_table[] and the file-static `next_pid`. */
#include "kernel/process.c"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

static void reset_world(void) {
    memset(proc_table, 0, sizeof(proc_table));
    proc_count = 0;
    linux_groups_init();
}

static void plant(struct ProcessDescriptor* pd, uint32_t pid, uint64_t cr3) {
    memset(pd, 0, sizeof(*pd));
    pd->pid = pid;
    pd->cr3 = cr3;
    pd->active = 1;
    pd->group_index = LINUX_GROUP_NONE;
}

/* linux_clone_thread() gives the child the parent's live register state, read
 * from the syscall entry scratch at %gs:0x10..0x80 — on a boot the only source
 * that cannot be clobbered by the -O2 call chain. A host process has no
 * per-CPU block, so the first capture read address 0x10 and faulted at
 * process.c:2452. Bind GS to a zeroed stand-in (ARCH_SET_GS), which is what a
 * freshly entered syscall with zero registers would leave there; this test
 * asserts the ids, not the inherited registers. */
static uint8_t host_gs_block[0x88];


int main(void) {
    printf("=== PID/TID collision and sidecar cap-table binding ===\n\n");

    CHECK(syscall(SYS_arch_prctl, 0x1001 /* ARCH_SET_GS */, host_gs_block) == 0,
          "the host binds %gs to a stand-in block: the clone's entry capture "
          "reads real memory instead of faulting at 0x10");

    /* ── 1. A Linux clone cannot take a live process's PID ───────────────
     * The live PID 104 sits exactly on the next sequence value, which is
     * the shape that produced the boot failure.  alloc_pid() must advance
     * past it, and linux_clone_thread() must stamp the child with that
     * fresh value rather than a separate next_pid++ sequence. */
    {
        reset_world();
        plant(&proc_table[1], 104, 0x1000);
        next_pid = 104;

        uint32_t first = alloc_pid();
        CHECK(first != 104, "alloc_pid() skips a live process PID already at the sequence head");
        CHECK(first == 105, "alloc_pid() advances to the next free PID after the live 104");

        reset_world();
        struct ProcessDescriptor* parent = &proc_table[0];
        plant(parent, 107, 0x2000);
        parent->personality = PROC_PERSONALITY_LINUX;
        parent->partition_id = 0;
        CHECK(linux_group_create(parent) == 0,
              "the parent Linux descriptor becomes a group of one");

        plant(&proc_table[1], 104, 0x1000);
        next_pid = 104;

        struct LinuxClonePlan plan;
        CHECK(linux_clone_plan(LINUX_CLONE_GO_FLAGS, 0x00007FFFF0000000ULL,
                               0, 0, parent->fs_base, &plan) == 0,
              "the clone plan accepts Go's shared-flag shape");

        uint32_t tid = 0;
        int rc = linux_clone_thread(parent, &plan, 0x0000000000401234ULL, &tid);
        CHECK(rc == 0, "linux_clone_thread() creates a sibling");
        CHECK(tid != 104, "the new Linux TID does not reuse live process PID 104");
        CHECK(tid == 105, "the new TID is the allocator's next free value, 105");

        struct ProcessDescriptor* child = &proc_table[2];
        CHECK(child->active && child->pid == tid && child->tid == tid,
              "the child descriptor carries the fresh PID/TID in its own slot");
        CHECK(child->group_index == parent->group_index,
              "the child joins the parent's group (shared cap table and cr3)");
    }

    /* ── 2. A sidecar stays on its own table when a duplicate PID exists ──
     * Bind the sidecar's table first, exactly as create_sidecar does when
     * it mints caps.  Then plant a Linux thread with the same PID in an
     * earlier descriptor slot, so proc_group_cap_owner(104) resolves to the
     * group's owner 107.  cap_table_index(104) must still return the
     * sidecar's original table, not the group's. */
    {
        reset_world();
        struct ProcessDescriptor* sidecar = &proc_table[1];
        plant(sidecar, 104, 0x3000);

        int own = cap_table_index(104);
        CHECK(own > 0, "the sidecar binds a capability table for its own PID");

        struct ProcessDescriptor* thread = &proc_table[0];
        struct ProcessDescriptor* leader = &proc_table[2];
        plant(leader, 107, 0x4000);
        CHECK(linux_group_create(leader) == 0,
              "the duplicate PID's Linux group gets its own cap owner");

        thread->pid = 104;
        thread->tid = 104;
        thread->active = 1;
        thread->group_index = leader->group_index;
        /* E7's membership record: linux_group_of() trusts group_index only
         * when the descriptor's tgid is that group's id, so a zeroed slot
         * cannot alias group table 0. Without it this hand-planted duplicate
         * is not a member and proc_group_cap_owner() returns 104. */
        thread->tgid = leader->tgid;
        thread->is_group_leader = 0;

        CHECK(proc_group_cap_owner(104) == 107,
              "the planted duplicate makes proc_group_cap_owner(104) resolve to group owner 107");

        int group_ti = cap_table_index(107);
        CHECK(group_ti > 0 && group_ti != own,
              "the group owner gets a distinct capability table");

        int again = cap_table_index(104);
        CHECK(again == own,
              "cap_table_index(104) keeps the sidecar on its own already-bound table");
        CHECK(again != group_ti,
              "the duplicate PID is not redirected to the Linux thread group's table");
    }

    printf("\npid_tid_collision_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
