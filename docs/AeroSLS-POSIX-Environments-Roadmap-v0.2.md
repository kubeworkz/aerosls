# AeroSLS POSIX Environments Roadmap v0.2 — environment persistence, in-environment networking, and placement

**Status:** Draft for review.
**Scope:** the three items `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §12 deferred to v0.2, in the order that section fixed: **(1) environment checkpoint/restore and durable storage**, **(2) networking inside environments**, **(3) cluster placement, migration and failover**. v0.1 §12 is explicit that they are one plan rather than three because each depends on the one before it.
**Resolves:** the prerequisites v0.1 §10 and §13 named — environment checkpoint/restore is E7's gate, durable storage is what makes an environment worth migrating, and the cluster items are what have to wait for both.
**Does not do:** design the E7 shim (that is `AeroSLS-Linux-ABI-Shim-Design-v0.1.md`), scope threads, signals or dynamic linking (§9 names their destinations), or build anything. This is the plan; findings land as addenda in place, the way v0.1's did.

---

## 0. Where this picks up

v0.1 §12 wrote three items and its own trigger for acting on them: *"When to write v0.2: after E4 lands and before E7 starts."* Both halves of that trigger have fired.

- **M1 is complete.** E1–E6 exist, each with a guard that runs in CI (v0.1 §9.1: *"M1 is complete."*). On-demand POSIX environments run inside partitions, beside the ring-0 control plane, each with its own ramdisk, its own terminal, and a frame budget charged to its partition.
- **E7 is in progress and has stopped exactly where v0.2 predicted.** Its design pass is done, its census is measured, its candidate is designated — and its *gate* is "a static binary doing the first user's actual work, running natively, **surviving a checkpoint/restore cycle**." §10 makes the second half a hard prerequisite, measured rather than assumed: `checkpoint_trigger()` persists sixteen kernel data regions and no process or sidecar state at all. E7 cannot pass its gate before this document's first phase lands.

So this is not a wish list written ahead of the tree. It is the plan for the two things E7 is waiting on plus the cluster work queued behind them.

### 0.1 The IBM i mapping, extended

v0.1 §0.1 mapped PASE onto "a POSIX environment inside a partition, created on demand", and said the naming had misled the project once already (LPAR roadmap §9). The same care applies to v0.2, because "persistence" and "migration" are words that mean something specific in IBM i:

| IBM i | What it is | AeroSLS equivalent | Phase |
|---|---|---|---|
| **IFS + Db2**, and the fact that a PASE program *shares* them with native jobs | Data outlives the program and the program can see the OS's data | Durable, partition-scoped environment storage the OS can also reach | P1b |
| A job that survives a restart, and a system that comes back where it was | Single-level store: there is no "reload the app" step | Environment checkpoint/restore — the same two sidecars, the same memory | P1a |
| PASE programs opening sockets | TCP/IP inside the environment, beside the host's own | A kernel-brokered socket service, quota'd per partition | P2 |
| A workload moving between LPARs; VIOS sharing I/O without nested virtualization | Work moves *across* kernel instances, never by nesting one inside another | Environment migration and failover across nodes — a whole environment leaving one node and resuming on another | P3 |

Two boundaries are stated now so they cannot drift later. **Nesting is still closed** (v0.1 §12, for the same structural reason LPAR roadmap §9 gives). And **P3 moves environments across nodes, not partitions inside a node** — the in-kernel `partition_id` mechanism E1–E6 built is the isolation boundary; migration is the same environment leaving one kernel instance for another, which is LPAR roadmap §9's answer applied to environments rather than partitions.

---

## 1. The ground this document stands on

Everything in this section is a reading of the tree, with the file named. Nothing here is inherited from v0.1's prose; §12 stated the *order* and this section states the *substrate*.

| Claim | Evidence |
|---|---|
| Checkpoints are a coordinated batch of **16 dirty-tracked regions** plus the SIMI tcache, written to fixed NVMe LBAs, with a state-tree snapshot and a versioned history | `kernel/checkpoint_mgr.c` — `checkpoint_trigger()`, the `persist_*()` call list, `ckpt_write_history()`, `ckpt_history[CKPT_MAX_HISTORY]` |
| The region set is a **compile-time enum with a spare bit already carved out** | `kernel/checkpoint_delta.h` — `CKPT_REGION_*` 0–15, `CKPT_REGION_TCACHE` **16**, `CKPT_NUM_REGIONS` **17**; `checkpoint_mgr.c`'s own comment says "adding a region bit is the right long-term shape" |
| The persist layer is **write-on-mutation, checksummed, and crash-ordered**: FNV-1a per region, the header written last, a shadow-compare that skips unchanged frames, and multi-page PRP batches | `kernel/persist.c` — `persist_write_array()`, `p_csum_fold()`, `p_region_*`/`persist_region_commit()`, `p_batch` (`NVME_MAX_PAGES_PER_XFER`) |
| The NVMe LBA map is **a documented budget with header/data splits and safety gaps**, ending at `PERSIST_TLS_LBA` 7680 and bounded by `STREAM_DIR_LBA` 8192 | `kernel/persist.h` — the `PERSIST_*_LBA` block; `checkpoint_mgr.c`'s "Margin before `STREAM_DIR_LBA` (8192)" comment |
| An environment's memory is **three contiguous, partition-charged regions** — ramdisk heap (256 KiB), ramdisk storage (1 MiB), POSIX heap (4 MiB) = **1344 frames** — plus the two sidecars' image/stack frames | `user/init/src/env_manager.rs` — `POSIX_HEAP_FRAMES`/`RD_HEAP_FRAMES`/`RD_STORAGE_FRAMES`, `alloc_region_in()`, `struct Environment` |
| The environment's **identity is already a small bounded record**: partition, index, the three region bases, and four channel endpoints | `user/init/src/env_manager.rs` — `struct Environment` |
| The tenant POSIX profile is **budget + console + its own ramdisk — and nothing else**; the system profile is the one with `network` → `drv.network.0` and a NIC BAR0 | `user/init/src/posix_manifest.rs` — `build_posix_manifest_tenant()` (3 caps) vs `build_posix_manifest()` (7–8 caps), and the tenant test's explicit "must not carry" list |
| A **kernel-brokered service is already how a tenant reaches something outside its partition**: the manifest names `kernel.env.console` / `kernel.env.control`, the kernel mints the far end into pid 0, and registers it against the environment | `kernel/env_console.h` (`env_console_register()`, `ENV_CONSOLE_MAX` 8, `ENV_CONSOLE_BUF` 4096); `kernel/env_service.c` |
| The **peer-scoping rule that makes a kernel service reachable without a cross-partition channel** is E2's: resolution matches within the caller's partition plus an allowlist of kernel-owned names | v0.1 §5 (E2 scope); `kernel/cap.c` sidecar registry (partition-scoped) |
| A **per-partition concurrency quota already exists** for inbound connections, keyed by uid→partition, with syscalls and an HTTP surface | `net/tcp_quota.h` — `tcp_conn_attribute()`, `tcp_partition_set_conn_quota()`, `SYS_SLS_PARTITION_CONN_QUOTA_SET/LIST` (277/278); `net/http.c` `api_partition_connquotas_list()` |
| Storage is already quota'd per partition too, and the rate limiter is a third choke point of the same shape | `kernel/storage_quota.h` — `SYS_SLS_PARTITION_STORAGE_QUOTA_SET/LIST` (275/276), `sys_sls_partition_storage_quota_set()` (`kernel/storage_quota.c`); `net/http_rate_limit.c` |
| **A running computation already crosses nodes and resumes.** Persistent Execution Contexts Phase 3 serialises a live SIMI context, chunks it over DSPP, reassembles it (~360 KiB, one transfer at a time) and loads it on the far side | `kernel/simi_ctx_migrate.h` — `simi_ctx_migrate_send()`, the `SimiCtxMigStatus` family |
| **Bulk data already moves page-by-page with per-page acknowledgement, and refuses to retire the source until every page is confirmed** | `kernel/stream.h`/`stream.c` — `stream_migrate_send_partition()`, `stream_migrate_recv_begin()/_page()`, `stream_count_for_partition()` |
| Streams are **persistent objects with their own NVMe LBA range and a directory that survives reboot** | `kernel/stream.h` — `stream_persist_directory()`, "the newly-received stream survives this node's own reboot" |
| `partition_migrate()` already pauses, steps the write lease down, moves **streams and the SIMI context**, aborts *before* the ownership handoff if the data did not follow, and resumes on the destination | `kernel/partition.c` `partition_migrate()`; `partition_lease_step_down()`; `streams_expected` vs `streams_relocated` abort |
| `partition_migrate()` moves **nothing about a process, a sidecar, or an environment** — the code says so, and scopes the gap to a named dependency | `kernel/partition.c` — "there is no function anywhere in net/dspp.c that moves or retags a catalog object's `partition_id`"; "Scoped to streams only" |
| The **cluster checkpoint transport is 24 KiB and single-slot**, and carries a serialized state tree with no names and no process data | `net/dspp_checkpoint.c` — `CKPT_RECV_MAX` (24 KiB), `dspp_ckpt_send()`, `dspp_ckpt_recv_ready/size/copy()`; `kernel/failover.c` synthesizes `recovered-N` because "the checkpoint carries no names" |
| Failover recovery **adopts ownership only** — a partition row and an owner stamp — and never data | `kernel/failover.c` — `failover_recover_from()`, `partition_set_owner_node()` |
| The environment's filesystem today is **aerofs-lite on a RAM region**: read-only format, 512 B blocks, 11 direct + 1 indirect block per file (max 71 168 bytes), formatted in place on first mount for a tenant | `user/vfs/src/aerofs.rs` — `NDIRECT`/`NINDIRECT`/`MAX_BLOCKS`; `user/init/src/ramdisk_manifest.rs` (tenant storage cap `0x3`, "format the empty region on first mount") |

Two consequences fall straight out of that table, and they shape everything below.

**The small-record band is already full and the bulk band is not.** The environment's *descriptor* is a few hundred bytes and belongs in the `PERSIST_*_LBA` band; the environment's *contents* are 5.5 MiB of regions plus two address spaces and do not. A checkpoint that carries an environment is therefore two mechanisms — a record and a bulk path — and the tree already has a proven bulk path (`stream_migrate_*`) and a proven bulk persistence story (`stream_persist_directory()`).

**"Serialize a running thing and resume it elsewhere" is not new here.** It is what SIMI context migration already does. What P1 adds is that the thing being serialized is a *POSIX environment* — two sidecars, their page tables, their channels and their storage — rather than one interpreter's context.

---

## 2. Design principles

v0.1 §2's three disciplines carry forward unchanged — tenant environments get zero hardware capabilities, the runtime ships with the OS while the programs vary, and every new check ships with its tooth — and v0.2 adds four, each stated so a reviewer can disagree with it:

1. **Persistence is a first-class object, not a side effect.** An environment's state is captured as a *versioned record with a checksum and an identity*, in the shape the kernel's own checkpoint already uses (`CkptHeader`, `#define PERSIST_CKPT_MAGIC`), not as "whatever happened to be in the frames". A partial or stale checkpoint must be *refused*, never half-applied.
2. **The descriptor is the unit of identity; the bytes are the payload.** Restore replays the descriptor through the *existing* environment-create path and then pours the bytes in. Nothing gets a second create path, because a second create path is the drift this project deletes on sight (E6 §9.1's `env_status_str()` deletion is the precedent).
3. **The kernel brokers; the partition bounds.** A tenant never gains a cross-partition channel to reach a service (LPAR Phase 11's IPC boundary stands). It reaches a *kernel* service through E2's kernel-owned-name allowlist — the shape `kernel.env.console` already established — and that service enforces the partition's quotas before it does anything.
4. **Never move ownership without the data.** `partition_migrate()` already learned this the hard way (a live cluster moved ownership while page 0 was refused, leaving the data on one node and the record on another, and the function now counts `streams_expected` before it moves anything). Every phase below that can leave two nodes disagreeing inherits that rule verbatim.

---

## 3. Roadmap

Phase numbers carry a `P` prefix so they cannot be confused with v0.1's `E1–E7`, the LPAR roadmap's 8–15, or the self-hosted Phases 1–6.

| Phase | Deliverable | Depends on | Risk / lift |
|---|---|---|---|
| **P1a** | Environment checkpoint/restore — a versioned descriptor plus the two sidecars' state, captured quiesced and replayed through create | — | **High** — the first thing here that touches process memory |
| **P1b** | Durable environment storage — a writable, reboot-surviving, quota-charged block device in place of the RAM region | P1a (the descriptor must name it) | Medium — the format is the work |
| **P2** | Networking inside environments — a kernel-brokered socket service, quota'd per partition, reached through E2's allowlist | — (independent of P1) | Medium — the surface is large, the mechanism is not new |
| **P3** | Placement, migration and failover — an environment moves as a unit, or is explicitly refused | P1a, P1b, P2 | **High** — the failure mode is two nodes that disagree |

P1a is first and unblocked. P2 is independent of P1 and can run in parallel on a second pair of hands, which matters because P3 waits on all three.

---

## 4. Phase P1a — Environment checkpoint and restore

**Why.** An environment that vanishes on reboot is a terminal session, not a system; and E7's gate is a static binary *surviving a checkpoint/restore cycle* (`AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §8). Today `checkpoint_trigger()` writes sixteen regions of kernel arrays and no process state at all, so a reboot leaves the catalog intact and every environment gone. This is the phase that closes that, and it is deliberately scoped to the **two POSIX sidecars**, not to the Linux task — an environment is checkpointable before E7 exists, which is what lets this phase start now and E7 depend on it rather than the reverse.

**Scope.**

- **A quiesce step.** A checkpoint of a running pair of sidecars is not a copy of memory; it is a copy of memory at a moment they both agree on. `partition_pause()` is the existing mechanism (it excludes the partition from `pick_next_partition()`), and `partition_migrate()` is the precedent for "pause, do the thing, resume". P1a pauses the environment's partition, records whether it was *already* paused, and restores exactly that prior state on every exit path — the same "restores, not resumes" discipline `partition_migrate()` was fixed to apply, because a failed checkpoint must not silently un-pause a tenant.
- **A versioned descriptor.** `struct Environment` (`user/init/src/env_manager.rs`) is already the environment's identity: partition, index, the three region bases, and init's four messenger endpoints. P1a serialises it with a version, a sequence number and a checksum, in the kernel-checkpoint idiom, and adds the fields a restore needs and a create does not: the environment's `env_id`, its console binding, and the boot identity that lets a restored environment re-register under the names it had (`drv.ramdisk.<index>`, `aerosls.posix.<index>`).
- **The captured payload, itemised.** The two sidecars' page tables and the mapped user frames; the environment's three private regions; each sidecar's `struct ProcessDescriptor` essentials (entry, stack, register save area, capability table, channel endpoints); and the console's buffered bytes (`ENV_CONSOLE_BUF`). What is *not* captured is named in §4's "explicitly not in scope".
- **Restore through create.** The descriptor is replayed by calling the environment manager's existing create for the partition and index, then overwriting the freshly-allocated regions with the captured bytes and re-pointing the channels. The property this buys is assertable: **a restored environment's `struct Environment` is field-for-field the one that was checkpointed**, except for the pids, which are not stable across a boot and are not part of identity (the E5 teardown already resolves pids by name for exactly this reason).
- **Where it is stored.** The descriptor goes in the small-record band on the same NVMe region family as the kernel checkpoint, following `persist.h`'s LBA discipline (header frame, entry array, one-frame safety gap, a named `PERSIST_ENV_*` block placed after `PERSIST_TLS_LBA` and before `STREAM_DIR_LBA`). The **payload does not** — 1344 frames alone is 5.25 MiB — so the region contents and page tables are written to a bulk area and the descriptor carries their extents. Two mechanisms, one record, is the honest shape; a single 5 MiB record in a 512-sector band is not.
- **A region bit, as the checkpoint already anticipated.** `CKPT_REGION_TCACHE` (16) exists and `checkpoint_mgr.c`'s own comment names the right long-term shape ("adding a region bit"). P1a adds `CKPT_REGION_ENV` and marks it dirty from the environment manager, so `checkpoint_trigger()` includes environments in a full checkpoint *and* the tcache's "unconditional because a missed mark is worse than a wasted write" reasoning is available as the fallback for the first cut.
- **Refusal over partial application.** A descriptor whose checksum or version does not match is refused with a named reason and leaves **no** environment behind — not a fresh one, not a half-restored one. `failover_recover_from()`'s "no checkpoint held" is the precedent for saying so rather than guessing.

**Explicitly not in scope.**

- **Live checkpoint of a *running* (unpaused) environment.** Quiesce, then capture. A non-disruptive snapshot is a later question and needs a copy-on-write story the frame pool does not have.
- **Checkpointing the Linux task.** It is not E7's `fs_base` or its parked state that is hard (a parked task is already a fully-saved `struct CapParkCtx` plus `park_req`/`park_syscall`, per the ABI design §8) — it is that it does not exist yet. P1a leaves the hook: the descriptor has an "extra tasks" list with zero entries in the first cut, and E7 fills it.
- **Cross-node restore.** That is P3. P1a restores on the node that took the checkpoint, from that node's own NVMe, after a reboot.
- **Environment names as operator-visible strings.** `failover.c` synthesizes `recovered-N` because "the checkpoint carries no names"; P1a's descriptor *does* carry the index, which is the name that matters for the registry. A human name is a later, smaller change.

**Verification plan.** `tests/env_checkpoint_restore_check.sh` (unified boot, modelled on `env_console_attach_check.sh`): create an environment in a fresh partition; run a command that writes a file and sets a shell variable; capture the environment's `(partition, env_id)`, its `posix_pid`'s process name, and its file's bytes; **trigger a checkpoint and reboot the node** against the same storage image; assert the environment is live again in the same partition, its file is byte-identical, and a command run *after* the reboot sees the variable set *before* it. The last assertion is the one that distinguishes a restore from a create, and it is asserted through the environment's own console, so the report cannot be faked by echoing the request.

**Teeth**, in `tests/env_checkpoint_restore_check_smoke.sh`:

- **`P1A_TOOTH=no-restore`** — the restore path is skipped (the create runs, the payload does not). Expected: red on the file-bytes clause and on the variable clause, with the "environment is live" clause **green** — the vacuity control, because an environment that comes back empty must not be described as restored.
- **`P1A_TOOTH=stale-descriptor`** — the descriptor is written with a bumped sequence but not committed. Expected: red on the refusal clause ("refused with no environment behind it"), proving the checksum/version gate bites rather than accepting whatever is on disk.
- **`P1A_TOOTH=leak-unpause`** — a checkpoint that fails after the pause leaves the partition paused. Expected: red on the "paused state restored" clause; every other clause stays green, so the failure is attributable.
- **`P1A_TOOTH=dirty-capture`** — the checkpoint is taken *without* quiescing. Expected: red on a consistency clause (the two sidecars' captured generations disagree). This arm exists because the pause is the load-bearing step a reader is most likely to treat as optional.

### Findings addendum — what has landed, increment by increment

P1a is now partly built. What exists is the **record layer**, chosen as the first
increment because it is the piece everything else in the phase writes into, and
because it is the piece that can be verified without a reboot.

Landed, and verified:

- `kernel/env_ckpt.h` / `kernel/env_ckpt.c` — `struct EnvCkptRecord` (versioned,
  magic'd, sized), the live table `env_ckpt_table[ENV_CKPT_MAX]`, the single
  validation gate `env_ckpt_valid()`, and `env_ckpt_adopt()` — the all-or-nothing
  replay of an on-disk snapshot.
- The NVMe region: `PERSIST_ENV_CKPT_HDR_LBA` 7696 / `..._ENT_LBA` 7704, one
  frame each, registered in `p_region_specs` so the existing torn-write checksum
  scan covers it, and written by `persist_environments()` through the standard
  stage/write/commit protocol.
- `CKPT_REGION_ENV` (17) with `CKPT_NUM_REGIONS` grown to 18, marked dirty from
  the record layer and consumed by `checkpoint_trigger()`'s dirty walk — the
  "adding a region bit" shape `checkpoint_mgr.c`'s tcache comment called for.
- `tests/env_ckpt_host_test.c` — clauses against the real `kernel/env_ckpt.c`,
  including the vacuity-controlled refusal arms (a snapshot good for records
  `0..k-1` and bad at `k` must leave **nothing** behind). 66 today, after the
  third increment's quiesce clauses.
- `tests/env_ckpt_check.sh` — a source-only guard over the *wiring* (region bit
  inside the mask, LBA envelope, distinct magics, protocol steps, both write
  paths, spec span vs. array size, adopt's clear-before-validate ordering, and
  every persist-linking host test naming the new translation unit). Invariants
  N–S are the third increment's.
- `tests/env_ckpt_check_smoke.sh` — 32 teeth, one per invariant, each requiring
  the guard to redden *naming that invariant*, plus the vacuity control.
- `ENV_REGISTER` (opcode 3) in the ENV_* protocol on both sides —
  `struct EnvCkptRegister` and `env_ckpt_register_from()` in
  `kernel/env_ckpt.{h,c}`, the byte codec in `kernel/env_proto.h`, its Rust twin
  in `user/proto/src/env_proto.rs`, `env_service_register_env()` called from
  `env_service_create()`, and the manager's arm and reply builder in
  `user/init/src/env_manager.rs`.
- `tests/env_register_host_test.c` — 69 clauses against the real
  `kernel/env_ckpt.c`: the wire body's byte offsets asserted as literals, the
  encode/parse round trip, the two-halves mapping, and a vacuity-controlled
  refusal arm for each way a registration can be unusable.
- `tests/env_register_pin_check.sh` — the cross-language pin (10 invariants),
  reading BOTH `kernel/env_proto.h` and `user/proto/src/env_proto.rs` and
  refusing to let the opcode, the sizes, the counts, the name length, the field
  offset formulas or the encoders' field order drift apart; teeth in
  `tests/env_register_pin_check_smoke.sh` (24, including the tooth that keeps
  every constant correct while transposing two fields in one encoder).
- `tests/env_service_wait_host_test.c` grew from 17 to 30 clauses: the create
  round trip now has a second leg, and the registration it produces is read back
  out of the REAL `env_ckpt_table[]` — regions, channels, names, and each task's
  entry resolved from the process table.
- `tests/env_ckpt_host_test.c` gained the duplicate-region-kind refusal clause
  that a swap with the kinds intact would otherwise slip past.

Four decisions worth recording, because the rest of the phase inherits them:

1. **The record is not the payload, and says so.** `state_lba`/`state_sectors`
   exist to be filled in by the payload mechanism; they are zero today. This is
   the split §4 called for, made explicit in the type rather than in a comment.
2. **A zero-record snapshot is written, not skipped.** Once the last environment
   is destroyed the writer records a committed empty snapshot. Skipping would
   leave the previous snapshot on disk and resurrect environments an operator
   had destroyed — the stale-versus-absent distinction, on disk this time.
3. **pids are absent by construction.** They are not identity and are not stable
   across a boot, so the record cannot carry them; §4's "field-for-field equal
   except the pids" is a property of the type, not a promise about a function.
4. **The refusal is a value, not a log line.** `env_ckpt_last_refusal()` returns a
   code with two operands, so a guard asserts the reason instead of grepping a
   transcript for wording that will drift.

### Second increment — the registration, and the pin that keeps the two sides in step

This is the increment that made the record layer non-vacuous: something now
actually writes records, for every environment the control plane creates.

A checkpoint record is assembled from two halves that live on opposite sides of
a language boundary. init owns the three frame-pool regions it allocated (their
bases and lengths), the four messenger endpoints it holds to the sidecars, and
the registry names those sidecars were created under. The kernel owns the format
stamps, the console binding, and where it actually *released* each sidecar
(`user_rip = image_vbase + image_entry`, set by `cap_create_sidecar` and never
clobbered — a park saves the resume RIP in `park_ctx`). Neither side alone can
describe an environment, so:

- **`ENV_REGISTER`** (opcode 3) joins the ENV_* protocol. The kernel asks
  `{ env_id, partition }` — the same pair `ENV_DESTROY` carries, for the same
  reason — and init answers with `struct EnvCkptRegister` (kernel/env_ckpt.h),
  which is simultaneously the C struct `env_ckpt_register_from()` consumes and
  the exact bytes user/proto/src/env_proto.rs writes.
- **`env_service_create()` registers.** Bind the console, then round-trip
  ENV_REGISTER, then store. Registration is inside the create rather than beside
  it, so there is no window in which an environment exists, answers, and is
  invisible to a checkpoint — and both callers (the HTTP route and the shell's
  `env create`) inherit it. A refused registration is not a refused create (the
  environment exists), but it is said out loud on the serial transcript.
- **The reply is checked against the request** on `(partition, env_id, index)`.
  `index` is the half of the identity the sidecar names are built from, so a
  registration that disagreed about it would name another environment's
  sidecars — which a restore would then re-register.
- **The tasks' entries are the kernel's**, resolved from the process table by
  the name init sent. A sidecar the kernel cannot place refuses the whole
  registration: an environment left *out* of the checkpoint is a cold start,
  which is honest; an environment half in it is a restore target nobody can
  find.

Four things this increment decided, and the reason each is load-bearing:

1. **The layout is pinned by reading both files, not by two parallel tests.**
   `tests/env_register_pin_check.sh` compares kernel/env_proto.h with
   user/proto/src/env_proto.rs: the opcode, both body sizes, the region/channel/
   task counts, the name length, every field offset's *formula* after prefix
   normalization, and the order the two encoders lay the fields down in. Two
   hand-written tests asserting the same constants would both be right while the
   fields disagreed; only a check that reads both files can see that. Its 24
   teeth include the interesting case: swapping two write lines in one encoder
   leaves every constant and every offset formula correct and still transposes
   `index` and `env_id` in the body (tooth E).
2. **`console_id` is the console's index within the partition, or 0.** The E6
   console is addressed by `(partition, index)` for attribution and
   `(partition, env_id)` by the control plane; the index is the half that
   survives a reboot. 0 means the environment had no console to bind — one
   nobody can reach, and therefore one nobody can checkpoint the output of
   either.
3. **`sequence` is stamped from `checkpoint_last_sequence()`** at registration
   time: the epoch the environment entered the checkpoint in. The alternative —
   leaving the field 0 until the payload layer needs it — would be a field with
   no writer, which is exactly the vacuity this increment removes.
4. **The region kinds are carried on the wire, and the gate now requires each of
   the three exactly once.** init's `struct Environment` declares its regions
   `(rd_heap, rd_storage, px_heap)`; the record's order is
   `(POSIX heap, ramdisk heap, ramdisk storage)`. Sending the kind with the base
   turns that reordering from a silent mis-filing into
   `ENV_CKPT_REFUSE_BAD_REGIONS` — the swap is only detectable because the names
   are on the wire, and not checking them would make the names decoration.

Stacks are the one thing left out on purpose. `stack_bottom`/`stack_top` stay 0:
`cap_create_sidecar` maps the sidecar's user stack but does not record its bounds
on the process descriptor (unlike `process_create`), so there is no honest value
to copy today, and the restore layer — which will map the stack it just created —
owns it. Inventing an address would put a wrong number in a record whose entire
purpose is to be trusted.

What this increment deliberately does **not** do, and what each still needs:

- **The quiesce step.** Built in the third increment below (it was still open
  when this section was written): `ENV_CKPT_FLAG_QUIESCED` is a field with a
  writer now, and a pause an operator had set is restored as a pause.
- **The payload.** The sidecars' page tables, mapped frames, register save areas
  and the console's buffered bytes are not captured; 1344 frames plus page tables
  is a bulk area this phase has not placed yet.
- **Restore-through-create.** Built in the fourth increment below, at the
  DESCRIPTOR level: a boot's adopted records are replayed through the
  environment manager's own create path, and what comes back is checked against
  what was written down. What is still missing is the payload, so the
  environment that comes back is empty.
- **The verification plan above.** Written in the fourth increment below —
  `tests/env_checkpoint_restore_check.sh` with the four `P1A_TOOTH` teeth in its
  smoke — and its **boot arm** landed in the fifth: the guard now boots, creates
  an environment, checkpoints, reboots against the same NVMe image and replays
  it, and `P1A_TOOTH=no-restore` is run both as a recorded-artifact mutant and as
  a live boot. What is still owed is the **payload** clauses — the file bytes and
  the shell variable the plan's own text names — and the guard says so in its
  header rather than passing silently: with no payload, "the file's bytes
  survived" is not a fact this tree can measure yet.

The honest summary: an environment's *identity, placement and structure* — the
two sidecars by name, the three kind-labelled regions, the four messenger ends,
the console binding, and where each sidecar was loaded — is now written into a
persisted record by the very call that creates the environment, and is read back
through a gate that refuses anything it cannot vouch for. What an environment
*contains* — its files, its memory, the bytes its console has buffered — cannot
survive a reboot yet, and neither can the record be turned back into a running
environment. Both of those are the payload and restore increments; P1a is still
not finished, and this addendum is still not a claim that it is.

### Third increment — the quiesce, and the pause that comes back

This is the increment that made `ENV_CKPT_FLAG_QUIESCED` mean something. Before
it, the flag existed in the record and nothing wrote it, and a partition an
operator had frozen was not described anywhere the restore could see. Two
halves, one per side of the capture:

**Freeze before writing down.** `persist_environments()` now calls
`env_ckpt_quiesce_for_capture()` before it stages its header and
`env_ckpt_release_capture()` after it commits. The call sits *after* the
writer's two bail-out gates (a deferred capture and an I/O-less capture write
nothing, so freezing a tenant for them would be a pause with no snapshot to show
for it), and the body between the two calls is deliberately straight-line: the
quiesce and its release are one interval with no `return` in it, because a
capture that bailed out there would leave a tenant frozen with nothing on disk
to show for it. That is exactly the failure the plan's `P1A_TOOTH=leak-unpause`
names, and it is now a *source-level* invariant: clause O of
`tests/env_ckpt_check.sh` reads the function and refuses an early return between
the quiesce and its release.

**The three rules.** `env_ckpt_quiesce_for_capture()` walks the live records and
applies one rule to each, and each rule exists because the obvious alternative is
wrong:

1. **A partition an operator had already paused stays paused.** The capture
   resumes exactly what it paused (`paused_by_capture[]` is the ledger, the same
   "restore, don't resume" discipline `partition_migrate()` was fixed to apply),
   and records the prior state as `ENV_CKPT_FLAG_PARTITION_PAUSED` so a restore
   can put it back. The subtlety that cost a red clause in the host test: a
   second record in a partition *this capture* froze must not be read as "already
   paused" — so the ledger is consulted *first*, before `partition_is_paused()`.
   Otherwise the second environment's record would carry `PARTITION_PAUSED` and
   come back frozen on a partition that only the capture had ever paused.
2. **`PARTITION_SYSTEM` is never frozen.** Pausing it would freeze the machinery
   performing the capture, and E3's boot-spawned tenant environments live there.
   Such a record is captured with `QUIESCED` **clear** and counted separately in
   `n_unfrozen` — the honest "this one was not frozen" is visible per record
   rather than assumed. (The host test asserts that the stub *can* pause
   partition 0, so the refusal is observably a decision rather than an inability.)
3. **A record naming a partition that no longer exists is dropped, not
   captured.** Nothing could restore it, so persisting it is the
   stale-versus-absent sin this phase exists to police; it is counted in
   `n_dropped` so the event is assertable. This is init's
   `forget_dead_environments` call made on the kernel's side of the same problem.

**Put the pause back.** `env_ckpt_apply_restored_pauses()` runs in the restore
block immediately after `env_ckpt_after_restore()` — after, because the live set
has to be settled before the records are acted on, and because `partition_table[]`
is itself restored in an earlier block, so "does this partition exist?" is
meaningful by then. Every adopted record carrying `PARTITION_PAUSED` re-pauses its
partition, once per partition however many records name it, and a record in
`PARTITION_SYSTEM` is deliberately declined: a boot that comes up with the control
plane's own partition frozen is a worse failure than losing the flag, and the
record still says what was true.

**A destroy drops the record.** `env_service_destroy()` now calls
`env_ckpt_drop_env()` on the same `(partition, env_id)` pair the destroy speaks,
but only for a destroy the manager actually acknowledged — a failed destroy
leaves the environment live, so its record is still the truth about it. Without
this a removed environment came back on the next restore.

Five things this increment decided:

1. **The quiesce is the record layer's, and it is the first thing to need
   `partition.c`.** `kernel/env_ckpt.c` now calls `partition_pause/_resume/
   _is_paused/_exists`, which is a new dependency for a TU that 27 host tests link
   *without* linking `kernel/partition.c`. Rather than grow four stubs in 27
   places (the drift `tests/process_host_stubs.h` was written to stop), the
   primitives live in one new shared header, `tests/partition_host_stubs.h`,
   whose stubs are **weak** so a test that links the real `partition.c` is
   unaffected. Clause S of the wiring guard is the tooth that keeps the next test
   from being added without it — the same shape as clause M for `env_ckpt.c`.
2. **The flags are two facts, not one.** `QUIESCED` means "this capture held the
   partition paused while it wrote"; `PARTITION_PAUSED` means "the operator had it
   paused before the capture ran". A shared bit would make the restore unable to
   tell which partitions to put back into the paused state — and clause P refuses
   a build where the two collapse onto one value.
3. **An unknown flag bit is a refusal, not a skip.** `env_ckpt_valid()` gained
   `ENV_CKPT_REFUSE_BAD_FLAGS` (12). A record whose meaning is partly unknown is
   one the restore cannot vouch for, exactly as an unknown task kind is not
   skipped. It has its own code *and* its own rendering, so the transcript says
   why.
4. **The quiesce walk does not advance its index after a drop.**
   `env_ckpt_drop()` keeps the table dense by shifting the tail down, so the
   record now at index `i` is the next one to examine; a naive `i++` would skip
   it and persist a record whose partition is gone.
5. **The count on the wire is per record, the pause is per partition.** A capture
   of two environments in one partition pauses once, stamps both records, and
   reports `n_quiesced` 2 with a one-entry ledger — and the release thaws the
   partition once. The `CAP`-style "count what you did" discipline, applied here
   so the vacuity control has something to read.

What this increment deliberately does **not** do: the payload and
restore-through-create are still open (the bullets above) — the fourth increment
below builds the second of those and writes the verification — and the
boot-level half of the verification plan, the one that would make "a paused
partition is restored paused across an actual reboot" a measured fact, waits on
the payload. What the quiesce has, today, is the two levels this increment could
give it: the host test drives the real `kernel/env_ckpt.c` against a settable
partition stand-in, and the source guard proves the writer is wired to it on a
single straight-line exit.

### Fourth increment — the replay, and the four teeth

This is the increment that makes a record worth keeping. Before it, a rebooted
node could read `env_ckpt_table[]` back, validate it, and then do nothing with
it: the identity, the sidecar names and the region kinds were all there, and
no environment came back. Now a pass replays them.

Landed, and verified:

- `kernel/env_ckpt.c` — the restore-side bookkeeping: the pending set armed by
  `env_ckpt_after_restore()` (runtime state, deliberately not persisted, the way
  `partition_paused[]` is), the hand-out (`env_ckpt_restore_next()`), the
  settle/refuse pair, the admissibility gate, the identity comparison, and the
  resume/re-pause pair a restored pause needs.
- `env_service_restore_pending()` (`kernel/env_service.c`) — the pass itself:
  per record, step out of a restored pause, create through the EXISTING
  `env_service_create()`, compare what came back with what was written down,
  then settle or refuse; put the pauses back at the end from the pass's ledger.
- `POST /api/env/restore` (`net/http.c`) — the surface that makes the pass
  reachable, answering `pending` / `replayed` / `refused` / `remaining` /
  `resumed` / `repaused` so an operator can tell what the pass did, and what it
  deliberately did not.
- `tests/env_ckpt_host_test.c` — sections 22–25, 105 clauses: the pending set
  and its shift-through-a-drop, the gate and its PARTITION_SYSTEM exemption, the
  identity comparison field by field, and the ledger.
- `tests/env_checkpoint_restore_check.sh` (14 source clauses, then the boot arm
  of the fifth increment below) and `tests/env_checkpoint_restore_check_smoke.sh`
  (30 arms) — the four `P1A_TOOTH` names this document's verification plan
  fixed: `no-restore`, `stale-descriptor`, `leak-unpause`, `dirty-capture`.

Five decisions this increment made:

1. **The replay IS the create, not a second create path.** Every record goes
   through `env_service_create()` — the same `ENV_CREATE` round trip an HTTP
   create makes — and the create's own `ENV_REGISTER` refreshes the record, so
   "restored" means "the same environment, created again, re-registered under
   the identity it had". Clause T1 refuses a driver that speaks the channel
   itself, which is the shape a second path would arrive in.
2. **`env_id` is a runtime handle, not identity.** The manager mints a fresh id
   on every create, so identity is (partition, index), the task names and kinds,
   and the region kinds and their frame counts. Bases, channels, entries, the
   console key and the sequence are what a create legitimately re-decides — and
   the pids are not in the record at all. This is v0.2 §4's "field for field
   except the pids", stated in the terms the descriptor actually has.
3. **A restored pause is stepped out of, then put back.** The snapshot re-pauses
   an operator-frozen partition at boot, and the create path REFUSES to place an
   environment into a paused partition (E4's placement gate) — so a replay that
   did not step out of the pause would fail for a reason that says nothing about
   the environment, and an operator-paused environment could never come back.
   The pass resumes the target, creates, and re-pauses from its own ledger (rule
   1's mirror: it puts back exactly what it resumed), with no `return` between
   the two — the capture's `leak-unpause` contract, mirrored.
4. **The dirty-capture gate lives in the record layer's hand-out.**
   `env_ckpt_restore_next()` refuses a record whose capture never froze it
   (`ENV_CKPT_REFUSE_UNQUIESCED`) and one whose partition is gone
   (`ENV_CKPT_REFUSE_NO_PARTITION`) before handing anything out, so no caller can
   forget to ask. `PARTITION_SYSTEM` is exempt because the capture is documented
   never to freeze it — the exemption is the same decision, carried to the other
   side, not a hole.
5. **A create that came back as a different environment is destroyed.** The
   identity mismatch is refused with its own reason AND the environment the
   create made is ended, so a refused restore leaves no environment behind —
   refusal over partial application, applied to the replay. That is the
   `stale-descriptor` tooth.

What this increment deliberately does **not** do:

- **The payload.** The environment that comes back is EMPTY: its files, its
  memory and its console's buffered bytes are not captured. The pass therefore
  counts a REPLAY, never a RESTORE. The fifth increment below gives the guard
  its boot arm, so "an environment came back, in the same partition, with the
  pause that was recorded" is a measured fact as of now — but the plan's two
  CONTENTS clauses (a file's bytes surviving, a shell variable set before the
  reboot visible after it) are still owed, together with the boot-side mutations
  for the other three tooth names. Until then, "the environment came back" is
  measured and "the environment came back with its contents" is not, and the
  guard's header says exactly that.
- **An automatic replay at boot.** The pass is invoked by the operator (the
  route), not by the kernel at boot: driving an `ENV_CREATE` round trip needs
  init running, and "init is up" is not a boot step the kernel can observe from
  `persist_restore_all()`'s straight-line path. The route is the honest surface
  for that, and clause T1 is what keeps it reachable.
- **Cross-node restore.** That is P3. This pass restores on the node that took
  the checkpoint, from that node's own NVMe.

### Fifth increment — the boot arm: the round trip, measured

Everything in increments one through four was asserted over the SOURCE ("the
replay is wired, it goes through the create, it puts the pause back") or over
`kernel/env_ckpt.c` linked into a host test. None of it had ever been booted.
This increment makes the round trip itself the thing measured, which is what
§4's verification plan asked for in the first place.

**The arm.** `tests/env_checkpoint_restore_check.sh` now has three modes. No
arguments is still the source clauses (unchanged, no build, run on every push).
`--replay DIR` validates a recorded run's artifacts with no QEMU and no build —
which is how the smoke can prove the boot clauses' teeth on a push that has no
ISO. `--live` runs the source clauses and then boots: grub entry 3 (the unified
boot) on a **fresh 10G NVMe image at `-m 1G`** (at 4G the NVMe's 64-bit BAR
lands above 4 GiB, the persistence stack honestly cold-starts, and every run
becomes a no-op-shaped pass — the same pitfall `tcache_roundtrip_check.sh`
documents), waits for the control plane, creates partition `p1arestore`, creates
an environment at index 2, **admin-pauses the partition**, checkpoints
(`POST /api/checkpoint`), records its evidence, `POST /api/node/reboot`s **in
place** (so the same disk file survives, no `-no-reboot`), selects grub entry 3
again, waits for the control plane to come back, re-reads the partition table,
runs `POST /api/env/restore`, and writes the whole set to `P1A_ARTIFACTS`
(default a per-run temp dir, printed at the end). The validator then reads those
artifacts as **B1–B9**: the first boot was cold for the environment region and
reached the NVMe; the create registered a record; the checkpoint wrote the
region (`[PERSIST] Environment checkpoint snapshot written (1 live record(s), 1
frozen, 0 dropped)`); the recorded pause is back **before any replay**; the
reboot adopted and re-paused; the replay went through the manager's own create
and re-registered the same (partition, index); the pass reported
`replayed=1 refused=0 remaining=0`; **the environment is live again** (the
console registry says an environment at this (partition, index) is bound AND the
process table says `aerosls.posix.2` carries that partition — two independent
surfaces, so a listing that merely repeats the request cannot satisfy it); and
the pause the pass stepped out of was put back.

**The tooth, twice.** `P1A_TOOTH=no-restore` runs as a mutant over recorded
artifacts (in the smoke, no QEMU) *and* as a live boot that withholds the
`POST /api/env/restore` call entirely. Both take **B8 and nothing else** red:
B6, B7 and B9 are notes under the tooth, because they are replay claims and
there was no replay. That is the tooth the plan asked for — with no payload the
DESCRIPTOR is the restore, so withholding it takes liveness red; when the payload
lands B8 stays green under it and the contents clauses become the ones that go
red.

**What the first live runs had to be taught.** Each of these presented as a
restore defect and was not one. They are recorded here because the boot arm is
the only thing that could have found any of them:

1. **`make` does not track headers.** `checkpoint_delta.x86.o` predated
   `CKPT_REGION_ENV`, so its `ckpt_mark_all_dirty()` still wrote `(1u << 17) - 1`
   and CLEARED the environment region's bit on every full checkpoint — the
   kernel logged `dirty=0x1ffff`, one bit short, and `persist_environments()`
   never ran. The source-level guards were all green. Fixed in the Makefile by
   naming `kernel/checkpoint_delta.h` as a prerequisite of the five objects that
   consume the region numbering, the same shape as the existing
   `sls-i386-stub-class.h` rule.
2. **`make x86-iso` ships the CHECKED-IN `sidecars.cpio` and never rebuilds
   it.** The ISO therefore carried an init from before `ENV_REGISTER` existed:
   the create succeeded, its registration round trip was answered by a manager
   that had never heard of the opcode, and the kernel reported "reply carries 28
   bytes, short of the 216-byte registration" — which reads as a restore-layer
   bug. Fixed by re-packing the archive (`make selfhost-bootimage`, it is a
   tracked artifact precisely because the default ISO ships it) — and then by
   closing the hole rather than the instance: the archive now carries the
   digest of the `user/` sources it was packed from
   (`tools/sidecar_source_digest.sh`, written by the target that packs), and
   **`make x86-iso` refuses to ship one whose digest does not match the tree**.
   A digest and not an mtime comparison, because a fresh clone writes the index
   in path order and every clean checkout would refuse; the stamp is tracked for
   the same reason the archive is. The live arm also notes a mismatched stamp
   before it boots, for a run that points `P1A_ISO` at an older image anyway.

   The rule has its own guard and teeth — `tests/sidecar_stamp_check.sh`
   (clauses A–N) and `tests/sidecar_stamp_check_smoke.sh` (54 teeth, plus five
   green arms: a stamp and an archive-bytes stamp whose only difference is a
   trailing newline, the e3_envs init the E3 pack overwrites, half an E3
   quartet with no record, and the vacuity control) — because a rule that lives in one recipe can be deleted,
   turned into a warning, or simply never run by a host that ships the committed
   archive, which CI's ISO job and `deploy/` both do. The guard asks the
   question a level earlier, with no Rust toolchain, no ISO and no boot: does
   the archive IN THE INDEX still correspond to the sources IN THE INDEX? That is
   the shape of defect no source-level guard could see — the stale thing is a
   committed artefact, and every other guard in the tree reads sources or the
   linked kernel. The smoke's teeth include turning the refusal into a warning,
   deleting the step, copying the archive before verifying it, replacing the
   archive with different bytes (a repack out of band, sources untouched),
   letting `.gitignore` swallow the stamp, and refreshing one stamp on its own —
   the state the pack-run record exists to catch.

   **The stamp attests the packed BINARIES too, and the lever is the build, not
   a binary hash.** A digest written by the same run that packs cannot tell a
   rebuild from a fallback — hash the binaries and the archive and the new hashes
   agree in both cases — so the honest fix is to make the packer refuse. The
   historical weakness was one line: `selfhost-bootimage`'s cargo build failed
   (the `x86_64-unknown-none` target missing, or a compile error), the target
   printed a warning and used whatever ELFs were on disk, then flattened,
   packed, and stamped them as if this tree's sources had produced them. That is
   the stale-archive failure one layer down and harder to see, because the
   SOURCES match and only the images inside are older. The build is now fatal:
   the target refuses to pack and refuses to write the stamp, so a stamp is only
   ever reached by a run that rebuilt all six sidecars from these sources.
   Clause **G** holds that — every `$(CARGO) build` in the recipe must be able to
   fail the target (no `||` that does not `exit`, no `-` ignore-error prefix)
   and the stamp write must sit after them. (Packing deliberately prebuilt
   images is still possible with `aerosls-bootimage` directly; that archive
   carries no stamp, so `x86-iso` will not ship it.)

   **The stamp is bound to the archive's own BYTES as well**, because the
   source digest answers "was this packed from these sources?" and cannot answer
   "are these the bytes that were packed?" — a `cpio` run by hand over stale
   `.bin` images, or a `cp` of an older `sidecars.cpio` over this one, leaves
   `user/` untouched and the source stamp still matching, so the substituted
   bytes would ship. `sidecars.cpio.sha256` now records the archive's own
   sha256, written next to the source digest by the same packing run and
   verified by `make x86-iso` before it copies the archive. It is still plain
   `sha256sum`, so a host with no cargo can check it — which is what the CI ISO
   job and `deploy/` do, shipping the committed archive and never building
   sidecars. Two tracked stamps, two different questions, both recomputable from
   the index alone: the sources (`sidecars.cpio.digest`) and the bytes
   (`sidecars.cpio.sha256`), each derived per-archive so the E3 image gets its
   own pair. Clause **H** holds that the committed bytes are the stamped ones,
   **I** that only the packing target writes the bytes stamp and only after it
   packs, and **J** that `x86-iso` verifies it before the copy.

   **The two stamps are one pack run's, not two files that can drift apart.**
   Each stamp was verified against its own subject, and that left one state
   invisible: edit a `user/` source and hand-run the source-digest tool over the
   old archive, and the source stamp matched the sources again while the archive
   still carried the previous binaries — the source check green, the bytes
   check green, the incident reached past its own stamp. `selfhost-bootimage`
   now runs `tools/sidecar_stamp_record.sh` once, which computes both halves in
   a single invocation into `sidecars.cpio.stamps`, and the two stamp files are
   PROJECTED out of that record (`sed -n 's/^sources //p'` and `sed -n 's/^bytes
   //p'`) instead of being written by two independent commands. Clause **K**
   recomputes the record, requires the committed one to match, and requires each
   stamp to equal its field; `x86-iso` refuses an archive whose record and
   stamps do not agree before it copies. The record is tracked for the same
   reason the archive is, and is plain coreutils, so a host that never builds
   sidecars still recomputes all three from the index. The smoke's K teeth
   include the one-sided source refresh above, a swapped archive with a
   hand-refreshed bytes stamp, a missing or edited record, the packer computing
   a stamp on its own again, the packer no longer writing the record, and
   `x86-iso` stopping its check.

   **The record NAMES the six packed binaries, and the packer checks them
   against its own output.** The source digest cannot see WHICH binaries are
   inside the archive and the bytes hash covers them without naming them, so the
   record is now v2: six `binary boot/*.bin <sha256>` lines beside the sources
   and bytes fields, read out of the archive by
   `tools/sidecar_archive_entry_digest.sh`. `selfhost-bootimage` also hands the
   record tool the six flattened `.bin` files it just packed (`--verify
   ENTRY=PATH`), and the record is not written at all if an archive entry is not
   that file — so the packer cannot bless images other than the ones the run
   built. Clause **L** then re-extracts each entry from the archive with
   coreutils alone, independent of the instrument that wrote the lines, so a
   record that misstates what is packed is red even if the record tool and the
   entry tool agree with each other. The smoke's L teeth swap an entry out of
   the archive (a repack from stale images), edit a binary line, swap two
   lines, drop a binary from the record, and replace the entry instrument with
   one that answers without reading the archive; the K teeth gain the
   isolating repack (the layout differs, the six binaries do not, the bytes
   stamp hand-refreshed) and the record step that stops handing its binaries
   for verification.

   **The archive must be the tree's own build output, where that output still
   exists.** A record and its two stamps can ALL be rewritten from a stale
   archive so that they agree with each other — the state K and L are blind to,
   because they compare those files to one another. `make x86-iso` now also
   runs `tools/sidecar_build_output_check.sh`: for each of the six entries whose
   flattened `.bin` file is still in the tree, the archive entry must hash to
   that file. The one exception is the `init.bin` the e3_envs pack overwrites:
   an init that IS the E3 archive's entry is a known build, not a stale image,
   and is skipped — and the E3 record that names it is trusted only after a
   live recomputation of `sidecars_e3.cpio` equals it, so a hand-written
   record that merely names the flattened init exempts nothing. Only init can
   differ between the variants, so the five shared entries are compared
   absolutely. Clause **M**
   makes the same comparison in the guard natively (paths read from the
   Makefile, its own `newc_entry_sha` extraction) and requires the recipe to
   keep the call above the copy. On a host with no build outputs — CI's ISO
   job, deploy, a fresh clone — there is nothing to compare against, and M says
   so and holds rather than failing or pretending: the claim cannot be made
   there. The smoke's M teeth move one build output, rewrite the record and
   stamps consistently from a stale archive (the residual state), and delete
   the recipe's call, plus one green arm for the E3 overwrite and two teeth
   for the fabricated and the mismatched E3 record. Clause **N** turns that
   anchoring into a standing rule for the E3 pack's own four files, wherever
   they are present: the E3 record must equal a live recomputation of
   `sidecars_e3.cpio` (v2 shape, all six binaries) and each E3 stamp must be a
   field of it, so a hand-refreshed E3 stamp or the default archive's record
   in the E3 record's place is red even on a tree where no init mismatch
   happens to be there to expose it. The whole quartet is `.gitignored` build
   output, so the clause says "no E3 pack in this tree" and holds when the
   files are absent — an ABORT there would report every CI tree as rot. The
   smoke plants a valid quartet and breaks one half at a time: a sources stamp
   refreshed on its own, a bytes stamp refreshed on its own, the default
   archive's record where the E3 one belongs, and a stamp deleted from under a
   live record; the green arm is an E3 archive whose record is absent.

   None of this is a signature: a hand can still re-derive the record and both
   stamps from a stale archive, and no cargo-free check can tell whether the
   packed binaries were built from the current sources — unless the flattened
   outputs are still in the tree, which is exactly what the build-output check
   uses them for; on a host that has only the committed files, the record's
   claims are re-derived, not certified. What the v2 record adds
   is that the packer's own record can only describe the images it just built,
   and that the committed record cannot describe an archive other than the one
   beside it — the two claims a host that ships the committed archive (CI's ISO
   job, deploy) can check with no toolchain at all.
3. **The pause state had no read surface at all**, and `GET
   /api/partition/{id}/env` puts the partition at the TOP level of the response,
   not on each entry. `GET /api/partitions` now exposes `paused` — the same gap
   and the same fix as the `owner_node`/`lease_role` fields beside it — and the
   guard reads the two halves where the route actually puts them. A hand-written
   fixture had happily carried the invented per-entry field for as long as the
   guard existed, which is the class of mistake only a real boot catches.

The arm's own verdict, from its last green live run: `L1–L10` and `B1–B9`
(9 artifact clauses), `replayed in 1 at index 2, with the recorded pause
restored`; and from the live tooth, `B8` alone, with `B6`/`B7`/`B9` noted.

**Still owed.** The payload, and with it the plan's two contents clauses. The
guard says so in a `note:` on every run rather than letting a green arm be read
as "the environment came back with its contents".

---

## 5. Phase P1b — Durable environment storage

**Why.** Even with P1a, an environment's files live in `rd_storage` — a 1 MiB **MEM region from the frame pool** (`RD_STORAGE_FRAMES` 256, passed as `storage_base`/`storage_len` in the ramdisk manifest). Frames are RAM: they are the thing a checkpoint has to carry, and they are gone the moment nothing restores them. v0.1 §12 named the fix in one sentence — *"storage that survives reboot, such as an SLS-backed aerofs in place of RAM-backed ramdisks"* — and gave the reason it matters beyond convenience: **"an environment that survives reboot is the single-level-store property containers do not have."** That is the IBM i claim, and it is the one this phase makes true.

**Scope.**

- **The block device stops being RAM.** The tenant ramdisk's `storage` cap resolves to a region backed by NVMe rather than by `alloc_region_in(RD_STORAGE_FRAMES, …)`. The tree already has the pattern: a stream is a persistent object with its own NVMe LBA range and a directory that survives reboot (`stream_persist_directory()`), and it is already moved between nodes page-by-page. P1b reuses that shape rather than inventing a second persistence story — the design question to settle in review is whether the environment's storage *is* a stream (a named blob with an LBA range, gaining migration and quota for free) or a dedicated per-environment LBA region with its own small directory.
- **A writable, larger on-disk format.** aerofs-lite is explicitly "the read-only root filesystem format" with 512 B blocks, 11 direct + 1 indirect block per inode, and a **71 168-byte maximum file size** (`NDIRECT` 11, `NINDIRECT` 128, so `MAX_BLOCKS` 139). A durable tenant filesystem cannot ship that as its ceiling. P1b's deliverable is a writable format — by extending aerofs-lite with a second indirect block (or a different on-disk layout with a compatibility rule) — and the *migration* of the format is a real decision, not an implementation detail, because the system rootfs image and every existing tenant ramdisk are formatted in it.
- **Quota-charged, like everything else.** The durable storage is charged to the partition through the existing per-partition storage quota (`kernel/storage_quota.h`), which closes the residue E4's findings recorded on the *frame* side by extending the same discipline to the durable side. A tenant's disk counts against the tenant.
- **The descriptor names it.** P1a's descriptor carries the storage's identity (LBA range or stream id) so a restore reattaches to the *same* store, and a migration (P3) can carry it. This is why P1b depends on P1a rather than the other way round: the descriptor is the thing that has to be able to name durable storage.
- **Ordering with the format change.** The durable region must be mountable after a reboot *and* after a version bump, so the format carries its own version and refuses an unknown one (the superblock already has `AEROFS_VERSION` and an `sb_crc`; a v2 is the honest way to add a writable layout rather than overloading v1's semantics).

**Explicitly not in scope.**

- **Sharing a filesystem between environments.** Partition-scoped, one store per environment, exactly as the ramdisk is today. Sharing is IBM i's IFS and is a much larger design (it needs the object catalog's authority model in front of it, not just a format).
- **Reaching the store from native SLS programs or the integrated DB.** That is the interop question §9 records with a destination; P1b makes the data durable and *reachable by the POSIX sidecar*, and says so plainly rather than implying more.
- **Snapshots of the storage.** A checkpoint (P1a) captures the region; a snapshot API is not in this plan.

**Verification plan.** `tests/env_storage_durable_check.sh`: create an environment, write a file larger than 71 168 bytes (the clause that proves the format moved) and a small one, record a hash of both; **reboot**; assert both are present and byte-identical through the environment's own console; assert the partition's **storage quota usage** moved by the file's size and that a write past the quota is refused with the quota's own error, not a frame-pool exhaustion. The quota clauses are what make this a durability *and* an isolation phase rather than a disk-driver task.

**Teeth**, in `tests/env_storage_durable_check_smoke.sh`:

- **`P1B_TOOTH=ram-backed`** — the storage is allocated from the frame pool as before. Expected: red on the post-reboot byte-identity clause (the file is gone), with the quota clauses green — the control that separates "durable" from "was written".
- **`P1B_TOOTH=unquotaed`** — the durable region is charged to `PARTITION_SYSTEM`. Expected: red on the quota-usage clause; this is E4 Finding 1's exact shape (`+1344` frames on the creator, `0` on the tenant) reappearing on the durable side, so the tooth is a regression test for a defect this project has already run once.
- **`P1B_TOOTH=format-v1-only`** — the 71 200-byte file write. Expected: red on the large-file clause while the small-file clause stays green, which is the tooth that keeps the format change honest.
- **`P1B_TOOTH=bad-format-version`** — a store labelled v3. Expected: refused by name, and red on the refusal clause, rather than mounted as v2.

---

## 6. Phase P2 — Networking inside environments

**Why.** v0.1 §6 put networking inside environments explicitly out of E3's scope, and the tenant profile enforces it: `build_posix_manifest_tenant()` grants **three** caps — budget, console, its own ramdisk — and its own test asserts `network` is absent. That is correct for M1 and it is a ceiling on what an environment can be: PASE's value rests on programs that talk to the network, and the E7 candidate evaluation eliminated the *monitoring* operation precisely because it needed outbound TCP (`AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §5.5.1). P2 lifts the ceiling without giving a tenant authority the profile denies.

**Scope.**

- **A kernel-brokered socket service.** The tenant's manifest gains a `network` Chan cap whose peer is a **kernel service** (`kernel.net.socket`), not `drv.network.0`. The difference is the whole design: `drv.network.0` is a sidecar in some partition, and pointing a tenant at it would cross the LPAR Phase 11 IPC boundary; a kernel service is reached through E2's kernel-owned-name allowlist, which is exactly how `kernel.env.console` and `kernel.env.control` already reach a tenant (`kernel/env_console.h`). The mechanism is *copied*, deliberately, so a reviewer has one shape to check rather than two.
- **NICs stay kernel-owned.** E1's device-ownership decision put the NICs in the kernel (control plane on the management NIC, DSPP on the cluster NIC) and gave `init` no network driver. P2 does not revisit that; the kernel's own stack (`net/ipv4.c`, `net/tcp.c`, `net/udp.c`) is what the brokered service fronts.
- **Every partition's share is bounded, by the mechanisms that already exist.** The brokered service admits a connection only after `tcp_conn_attribute(conn_id, uid)` has attributed it to the caller's partition and the partition's quota admitted it (`net/tcp_quota.h`, `SYS_SLS_PARTITION_CONN_QUOTA_SET/LIST` 277/278, `GET/POST /api/partition/connquota(s)`), and the request family is subject to the existing per-partition rate limiting (`net/http_rate_limit.c`). The *reason* this is a phase and not a knob is the failure mode `tcp_quota.h` documents: a partition whose clients hold many simultaneous slow connections starves neighbours without ever tripping a time-window rate limit. That is the hazard an environment must not reintroduce, and it is already solved one layer down.
- **The quota's default stays 0 = unlimited**, following the module's own BSS-zero-safe convention, so a fresh boot needs no init step and existing behaviour is unchanged until an operator opts a partition in. P2 states the *recommended* default for a tenant partition (non-zero) as an open question (§11) rather than changing it here.
- **The surface, scoped honestly.** TCP client and listener first — the syscalls the E7 census and the eliminated monitoring candidate both name (`socket`, `connect`, `send`/`sendto`, `recv`/`recvfrom`, `shutdown`, `close`). UDP and name resolution are *named* as following, because `net/udp.c`, `net/arp.c`, `net/dhcp.c` and `net/ipv4.c` exist but exposing a resolver's cache and lifecycle to a tenant is its own design. The Linux shim's socket path (excluded from E7 in `AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §9 as "v0.2 item 2's") becomes forwardable to this same service, with no second implementation.
- **The descriptor angle, stated once.** Networking does not survive a checkpoint and should not pretend to: a checkpointed environment's sockets are closed and its listener is re-established on resume, because a connection is a property of a running peer and a half-open socket is not a thing to restore. The descriptor carries the *listener configuration*, not the connections, and P2 says so explicitly so P1a's reviewers are not asked to capture TCP state.

**Explicitly not in scope.**

- **A tenant NIC, a tenant network stack, or a tenant network driver.** The zero-hardware-capability principle is unchanged; the tenant has a socket API, not a device.
- **Raw sockets, packet capture, or ARP-level access.** Those are exactly the hardware-adjacent authorities §2.1 of v0.1 exists to withhold.
- **Cross-partition sockets.** A tenant reaches its own partition's service; `tcp_quota.h` already reasons about the inbound pool only.
- **TLS inside the environment.** The kernel's TLS (mbedTLS, `kernel/tls_server.h`) terminates its own HTTP; a tenant's TLS is the tenant's business and arrives with whatever runtime needs it.

**Verification plan.** `tests/env_net_isolation_check.sh` (unified boot): create environments in **two** partitions; give each a listener; from partition A's environment, connect to A's listener and to B's; assert A reaches A and **not** B; set a small conn quota on A and open more connections than it allows; assert A is refused **and B keeps serving** throughout; assert a partition with no quota set behaves as before (0 = unlimited). The B-keeps-serving clause is the whole point of the phase.

**Teeth**, in `tests/env_net_isolation_check_smoke.sh`:

- **`P2_TOOTH=no-quota`** — the quota check is removed from the brokered admit path. Expected: red on the starvation clause (B is starved by A's flood), with the reachability clauses green — the vacuity control *and* the reason the phase exists.
- **`P2_TOOTH=system-peer`** — the tenant's `network` cap is pointed at `drv.network.0` instead of the kernel service. Expected: red at **channel creation**, refused for crossing partitions, before any socket is opened — so the tooth tests the boundary, not the API.
- **`P2_TOOTH=unattributed`** — connections are admitted without attribution. Expected: red on the attribution clause (a connection with no partition counts against nobody), which is the same "denial looks like absence" family the E6 console guard was built around.
- **`P2_TOOTH=restore-sockets`** — the descriptor is made to carry live connection ids. Expected: the guard's own consistency clause reddens after a checkpoint/restore, which is the tooth that keeps P1a and P2's boundary from blurring.

---

## 7. Phase P3 — Placement, migration and failover

**Why.** The cluster work exists and is real: partitions replicate, the owned set is synced and GC'd (`partition_sync_ownedset()`), a partition holds a write lease (`partition_holds_write_lease()`), `partition_migrate()` moves a partition's streams and its SIMI context with per-page acknowledgement and refuses to hand over ownership when the data did not follow, and `failover_recover_from()` adopts a dead node's partitions. **None of it knows an environment exists.** A partition with an environment that migrates today hands over an owner stamp and a stream set, and leaves the environment — its two sidecars, its memory, its storage, its console — on the source, where it is either orphaned or destroyed by teardown. That is the gap P3 closes, and it is the last one, because it needs P1a's checkpoint as its payload and P1b's durable storage to be worth carrying.

**Scope.**

- **Placement is a gate, not a preference.** v0.1 §7 already fixed placement at the write-lease holder. P3 extends the *refusal* discipline E4 built (a paused target is refused with `CAP_EPERM`, an absent one with `CAP_EINVAL`, and a refused placement allocates **nothing**) to cover the environment's full footprint: 1344 frames of regions, the two sidecars' image and stack frames, and P1b's durable storage extent. An environment that cannot be placed is refused *by name*, before anything is created, which is the property E4 Finding 1's fix established for regions and P3 must not lose for whole environments.
- **Migration is checkpoint, transfer, restore, resume.** The payload is P1a's descriptor plus its extents; the transfer is the existing bulk path — `stream_migrate_send_partition()`'s page-by-page, per-page-acknowledged, source-retired-only-on-confirmation discipline (`kernel/stream.h`) for streams and storage, and the DSPP chunk family for the descriptor. The destination replays the descriptor through the environment-create path (P1a's rule 2), pours in the bytes, and resumes the partition there. The source frees the environment only after the destination confirms — the `streams_expected` vs `streams_relocated` rule, applied to an environment's *whole* state rather than to its streams.
- **The write lease is what makes it safe.** `partition_migrate()` already steps the lease down before the move; P3 keeps that ordering and adds the environment to the same critical section: no syscall from a Linux task, and no file write from the POSIX sidecar, may land between the checkpoint and the resume. The pause is the mechanism (P1a's quiesce), and the lease is the cluster-level guarantee that no *other* node is serving the partition meanwhile.
- **Failover is restore-or-refuse, never half.** `failover_recover_from()` adopts by ownership and comments plainly that "the checkpoint carries no names" — the leader's live broadcast (`failover_live_checkpoint_broadcast()`) is a serialized state tree, and `net/dspp_checkpoint.c`'s receive buffer is **24 KiB**. An environment's 5.5 MiB of regions does not fit that path, and P3 does not pretend it does. Two honest destinations, to be chosen in review: **(a)** extend the leader's broadcast to a bulk transfer of surviving environments (so a dead node's environments come back on the survivor), or **(b)** declare that failover adopts *partitions* and environments are restored only from their own durable storage (P1b) on the survivor, with the gap reported by name. (b) is the smaller, more honest first cut and it is consistent with `failover.c`'s existing "ownership only" posture; (a) is the real PASE-grade answer. The plan is written so either can land without the other.
- **The console and the terminal follow.** E6's per-environment consoles are `(partition, env_id)`-keyed and partition-scoped (`kernel/env_console.h`). On the destination the environment must re-register its console and announce its identity (BIB v3's `[env-id] index=<index> pid=<pid>`) so an attached operator's stream does not silently become someone else's — the E6 guard's cross-partition witness is the test that this held.

**Explicitly not in scope.**

- **Migration of a partition's in-kernel `partition_id` isolation semantics.** The catalog objects keep their `partition_id`; moving the *environment* does not re-tag the tenant's data, and `partition_migrate()`'s own comment names the reason (per-partition page indexing is Storage Isolation Roadmap Phase 1's job, still unbuilt). P3 moves the environment; it does not move the partition's database pages, and it says so rather than implying a completeness it does not have.
- **Live migration with zero downtime.** The environment pauses for the transfer. A sub-second handover is a later question and depends on P1a's non-disruptive snapshot, which is already out of scope.
- **Multi-node placement policy beyond the lease holder.** Choosing *which* node is a scheduler question; P3 moves to a named destination.
- **Migrating `PARTITION_SYSTEM` or its environments.** `partition_migrate()` refuses id 0 outright, and an environment in the system partition has no second node to move to.

**Verification plan.** `tests/env_migration_check.sh`, on the repo's established two-simulated-node technique (the one `simi_ctx_migrate`'s own tests and the stream-migration tests use, described in `kernel/stream.h`'s Phase 7 comment): create an environment with a durable file and a shell variable; migrate its partition to the second node; assert the environment is live **on the destination** with the file byte-identical and the variable present; assert the **source** has no residue — no registry entry under either sidecar name, no frames still charged to the partition, no console still listed; assert the write lease moved; assert an operator attached to the destination sees the environment's own `[env-id]` announcement, not the source's. `tests/env_failover_check.sh` covers the death path: kill the owning node, assert the leader adopts the partition and that the environment is either restored under the chosen destination (a) or reported **by name** as not restored (b) — never silently absent.

**Teeth**, in `tests/env_migration_check_smoke.sh` and `tests/env_failover_check_smoke.sh`:

- **`P3_TOOTH=skip-environment`** — the migration moves the partition and not the environment. Expected: red on the destination-liveness clause and on the source-residue clause; the ownership clauses stay green. This is the arm that proves the phase's whole subject is under test.
- **`P3_TOOTH=ownership-first`** — ownership is handed over before the payload arrives. Expected: red on the "never ownership without data" clause — the partial-transfer defect `partition_migrate()` already paid for once, reproduced deliberately so its regression test is an environment-shaped one.
- **`P3_TOOTH=capacity-unchecked`** — the destination's frame reservation is skipped. Expected: red on the refusal clause (an unplaceable environment is created anyway), with the successful-migration clauses green.
- **`P3_TOOTH=console-unrebound`** — the destination does not re-register the console. Expected: red on the attached-operator clause, and only there — the E6 lesson that an environment nobody can reach is not an environment.
- **`P3_TOOTH=silent-failover`** — failover adopts the partition and reports nothing about the environment. Expected: red on the by-name reporting clause under destination (b), which is the tooth that keeps an honest gap honest.

---

## 8. Capacity for the first cut

v0.1 §11 sized M1 at **four concurrent environments per node** and named the ceilings: `PROC_MAX` 16 (two slots per environment), `SIDECAR_REGISTRY_MAX` 16, `ENV_CONSOLE_MAX` 8. v0.2 adds its own, all of which are new *persistent* consumers rather than new process slots:

| Resource | Per environment | Limit | Note |
|---|---|---|---|
| Descriptor record | a few hundred bytes | small-record band (§4) | one entry per environment, checksummed |
| Bulk payload | ~1344 frames (5.25 MiB) + two address spaces | NVMe bulk area | the reason a descriptor and a payload are two mechanisms |
| Durable storage | 1 MiB in the first cut | partition storage quota | grows with the format (P1b) |
| Sockets | the partition's quota | `tcp_quota.h` (`PARTITION_MAX` 256) | 0 = unlimited until an operator says otherwise |
| Consoles | 1 | `ENV_CONSOLE_MAX` 8 | unchanged from E6 |
| Process slots | 2 | `PROC_MAX` 16 | unchanged from v0.1 §11 |

The honest statement is that **P1–P3 do not raise the four-environment ceiling and are not meant to**. Raising `PROC_MAX` and `SIDECAR_REGISTRY_MAX` "touches many fixed-size arrays" (v0.1 §11) and remains its own audited phase, which P3's capacity gate will *refuse placement against* rather than quietly exceed.

---

## 9. Not in this plan

Each excluded item has a stated destination, so none is silently dropped.

**Closed — not applicable.**

- **Nested environments.** Unchanged from v0.1 §12 and LPAR roadmap §9: PASE has no environment inside an environment, for the same structural reason.

**Belongs to E7, not here.**

- **The Linux personality, its six-argument capture, the initial stack and auxv, `fs_base`/TLS, the errno map and the `brk`/`mmap` budget mapping.** All of it is `AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §3 and §5. P1a leaves the descriptor's "extra tasks" hook for it; nothing else in v0.2 needs E7 to exist.
- **Signal *delivery*.** The E7 census shows the designated candidate calls `rt_sigaction`, `rt_sigprocmask` and `sigaltstack`, while the design records them and never delivers them. That tension is E7's to resolve, and it is recorded here because it is a live gap in a document that is otherwise settled — not because v0.2 solves it.

**Needs its own phase, unscoped.**

- **Threads (`clone`/`CLONE_VM`, `futex`, thread groups, a shared address space).** The structural blocker behind Go, threaded CPython, Node, and most AIX-era daemons; the ABI design (§7) names it as Go's gate and does not scope it. It is a kernel phase the size of the ARM64 port, and it is *not* a v0.2 item because P1a's checkpoint of a single-threaded pair is a prerequisite for doing it safely, not a substitute for it.
- **Dynamic linking.** Refused in v1 (`AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §6) with a named falsifiable trigger. Unchanged.
- **The ARM64 userspace OS.** H2 item #1's work, and the reason E7 targets x86-64 first. Unchanged.
- **A capacity phase** that raises `PROC_MAX`/`SIDECAR_REGISTRY_MAX` (§8).

**A real design question, deliberately not answered here.**

- **Native ↔ POSIX interop.** PASE programs share the IFS and Db2 with native ILE jobs; that sharing is arguably *more* of PASE's value than the binary compatibility itself. Today a Linux task in AeroSLS gets **exactly one capability — the channel to its own POSIX sidecar** (`AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §3.5) — which is superb isolation and makes the environment a sealed box: it cannot see the SLS object catalog, the integrated DB, or a native SIMI program. Whether a *mediated* interop surface (the sidecar proxying specific requests to the catalog or the row store through the kernel's existing capability checks, so the tenant never gains authority the profile denies) is the right answer, and how narrow it should be, is the largest architectural question left in the project. v0.2 does not decide it — it records it, because P1b's durable storage and P2's brokered sockets are both inputs any answer would need, and deciding it before both exist would decide it on guesses.

---

## 10. Suggested execution order

P1a is unblocked and is the thing E7 waits on, so it starts first. P2 has no dependency on P1 and should run **in parallel** — it is the phase most independent of the others, and serialising it behind persistence would idle a second pair of hands. P1b follows P1a because its descriptor field is P1a's. P3 starts only once P1a, P1b and P2 are all in, because every one of them is part of its payload, and a migration that is missing one of the three is exactly the "two nodes that disagree" failure the phase exists to prevent.

| Order | Phase | Gate to leave it |
|---|---|---|
| 1 | P1a — checkpoint/restore | an environment survives a reboot with its file and its shell variable, via its own console |
| 1 (parallel) | P2 — networking | two partitions, one throttled and one not, and the unthrottled one keeps serving |
| 2 | P1b — durable storage | a file larger than 71 168 bytes survives a reboot and is charged to the partition |
| 3 | P3 — placement/migration/failover | an environment moves with its data, its console and its lease, or is refused by name |

E7's own gate — *"a static binary doing the first user's actual work, running natively, surviving a checkpoint/restore cycle"* — becomes reachable at the end of step 1 for the first half and the end of step 2 for the second, and E7's deliverable list (`AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §10.4) is already written to report those halves separately rather than blur them.

---

## 11. Open questions for review

1. **P1b: is an environment's storage *a stream*, or its own LBA region?** A stream gains migration, quota and persistence for free (`stream_persist_directory()`, `stream_migrate_*`) but carries a stream's own size and directory semantics; a dedicated region is simpler to size per environment and duplicates the persistence story. This is the same class of choice v0.1 §14 Q2 left open (ramdisk sidecar vs in-process ramfs), and it should be settled the same way — on measurement, not taste.
2. **P1a: is quiescing the whole partition the right granularity, or should an environment pause alone?** Today `partition_pause()` is the only pause mechanism, and it stops the tenant's other work too. If environments are the unit of checkpointing, a per-environment pause may be the correct primitive — but it is a new scheduling concept and it should be argued for rather than assumed.
3. **P1b: does the durable format replace aerofs-lite or extend it?** v1's superblock is already versioned (`AEROFS_VERSION`, `sb_crc`), so a v2 is the clean path — but every existing tenant ramdisk and the system rootfs are v1 images, and the migration rule (read v1, write v2, refuse v3) needs to be written down before it is implemented.
4. **P2: what is a tenant partition's *recommended* connection quota?** The mechanism defaults to 0 = unlimited for backward compatibility; a fresh tenant that never opts in is exactly the starvation case `tcp_quota.h` documents. Should the environment-create path set a non-zero default as part of its budget, and if so, what number is defensible?
5. **P3: failover destination (a) or (b)?** Bulk-transfer surviving environments during failover (the PASE-grade answer, a much larger lift) or restore them from their own durable storage on the survivor and report the gap by name (the smaller, consistent-with-`failover.c` first cut). The plan is written to admit either; the decision is a product one about what "a cluster that doesn't lose your environments" has to mean in this window.
6. **§9's interop question.** Should a Linux-POSIX task be able to reach the SLS catalog or the integrated DB through a mediated service, and if so, is that service part of the sidecar it already talks to or a second kernel-brokered service in the `kernel.env.console` shape? This is the question that decides whether AeroSLS's POSIX environments are PASE-with-IFS or a sealed compatibility box, and it is deliberately left open here rather than answered by omission.

---

## Appendix A — Evidence index

Everything this document asserts about the tree, with its source.

| Claim | Source |
|---|---|
| The checkpoint batches 16 regions + the SIMI tcache, writes a versioned history, and snapshots a state tree | `kernel/checkpoint_mgr.c` — `checkpoint_trigger()`, `ckpt_write_history()`, `ckpt_read_history()` |
| The checkpoint persists no process, sidecar, address-space, capability, channel or ramdisk state | `kernel/checkpoint_mgr.c`'s `persist_*()` call list (the sixteen regions), and v0.1 §10's measured statement of the same |
| The region enum already has a spare bit, and the comment names the right fixes | `kernel/checkpoint_delta.h` — `CKPT_REGION_TCACHE` 16, `CKPT_NUM_REGIONS` 17; `checkpoint_mgr.c`'s tcache-block comment |
| Persistence is checksummed, crash-ordered and shadow-diffed, with a documented LBA map | `kernel/persist.c` — `persist_write_array()`, `p_csum_fold()`, `p_region_*`; `kernel/persist.h` — `PERSIST_*_LBA` |
| The small-record band ends before the stream directory | `kernel/persist.h` — `PERSIST_TLS_LBA` 7680; `kernel/checkpoint_mgr.c` — "Margin before `STREAM_DIR_LBA` (8192)" |
| An environment is three partition-charged regions plus its sidecars' frames, and its identity is a small record | `user/init/src/env_manager.rs` — `POSIX_HEAP_FRAMES`/`RD_HEAP_FRAMES`/`RD_STORAGE_FRAMES`, `alloc_region_in()`, `struct Environment` |
| Restore must resolve pids by name, because a kill drops the registry entry | `user/init/src/env_manager.rs` — `kill_environment()`'s "resolved BEFORE the kill" comment |
| The tenant profile is exactly three caps and the test asserts what is absent | `user/init/src/posix_manifest.rs` — `build_posix_manifest_tenant()`, `tenant_profile_has_only_budget_console_and_its_own_ramdisk` |
| Tenant ramdisk storage is writable and formatted on first mount; the system's is read-only | `user/init/src/ramdisk_manifest.rs` — `storage_rights` 0x3 vs 0x1 |
| The filesystem is read-only in format, 512 B blocks, 139 blocks per file | `user/vfs/src/aerofs.rs` — `AEROFS_VERSION`, `NDIRECT`, `NINDIRECT`, `MAX_BLOCKS` |
| The kernel brokers a tenant-reachable service through a `kernel.*` name, keyed to `(partition, index)` | `kernel/env_console.h` — `env_console_register()`, `env_console_name_index()`; `kernel/env_service.c` (`kernel.env.control`) |
| Per-partition connection quotas exist, with syscalls and an HTTP surface, and 0 means unlimited | `net/tcp_quota.h` — `tcp_conn_attribute()`, `tcp_partition_set_conn_quota()`, `SYS_SLS_PARTITION_CONN_QUOTA_SET/LIST`; `net/http.c` — `api_partition_connquotas_list()` |
| The starvation failure mode the quota closes is documented, not assumed | `net/tcp_quota.h`'s "Why this is a genuinely different mechanism" block |
| A live computation already serialises, crosses nodes and resumes | `kernel/simi_ctx_migrate.h` — `simi_ctx_migrate_send()`, `SimiCtxMigStatus` |
| Bulk data moves page-by-page with per-page ACKs, and the source is retired only on confirmation | `kernel/stream.h` — `stream_migrate_send_partition()`, `stream_migrate_recv_begin()/_page()`, `stream_count_for_partition()` |
| Streams persist to their own LBA range and survive reboot | `kernel/stream.h` — `stream_persist_directory()`, `stream_migrate_recv_page()`'s "survives this node's own reboot" |
| Migration pauses, steps the lease down, moves streams + SIMI context, and aborts before ownership when the data did not follow | `kernel/partition.c` — `partition_migrate()`; `partition_lease_step_down()`; the `streams_expected`/`streams_relocated` abort |
| Migration moves nothing about a process, sidecar or environment | `kernel/partition.c` — "Scoped to streams only"; the `net/dspp.c` re-tagging comment |
| The owner table is a separate, persisted table so ownership can move without touching identity | `kernel/partition.h` — `struct SLSPartitionOwner`, `partition_owner_table[]`; `kernel/persist.h` — `PERSIST_PART_OWNER_LBA` |
| The cluster checkpoint transport is 24 KiB, single-slot, and carries a nameless state tree | `net/dspp_checkpoint.c` — `CKPT_RECV_MAX`, `dspp_ckpt_send()`, `dspp_ckpt_recv_*()`; `kernel/failover.c` — `fo_recovered_name()` |
| Failover recovery adopts ownership and nothing else, and only the leader recovers | `kernel/failover.c` — `failover_recover_from()`, `failover_tick()`'s leader comment |
| `PARTITION_SYSTEM` never migrates | `kernel/partition.c` — `partition_migrate()`'s id-0 guard; `kernel/failover.c`'s id-0 skip |
| E1's device ownership keeps the NICs in the kernel | v0.1 §4.1 ("the kernel keeps the NICs … `init` does **not** spawn … the network sidecar") |
| v0.2's three items, their order, and the reason they are one plan | `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §12; the trigger "after E4 lands and before E7 starts" |
| The single-level-store claim as the differentiator | `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §12 item 1 |
| E4's two tooth findings (the `PARTITION_SYSTEM` branch unreachable from a boot; region charging landing on the creator) | `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §7.2 |
| E6's console guard, its cross-partition witness and its vacuity control | `AeroSLS-POSIX-Environments-Roadmap-v0.1.md` §9; `tests/env_console_attach_check.sh` |
| The Linux task's capability table holds exactly one entry, and per-partition page indexing is unbuilt | `AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §3.5; `kernel/partition.c`'s Storage Isolation Phase 1 reference |
| E7's gate, its two-halves reporting rule, and the monitoring candidate eliminated by its need for sockets | `AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §8, §10.4, §5.5.1 |
| Signals are accepted and never delivered, and the candidate calls them | `AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §5.6; `tools/linux_syscall_census.txt` |
| Threads/`clone`/`futex` are unscoped and are Go's gate | `AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §7 |

## Appendix B — Named gaps, each with its destination

| Gap | Consequence if unaddressed | Destination |
|---|---|---|
| No environment in the checkpoint region set | every environment dies on reboot; E7 cannot pass its gate (§8) | P1a |
| No quiesce before capture | a checkpoint of two sidecars is internally inconsistent | P1a |
| No descriptor version/checksum/refusal | a stale checkpoint is half-applied instead of refused | P1a |
| Environment storage is a RAM region | files do not survive reboot; the durable-storage claim is false | P1b |
| aerofs-lite is read-only with a 71 168-byte file cap | a durable tenant filesystem would ship an unusable ceiling | P1b |
| Durable storage is not quota-charged to the partition | a tenant's disk is unbounded and unmetered | P1b |
| The tenant profile has no network cap at all | the environment cannot be PASE-for-real; E7's socket path stays unroutable | P2 |
| No kernel-brokered socket service | reaching networking would require a cross-partition channel, violating LPAR Phase 11 | P2 |
| No per-partition bound on a tenant's simultaneous connections | one environment starves its neighbours without tripping the rate limiter | P2 |
| `partition_migrate()` does not move an environment | migration leaves the environment on the source (orphaned or destroyed) | P3 |
| `failover_recover_from()` adopts ownership only | a dead node's environments vanish with no name in the report | P3 |
| The failover broadcast path is 24 KiB | a 5.5 MiB environment cannot ride it; the gap must be stated, not implied | P3 (destination (a) or (b)) |
| No thread groups / `clone` / `futex` | Go, threaded CPython and Node are out | unscoped — §9 |
| No mediated native↔POSIX interop | environments are a sealed box, not PASE-with-IFS | §9, §11 Q6 |
