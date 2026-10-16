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
| The environment's filesystem is **aerofs-lite on a RAM region**: v1 is read-only in format (512 B blocks, 11 direct + 1 indirect, max 71 168 bytes) and v2 is writable (10 direct + 2 indirect, max 136 192 bytes) since P1b's first increment — but the region itself is still RAM, formatted in place on first mount for a tenant | `user/vfs/src/aerofs.rs` — `NDIRECT`/`NINDIRECT`, `MAX_FILE_BYTES_V1`/`_V2`; `user/init/src/ramdisk_manifest.rs` (tenant storage cap `0x3`, "format the empty region on first mount") |

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
  is a bulk area this phase has not placed yet. Built in the sixth increment
  below — the bulk band is placed, and the capture takes all four.
- **Restore-through-create.** Built in the fourth increment below, at the
  DESCRIPTOR level: a boot's adopted records are replayed through the
  environment manager's own create path, and what comes back is checked against
  what was written down. What is still missing is the payload, so the
  environment that comes back is empty — closed by the sixth increment below,
  at the DESCRIPTOR level unchanged: the pour is added *after* the replay, not
  as a second create path.
- **The verification plan above.** Written in the fourth increment below —
  `tests/env_checkpoint_restore_check.sh` with the four `P1A_TOOTH` teeth in its
  smoke — and its **boot arm** landed in the fifth: the guard now boots, creates
  an environment, checkpoints, reboots against the same NVMe image and replays
  it, and `P1A_TOOTH=no-restore` is run both as a recorded-artifact mutant and as
  a live boot. The **payload** clauses the plan's own text names — the file
  bytes and the shell variable — landed in the sixth: B10/B11 are assertions
  fed by the contents phase, not a `note:`.

The honest summary: an environment's *identity, placement and structure* — the
two sidecars by name, the three kind-labelled regions, the four messenger ends,
the console binding, and where each sidecar was loaded — is now written into a
persisted record by the very call that creates the environment, and is read back
through a gate that refuses anything it cannot vouch for. What an environment
*contains* — its files, its memory, the bytes its console has buffered — could
not survive a reboot when this section was written, and neither could the record
be turned back into a running environment. Both of those are the payload and
restore increments: the restore came in the fourth below, the payload in the
sixth, and P1a's contents claim stands or falls on the live run of the arm that
fifth and sixth built together.

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
  pause that was recorded" is a measured fact as of now — and the sixth closes
  what this bullet owes: the plan's two CONTENTS clauses (a file's bytes
  surviving, a shell variable set before the reboot visible after it) are B10
  and B11 today, fed by the contents phase through the environment's own
  console. The pass counts payloads poured (and skips reported by name) beside
  its replays.
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
DESCRIPTOR is the restore, so withholding it takes liveness red. As of the
sixth increment the tooth's target moved with it: it now withholds the PAYLOAD
(`"payload":false`) through the same pass, B8 stays green under it (an empty
environment is still an environment) and B10/B11 are what go red — the split
§4's tooth list anticipated, and the reason the header's wording changed with
the payload rather than around it.

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
   (clauses A–N) and `tests/sidecar_stamp_check_smoke.sh` (55 teeth, plus six
   green arms: a stamp and an archive-bytes stamp whose only difference is a
   trailing newline, the two shared binaries the E3 pack rebuilds — skipped
   once by the guard and once by the recipe's own build-output check — half an
   E3 quartet with no record, and the vacuity control) — because a rule that lives in one recipe can be deleted,
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
   that file. The one exception is a file the e3_envs pack rebuilt: that pack
   rebuilds all six sidecars and flattens them over the shared paths, and a
   rebuild is not byte-identical across toolchains (CI's kernel-guards job
   builds the E3 ISO before it runs the guard), so the default archive may
   legitimately hold binaries the on-disk files no longer match — same sources,
   different compiler. A file that IS an entry of the E3 archive is a known
   build, not a stale image, and is skipped, whatever its path — and the E3
   record that names it is trusted only after a live recomputation of
   `sidecars_e3.cpio` equals it, so a hand-written record that merely names the
   flattened file exempts nothing. Clause **M**
   makes the same comparison in the guard natively (paths read from the
   Makefile, its own `newc_entry_sha` extraction) and requires the recipe to
   keep the call above the copy. On a host with no build outputs — CI's ISO
   job, deploy, a fresh clone — there is nothing to compare against, and M says
   so and holds rather than failing or pretending: the claim cannot be made
   there. The smoke's M teeth move one build output, rewrite the record and
   stamps consistently from a stale archive (the residual state), and delete
   the recipe's call, plus two green arms for the E3 rebuild — the guard's own
   skip, and the recipe's check tool skipping the same two overwritten files,
   which is the gate `make x86-iso` actually refuses on — and three teeth for
   the fabricated record, the mismatched E3 record, and the fabricated record
   at the check tool. Clause **N** turns that
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

The arm's own verdict, from its last green live run (the sixth increment's
first): `L1–L11` and `B1–B11` (11 artifact clauses), `replayed in 1 at index 2,
with the recorded pause restored`, `1 payload(s) poured and 0 skipped`, and the
file and the variable read back through the environment's own console after the
replay; and from the live tooth, `B1–B9` green with B7 naming the withholding
(`payloads=0`, `payload_skips=1`) and exactly `B10`/`B11` red — two failures and
nothing else, which is what makes the tooth attributable.

### Sixth increment — the payload: the two CONTENTS clauses become assertions

Everything before this increment measured the DESCRIPTOR: an environment came
back, in the same partition, with the pause that was recorded — and it came back
**empty**. The plan's two contents clauses (a file's bytes surviving, a shell
variable set before the reboot visible after it) were printed as a `note:` on
every run rather than asserted, because "the file's bytes survived" was not a
fact this tree could measure. This increment makes it one, and the `note:` is
gone: `B10` and `B11` are artifact clauses now, fed by `contents_pre.json` and
`contents_post.json`, both written and read through the environment's **own
console**.

**The payload layer.** `kernel/env_payload.h` / `kernel/env_payload.c` carry it.
Storage is a slot directory (`PERSIST_ENV_PAYLOAD_HDR_LBA` 7744 / `..._ENT_LBA`
7752 in persist.c's small-record band, entries first then header, the same
one-frame-each discipline) over an 8 MiB-slot bulk band
(`ENV_PAYLOAD_DATA_LBA_BASE` 1130496, immediately after P1b's extent band — the
"two mechanisms, one record" shape §4's scope asked for: the descriptor stays
small, the bytes go to a bulk area it names). Magics `SLSPLOA1`/`SLSPBLO1`,
version 1, per-page CRCs with the index's own CRC over them.

**Captured frozen, inside the quiesce interval.** `persist_environments()` calls
`env_payload_capture_all()` after the region commit and before
`env_ckpt_release_capture()` — between the freeze and the release, so what the
payload captured is what the checkpoint froze (clause T7 pins the line order).
What it takes: the sidecars' mapped frames in the image window, each identity
region's bytes (the shell's variable table lives in the POSIX sidecar's heap —
a shell variable is *payload*, not metadata), each parked sidecar's save area
(`CapParkCtx`, the nine qwords, plus the user rip/rsp), and the console's
buffered bytes through a new **non-destructive** `env_console_snapshot()` — the
existing `env_console_read()` drains the buffer, and capture must not consume
what restore has to replay.

**The freeze has to LAND on a parked posture.** The increment's first live run
refused the pour with `EP_REFUSE_RUNNING`: the capture had recorded
`ENV_PAYLOAD_FORM_RUNNING` for `drv.ramdisk.2` although the partition was
frozen. Two facts were true at the same time. A sidecar that parks on a finite
deadline (the ramdisk's 200 ms discovery poll) is mid-wake once a cycle; and a
partition paused in that window cannot run it back into its park —
`cap_park_deadline_tick` still fires from the timer ISR and marks the task
runnable with `waiting_nchans` cleared, while `pick_next_partition` skips paused
partitions, so for the whole freeze the capture reads a posture that is neither
parked nor actually computing. Both halves are closed. The deadline tick now
leaves a frozen partition's task parked — the timeout is delivered on the first
tick after `partition_resume()`, one pause later, to a process whose world was
frozen alongside it — and `persist_environments()` waits for each environment's
sidecars to reach their idle point **before the first pause** (bounded,
yielding, and legal there, because nothing is frozen yet), so the posture the
capture records is the posture the freeze caught. That wait is best-effort by
construction: a sidecar that never parks is captured as the mid-compute posture
it really is, and the restore refuses it by name rather than pouring into it.

**Poured after the repause, straight-line, onto the captured placement.** The
restore pass records what it replayed, waits (bounded, yielding legally — this
is inside the resume/repause interval) for the sidecars to be parked again, and
then pours with **no yield between the first byte and the last**: Ring-3 runs
only while the control plane yields, so a yield in the pour body would let the
environment run on half-restored bytes. Two rules are load-bearing and both are
source clauses: bytes move **only onto the captured placement** — a region whose
base the replay moved is refused (`EP_REFUSE_PLACEMENT`) *before* a byte moves,
because the sidecar's own allocator holds raw pointers into those regions — and
the register half restores the park form only; boot-local channel ids are not
resurrected, because they are per-boot and were never identity. Console bytes
come back through `env_console_inject()` **under the new env_id**, so the first
command the restored environment answers is its own history.

**Reported by name.** The restore response carries `payloads` and
`payload_skips`, and `POST /api/env/restore` takes `"payload":false` — a
metadata-only replay that says what it withheld instead of passing silently.
A pour that fails leaves the replay standing and logs
`[ENV_PAYLOAD] contents not restored` — the honest state: an environment without
its contents is not a restored environment, and nothing claims otherwise.

**The clauses.** Source clause **T7** (capture inside the frozen window, the
pre-freeze wait that lands the freeze on a parked posture, pour after the
repause with no yield, the placement refusal, the knob and its route,
the report fields, the console halves, `env_payload.c` linked, every host test
that links `persist.c`/`env_service.c` resolving the payload symbols — linked
via `tests/payload_host_stubs.h` where the test does not exercise them — and
`tests/env_payload_host_test.c` driving the entry points); live clauses **L4b**
(file written, variable set, both read back through the console *before* the
checkpoint) and **L11** (the same two read back *after* the replay); artifact
clauses **B10** (byte-identical) and **B11** (the variable set before is visible
after) — the plan's two contents clauses, asserted.

**The tooth changed meaning, as §4's tooth list anticipated.**
`P1A_TOOTH=no-restore` no longer withholds the replay — it withholds the
**payload** (`"payload":false` through the same pass). B8 (liveness) stays green
because an empty environment is still an environment, and B10/B11 are what go
red; B7 must report the withholding by name (`payloads=0`,
`payload_skips≥1`). Withholding the descriptor now would prove nothing about
the bytes it is the payload of. The source tooth group `P1A_TOOTH=payload`
adds nine teeth over T7 — capture called outside the frozen window, a yield or
a return between the repause and the pour, the knob and the report dropped, a
host test left with unresolvable symbols, the placement refusal disabled, the
console inject disabled, and the host test itself deleted (a claim, not a
feature).

`tests/env_payload_host_test.c` (43 checks: the round trip, the full refusal
matrix — absent, format, sequence, placement, size, no process, running, not
parked, no mapping, console, both CRC layers — the bounded wait, and the
refusal text) links the real `kernel/env_payload.c`; the host suite runs 120
tests green with it included; the guard's smoke runs 45 checks green including
the new split (B8 green under the tooth while B10/B11 redden).

**Still owed.** Nothing inside this increment: the live round trip with contents
is measured — `--live` writes the file and sets the variable through the console
before the checkpoint and reads both back after the replay — and so is its
`no-restore` tooth arm, the last measurement §4's verification plan names. What
remains beyond P1a is unchanged: the boot-side mutations for the other three
tooth names at boot granularity (they are source teeth today), and per §11 Q2
the question of whether an environment should pause alone rather than take its
partition with it.

### The announce gate — the pour waits for the replayed sidecar's `[env-id]` before it lands

The sixth increment's payload made the restore path land on a sidecar that was
parked, and *parked* turned out to be the wrong question. The payload's first
CI run reddened three times at the same place — the durable guard's post-reboot
identity wait (`cons_wait "[env-id] index=$INDEX" 90`, feeding clause L16 in
`tests/env_storage_durable_check.sh`), whose failure is "the environment
never announced its identity after the reboot" — and the first attempt was
deterministic, reproduced on the build host with the guard's own exit status.
The cause was in this phase, not the guard's: a freshly replayed sidecar blocks
mid-boot at the ramdisk's RD_INFO handshake, parked at a channel in a `recv`,
and that posture is indistinguishable from the captured idle park — so
`env_payload_wait_parked()` said yes, and the pour replaced the half-finished
boot's pages, `user_rip` and `user_rsp` with captured state. After the resume
there was no boot left to finish: `boot()` never reached its `[env-id]` line —
`IDENTITY_MARKER` (`user/sidecar/src/boot.rs`) is written to the environment's
own console only and never appears on serial — and the guard, draining that
console over HTTP, timed out against a buffer nothing was running to write to.
Main never tripped this because main has no pour: as the guard's own comment
records, the re-pause froze the fresh sidecar mid-boot and the operator's next
act was to resume the partition and let the boot finish — the sequence the pour
silently broke.

**The gate.** Parked is now the first condition, not the only one. `struct
EnvConsole` carries an `identity` flag (`kernel/env_console.c`), set only when
the console's own drain path sees the sidecar's marker among the bytes it
drains (`ec_sees_identity()`), and cleared at wiring and at release — the
inject path writes the buffer directly and can never set it, so the flag can
only come from this boot's announcement, never from the restore's echo of it.
`env_payload_wait_parked()` (`kernel/env_payload.c`) is ready only when every
task is parked **and** `env_console_identity_seen(partition, index)` sees that
announcement. Everything else about the wait is untouched: the tasks loop runs
first, so a missing sidecar is still answered at once; the bound stays
`EP_WAIT_TICKS` 3000 (~30 s at timer.h's documented ~100 Hz) with the same
no-yield discipline; and a timeout logs `announced=%d — proceeding on the
posture as it stands` and proceeds — the gate decides *where the pour lands*,
it is not a new refusal, and the capture-side pre-freeze wait is unchanged in
practice: the record exists because the environment ran, so its flag has been
set since boot.

**The clauses.** T7's pins did not move: the pre-freeze wait before the
quiesce, `env_payload_wait_parked()` before the repause, the pour after it with
no yield between. `tests/env_payload_host_test.c` gained the check that names
the defect — "parked but not yet announced is not the pour's go signal"
returns the tick bound instead of proceeding — bringing the suite to 44 green.

**The evidence.** The repair landed with all seven of its checks green,
including `kernel-guards` at 1 h 32 m, whose live arm contains the very wait
that failed: after the reboot the environment announces `[env-id]`, the shell
answers it, and both files read back through the environment's own console
(clause L16). Worth recording plainly: the defect was in this phase's restore
path and was caught by P1b's guard, because P1b's reboot evidence rides P1a's
replay and pour — each phase's gate now runs the other's payload.

---

## 5. Phase P1b — Durable environment storage

**Why.** Even with P1a, an environment's files live in `rd_storage` — a 1 MiB **MEM region from the frame pool** (`RD_STORAGE_FRAMES` 256, passed as `storage_base`/`storage_len` in the ramdisk manifest). Frames are RAM: they are the thing a checkpoint has to carry, and they are gone the moment nothing restores them. v0.1 §12 named the fix in one sentence — *"storage that survives reboot, such as an SLS-backed aerofs in place of RAM-backed ramdisks"* — and gave the reason it matters beyond convenience: **"an environment that survives reboot is the single-level-store property containers do not have."** That is the IBM i claim, and it is the one this phase makes true.

**Scope.**

- **The block device stops being RAM.** The tenant ramdisk's `storage` cap resolves to a region backed by NVMe rather than by `alloc_region_in(RD_STORAGE_FRAMES, …)`. The tree already has the pattern: a stream is a persistent object with its own NVMe LBA range and a directory that survives reboot (`stream_persist_directory()`), and it is already moved between nodes page-by-page. P1b reuses that shape rather than inventing a second persistence story — the design question to settle in review is whether the environment's storage *is* a stream (a named blob with an LBA range, gaining migration and quota for free) or a dedicated per-environment LBA region with its own small directory.
- **A writable, larger on-disk format.** *(Landed — see the addendum below.)* aerofs-lite is explicitly "the read-only root filesystem format" with 512 B blocks, 11 direct + 1 indirect block per inode, and a **71 168-byte maximum file size** (`NDIRECT` 11, `NINDIRECT` 128, so `MAX_BLOCKS` 139). A durable tenant filesystem cannot ship that as its ceiling. P1b's deliverable is a writable format — by extending aerofs-lite with a second indirect block (or a different on-disk layout with a compatibility rule) — and the *migration* of the format is a real decision, not an implementation detail, because the system rootfs image and every existing tenant ramdisk are formatted in it.
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

### Findings addendum — what has landed, increment by increment

P1b is now partly built. What exists is the **format layer**: the writable
on-disk v2 that a durable region has to be formatted in. It went first because
the device swap is worthless without it — a durable region formatted in v1
inherits the 71 168-byte ceiling and the read-only semantics — and because it is
the part that can be verified on the host, with no reboot.

Landed, and verified:

- **`user/vfs/src/aerofs.rs` — the v2 layout.** aerofs-lite grew a version
  rather than a second format: `AEROFS_VERSION_V2` keeps the
  superblock-on-block-0 shape, the 512 B blocks and the dirent encoding, and
  widens the file layout to 10 direct + 2 indirect blocks (`MAX_BLOCKS_V2` 266,
  `MAX_FILE_BYTES_V2` 136 192). The superblock gains `version`, `total_blocks`,
  `alloc_start`, `alloc_blocks` and a CRC over the fixed 48-byte header; the
  allocation map is two bitmaps (blocks first, inodes second). **The
  compatibility rule is written into the code — read v1, write v2, refuse the
  rest** — and an unknown magic, version, block size, CRC or layout is refused
  *by name* (`SuperblockRefusal::Version { found }`, not "corrupt"), so a v3
  store fails as v3 and not as a crash.
- **`user/vfs/src/vfs.rs` — the writable mount.** An `AerofsFs` is writable iff
  its superblock says v2; nothing else grants it. On a v1 store (the system
  rootfs image, and every existing tenant store) reads dispatch to v1's inode
  parse and block walk, and **every** mutating entry point refuses with `ERofs`
  — proven by a test that mounts a v1 image through the formatting mount and
  watches its device bytes stay v1. The v2 half is the full set:
  `write`/`truncate`/`create_file`/`create_dir`/`unlink`/`rmdir`/`rename`,
  block and inode allocation from the on-disk bitmaps, tombstoned dirents
  (name removed first, blocks second — a crash can leak a block, never hand it
  to two owners), and a flush that stores the allocation map before content.
  Mounting an *empty* store formats it v2 sized by the device's own block count
  rather than a constant, and writes only the non-zero blocks (11 writes for a
  1 MiB store).
- **`user/vfs/src/errno.rs`** — `EFbig` (27) is the ceiling's own error: a write
  past 136 192 bytes is refused as a file too big, not as ENOSPC.
- **`user/vfs/tests/vfs_tests.rs` — five new integration tests**, including the
  one that makes the durability claim the format layer's to carry: a
  71 200-byte file (larger than v1's ceiling) written through the mounted
  filesystem, then **a second power-on** — a fresh fake kernel, a fresh driver
  and a fresh mount over the same device bytes — reads it back byte-identical
  and still writable. The others: an empty store formats writable; the allocator
  frees blocks on unlink and refuses clearly (`EFbig` past the ceiling, `ENOSPC`
  when the store is full, proven by churning a 120 KiB file); and the v1
  read-only gate.
- **`tests/aerofs_v2_check.sh` + `tests/aerofs_v2_check_smoke.sh`** — the guard
  pins every claim above as a source clause (the version rule and the named
  refusal, the two ceilings and the inode pointer offsets, the builder that must
  stay v1, writability derived from the version with the `ERofs` gates, the
  block-count-sized formatting, the named host tests) and runs the host tests
  where a toolchain exists; its smoke renames each clause away and requires
  ABORT, with 17 teeth biting.

**The decisions this increment made**, and the reason for each:

- **Extend, not replace.** §11 Q3 is answered by the implementation: v2 is a
  version the existing parser can refuse cleanly, so the system rootfs image and
  every existing tenant ramdisk keep mounting — read-only — with no migration
  step, and no image is rewritten before the durable region exists.
- **Writability is a property of the volume, not a mount flag.** A `-w` mount
  option would admit two ways to be wrong (a v1 store written, a v2 store
  mounted read-only); the version in the superblock is the one thing a device
  cannot lie about.
- **No upgrade-in-place path yet.** `mount_aerofs_or_format` formats an empty
  store v2 and never re-formats a v1 one; the only v1 stores in the tree are
  read-only images, and an in-place upgrader would be a migration story with no
  customer.
- **The image builder stayed v1 on purpose.** The system rootfs ships as a v1
  image, and a guard clause pins that the builder does not drift into v2 while
  the durable half is missing.

**The rest of P1b — the durable device — landed in the second increment,
recorded below.** The tenant storage stopped being `alloc_region_in(RD_STORAGE_FRAMES, …)`
with it: the region is quota-charged, P1a's descriptor names it, and §11 Q1
(stream vs dedicated LBA region) is settled there on the same rule Q3 was —
by implementation, with the reasons written down.

### Second increment — the durable region, quota-charged

**What exists now.** The tenant ramdisk's `storage` cap still allocates the
same 1 MiB frame region (the RAM copy the POSIX side reads), but it is no
longer the LAST copy: an attach registers the region with the kernel as a
durable store, and every write is mirrored into a dedicated per-environment
extent on NVMe, charged to the tenant's partition before a byte moves.

Landed, and verified:

- **`kernel/env_storage.h` / `kernel/env_storage.c` — the extent band and the
  table.** A dedicated LBA region (§11 Q1's answer — the reasons are below):
  `ENV_STORAGE_DATA_LBA_BASE` 1 114 112 is exactly where stream data ends,
  eight extents of 2048 sectors (1 MiB each — `RD_STORAGE_FRAMES` 256 × 4 KiB,
  so region page *i* maps extent page *i*) end at 1 130 496, far below
  `ROWSTORE_LBA_BASE` 2 000 000. The table is keyed `(partition_id, index)` —
  the stable identity a reboot restores — while `region_base` is this boot's
  placement, the IO lookup key. Attach zeroes the extent (a recycled slot must
  not hand the next tenant the previous one's bytes), restore reads it back
  into a FRESH region, write persists through, release hands the pages back.
  `sizeof(struct EnvStorageEntry) == 32` is pinned by a `_Static_assert` where
  persist.c's checksum span means it.
- **The directory, persisted like every other array.** Header at LBA 7720,
  entries at 7728 (P1a's entries end at 7712; the one-frame gap every
  `persist.h` boundary carries is kept), magic `PERSIST_MAGIC_ENV_STORAGE`,
  pending bit 17, written and restored through `persist.c`'s own
  checksum/torn-write machinery — so occupancy SURVIVES the reboot and the
  re-attach re-charges it (or refuses `ERR_QUOTA` if it no longer fits). The
  restore validates record size and version before touching the live table and
  refuses a foreign snapshot BY NAME, and `env_storage_boot_reset()` runs
  before the directory read so "once per boot" is one line a guard can pin.
- **First-touch quota charging.** `storage_page_reserve(e->partition_id)` —
  the store's OWN tenant, never `PARTITION_SYSTEM` — before any NVMe byte; a
  refusal reverts this call's pages, prints `[ENV-STORAGE] quota denied
  partition=…`, and replies the quota's own status. The persisted
  `charged_pages` high-water is the occupancy: monotone, re-charged at boot,
  handed back at release. E4 Finding 1's residue (frames charged to the
  creator) does not reappear here.
- **Syscalls 323-326** (`SYS_SLS_ENV_STORAGE_ATTACH/RESTORE/WRITE/RELEASE`),
  declared in `env_storage.h`, wrapped in `cap.c` (attach/release
  PARTITION_SYSTEM-only; restore/write owner-partition-checked via
  `env_storage_owner()`), dispatched in `syscall_dispatch.c`. Statuses are
  named, and `ERR_QUOTA` is its own code — distinguishable from a frame-pool
  exhaustion the same way `EnvCkptRefusal`'s codes are.
- **The data flow, end to end.** init attaches the store BEFORE creating the
  ramdisk server (a refusal frees all three regions and replies the new
  `ENV_ERR_QUOTA` 7, so a denied placement allocates nothing); the ramdisk
  server restores at startup and write-throughs every `RD_WRITE` into the
  extent (quota refusal crosses the wire as the new `RD_ERR_QUOTA` 9); the
  VFS maps it to the new `EDquot` (122) so a tenant sees "disk quota exceeded"
  rather than EIO; `reclaim_environment` releases the store before freeing the
  regions; and P1a's descriptor stamps `state_lba/state_sectors/state_bytes`
  from `env_storage_extent_of()` so the checkpoint names the durable store.
- **Refusal by name, the rest of the way down.** `mount_aerofs_or_format`
  consults `parse_superblock_refusing` and refuses a NON-EMPTY unparsable
  block 0 with `EInval` instead of formatting v3 over as v2 (part 1's S8
  clause had left that hole — the format refused by name but the mount
  erased); the sidecar's boot probes block 0 first and surfaces the reason as
  `BootErr::MountRefused("version" …)`, so the serial log names the refusal.
- **`tests/env_storage_host_test.c`** — the module executed on the host: a
  RAM disk behind the real NVMe entry points, 32 checks covering attach/zero,
  write-through and restore across a fresh region, quota refusal with RAM
  revert, re-attach re-charge refusal, release, and the unattached-is-RAM
  invariant. **`tests/env_storage_durable_check.sh` + its smoke** pin every
  wiring claim as a source clause (S1-S12, the band arithmetic derived in the
  guard itself), build and RUN that host test where a toolchain exists,
  validate a recorded run's artifacts (D1-D5), and prove themselves with 12
  teeth across the four §5 tooth names.

**The decisions this increment made**, and the reason for each:

- **A dedicated LBA extent band, not a stream — §11 Q1 answered.** Streams
  are eight fixed 64 MiB slots, deliberately EXCLUDED from the storage quota
  (`stream.c`'s own rule), so a stream-backed store would get persistence and
  migration at the cost of the phase's central requirement: a tenant's disk
  counting against the tenant. A stream slot is also 64× the store it would
  hold, and its directory speaks "named blob" where the store's identity is
  `(partition, index)`. The dedicated band reuses the persistence STORY
  (persist.c's checksummed region) without inheriting a stream's semantics,
  and its extent is exactly the region it mirrors — page-for-page, which is
  what makes the write-through a copy and the restore a read.
- **First-touch high-water, not write-accurate metering.** A page is charged
  once and the figure is persisted; there is no per-write ledger to repair
  after a crash, and the refusal decision is made BEFORE the device write, so
  "refused" means nothing moved. The cost — a truncated-then-rewritten file
  keeps its high-water — is the same trade frame quotas already make.
- **Identity and placement are different keys.** The table is keyed by
  `(partition_id, index)` because that is what survives a reboot and what a
  descriptor re-attaches to; `region_base` changes every boot (a reboot
  allocates fresh frames) and is only the IO key. Conflating them would make
  restore depend on where the frames happened to land.
- **An unattached region is RAM by design.** The system rootfs never attaches
  a store; its ramdisk staying memory-only is the documented `NOENT`-means-
  unattached path, not a gap — which is also why permission refusals answer
  `INVAL` and never `NOENT` (the two must not be confusable at the wire).
- **No device is no reason to refuse the environment.** The first cut of
  attach fail-closed when the I/O queue was absent, and the E5/E6 smokes
  caught it the first time they booted: refusing attach refused ENVIRONMENT
  CREATION on every boot whose NVMe never came up (a BAR below 4 GiB), which
  is a regression of the environment, not of durability. attach now degrades
  to a RAM-backed store — slot −1, durable flag off, the serial line saying
  `durable=0`, metering intact — the same posture stream.c's cold-start skip
  has. Fail-closed still holds where a device IS present but unreachable:
  a zeroing that does not reach media refuses the attach, and the host
  test's stubs now enforce the driver's own buffer contract (alignment,
  transfer cap) so "durable that never reaches media" cannot pass on a
  permissive stub again.
- **The mount's zero-gate is the refusal's other half.** Naming the refusal
  in the parser is worthless if the formatter never asks: a non-empty
  unparsable store is refused (S8e/S8f, the `bad-format-version` tooth's
  target), and the sidecar's probe is what turns the errno into a name.

**What this increment does not claim.** The live arm of
`env_storage_durable_check.sh` (the boot: write, reboot, re-read, quota
re-charge, over-quota refusal on serial) runs on a build host where an ISO
can be built — CI's verify job proves the source clauses and the teeth, and
kernel-guards boots the ISO for the format layer's guard; the boot evidence
for the durable region itself is that arm. Migration of a durable store
(P3) and snapshotting one are still out of scope, as §5's scope said.

### Third increment — the boot evidence, on every push

**What exists now.** The gate §10 names for step 2 — a file larger than
71 168 bytes survives a reboot and is charged to the partition — is no
longer something that ran by hand on a build host and was believed:
`tests/env_storage_durable_check.sh --live` runs in CI's `kernel-guards`
job (the build host that already has the ISO) and re-makes the whole
evidence chain — write past the ceiling, checkpoint, in-place node reboot,
re-read byte-identically through the environment's own console, quota
re-charged, an over-quota write refused by the quota's own error — on
every push.

Landed, and verified:

- **`.github/workflows/ci.yml` — the step.** "P1b durable region live
  boot (write, reboot, re-read, quota)" runs the guard's `--live` arm
  right after the service smoke: the cheap boot sanity names a kernel
  that does not boot first, and this step names a durability regression
  second. The arm brings its own QEMU (host port from
  `tests/free_port.sh`, its own temp workdir and NVMe image), so it
  neither collides with the job's other boots nor leaves state behind.
- **`tests/env_storage_durable_check.sh` — clauses S13 and S13b.** The
  guard now reads `.github/workflows/ci.yml` as part of its surface (a
  missing one is the same exit-2 ABORT as any other missing file) and
  pins two halves: ci.yml invokes the guard's `--live` arm at all (S13),
  and the invocation sits AFTER the ISO build (S13b — a boot guard ahead
  of its image aborts before booting, reddening for the wrong reason
  while proving nothing about the reboot). A source clause cannot reboot
  anything; it can insist that the job that can does.
- **`tests/env_storage_durable_check_smoke.sh` — three wiring teeth.**
  A missing ci.yml aborts (exit 2) instead of reporting the wiring
  sound; deleting the `--live` step reddens S13 **and only S13** (the
  guard's own `elif` keeps the attribution as narrow as the mutation);
  ordering the step before the ISO build reddens S13b while S13 holds
  green. The smoke is 15 checks now and still source-level — no QEMU,
  no ISO, no build, only gcc.

**The decisions this increment made**, and the reason for each:

- **Pin the wiring where the guard that needs it lives.** Guards already
  read ci.yml when the wiring is their own property
  (`guard_port_band_check.sh` for the hostfwd ports,
  `kernel_copy_matrix_check.sh` for wired sources); P1b's one CI-only
  claim is this arm, so its clause sits with the rest of the guard's —
  one file to read for everything the phase asserts.
- **After the service smoke, not before it.** Both boot the same ISO;
  running the cheap health check first means a boot failure is named by
  the step whose whole job is that, and a durability failure by this
  one. Attribution, not ordering convenience.
- **Still not in `verify`.** The arm needs an ISO and QEMU; `verify`
  proves the source clauses and every tooth (wiring teeth included),
  kernel-guards runs the boot. The second increment's "does not claim"
  paragraph above stays as written — it records what THAT increment
  shipped; this one retires its conclusion only as far as CI is
  concerned.

**What this increment does not claim.** It does not make the arm faster
or hermetic: it costs two boots of wall time inside kernel-guards (the
job's long pole remains its own total), and it still fails closed with
the artifacts directory printed for `--replay`. A hand run on a build
host remains how an operator obtains a fresh artifacts directory.
Migration of a durable store (P3) and snapshotting one remain out of
scope, as §5's scope said.

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

### The increments — four PRs, one tooth each

§6 names the design, the guard that will measure it
(`tests/env_net_isolation_check.sh`) and the four teeth; what is not named is
the order. The order below is the rule P1a and P1b were broken by: land the
piece everything else writes into first — the piece verifiable without the
hard part — and give every increment exactly one of the guard's named teeth,
so each PR reddens something real when the property it carries is removed.
§6's clause set closes on the fourth.

Named once, so no increment claims credit for what already exists: the NET_*
wire and its client (`user/proto/src/lib.rs`, `user/sidecar/src/net_client.rs`
— socket/bind/connect/listen/accept/send/recv/poll/shutdown), the `nc` applet
the console already drives it with (`user/sidecar/src/applets.rs`), the network
driver's own server (`user/network/src/server.rs`), the kernel stack the
service will front (`net/ipv4.c`, `net/tcp.c`), the attribution and quota
module (`net/tcp_quota.h`, syscalls 277/278, `GET/POST
/api/partition/connquota(s)`), and E2's mint path — `cap_create_sidecar()`'s
kernel-service branch, the `kernel.` prefix arm whose per-name cases today are
`kernel.env.control` and `kernel.env.console` (`kernel/cap.c`). What P2 adds is
the tenant's right to reach any of it, a kernel-owned peer to reach through,
the bounds at the admit path, and the guard that proves it.

### First increment — the cap and the kernel-owned name: the wire

The tenant manifest gains the `network` Chan cap — peer `kernel.net.socket`,
rights 0x7 beside its console and ramdisk siblings — `cap_create_sidecar()`'s
kernel-service branch grows the `kernel.net.socket` arm beside the two that
already mint kernel ends, and the service behind that name speaks NET_* but
answers every verb with a refusal by name. Nothing admits yet: the channel
exists, is typed, and fails honestly, which is the wire the rest of the phase
writes into. The pin it rotates is `tenant_profile_has_only_budget_console_and_its_own_ramdisk`,
which counts caps (`n_caps == 3`) and asserts `network` among the absent — both
halves move: four caps, `network`'s absence replaced by a positive pin that
its peer is exactly `kernel.net.socket` and never `drv.network.0` (the test's
name counts caps too and rotates with the claim). The system profile's own
`network → drv.network.0` cap (`build_posix_manifest()`) is untouched. Proven
without a byte of TCP: host tests on the refusal rendering, source clauses on
the manifest pin, the mint arm and the registration under the kernel-owned
name, and one live clause that needs no peer — a tenant console's `nc` reaches
the service and is refused **by name**. Tooth: **`system-peer`** — point the cap
at `drv.network.0` instead and channel creation refuses it for crossing
partitions, red before a socket is ever opened (the boundary, not the API).

### Second increment — the outbound path, attributed and admitted at the connect

The service implements the verbs §6 scopes (`socket`, `connect`,
`send`/`sendto`, `recv`/`recvfrom`, `shutdown`, `close`) against the kernel's
own stack — never `drv.network.0`, which is the whole reason the peer is
kernel-owned — and the admit path runs `tcp_conn_attribute(conn_id, uid)` and
the caller's partition quota before a connection exists. The default stays
0 = unlimited, so behaviour is byte-identical for every operator who has not
opted in (§6's BSS-zero-safe rule kept), and `nc <host> <port>` from a
tenant's own console carries real bytes end to end. The guard gains three
clauses: the outbound reach clause (live, through the environment's own
console), the attribution clause pinned at the admit path's line order
(source), and the 0 = unlimited regression clause. Tooth: **`unattributed`** —
admit without attribution and the attribution clause reddens, because a
connection with no partition counts against nobody (the "denial looks like
absence" family E6's console guard was built around). Not yet: listeners
(third), the quota's starvation measurement and CI wiring (fourth — the
mechanism lands here, its proof lands there), UDP and name resolution (§6
names them as following), and the Linux shim's socket syscalls (§9's
forward-to-this-service, no second implementation).

### Third increment — the listener, the isolation, and the descriptor's line

The other half of §6's "client and listener": bind/listen/accept through the
service, each partition's listeners reachable only from inside that
partition — a tenant reaches its own partition's service and nothing beyond
it. The guard's isolation clauses land with it: two environments in two
partitions, each listener driven by `nc` through its own environment's
console — A reaches A and **not** B, the refusal by name. And the checkpoint
boundary becomes measurable: the descriptor carries the listener's
configuration and never a connection id, a checkpoint/restore re-establishes
the listener with every socket closed (§6's descriptor angle, so P1a's
reviewers are never asked to capture TCP state). Tooth: **`restore-sockets`** —
make the descriptor carry live connection ids and the guard's consistency
clause reddens after the reboot.

### Fourth increment — the bounds measured, and the gate on every push

§6's verification plan in full: a small conn quota set on partition A through
the surface that already exists (syscalls 277/278, `POST
/api/partition/connquota` — no new API), A's environment flooding past it, the
excess refused **by name** while **B keeps serving throughout** — the clause
that is the whole point of the phase — the request family still under
`net/http_rate_limit.c`, and a partition with no quota set behaving as before
(0 = unlimited). The `--live` arm is wired into CI's `kernel-guards` job after
its ISO build, the S13 shape P1b established: the guard reddens by name if the
step is deleted or reordered. §10's P2 gate closes here, and §11 Q4 (the
recommended non-zero default) stays open — these increments make the quota
enforceable, not prescribe its number. Tooth: **`no-quota`** — remove the quota
check from the admit path and the starvation clause reddens (B starved by A's
flood) while the reachability clauses stay green: the vacuity control and the
reason the phase exists.

### Findings addendum — what landed, increment 4 (the bounds measured, and the gate on every push)

The fourth increment is in, on the source side. What the design prose above
promised, and what actually landed — with the one honest limit stated
plainly:

- **No new API, pinned end to end.** The live arm drives the surface that
  already existed: syscalls 277/278 and their dispatch, `POST
  /api/partition/connquota` with the exact `{partition_id, quota}` body, the
  `GET /api/partition/connquotas` readback, and the operator shell pair. The
  guard's **N15** pins every one of those links, so the surface cannot rot
  under the gate that leans on it.
- **The starvation clause, named.** **N16** pins the gate in
  `net/tcp_quota.c` itself — deny before attributing, and the `quota != 0`
  half that keeps 0 = unlimited (§11 Q4's contract). This is the clause
  §6's named `no-quota` tooth reddens: the gate becomes constant-false, N16
  reddens, and the reachability clauses N12-N14 stay green — the vacuity
  control the roadmap names, asserted as NARROWNESS (no `FAIL:` line for
  N12/N13/N14 may appear), not as an exit code alone.
- **The gate is on every push.** ci.yml's kernel-guards job gained the live
  step right after its ISO build — the S13 shape P1b established, point for
  point: **N17** reddens if the step is deleted, **N17b** alone if it is
  reordered ahead of its image, and the smoke proves both halves bite
  separately.
- **The live arm (L5), written and honestly scoped.** It sets a quota of 1
  on partition A, arms a loopback listener from a second environment in A,
  floods from A's first environment, requires the EXCESS accept to be
  refused **by name** (closed, charged to nobody), and then requires
  partition B — quota 0, read back — to serve a client of its own
  throughout, with B's identity carrying no refusal: §10's P2 gate, in full.
  **Limit: the arm has never booted.** Its first real run happens in CI's
  kernel-guards job on the next push; the loopback leg (guest→its own NAT
  address through QEMU user-net) is the named risk, and the arm fails BY
  NAME with that diagnosis rather than passing vacuously if user-net does
  not hairpin the guest's own address back to it.

Proven by: the guard's N15-N17b clauses and the smoke's four new teeth
(`no-quota-surface`, `no-quota`, `no-ci-live`, `no-ci-order`) — 24 teeth in
total, all biting with the vacuity control green; ci.yml parses as YAML and
carries the step after the ISO build. §11 Q4 stays open, exactly as the
prose above requires: these increments make the quota enforceable, not its
number prescribed.

### Findings addendum — what landed, increment 3 (the listener, the isolation, and the descriptor's line)

The third increment is in. What the design prose above promised, and what
actually landed:

- **The listener verbs.** `kernel/net_socket_service.c` now admits `bind`,
  `listen` and `accept` alongside the client verbs, against the kernel's own
  `net/tcp.c`. **Finding — the stack has no separate `tcp_bind`.** The bind
  claims the port on the channel's own socket (`sk->lport`); the stack's
  listener slot is drawn at `listen()` (`tcp_listen(sk->lport)`), so a bind
  that is never followed by `listen()` costs a conn slot nothing. The accept
  is the same non-blocking scan `tcp_accept()` and `http.c`'s pickup loop do
  — one pass per service tick, answering `NET_EAGAIN` (status 11) when
  nothing is pending, which the client's bounded retry treats as "poll again",
  never an error.
- **Attribution binds at accept, not at listen.** The listener itself is
  deliberately unattributed; the accepted conn is charged to the caller's
  partition (`tcp_conn_attribute_partition(found, e->partition)`) the instant
  it is handed out — the same admit-at-the-moment rule the connect path
  keeps, and the inbound half of the `unattributed` tooth. Over quota, the
  accept closes the conn and charges nobody (deny before side-effect). The
  host test pins all four arms: a ready accept, the listener staying listening
  for a second accept, the over-quota refusal-by-name, and the non-listener
  refusal.
- **Isolation is keyed to the channel, and the guard now says so.** Every
  socket — listener and accepted — is bound to the partition whose sidecar
  opened the channel, so a listener belongs to exactly one partition. The
  guard (`tests/env_net_isolation_check.sh`) gains three source clauses: **N12**
  (bind claims the port, listen/accept reach the stack, accept attributes),
  **N13** (no cross-partition reach), and **N14** (the descriptor's line). Its
  `--live` arm gains **L4**: two partitions, B opens a listener through its
  own console, A runs `nc` at it and must be answered **by name** — the
  isolation property, end-to-end. The teeth
  (`tests/env_net_isolation_check_smoke.sh`) gain `no-listener-verbs`,
  `no-accept-attribution`, `no-bind-port` and the named **`restore-sockets`**
  tooth, all biting with the vacuity control green.
- **The descriptor's line, stated honestly.** The checkpoint descriptor
  (`RegisterReply`, `user/proto/src/env_proto.rs`) carries the environment's
  identity, its three regions and its four messenger endpoints — and never a
  connection id, socket id or listener handle. **Finding — the descriptor
  carries no port or listener *configuration* either.** The listener is
  re-established by re-running the applet over the restored channels, not
  carried as a config record; N14 therefore pins the negative (no conn id /
  socket / listener handle enters the record) plus the messenger endpoints a
  restore does re-wire, rather than asserting a config field that does not
  exist. §6's descriptor angle holds unchanged: P1a's reviewers are never
  asked to capture TCP state.

Proven by: `tests/net_socket_service_host_test.c` (137 checks — the outbound
flow plus this increment's listener arms, against the real `net/tcp_quota.c`
with only `net/tcp.c`'s data-path entry points stubbed); the guard's N12–N14
source clauses and its L4 live arm; and the smoke's four new teeth, all
verified with the vacuity control green.

### Findings addendum — what landed, increment 2 (the outbound path)

The second increment is in. What the design prose above promised, and what
actually landed, with the one finding worth recording:

- **The verbs.** `kernel/net_socket_service.c` implements the client verbs
  §6 scopes — `socket`, `connect`, `send`, `recv`, `shutdown`, `close` —
  against the kernel's own `net/tcp.c`, never `drv.network.0`. `bind`,
  `listen` and `accept` stay refused **by name** (increment 3's listener), so
  a tenant is never left wondering whether a verb is absent or broken.
- **The admit path.** A `NET_CONNECT` is attributed to the caller's partition
  (`tcp_conn_attribute_partition`) and checked against that partition's quota
  (`tcp_partition_get_conn_quota`/`usage`) **before** a connection exists —
  the property the whole phase rests on, and the one the `unattributed` tooth
  removes. The quota's default stays 0 = unlimited (the module's BSS-zero-safe
  rule), so behaviour is byte-identical for an operator who has not opted in.
  The attribution module (`net/tcp_quota.c`) was widened to the full
  `tcp_conns[]` range for this: an outbound connection lands in the reserved
  tail and, unattributed, would count against nobody.
- **The counters.** Admission is independently countable —
  `net_socket_admits()` and `net_socket_quota_refusals()` — so an admit and a
  quota deny are each observable, which is what the `unattributed` tooth and
  the guard's N10/N11 clauses pin.
- **The handshake.** `NET_INFO` now answers `max_sockets` the real
  `NSS_MAX_SOCKS` (8) rather than increment 1's 0 — the increment-2 change
  from "nothing admits" to "here is the ceiling a client can plan against".
- **Finding — the sockaddr byte order.** The wire is little-endian
  (`user/proto`'s `encode_sockaddr` writes the client's logical `ip` with
  `to_le_bytes`), but the kernel stores `IPv4Addr` in **network** order
  (`include/config.h`: `KERNEL_STATIC_IP = 0x0F02000A` for 10.0.2.15), which
  is what `tcp_connect()`/`ipv4_send()`/`arp_lookup()` all compare and write —
  `tcp_connect()`'s `(ntohl(dst_ip) & mask)` turns that network-order value
  into logical form for its subnet test. The service therefore decodes the
  wire bytes in **memory order** (most-significant address byte first), yielding
  the network-order `IPv4Addr` directly. Decoding LE here — the obvious reading
  of "the wire is LE" — would hand `tcp_connect` a byte-swapped address and
  route every outbound connect to the wrong host. The host test pins the exact
  value the stack receives (`0x0100007F` for 127.0.0.1) so this cannot regress
  silently.

Proven by: `tests/net_socket_service_host_test.c` (109 checks — the outbound
flow against the real `net/tcp_quota.c` with only `net/tcp.c`'s five data-path
entry points stubbed, the LE/network-order decode pinned, the quota teeth, and
the deny-before-side-effect clause); `tests/env_net_isolation_check.sh` N10
(admit path: verbs reach the stack, the connect is attributed and quota-checked
before the connection exists) and N11 (the admission counters and the quota
module are compiled in and linked by the host test); and the `unattributed`
tooth in `tests/env_net_isolation_check_smoke.sh`, which reddens N10 when the
attribution call is removed.

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

### Findings addendum — what landed, increment 1 (the placement gate)

The first increment is in. It is the "refused *by name*, before anything is
created" half of the placement bullet above — the whole-environment version of
E4 Finding 1's fix — and it landed as one question plus three rollbacks:

- **The gate became a question.** `SYS_SLS_PLACEMENT_CAN_HOLD` (327,
  `kernel/cap.h`) answers "can partition P hold N more frames on this node?"
  from the two counters only the kernel owns — the partition's frame
  quota/usage (LPAR Phase 13's `partition_get_frame_quota`/`_usage`) and the
  node's free-frame count (`frame_pool_free_count()`, new) — granting nothing
  and mutating nothing. The environment manager asks it before the first
  allocation; a NO is refused `ENV_ERR_CAPACITY` (status 8, named in
  `env_status_name()` so the shell and the HTTP routes both say "insufficient
  frame capacity") with nothing allocated anywhere. The question covers the
  three regions exactly (1344 frames); the sidecars' image/stack frames ride
  the allocator's own quota check, whose rollback is now complete.
- **Finding — the pre-flight cannot be the whole answer, so the rollback was
  the real increment.** Another create can land between question and answer;
  the allocator's own quota check stays the backstop for that race. Writing
  the backstop honest exposed that **three of the create path's failure arms
  leaked**: the alloc-shortfall arm returned `NOMEM` leaving whichever regions
  had already succeeded charged to the tenant, and a first- or second-sidecar
  create failure left the storage attach and all three regions held. All four
  arms now unwind completely — shortfall frees what succeeded (a zero base is
  the allocator's own failure value, so one unconditional call per region is
  safe); a sidecar failure releases the store and frees the regions; a
  second-sidecar failure additionally ends the half-created ramdisk first.
- **Finding — the rollback must not free live memory, so it confirms the
  kill.** The kernel DEFERS a kill whose target is RUNNING
  (`kernel/process.c`), so freeing the half-created ramdisk's heap on the
  strength of having *asked* would be a use-after-free in exactly the window
  E5's destroy path learned to wait out. The rollback resolves the sidecar
  name again after the kill and releases everything only when it no longer
  resolves; on a deferred kill the regions stay **held** — a bounded leak is
  the right side of that trade, and the deferred teardown still returns the
  sidecar's own frames. Pinned by the sim's `kill_deferred` negative control.
- **Finding — the question broke twelve host tests at link time.**
  `sys_sls_placement_can_hold` is the first cap.c code to reference
  `partition_get_frame_quota`/`_usage` and `frame_pool_free_count`, and no
  cap.c host test links `frame_pool.c` (they fake the allocator).
  `tests/process_host_stubs.h` gained weak **settable** stand-ins — quota 0 =
  unlimited (the kernel's own BSS-zero default), usage 0, free count
  effectively unbounded — so every existing test's world is unchanged and the
  P3 section of `cap_create_sidecar_host_test.c` drives the knobs directly.
- **What the question deliberately does not answer.** Absent and paused
  partitions keep their own named refusals at the first alloc/create (the
  order 4a2 fix); a tenant reading "insufficient frame capacity" for a paused
  partition would be a lie about which gate said no. A caller without
  `sidecar_authority` gets 0 ("cannot hold") rather than an error — the same
  refusal path — because a probe that reports node free-frame counts to
  arbitrary ring-3 programs is reconnaissance it does not need.

Proven by: six new host tests in `user/init/src/env_manager.rs` (48 green) —
the pre-flight's question asked exactly once about the target partition with
the whole demand; quota and node refusals allocating **nothing** (no region
attempted, no sidecar created, no sidecar registered) and answering
`ENV_ERR_CAPACITY` by name through the request path; the mid-way shortfall and
the second-sidecar rollback freeing exactly what was taken; and the deferred-
kill negative control freeing nothing; plus `tests/cap_create_sidecar_host_test.c`
section 18 driving the real `sys_sls_placement_can_hold` — quota arithmetic
exact at the boundary, node arithmetic one-short/one-exact, the zero-frame
edge, the authority gate, and the across-every-scenario clause that the
QUESTION reached the allocator not once.

**The live proof, and the finding it caught.** The rebuilt ISO's
`env_create_boot_check.sh` gained Phase 8: a third, live partition capped at
128 frames over HTTP (`POST /api/partition/quota`), where the create must be
refused **by name** — "insufficient frame capacity", which only the
pre-flight can say — with target and creator both moving +0 frames and no
process placed; then the arm's own control, lifting the cap so the very same
partition must accept (index 1), making the quota's arithmetic the only
variable between refusal and success. The `capacity-unchecked` tooth
withholds the cap so the arm must go red, and the smoke's third tooth arm
requires the guard to fail naming that arm.

- **Finding — the first live run named the wrong gate, and the fix is
  deferral.** With the absent-target arm of Phase 7 (still asserted by its
  kernel line), the pre-flight answered "cannot hold" for partition
  0xFFFFFFFF — a true statement about a partition that does not exist, and
  exactly the lie cap.h's contract forbids ("insufficient frame capacity"
  for an absent or paused target). The query now DEFERS on any target its
  state gates own — out of range, absent or paused answers "can hold", so
  the allocator's own `CAP_EINVAL`/`CAP_EPERM` gate still refuses, by name,
  before the first frame. The host test pins all three deferrals; the live
  rerun shows the absent arm back on its `CAP_EINVAL` line and the capacity
  arm on its own name.
- **Finding — the worktree ships seeded objects, and the digest gate does
  not see them.** The first live run refused every capacity create as
  "unknown": `sidecars.cpio`'s digest gate fired correctly on the stale
  archive, but the kernel OBJECTS (`net/http.o` embedding
  `env_status_name`) were seeded from a build of older sources and the
  Makefile has no header-dependency tracking, so `env_proto.h`'s new case
  never reached the ISO until a full `make clean` rebuild. The digest gate
  guards the archive; nothing guarded the objects. Any future
  header-semantic change (a status name, a wire constant) needs the same
  clean rebuild on a seeded tree — worth a dependency-tracking fix in its
  own right.

### Findings addendum — what landed, increment 2 (the transfer)

The payload half of "checkpoint, transfer, restore, resume": `kernel/env_migrate.c` moves one
environment's whole footprint — its P1a descriptor plus every byte that
descriptor names (the three regions' frames and the P1b durable extent's
pages) — over the DSPP migrate wire family the streams already ride, opcodes
**18–21** (11–14 belongs to the checkpoint family, which shares the magic and
would have swallowed the frames), the descriptor crossing WHOLE in the BEGIN,
page numbering derived from the record on both sides with the destination
refusing a declared total that disagrees, staging into partition-charged
frames, never persisting what is staged. It killed nothing, freed nothing and
dropped no record, so a short transfer left the environment exactly where it
was. It deliberately did not wire itself in — that is this increment — and it
repaired a guard this branch left red (`env_storage_durable_check.sh`'s S10
ceiling, now 327 with S10f asserting 327 *is* dispatched). Full summary:
commit `8379e82` / PR #74; proofs: `tests/env_migrate_host_test.c`, 107 checks.

### Findings addendum — what landed, increment 3 (the environment inside the critical section)

The step is now reachable from the only place that matters. `partition_migrate()`
gains **Step 3c**: pause → lease step-down → streams → contexts → **environments**
→ ownership handoff, so an ownership handoff can no longer precede its
environment's payload. That is `P3_TOOTH=ownership-first`, closed at the
environment level — the same partial-transfer defect this function already
paid for once with a page-0 refusal, reproduced deliberately so its regression
test is environment-shaped.

- **The whole step is one call, deliberately.** `env_migrate_partition(partition_id,
  dest_node_id, &expected, &moved)` (`kernel/env_migrate.c`) counts the
  partition's records, freezes them, sends each through the real
  `env_migrate_send()`, and thaws on every path out. partition.c grows **one**
  new external reference rather than a loop over another module's table, so
  every host test that links it needs **one** stand-in — 24 files, the exact
  judgment call `stream_migrate_send_partition()`'s and
  `simi_ctx_migrate_send_partition()`'s stubs already made (and the same
  cascade, for the same reason: a new dependency of a widely-linked file).
  The status-name mapping is `static inline` in `env_migrate.h` for the same
  reason — a pure string map must not become a second dependency.
- **Finding — one test was invisible to a search for the phrase "Build and
  run:".** The first sweep over the suite found 23 files linking partition.c
  by that phrase and stubbed them; `tests/partition_host_test.c` says "built
  and run directly:" instead, was never seen, and the host suite went red at
  the linker on `undefined reference to env_migrate_partition` — a missing
  stub reported as a compile error, in the runner that builds the tests. The
  fix is in the DETECTION, not the symptom: the guard's clause I (and this
  note) now read the build command exactly the way `tests/run_all.sh` does —
  from the `* gcc` line, backslash continuations included — so the guard sees
  what the runner builds, and its tooth now removes *that* file's stand-in.
- **The abort is the streams' own rule, applied wholesale.**
  `envs_rc != 0 || envs_moved < envs_expected` returns **before** Step 4, with
  the pause state restored and nothing reclaimed: fewer confirmed than exist is
  a failure, not a partial success, and 0 of 0 stays a success — the distinction
  `streams_expected` was introduced to buy.
- **Finding — the quiesce had to become partition-scoped.** The capture's
  `env_ckpt_quiesce_for_capture()` freezes *every* partition holding a record.
  Inside one tenant's migration that is a blast radius nobody asked for: the
  neighbours would be paused for the length of a wire transfer. New
  `env_ckpt_quiesce_partition()` / `env_ckpt_release_partition()` freeze ONE
  partition and **require that the caller already holds it paused** (Step 1
  does), so there is exactly one pause in play and the release never has to
  guess whose it was. They mark `QUIESCED | PARTITION_PAUSED` — the first so
  the destination's restore rule 2 accepts the record that crossed, the second
  because a successful migration always leaves the partition paused (resume
  happens over there) — and they remember each record's **prior** flags and
  restore them bit for bit, so an abort leaves no record claiming a freeze that
  is already over. The full-table quiesce is untouched; checkpoint captures
  still do what they did.
- **Finding — a record table is not a list of live environments.** A record
  this boot has not replayed yet describes an environment that does not exist
  here: its region bases are last boot's placement, and reading them would
  push whoever's bytes now occupy those frames across the wire under this
  environment's name. New `env_ckpt_restore_pending_record()` gates the step
  **before** anything is frozen or read, refusing
  `ENV_MIGRATE_REFUSE_UNREPLAYED`. What it deliberately does not catch is
  named in its own comment: a record whose replay was *refused* leaves the
  pending set while staying in the table, so it reads as live. Telling those
  apart needs a liveness mark the record layer does not carry — P1a's gap (the
  refused record's destination), not something a wire protocol can infer.
- **The default posture is untouched.** The step sits behind
  `cluster_local_node_id() != 0`, exactly as the stream and context steps do:
  with no real cluster identity there is nowhere to send an environment, so
  the step is a no-op and ownership moves as it always did. Proven by a
  *counting* stub in `partition_migrate_default_path_host_test.c` — the default
  path asserts the step never runs, rather than assuming it.
- **Deliberately NOT here, each its own increment:** the source environment's
  teardown (still open) and the destination's restore (landed as increment 4,
  addendum below). On success the source's
  records still describe environments that have moved and
  `partition_reclaim_all_frames()` still reclaims the partition's frames as it
  did before this increment — the roadmap's own "orphaned or destroyed by
  teardown" boundary, named here instead of being quietly changed by a
  transfer half.

Proven by: `tests/env_migrate_host_test.c` **Phase F (31 new checks, 138
total)** against the real sender, record layer and wire — refused-not-paused
and refused-not-yet-replayed each cost zero frames and left the flags alone;
the happy path sending **2 × 6 pages**, the crossing descriptor carrying
`QUIESCED | PARTITION_PAUSED` although the table had neither, both slots
complete after the bytes are replayed through the real receive path as node 2,
and the flags **bit-for-bit restored on the failure path too**; 0 of 0
succeeding; `PARTITION_SYSTEM` refused by name. `tests/partition_migrate_phase6_host_test.c`
**Scenario 7 (72 checks total)** asserts, at the instant the step runs, that
the partition is **paused** and the owner is **still this node** — then that a
1-of-2 shortfall fails the migration, leaves ownership local, reclaims nothing
and restores the pause state — and `partition_migrate_default_path_host_test.c`
**(11 checks)** asserts the step never runs without a cluster.

**The guard, and its teeth.** `tests/env_migration_check.sh` (source-only, ten
clauses, runs in CI's verify job) checks the wiring a runtime test cannot see:
the call exists, sits **before** `partition_set_owner_node()`, is followed by a
non-constant abort that returns before the handoff, stays behind the cluster
gate; the step freezes once and releases on **both** paths out; the freeze
marks both bits and the release restores the prior flags; the pending gate is
declared, defined and called; every host test linking partition.c satisfies
its new dependency; and both orchestration tests still *assert* what they own.
`tests/env_migration_check_smoke.sh` proves all ten bite — 12 teeth, including
`ownership-first` (the step moved after the handoff), the neutered abort (the
live-cluster defect verbatim), the never-released freeze (the leak-unpause
shape), and the vacuity control on the untouched tree.

### Findings addendum — what landed, increment 4 (the destination's second act)

Increment 3 sent the bytes; this one brings them back to life. A complete
staging slot now becomes a **live environment** on the node that owns the
partition — `env_migrate_restore(partition_id, index)` in `kernel/env_migrate.c`,
driven by a new `env_migrate_tick()` on the BSP sweep in `net/http.c` (with the
heartbeat and checkpoint broadcast: it TRANSMITS nothing itself, but it calls
`env_service_create`, and only one core may run a create). The order is the
boot replay's own — resume → create → identity → settle → wait-parked →
repause → pour — with the bytes sourced from the staging slot instead of NVMe,
and one gate the boot pass never needs:

- **Ownership gates the create, before it.** `partition_get_owner_node() !=
  cluster_local_node_id()` refuses `NOT_OWNER` before a single record is
  stored: a follower rebuilding an environment for a partition served
  elsewhere would be a second, unaccounted tenant charging its frames — the
  mirror image of increment 3's "ownership must not move before the payload".
  One gate, before the side effects, so the refusal costs nothing.
- **The slot-retention rule, stated once and obeyed by every refusal.** The
  staged slot is **kept** while the state is exactly what it was before the
  attempt (nothing stored, nothing created — a retry can succeed where this
  attempt could not) and **dropped the moment the environment exists** (a pour
  refusal leaves the empty replay standing; retrying a pour into a live
  environment would double-create). `env_migrate_recv_drop()` is the last thing
  a successful restore does and the last thing an unappliable refusal does —
  never the first. Finding: the NVMe-extent write is the sharp edge — the
  environment is live and its memory holds the contents when the durable half
  fails, so the slot is dropped *not* retried (a retry would re-run the CREATE
  that now collides), and the refusal names the IO.
- **A return migration is handled, not dodged.** This node already holding an
  environment at that identity means the source-teardown half (increment 3's
  named gap) has not landed: the staged bytes just crossed WITH ownership
  returned, so the local instance is the stale one — it is ended and what
  crossed is rebuilt, over a `NOENT`-as-good destroy.
- **Placement is validated WHOLE before a byte.** Every staged region must
  land at the base the staged record names and the durable extents must agree:
  the sidecars' MEM regions are identity-mapped and their heaps hold RAW
  POINTERS into them, so a fresh environment that landed anywhere else could
  not receive these bytes without every interior pointer dangling silently.
  Finding — the pause dance had to be *straight-line*: E4's placement gate
  refuses a paused target, so a record recorded as paused is resumed for the
  length of the create and put back by the ledger, and between the first
  resume and the repause there is **no `return`** — a bail there would leave a
  partition the record said was frozen RUNNING.
- **The record's PAUSED flag is honoured.** A successful migration always
  leaves its partition paused (resume happens over here, by an operator, after
  the bytes landed) — `ENV_CKPT_FLAG_PARTITION_PAUSED` crossed the wire for
  exactly this and now gates a `partition_pause()` on the destination.
- **Finding — a live environment at the identity changes the gate's meaning.**
  `env_ckpt_restore_pending_record()` (increment 3's gate) now has a second
  caller: the restore's own stale-instance check must ask whether this boot
  still OWES the record a replay before treating it as a live environment to
  end. One gate, two questions — "may these bytes be read" at the wire and
  "does something already live here" at the create.
- **Deliberately NOT here:** the source's teardown. Increment 3's boundary is
  unchanged — the source's records still describe an environment that has
  moved, and this increment's return-migration path is what makes that
  observation safe rather than silently fixed.

Proven by: `tests/env_migrate_host_test.c` **Phase G (59 new checks, 193
total)** against the real staging table, the real record layer and the real NVMe
fake — the happy path pouring every region byte-for-byte and dropping the slot;
every refusal checked for BOTH halves of the retention rule (kept where a retry
could succeed, dropped where the environment exists); placement and extent
mismatches refused before a byte; the identity mismatch ending what the create
made; the attempts cap actually bounding the sweep; and the PAUSED flag pausing
the destination's partition.

**The guard, and its teeth.** `tests/env_migration_check.sh` grows from ten
clauses to **eighteen (A–R)**: the restore is driven by the BSP sweep; the
ownership gate precedes the create; both pre-create refusal paths drop the
stored record while the slot survives them; the placement rule is decided
before the first byte of the pour; the PAUSED flag gates the pause; the sweep's
retry is bounded (period, cap, cap actually checked); and the host test still
drives both the restore and its sweep. `tests/env_migration_check_smoke.sh`
proves all eighteen bite — **20 teeth**, including the worth-stating pair
`L` (the ownership gate moved after the create — a follower rebuilds someone
else's tenant) and `N` (a pre-create refusal releasing the staged slot — bytes
a retry could still have used), plus the vacuity control. Finding: tooth H's
mutation had to widen from the send-side gate call to *every* call — the
restore added a second call site, and a tooth that left any one in place left
clause H green and proved nothing.

### Findings addendum — what landed, increment 5 (the destination's word, then the teardown)

The boundary increments 3 and 4 both named is now closed, and closed
**gated**: the source tears its half of a migrated environment down only on
the destination's own word that it is live with its contents there — never
on the bytes having been sent. One sentence governs the whole increment:
**no record may describe an environment that moved.**

- **The pull, not a push.** `DSPP_MIGRATE_ENV_LIVE_REQ`/`LIVE_ACK`
  (opcodes 22/23, `net/dspp.h`/`net/dspp.c`) — a header-only question the
  source asks on the sweep's period, a header-only answer the destination
  gives from its own state. A lost question or a lost answer is recovered
  by the next period; nothing blocks, nothing is assumed.
- **The word is grounded in a marker set exactly once.**
  `env_migrate_live_query()` answers live only when this node owns the
  partition, the record stands here, its restore has settled, AND the
  pour's success tail set the restored marker — one line, the only one.
  Staged is not restored: confirming from a completed slot would have the
  source destroy the only complete copy. A refused pour leaves a record
  behind and can never confirm anything.
- **Arming is per environment, at the end of a fully-confirmed send.**
  `env_migrate_partition()` arms one confirmation per environment after
  every payload crossed (partial steps are aborts, and aborts arm nothing —
  their environments are still here, so their records must stay). Entries
  disarm themselves when ownership comes back locally (aborted handoff, a
  return) or when the partition no longer exists (that partition's own
  teardown owns its records and frames). A full confirmation table leaves
  the un-armed environment's records standing, named in the log — every
  overflow degrades toward records that describe a moved environment
  (countable, an operator's to finish), never toward a teardown without the
  destination's word.
- **The teardown runs by name, bounded, with `NOENT` as good.** The
  confirmed branch calls `env_service_destroy()`; the destroy path's own
  bookkeeping drops the record (exactly like the HTTP destroy route), a
  `NOENT` answers just as well (the record outlived its environment — drop
  it), and no-reply or a refusal keeps the record with at most five
  attempts before the sweep stops. An environment whose fate is unknown is
  **described, not forgotten** — and countable:
  `env_migrate_pending_count()` is the same contract
  `env_migrate_staging_count()` gives the staging table.
- **The frames wait for the records.** partition.c's Step 5 reclamation is
  deferred while `envs_moved > 0`: the source's environments' sidecars are
  still running on exactly those frames, and reclaiming first would put two
  owners on one frame — the defect this increment exists to remove,
  reproduced in miniature. The confirmation walk completes the
  reclamation when the partition's LAST record leaves this node's books,
  gated on a records-remain check so a never-armed or never-succeeded
  sibling keeps its frames with its record. Finding: the walk shares the
  restore loop's period deliberately — one node can be both roles in the
  same sweep (a partition migrating out while another arrives).
- **Finding — the reset clears BOTH tables.** A pending confirmation (or a
  restored marker) surviving `env_migrate_reset()` could let a later walk
  tear down an environment nothing ever sent in this boot — the defect in
  miniature, on the test's own seam.
- **Deliberately NOT here:** the live-cluster arm — a real two-node run of
  the confirmation and the teardown — which needs a running cluster, same
  honesty as increments 3 and 4's live clauses.

Proven by: `tests/env_migrate_host_test.c` **Phase H (45 new checks, 238
total)**. The destination half: a record alone, a partition owned
elsewhere, a staged slot, a real restore, an unsettled restore and a gone
instance — each answered not-live except the restore. The source half: a
refused send arms nothing; a success arms exactly one per environment;
"not yet" keeps the record; the word ends THAT environment by name with
its unconfirmed sibling intact; ownership returning disarms without a
destroy; the completion reclaims exactly once, for that partition, after
the last record; a vanished partition ends its entry; 60 polls then stop;
5 destroy attempts then stop with the record still standing and countable.

**The guard, and its teeth.** `tests/env_migration_check.sh` grows from
eighteen clauses to **twenty-five (A–Y)**: the confirmation pair wired end
to end; the restored marker set exactly once; the teardown gated (the
not-confirmed branch before `env_service_destroy()`, local ownership
disarming); the arm exactly once with pending confirmations countable; the
reclamation deferred and completed behind the records-remain gate; every
`net/dspp.c`-linking host test standing in for the new pair (clause I's
link class, for dspp.c); and the host test still driving the confirmation.
`tests/env_migration_check_smoke.sh` proves all **25 teeth** bite, including
the worth-stating `U` (the not-confirmed gate deleted — a teardown
reachable on the arm alone, this increment's defect verbatim), plus the
vacuity control. Finding: tooth S's first draft had the guard matching the
function NAME in `dspp.c` — and `dspp.c`'s own comments mention it, so the
guard reddened on nothing; the clause now names the call with its
arguments, and comments are not wiring.

### Findings addendum — what landed, increment 6 (the round trip, proven live)

The confirmation and the teardown were correct in the tree and unproven on
metal. This increment ran them against a **real 2-node cluster** and fixed
what the run exposed — three defects, each only visible with two live nodes:

- **`env_payload_wait_parked()` now pumps services while it waits** (the
  same `microkernel_service_poll()` + `smp_uniprocessor_tick()` + yield
  pair `env_service_rpc`'s loop makes). The wait was starving the very drain
  whose `[env-id]` announcement it waits for on a uniprocessor boot, so
  every live restore REFUSE_PARKED at 30 s with `announced=0` — a gate that
  could never open, on the only posture where it matters.
- **`env_migrate_in_flight()` defers failover death detection across a
  migration's whole life**: a young staging slot, an armed confirmation, and
  a RESOLVED tail set at both resolution points. The resolved tail was the
  one the live run forced: without it the source declared the still-blocked
  destination DEAD one sweep after its own teardown completed. Proven live:
  `declaration DEFERRED: an environment migration is in flight` at 304
  ticks.
- **`dspp_env_migrate_rx()` logs every refusal it answers with a non-zero
  ACK**, so `partition_migrate()`'s "check node N's log" promise is one this
  family can now keep.

And one **operational** fix that was invisible until a live check needed it:
`run-cluster.sh` boots nodes UNIFIED with a digest-guarded `sidecars.cpio`.
Without it cluster nodes are kernel-only and the environment manager does
not exist (`env create` → no reply) — invisible until a live confirmation
check needs an environment at all.

New `tests/env_migrate_confirm_live_check.sh` runs the whole round trip
against a live cluster: migrate byte-confirmed, destination rebuilds LIVE
from the wire, source tears down ONLY on the destination's word,
reclamation completes, registries cross-checked.

---

### Findings addendum — what landed, increments 7 and 8 (the idle-observer arm)

Increment 6's live runs surfaced a failure that no 2-node shape can produce,
and it was written up first as a finding
(`docs/AeroSLS-Idle-Observer-Adoption-Finding-v0.1.md`) before being fixed
in two increments.

**The finding.** The consensus leader is the ONLY heartbeat transmitter, and
it transmits from the BSP sweep — the same context this roadmap sanctions
blocking work on (the env-create RPC wait, the synchronous migrate send). A
blocked leader stops beating and cannot resume until the work ends.
Followers' election timers expire, a new leader takes the term the blocked
node cannot contest, and its failover peer entry for the old leader — fed
ONLY by the leader's heartbeat stream — crosses `FAILOVER_DEAD_TICKS`. The
new leader declares the old one DEAD and `failover_recover_from()` adopts
every partition in its last checkpoint: ownership flipped, synthesized
names, no data. **Every node alive; the migrating environment silently
destroyed.** Observed twice on real 3-node TCG clusters, roles rotated.

`env_migrate_in_flight()` can never see this on the declarer: the migration
knowledge lives on the blocked node, and the source's own inputs arm only at
destination-side BEGIN and teardown-confirm — after the block that matters.

**Increment 7 — the migration-intent announce (option A of the finding).**
Before the blocking phase begins, the node about to block makes that work
CLUSTER KNOWLEDGE: `env_migrate_intent_begin()` stamps a local gate (so its
own declarations defer too) and TXes `DSPP_MIGRATE_ENV_INTENT`, a broadcast.
Every peer latches it through `env_migrate_recv_intent()`, clamped to
`ENV_MIGRATE_INTENT_GRACE_TICKS`, and `failover_tick()` asks
`env_migrate_peer_in_flight(peer)` — by peer IDENTITY, before the local
gate. The RX runs BEFORE the point-to-point self-filter, because the
announce is the one broadcast in this family (`node_dest_id` 0) and the
filter would otherwise drop every peer's announce on every node, reinstate
the exact blind spot it exists to close. The destination announces too,
before each restore attempt: the restore blocks THIS node's sweep, and on
the leader that sweep carries the only heartbeat stream. Bounded like every
other face — the budget expires and a genuinely dead peer is declared.
**The announce now RELEASES (2026-10-14).** The budget covering the whole
blocking phase meant the latch outlived the phase by up to 300 s — found
live when a leader killed right after an env create stayed undeclarable
DEAD (the adoption guard's wait lost to it). `env_migrate_intent_end()`
clears the local gate and re-broadcasts the same announce with budget 0 —
the one value the clamp made unrepresentable — and every announcing route
calls it on every exit (`partition_migrate()`'s three refusals + success,
both env-create routes, the sweep's restore loop per attempt). The budget
stays the backstop for a lost release; clause AG of
`env_migration_check.sh` holds the wiring.

One adjacent wire defect surfaced while proving it and is fixed in the same
increment: `env_migrate_recv_begin()` refused ANY BEGIN for an
already-staged identity (rc=5), so the documented retransmission model
failed exactly when a fresh-boot link needed it — a slow BEGIN ACK made the
source re-ask, the refusal abandoned a healthy migrate mid-send ("a page
never confirmed"). A re-BEGIN carrying the same identity-derived
`transfer_id` AND the same record is now answered READY without touching
the slot; a different record stays refused.

**Increment 8 — the silence arithmetic saturates.** One `declaration
DEFERRED` line was observed printing `silent 18446744073709551606 ticks`
(−10 unsigned). The cause is structural, not a clock bug: `now` is read
once at `failover_tick()`'s BSP-sweep call site while `last_seen_tick` is
stamped by the heartbeat RX path (timer ISR / AP core), so a heartbeat
landing between those reads legitimately leaves `last_seen_tick > now` and
the unsigned subtraction wraps past `FAILOVER_DEAD_TICKS`. Fixed with a
saturating subtraction, deliberately NOT the `last_seen = now` rewrite the
finding first suggested — clamping the stamp would discard the fresher
evidence for every later sweep.

**Live proof** (fresh-disk clusters, the confirm guard): 3-node PASS twice,
every face firing — both observers latched the announces and printed
`declaration DEFERRED`, the destination deferred by name, the restore
completed LIVE, teardown ran only on the destination's word — and the
original 2-node shape still PASSes. `tools/repro_observer_incident.sh`
reproduces the incident on demand and now PASSES on that shape;
`tools/regress_2node_confirm.sh` is the 2-node regression.

---

### Findings addendum — what landed, increment 9 (failover destination (b))

§7 scoped failover as "restore-or-refuse, never half", and §11 Q5 left the
destination open between (a) bulk-transfer surviving environments and (b)
restore from durable storage with the gap **reported by name**. This
increment implements **(b)**, chosen because the leader's checkpoint
broadcast is 24 KiB and one environment's regions are 5.5 MiB — (a) is not
a small extension of what exists, and (b) is consistent with `failover.c`'s
existing ownership-only posture.

Increments 1–8 are all MIGRATION: an operator asks for a partition to move
and the environment moves with it. The DEATH path was untouched by every one
of them, and Appendix B named the gap — "`failover_recover_from()` adopts
ownership only; a dead node's environments vanish with no name in the
report". That makes the REPORT the whole deliverable:

- **The tree carries the names.** `ST_NODE_ENVIRONMENT` (a new node type in
  `kernel/state_tree.h`) is emitted by `state_tree_build()` — one node per
  live `env_ckpt_table` record, attached to its partition, `entity_id` = the
  environment's INDEX (the name the console registry and every control-plane
  route key it by) and `size` = its region count. The payload deliberately
  does not ride here; carrying the NAME is what turns "the environment
  vanished" into "environment index 2 of partition 3 was not restored".
  `ST_MAX_NODES` 410 → 418; `ST_SERIAL_MAX` 16 448 B stays well inside the
  receiver's 24 KiB.
- **The adoption names what it did not restore.**
  `failover_recover_from()` walks those nodes and prints each environment
  of an ADOPTED partition as not restored, then a summary. Scoped to
  adopted partitions only: an environment in a partition this node did not
  adopt belongs to whoever owns that partition now, and naming it here would
  point an operator at state that is not this node's to restore — the same
  defect as silence, with better manners.
- **The gap is countable, not just logged.**
  `failover_last_unrestored_envs()`, reset at FUNCTION ENTRY so an early
  return (no checkpoint, wrong source, no partitions) reports zero rather
  than the previous recovery's count.
- **The format is version 2.** Bumped rather than extending v1 silently, so
  a v1 tree is REFUSED by name instead of being read by a parser that would
  silently drop the environment names — the exact vanish (b) exists to
  prevent. Safe because the serialized form crosses only the live cluster's
  broadcast, never NVMe: `checkpoint_mgr.c` builds a tree in memory and
  never serializes it.

`tests/env_failover_check.sh` (7 clauses, B–H, plus an A that refuses to
evaluate when a file is missing) and its smoke (9 teeth, including
the worth-stating **`silent-failover`** — the walk deleted, the adoption
still succeeds, nothing else notices) hold the wiring. Proven by
`tests/checkpoint_recovery_host_test.c` Scenario 4b (7 new checks): exactly
the environments of the ADOPTED partitions are named, the one in a
non-adopted partition is not, and a second recovery that adopts nothing
reports zero rather than the first one's count.

**Two findings from writing the teeth, both about vacuity.** First, clause
B originally asserted the bare identifier `ST_NODE_ENVIRONMENT` against
`state_tree.c` — which appears in that file's own comments and in the
serializer's `case`, so the clause held up after the emission itself was
deleted. This is the same defect the increment-5 addendum records for
tooth S ("comments are not wiring"); it recurred, and the clause now
anchors to the emission. Second, three of the first teeth did not actually
mutate what they claimed — one typoed the identifier, one renamed a line to
a string that still prefix-matched the guard's own lookup, and one deleted
a file-scope static initializer (harmless; statics zero-init) instead of
the in-function reset. All three reported "did not bite", pointing at the
GUARD when the fault was the MUTATION. `tooth()` now proves its mutation
landed (`cmp` against the pristine copy) before trusting the result, and
clause F's own lookup is anchored to the indented in-function line for the
same reason.

**Remaining boundary, stated rather than implied:** this is the REPORT half
of (b). The restore half — the survivor actually recovering the
environment from its own durable storage — is not implemented, and the
report says so in its own words (`restore it from its own durable storage
or recreate it`). Destination (a) remains the PASE-grade answer.

---

### Findings addendum — what landed, increment 10 (the console, pinned)

§7's bullet "the console and the terminal follow" was reported at one point
as a MISSING MECHANISM. Checking the tree showed it is not one — the
console is **inherited**, and what was missing was the PIN.

The chain: the destination's restore calls `env_service_create()`, the same
ENV_CREATE round trip the HTTP route makes; inside that round trip init
creates the POSIX sidecar, so `cap.c` registers its console from the
sidecar's own name (`aerosls.posix.<index>`) against `(partition, index)`;
and `env_service_create()` then binds the fresh `env_id` to it. Nothing in
`kernel/env_migrate.c` needs to name `env_console` at all, which is why a
grep of that file was the wrong test.

What was genuinely absent was any clause or tooth holding that chain — a
restore that stopped going through `env_service_create`, or a create that
stopped binding, would leave an operator's attach answering `no such
environment` on the destination for an environment that IS live there, and
no run where nobody attached would notice. Clauses **AD** and **AE** of
`tests/env_migration_check.sh` now hold both halves, with teeth **AD-a**
(the restore no longer goes through create), **AD-b** (create no longer
binds the `env_id`) and **AE** (the pour no longer waits for the
destination's own `[env-id]` announcement, so the injected bytes would
erase the very identity they are keyed by). The guard grew to **30
clauses** (B–AE) and its smoke to **34 teeth**, all biting, vacuity control
green.

Deliberately NOT done here, and named: §7's runtime arm — boot a cluster,
migrate, attach on the destination, and observe the stream — is still
unbuilt, as is the `console-unrebound` LIVE tooth. The E6 guard's
cross-partition witness covers the console registry's own scoping; what it
does not cover is a migration, and that live arm is the honest next step
for this bullet rather than more source clauses.

---

### Findings addendum — what landed, increment 11 (the restore half of destination (b))

Increment 9's addendum closed with its own remaining boundary stated
plainly: "the restore half — the survivor actually recovering the
environment from its own durable storage — is not implemented, and the
report says so in its own words". This increment implements it, and the
report stops being a blanket and becomes a per-environment OUTCOME.

The whole of the change is one re-classification of what "its own durable
storage" means, and where to ask it:

- **The durable store is `env_ckpt_table`, and the survivor already has
  its copy.** The descriptor records are P1a's record layer — written by
  `persist_environments()`, adopted at boot through `env_ckpt_adopt()`. A
  survivor's table holds the dead node's environments' records exactly
  when its own boot saw them (the shared single-level store's normal case;
  per-node stores simply have nothing, and the report names that reason
  instead of implying a restore happened). Nothing new persists anything:
  destination (b) was always "restore from what is already here", and this
  increment is the code that goes and looks.
- **The replay is the reboot's own driver, not a copy.**
  `env_service_restore_pending()` — the create round trip, the identity
  check, the pause ledger, the payload pour — is driven from
  `failover_recover_from()` for exactly the adopted partitions' records.
  The driver is reused because all four of those behaviours are already
  its, and a second copy would be a second place for them to rot. It runs
  on the BSP sweep, the same context the migration destination's restore
  already runs in.
- **Arming is per-record, because the boot-shaped form is a trap.**
  `env_ckpt_restore_begin()` arms EVERY record; calling it on a live
  survivor would replay the survivor's OWN healthy environments as if they
  were dead ones. `env_ckpt_restore_arm(partition, index)` narrows it to
  one pair, idempotent, refusing (return 1) to arm nothing.
- **The classification is exact, not inferred.** A settle and a refusal
  both clear the pending bit, so "still pending" alone cannot tell a
  restored environment from a refused one. The record layer gains a
  per-slot refused mark (`ec_restore_refused_slot`, set by
  `env_ckpt_restore_refuse()`, cleared by `settle()` and
  `restore_begin()`, SHIFTED by `drop()`), and failover classifies each
  armed record three ways: still pending (the replay never got to it —
  `env_service_restore_pending()` returns -1 when the environment manager
  is not up), refused (counted unrestored, named with the refusal's
  provenance), or restored. The admission gate is untouched: a record that
  was not captured frozen still refuses (UNQUIESCED), and the survivor
  names it — refusal over partial application, applied to the death path.
- **Two counters, both reset at entry.**
  `failover_last_restored_envs()` joins `failover_last_unrestored_envs()`:
  the first counts what came back, the second now counts what did not —
  whether for want of a record, an incomplete replay, or a refusal. Both
  zero on a recovery that adopts nothing.

The guard grew from 8 clauses to **13** (B–M: the arm, the driver, the
refused query, the counter's four properties, the tracker's three lines)
and its smoke from 9 teeth to **15**, the new sharpest being
**`refused-as-restored`** — the refused mark's one query deleted, so a
record the replay REFUSED is reported restored. That tooth exists because
the lie is only reachable in this pairing: before this increment there was
no restore to misreport, and the direction it errs in (a success that is
not one) is the direction no operator questions. Proven by
`tests/checkpoint_recovery_host_test.c` Scenario 4c (12 new checks over
four properties): a present record is replayed and counted restored with
the armed set scoped to the adopted partitions (the partition-9 witness);
a REFUSED record is counted unrestored, never restored; with no durable
record nothing is armed, the driver is not even called, and both
environments are named; and both counters reset at entry. Scenario 4b's
fixture now keeps the driver UNREADY, which is what makes its original
counts also the pin that an incomplete replay never reads as a success.

**What this increment deliberately does NOT claim.** The CONTENTS. The
replay brings the environment back — identity, sidecars, bindings — but
the bytes follow only as far as the environment's durable extent is
readable on this survivor, and that refusal is the payload layer's own to
print (`[ENV_PAYLOAD]`; the restored-count's summary line says so rather
than leaving it to inference). Destination (a) — the bulk transfer that
makes contents follow ownership itself — remains the PASE-grade answer and
is unchanged. The live arm it owed landed with increment 12's addendum
below (2026-10-16: the boot-a-cluster, kill-a-node, watch-it-come-back
run, end to end, on a fresh 3-node TCG cluster).

### Findings addendum — what landed, increment 12 (the operator's pause crosses the boundary; the live arm lands)

Two halves, each earned by a live run of an arm this phase already had.

**The defect (found 2026-10-14, by increment 10's console live arm).** An
operator-paused partition that migrated came back RUNNING on the
destination: `partition_migrate()` captured nothing about who had paused
the partition, and the staged records' "come back paused" flag was
stamped from the migration's OWN freeze — the very freeze this function
releases at its end. So the operator's intent was dropped at the
boundary while the migration's internal state crossed as if it were one
— an operator's pause never survived a move, and a destination honouring
the wrong half left a tenant live by the records and dead to the
scheduler.

**The fix.** `partition_migrate()` captures `was_paused_before_migrate`
BEFORE its own pause and threads it into `env_migrate_partition()`: the
staged records may carry "come back paused" only when the operator's
pause predates the move. Held by clause AF of `env_migration_check.sh`
(the argument must reach the environment step; its smoke tooth deletes
the threading) and pinned on both halves in `tests/env_migrate_host_test.c`
— the migration's own freeze does NOT cross (F3a), the operator's pause
DOES (F3b).

**The live arm (increment 11's owed run) landed the same week, and its
first runs earned their keep.** `tests/failover_adoption_live_check.sh`
grew the environment arm: an environment created on the leader inside its
own partition, a gate that a checkpoint carrying its NAME reached both
survivors, then the kill — followed by the survivor's declare → adopt →
the step-7.5 lines that NAME the unrestored environment and classify it
(`no durable record for it on this survivor; recreate it`), with the
resurrect/migrate-back/lease/service choreography unchanged behind it.
Live 2026-10-16 on a fresh 3-node TCG cluster: PASS end to end, and the
path there was the honest one — the env create (the longest synchronous
command in the system) churned leadership twice on that loaded host, the
guard diagnosed the lost precondition (`node N is no longer LEADER`) and
re-ran itself against whoever led now (restarts 1 and 2 of 3; a leader
that lost its role never puts the env name into another broadcast, because
the env lives on ITS tree), and the warm third run carried the whole
choreography through, classification line included. Its teeth are 29,
proven on every push by `tests/failover_adoption_live_smoke.sh` (3 fake
nodes); the churn and self-restart paths are live-only and named as the
smoke's one gap.

**A third defect, found by the live runs and fixed in the same
increment.** The environment store's restored directory carried LAST
boot's `region_base` — a frame address this boot's pool re-hands to
whoever attaches next, which `es_find_base()`'s first match then resolved
to the STALE entry: found live (2026-10-15) as a 12 241-line
restore-denial loop ending in `REFUSE_PARKED`, which killed the migrate's
destination before its environment ever announced. `env_storage_after_restore()`
surrenders every restored placement at the load — identity, extent and
occupancy kept; only the claim on an address this node no longer holds —
called by `persist_restore_all()` right after the table is read. Held by
clause S3i of `env_storage_durable_check.sh` (order-in-the-load + the
surrender) with its `stale-region-base` smoke tooth, and pinned by
`tests/env_storage_host_test.c`.

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

P1a is unblocked and is the thing E7 waits on, so it starts first. P2 has no dependency on P1 and should run **in parallel** — it is the phase most independent of the others, and serialising it behind persistence would idle a second pair of hands. §6 breaks it into four increments — the wire, the outbound path, the listener and the descriptor's line, and the measured bounds — each carrying one of the guard's teeth, so the parallel lane starts at a named first PR. P1b follows P1a because its descriptor field is P1a's. P3 starts only once P1a, P1b and P2 are all in, because every one of them is part of its payload, and a migration that is missing one of the three is exactly the "two nodes that disagree" failure the phase exists to prevent.

| Order | Phase | Gate to leave it |
|---|---|---|
| 1 | P1a — checkpoint/restore | an environment survives a reboot with its file and its shell variable, via its own console |
| 1 (parallel) | P2 — networking | two partitions, one throttled and one not, and the unthrottled one keeps serving |
| 2 | P1b — durable storage | a file larger than 71 168 bytes survives a reboot and is charged to the partition |
| 3 | P3 — placement/migration/failover | an environment moves with its data, its console and its lease, or is refused by name |

Step 2's three increments have landed: the format layer — the writable aerofs v2 — first (§5), then the durable device itself — the extent band, the persisted directory and the first-touch quota charge (§5's second-increment addendum), then the boot evidence itself: `tests/env_storage_durable_check.sh --live` runs in CI's `kernel-guards` job — the build host that has the ISO — re-making the gate on every push (write past 71 168 bytes, reboot, re-read through the environment's console, quota re-charged, over-quota refused by the quota's own error), with the wiring pinned by the guard's S13 clause (§5's third-increment addendum). Step 1's gate — an environment survives a reboot with its file and its shell variable, via its own console — is now measured rather than owed: the payload increment (§4's sixth) made the two CONTENTS clauses B10/B11 in the guard that already re-made the descriptor half, and the gate closes on that arm's first green live run. The payload's first
CI run then found the restore path pouring onto a freshly replayed sidecar
blocked mid-boot; §4's announce-gate repair closes that, and P1b's live arm
re-makes it on every push — the identity wait it runs before clause L16 reads
an environment that finished its own boot.

E7's own gate — *"a static binary doing the first user's actual work, running natively, surviving a checkpoint/restore cycle"* — becomes reachable at the end of step 1 for the first half and the end of step 2 for the second, and E7's deliverable list (`AeroSLS-Linux-ABI-Shim-Design-v0.1.md` §10.4) is already written to report those halves separately rather than blur them.

---

## 11. Open questions for review

1. **P1b: is an environment's storage *a stream*, or its own LBA region?** *(Settled by P1b's second increment, §5: a **dedicated per-environment LBA extent band** — streams are fixed 64 MiB slots deliberately excluded from the storage quota, so a stream-backed store would get persistence at the cost of the phase's central requirement; the band reuses persist.c's checksummed-region story without inheriting a stream's semantics, and its extent is exactly the region it mirrors, page for page. The reasons are recorded in §5's second-increment addendum.)* A stream gains migration, quota and persistence for free (`stream_persist_directory()`, `stream_migrate_*`) but carries a stream's own size and directory semantics; a dedicated region is simpler to size per environment and duplicates the persistence story. This is the same class of choice v0.1 §14 Q2 left open (ramdisk sidecar vs in-process ramfs), and it was settled the way this question asked — by implementation, with the reasons written down.
2. **P1a: is quiescing the whole partition the right granularity, or should an environment pause alone?** Today `partition_pause()` is the only pause mechanism, and it stops the tenant's other work too. If environments are the unit of checkpointing, a per-environment pause may be the correct primitive — but it is a new scheduling concept and it should be argued for rather than assumed.
3. **P1b: does the durable format replace aerofs-lite or extend it?** *(Settled by P1b's first increment, §5: the rule is implemented as **read v1, write v2, refuse the rest by name** — v2 is a version bump, no existing image is rewritten, and the rootfs builder deliberately stays v1.)* The rule was written down before it was implemented, as this question asked; the durable region it is for landed with P1b's second increment (the extent band), and its reboot evidence is re-made in CI on every push since the third.
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
| aerofs-lite v1 is read-only in format (512 B blocks, 139 blocks per file); v2 is writable (266 blocks, 136 192 bytes) and the superblock's version — not a mount flag — decides | `user/vfs/src/aerofs.rs` — `AEROFS_VERSION_V1`/`_V2`, `MAX_FILE_BYTES_V1`/`_V2`, `parse_superblock_refusing()`, `format_v2()` |
| A tenant store is a dedicated per-environment LBA extent band, sized exactly like the frame region it mirrors, and it cannot collide with the stream or row-store pools | `kernel/env_storage.h` — `ENV_STORAGE_DATA_LBA_BASE` 1114112, `ENV_STORAGE_EXTENT_SECTORS` 2048, `ROWSTORE_LBA_BASE`; derived by `tests/env_storage_durable_check.sh` clause S1 |
| The directory is persisted through persist.c's own machinery (checksum, torn-write, pending bit 17), re-charges occupancy at boot, and refuses a foreign snapshot by name | `kernel/persist.h` — `PERSIST_ENVSTOR_HDR_LBA` 7720/7728, `PERSIST_MAGIC_ENV_STORAGE`, `PERSIST_PEND_ENVSTOR`; `kernel/persist.c` — `persist_env_storage()`, the restore block; `kernel/env_storage.c` — `env_storage_boot_reset()` |
| Storage quota is charged to the store's OWN partition before any NVMe byte, with the denial printed by name and the pages reverted | `kernel/env_storage.c` — `es_charge_to()`, `[ENV-STORAGE] quota denied`; `kernel/storage_quota.h` — `storage_page_reserve()` |
| The durable surface crosses the wire as four gated syscalls (323-326) and named statuses, and the tenant sees EDQUOT not EIO | `kernel/cap.c` — `sys_sls_env_storage_*`; `kernel/env_proto.h` / `user/proto/src/env_proto.rs` — `ENV_ERR_QUOTA` 7; `user/proto/src/lib.rs` — `RD_ERR_QUOTA` 9; `user/vfs/src/errno.rs` — `EDquot` 122 |
| The kernel module is executed on the host, and the guard's teeth are proven (15 teeth: the four §5 tooth names plus the third increment's S13 wiring teeth) | `tests/env_storage_host_test.c` (39 checks); `tests/env_storage_durable_check.sh` + `tests/env_storage_durable_check_smoke.sh` |
| The reboot evidence runs in CI on every push — kernel-guards invokes the live arm after its ISO build, and the guard reddens BY NAME if that wiring is deleted or reordered | `.github/workflows/ci.yml` — the "P1b durable region live boot" step; `tests/env_storage_durable_check.sh` clauses S13/S13b, teeth B/B1/B2 in the smoke |
| The payload rides a slot directory over an 8 MiB-slot bulk band placed after P1b's extent band, separate from the small-record descriptor | `kernel/env_payload.h` — `ENV_PAYLOAD_SLOT_SECTORS` 16384, `ENV_PAYLOAD_DATA_LBA_BASE` 1130496; `kernel/persist.h` — `PERSIST_ENV_PAYLOAD_HDR_LBA` 7744 / `..._ENT_LBA` 7752 |
| The capture runs inside the frozen window and the pour runs after the repause with no yield, and a moved placement is refused before a byte moves | `kernel/persist.c` — `(void)env_payload_capture_all();` between the region commit and `env_ckpt_release_capture()`; `kernel/env_service.c` — `env_payload_wait_parked()` then `env_payload_restore()`; `kernel/env_payload.c` — `EP_REFUSE_PLACEMENT`; source clause T7 in `tests/env_checkpoint_restore_check.sh` |
| A shell variable and the console's buffered bytes are payload, and both come back under the new boot's ids | `kernel/env_console.h` — `env_console_snapshot()`, `env_console_inject()`; `tests/env_payload_host_test.c` (43 checks, round trip plus the full refusal matrix) |
| The two CONTENTS clauses are assertions fed through the environment's own console, and the tooth withholds the payload rather than the descriptor | `tests/env_checkpoint_restore_check.sh` — clauses B10/B11, live clauses L4b/L11, T7; `tests/env_checkpoint_restore_check_smoke.sh` — the `payload` tooth group (9 teeth) and the `no-restore` split |
| The pour waits for parked **and** announced, the flag can only come from this boot's drained `[env-id]`, and a timeout proceeds by name instead of refusing | `kernel/env_payload.c` — `env_payload_wait_parked()`, `EP_WAIT_TICKS` 3000, the `announced=` timeout log; `kernel/env_console.c` — the `identity` flag, `ec_sees_identity()`, `env_console_identity_seen()`; `tests/env_payload_host_test.c` (44 checks); source clause T7 in `tests/env_checkpoint_restore_check.sh` |
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
| The payload is never captured or poured — a replayed environment comes back empty | the file-bytes and shell-variable clauses cannot be asserted; "the environment came back" is measured but "came back with its contents" is not | P1a (sixth increment) |
| The pour lands on a freshly replayed sidecar's half-finished boot, so the environment never announces its identity after the resume | the durable guard's post-reboot identity wait times out against a console nothing is running to write to, and a restored environment's boot is erased by its own restore | P1a (the announce gate) |
| Environment storage is a RAM region | files do not survive reboot; the durable-storage claim is false | P1b |
| aerofs-lite is read-only with a 71 168-byte file cap | a durable tenant filesystem would ship an unusable ceiling | P1b |
| Durable storage is not quota-charged to the partition | a tenant's disk is unbounded and unmetered | P1b |
| The tenant profile has no network cap at all | the environment cannot be PASE-for-real; E7's socket path stays unroutable | P2 |
| No kernel-brokered socket service | reaching networking would require a cross-partition channel, violating LPAR Phase 11 | P2 |
| No per-partition bound on a tenant's simultaneous connections | one environment starves its neighbours without tripping the rate limiter | P2 |
| `partition_migrate()` does not move an environment | migration leaves the environment on the source (orphaned or destroyed) | P3 *(closed — increments 3–5)* |
| `failover_recover_from()` adopts ownership only | a dead node's environments vanish with no name in the report | P3 *(report landed as increment 9; the restore half — the survivor replaying what its own durable storage still holds — as increment 11)* |
| The failover broadcast path is 24 KiB | a 5.5 MiB environment cannot ride it; the gap must be stated, not implied | P3 (destination (a) or (b)) *((b) as built: named by increment 9, restored from durable storage by increment 11; (a) remains the PASE-grade answer)* |
| No thread groups / `clone` / `futex` | Go, threaded CPython and Node are out | unscoped — §9 |
| No mediated native↔POSIX interop | environments are a sealed box, not PASE-with-IFS | §9, §11 Q6 |
