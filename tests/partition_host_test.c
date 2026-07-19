/*
 * partition_host_test.c — Phase 8 verification: a standalone host-buildable
 * unit test for kernel/partition.c's core logic (partition_create/
 * partition_assign_uid/partition_get_for_uid), compiled and linked directly
 * against the REAL, unmodified kernel/partition.c — not a reimplementation.
 *
 * As of Phase 8, kernel/partition.c had exactly one external dependency
 * (kernel_io.h's kernel_serial_print/kernel_serial_printf). Phase 10 added
 * a persist_partitions() call, and Phase 14 added partition_destroy()'s
 * three cross-subsystem calls (process_kill_partition(),
 * catalog_vfree_partition(), partition_reset_frame_usage()) — none of
 * which this test exercises, so they're stubbed the same way
 * persist_partition_host_test.c and ipc_partition_host_test.c already
 * stub them. Still cheap enough to link and actually EXECUTE, giving
 * stronger verification than the compile-check-only precedent every prior
 * phase's kernel-side wiring has settled for (see
 * AeroSLS-TIMI-ISA-v0.1.md §15 for the write-up).
 *
 * Not part of any Makefile target — a one-off verification artifact, built
 * and run directly:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -o /tmp/partition_host_test \
 *       tests/partition_host_test.c kernel/partition.c
 *   /tmp/partition_host_test
 */
#include "kernel/partition.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ─── Stubs for partition.c's external dependencies (see header comment) ── */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void persist_partitions(void) { /* Phase 10's persistence hook — irrelevant here */ }
uint32_t process_kill_partition(uint32_t partition_id) { (void)partition_id; return 0; }
uint32_t catalog_vfree_partition(uint32_t partition_id) { (void)partition_id; return 0; }
int partition_reset_frame_usage(uint32_t partition_id) { (void)partition_id; return 0; }

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    partition_init();

    /* ── Defaults, no assignments made yet ────────────────────────────── */
    CHECK(partition_get_for_uid(0) == PARTITION_SYSTEM, "uid 0 -> PARTITION_SYSTEM");
    CHECK(partition_get_for_uid(999) == PARTITION_DEFAULT, "unassigned uid -> PARTITION_DEFAULT");
    CHECK(PARTITION_SYSTEM == PARTITION_DEFAULT, "SYSTEM and DEFAULT are the same value (backward-compat invariant)");

    /* ── Creating partitions ──────────────────────────────────────────── */
    uint32_t pid_a = partition_create("tenant-a");
    uint32_t pid_b = partition_create("tenant-b");
    CHECK(pid_a != 0xFFFFFFFFu, "partition_create('tenant-a') succeeds");
    CHECK(pid_b != 0xFFFFFFFFu, "partition_create('tenant-b') succeeds");
    CHECK(pid_a != pid_b, "two created partitions get distinct ids");
    CHECK(pid_a != PARTITION_SYSTEM && pid_b != PARTITION_SYSTEM,
          "created partitions never collide with PARTITION_SYSTEM (slot 0 reserved)");

    /* ── Assignment: insert, lookup, update in place ──────────────────── */
    CHECK(partition_assign_uid(42, pid_a) == 0, "assign uid 42 -> tenant-a succeeds");
    CHECK(partition_get_for_uid(42) == pid_a, "uid 42 now resolves to tenant-a");
    CHECK(partition_assign_uid(42, pid_b) == 0, "reassign uid 42 -> tenant-b succeeds (update in place)");
    CHECK(partition_get_for_uid(42) == pid_b, "uid 42 now resolves to tenant-b, not stuck on tenant-a");

    /* ── Rejections ────────────────────────────────────────────────────── */
    CHECK(partition_assign_uid(0, pid_a) != 0, "cannot reassign uid 0 (kernel) — rejected");
    CHECK(partition_get_for_uid(0) == PARTITION_SYSTEM, "uid 0 still PARTITION_SYSTEM after rejected attempt");
    CHECK(partition_assign_uid(7, 0xDEADBEEF) != 0, "assigning to an undefined partition id is rejected");
    CHECK(partition_get_for_uid(7) == PARTITION_DEFAULT, "uid 7 still unassigned/default after rejected attempt");

    char toolong[PARTITION_NAME_LEN + 8];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    CHECK(partition_create(toolong) == 0xFFFFFFFFu, "partition_create rejects an oversized name");

    /* ── Capacity: fill the partition table ───────────────────────────── */
    int created = 0;
    char namebuf[PARTITION_NAME_LEN];
    for (;;) {
        snprintf(namebuf, sizeof(namebuf), "p%d", created);
        uint32_t id = partition_create(namebuf);
        if (id == 0xFFFFFFFFu) break;
        created++;
        if (created > PARTITION_MAX + 4) { printf("FAIL: partition_create never reports full\n"); g_fail++; break; }
    }
    /* slot 0 is PARTITION_SYSTEM; 2 were already created above (tenant-a/b);
     * remaining free slots = PARTITION_MAX - 1 - 2 */
    CHECK(created == PARTITION_MAX - 3, "partition table fills to exactly PARTITION_MAX (accounting for system + 2 earlier)");
    CHECK(partition_create("one-too-many") == 0xFFFFFFFFu, "partition_create fails once table is full");

    /* ── Capacity: fill the assignment table, confirm updates still work
     * even when the table is full (only NEW inserts should be blocked) ── */
    int assigned = 0;
    for (uint32_t uid = 1000; uid < 1000 + PARTITION_ASSIGN_MAX + 4; uid++) {
        int rc = partition_assign_uid(uid, pid_a);
        if (rc != 0) break;
        assigned++;
    }
    /* uid 42 already occupies one slot from earlier, so capacity for NEW
     * uids is PARTITION_ASSIGN_MAX - 1 */
    CHECK(assigned == PARTITION_ASSIGN_MAX - 1, "assignment table fills to exactly PARTITION_ASSIGN_MAX (accounting for uid 42's earlier slot)");
    CHECK(partition_assign_uid(42, pid_a) == 0, "updating an EXISTING uid's assignment still succeeds when table is full");
    CHECK(partition_get_for_uid(42) == pid_a, "uid 42's update took effect");
    CHECK(partition_assign_uid(99999, pid_a) != 0, "a genuinely NEW uid is rejected once the assignment table is full");

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
