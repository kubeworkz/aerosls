/*
 * tests/partition_host_stubs.h — the partition primitives kernel/env_ckpt.c
 * needs (P1a quiesce), for host tests that link env_ckpt.c but NOT the real
 * kernel/partition.c.
 *
 * ─── Why this exists ──────────────────────────────────────────────────────
 * env_ckpt_quiesce_for_capture() freezes each captured environment's partition
 * and env_ckpt_release_capture()/env_ckpt_apply_restored_pauses() thaw the ones
 * they paused, so env_ckpt.c now calls partition_exists/_is_paused/_pause/
 * _resume. The strong definitions live in kernel/partition.c, which drags in
 * cluster/dspp/persist/stream dependencies (see partition.c's own include set)
 * that no env-record host test wants or needs. These are the same four
 * questions cap_create_sidecar_in's E4 gate asks, answered over a tiny table a
 * test can drive directly.
 *
 * ─── Why every stub is weak ──────────────────────────────────────────────
 * The same idiom tests/process_host_stubs.h uses: a test that DOES link
 * kernel/partition.c (most persist/SQL host tests do) gets the real
 * definitions and this header's are shadowed, so a TU can include it
 * harmlessly either way. Do NOT include this header alongside
 * tests/process_host_stubs.h in one TU — that one already carries its own
 * partition_exists/_is_paused pair, and two weak definitions in a single
 * translation unit is a redefinition error, not a link-time choice.
 *
 * ─── The model ────────────────────────────────────────────────────────────
 * A byte-per-partition active table (slot 0, PARTITION_SYSTEM, active from the
 * start; every other slot inactive — exactly the state partition_init() leaves
 * a fresh boot in) and a byte-per-partition paused flag. partition_pause()/
 * _resume() are the ONLY operations that fail, and only for an inactive/
 * out-of-range id — mirroring partition.c, where PARTITION_SYSTEM is itself
 * pausable. That last detail is load-bearing: it is what makes the checkpoint's
 * refusal to freeze PARTITION_SYSTEM a DECISION the test can observe rather
 * than a no-op it would have gotten anyway.
 */
#ifndef PARTITION_HOST_STUBS_H
#define PARTITION_HOST_STUBS_H

#include <stdint.h>

#include "kernel/partition.h"   /* PARTITION_MAX, PARTITION_SYSTEM */

__attribute__((weak)) uint8_t host_partition_active[PARTITION_MAX] = { 1u };
__attribute__((weak)) uint8_t host_partition_paused[PARTITION_MAX];

__attribute__((weak)) int partition_exists(uint32_t partition_id) {
    return partition_id < PARTITION_MAX && host_partition_active[partition_id] != 0;
}
__attribute__((weak)) int partition_is_paused(uint32_t partition_id) {
    return partition_id < PARTITION_MAX && host_partition_paused[partition_id] != 0;
}
__attribute__((weak)) int partition_pause(uint32_t partition_id) {
    if (!partition_exists(partition_id)) return 1;
    host_partition_paused[partition_id] = 1;
    return 0;
}
__attribute__((weak)) int partition_resume(uint32_t partition_id) {
    if (!partition_exists(partition_id)) return 1;
    host_partition_paused[partition_id] = 0;
    return 0;
}

/* Test-side conveniences. Weak like everything else so a test is free to
 * replace them, even though in practice only the tests below call them. */

/* Back to a fresh boot's partition state: 0 active and running, everything
 * else inactive and running. Call between phases, like env_ckpt_reset(). */
__attribute__((weak)) void host_partition_stubs_reset(void) {
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        host_partition_active[i] = (i == PARTITION_SYSTEM) ? 1u : 0u;
        host_partition_paused[i] = 0u;
    }
}

/* Define a partition as active (and running), the way partition_create() would
 * after its slot is chosen. */
__attribute__((weak)) void host_partition_define(uint32_t partition_id) {
    if (partition_id < PARTITION_MAX) {
        host_partition_active[partition_id] = 1u;
        host_partition_paused[partition_id] = 0u;
    }
}

/* The operator-paused state a capture must never disturb (rule 1). */
__attribute__((weak)) void host_partition_set_paused(uint32_t partition_id, int paused) {
    if (partition_id < PARTITION_MAX) host_partition_paused[partition_id] = paused ? 1u : 0u;
}

/* Destroy (deactivate) a partition, the way partition_destroy() does — the
 * state rule 3's drop is about. */
__attribute__((weak)) void host_partition_destroy(uint32_t partition_id) {
    if (partition_id < PARTITION_MAX) {
        host_partition_active[partition_id] = 0u;
        host_partition_paused[partition_id] = 0u;
    }
}

#endif /* PARTITION_HOST_STUBS_H */
