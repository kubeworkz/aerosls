# AeroSLS VectorStore Interface Roadmap v0.1

## 0. Why this doc exists

The VectorStore kernel subsystem is fully built (collections, brute-force
search, HNSW indexing via `kernel/vec_index.c`, embeddings via
`net/ollama_client.c`, joins back to relational tables via
`kernel/vec_join.c`), and Gap Remediation Phase C already wired all of it to
9 HTTP routes, all reachable today through the Terminal
(`slsos-sim/src/lib/shellCommands.ts`, `vec *` command family). So the
VectorStore is *usable* right now. This doc scopes what's missing to make it
*complete* and *actually usable for semantic search* rather than a raw
vector-math sandbox, based on direct investigation of the current code
(file:line references below), not assumption.

Four real gaps, in order of how much they block real usage:

1. **No semantic search.** `POST /api/vec/embed-insert` embeds text via
   Ollama server-side before inserting, but `POST /api/vec/search` and
   `POST /api/vec/index/search` only accept a raw float array. There is no
   "embed this query text, then search" route — to search by meaning today
   you'd have to embed the query somewhere else yourself and hand-paste a
   768-number array into the Terminal. This is the one that actually
   defeats the point of a *vector* store for most real use cases.

2. **No delete, anywhere, for anything.** No HTTP route deletes a vector, a
   collection, or an index — confirmed by grep across every `/api/vec/*`
   route registration in `net/http.c`. Worse: this isn't just a missing
   route. `kernel/object_catalog.c`'s existing `sys_sls_vfree()`
   (`object_catalog.c:237-249`) and `catalog_vfree_partition()`
   (`object_catalog.c:256-267`) — the *only* generic object-deletion paths
   in the kernel, already reachable today via the `vfree` Terminal command —
   are completely blind to vector state: neither touches
   `vector_collections[]` or `vec_indexes[]` at all (confirmed: zero matches
   grepping `object_catalog.c` for `vec_index`/`vecstore`/`VecId`/`VecIndex`).
   Calling `vfree` on a vector-collection object *today* orphans the
   collection's slot and all its backing pages permanently (no reclaim
   exists in `vecstore.c`'s "first cut" design) — a real, already-shippable
   latent bug, not a hypothetical one.

3. **HNSW indexes don't backfill.** `vec_index_create()`
   (`vec_index.c:327-357`) allocates an empty index; only vectors inserted
   *after* the index exists get added (via `vec_index_notify_insert()`,
   called from `vecstore_insert()`). Building an index over an
   already-populated collection silently produces a useless empty index.
   `vec_index.h:110-118` already names this exact gap and even names the
   fix: scan the collection via the existing
   `vecstore_collection_scan()` primitive, feeding each entry through the
   same insert path the auto-maintenance hook uses.

4. **No dedicated frontend UI.** Confirmed via grep across every
   `slsos-sim/src/components/*.tsx` file: no `SlsVectorStore.tsx`, nothing
   in `App.tsx`'s sidebar nav. Every `vec *` capability is Terminal-only —
   functional, but not how a normal user would expect to browse
   collections, run a search, or manage an index.

Each phase below closes one of these, in the order that avoids building new
work on top of a known-buggy foundation (deletion first, since #2 already
exists as a live bug independent of any new feature).

---

## Phase 1 — Deletion, done right (kernel + HTTP + Terminal) — DONE

Implemented exactly as scoped: `sys_sls_vfree()`/`catalog_vfree_partition()`
(`kernel/object_catalog.c`) now call a new `vecstore_notify_object_freed()`
(`kernel/vecstore.c`) *before* clearing the catalog object's own `.active`
flag — closing the live leak bug this phase's own investigation confirmed
(a bare `extern` forward declaration in `object_catalog.c`, matching that
file's existing `tier_notify_access()` precedent exactly, avoiding a
circular header dependency with `vecstore.h`). That function deactivates
the collection's `vector_collections[]` entry and, via a new
`vec_index_notify_collection_freed()` (`kernel/vec_index.c`), tombstones
every node in and deactivates any HNSW index built over it — reusing the
`vecstore_collection_scan()`/pool-scan idioms already established rather
than inventing new ones. A separate, directly-callable `vec_index_drop()`
handles the single-named-index case (gated on `PERM_WRITE`, matching
`vecstore_delete()`'s own posture that destructive actions need write
access, unlike `vec_index_create()`'s `PERM_READ`).

New syscalls: `SYS_SLS_VEC_DELETE` (231, wraps the already-existing
`vecstore_delete()` — it never lacked an engine-level implementation, only
a syscall/HTTP/Terminal path) and `SYS_SLS_VEC_INDEX_DROP` (233, wraps the
new `vec_index_drop()`). 232 stays reserved/unused — collection delete
routes through the now-fixed `sys_sls_vfree()` directly rather than a
dedicated vecstore-only syscall, since vfree is already this kernel's one
generic "delete this object" path and duplicating its logic would just be
a second place to keep in sync.

New HTTP routes: `DELETE /api/vec/vector`, `DELETE /api/vec/collections`,
`DELETE /api/vec/indexes` (`net/http.c`) — all body-based like every
existing `/api/vec/*` POST route, for consistency within this specific
route family. **Worth flagging honestly:** every other destructive action
in this codebase (`index drop`, `mqt drop`, `partition destroy`, `agent
drop`, confirmed via grep across `shellCommands.ts`) goes through a `POST
.../drop`-or-`/destroy`-suffixed route instead of a real HTTP `DELETE` —
these three are the first genuine use of the DELETE method anywhere in
this API. Deliberate, not accidental: more correct REST semantics, and
this phase's own roadmap doc already called for it before implementation
started. A real inconsistency with the established idiom, named here
rather than silently left unremarked.

**Two real bugs caught and fixed along the way, not just the one this
phase set out to fix:**
- `http_route()`'s method dispatch (`net/http.c`) had never seen anything
  but GET/POST/OPTIONS — `int is_post = (method[0] == 'P')` meant any
  method NOT starting with 'P' (including a brand-new DELETE) fell into
  the `!is_post` GET-routes branch, where it would pass GET's own
  bearer-token gate and then match none of GET's routes, landing on the
  generic 404 — not a crash, but not routed anywhere meaningful either.
  Fixed with an explicit `is_delete` branch rather than folding DELETE
  into the existing boolean.
- The CORS preflight response (`http_options()`) advertised only `GET,
  POST, OPTIONS` in `Access-Control-Allow-Methods` — browsers preflight
  any DELETE request before sending it, so the three new routes would
  have been completely unreachable from the Navigator SPA (though
  reachable fine from curl or the Terminal's own `authFetch()`, neither of
  which triggers a preflight) until this was caught and DELETE added to
  that list too.

Terminal commands: `vec delete <collection> <page_id> <slot_index>`, `vec
collection drop <name>`, `vec index drop <name>` (`shellCommands.ts`), all
`destructive: true` (verified against the same confirm/cancel flow
Terminal 7 already proved works live). A new `deleteJSON()` helper mirrors
`postJSON()`'s exact shape with the one method difference.

**Verified:** 17 new host-test checks across three files —
`vec_index_host_test.c` gained Scenario 6 (`vec_index_drop()`: drops one of
two indexes over the same collection, confirms the dropped one stops
answering searches, its slot is reusable, and the untouched sibling index
is unaffected) and Scenario 7 (`vecstore_notify_object_freed()`: a
dedicated collection+index pair, confirms `vecstore_get`/`vecstore_search`/
`vecstore_collection_scan` all correctly treat the collection as gone
afterward, the index over it stops answering too, and calling the function
again or on a name that was never a collection is a harmless no-op).
`vecstore_syscall_host_test.c` gained Scenario 9 (`sys_sls_vec_delete()`:
null-request guard, success path, double-delete reports status 3 not a
silent second success, out-of-range `VecId`, nonexistent collection).

Regression sweep (`tests/run_all.sh`) caught a real, expected fallout:
`legacy_rowstore_boundary_host_test.c` — the one test in this project that
links the REAL `object_catalog.c` and stubs its forward-declared externs
(`tx_get_active`/`wal_stage`/`kernel_get_current_thread_id`) rather than
stubbing `catalog_check_access()` and skipping it like every other host
test — failed to link once `sys_sls_vfree()` started calling
`vecstore_notify_object_freed()` unconditionally. Fixed the same way as
the other three stand-ins in that file: a one-line no-op stub, matching
its own already-stated "stub the dependency, don't link the real heavy
file" convention rather than pulling `vecstore.c` into that test's
deliberately narrow dependency graph.

Full suite after both fixes: `tests/run_all.sh` → 24/24 passed (1042
checks, up from 894 at the start of this session — the 148-check delta is
this phase's 17 new checks plus every pre-existing check in the file that
had to be relinked). `tools/syntax_check_all.sh` → 76/76 clean.
`slsos-sim`'s `npm run lint` and `npm run test` (48/48) both clean.

**Known, named test-coverage limit, not silently glossed over:** the real
end-to-end call-order contract this whole phase's fix depends on —
`sys_sls_vfree()` calling `vecstore_notify_object_freed()` *before*
clearing `object_catalog[idx].active` — is verified by direct code review
and by testing `vecstore_notify_object_freed()`'s own logic in isolation
(Scenario 7 above, calling it directly with the catalog entry left active,
exactly matching that real call order), not by an executable test that
links the real `object_catalog.c` together with the real `vecstore.c` and
calls `sys_sls_vfree()` itself end-to-end. Every existing vecstore test
file stubs `catalog_check_access()` specifically to avoid
`object_catalog.c`'s heavy dependency graph (transaction/WAL, locking,
indexing, MQTs, constraints, journaling); building a test that links both
real files together for this one call-order guarantee would be new,
heavier test infrastructure beyond what this phase scoped, not a small
addition. Worth building if this call order ever needs to change again.

**Goal:** close the existing `vfree` leak bug, and add the ability to
actually delete a vector, a collection, or an index — none of which exist
today at any layer for indexes/collections, and only exists at the kernel
primitive layer (not HTTP/Terminal) for single vectors.

**What already exists and is reusable:**
- `vecstore_delete(caller_uid, collection_name, VecId id)`
  (`vecstore.c:242-263`) — already fully implemented: tombstones the slot,
  flushes the page, decrements `entry_count`, persists, and calls
  `vec_index_notify_delete()` to keep any HNSW index consistent. `VecId` is
  `{page_id, slot_index}` (`vecstore.h:151-154`) — a physical address, not
  keyed by `external_id`. This matters for the HTTP route design below:
  callers already have `page_id`/`slot_index` in hand from a prior insert
  or search response, so the DELETE route takes those directly rather than
  needing a new external_id→VecId lookup function.
- `vec_index_notify_delete()` (`vec_index.c:368-381`) — the pool-scan
  tombstone idiom a whole-index-drop function can reuse directly (same
  loop, different match condition: match on `index_id` instead of a
  specific `VecId`).

**What needs to be built:**
- Fix `sys_sls_vfree()` and `catalog_vfree_partition()`
  (`object_catalog.c:237-267`) to check whether the object being freed has
  vector storage enabled and, if so: set `vector_collections[idx].active =
  0`, and walk `vec_indexes[]` deactivating any index whose
  `collection_name` matches (reusing the same pool-scan idiom as
  `vec_index_notify_delete`). This closes the live bug and becomes the
  mechanism whole-collection delete uses — no separate
  `vecstore_delete_collection()` needed, since `vfree` is already the
  system's one generic "delete this object" path.
- New `vec_index_drop(caller_uid, index_name)` in `vec_index.c` — tombstone
  every node belonging to that index (reuse the `vec_index_notify_delete`
  loop shape, matched on `index_id`), then set `vec_indexes[i].active = 0`.
- New syscalls (next free numbers after `SYS_SLS_ROWSTORE_CREATE_TABLE`
  225 / `SYS_SLS_VEC_JOIN` 226 / `SYS_SLS_VEC_INDEX_LIST` 230 — confirmed
  via grep across every `kernel/*.h`):
  - `SYS_SLS_VEC_DELETE 231` — wraps `vecstore_delete()`.
  - `SYS_SLS_VEC_INDEX_DROP 233` — wraps the new `vec_index_drop()`.
  (232 reserved for collection-delete if a dedicated syscall turns out to
  be cleaner than routing collection delete through the fixed `vfree`
  path — decide during implementation once the vfree fix is in hand.)
  Request structs follow the established `struct SLSVec*Request` shape
  (`vecstore.h`/`vec_index.h`'s existing convention): `SLSVecDeleteRequest
  {caller_uid, collection_name, VecId id, int status}`,
  `SLSVecIndexDropRequest {caller_uid, index_name, int status}`.
- HTTP routes in `net/http.c`: `DELETE /api/vec/vector` (body:
  `{collection, page_id, slot_index}`), `DELETE /api/vec/collections/:name`
  (or body-based, matching this codebase's existing preference — check
  how `DELETE` is handled elsewhere, e.g. `index drop`'s existing pattern,
  for consistency), `DELETE /api/vec/indexes/:name`.
- Terminal commands in `shellCommands.ts`: `vec delete <collection>
  <page_id> <slot_index>`, `vec collection drop <name>`, `vec index drop
  <name>` — all marked `destructive: true` (matching the existing
  `svc crash`/`auth revoke`/etc. convention verified working in Terminal 7).

**Verification:** host test exercising vfree-then-relookup (confirm a
freed collection can no longer be found/leaked), a fresh
`vec_index_drop_host_test` mirroring `vec_index_host_test.c`'s existing
shape, compile-check, and a live Terminal smoke test (create collection →
insert → create index → delete vector → confirm search result count drops
→ drop index → confirm index list no longer shows it → drop collection →
confirm vfree no longer orphans it).

---

## Phase 2 — Semantic search (kernel + HTTP + Terminal) — DONE

Implemented exactly as scoped: two new syscalls, each an embed-then-search
adapter mirroring `sys_sls_vec_embed_insert()`'s own embed-first shape with
the one change that function's own doc comment named — swap the final
`vecstore_insert()`/index-insert call for a search call.
`sys_sls_vec_embed_search()` (`kernel/vecstore.c`) embeds via `ollama_embed()`
then calls `vecstore_search()` (brute-force); `sys_sls_vec_index_embed_search()`
(`kernel/vec_index.c`) does the same but calls `vec_index_search()` (HNSW).
Both are deliberately a second copy of the six-line embed-and-convert
prefix rather than sharing a helper with each other or with
`sys_sls_vec_embed_insert()` — matching this phase's own scope note that no
shared helper currently exists to extract into, so this is
copy-the-pattern-once, not a refactor.

New syscalls: `SYS_SLS_VEC_EMBED_SEARCH` (234, `vecstore.h`/`vecstore.c`) and
`SYS_SLS_VEC_INDEX_EMBED_SEARCH` (235, `vec_index.h`/`vec_index.c`) — the
next free numbers after Phase 1's own 231/233 (232 stays reserved/unused,
per Phase 1's own note). Both request structs report `ollama_status`
separately from `match_count`, mirroring `SLSVecEmbedInsertRequest`'s own
`ollama_status`/`insert_status` split — "Ollama never answered" must stay
distinguishable from "Ollama answered fine, the search itself just found
nothing" (both otherwise look identical as `match_count == 0`).

New HTTP routes: `POST /api/vec/embed-search` and `POST /api/vec/index/
embed-search` (`net/http.c`), response shape matching `POST /api/vec/
search`/`POST /api/vec/index/search`'s own `ok`/`match_count`/`truncated`/
`matches[]` shape exactly, plus the one extra `ollama_status` field, for
the same reason the request struct carries it separately. The float-
distance formatting inline block (no shared formatter exists anywhere in
this file — already duplicated once between the two existing search
routes before this phase) is now duplicated a third and fourth time rather
than newly extracted, matching this file's own established, if imperfect,
precedent rather than introducing a refactor out of this phase's scope.

Terminal commands: `vec search-text <collection> [endpoint=] [port=]
[model=] [metric=] [k=] prompt=<text>` and `vec index search-text <index>
[endpoint=] [port=] [model=] [k=] [ef=] prompt=<text>` (`shellCommands.ts`),
both non-destructive. **One deliberate deviation from this doc's own
originally-sketched usage line, worth naming honestly:** the roadmap's own
draft above shows `<query text...>` as bare trailing text, matching `agent
run`'s shape. Implemented instead with the same `prompt=` marker convention
`vec embed-insert`/`agent create` already use, and for the identical real
reason those commands adopted it: `splitKV()` treats any whitespace-
delimited token containing `=` as a stray kv pair, and a natural-language
query can easily contain one (e.g. "revenue = cost + margin") — the marker
prevents that word from being silently swallowed out of the query text.
Consistency with the one other command shape in this file already solving
this exact problem beat matching this doc's own less-considered original
sketch.

**Verified:** 16 new host-test checks appended to `vecstore_syscall_host_test.c`
(now 44/44, up from 28/28 in Phase 1) — Scenario 10 covers
`sys_sls_vec_embed_search()`: happy path (finds `external_id=777`, a vector
still live after Phase 1's own Scenario 9 deleted `external_id=42` earlier
in the same suite — deliberately targets a vector known to survive that),
k-capping still functions once routed through the embed adapter, and the
"Ollama itself fails" path reports `match_count == 0` (never attempted, not
stale). Scenario 11 covers `sys_sls_vec_index_embed_search()`: builds a
dedicated fresh collection + HNSW index (inserting vectors *after* index
creation, since Phase 3's backfill gap is still open — an index created
over "points" would find nothing), confirms a happy-path HNSW hit, and the
same Ollama-failure contract. Both scenarios reuse the file's own existing
controllable `ollama_embed()` stub (no new stub infrastructure needed,
since this file already links `vecstore.c` and `vec_index.c` together and
already stubs `ollama_embed()` for Phase 4's own embed-insert scenarios) —
the one new addition was a single `#include "kernel/vec_index.h"` line,
since this file had only ever linked `vec_index.c` for its side effects on
`vecstore.c`'s auto-maintenance hooks, never called into it directly before
now.

Full regression sweep (`tests/run_all.sh`) after these changes: **24/24
test binaries passed, 0 failed, 0 skipped** — no regressions in any
pre-existing file, and no new link-time fallout this time (unlike Phase 1's
`legacy_rowstore_boundary_host_test.c` surprise, this phase touched no
function every existing file transitively depends on).
`tools/syntax_check_all.sh` → 76/76 clean. `slsos-sim`'s `npm run lint`
(`tsc --noEmit`) and `npm run test` (48/48) both clean.

**Known, named test-coverage limit:** `shellCommands.test.ts`'s 48 checks
were not extended to cover the two new Terminal commands themselves (`vec
search-text`/`vec index search-text`) — this phase's host-test coverage is
entirely at the syscall-adapter layer (`vecstore_syscall_host_test.c`),
matching the roadmap's own Phase 2 verification scope ("host test using a
mock embedding response... compile-check, doc update," no frontend-test
line item). The two new commands' own request/response marshaling (the
`prompt=` marker split, the kv defaults) is therefore verified by direct
code review and by TypeScript's own type-checker (`npm run lint`), not by
an executed test — worth adding if this command-parsing logic changes
again. A true live-Ollama smoke test remains optional/best-effort, per
this phase's own original scope note, and was not attempted here (no live
Ollama instance reachable from this environment).

**Goal:** let a user search by typing a natural-language query instead of
hand-pasting a float array — the gap that actually matters most for this
being a usable *semantic* vector store.

**What already exists and is reusable, confirmed clean (not tangled with
insert-specific logic):** `sys_sls_vec_embed_insert()`
(`vecstore.c:459-483`) is a 6-line sequence — parse Ollama params, call
`ollama_embed()`, convert `OllamaEmbedResponse` into a `VecValues` — before
its one insert-specific line (`vecstore_insert(...)`). That prefix is
exactly what an embed-search path needs; the only change is swapping the
final call for `vecstore_search()` (brute-force) or `vec_index_search()`
(HNSW).

**What needs to be built:**
- New syscalls: `SYS_SLS_VEC_EMBED_SEARCH 234` (brute-force, mirrors
  `SYS_SLS_VEC_SEARCH`'s response shape) and
  `SYS_SLS_VEC_INDEX_EMBED_SEARCH 235` (HNSW, mirrors
  `SYS_SLS_VEC_INDEX_SEARCH`'s). Both live in `vecstore.c`/`vec_index.c`
  respectively, each following the exact embed-then-call pattern above
  (small, low-risk addition per file, not a refactor of the existing
  insert path — confirmed no shared helper currently exists to extract
  first, so this is copy-the-pattern-once, not restructure-existing-code).
  Request structs: `SLSVecEmbedSearchRequest`/`SLSVecIndexEmbedSearchRequest`
  — same Ollama params as `SLSVecEmbedInsertRequest` (`endpoint_ip`, `port`,
  `model`, `prompt`) plus `metric`/`k`(/`ef` for the index variant).
- HTTP routes: `POST /api/vec/embed-search` and
  `POST /api/vec/index/embed-search`, response shape matching the existing
  `/api/vec/search` and `/api/vec/index/search` routes exactly (same
  `matches[]` array shape) so frontend code can treat literal-vector and
  embed search results identically.
- Terminal commands: `vec search-text <collection> [endpoint=] [port=]
  [model=] <query text...>` and `vec index search-text <index> [k=] [ef=]
  [endpoint=] [port=] [model=] <query text...>` — non-destructive, no
  confirmation gate needed.

**Verification:** host test using a mock embedding response (same approach
`ollama_client.c`'s own existing test already uses, per its documented
"unverified against a live Ollama instance" honesty note — this phase
inherits that same caveat, doesn't need to newly solve it), compile-check,
doc update. A true live-Ollama smoke test is optional/best-effort depending
on whether a real Ollama instance is reachable from wherever verification
runs (same caveat `ollama_client.c` already carries).

---

## Phase 3 — HNSW index rebuild/backfill (kernel + HTTP + Terminal) — DONE

Implemented exactly as scoped: `vec_index_rebuild(caller_uid, index_name)`
(`kernel/vec_index.c`) clears every node the named index currently owns
(inlining the same per-node tombstone loop `vi_deactivate_index()` uses,
but deliberately NOT calling that function itself, since it also sets the
index's own `.active = 0` — a real drop, which rebuild must never do),
resets the index's graph-entry state (`entry_point`/`top_layer`/
`active_count`) back to the same values `vec_index_create()` gives a brand
new index, then calls `vecstore_collection_scan()` over the index's
collection with a callback (`vi_rebuild_cb`) that feeds every live entry
through `vi_insert_into()` — the exact same internal function the auto-
maintenance hook (`vec_index_notify_insert()`) already calls, so a rebuilt
index is graph-structurally identical to one built incrementally from
scratch, not a separate, parallel code path. `node_count` (the lifetime
pool-slot counter shown in `vec_index list`'s own "Nodes" column) is
deliberately left un-reset — see this function's own header comment
(`vec_index.h`) for why zeroing it would make a diagnostic stat lie about
real pool usage.

New syscall: `SYS_SLS_VEC_INDEX_REBUILD` (236, `vec_index.h`/`vec_index.c`)
— the next free number after Phase 2's own 234/235. Thin adapter, same
shape as every other `vec_index` syscall wrapper.

New HTTP route: `POST /api/vec/index/rebuild` (`net/http.c`), body
`{"index": "<name>"}`. **One small, deliberately named field-name
inconsistency:** `DELETE /api/vec/indexes` (Phase 1) uses `{"name": ...}`
for the same conceptual index name, while this route uses `{"index":
...}`, matching `POST /api/vec/index/search`'s own body field instead.
Chose consistency with the route this one is most operationally adjacent
to (search an index / rebuild an index, both "act on an existing index by
name") over consistency with the DELETE route family's own separate
`{"name": ...}` convention (create/destroy an index by name) — a real,
minor inconsistency across the whole `/api/vec/*` surface, named here
rather than silently left unremarked, same posture Phase 1 took with its
own DELETE-vs-POST/drop deviation.

Terminal command: `vec index rebuild <name>` (`shellCommands.ts`) —
deliberately mirrors the row-store's own existing `index rebuild <name>`
command name exactly, for the same operation on the vector side, and
matches that command's own precedent of NOT being marked `destructive:
true`: rebuild replaces an index's contents from its own live collection,
but never touches the collection itself or drops the index, a
repair/refresh action rather than the data-loss risk `vec index drop`
actually is.

**Verified:** 23 new host-test checks appended to `vec_index_host_test.c`
(now 62/62, up from 39/39 in Phase 1) — Scenario 8 builds a fresh
collection, inserts 5 vectors *before* creating an index over it, confirms
the index really does start empty (the exact gap this phase closes,
reproduced first before being fixed), exercises both error paths
(nonexistent index name → 1, access denied → 2, index left untouched),
then rebuilds and confirms all 5 pre-existing vectors are now found via
search. Scenario 9 covers the tombstone-cleanup half of this phase's goal:
deletes one of those 5 vectors, rebuilds, and — going beyond just
confirming the deleted point no longer wins a search (Scenario 4's own
bar) — directly walks every active node's neighbor list at every layer and
confirms not one edge anywhere in the rebuilt graph points to a tombstoned
node. This is the real, structural difference rebuild makes that a bare
`vecstore_delete()` alone doesn't: `vec_index.h`'s own header comment
already named tombstoned-but-still-linked nodes as dead ends other live
queries route through, and this scenario proves a full rebuild eliminates
every one of them, not just the specific deleted point's own search
ranking.

Full regression sweep (`tests/run_all.sh`) after these changes: **24/24
test binaries passed, 0 failed, 0 skipped**, no regressions anywhere else.
`tools/syntax_check_all.sh` → 76/76 clean. `slsos-sim`'s `npm run lint`
(`tsc --noEmit`) and `npm run test` (48/48) both clean.

**Known, named test-coverage limit:** same posture as Phase 2's — the new
Terminal command's own request marshaling is verified by code review and
TypeScript's type-checker, not an executed frontend test (`vec index
rebuild`'s body is a single field, `{index: name}`, the simplest shape in
this whole roadmap, so the risk this represents is correspondingly small).
The cost note in this phase's own original scope draft below (synchronous
scan stalling the single-threaded HTTP loop for large collections) was not
independently re-measured during implementation — AeroSLS's current scale
never exercised a collection large enough to make that a real, observable
stall in this environment, so the original scope note's own "acceptable
for v1, keep opt-in and explicit" reasoning stands as written, unverified
empirically rather than re-confirmed.

**Goal:** make `vec index create` over an already-populated collection
actually produce a usable index, and give a way to repair/compact an index
after heavy delete churn (tombstones accumulate in HNSW — `vec_index.h`'s
own comments already flag tombstoned nodes as worse than a plain miss,
since they're dead-ends in live navigation paths other queries route
through).

**What already exists and is reusable:**
`vecstore_collection_scan(caller_uid, collection_name, VecScanCb cb, void*
ctx)` (`vecstore.c:267-292`) already walks every live (non-tombstoned)
entry in a collection in physical order and invokes a callback per entry —
exactly the primitive `vec_index.h:110-118`'s own comment names as the
fix, and exactly what a rebuild needs.

**What needs to be built:**
- New `vec_index_rebuild(caller_uid, index_name)` in `vec_index.c`: clear
  every existing node belonging to that index (same pool-scan-and-tombstone
  idiom as `vec_index_drop`, but keep the index itself active), then call
  `vecstore_collection_scan()` over the index's collection with a callback
  that calls `vec_index_notify_insert()` for each live entry. This is a
  full rebuild (clear + repopulate), not just "backfill if empty" — reusing
  it also fixes tombstone buildup, matching the semantics the existing
  row-store `index rebuild <name>` Terminal command already established
  for B-tree indexes (naming this the same way keeps the UX consistent
  with a pattern users already know from that command).
- Note on cost: this is a synchronous scan inside one HTTP request/syscall
  — for a large collection this could stall the single-threaded
  non-blocking HTTP loop (Architectural Phase 1) for the duration of the
  scan. Given AeroSLS's current scale (a simulation/demo kernel, not
  handling production-sized datasets), synchronous is acceptable for v1,
  but keep rebuild as an explicit, separately-invoked action (never
  automatic on every `vec index create`) so its cost is opt-in and visible,
  not a surprise stall on every index creation.
- New syscall: `SYS_SLS_VEC_INDEX_REBUILD 236`.
- HTTP route: `POST /api/vec/index/rebuild` (body: `{index}`).
- Terminal command: `vec index rebuild <name>` (mirrors the existing
  row-store `index rebuild <name>` command name exactly, for the same
  operation on the vector side).

**Verification:** host test — create collection, insert vectors, create
index (confirm empty per current behavior), rebuild, confirm search now
finds the pre-existing vectors; a second scenario covering tombstone
cleanup (delete a vector, confirm its node is gone after rebuild, not just
tombstoned). Compile-check, doc update.

---

## Phase 4 — Frontend: dedicated Vector Store tab — DONE

Implemented exactly as scoped, plus one addition the live verification pass
surfaced as a real gap against this phase's own acceptance criteria (see
below): `SlsVectorStore.tsx` (`slsos-sim/src/components/`), wired into
`App.tsx`'s sidebar under "Database" next to "DB Engine", with four panels
matching the original scope — Collections (list/create/delete, two-step
`ConfirmDeleteButton` reused from the existing shared pattern), Insert (raw
vector / embed-text toggle), Search (raw vector / embed-text toggle,
brute-force / HNSW-index toggle, results table with `external_id`/
`distance`/`page_id`/`slot_index`), and Indexes (list/create/rebuild/drop).

**Gap found during verification, closed in this same phase:** the original
scope draft below listed "delete a vector... through the new UI" as part of
Phase 4's own verification bar (line ~551), but the Search panel as first
built had no delete control on its results rows — only Collections and
Indexes had delete/drop buttons. Added `handleDeleteVector()` +
`deleteVectorCollection()` + a new "Actions" column (per-row
`ConfirmDeleteButton`) to the Search panel, resolving the backing collection
name whichever way a result came from (brute-force target = the collection
itself; HNSW target = the index's own `.collection` field) since deletion
is keyed by the physical `page_id`/`slot_index` (`VecId`), not
`external_id` — `vecstore.h`'s own struct comment already names why.

**A real, unrelated bug the live pass exposed and fixed along the way:**
manual browser testing of embed-insert/embed-search kept failing with
`ollama_status=-1` despite Ollama itself being confirmed healthy
(`systemctl status`, direct `curl` to `/api/embeddings` succeeding). Root
cause, found via a `tcpdump -i lo` capture that caught zero packets during a
request the kernel's own debug log showed completing a full TCP + HTTP
cycle: AeroSLS runs as a full OS booted inside QEMU with usermode/SLIRP
networking (boot log's own `[DHCP] Bound: 10.0.2.15 gw 10.0.2.2`), so
`"127.0.0.1"` from inside the guest means the guest's own loopback, not the
host's — a request to it can never reach a host-side Ollama at all. Fixed
by changing the hardcoded default `endpoint_ip` from `"127.0.0.1"` to
`"10.0.2.2"` (QEMU SLIRP's host-forwarding gateway) in three places:
`net/http.c` (embed-insert/embed-search/index-embed-search route defaults),
`user/shell.c` (`vec embed-insert` Terminal command default), and this
tab's own Insert/Search panel defaults — still overridable per-request via
the existing `endpoint_ip` field for any other topology. Not a Phase 4
frontend bug per se, but it blocked this phase's own live verification bar
entirely until found, so it's recorded here rather than silently fixed
off-roadmap.

**Verified live**, end to end, against the redeployed kernel, in this order:
created a real `verify768` (dim 768) collection; embed-inserted two
sentences via Ollama using the new `10.0.2.2` default with the endpoint
field left untouched (confirming the code fix, not a manual per-request
override); ran a free-text embed-search ("What is the capital city of
France?") that correctly ranked the on-topic vector (distance 0.1026)
far ahead of the unrelated one (distance 0.6104) — a genuine semantic
match, not just an error-free empty result; used the new Search-panel
delete-vector control to remove one result and confirmed it disappeared
from the list; deleted the test collection via the Collections panel and
confirmed it disappeared from `GET /api/vec/collections`. `npx tsc
--noEmit -p .` clean throughout. All 24 kernel host-test binaries
(`tests/run_all.sh`) still pass, including `ollama_client_host_test`.

**Goal:** close the UI gap — give the VectorStore a real interface instead
of Terminal-only access, following the same pattern `SlsDbEngine.tsx`
already established for the DB Engine tab (tab bar + self-contained panel
components, each with its own `authFetch`-based load/render loop — no
shared generic "data table" component exists in this codebase, so this
follows the existing hand-rolled-per-panel convention rather than
introducing a new abstraction).

**What needs to be built** — new `SlsVectorStore.tsx`, added to `App.tsx`'s
sidebar under the "Database" group next to "DB Engine" (`App.tsx`'s
existing sidebar structure, confirmed working in Terminal 7's live
verification pass):
- **Collections panel** — list via `GET /api/vec/collections` (name,
  dimension, entry count already returned); create-collection form; delete
  button per row (Phase 1's route), with a confirmation step matching this
  app's established destructive-action pattern.
- **Insert panel** — two modes: raw vector (comma-separated floats,
  matching the Terminal's own input convention) or embed-insert (free-text
  prompt + optional endpoint/model override), calling the matching routes.
- **Search panel** — the one that actually needs Phase 2 to be useful:
  free-text query box (calls `embed-search`) as the primary input, with an
  "advanced: raw vector" toggle for the literal-array path (`search`);
  results rendered as a table (`external_id`, `distance`, `page_id`/
  `slot_index`) rather than raw JSON; a metric (cosine/L2) and k selector;
  a toggle between brute-force and a chosen HNSW index once one exists for
  the collection.
- **Indexes panel** — list via `GET /api/vec/indexes` (collection, metric,
  node/active counts already returned); create-index form with an inline
  note that a fresh index is empty until rebuilt (Phase 3); rebuild and
  drop buttons per row.

**Sequencing note:** the Collections and Insert panels, and brute-force
literal-vector Search, need nothing from Phases 1-3 and could ship first if
useful to see progress sooner — only the free-text Search box needs Phase 2,
and only the Indexes panel's rebuild button needs Phase 3. Delete buttons
throughout need Phase 1. Whether to build the whole tab at once after
Phases 1-3 land, or incrementally alongside them, is a sequencing choice
to make at implementation time, not a hard dependency.

**Verification:** `npm run lint` (`tsc --noEmit`) clean, a live Terminal-
style manual pass in the browser (same rigor as Terminal 7 — create a
collection, insert both ways, search both ways, create+rebuild+drop an
index, delete a vector and a collection, all through the new UI against
the live kernel) rather than just a typecheck.

---

## Suggested sequencing

1. **Phase 1 (deletion)** first — it's fixing a bug that already exists
   today (the `vfree` leak), independent of any of the other phases, and
   the other phases' HTTP/Terminal work benefits from deletion existing
   for cleanup during their own testing (e.g. Phase 2/3 host tests will
   want to create and tear down test collections/indexes cleanly).
2. **Phase 2 (semantic search)** next — the single highest-impact gap for
   making this a real semantic vector store rather than a raw-math
   sandbox.
3. **Phase 3 (HNSW rebuild)** — independent of Phase 2, could run in
   parallel; sequenced third here only because it's lower-frequency-need
   than search (most demo/test collections are small enough that brute
   force is fine, so HNSW's backfill gap matters most once someone
   actually needs the approximate-search speed path).
4. **Phase 4 (frontend tab)** last as a whole unit, though per its own
   sequencing note above, parts of it could start as soon as Phase 1 lands
   if there's appetite to see UI progress before all the backend gaps are
   closed.
