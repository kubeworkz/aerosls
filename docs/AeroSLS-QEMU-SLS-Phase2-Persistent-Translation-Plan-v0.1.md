# AeroSLS QEMU-SLS Phase 2 — Persistent Translation Cache

**Plan v0.1 · 2026-08-04**

*Companion to `AeroSLS-QEMU-SLS-Cross-ISA-Repositioning-v0.1.md`. That document
fixed the strategy; this one picks the next phase and says how it will be
judged.*

---

## 0. Why this phase, and why now

Measured on node 2, `qemu bench 500`, five consecutive runs:

```
TRANSLATE  ~149,000,000 cycles   85%
EXEC        ~25,800,000 cycles   15%
```

**Compiling the guest code costs five to six times more than running it.**

Phase 1 (shadow page tables) attacks the 15%. Phase 2 attacks the 85%.

That alone would justify the ordering. Two further facts settle it:

**It is the only part of this work a competitor cannot copy.** Shadow paging
needs OS privileges — hard, but any OS vendor could do it, and KQEMU did in
2005. A translation cache that survives reboot needs the *address space itself*
to be persistent. That is what single-level storage is. It is the one place
where AeroSLS's architecture produces a capability rather than an optimisation.

**It is measurable on hardware we already have.** This is the practical
argument and it should not be underweighted. The Phase 1 effect is a fraction
of 15%, on a box with no KVM where an outer emulator contaminates every timing
— an entire day went into building instrumentation honest enough to measure it,
and the conclusion was still "no timing claim without new hardware." The Phase 2
effect is *did we compile 502 instructions, or did we not*. Cold versus warm
boot, an 85% swing, visible straight through the noise. **The more valuable
result is also the easier one to demonstrate.**

---

## 1. What already exists — verified in tree, not assumed

`kernel/qemu_sls_tcache.c` (243 lines) is a real implementation, not a stub:

| Component | Status |
|---|---|
| NVMe layout, LBA 10000–18968, clear of the stream region | **exists** |
| `QemuTBDesc` — guest_pc, code_offset, code_len, gen_expected, guest_page | **exists**, 32 bytes, 4096-entry open-addressing table |
| `qemu_sls_page_gen[65536]` — one generation counter per guest 4 KiB page | **exists** |
| `gen_expected` per TB, compared against the page's current generation | **exists** — this is the invalidation design, and it is the right one |
| Restore path with warm/cold start reporting | **exists** |
| `qemu_sls_tcache_sync()` — write-back | **exists**, called from `qemu_sls_vm.c:51` |
| `qemu_sls_tcache_lookup()` / `update_tb()` | **exists**, called from `qemu_sls_pgo.c` |
| 4 MiB code buffer in `.bss` for a deterministic VA across reboots | **exists** |

This is further along than the boot banner suggests. The storage layer, the
descriptor format and the invalidation scheme are done and considered.

---

## 2. The two gaps

### Gap A — the launcher never consults the cache

```
$ grep -c "tcache\|page_gen\|codebuf" ../qemu/sls/sls-launcher.c
0
```

`sls_launch_guest()` calls `sls_x86_translate_block()` and `tcg_gen_code()`
**unconditionally, every launch, for every block.** The cache is initialised at
boot (`kernel.c:409`), read by the PGO scanner, written back by the VM
snapshot path — and never once consulted by the code that translates.

That is the whole reason TRANSLATE is 85% on the fifth identical run of the
same program in the same boot. Nothing is broken; the hot path simply does not
ask.

**This is the phase** — but it is not the one-line change that description
implies, and Gate 0's investigation showed why.

TCG generates code into `sls_code_buffer` (32 MiB, `sls/sls-runtime.c`, handed
out by `sls_code_alloc()` via the `mmap` stub). The tcache persists
`qemu_sls_codebuf` (4 MiB, `kernel/qemu_sls_tcache.c`). **They are different
buffers**, and nothing copies between them.

So wiring the cache means deciding one of:

- **TCG generates directly into the tcache buffer** — `sls_code_alloc()` returns
  `qemu_sls_codebuf`. Cleanest, but the sizes disagree (32 vs 4 MiB) and the
  buffer must live at a deterministic virtual address across reboots, which is
  why it is `.bss` today.
- **Copy on insert** — after `tcg_gen_code()`, copy the emitted bytes into the
  tcache buffer and record the offset. Simpler and keeps the two independent,
  at the cost of a memcpy per block and double the memory.

That choice belongs to Gate 2 and should be made explicitly rather than
discovered.

### Gap B — restore validates a constant, not an identity

```c
#define QEMU_TCACHE_MAGIC  0xCAFE000000000020ULL
```

That is the only check on the restore path. It proves the region was written by
*some* build of this code. It does not prove it was written by *this* build.

The cache holds **host machine code**. Restoring one produced by a different
compiler, a different TCG revision, or a different set of `-m` flags hands the
CPU instructions built against different assumptions. There is no fault to
catch it. It executes.

Today's session found five bugs; the single most expensive property of the
worst of them was that a wrong state was indistinguishable from a right one.
This has exactly that shape, and it must be closed before the cache is ever
read on the execution path.

---

## 3. Gates

Nothing starts before its predecessor's gate is met.

### Gate 0 — does the existing machinery actually round-trip?

**Partly answered statically, 2026-08-04, and the answer was not what the gate
expected.**

`qemu_sls_tcache_sync()` was called from `qemu_sls_snapshot_save()`, and
`qemu_sls_snapshot_save()` **was called from nowhere.** Both were dead code.
`checkpoint_trigger()` — the system checkpoint the shell's `checkpoint` command
runs — belongs to a different subsystem and never touched the QEMU-SLS cache.

So every boot has reported `no snapshot — cold start` for a reason nobody would
have guessed from the message: **a snapshot had never once been written.** The
restore path had nothing to restore and was indistinguishable from a restore
path that did not work.

`checkpoint_trigger()` now calls `qemu_sls_tcache_sync()` directly. Deliberately
outside the dirty-region mask: those regions rely on explicit
`checkpoint_mark_dirty()` calls and the tcache has none, so an unconditional
sync cannot silently skip a checkpoint because nobody remembered to mark it.

**Corrected gate.** The original criterion — `codebuf_used=N` with N > 0 — was
wrong, and for a second reason worth writing down: *nothing populates the cache
either.* `sls_launch_guest()` contains no reference to `tcache`, `page_gen` or
`codebuf`, and TCG generates into `sls_code_buffer` (32 MiB, `sls-runtime.c`),
which is a **different buffer** from `qemu_sls_codebuf` (4 MiB, the one the
tcache persists). Connecting those two is Gate 2's real work, and it is a larger
job than "add a lookup".

So Gate 0 now tests only what it can: **does the storage layer round-trip?**

```
checkpoint              # now reaches qemu_sls_tcache_sync()
<restart node>
```

*Gate:* the boot banner reads `warm start` rather than `no snapshot`.
`codebuf_used=0` is the **expected** value until Gate 2 — it means the header
was written, found and validated, which is the whole of what this gate can
prove today.

If it still says `no snapshot`, the bug is in the storage layer and is now
genuinely reachable for the first time.

### Gate 1 — identity stamping (Gap B), before any lookup is wired in

Extend the header beyond the magic with a build identity: TCG target, pointer
width, `__DATE__`/`__TIME__` or a git hash, and a hash of the compiler flags
that affect code generation. Mismatch ⇒ **discard silently and cold-start.**

*Gate:* a host test that writes a header, mutates one identity field, and
asserts the restore path refuses it. Mutation-tested — a check for this that
has never been observed rejecting anything has not been tested, only executed.

### Gate 2 — wire the lookup into `sls_launch_guest`

Before `tcg_tb_alloc`/`tcg_func_start`, ask `qemu_sls_tcache_lookup(cpu.eip)`.
On a hit whose `gen_expected` still matches `qemu_sls_page_gen[gpa >> 12]`,
execute the cached code and skip translation entirely. On a miss, translate as
today and insert.

*Gate:* `qemu bench 500` run twice **in one boot** reports
`TRANSLATE ≈ 0, 0 block(s) compiled` on the second run, with **`CODE` and the
guest result bit-identical to the first.** Both halves matter — a cache that
returns fast and wrong is the failure this phase is most exposed to.

### Gate 3 — the headline measurement

```
<cold boot>  qemu bench 500     -> record TRANSLATE, EXEC, CODE
checkpoint
<reboot>     qemu bench 500     -> record the same
```

*Gate:* warm-boot TRANSLATE is < 10% of cold-boot TRANSLATE, over five runs
each, with identical `CODE` bytes and identical instruction counts.

That is the number the phase exists to produce, and unlike anything in Phase 1
it needs no KVM and no new hardware.

### Gate 4 — invalidation actually invalidates

The generation-counter design is sound; whether it is *wired* is a separate
question, and this phase is precisely about connecting designed machinery to
the live path.

*Gate:* a test that translates a block, writes to the guest page containing it,
and asserts the next execution re-translates rather than running the stale
code. Then the same across a reboot.

Self-modifying guest code and DMA into code pages are the real cases. A guest
that rewrites its own instructions and gets the old ones back is silent
corruption, and this gate is the only thing standing between the design and it.

---

## 3b. RESULT — the cache works, same boot (2026-08-04)

`qemu bench 500` twice in one boot, node 2:

| | run 1 (cold) | run 2 (warm) |
|---|---|---|
| **blocks compiled** | **8** | **1** |
| **instructions compiled** | **502** | **64** |
| instructions **executed** | 502 | **502** |
| TCACHE | 0 hit, 8 miss | **7 hit, 1 miss** |
| TRANSLATE cycles | 37,913,139 | 3,918,114 |
| total cycles | 44,280,246 | 5,283,964 |

**The defensible claim is the work ratio, not the time ratio.** Compilation
fell from 502 instructions to 64 — an **87.2% reduction** — while the guest
executed the identical 502 instructions. Blocks compiled fell 8 → 1. Those are
counts: deterministic, immune to the outer emulator, and reproducible.

The 8.4× drop in total cycles is consistent with that but is **not** a
publishable figure: this host has no KVM, so every cycle count is contaminated
by the outer emulator (see §5c of the repositioning plan). The count of blocks
compiled is not.

The single remaining miss is the block at **guest_pc 0**, which
qemu_sls_tcache_insert() cannot represent because it uses 0 as its empty-slot
marker. The benchmark guest loads at GPA 0, so this recurs every run. A real
guest rarely begins at address 0, but the limitation is real and tracked.

### What it took, and what was wrong on the way

**Copy-on-insert cannot work.** The first implementation memcpy'd generated
code into the persistent buffer. TCG output is position-dependent -- relative
calls and jumps to helpers, the epilogue and exit_tb carry displacements
computed against the address the block was generated at -- so a relocated block
has real bytes and broken targets. Observed exactly that way: 7 blocks stored
cleanly, and executing them hung with no fault, jumps landing in mapped memory
and wandering. Replaced with reserve/commit: the block is GENERATED at its
permanent address and never moves.

**And TCG had no output address at all.** `tcg_gen_code()` begins with
`s->code_buf = tb->tc.ptr` (tcg.c:6665). In QEMU that field is filled by
`tb_gen_code()`; this launcher never calls it, so it was NULL and every
translated block was emitted at **host address 0** and executed from there --
for the entire life of the launcher. It worked only because AeroSLS
identity-maps low memory writable and executable, and because no two blocks
ever had to coexist. Phase 2 was the first thing that required two, which is
the only reason it surfaced.

---

## 3c. RESULT — the cache survives a reboot (2026-08-04)

The claim the phase exists for, validated on node 2:

```
qemu bench 500        ->  8 blocks compiled, 502 insn(s) compiled, 0 hits
checkpoint            ->  [QEMU-SLS TCACHE] synced: 7 TBs, 7203 code bytes
< node restarted >
qemu bench 500        ->  ARENA 2%, ALLOC 6 calls since boot   (fresh boot)
                          TCACHE 7 hit(s), 1 miss(es)
                          TRANSLATE 1 block(s), 64 insn(s) compiled
                          502 insn(s) executed
```

**Seven of eight translated blocks were read back from NVMe, at the address
they were generated at, and executed correctly on the first run after a
restart.** The guest performed the identical 502 instructions.

### The defensible claim

**Blocks compiled across a restart: 8 → 1. Instructions compiled: 502 → 64.**

Counts, not times: deterministic, immune to the outer emulator, reproducible.
That is an 87.5% reduction in compilation work on a cold boot -- work that every
other emulator repeats in full, every start, because its translation cache dies
with the process.

Cycle figures fell too (44.3M → 18.5M total) but are **not publishable**: this
host has no KVM, so every timing is contaminated (repositioning plan §5c). Note
also that the post-reboot TRANSLATE (13.7M) is higher than the same-boot warm
run (3.9M) because a first launch carries one-time TCG initialisation. The block
count does not move, which is exactly why the block count is the number quoted.

### What is still true and unfinished

- **guest_pc 0 can never be cached.** `qemu_sls_tcache_insert()` uses 0 as its
  empty-slot marker and the benchmark guest loads at GPA 0. That is the one
  remaining miss, every run. A real guest rarely starts at address 0, but the
  limitation is real.
- **Gate 4 (invalidation) is not done.** Nothing yet proves that writing to a
  guest code page forces re-translation. The generation counters exist and are
  persisted; whether they are *wired* is a separate question, and a stale hit is
  a far worse failure than a missed one.
- **The identity stamp has not been tested against a genuinely different build
  that produces different code.** It has only refused an unstamped cache and a
  differing hash.

### 3d. COMPLETE — zero translation on a warm run (2026-08-04)

After biasing `guest_pc` by one in the descriptor, the last uncacheable block
was gone and a warm run compiled **nothing at all**:

| | cold | warm |
|---|---|---|
| blocks compiled | 8 | **0** |
| instructions compiled | 502 | **0** |
| TRANSLATE cycles | 24,495,001 | **0** |
| TCACHE | 0 hit, 8 miss | **8 hit, 0 miss** |
| instructions executed | 502 | **502** |
| arena consumed | 11,829,392 | **0** |
| total cycles | 28,607,572 | 719,712 |

**100% of translation eliminated**, with identical guest results.

The arena figure is a second-order confirmation and worth noting: the ~1.48 MB
per block that `sls_malloc` leaks comes entirely from TCG's translation
allocations, so a run that compiles nothing allocates nothing. The leak that
limited a boot to ~5 benchmark runs applies only to cold runs.

### 3e. Gate 4 — invalidation, and the two doors into guest code

`qemu_sls_tcache_flush_page()` and `_flush_all()` existed and were **called from
nowhere**. Worse, with softmmu off there was nothing to call them *from*: a
guest store compiles to a bare host MOV -- no helper, no TLB lookup, no hook --
so a guest rewriting a page it had already executed produced no signal at all,
and the cache went on serving translations of bytes that no longer existed.

Two distinct paths write guest code, and they need opposite treatments.

**Guest stores — go through the GUEST window.** A page that translated code was
generated from is now write-protected there after a successful commit. The next
guest store faults, which is the hook that did not otherwise exist:
`shadow_fault()` bumps the page generation (making every TB from it a lookup
miss), restores write permission, and reports the fault resolved so the store
retries and succeeds. Present-but-not-writable, so execute and read are
unaffected: only a store can invalidate a translation.

**Emulator writes — go through the EMULATOR window, which is deliberately
unprotected**, because that is how the emulator reads guest page tables and
loads images. So they overwrite guest code with no fault and no signal. Found
while testing the above: `sls_launch_guest()` memcpy's the image on every
launch, straight over the code page, invisibly.

### The correction that mattered most

Flushing on that memcpy was correct and ruinous. Re-launching the same image
rewrites identical bytes and invalidates every page it touches -- and all eight
blocks of this benchmark live in the single page the 3006-byte image occupies.
**A warm run went from 8 hits to 8 misses the moment the flush was added.**

The image copy now compares per page and flushes only where the bytes actually
differ. A 4 KiB scan against ~25 million cycles of re-translation; not really an
optimisation, but the difference between a cache that survives its own loader
and one that cannot.

```
warm run, after all three rounds:
   TCACHE 8 hit(s), 0 miss(es)      TRANSLATE 0 cycles in 0 block(s)
   502 insn(s) executed             ARENA 0 consumed by this launch
```

83 host checks, 5 mutations caught.

**Still open:** invalidation for guests with paging ON (the faulting address is
then a GVA and needs a page-table walk before its physical page can be
identified -- currently refused loudly rather than allowed), and an audit of
every other caller of `qemu_sls_dma_host_ptr()`. Making that accessor invalidate
by itself would make the safe path the default rather than something each caller
has to remember.

### The marketing claim this supports

> Every other emulator recompiles from scratch on every start. AeroSLS does not:
> in a single-level store, compiled code is a persistent object like any other.
> On this workload a warm node compiled **nothing** — 0 blocks instead of 8 —
> and executed identical results.

Capability, with a measurement, in counts. No speed multiplier is claimed, and
none should be until the work runs on hardware with KVM or on a non-x86 host.

---

## 4. Risks, stated before the work

**A stale cache is worse than no cache.** Every other risk here is a variation
of this one. The failure mode is not a crash — it is executing host code built
against assumptions that no longer hold, with no signal. Gates 1 and 4 exist
solely for this, and neither should be waived for a demo.

**"Compiled code survives reboot" invites a security question.** The cache is
executable host code on persistent storage. Anything that can write LBA
10776–18968 can choose what the kernel executes after the next boot. Today the
NVMe namespace is trusted, but this materially raises what a storage-level
compromise is worth, and the threat model should say so explicitly rather than
inherit the stream region's assumptions by adjacency.

**The 85% may not all be reclaimable.** TB lookup, generation checks and cache
management have their own cost. A warm run will not be free, only much cheaper.
Gate 3's threshold is <10% of cold, not zero, for that reason.

**The measurement could be too good to be true.** If warm TRANSLATE reads 0 and
`CODE` also reads 0, that is not a triumph, it is a cache returning blocks that
were never validated. Gate 2 requires bit-identical guest results for exactly
this reason. The most dangerous outcome of this phase is a spectacular number
produced by skipping the work rather than reusing it.

---

## 5. What can be claimed, and when

**After Gate 3, defensibly:**

> Every other emulator recompiles from scratch on every start. AeroSLS does not,
> because in a single-level store, compiled code is a persistent object like any
> other. On this workload, restarting eliminated N% of translation cost.

That is a capability claim with a measurement behind it, and it does not depend
on beating anyone's benchmark.

**Not claimable, on this hardware, in this phase:** any end-to-end speed
multiplier. §9 of the repositioning plan is unchanged — timing claims need KVM
or real non-x86 hardware. The Phase 2 result is about *work not done*, which is
countable, not about time, which here is not.

---

## 6. Sequencing against the rest

**Step 5** (`tcg_use_softmmu` false) should still be finished first. It is one
flag plus a six-reference audit, the baseline is captured, and its real value
was never the number — it is validating the shadow-PT implementation before
anyone ports it to a second architecture. Days.

Then Phase 2, Gates 0 through 4.

Cross-ISA (§8 Steps 4–6 of the repositioning plan) stays after both, and still
gates on acquiring non-x86 hardware.

---

## Appendix — measurements this plan rests on

All from node 2, 2026-08-04, `qemu bench 500`, softmmu=ON, five consecutive
runs in one boot:

| | value | variation |
|---|---|---|
| CODE emitted | 43,293 bytes (86/load) | **0** across 9 samples, 5 boots, 3 builds |
| TRANSLATE | ~149M cycles, 8 blocks | ~85% of total |
| EXEC | 51,182 cycles/load | σ ≈ 982, CV 1.9% |
| arena consumed | 10,353,840 bytes/launch | ~1.48 MB leaked per block |

The CODE column is what makes Gate 2's "bit-identical" check meaningful: it has
never varied, so any change in it after wiring the cache is a real difference
and not noise.
