/*
 * tests/env_ckpt_host_stubs.h — the one checkpoint-record call kernel/cap.c
 * makes (E7 increment 20), for host tests that link cap.c but NOT
 * kernel/env_ckpt.c.
 *
 * ─── Why this exists ──────────────────────────────────────────────────────
 * cap_create_sidecar_in() stamps each Linux child into its environment's
 * checkpoint record (entry, stack span, cap count) so the next capture writes
 * a record that can place the task instead of the zeros registration had to
 * leave. The strong definition lives in kernel/env_ckpt.c, which drags in the
 * record table, the payload band and the NVMe staging — dependencies no
 * cap/console/net host test wants, and none of their scenarios reach.
 *
 * ─── Why not tests/process_host_stubs.h ───────────────────────────────────
 * Every test that includes this header already includes that one, and so does
 * tests/cap_create_sidecar_host_test.c — which defines this symbol ITSELF so
 * the stamp's call is observable there. A weak definition in the same
 * translation unit as that one is a redefinition, not a link-time choice, so
 * the stub lives in its own header: included by the tests that only need the
 * link closed, never by the test that owns the definition.
 *
 * ─── The model ────────────────────────────────────────────────────────────
 * A host world has no checkpoint registry, so no record names the child. The
 * honest answer is 0 — "no record with a matching Linux slot was stamped" —
 * which is exactly what kernel/env_ckpt.h documents for an environment outside
 * the registry, and explicitly not an error. The caller ignores the value; it
 * is spelled here so a future caller does not read 0 as a failure.
 */
#ifndef ENV_CKPT_HOST_STUBS_H
#define ENV_CKPT_HOST_STUBS_H

#include "kernel/env_ckpt.h"   /* the signature this stand-in must match */

__attribute__((weak)) int env_ckpt_linux_task_stamp(uint32_t partition_id,
                                                    const char* parent_name,
                                                    const char* child_name,
                                                    uint64_t entry,
                                                    uint64_t stack_bottom,
                                                    uint64_t stack_top,
                                                    uint32_t cap_count) {
    (void)partition_id; (void)parent_name; (void)child_name; (void)entry;
    (void)stack_bottom; (void)stack_top; (void)cap_count;
    return 0;
}

#endif /* ENV_CKPT_HOST_STUBS_H */
