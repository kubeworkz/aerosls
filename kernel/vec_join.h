/*
 * vec_join.h — Vector Store Roadmap Phase 5: ID-based join back to
 * relational tables. See docs/AeroSLS-VectorStore-Roadmap-v0.1.md §7 for
 * the full design writeup and the "Findings addendum" for what's built
 * here.
 *
 * ─── Why this is a new, third file, not an addition to vecstore.c or
 * sql_exec.c ────────────────────────────────────────────────────────────
 * Phase 4's own precedent put its one cross-subsystem adapter
 * (sys_sls_vec_embed_insert()) INSIDE vecstore.c, because net/
 * ollama_client.h is a genuinely light dependency (a handful of structs,
 * no transitive subsystem chain). sql_exec.h is not light: pulling it into
 * vecstore.c would drag predicate.h, cursor.h, mvcc.h, rowstore.h,
 * row_index.h, persist.h — and therefore a fake-NVMe backing store — into
 * every vecstore.c host test that exists today (vecstore_host_test.c,
 * vecstore_search_host_test.c, vecstore_syscall_host_test.c), none of
 * which need any of that to test vecstore.c's own storage/search logic.
 * The reverse (putting this in sql_exec.c, which would then need to
 * include vecstore.h/net/ollama_client.h) has the identical problem in the
 * other direction. So this phase's join-back logic lives in its own leaf
 * file instead: it depends on both vecstore.h and sql_exec.h, but neither
 * of THEM depends on it, keeping every existing host test's dependency
 * graph exactly as light as it already was. This is a real, deliberate
 * divergence from Phase 4's own placement choice, not an inconsistency —
 * the two cases have genuinely different dependency weights.
 *
 * ─── Why no new SQL grammar (explicitly out of scope, per the roadmap) ──
 * A `WHERE id IN (...)` clause is NOT supported by sql_parser.c today
 * (confirmed by direct audit: no IN token, no value-list parse rule) —
 * but `WHERE id = X OR id = Y OR ...` already works with zero grammar
 * changes (parse_predicate()/parse_and_expr() in sql_parser.c already
 * support arbitrary OR-chains). vec_join_resolve() builds exactly that
 * kind of query as ordinary text and calls the real, unmodified
 * sql_execute() — the relational engine has no idea a vector search was
 * ever involved, matching this whole roadmap's "vectors are a separate
 * subsystem, joined at the application/caller boundary, not the SQL
 * grammar" thesis (see the roadmap's own §0).
 *
 * ─── Why the relational side needs zero vecstore awareness ──────────────
 * sql_exec.c has no reserved "primary key"/"id" concept anywhere —
 * find_column_index() is a flat linear name match against whatever
 * columns exist, nothing more. `id_column` below is just an ordinary
 * column name the CALLER (not this file, not sql_exec.c) knows happens to
 * hold the same external_id values used at vecstore_insert() time — this
 * matches vecstore.h's own external_id comment ("the correlation key a
 * caller uses to join a search result back to a relational row").
 *
 * ─── The real, binding batch-size limit is the predicate node pool, not
 * SQL_MAX_TEXT_LEN ──────────────────────────────────────────────────────
 * predicate.h's PREDICATE_MAX_NODES is 32; a flat OR-chain of k equality
 * comparisons consumes 2k-1 nodes (1 comparison for the first term, then
 * 1 comparison + 1 OR-combinator per remaining term), so k <= 16 is the
 * real structural ceiling — confirmed by reading predicate_add_comparison/
 * _add_and/_add_or's own node-exhaustion behavior, not assumed from the
 * 512-char SQL_MAX_TEXT_LEN alone (which a long table/column name could
 * exceed well before 16 terms anyway). vec_join_resolve() respects BOTH
 * limits independently (see vec_join.c) rather than assuming either one
 * dominates.
 */
#ifndef VEC_JOIN_H
#define VEC_JOIN_H

#include <stdint.h>
#include "vecstore.h"
#include "rowstore.h"   // struct RowValues

// Real, binding cap: a flat "col = X OR col = Y OR ..." chain of k terms
// consumes 2k-1 predicate.h nodes out of PREDICATE_MAX_NODES (32) -- see
// header comment above. vec_join_resolve() also independently respects
// SQL_MAX_TEXT_LEN (sql_parser.h) at the text-buffer level, since a long
// table/column name can make even this many terms too long first; whichever
// limit is hit first ends the current batch, and resolution continues with
// a fresh query for the rest -- see vec_join.c.
#define VEC_JOIN_MAX_IDS_PER_QUERY 16

// Called once per (VecMatch, resolved row) pair -- match is the original
// search result (external_id/id/distance) that produced this row; row is
// the matching relational row's full column data (positionally indexed,
// same convention as RowScanCb/cursor_fetch_rows). If id_column holds
// duplicate values (multiple rows sharing one external_id) or matches[]
// itself contains a duplicate external_id (vecstore.h explicitly permits
// this -- uniqueness is the caller's own responsibility, not vecstore's or
// this file's to enforce), cb() is called once per real (match, row) pair,
// not deduplicated -- every real correlation is reported, not just the
// first one found.
typedef void (*VecJoinRowCb)(const struct VecMatch* match, const struct RowValues* row, void* ctx);

// Resolves match_count VecMatch results (typically straight from
// vecstore_search()) to real rows in table_name, joining on id_column (an
// ordinary column in that table -- see header comment on why no special
// vecstore awareness is needed on the relational side). Issues one or more
// real SQL SELECT statements via the real, unmodified sql_execute() --
// zero new SQL grammar (see header comment) -- and delivers each resolved
// row via cb(), paired with the VecMatch that produced it.
//
// Returns the number of (match, row) pairs actually delivered via cb() --
// may be less than match_count (some external_ids may not correspond to
// any row -- not an error, the same "0/fewer results is a valid outcome"
// contract vecstore_search()/vecstore_collection_scan() already have), or
// more than match_count (if id_column or matches[] itself has duplicates,
// see above). A query failing for one internal batch (bad table/column
// name, permission denied, etc.) does not abort the whole call -- matches
// outside that batch still get resolved; unresolved matches are simply
// never delivered via cb(), not reported through a separate error channel.
uint32_t vec_join_resolve(uint32_t caller_uid, const char* table_name,
                          const char* id_column, const struct VecMatch* matches,
                          uint32_t match_count, VecJoinRowCb cb, void* ctx);

// ─── Gap Remediation Phase C: live reachability ────────────────────────────
// vec_join_resolve() had no syscall/shell/HTTP surface at all before this --
// Phase 5's own scope note called that a deliberate cut ("no reachability
// blocker forcing a live surface"), which held until a frontend actually
// needed to demonstrate the join-back capability. caller_uid travels inside
// the request struct, matching SYS_SLS_SQL_EXECUTE/SYS_SLS_VEC_*'s own
// established convention.
//
// This syscall takes matches[] directly (typically copied straight from a
// prior SLSVecSearchRequest.matches[]) rather than re-running a search
// itself -- it is the join primitive alone, matching vec_join_resolve()'s
// own real contract (a caller who already has matches from elsewhere, e.g.
// a separate /api/vec/search call, can join them without paying for a
// second search).
#define SYS_SLS_VEC_JOIN 226

// Same cap as VEC_SEARCH_MAX_K (vecstore.h) -- a join is only ever run
// against a search's own result set in practice, so there is no reason for
// this cap to differ from the one bounding how many matches can exist to
// join in the first place.
#define VEC_JOIN_MAX_RESULTS VEC_SEARCH_MAX_K

struct VecJoinResultRow {
    struct VecMatch  match;   // the original search result that produced this row
    struct RowValues row;     // the resolved relational row's full column data
};

struct SLSVecJoinRequest {
    uint32_t          caller_uid;
    char               table_name[OBJECT_NAME_LEN];
    char               id_column[RECORD_KEY_LEN];
    struct VecMatch     matches[VEC_SEARCH_MAX_K];   // caller-supplied, e.g. copied from a
                                                        // prior SLSVecSearchRequest.matches[]
    uint32_t            match_count;                   // capped to VEC_SEARCH_MAX_K internally
    struct VecJoinResultRow results[VEC_JOIN_MAX_RESULTS];   // filled in by the call
    uint32_t            result_count;                  // TRUE delivered count -- may exceed
                                                          // VEC_JOIN_MAX_RESULTS; compare against
                                                          // it (or check `truncated`) to detect
    uint8_t             truncated;                      // 1 if result_count > VEC_JOIN_MAX_RESULTS
};

// Always returns 0 -- vec_join_resolve() itself has no failure mode beyond
// "0 (or fewer) results delivered," matching its own "0 is a valid outcome,
// not an error" contract (see vec_join.h's own header comment); req->
// result_count/truncated are always filled in.
uint64_t sys_sls_vec_join(struct SLSVecJoinRequest* req);

#endif /* VEC_JOIN_H */
