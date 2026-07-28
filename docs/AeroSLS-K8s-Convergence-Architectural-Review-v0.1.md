# AeroSLS ↔ Kubernetes Convergence: Architectural Fit Review v0.1

## 0. What was asked, and the short answer

Review the newer design docs — `AeroSLS-Memory-Manager`, `AeroSLS-Persistent-Execution-Contexts`, `AeroSLS-Service-Mesh`, `AeroSLS-Control-Plane`, `AeroSLS-Full-Integration`, `AeroSLS-Compiler-Architecture`, the two sample applications, and the `AeroSLS-vs-K8-Docker-Gaps` feasibility study — and determine which of those elements fit the architecture, and what already exists.

Three findings, in order of how much they should change your plans:

1. **Far more is already built than the gap analysis credits.** Of 24 Kubernetes-analogous capabilities checked against real code, **23 exist and are tested**. The feature matrix in `AeroSLS-vs-K8-Docker-Gaps.md` marks several of them ❌ "Critical" when they shipped months ago. Planning from that matrix would fund work that is already done.
2. **The three new concept docs are hosted Linux userspace designs, not kernel work.** `Memory-Manager`, `Persistent-Execution-Contexts`, `Service-Mesh`, `Control-Plane` and `Full-Integration` all depend on `mmap`, `pthread`, `ucontext`, `setjmp`/`signal`, `eventfd`, `numaif`, `libmicrohttpd` and `malloc`. AeroSLS's kernel is `-ffreestanding -nostdlib` with no libc at all — it hand-rolls `p_memcpy()` because there isn't one. **None of that reference code can be lifted into the kernel.** It is a valid prototype track, but it is a second program, and the docs do not currently say so.
3. **The Kubernetes convergence is real but the framing is backwards in one important way.** The genuinely valuable ideas in these docs are the ones SLS makes *simpler* than K8s, and the gap doc's own §"What SLS Changes" gets this right. The roadmap that follows it does not — it schedules containerd/CRI-O and Envoy, which are the two things that cannot exist here.

## 1. What is already built

Verified by grepping the real source tree, not from memory. Every row below resolves to compiled kernel code with host-test coverage.

| Kubernetes concept | AeroSLS equivalent | Where | Gap doc says |
| --- | --- | --- | --- |
| Namespaces / multi-tenancy | `partition_id` (256), `tenant_create()`, `database_id` | `kernel/partition.c`, `tenant.c`, `database.c` | ❌ Critical |
| ResourceQuota (memory) | per-partition RAM frame quota | `kernel/frame_pool.c` | ❌ High |
| ResourceQuota (CPU) | weighted CPU scheduling | `kernel/process.c` | ⚠️ Basic |
| ResourceQuota (storage) | per-partition disk page quota **+ physically reserved sub-ranges** | `kernel/storage_quota.c`, `rowstore.c` | ❌ High |
| ResourceQuota (connections) | per-partition concurrent inbound conn quota | `net/tcp_quota.c` | ❌ |
| Rate limiting / throttling | per-partition request-rate window | `net/http_rate_limit.c` | ❌ |
| RBAC | roles, group profiles, authorization lists, database grants | `kernel/auth.c`, `group_profile.c`, `authlist.c`, `database.c` | ⚠️ Basic |
| Audit logging | security audit log | `kernel/security_audit.c` | ⚠️ Basic |
| Metrics / observability | per-partition usage metering, disk/network status | `kernel/usage_metering.c`, `net/http.c` | ⚠️ Basic |
| PersistentVolume / StatefulSet | SLS objects + streams + checksummed persistence | `kernel/object_catalog.c`, `stream.c`, `persist.c` | ❌ Critical |
| Pod eviction / drain | `partition_pause()` / `partition_resume()` | `kernel/partition.c` | ❌ |
| Workload migration | `partition_migrate()` with **real cross-node byte movement** over DSPP | `kernel/partition.c`, `stream.c`, `net/dspp.c` | ❌ |
| Cluster membership | `cluster_init()` / `cluster_register_peer()` | `net/consensus.c` | ⚠️ |
| Leader election / leases | per-partition Raft-lite write leases | `net/consensus.c` | ❌ |
| Liveness probe + restart | microkernel service watchdog (`svc crash`/`svc restart`) | `kernel/microkernel.c` | ⚠️ |
| Secrets | `sys_sls_seal()` key derivation | `kernel/secure_api.c` | ❌ High |
| ConfigMap-ish | catalog objects + records | `kernel/object_catalog.c` | — |
| Message bus | IPC ports + message queues | `kernel/ipc.c`, `msgqueue.c` | — |
| kubectl | shell (156 command branches) + 131 REST routes | `user/shell.c`, `net/http.c` | ⚠️ Basic |

**The matrix in `AeroSLS-vs-K8-Docker-Gaps.md` §"Feature Comparison Matrix" should be regarded as obsolete.** It appears to predate the LPAR, Multitenant-Isolation, Storage-Isolation, Network-Fairness and Multi-Node phases. Its "Implementation Roadmap" schedules Q3 2026 work — RBAC, namespace isolation, multi-tenancy with quotas — that is already in the tree and covered by 66 passing host tests.

### 1.1 The one real gap

**Service discovery / dynamic service registry.** `kernel/microkernel.c` has a `ServiceDescriptor services[MAX_SERVICES]` table, but it is capped at **8**, populated only at boot by `microkernel_init()`, and keyed on a fixed name — it is a supervision table for five internal kernel services, not a registry applications can register into or resolve against. Nothing maps a logical service name to a partition/node/endpoint at runtime.

That is a genuine hole, it is small, and §3 argues it is the right next thing to build.

## 2. The three new concept docs: what they actually are

`AeroSLS-Memory-Manager.md`, `AeroSLS-Persistent-Execution-Contexts.md` and `AeroSLS-Service-Mesh.md` are substantial and internally coherent. They are also **hosted Linux programs**:

| Doc | Depends on |
| --- | --- |
| Memory-Manager | `sys/mman.h`, `numaif.h`, `fcntl.h`, `unistd.h`, `stdlib.h`, `immintrin.h` |
| Persistent-Execution-Contexts | `ucontext.h`, `setjmp.h`, `signal.h`, `sys/eventfd.h`, `sys/syscall.h`, `sys/mman.h` |
| Service-Mesh | `pthread.h`, `stdatomic.h`, `stdlib.h` |
| Control-Plane | `microhttpd.h`, `sys/socket.h`, `arpa/inet.h`, `netinet/in.h` |
| Full-Integration | `pthread.h`, `signal.h`, `stdlib.h` |

They also assume a source tree (`include/aerosls/sls/`, `src/sls/`, `src/exec/`, `src/mesh/`) that **does not exist in the repository** — nothing from any of them is built.

This matters concretely, not pedantically:

- The kernel compiles with `-ffreestanding -nostdlib -mno-sse`. There is no `malloc`, no `pthread`, no `mmap`. `kernel/persist.c` defines its own `p_memcpy`/`p_memset`/`p_memcmp` for exactly this reason.
- `sls_memory_manager.c`'s design centres on `mmap`-ing a backing file. The kernel's equivalent layer is `frame_pool.c` + `persist.c` + the NVMe driver, and it is already written, quota-enforced, checksummed and crash-consistent.
- `AeroSLS-Control-Plane.md` proposes a REST control plane on **libmicrohttpd over Linux sockets**. The kernel already serves **131 REST routes** from `net/http.c` on its own TCP/IP stack and e1000 driver. Building a second HTTP server in userspace Linux would not be a control plane *for* AeroSLS; it would be a control plane *beside* it.

None of this makes the docs worthless — a hosted prototype is a legitimate way to explore semantics quickly, and `Persistent-Execution-Contexts` in particular explores something the kernel genuinely lacks (§3.2). But the docs currently read as implementation plans for AeroSLS, and they should say plainly which target they are for. **As written, a reader would reasonably try to `make` them into the kernel and fail at the first `#include`.**

## 3. What actually fits, ranked

### 3.1 Service registry — build this, it is the real gap

The mesh doc's `sls_mesh_register_service()` / `sls_mesh_discover_service()` is the right idea. AeroSLS has every substrate piece already: an object catalog with named entries, `partition_id` for ownership, `cluster_local_node_id()` + `partition_owner_table[]` for locality, DSPP for cross-node transport, and IPC/MQ for local delivery. What is missing is one table and two operations.

The SLS-native form is *simpler* than the doc's: a service registration is a catalog object, so it persists across reboot for free, inherits partition ownership and RBAC, and shows up in the existing `/api/objects` surface. No separate registry, no gossip protocol, no sidecar.

Small, well-bounded, and unlocks the rest of the mesh story.

### 3.2 Persistent execution contexts — the genuinely novel idea, but not via `ucontext`

This is the most interesting concept in the new docs and the one with the strongest SLS argument: if memory is persistent, a computation should be resumable, and "cold start" stops being a thing.

AeroSLS already has the *state* half: partitions pause/resume, objects and streams persist, and a migrating partition's data physically moves. What it lacks is the *continuation* half — resuming a computation mid-instruction.

The doc reaches for `ucontext`/`setjmp`, which cannot work in the kernel. But AeroSLS has something better suited that the doc does not mention: **SIMI**, its own bytecode ISA, already in the tree (`kernel/simi_runtime.c`, `simi_translate.c`, `simi_x86.c`, `simi_riscv.c`) and syscall-reachable via `SYS_SLS_SIMI_INFO`.

> **Correction (added after reading the runtime).** The sentence originally here — *"a bytecode interpreter has an explicit, serialisable program counter, so checkpointing one is tractable"* — needs splitting in two. The **ISA** does have such a model, and a working reference interpreter exists at `tools/simi/simi_interp.c`. But the **kernel does not interpret SIMI**: `simi_translate.c` AOT-translates to native x86-64 and enters it at a RIP, and `simi_runtime.c` is three callback helpers, not a VM. Checkpointing the path SIMI runs on in-kernel today is therefore exactly as hard as checkpointing any native code.
>
> The recommendation survives, but the work is larger than implied: the reference interpreter must be **ported into the kernel as a second, checkpointable execution mode** alongside translation. That is a bounded job — 499 lines, 31 opcodes, and its only libc uses are `fprintf`/`strcmp` — and the interpreter's state (`pc` as an array index, a plain-integer register file, an explicit frame array, flat memory) is genuinely textbook-serialisable. A typical checkpoint computes to ~100 KiB, which is one NVMe command given the multi-page transfer work already done.
>
> Full analysis and phased plan: `AeroSLS-Orchestration-Implementation-Plan-v0.1.md`.

**Recommendation:** keep the concept, re-target it onto SIMI, and drop the `ucontext` approach. This is a research-grade capability and genuinely differentiating; it is also a large project and should not be started before §3.1 — **except** that §3.1 and the PEC track are independent, so they can proceed in parallel if there is appetite for both.

### 3.3 Service mesh — the substrate exists, the implementation does not transfer

"Memory-speed mesh" is a sound observation: when two services share an address space, an RPC is a pointer hand-off. AeroSLS can do that locally today via IPC/MQ, and cross-node via DSPP.

Worth taking from the doc: circuit breaker state, health checking, per-service metrics — all small and all fit the existing model. Worth discarding: the `pthread`-based implementation, mTLS-over-sockets, and anything assuming a sidecar process model.

### 3.4 Declarative workload spec — fits, and is cheap

`AeroSLS-Control-Plane.md`'s workload YAML is a good idea in the wrong location. As a *kernel* concept it maps cleanly: a workload definition is an SLS object; applying it is a syscall; reconciliation is a poll on the AP core alongside `tier_mgr_tick()`. That gives declarative deployment without a separate control-plane binary, and it is the piece that would make the system *feel* Kubernetes-like at the smallest cost.

### 3.5 Compiler — already aligned, with one caution

`AeroSLS-Compiler-Architecture.md` is the best-fitting of the new docs: 223 mentions of SIMI, which genuinely exists in-kernel and has a real ISA spec (`AeroSLS-SIMI-ISA-v0.1.md`, 170 KB). Compiling to SIMI is exactly right.

Caution: the doc also carries ~96 mentions of WebAssembly as a target. That is a second, unbuilt backend competing with a first, partially-built one. SIMI already has x86 and RISC-V translation in the tree. Adding Wasm before SIMI's own toolchain is finished would split a small amount of effort across two ISAs.

### 3.6 What does not fit, and should be cut from the roadmap

- **Container runtime (containerd / CRI-O).** Requires Linux namespaces, cgroups and a Linux kernel. AeroSLS *is* the kernel. This is not "hard", it is categorically unavailable, and it sits in the roadmap as Q3 2026 Phase 1. The SLS answer to "run arbitrary containers" is that you don't — you run SIMI programs and native partitions.
- **Envoy / Istio.** Same reason: a userspace proxy process model that has no host to run on.
- **A libmicrohttpd control plane.** Duplicates `net/http.c` in a different process on a different OS.
- **`PersistentVolumeClaims`, `StatefulSets`, CSI, volume snapshots, init containers.** The gap doc's own "Things That Become IRRELEVANT" list is correct, and those items should be deleted from the roadmap rather than left ambiguous.

## 4. On the Kubernetes-convergence direction

The instinct is sound: these are the right *problems* — isolation, quotas, scheduling, discovery, declarative deployment, observability. Kubernetes has good answers and it is reasonable to borrow its vocabulary.

The risk is borrowing its *mechanisms*. Most of Kubernetes' complexity exists to compensate for two assumptions AeroSLS does not share: that processes are ephemeral, and that state lives somewhere else. Remove those and large parts of the design collapse into nothing — which is exactly what the gap doc's "obsolete features" list observes, and it is the strongest idea in the whole document.

The honest framing is not "AeroSLS needs to catch up to Kubernetes." On the isolation and quota axes it is arguably **ahead** already: quotas are enforced against physically reserved disk sub-ranges and real frame accounting, not cgroup accounting over a shared filesystem. What it lacks is the *orchestration surface* — discovery, declarative specs, a reconciliation loop — which is a much smaller body of work than the roadmap implies, precisely because the substrate is done.

**Suggested reframing of the roadmap:** drop Phase 1 (container runtime) and Phase 2's Envoy mesh entirely; mark the multi-tenancy, quota and RBAC items as complete; and replace the first two phases with (a) service registry, (b) declarative workload objects + reconciliation loop, (c) mesh policy on the existing IPC/DSPP substrate.

## 5. Suggested next step

If one thing is picked up from this review: **build the service registry (§3.1).** It is the only verified gap in the parity list, it is small, it fits the object-catalog model without new mechanisms, and both the mesh and the declarative-workload ideas depend on it.

Before any of it: **correct the feature matrix in `AeroSLS-vs-K8-Docker-Gaps.md`.** It is the document most likely to drive planning, and it currently understates the system by a wide margin — which risks re-funding finished work while the one real gap goes unnoticed.

## 6. Verification ceiling

Every "already built" claim in §1 was checked by grepping the compiled source tree for the named symbol and confirming a real definition in `kernel/`, `net/` or `drivers/`; the capability names are mine, mapping AeroSLS primitives onto Kubernetes vocabulary, and reasonable people could draw those equivalences slightly differently. Test coverage is asserted from this session's own regression runs (66/66 host tests).

The dependency findings in §2 come from extracting `#include <...>` lines from each doc's code blocks — that is what the docs specify, though a reader could of course intend those as illustrative pseudocode rather than literal source.

The new docs total roughly 340 KB; this review is based on their structure, their code-block dependencies, their feature matrices and their roadmaps, **not on a line-by-line reading of every reference implementation**. Specific designs inside them may merit closer attention than a structural review gives. No code was written or changed for this review.
