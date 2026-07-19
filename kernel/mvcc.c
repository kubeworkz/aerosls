/*
 * mvcc.c — Phase 21 (relational layer) concurrency control. See mvcc.h for
 * the full design writeup (why MVCC over blocking 2PL, the layering-over-
 * unmodified-rowstore.c shape, and the first-updater-wins conflict rule).
 *
 * ─── Why mvcc_table_scan() needs no de-duplication pass ───────────────────
 * A commit stamps ONE new global sequence number `K` onto both halves of a
 * supersession in the same instant: the OLD version's `xmax` becomes `K`
 * and the NEW version's `xmin` becomes that same `K`. A version is visible
 * to a snapshot `S` over the half-open range `xmin <= S < xmax` (treating
 * `xmax == 0`/uncommitted as +infinity). Because consecutive versions of
 * one logical row share an exact boundary (old.xmax == new.xmin == K) with
 * no gap and no overlap, for any snapshot S there is at most one version
 * of a given logical row with `xmin <= S < xmax` among fully-committed
 * versions — S either lands strictly before K (sees the old version, not
 * yet superseded from S's point of view) or at/after K (sees the new one,
 * already superseded). The one case needing separate care is a
 * transaction's view of ITS OWN uncommitted work, which is why visibility
 * also special-cases `xmin_txn == txn_id` / `xmax_txn == txn_id` directly
 * rather than relying on committed sequence numbers alone — see
 * mv_visible() below.
 */
#include "mvcc.h"
#include "object_catalog.h"
#include "../user/permissions.h"
#include <stddef.h>

// ─── String helper (no libc — each kernel source file keeps its own small
// copy, matching this codebase's established convention: rs_* in
// rowstore.c, ri_* in row_index.c, pe_* in predicate.c, mv_* here). ───────
static int mv_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// ─── State ────────────────────────────────────────────────────────────────
struct MvccTxn {
    uint64_t txn_id;
    uint64_t snapshot_seq;
    uint8_t  active;
};

struct MvccVersion {
    uint8_t      active;           // 0 = free/reclaimed slot (rolled-back insert/update), 1 = in use
    uint64_t     logical_id;
    uint64_t     table_object_id;
    struct RowId physical_id;
    uint64_t     xmin;
    uint8_t      xmin_committed;
    uint64_t     xmin_txn;
    uint64_t     xmax;
    uint8_t      xmax_committed;
    uint64_t     xmax_txn;         // 0 = nobody has superseded this version (yet)
};

static struct MvccTxn    mvcc_txns[MVCC_MAX_TXNS];
static struct MvccVersion mvcc_versions[MVCC_MAX_VERSIONS];
static uint32_t          mvcc_version_count;
static uint64_t          mvcc_next_txn_id;
static uint64_t          mvcc_next_logical_id;
static uint64_t          mvcc_commit_seq;   // next commit-sequence number to hand out

void mvcc_init(void) {
    for (uint32_t i = 0; i < MVCC_MAX_TXNS; i++) mvcc_txns[i].active = 0;
    for (uint32_t i = 0; i < MVCC_MAX_VERSIONS; i++) mvcc_versions[i].active = 0;
    mvcc_version_count = 0;
    mvcc_next_txn_id = 1;       // 0 reserved: "no transaction" / invalid, matching
                                 // transaction.h's own tx_get_active() "0 = none" convention
    mvcc_next_logical_id = 1;   // 0 reserved: invalid MvccRowId
    mvcc_commit_seq = 1;        // 0 reserved: "not yet committed"
}

// ─── Small lookups ────────────────────────────────────────────────────────
static int mv_find_txn(uint64_t txn_id) {
    if (txn_id == 0) return -1;
    for (uint32_t i = 0; i < MVCC_MAX_TXNS; i++) {
        if (mvcc_txns[i].active && mvcc_txns[i].txn_id == txn_id) return (int)i;
    }
    return -1;
}

static int mv_find_table_index(const char* table_name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!object_catalog[i].uses_rowstore) continue;
        if (mv_streq(object_catalog[i].name, table_name)) return (int)i;
    }
    return -1;
}

static MvccError map_rowstore_err(int rc) {
    switch (rc) {
        case 0: return MVCC_OK;
        case 1: return MVCC_ERR_TABLE_NOT_FOUND;
        case 2: return MVCC_ERR_PERMISSION_DENIED;
        case 3: return MVCC_ERR_ROW_NOT_VISIBLE;
        case 4: /* fallthrough */
        case 5: return MVCC_ERR_VALUES_INVALID;
        case 6: return MVCC_ERR_VERSION_POOL_FULL;   // rowstore's own page pool, not ours --
                                                      // still "storage exhausted" from the caller's view
        default: return MVCC_ERR_VALUES_INVALID;
    }
}

// A version is visible to txn t if t created it (even if still uncommitted
// -- "read your own writes") or it was committed before t's snapshot began,
// AND it hasn't been hidden from t: not superseded by t itself, and not
// superseded by a commit that landed before t's snapshot began.
static int mv_visible(const struct MvccVersion* v, const struct MvccTxn* t) {
    int created_visible = (v->xmin_committed && v->xmin <= t->snapshot_seq) || (v->xmin_txn == t->txn_id);
    if (!created_visible) return 0;
    int hidden = (v->xmax_committed && v->xmax <= t->snapshot_seq) ||
                 (v->xmax_txn != 0 && v->xmax_txn == t->txn_id);
    return !hidden;
}

// Returns the index into mvcc_versions[] of the version of logical_id
// visible to t, or -1 if none (never existed, or not visible from here).
static int mv_find_visible_version(uint64_t logical_id, const struct MvccTxn* t) {
    for (uint32_t i = 0; i < mvcc_version_count; i++) {
        if (!mvcc_versions[i].active) continue;
        if (mvcc_versions[i].logical_id != logical_id) continue;
        if (mv_visible(&mvcc_versions[i], t)) return (int)i;
    }
    return -1;
}

// ─── Transaction lifecycle ────────────────────────────────────────────────
uint64_t mvcc_begin(void) {
    for (uint32_t i = 0; i < MVCC_MAX_TXNS; i++) {
        if (!mvcc_txns[i].active) {
            mvcc_txns[i].active = 1;
            mvcc_txns[i].txn_id = mvcc_next_txn_id++;
            // mvcc_commit_seq is the NEXT commit-sequence number that will
            // be handed out, not yet used by anyone -- the snapshot must be
            // the LAST one actually used ("everything committed as of right
            // now"), i.e. one less. Off-by-one matters here: without the
            // "-1", a transaction beginning right before another one
            // commits would be assigned the same sequence number that
            // commit is about to hand out, making the concurrent commit's
            // version incorrectly visible to a snapshot that started before
            // it. Caught by this phase's own host test (scenarios 2/3/6),
            // not just reasoned through -- see the Findings addendum.
            mvcc_txns[i].snapshot_seq = mvcc_commit_seq - 1;
            return mvcc_txns[i].txn_id;
        }
    }
    return 0;   // MVCC_MAX_TXNS concurrently active already
}

MvccError mvcc_commit(uint64_t txn_id) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return MVCC_ERR_TXN_NOT_ACTIVE;
    uint64_t seq = mvcc_commit_seq++;
    for (uint32_t i = 0; i < mvcc_version_count; i++) {
        struct MvccVersion* v = &mvcc_versions[i];
        if (!v->active) continue;
        if (v->xmin_txn == txn_id && !v->xmin_committed) { v->xmin = seq; v->xmin_committed = 1; }
        if (v->xmax_txn == txn_id && !v->xmax_committed) { v->xmax = seq; v->xmax_committed = 1; }
    }
    mvcc_txns[tidx].active = 0;
    return MVCC_OK;
}

MvccError mvcc_rollback(uint64_t txn_id, uint32_t caller_uid) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return MVCC_ERR_TXN_NOT_ACTIVE;
    for (uint32_t i = 0; i < mvcc_version_count; i++) {
        struct MvccVersion* v = &mvcc_versions[i];
        if (!v->active) continue;
        if (v->xmin_txn == txn_id && !v->xmin_committed) {
            // Real garbage -- this version never became visible to anyone
            // and never will. Physically remove the row it occupies (the
            // one case this phase ever calls rowstore_row_delete()) and
            // free the version slot itself for reuse. Best-effort: if
            // caller_uid lacks PERM_WRITE, the physical row is leaked, not
            // a correctness problem -- see mvcc.h's header comment on this
            // function.
            for (uint32_t ti = 0; ti < object_catalog_count; ti++) {
                if (object_catalog[ti].object_id == v->table_object_id && object_catalog[ti].active) {
                    rowstore_row_delete(caller_uid, object_catalog[ti].name, v->physical_id);
                    break;
                }
            }
            v->active = 0;
        }
        if (v->xmax_txn == txn_id && !v->xmax_committed) {
            // This transaction had started superseding v but never
            // committed -- un-supersede it, v is current again.
            v->xmax_txn = 0;
        }
    }
    mvcc_txns[tidx].active = 0;
    return MVCC_OK;
}

// ─── Row CRUD ─────────────────────────────────────────────────────────────
MvccError mvcc_row_insert(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                          const struct RowValues* values, struct MvccRowId* out_id) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return MVCC_ERR_TXN_NOT_ACTIVE;

    struct RowId phys;
    int rc = rowstore_row_insert(caller_uid, table_name, values, &phys);
    if (rc != 0) return map_rowstore_err(rc);

    int cidx = mv_find_table_index(table_name);
    if (cidx < 0) return MVCC_ERR_TABLE_NOT_FOUND;   // shouldn't happen -- insert just succeeded

    if (mvcc_version_count >= MVCC_MAX_VERSIONS) {
        rowstore_row_delete(caller_uid, table_name, phys);   // don't leave an orphan physical row
        return MVCC_ERR_VERSION_POOL_FULL;
    }

    struct MvccVersion* v = &mvcc_versions[mvcc_version_count++];
    v->active = 1;
    v->logical_id = mvcc_next_logical_id++;
    v->table_object_id = object_catalog[cidx].object_id;
    v->physical_id = phys;
    v->xmin = 0; v->xmin_committed = 0; v->xmin_txn = txn_id;
    v->xmax = 0; v->xmax_committed = 0; v->xmax_txn = 0;

    if (out_id) out_id->logical_id = v->logical_id;
    return MVCC_OK;
}

MvccError mvcc_row_get(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                       struct MvccRowId id, struct RowValues* out) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return MVCC_ERR_TXN_NOT_ACTIVE;
    int vidx = mv_find_visible_version(id.logical_id, &mvcc_txns[tidx]);
    if (vidx < 0) return MVCC_ERR_ROW_NOT_VISIBLE;
    int rc = rowstore_row_get(caller_uid, table_name, mvcc_versions[vidx].physical_id, out);
    return map_rowstore_err(rc);
}

MvccError mvcc_row_update(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                          struct MvccRowId id, const struct RowValues* values) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return MVCC_ERR_TXN_NOT_ACTIVE;
    int vidx = mv_find_visible_version(id.logical_id, &mvcc_txns[tidx]);
    if (vidx < 0) return MVCC_ERR_ROW_NOT_VISIBLE;

    struct MvccVersion* old = &mvcc_versions[vidx];
    if (old->xmax_txn != 0 && old->xmax_txn != txn_id) return MVCC_ERR_WRITE_CONFLICT;

    struct RowId phys;
    int rc = rowstore_row_insert(caller_uid, table_name, values, &phys);
    if (rc != 0) return map_rowstore_err(rc);

    if (mvcc_version_count >= MVCC_MAX_VERSIONS) {
        rowstore_row_delete(caller_uid, table_name, phys);
        return MVCC_ERR_VERSION_POOL_FULL;
    }

    struct MvccVersion* nv = &mvcc_versions[mvcc_version_count++];
    nv->active = 1;
    nv->logical_id = id.logical_id;
    nv->table_object_id = old->table_object_id;   // old may be a stale pointer after the array grew?
                                                    // no -- mvcc_versions is a fixed array, never moved.
    nv->physical_id = phys;
    nv->xmin = 0; nv->xmin_committed = 0; nv->xmin_txn = txn_id;
    nv->xmax = 0; nv->xmax_committed = 0; nv->xmax_txn = 0;

    old->xmax_txn = txn_id;   // pending supersede -- becomes real at commit, undone at rollback
    return MVCC_OK;
}

MvccError mvcc_row_delete(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                          struct MvccRowId id) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return MVCC_ERR_TXN_NOT_ACTIVE;

    // The one deliberate second permission gate in this file -- see mvcc.h's
    // header comment for why: delete makes no rowstore.c call to piggyback
    // on (it's a pure metadata tombstone; calling rowstore_row_delete() here
    // would physically destroy bytes a concurrent older-snapshot reader may
    // still legitimately need).
    if (!catalog_check_access(caller_uid, table_name, PERM_WRITE)) return MVCC_ERR_PERMISSION_DENIED;

    int vidx = mv_find_visible_version(id.logical_id, &mvcc_txns[tidx]);
    if (vidx < 0) return MVCC_ERR_ROW_NOT_VISIBLE;

    struct MvccVersion* v = &mvcc_versions[vidx];
    if (v->xmax_txn != 0 && v->xmax_txn != txn_id) return MVCC_ERR_WRITE_CONFLICT;

    v->xmax_txn = txn_id;   // pending delete -- becomes real at commit, undone at rollback
    return MVCC_OK;
}

// ─── Snapshot-consistent scan ─────────────────────────────────────────────
uint32_t mvcc_table_scan(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                         MvccScanCb cb, void* ctx) {
    int tidx = mv_find_txn(txn_id);
    if (tidx < 0) return 0;
    int cidx = mv_find_table_index(table_name);
    if (cidx < 0) return 0;
    if (!catalog_check_access(caller_uid, table_name, PERM_READ)) return 0;

    uint64_t table_object_id = object_catalog[cidx].object_id;
    uint32_t visited = 0;
    for (uint32_t i = 0; i < mvcc_version_count; i++) {
        struct MvccVersion* v = &mvcc_versions[i];
        if (!v->active || v->table_object_id != table_object_id) continue;
        if (!mv_visible(v, &mvcc_txns[tidx])) continue;
        struct RowValues row;
        if (rowstore_row_get(caller_uid, table_name, v->physical_id, &row) != 0) continue;
        visited++;
        if (cb) {
            struct MvccRowId id = { v->logical_id };
            cb(id, &row, ctx);
        }
    }
    return visited;
}
