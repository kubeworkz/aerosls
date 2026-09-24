/*
 * tests/process_host_stubs.h — the shared stub set for host tests that
 * compile kernel/process.c into their translation unit (`#include
 * "kernel/process.c"`), following the qemu_sls_test_window.h precedent of a
 * shared test-support header.
 *
 * These are process.c's heavier dependencies — the per-process syscall-stack
 * wiring (per_cpu_data, do_syscall, syscall_return_path) and the Seed Kernel
 * Phase 2 teardown paths (free_physical_ram_frame_for_partition,
 * frame_pool_frame_owner, cap_table_teardown, user_destroy_page_table) —
 * plus the arch/x86/boot.asm bootstrap-stack bounds (stack_bottom/stack_top)
 * that frame_pool_init() reserves by name. No test that drives process.c's
 * scheduling/priority/hold helpers directly ever REACHES any of these; they
 * exist purely so including process.c as source compiles and links.
 *
 * ─── Why one copy ────────────────────────────────────────────────────────
 * Each host test used to carry its own copy, and the copies drifted: when
 * the Seed Kernel Phase 1.5/2 milestones added these references to
 * process.c, scheduler_fairness_host_test.c gained the stubs while
 * workmgmt_phase4_host_test.c did not, and the suite went red at the linker
 * with "undefined reference" instead of failing at the stub boundary. One
 * copy, in one place, is the fix: the next time process.c grows a
 * dependency, this header is the single place it has to be added.
 *
 * ─── Why every stub is weak ──────────────────────────────────────────────
 * cap_lifecycle_host_test.c links cap.c and frame_pool.c for real, and
 * those files carry the STRONG definitions of cap_table_teardown(),
 * frame_pool_frame_owner() and free_physical_ram_frame_for_partition(). A
 * strong stub here would be a multiple-definition link error in that test.
 * Marking this header's stubs weak — the same idiom cap.c already uses for
 * its arch hooks (cap_arch_map_page etc.), which cap_lifecycle_host_test.c
 * relies on to override them — makes the header safe in ANY host-test TU:
 * where a real definition exists it wins, everywhere else the stub is used.
 *
 * ─── Self-contained by design ────────────────────────────────────────────
 * It includes arch/x86/user_paging.h itself (the home of struct PerCPUData,
 * which process.c pulls in transitively), so it can be included from ANY
 * host-test TU, including ones like cap_lifecycle_host_test.c that never
 * include process.c and only need the stack_bottom/stack_top pair. There
 * the process stubs are inert: either shadowed by the real definitions in
 * the link, or unused non-static definitions, which -Wall -Wextra does not
 * warn on.
 */
#ifndef PROCESS_HOST_STUBS_H
#define PROCESS_HOST_STUBS_H

#include <stdint.h>

#include "arch/x86/user_paging.h"
#include "kernel/process.h"   /* PROC_MAX, struct ProcessDescriptor */

/* ─── Seed Kernel Phase 1.5: syscall-stack wiring ───────────────────────── */
/* process.c writes per_cpu_data[0].kernel_rsp/user_rsp when it sets up a
 * process's syscall stack and when it parks/resumes a blocking cap_recv;
 * do_syscall() and syscall_return_path() are the C dispatch and the asm
 * return stub of the blocking-resume path. None are reached by tests that
 * drive the scheduling helpers directly. */
struct PerCPUData per_cpu_data[4] __attribute__((weak));
__attribute__((weak)) uint64_t do_syscall(uint64_t num, void* arg) { (void)num; (void)arg; return 0; }
__attribute__((weak)) void syscall_return_path(void) { for (;;) {} }

/* ─── Seed Kernel Phase 2: teardown paths ───────────────────────────────── */
/* process_exit()/process_kill() free the process's syscall-stack frames,
 * destroy its capability table, and tear down its page tables. Tests that
 * never spawn or kill a process stand in with permissive no-ops. The first
 * three are strong in cap.c/frame_pool.c, so tests that link those (e.g.
 * cap_lifecycle_host_test.c) get the real definitions; tests that only
 * include process.c get these. */
__attribute__((weak)) int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id) { (void)frame; (void)partition_id; return 0; }
__attribute__((weak)) uint32_t frame_pool_frame_owner(uint64_t frame_index) { (void)frame_index; return 0; }
__attribute__((weak)) void cap_table_teardown(uint32_t pid) { (void)pid; }
__attribute__((weak)) void user_destroy_page_table(uint64_t pml4_phys) { (void)pml4_phys; }

/* ─── POSIX-Environments E1: the unified-boot yield's asm half ──────────── */
/* process.c references arch/x86/process_enter.asm's kernel_yield_switch() (the
 * Ring-0 -> Ring-3 cooperative switch) and kernel_resume_control_plane() (the
 * mirror the resume frame's rip points at). No host test that drives the
 * scheduling helpers ever yields, so no-ops that return stand in. Weak, like
 * every other stub here, so a test may override them if it ever wants to. */
__attribute__((weak)) void kernel_yield_switch(uint64_t* rsp_save, uint64_t cr3,
                                               const uint64_t* frame) {
    (void)rsp_save; (void)cr3; (void)frame;
}
__attribute__((weak)) void kernel_resume_control_plane(void) { for (;;) {} }

/* ─── arch/x86/boot.asm's bootstrap-stack bounds ────────────────────────── */
/* frame_pool_init() reserves [stack_bottom, stack_top) by name instead of
 * trusting _kernel_image_end to cover it, so every host test that links
 * frame_pool.c must supply the pair. */
char stack_bottom[16] __attribute__((weak));
char stack_top[16] __attribute__((weak));

/* ─── POSIX-Environments E4: partition-state queries ────────────────────────
 * cap_create_sidecar_in's E4 target-partition gate asks whether a target
 * partition exists and whether it is paused. The strong definitions live in
 * partition.c, which no cap.c host test links (its cluster/dspp/persist
 * dependency chain is irrelevant here), so these weak inert stubs — true only
 * for PARTITION_SYSTEM (0) — close the link. A test that inherits the caller's
 * partition (target 0) never reaches the gate, so they stay inert. (The quota
 * getters are deliberately NOT stubbed here: cap.c's E4 path leaves quota
 * enforcement to the per-frame allocator, and several tests define their own
 * partition_get_frame_usage/_quota, which a weak def here would collide with.) */

/* ...and one test may opt a SECOND partition into existence (and pause it)
 * without touching this default: with host_stub_partition_extra_id left at
 * 0xFFFFFFFF nothing matches and every other test behaves exactly as before,
 * while a test that sets it can exercise the POSITIVE path of a targeted
 * operation and its paused refusal. cap_create_sidecar_host_test.c's E4
 * alloc-region section is the first caller (the targeted charging and its
 * three refusals cannot all be reached through partition 0 alone). */
__attribute__((weak)) uint32_t host_stub_partition_extra_id     = 0xFFFFFFFFu;
__attribute__((weak)) int      host_stub_partition_extra_paused = 0;
__attribute__((weak)) int partition_exists(uint32_t partition_id) {
    return partition_id == 0 || partition_id == host_stub_partition_extra_id;
}
__attribute__((weak)) int partition_is_paused(uint32_t partition_id) {
    return partition_id == host_stub_partition_extra_id &&
           host_stub_partition_extra_paused != 0;
}

/* ─── POSIX-Environments E4 (part 3b): env-manager control channel ───────────
 * cap.c's "kernel.*" wiring calls env_service_register() when it wires the
 * kernel.env.control channel, and console_service_tick() asks
 * env_service_reply_slot() to skip the env channel's reply slot. The strong
 * definitions live in env_service.c, which no cap.c/console_service.c host test
 * links, so these weak inert stubs close the link: register is a no-op (no env
 * service in host context) and reply_slot returns 0 (no slot is the env reply
 * slot, so the console tick drains every slot exactly as before E4). */
__attribute__((weak)) void env_service_register(uint16_t kernel_chan_r, uint16_t kernel_chan_w) { (void)kernel_chan_r; (void)kernel_chan_w; }
__attribute__((weak)) int env_service_reply_slot(uint16_t slot) { (void)slot; return 0; }

/* ─── POSIX-Environments E6: per-environment consoles ───────────────────────
 * cap.c's "kernel.*" wiring calls env_console_register() when it wires a
 * tenant's kernel.env.console peer, and console_service_tick() asks
 * env_console_kernel_slot() to skip that slot (an environment's output must
 * not reach the kernel's serial transcript). The strong definitions live in
 * env_console.c, which no cap.c/console_service.c host test links, so these
 * weak inert stubs close the link: nothing registers (no console exists in
 * host context) and no slot is claimed, so the console tick drains every slot
 * exactly as it did before E6. tests/env_console_host_test.c links the real
 * file, where the strong definitions win. */
__attribute__((weak)) int env_console_register(uint16_t k_rd, uint16_t k_wr, uint32_t partition, uint32_t pid, const char* name) { (void)k_rd; (void)k_wr; (void)partition; (void)pid; (void)name; return 0; }
__attribute__((weak)) int env_console_kernel_slot(uint16_t slot) { (void)slot; return 0; }

/* E6's BIB v3 has cap.c ask the same identity question the console registry
 * asks — "is this sidecar's name an environment's name, and which index?" — so
 * cap_create_sidecar_in() calls env_console_name_index() to fill the header's
 * own_index. A test that links cap.c but not env_console.c gets this inert
 * answer (not an environment), which is exactly what a name like "drv.child.0"
 * parses to anyway. tests/cap_create_sidecar_host_test.c DOES link
 * env_console.c, so its BIB assertions read the real parse. */
__attribute__((weak)) int env_console_name_index(const char* name, uint32_t* out_index) { (void)name; (void)out_index; return 0; }

/* ─── the deferred drains cap_wait_chans' hlt wake now runs ──────────────── */
/* kernel/process.c's cap_wait_chans drains the deferred console tick and
 * the latched device-IRQ notifications at its post-hlt wake (the only
 * process-context cadence on a single-CPU boot). Host tests that include
 * kernel/process.c but not console_service.c/cap.c get counting stubs so
 * "the drain ran" stays observable and the link stays closed. */
__attribute__((weak)) void console_service_deferred_tick(void) { }
__attribute__((weak)) void cap_irq_drain_pending(void)         { }

/* NB: the frame-pool allocators (allocate_physical_ram_frame_for_partition,
 * allocate_contiguous_frames_for_partition, free_contiguous_frames_for_
 * partition, ...) are deliberately NOT stubbed here either, for the same
 * reason as the quota getters below: several tests define their own — some
 * inert, one a RECORDING stub cap_create_sidecar_host_test.c asserts on — and
 * a definition in this header would be a plain redefinition inside those
 * translation units rather than a weak/strong choice at link time. Each test
 * that includes process.c and does not link frame_pool.c carries its own next
 * to its own allocate_physical_ram_frame_for_partition(), which is the pattern
 * that was already in place; tests/syscall_stack_pair_host_test.c links the
 * real frame_pool.c and needs none. */

#endif /* PROCESS_HOST_STUBS_H */
