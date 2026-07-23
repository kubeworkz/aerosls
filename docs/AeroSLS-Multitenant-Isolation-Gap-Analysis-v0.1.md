# AeroSLS Multi-Tenant Isolation Gap Analysis v0.1

## 0. Why this doc exists

This is a scoping document, not a findings-from-execution document like the LPAR or Multi-Node roadmaps' own "Findings addendum" sections — it exists to answer one question honestly before any architecture gets committed to paper: **if AeroSLS leased address space and services to real, paying customers today, exactly where would tenant A's workload touch tenant B's, and what would it cost to close each gap.** Every claim below is checked against the code and the existing roadmap docs as they stand today (`AeroSLS-LPAR-Roadmap-v0.1.md`, `AeroSLS-Multi-Node-Partition-Scaling-Roadmap-v0.1.md`, `AeroSLS-Database-Gap-Analysis-v0.1.md`, plus direct reads of `kernel/partition.c`, `kernel/database.c`, `kernel/frame_pool.c`, `kernel/auth.c`, `net/http.c`, `kernel/group_profile.c`/`authlist.c`/`security_audit.c`, `kernel/stream.h`, `kernel/tier_mgr.c`, `kernel/persist.c`), not recalled from memory.

**The short answer up front:** AeroSLS has real, well-tested, single-node multi-tenant isolation for four things — catalog/object access, process spawn, IPC, and CPU scheduling fairness — plus a partial, honestly-scoped start on cross-node placement. It has **no** concept of a tenant as a single provisionable entity, **no** storage-level isolation, **no** network-level isolation, **no** usage metering, and **no** tenant-scoped administration. Everything built so far answers "can tenant A's *code* reach tenant B's *objects*" — nothing yet answers "can I sell tenant A a guaranteed slice of storage, bandwidth, or admin control that tenant B cannot touch or see, and bill for it."

## 1. Inventory: what real isolation already exists

Two build efforts already exist and are both marked done. Neither was designed with billing/leasing in mind, but both are real, load-bearing primitives worth building on rather than replacing.

### 1.1 `partition_id` — resource and execution isolation, single node (LPAR Phases 8–14, all done)

A `partition_id` tag threaded through the choke points that already existed for something else, exactly the same pattern each time: `catalog_check_access()` gates object access, `process_create()`/`program_spawn()` gate process spawn, `ipc_user_send()`/`ipc_user_recv()` gate IPC ports, `schedule_ring3()`'s two-level search gates CPU turns, `frame_pool.c`'s accounted allocator gates physical RAM for 5 of 20 real allocation call sites. All host-tested with real execution, not just compile-check, and all real, caught bugs (Phase 12's scheduler starvation bug, the missing frame-free primitive in Phase 14) along the way.

**What this buys a leasing story:** a tenant assigned to partition *P* cannot read/write another partition's catalog objects, cannot spawn processes into another partition, cannot send/receive on another partition's bound IPC ports, and cannot starve another partition of every CPU turn. That's a real, defensible boundary for co-tenants sharing one machine.

**What it does not buy:** everything in §3 below.

### 1.2 `database_id` — SQL/catalog namespace and grants (Database Namespace & Access Roadmap, Phases 1–5, done; gap-closed further in the Database Gap Analysis)

A separate tag, on the same `object_catalog[]` entries, resolved through `database_check_access()` as an additive extension of `catalog_check_access()`. `CREATE DATABASE`, `GRANT`/`REVOKE` per database, `ALTER TABLE ... SET DATABASE`, cascading `DROP DATABASE ... CASCADE` — a real SQL-shaped multi-schema/multi-tenant namespace, independently gated.

**What this buys:** two SQL tenants sharing one AeroSLS instance can each have their own `CREATE DATABASE mycompany` namespace with independent grants, invisible to each other via `SHOW`/`SELECT` unless explicitly granted.

### 1.3 Cross-node scaling and cold migration (Multi-Node Partition Scaling Roadmap, Phases 1–6, done)

Real node identity/roster (`cluster_roster[]`), per-partition node ownership (`partition_owner_table[]`), per-partition Raft-lite write leases (`partition_lease_table[]`), partition-aware DSPP packet routing (`partition_id` added to the wire header, with a magic-value version bump), and `partition_migrate()` orchestration (pause → step down lease → reassign ownership → reclaim source frames).

**What this buys:** the bookkeeping for "which physical node currently owns tenant X's partition" is real, tested, and persisted. That's a genuine prerequisite for a dedicated-node tier.

**What this does not buy, named explicitly by the roadmap's own findings, twice:** *no actual page data moves.* `partition_migrate()`'s own findings addendum says it plainly — "there is no function anywhere in `net/dspp.c` that moves or retags a catalog object's `partition_id`, because no object-to-physical-frame resolution plumbing exists to move." Migration today moves an ownership *record*, not a tenant's data. A customer told "we moved you to a dedicated node" would, on the current codebase, have every object still physically sitting on the source machine's NVMe and RAM, just logically marked as belonging elsewhere and refusing to serve requests locally. This is the single largest gap standing between what's built and an actual dedicated-node product.

### 1.4 Authentication (Architectural Phase 4, Operational Phase E)

Bearer tokens (`struct LeaseToken`: email, uid, role, TTL) validated on every `/api/*` route except `/api/health`/`/auth/token`/`/auth/verify`, real credential check on issuance (password-derived key, not a bare-email handout), CORS locked down, TLS via a reverse proxy. This is real and already closes a whole class of "anyone can mint a token for any account" bugs the Architectural roadmap found and fixed.

## 2. The core architectural gap: tenancy is not one thing, it's two unconnected things

This is the most important finding in this document, and it did not require deep investigation to surface — a single grep proves it: `kernel/database.c` never references `partition_id`, anywhere, in either direction. `kernel/partition.c` never references `database_id`. They are two completely independent tagging systems living on the exact same `object_catalog[]` array, built by two different roadmaps that never introduced themselves to each other.

Concretely, today there is no such thing as "provision tenant Acme Corp." There are two separate, manual, uncoordinated procedures that happen to both use the word "isolation":

- `partition create acme` + `partition assign uid <n> acme` — gives Acme's processes/frames/IPC/CPU-turns a resource boundary, says nothing about SQL schema visibility.
- `CREATE DATABASE acme` + `GRANT ... IN DATABASE acme` — gives Acme's SQL objects a namespace boundary, says nothing about resource isolation. A uid granted access to database `acme` but never assigned to partition `acme` gets full SQL namespace isolation and zero resource isolation from every other tenant on the box, and vice versa.

Nothing stops these two axes from being assigned inconsistently for the same real-world customer (uid 500 could be in partition `acme` but granted into database `contoso`), because nothing today treats "tenant" as a single entity with both a partition and a database that must move together. Any future "tenant" concept needs to either unify these two axes under one identity or make the relationship between them an explicit, enforced invariant — right now it's neither; it's two coincidentally-parallel systems.

## 3. What's shared vs. dedicated today, resource by resource

| Resource | Isolation today | Real gap for leasing |
|---|---|---|
| Catalog objects (tables, vector collections, streams-as-objects) | Real, enforced, tested (`catalog_check_access()` + `database_check_access()`) | None at the logical layer |
| Process spawn / execution | Real, enforced, tested (Phase 9) | None |
| IPC ports | Real, enforced, tested (Phase 11) | None |
| CPU scheduling | Starvation-prevention only, not proportional/weighted (Phase 12's own explicit scope cut) | Cannot sell "guaranteed 2 vCPU-equivalent" — every partition gets an equal round-robin slot regardless of what a customer paid for |
| Physical RAM frames | Accounted for exactly 5 of 20 real allocation call sites (process stacks/code, ELF segment loads); the other 15 — page-table internals, NVMe queues, SMP boot stacks, the shared SIMI activation cache, catalog index nodes, **and stream/blob storage** — are unaccounted, either correctly attributed to system overhead or (streams) genuinely untracked | A tenant uploading large blobs via the stream/blob store consumes real, unbounded, unquota'd physical memory today — `struct StreamEntry` (`kernel/stream.h`) carries no owner or partition field at all, confirmed by direct read, a gap LPAR Phase 13's own findings already named and left open |
| Physical frame *reclamation* | Real per-frame tracking exists for the accounted 5 sites (Multi-Node Phase 3 closed the LPAR Phase 14 leak) | Still bounded to those same 5 sites; frames from the other 15 (notably stream storage) are never reclaimed per-tenant, ever |
| NVMe/disk storage | **No partitioning at all.** `kernel/persist.c` writes every subsystem's data into one flat, shared LBA space; `kernel/frame_pool.c`'s bitmap is one shared free-list. There is no per-tenant NVMe region, quota, or byte accounting | A "your data lives on dedicated storage" tier is not physically true today under any tenant — everyone's bytes interleave in the same disk image |
| Storage tiering (hot/cold) | `kernel/tier_mgr.c` has zero partition/tenant awareness, confirmed by direct grep | One tenant's access pattern can promote/demote another tenant's data between tiers; no per-tenant tier SLA is possible today |
| Network / HTTP | **[Partially closed — see §8]** Per-partition HTTP request-rate limiting now real (`net/http_rate_limit.c`), gating all three HTTP auth chokepoints. `TCP_MAX_CONNS = 24` (`net/tcp.h`) is still one shared, cluster-wide, un-partitioned connection pool — no per-tenant connection quota exists | Request-level noisy-neighbor risk (one tenant flooding the server with requests) is closed. Connection-level risk (one tenant holding many long-lived connections) remains open — deliberately scoped out of this phase; see §8 for why |
| Cross-node placement | Real ownership bookkeeping (§1.3) | No real data movement (§1.3) — a dedicated-node tier cannot actually be fulfilled yet |
| Authentication / identity | Real bearer tokens, uid + role | uid is a flat, global namespace shared by every tenant; there is no tenant-scoped admin role (see §4) |
| RBAC (roles, groups, authorization lists) | `kernel/group_profile.c`, `kernel/authlist.c` — confirmed by grep to have **zero** `partition_id`/`database_id` awareness | A group or authorization-list entry is either global system-wide RBAC or nothing — there is no way to grant a customer's own staff an "admin within your partition only" role. Any uid with elevated RBAC authority has it everywhere, not scoped to one tenant |
| Security audit log | `kernel/security_audit.c` — same zero-awareness finding | Cannot hand a customer a filtered audit trail of "actions within your tenant only"; the log is one global stream |
| Billing / usage metering | **Does not exist.** Repo-wide search for billing/metering/usage-report/cost-tracking infrastructure returns nothing relevant — the only "quota" concept anywhere is the frame-count quota from LPAR Phase 13, which is an admission-control limit, not a metered, exportable usage record | Nothing today could generate an invoice. No subsystem records "partition X consumed Y frame-hours / Z requests / W storage-bytes over this billing period" |

## 4. Capacity ceilings that block real scale, independent of the isolation-completeness question

Even if every gap above were closed, three compiled-in constants cap how many real customers this architecture could hold *today*, all trivially raisable in isolation but each is also a signal that these tables were sized for a development sandbox, not a multi-tenant deployment, and a real leasing rollout needs to decide real numbers, not inherit these by accident:

- `PARTITION_MAX = 16` (`kernel/partition.h`) — 16 partitions, system-wide, full stop. The Multi-Node roadmap's own §1 already calls this "an arbitrary array size chosen to match `PROC_MAX`, trivially raised" — true, but 16 concurrent tenants (or fewer, once system/reserved partitions are subtracted) is not a real ceiling to launch a leasing product against.
- `PARTITION_ASSIGN_MAX = 64` — 64 total `uid → partition` assignments, **across every tenant combined**, not per tenant. A single customer with a 20-person team already consumes a third of the entire system's user-assignment capacity.
- `TCP_MAX_CONNS = 24` — already flagged in §3 as a shared-resource gap; it's also a hard scale ceiling independent of isolation, since even a single-tenant deployment would struggle at 24 concurrent connections.

None of these require new architecture to fix — they're `#define`s — but they need to be sized deliberately as part of whatever tiering model gets chosen, not left at their current development-sandbox defaults.

## 5. What "leasing address space and services" actually requires that doesn't exist yet

Framed directly against the user's stated goal — leasing AeroSLS to real customers, shared vs. dedicated tiers, and scaling — here is the concrete, currently-missing list, ordered by how foundational each piece is to everything above it:

1. **A single "tenant" identity that unifies `partition_id` and `database_id`.** Today provisioning a customer is two unrelated manual procedures (§2). A real product needs one `tenant_create()`-shaped operation that atomically creates both, and an invariant somewhere (probably a new small table, mirroring `partition_owner_table[]`'s own "separate table, not a bolted-on field" precedent) that keeps them consistent going forward.
2. **Storage isolation.** Nothing physically separates tenant bytes on disk today (§3). This is the largest, highest-risk gap for any tier that claims data isolation, let alone dedicated storage.
3. **Real frame reclamation and quota coverage for stream/blob storage**, closing the one named-but-unclosed gap from LPAR Phase 13.
4. **[Partially closed — see §8]** Connection-level and request-level fairness at the network layer — per-tenant connection quotas and/or rate limiting, and `TCP_MAX_CONNS` sized for real concurrent tenancy, not today's shared 24. Request-level rate limiting is now real; connection-level quotas and the `TCP_MAX_CONNS` resize remain open.
5. **Tenant-scoped RBAC.** A customer needs to be able to administer their own users without that authority leaking to (or being grantable over) another tenant's partition/database. `group_profile.c`/`authlist.c` need a `partition_id`/`database_id` scope dimension, the same shape Phase 3 of the security work already gave `catalog_check_access()` for database grants.
6. **Usage metering.** A new subsystem, not an extension of an existing one — nothing today counts and exports per-partition consumption over time. This is a hard prerequisite for any usage-based billing, and a soft prerequisite even for flat-rate tiers (you still need to prove SLA compliance and detect abuse).
7. **Real data movement for migration**, closing the gap named twice in the Multi-Node roadmap, before a dedicated-node tier can be sold honestly — "records move" is not "your data moved."
8. **Weighted/proportional CPU scheduling**, if any tier wants to sell a CPU guarantee stronger than "won't be starved" (Phase 12's own explicit, honest scope cut).
9. **Deliberate capacity sizing** for `PARTITION_MAX`/`PARTITION_ASSIGN_MAX`/`TCP_MAX_CONNS` (and an audit for any other similarly-sized fixed table not surfaced by this pass) against a real target tenant count, not the current development defaults.

## 6. A first-cut tiering model this inventory naturally supports

Not a commitment, offered as a starting frame for the architecture conversation this doc exists to enable:

- **Shared tier** — multiple tenants per partition-set on one node, isolated by `partition_id` + `database_id` only (§1.1–1.2, already real). Cheapest to offer today, but honestly must be sold with the caveats in §3 disclosed (no storage isolation, no network fairness, no CPU guarantee) until those close.
- **Dedicated-partition tier** — one tenant per partition, still co-located on shared physical hardware with other tenants' partitions. Meaningfully stronger than shared once §5 items 2–5 close (storage isolation and network/RBAC fairness are what actually differentiate this from the shared tier today, since resource-level isolation is already real).
- **Dedicated-node tier** — one tenant, one physical machine, using Multi-Node's node-pinning and migration machinery. Cannot be sold honestly until §5 item 7 (real data movement) exists — right now "dedicated node" would be a true statement about ownership bookkeeping and a false statement about where the customer's bytes actually live.

## 7. Suggested priority if any of this gets picked up

1. **[Partially closed — see §8] Network fairness (§5.4)** — cheapest fix in this document (`TCP_MAX_CONNS` alone is a `#define`, though real per-tenant rate limiting is more work), highest immediate noisy-neighbor risk reduction, blocks nothing else.
2. **Unify the tenant identity (§5.1)** — architecturally foundational; every other gap is easier to reason about and test once "tenant" is one thing instead of two coincidentally-parallel systems.
3. **Stream/blob ownership + quota (§5.3)** — closes a already-named, already-understood gap (LPAR Phase 13's own findings scoped exactly what's missing); small, bounded, no new concepts needed.
4. **Tenant-scoped RBAC (§5.5)** — needed before any tier can be self-service administered by the customer's own staff, which is close to a hard requirement for "real customers," not a nice-to-have.
5. **Storage isolation (§5.2)** — large, foundational for any data-isolation claim; likely deserves its own dedicated roadmap doc the way LPAR and Multi-Node each got one, given its size.
6. **Usage metering (§5.6)** — can be built in parallel with the above once a stable tenant identity (item 2) exists to key metering records on; genuinely new subsystem, not an extension.
7. **Real migration data movement (§5.7)** — gates the dedicated-node tier specifically; can be deferred until that tier is actually being sold, the same "don't build ahead of a concrete need" discipline the LPAR roadmap itself used to correctly close out nested partitions (§9 of that doc).
8. **Weighted CPU scheduling (§5.8)** and **capacity resizing (§5.9)** — lower urgency individually, but item 9 in particular should be revisited the moment any concrete target tenant count exists, since it's pure sizing work with no design risk.

## 8. Findings addendum: Network fairness, Phase 1 (§5 item 4 / §7 item 1) — partially closed

Implemented: per-partition HTTP request-rate fairness. New files `net/http_rate_limit.h`/`.c`, wired into all three existing auth chokepoints in `net/http.c` (GET/POST/DELETE route gates), each now returning HTTP 429 once a partition exhausts its window budget.

**Mechanism.** A fixed-window counter keyed on `partition_id` (resolved via `partition_get_for_uid(uid)`, matching every other multitenancy chokepoint in this codebase), not `uid` — so multiple uids in the same partition correctly share one budget rather than getting independent allowances. Window: 1000 ticks (~10s at the documented ~100Hz tick rate). Limit: 100 requests/window/partition, sized as roughly 8x headroom over the frontend's own documented background-polling baseline — a deliberately generous, tunable first cut, not a measured production value.

**Why request-rate, not connection-count.** Connection-level quotas were the other candidate mechanism this doc originally named. Two real properties of this server rule that out for now: `tcp_conns[]`/`TCP_MAX_CONNS` (`net/tcp.h`) is one small pool shared by both inbound tenant HTTP traffic and the kernel's own outbound connections (e.g. `net/ollama_client.c`), and `net/tcp.h`'s own comment already documents a real incident where inbound polling starved an outbound call out of that shared pool — slicing it per-tenant needs inbound/outbound separation first, a larger change not attempted here. Separately, `http_server_run()`'s own architecture serializes request dispatch (one request runs to completion before the next connection is serviced), so there's no "concurrent requests in flight per tenant" concept for a connection-level cap to even bound. Request-rate over time is the real, meaningful resource being protected.

**What's still open.** `TCP_MAX_CONNS = 24` remains one shared, un-partitioned, cluster-wide pool — connection-level noisy-neighbor risk (a tenant holding many long-lived connections) is not addressed by this phase, and neither is deliberate resizing of that constant against a real target tenant count (§5 item 9 / §7 item 8, unchanged).

**Verification.** `tests/http_rate_limit_host_test.c` — 6 scenarios (independent per-partition budgets, shared budget across uids in one partition, window reset timing, unassigned-uid fallback, out-of-range defensive guard), all real execution against the unmodified `net/http_rate_limit.c`, not a reimplementation. Full regression: `tests/run_all.sh`, 50/50 host tests passing, including this new one. `net/http.c` and `net/http_rate_limit.c` both compile clean under the real kernel cross-compile flags, no new warnings.

**Status:** §5 item 4 and §7 item 1 partially closed — request-level fairness done, connection-level fairness and capacity resizing remain, tracked under §5 item 9 / §7 item 8.
