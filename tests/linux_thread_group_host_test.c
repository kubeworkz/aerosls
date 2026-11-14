/*
 * linux_thread_group_host_test.c — M4.1's last-reference teardown teeth.
 *
 * The group object is deliberately independent of the scheduler: it only
 * needs a ProcessDescriptor's pid/cr3 and its per-thread fields. This test
 * therefore links the REAL kernel/linux_threads.c with no kernel, page
 * table, or boot, and drives the exact lifecycle the M4.2 clone path will
 * use:
 *
 *   leader + two attached threads -> refs 3, live 3;
 *   leader exits first           -> NO address-space destruction;
 *   one thread exits             -> still NO destruction;
 *   last thread exits            -> exactly one PML4 returned for destroy;
 *   double detach                -> handled 0, nothing destroyed twice;
 *   group slot reuse             -> only after refs/live reach zero.
 *
 * The test also pins the group table bound, the leader/tid fields, the
 * cr3 mismatch refusal, and the non-Linux legacy path (detach returns 0).
 *
 * Build and run:
 *   gcc -no-pie -std=c11 -Wall -Wextra -I . -I kernel \
 *       -o /tmp/linux_thread_group_host_test \
 *       tests/linux_thread_group_host_test.c kernel/linux_threads.c
 *   /tmp/linux_thread_group_host_test
 */
#include "kernel/linux_threads.h"
#include "kernel/process.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, what) do { \
    checks++; \
    if (!(cond)) { \
        printf("FAIL: %s\n", (what)); \
        failures++; \
    } \
} while (0)

/* The test's caller-side teardown oracle. The module returns the PML4; the
 * real process_exit()/process_kill()/deferred teardown call
 * user_destroy_page_table() after cap_table_teardown(). Counting here is
 * what makes "exactly once, only on the last reference" observable. */
static unsigned destroyed_count;
static uint64_t destroyed_cr3;

static void destroy_like_process_exit(uint64_t cr3) {
    if (cr3 == 0) return;
    destroyed_count++;
    destroyed_cr3 = cr3;
}

static void pd_init(struct ProcessDescriptor *pd, uint32_t pid, uint64_t cr3) {
    memset(pd, 0, sizeof(*pd));
    pd->pid = pid;
    pd->cr3 = cr3;
}

static void test_multi_thread_lifecycle(void) {
    struct ProcessDescriptor leader, thread1, thread2;
    uint32_t group;

    linux_thread_groups_reset();
    destroyed_count = 0;
    destroyed_cr3 = 0;

    pd_init(&leader, 100, 0x1000);
    group = linux_thread_group_create(&leader, 0x1000);
    CHECK(group == 1, "the first Linux group gets id 1");
    CHECK(leader.linux_tgid == 1, "the leader stores its group id");
    CHECK(leader.linux_tid == 100, "the leader's TID is its pid");
    CHECK(leader.linux_is_thread == 0, "the leader is not marked as a thread");
    CHECK(leader.linux_clear_child_tid == 0, "a fresh leader has no clear-child-tid");
    CHECK(linux_thread_group_refs(1) == 1, "a fresh group has one cr3 reference");
    CHECK(linux_thread_group_live(1) == 1, "a fresh group has one live thread");
    CHECK(linux_thread_group_tgid(1) == 100, "the group TGID is the leader pid");
    CHECK(linux_thread_group_cr3(1) == 0x1000, "the group stores the leader cr3");

    pd_init(&thread1, 101, 0x1000);
    CHECK(linux_thread_group_attach(&thread1, 1, 101) == 1,
          "thread 101 joins the leader's group");
    CHECK(thread1.linux_tgid == 1, "thread 101 stores the group id");
    CHECK(thread1.linux_tid == 101, "thread 101 gets its own TID");
    CHECK(thread1.linux_is_thread == 1, "thread 101 is a non-leader thread");
    CHECK(linux_thread_group_refs(1) == 2, "attaching a thread takes a cr3 reference");
    CHECK(linux_thread_group_live(1) == 2, "attaching a thread raises the live count");

    pd_init(&thread2, 102, 0x1000);
    CHECK(linux_thread_group_attach(&thread2, 1, 102) == 1,
          "thread 102 joins the same group");
    CHECK(linux_thread_group_refs(1) == 3, "three threads hold three references");
    CHECK(linux_thread_group_live(1) == 3, "three threads are live");

    /* A leader exits first. This is the exact M4 failure the group object
     * exists to prevent: the address space must remain alive. */
    uint64_t dead = 0;
    CHECK(linux_thread_group_detach(&leader, &dead) == 1,
          "the leader's detach is handled by the group");
    CHECK(dead == 0, "the leader's exit does not destroy the shared PML4");
    CHECK(leader.linux_tgid == 0 && leader.linux_tid == 0 &&
          leader.linux_is_thread == 0,
          "the leader's per-thread fields are cleared");
    CHECK(linux_thread_group_used(1) == 1, "the group survives the leader's exit");
    CHECK(linux_thread_group_refs(1) == 2, "two references remain after the leader exits");
    CHECK(linux_thread_group_live(1) == 2, "two threads remain live");
    CHECK(linux_thread_group_tgid(1) == 100, "the TGID stays the original leader pid");

    CHECK(linux_thread_group_detach(&thread1, &dead) == 1,
          "thread 101's detach is handled");
    CHECK(dead == 0, "the second-to-last exit does not destroy the PML4");
    CHECK(linux_thread_group_refs(1) == 1, "one reference remains");
    CHECK(linux_thread_group_live(1) == 1, "one thread remains live");

    CHECK(linux_thread_group_detach(&thread2, &dead) == 1,
          "the last thread's detach is handled");
    CHECK(dead == 0x1000, "the last detach returns the shared PML4 for destruction");
    CHECK(linux_thread_group_refs(1) == 0, "the last detach releases the last reference");
    CHECK(linux_thread_group_live(1) == 0, "the last detach clears the live count");
    CHECK(linux_thread_group_used(1) == 0, "the group is free after the last reference");
    CHECK(linux_thread_group_tgid(1) == 0, "a freed group has no stale TGID");
    CHECK(linux_thread_group_cr3(1) == 0, "a freed group has no stale cr3");
    CHECK(thread2.linux_tgid == 0 && thread2.linux_tid == 0,
          "the last thread's fields are cleared");

    destroy_like_process_exit(dead);
    CHECK(destroyed_count == 1, "the caller destroys exactly one address space");
    CHECK(destroyed_cr3 == 0x1000, "the destroyed address space is the shared PML4");

    dead = 0xDEAD;
    CHECK(linux_thread_group_detach(&thread2, &dead) == 0,
          "a double detach is not handled by the group");
    CHECK(dead == 0, "a double detach returns no address space to destroy");
    CHECK(destroyed_count == 1, "a double detach cannot destroy a second time");
}

static void test_second_group_and_slot_reuse(void) {
    struct ProcessDescriptor a, b, c;
    uint64_t dead = 0;

    linux_thread_groups_reset();
    pd_init(&a, 200, 0x2000);
    CHECK(linux_thread_group_create(&a, 0x2000) == 1, "group A is created");
    pd_init(&b, 201, 0x3000);
    CHECK(linux_thread_group_create(&b, 0x3000) == 2, "group B gets a distinct slot");
    CHECK(linux_thread_group_tgid(1) == 200, "group A keeps its TGID");
    CHECK(linux_thread_group_tgid(2) == 201, "group B keeps its TGID");
    CHECK(linux_thread_group_cr3(2) == 0x3000, "group B keeps its own cr3");

    /* A group slot must not be reused while its reference is live. */
    pd_init(&c, 202, 0x4000);
    CHECK(linux_thread_group_create(&c, 0x4000) == 3,
          "a live group's slot is not handed to a new leader");

    CHECK(linux_thread_group_detach(&a, &dead) == 1 && dead == 0x2000,
          "group A's only thread returns its PML4");
    dead = 0;
    pd_init(&c, 203, 0x5000);
    CHECK(linux_thread_group_create(&c, 0x5000) == 1,
          "the freed slot is reused by the next leader");
    CHECK(linux_thread_group_tgid(1) == 203, "the reused slot has the new TGID");
    CHECK(linux_thread_group_cr3(1) == 0x5000, "the reused slot has the new cr3");
    CHECK(linux_thread_group_refs(1) == 1, "the reused slot starts at one reference");
    CHECK(linux_thread_group_detach(&c, &dead) == 1 && dead == 0x5000,
          "the reused group tears down normally");

    dead = 0;
    CHECK(linux_thread_group_detach(&b, &dead) == 1 && dead == 0x3000,
          "group B still tears down its own cr3");
}

static void test_refusals_and_non_linux_path(void) {
    struct ProcessDescriptor leader, thread, plain;
    uint64_t dead = 0x99;

    linux_thread_groups_reset();
    pd_init(&leader, 300, 0x6000);
    CHECK(linux_thread_group_create(0, 0x6000) == 0, "no descriptor is refused");
    CHECK(linux_thread_group_create(&leader, 0) == 0, "a zero cr3 is refused");
    CHECK(linux_thread_group_create(&leader, 0x6000) == 1, "the valid leader is accepted");
    CHECK(linux_thread_group_create(&leader, 0x6000) == 0,
          "a descriptor already in a group is refused");

    pd_init(&thread, 301, 0x6000);
    CHECK(linux_thread_group_attach(&thread, 99, 301) == 0,
          "an unknown group id is refused");
    CHECK(thread.linux_tgid == 0, "a refused attach leaves no group id");
    CHECK(linux_thread_group_attach(&thread, 1, 0) == 0,
          "a zero TID is refused");
    CHECK(linux_thread_group_attach(&thread, 1, 301) == 1,
          "a matching thread is accepted");

    CHECK(linux_thread_group_attach(&thread, 1, 302) == 0,
          "a descriptor that is already grouped is refused");
    struct ProcessDescriptor mismatch;
    pd_init(&mismatch, 303, 0x7000);
    CHECK(linux_thread_group_attach(&mismatch, 1, 303) == 0,
          "a different cr3 cannot join the group");

    pd_init(&plain, 304, 0x8000);
    dead = 0x99;
    CHECK(linux_thread_group_detach(&plain, &dead) == 0,
          "a non-Linux descriptor takes the legacy path");
    CHECK(dead == 0, "the legacy path returns no group-owned PML4");
    CHECK(plain.pid == 304 && plain.cr3 == 0x8000,
          "the legacy path does not mutate the descriptor's identity");

    dead = 0x99;
    CHECK(linux_thread_group_detach(0, &dead) == 0, "a null descriptor is refused");
    CHECK(dead == 0, "a null descriptor returns no PML4");
    CHECK(linux_thread_group_refs(99) == 0, "an unknown group has no references");
    CHECK(linux_thread_group_used(99) == 0, "an unknown group is not used");
}

static void test_table_bound(void) {
    struct ProcessDescriptor pds[LINUX_THREAD_GROUP_MAX + 1];
    uint64_t cr3s[LINUX_THREAD_GROUP_MAX + 1];
    uint64_t dead = 0;

    linux_thread_groups_reset();
    for (uint32_t i = 0; i < LINUX_THREAD_GROUP_MAX; i++) {
        cr3s[i] = 0x10000ULL + (uint64_t)i * 0x1000ULL;
        pd_init(&pds[i], 400 + i, cr3s[i]);
        CHECK(linux_thread_group_create(&pds[i], cr3s[i]) == i + 1,
              "every group slot can be created in order");
    }
    pd_init(&pds[LINUX_THREAD_GROUP_MAX], 999, 0x9000);
    CHECK(linux_thread_group_create(&pds[LINUX_THREAD_GROUP_MAX], 0x9000) == 0,
          "a full group table refuses a new group");

    for (uint32_t i = 0; i < LINUX_THREAD_GROUP_MAX; i++) {
        dead = 0;
        CHECK(linux_thread_group_detach(&pds[i], &dead) == 1 && dead == cr3s[i],
              "each group's last reference returns its own cr3");
        CHECK(linux_thread_group_used(i + 1) == 0,
              "each group is freed after its last reference");
    }
}

static void test_reset_clears_live_groups(void) {
    struct ProcessDescriptor leader, thread;
    uint64_t dead = 0xFF;

    linux_thread_groups_reset();
    pd_init(&leader, 500, 0xA000);
    CHECK(linux_thread_group_create(&leader, 0xA000) == 1, "reset test group is created");
    pd_init(&thread, 501, 0xA000);
    CHECK(linux_thread_group_attach(&thread, 1, 501) == 1, "reset test thread attaches");
    linux_thread_groups_reset();
    CHECK(linux_thread_group_used(1) == 0, "reset frees every group slot");
    CHECK(linux_thread_group_refs(1) == 0, "reset clears the reference count");
    CHECK(linux_thread_group_live(1) == 0, "reset clears the live count");
    /* Reset is a boot/test boundary, not a teardown: a descriptor that still
     * names a cleared group takes the legacy path rather than freeing an
     * address space the reset already forgot. */
    CHECK(linux_thread_group_detach(&thread, &dead) == 0,
          "a descriptor whose group was reset takes the legacy path");
    CHECK(dead == 0, "reset does not fabricate a destroy request");
}

int main(void) {
    printf("linux_thread_group_host_test — M4.1 shared-CR3 lifetime\n");
    test_multi_thread_lifecycle();
    test_second_group_and_slot_reuse();
    test_refusals_and_non_linux_path();
    test_table_bound();
    test_reset_clears_live_groups();

    printf("%d checks, %d failure(s)\n", checks, failures);
    if (failures != 0) {
        printf("linux_thread_group_host_test: FAIL\n");
        return 1;
    }
    printf("linux_thread_group_host_test: PASS\n");
    return 0;
}
