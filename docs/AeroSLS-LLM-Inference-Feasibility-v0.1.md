# AeroSLS LLM Inference Feasibility Assessment v0.1 — would a flat single-level store help MoE expert streaming?

## 0. The question, and the short answer

The question asked: *if we run an LLM in AeroSLS's flat memory space, could we increase inference speed by removing disk I/O?* The specific target is a GLM 5.2 744B int4 build following the approach in Fareed Khan's "Building GLM 5.2 744B Model in C to Run on 25GB RAM" (Level Up Coding, Jul 2026), which keeps the always-on part of a Mixture-of-Experts model resident and streams sleeping experts from disk.

**Short answer: no, not as stated — and the current build would be substantially slower than the reference C engine, not faster.** The premise contains a category error (§1), and five independent hard blockers exist in the current codebase (§2), two of which are three-orders-of-magnitude capacity problems rather than tuning gaps. There is, however, a real and genuinely differentiated idea underneath the premise (§4) — it is just a much longer project than "run it on our architecture," and it needs a different framing to be worth doing.

> **§7 supersedes parts of §1, §4, and §5.** This document was first written against only the accessible introduction of the source article. The full article was subsequently made available and contains the author's own measurements — including a direct test of this document's central question, with a result stronger than the analytical reasoning in §1 anticipated. **§7 is the authoritative section**; §1–§6 are retained unchanged for provenance, and §7 states precisely which of their claims it upgrades, narrows, or withdraws.

This doc exists so that decision is made against measured facts about this codebase rather than against the architecture's aspirations. Everything in §2 was verified by direct code audit; line references are to the working tree as of this writing.

## 1. Why "flat memory removes disk I/O" does not hold

A single-level store changes *how memory and storage are addressed*. It does not change *how many bytes must cross the PCIe bus*.

The reference implementation's whole point is that the model does not fit in RAM: 744B parameters at int4 is roughly **372 GB**, targeted at a machine with 16–25 GB of usable RAM. Whenever a token needs an expert that is not currently cached, that expert's weights must be read from NVMe. That read happens identically whether the addressing model is `mmap` + page cache, an explicit application-managed buffer pool, or an SLS object space. Flat addressing removes *bookkeeping* — pointer translation, explicit load calls, a hand-rolled cache layer — not *data movement*.

Where a single-level store can legitimately win is in **who decides what to keep resident and when**, because that decision can be made with semantic knowledge the application has and a generic OS page cache does not (§4). That is a real advantage, but it is an advantage about *scheduling* I/O, not eliminating it. Any speedup has to come from better overlap and better prediction — never from the bytes not moving.

One further correction worth stating plainly: the reference engine is already close to I/O-optimal for its design. It streams int4 weights with a small resident hot set and no framework overhead. There is not a large pool of wasted I/O sitting there for a better memory architecture to reclaim.

## 2. Current-state blockers, measured

### 2.1 NVMe transfers are 4 KiB each, synchronous, queue depth 1

`drivers/nvme_io.c` creates a real 64-entry submission/completion queue pair (`NVME_IO_QUEUE_SIZE 64`, `nvme_io.h:9`) with a correct phase-tag protocol — the hardware plumbing genuinely exists. But `nvme_io_submit_sync()` (`nvme_io.c:112`) submits exactly one command, then busy-polls that one completion slot before returning:

```c
sq[io_sq_tail] = *cmd;
io_sq_tail = (io_sq_tail + 1) % NVME_IO_QUEUE_SIZE;
mmio_write32(io_sq_doorbell(), io_sq_tail);
volatile struct NVMeCqe* cqe = &cq[io_cq_head];
while (((cqe->status) & 0x1) != io_cq_phase) {
    __asm__ volatile("pause");
    ...
}
```

Nothing in the codebase ever submits a second command before draining the first. There is no interrupt-driven completion path (no ISR is wired to the I/O completion queue vector at all), no outstanding-request tracking, and no way for a caller to issue a read and do something else. Effective queue depth is 1 out of an available 64.

Worse for this workload than the depth: **every transfer is exactly one 4 KiB page.** Both `nvme_read_sync()` and `nvme_write_sync()` set `cmd.prp1 = buf` with `cdw12 = SECTORS_PER_FRAME - 1` (7, i.e. 8 × 512 B), and a PRP1-only command cannot span more than one page. There is no PRP list, no PRP2 chaining, no multi-page transfer anywhere in the driver.

The consequence for expert streaming is severe. An MoE expert at int4 is on the order of megabytes to tens of megabytes. Moving one requires *thousands* of separate 4 KiB commands, each one a full submit-and-spin round trip with zero overlap, repeated for 8 experts per layer, for every layer, for every token. A hand-tuned engine issuing large multi-page reads at high queue depth is not marginally better here — it is in a different performance class.

### 2.2 SIMD is disabled, and the scaffolding to safely enable it is not wired up

`Makefile` disables vector codegen in both build paths:

- Kernel (`X86_CFLAGS`, line 17–19): `-mno-sse -mno-sse2 -mno-mmx ... -fno-tree-vectorize`
- User programs (`USER_CFLAGS`, line 178–181): `-mno-sse -mno-sse2 -mno-avx`

Int4 dequantize-and-matmul is close to the archetypal SIMD workload; running it scalar-only forfeits most of the available arithmetic throughput. `kernel/vecstore.c` already shows what living without SSE costs in practice: `-mno-sse` breaks float **by-value return** on x86-64, so every float-returning helper in that file had to be rewritten to use out-parameters (`vs_sqrtf(float* out, float x)`, `vecstore.c:432`), and the math falls back to x87.

The lazy-FPU scaffolding needed to enable SIMD safely is *partially* present, which is easy to misread as "nearly done." What exists and what does not:

| Piece | State |
| --- | --- |
| IDT vector 7 (#NM) gate | **Present** — `arch/x86/idt.c:48` → `isr7_stub` → `handle_device_not_available_fault()` |
| `xsave`/`xrstor` save/restore logic | **Present** — `arch/x86/lazy_fpu.c:37,52`, full-mask (`0xFFFFFFFF`) |
| Per-task state buffer, correctly sized and aligned | **Present** — `avx512_state_buffer[2688]`, `__attribute__((aligned(64)))`, `scheduler_extended.h:16` |
| `CR4.OSXSAVE` (bit 18) ever set | **Absent** — `boot.asm` sets only PAE (bit 5, line 127–130); no other CR4 write exists |
| `xsetbv` / XCR0 configuration | **Absent** — appears only in `docs/SLS-OS.md` (a design doc) and one comment; zero occurrences in compiled code |
| `CR0.TS` set on context switch | **Dead code** — `perform_lazy_context_switch` (`switch_lazy.asm:8`) sets TS at line 34–36, but is **never called from anywhere**; compiled and linked, never invoked |
| Task identity backing the FPU owner | **Stub** — `kernel_get_current_task_struct()` lives in `kernel/stubs.c:263` over a 64-slot shadow table keyed on kernel *thread* id, and its own comment concedes it "ties FPU/AVX-512 ownership to scheduler.c's cooperative kernel-thread id space, not to a separate user-process identity" |

Without `CR4.OSXSAVE`, the `xsave`/`xrstor` instructions in the #NM handler would themselves raise #UD — a fault inside a fault handler. And because `perform_lazy_context_switch` is never called, `CR0.TS` is never set, so #NM never fires and the handler never runs regardless. The mechanism is unreachable in both directions today.

(A stale comment in `vecstore.c:415–431` states there is "no IDT vector 7 gate"; that specific claim is now out of date — the gate was added later. The `CR4.OSXSAVE`/`xsetbv` half of that comment is still accurate.)

### 2.3 Tiered storage relabels metadata; it does not move data

The "L1 cache / L2 DRAM / L3 SSD" tiering is bookkeeping, not a caching layer. `tier_mgr_tick()` (`kernel/tier_mgr.c:61`) runs every 10th poll on the AP core and, on threshold, does exactly this:

```c
e->storage_tier = (SLSStorageTier)(e->storage_tier - 1);   // promote
```

That is the entire promotion action — a field assignment on an object catalog entry, plus a log line and a counter. No frame is allocated, no `nvme_read_sync()` is issued, no page changes residency. The same is true of the explicit `tier_promote`/`tier_demote` paths (`tier_mgr.c:175,274`).

Actual residency is handled separately and is strictly reactive load-on-first-touch, e.g. `rowstore_load_page()`:

```c
if (row_pages[page_id]) return row_pages[page_id];   // already resident
uint8_t* frame = allocate_physical_ram_frame();
nvme_read_sync(ROWSTORE_LBA_BASE + (uint64_t)page_id * 8, frame);   // only when touched
```

So the tier labels an operator sees in `disk status` do not correspond to where bytes physically are. For a workload whose entire performance story is residency management, the subsystem that appears to manage residency does not.

### 2.4 The one prefetcher is a network prefetcher, and it is dead code

`net/prefetch.c` looks at first glance like the missing piece. It is not, for two independent reasons.

It is never called: `issue_speculative_prefetch()` is defined at `prefetch.c:12` and referenced nowhere else in the tree. `prefetch_worker_kernel_thread()` is documented as "deployed permanently on CPU Core 3" (`prefetch.c:30`) but no such core is ever booted (§2.5).

More fundamentally, it does not read from disk at all. Its action is to build a DSPP packet and put it on the **network** to pull a page out of a *remote node's* memory (`prefetch.c:39–54`). It is a distributed-shared-memory prefetcher, not a storage prefetcher. Its prediction is also next-page-only (`current_fault_vaddr + 4096`, line 14) — a sequential-scan heuristic, structurally unable to express "these 8 specific expert objects, now," which is the only prediction that matters for MoE.

### 2.5 Capacity: three orders of magnitude short, plus a structural object cap

This is the blocker that makes a near-term experiment on the current build impossible regardless of the four above.

| Resource | Current | Needed for GLM 5.2 int4 |
| --- | --- | --- |
| Model size on disk | 10 GB image (`Makefile:133`) | ~372 GB |
| RAM | 4 GB (`-m 4G`, `Makefile:140`) | 16–25 GB per the reference |
| Total SLS stream capacity | **512 MiB** — `STREAM_MAX 8` slots × 64 MiB (`stream.h:8,9,22`) | ~372 GB |
| Separately addressable catalog objects | **128** (`CATALOG_MAX_OBJECTS`, `object_catalog.h:77`) | thousands (256 experts × N layers) |
| Rowstore capacity | 1 GiB total, 4 MiB per partition (`rowstore.h:115,144`) | n/a |

The disk image and RAM figures are trivially changed (a bigger `qemu-img create`, a bigger `-m`). The **512 MiB across 8 stream slots** and the **128-object catalog** are not — they are compile-time table sizes woven through the persistence LBA layout, and the natural way to represent experts as SLS objects (one object per expert) collides with the object cap by more than an order of magnitude before it collides with the byte capacity by nearly three.

### 2.6 No parallel compute

`-smp 4` is requested of QEMU, but `kernel/kernel.c:231–234` brings up exactly one application processor (`boot_application_processors(1)`), and that core runs `flush_daemon_tick()` + `microkernel_service_poll()` plus the HTTP server — housekeeping, not compute. There is no worker-pool or parallel-matmul capability, so the overlap that makes expert streaming viable (load expert *n+1* while computing on expert *n*, across several cores) has no substrate.

## 3. Scoping: what closing each gap would take

Ordered by whether it is worth doing *at all*, not by difficulty.

**A. Async/multi-queue NVMe with multi-page transfers.** The highest-value item, and worth doing on its own merits regardless of the LLM question — it would speed up rowstore, vecstore, stream, and persist alike. Work: add PRP-list support for transfers spanning more than one page; add an outstanding-request table keyed on `command_id` (the field is already there, `nvme_io.c:113`); split submit from completion so callers can issue N reads and reap them later; optionally wire an ISR to the I/O completion queue instead of polling. Blast radius is the real cost: **35 call sites across `kernel/persist.c`, `rowstore.c`, `stream.c`, `vecstore.c`**, all of which assume a blocking call that returns a status. The clean path is to add async entry points alongside the existing sync ones and leave all 35 callers untouched — `nvme_read_sync()` becomes a thin wrapper over submit-then-immediately-reap. Risk: moderate, and testable in isolation. This is a genuine multi-week item, not an afternoon.

**B. Enable SIMD.** Bounded and well-understood: set `CR4.OSXSAVE`, configure XCR0 via `xsetbv` (guarded on a `CPUID` feature check), actually call `perform_lazy_context_switch` from the real scheduler or abandon the CR0.TS approach in favour of unconditional `xsave`/`xrstor` on switch, replace the `kernel/stubs.c` shadow-table identity with real process identity, then drop `-mno-sse`/`-mno-avx` and fix the fallout (the `vecstore.c` out-parameter convention could be reverted). The pieces are individually small; the risk is that FPU-state corruption bugs are nondeterministic and miserable to debug, and this touches the context-switch path that everything depends on. **The critical caveat: this cannot be verified in the current dev environment at all** — this sandbox has no cross-compiler, no `nasm`, and no QEMU (see the Multi-Node roadmap's Phase 7 addendum for the same ceiling), and FPU context-switch correctness is exactly the class of bug a host test cannot catch. It needs real hardware or a real VM plus a deliberate stress test.

**C. Expert-aware residency.** Only meaningful after A. The right shape is not a fix to `net/prefetch.c` (wrong layer, wrong medium, wrong prediction — see §2.4) but a new, small, honest API: something like `sls_pin_objects(ids[], n)` that issues N async reads and reports when all are resident, called by the model's router *after* it selects experts for the next layer but *before* the matmul for the current one finishes. That is the piece with no equivalent in a generic OS page cache, and it is where the architecture's real claim lives (§4). It is also the least well-defined and should not be designed before A exists to build on.

**D. Capacity.** Raising `STREAM_MAX`/`CATALOG_MAX_OBJECTS` to the thousands is not a `#define` edit: the capacity-sizing pass documented in the Multitenant Isolation Gap Analysis §18 showed how these constants thread through `kernel/persist.h`'s LBA layout, and that pass found a real corruption risk from exactly this kind of resize. Doing it at the scale needed here (128 → thousands of objects; 512 MiB → hundreds of GB) is a storage-architecture project, not a resize.

**E. Parallel compute.** Boot the remaining APs, build a real work-queue. Substantial, and orthogonal to everything above.

## 4. Where the architecture's real claim actually lies

Stripping out the parts that do not survive scrutiny, one genuine idea remains, and it is worth stating precisely because it is *not* "flat memory is faster."

An MoE router knows, one layer ahead, exactly which 8 of 256 experts it will need next. That is perfect prefetch information — and no generic OS page cache can exploit it, because `mmap` + LRU has no way to be told "these specific 30 MB regions, in the next few milliseconds, and none of the others." Applications work around this with `madvise`/`readahead` hints that are advisory and coarse. A single-level store whose residency primitives are *first-class and semantic* — where the model can pin an object set and be told when it is resident — could in principle beat a page-cache-based engine on cache hit rate and I/O overlap, not by moving fewer bytes but by moving them earlier and never moving the wrong ones.

That is a defensible, interesting research claim. It is also a claim about **prefetch scheduling quality under a semantically-informed API**, which means the honest way to test it is a focused benchmark against `mmap`+`madvise` on the same hardware — not a 744B model port. If the mechanism does not win at small scale with synthetic access patterns, it will not win at 372 GB.

## 5. Recommendation

**Do not attempt a GLM 5.2 744B port on the current build.** It would not produce a fair measurement of anything: with queue-depth-1 4 KiB synchronous reads, scalar-only arithmetic, no prefetch, and 512 MiB of usable object capacity against a 372 GB model, a negative result would tell you nothing about the architecture's actual merits, and the effort would be dominated by working around §2 rather than testing the idea.

**Suggested sequencing if this direction is worth pursuing:**

1. **Item A (async/multi-page NVMe) first, justified independently.** It benefits every existing subsystem, it is testable in this environment, and nothing else in this assessment is meaningful without it. Do it because the storage layer should be better, and let the LLM question benefit as a side effect.
2. **Then a small, honest prefetch benchmark** — item C's API against a synthetic MoE-shaped access pattern (a few hundred MB of "experts," a router trace, measured hit rate and stall time), compared against `mmap`+`madvise` on Linux on the same disk. This tests the actual claim from §4 at a scale that fits current capacity. Cheap, fast, and genuinely informative either way.
3. **Item B (SIMD) when there is real hardware to validate it on** — worth doing regardless of the LLM work, but do not attempt it blind from this environment.
4. **Revisit the large-model question only if step 2 shows a real advantage.** If it does, that result — "semantically-informed residency beats page-cache LRU by X% on MoE-shaped access" — is a more interesting and more publishable finding than a 744B port would have been, and it is what would justify items D and E.

**What to be careful about regardless:** the new hardware purchase should be justified by the reference implementation's own requirements (fast NVMe, enough RAM for the resident set), not by an expected AeroSLS speedup. On the evidence above, AeroSLS will not be the fast path on that machine for a long time, and possibly never for this specific workload — the reference engine's approach is well-matched to what it is doing.

## 6. Verification ceiling on this document (as first written)

§2's findings are all from direct code audit and are cited to file and line. §3's effort estimates are informed judgement, not measurement, and the 35-call-site blast radius for item A is a count, not an estimate of the work to change them. The §1 and §5 performance reasoning is analytical: **no benchmark was run**, because this environment cannot build or boot the kernel (no cross-compiler, `nasm`, or QEMU — the same ceiling disclosed in the Multi-Node Partition Scaling Roadmap's Phase 7 addendum), and because the 744B model, its ~372 GB int4 weights, and the target hardware are all unavailable here. The article behind the premise is paywalled beyond its introduction; its architecture is summarised from the accessible portion, and the specific figures attributed to it (744B parameters, ~1.5 TB bf16, 8-of-256 experts per layer, int4, 16–25 GB RAM target) come from that portion. The ~372 GB int4 figure is arithmetic (744e9 × 0.5 B), not a quoted claim.

*(That paywall limitation no longer applies — see §7.)*

---

## 7. Revision after reading the full article — the premise was measured, and it produced no gain

The complete article is now available locally (`slsos-sim/Building GLM 5.2 744B Model in C to Run on 25GB RAM.html`). It contains extensive measurements from the author's own bench, including a controlled experiment that directly tests this document's central question. That experiment's result is the single most important fact in this assessment, and it is stronger than §1's analytical argument.

### 7.1 The author already ran the experiment. Eliminating disk I/O bought nothing.

Two runs, same model, same machine, same binary — only the RAM budget differs:

```
CPU 20 GB:  24 tokens in 84.68s (0.28 tok/s) | hit 3.5%  | RSS 16.09 GB
            PROFILE: expert-disk 46.5s | expert-matmul 27.0s | attention 5.0s
CPU 200 GB: 24 tokens in 85.56s (0.28 tok/s) | hit 68.6% | RSS 181.92 GB
            PROFILE: expert-disk 36.0s | expert-matmul 36.3s | attention 5.9s
```

Raising the expert cache hit rate from **3.5% to 68.6%** — i.e. removing the large majority of disk reads, which is exactly what the "flat memory removes disk I/O" theory proposes to achieve — produced **identical throughput: 0.28 tok/s in both cases.** The author's separate RAM sweep says the same thing across the full range: 20 GB → 200 GB lifts the hit rate from 2% to 61% while "the tokens per second does not follow. It is flat, and it even declines a little."

The reason is stated plainly: *"once the experts are in RAM, the bottleneck moves from the disk to the CPU doing the integer matmuls, and this AVX2 EPYC is the wall."*

This **upgrades §1 from an argument to a measurement**. §1 reasoned that flat addressing cannot make bytes travel faster; the stronger finding is that on this workload, making the bytes travel faster *at all* does not increase throughput, because disk is not the binding constraint once a modest cache is warm.

### 7.2 The real wall is memory bandwidth — which no memory architecture fixes

The author's thread sweep isolates the actual limit:

```
1 thread:    48 tokens in 887.83s (0.05 tok/s) | IPC 2.11
124 threads: 48 tokens in 227.46s (0.21 tok/s) | IPC 0.23
```

Throughput peaks at 32 threads and *declines* through 124. IPC collapses from 2.11 to 0.23 — the cores are stalled on memory, not computing. All 124 cores together buy roughly 4× one core. The measured DRAM ceiling on that box is **~58 GB/s (write)**, and the author's conclusion is direct: *"when your matmul has to stream tens of gigabytes of weights per token, a 58 gigabyte per second memory system is the wall, and no number of cores fixes that."*

That figure is grounded: with ~11 GB of expert weights touched per token (§7.4), a single token's matmul must stream that volume through the memory system regardless of where it came from.

**This is the finding most damaging to the original premise.** A single-level store changes addressing and residency policy. It does not change DRAM bandwidth, cache hierarchy behaviour, or arithmetic throughput. The binding constraint on this workload sits entirely in territory the architecture does not touch — and AeroSLS's position there is far worse than the reference's, because the reference's wall was an AVX2 EPYC while AeroSLS has **no SIMD at all** (§2.2) and would be doing int4 dequant-and-matmul in scalar x87.

### 7.3 Overlapping I/O with compute was measured 2.4× *slower*, and prefetch was defaulted off

This directly narrows §3 item C and §5 step 2, which proposed prefetch quality as AeroSLS's most promising angle. The author built precisely that mechanism and measured it:

```
PIPE=0 (serial load then matmul):        32 tokens in 83.27s (0.38 tok/s)
PIPE=1 (overlap disk load with matmul):  32 tokens in 197.65s (0.16 tok/s)
```

Overlapping made it **2.4× slower**. The cross-layer prefetch had the same outcome — it raised the hit rate from 58% to 62% but *dropped* throughput, "because the speculative loads created eviction pressure the disk could not keep up with." Both features ship **defaulted off**, with the honest note that on a disk with true parallelism they should help, and on the author's virtualized-NVMe box they did not.

Notably, the prediction quality was never the problem: the router lookahead recovers **72%** of the next layer's true top-8 (versus 32% from just reusing the previous token's routing). The signal is strong. The mechanism still lost, because on a serialized-latency disk, concurrency in the I/O path is a cost rather than a benefit.

Two consequences for this assessment. First, §4's claim that semantically-informed residency is an *untested* differentiator is **withdrawn** — the author implemented it (persistent per-expert usage histogram, startup auto-pinning scaled by confidence, LFRU eviction with a 25%-plus-constant anti-thrash margin, live re-pinning between turns, plus the router lookahead above) and measured it. Second, §5's proposed step-2 benchmark is largely **redundant**: the answer is already known to be hardware-dependent, and the deciding variable is whether the disk has real parallelism — a property of the drive, not of the OS's memory model.

What survives is much narrower than §4 claimed: on a disk that *does* have real parallelism, a residency API with semantic knowledge might beat page-cache LRU. That is a genuine but modest and conditional claim, not an architectural advantage.

### 7.4 Exact figures, and what they mean for AeroSLS's 4 KiB transfers

The article supplies precise numbers that let §2.1's finding be quantified rather than characterised:

| Quantity | Value |
| --- | --- |
| Layers | 78 (3 dense + 75 MoE) |
| Experts per MoE layer / active per token | 256 / top-8 (~3% fire) |
| Always-on ("dense") part, int4 | ~17B params ≈ **9.9 GB**, stays resident |
| Routed experts, int4 | ~727B params ≈ **362 GB** on disk |
| **Expert weights touched per token** | **~11 GB** |
| **One expert (gate+up+down), int4** | **~19 MB** — "the unit of work" |
| Full model on disk | 383.7 GB across 144 shards |
| Stated hardware floor | 16–26 GB RAM, NVMe SSD, no GPU |

The reference engine reads one expert in **a single coalesced ~19 MB `pread`**, because the prequantized container lays the three matrices contiguously, and the three weight views then point into that one slab with zero copying.

AeroSLS's driver caps every transfer at one 4 KiB page (`prp1` only, `cdw12 = 7`; §2.1). The same 19 MB expert therefore requires **~4,860 separate synchronous submit-and-spin round trips**, with no overlap between any of them. At ~11 GB touched per token, and with AeroSLS's 512 MiB of total object capacity (§2.5) making a meaningful cache hit rate impossible against a 362 GB expert pool, that is on the order of **2.7 million serialized 4 KiB NVMe commands per token**. This is not a tuning delta against "one `pread`" — it is the difference between one I/O operation and several million.

### 7.5 One genuine point in AeroSLS's favour

The author's I/O-mode comparison found unbuffered reads fastest on his hardware:

```
buffered: 0.17 tok/s   drop: 0.20 tok/s   direct: 0.23 tok/s   mmap: 0.19 tok/s
```

`O_DIRECT` beat both buffered and `mmap`. AeroSLS's raw NVMe path is inherently direct — no page cache, no double buffering — so it is structurally aligned with the mode that measured best here. That is a real, if narrow, point of architectural agreement, and worth noting since the rest of this section is unfavourable.

It comes with an important counterweight, though: the same article shows the OS page cache is worth a great deal when reads *do* reach storage — cold 0.31 tok/s versus warm 0.48 tok/s (+55%), collapsing to 0.13 tok/s when the page cache is deliberately defeated with `fadvise(DONTNEED)`. AeroSLS has no page-cache equivalent for object data beyond per-subsystem lazy frame loading, so it forgoes that free second-level cache entirely. Direct I/O measured best *in the presence of* a healthy page cache holding 93 GB of streamed experts, not as a replacement for one.

### 7.6 Where the performance actually comes from, and why AeroSLS cannot go there

The article's own summary of the recipe: *"Get the experts resident, and if your CPU is the bottleneck, move the compute to the GPU."*

The measured spread across placements of the identical model on one box:

| Placement | Throughput |
| --- | --- |
| CPU only, 20 GB budget | 0.28 tok/s |
| CPU only, 200 GB budget | 0.28 tok/s |
| 4× NVIDIA L40 (185 GB VRAM hot tier) | **2.02 tok/s** |

The ~7× gain comes from moving *compute* to the GPU, not from moving *data* closer. On the GPU the kernel dominates (290 ms) while PCIe transfers total under 100 ms — compute-bound again, just at a much higher ceiling. Reported third-party CPU results cluster in the same low range as the CPU rows above (Framework 13 ~0.37 tok/s; Ryzen 9950X 0.10–0.28; i5-12600K ~0.08), with the notable exception of an Apple M5 Max at ~2.06 tok/s **using its integrated GPU via the Metal backend** — again, an accelerator result, not a memory-architecture result.

AeroSLS has no GPU support of any kind: no PCIe device driver for a GPU, no CUDA/Metal/Vulkan path, no way to reach an accelerator. The only lever that measurably moved this workload is unavailable, and building it is far outside anything contemplated in §3.

### 7.7 Revised recommendation

The prior recommendation (§5) stands as to what *not* to do, and strengthens: **do not attempt this port.** The reasoning changes, though, and matters for how the effort is redirected.

§5 argued the port would be unfair to AeroSLS because §2's blockers would dominate. The stronger reason is now that **the experiment has already been run by the author and answered in the negative**: eliminating disk I/O on this workload does not increase throughput, because the constraint is memory bandwidth and arithmetic throughput. There is no version of AeroSLS — however good its residency management becomes — that improves DRAM bandwidth or supplies vector arithmetic it does not have. The premise is not merely hard to test on the current build; it is contradicted by the primary source's own controlled measurement.

Revised priorities:

1. **§3 item B (SIMD) moves to first place, and its justification changes.** It is no longer "nice for LLM math" — it is the only item on the list that addresses the constraint the article identifies as binding. It also stands on its own merits for `vecstore`/`rowstore` (this codebase currently computes vector distances in scalar x87 with hand-rolled `vs_sqrtf`). Still cannot be validated in this environment; still needs real hardware.
2. **§3 item A (async/multi-page NVMe) stays worth doing, with an important caveat.** The multi-page transfer fix is unambiguously right — one 19 MB read versus ~4,860 4 KiB reads is not a close call, and it benefits every existing subsystem. But the *concurrency* half should be treated with more caution than §3 implied: the author measured concurrent reads making things worse on a serialized-latency disk. Build the capability, default it off, and measure per machine — exactly the posture the article arrived at.
3. **§5 step 2 (the prefetch benchmark) is withdrawn as framed.** The author's router lookahead already achieves 72% next-layer prediction and still lost to serial loading on his disk. Re-running a weaker version of that experiment on a kernel with no SIMD and 512 MiB of object capacity would not produce a publishable or actionable result.
4. **Hardware justification, restated more sharply.** §5 advised buying for the reference implementation's needs rather than an expected AeroSLS speedup. The article's data makes the specific priorities clear: for CPU-only operation, memory *bandwidth* matters more than capacity past ~20–26 GB (the 200 GB run was no faster than the 20 GB run), and an accelerator is the only thing that produced a large gain. If the goal is to run this model quickly, the purchase decision is about GPU and memory bandwidth. If the goal is to study AeroSLS, this model is not the right instrument.

### 7.8 Verification note on §7

Every figure in §7 is quoted or directly derived from the local copy of the article; none is estimated. Derived values are: the ~4,860-commands-per-expert figure (19 MB ÷ 4 KiB) and the ~2.7-million-commands-per-token figure (11 GB ÷ 4 KiB), both arithmetic on the article's stated quantities and this codebase's audited 4 KiB transfer cap. The article's numbers are one author's measurements on specific hardware — notably a 124-core EPYC and a virtualized-filesystem NVMe whose serialized latency he explicitly flags as the cause of the pipelining regression — and the I/O-concurrency findings in §7.3 in particular should be read as hardware-specific, as he himself insists. No AeroSLS measurements were taken; §2's blockers remain code-audit findings, and this environment still cannot build or boot the kernel.
