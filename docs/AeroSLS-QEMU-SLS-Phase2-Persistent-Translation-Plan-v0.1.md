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

**This is the phase.** Not new machinery — a lookup on the translate path, and
an insert after it.

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

Before writing any new code, find out whether the cache that exists today can
be written and read back at all. Every boot log so far says `no snapshot —
cold start`; a snapshot has never been observed being produced.

```
qemu bench 8            # populate
checkpoint              # qemu_sls_vm.c:51 calls qemu_sls_tcache_sync()
<restart node>
```

*Gate:* the boot banner reads `warm start — codebuf_used=N` with N > 0.

If it does not, that is the phase's first bug and it is in the storage layer,
not the hot path. Do not proceed on the assumption that persistence works.

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
