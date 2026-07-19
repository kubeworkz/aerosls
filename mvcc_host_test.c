/*
 * mvcc_host_test.c — Phase 21 (relational layer) verification: a standalone
 * host-buildable test for kernel/mvcc.c's snapshot-isolation transactions,
 * linked against the REAL, unmodified kernel/mvcc.c, kernel/rowstore.c, and
 * kernel/persist.c — not a reimplementation.
 *
 * "Concurrent transactions" in a single-threaded host test means
 * INTERLEAVED calls: begin T1, begin T2, issue operations against each in
 * whatever order a real concurrent schedule could produce, and check the
 * result matches what real snapshot isolation guarantees. This is the same
 * "simulate concurrent transactions" bar Phase 21's own roadmap
 * verification plan named, and the same shape Phase 12's fairness-logic
 * host test already proved out for a different subsystem.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I kernel -I drivers \
 *       -o /tmp/mvcc_host_test mvcc_host_test.c \
 *       kernel/mvcc.c kernel/rowstore.c kernel/persist.c \
 *       kernel/row_constraint.c kernel/row_journal.c
 *   /tmp/mvcc_host_test
 *
 * Phase 23 update: mvcc.c now calls into kernel/row_constraint.c (constraint
 * enforcement) and kernel/row_journal.c (audit trail) automatically from
 * mvcc_row_insert()/_update()/_delete(), so this test's link line now
 * includes both -- with row_constraint_init()/row_journal_init() never
 * called here, both subsystems have zero registered constraints/
 * attachments, so every call into them is a guaranteed no-op (see their own
 * header comments) and every scenario below still passes unchanged.
 */
#include "kernel/object_catalog.h"
#include "kernel/loader.h"
#include "kernel/partition.h"
#include "kernel/rowstore.h"
#include "kernel/mvcc.h"
#include "user/permissions.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

struct SLSObjectEntry  object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
struct SLSObjectSchema object_schemas[CATALOG_MAX_OBJECTS];
struct SLSRoleEntry    role_table[ROLE_TABLE_MAX];
struct SLSObjectRecord object_records[CATALOG_MAX_OBJECTS];
struct ServiceBinary   service_binaries[MAX_SERVICE_BINARIES];
struct SLSPartitionEntry  partition_table[PARTITION_MAX];
struct SLSPartitionAssign partition_assign_table[PARTITION_ASSIGN_MAX];
void catalog_after_restore(void) { /* no-op for this test */ }
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
void kernel_serial_print_hex64(unsigned long long v) { (void)v; }

static int g_access_force_deny = 0;
int catalog_check_access(uint32_t uid, const char* obj_name, uint32_t needed_perm) {
    (void)uid; (void)obj_name; (void)needed_perm;
    return g_access_force_deny ? 0 : 1;
}

/* rowstore.c (Phase 17) unconditionally calls these three hooks after every
 * mutation; this test has no interest in row_index.c, so they're stubbed
 * as no-ops purely so the linker resolves them -- matching
 * rowstore_host_test.c's own established precedent for this exact link
 * requirement. */
void row_index_notify_insert(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}
void row_index_notify_update(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* old_values, const struct RowValues* new_values,
                             const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)old_values; (void)new_values; (void)layout;
}
void row_index_notify_delete(uint64_t table_object_id, struct RowId id,
                             const struct RowValues* values, const struct RowTableLayout* layout) {
    (void)table_object_id; (void)id; (void)values; (void)layout;
}
void* allocate_physical_ram_frame(void) { return malloc(4096); }

#define FAKE_NVME_MAX_FRAMES 512
static struct { uint64_t lba; uint8_t data[4096]; int used; } g_fake_nvme[FAKE_NVME_MAX_FRAMES];
void* io_sq = (void*)1;
void* io_cq = (void*)1;
static int find_or_alloc_frame(uint64_t lba) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) return i;
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (!g_fake_nvme[i].used) { g_fake_nvme[i].used = 1; g_fake_nvme[i].lba = lba; return i; }
    return -1;
}
int nvme_write_sync(uint64_t lba, const void* buf) {
    int idx = find_or_alloc_frame(lba);
    if (idx < 0) return 1;
    memcpy(g_fake_nvme[idx].data, buf, 4096);
    return 0;
}
int nvme_read_sync(uint64_t lba, void* buf) {
    for (int i = 0; i < FAKE_NVME_MAX_FRAMES; i++)
        if (g_fake_nvme[i].used && g_fake_nvme[i].lba == lba) { memcpy(buf, g_fake_nvme[i].data, 4096); return 0; }
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
    else          { printf("ok:   %s\n", msg); } \
} while (0)

static void make_table(const char* name, uint64_t object_id) {
    int slot = (int)object_catalog_count++;
    memset(&object_catalog[slot], 0, sizeof(object_catalog[slot]));
    strcpy(object_catalog[slot].name, name);
    object_catalog[slot].type = OBJ_TYPE_DB_TABLE;
    object_catalog[slot].object_id = object_id;
    object_catalog[slot].active = 1;

    memset(&object_schemas[slot], 0, sizeof(object_schemas[slot]));
    strcpy(object_schemas[slot].fields[0].key, "id");
    object_schemas[slot].fields[0].type = FIELD_TYPE_UINT64;
    object_schemas[slot].fields[0].active = 1;
    strcpy(object_schemas[slot].fields[1].key, "value");
    object_schemas[slot].fields[1].type = FIELD_TYPE_STRING;
    object_schemas[slot].fields[1].active = 1;
    object_schemas[slot].field_count = 2;
    rowstore_create_table(name);
}

static struct RowValues rv(const char* id, const char* value) {
    struct RowValues v;
    v.count = 2;
    strcpy(v.values[0], id);
    strcpy(v.values[1], value);
    return v;
}

struct scan_ctx { char values[64][64]; uint32_t count; };
static void scan_cb(struct MvccRowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct scan_ctx* ctx = (struct scan_ctx*)ctxp;
    if (ctx->count < 64) { strncpy(ctx->values[ctx->count], v->values[1], 63); ctx->count++; }
}
// Separate callback for direct rowstore_table_scan() calls (Phase 16's own
// RowId, not MvccRowId) -- used only to independently verify physical
// cleanup after a rollback, bypassing mvcc.c's version bookkeeping entirely.
static void phys_count_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id; (void)v;
    uint32_t* count = (uint32_t*)ctxp;
    (*count)++;
}

int main(void) {
    rowstore_init();
    mvcc_init();
    make_table("accounts", 0xA001);

    /* ── Scenario 1: basic single-txn insert, commit, and a fresh read. ──── */
    uint64_t t1 = mvcc_begin();
    CHECK(t1 != 0, "s1: mvcc_begin() returns a nonzero txn_id");
    struct MvccRowId row_a;
    struct RowValues va = rv("1", "alpha");
    CHECK(mvcc_row_insert(t1, 1, "accounts", &va, &row_a) == MVCC_OK, "s1: insert under t1 succeeds");
    struct RowValues got;
    CHECK(mvcc_row_get(t1, 1, "accounts", row_a, &got) == MVCC_OK && strcmp(got.values[1], "alpha") == 0,
          "s1: t1 can read its own uncommitted insert (read-your-own-writes)");
    CHECK(mvcc_commit(t1) == MVCC_OK, "s1: commit succeeds");
    uint64_t t2 = mvcc_begin();
    CHECK(mvcc_row_get(t2, 1, "accounts", row_a, &got) == MVCC_OK && strcmp(got.values[1], "alpha") == 0,
          "s1: a fresh transaction begun after commit sees the committed row");
    mvcc_commit(t2);

    /* ── Scenario 2: snapshot isolation -- a transaction that began BEFORE
     * a commit never sees it, even after the commit happens; one that
     * begins AFTER does. ──────────────────────────────────────────────────── */
    uint64_t t3 = mvcc_begin();   // snapshot taken now, before the insert below
    uint64_t t4 = mvcc_begin();
    struct MvccRowId row_b;
    struct RowValues vb = rv("2", "beta");
    CHECK(mvcc_row_insert(t4, 1, "accounts", &vb, &row_b) == MVCC_OK, "s2: insert under t4 succeeds");
    CHECK(mvcc_row_get(t3, 1, "accounts", row_b, &got) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s2: t3 (older snapshot, uncommitted sibling) cannot see t4's uncommitted insert");
    CHECK(mvcc_commit(t4) == MVCC_OK, "s2: t4 commits");
    CHECK(mvcc_row_get(t3, 1, "accounts", row_b, &got) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s2: t3 STILL cannot see it after t4 commits -- t3's snapshot predates the commit (repeatable read)");
    uint64_t t5 = mvcc_begin();   // begins after t4's commit
    CHECK(mvcc_row_get(t5, 1, "accounts", row_b, &got) == MVCC_OK && strcmp(got.values[1], "beta") == 0,
          "s2: t5 (begun after the commit) sees it correctly");
    mvcc_commit(t3);
    mvcc_commit(t5);

    /* ── Scenario 3: UPDATE creates a new version; a transaction's own
     * snapshot keeps seeing the OLD value even after someone else commits
     * a change -- classic snapshot-isolation repeatable read. ──────────── */
    uint64_t t6 = mvcc_begin();
    CHECK(mvcc_row_get(t6, 1, "accounts", row_a, &got) == MVCC_OK && strcmp(got.values[1], "alpha") == 0,
          "s3: t6 sees the original value of row_a");
    uint64_t t7 = mvcc_begin();
    struct RowValues va2 = rv("1", "alpha-v2");
    CHECK(mvcc_row_update(t7, 1, "accounts", row_a, &va2) == MVCC_OK, "s3: t7 updates row_a");
    CHECK(mvcc_commit(t7) == MVCC_OK, "s3: t7 commits");
    CHECK(mvcc_row_get(t6, 1, "accounts", row_a, &got) == MVCC_OK && strcmp(got.values[1], "alpha") == 0,
          "s3: t6 STILL sees the OLD value after t7's commit -- repeatable read across an update");
    uint64_t t8 = mvcc_begin();
    CHECK(mvcc_row_get(t8, 1, "accounts", row_a, &got) == MVCC_OK && strcmp(got.values[1], "alpha-v2") == 0,
          "s3: a fresh t8 sees the NEW value");
    mvcc_commit(t6);
    mvcc_commit(t8);

    /* ── Scenario 4: write-write conflict, eager (first-updater-wins) --
     * detected the moment a second transaction tries to touch a row
     * another transaction has already started superseding, before either
     * one commits. ────────────────────────────────────────────────────────── */
    uint64_t t9 = mvcc_begin();
    uint64_t t10 = mvcc_begin();
    struct RowValues va3 = rv("1", "alpha-v3-by-t9");
    CHECK(mvcc_row_update(t9, 1, "accounts", row_a, &va3) == MVCC_OK, "s4: t9 updates row_a first");
    struct RowValues va4 = rv("1", "alpha-v4-by-t10");
    CHECK(mvcc_row_update(t10, 1, "accounts", row_a, &va4) == MVCC_ERR_WRITE_CONFLICT,
          "s4: t10's concurrent update on the SAME row is rejected with MVCC_ERR_WRITE_CONFLICT");

    /* ── Scenario 5: the conflict clears once the blocker ROLLS BACK. ──── */
    CHECK(mvcc_rollback(t9, 1) == MVCC_OK, "s5: t9 rolls back");
    CHECK(mvcc_row_update(t10, 1, "accounts", row_a, &va4) == MVCC_OK,
          "s5: t10's retry now succeeds -- the conflict cleared when t9 aborted");
    CHECK(mvcc_commit(t10) == MVCC_OK, "s5: t10 commits");
    uint64_t t11 = mvcc_begin();
    CHECK(mvcc_row_get(t11, 1, "accounts", row_a, &got) == MVCC_OK && strcmp(got.values[1], "alpha-v4-by-t10") == 0,
          "s5: the committed value is t10's, confirming t9's rolled-back write never took effect");
    mvcc_commit(t11);

    /* ── Scenario 6: the conflict also correctly fires against a STALE
     * snapshot even after the blocker has already committed (not just
     * while it's still active) -- proves the check isn't merely "is
     * anyone currently active on this row," it's real MVCC write-skew
     * prevention. ─────────────────────────────────────────────────────────── */
    uint64_t t12 = mvcc_begin();   // snapshot taken before t13's update+commit below
    uint64_t t13 = mvcc_begin();
    struct RowValues va5 = rv("1", "alpha-v5-by-t13");
    CHECK(mvcc_row_update(t13, 1, "accounts", row_a, &va5) == MVCC_OK, "s6: t13 updates row_a");
    CHECK(mvcc_commit(t13) == MVCC_OK, "s6: t13 commits");
    struct RowValues va6 = rv("1", "alpha-v6-by-t12-stale");
    CHECK(mvcc_row_update(t12, 1, "accounts", row_a, &va6) == MVCC_ERR_WRITE_CONFLICT,
          "s6: t12's update against its now-stale snapshot correctly conflicts, even though t13 already committed");
    mvcc_rollback(t12, 1);

    /* ── Scenario 7: DELETE participates in the same conflict rule as
     * UPDATE (both are "supersede this version" operations). ──────────── */
    struct MvccRowId row_c;
    uint64_t t14 = mvcc_begin();
    struct RowValues vc = rv("3", "gamma");
    CHECK(mvcc_row_insert(t14, 1, "accounts", &vc, &row_c) == MVCC_OK, "s7: t14 inserts row_c");
    CHECK(mvcc_commit(t14) == MVCC_OK, "s7: t14 commits");
    uint64_t t15 = mvcc_begin();
    uint64_t t16 = mvcc_begin();
    CHECK(mvcc_row_delete(t15, 1, "accounts", row_c) == MVCC_OK, "s7: t15 deletes row_c");
    struct RowValues vc2 = rv("3", "gamma-v2-by-t16");
    CHECK(mvcc_row_update(t16, 1, "accounts", row_c, &vc2) == MVCC_ERR_WRITE_CONFLICT,
          "s7: t16's update against the same row t15 is deleting also conflicts");
    CHECK(mvcc_commit(t15) == MVCC_OK, "s7: t15 commits the delete");
    uint64_t t17 = mvcc_begin();
    CHECK(mvcc_row_get(t17, 1, "accounts", row_c, &got) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s7: a fresh transaction confirms row_c is really gone");
    mvcc_commit(t17);
    mvcc_rollback(t16, 1);

    /* ── Scenario 8: read-your-own-delete within one transaction. ──────── */
    struct MvccRowId row_d;
    uint64_t t18 = mvcc_begin();
    struct RowValues vd = rv("4", "delta");
    mvcc_row_insert(t18, 1, "accounts", &vd, &row_d);
    mvcc_commit(t18);
    uint64_t t19 = mvcc_begin();
    CHECK(mvcc_row_delete(t19, 1, "accounts", row_d) == MVCC_OK, "s8: t19 deletes row_d");
    CHECK(mvcc_row_get(t19, 1, "accounts", row_d, &got) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s8: t19 itself no longer sees row_d after its own uncommitted delete");
    mvcc_commit(t19);

    /* ── Scenario 9: two transactions touching DIFFERENT rows never
     * conflict, concurrently or otherwise. ────────────────────────────────── */
    uint64_t t20 = mvcc_begin();
    uint64_t t21 = mvcc_begin();
    struct RowValues ve = rv("5", "epsilon");
    struct RowValues vf = rv("6", "zeta");
    struct MvccRowId row_e, row_f;
    CHECK(mvcc_row_insert(t20, 1, "accounts", &ve, &row_e) == MVCC_OK, "s9: t20 inserts a distinct row");
    CHECK(mvcc_row_insert(t21, 1, "accounts", &vf, &row_f) == MVCC_OK, "s9: t21 inserts a different distinct row -- no conflict");
    CHECK(mvcc_commit(t20) == MVCC_OK && mvcc_commit(t21) == MVCC_OK, "s9: both commit cleanly");

    /* ── Scenario 10: snapshot-consistent table_scan -- doesn't see an
     * uncommitted sibling's insert, DOES see its own. ─────────────────── */
    uint64_t t22 = mvcc_begin();
    struct scan_ctx before;
    memset(&before, 0, sizeof(before));
    uint32_t count_before = mvcc_table_scan(t22, 1, "accounts", scan_cb, &before);
    uint64_t t23 = mvcc_begin();
    struct RowValues vg = rv("7", "eta-uncommitted");
    struct MvccRowId row_g;
    mvcc_row_insert(t23, 1, "accounts", &vg, &row_g);
    struct scan_ctx during_t22;
    memset(&during_t22, 0, sizeof(during_t22));
    uint32_t count_during_t22 = mvcc_table_scan(t22, 1, "accounts", scan_cb, &during_t22);
    CHECK(count_during_t22 == count_before,
          "s10: t22's scan is unaffected by t23's uncommitted insert (still the same row count)");
    struct scan_ctx during_t23;
    memset(&during_t23, 0, sizeof(during_t23));
    uint32_t count_during_t23 = mvcc_table_scan(t23, 1, "accounts", scan_cb, &during_t23);
    CHECK(count_during_t23 == count_before + 1,
          "s10: t23's OWN scan sees its own uncommitted insert (one more row than t22 sees)");
    mvcc_rollback(t23, 1);
    mvcc_commit(t22);

    /* ── Scenario 11: exactly one version ever shows up per logical row in
     * a scan, even after an update -- the invariant mvcc.c's header
     * comment names as the reason no de-duplication pass is needed. ────── */
    uint64_t t24 = mvcc_begin();
    uint32_t occurrences = 0;
    struct scan_ctx dup_check;
    memset(&dup_check, 0, sizeof(dup_check));
    mvcc_table_scan(t24, 1, "accounts", scan_cb, &dup_check);
    for (uint32_t i = 0; i < dup_check.count; i++)
        if (strncmp(dup_check.values[i], "alpha", 5) == 0) occurrences++;
    CHECK(occurrences == 1, "s11: row_a's value (updated multiple times across this test) appears exactly once in a full scan");
    mvcc_commit(t24);

    /* ── Scenario 12: rollback of an INSERT physically removes the row --
     * verified via a direct rowstore_table_scan() (bypassing mvcc.c
     * entirely) confirming the garbage is genuinely gone, not just
     * logically hidden. ───────────────────────────────────────────────────── */
    struct MvccRowId row_h;
    uint64_t t25 = mvcc_begin();
    struct RowValues vh = rv("8", "theta-to-be-aborted");
    mvcc_row_insert(t25, 1, "accounts", &vh, &row_h);
    uint32_t phys_count_before_rollback = 0;
    rowstore_table_scan(1, "accounts", phys_count_cb, &phys_count_before_rollback);
    CHECK(mvcc_rollback(t25, 1) == MVCC_OK, "s12: t25 rolls back its own insert");
    uint32_t phys_count_after_rollback = 0;
    rowstore_table_scan(1, "accounts", phys_count_cb, &phys_count_after_rollback);
    CHECK(phys_count_after_rollback == phys_count_before_rollback - 1,
          "s12: rollback physically deleted the aborted row (real rowstore_table_scan count dropped by exactly 1)");

    /* ── Scenario 13: rollback of an UPDATE leaves the old version current
     * and cleans up the new (garbage) physical row. ────────────────────── */
    uint64_t t26 = mvcc_begin();
    struct RowValues vf2 = rv("6", "zeta-v2-to-be-aborted");
    CHECK(mvcc_row_update(t26, 1, "accounts", row_f, &vf2) == MVCC_OK, "s13: t26 updates row_f");
    CHECK(mvcc_rollback(t26, 1) == MVCC_OK, "s13: t26 rolls back");
    uint64_t t27 = mvcc_begin();
    CHECK(mvcc_row_get(t27, 1, "accounts", row_f, &got) == MVCC_OK && strcmp(got.values[1], "zeta") == 0,
          "s13: row_f's value reverted to the pre-update value after rollback");
    struct RowValues vf3 = rv("6", "zeta-v2-real");
    CHECK(mvcc_row_update(t27, 1, "accounts", row_f, &vf3) == MVCC_OK,
          "s13: row_f is updatable again after the rollback un-superseded it (no lingering conflict)");
    mvcc_commit(t27);

    /* ── Scenario 14: error paths. ─────────────────────────────────────── */
    CHECK(mvcc_begin() != 0, "s14: sanity -- begin still works before exhaustion test");
    {
        uint64_t fill[MVCC_MAX_TXNS];
        uint32_t n = 0;
        uint64_t x;
        while ((x = mvcc_begin()) != 0 && n < MVCC_MAX_TXNS) fill[n++] = x;
        CHECK(mvcc_begin() == 0, "s14: MVCC_MAX_TXNS concurrently active transactions -- the next begin() fails cleanly");
        for (uint32_t i = 0; i < n; i++) mvcc_rollback(fill[i], 1);
    }
    uint64_t t28 = mvcc_begin();
    CHECK(mvcc_row_insert(t28, 1, "no_such_table", &va, &row_a) == MVCC_ERR_TABLE_NOT_FOUND,
          "s14: insert into an unknown table fails cleanly");
    struct MvccRowId bogus = { 999999 };
    CHECK(mvcc_row_get(t28, 1, "accounts", bogus, &got) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s14: get on a bogus logical_id fails cleanly (never existed)");
    CHECK(mvcc_row_update(t28, 1, "accounts", bogus, &va) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s14: update on a bogus logical_id fails cleanly");
    CHECK(mvcc_row_delete(t28, 1, "accounts", bogus) == MVCC_ERR_ROW_NOT_VISIBLE,
          "s14: delete on a bogus logical_id fails cleanly");
    CHECK(mvcc_commit(999999999ULL) == MVCC_ERR_TXN_NOT_ACTIVE, "s14: commit on a bogus/inactive txn_id fails cleanly");
    CHECK(mvcc_rollback(999999999ULL, 1) == MVCC_ERR_TXN_NOT_ACTIVE, "s14: rollback on a bogus/inactive txn_id fails cleanly");
    mvcc_commit(t28);
    uint64_t t29 = mvcc_begin();
    mvcc_commit(t29);
    CHECK(mvcc_row_get(t29, 1, "accounts", row_a, &got) == MVCC_ERR_TXN_NOT_ACTIVE,
          "s14: using an already-committed txn_id for a further operation fails cleanly");

    g_access_force_deny = 1;
    uint64_t t30 = mvcc_begin();
    CHECK(mvcc_row_insert(t30, 1, "accounts", &va, &row_a) == MVCC_ERR_PERMISSION_DENIED,
          "s14: permission denial propagates through mvcc_row_insert (rowstore.c's own gate)");
    CHECK(mvcc_row_delete(t30, 1, "accounts", row_f) == MVCC_ERR_PERMISSION_DENIED,
          "s14: permission denial is caught by mvcc_row_delete's own direct catalog_check_access() gate");
    g_access_force_deny = 0;
    mvcc_rollback(t30, 1);

    printf("\n%s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
