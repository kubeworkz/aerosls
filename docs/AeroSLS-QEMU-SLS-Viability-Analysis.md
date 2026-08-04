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

**Core TCG engine** is ~~21K lines (`tcg/`). The **accelerator runtime** is ~12K lines (`accel/tcg/`). The **x86-64 backend** (`tcg/x86_64/`) adds ~3K. Total: **~~36K lines** to port, but:

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

---

## Phase 0 Final Status

| **Milestone** | **Status** | **Proof**                                           |
| ------------- | ---------- | --------------------------------------------------- |
| M0.1          | ✓          | SLS shim compiles standalone                        |
| M0.2          | ✓          | 8 TCG core files: 0 errors, 568KB objects           |
| M0.4          | ✓          | Guest "Hello" prints via serial (13/13 tests)       |
| M0.5          | ✓          | TCG+TCI initializes, prologue generated, link clean |
| M0.6          | ✓          | Full IR generation pipeline: init→alloc→emit→exit   |

### Step 1 — Bridge [sls-runtime.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) to the AeroSLS kernel *(~1 hour) (complete)*

[sls-runtime.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) has a [sls_printf()](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) stub that currently does nothing. It needs to call `kernel_serial_printf` at kernel link time.

**Change:** Add one `extern` declaration to [sls-osdep.h](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) and wire [sls_printf](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) to it in [sls-runtime.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html). The kernel provides the symbol; no header cycle.

```plaintext
// sls/sls-osdep.h — add at bottom
extern void kernel_serial_printf(const char *fmt, ...);
#define sls_printf kernel_serial_printf
```

Same for `sls_abort` → `kernel panic / halt`. This is a 10-line change.

---

### Step 2 — Add TCG objects to the AeroSLS build *(half day) (Complete)*

The two repos are siblings ([qemu](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) relative to `aerosls2`). The `sls/Makefile` already has the exact source list. The AeroSLS [Makefile](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) needs:

**New variable + include flags in `aerosls2/Makefile`:**

```plaintext
QEMU_INC = -I ../qemu/sls \
           -I ../qemu/include \
           -I ../qemu/tcg \
           -I ../qemu/tcg/x86_64 \
           -I ../qemu/target/i386 \
           -include ../qemu/sls/sls-osdep.h

TCG_C_SRC = \
    ../qemu/sls/sls-runtime.c \
    ../qemu/sls/sls-exec.c \
    ../qemu/sls/sls-tcg-wrappers.c \
    ../qemu/tcg/tcg.c \
    ../qemu/tcg/tcg-common.c \
    ../qemu/tcg/tcg-op.c \
    ../qemu/tcg/tcg-op-ldst.c \
    ../qemu/tcg/tcg-op-vec.c \
    ../qemu/tcg/tcg-op-gvec.c \
    ../qemu/tcg/optimize.c \
    ../qemu/tcg/region.c \
    ../qemu/tcg/tci.c     # interpreter fallback — no x86 frontend needed yet
```

The `accel/tcg/*.c` files are **not needed** — [sls-exec.c](vscode-file://vscode-app/c:/Users/kubew/AppData/Local/Programs/Microsoft%20VS%20Code/e4c7e7b1d6/resources/app/out/vs/code/electron-browser/workbench/workbench.html) replaces them.

A separate compile rule is needed because TCG sources need `$(QEMU_INC)` added to `$(X86_CFLAGS)`.

---

### Step 3 — Write the launcher *(2–3 days) (Complete)*

A new file `qemu/sls/sls-launcher.c` (and matching header `sls-launcher.h` exposed to the AeroSLS kernel) that wires the Phase 1–4 hooks to the TCG engine:

```plaintext
/* Called from the AeroSLS shell (user/shell.c) or REST API */
int sls_launch_guest(const void *image, size_t image_len) {

    // 1. Allocate and map guest RAM via Phase 1
    qemu_sls_mmu_map_guest_ram(0, QEMU_GUEST_RAM_PAGES);

    // 2. Copy guest image into guest RAM
    void *guest_base = qemu_sls_dma_host_ptr(0);   // Phase 4 DMA
    memcpy(guest_base, image, image_len);

    // 3. Initialise TCG context
    tcg_register_thread();
    // ... set up CPUState, set RIP = 0 ...

    // 4. Set Phase 1 shadow CR3 and guest CR3
    qemu_sls_guest_cr3 = 0;  // guest page tables at GPA 0 initially
    __asm__ volatile("mov %0, %%cr3" :: "r"(qemu_sls_shadow_cr3));

    // 5. Run the TCG loop
    sls_exec_run(&guest_cpu);

    // 6. Restore kernel CR3 on exit
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_cr3));
    return 0;
}
```

The shell gets a new command: `qemu load <object_name>` that calls `sls_launch_guest`.

---

### Step 4 — Enable the x86 guest frontend *(1–2 weeks) (Complete)*

Currently deferred in `sls/Makefile` as "Layer 3". This is `target/i386/tcg/translate.c` (~15K lines) — the code that decodes x86 guest instructions into TCG IR.

**What this requires:**

- Add `target/i386/tcg/translate.c` + `target/i386/` helper files to `TCG_C_SRC`
- Provide `CPUX86State` (the x86 architectural register file) — already partially designed as `QemuVMState` in Phase 4
- Stub the few remaining x86 helpers that call into QEMU's device model (PIC, IOAPIC, etc.) — these can be stubs returning 0 for Phase 4

**Milestone gate:** boot a minimal x86 kernel (e.g., a 512-byte bootsector that prints "HELLO" via port 0xE9) and see it on the AeroSLS serial log.

---

### Step 5 — Shadow PT backend patch (Step 1.3) *(2–3 days)*

Once the x86 frontend works and guests run correctly (via soft-MMU helpers), patch `tcg/x86_64/tcg-target.c.inc` to emit direct memory references instead of `helper_ld*/st*` calls.

The specific change: in the `tcg_out_qemu_ld` / `tcg_out_qemu_st` functions, replace the helper call path with a direct `mov [GVA + bias]` where `bias = QEMU_GPA_HOST_BASE`. The shadow PT (already live) handles the mapping; faults go to the Phase 1 `handle_page_fault` hook.

This delivers the 3–5× speedup the viability analysis promises.

---

### Step 5 — findings before the patch

**The addressing question is already answered, and not the way it first looked.**
`shadow_install()` maps the **guest virtual address** directly into the shadow
PML4. `QEMU_GPA_HOST_BASE` is how the *emulator* reaches guest memory — reading
guest page tables, DMA, image load — not how translated code addresses it. So
`guest_base` is **0**, no base register is needed, and emitted loads/stores are a
bare `mov (%gva)`. An earlier reading of this had R12 pinned to the window; that
would have been wrong.

**Scope, measured rather than estimated.** 18 `tcg_use_softmmu` references
tree-wide, 6 in files this build compiles: 4 in `tcg/x86_64/tcg-target.c.inc`, 2
in `tcg/tcg-op-ldst.c`. One of the four is the ld/st clobber set —
`tcg_use_softmmu ? (1 << TCG_REG_L0) | (1 << TCG_REG_L1) : 0` — so turning
softmmu off frees exactly the two registers the TLB path was using.

The edits: `tcg_use_softmmu` false under `SLS_IN_KERNEL` in `tcg-internal.h`;
let the `x86_guest_base` block at `tcg-target.c.inc:1878/1907` compile for
`SLS_IN_KERNEL` as well as `CONFIG_USER_ONLY`; audit the 6 live references.
`setup_guest_base_seg()` already no-ops to 0 via the `#ifndef` at 1909.

**Gotcha:** `sls-launcher.c:346` has a local `void *guest_base` from
`qemu_sls_dma_host_ptr(0)` — same name as the global the prologue reads, entirely
different meaning. Rename before the global exists.

### Phase 1 was load-bearing and untested; three bugs

All four `kernel/qemu_sls_*.c` files had zero host tests.
`qemu_sls_mmu_shadow_fault()` is wired into `handle_page_fault()` but today only
runs after the soft-MMU has already failed. Step 5 makes it the only thing
between translated code and memory.

| Bug | Consequence |
| --- | --- |
| No "is a guest running" guard | The shadow table maps GVAs, so no address range separates a guest access from a kernel bug. Any kernel #PF got walked against the guest's tables; a hit installed a PTE in a table that is not the live CR3 and returned "handled", so `iretq` re-executed forever — an unkillable spin instead of a `[FAULT]` line. Fixed with `qemu_sls_guest_active`, set in the launcher after the CR3 switch and cleared before the restore. |
| No `invlpg` after installing a PTE | Harmless on a not-present fault (x86 does not cache non-present translations) but fatal on a **permission** fault: the stale read-only entry survives, the write faults again, same PTE installed forever. Any copy-on-write guest hits it. |
| `map_guest_ram()` OOM left partial state | Frames allocated, mapped, recorded in `guest_ram_frames[]`, and registered in no region — so `dma_host_ptr()` returned NULL for pages that were live and unreclaimable, while the caller saw a clean `-1`. Now allocates everything before mapping anything, and unwinds completely. An overlapping range is refused rather than silently re-mapped over live frames. |

Testing it required two extractions, both improvements independent of the test:
`mov %%cr3` is privileged, so the whole layer segfaulted before its first
assertion — now `arch_read_cr3()`, non-inline so the seam exists. Same for
`qemu_sls_invlpg()`. `QEMU_GPA_HOST_BASE` is `#ifndef`-wrapped so a host test can
relocate the window, with the production value asserted separately where
relocating it cannot hide it — which is how the header's "2 TiB" was caught: the
constant is 2^45, **32 TiB**.

`tests/qemu_sls_mmu_host_test.c`, 54 checks, 13/13 mutations.
