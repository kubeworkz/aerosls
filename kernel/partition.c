/*
 * partition.c — Phase 8 (v0.3) LPAR groundwork. See partition.h for the
 * contract and AeroSLS-SIMI-ISA-v0.1.md §15 for the design writeup.
 *
 * Deliberately shaped to mirror object_catalog.c's existing role_table
 * pattern (find-or-insert-by-uid, active flag, linear scan over a small
 * fixed table) rather than invent a new idiom — partition assignment
 * *is* role assignment's sibling concept, just gating a different axis
 * (which partition's single-level store you can see) instead of role
 * (what you're allowed to do to what you can see).
 */
#include "partition.h"
#include "kernel_io.h"
#include "persist.h"
#include "process.h"
#include "object_catalog.h"
#include "frame_pool.h"

struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];

/* Phase 14 (LPAR): pause/resume state. Deliberately kept as separate,
 * ephemeral runtime state rather than a field on struct SLSPartitionEntry
 * -- it's scheduling state, not partition identity, so it doesn't survive
 * a reboot (there is no persist_partitions() call anywhere in this file
 * that touches it) any more than a process's "currently running" state
 * does. BSS zero-init = "not paused", the correct default for every
 * partition until partition_pause() is explicitly called, same
 * backward-compatible-by-construction discipline as every other LPAR
 * phase's new state. */
static uint8_t partition_paused[PARTITION_MAX];

/* ─── String helpers (no libc dependency, same discipline as every other
 * kernel source file) ────────────────────────────────────────────────── */
static void pt_strcpy(char* dst, const char* src, int n) {
    int i; for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static int pt_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

void partition_init(void) {
    for (int i = 0; i < PARTITION_MAX; i++) partition_table[i].active = 0;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) partition_assign_table[i].active = 0;

    /* Slot 0 == PARTITION_SYSTEM, pre-populated and permanent — this is
     * the partition every pre-existing object/uid is already implicitly
     * in (see partition.h's backward-compatibility note), so it's given
     * an explicit, named entry rather than just being "whatever id 0
     * happens to mean." */
    partition_table[0].partition_id = PARTITION_SYSTEM;
    pt_strcpy(partition_table[0].name, "system", PARTITION_NAME_LEN);
    partition_table[0].active = 1;

    kernel_serial_print("[PARTITION] LPAR groundwork initialised (1 partition: system).\n");
}

uint32_t partition_create(const char* name) {
    if (!name || pt_strlen(name) >= PARTITION_NAME_LEN) return 0xFFFFFFFFu;
    for (int i = 1; i < PARTITION_MAX; i++) {   /* slot 0 is permanently PARTITION_SYSTEM */
        if (!partition_table[i].active) {
            partition_table[i].partition_id = (uint32_t)i;
            pt_strcpy(partition_table[i].name, name, PARTITION_NAME_LEN);
            partition_table[i].active = 1;
            kernel_serial_printf("[PARTITION] created '%s' (id=%u).\n", name, (unsigned)i);
            persist_partitions();   // Phase 10
            return (uint32_t)i;
        }
    }
    kernel_serial_print("[PARTITION] ERROR: partition table full.\n");
    return 0xFFFFFFFFu;
}

static int partition_id_valid(uint32_t partition_id) {
    if (partition_id == PARTITION_SYSTEM) return 1;
    if (partition_id >= PARTITION_MAX) return 0;
    return partition_table[partition_id].active;
}

int partition_assign_uid(uint32_t uid, uint32_t partition_id) {
    if (uid == 0) {
        kernel_serial_print("[PARTITION] ERROR: uid 0 (kernel) cannot be reassigned.\n");
        return 1;
    }
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf("[PARTITION] ERROR: partition id %u is not defined.\n",
                             (unsigned)partition_id);
        return 1;
    }

    /* Update an existing assignment in place, if any. */
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (partition_assign_table[i].active && partition_assign_table[i].uid == uid) {
            partition_assign_table[i].partition_id = partition_id;
            kernel_serial_printf("[PARTITION] uid %u reassigned to partition %u.\n",
                                 (unsigned)uid, (unsigned)partition_id);
            persist_partitions();   // Phase 10
            return 0;
        }
    }
    /* Otherwise insert into a free slot. */
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (!partition_assign_table[i].active) {
            partition_assign_table[i].uid          = uid;
            partition_assign_table[i].partition_id = partition_id;
            partition_assign_table[i].active       = 1;
            kernel_serial_printf("[PARTITION] uid %u assigned to partition %u.\n",
                                 (unsigned)uid, (unsigned)partition_id);
            persist_partitions();   // Phase 10
            return 0;
        }
    }
    kernel_serial_print("[PARTITION] ERROR: assignment table full.\n");
    return 1;
}

uint32_t partition_get_for_uid(uint32_t uid) {
    if (uid == 0) return PARTITION_SYSTEM;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (partition_assign_table[i].active && partition_assign_table[i].uid == uid)
            return partition_assign_table[i].partition_id;
    }
    return PARTITION_DEFAULT;
}

// ─── Phase 14 (LPAR): partition lifecycle ──────────────────────────────────
int partition_destroy(uint32_t partition_id) {
    if (partition_id == PARTITION_SYSTEM) {
        kernel_serial_print("[PARTITION] ERROR: PARTITION_SYSTEM can never be destroyed.\n");
        return 1;
    }
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot destroy -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }

    // Step 1: kill every process in this partition (process.c, reuses
    // process_kill() per-pid).
    uint32_t killed = process_kill_partition(partition_id);

    // Step 2: vfree every catalog object in this partition
    // (object_catalog.c, mirrors sys_sls_vfree()'s own per-entry actions).
    uint32_t freed_objects = catalog_vfree_partition(partition_id);

    // Step 3: reset frame-usage accounting. ACCOUNTING ONLY -- the underlying
    // physical frames are not actually reclaimed by this call. Gap
    // Remediation Phase F added a real free_physical_ram_frame_for_partition()
    // primitive (frame_pool.h) but does NOT call it here: this partition's
    // held frames aren't tracked as individual addresses anywhere (only an
    // aggregate count), so there's no list of frames to pass it. See
    // frame_pool.h's own comment on free_physical_ram_frame() for the full
    // explanation, including why a naive page-table walk to recover that
    // list would itself be unsafe with this kernel's current
    // user_clone_page_table() design.
    partition_reset_frame_usage(partition_id);

    // Step 4: clear every uid assignment pointing at this partition. Those
    // uids fall back to PARTITION_DEFAULT automatically the next time
    // partition_get_for_uid() is called -- no assignment row means
    // "unassigned," exactly the same fallback every never-assigned uid
    // already gets.
    uint32_t cleared_assignments = 0;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (partition_assign_table[i].active &&
            partition_assign_table[i].partition_id == partition_id) {
            partition_assign_table[i].active = 0;
            cleared_assignments++;
        }
    }

    // Step 5: deactivate the table entry itself, freeing the slot for a
    // future partition_create(), and clear any stale pause flag so a
    // reused slot doesn't inherit "paused" from a previous partition that
    // happened to occupy the same id.
    partition_table[partition_id].active = 0;
    partition_paused[partition_id] = 0;

    kernel_serial_printf(
        "[PARTITION] destroyed partition %u: %u process(es) killed, "
        "%u object(s) freed, %u uid assignment(s) cleared.\n",
        (unsigned)partition_id, (unsigned)killed, (unsigned)freed_objects,
        (unsigned)cleared_assignments);

    persist_partitions();   // Phase 10 -- partition_table[]/partition_assign_table[] mutated
    return 0;
}

int partition_pause(uint32_t partition_id) {
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot pause -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }
    partition_paused[partition_id] = 1;
    kernel_serial_printf(
        "[PARTITION] partition %u paused -- excluded from scheduling "
        "rotation until resumed.\n", (unsigned)partition_id);
    return 0;
}

int partition_resume(uint32_t partition_id) {
    if (!partition_id_valid(partition_id)) {
        kernel_serial_printf(
            "[PARTITION] ERROR: cannot resume -- partition id %u is not an "
            "active, defined partition.\n", (unsigned)partition_id);
        return 1;
    }
    partition_paused[partition_id] = 0;
    kernel_serial_printf("[PARTITION] partition %u resumed.\n", (unsigned)partition_id);
    return 0;
}

int partition_is_paused(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return 0;
    return partition_paused[partition_id];
}

uint64_t sys_sls_partition_destroy(uint32_t partition_id) {
    return (uint64_t)partition_destroy(partition_id);
}
uint64_t sys_sls_partition_pause(uint32_t partition_id) {
    return (uint64_t)partition_pause(partition_id);
}
uint64_t sys_sls_partition_resume(uint32_t partition_id) {
    return (uint64_t)partition_resume(partition_id);
}

uint64_t sys_sls_partition_create(struct SLSPartitionCreateRequest* req) {
    if (!req) return 0xFFFFFFFFu;
    return (uint64_t)partition_create(req->name);
}

uint64_t sys_sls_partition_assign(struct SLSPartitionAssignRequest* req) {
    if (!req) return 1;
    return (uint64_t)partition_assign_uid(req->uid, req->partition_id);
}

void sys_sls_partition_list(void) {
    kernel_serial_print("\n[PARTITION] Defined partitions:\n");
    int shown = 0;
    for (int i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        kernel_serial_printf("  id=%-3u  name=%s\n",
                             (unsigned)partition_table[i].partition_id,
                             partition_table[i].name);
        shown++;
    }
    kernel_serial_printf(" %u partition(s) total.\n\n", (unsigned)shown);

    kernel_serial_print("[PARTITION] UID assignments:\n");
    int nassign = 0;
    for (int i = 0; i < PARTITION_ASSIGN_MAX; i++) {
        if (!partition_assign_table[i].active) continue;
        kernel_serial_printf("  uid=%-6u -> partition %u\n",
                             (unsigned)partition_assign_table[i].uid,
                             (unsigned)partition_assign_table[i].partition_id);
        nassign++;
    }
    kernel_serial_printf(" %u assignment(s), all other uids -> partition %u (default/system).\n\n",
                         (unsigned)nassign, (unsigned)PARTITION_DEFAULT);
}
