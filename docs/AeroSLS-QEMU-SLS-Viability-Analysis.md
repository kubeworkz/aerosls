## Viability Analysis: QEMU-SLS (Phases 0–4)

### Phase 0 — Baseline: QEMU TCG in Kernel Mode

**Verdict: Viable but significant effort. This is the hardest phase.**

| **Dependency**                                                                                                                                                                                           | **Scope**                                                                                                                                                                                                      | **Replacement Strategy**                                                                                                                                                                                                                                                                 |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **GLib** (`g_malloc`, `g_free`, `g_hash_table`, `g_assert`)                                                                                                                                              | ~36 calls in tcg.c, 12 in [cputlb.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html)      | Replace with a kernel slab allocator. AeroSLS already has [frame_pool.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html); a bump allocator for TCG temps is trivial |
| **libc** (`mmap`, `mprotect`, `munmap`)                                                                                                                                                                  | [tcg/region.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) code-gen buffer allocation | Replace with direct page-table manipulation — you already do this in [user_paging.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html)                                |
| **POSIX threads** (`pthread_*`)                                                                                                                                                                          | Multi-threaded TCG (MTTCG)                                                                                                                                                                                     | Replace with AeroSLS's process/scheduler or run single-vCPU initially                                                                                                                                                                                                                    |
| `qemu/osdep.h`                                                                                                                                                                                           | 42 includes in [cputlb.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) alone           | The largest shim layer needed — provide thin wrappers for the ~20 types/macros it defines                                                                                                                                                                                                |
| **Coroutines** ([qemu-coroutine.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html)) | Block layer, not TCG hot path                                                                                                                                                                                  | Not needed for Phase 0 (no block devices in the initial SLS target)                                                                                                                                                                                                                      |

**Core TCG engine** is ~21K lines (`tcg/`). The **accelerator runtime** is ~12K lines (`accel/tcg/`). The **x86-64 backend** (`tcg/x86_64/`) adds ~3K. Total: **~36K lines** to port, but:

- **Only ~13K lines are the hot path** (tcg.c + [cputlb.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) + cpu-exec.c + [translate-all.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) + tcg-op-ldst.c)
- TCG already has its own internal allocator (`tcg_malloc_internal` → `TCGPool`) that uses `g_malloc` underneath. Replacing `g_malloc` with a single persistent-memory allocator covers 80% of GLib usage
- TCG's code-gen buffer is allocated ONCE at startup — this maps perfectly to a fixed SLS region

**Risk:** The main risk is the transitive header dependency web. `qemu/osdep.h` pulls in system headers, which pull in more. A **shim header** (`qemu-sls-shim.h`) providing `stdint.h`, `stddef.h`, `stdbool.h`, [assert()](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html), and the handful of QEMU-internal types would cut the dependency graph.

**Estimated effort:** 2–4 weeks for a single developer to get `qemu-system-x86_64 -machine q35 -nographic -kernel <tiny-guest>` booting under AeroSLS with basic serial output.

---

### Phase 1 — Host-MMU Shadow Page Tables (Soft-MMU Elimination)

**Verdict: Highly viable. This is where AeroSLS's architecture gives you an unfair advantage.**

The current QEMU soft-MMU path in [cputlb.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) (2902 lines) does:

1. TLB lookup (fast path ~5 lines of inline code)
2. On miss: `victim_tlb_hit()` check → page table walk → TLB refill
3. MMIO detection → `io_failed()` slow path

With AeroSLS controlling page tables directly:

- **Step 1.1** (contiguous GPA mapping): You already manage physical memory via [frame_pool.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html). Mapping guest RAM at a fixed 1:1 offset is literally `set_pte()` in a loop — the same operation [user_paging.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) already performs for Ring-3 processes
- **Step 1.2** (shadow page tables): AeroSLS's [walk_page_tables_x86.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) already walks x86 4-level page tables. Building a shadow is the same code with different source/dest
- **Step 1.3** (TCG backend change): Replace `qemu_ld/st` helper calls with direct `mov` instructions. The TCG x86-64 backend (`tcg/x86_64/tcg-target.c.inc`) already has a fast-path for `qemu_ld` that inlines the TLB check — you'd replace the TLB miss path with "just let it fault"
- **Step 1.4** (fault handler): You already have `handle_page_fault()` in [stubs.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) that dispatches on Ring-0 vs Ring-3. Adding "if faulting address is in guest region → walk guest page table → update shadow → iret" is ~50 lines

**Key insight:** The [cputlb.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) file becomes **almost entirely dead code** after this phase. You keep only the MMIO dispatch path.

**Expected impact:** 3–5x speedup on memory-intensive workloads (validated by KQEMU's historical results and FEX-Emu's MMU-bypass measurements).

**Estimated effort:** 1–2 weeks on top of Phase 0.

---

### Phase 2 — Persistent Shared Translation Cache

**Verdict: Straightforward. SLS makes this nearly free.**

Currently QEMU's translation cache:

- Allocated via `mmap` in [tcg/region.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) (one call)
- Indexed by a hash table in `tb-hash.h` (simple open-addressing)
- Flushed on `tb_invalidate` via linked lists

In SLS:

- **The code-gen buffer IS already persistent** — you just don't `memset` it on boot
- **The hash table IS already persistent** — it lives in the same address space
- **Translation blocks survive reboots** — no re-translation of hot code

What you actually need to build:

1. A generation counter per guest code page (4 bytes per page, stored in a small array)
2. A block-prologue check: `cmp [page_gen], expected_gen; jne invalidate` (~6 bytes of x86 code per block)
3. CAS-based insertion for multi-vCPU (the [qht.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) in `util/` is already lock-free, just needs GLib shims)

**Risk:** Stale translations after a guest OS upgrade (different binary at the same address). Mitigated by the generation counter — any write to a code page bumps it.

**Estimated effort:** 1 week — mostly plumbing the generation array and modifying `tb_gen_code()` to emit the prologue check.

---

### Phase 3 — Persistent PGO

**Verdict: Viable as a background enhancement. Low risk, high long-term reward.**

QEMU already has execution counting infrastructure (`icount`). The change is:

1. Embed a counter per TB in persistent memory (already there from Phase 2)
2. A background thread scans for hot blocks (your scheduler already supports this — [workload.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html))
3. Hot blocks get re-translated with TCG's existing [optimize.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) pass applied more aggressively

The "hard" part — actually making the optimizer produce better code — is bounded by TCG's existing optimization passes. You're not writing a new optimizer; you're just running the existing one on a hotter input set.

**Risk:** Minimal. The re-optimization is async and non-blocking. A bad optimization just means you fall back to the baseline translation.

**Estimated effort:** 2–3 weeks for the full loop (profiling → detection → re-translation → patching).

---

### Phase 4 — Refinements (Zero-Copy DMA, Snapshots, Adaptive)

**Verdict: These are natural consequences of Phases 0–2, not engineering challenges.**

- **Zero-copy DMA:** Already how AeroSLS works — [stream_store[].frames[]](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) are physical pointers. A guest DMA buffer at GPA X is at `gpa_base + X` in host VA space. No copy needed.
- **Snapshot:** The SLS checkpoint system you just built (Steps 1–5) already snapshots the entire system state. Adding QEMU's register file to the checkpoint is one struct.
- **Adaptive policies:** The [checkpoint_delta.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) dirty-tracking pattern applies directly — mark TBs dirty when their counters exceed a threshold.

**Estimated effort:** 1 week of integration, not new design.

---

## Overall Viability Assessment

| **Phase** | **Feasibility**                              | **Impact**                    | **Effort** | **Dependencies** |
| --------- | -------------------------------------------- | ----------------------------- | ---------- | ---------------- |
| 0         | **High** (mechanical porting)                | Baseline — no speedup yet     | 2–4 weeks  | None             |
| 1         | **Very High** (AeroSLS's sweet spot)         | **3–5x** speedup              | 1–2 weeks  | Phase 0          |
| 2         | **Very High** (SLS makes it trivial)         | Eliminates cold-start penalty | 1 week     | Phase 0          |
| 3         | **High** (bounded by existing TCG optimizer) | 10–30% on hot loops           | 2–3 weeks  | Phase 2          |
| 4         | **Trivial** (consequence of prior phases)    | Polish + integration          | 1 week     | Phases 1–3       |

**Total estimated path to a working QEMU-SLS with shadow paging:** 4–6 weeks.

**The biggest single risk** is Phase 0's dependency untangling. The path to de-risk it: start with [tcg/tci.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) (the TCG Interpreter — a pure-C fallback that doesn't JIT) instead of the x86-64 backend. TCI has zero platform dependencies beyond integer arithmetic and memory access. Get TCI booting a guest first, then swap in the native backend.

**Is this novel?** Yes. No existing system combines:

1. Kernel-mode DBT with direct page-table control
2. Persistent translation caches that survive reboots
3. Hardware-MMU-assisted guest memory without KVM/VT-x
4. Cross-ISA support (unlike KQEMU which was x86-only)

The closest analog is Apple's Rosetta 2 (DBT + host MMU for memory), but Rosetta is userspace-only, proprietary, and non-persistent. This would be the first open, kernel-native, persistent DBT with hardware MMU acceleration.
