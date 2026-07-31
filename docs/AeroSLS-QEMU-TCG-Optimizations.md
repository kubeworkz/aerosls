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

---

Let’s design a concrete, phased roadmap that turns the three synergistic ideas—host‑MMU‑based guest memory, persistent shared translation cache, and persistent profiling‑guided optimisation—into a working architecture for QEMU TCG running kernel‑native on AeroSLS.

#### We’ll assume the following AeroSLS properties:

- **Single Level Storage:** All memory is persistent by default, byte‑addressable, and part of a single unified virtual address space (no separate “disk”). Reboot restores the entire memory state.
- **Kernel‑native execution:** QEMU runs in privileged mode; no libc, no system calls. It can directly manipulate page tables, manage its own address space, and use atomic instructions without user/kernel crossings.
- **Multiple QEMU nodes:** The system can run several emulated machines (or vCPUs) concurrently, potentially sharing code and data.

---

## Phase 0 – Baseline: QEMU TCG in Kernel Mode

Before adding magic, we need a stable foundation.

**Minimal changes:**

- Replace libc dependencies with kernel‑native equivalents: direct `mmap`‑like allocations from SLS, simple memory allocator (slab/buddy), atomic ops, and a spinlock‑based synchronisation.
- The TCG translator and runtime must use only raw kernel‑exposed functionality. No `printf`; use a ring buffer for logging.
- Leverage SLS to keep the TCG code cache in persistent memory, but initially still with the standard `tb_invalidate` mechanism (hash table + flushing on write).

**Deliverable:** A working, kernel‑mode QEMU that boots a guest OS, using standard soft‑MMU helpers. This gives us a reference performance baseline.

## Phase 1 – Host‑MMU‑Based Guest Memory Emulation (Soft‑MMU Elimination)

The goal: make guest loads/stores execute as a single host memory instruction, completely removing the soft‑MMU helper call from the hot path.

### 1.1 Map Guest Physical Address Space Contiguously

- Allocate a large, aligned region in the host kernel’s virtual address space – say `gpa_base`.
- At QEMU initialisation, map all of guest physical RAM into `gpa_base..gpa_base+ram_size` using a 1:1 physical mapping (or an offset, e.g., `gpa_base = phys_offset`). Use huge pages (1 GB or 2 MB) to minimise TLB pressure.
- This region becomes the “direct map” for guest physical memory. Any guest‑physical address `gpa` is accessed at `gpa_base + gpa`.

### 1.2 Shadow Page Tables for Guest Virtual Addresses

Each guest virtual address space (typically one per guest process) needs a host‑side page table that maps guest virtual pages to the corresponding host virtual pages (i.e., to `gpa_base + guest_physical_page`). This is exactly a shadow page table.

- **Structure:** Maintain a per‑guest‑address‑space shadow page table (top‑level directory). When a guest switches its page tables (e.g., `mov to cr3` on x86), we switch the host’s active page tables for that vCPU thread to the corresponding shadow table.
- **Synchronisation:** Write‑protect the guest’s page‑table pages themselves (using host page table permissions). Any guest write to a page table page triggers a fault. The fault handler:
  1. Walks the guest page table to see what changed.
  2. Updates the shadow page table entry (or entire subtree) accordingly.
  3. Invalidates any translation blocks (TCG code) that reference the affected virtual pages (more on this later).
  4. Returns.
- **Self‑modifying code:** Guest writes to code pages are already trapped via page‑write protection (standard QEMU technique). The same mechanism can be reused for page‑table write protection.

### 1.3 Adapting TCG to Emit Direct Accesses

With shadow paging active, a load from guest virtual address `gva` becomes a simple host load from `gva`. This works because we arrange the host virtual address space so that the guest’s virtual addresses map directly to the host virtual addresses managed by the shadow tables. We can do this by allocating a large virtual address range (e.g., the lower 48 bits) for each guest address space. Since we control the host page tables, we can set up the root page table to give each guest process its own virtual address mapping. For instance, we could reserve a huge region of the host virtual address space (`guest_virtual_base`) and for each guest process, use a different top‑level page‑table entry (e.g., ASID‑tagged or separate CR3). The generated host code then uses absolute addresses within that region.

**TCG backend changes:**

- When a TCG `qemu_ld/st` op is generated for a guest virtual address, if the address is known at translation time (direct address), emit a direct load/store to that address (relative to the process’s base). If the address is computed (register), emit the load/store using the guest virtual address register directly – the hardware will walk the shadow tables. No helper call needed.
- For MMIO regions, we cannot map them directly (they require device emulation). These must still go through a slow path. We handle this by:
  - Using a “fault‑and‑fixup” strategy: mark MMIO pages as non‑present in the shadow tables. When a direct access faults, the handler inspects the faulting guest physical address, and if it falls in an MMIO region, it calls the device emulation code. This is still expensive, but MMIO is rare.
  - Alternatively, we can keep a small per‑vCPU lookup table that maps “problematic” GPAs to callbacks, but the faulting method is cleaner and leverages the hardware.

**Self‑modifying code and translation invalidation:**

- Since generated code now contains absolute guest virtual addresses, any change to the corresponding guest→host mapping (i.e., page table modification) requires invalidating all translation blocks that reference the old virtual page. We implement this with a reverse mapping: for each guest physical page that contains code, maintain a list of translation blocks that reference it. When the guest writes to a code page (caught by write‑protection), we invalidate those blocks. For page table changes that alter a mapping, we invalidate blocks that contain a memory access to the remapped virtual page; this is trickier. One efficient approach: keep a per‑guest‑virtual‑page list of translation blocks that access that page. The number is small because a block typically accesses a few addresses. We can build this list at translation time and store it alongside the translation cache entry.

### 1.4 Handling Guest Context Switches

When the guest OS switches to a different process (different set of page tables), the vCPU thread must switch to the shadow table for that process. This requires:

- A quick mapping from guest CR3 (or equivalent) to the host CR3 for that shadow table.
- The host kernel can manage a small cache of shadow tables (like KVM’s shadow MMU caches). Since all memory is SLS, we can afford to keep many shadow tables persistent.
- The TCG code that runs after the switch will use the new address mappings transparently because they use guest virtual addresses directly.

### 1.5 Expected Performance Gain

This phase removes the largest overhead (soft‑MMU calls) from most memory accesses. On CPU‑bound workloads, a 3–5× speedup is realistic. The cost of page‑fault handling for initial TLB misses is amortised, especially with huge pages and persistent shadow tables.

---

## Phase 2 – Persistent Shared Translation Cache

Now we make translation results durable and shareable across QEMU nodes.

### 2.1 Design of the Cache

- **Storage:** A large, contiguous SLS‑backed region, managed as a set of “cache lines” or “blocks”.
- **Key:** The guest instruction pointer (physical address in system‑emulation mode, or virtual address + ASID if we extend) uniquely identifies a basic block. We use `(guest_pc, asid)` as the key for a translation block.
- **Data structure:** A lock‑free, concurrent hash table (e.g., split‑ordered list, or a multi‑level array indexed by PC). Because SLS memory allows atomic operations and is persistent, we can use a wait‑free design for lookups. For insertion, we can use compare‑and‑swap on pointer updates.
- **Block metadata:** Alongside the host code, we store:
  - Size of translated code.
  - Source guest address and flags.
  - List of referenced guest pages (for invalidation).
  - Profiling counters (execution count, timestamp).
  - Chain links to direct jumps.

### 2.2 Sharing Between Nodes

Multiple QEMU vCPUs (or whole QEMU instances) can map the same cache region read‑only and rely on a single writer to insert new translations. Alternatively, each node can have a private write area and later merge into a global cache asynchronously. Simpler: use a single global cache with CAS‑based insertion; contention on the hot path is minimal because translation is much rarer than execution.

**Coherence:** When one node invalidates a block (because the guest wrote to the code page), it must be reflected globally. We use a versioned invalidation:

- Each guest physical code page has a generation counter stored in SLS.
- A translation block is tagged with the generation of its source page. Before executing a block, we check the generation matches (a fast load). On mismatch, the block is invalidated and a new translation is fetched/created.
- This avoids expensive cross‑CPU TLB shootdowns; the generation check is a simple compare‑and‑branch in the block prologue, costing ~2 cycles.

### 2.3 Integration with Shadow Paging (from Phase 1)

The translated blocks contain direct memory references. Those references are tied to a particular shadow‑table configuration (which is tied to a guest page‑table root and its mappings). Therefore, the translation cache must be keyed by something that captures this context. Possibilities:

- **Key by guest virtual address + current ASID (CR3).** This is clean: each guest process gets its own translations, and they are valid as long as the shadow table for that process hasn’t changed. When the guest modifies its page tables, the affected shadow page mappings change; we must invalidate blocks that reference the remapped virtual pages. As described, we maintain per‑virtual‑page reverse mappings. The cache then holds per‑process blocks, which is acceptable because they are small.
- Alternatively, keep translations keyed by guest physical address (code) and use a level of indirection for data accesses: instead of embedding absolute guest virtual addresses, embed “handle” that indexes into a per‑thread TLB. But that’s a soft‑MMU again. We prefer the full shadow‑table approach.

So the cache key becomes `(guest_cr3, guest_virtual_pc)` for user‑mode code, or simply `(guest_physical_pc)` for kernel code that uses a 1:1 mapping. The design is flexible.

### 2.4 Invalidation Details

- On a guest write to a code page (detected via write‑protect), we bump the generation number of that physical page. Any block tagged with the old generation will fail the prologue check and fall through to a slow path that removes it and requests a retranslation.
- On a guest page table modification that changes a virtual mapping, we iterate over the reverse mapping of the affected virtual page and invalidate all blocks that reference it by setting a “zombie” flag or removing them from the hash table. The next attempt to jump to such a block will trap and retranslate.

---

## Phase 3 – Persistent Profiling‑Guided Optimisation (PGO)

With a persistent translation cache, we can accumulate execution profiles across runs and use them to generate better code.

### 3.1 Lightweight Profiling Counters

- Embed a 64‑bit counter at the beginning of each translated block. The block prologue atomically increments it (or a per‑block counter in a separate array to avoid cache‑line bouncing). Since SLS allows persistent atomic writes, the counter survives reboots.
- Optionally, also count indirect branch targets using a small miss‑table per indirect branch site, stored in the block’s metadata.

### 3.2 Background Optimisation Agent

- A kernel thread (or idle vCPU) periodically scans the translation cache for “hot” blocks – those with high execution counts.
- For hot blocks, it triggers a **re‑translation with higher optimisation**:
  - Full liveness analysis and register allocation across the block (or a trace).
  - Inlining of common soft‑MMU slow paths (now they are rare, but MMIO still needs inlining).
  - Constant propagation from known CPU state (e.g., EFLAGS, segment bases).
  - Vectorisation opportunities using host SIMD.
- The optimised block is placed in a separate region of the cache, and the original block’s direct‑jump links are patched to point to the new version. The old version remains as a fallback.

### 3.3 Trace Formation

The profiling data also reveals hot paths through multiple blocks. The optimiser can form **traces** (linear sequences of hot blocks) and compile them as a single unit, eliminating inter‑block jumps and enabling cross‑block optimisations. Traces are keyed by the starting PC and the path of taken branches. This is especially powerful with persistent profiling because the trace may span code that is not hot on the first run but becomes hot after the system has been running for a while.

### 3.4 Co‑optimisation with Shadow Paging

Optimised blocks can embed direct memory accesses with known guest virtual addresses. The same invalidation mechanisms apply: if a guest page table update remaps a page that the optimised block touches, the reverse mapping will catch it and invalidate the optimised block. Since optimised blocks are larger and touch more pages, the reverse mapping overhead is still linear in the number of pages accessed, which is manageable.

### 3.5 Leveraging Persistent SLS

- Profiling data and optimised traces are persistent. After a reboot, the system immediately starts with highly optimised code for the parts of the guest OS that were hot before. This effectively provides **AOT‑like performance with JIT‑like adaptability**.
- The system can even run a “training mode” the first time a guest workload is run, then lock the optimised cache for subsequent runs.

---

## Phase 4 – Refinements and AeroSLS‑Specific Enhancements

### 4.1 Zero‑Copy Device Emulation

Because all memory is SLS, DMA buffers can be passed between guest and device emulation by simply sharing pointers. No copying, no pinning. The shadow page table can map guest‑physical DMA pages directly into the device emulation thread’s address space.

### 4.2 Snapshot and Restore at Native Speed

Since translated code and all guest‑state are in persistent SLS memory, taking a snapshot is just recording the state of the page tables and register files. Restoring reuses the existing translation cache and shadow tables. No re‑translation on resume.

### 4.3 Adaptive Optimisation Policies

Use the persistent counters to feed simple ML models (a decision tree running in a kernel thread) that decide when to optimise, when to trace, and when to evict cold blocks. This can be made completely autonomic.

### 4.4 Security and Stability

Running complex optimisation passes in kernel mode requires careful validation. All generated code must be sandboxed: the host page tables can mark the generated code region as read‑only after writing, and use execute‑only memory if the architecture supports it. The optimiser itself can run in a separate process context with limited privileges.

---

## Design Path Summary (Milestones)

1. **Baseline QEMU TCG** in kernel mode (standard soft‑MMU).
2. **Add shadow paging** – direct memory access for RAM, fault‑driven MMIO. Measure speedup.
3. **Build persistent global translation cache** with CAS‑based insertion and generation‑based invalidation. Ensure coherence across nodes.
4. **Integrate reverse mappings** and handle translation invalidation on page‑table changes.
5. **Implement persistent profiling counters** and a re‑optimisation loop that produces trace‑optimised code.
6. **Enable cross‑node sharing** of the optimised cache.
7. **Polish** – zero‑copy device, snapshot/restore, adaptive policies.

At each stage, the kernel‑native, SLS environment directly enables the design. The result is a dynamic binary translator that approaches the performance of hardware virtualisation (KVM‑like direct execution) for memory accesses, while retaining the portability and flexibility of QEMU’s TCG multi‑target frontend.

This roadmap is ambitious but logically incremental. Each phase builds on the previous one, and the end result is a fundamentally more efficient emulation engine tailored to the unique strengths of AeroSLS.

---

## Is this KQEMU?

KQEMU is a perfect historical lens—because it shared the same goal (make QEMU much faster by moving into kernel space and using the host MMU), but it chose a fundamentally different mechanism. Let’s contrast that with the design path we’ve just outlined.

## KQEMU: The “Run‑it‑natively‑when‑possible” Accelerator

KQEMU (2004–2009, x86‑only, Linux/Windows kernel modules) was an **accelerator**, not a re‑built TCG. Its core idea:

- **Split execution:** Most guest code (user‑mode, ring‑3, and some ring‑0 without privileged instructions) runs **directly on the host CPU** at full speed.
- **Fallback:** When a privileged instruction, MMIO access, or unsupported corner case occurs, the kernel module traps and hands control back to QEMU userspace (or the module’s own slow path) for full software emulation.
- **Guest memory mapping:** The module mapped guest physical RAM into a large contiguous chunk of the host kernel’s virtual address space, so native guest code could access memory without translation—the host MMU handled it.
- **No binary translation for “hot” code:** It executed native instructions, not translated ones. TCG (then called “dyngen”) only emulated the tricky parts.

**The result:** Near‑native CPU‑bound performance, as long as the workload spent most of its time in non‑privileged code. Overheads came from frequent context switches between kernel module and QEMU userspace, and from the need to constantly trap and emulate privileged operations.

KQEMU was eventually obsoleted by KVM, which used Intel VT‑x / AMD‑V to let the *hardware* handle privileged instruction trapping and guest page tables, doing the whole thing much more cleanly.

---

## Our AeroSLS Design: The “Accelerate DBT with Hardware MMU” Approach

The current proposal *keeps TCG as the sole execution engine*. It does **not** run any guest code natively. Instead:

- **TCG still translates guest instructions → host instructions.** Every instruction is emulated via dynamic binary translation.
- **But memory accesses in the translated host code become direct loads/stores**, because the host MMU is set up with shadow page tables that map guest virtual addresses to host virtual addresses (which point to guest physical RAM). No soft‑MMU helpers on the hot path.
- **Trap‑and‑emulate for MMIO and page‑table changes** is used sparingly, by marking those pages non‑present in the host page tables.
- **Persistence and profiling** allow the translation cache and optimised traces to survive reboots and improve over time—features KQEMU never had.

So **functionally it’s closer to Rosetta 2**, FEX‑Emu’s “host‑MMU mode”, or Box64’s “native address space” techniques: a *pure DBT that borrows the host MMU to make guest memory accesses cheap*.

---

## Side‑by‑Side Comparison

| **Aspect**                  | **KQEMU**                                                             | **Proposed AeroSLS Design**                                                                                                  |
| --------------------------- | --------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| **Execution model**         | Direct native execution for most code, fallback to software emulation | Always DBT (TCG), never native                                                                                               |
| **Guest ISA support**       | Only x86‑on‑x86 (same ISA)                                            | Cross‑ISA (any TCG target‑host pair)                                                                                         |
| **Memory access**           | Direct mapping of guest physical RAM; host MMU used natively          | Shadow page tables give direct virtual→virtual mapping, host MMU walks them                                                  |
| **Privileged instructions** | Trapped and emulated by kernel module                                 | Emulated entirely by TCG code (with no host privilege traps)                                                                 |
| **Performance ceiling**     | Near‑native for non‑privileged user code                              | Still DBT overhead, but soft‑MMU eliminated → can reach 50–80% of native for CPU‑bound code if TCG optimizations are applied |
| **Flexibility**             | Tied to x86 host and guest; fragile, architecture‑specific            | Fully portable; works for any TCG target because DBT remains in charge                                                       |
| **State persistence**       | None; cache lost on reboot                                            | All translations, profiles, and shadow tables persistent across reboots                                                      |
| **Multi‑node sharing**      | Not designed for it                                                   | Shared persistent translation cache and shadow page tables across QEMU nodes                                                 |
| **Optimisation level**      | Static, monomorphic                                                   | Profile‑guided, tiered compilation and trace formation evolve over time                                                      |

## Why Not Just Do KQEMU 2.0 on AeroSLS?

You could, in theory, build something like KQEMU for AeroSLS—run unmodified guest code directly on the host CPU, using the kernel’s page tables to map guest memory. But:

1. **It would be same‑ISA only** (guest ISA must match host). AeroSLS/QEMU’s strength is multi‑architecture support.
2. **It requires handling all privilege‑level transitions in software**, which is extremely complex and fragile (the same reason KQEMU was abandoned).
3. **Hardware‑assisted virtualisation (KVM) does it better**, and on modern CPUs it’s already there. If you have VT‑x / AMD‑V, you just use KVM. If you don’t, a KQEMU‑like approach is an immense amount of work for a shrinking benefit.
4. **The kernel‑native, SLS environment doesn’t magically simplify privilege emulation**; it still requires ring compression gymnastics.

In contrast, the DBT‑plus‑host‑MMU approach keeps QEMU’s full cross‑ISA portability and doesn’t wrestle with guest privilege rings—it just emulates them inside the translated code, which is already battle‑tested.

## So What’s the Right Analogy?

This design is best described as **“Self‑Modifying Shadow‑Page‑Table‑Assisted DBT”** – a software‑only dynamic translator that gets hardware‑grade memory performance via kernel control of the MMU, plus all the benefits of persistent storage.

- It’s like **KVM’s shadow page table logic**, but applied to a userspace/kernel DBT instead of hardware virtualisation.
- It’s like **Rosetta 2** on Apple Silicon, which translates x86‑64 to ARM64 and uses the host’s TLB and page tables for memory access, but with the added twist of persistence and kernel‑native execution.
- It’s **not KQEMU**, because it never runs a single guest instruction natively.

If KQEMU was “virtualization without hardware assist,” this is “memory virtualization without hardware assist, but with DBT.” It borrows the memory management tricks of virtualization, but not the execution‑mode tricks. That’s a sweet spot on a system like AeroSLS where kernel‑level MMU control is free, persistent memory is infinite, and cross‑ISA flexibility is paramount.
