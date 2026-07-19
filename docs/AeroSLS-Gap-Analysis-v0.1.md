# AeroSLS Gap Analysis v0.1 — where the system actually stands before the frontend

## 0. Purpose and method

Six roadmaps have been built and marked done across this project: the TIMI/SLIC instruction set and toolchain, the LPAR/partition subsystem, the RDBMS engine, and the Vector Store (including its HNSW stretch phase). Before starting `/slos-sim` — a new web frontend styled on IBM i Navigator — this document asks the obvious next question: what's actually usable today, end to end, versus what only exists as C code a host test can reach?

This is not a re-litigation of any roadmap's own scope decisions. Every roadmap named its own deliberate cuts honestly in its own findings addenda, and those are respected here, not re-argued. This document instead looks for two different kinds of gap that don't show up phase-by-phase: (1) capabilities that exist in the kernel but that no live caller — shell, syscall from a real client, or HTTP — can actually reach, and (2) system-wide properties (does it boot, does anything persist, is anything secured) that only become visible once you stop looking at one phase at a time.

**Methodology and its own limits.** This analysis is a documentation-and-source audit — reading every roadmap doc's findings addenda, grepping `kernel/syscall_dispatch.c`, `user/shell.c`, and `net/http.c`'s `http_route()` for what's actually wired up, and cross-checking claims against real line numbers. It found real, previously-unstated gaps this way (see §1 and §7). What it did **not** do, and could not do in this environment: actually cross-compile or boot AeroSLS. That limit is itself the first and most important finding.

## 1. The verification ceiling every phase has operated under, made explicit

Every single phase across all six roadmaps was verified the same way: host-compiled unit tests (`gcc` on a normal Linux host, linking real kernel `.c` files against hand-written stubs for heavy dependencies) plus `gcc -fsyntax-only -ffreestanding`-style checks, with a documented fallback to "compile-check plus targeted grep" for pieces too kernel-wired to host-test at all. This has been genuinely rigorous for **logic correctness** — real execution, real assertions, real bugs caught (the Phase 12 scheduler starvation bug, the Phase 24 `uses_rowstore` guard gap, this session's own vec_join batching bug and vec_index recall variance, among others).

It has never once proven that **the kernel as a whole cross-compiles and boots.** Confirmed directly in this session: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, and `qemu-system-x86_64` are not installed in this development sandbox (only `grub-mkrescue` is present) — the `setup-toolchain.sh` script in the repo root documents that this toolchain requires a 20-40 minute from-source OSDev-style build, and it has never been run here. The tracked `my_sls_kernel.bin` (1.1 MB) is stale: its modification time corresponds to roughly the "added more real DB capability" commit, **before** RDBMS Phases 16-24 and the entire Vector Store roadmap existed. No version of the kernel containing this session's work — most of the RDBMS engine and 100% of the Vector Store — has ever been linked as a whole binary, let alone booted.

This doesn't mean the code is wrong — the host-test discipline has been unusually thorough by most standards. It means **"we have all the features we require" is true at the level of "the logic has been proven correct in isolation," not yet at the level of "this boots and runs as one system."** Before or alongside frontend work, getting a real cross-compiler into whatever environment will actually run this (the user's own machine, most likely, given `my_sls_kernel.bin`'s provenance) and doing one real `make && x86-run` cycle would upgrade every finding in this document from "confirmed in source" to "confirmed running" — genuinely the single highest-leverage next step, independent of the frontend.

## 2. The pattern that shows up in every subsystem: implemented, but unreachable

This is the most consistent finding across all four roadmaps, and it matters most for frontend scoping because a frontend can only be as capable as its backend surface. The same shape recurs everywhere:

| Capability | Exists in C? | Syscall? | Shell command? | HTTP route? |
|---|---|---|---|---|
| Create a row-set table | ✅ `rowstore_create_table()` | ❌ never wrapped | ❌ | ❌ |
| Run SQL (SELECT/INSERT/UPDATE/DELETE/JOIN) | ✅ `sql_execute()` | ✅ `SYS_SLS_SQL_EXECUTE` | ✅ `sql <stmt>` | ❌ zero "sql" routes |
| Create/manage a partition | ✅ `partition.c`, syscalls 210-216 all wired | ✅ | ❌ | ❌ |
| List a partition's frame quota usage | ✅ `sys_sls_partition_quota_list()` | ❌ no syscall number assigned at all | ❌ | ❌ |
| Inspect a loaded TIMI program's header/entry points | ✅ `loader_timi_info()` | ✅ `SYS_SLS_TIMI_INFO` | ❌ (serial-console print only, no return value to caller) | ❌ |
| Resolve a vector search back to relational rows | ✅ `vec_join_resolve()` | ❌ | ❌ | ❌ |
| Approximate (HNSW) vector search | ✅ `vec_index_search()` | ❌ | ❌ | ❌ |
| Create/insert/search a vector collection (exact) | ✅ | ✅ `SYS_SLS_VEC_*` (221-224) | ✅ `vec create/insert/embed-insert/search` | ❌ zero "vec" routes |
| List existing vector collections/indexes | ✅ data exists in `vector_collections[]`/`vec_indexes[]` | ❌ no list/stat syscall was ever built | ❌ | ❌ |

Two of these (`rowstore_create_table()`, `sys_sls_partition_quota_list()`) are dead ends with **no live path at all**, not even a partial one — confirmed independently by both research agents, and the `rowstore_create_table()` gap is significant enough that this project's own Vector Store Phase 4 findings addendum already named it in passing while building something else. The others have a syscall or shell path but no HTTP path, which matters specifically because `/slos-sim` will almost certainly talk to the kernel over HTTP, not by embedding a shell.

**The practical consequence for `/slos-sim`, stated plainly: there is currently no way, over HTTP, to create a new table, run a SQL query, manage a partition, or touch the vector store in any way.** The existing HTTP API (see §5) is rich for the *legacy* catalog/process/program/journal/cursor/agent surface — genuinely useful groundwork — but it predates the RDBMS and Vector Store roadmaps entirely and was never extended to cover either.

## 3. TIMI / SLIC toolchain

What's real and solid: a documented ISA (through v0.3, object-typed opcodes), a working assembler/interpreter/disassembler, two native translator targets (x86-64 wired into the kernel and execution-verified on real QEMU hardware at Phase 4; RV64 verified only at the host-toolchain level via a custom, admittedly-weaker decoder), capability tags, and authority-checked `RESOLVE`. Programs (including TIMI ones) upload and spawn transparently through the existing generic `/api/program/*` HTTP routes and shell commands — this part **is** reachable today.

Concrete gaps, most to least significant for a frontend:

- **No compiler exists that targets TIMI.** Every `.timi` file in the repo is hand-written assembly. `compiler/SLSAllocationPassV2.cpp` is an unrelated LLVM pass for native ELF globals. If the frontend's story is ever "write and run a program," there is currently no path from source text to TIMI bytecode other than hand assembly.
- **Introspection writes to the serial console, not to a caller.** `loader_timi_info()` and the partition-list equivalent (§2) both `kernel_serial_printf` their output rather than filling a caller-supplied buffer — the same refactor every other `/api/*` handler in `net/http.c` already uses (a JSON-builder pattern) would need to be applied before either could back a frontend view.
- **The HTTP program-spawn path hardcodes `owner_uid = 0`** (`net/http.c`, `api_program_create`/`api_program_spawn_handler`) — bearer-token identity is extracted and threaded through for agent/workflow routes in the same file but never applied to program routes. Every program spawned via HTTP today runs as uid 0/`PARTITION_SYSTEM`, meaning Phase 9's partition-gated spawn permission model has zero effect through the surface a web frontend would use.
- Unhandled `DIV`/`MOD`-by-zero traps at the kernel level (no ISR0 handler exists anywhere in the tree) — a running TIMI program hitting this will fault the machine, not fail cleanly.
- No capability propagation across `CALL`/`RET`, no capability revocation, a hard 64-register ceiling on the native translators (v1 narrowing from the ISA's own 1024), and orphaned code frames on re-upload (this kernel has no frame-free primitive at all — same root cause as the LPAR leak below).
- `user/libsls/sls.h` (the userspace syscall binding library) exposes none of the TIMI or partition syscalls — a native program can't call `SYS_SLS_TIMI_INFO` or any `SYS_SLS_PARTITION_*` without hand-rolling the trap itself.

## 4. LPAR / partition subsystem

What's real: partitions genuinely gate object access, process spawn, IPC, scheduling fairness (with a real starvation bug caught and fixed in Phase 12), and physical memory quotas — all backed by host-executed test suites, and partition identity/assignment genuinely persists across reboot (Phase 10, execution-verified).

Concrete gaps:

- **Zero shell commands and zero HTTP routes for partitions at all** — confirmed exhaustively (no `partition` command anywhere in `user/shell.c`'s 90+ dispatch branches; no `/api/partition/*` route in `net/http.c`). Every partition syscall is correctly wired in `syscall_dispatch.c` and completely unreachable from any user- or network-facing surface. For frontend planning purposes: **there is no existing API layer to build a partition-management view against — this would be new backend work from the ground up**, not a wiring exercise.
- **`sys_sls_partition_quota_list()` is dead code** — implemented, never assigned a syscall number, never referenced in the dispatcher. Identical shape to the `rowstore_create_table()` gap.
- **Destroying a partition cannot reclaim physical memory — a real, permanent leak**, named explicitly in this project's own Phase 14 findings addendum: this kernel has no `free_physical_ram_frame()` at all, for any subsystem, so `partition_destroy()` zeroes a usage *counter*, not the underlying `physical_memory_bitmap` allocation. A deployment that creates/destroys partitions repeatedly exhausts memory over time.
- Partition frame quotas do **not** persist across reboot (only partition identity/assignment does) — a quota configured today silently reverts to unlimited after any restart, which could give a false sense of durable tenant isolation.
- The existing HTTP listing routes (`/api/scan`, `/api/objects`, etc.) are entirely partition-blind: they iterate the whole catalog with no partition filter and no `partition_id` field in any response. If `/slos-sim` needs to respect partition boundaries in what it displays, that filtering doesn't exist server-side today.
- Stream/blob storage was explicitly left un-partitioned (no owner/partition identity on `StreamEntry` at all) — named as a deliberate, acknowledged gap in the Phase 13 findings.

## 5. RDBMS engine

What's real: row-set storage, B-tree indexing, a real WHERE/predicate engine, a working SQL parser and executor (SELECT/INSERT/UPDATE/DELETE/two-table INNER JOIN), MVCC-based autocommit concurrency, constraints (UNIQUE/NOT NULL/RANGE/FOREIGN KEY), and row-level journaling — each phase genuinely host-test-verified, several with real bugs caught along the way.

Concrete gaps, ranked by how much they'd block a management console:

- **No HTTP route reaches the SQL engine at all** (confirmed: zero "sql" hits in `net/http.c`). The closest-looking existing route, `GET /api/query`, is not a real query — it's a keyword classifier (`query_engine.c`'s "Cognitive Direct Object Space Query... No SQL") that ignores its `q` parameter for filtering and just dumps the entire legacy catalog. **This is the same reachability gap as §2's table above, restated at the engine level: today, over HTTP, you cannot create a table, run a SELECT, or get a filtered result from the relational engine at all.**
- **The legacy KV path cannot substitute**, and is actively blocked once it would need to: `sys_sls_valloc`/`schema_set`/`insert` never promote an object to a row-set table (only `rowstore_create_table()` does that, and nothing calls it outside host tests), and Phase 24 added an explicit guard that *rejects* legacy DML once an object *is* a row-set table. There is no live path — shell, syscall, or HTTP — to create a new row-set table anywhere in this system today.
- **A real, currently-invisible performance regression**: this project's own Phase 22 findings addendum states that once `sql_exec.c` was rewired through MVCC, the index-aware query planner was removed entirely — every live SQL query today is a full table scan, regardless of any B-tree index built on the table. The indexes are still correctly maintained on every write; they're just never consulted by the only live query path.
- **Indexes, constraints, and the journal are 100% RAM-only** and evaporate on every reboot — while the row *data* itself does survive (real NVMe paging, execution-verified). A frontend built against this today would show a table with its data intact after a restart, silently missing every UNIQUE/NOT NULL/RANGE/FK guarantee and the entire audit trail, with no live way to notice or rebuild any of it.
- SQL grammar gaps, all explicitly named in their own phases rather than hidden: no DDL of any kind (no CREATE/ALTER/DROP TABLE, no CREATE INDEX), no subqueries, no views, no `GROUP BY`/aggregates in SQL (aggregation only exists via the older, separate `aggregate.c`/`mqt.c` API, which — per this project's own Phase 24 audit — runs over legacy attribute-bag objects, not row-set tables, so it's the wrong data model for aggregating real SQL table data), no `IN` lists, no `LIKE`, no arithmetic/string functions, no `BEGIN`/`COMMIT`/`ROLLBACK` keywords. Column projection (`SELECT col1`) is metadata-only — the full row is always materialized underneath regardless of the SELECT list, worth knowing so a frontend never assumes a narrow SELECT limits what's actually in the row payload.

## 6. Security and networking

**Auth is real but shallow and inconsistently enforced — the most important callout before pointing anything beyond localhost/QEMU at this system.** Every HTTP GET route is completely unauthenticated (the entire catalog, WAL, journal contents, and process table are readable by anyone with network access, no token required); POST routes get one binary gate (any non-GUEST token passes, with no per-endpoint role distinction, and `catalog_check_access()`'s real per-object authorization frequently never receives the right caller identity to check against). The four demo accounts carry hardcoded, non-expiring tokens compiled directly into the source and printed to the boot log. There is no TLS anywhere in the networking stack (confirmed by grep — zero hits for `tls`/`ssl`/`aes` across `net/*`), so all of this travels in cleartext. `sys_sls_secure_seal()` — the one API that looks like it offers encryption — derives a key and never uses it to encrypt anything; it logs a success message and does nothing to the underlying data. None of this is a defect in any single phase's own scope — it's a genuine, system-level gap worth fixing or at minimum clearly labeling before `/slos-sim` exposes any of it as a real "secure" action in a UI.

**Networking**: real DHCP client, no DNS at all (every outbound connection, including the Ollama client, needs a literal dotted-decimal IP). The Ollama client itself was, by this project's own documentation, never tested against a live server — network access was sandboxed throughout its own development — and it targets Ollama's legacy `/api/embeddings` shape rather than the newer `/api/embed`, a real, named, still-unverified compatibility assumption. No WebSocket or Server-Sent-Events support anywhere — `/slos-sim` cannot get live/streaming updates from this backend today without either polling or new engine work on `net/http.c`/`net/tcp.c`. TCP itself is real but capped at 8 simultaneous connections with no retransmission/congestion control beyond SYN retry — fine for a single-frontend/localhost setup, a real constraint if `/slos-sim` ever needs many concurrent clients.

## 7. Vector Store — this session's own work, audited the same way

Applying the same reachability lens to the roadmap just finished:

- **Zero HTTP routes** for any vector-store capability — confirmed independently by both research agents (zero "vec" hits in `net/http.c`), exactly parallel to the SQL engine's own gap. Exact create/insert/embed-insert/search (Phase 4) do have syscalls and shell commands; nothing vector-related is reachable over HTTP today.
- **`vec_join_resolve()` (Phase 5) and `vec_index_search()`/the whole HNSW index (Phase 6) have no syscall or shell surface at all** — both are, today, pure C functions reachable only from their own host tests, the same "implemented, unreachable" shape as `rowstore_create_table()`. This was a deliberate scope decision in both phases (no reachability *blocker* forced a live surface the way Phase 4's `SYS_SLS_VEC_CREATE` was forced), but it means neither capability can be demonstrated to a user or a frontend today without new syscall/dispatch/shell work first.
- **No list/stat capability for collections or indexes** — there's no way to enumerate "what vector collections exist" or "what indexes are defined on them" via syscall, shell, or HTTP; a caller has to already know a collection's name.
- **100% RAM-only**, same as the RDBMS engine's newer phases: collections, embeddings, and the HNSW graph all evaporate on reboot with no persistence and no live rebuild path.
- **Phase 6 (HNSW) was built ahead of its own stated trigger condition** ("revisit only once brute-force is demonstrably too slow — not before") on an explicit request, not because a real workload needed it — recorded honestly in that phase's own header comments and findings addendum, restated here as a fact worth knowing rather than a criticism.
- HNSW deletion is tombstone-only with no neighbor re-linking, a real (and, per that phase's own documentation, more severe than its B-tree analog) risk of graph fragmentation and degraded recall under heavy delete churn — not yet stress-tested at scale.
- The Ollama embedding pipeline this subsystem depends on carries the same unverified-against-a-live-server caveat named in §6.

## 8. What this means for scoping `/slos-sim`

Ranked by how directly each finding constrains what an IBM-i-Navigator-style console can actually do on day one:

1. **The existing HTTP API is real groundwork, but it's a console for the *legacy* system (objects, records, processes, programs, journals, cursors, agents/workflows) — not for the RDBMS engine or Vector Store this whole project spent most of its recent effort on.** A frontend that wants to browse/query real SQL tables or vector collections needs new backend routes first; there's no way around this by picking a different frontend approach.
2. **Nothing in this system has ever been proven to boot as a whole.** Getting a real cross-compile + boot cycle running (§1) is independent of frontend work but arguably higher priority — it would validate (or falsify) everything else in this document at once.
3. **Auth/security is not production-shaped** (§6) — fine for a local dev console talking to a QEMU instance on localhost, a real problem the moment `/slos-sim` is reachable from anywhere else. Worth deciding explicitly, up front, whether this frontend is scoped as "trusted local dev tool" (matches the system's current actual security posture honestly) or "needs the backend hardened first."
4. **No live way to create a table, define an index, or set up a vector collection outside a host test** (§2, §5, §7) — an IBM i Navigator-style "create a new database table" wizard has no backend to call yet.
5. **Everything interesting (indexes, constraints, journal, vector store) is RAM-only** (§5, §7) — a console needs to either surface this honestly (a "this resets on reboot" indicator) or the persistence gaps need closing first, depending on how permanent the demo/deployment is meant to look.
6. **No live updates** (§6) — IBM i Navigator's own job-monitor/message-queue style views assume some kind of push or fast poll; today's backend only supports request/response polling.

None of this blocks starting `/slos-sim` — the existing HTTP API is a legitimate foundation for a first version covering the legacy catalog/process/program/agent surface, which is itself a reasonable IBM i Navigator-style starting point (object browser, job/process list, program management). But the RDBMS and Vector Store work — most of this project's recent effort — has no backend door for a web frontend to walk through yet, and that's the first real design fork to resolve before writing frontend code.
