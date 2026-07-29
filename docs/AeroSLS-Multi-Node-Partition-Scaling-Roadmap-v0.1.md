# AeroSLS Multi-Node Partition Scaling Roadmap v0.1 — wiring `partition_id` into the distributed layer

## 0. Where this picks up

Two subsystems exist in this codebase today, built independently and never introduced to each other. `AeroSLS-LPAR-Roadmap-v0.1.md` (Phases 8-14, all done) built real multi-tenant isolation *within one running kernel*: a `partition_id` tag checked at catalog access, process spawn, IPC ports, scheduling, and frame quotas — all inside one address space, one boot, one machine. Separately, `docs/SLS-OS-Scaling.md` designed and — unusually for a paper design in this project — actually got built: `net/consensus.c`, `net/dspp.h`, `net/e1000.c`, `net/prefetch.c` are all real, compiled, wired into `Makefile`'s `SRCS`. It gives AeroSLS a working page-mirroring protocol (DSPP) and a Raft-lite leader-election layer (`ClusterNode`/`ConsensusMessage`) across physical nodes.

Neither subsystem knows the other exists. DSPP packets carry a `system_object_id` and a `virtual_address` — never a `partition_id`. Consensus operates on one global `static struct ClusterNode local_cluster_state` (`net/consensus.h:33`) with no `node_id` field, no roster of peers, and a `node_source_id` that's a hardcoded literal (`1` or `2`) at every call site, not a real identity. The distributed layer was written assuming one shared object namespace and one shared tenant across the whole cluster — it has no concept of "this partition belongs on that node" to hook into.

This is a planning document, not a findings document — nothing below has been built yet. It follows the same phase shape the LPAR and SIMI-ISA docs use (deliverable, dependencies, scope, verification plan), so that when a phase actually gets built, its findings can be added as a "findings addendum" under that phase's own heading, the same convention every other roadmap in this project follows.

## 1. Why horizontal, not vertical

Real IBM-style LPAR density limits (the mainframe's ~85-partition ceiling) come from firmware/hardware constraints that don't exist here — `PARTITION_MAX = 16` (`kernel/partition.h:36`) is an arbitrary array size chosen to match `PROC_MAX`, trivially raised. But raising it doesn't get you more real capacity: AeroSLS's partition quotas are static and fail-closed with zero overcommit (`kernel/frame_pool.c`'s `allocate_physical_ram_frame_for_partition()` denies outright at the quota line, no eviction) — there's no VM-style hypervisor trick available to pack more tenants onto one machine's physical RAM than that RAM actually holds. Vertical density is a dead end for this architecture without inventing overcommit, which would abandon the "fixed, dedicated, predictable" property that makes the current design LPAR-*flavored* in the first place.

What's actually unbuilt is the other axis: nothing today lets a partition exist on, or move to, a different physical machine. That gap is what this roadmap closes, by extending the tenant-isolation primitive Phases 8-14 already proved out to span the distributed transport Phase 3 (of `SLS-OS-Scaling.md`) already built — rather than inventing a third mechanism.

## 2. Design principles carried forward

1. **No free lunch this time.** Phase 8's partition isolation came for free across every `catalog_check_access()` caller because one choke point already existed. No equivalent choke point exists in the distributed layer — DSPP's packet format and consensus's global state both have to be extended explicitly, field by field, the same way Phase 11 (cross-partition IPC) had to build new gating from scratch because `ipc.c` had no existing hook either.
2. **Fix the bug that compounds under migration before building migration.** LPAR Phase 14's own findings named a real, permanent gap: `partition_destroy()` has no way to free the physical frames a partition held — only a usage *counter* reset, not real reclamation (`kernel/frame_pool.c`'s `partition_reset_frame_usage()`). On one machine this is a slow leak. Under routine migration — where a partition's local footprint on the source node is effectively destroyed and recreated on the destination every time it moves — the same bug becomes an unbounded leak on a much shorter clock. This roadmap treats that fix as a prerequisite gate, not an optional nice-to-have, and sequences it before any phase that makes migration real.
3. **Reuse the transport that's already built and compiled.** `net/consensus.c`/`net/dspp.h`/`net/e1000.c`/`net/prefetch.c` exist, compile, and are wired into the kernel image today. Every phase below extends this stack's data model and call sites; none of them propose a second, competing distributed-systems mechanism.
4. **Verification ceiling honesty, checked before assumed.** The LPAR roadmap's recurring surprise was that supposedly compile-check-only phases (10-14) turned out host-testable once their real dependency graphs were actually traced, catching real bugs along the way. `net/consensus.c` is 105 lines with a shallow, mostly self-contained dependency graph — worth checking for the same host-testable ceiling before assuming compile-check-only, the same discipline that paid off four times in a row in the LPAR roadmap.

## 3. Roadmap

| Phase | Deliverable | Depends on | Risk / lift |
|---|---|---|---|
| 1 | Real node identity & membership registry — replace the hardcoded `node_source_id` literal and the single global `ClusterNode` with a real roster | none | Medium — touches consensus.c's core global-state assumption |
| 2 | Partition ownership & node pinning — which node owns a given partition, and a local/remote decision point | 1 | Low-medium — mirrors the existing `partition_assign_table` pattern |
| 3 | Fix physical frame reclamation before migration compounds it — real per-frame ownership tracking, closing the LPAR Phase 14 leak | none (independent bug fix, gates Phase 6) | High — LPAR roadmap's own Phase 14 findings called this "arguably its own phase" |
| 4 | Partition-scoped consensus leases — extend Raft-lite from one global term/role per node to per-partition write-ownership | 1, 2 | High — the single biggest structural mismatch the investigation found |
| 5 | Partition-aware DSPP routing — tag page packets with `partition_id`, not just `system_object_id` | 2 | Medium-high — a real wire-protocol format change, needs version compatibility thought |
| 6 | Cold partition migration — `partition migrate <id> <dest_node>`, partition unavailable during the move | 2, 3, 4, 5 | High — first real end-to-end use of the whole stack |
| 7 | ~~Stretch: live/hot migration~~ — **deferred, not scoped**, see §10 | 6 | N/A for v1 |

Weighted/proportional-fair CPU scheduling across partitions (LPAR Phase 12 built round-robin, explicitly not proportional fairness) is **not** a phase in this roadmap. It's a real single-node concern, but it's orthogonal to horizontal scaling — a partition's CPU share on whichever node currently hosts it is unaffected by whether that partition can also move between nodes. Left out deliberately, not missed.

## 4. Phase 1 — Real node identity & membership registry — DONE

**Why this is first.** Every later phase needs to answer "which nodes exist, and which one am I" — a question the code cannot currently answer. `local_cluster_state` (`net/consensus.h:33`) is one global struct with no `node_id` field at all; `active_nodes_count = 3` and `stable_quorum_threshold = 2` are compiled-in constants, not derived from any discovery or config mechanism; `node_source_id` on `struct DSPPPacketHeader` is populated with a hardcoded `1` or `2` literal at each call site in `consensus.c`/`prefetch.c`, never read from a real identity. There is no roster — nothing enumerates "the other nodes in this cluster."

**Scope.**
- A real per-node identity: a `node_id` this instance is configured or discovers itself to be, replacing every hardcoded `node_source_id` literal call site.
- A membership table — an array of known peers (address, `node_id`, last-seen-alive), not the single unnamed "3 active nodes" constant `local_cluster_state` currently assumes. Static/config-file membership for a first cut is enough; dynamic discovery is out of scope here the same way Phase 8's LPAR groundwork deliberately didn't attempt dynamic partition discovery.
- `local_cluster_state` gains a `node_id` field; `active_nodes_count`/`stable_quorum_threshold` become derived from the real roster's size rather than compiled-in literals.

**Explicitly not in scope for Phase 1:** dynamic node join/leave, failure detection beyond what the existing heartbeat mechanism already does, and any notion of node capacity/weight — this phase answers "who exists," not "how much of what they have."

**Verification plan.** Check first whether `net/consensus.c`'s dependency graph is as shallow as its 105-line size suggests (LPAR Phases 10-14 all found real execution reachable where compile-check was assumed) — if so, a host test can drive the roster/identity logic directly, the strongest verification tier this project recognizes. Compile-check as the floor regardless.

**Findings addendum (as built).** Landed close to the scope bullets above, plus two real bugs found and fixed along the way that the scope bullets didn't anticipate.

`net/consensus.h`: `struct ClusterNode` gained a `node_id` field (0 reserved as the "uninitialized" sentinel, mirroring `PARTITION_SYSTEM`'s own use of 0 as a meaningful reserved id elsewhere in this project). `local_cluster_state` changed from `extern`-less to `extern` — see the first real bug below. A new `#define CLUSTER_NODE_MAX 8`, `struct ClusterPeer { uint32_t node_id; uint8_t active; }`, and `extern struct ClusterPeer cluster_roster[CLUSTER_NODE_MAX]; extern uint32_t cluster_roster_count;` back the membership table. Three new functions: `cluster_init(uint32_t local_node_id)` (returns 0/1, resets to a fresh FOLLOWER with an empty roster), `cluster_register_peer(uint32_t node_id)` (returns 0=newly added / 1=already active / 2=re-activated / -1=invalid id / -2=roster full), and two accessors, `cluster_local_node_id()`/`cluster_active_node_count()`.

**First real bug found (not exercised before this phase, but real): `local_cluster_state` was a `static struct ClusterNode` defined directly inside the header.** `static` at file scope in a header gives every `.c` file that includes it its own private copy of the struct. Harmless so far only because `prefetch.c` never touched `local_cluster_state` before this phase and `consensus.h` had exactly one includer that read/wrote it — but the moment a second file needed to read the same cluster state (which this very phase's own `prefetch.c` change was about to become the first case of, once `cluster_local_node_id()` needed calling from there too), it would have silently desynced into two independent copies instead of erroring. Fixed by converting to `extern` declaration + single definition in `consensus.c`, the same single-source-of-truth convention `partition_table[]` already established in `partition.c`.

**Second real bug found, this one caught only by writing the host test, not by code review:** giving `process_consensus_packet`/`trigger_kernel_election_campaign`/`check_consensus_heartbeat_tick` real prototypes in the header (needed so the new host test could call them without implicit-declaration warnings — none of the three had ever been declared anywhere before this phase, only defined) initially declared `process_consensus_packet(struct DSPPFullPagePacket* packet)` using a bare parameter-list-scoped struct tag, since `consensus.h` doesn't (and, given `dspp.h` has no include guard of its own, safely can't) `#include "dspp.h"` directly. That created a *second*, incompatible `struct DSPPFullPagePacket` tag scoped only to that one parameter list, which then conflicted with the real one `dspp.h` defines once both headers are included together in `consensus.c` — a "conflicting types" compile error, not a silent bug, but one that would have been easy to paper over with an unsafe cast instead of the correct fix: a plain top-level forward declaration (`struct DSPPFullPagePacket;`) before the prototype, which composes correctly with the later full definition from `dspp.h` regardless of include order.

`net/consensus.c`: every hardcoded `node_source_id = 1` literal (four call sites: the LEADER heartbeat, the REQUEST_VOTE campaign packet, the VOTE_REPLY response, and — the `candidate_id` field, a second hardcoded `1` — the vote message itself) now reads `(uint16_t)local_cluster_state.node_id`. `voted_for = 1` ("vote for self") now reads `local_cluster_state.node_id`. The one log line that printed "Node 1 elected LEADER" now prints the real node id via `%u`. `cluster_recompute_quorum()` (static, internal) recomputes `active_nodes_count`/`stable_quorum_threshold` from the roster's real size after every membership change — majority quorum `(active_nodes_count / 2) + 1` — rather than keeping an incremental counter that could drift out of sync with the roster array itself.

`net/prefetch.c`: gained `#include "consensus.h"` and its one `node_source_id = 1` literal (the speculative-prefetch DSPP read request) now reads `(uint16_t)cluster_local_node_id()`.

**Verification exceeded the plan.** `net/consensus.c`'s dependency graph turned out exactly as shallow as its size suggested: three extern dependencies (`kernel_serial_print`/`kernel_serial_printf`/`update_page_table_permissions_globally`), all trivially stubbable, plus a call-tracking stub for `e1000_transmit_packet` that captures the actual bytes of the last packet transmitted. `tests/consensus_phase1_host_test.c` links the real, unmodified `net/consensus.c` and gives real execution — the strongest tier this project recognizes — of both the roster/quorum bookkeeping (init, register, duplicate rejection, invalid-id rejection, roster-full rejection, re-init-is-a-fresh-reset) and, critically, the actual packet-construction code paths: it drives `trigger_kernel_election_campaign()`, `check_consensus_heartbeat_tick()`, and `process_consensus_packet()` for real and inspects the transmitted bytes to confirm `node_source_id`/`candidate_id` genuinely carry the configured node's real id (55, in the test) rather than the old hardcoded `1` — the only way to actually prove that class of bug is fixed, since a test that only checked `cluster_local_node_id()`'s return value would never catch a regression where a future edit reintroduces a hardcoded literal at one of the four packet-construction sites while leaving the accessor itself correct. 39 checks, all passing.

`net/consensus.c` and `net/prefetch.c` additionally compile cleanly under `gcc -Wall -Wextra -std=c11 -fsyntax-only` with zero new warnings — one pre-existing warning (`trigger_kernel_election_campaign` called before its own definition further down the same file, with no forward declaration) was confirmed to predate this phase's edits (same function ordering existed in the original file) and was left alone, consistent with this project's convention of not fixing unrelated pre-existing warnings while touching a file for an unrelated reason. Full regression sweep: `bash tests/run_all.sh` — 41/41 host test files pass (40 pre-existing, unmodified, plus this phase's new one), confirming Phase 1's changes are genuinely additive and didn't disturb anything downstream (a repo-wide grep confirmed `net/consensus.c` and `net/prefetch.c` are the only two files that include `net/consensus.h`, so no third file could have been affected either way).

**What this phase deliberately did not do, named rather than silently skipped** *(the heartbeat half of this is now closed — see §9b, which also covers why leaving it open for four phases was worse than it looked)***:** neither `check_consensus_heartbeat_tick()` (the 10ms timer hook) nor `prefetch_worker_kernel_thread()` is called from anywhere in this codebase — confirmed by a repo-wide grep before this phase started, and unchanged by it. This whole distributed layer is compiled into the kernel image but dormant at boot; nothing wires the heartbeat timer into the interrupt/timer infrastructure, and nothing decides where a real node's `node_id` and initial peer list come from at boot (a command-line arg, an NVMe-persisted config block, a build-time constant — all still open). `cluster_init()`/`cluster_register_peer()` are real, tested, and ready to be called by whatever future phase makes that boot-time wiring decision, but this phase deliberately didn't invent a config source just to have something call them — that's a separate, larger design question than "replace the hardcoded literal with a real, settable one," the same distinction LPAR Phase 9 drew when it scoped process-level partitioning tightly rather than also building every downstream consumer of `partition_id` in the same pass.

## 5. Phase 2 — Partition ownership & node pinning — DONE

**Why this is next, and why it's mechanical.** `kernel/partition.h` already has a precedent for exactly this shape: `partition_assign_table[]` is a separate table mapping `uid -> partition_id`, kept apart from `partition_table[]` itself. Node ownership is the same pattern one level up: a `partition_id -> node_id` mapping, kept as its own table rather than a field bolted onto `struct SLSPartitionEntry`, so a future migration (Phase 6) is a row update in one small table, not a mutation of partition identity itself.

**Scope.**
- A new `partition_node_owner[PARTITION_MAX]` table (or equivalent), populated at `partition_create()` time with the local node's `node_id` by default — so every existing single-node deployment is a no-op with "this partition lives here," matching every prior phase's zero-partitions-defined/zero-nodes-defined backward-compatibility discipline.
- A local/remote decision point: before any partition-scoped kernel operation proceeds, check whether the owning `node_id` matches this node's own identity (Phase 1). What happens on a remote-owned partition (forward the request, deny it, queue it) is explicitly **not** decided in this phase — that's Phase 4/6's problem. This phase only establishes that the check exists and where it sits.

**Verification plan.** The table and the ownership check are small, self-contained struct-array logic in the same shape `partition.c` itself already proved host-testable in LPAR Phase 8 — expect real execution, not just compile-check.

**Findings addendum (as built).** Landed close to the scope bullets, plus a persistence extension the scope bullets didn't originally call out (see below), and one design decision worth naming: the ownership table was kept deliberately separate from `struct SLSPartitionEntry`, matching `partition_assign_table[]`'s own precedent exactly, rather than adding a `node_id` field to the entry struct itself.

`kernel/partition.h`: new `struct SLSPartitionOwner { uint32_t partition_id; uint32_t node_id; uint8_t active; }`, `extern struct SLSPartitionOwner partition_owner_table[PARTITION_MAX]`, and three new functions — `partition_get_owner_node(uint32_t partition_id)` (linear scan, returns 0 if no active row, the "honest absence" convention this project uses throughout rather than a sentinel that could be confused with a real node id), `partition_set_owner_node(uint32_t partition_id, uint32_t node_id)` (update-existing-row-or-create-new-row, returns 1 if `partition_id` isn't a valid/active partition), and `partition_is_local(uint32_t partition_id)` (one-line comparison: `partition_get_owner_node(partition_id) == cluster_local_node_id()`).

`kernel/partition.c`: `#include "../net/consensus.h"` added — an architecturally consistent cross-include, not a new pattern (`kernel/kernel.c` already includes multiple `net/*.h` headers). `partition_init()` stamps `PARTITION_SYSTEM`'s owner row at `cluster_local_node_id()`. `partition_create()` stamps every new partition's owner row the same way at creation time, and its log line now reports the owner node. `partition_destroy()` gained a fourth cleanup step: deactivating the matching `partition_owner_table[]` row, so a slot reused by a later `partition_create()` can't inherit a stale owner from whatever partition previously held that id — verified directly by this phase's own test (create → pin to a foreign node → destroy → confirm the lookup reads back to "no row" → create again on the same freed slot → confirm the new row gets a fresh stamp, not the old pin).

**The honest-degrade property, confirmed by design and by test.** Since `cluster_init()` is still never called at boot (Phase 1's own finding, unchanged by this phase), `cluster_local_node_id()` returns the Phase 1 uninitialized sentinel (0) on every deployment that exists today. Every partition's owner row also defaults to 0 at creation time. `partition_is_local()` therefore evaluates `0 == 0` → true for every partition on every current deployment — every partition reads as locally owned, which is correct and honest (there's only one node, so everything genuinely is local), not a fabricated multi-node claim. The test file makes this the very first assertion for both `PARTITION_SYSTEM` and a freshly-created partition before doing anything else, matching Phase 1's own discipline of never letting a "looks reasonable" default go unverified.

**Persistence extension, found necessary mid-implementation, not originally scoped.** The scope bullets above didn't call out persistence explicitly, but leaving `partition_owner_table[]` un-persisted would have meant every ownership pin silently reverted on reboot — inconsistent with `partition_table[]`/`partition_assign_table[]`'s own Phase 10 persistence one struct over. `persist_partitions()` (`kernel/persist.c`) now also writes `partition_owner_table[]`, reusing `write_hdr()`'s previously-always-`0` third size slot (`v2`) for `owner_bytes` rather than changing `write_hdr()`'s signature. A new `PERSIST_PART_OWNER_LBA = 5816ULL` fits exactly into a pre-existing one-frame gap between `PERSIST_PART_ASSIGN_LBA` (ends at 5816) and `PERSIST_ROWSTORE_HDR_LBA` (starts at 5824) — zero collision, no LBA layout renumbering needed elsewhere. The restore path checks `owner_bytes == sizeof(partition_owner_table)` before restoring, so a snapshot written by pre-Phase-2 code (which always wrote `0` into that slot) is correctly recognized as "no valid ownership data here" and leaves the freshly-`partition_init()`'d owner table alone rather than reading stale or garbage bytes from `PERSIST_PART_OWNER_LBA` — this backward-compatibility path is exercised directly by the test, not just reasoned about.

**Verification exceeded the plan.** Two host tests, both linking the real, unmodified `kernel/partition.c` (and `persist_partition_host_test.c` also the real `kernel/persist.c`), extended rather than duplicated:

`tests/partition_host_test.c` (Phase 8's original test, extended): rather than link the real `net/consensus.c` just to drive `cluster_local_node_id()` — a heavier dependency for a partition-focused test — this test stubs it as a *settable* fake (`g_fake_local_node_id`, a static the test can reassign between checks), so `partition_is_local()`'s real comparison logic still executes against multiple real node-id values, not just a single fixed stub return. 15 new checks cover: default-to-current-node ownership stamping at create time, `partition_is_local()` flipping correctly as `cluster_local_node_id()` and the pinned owner move independently of each other (proving a real re-read each call, not a cached/stale comparison), the update-existing-row vs. create-new-row branches of `partition_set_owner_node()`, rejection of pinning an undefined partition id, and the destroy/reuse-doesn't-inherit-stale-owner sequence described above.

`tests/persist_partition_host_test.c` (Phase 10's original round-trip test, extended): 9 new checks prove the full persistence round trip against the same fake-NVMe harness Phase 10 already built — pin an owner, simulate a reboot (wipe all three in-memory tables, confirm the pin is genuinely gone pre-restore), restore, confirm the pin survived. A second block directly exercises the backward-compatibility path: stomp just the `owner_bytes` header field (offset +16, matching `persist.c`'s own read offset) back to 0 on an otherwise-valid snapshot to simulate a pre-Phase-2 snapshot, and confirm `partition_table[]`/`partition_assign_table[]` still restore normally while the owner table is correctly left at `partition_init()`'s fresh stamp rather than reading the stale/mismatched data.

Both files compile cleanly under `gcc -Wall -Wextra -std=c11 -fsyntax-only` with zero new warnings. Full regression sweep: `bash tests/run_all.sh` — 41/41 host test files pass (`partition_host_test` now 41 checks, `persist_partition_host_test` now 23 checks, both up from their Phase 8/10 baselines, no new test files added since this phase extended existing ones rather than adding new ones).

**The regression cascade this phase's kernel changes triggered, and how it was resolved.** Adding `partition_owner_table` as a new global that `persist.c` unconditionally references broke every host test that stubs `partition_table[]`/`partition_assign_table[]` (to satisfy `persist.c`'s linker requirements) without linking the real `partition.c` — 20 files, fixed by adding a third stub line (`struct SLSPartitionOwner partition_owner_table[PARTITION_MAX];`) alongside their existing two. Separately, adding the real `cluster_local_node_id()` call inside `partition.c` itself broke every host test that links the real, unmodified `partition.c` without also linking `net/consensus.c` — 5 files (`partition_host_test.c`, `persist_partition_host_test.c`, `scheduler_fairness_host_test.c`, `ipc_partition_host_test.c`, `workmgmt_phase4_host_test.c`), fixed by adding a one-line `uint32_t cluster_local_node_id(void) { return 0; }` stub to each (two of these five, `partition_host_test.c` and `persist_partition_host_test.c`, were later upgraded to the settable-fake version described above once this phase's own new tests needed to drive real node-id values through). Both cascades are the same "new global/dependency breaks stub-based lighter-scaffold tests" pattern this project has hit and resolved the same way in every prior phase that added a cross-cutting dependency — not a new failure mode, just this phase's instance of it.

**What this phase deliberately did not do, named rather than silently skipped.** No local/remote *enforcement* exists yet — `partition_is_local()` is a query, not a gate; nothing in this phase makes any partition-scoped kernel operation actually consult it before proceeding. That's explicitly out of scope per this phase's own scope bullets (Phase 4/6's problem). Also unchanged: `cluster_init()` is still never called anywhere at boot, so every ownership row's "current node" default is still the Phase 1 sentinel (0) on every real deployment today — the honest-degrade property described above, not a gap this phase could or should have closed on its own.

## 6. Phase 3 — Fix physical frame reclamation before migration compounds it — DONE

**Why this has to land before Phase 6, regardless of its own dependency graph.** This is the one phase in this roadmap that isn't new distributed-systems work — it's closing a gap the LPAR roadmap named and explicitly deferred: `partition_destroy()` has no way to free the physical frames a partition held, only a usage counter reset (`kernel/frame_pool.c`, `partition_reset_frame_usage()`; see LPAR roadmap §8's findings addendum). Today that's a slow, single-machine leak. The moment Phase 6 makes migration real, a partition's footprint on its source node gets torn down on every single move — the same leak, but now running on a per-migration clock instead of a per-partition-lifetime clock. Sequencing this after Phase 6 would mean building migration on top of a bug that migration itself turns from cosmetic into operationally serious.

**Scope.**
- Real per-frame ownership tracking: a reverse map (physical frame address -> owning `partition_id`) or a per-partition free-list, replacing the aggregate-only counter `frame_pool.c` currently keeps. This was named in the LPAR roadmap's own Phase 14 findings as "a materially bigger change than 'wire up destroy' — arguably its own phase." This is that phase.
- `free_physical_ram_frame()` (built in the Gap-Remediation roadmap's Phase F for the generic case) gets a partition-aware sibling that actually walks the partition's owned frames and frees each one, replacing `partition_reset_frame_usage()`'s counter-only reset in `partition_destroy()`.
- The five call sites already threaded onto the accounted allocation path in LPAR Phase 13 (`process.c`'s three, `loader.c`'s two) are the natural first candidates for real per-frame tracking, since they're already partition-aware at allocation time — this phase is about closing the loop on the same five, not re-auditing all 20 original call sites from scratch.

**Verification plan.** `frame_pool.c` proved host-testable with real execution in LPAR Phase 13 (33 checks, no stubs beyond two logging functions) — the same file, extended, should hold the same verification ceiling. A test that allocates, frees via the new mechanism, and confirms the exact physical addresses freed are the exact ones later reallocated (not just "the counter went to zero," which Phase 14's own test already showed can pass without a real free happening) is the concrete bar this phase's own verification must clear.

**Findings addendum (as built).** Landed as scoped, one design decision worth naming: rather than a per-partition free-list (a linked structure, more bookkeeping) the roadmap's own "or" alternative — a flat reverse map, `frame_owner[TOTAL_FRAMES]`, one byte per physical frame — was chosen, mirroring `physical_memory_bitmap[]`'s own flat-array shape one field over rather than introducing a new data structure kind into this file.

`kernel/frame_pool.c`: new `static uint8_t frame_owner[TOTAL_FRAMES]` (1 MiB, `TOTAL_FRAMES = 1048576`). Populated at both accounted call sites — `allocate_physical_ram_frame()` tags `PARTITION_SYSTEM`, `allocate_physical_ram_frame_for_partition()` tags the real caller-supplied `partition_id` — and cleared back to the default on every free path (`free_raw_frame()`, the shared helper both `free_physical_ram_frame()` and `free_physical_ram_frame_for_partition()` already funneled through). New `uint32_t partition_reclaim_all_frames(uint32_t partition_id)`: walks all `TOTAL_FRAMES` entries, and for every one tagged with the given `partition_id` *and* independently confirmed still-set in `physical_memory_bitmap` (never trusting the owner tag alone — see the design note below), clears the bitmap bit and the owner tag and counts it. Zeroes the usage counter once real reclamation is done, so that counter is now truthfully zero rather than an accounting fiction. Returns the real count freed, or `0xFFFFFFFFu` for an out-of-range `partition_id`, mirroring `partition_create()`'s own sentinel convention.

**The BSS-zero ambiguity, named and resolved rather than worked around.** `frame_owner[]`'s default value (0, from BSS zero-init) is indistinguishable by value alone from "genuinely owned by `PARTITION_SYSTEM`" (also 0 — `PARTITION_SYSTEM == PARTITION_DEFAULT == 0`, per `partition.h`). Rather than reserve a sentinel byte value and have to explicitly initialize a megabyte array at some boot step this module has never had (there is no `frame_pool_init()` — every piece of this file's state has always relied on BSS zero as its only init, and this phase kept that discipline rather than breaking it), `partition_reclaim_all_frames()` never trusts `frame_owner[]` alone: every reclaim decision also independently checks `physical_memory_bitmap`'s real allocated bit first. A frame that was never allocated reads `frame_owner == 0` (ambiguous) but `bitmap bit == 0` (unambiguous "not allocated"), so it's correctly skipped regardless of what its stale/default owner byte says. The same "0 is honest, verify independently before trusting" discipline `partition_owner_table[]` established one layer up in Phase 2.

**Safety property carried over from the original Phase F gap, made explicit.** The original comment on `free_physical_ram_frame()` (Gap Remediation Phase F) named a *separate*, larger, still-unsafe project: a per-process page-table walker, needed if you wanted to reclaim every frame a process's page table merely *points at*. `user_clone_page_table()`'s current design (`arch/x86/user_paging.c`) clones a new process's PML4 by copying all 512 raw entries from the running kernel's own PML4, so a walker that freed everything reachable from a process's own page table without first distinguishing owned-vs-inherited-by-copy entries would free live kernel/shared frames out from under the rest of the system. `partition_reclaim_all_frames()` sidesteps this entirely by construction: it never touches any process's page table at all, only `frame_pool.c`'s own ownership tracking for frames that were specifically tagged with a real `partition_id` at allocation time (the five accounted call sites). It cannot, and does not try to, reclaim a frame a process's page table merely references without `frame_pool.c` itself having attributed that frame to the partition being destroyed. The page-table-walker gap named in LPAR Phase 14's findings remains exactly as unresolved as it was before this phase — this phase closes the narrower, concretely-scoped gap the roadmap named, not the broader one.

`kernel/partition.c`: `partition_destroy()`'s step 3 now calls `partition_reclaim_all_frames(partition_id)` (captured as `frames_reclaimed`) instead of `partition_reset_frame_usage(partition_id)`. The function's closing log line now also reports `%u physical frame(s) reclaimed`. `partition_reset_frame_usage()` itself is untouched and still exported — no longer called from `partition_destroy()`, but kept as a still-valid, lower-level accounting-only primitive per its own updated header comment, the same "narrow the call site, don't delete the function" discipline this project uses whenever a superseded primitive might still have a legitimate standalone use.

**Verification exceeded the plan.** `frame_quota_host_test.c` (LPAR Phase 13's original test, extended, still linking the real, unmodified `frame_pool.c`) gained a new scenario 12 with 15 checks: allocates 4 frames to one partition and 1 to an unrelated partition interleaved between them, reclaims the first partition, and proves — not just asserts — the reclaim was real by re-allocating and confirming the exact same 4 physical addresses come back (they are provably the only free slots in the bitmap at that point in the test, since everything below them was already permanently consumed by earlier scenarios, making this a causal proof rather than a coincidence); confirms the unrelated partition's frame survives completely untouched and remains independently freeable afterward (proving the reclaim was correctly scoped, not a blanket sweep); confirms the out-of-range sentinel, the legitimate "never allocated anything" zero case, and idempotent re-reclaim of a partition that (correctly) re-acquired frames during the test's own verification loop. `scheduler_fairness_host_test.c` (LPAR Phase 12's original test, which already call-tracked `partition_destroy()`'s orchestration with fake stand-ins for its heavier cross-subsystem calls) had its `partition_reset_frame_usage()` call-tracking stub swapped for one tracking `partition_reclaim_all_frames()` instead, confirming `partition_destroy()` calls the new function exactly once with the destroyed partition's real id — the same orchestration-correctness proof the old stub gave, now pointed at the new call site.

Both `frame_pool.c` and `partition.c` compile cleanly under `gcc -Wall -Wextra -std=c11 -fsyntax-only` with zero new warnings.

**The regression cascade this phase's `partition_destroy()` call-site change triggered, and how it was resolved.** Every host test that links the real, unmodified `partition.c` was, by definition, compiling against `partition_destroy()`'s object code referencing whatever function that call site names — changing the name broke the link for all 5 such files (`partition_host_test.c`, `ipc_partition_host_test.c`, `persist_partition_host_test.c`, `scheduler_fairness_host_test.c`, `workmgmt_phase4_host_test.c`), the same 5 files Phase 2's `cluster_local_node_id()` addition broke for the identical structural reason. Three (`partition_host_test.c`, `ipc_partition_host_test.c`, `persist_partition_host_test.c`) never assert anything about frame reclamation, so their `partition_reset_frame_usage()` no-op stub was simply renamed to a `partition_reclaim_all_frames()` no-op stub. The other two (`scheduler_fairness_host_test.c`, `workmgmt_phase4_host_test.c`) previously call-tracked the old function; `scheduler_fairness_host_test.c` actually asserted on that tracking (see above, updated accordingly), while `workmgmt_phase4_host_test.c` only ever incremented an unread counter, so it collapsed to a plain no-op stub. Same "new dependency breaks stub-based lighter-scaffold tests" pattern this project has now hit and resolved identically across Phase 1, Phase 2, and this phase — not a new failure mode.

**What this phase deliberately did not do, named rather than silently skipped.** Frames handed out through the plain, unaccounted `allocate_physical_ram_frame()` path (page-table internals, NVMe queues, SMP stacks, the shared SIMI activation cache, catalog index nodes) are still attributed to `PARTITION_SYSTEM` and are never reclaimed per-partition — but `PARTITION_SYSTEM` can never itself be destroyed (`partition_destroy()` rejects it outright), so this is not a gap this phase left open, it's the same permanent, correct scope boundary LPAR Phase 13 originally drew. The per-process page-table-walker project named above remains fully out of scope, unattempted, and unsafe to attempt with this kernel's current `user_clone_page_table()` design — exactly as it was before this phase, just no longer blocking the narrower gap Phase 3 needed to close.

## 7. Phase 4 — Partition-scoped consensus leases — DONE

**Why this is the largest structural change in this roadmap.** Consensus today has no seam to attach partition-level meaning to. `local_cluster_state` is one global term/role/vote per node; the entire practical effect of losing quorum is a single global flip (`update_page_table_permissions_globally(1)`, called from `trigger_kernel_election_campaign()` in `net/consensus.c`) stripping write access from every SLS object process-wide, all-or-nothing. There is no per-object, per-partition, or per-anything-except-the-whole-node granularity in the Raft-lite implementation as it stands.

**Scope.**
- Extend the leadership/term concept from "one role per node" to "one lease per partition per node" — i.e., which node currently holds write-ownership of partition *P*, agreed via the same request-vote/heartbeat message shapes (`ConsensusMessage`) already defined, just scoped to a partition rather than the whole cluster.
- `update_page_table_permissions_globally()`'s all-or-nothing behavior becomes partition-scoped: losing a lease for partition *P* strips write access to *P*'s objects only, not every object on the node.
- This phase does **not** attempt full per-partition Raft logs — that's a materially larger change (per-partition log replication, not just per-partition lease state) that this roadmap explicitly scopes out for v1, the same way LPAR Phase 12 explicitly scoped out real weighted fairness in favor of starvation-prevention as a first cut. A lease is enough to answer "which node may currently accept writes for this partition," which is what Phase 6 (migration) actually needs.

**Verification plan.** Compile-check at minimum; attempt the same host-test reachability check Phase 1 already establishes for `consensus.c` — if the roster/identity logic is host-testable, the lease state machine built on top of it very likely is too, since it's the same file with the same shallow dependency graph.

**Findings addendum (as built).** Landed as a mechanism layered *alongside* Phase 1's cluster-wide election machinery, not a replacement for it — the single biggest design decision this phase made, and worth stating plainly: `local_cluster_state`/`cluster_roster[]` (Phase 1) still answer "which nodes exist and what's the majority quorum," a real, node-level, cluster-wide question independent of any one partition. What this phase adds on top is a second, separate question asked once per partition *P*: "which node currently holds *P*'s write lease" — collapsing the two into one term/role, as a naive extension of Phase 1's `struct ClusterNode` might have done, would mean one partition's lease churn forces an unrelated cluster-membership re-election. Kept apart instead.

`net/consensus.h`: `struct ConsensusMessage` gained a `partition_id` field. Safe to grow for free in this codebase today — the struct is always carried inside a `struct DSPPFullPagePacket`'s 4KB `payload_4kb` buffer, never marshaled across an actual wire boundary anywhere yet (`cluster_init()` is still never called at boot, per Phase 1's own finding, unchanged by this phase), so there's no deployed older-layout node to misparse against. Three new opcodes, `DSPP_CMD_PARTITION_REQUEST_VOTE`/`_VOTE_REPLY`/`_HEARTBEAT` (0x13-0x15), distinct from Phase 1's originals (0x10-0x12) rather than reusing them — the new opcodes' payloads always carry `partition_id`, the old ones never do, and conflating them would mean every RX handler needs to sniff payload content to know which state machine a packet belongs to instead of just checking `header.opcode`. New `struct PartitionLease { partition_id, term, voted_for, role, heartbeat_ticks_elapsed, accumulated_votes, active }` and `extern struct PartitionLease partition_lease_table[PARTITION_LEASE_MAX]` (`PARTITION_LEASE_MAX = 16`, an independent constant mirroring `PARTITION_MAX` rather than `#include`-ing `kernel/partition.h` — net/ headers stay strictly one-directional relative to kernel/ headers, the same discipline `CLUSTER_NODE_MAX` already follows). No validation against `kernel/partition.c`'s `partition_table[]` — a lease can be pre-established for a `partition_id` that doesn't exist yet, the identical posture `frame_pool.h`'s `partition_set_frame_quota()` already documents for the same reason (and, more importantly, avoids `net/consensus.c` reaching back into `kernel/partition.c`, the wrong-direction dependency Phase 2 was careful to avoid when it went the other way).

`net/consensus.c`: `find_lease_row()` (static, find-by-`partition_id`, linear scan) backs the whole API, the identical table shape `kernel/partition.c`'s `partition_owner_table[]` established in Phase 2. `partition_lease_init()`, `partition_lease_get_role()`/`_get_term()`, `partition_holds_write_lease()`, `partition_lease_trigger_election()`, `partition_lease_heartbeat_tick()` / `check_partition_lease_heartbeat_tick()` (ticks every active row — the public, boot-wireable entry point, mirroring `check_consensus_heartbeat_tick()`'s own role for Phase 1), and `process_partition_consensus_packet()` (routes the three new opcodes to a specific lease row, creating one on demand if none exists yet — an incoming `REQUEST_VOTE` for a partition this node has never leased must still be answerable). `update_page_table_permissions_for_partition(partition_id, force_read_only)` — a new extern, defined as a still-stub sibling of Phase 1's `update_page_table_permissions_globally()` in `kernel/stubs.c` (same "deferred until page table management is complete" honesty, now narrowed to a specific partition's processes instead of every process on the node) — replaces the global call at both of this phase's own strip/restore points (election start strips just *P*; quorum-achieved restores just *P*).

**A real design adaptation, not a copy-paste, and worth naming.** Phase 1's `process_consensus_packet()` accumulates votes in a single `static uint32_t accumulated_votes = 1` local — correct for exactly one concurrent election (the whole node's), wrong for this phase: with up to `PARTITION_LEASE_MAX` partitions potentially campaigning at once, each at its own term, a shared counter would conflate votes meant for entirely different partitions' elections. `accumulated_votes` instead lives as a field on `struct PartitionLease`, reset to 1 (self) whenever `partition_lease_trigger_election()` starts a fresh campaign for that specific row. The quorum threshold itself, though, deliberately *is* shared: `partition_holds_write_lease()`'s promotion check reuses `local_cluster_state.stable_quorum_threshold` (Phase 1's roster-derived majority) rather than inventing a second majority concept — partition leases are being agreed among the exact same cluster nodes Phase 1's roster already tracks, not a sub-cluster, so a second quorum notion would be redundant, not more correct.

**Verification exceeded the plan.** Extended `tests/consensus_phase1_host_test.c` in place (Scenarios 11-19, 45 new checks) rather than splitting into a second file — same file, same stub set, the established "reuse the test that already links the right dependency graph" precedent this whole roadmap follows. Real execution throughout, not just accessor checks: a new call-tracking stub for `update_page_table_permissions_for_partition()` proves election-start and quorum-achieved actually call it with the correct `partition_id` and `force_read_only` value (1 then 0), not just that *some* permission function gets called somewhere. Real transmitted-packet inspection confirms the new `partition_id` field is genuinely populated on the wire for `PARTITION_REQUEST_VOTE`/`PARTITION_HEARTBEAT`/`PARTITION_VOTE_REPLY`, the same "prove the packet contents, not just the state transition" discipline Phase 1's own Scenarios 8-10 established. The concurrency-isolation design decision above is directly tested: partitions 5 and 6 campaign simultaneously (partition 6's election triggered while 5 is still mid-flight as CANDIDATE), and every subsequent check on one partition's state is paired with a check that the other partition's state is completely untouched — proving `accumulated_votes`-per-row actually prevents cross-contamination, not just asserting it by design. The quorum test itself raises the roster to 5 active nodes (quorum 3) specifically so "one external vote is insufficient, a second reaches quorum" can be demonstrated as two distinct steps, mirroring Phase 1's own two-step quorum rigor rather than settling for a single-vote quorum-of-1 case that wouldn't actually exercise the accumulation logic. 84 checks total in the file (39 Phase 1 + 45 Phase 4), all passing.

Both `net/consensus.c` and `kernel/stubs.c` compile cleanly under `gcc -Wall -Wextra -std=c11 -fsyntax-only` with zero new warnings — checked both with explicit `-I` flags (the host-test style) and with **zero** `-I` flags, matching the real Makefile's `X86_CFLAGS` exactly (see the standalone build-break fix earlier this session: `net/consensus.c`'s original Phase 1 `#include "kernel_io.h"` only worked under `-I kernel`, and broke the real cross-build the first time it ran, since `X86_CFLAGS` has no `-I` paths at all and resolves quoted includes purely relative to the including file's own directory). This phase's own new `#include`s (none added -- `consensus.c`'s include list is unchanged) and the new `struct DSPPFullPagePacket*` forward-declaration placement were written with that exact failure mode in mind: the forward declaration was moved to the very top of `consensus.h`, before *any* function prototype that takes that pointer type, closing off the "declared inside a function prototype's own parameter-list scope creates a second, incompatible tag" bug class Phase 1 already hit once (see its own findings addendum) before this phase's new `process_partition_consensus_packet()` prototype could have silently reintroduced it by being declared too early in the file.

Full regression sweep: `bash tests/run_all.sh` — 41/41 host test files pass (unchanged count — this phase extended an existing test file rather than adding a new one, the same pattern Phase 2's `partition_host_test.c`/`persist_partition_host_test.c` and Phase 3's `frame_quota_host_test.c` extensions used). A repo-wide grep confirmed `net/consensus.c`, `net/prefetch.c`, and `kernel/partition.c` remain the only real (non-test) files including `net/consensus.h`, and both `net/prefetch.c` and `kernel/partition.c` were independently re-verified to still compile cleanly under the zero-`-I`-flags check above — this phase's header changes didn't disturb either.

**What this phase deliberately did not do, named rather than silently skipped.** No RX dispatcher exists anywhere in this codebase that actually routes incoming DSPP packets to `process_partition_consensus_packet()` vs. `process_consensus_packet()` vs. any of DSPP's own page-mirroring handlers based on `header.opcode` — confirmed unwired before this phase and unchanged by it, the same "compiled into the kernel image but dormant, nothing decides who calls it" honesty caveat Phase 1's own findings gave `check_consensus_heartbeat_tick()`/`process_consensus_packet()`. **Both are now closed:** Phase 7 built the RX dispatcher (`dspp_rx_dispatch()`, §9a) and §9b wired the heartbeat ticks to the BSP sweep. The lease mechanism is live. `update_page_table_permissions_for_partition()` remains a stub with no real body, same as its Phase 1 sibling — real enforcement (walking a specific partition's processes' page tables and actually clearing/restoring `PTE_WRITABLE`) is deferred until page table management is complete, a pre-existing gap this phase narrows the *scope* of but does not close. No leader-to-leader conflict resolution beyond what Raft-lite's term comparison already provides — this phase reuses that mechanism per-partition exactly as-is, it doesn't harden it. Full per-partition Raft log replication remains explicitly out of scope for v1, per this phase's own scope bullets — a lease only answers "which node may currently write," not "what has been durably agreed," which is what Phase 6 (migration) actually needs and all this phase set out to provide.

## 8. Phase 5 — Partition-aware DSPP routing — DONE

**Why this is a real protocol change, not a filter.** `struct DSPPPacketHeader` (`net/dspp.h:10-17`) carries `system_object_id` and `virtual_address` — enough to address a page, nothing about which partition that object belongs to. Bolting a partition check on top of the existing packet format without extending it would mean inferring partition membership from `system_object_id` on every packet handler, duplicating a lookup `object_catalog.c` already owns and risking the two falling out of sync — the same category of mistake Phase 8's "reuse the existing choke point" principle exists to avoid.

**Scope (as planned).**
- Add `partition_id` to `struct DSPPPacketHeader`, sourced from the object catalog at packet-construction time (mirroring how `catalog_check_access()` already resolves `partition_id` from `object_catalog[]`, not re-deriving it).
- `DSPP_PAGE_READ_REQ`/`DSPP_PAGE_WRITE_REQ` handlers gate on Phase 2's local/remote ownership check plus Phase 4's lease before servicing a request for a page whose partition isn't (or is no longer) owned locally.
- Wire format version compatibility: this is a real on-the-wire change to a packed struct (`__attribute__((packed))`), and needs an explicit magic/version bump story so a node running pre-Phase-5 code and a node running post-Phase-5 code don't silently misparse each other's packets — worth deciding concretely when this phase is scoped in detail, not resolved here.

**Findings addendum (as built).**

`net/dspp.h`: `struct DSPPPacketHeader` gained `uint32_t partition_id` as its last field (offset 32, struct grows 32→36 bytes; `struct DSPPFullPagePacket` grows 4128→4132 bytes — both `__attribute__((packed))`, so these are exact byte counts, not padding-dependent estimates). The version-compatibility question this phase's own scope bullet explicitly deferred is resolved here via a **magic-value bump**: `DSPP_MAGIC` changed from `0x534c534e45544d41ULL` ("SLSNETMA") to `0x534c534e45544d42ULL` ("SLSNETMB"); the old value is kept as `DSPP_MAGIC_V1`, not deleted, so a future real RX handler has something concrete to compare an incoming `header.magic` against and reject cleanly as "pre-Phase-5, no `partition_id` available" rather than silently trusting whatever bytes happen to sit at the new field's offset. This reuses `kernel/persist.c`'s `PERSIST_MAGIC_*` convention (a distinct magic value signals an incompatible layout) one layer over — wire instead of disk — rather than inventing a second version-signaling mechanism. Every real packet-construction call site (`net/consensus.c` ×6, `net/prefetch.c` ×1) already referenced the `DSPP_MAGIC` symbol, never a hardcoded literal, so the bump itself required zero call-site edits — confirmed by a repo-wide grep before considering this done. `net/dspp.h` also gained a proper `#ifndef DSPP_H` include guard it never had before (`net/consensus.h`'s own comment explicitly named this absence as the reason it uses a forward-declaration-only approach instead of a real `#include "dspp.h"`) — a small, low-risk, in-passing fix made while touching the file for a substantive reason; `net/consensus.h`'s forward-declaration approach itself was left untouched, not this phase's scope to revisit.

`net/dspp.c` (brand new — `dspp.h` previously had no corresponding `.c` file at all): `dspp_resolve_partition_id(system_object_id)` scans `object_catalog[]` for an active entry with a matching `object_id` and returns its `partition_id` (0 — "not found" — if none matches), mirroring an inline scan pattern already repeated at several call sites in this codebase rather than inventing a new lookup mechanism. `dspp_page_read_allowed(partition_id)` is Phase 2's `partition_is_local()` alone. `dspp_page_write_allowed(partition_id)` is `partition_is_local()` **AND** Phase 4's `partition_holds_write_lease()` — ownership and lease are deliberately kept as two separate checks, not collapsed into one, because a node can locally hold a partition's data while a different node holds the actual write lease (mid-migration, or during a split-brain window), and only a real write needs to check both. `process_dspp_page_packet()` is the real per-packet gate for `DSPP_PAGE_READ_REQ`/`DSPP_PAGE_WRITE_REQ`: resolves `partition_id` from the packet's `system_object_id`, checks the appropriate allow function, returns 1 (would be serviced) or 0 (denied) — deliberately does **not** move any actual page data, since no object-to-physical-frame resolution plumbing exists anywhere in this codebase to move; that remains a separate, larger integration this phase does not attempt. `net/prefetch.c`'s `prefetch_worker_kernel_thread()` was wired to populate the new `partition_id` field via `dspp_resolve_partition_id()` at its one real packet-construction site, immediately alongside the existing `system_object_id`/`virtual_address`/`opcode` assignments. `Makefile`'s `X86_C_SRC` gained `net/dspp.c` so it's actually compiled into the real kernel image, not just host-tested.

**Verification exceeded the plan.** The scope bullet above set the ceiling at "compile-check plus logic review of the packet size/offset math" — real cross-node execution being out of reach in this sandbox. That ceiling still holds for actual network transmission, but this phase's own new *logic* (partition resolution and the two allow-functions' gating) turned out to be real host-testable code, not just wire-format math, so a dedicated `tests/dspp_phase5_host_test.c` was written and run for real rather than settling for logic review. It links the actual, unmodified `net/dspp.c`, `kernel/partition.c`, and `net/consensus.c` together (not a reimplementation of any of them) — proving `dspp_resolve_partition_id()`/`dspp_page_read_allowed()`/`dspp_page_write_allowed()`/`process_dspp_page_packet()` genuinely call into Phase 2's real `partition_is_local()` and Phase 4's real `partition_holds_write_lease()`, not fakes. `object_catalog[]`/`object_catalog_count` are provided as plain dummy globals populated by direct struct writes — the same shortcut `persist_partition_host_test.c` already takes to avoid linking the much heavier real `kernel/object_catalog.c` dependency graph (journal/lock_mgr/index_mgr/etc.) this test has no interest in. 20 checks, all real execution: object resolution (found and not-found cases); read-allowed for a freshly-created local partition; write-denied for that same partition before any lease exists (proving locality alone isn't enough); a real election driven to a real quorum-of-1 via an actual constructed `DSPP_CMD_PARTITION_VOTE_REPLY` packet fed through `process_partition_consensus_packet()`, after which write flips to allowed and read is confirmed unaffected; a partition pinned to a remote node (`partition_set_owner_node()`) denied for both read and write regardless of lease state; and `process_dspp_page_packet()` exercised end-to-end for READ_REQ (local: serviced, remote: denied), WRITE_REQ (local + lease: serviced, local without lease: denied — the case that specifically proves the write gate checks the lease and not just locality), and an unrelated opcode (correctly unserviced). A `transmit_call_count` stub confirmed exactly one real packet was ever transmitted across the whole test (`partition_lease_trigger_election()`'s own `REQUEST_VOTE`) — `process_dspp_page_packet()` itself never transmits, consistent with its documented "gate only, no page-move plumbing" scope.

Both `net/dspp.c` and `net/dspp.h` (plus `net/prefetch.c` and `net/consensus.c`, re-checked since they consume the changed header) compile cleanly under `gcc -Wall -Wextra -std=c11 -fsyntax-only` with zero new warnings, checked both with explicit `-I` flags (host-test style) and with **zero** `-I` flags, matching the real Makefile's `X86_CFLAGS` exactly — the standing practice this whole roadmap adopted after the real `make x86-iso` build break earlier in this project (a bare `#include "kernel_io.h"` that only worked under `-I kernel`). `net/dspp.c`'s own `#include`s use the same relative-path convention every other `net/*.c` file uses (`"../kernel/object_catalog.h"`, `"../kernel/partition.h"`, `"../kernel/kernel_io.h"`), verified against that exact failure mode before considering this phase done.

Full regression sweep: `bash tests/run_all.sh` — **42/42 host test files pass** (up from 41 — this phase added a genuinely new test file, `dspp_phase5_host_test.c`, rather than extending an existing one, since `net/dspp.c` has a distinct three-way dependency graph — `object_catalog[]` + `kernel/partition.c` + `net/consensus.c` together — not currently linked together in any prior test file). `tests/consensus_phase1_host_test.c` (the file housing Phases 1 and 4's own 84 checks) was independently re-run standalone after this phase's `DSPPPacketHeader` size/layout change and magic-value bump, confirming zero regression into already-passing Phase 1/4 logic from Phase 5's wire-format changes.

**What this phase deliberately did not do, named rather than silently skipped.** The single biggest finding this phase surfaced: **there is no real RX dispatcher for any DSPP opcode anywhere in this codebase.** `net_rx_dispatch()` (`net/net.c`) only handles ARP/IPv4 Ethernet frames — the "DSPP_PAGE_READ_REQ/WRITE_REQ handlers" this phase's own scope bullet referred to do not exist as real, wired-in code anywhere; they only ever existed as narrative in `docs/SLS-OS-Scaling.md`, a design doc, never implemented. This is a more fundamental gap than the scope bullets assumed when this phase was originally drafted — consistent with this whole roadmap's established pattern (Phase 1 and Phase 4 both landed real, callable, host-tested primitives that are "compiled into the kernel image but dormant, nothing decides who calls it"), this phase built `process_dspp_page_packet()` as a real, correct, host-tested gate anyway, rather than treating the absent dispatcher as a blocker — but it remains genuinely uncalled by anything at runtime. No object-to-physical-frame resolution plumbing exists to let `process_dspp_page_packet()`'s allowed path actually move page bytes onto the wire (constructing and transmitting a `DSPP_PAGE_READ_ACK`/`WRITE_ACK`) — that is a separate, larger integration, out of scope here. Real execution across two actual network-connected nodes remains out of reach in this sandbox, unchanged from the original verification plan's own honest ceiling.

**Verification plan (original, superseded above).** Compile-check, plus logic review of the packet size/offset math given the packed-struct field addition. Real execution across two actual network-connected nodes is out of reach in this sandbox (no multi-node test harness exists for `net/e1000.c`) — flagged honestly as a verification ceiling this phase cannot exceed, the same way every kernel-side phase since Phase 4 of the SIMI-ISA doc has flagged the missing cross-compiler.

## 9. Phase 6 — Cold partition migration — DONE

**Why this comes last.** Migration is the orchestration layer sitting on top of every phase above it: it needs to know which nodes exist (1), which node currently owns the partition and where it's moving to (2), that moving it won't leak the source node's frames (3), a clean way to hand off write-ownership (4), and a way to actually move the partition's pages (5). Building migration before those exist would mean, the same way LPAR Phase 14 warned about lifecycle management, that "migrate" quietly leaves stale state on the source node — worse than not having the operation at all.

**Scope (as planned).**
- `partition migrate <id> <dest_node>`: pause the partition (reusing `partition_pause()` from LPAR Phase 14 directly, not reimplementing it), transfer ownership of the partition's catalog objects to the destination node via Phase 5's now-partition-aware DSPP path, hand off the Phase 4 lease, reassign Phase 2's ownership table entry, free the source node's frames via Phase 3's real reclamation, then resume on the destination.
- Explicitly **cold** migration only: the partition is unavailable for the duration of the move. No attempt at live/hot migration (keeping the partition servicing requests while pages transfer in the background) in this phase — see §10.
- Partial-failure handling gets the same explicit design attention LPAR Phase 14 gave partition destroy: what happens if the transfer is interrupted partway through. This roadmap does not resolve that here, the same way LPAR Phase 14 flagged it as "worth real design attention when this phase is actually scoped in detail" rather than inventing a rollback mechanism against a failure mode not yet concretely understood.

**Findings addendum (as built).**

`kernel/partition.c`'s new `partition_migrate(partition_id, dest_node_id)` is the real orchestration, in this exact order: (1) `partition_pause()` (LPAR Phase 14, reused directly), (2) a new `partition_lease_step_down()` (net/consensus.c, Phase 6's own small addition alongside Phase 4's lease API — see below), (3) `partition_set_owner_node()` (Phase 2, the actual load-bearing ownership handoff, which already persists internally via Phase 10's `persist_partitions()`), (4) `partition_reclaim_all_frames()` (Phase 3) — deliberately only reached if step 3 succeeds; see the partial-failure paragraph below for why. Guards up front reject `PARTITION_SYSTEM` (mirrors `partition_destroy()`'s own permanent-partition guard), an unknown/inactive `partition_id`, `dest_node_id == 0` (the reserved Phase 1 "uninitialized" sentinel — migrating *to* it is a caller bug, not a real destination), and `dest_node_id` equal to the current owner (a no-op, rejected rather than silently succeeding).

**The scope bullet's "hand off the Phase 4 lease" turned out to need one small, genuinely new primitive, not a reuse of an existing one.** Phase 4 built `partition_lease_trigger_election()` (campaign to *acquire* a lease) but nothing to voluntarily *relinquish* one — the opposite operation. This phase adds `partition_lease_step_down(partition_id)` to `net/consensus.h`/`.c`: transitions the row to `ROLE_FOLLOWER`, resets `voted_for`/`accumulated_votes`/`heartbeat_ticks_elapsed`, leaves `term` unchanged (stepping down isn't itself a new term — a future election, on whichever node campaigns next, advances the term when it actually campaigns, the same way Raft never needs an outgoing leader to manufacture a term bump for itself). Returns 0 if a real lease row existed and was stepped down, 1 if `partition_id` had no lease row at all — "nothing to relinquish" is a normal outcome (a partition that was never contested on this node), not an error, and `partition_migrate()` doesn't treat it as one. Like every other Phase 4/5 lease and routing primitive, this is deliberately **local-only and transmits nothing**: there is no `DSPP_CMD_PARTITION_*` opcode for "I am voluntarily stepping down" and no RX dispatcher anywhere in this codebase that would receive one if there were (Phase 5's own finding, still true here) — a real destination node would simply call `partition_lease_trigger_election()` itself once it observes, via Phase 2's now-updated `partition_owner_table[]`, that it owns the partition and no one is heartbeating it.

**The scope bullet's "transfer ownership of the partition's catalog objects... via Phase 5's now-partition-aware DSPP path" does not correspond to any real mechanism, and this phase says so rather than inventing one to fit the sentence.** Phase 5's `dspp_page_read_allowed()`/`_write_allowed()` are per-packet *gating* checks ("should this specific request be serviced right now"), not an object-catalog reassignment mechanism — there is no function anywhere in `net/dspp.c` that moves or retags a catalog object's `partition_id`, because no object-to-physical-frame resolution plumbing exists to move (Phase 5's own findings addendum already named this). A partition's catalog objects (`kernel/object_catalog.c`) keep their existing `object_id`/`partition_id` completely unchanged by a `partition_migrate()` call; what actually moves is the ownership *record* (step 3 above), which is what makes `dspp_page_read_allowed()`/`_write_allowed()` on the new owner node start returning true. This is consistent with this whole roadmap's "groundwork, not a hypervisor" framing (see `partition.h`'s own top-of-file comment): "migration" here means the ownership record moves, not that any data is copied.

**"Resume on the destination" is explicitly NOT performed, named rather than silently dropped or faked.** `partition_migrate()` runs entirely on the source node. Calling `partition_resume()` at the end of the function, on the source, would resume the partition on the node that no longer owns it — `partition_is_local(partition_id)` is already false there by that point. Real resumption on the destination node would require that node to itself notice the ownership change (e.g. by polling `partition_owner_table[]`) and act, or to receive a real handoff message — the latter needs a wire protocol this codebase doesn't have (no RX dispatcher for any DSPP opcode, unchanged from Phase 5). The partition is intentionally left **PAUSED** when `partition_migrate()` returns successfully.

**Partial-failure handling: a real, meaningful ordering decision, not just a note.** `partition_reclaim_all_frames()` (step 4) is deliberately gated on step 3 (`partition_set_owner_node()`) succeeding first — if the ownership handoff itself fails, the function aborts immediately and does **not** reclaim frames, logging that ownership never actually moved. Freeing this node's physical frames before ownership has genuinely moved would free memory a partition that still (correctly) believes itself locally owned might still be using — the same "don't reclaim until the state that justifies it is real" discipline `partition_destroy()`'s own step ordering already follows. In practice, `partition_set_owner_node()` can only fail if `partition_id` becomes invalid between this function's own upfront validation and that call, or if `partition_owner_table[]` is completely full with no existing row for `partition_id` — the latter is structurally unreachable via the public API (every valid `partition_id` always gets an owner row stamped at `partition_create()`/`partition_init()` time, so the owner table's active-row count never exceeds the partition table's), named honestly here rather than contrived into an artificial test.

Syscall `SYS_SLS_PARTITION_MIGRATE` (218 — the next free number after Phase 13's 217) with a `struct SLSPartitionMigrateRequest { partition_id, dest_node_id }` two-field request struct (do_syscall()'s single-arg ABI can't carry two `uint32_t`s directly, the same reason `SLSPartitionAssignRequest` exists), dispatched in `syscall_dispatch.c`, and a `partition migrate <id> <dest_node>` shell command in `user/shell.c` following the exact two-token parsing shape `"partition assign "` already established.

**Verification matched the plan, and then went further where it was cheap to.** The scope's own verification plan set the ceiling at compile-check-plus-logic-review, matching LPAR Phase 14's own ceiling for `partition_destroy()`. That held for the orchestration's ordering logic, but — as Phase 14's own test already proved for `partition_destroy()` — a call-tracking-stub test can verify real orchestration (order, arguments, return values) without needing every cross-subsystem effect to be real, so this phase wrote one: `tests/partition_migrate_phase6_host_test.c`, reusing the exact three-file dependency graph Phase 5's `dspp_phase5_host_test.c` already established (`kernel/partition.c` + `net/consensus.c`, real and unmodified, linked together) since `partition_migrate()` itself calls straight into Phase 4's real `partition_lease_step_down()`/`partition_holds_write_lease()` and Phase 2's real `partition_set_owner_node()`/`partition_is_local()` — real cross-phase orchestration, not three isolated units. `partition_reclaim_all_frames()` (Phase 3, `frame_pool.c`) is stubbed as a call-tracking counter, the same technique `tests/scheduler_fairness_host_test.c` already used to verify `partition_destroy()`'s own orchestration, rather than linking `frame_pool.c`'s real bitmap logic (already covered on its own in `tests/frame_quota_host_test.c`). 30 checks, all real execution: every validation guard rejected with zero state change (including confirming zero frames were reclaimed by any rejected call); `partition_lease_step_down()` exercised directly, both its "nothing to relinquish" (returns 1) and "really relinquished" (returns 0, verified via `partition_lease_get_role()` flipping to `ROLE_FOLLOWER`) outcomes; a full successful migration of a partition that genuinely held its own write lease (won via a real election driven to a real quorum-of-1, the same technique Phase 5's own test used) — ownership moved, local-ness flipped false, left paused, lease relinquished, and frame reclamation called exactly once with the correct `partition_id`; a second migration of a partition that was *never* leased at all, proving step-down's "nothing to relinquish" outcome doesn't block the rest of the orchestration; and a transmitted-packet count confirming `partition_migrate()` itself transmits nothing (every packet counted came from the test's own two election setups, not from either migration).

**A real regression this phase caused and fixed, named rather than glossed over.** Adding `partition_lease_step_down()` as a new external dependency of `kernel/partition.c` broke the link step of five pre-existing host tests that link the real `kernel/partition.c` but — reasonably, before this phase — never needed to stub or link anything from `net/consensus.c` beyond `cluster_local_node_id()`: `tests/partition_host_test.c`, `tests/persist_partition_host_test.c`, `tests/scheduler_fairness_host_test.c`, `tests/workmgmt_phase4_host_test.c`, and `tests/ipc_partition_host_test.c`. Caught by the full regression sweep (`bash tests/run_all.sh`), not by this phase's own new test (which links the real function, so never hit the gap). Fixed by adding the identical one-line stub (`int partition_lease_step_down(uint32_t partition_id) { (void)partition_id; return 1; }`, right alongside each file's existing `cluster_local_node_id()` stub) to all five — the safe, honest default ("nothing to relinquish") for tests that don't exercise migration at all, the same "stub returns the value that changes nothing" discipline every other cross-file stub in these tests already follows.

Both `net/consensus.c` and `kernel/partition.c` compile cleanly under `gcc -Wall -Wextra -std=c11 -fsyntax-only` with zero new warnings, checked both with explicit `-I` flags (host-test style) and with **zero** `-I` flags, matching the real Makefile's `X86_CFLAGS` exactly — the standing practice this whole roadmap adopted after the real `make x86-iso` build break earlier in this project. `kernel/syscall_dispatch.c` and `user/shell.c` were also compile-checked both ways; both have pre-existing implicit-declaration warnings unrelated to and unchanged by this phase's edits (confirmed by line number — none near this phase's additions), named here rather than silently ignored but out of this phase's own scope to fix.

Full regression sweep: `bash tests/run_all.sh` — **43/43 host test files pass** (up from 42 — this phase added a genuinely new test file, `partition_migrate_phase6_host_test.c`, plus the five one-line stub fixes above to keep the pre-existing files linking).

**What this phase deliberately did not do, named rather than silently skipped.** No real object/page data is moved by `partition_migrate()` — see the DSPP-path finding above; this remains a records-only, "groundwork, not a hypervisor" operation, consistent with every phase before it. No real resumption on the destination node — the partition is left paused, and closing that gap for real needs the wire protocol (RX dispatcher for DSPP opcodes) that Phases 1 and 5 both already identified as absent from this entire codebase, not something this phase can close in passing. No rollback of steps 1-2 (pause, lease step-down) if step 3 fails partway — the function aborts and reports failure, but a partition left paused with its lease already relinquished after a failed migration attempt must currently be recovered manually (`partition resume`, and a fresh election if the lease matters); an automatic rollback was considered and deliberately not built, the same "worth real design attention when this phase is actually scoped in detail" posture LPAR Phase 14 took for `partition_destroy()`'s own partial-failure handling, rather than inventing an untested rollback path for a failure mode (`partition_set_owner_node()` failing after upfront validation already passed) that's structurally near-unreachable via the public API today. Real execution across two actual network-connected nodes remains out of reach in this sandbox, unchanged from every prior phase's own honest ceiling.

**Verification plan (original, superseded above).** The orchestration layer itself will likely stay at compile-check-plus-logic-review, matching LPAR Phase 14's ceiling for `partition_destroy()`'s own orchestration. The pieces it calls into (Phase 2's ownership table, Phase 3's frame reclamation) should already have real-execution host tests from their own phases — this phase's test, if any is reachable, is really an integration check that the orchestration calls the right functions with the right arguments in the right order, the same call-tracking-stub technique LPAR Phase 14's own test used to verify `partition_destroy()` without needing every cross-subsystem effect to be real inside that one test.

**Phase 6 addendum: real stream/blob data movement (as built, see Multitenant Isolation Gap Analysis §13).** Both "records move, not data moved" quotes above are now half-superseded, named honestly rather than left to silently go stale: `kernel/stream.c`'s new `stream_relocate_partition()` is wired into `partition_migrate()`'s Step 3 and physically copies, verifies byte-for-byte, and relocates every stream/blob slot a migrating partition owns — the first genuine data-plane copy this function has ever performed, not just an ownership-record flip. Scoped to streams only: rowstore/vecstore table pages still move only in the ownership-record sense described above, since they have no per-partition page index to walk yet (that's `docs/AeroSLS-Storage-Isolation-Roadmap-v0.1.md` Phase 1's job). A further honest scope note: since `cluster_init()` is never invoked from any real boot path and this codebase has exactly one shared NVMe image, "destination" in this relocation is a fresh slot within the same shared pool, not literally a different machine's storage — the real cross-machine transport gap named throughout this section (no DSPP RX dispatcher) remains open. Verified by `tests/migration_data_movement_host_test.c`, 28 checks, full details in the gap analysis doc's §13.

## 9a. Phase 7 — Real cross-node data movement (RX dispatcher + wire framing) — DONE

Phase 5 and Phase 6 each independently surfaced the same finding: there is no receive-side dispatcher for any DSPP opcode anywhere in this codebase, and (a deeper finding this phase's own investigation added) no existing DSPP send site applies any Ethernet framing either — every send is a raw packed struct handed straight to `e1000_transmit_packet()`. A receive dispatcher built against nothing but `net_rx_dispatch()`'s existing ARP/IPv4 branches would have had no ethertype to key off, so both gaps were closed together: a new `ETHERTYPE_DSPP` (`0x88B5`) plus a single `dspp_transmit_raw()` choke point every DSPP send now goes through, and a real `dspp_rx_dispatch()` wired into `net/net.c`'s `net_rx_dispatch()`.

On top of that framing fix, a new, deliberately separate wire family (`DSPP_MIGRATE_MAGIC`, four opcodes: `BEGIN_REQ`/`BEGIN_ACK`/`PAGE_REQ`/`PAGE_ACK`) carries real stream/blob data between nodes: `kernel/stream.c`'s new `stream_migrate_send_partition()` (sender) and `stream_migrate_recv_begin()`/`stream_migrate_recv_page()` (receiver) genuinely push and apply page bytes across the wire, fire-and-forget (the sender never blocks on or retries against the ACKs the receiver does send — `kernel/net_event.h`'s `net_event_hlt_wait()` contains raw `sti; hlt` that faults in userspace, making any blocking wait fundamentally unexecutable in this sandbox, the same ceiling every blocking `net/tcp.c` primitive already hit in the Network Fairness phase). `kernel/partition.c`'s `partition_migrate()` now branches on `cluster_local_node_id() != 0`: the new wire path once a real cluster is configured, the old §9 same-disk `stream_relocate_partition()` path by default — every real deployment and every pre-Phase-7 host test, unchanged.

Verified with a real two-simulated-node host test (`tests/cross_node_migration_host_test.c`, 38 checks): the real sender logic runs once as "node A," its exact real Ethernet-framed output bytes are captured, only the receiving side's own bookkeeping is reset to represent a fresh "node B," and those exact captured bytes are fed through the real, unmodified `dspp_rx_dispatch()` — proving genuine wire encode/decode and apply-side fidelity without literally running two kernels. Full detail, the framing/buffer-sizing bug this test caught and fixed, the fire-and-forget rationale, and the twelve-file linker blast-radius repair are all written up in the Multitenant Isolation Gap Analysis doc's §20, not duplicated here.

**What this closes and what it doesn't.** The "no RX dispatcher anywhere in this codebase" finding both Phase 5 and Phase 6 named is closed — a real one exists, is wired into the real receive path, and genuinely applies received stream data locally. What Phase 6 left open remains open, now more precisely: "resume on the destination" still has no opcode or handler built on top of the dispatcher that now exists; real execution across two actual physical network-connected nodes is still out of reach in this sandbox; and rowstore/vecstore table data still moves in the ownership-record sense only (§6's Storage Isolation Phase 1 dependency, unchanged).

**Addendum: boot-time node identity closed — real hardware/VM testing is now meaningful.** A user asking "can I actually boot two real nodes to test this?" surfaced a finding this whole roadmap had disclosed but never closed: `net/consensus.h`'s own header comment on `cluster_init()` named the boot-time config-source question ("a command line arg, an NVMe-persisted config block, a build-time constant") as "deliberately NOT part of this phase," and a repo-wide grep confirmed every real call site was in a host test — `cluster_init()` was dead code from any real boot path. Booting two real QEMU instances (or two physical machines) before closing this would have both come up at node-id 0, silently taking the old same-disk relocate path instead of this phase's new DSPP wire path.

Closed via the mechanism the accessor comment's own forward-looking note already anticipated ("a future cluster-status HTTP/shell surface"), not a multiboot2 command-line parser (a valid, more automatic alternative, not attempted here — this kernel boots from a GRUB ISO via QEMU's `-cdrom`, not `-kernel`, so a real cmdline parser would also need a `grub.cfg` templating change, a heavier lift than an operator command): new syscalls `SYS_SLS_CLUSTER_INIT`/`SYS_SLS_CLUSTER_STATUS` (279–280, `net/consensus.h`/`.c`, thin wrappers over `cluster_init()`/a new serial-printing status report), dispatched in `kernel/syscall_dispatch.c`, and shell commands `cluster init <node_id>` / `cluster status` (`user/shell.c`), mirroring `partition destroy <id>`'s own single-`uint32_t`-argument ABI shape exactly. `sys_sls_cluster_status()` explicitly warns on-screen when `node_id == 0` that `partition_migrate()` will take the same-disk path, not the cross-node one — the exact confusion this addendum exists to prevent.

**Addendum: a real two-node launch script.** Following straight on from the above, `run-two-nodes.sh` (repo root) automates the actual two-instance boot: it runs `make x86-iso` once, creates two independent 10GB disk images (`sls_storage_nodeA.img`/`_nodeB.img` — the default `sls_storage.img` `make x86-run` uses is left untouched), and launches two `qemu-system-x86_64` instances whose e1000 NICs are connected directly to each other via QEMU's `-netdev socket` (one `listen`s, one `connect`s) rather than the Makefile's own `x86-run` target's `-netdev user` NAT mode — chosen specifically because NAT mode *isolates* each VM from every other VM, which is exactly why two instances launched via `make x86-run` twice would never exchange a single Ethernet frame regardless of `cluster_init()`. `-netdev socket` needs no host bridge/tap setup or elevated privileges, unlike a "more real" bridged network, at the cost of only connecting exactly two peers rather than an arbitrary LAN — sufficient for this test. The script prints the exact `cluster init 1` / `cluster init 2` commands to type into each instance's console once its shell prompt appears, and the `partition migrate` command to run afterward.

**Script superseded and removed.** `run-two-nodes.sh` no longer exists; `./run-cluster.sh --nodes 2` does everything it did without the two-node cap, and nodes now self-identify at boot rather than being told over a console. The paragraph below is kept as the record of why the topology was chosen — the reasoning still holds, only the file name and the peer count have changed.

**Topology superseded.** The `-netdev socket` `listen`/`connect` pair described above was strictly point-to-point, capping the cluster at exactly two nodes. It is now `-netdev socket,mcast=` — one shared L2 segment that any number of nodes join independently, which also removes the launch-ordering constraint (nothing waits for a listener to bind). See `docs/AeroSLS-N-Node-Launcher-Plan-v0.1.md` Phase 2, including the self-echo guard that a shared segment made necessary.

**Verification ceiling retired, and what the first real run found.** This script previously carried a note that it had never been executed. It has now, on a headless Ubuntu server, and it failed three ways — all since fixed, and two of them were never headless-specific:

1. `-display gtk` was hardcoded, so a machine with no X/Wayland display got `gtk initialization failed` and an immediate exit. The backend is now selected at runtime (`AEROSLS_DISPLAY` overrides).
2. Nothing checked that QEMU survived. `$!` is set for a process that has already died, so the script printed "Both instances launched" plus two PIDs *after node A had exited*. Each launch is now probed and QEMU's own stderr is surfaced.
3. **The one that was wrong everywhere:** `-serial file:` is output-only, and this kernel's shell is serial-*only* — `read_line()` (`kernel/kernel_io.c`) polls `inb(SERIAL_COM1_BASE)` and there is no PS/2 keyboard driver in the tree. So the documented workflow of typing `cluster init 1` into a QEMU window could never have worked on any host, display or not: the window renders VGA and has nowhere to send a keystroke. Each console is now a loopback socket chardev (`telnet=on`, plus `logfile=` so the debug log is unchanged), attached with `telnet 127.0.0.1 12341` / `12342`.

Its successor is exercised by `tests/run_cluster_harness.sh`, which puts a stub `qemu-system-x86_64` on `PATH` — one that reproduces the reported gtk failure — and drives the real script through the paths that broke: 75 checks, and mutation-tested by reintroducing each bug. That harness also found a fourth, of its own making: capturing the PID via `PID=$(launch_node ...)` runs the launch in a command substitution, and bash fires `EXIT` traps when such a subshell finishes, deleting the stderr capture the error report then needed. `cluster_register_peer()` is not required for the migrate path itself to work — DSPP migrate frames are self-filtered by destination node id, not by roster membership — only `cluster_init()` needs to have actually run.

Verified: `tests/consensus_phase1_host_test.c` Scenario 20 (4 new checks, 88 total, up from 84) proves `sys_sls_cluster_init()` genuinely flips `cluster_local_node_id()` (not a no-op wrapper) and correctly rejects the reserved sentinel, and that `sys_sls_cluster_status()` runs to completion. `net/consensus.c` compiles clean under `gcc -fsyntax-only` both with and without `-I` flags (matching the real Makefile's `X86_CFLAGS`); `kernel/syscall_dispatch.c` and `user/shell.c` compile clean the same way, zero new errors. Full regression: 60/60 host tests passing, zero regressions.

## 9b. The heartbeat was never wired, and the timeout could not have worked once it was

**Found by running a real four-node cluster and reading its status output.** `tools/aeroslsctl --host localhost:3001 cluster status` reported `role FOLLOWER`, `term 0`, `active nodes 4`, `quorum threshold 3`. Every number is correct. The cluster is also completely inert, and nothing on that screen says so.

### The finding

`check_consensus_heartbeat_tick()` had **no caller anywhere in the kernel**. A repo-wide grep returned its definition, its prototype in `consensus.h`, and one host test. Its own comment read:

```c
// Executed every 10ms by the kernel timer interrupt handler on Core 3
```

That was never true. There is no Core 3 (`docs/AeroSLS-LLM-Inference-Feasibility-v0.1.md` §2.5 already recorded the same fiction about `prefetch_worker_kernel_thread()`), and the LAPIC timer ISR calls nothing of the sort. `check_partition_lease_heartbeat_tick()` was in the same position — though *its* header comment in `consensus.h` was honest, carrying a "not actually wired into the real timer yet" caveat. The two comments contradicted each other for four phases, and the confident one won every code review.

Consequences, in order:

- No node ever reaches the silence threshold, so no node ever campaigns.
- No campaign means no leader, ever. Every node stays `FOLLOWER` at `term 0` indefinitely — which reads exactly like a healthy freshly-started cluster.
- Nothing calls `partition_lease_init()` outside `consensus.c`, so no lease row exists, so `partition_holds_write_lease()` returns 0 for every partition.
- Therefore `dspp_page_write_allowed()` (`net/dspp.c`) is permanently false.

The practical blast radius today is small, because the page-move plumbing that gate protects was never built either (`dspp.c` says so in the `READ_REQ` branch), and `partition_migrate()` takes the stream path without consulting a lease. But "the safety gate is stuck closed and nothing notices because the thing it guards is also missing" is not a state to leave undocumented.

### The second bug, which the fix exposed

Wiring the ticks to a caller is one line each. Doing it *correctly* is not, because both functions were written against an assumption that no longer holds.

The natural home is the BSP's HTTP sweep in `net/http.c`, next to `service_heartbeat_tick()` — both **transmit**, and the NIC TX path is not safe to drive from two cores at once, the same constraint `kernel/workload.h` documents for the reconciler. But that sweep is a busy loop whose rate depends on request load. Against it:

- `heartbeat_ticks_elapsed++` counted **calls**, not time. The `150` threshold meant "1.5 seconds" only at exactly 100 Hz. From the sweep it means "150 sweeps" — milliseconds when idle, many seconds under load, and a different duration on every node.
- The `ROLE_LEADER` branch transmitted on **every** call. Sane at 100 Hz; a broadcast storm from a busy loop, every frame of which every other node on the shared segment must receive and parse.

Both are now anchored to `kernel_tick_counter`, which the LAPIC timer advances at ~100 Hz (`kernel/timer.c`; `kernel/auth.h` already depends on the same rate for token TTL). The constants keep their original documented meanings for the first time. Callers pass `now` rather than the tick reading the clock itself, matching `service_heartbeat_tick(kernel_tick_counter)` — it keeps `consensus.c` free of an extern and lets a host test make 1.5 seconds pass without waiting 1.5 seconds.

### Election timing (`net/consensus.h`)

| Constant | Value | Meaning |
| --- | --- | --- |
| `ELECTION_TIMEOUT_BASE_TICKS` | 150 | 1.5 s of silence before a FOLLOWER campaigns |
| `ELECTION_STAGGER_TICKS` | 25 | 250 ms per node id, via `consensus_election_timeout()` |
| `LEADER_HEARTBEAT_TICKS` | 30 | 300 ms — five heartbeats per election window |

**The stagger replaces Raft's randomised timeout, and the trade is deliberate.** Raft randomises because its nodes are otherwise symmetric and would split the vote by campaigning together. This kernel already has something randomness cannot offer: a unique, stable node id from `node=N` on the boot command line (`kernel/boot_params.h`). Deriving the offset from it is deterministic — a host test can assert the exact tick a given node campaigns on — and it cannot collide, which random backoff can. The cost, stated plainly: node 1 always campaigns first, so leadership follows a fixed priority order, and a flapping node 1 will repeatedly take leadership back from a stable node 2. That is precisely what Raft's randomisation buys out of. For a cluster capped at `CLUSTER_NODE_MAX=8` on a local segment where 250 ms is far longer than a vote round trip, the first candidate simply wins and no split vote occurs at all.

### Three further bugs found while testing this

1. **`accumulated_votes` was a function-local `static`** in `process_consensus_packet()`, reset **only on reaching quorum**. A campaign that fell short left its votes banked, so the next campaign started pre-loaded and could declare quorum on fewer real votes than the threshold — two nodes each believing they held a majority. Unreachable while nothing drove elections; a live split-brain risk the moment something did. Phase 4's per-partition lease always had this right; the cluster-wide half did not. Now a field on `struct ClusterNode`, reset by `trigger_kernel_election_campaign()`.
2. **A campaign that does not restamp its own timer re-campaigns every tick**, inflating the term at sweep speed and invalidating every vote reply still in flight for the previous term. The node would never win an election it kept restarting.
3. **Granting a vote must restart the voter's timer.** Otherwise a follower that votes then immediately times out campaigns against the very candidate it just endorsed, at a higher term, invalidating its own vote — one election becoming an unbounded term-inflation race that elects nobody.

### Verification

`tests/consensus_phase1_host_test.c` Scenarios 21–31, **134 checks total** (up from 88). The two that matter most are a matched pair: 10 000 ticks at a frozen clock cause *no* campaign, and one tick past the deadline causes exactly one. A test with only the first would also pass against a tick that never fires — which is the original bug.

**10 of 10 mutations caught**, and two of them survived the first sweep and are the reason two scenarios exist at all:

- A mutation stopping `PARTITION_HEARTBEAT` from restarting a lease row's timer survived the entire suite. Scenario 17 checked the demotion and the term catch-up but never looked at the clock. Scenario 30 now does.
- A mutation removing the per-partition leader's rate limit survived, because Scenario 19 called the tick *once* — and once is indistinguishable throttled or not. Scenario 31 calls it 1000 times.

The recurring lesson, again: a property is only tested at the call count and buffer size where it can actually go wrong.

Whole-image link clean (97/97 TUs, 12 remaining undefined symbols all asm/linker-provided, unchanged). Full regression **80/80 host tests**. Five test files needed signature updates for the new `now` parameter (`dspp_phase5`, `cross_node_migration`, `partition_migrate_phase6`, `simi_ctx_migrate`, and the consensus test itself); each got a stated reason for its fixed clock reading rather than a bare `0`.

### What this still does not do

> **Correction, written after the next real run.** This section originally opened *"A leader is now elected, and that is a real end-to-end proof the multicast segment carries DSPP both ways."* That was wrong. No leader was elected. The cluster went from `FOLLOWER`/term 0 forever to `CANDIDATE` with a term climbing once per election timeout forever — a different failure wearing more convincing clothes. Two further bugs stood between the wired-up tick and an actual election, and §9c is the account of them. The paragraph is left standing below because the *reasoning* in it was sound: a completed election really would have been proof of a working round trip. The mistake was asserting the conclusion before watching it happen on the four-node cluster.

A leader being elected is a real end-to-end proof the multicast segment carries DSPP both ways — a `REQUEST_VOTE` has to leave one node and a `VOTE_REPLY` come back for it to happen at all. But the leader does not *do* anything yet: there is no log replication, and `dspp_page_write_allowed()` now opens for a lease holder that still has no page-move plumbing behind it. `update_page_table_permissions_globally()` and its per-partition sibling remain stubs in `kernel/stubs.c`, so the split-brain write-strip is still bookkeeping rather than enforcement. Named here so "the cluster elects a leader" is not mistaken for "the cluster replicates."

## 9c. Two more bugs between a wired heartbeat and an actual election

§9b wired the tick and declared victory. The four-node cluster then reported:

```
role   CANDIDATE
term   86
active nodes 4   quorum threshold 3
```

Correct numbers, no errors, no leader — and a term climbing once per 1.75-second election timeout. Two independent bugs, found in that order.

### Bug 1: the 4 KB DSPP families cannot cross an Ethernet segment

`struct DSPPFullPagePacket` is **4132 bytes**. A `REQUEST_VOTE` transmitted all of it, because `struct ConsensusMessage` (20 bytes) lives in that struct's `payload_4kb` field and every send site passed `sizeof(struct DSPPFullPagePacket)`. With 14 bytes of Ethernet header that is 4146 on a link whose MTU is 1500.

Three independent reasons it cannot work, each sufficient on its own:

| Layer | |
| --- | --- |
| TX | 4146 bytes in a single EOP descriptor on a 1500-byte link |
| RX (MAC) | `net/e1000.c` sets `RCTL = EN\|BAM\|UPE\|MPE`. **LPE — long packet enable, bit 5 — is clear**, so the MAC discards anything over 1522 bytes before a descriptor ever sees it |
| RX (driver) | `E1000_RX_BUF_SIZE` is 2048 and `e1000_poll_rx()` treats one descriptor as one whole frame — no EOP check, no chaining, no reassembly |

Measured sizes, and what each means:

```
DSPPPacketHeader             36 B   fits   <- cluster HEARTBEAT, the one that worked
DSPPServiceHeader           107 B   fits   <- service announce/withdraw
DSPPCtxMigrateHeader        121 B   fits   <- context migrate BEGIN
DSPPMigrateHeader           177 B   fits   <- stream migrate BEGIN
DSPPFullPagePacket         4132 B   DROPPED  <- votes, all PARTITION_*, PAGE_READ/WRITE_REQ
DSPPCtxMigrateChunkPacket  4217 B   DROPPED  <- the context payload
DSPPMigratePagePacket      4273 B   DROPPED  <- the stream payload
```

The blast radius is wider than the election. **Cross-node `partition migrate` has never moved a byte of payload over a real wire, and neither has live context migration.** In both, the BEGIN header fits and every packet after it is dropped — so a migration appears to start and then silently transfers nothing. Phase 7 and PEC Phase 3 were both signed off on this basis.

**Fixed for consensus, unfixed for pages.** Consensus messages have no business being 4 KB: `CONSENSUS_WIRE_LEN` is now `sizeof(DSPPPacketHeader) + sizeof(ConsensusMessage)` = **56 bytes**, at all five send sites, with a `_Static_assert` so a future field addition breaks the build rather than silently resuming undeliverable frames. Receivers needed no change — `dspp_rx_dispatch()` admits anything ≥ 36 bytes and both handlers read only the leading `ConsensusMessage`.

`dspp_transmit_raw()` now refuses anything over `DSPP_MAX_WIRE_PAYLOAD` (1486), counts it in `dspp_tx_oversize_dropped`, and logs it throttled (first occurrence, then every 1000th — it sits under a per-page loop). The counter is surfaced in `/api/cluster` as `dspp_oversize_dropped`, deliberately alongside role and term: *CANDIDATE + climbing term + rising drops* is a different diagnosis from *CANDIDATE + climbing term + zero drops*, and this failure previously presented as neither.

Refusing at the DSPP layer rather than in the driver is the point. `e1000_transmit()` would take the descriptor, return success, and the frame would simply never be seen by anyone.

### Bug 2: every granted vote was discarded on arrival

With frames finally crossing, the cluster still sat at `CANDIDATE`. `process_consensus_packet()` built its reply like this:

```c
reply_msg->term = local_cluster_state.current_term;      /* the OLD term */
if (msg->term > local_cluster_state.current_term) {
    local_cluster_state.current_term = msg->term;        /* now updated */
    reply_msg->vote_granted = 1;
```

A node granting a vote replied carrying the term it held **before** adopting the candidate's. The candidate counts a reply only when `msg->term == local_cluster_state.current_term` — the new one. Every granted vote arrived exactly one term stale and was thrown away.

Four nodes, every one voting yes, none ever reaching quorum. Each campaigns, receives three grants, counts zero, times out, campaigns again. One line moved below the branch fixes it. The partition-lease handler had the identical ordering, in the mechanism that actually gates writes.

### Why the test suite could not have caught either

Both failures live in the same blind spot, and it is structural rather than an oversight:

- **Every DSPP host test calls `dspp_rx_dispatch()` with a buffer already in memory.** The wire is the one part the strategy cannot reach. Worse, `cross_node_migration_host_test.c`'s `e1000_transmit()` stub accepted 4287-byte frames without complaint — *more permissive than the hardware it stood in for* — so the oversize assertion didn't merely go untested, it actively passed.
- **Every consensus scenario drove one node's handlers with hand-built packets.** "A REQUEST_VOTE produces a reply" passed. "A reply with the right term is counted" passed. Both halves were correct in isolation. Nothing took the reply one node *really* emits and fed it to the candidate that *really* asked.

The general lesson, which has now cost this project twice: **a stub that is more permissive than the thing it replaces converts a real failure into a passing test**, and testing two halves of a round trip separately proves nothing about the round trip.

### What was added

| | |
| --- | --- |
| `consensus_phase1_host_test.c` | **148 checks** (88 at the start of this work). Scenario 32 asserts every consensus send site fits an Ethernet frame, reading what the site actually passed rather than a constant. Scenario 33 is the full round trip: node 2's real reply bytes, fed to node 1 as the candidate that asked, ending in `ROLE_LEADER`. |
| `dspp_phase5_host_test.c` | 31 checks. The MTU boundary on both sides — exactly at the limit is accepted, one byte over is refused and counted — plus Scenario 10, which asserts the *broken* state of the 4 KB families so a later fix has to update the record rather than quietly diverge from it. |
| `cross_node_migration_host_test.c` | Scenario 1 rewritten to the truth: one BEGIN frame reaches the driver, both page packets are refused and counted. The byte-for-byte receive proof is preserved by rebuilding the page frames from node A's real disk. |

**18 mutations, all caught** across the session's two sweeps, including three that survived their first run and are the reason three scenarios exist at all.

### The testability seam, and its hazard

`dspp_max_wire_payload` is a variable, not a constant. No kernel code assigns to it. It exists so `simi_ctx_migrate_host_test.c` and `workload_ctx_host_test.c` — which test chunk reassembly, ordering, duplicate rejection and migration *orchestration*, all link-independent — can keep their coverage instead of being deleted or rewritten to hand-build every packet.

This project has already learned once that an override added for testability can remove the property it was added around, so it is written down in three places: the two files that use it say plainly that passing is **not** evidence a context crosses a real wire, and both name the tests that assert the link limit and never touch the variable.

### Closed by fragmentation — §9d

## 9d. Fragmentation: every DSPP family now fits an Ethernet frame

§9c left the two 4 KB families undeliverable and named the choice: jumbo frames or protocol-level fragmentation. Fragmentation won, and the reasoning is worth keeping because the obvious industry answer pointed the other way.

### Why the usual case against fragmentation does not apply here

The standard argument — one IP header per fragment, router-mediated reassembly, CPU cost, DF-bit black-holing, one lost fragment discarding the chain — is about **IP** fragmentation. DSPP has no IP layer at all; §0 of `docs/AeroSLS-Multi-NIC-Plan-v0.1.md` establishes that it is pure L2 with zero references to IP anywhere in `net/dspp.c`. So:

- A fragment carries the same 121–181 byte DSPP header it already carried, not an added IP header.
- There are no routers. It is one broadcast segment.
- Reassembly happens in this kernel, where it can be tested.
- Nothing sets a DF bit, so nothing black-holes.

And the decisive fact: **half the work already existed.** `DSPP_CTX_CHUNK_BYTES` was a single constant. Context migration already carried `chunk_index`, `total_chunks` and `chunk_bytes`, with a reassembly buffer, a presence bitmap, duplicate rejection, out-of-order tolerance and short-final-chunk validation — 78 tests behind it. The chunk size had simply been set to 4096 to match a memory page, for no reason the protocol required.

The case *for* jumbo also weakened on inspection. It is not a flag flip: `DSPPMigratePagePacket` was 4273 bytes, so 4096-byte RX buffers do not fit it either — it needs `BSEX` and 8192, quadrupling RX buffer memory. And the cluster segment is `-netdev socket,mcast=`, which wraps Ethernet frames in UDP; an oversized frame becomes a datagram the **host kernel** IP-fragments. Jumbo would not have eliminated fragmentation, only moved it somewhere unmeasurable, and made the design depend on host behaviour the moment nodes span two machines.

### What changed

| | |
| --- | --- |
| `DSPP_CTX_CHUNK_BYTES` | 4096 → **1024**. One constant. Chunk packet 4217 → 1145 bytes. |
| `DSPP_MIGRATE_FRAG_BYTES` | **new, 1024**. `DSPPMigratePagePacket` carries a slice, not a page: 4273 → 1205 bytes. |
| `DSPPMigrateHeader` | gains `frag_index` in its stream-specific **tail**, so the prefix contract with `DSPPCtxMigrateHeader` is untouched. 177 → 181 bytes. |
| `dspp_migrate_send_page()` | emits `DSPP_MIGRATE_FRAGS_PER_PAGE` frames. The slicing lives here so `stream_migrate_send_partition()` still thinks in whole pages. |
| `stream_migrate_recv_page()` | takes a fragment. Stages into the inflight row with a presence bitmap; writes the page to NVMe only when all fragments are present. |

1024 for both, deliberately: two families slicing at different sizes for no reason is a thing to get wrong later. It divides 4096 exactly, which is what lets `frag_index` alone locate a slice — no per-fragment length field, unlike the context family whose checkpoints are of arbitrary length.

**Staging in RAM rather than read-modify-writing the destination LBA per fragment.** RMW needs no state and tolerates any arrival order, but costs a read and a write per fragment and would leave a genuinely half-written 4 KiB block on disk if a fragment were lost. A page that is either fully written or not written at all is the better failure mode for storage. Disk cost is unchanged: still one write and one verify read per page.

### Compile-time enforcement, because runtime silence was the original bug

`net/dspp.c` now static-asserts that **every** packet family fits `DSPP_MAX_WIRE_PAYLOAD`, plus that the fragment size divides a page exactly. Anything added to a header, or any chunk size raised for throughput, now breaks the build rather than the cluster.

`DSPPFullPagePacket` is deliberately **not** asserted and remains 4132 bytes. Every live sender transmits only its 56-byte prefix; the unused tail exists because `ConsensusMessage` had to live somewhere. If `DSPP_PAGE_*_REQ` is ever implemented it will need fragmenting too, and `dspp_transmit_raw()`'s runtime guard will say so.

### The test-only MTU seam is gone

§9c introduced `dspp_max_wire_payload` as a variable so two tests exercising layers above the link could keep their coverage. With every family fitting, nothing needs to raise it — both `raise_link_limit_*()` helpers are deleted. The variable remains (the runtime guard reads it) but has no writer anywhere, tests included. A seam that stops being needed and stays anyway is how the next person concludes the limit is negotiable.

### Verification

**Full suite 80/80.** `cross_node_migration_host_test.c` **73 checks**, and it is a genuine end-to-end wire test again: Scenario 1 replays real captured fragment frames rather than the hand-rebuilt substitutes §9c needed, and asserts on the length of every frame handed to the driver, not on a struct size.

**6 of 6 mutations caught**, two only after the sweep found them surviving:

- *Duplicate fragments double-counted* survived everything. Nothing checked that a duplicate cannot stand in for a missing fragment — which would write a page with a hole in it.
- *Staging not retired after a page completes* survived even its first purpose-built check. The check asserted "page 1 is absent", which was true either way; the bug's real effect is that the **transfer** retires early, so a legitimately-sent page 1 is later rejected as belonging to an unknown transfer. Asserting that page 1 still lands is what catches it.

That second one is the sharper lesson: an assertion about something *not* happening often holds for reasons unrelated to the property under test. The useful assertion was that the system still works, not that the damage is absent.

### Two coupled constants this shook loose

- `tests/simi_ctx_migrate_host_test.c` and `tests/workload_ctx_host_test.c` both had `#define MAX_FRAMES 64`. A checkpoint is ~66 KiB, so quartering the chunk size pushed the count past 64, `e1000_transmit()` silently stopped recording, and reassembly could never complete — presenting as four failures about a context that "did not arrive", nothing to do with the receive path. Both are now derived from `SIMI_MAX_FRAMES`/`SIMI_MEM_SIZE`/`DSPP_CTX_CHUNK_BYTES`, the same discipline the real `chunk_present[]` in `kernel/simi_ctx_migrate.c` already used.
- An ACK assertion indexed `acks_before + 2` to mean "the second page's ACK". With four frames per page, index 2 is page 0's second fragment. Now derived from the frame count.

### Still open

`partition migrate` can now move data across nodes on a standard segment. What remains from §9b is unchanged: the elected leader does no log replication, and `update_page_table_permissions_globally()` and its per-partition sibling are still stubs in `kernel/stubs.c`, so the split-brain write-strip remains bookkeeping rather than enforcement.

Retransmission is addressed in §9e.

## 9e. Retransmission, and the retirement bug it exposed

§9d made frames deliverable. It did not make delivery *reliable*: the sender still handed frames to the NIC and moved on, so a single dropped fragment left a page unwritten.

### The bug that made this urgent rather than nice-to-have

`stream_migrate_send_partition()` retired the source slot **unconditionally**, immediately after the last frame was queued. Combine that with §9c's silently-dropped page frames and a migration *deleted the original and delivered nothing* — the worst available outcome, with no error anywhere.

So retransmission without changing that is pointless: retrying a page and then deleting the source regardless is only a slower way to lose data. Retirement is now conditional on every page of the stream being acknowledged. Anything else leaves the source intact and says why.

### How a synchronous sender can wait at all

This looked impossible at first: `partition_migrate()` is synchronous and runs on the BSP, so a sender that blocks for an ACK appears to block the thread that would deliver it.

It does not, and the reason is that **RX is driven by the timer ISR, not the HTTP sweep**. `kernel/timer.c`'s handler calls `net_poll_tick()` → `e1000_poll_rx()` → `net_rx_dispatch()` → `dspp_rx_dispatch()`. So a spin with interrupts enabled *is* progress — the same arrangement `kernel_sleep_ticks()` already depends on. Establishing that before designing anything is what kept this simple; had RX been sweep-driven, the whole thing would have needed an asynchronous retry queue.

### What was built

| | |
| --- | --- |
| ACKs identify a fragment | `dspp_migrate_send_ack()` set `page_index` but never `frag_index`, so a sender could not tell *which* slice landed. One field. |
| Sender-side ACK record | `BEGIN_ACK`/`PAGE_ACK` were received and discarded (dspp.h said so, honestly). Now recorded in a single `volatile` record — written by the ISR, read by the spinning BSP. |
| Per-fragment retransmit | `dspp_migrate_send_frag()` resends one slice. Resending a whole page on any loss would multiply traffic by the fragment count on exactly the link already dropping frames. |
| Budget | `DSPP_MIGRATE_ACK_TIMEOUT_TICKS` 25 (250 ms) × `DSPP_MIGRATE_MAX_ATTEMPTS` 4 — about a second per page before the transfer is declared failed. |
| Conditional retirement | The source is retired only when every page is confirmed. |

A single record rather than a table, because exactly one page is ever in flight — `partition_migrate()` is synchronous. A table would imply concurrency that does not exist and would need eviction rules to match.

**A refusal is not a loss.** A non-zero ACK status means the destination *cannot* take this page — no free slot, index out of range. Resending an identical request cannot change that answer, so the sender aborts immediately instead of burning the budget. Distinguishing the two is what stops a full-disk destination from triggering a retransmit storm.

**There is deliberately no separate wait on `BEGIN_ACK`.** A `PAGE_ACK` with status 0 already proves the BEGIN landed, because `stream_migrate_recv_page()` refuses any page for a transfer it has no inflight row for. The cost, named rather than hidden: if the BEGIN frame itself is lost, every page is refused and the sender aborts where a BEGIN retry would have succeeded. It fails safely — the source survives — but it fails.

### Two hazards found while building it

**1. `dspp_transmit_raw()` builds into a single `static frame_buf`, and the timer ISR can reach it.** `net_poll_tick()` → … → `dspp_migrate_send_ack()` → `dspp_transmit_raw()`, while the BSP is midway through building a frame. Latent for as long as senders were fire-and-forget — nothing spent measurable time in that function. Waiting for ACKs with interrupts enabled is precisely the window that surfaces it. Now guarded, dropping the nested frame and counting it in `dspp_tx_reentrant_dropped`: the nested caller is always an ACK, and a lost ACK is what retransmission is *for*, whereas a corrupted in-progress frame is not recoverable. `e1000_poll_rx()` guards its own re-entrancy the same way.

**2. Waiting on `kernel_tick_counter` hangs the node if the clock stops.** Interrupts disabled by a caller, the LAPIC timer not yet calibrated, a fault in the handler — and `while (tick < deadline)` never terminates. This kernel has been bitten by exactly that shape before: `boot_application_processors()` spun unbounded on an AP that never came up and hung every `-smp 1` boot.

A plain iteration cap cannot serve, because any value short enough to be useful in a host test is far shorter than 25 real ticks and would fire first on real hardware, silently converting the timeout into "spin N times". So the second bound is on the clock being **stalled**: if `DSPP_MIGRATE_STALL_SPINS` pass without `kernel_tick_counter` changing *at all*, the clock is not running and no further waiting will help. On real hardware the tick advances every ~10 ms so the counter resets long before it trips — it is a safety net, not part of the timing. In a host test with a static clock it trips at once, which is what makes the timeout path testable without waiting real seconds.

That second one was found by the test hanging, not by review.

### Verification

`cross_node_migration_host_test.c` is now **97 checks**, and its `e1000_transmit()` stub is a *live destination*: it calls `dspp_migrate_note_ack()` on seeing a `PAGE_REQ` — which is exactly what the real timer ISR does — and advances the tick. A record-only stub had become an unfaithful stand-in the moment the sender started waiting: it models a crashed peer, and every migration would correctly abort.

Five loss scenarios: a dropped fragment is retransmitted and the transfer still completes; only the missing fragment is resent (exactly one extra frame, not a page's worth); an unrecoverable transfer leaves the source active with its bytes still on disk; a refusal is not retried; and a lossless send transmits no speculative extras.

**8 of 8 mutations caught, three only after being sharpened:**

- *Keep retrying a refusal* survived — because I had mutated a latency optimisation (the inner-loop check) rather than the guard that does the work (the outer one). My mistake, not a test gap.
- *Accept ACKs for the wrong transfer* and *for the wrong page* both survived every loss scenario. Those scenarios have one transfer sending pages in order, so a mismatched ACK never occurs. Scenario 6 now drives `dspp_migrate_note_ack()` directly with stale and cross-transfer ACKs.
- *Out-of-range `frag_index` not rejected* survived a test that fed **one** bad index — one increment cannot reach the completion threshold, so the check passed with the bounds check removed. Feeding enough to cross the threshold catches it.

That last one is the same lesson as §9d's staging mutation, in a new costume: **a guard is only tested at the multiplicity where its absence changes the outcome.**

Full suite **80/80**; whole-image link 97/97 with the 12 asm/linker-provided undefineds unchanged. Three tests that link `kernel/stream.c` without `net/dspp.c` needed retransmission stubs — each documented as faithfully "always acknowledged", with the reason stated: they have no wire, so modelling a silent peer would turn them into accidental tests of the give-up path.

### Still open

No BEGIN retry (above). No reordering tolerance beyond what the fragment bitmap gives. And the budget is fixed rather than adaptive — a link with real latency would want the timeout derived from observed round-trip times rather than a constant, which is a measurement problem this has no instrumentation for yet.

## 9f. Split brain: a broadcast vote counted by everyone

§9c fixed vote *counting* and the cluster stopped reporting CANDIDATE forever. It reported FOLLOWER instead — and the term went from **48 to 553 between two consecutive `cluster status` calls**. Hundreds of elections a minute.

Node 1 was FOLLOWER, so it was not campaigning; it was *adopting* terms from others. Something else was campaigning continuously.

### Bug 1: a vote reply named no candidate

DSPP is pure L2 broadcast — there is no node-to-MAC table, so every frame reaches every node (`dspp_transmit_raw()`). A `VOTE_REPLY` therefore arrives at **every** candidate, not just the one it answers.

`reply_msg->candidate_id` was never assigned. Worse, `reply` was a stack `struct DSPPFullPagePacket` that was never zeroed, so the field carried whatever bytes happened to be on the stack — and `CONSENSUS_WIRE_LEN` puts the first 20 bytes of `payload_4kb` on the wire, making this an information leak as well as a correctness bug.

The reply handler then counted **any** granted reply at its own term:

```c
if (msg->term == local_cluster_state.current_term && msg->vote_granted) {
    local_cluster_state.accumulated_votes++;
```

So two nodes campaigning at the same term both counted the same three grants, both reached quorum, and **both became LEADER**. On a cluster whose entire purpose is preventing that.

The per-partition lease had the same bug with an extra twist: it *did* set `candidate_id`, to `local_cluster_state.node_id` — the **voter's** own id. Not merely useless to the receiver but actively wrong in the field the handler needs. And that mechanism gates writes, so two nodes could each hold one partition's write lease and each believe `dspp_page_write_allowed()` was true for it.

### Bug 2: `>=` let two leaders depose each other

```c
if (packet->header.transaction_id >= local_cluster_state.current_term) {
    local_cluster_state.current_term = packet->header.transaction_id;
    local_cluster_state.role = ROLE_FOLLOWER;
```

A LEADER receiving another node's heartbeat at its **own** term demoted itself. With two same-term leaders from Bug 1, they demoted each other on their first beats, leaving nobody leading — so every node timed out, campaigned, and the term ran away. That is the 48 → 553.

Now split three ways, because the three cases genuinely differ:

| Heartbeat term | Action |
| --- | --- |
| lower than ours | ignore entirely — stale |
| **equal** | reset the watchdog; a CANDIDATE yields, a LEADER **keeps** leadership |
| higher | adopt the term and stand down whatever we were |

Resetting the watchdog on an equal term is essential and easy to lose: that is the *normal* case, the leader beating at the term we already hold. A follower that ignored it would time out and depose a healthy leader.

### Verification

`consensus_phase1_host_test.c` is now **169 checks**. Scenarios 34–36 are the new ones, and they needed a shape no previous scenario had: **two simultaneous candidates.** Every earlier scenario has exactly one, so a reply meant for someone else never existed to be miscounted — which is why 148 passing checks did not notice.

**7 of 7 mutations caught**, one only after a second attempt: removing the "is this vote for me" check from the *partition* handler survived Scenarios 34–35, because those cover only the cluster-wide election. Scenario 36 now drives the lease directly — votes granted to node 2 must not give node 1 the write lease, and a lease holder must not relinquish on an equal-term heartbeat.

Five existing tests hand-built `VOTE_REPLY` packets without naming a candidate, which is now something no real voter produces; each was corrected with the reason stated in place. Full suite **80/80**, whole-image link 97/97, 12 asm-provided undefineds unchanged.

### The pattern across §9c–§9f

Four separate bugs stood between a wired heartbeat and a working election, and each was hidden by the one in front of it:

1. The tick had no caller — nothing ran.
2. Vote frames were 4132 bytes and could not cross the link — nothing arrived.
3. The reply carried a stale term — arrivals were discarded.
4. The reply named no candidate — arrivals were counted by everyone.

Each fix revealed the next, and each revealed one presented as a *plausible* state: FOLLOWER at term 0, then CANDIDATE forever, then FOLLOWER with a racing term. None of them printed an error. The general lesson, stated once here rather than four times above: **in a distributed protocol, the failure mode is silence, and every layer that can silently drop a message needs a test that proves the message got through — not a test that proves the sender sent it.**

## 9g. The empty stream: retired on faith

The first real `partition migrate` on a working cluster printed:

```
[STREAM] migrate: partition 1's stream 'report.pdf' (slot 0, 0 page(s))
         CONFIRMED received by node 2 -- every page acknowledged.
```

**Zero pages.** "Every page acknowledged" is vacuously true over none, and it reads like a successful data transfer.

`stream_confirmed` was initialised to `1` and the page loop ran `frames_used` times, so a stream with no pages skipped the loop entirely and fell straight through to `stream_retire_slot()`. The source was deleted having confirmed **nothing whatsoever** — precisely the fire-and-forget deletion §9e was written to prevent, surviving in the empty case.

§9e had also argued, in `dspp.h`, that no separate `BEGIN_ACK` wait was needed because a `PAGE_ACK` proves the BEGIN landed. That holds only when there is at least one page. The reasoning was correct and the scope of its validity was not checked.

**Fixed by waiting on the BEGIN unconditionally.** One round trip per *stream*, not per page. It removes the special case, and closes the other gap the old note named: a lost BEGIN is now retransmitted rather than causing every subsequent page to be refused as belonging to an unknown transfer.

Two log lines also overclaimed and now don't. An empty stream says so explicitly instead of borrowing the language of a page transfer, and `partition.c`'s summary says "sent and confirmed by the destination" rather than "byte-verified" — zero bytes were verified.

`cross_node_migration_host_test.c` is **107 checks**: an empty stream is retired only once the BEGIN is acknowledged; an unacknowledged one leaves the source active with the BEGIN retransmitted the full budget; and a BEGIN lost once is retried and the transfer completes, costing one extra frame rather than a restart. 3/3 mutations caught.

### The operator-facing half, which was my error not the kernel's

Four commands handed over in that session did not work: `stream list` (no `stream` command exists in the dispatch at all), `aeroslsctl raw /api/streams` (method is positional and comes first), `--method POST` (not a flag), and an upload body using `data` where the parser reads `hex`.

The uncomfortable part: **`raw GET|POST <path> [--body JSON]` was already documented correctly** in `docs/COMMANDS.md`, and 164 of 165 shell commands were documented. The grammar being written down did not prevent the mistake, because a bullet-list grammar gets skimmed. What was missing was something pasteable.

So `docs/COMMANDS.md` now carries worked `raw` examples with literal methods, a right/wrong pair, the `hex` field with a real 8 KiB invocation, and an explicit statement that no `stream` shell command exists. `tests/commands_doc_check.sh` verifies all of it against `user/shell.c`'s dispatch (both matchers — missing `sh_eq` while writing that check hid 47 commands and made the doc look full of phantoms) and `tools/aeroslsctl`'s argparse definitions.

That checker has a deliberate subtlety worth recording: its first version simply banned the strings `--method` and `stream list`, and failed against the very section written to prevent the mistake. **A counter-example is good documentation.** The check now requires those strings to appear only in a context marked as wrong, rather than forbidding them.

## 9h. `frames_used` did not survive a reboot

A stream written with 8 KiB came back from a restart as:

```
"size": 8192,  "frames": 0
```

Two fields describing the same data, disagreeing. `dir_write_entry()` persisted name, mime, size, lba_base, active, owner_uid and partition_id — **not `frames_used`** — and `dir_read_entry()` ended with `se->frames_used = 0;` unconditionally.

`frames_used` bounds every page loop in `kernel/stream.c`. So a restored stream relocated nothing, migrated nothing, and reported success at each step over zero pages. The bytes stayed on disk at `lba_base`, unreachable and unmovable.

**Why zeroing it looked reasonable.** The adjacent `frames[]` array holds volatile physical RAM addresses which genuinely cannot survive a reboot, so it must be zeroed. `frames_used` sits beside it and was zeroed along with it — but it counts *disk pages* within `lba_base`'s range (`stream_migrate_send_partition()`'s `src_lba + p * 8`), which is durable data.

Persisted at offset 149 of a 448-byte entry with 149 bytes in use. No `DIR_VERSION` bump: the entry is `memset` to zero before writing, so an old snapshot reads 0 there — exactly its existing behaviour — and no existing snapshot is invalidated.

**Plus a repair, announced rather than silent.** An old snapshot carries a real byte count and no page count, so `frames_used` is derived as `ceil(size / 4096)` — pages fill contiguously from `lba_base`, making this exact recovery rather than a guess. It logs when it fires, because a field changing during restore is precisely the thing that should be visible in a boot log.

### The test the suite did not have, and the trap in writing it

Every existing scenario built `stream_store[]` directly in memory via a helper. **No test ever round-tripped the directory through the real writer and the real reader**, so the only code path where a missing field could surface was never taken. `stream_persist_directory()` is no longer `static`, declared in `stream.h` with that reason stated, specifically so the round trip is testable — a test that reimplemented the writer would have reproduced the same omission and passed.

Three mutations survived the first version of that test, and two are instructive:

- **The repair masked the bug it recovers from.** With `frames_used` left out of the snapshot, the reader gets 0, the repair derives 2 from `size`, and `frames_used == 2` passes either way. The fix is to assert on the persisted *bytes* — 4 bytes at offset 149 of the directory page — which is true only if the writer really wrote them.
- **8192 divides exactly by 4096**, so the round-trip case could not tell rounding up from truncating. A separate 8193-byte case makes it three pages; truncation would report two and leave the tail of every non-page-multiple stream unreachable, which is most of them.

The third mutation — removing the `size > 0` guard on the repair — survived because it is semantically equivalent: `ceil(0/4096)` is 0, so the guard is defensive rather than load-bearing. Recorded as an equivalent mutant rather than chased with a contrived test.

25 checks in `stream_gather_flush_host_test.c`, 3/3 non-equivalent mutations caught. Full suite 80/80, link 97/97.

### Operator visibility, added in the same pass

Partition ownership had no read surface at all: `partition list` printed id and name, `/api/partitions` printed quotas, `/api/nodes` printed a count rather than a mapping. Since `partition_migrate()` refuses a destination that already owns the partition, and only the owner holds data to send, the one field governing whether a migration can proceed was discoverable **only by attempting one and reading the error**. Three separate debugging cycles went to that.

`partition list` and `/api/partitions` now report `owner_node`, and `aeroslsctl partitions` places it second rather than last. `docs/COMMANDS.md` states the two non-guessable rules next to `partition migrate`: a stream's partition is stamped at creation and `partition assign` is not retroactive, so the order `create → assign → create the stream` is mandatory.

## 9i. Migration lost a stream — acknowledging durability not yet achieved

After §9h, a reboot produced this:

```
node 1: {"streams": []}
node 3: {"streams": []}
```

The stream was gone from both nodes. Not corrupted, not stranded — **destroyed by a successful migration**, with every log line reporting OK.

### The ordering that was backwards

`stream_migrate_recv_begin()` allocated a slot, stamped its metadata, set `active = 1`, and returned. The only persist on the receive side lived in `stream_migrate_recv_page()`, and only on transfer *completion* — so a stream with `frames_used == 0` was never written to disk at all.

The sender, meanwhile, treats the `BEGIN_ACK` as permission to retire its source, and persists that retirement. So across a reboot:

| | |
| --- | --- |
| source node | slot retired, **durably** |
| destination node | slot existed in RAM only, **gone** |

`dspp_migrate_rx()` sends the ACK *after* `stream_migrate_recv_begin()` returns, so persisting inside that function puts the write before the acknowledgement. One line, and the rule it encodes is the general one: **never acknowledge durability you have not yet achieved.**

A failed write now refuses the slot rather than accepting it unpersisted — a refusal makes the sender keep its copy, which is the safe direction to fail.

### Why the test suite could not see it

The receive path was well covered: `cross_node_migration_host_test.c` proved node B's slot had the right name, mime, owner, partition and byte-for-byte page content. Every one of those assertions read `stream_store[]` — **the in-RAM slot, which was always correct.** Nothing asked whether the slot was on disk.

The new check reads the directory page from the fake NVMe instead, which is true only if `recv_begin` wrote it. 111 checks, and the mutation removing the persist is caught.

### This closes a chain of five

§9c through §9i were one fault repeated at five layers, each hidden behind the one in front:

| | what looked fine |
| --- | --- |
| §9b | the tick had no caller — nothing ran |
| §9c | vote frames were 4132 bytes — nothing arrived |
| §9c | replies carried a stale term — arrivals were discarded |
| §9f | replies named no candidate — arrivals were counted by everyone |
| §9g | an empty stream was retired with no confirmation |
| §9h | `frames_used` was dropped from the snapshot |
| §9i | the received slot was never persisted |

Every single one presented as success. None printed an error. Four of the seven were found by *running the thing on real hardware*, not by review and not by the host suite — and in each case the suite had coverage that looked thorough while testing the wrong side of the boundary: the sender rather than the wire, the struct rather than the disk, the grammar rather than the paste.

The reusable rule, since it has now cost seven bugs: **in a distributed or persistent system, assert on the far side of the boundary you are crossing.** Not that the frame was sent — that it arrived. Not that the field was set — that it was written. Not that the API was called — that the state changed where it needed to.

## 10. Live/hot migration — deferred, not scoped

Named explicitly rather than silently omitted: keeping a partition servicing reads and writes while its pages transfer in the background is a materially different and larger problem than cold migration — it needs the DSPP layer to serve reads from whichever node currently holds a given page mid-transfer, and writes to be either fenced or dual-written during the handoff window, neither of which this roadmap's Phase 4 lease model (a single "which node may write" flag) is designed to support mid-transfer. This is the same category of decision LPAR Phase 15 made about nested partitions — not "no concrete plan yet, revisit later," but "the mechanism this roadmap builds (a binary per-partition lease, cold-swapped) doesn't extend to this use case without a different design," worth naming now so Phase 6's lease model isn't mistaken for a stepping stone to something it structurally isn't.

## 11. Suggested execution order

Phase 1 has no dependency and should land first — nothing else can reference a real node identity before it exists. Phase 3 has no real dependency on Phases 1/2 either and can be built in parallel with them — it's a `frame_pool.c`-local fix, not distributed-systems work, and the earlier it lands the less migration testing (Phase 6) has to work around a known-leaky reclamation path. Phases 2, 4, and 5 each depend on Phase 1 and should follow it in roughly that order, since 4 (leases) and 5 (DSPP routing) both want Phase 2's ownership table to already exist. Phase 6 is the integration point and comes last, gated on all of 2 through 5. Phase 7 (real cross-node data movement, §9a) closes the RX-dispatcher gap Phases 5 and 6 both named, and is done. Live/hot migration (§10) is explicitly not scheduled.
