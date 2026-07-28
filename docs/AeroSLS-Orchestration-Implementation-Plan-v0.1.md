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

### Phase 1 — Interpreter in-kernel (foundation)

**Deliverable:** `kernel/simi_interp.c`, a freestanding port of the reference interpreter, selectable as an alternative to translation.

- Port 31 opcodes; replace the three libc uses (§2.2).
- Expose `struct SimiContext` (the state in §2) as a first-class type rather than file-scope statics — this is the change that makes everything later possible.
- Add an execution-mode flag: `translated` (existing, fast) vs `interpreted` (new, checkpointable). **Not a replacement** — a per-workload choice.
- Cross-validate against the host interpreter: run the existing `tools/simi/tests` corpus through both and require identical results. That corpus already exists and already cross-validates three implementations (host interpreter, x86 JIT, RISC-V), so this is joining an established discipline, not inventing one.

**Risk: low.** Bounded, mechanical, and directly testable — a host test can link the real interpreter and run programs, no QEMU needed.

**Honest cost:** interpreted SIMI will be perhaps an order of magnitude slower than translated. That is the price of checkpointability and should be stated in the docs as a deliberate trade, not discovered later.

### Phase 2 — Checkpoint / restore

**Deliverable:** `sls_ctx_checkpoint(ctx_id)` / `sls_ctx_restore(ctx_id)`, persisting a `SimiContext` as an SLS object.

- Serialise `{pc, frame_top, frames[0..frame_top], mem}`. Only live frames — a depth-4 context writes 100 KiB, not the 2.3 MiB worst case.
- Store as a catalog object, which inherits persistence, `partition_id` ownership, RBAC and the `/api/objects` surface **for free**. No new registry.
- Reuse `persist_write_array_diffed()` so repeat checkpoints are incremental.
- Version the checkpoint format with a magic + the ISA version, following the `PERSIST_MAGIC_*` convention — a context checkpointed by one build must not be resumed by an incompatible one.

**Tests:** checkpoint mid-loop → restore → verify identical final result vs. an uninterrupted run. Checkpoint at every instruction of a small program and verify all N restores converge. That exhaustive form is the analogue of the persist shadow test's per-record sweep, and it is the one that catches state you forgot to save.

**Risk: low-moderate.** The failure mode — silently omitting a field from the serialiser — is caught by the exhaustive restore test.

### Phase 3 — Migrate a live context across nodes

**Deliverable:** a running computation moves node-to-node and resumes.

- Extend the DSPP migrate opcode family (`DSPP_MIGRATE_*` in `net/dspp.h`) with a context payload, mirroring the existing stream-migration path.
- Wire into `partition_migrate()` alongside `stream_migrate_send_partition()`.
- Verify with the same two-simulated-node technique already used by `tests/cross_node_migration_host_test.c`: run as node A, capture real Ethernet-framed packets, reset, replay through the real RX dispatcher as node B.

**Risk: moderate**, mostly integration. The transport, framing and receive dispatcher are built and tested; this adds a payload type.

**This is the demo.** A computation halfway through a loop on node 1, resuming on node 2 with the loop counter intact.

### Phase 4 — Service registry (unblocks the rest)

**Deliverable:** name → partition/node/endpoint resolution.

- A registration is a catalog object; persistence, ownership and RBAC come free.
- Resolve locally via the catalog; cross-node via the existing cluster roster.
- Replaces nothing: `services[MAX_SERVICES]` (8, boot-populated, `kernel/microkernel.c`) stays as internal-service supervision.

**Risk: low.** Small and additive.

### Phase 5 — Declarative workload objects + reconciliation

**Deliverable:** declare desired state; the kernel converges on it.

- A workload definition is an SLS object; applying it is a syscall.
- The reconcile loop is a poll on the AP core beside `tier_mgr_tick()`, using the same throttled-tick pattern (`if (counter % N) return;`).
- **Concurrency constraint, learned the hard way:** the AP core must not call `persist_*()`. Those run on the BSP and share an unlocked DMA staging buffer (`p_buf`). The reconciler must either queue work for the BSP or restrict itself to non-persisting operations. This is written down here because it is exactly the kind of thing that produces a rare, undebuggable corruption if discovered late.

**Risk: moderate** — the first thing in this system that acts autonomously. Bound it: reconcile a small, explicit set of conditions, log every action, and make it disableable.

### Phase 6 — Mesh policy (optional)

Circuit breaking, health state, per-service metrics over IPC (local) and DSPP (cross-node). Concepts from `AeroSLS-Service-Mesh.md`; **not** its `pthread`/socket implementation.

## 5. Sequencing

```
Phase 1 (interpreter) ─→ Phase 2 (checkpoint) ─→ Phase 3 (live migration)  ← the differentiator
                                                          │
Phase 4 (registry) ───────────────────────────────────────┴─→ Phase 5 (workloads) ─→ Phase 6 (mesh)
```

Phases 1–3 are the research bet; 4–6 are the orchestration surface. They are independent, so if PEC stalls the platform work continues.

**If only one thing is done: Phases 1–3.** Phase 4 is more *useful*; Phases 1–3 are what makes AeroSLS something other than a smaller Kubernetes.

## 6. Risks, and what would kill this

| Risk | Severity | Mitigation / kill criterion |
| --- | --- | --- |
| Interpreted SIMI too slow to be useful | Medium | Measure at Phase 1. Frame as a per-workload choice. **Kill criterion:** if interpretation is >50× translated, the checkpointable path is a toy — reconsider translator-assisted checkpointing at explicit yield points instead. |
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

**Effort and risk ratings are judgement.** No prototype was built for this plan, and the "order of magnitude slower" figure for interpretation is an expectation, not a measurement — Phase 1 exists partly to replace it with a real number.
