/*
 * linux_clone_host_test.c — E7 TG-1 increment I2: the kernel-local clone.
 *
 * What this proves without a boot or a process table allocator:
 *   - the accepted flag word is EXACTLY the Go thread shape
 *     (VM|FS|FILES|SIGHAND|SYSVSEM|THREAD), with SETTLS optional and every
 *     other bit refused EINVAL;
 *   - every mandatory shared flag is actually enforced, so a subset cannot
 *     be accepted while the group keeps sharing what the flag would make
 *     private;
 *   - the child stack must be canonical and non-zero (the kernel does not
 *     allocate it), and the TLS base is canonical when SETTLS is present;
 *   - fs_base is the TLS argument when SETTLS is set, otherwise the parent's
 *     live base;
 *   - the synthesized 20-word context starts at the parent's return RIP with
 *     rax=0, rsi/rsp=child stack, and the ring-3 selectors;
 *   - joining a group stamps tid/tgid/group_index/leader=0 and grows
 *     live_threads exactly once; a full group or an already-joined child is
 *     refused without changing the count;
 *   - clone (56) is LSC_THREAD in the real ABI table and its class name is
 *     "thread", so the table cannot silently reclassify it as an fd row.
 *
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/linux_clone_host_test \
 *       tests/linux_clone_host_test.c kernel/linux_thread.c kernel/linux_abi.c \
 *           kernel/linux_futex.c
 *   /tmp/linux_clone_host_test
 *
 * The process-table teardown hooks are counted stubs; the integration half
 * (allocating a descriptor, sharing the group cr3 and syscall stack) lives in
 * process.c and is pinned by the source guard, not reachable on the host.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kernel/process.h"
#include "kernel/linux_thread.h"
#include "kernel/linux_abi.h"

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* The descriptor table linux_thread.c resolves membership through. */
struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count;

void cap_table_teardown(uint32_t pid) { (void)pid; }
void user_destroy_page_table(uint64_t pml4_phys) { (void)pml4_phys; }

static void plant(struct ProcessDescriptor* pd, uint32_t pid, uint64_t cr3) {
    memset(pd, 0, sizeof(*pd));
    pd->pid = pid;
    pd->cr3 = cr3;
    pd->active = 1;
    pd->group_index = LINUX_GROUP_NONE;
}

int main(void) {
    printf("=== linux_clone: E7 TG-1 I2 Go-shape clone planning ===\n\n");

    const uint64_t GO = LINUX_CLONE_GO_FLAGS;
    const uint64_t STACK = 0x0000700000000000ull;
    const uint64_t TLS = 0x0000700000001000ull;
    const uint64_t PARENT_FS = 0x0000700000002000ull;

    /* ── 1. The exact Go shape ───────────────────────────────────────────── */
    {
        struct LinuxClonePlan p;
        CHECK(linux_clone_plan(GO, STACK, 0, 0, PARENT_FS, &p) == 0 &&
              p.flags == GO && p.child_stack == STACK &&
              p.fs_base == PARENT_FS && p.ctid == 0,
              "the six shared flags without SETTLS are accepted and the child inherits the parent fs_base");
        CHECK(linux_clone_plan(GO | LINUX_CLONE_SETTLS, STACK, TLS, 0, PARENT_FS,
                               &p) == 0 &&
              p.fs_base == TLS,
              "SETTLS makes the child's fs_base the TLS argument");
        CHECK(linux_clone_plan(GO | LINUX_CLONE_SETTLS, STACK, 0, 0, PARENT_FS,
                               &p) == 0 && p.fs_base == 0,
              "SETTLS with a zero TLS base is legal (Linux's degenerate base)");
    }

    /* ── 2. Every outside/vfork/parent/namespace/ptrace/clear-TID bit ────── */
    {
        const uint64_t bad[] = {
            0x00000001ull,   /* an unknown low bit                       */
            0x00004000ull,   /* CLONE_VFORK                              */
            0x00008000ull,   /* CLONE_PARENT                             */
            0x00002000ull,   /* CLONE_PTRACE                             */
            0x00020000ull,   /* CLONE_NEWNS (a namespace flag)           */
            0x00100000ull,   /* CLONE_PARENT_SETTID (a parent's write)    */
            0x01000000ull,   /* CLONE_CHILD_SETTID                      */
        };
        int all_refused = 1;
        for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            struct LinuxClonePlan p;
            if (linux_clone_plan(GO | bad[i], STACK, TLS, 0, PARENT_FS, &p) !=
                -LINUX_EINVAL)
                all_refused = 0;
        }
        CHECK(all_refused,
              "every flag outside the allowlist — vfork, parent, ptrace, namespace, PARENT_SETTID, CHILD_SETTID and unknown — is refused EINVAL");
    }

    /* ── 3. Each mandatory shared flag is enforced ───────────────────────── */
    {
        const uint64_t required[] = {
            LINUX_CLONE_VM, LINUX_CLONE_FS, LINUX_CLONE_FILES,
            LINUX_CLONE_SIGHAND, LINUX_CLONE_SYSVSEM, LINUX_CLONE_THREAD,
        };
        int all_refused = 1;
        for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
            struct LinuxClonePlan p;
            if (linux_clone_plan(GO & ~required[i], STACK, TLS, 0, PARENT_FS,
                                 &p) != -LINUX_EINVAL)
                all_refused = 0;
        }
        CHECK(all_refused,
              "removing any one of VM/FS/FILES/SIGHAND/SYSVSEM/THREAD refuses EINVAL");
    }

    /* ── 4. Stack and TLS canonicality ───────────────────────────────────── */
    {
        struct LinuxClonePlan p;
        CHECK(linux_clone_plan(GO, 0, TLS, 0, PARENT_FS, &p) == -LINUX_EINVAL,
              "a zero child stack is refused (the kernel allocates nothing)");
        CHECK(linux_clone_plan(GO, 0x0000800000000000ull, TLS, 0, PARENT_FS, &p) ==
              -LINUX_EINVAL,
              "a non-canonical child stack is refused");
        CHECK(linux_clone_plan(GO, LINUX_USER_ADDR_MAX, TLS, 0, PARENT_FS, &p) == 0,
              "the top canonical user address is accepted as a stack");
        CHECK(linux_clone_plan(GO | LINUX_CLONE_SETTLS, STACK,
                               0x0000800000000000ull, 0, PARENT_FS, &p) ==
              -LINUX_EINVAL,
              "a non-canonical TLS base is refused when SETTLS asks to install it");
        CHECK(linux_clone_plan(GO, STACK, TLS, 0, PARENT_FS, 0) == -LINUX_EINVAL,
              "a NULL plan output is refused");
    }

    /* ── 5. The child's synthetic context ────────────────────────────────── */
    {
        struct TaskContext c;
        memset(&c, 0xAA, sizeof(c));
        linux_clone_build_context(&c, 0x0000000000401234ull, STACK);
        CHECK(c.rax == 0 && c.rsi == STACK && c.rip == 0x0000000000401234ull &&
              c.rsp == STACK,
              "the child returns 0 at the parent's RIP with rsi/rsp on its own stack");
        CHECK(c.cs == 0x23 && c.ss == 0x1B && c.rflags == 0x202,
              "the context carries ring-3 code/data selectors and IF on");
        CHECK(c.r15 == 0 && c.r14 == 0 && c.r13 == 0 && c.r12 == 0 &&
              c.r11 == 0 && c.r10 == 0 && c.r9 == 0 && c.r8 == 0 &&
              c.rbp == 0 && c.rdi == 0 && c.rdx == 0 && c.rcx == 0 &&
              c.rbx == 0,
              "every other GPR is zeroed — no parent register leaks into the new thread");
    }

    /* ── 5b. I5: CLONE_CHILD_CLEARTID and the exit glue's decisions ──────── */
    {
        const uint64_t CTID = 0x0000700000003000ull;
        struct LinuxClonePlan p;

        CHECK(linux_clone_plan(GO | LINUX_CLONE_CHILD_CLEARTID, STACK, 0, CTID,
                               PARENT_FS, &p) == 0 &&
              p.ctid == CTID && p.flags == (GO | LINUX_CLONE_CHILD_CLEARTID),
              "CLEARTID is accepted on the Go shape and the plan carries the word the exit will clear");
        CHECK(linux_clone_plan(GO | LINUX_CLONE_CHILD_CLEARTID, STACK, 0, 0,
                               PARENT_FS, &p) == 0 && p.ctid == 0,
              "CLEARTID with a zero word is legal (Linux's \"no clear\")");
        CHECK(linux_clone_plan(GO | LINUX_CLONE_CHILD_CLEARTID, STACK, 0,
                               0x0000800000000000ull, PARENT_FS, &p) ==
              -LINUX_EINVAL,
              "a non-canonical CLEARTID word is refused BEFORE the child exists (the dying thread must never be handed a bad address)");
        CHECK(linux_clone_plan(GO, STACK, 0, CTID, PARENT_FS, &p) == 0 &&
              p.ctid == 0,
              "without the flag args[3] is not a promise: the plan's ctid stays 0 even when the register holds an address");

        CHECK(linux_ctid_ok(0) && linux_ctid_ok(CTID),
              "linux_ctid_ok accepts Linux's zero and a canonical user word");
        CHECK(!linux_ctid_ok(0x0000800000000000ull) &&
              !linux_ctid_ok(LINUX_USER_ADDR_MAX + 1u),
              "linux_ctid_ok refuses non-canonical words and the first address past the user range");

        CHECK(linux_robust_list_ok(CTID, LINUX_ROBUST_LIST_HEAD_BYTES) == 1 &&
              linux_robust_list_ok(CTID, 16) == 0 &&
              linux_robust_list_ok(CTID, 32) == 0 &&
              linux_robust_list_ok(0, LINUX_ROBUST_LIST_HEAD_BYTES) == 0 &&
              linux_robust_list_ok(CTID + 4, LINUX_ROBUST_LIST_HEAD_BYTES) == 0 &&
              linux_robust_list_ok(0x0000800000000000ull,
                                   LINUX_ROBUST_LIST_HEAD_BYTES) == 0,
              "set_robust_list takes exactly the 24-byte, 8-aligned, canonical head — a foreign sizeof is refused by name");

        const uint32_t T = 117u;
        CHECK(linux_robust_died_word(0, T) == 0,
              "an unlocked robust futex is nobody's: a dying thread leaves it 0");
        CHECK(linux_robust_died_word(T, T) ==
              (LINUX_FUTEX_OWNER_DIED | T),
              "a word this thread owns becomes OWNER_DIED|tid");
        CHECK(linux_robust_died_word(0x80000000u | 9u, T) == 0,
              "a word another thread owns is left alone (its owner's death is not ours to mark)");
        CHECK(linux_robust_died_word(LINUX_FUTEX_OWNER_DIED | T, T) ==
              (LINUX_FUTEX_OWNER_DIED | T),
              "an already-marked word round-trips: the mark survives a second walk");

        uint64_t a = 0;
        CHECK(linux_robust_word_addr(0x0000700000001000ull, 8, &a) == 0 &&
              a == 0x0000700000001008ull,
              "a positive futex_offset moves the word forward from the entry");
        CHECK(linux_robust_word_addr(0x0000700000001000ull, -8, &a) == 0 &&
              a == 0x0000700000000ff8ull,
              "glibc's negative futex_offset moves it back (the mutex's lock word before its list node)");
        CHECK(linux_robust_word_addr(0x10, -0x20, &a) == -LINUX_EINVAL,
              "a negative offset that would underflow the entry is refused, not wrapped into a kernel write");
        CHECK(linux_robust_word_addr(0x00007fffffffffffull, 0x1000, &a) ==
              -LINUX_EINVAL,
              "an offset that leaves the canonical user range is refused");
        CHECK(linux_robust_word_addr(0x0000700000001000ull, 8, 0) == -LINUX_EINVAL,
              "a NULL output is refused");
    }

    /* ── 5c. I5b: the words are taken ONCE, from whichever funnel ─────────── */
    {
        struct ProcessDescriptor d;
        uint64_t ctid = 0, robust = 0;

        plant(&d, 120, 0xABCD5000ull);
        d.tid = 120;
        d.linux_clear_tid  = 0x0000700000004000ull;
        d.linux_robust_list = 0x0000700000004100ull;
        CHECK(linux_thread_take_exit_words(&d, &ctid, &robust) == 1 &&
              ctid == 0x0000700000004000ull &&
              robust == 0x0000700000004100ull &&
              d.linux_clear_tid == 0 && d.linux_robust_list == 0,
              "a thread that owed both words hands them over exactly once and forgets them");
        CHECK(linux_thread_take_exit_words(&d, &ctid, &robust) == 0 &&
              ctid == 0 && robust == 0,
              "the SECOND call takes nothing — the idempotence that lets exit(60) and the group teardown both run the glue");
        CHECK(d.tid == 120 && d.cr3 == 0xABCD5000ull && d.pid == 120,
              "taking the words touches nothing else on the descriptor (a joiner still resolves this thread's identity while it dies)");

        plant(&d, 121, 0xABCD6000ull);
        d.linux_robust_list = 0x0000700000004200ull;
        CHECK(linux_thread_take_exit_words(&d, &ctid, &robust) == 1 &&
              ctid == 0 && robust == 0x0000700000004200ull,
              "one word set is still work: the CTID half stays untouched when only a robust list was registered");

        plant(&d, 122, 0xABCD7000ull);
        CHECK(linux_thread_take_exit_words(&d, &ctid, &robust) == 0,
              "a thread that registered neither word owes nothing (the glue returns without a console line)");
        CHECK(linux_thread_take_exit_words(0, &ctid, &robust) == 0,
              "a NULL descriptor is owed-nothing, not a fault");
        d.linux_clear_tid = 0x0000700000004300ull;
        CHECK(linux_thread_take_exit_words(&d, 0, 0) == 1 &&
              d.linux_clear_tid == 0,
              "the outputs are optional: a caller that only wants the consumption still gets it");
    }

    /* ── 6. Group membership ─────────────────────────────────────────────── */
    {
        memset(proc_table, 0, sizeof(proc_table));
        linux_groups_init();
        plant(&proc_table[0], 100, 0xABCD1000ull);
        CHECK(linux_group_create(&proc_table[0]) == 0,
              "the leader's group is created");

        struct LinuxThreadGroup* g = linux_group_of(&proc_table[0]);
        plant(&proc_table[1], 101, 0xDEAD0000ull);
        CHECK(linux_group_join_thread(g, &proc_table[1]) == 0,
              "a fresh child joins the group");
        CHECK(proc_table[1].tid == 101 && proc_table[1].tgid == 100 &&
              proc_table[1].group_index == proc_table[0].group_index &&
              proc_table[1].is_group_leader == 0 && g->live_threads == 2 &&
              proc_table[1].cr3 == g->cr3,
              "the child is a non-leader with tid=pid, tgid=group id, the shared index and the group's cr3");
        CHECK(proc_group_cap_owner(101) == 100,
              "the child resolves the leader's cap table through the group");

        CHECK(linux_group_join_thread(g, &proc_table[1]) == -1 &&
              g->live_threads == 2,
              "joining the same child twice is refused without changing the count");
        CHECK(linux_group_join_thread(0, &proc_table[1]) == -1,
              "a NULL group is refused");

        /* Fill to the admission cap, then refuse the next joiner. */
        int joined = 0;
        for (int i = 2; i < LINUX_THREADS_MAX; i++) {
            plant(&proc_table[i], (uint32_t)(101 + i), g->cr3);
            if (linux_group_join_thread(g, &proc_table[i]) == 0) joined++;
        }
        CHECK(joined == LINUX_THREADS_MAX - 2 &&
              g->live_threads == LINUX_THREADS_MAX,
              "the group fills to LINUX_THREADS_MAX through joins");
        plant(&proc_table[LINUX_THREADS_MAX], 200, g->cr3);
        CHECK(linux_group_join_thread(g, &proc_table[LINUX_THREADS_MAX]) == -1 &&
              g->live_threads == LINUX_THREADS_MAX &&
              proc_table[LINUX_THREADS_MAX].group_index == LINUX_GROUP_NONE,
              "a join at the cap is refused EAGAIN-equivalent without stamping identity");

        /* I2's departure half: the last-thread rule is the group's destroy
         * trigger, and only the last leave reports it. */
        {
            int last = 0;
            for (int i = 0; i < LINUX_THREADS_MAX; i++)
                last = linux_group_leave_thread(g);
            CHECK(last == 1 && g->live_threads == 0,
                  "leaving every thread reports the last one (the group destroy is owed exactly once)");
            CHECK(linux_group_leave_thread(g) == 0 && g->live_threads == 0,
                  "a leave on an empty group is a no-op, not a second destroy");
            CHECK(linux_group_leave_thread(0) == 0,
                  "a NULL group leave is a no-op");
        }
    }

    CHECK(linux_syscall_class(LINUX_CLONE) == LSC_THREAD,
          "clone (56) has the LSC_THREAD class in the real ABI table");
    CHECK(strcmp(linux_syscall_class_name(LSC_THREAD), "thread") == 0,
          "LSC_THREAD's class name is \"thread\"");
    CHECK(linux_syscall_class(LINUX_GETTID) == LSC_TASK &&
              linux_syscall_class(LINUX_GETPID) == LSC_TASK &&
              linux_syscall_class(LINUX_EXIT) == LSC_TASK,
          "gettid (186), getpid (39) and exit (60) are LSC_TASK — kernel-local identity and lifecycle, never a sidecar request");
    CHECK(strcmp(linux_syscall_class_name(LSC_TASK), "task") == 0,
          "LSC_TASK's class name is \"task\"");

    printf("\nlinux_clone_host_test: %d passed, %d failed\n",
           checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
