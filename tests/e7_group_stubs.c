/*
 * e7_group_stubs.c — the E7 cross-module entries a unit host test never
 * exercises, as weak counted stubs.
 *
 * E7 wired the thread group into three places a unit test links for other
 * reasons:
 *
 *   kernel/cap.c        calls env_ckpt_linux_group_stamp() and
 *                       linux_restore_group_siblings() when a sidecar spawns
 *                       or restores a Linux group, and the two signal-frame
 *                       applies when a handler replies or returns.
 *   kernel/process.c    owns linux_sig_reply_apply(), linux_sig_return_apply(),
 *                       linux_futex_deadline_tick() and calls tss_set_rsp0().
 *   kernel/linux_thread.c  owns linux_group_release_restored().
 *
 * Linking those objects into, say, tests/cap_msg_host_test.c pulls in the
 * partition service, the loader, the object catalog, the IDT/page tables and
 * the scheduler — none of which that test asserts, and several of which it
 * already stubs itself. Before E7 the same files linked cleanly, so the
 * seam here is "the group and signal paths need a boot", and a stub is the
 * honest way to say so.
 *
 * Every symbol is `weak`: a test that links the real kernel object wins
 * silently over these, and a test that links both still resolves the real
 * one. The counters record that a path was reached, without asserting a
 * behaviour the unit test cannot observe.
 */

#include <stdint.h>

#include "arch/x86/gdt.h"
#include "env_ckpt.h"
#include "linux_futex.h"
#include "linux_thread.h"
#include "process.h"

struct E7GroupStubCalls {
    uint32_t tss_set_rsp0;
    uint32_t group_stamp;
    uint32_t group_restore_indexes;
    uint32_t restore_group_siblings;
    uint32_t sig_reply_apply;
    uint32_t sig_return_apply;
    uint32_t futex_waiter_info;
    uint32_t futex_deadline_tick;
    uint32_t group_release_restored;
};
struct E7GroupStubCalls e7_group_stub_calls;

__attribute__((weak)) void tss_set_rsp0(uint64_t top) {
    (void)top;
    e7_group_stub_calls.tss_set_rsp0++;
}

__attribute__((weak)) int env_ckpt_linux_group_stamp(uint32_t partition_id,
                                                    const char* parent_name,
                                                    const char* child_name,
                                                    uint32_t group_id,
                                                    uint16_t thread_index,
                                                    uint8_t is_leader) {
    (void)partition_id; (void)parent_name; (void)child_name;
    (void)group_id; (void)thread_index; (void)is_leader;
    e7_group_stub_calls.group_stamp++;
    return 0;   /* no matching Linux slot: the caller keeps its record as-is */
}

__attribute__((weak)) int env_ckpt_linux_group_restore_indexes(
    uint32_t partition_id, const char* leader_name, uint32_t group_id,
    uint16_t* out_indexes, uint32_t cap) {
    (void)partition_id; (void)leader_name; (void)group_id;
    (void)out_indexes; (void)cap;
    e7_group_stub_calls.group_restore_indexes++;
    return 0;   /* no record names this group: nothing to replay */
}

__attribute__((weak)) int linux_restore_group_siblings(
    struct ProcessDescriptor* leader, const uint16_t* thread_indexes,
    uint32_t n) {
    (void)leader; (void)thread_indexes; (void)n;
    e7_group_stub_calls.restore_group_siblings++;
    return 0;
}

__attribute__((weak)) int linux_sig_reply_apply(struct ProcessDescriptor* pd,
                                                const uint8_t* pay,
                                                uint32_t n) {
    (void)pd; (void)pay; (void)n;
    e7_group_stub_calls.sig_reply_apply++;
    return -1;  /* refused: delivery needs the frame and the resume path */
}

__attribute__((weak)) int linux_sig_return_apply(struct ProcessDescriptor* pd) {
    (void)pd;
    e7_group_stub_calls.sig_return_apply++;
    return -1;  /* refused: no handler frame was ever installed here */
}

__attribute__((weak)) int linux_futex_waiter_info(uint32_t tid,
                                                  struct LinuxFutexWaitInfo* out) {
    (void)tid;
    if (out) {
        out->group_id = 0;
        out->tid = 0;
        out->uaddr = 0;
        out->deadline = 0;
    }
    e7_group_stub_calls.futex_waiter_info++;
    return 0;   /* this tid is parked in nothing */
}

__attribute__((weak)) void linux_futex_deadline_tick(void) {
    e7_group_stub_calls.futex_deadline_tick++;
}

__attribute__((weak)) uint32_t linux_group_release_restored(uint32_t group_id) {
    (void)group_id;
    e7_group_stub_calls.group_release_restored++;
    return 0;
}
