## QEMU TCG Optimization Strategies in the Context of a Single Level Storage Architecture

AeroSLS’s kernel‑native, SLS‑based environment eliminates the user/kernel boundary, provides direct control over the MMU, and makes persistence a first‑class citizen. That fundamentally alters the cost/benefit ratio of nearly every traditional TCG optimzation idea.

Here’s how each concept shifts when QEMU runs **in kernel mode** on a **Single Level Storage** architecture, without libc or traditional userspace:

---

### 1. Tiered Compilation (Baseline + Optimizing JIT)

**Massively easier to justify**

- You can store profiling data, baseline code, and optimised traces all in persistent SLS memory. No need for a separate disk cache; persistence is free.
- A background optimisation thread can run directly in the kernel without IPC overhead, exploiting idle cores aggressively.
- Code layout can be optimised for the CPU’s cache hierarchy, and the result can be reused across reboots simply by keeping it in SLS.

**Caveat:** You must manage kernel‑space memory pressure carefully; a runaway optimiser could consume too much persistent memory, so a limit/eviction policy is still needed.

---

### 2. Trace‑Based JIT

**Ideal fit**

- Because all memory is SLS, traces can reference guest memory addresses directly without worrying about paging or segmentation faults (as long as the host‑side mapping is set up correctly).
- The translator can perform aggressive *link‑time optimisation* across traces, safe in the knowledge that the translated code will persist and never be paged out unexpectedly.
- Persistent trace caches can be shared among multiple QEMU nodes, amortising translation cost across identical guest workloads.

---

### 3. Indirect Branch Acceleration

**Can be implemented with hardware support**

- Running in kernel mode gives you full access to performance monitoring (LBR, BTS, Intel PT). You can implement a *software branch predictor* that periodically reprograms branch targets based on real hardware traces—far more accurate than static heuristics.
- Polymorphic inline caches are still valuable, but you can also use the MMU to trap mispredictions: map indirect jump target pages in a way that a page fault directs you to a correction handler. This is heavy but possible in a kernel‑native DBT.

---

### 4. Profile‑Guided Optimization (PGO)

**Becomes free and omnipresent**

- In SLS, *all* translated code is persistent. There’s no “cold start” after reboot—the entire translation cache survives.
- Profiling counters can be embedded directly in translated blocks and written back to SLS without filesystem overhead. Aggregated over many runs, you get true *lifetime* PGO, not just single‑execution sampling.
- The kernel can even share profile data between different nodes running the same guest binary, thanks to the global SLS address space.

---

### 5. Soft‑MMU Overhead Reduction via Host‑MMU Tricks

**Transformative – this becomes the highest‑impact optimisation**

- QEMU in kernel mode can manage its own page tables directly. You can build a *shadow page table* that mirrors the guest’s address translation, then let hardware TLB miss handling do the work.
- Guest‑physical memory can be mapped contiguously in the host’s SLS address space using large pages (1 GB or 2 MB), virtually eliminating soft‑TLB lookups for guest memory access.
- On a TLB miss, you can take a page fault, quickly walk the guest’s page table in software, update the shadow tables, and return—exactly like a VMM but without the expensive VM exit. This is **orders of magnitude faster** than calling `softmmu_template.h` on every load/store.
- Because SLS makes all memory persistent and uniform, you can pre‑populate these mappings at boot and never tear them down.

**Note:** You’d still need a fallback for guests that remap memory frequently, but even then, a lightweight TLB‑like structure in the shadow fault handler is far cheaper than the current helper‑call model.

---

### 6. SIMD‑Style Emulation of Multiple Guest Instructions

**Unchanged in feasibility, but easier to coordinate**

- Kernel‑mode access to wide SIMD units is identical to userspace (Ring 0 vs. Ring 3 doesn’t matter for AVX‑512).
- However, you can schedule vectorised emulation threads on specific cores without the overhead of system calls, making it simpler to pin worker threads to SMT siblings.

---

### 7. Machine Learning–Guided Translation

**Slightly easier to deploy, but watch latency**

- An ML inference engine can be embedded as a kernel module and can directly read profiling counters from persistent memory. Training could occur offline, but inference can run in the kernel context.
- Still, you must avoid blocking the TCG translation pipeline, so the model should be ultra‑lightweight (e.g., a decision tree or a tiny neural net using CPU‑friendly inference).

---

### 8. Decoupled Access–Execute (DAE) Emulation

**May become unnecessary with host‑MMU tricks**

- The primary motivation for DAE was to hide memory latency. If memory access is handled by hardware page walkers (after shadow‑page‑table mapping), the latency is hidden naturally by the core’s out‑of‑order execution. DAE then adds complexity for marginal gain.
- However, if you’re emulating guest devices or MMIO, a DAE‑like split between pure computation and MMIO handling might still be useful.

---

### 9. Offloading to Accelerators (GPU / eBPF)

**eBPF becomes incredibly attractive**

- Since you’re kernel‑native, you can generate eBPF directly from TCG output and feed it to the kernel’s in‑kernel JIT. This would let you run simple guest‑code fragments in a highly optimised sandbox that the kernel already manages.
- GPU offloading is trickier: kernel drivers for GPUs exist, but managing GPU tasks from a kernel‑native emulator would require deep integration with the GPU scheduler. Likely not worth the engineering overhead unless you emulate many VCPUs that do data‑parallel work.

---

### 10. Persistent and Shareable Translation Cache

**This is no longer “novel”; it’s the default**

- SLS inherently means that the entire address space is persistent. You simply place the translation cache in a known memory region, and it survives reboots.
- Multiple QEMU nodes can map the same cache region read‑only (or copy‑on‑write) and share translated code. The kernel can arbitrate concurrent updates with atomic operations, essentially giving you a “translation‑cache daemon” without any daemon at all.
- You could even ship optimised translations for popular guest binaries in a pre‑seeded SLS region—an AOT model without a separate build step.

---

### What New Opportunities Emerge?

- **Zero‑copy device emulation:** Because all storage is SLS, you can map guest DMA buffers directly into the host’s address space and let device emulation work on them with zero copy.
- **Whole‑system snapshot optimisation:** Translated code is part of the SLS snapshot; restoring a snapshot doesn’t require re‑translation.
- **Kernel‑assisted concurrency:** If you run multiple QEMU nodes, the kernel can schedule them with complete transparency, and they can share a single soft‑MMU page fault handler, avoiding redundant translations.

---

### Bottom Line: Priorities Shift

The **#1 priority** becomes **host‑MMU‑based guest memory emulation** (idea #5). Combined with a **persistent shared translation cache** (idea #10) and **persistent PGO** (idea #4), you can likely achieve near‑hardware‑virtualisation performance *without* KVM, because the major TCG bottlenecks—soft‑MMU and cold start—are eliminated by the architecture.

Ideas like tiered compilation and trace‑based JIT are now “nice to have” for further squeezing out CPU‑bound performance, rather than existential improvements. The kernel‑native, SLS context makes the most impactful optimisations practical and almost trivial to implement relative to a userspace‑only QEMU.
