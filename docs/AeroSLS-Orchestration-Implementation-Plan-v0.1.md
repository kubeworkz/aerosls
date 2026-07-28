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
