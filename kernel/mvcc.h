/*
 * mvcc.h — Phase 21 (relational layer): real concurrency control for
 * row-set tables, via snapshot-isolation MVCC. See
 * docs/AeroSLS-RDBMS-Roadmap-v0.1.md §8 for the design-fork investigation
 * that led here and the "Findings addendum" for what's built.
 *
 * ─── Why MVCC, not blocking 2PL (the roadmap's own named fork) ─────────────
 * Investigation before writing any code found two decisive, concrete facts,
 * not just a preference:
 *
 *   1. `lock_mgr.h`'s own header comment already states the design constraint
 *      point-blank: "no blocking / waiting in a bare-metal kernel." That's
 *      not a gap Phase 21 was assigned to close — it's a pre-existing,
 *      deliberate architectural stance this project already committed to.
 *      Blocking 2PL requires a transaction to genuinely suspend (parking
 *      itself on a wait queue until a lock frees, needing real integration
 *      with the scheduler — Phase 12's `partition.c` fairness machinery —
 *      to yield the CPU and be woken later) plus deadlock detection (a
 *      wait-for graph, cycle detection, victim selection). Nothing else in
 *      this codebase has ever made a syscall genuinely block a caller this
 *      way; building that integration would be a much larger, riskier lift
 *      than this phase's own "groundwork, not a full RDBMS" framing invites.
 *   2. Direct inspection (not assumption) found `kernel/rowstore.c` —
 *      Phases 16-20's entire row-set/SQL storage path — calls neither
 *      `lock_mgr.c` nor `transaction.c`/WAL staging at all. The ONLY
 *      concurrency protection anywhere in this codebase today
 *      (`lock_mgr.c`'s fail-fast exclusive locking + `transaction.c`'s WAL)
 *      belongs entirely to the legacy `sys_sls_select/insert/update/delete`
 *      KV-record path (`object_catalog.c`) — a completely different,
 *      pre-Phase-16 code path that Phase 17 already established stays on
 *      its own array-index track, untouched by the row-set/SQL work. This
 *      phase is not "replacing (object_id, key) locking" — it's building
 *      the FIRST concurrency protection the row-set/SQL path has ever had.
 *
 * Given (1), MVCC's non-blocking nature is the only option consistent with
 * this project's own established bare-metal posture. Given (2), this phase
 * is free to build a genuinely new, independent subsystem — the same "new
 * parallel capability, not a migration" shape Phase 17's `row_index.c` took
 * relative to legacy `index_mgr.c` — rather than needing to reconcile with
 * or migrate `lock_mgr.c`/`transaction.c`, which this phase does not call,
 * modify, or depend on at all. `journal.c`/`transaction.c`'s own WAL commit-
 * and-recovery path for the legacy KV path is therefore untouched by
 * construction, not by careful avoidance — satisfying the roadmap's own
 * "must not silently break the existing WAL/journal path" scope bullet
 * trivially, since there is no shared code between them to break.
 *
 * ─── Design: layered on top of unmodified rowstore.c, not a storage rewrite ─
 * A "logical row" (`struct MvccRowId`, one monotonic uint64_t) is a stable
 * identity across however many physical versions it accumulates. An UPDATE
 * never calls `rowstore_row_update()` (which writes in place, destroying
 * the old bytes a concurrent older-snapshot reader might still need) —
 * instead it calls `rowstore_row_insert()` to create a brand-new physical
 * row for the new version, and marks the old version's in-memory metadata
 * (`struct MvccVersion`) as superseded. This means `rowstore.c` needed ZERO
 * modification — every version's actual bytes live in an ordinary
 * Phase-16 physical row, addressed by an ordinary `struct RowId`; `mvcc.c`
 * only tracks which physical row is the correct one to read for a given
 * transaction's snapshot. A DELETE is even cheaper: no new physical row at
 * all, just a metadata tombstone on the current version.
 *
 * Versions are tagged `xmin`/`xmax` — the global commit-sequence-number at
 * which they became visible / superseded (0 = "not yet committed" or "not
 * yet superseded by anyone"), plus `xmin_txn`/`xmax_txn` recording WHICH
 * transaction is responsible, needed for "read your own uncommitted
 * writes" and for conflict detection (below). A version is visible to a
 * transaction with snapshot `S` and id `T` if:
 *   (xmin_committed && xmin <= S)  OR  (xmin_txn == T)        -- I can see it
 *   AND NOT [ (xmax_committed && xmax <= S)  OR  (xmax_txn == T) ]  -- and
 *                                                     nobody's hidden it from me
 * A COMMIT assigns ONE new global commit-sequence number to every version
 * this transaction touched — stamping it as both the new version's `xmin`
 * AND the superseded old version's `xmax` in the same instant. This exact
 * coincidence is what guarantees the "at most one version of a logical row
 * is ever visible to a given snapshot" invariant `mvcc_table_scan()` relies
 * on to avoid needing a de-duplication pass (see mvcc.c's header comment
 * for the short proof) — a version is visible over the half-open commit-
 * sequence range `[xmin, xmax)`, and consecutive versions' ranges share an
 * exact boundary with no gap and no overlap, by construction.
 *
 * ─── First-updater-wins conflict detection — eager, not deferred to commit ──
 * Classic "first committer wins" MVCC defers the conflict check to COMMIT
 * time (a second validation pass over everything the transaction touched).
 * This phase instead detects a write-write conflict EAGERLY, the moment a
 * second transaction attempts to UPDATE/DELETE a row another transaction
 * has already started superseding: `mvcc_row_update()`/`mvcc_row_delete()`
 * check the visible version's own `xmax_txn` field — if it's already
 * nonzero and belongs to some OTHER transaction (whether that transaction
 * has committed yet or not), the attempt is rejected immediately with
 * `MVCC_ERR_WRITE_CONFLICT`, no commit-time validation pass needed at all.
 * This is a deliberate, named trade-off, not an oversight: it can reject a
 * transaction slightly more eagerly than strict first-committer-wins would
 * (a transaction blocked by an in-flight sibling that later rolls back
 * could, under the classic deferred rule, have been allowed to proceed) —
 * but it is simpler to implement correctly, requires no second commit-time
 * pass over transaction history, and is easy to verify by host test. See
 * the Findings addendum for how a conflict clears once the blocking
 * transaction resolves (commit OR rollback), verified both ways.
 *
 * ─── No physical reclaim of committed history, but rollback IS cleaned up ──
 * Matching this whole roadmap's "no reclaim in first cut" posture
 * (`rowstore.c`'s page pool, `row_index.c`'s node pool never reclaim
 * either): a version superseded by a COMMITTED transaction keeps its
 * physical row forever in this first cut — a real, honest side effect of
 * genuine point-in-time versioning, though this phase builds no
 * time-travel query API to exploit it. A version created by a transaction
 * that ROLLS BACK, however, is real garbage with zero value (nobody will
 * ever need to read it) and IS cleaned up immediately via
 * `rowstore_row_delete()` — a deliberate asymmetry, not an inconsistency:
 * historical-but-committed data has genuine (if unexploited) value;
 * aborted-attempt data has none.
 *
 * ─── Explicitly out of scope this phase ─────────────────────────────────────
 * Wiring `sql_exec.c`'s SELECT/INSERT/UPDATE/DELETE/JOIN execution paths
 * (Phases 19-20) to run through `mvcc.c` instead of calling `rowstore.c`
 * directly — that's real, valuable follow-on integration work (giving
 * every SQL statement a snapshot, routing cursor iteration through a
 * stable view, etc.) but a separate, large lift deserving its own scope,
 * matching this roadmap's own precedent of naming integration debt
 * explicitly rather than rushing it (Phase 18 deferred index-assisted
 * scan integration the same way, for the same reason: "there is no
 * [consumer] yet to decide/drive it, building the integration now would be
 * speculative plumbing"). This phase builds and proves the mechanism in
 * complete isolation instead — real transactions, real conflicts, real
 * snapshot isolation, verified directly against `mvcc.c`'s own API.
 * Persistence (surviving a restart) is also out of scope, for the same
 * reason Phase 17 gave for not persisting B-tree indexes: nothing forces
 * it yet, and there's no in-flight-transaction-recovery story to build
 * alongside it regardless. No garbage collection / vacuum of old committed
 * versions (named above). No SELECT ... FOR UPDATE / explicit read locks —
 * MVCC readers never lock anything, by design.
 */
#ifndef MVCC_H
#define MVCC_H

#include <stdint.h>
#include "rowstore.h"

// ─── Limits ─────────────────────────────────────────────────────────────────
#define MVCC_MAX_TXNS      64     // deliberate echo of transaction.h's own
                                   // MAX_ACTIVE_TRANSACTIONS=64 — same round
                                   // number, not a coincidence
#define MVCC_MAX_VERSIONS  4096   // shared pool across every MVCC-managed
                                   // table, bump-allocated, no reclaim except
                                   // for rolled-back versions (see above)

typedef enum {
    MVCC_OK = 0,
    MVCC_ERR_TABLE_NOT_FOUND,
    MVCC_ERR_PERMISSION_DENIED,
    MVCC_ERR_ROW_NOT_VISIBLE,     // never existed, or deleted/not-yet-committed as of this snapshot
    MVCC_ERR_WRITE_CONFLICT,      // another transaction already has a pending or committed supersession
    MVCC_ERR_TXN_NOT_ACTIVE,      // bad, already-committed, or already-rolled-back txn_id
    MVCC_ERR_TXN_TABLE_FULL,      // MVCC_MAX_TXNS concurrently active transactions already
    MVCC_ERR_VERSION_POOL_FULL,   // MVCC_MAX_VERSIONS exhausted
    MVCC_ERR_VALUES_INVALID,      // propagated from rowstore_row_insert's own column/type validation
} MvccError;

// A logical row's stable identity across however many physical versions it
// accumulates over its lifetime — distinct from Phase 16's `struct RowId`,
// which addresses exactly one physical version's bytes.
struct MvccRowId {
    uint64_t logical_id;
};

// ─── Transaction lifecycle ───────────────────────────────────────────────
// Begins a new transaction, taking its snapshot as "everything committed
// up to right now." Returns a nonzero txn_id on success, 0 on failure
// (MVCC_MAX_TXNS concurrently active transactions already).
uint64_t mvcc_begin(void);

// Commits txn_id: assigns one new global commit-sequence number and stamps
// it onto every version this transaction created or superseded, making
// them visible (or hidden) to every snapshot taken from now on. Returns
// MVCC_ERR_TXN_NOT_ACTIVE if txn_id isn't a currently-active transaction.
MvccError mvcc_commit(uint64_t txn_id);

// Rolls back txn_id: any version it created (INSERT, or the new-version
// half of an UPDATE) is discarded and its physical row genuinely deleted
// (real garbage, not historical data) via a real rowstore_row_delete()
// call gated on caller_uid exactly like every other mutating call in this
// file; any version it was in the middle of superseding (UPDATE/DELETE)
// has that supersession undone, becoming current again for everyone.
// caller_uid need not be the same uid that ran the transaction's own
// operations (rollback is presumed to be run by whoever is cleaning up,
// e.g. after a crash) -- if that uid lacks PERM_WRITE on a given table,
// this transaction's version metadata is still correctly unwound (the
// logical effects are undone either way), but that one physical row's
// bytes are leaked rather than reclaimed -- a storage leak, not a
// correctness violation, and consistent with this whole roadmap's "no
// reclaim in first cut" posture elsewhere. Returns MVCC_ERR_TXN_NOT_ACTIVE
// if txn_id isn't currently active.
MvccError mvcc_rollback(uint64_t txn_id, uint32_t caller_uid);

// ─── Row CRUD, all under txn_id's snapshot ───────────────────────────────
// caller_uid is threaded through to the real rowstore_row_insert/get
// call underneath, which performs the actual catalog_check_access() gate
// (PERM_WRITE for insert/update, PERM_READ for get) — mvcc.c adds no
// second permission check for these three, reusing rowstore.c's own gate
// exactly as predicate_table_scan() (Phase 18) and exec_select_join()
// (Phase 20) already do. mvcc_row_delete() is the one deliberate
// exception: since it makes no rowstore.c call at all (a delete is a
// pure metadata tombstone, see the header comment above for why it can't
// safely call rowstore_row_delete() without breaking concurrent
// older-snapshot readers), it calls catalog_check_access() directly for
// PERM_WRITE — a genuine, named second gate, not a copy-paste duplicate.

MvccError mvcc_row_insert(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                          const struct RowValues* values, struct MvccRowId* out_id);
MvccError mvcc_row_get(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                       struct MvccRowId id, struct RowValues* out);
MvccError mvcc_row_update(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                          struct MvccRowId id, const struct RowValues* values);
MvccError mvcc_row_delete(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                          struct MvccRowId id);

// Visits every logical row of table_name whose version is visible under
// txn_id's snapshot (at most one version per logical row is ever visible —
// see mvcc.c's header comment for why no de-duplication pass is needed).
// Returns the number of rows visited (0 if the transaction isn't active,
// the table doesn't exist, or access is denied).
typedef void (*MvccScanCb)(struct MvccRowId id, const struct RowValues* values, void* ctx);
uint32_t mvcc_table_scan(uint64_t txn_id, uint32_t caller_uid, const char* table_name,
                         MvccScanCb cb, void* ctx);

// ─── Lifecycle ────────────────────────────────────────────────────────────
// Zeroes the transaction table and version pool, resets both monotonic
// counters (next txn_id, next logical_id) and the global commit-sequence
// counter. Called once at boot (kernel.c, alongside row_index_init()) —
// RAM-only, no restore step, matching this phase's explicit non-goal of
// persistence (see the header comment above).
void mvcc_init(void);

#endif /* MVCC_H */
