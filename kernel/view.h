/*
 * view.h — Query-Surface Roadmap Phase 5: CREATE VIEW / DROP VIEW. A
 * dedicated view registry, not object-record reuse — RECORD_VAL_LEN=256
 * can't hold a real SELECT statement's text. Mirrors kernel/database.h's
 * own lifecycle shape (a small fixed-size struct array + create/drop/
 * find-by-name + a Gap-1-style persistence hook), not a coincidence: both
 * are "a small named registry of definitions, looked up by name at SQL
 * exec time" — the same shape, applied to a different kind of definition.
 *
 * ─── Storage decision ────────────────────────────────────────────────────
 * sql_text is sized SQL_MAX_TEXT_LEN (512, sql_parser.h), not a fresh 1024
 * as an early sketch of this phase proposed: the captured tail (everything
 * after "CREATE VIEW <name> AS") is always a suffix of the CREATE VIEW
 * statement's own input text, which itself can never exceed
 * SQL_MAX_TEXT_LEN — reusing that constant keeps the invariant visible at
 * the definition site, the exact same reasoning sql_parser.h's own
 * SQL_SETOP_RHS_TEXT_LEN already applies to Phase 4's UNION right-branch
 * capture.
 *
 * ─── Scope (see docs/AeroSLS-SQL-Query-Surface-Roadmap-v0.1.md Phase 5) ──
 * CREATE VIEW v AS <select...> stores the tail text verbatim, validated by
 * a parse-only check at create time (sql_parser.c's own g_view_skip_scratch/
 * g_view_validating reentrancy guard, mirroring Phase 4's set-op capture
 * and Phase 7's subquery capture). DROP VIEW v removes it.
 *
 * Query side is deliberately narrow: `SELECT ... FROM v [WHERE ...]
 * [ORDER BY ...] [LIMIT n]` where v resolves to a view executes the stored
 * text as an ordinary NESTED statement at depth 1 (Query-Surface Roadmap
 * Phase 3's banking), then applies the OUTER projection/WHERE/ORDER BY/
 * LIMIT over the materialized result (sql_exec.c's exec_select_view()).
 * Views inside JOINs, views of views, and INSERT/UPDATE/DELETE through
 * views are all rejected loud — each a real follow-on, not an oversight.
 *
 * Permission: executing a view runs the stored text under the CALLER's
 * uid (invoker's-rights, the simpler and safer first cut — a view owner
 * cannot use a view to grant access to rows the caller couldn't otherwise
 * see; definer's-rights, where the view runs under its OWNER's uid instead,
 * is the named alternative NOT taken here).
 *
 * ─── Namespace collision, named not silently ignored ────────────────────
 * view_create() refuses to shadow an existing TABLE name (a table and a
 * view share the same `FROM <name>` resolution space) — see
 * find_table_name_collision() below. The reverse is NOT defended:
 * rowstore_create_table() does not check the view registry, so a later
 * CREATE TABLE can still shadow an EARLIER view's name. In that case the
 * table always wins at query time (sql_exec.c's exec_select() resolves
 * find_table_catalog_index() before ever consulting the view registry),
 * so the view simply becomes unreachable by that name rather than causing
 * any crash or ambiguous result — a real, named gap, not a silent
 * correctness bug, and a natural follow-on if it ever matters in practice.
 */
#ifndef VIEW_H
#define VIEW_H

#include <stdint.h>
#include "sql_parser.h"   // OBJECT_NAME_LEN (via object_catalog.h), SQL_MAX_TEXT_LEN

#define VIEW_MAX          16
#define VIEW_SQL_TEXT_LEN SQL_MAX_TEXT_LEN

struct SLSViewDef {
    char     name[OBJECT_NAME_LEN];
    char     sql_text[VIEW_SQL_TEXT_LEN];   // the captured "AS <select...>" tail, verbatim
    uint32_t owner_uid;
    uint8_t  active;
};

extern struct SLSViewDef views[VIEW_MAX];

// view_create() return codes:
//   0 = success
//   1 = bad/empty/too-long name, sql_text too long, duplicate view name,
//       a TABLE already has this name (see the namespace-collision note
//       above), or the view table is full
int view_create(uint32_t caller_uid, const char* name, const char* sql_text);

// view_drop() return codes:
//   0 = success
//   1 = not found
//   2 = permission denied (caller is neither the owner nor ROLE_SYSTEM_KERNEL)
int view_drop(uint32_t caller_uid, const char* name);

// -1 if no active view has this name.
int view_find_index(const char* name);

#endif /* VIEW_H */
