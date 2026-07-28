# AeroSLS Orchestration & Persistent Execution — Implementation Plan v0.1

## 0. Scope

An implementation plan for the features the K8s convergence review (`AeroSLS-K8s-Convergence-Architectural-Review-v0.1.md`) identified as genuinely useful and genuinely missing, with **Persistent Execution Contexts (PEC)** as the headline capability.

Nothing here is built yet. Every technical claim is grounded in the current source tree and cited; effort and risk figures are judgement, not measurement, and are labelled as such.

## 1. A correction to the review, before anything is planned on it

The review recommended re-targeting PEC onto SIMI, on the reasoning that *"a bytecode interpreter has an explicit, serialisable program counter."* Reading the runtime properly shows that statement needs splitting in two:

**What is true:** SIMI has a clean, serialisable machine model, and a working reference interpreter exists at `tools/simi/simi_interp.c`.

**What is false:** the *kernel* does not interpret SIMI. `kernel/simi_translate.c` AOT-translates a SIMI object to native x86-64 via `simi_x86_translate()`, maps it executable, and enters it at a RIP (`simi_translate_and_map()`). `kernel/simi_runtime.c` is not a VM — it is three helper functions (`simi_rt_resolve/objsize/objtype`) that translated native code calls back into.

**So checkpointing the path SIMI currently runs on in-kernel is exactly as hard as checkpointing any native x86-64 code** — which is the problem `ucontext` was reaching for and failing to solve portably.

This does not sink the plan. It changes it from "use the thing that's there" to "bring the interpreter in as a second execution mode" — which is a real, bounded piece of work rather than a free win, and §2 shows it is small.

## 2. Why SIMI is still the right substrate — measured

`tools/simi/simi_interp.c` is **499 lines** implementing **31 opcodes**. Its entire execution state is:

```c
#define MEM_SIZE   (64 * 1024)
#define MAX_FRAMES 256
#define NREGS      1024

typedef struct {
    uint64_t regs[NREGS];
    uint8_t  cap_tag[NREGS];   /* capability tags, one byte per register */
    uint32_t declared_nregs;
    uint32_t return_pc;        /* instruction index to resume caller at */
    int      caller_frame;     /* index into frame stack, -1 at top level */
} Frame;

static Frame   frames[MAX_FRAMES];
static int     frame_top;
static uint8_t mem[MEM_SIZE];
/* plus: uint32_t pc — an instruction INDEX, not an address */
```

This is a textbook checkpointable machine, and the properties that make it so are worth naming because they are not accidental:

- **`pc` is an array index into the instruction stream**, not a host address. It survives being written to disk and read back on a different machine, or a different architecture.
- **The register file is plain integers.** No host pointers, no stack addresses.
- **The frame stack is an array with explicit `caller_frame` links**, not the host call stack. Unwinding is data, not control flow.
- **Memory is one flat array.** No mapping table, no aliasing.

Serialising all of that is writing three arrays and two scalars. There is nothing to reconstruct.

### 2.1 Checkpoint size, computed

| Call depth | Checkpoint size | NVMe frames | Commands (post multi-page work) |
| --- | --- | --- | --- |
| 1 | 73 KiB | 19 | 1 |
| 4 (typical) | 100 KiB | 26 | 1 |
| 16 | 208 KiB | 53 | 2 |
| 256 (max) | 2.3 MiB | 593 | 19 |

**A typical checkpoint is one NVMe command.** That is a direct consequence of the multi-page transfer work completed earlier in this project — before it, 26 frames meant 26 synchronous round trips. Shadow-compare (also already built) means a *repeat* checkpoint of a mostly-unchanged context writes only the frames that changed.

The infrastructure PEC needs was, coincidentally, the infrastructure just finished.

### 2.2 Portability of the interpreter

`simi_interp.c` includes only `stdio.h`, `stdlib.h`, `string.h`, `stdint.h`. In practice:

- `stdio.h` — used by `die()` (`fprintf`) and CLI argument handling. Replace with `kernel_serial_printf()`.
- `string.h` — `strcmp` for entry-point lookup. Replace with a local `si_streq()`, matching this codebase's per-file helper convention (`p_memcpy`, `tn_streq`, `db_*`).
- `stdlib.h` — CLI only.

**No `malloc`, no `mmap`, no host-stack dependency in the execution core.** The port is mechanical.

## 3. The actual differentiator

Checkpoint/restore alone is CRIU, and CRIU exists. The thing AeroSLS can do that Docker and Kubernetes genuinely cannot is:

> **Checkpoint a running computation, move it to another node, and resume it mid-instruction — where the transport, the data movement and the crash-consistency are already built and tested.**

Concretely, every piece of that path exists today:

| Need | Already built | Where |
| --- | --- | --- |
| Persist the context durably | checksummed, crash-consistent, torn-write-detecting persist layer | `kernel/persist.c` |
| Write it in ~1 command | multi-page + scatter-gather NVMe | `drivers/nvme_io.c` |
| Incremental re-checkpoint | shadow-compare (writes only changed frames) | `kernel/persist.c` |
| Move bytes to another node | real DSPP wire protocol, Ethernet-framed, RX dispatcher | `net/dspp.c` |
| Move ownership | `partition_migrate()` | `kernel/partition.c` |
| Stop/start the workload | `partition_pause()` / `_resume()` | `kernel/partition.c` |
| Isolate and account for it | partitions, quotas, RBAC | `kernel/partition.c` et al. |

PEC is the missing 20%: the execution state itself. Everything around it is finished.

Kubernetes cannot do this because a container's state is the Linux kernel's process state — page tables, file descriptors, socket buffers, kernel objects — and CRIU's fragility comes from trying to reconstruct all of it. A SIMI context has none of that. Its state is 3 arrays.

## 4. Phased plan

Effort figures are **judgement, not measurement**. Each phase ends green with a full regression pass, per this project's standing practice.

### Phase 1 — Interpreter in-kernel (foundation) — **DONE**

> **Built and passing.** `kernel/simi_interp.{c,h}` + `tests/simi_interp_host_test.c` (27 checks). Full regression 67/67. As-built notes in §Phase 1 Findings below.

**Deliverable:** `kernel/simi_interp.c`, a freestanding port of the reference interpreter, selectable as an alternative to translation.

- Port 31 opcodes; replace the three libc uses (§2.2).
- Expose `struct SimiContext` (the state in §2) as a first-class type rather than file-scope statics — this is the change that makes everything later possible.
- Add an execution-mode flag: `translated` (existing, fast) vs `interpreted` (new, checkpointable). **Not a replacement** — a per-workload choice.
- Cross-validate against the host interpreter: run the existing `tools/simi/tests` corpus through both and require identical results. That corpus already exists and already cross-validates three implementations (host interpreter, x86 JIT, RISC-V), so this is joining an established discipline, not inventing one.

**Risk: low.** Bounded, mechanical, and directly testable — a host test can link the real interpreter and run programs, no QEMU needed.

**Honest cost — now measured, not guessed.** See §4.0 below: interpretation costs **3–9×**, not the order-of-magnitude-plus that was assumed. This is a deliberate per-workload trade and should be documented as such.

### Phase 0 — Kill-criterion measurement (DONE)

Before committing to Phase 1, the risk that would invalidate the whole track — "interpretation is too slow to be useful" — was measured on the host, using the existing toolchain. No kernel code was needed.

Both paths were run on identical programs and **produced identical results**, which is what makes the comparison meaningful:

| Workload | Interpreter (`simi-run`) | Translated (x86 JIT) | **Ratio** |
| --- | --- | --- | --- |
| Arithmetic-heavy — 1M-iteration sum loop | 12.20 ms | 1.42 ms | **8.6×** |
| Call-heavy — 200k CALL/RET | 11.20 ms | 3.57 ms | **3.1×** |

Method: `simi-run` timings are medians of 7 runs with process startup (measured separately against a trivial program, ~9 ms) subtracted; translated timings are the bench harness's own `clock_gettime` measurement averaged over 200 calls, excluding translation.

**Against a 50× kill criterion, the worst case is 8.6×. The risk is retired.**

Two observations worth carrying forward:

- **Call-heavy code narrows the gap to 3.1×**, and call-heavy is precisely the shape PEC cares about — the frame stack is what gets checkpointed. The JIT's advantage is largest on tight register arithmetic, which is where its allocator pays off; it has much less to offer across a CALL boundary.
- This is the **unoptimised reference interpreter**. It was written for clarity and cross-validation, not speed. A kernel port has room to improve, not just to regress.

Caveats, stated plainly: measured on host x86-64 Linux rather than the kernel; both programs are small and arithmetic/call shaped, with memory-heavy and object-op workloads unmeasured; and a kernel port's numbers may differ. The conclusion — that the ratio is single-digit rather than catastrophic — is robust to all of those, which is what the decision actually turned on.

### Phase 1 Findings (as built)

**What landed.** `kernel/simi_interp.h` defines `struct SimiContext` — the whole execution state as plain data — and `kernel/simi_interp.c` is the freestanding interpreter over it. Added to `Makefile`'s `X86_C_SRC`, so it ships in the kernel image. Compiles clean with `-I` flags and with none (matching the real `X86_CFLAGS` shape), zero errors, **zero warnings**.

**Cross-validation is complete, not sampled.** The test runs the **entire 17-program corpus** — all 16 integer programs match the reference's results exactly, and the one float program is refused rather than approximated.

**Three things worth recording because they changed the design:**

1. **The expected-value table is generated, not transcribed.** The first hand-written version had `branch_cmp` as 1 when the reference says 99 — surfacing as a "the port is wrong" failure that was actually a wrong test. The table is now produced by running `simi-run` across the corpus. A test whose oracle is hand-copied from comments is a test that can be wrong in the same direction as the code.

2. **Float ops trap, deliberately.** The reference's float helpers return `double`/`float` **by value** — precisely the construct that broke this project's cross-compile once before (`vecstore.c`: *"SSE register return with SSE disabled"*). That much is avoidable with out-parameters. What is not avoidable: with `-mno-sse`, x86-64 falls back to x87, whose 80-bit intermediates can differ in the last bit from the SSE arithmetic the reference and JIT both use — so bit-exact cross-validation would silently stop holding for float programs. Trapping is the honest option, and the test **asserts the refusal**, so a future change that starts computing floats slightly differently fails rather than passes quietly. Revisit when SIMD enablement lands.

3. **The ISA copy is guarded against drift.** `kernel/simi_x86.h` sets the convention — *"Duplicated (not #included) so this file has zero dependency on either tree"* — and the first draft of this header violated it by including `tools/simi/simi_isa.h`, which also dragged host-only name tables into the kernel build. It now mirrors the definitions like `simi_x86.c` does. Duplication is only safe if divergence is caught, so `simi_interp_isa_fingerprint()` packs the numbering as the kernel TU sees it, and the test — the one place both definitions legitimately coexist, in separate TUs — compares it against the authoritative header. `simi_x86.c` has the same exposure today protected only by a comment; this is slightly stricter.

**The two properties Phase 2 depends on are proven, not assumed:**

- **Resumability.** Running `loop_sum` in slices of *every* budget from 1 to 40 gives the identical result to one uninterrupted run — including **single-instruction slices**, i.e. a potential checkpoint boundary between every pair of instructions, on taken branches and across CALL/RET.
- **State completeness.** A raw `memcpy` of a mid-execution context into a fresh struct resumes to the identical result. That is checkpoint/restore in miniature, minus the disk, and it directly validates the claim that `struct SimiContext` *is* the execution state.

Also verified: traps are terminal (a re-run returns the same trap and retires **zero** additional instructions, so a scheduler polling runnable contexts cannot resurrect a dead one), and a depth-1 context's live bytes are **74,808 B (19 NVMe frames)** against a 361,000 B full struct — confirming §2.1's sizing and that checkpoints pay for depth actually used.

**Not done in Phase 1:** the interpreter is not yet reachable from the loader or any syscall — nothing selects interpreted mode at runtime yet. It is compiled into the image and fully tested, but dormant, in the same sense Phase 1 and Phase 4 of the Multi-Node roadmap landed real primitives before anything called them. Wiring the mode selector belongs with Phase 2, where there is finally a reason to choose it.

### Phase 2 — Checkpoint / restore — **DONE**

> **Built and passing.** `kernel/simi_ckpt.{c,h}` + `tests/simi_ckpt_host_test.c` (29 checks). Full regression 68/68. As-built notes in §Phase 2 Findings below.

### Phase 2 (original scope)

**Deliverable:** `sls_ctx_checkpoint(ctx_id)` / `sls_ctx_restore(ctx_id)`, persisting a `SimiContext` as an SLS object.

- Serialise `{pc, frame_top, frames[0..frame_top], mem}`. Only live frames — a depth-4 context writes 100 KiB, not the 2.3 MiB worst case.
- Store as a catalog object, which inherits persistence, `partition_id` ownership, RBAC and the `/api/objects` surface **for free**. No new registry.
- Reuse `persist_write_array_diffed()` so repeat checkpoints are incremental.
- Version the checkpoint format with a magic + the ISA version, following the `PERSIST_MAGIC_*` convention — a context checkpointed by one build must not be resumed by an incompatible one.

**Tests:** checkpoint mid-loop → restore → verify identical final result vs. an uninterrupted run. Checkpoint at every instruction of a small program and verify all N restores converge. That exhaustive form is the analogue of the persist shadow test's per-record sweep, and it is the one that catches state you forgot to save.

**Risk: low-moderate.** The failure mode — silently omitting a field from the serialiser — is caught by the exhaustive restore test.

### Phase 2 Findings (as built)

**Scope changed in one important way: this is a pure serialiser, not a storage layer.** The plan said "persisting a `SimiContext` as an SLS object." What landed is `simi_ckpt_save()` / `simi_ckpt_load()` converting a context to and from a flat byte buffer, with no I/O at all. Three reasons, and the third is the one that matters:

1. The dangerous failure here is **omission**, and a pure function is exhaustively testable without a disk.
2. Storage stays open — a stream object is the natural home (streams already have NVMe backing, a reboot-surviving directory, partition ownership).
3. **Phase 3 moves exactly these bytes.** Had serialisation been entangled with local storage, cross-node migration would need a second, parallel implementation. It now does not.

Same instinct as isolating `nvme_build_prp()`: separate the part where a mistake corrupts something silently, then test that part hard.

**A hazard found while designing the format.** The program image is deliberately *not* in the checkpoint — it is immutable and would dominate the payload. But `pc` is an instruction **index**, so restoring against a different image would resume at a valid-looking index in unrelated code, with every other field still plausible. The header therefore carries a hash over the instruction stream, literal pool and name pool (all three, because `RESOLVE` takes a name-pool index and `LOADI64` a literal index — same instructions with different pools is a different program to a resumed `pc`). Mismatches are refused.

The format validates, in order: magic, format version, buffer length, **ISA fingerprint**, struct layout, program hash, payload checksum — and writes nothing into the destination context until all of them pass, so a rejected restore leaves a live context byte-identical rather than half-overwritten. The ISA-fingerprint check reuses Phase 1's drift guard for a second purpose: a build that renumbered opcodes would resume a context executing something else entirely.

**The test caught a real weakness in itself.** The exhaustive scenario originally compared only the *final result* after resuming from each boundary. Injecting a deliberate bug — "forget to restore `pc`" — **passed 29/29**, because `loop_sum` restarted from the top re-initialises its accumulators and recomputes 55 either way. End-result equivalence is not state completeness.

The check is now **field-level identity**: the restored context must be byte-identical to the saved one, at every boundary. Re-running the mutations against the strengthened test:

| Injected bug | Result-only test | Identity test |
| --- | --- | --- |
| Drop `pc` on restore | **passed (missed it)** | **caught** |
| Write one fewer live frame | caught | caught |
| Drop `steps` (non-behavioural) | not tried | **caught** |

Worth stating plainly because it generalises: a checkpoint test that only compares outcomes will miss any field a given program happens not to depend on. The right assertion is that the restored state *is* the saved state.

**Verification.** 29 checks: exact size accounting (depth-1 = **74,848 B / 19 NVMe frames**, one command); checkpoint-and-restore at **all 58 instruction boundaries** into a deliberately poisoned destination, each byte-identical and each finishing correctly; all nine rejection paths including a genuine checkpoint of a *different* program refused against this one; verify-before-commit; a halted context round-tripping as halted rather than runnable; and mid-call-frame state surviving with only live frames written. Clean compile with `-I` flags and without, zero warnings. Regression 68/68.

**Not done in Phase 2:** no storage binding and no syscall/shell surface yet — checkpoints exist as byte buffers only. Phase 3 needs the bytes, not a filing system, so wiring them into a stream object is better done alongside the operator-facing surface than speculatively now.

### Phase 3 — Migrate a live context across nodes — **DONE**

> **Built and passing.** `kernel/simi_ctx_migrate.{c,h}` + `net/dspp.{c,h}` (new CTX opcode family) + `tests/simi_ctx_migrate_host_test.c` (78 checks). Full regression 69/69. As-built notes in §Phase 3 Findings below.

### Phase 3 (original scope)

**Deliverable:** a running computation moves node-to-node and resumes.

- Extend the DSPP migrate opcode family (`DSPP_MIGRATE_*` in `net/dspp.h`) with a context payload, mirroring the existing stream-migration path.
- Wire into `partition_migrate()` alongside `stream_migrate_send_partition()`.
- Verify with the same two-simulated-node technique already used by `tests/cross_node_migration_host_test.c`: run as node A, capture real Ethernet-framed packets, reset, replay through the real RX dispatcher as node B.

**Risk: moderate**, mostly integration. The transport, framing and receive dispatcher are built and tested; this adds a payload type.

**This is the demo.** A computation halfway through a loop on node 1, resuming on node 2 with the loop counter intact.

### Phase 3 Findings (as built)

**The demo works.** `loop_sum` runs 29 of its 58 instructions on node 1, is checkpointed, chunked across 19 real Ethernet-framed DSPP packets, reassembled on node 2, and runs the remaining 29 instructions there — producing `55`, the same answer as an uninterrupted single-node run, in the same total step count. No work lost, none repeated.

**1. A second header on the same magic, not new fields on the old one.** `DSPPMigrateHeader`'s tail is stream-specific (name, MIME type, size, frames used, owner uid) and meaningless for a context, which instead needs total length, chunk count and a program hash. `DSPPCtxMigrateHeader` is a separate 121-byte struct sharing `DSPP_MIGRATE_MAGIC` so the dispatcher still treats both as one family.

**2. The bug that design choice created, and the guard against it.** The dispatcher must read magic, opcode and `node_dest_id` *before* it knows which header it holds. The first implementation gated the whole family on `len < sizeof(DSPPMigrateHeader)` (177 bytes) — and a context header is only **121**, so every legitimate context BEGIN was silently dropped as "too short". Fixed by reading the opcode first and applying the right minimum per family. The prefix-compatibility that makes that safe is now enforced by `_Static_assert` on every shared field offset in `net/dspp.c`: reordering either struct's prefix is a **compile error naming the drifted field**, rather than a wire bug whose only symptom is misrouted packets. Verified by deliberately reordering the prefix and confirming the build fails.

**3. Chunking was necessary, and it is where the real hazards live.** A checkpoint is ~74 KB against a 4 KiB packet, so BEGIN + N chunks. The non-obvious requirements, each with a test:
   - Track **which** chunks arrived, not how many — counting alone lets a duplicate substitute for a missing chunk and completes a transfer with a hole in it. A hole is undetectable afterwards: the checksum simply fails with no indication why.
   - Only the **last** chunk may be short; a short interior chunk leaves a zero gap the length arithmetic still considers covered.
   - `total_chunks` must equal what `total_bytes` implies, or the two numbers disagree and either can index the bitmap.
   - Reject an unknown program hash at **BEGIN**, not at the last chunk — otherwise 74 KB is reassembled before discovering the receiver cannot run it.

**4. Mutation testing found a vacuous assertion.** The test checks that a short final chunk's unused tail is zeroed rather than leaking kernel stack. It passed *with the zeroing loop removed*, because `loop_sum` leaves nearly all 64 KiB of its flat memory zero — so the leftover bytes were zeros regardless. Fixed by poisoning the context's memory with a non-zero pattern before checkpointing. All eleven mutations tried are now caught; the reordered-prefix mutation is caught at compile time.

**5. `partition_migrate()` is wired, and honestly inert.** Step 3b calls `simi_ctx_migrate_send_partition()` next to the existing stream move, under the same `cluster_local_node_id() != 0` condition (no cluster identity, nowhere to send). There is no same-disk fallback, because relocating a context to another slot on the same node migrates nothing.

   **The gap this exposed, stated plainly:** nothing in this kernel owns long-lived execution contexts. No scheduler or service runtime holds them; the interpreter runs a context its caller owns. So the registry `partition_migrate()` iterates is real and really iterated, but **empty on every current boot**, and the partition path moves zero contexts. The transport, checkpointing, chunking and resume are all proven end-to-end by the host test — what is missing is a *producer* of live contexts. That is named work (it belongs with Phase 5's workload objects), not an oversight. It is wired now so the capability is reachable from the system the moment that producer exists, rather than only from a test.

**6. No same-disk fallback and no retransmission.** Fire-and-forget, matching the stream path and for the same reason (`kernel/net_event.h`'s blocking wait uses privileged `sti; hlt`). The receiver *does* ACK every packet, carrying a refusal reason, so retry can be added later without another wire-format change — but nothing reads ACKs yet. A genuinely dropped chunk therefore leaves a transfer incomplete; the test verifies this is detected and refused rather than restoring a partial context.

**Verification ceiling:** all of the above is host-verified. `x86_64-elf-gcc` is not available in this environment, so the full kernel link was not run; every touched file was compiled clean at `-O2` under the Makefile's exact freestanding flag set. Two *simulated* nodes in one process — real packet loss, reordering beyond what the test injects, and NIC behaviour remain unverified.

**Not done in Phase 3:** one inbound transfer at a time (a second is refused with a distinct status, not silently corrupted); a raised limit is an array, not a redesign. No syscall or shell surface — migration is reachable only through `partition_migrate()`, which is the honest place for it until contexts have an owner.

### Phase 4 — Service registry (unblocks the rest) — **DONE**

> **Built and passing.** `kernel/service_registry.{c,h}` + persist region + syscalls 281–284 + 4 shell commands + 3 REST routes + `tests/service_registry_host_test.c` (46 checks). Full regression 70/70. As-built notes in §Phase 4 Findings below.

### Phase 4 (original scope)

**Deliverable:** name → partition/node/endpoint resolution.

- A registration is a catalog object; persistence, ownership and RBAC come free.
- Resolve locally via the catalog; cross-node via the existing cluster roster.
- Replaces nothing: `services[MAX_SERVICES]` (8, boot-populated, `kernel/microkernel.c`) stays as internal-service supervision.

**Risk: low.** Small and additive.

### Phase 4 Findings (as built)

**1. "A registration is a catalog object" was wrong, and checking it produced a better design.** `struct SLSObjectEntry` has name, `partition_id`, `owner_uid`, `owner_role` and `perm_mask` — but nowhere to put an **endpoint**, which is the entire point of a registration. Making it a catalog object would mean adding service-specific fields to a 128-entry struct shared by every object type and persisted on every boot, to serve one caller. So this is a dedicated `services_registry[64]` table, the idiom `partition_owner_table[]` and `tenants[]` already use. The salvageable half of the claim was kept: registrations carry `owner_uid`/`partition_id` and gate through `catalog_get_role()`, so RBAC really is the existing mechanism rather than a second permission system.

**2. The node is NOT stored — and that is the design.** A registration records name → **partition**. `node_id` is derived at resolve time via `partition_get_owner_node()`.

   `partition_migrate()` already updates `partition_owner_table[]` (its Step 4). Storing a node id here would be a second copy of a fact that already moves, stale the instant a partition migrated, requiring a reconciliation loop to chase it — which is precisely the machinery Kubernetes needs and this design gets to not build.

   **The consequence is the phase's best property: service resolution follows partition migration automatically.** Scenario 3 of the host test runs the *real* `partition_migrate()` and re-resolves: the lookup reports the new node, `is_local` flips correctly, and **zero writes happen to the registry** — asserted by counting `persist_services()` calls. Mutation testing confirms the test is real: caching the node instead of deriving it fails 5 checks.

**3. Destruction is the converse, and needed wiring.** A registration pointing at a destroyed partition would resolve to the owner node of a partition that no longer exists — a confidently wrong answer, worse than "no such service." `partition_destroy()` Step 3b now calls `service_unregister_partition()`. Dropped rather than expired because the registry is authoritative, not a cache.

**4. Resolution is deliberately not role-gated.** Registration and removal require `DB_ADMIN`; looking up where something lives does not — the same posture `partition_get_owner_node()` takes. Both directions are asserted, and gating reads is one of the eight mutations the test catches.

**5. A stray line found in `persist_restore_all()`.** A `persist_region_commit()` call sat inside the tenant cold-start branch, left over from the crash-consistency work. Assessed honestly: `p_region_open` is 0 throughout restore (nothing calls `stage_hdr()` there, and `persist_scan_regions()` only reads), so it returned at its first line — **dead code, not live corruption**. Removed anyway: it implies a write happens during restore, and it would fire at the wrong moment the first time any restore block staged something.

**6. Blast radius, as usual.** One new symbol in `partition.c` needed faithful stubs in 16 tests; `persist.c` snapshotting the new array needed a real (zero-initialised) definition in 29. Faithful in both cases — an empty registry returning 0 is exactly what the real code does under those tests' conditions, not a convenient lie.

**Verification ceiling:** host-verified only; `x86_64-elf-gcc` is unavailable here, so no full kernel link — every touched file compiles clean at `-O2` under the Makefile's exact freestanding flags. The new persist region's placement is checked by `tests/persist_lba_layout_host_test.c`, and that check was confirmed to genuinely fail for two deliberately-overlapping placements rather than passing by default. The REST and shell surfaces are compile-verified but not exercised against a running kernel.

**Not done in Phase 4:** no health checking or TTL — a registration is a static fact until changed, not a liveness signal (`kernel/microkernel.c`'s watchdog remains the only liveness mechanism, and only for the 5 internal services). No cross-node registry replication: each node holds its own registry, so a name registered on node 1 does not resolve on node 2. That is a real limit and the natural companion to Phase 5's reconciliation, not something to bolt on here.

### Phase 5 — Declarative workload objects + reconciliation — **DONE**

> **Built and passing.** `kernel/workload.{c,h}` + persist region + syscalls 285–288 + 4 shell commands + 3 REST routes + `tests/workload_reconcile_host_test.c` (71 checks). Full regression 71/71. As-built notes in §Phase 5 Findings below.

### Phase 5 (original scope)

**Deliverable:** declare desired state; the kernel converges on it.

- A workload definition is an SLS object; applying it is a syscall.
- The reconcile loop is a poll on the AP core beside `tier_mgr_tick()`, using the same throttled-tick pattern (`if (counter % N) return;`).
- **Concurrency constraint, learned the hard way:** the AP core must not call `persist_*()`. Those run on the BSP and share an unlocked DMA staging buffer (`p_buf`). The reconciler must either queue work for the BSP or restrict itself to non-persisting operations. This is written down here because it is exactly the kind of thing that produces a rare, undebuggable corruption if discovered late.

**Risk: moderate** — the first thing in this system that acts autonomously. Bound it: reconcile a small, explicit set of conditions, log every action, and make it disableable.

### Phase 5 Findings (as built)

**1. The concurrency constraint was right, and it forces the queue.** The alternative — run the whole reconciler on the BSP and persist directly — was examined and rejected on evidence, not preference: the BSP's foreground loop is `http_server_run()`, entered **only when a NIC is present**; without one the BSP falls through to `sls_shell_loop()`, which blocks in `read_line()` and has no idle point at all. The AP core's `ap_kernel_main()` is the only loop that reliably ticks. So the reconciler sweeps on the AP core, applies directly only what does *not* persist, and hands the rest to the BSP through a single-producer/single-consumer ring.

   The invariant is asserted, not just documented: scenario 5 counts `persist_*()` calls and requires **zero** from `reconcile_tick()`, then requires exactly one from `reconcile_drain()`. Mutating the sweep to persist directly fails 4 checks.

**2. The ring is sized so one sweep cannot overflow it.** Pass 2 emits at most one intent per workload, so `WL_INTENT_MAX >= WORKLOAD_MAX` means overflow signals exactly one condition — *the BSP stopped draining* — rather than routine busyness. That makes the dropped counter diagnostic instead of noise. Both properties are `_Static_assert`ed, because shrinking the ring or growing the workload table would otherwise break them silently. The test provokes overflow the way it can really happen (repeated sweeps with no drain), confirms it is counted, and confirms convergence still completes afterwards — dropped intents are re-derived because the sweep reads *actual* state rather than trusting a past enqueue.

**3. The bug the test found: shared partitions oscillated.** The first version decided partition run-state per workload. Two workloads in one partition with opposite desired states then fought — each sweep, one paused it and the other resumed it, forever, and the reconciler never settled. A reconciler that never settles has no error message; the production symptom is just wear.

   Fixed with **union semantics**: a partition runs if *any* active workload in it wants to run. Deterministic, order-independent, and it matches what the words mean — a partition has to be up for anything in it to run. A STOPPED workload still withdraws its own service; what it cannot do is take the partition down from under its neighbours.

   **Mutation testing then found the test was too weak to prove the fix.** Replacing the union with "last declaration in table order wins" still passed, because the scenario happened to declare the STOPPED workload at a *lower* index. Strengthened with a third workload declared after the running one, exercising the opposite order. Also caught this way: a hard-coded `converged = 1` passed everything, because nothing asserted the flag is ever *false*.

**4. `actions_taken` changed meaning, and the field says so.** After the union refactor it counts only actions attributable to one workload — its own service registration. Partition pause/resume is shared and is deliberately not charged to an arbitrary member.

**5. Reconciliation is OFF at boot, including after a restore.** Restoring a declaration is not the same as deciding to start acting on it, and a reboot is the worst moment to begin converging unasked. The restore path says so explicitly.

**Verification ceiling:** host-verified only; no `x86_64-elf-gcc` here, so no full kernel link — every touched file compiles clean at `-O2` under the Makefile's exact freestanding flags. Critically, **the AP/BSP split itself is not exercised concurrently**: the test calls `reconcile_tick()` and `reconcile_drain()` from one thread in sequence. It proves the *separation of duties* (the sweep performs no persisting calls) but not the ring's memory ordering under genuine concurrent access on two cores. That would need a real two-core run, and is the main thing this phase has not proven.

**Not done in Phase 5:** no restarts, scaling, health-based action or scheduling — all need a liveness signal that does not exist for workloads (`microkernel.c`'s watchdog covers only the 5 internal services).

### Phase 5 Gap Closure (follow-on)

The three things Phase 5 shipped without were taken as their own piece of work. All three are now closed; regression **73/73**.

**Gap 1 — the ring was never tested concurrently.** `tests/reconcile_ring_concurrency_host_test.c` now runs the real enqueue/drain protocol from two OS threads at 400,000 intents, checking no-loss, no-duplication, ordering, tearing (each probe carries its sequence in five redundant fields plus a derived 64-byte string) and exact accounting. The ring genuinely filled 15,151 times, so it was contended, not serial.

**The ceiling was then measured rather than guessed at**, by mutating the ring and re-running:

| Mutation | Result |
| --- | --- |
| publish head *before* writing the slot | CAUGHT |
| advance tail *before* copying the slot out | CAUGHT |
| fullness test off by one (overwrite a live slot) | CAUGHT |
| replace every `__atomic` with a plain access | **SURVIVED** |

So it catches the structural bugs and does **not** catch missing barriers — x86-64's TSO makes acquire/release on a plain load/store free. A barrier-free ring is indistinguishable here and would still be wrong on the RISC-V target this codebase also builds. That residual gap is real; ThreadSanitizer is the right tool and does not run in this environment ("unexpected memory mapping"). The barriers are correct by review, not by test.

**Gap 2 — nothing created live contexts.** `kernel/workload_ctx.{c,h}`: a workload can now declare a **program**, and the reconciler instantiates it as a live interpreted context and calls `simi_ctx_register()`. `tests/workload_ctx_host_test.c` runs the whole path — declare → reconcile → instantiate → execute partway → `partition_migrate()` → replay the real captured packets as node 2 → resume — and gets `55` in 29 + 29 steps.

The load-bearing assertion is the **negative control**: scenario 1 shows the same migration call moving *zero* contexts beforehand, so "it moved one" afterwards means something. Images are copied into naturally-aligned per-slot storage because a `.tmo`'s instruction stream sits at byte offset 20, which would make a `uint64_t*` misaligned — tolerated on x86, a fault on RISC-V.

Mutation testing found the test masking a real defect: it registered the program image itself on the receiving side, so deleting `wlctx_start()`'s `register_image()` call survived. Removed that crutch; all eight mutations now caught.

**Gap 3 — the registry was per-node.** A third DSPP opcode family (`DSPP_SVC_ANNOUNCE`/`WITHDRAW`) replicates registrations. It **announces rather than queries** — a request/response would need a reply timeout, and every blocking wait here routes through `net_event.h`'s privileged `sti; hlt`. So each node broadcasts what it owns and caches what it hears, and a resolve stays a purely local lookup with no network round trip on the hot path.

Remote entries live in their own table, which makes two properties structural rather than remembered: **local always wins** (a remote announcement can never shadow a service running here), and **remote entries are never persisted** (restoring a stale cache would resurrect services that moved or vanished while this node was down). Only the announcing node may withdraw its own entry.

The prefix-compatibility `_Static_assert`s were extended to the third header and verified by deliberately reordering it — a compile error naming the drifted field.

**Gap 3a — TTL and health (follow-on).** The item above named the remaining hole: a node that dies silently never withdraws anything, because withdrawal requires it to *send*, so its services would resolve forever on every other node. Now closed.

Each node re-announces what it owns every ~5 s; a cached entry ages through three states rather than two, because a binary alive/dead would delete a service the instant one heartbeat was late and give an operator no warning:

| State | Age | Behaviour |
| --- | --- | --- |
| FRESH | < 2 heartbeats | resolves normally |
| STALE | < TTL (~20 s) | **still resolves** — overdue is a warning, not a deletion |
| EXPIRED | ≥ TTL | does not resolve |

TTL is four heartbeats deliberately: a single dropped announcement is normal on a broadcast protocol with no retransmission, and must not evict a healthy service.

**The design decision that matters: expiry is checked at LOOKUP, not by the sweep.** `service_resolve()` evaluates freshness itself, so an entry past TTL stops resolving whether or not a sweep has run. The sweep only reclaims slots. That makes correctness independent of sweep scheduling — which matters concretely, because on a NIC-less boot the BSP loop that runs it never executes at all. A test asserts exactly this: past TTL, with the slot still occupied and nothing swept, the lookup still refuses.

The heartbeat transmits, so it runs on the **BSP only** — the NIC TX path cannot be driven from two cores at once. It also fires once immediately on the first call rather than waiting a full interval, so a freshly booted node is discoverable in milliseconds. Local registrations never age: this node is authoritative for its own.

**Mutation testing found one hole.** Age arithmetic saturates at zero because `kernel_tick_counter` is incremented by whichever core takes the timer IRQ, so a reading can land marginally *behind* a stamp; an unsigned subtraction would wrap to an astronomical age and instantly expire a healthy entry. Removing the saturation **survived the whole suite** — nothing ever moved the clock backwards. Added that case; all seven mutations now caught.

**Gap 3b — endpoint liveness (follow-on).** The item above named its own limit: TTL is liveness by *absence of announcement*, which is node-level. A node whose kernel is fine but whose service has died keeps heartbeating, and keeps resolving FRESH. Now addressed.

**The design turn: the owning node probes its OWN endpoints and reports the verdict in its heartbeat.** No node ever probes another's. That matters — a cross-node probe needs a request/response with a timeout, and no blocking wait is usable from these paths. Riding the existing announcement costs one byte and no new round trip.

What is actually observed, in both cases from state the kernel already maintains:

| Endpoint | Observation | Source |
| --- | --- | --- |
| TCP | is a socket LISTENing on that port? | `tcp_conns[]`, via `tcp_port_is_listening()` |
| IPC | what does the watchdog say about the service owning that port? | `services[]`, via `mk_ipc_port_state()` |

The IPC case is the strongest signal in the system: `microkernel_service_poll()`'s watchdog maintains ONLINE/CRASHED from real crash and restart events, so the answer is observed rather than inferred. **DEGRADED counts as DOWN** — routing to a degraded service on the strength of "it hasn't fully crashed yet" is how a degraded service becomes an outage.

**`serving` is a separate field from `health`, deliberately.** They answer different questions — "is the endpoint accepting?" versus "is this information current?" — and collapsing them would lose the distinction between *"I have not heard lately"* and *"I have heard, and it is down"*, which are opposite situations for anyone deciding whether to route or to page someone. A test asserts the independence directly: STALE + UP, and FRESH + DOWN, are both reachable and both meaningful.

Two smaller decisions: registration probes immediately rather than waiting up to a heartbeat, so an operator who registers a service and asks about it gets the truth. And a DOWN service still **resolves** — the registry reports, it does not hide; hiding it would make "gone" and "broken" indistinguishable to a caller.

Layering note: the two probes live in the files that own the data (`net/tcp.c`, `kernel/microkernel.c`) and are exported as one predicate each. `tcp_conns[]` alone is 16 MiB; pulling it into the registry would have dragged it into every host test that links the registry.

Eight mutations tried against the probe logic, all caught — including DEGRADED-as-UP, guessing UP for an unowned IPC port, and announcing without probing first.

**What is still not closed:** a process that is alive and holding its port but whose *handler* has wedged reports UP. Catching that needs an application-level probe — send something, require an answer — which is a different mechanism and is not built. UNKNOWN is returned honestly for an IPC port with no supervised owner rather than guessed. `wlctx_step_all()` also advances every context by a fixed budget with no fairness or priority; that is a scheduler, and naming it is not the same as having one.

### Phase 6 — Mesh policy (optional)

Circuit breaking, health state, per-service metrics over IPC (local) and DSPP (cross-node). Concepts from `AeroSLS-Service-Mesh.md`; **not** its `pthread`/socket implementation.

### Interlude — the whole-image link check, and what it found

Five phases plus three gap closures had been built, all host-verified, and the kernel had **never once been linked as a whole image**. Every phase carried the same ceiling note. Before starting Phase 6 that was finally done: all 94 C translation units compiled under the Makefile's exact freestanding flags and linked against the real `arch/x86/linker.ld`.

**The link itself is clean** — zero duplicate symbols, zero genuinely-missing symbols (the only undefined is `_start`, which lives in `boot.asm`). Verified two ways: the linker's own output, and independently by differencing all-undefined against all-defined across the 94 objects and subtracting the `.asm`-provided allowlist. `nasm` and `qemu-system-x86_64` are absent in this environment, so the assembly objects and an actual boot remain unverified.

**It found a live bug in the physical frame allocator.**

`.bss` totals **117 MiB**, so the image occupies physical **1 MiB → 119.5 MiB**. Meanwhile `physical_memory_bitmap[]` lives in `.bss` — it boots all-zero, meaning *every frame marked free*, including the kernel's own. There was no `frame_pool_init()` anywhere; nothing reserved the image, nothing read the multiboot memory map (which was parsed, but only to print it). `alloc_raw_frame()` started at physical frame 1 and walked upward, and `boot.asm` identity-maps 0–4 GiB, so a caller's write went straight through the returned pointer into the running image:

| Frames | Address | What is actually there |
| --- | --- | --- |
| 1–159 | 0x1000–0x9FFFF | conventional RAM — genuinely usable |
| 160–191 | 0xA0000–0xBFFFF | VGA framebuffer; `0xB8000` is the text buffer `vga.c` writes to |
| 192–255 | 0xC0000–0xFFFFF | BIOS ROM shadow — not RAM |
| **256–30592** | **0x100000–0x7780C40** | **the kernel's own .text/.rodata/.data/.bss** |

Allocation #256 returns the multiboot header. Process spawn and `loader.c` take several frames each.

**Pre-existing, not introduced by this track.** `alloc_raw_frame()` has always started at frame 1 with no reservation. The 4.2 MiB of `.bss` added across Phases 3–5 is 3.6% of the 117 MiB total — the dominant consumers are `http_conns` (32 MiB), `g_add_column_scratch` (16 MiB) and `tcp_conns` (16 MiB). This work widened an already-wide window rather than opening it.

**The fix.** `linker.ld` gained `_kernel_image_end` (it previously exported no end-of-image symbol at all, so the allocator had no way to ask). `frame_pool_init()` reserves one contiguous span from 0 up to it — which covers frame 0, low RAM, the VGA hole, the ROM shadow and the whole image in a single rule. Giving up the 640 KiB of genuinely usable conventional RAM costs 0.5% of a 128 MiB machine and removes every question about which parts of low memory are safe. `frame_pool_limit_ram()` additionally reserves everything above the top of real RAM, taken from the multiboot mmap the boot path already walks — the bitmap spans a fixed 4 GiB regardless of what is installed, so without it a 256 MiB machine would be handed a page at 3 GiB.

The reservation logic is split into `frame_pool_reserve_below(end_addr)` so it can be tested: a linker symbol has no address a host test can choose, and an untestable boot-path reservation is precisely what was wrong here to begin with. `tests/frame_pool_reserve_host_test.c` (23 checks) opens with a **negative control** that reproduces the unreserved allocator and shows allocation #256 really does return `0x100000` — without which "allocations are above the kernel now" would not distinguish a fix from a coincidence. Seven mutations tried, all caught.

**Still unverified:** no `nasm`, so the six `.asm` objects are not in this link; no QEMU, so nothing has been booted. The fix is correct by construction and by host test, not by observation on hardware.

## 5. Sequencing

```
Phase 1 (interpreter) ─→ Phase 2 (checkpoint) ─→ Phase 3 (live migration)  ← the differentiator
   DONE                     DONE                    DONE
                                                          │
Phase 4 (registry) ───────────────────────────────────────┴─→ Phase 5 (workloads) ─→ Phase 6 (mesh)
   DONE                                                          DONE
```

Phases 1–3 are the research bet; 4–6 are the orchestration surface. They are independent, so if PEC stalls the platform work continues.

**The research bet is now settled: a live computation genuinely moves between nodes and resumes.** What Phases 1–3 did *not* produce is anything that creates long-lived contexts — see §Phase 3 Findings item 5. That producer is the substance of Phase 5, which makes Phase 5 the phase that turns a proven capability into a used one, rather than more surface area.

**If only one thing is done: Phases 1–3.** Phase 4 is more *useful*; Phases 1–3 are what makes AeroSLS something other than a smaller Kubernetes.

## 6. Risks, and what would kill this

| Risk | Severity | Mitigation / kill criterion |
| --- | --- | --- |
| ~~Interpreted SIMI too slow to be useful~~ | ~~Medium~~ → **RETIRED** | **Measured (§Phase 0): 8.6× on arithmetic, 3.1× on call-heavy, against a 50× kill criterion.** Both paths verified to produce identical results. No longer a project risk; remains a per-workload performance note. |
| Checkpoint omits state | High if it ships | Exhaustive checkpoint-at-every-instruction test (Phase 2). This is a data-loss class, so the test is not optional. |
| Reconciler races persistence on the AP core | High | §4 Phase 5. Named up front rather than discovered. |
| Format drift between checkpoint versions | Medium | Magic + ISA version, refuse mismatches. Precedent: `PERSIST_MAGIC_*`. |
| SIMI ISA still evolving under us | Medium | Checkpoint format versions with the ISA; cross-validate against the host corpus every phase. |
| **Verification ceiling** | **Standing** | None of this can be booted here — no cross-compiler, `nasm` or QEMU. Everything lands as host tests; real-hardware behaviour stays unverified until run on your machine. Unchanged from every prior phase. |

## 7. What this plan deliberately excludes

- **Checkpointing translated (native) SIMI.** Requires mapping native RIP back to SIMI PC. A research project; revisit only if Phase 1 measurement shows interpretation is unusably slow.
- **Container runtime, Envoy, libmicrohttpd control plane.** Inapplicable — see the review.
- **The reference implementations in `AeroSLS-Memory-Manager.md` / `-Persistent-Execution-Contexts.md` / `-Service-Mesh.md`.** Their *concepts* feed Phases 2, 3 and 6; their code targets Linux userspace (`mmap`, `pthread`, `ucontext`) and cannot be linked into a freestanding kernel.

## 8. Verification ceiling on this document

The SIMI machine-model description, the 499-line/31-opcode counts, the libc dependency list and the state-struct layout are read directly from `tools/simi/simi_interp.c` and `kernel/simi_*.{c,h}`. Checkpoint sizes in §2.1 are arithmetic on those declared constants (`NREGS 1024`, `MAX_FRAMES 256`, `MEM_SIZE 64 KiB`) — exact for the struct as written, though real `sizeof` may differ slightly with padding.

The §3 "already built" table reflects work completed and regression-tested earlier in this project (66 host tests passing).

**Effort and risk ratings are judgement**, with one exception: the interpretation-speed figures in Phase 0 are a real measurement, taken on host x86-64 Linux using the existing `tools/simi` toolchain (`simi-run` vs `bench_harness.c` + `simi_x86.c` — the same translator the kernel uses). Both paths were verified to produce identical results on identical inputs. Those numbers do not carry over to kernel-side performance unchanged; what they establish is the *ratio*, which is what the kill criterion was about.

No other prototype was built for this plan, and no kernel code was written.
