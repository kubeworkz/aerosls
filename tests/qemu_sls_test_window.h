/*
 * qemu_sls_test_window.h — force-included (via -include) into BOTH the host
 * test and the real kernel/qemu_sls_mmu.c when building
 * tests/qemu_sls_mmu_host_test.c.
 *
 * The production GPA window sits at 32 TiB, which a host process cannot
 * address. qemu_sls_mmu.h therefore wraps the constant in #ifndef so this
 * header can point it at a real buffer instead. Both translation units must
 * agree, which is why it is force-included rather than #defined on the command
 * line: a -D reaches the .c, but the extern declaration it needs does not.
 *
 * Relocating the window does NOT weaken the test. The production value is a
 * separate assertion in the test itself -- 32 TiB, canonical, page-aligned,
 * with the whole 256 MiB guest span below the 128 TiB hole -- so a wrong
 * constant fails there rather than hiding here.
 */
#ifndef QEMU_SLS_TEST_WINDOW_H
#define QEMU_SLS_TEST_WINDOW_H

#include <stdint.h>

extern uint64_t qemu_sls_test_gpa_base;
#define QEMU_GPA_HOST_BASE qemu_sls_test_gpa_base

/* ─── The GUEST window, one PML4 slot above the emulator's ─────────────────
 * The two windows must be DIFFERENT PML4 slots or shadow_va_in_window() can
 * no longer tell an emulator access from a guest one -- which is the whole
 * point of splitting them. +512 GiB is exactly one slot.
 *
 * Note the asymmetry, and that it is safe: the emulator window has to point
 * at real memory because gpa_to_hva() reads through it, but the guest window
 * is never dereferenced by qemu_sls_mmu.c. It is only ever an address handed
 * to user_map_page(), which the test stubs. So it can be a synthetic value
 * far outside anything this process has mapped. */
extern uint64_t qemu_sls_test_guest_window;
#define QEMU_GUEST_WINDOW_BASE qemu_sls_test_guest_window

#endif
