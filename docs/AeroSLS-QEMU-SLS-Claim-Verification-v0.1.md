# AeroSLS QEMU-SLS — Claim Verification v0.1

*Every quantitative claim in the QEMU-SLS documents, checked against the tree.
Each row carries the command that produced the measurement, so this document
can be re-run rather than believed.*

**Measured at:** `88003e7`, against `my_sls_kernel.bin` built from those sources
(staleness guard passing).
**Scope:** `docs/AeroSLS-QEMU-SLS-*.md`, `docs/AeroSLS-QEMU-TCG-Optimizations.md`,
`Makefile`, and the `../qemu` tree the kernel links against.

---

## 0. Why this document exists

The QEMU-SLS documents are unusually careful about labelling claims as
**verified** or **unverified** — §2 of the Cross-ISA Repositioning plan is a
model of it. That discipline covers *whether a claim was ever measured*. It does
not cover *whether the measurement is still true*, and those are different
properties. A number written down as verified stays labelled verified while the
thing it measured moves underneath it.

Five stale figures were found in one afternoon. Four were the same shell-frame
number copied into four files. The fifth was the kernel image size, and it is
the one that matters for positioning, because it is quoted in a section about
whether this fits on small hardware.

**Rule this document proposes:** any figure that appears in user-facing or
marketing material carries the command that reproduces it. A number without a
command beside it is a number nobody can re-check.

---

## 1. Verified — safe to quote

| Claim | Source | Measured | Command |
|---|---|---|---|
| `TCG_MAX_INSNS` is 512 | Cross-ISA §2 | **512**, `include/tcg/tcg.h:122` — line number still exact | `grep -n TCG_MAX_INSNS ../qemu/include/tcg/tcg.h` |
| `gen_insn_end_off[TCG_MAX_INSNS]` fixed array | Cross-ISA §2 | **Confirmed**, `tcg.h:436` — line number still exact | `sed -n 436p ../qemu/include/tcg/tcg.h` |
| QEMU has TCG backends for ARM64 and RISC-V | Cross-ISA §2 | **Both present**: `tcg/aarch64/`, `tcg/riscv64/` | `ls -d ../qemu/tcg/aarch64 ../qemu/tcg/riscv64` |
| AeroSLS has a working RISC-V port | Cross-ISA §2 | **Present**: `arch/riscv/` — boot, traps, PLIC, SBI, page-table walker | `ls arch/riscv/` |
| AeroSLS on ARM64 does not exist | Cross-ISA §4 | **Confirmed absent.** `arch/` contains `riscv` and `x86` only; zero `aarch64`/`arm64` matches in any source file | `ls arch/; grep -rli 'aarch64\|arm64' --include=*.c --include=*.h .` |
| Kernel links 15 objects from `../qemu` | `boot.asm`, deploy.sh | **15** (`tci.x86.o` is deliberately excluded, in a comment) | `sed -n '145,160p' Makefile` |
| `QEMU_GUEST_RAM_PAGES` = 65,536 = 256 MiB | Cross-ISA §6 | **65536U**, `kernel/qemu_sls_mmu.h:74` → 256 MiB exactly | `grep -n QEMU_GUEST_RAM_PAGES kernel/qemu_sls_mmu.h` |
| GPA window at 32 TiB | Guest-Address-Space | **`0x200000000000`** = 32 TiB, `qemu_sls_mmu.h:70` | `grep -n QEMU_GPA_HOST_BASE kernel/qemu_sls_mmu.h` |
| `sls_code_buffer` is 32 MiB | Phase2 §1 | **32 MiB** declared, **32.00 MiB** in `.bss` | `grep -n SLS_CODE_BUFFER_SIZE ../qemu/sls/sls-runtime.c` |
| `qemu_sls_codebuf` is 4 MiB | Phase2 §1 | **4 MiB** declared, **4.00 MiB** (`codebuf_storage`) in `.bss` | `grep -n QEMU_TCACHE_CODEBUF_SIZE kernel/qemu_sls_tcache.h` |
| The two code buffers are different buffers | Phase2 §1 | **Confirmed** — 32 MiB and 4 MiB, separate symbols, both present | `nm --size-sort -S my_sls_kernel.bin` |
| `QemuTBDesc` 32 bytes, `tb_table[4096]` | Phase2 §1 | **Consistent**: 4096 × 32 = 128 KiB, as the header states | `grep -n 'tb_table\|32 bytes' kernel/qemu_sls_tcache.h` |
| `qemu_sls_page_gen[65536]` | Phase2 §1 | **65536U** via `QEMU_TCACHE_GEN_PAGES` | `grep -n QEMU_TCACHE_GEN_PAGES kernel/qemu_sls_tcache.h` |
| `max_insns` bug is clamped | Cross-ISA App. A | **Fixed**, two places: refusal at `sls-launcher.c:1185`, hard clamp at `sls-x86-frontend.c:96` | `grep -n TCG_MAX_INSNS ../qemu/sls/*.c` |
| x86 guest frontend is minimal | Cross-ISA §4 | **18 case labels**; `0x8B`/`0x89` + ModRM as described | `grep -cE 'case 0x[0-9A-Fa-f]+:' ../qemu/sls/sls-x86-frontend.c` |
| 89 host test files | session record | **89** | `ls tests/*_host_test.c \| wc -l` |
| Stack is 1 MiB | `boot.asm` | **1,048,576 bytes**, read from the linked binary | `tests/stack_frame_budget_check.sh` |

---

## 2. Stale — corrected in this commit

> **Status: all four fixed.** They are recorded here in full, with the original
> wording, because the corrections are worth less than the record of what went
> wrong and how it was found. Deleting the trail would leave a document that
> asserts the current numbers with no evidence they were ever checked — which is
> the condition this document exists to end.

### 2.1 Kernel image size — the one that affects positioning

| | |
|---|---|
| **Cross-ISA §9 says** | "The kernel image is 173 MiB and growing." |
| **Cross-ISA §6 says** | "a kernel image that has grown to **173 MiB**" |
| **Guest-Address-Space §… says** | "The kernel image occupies 1–221 MiB." |
| **Measured** | **221.6 MiB** loaded (`memsz` = `0xdd8e810`); `_kernel_image_end` = 222.6 MiB |

```bash
size my_sls_kernel.bin                       # text 2,941,065  data 31,800  bss 229,345,296
readelf -lW my_sls_kernel.bin | grep LOAD    # memsz 0xdd8e810 = 232,318,992 = 221.6 MiB
nm my_sls_kernel.bin | grep _kernel_image_end
```

**Verdict:** the Guest-Address-Space figure (**221 MiB**) is correct and current.
The Cross-ISA figure (**173 MiB**) was stale and understated by ~48 MiB. Two
documents in the same directory disagreed by 28%, and the wrong one was the one
in the strategy document. **Corrected** at Cross-ISA §6 and §9.

**Note on file size.** `ls -l my_sls_kernel.bin` shows **2.0 MiB**. That is the
ELF on disk and it is the wrong measurement for every purpose in these
documents — `.bss` is `nobits` and occupies no file space. The number that
matters for "does this fit on the target" is `memsz`, which is 100× larger.
Anyone sizing hardware from the file listing will be wrong by two orders of
magnitude.

### 2.2 The attribution of that size is also wrong

**Cross-ISA §9 says:** "The kernel image is 173 MiB and growing. **Linking TCG
did that.**"

Measured `.bss` composition:

| Object | Size | Owner |
|---|---|---|
| `sls_heap` | 64.00 MiB | core AeroSLS |
| `http_conns` | 32.01 MiB | core AeroSLS |
| `sls_code_buffer` | 32.00 MiB | **TCG** |
| `g_add_column_scratch` | 16.03 MiB | core (SQL) |
| `tcp_conns` | 16.02 MiB | core AeroSLS |
| `btree_nodes` | 13.81 MiB | core (RDBMS) |
| `cursor_table` | 8.01 MiB | core (RDBMS) |
| `vec_index_nodes` | 4.25 MiB | core (vecstore) |
| `codebuf_storage` | 4.00 MiB | **TCG** |
| *(top-20 subtotal)* | *206.79 MiB* | of 218.7 MiB `.bss` |

```bash
nm --size-sort -S --radix=d my_sls_kernel.bin | awk '$3 ~ /^[bB]$/ {print $2, $4}' | sort -rn | head -20
```

**TCG accounts for 36 MiB of 218.7 MiB — about 16%.** `sls_heap` alone is 64 MiB,
nearly twice TCG's entire contribution. The footprint is dominated by
fixed-size static arrays in core AeroSLS subsystems, not by the emulator.

This matters for the edge/ARM discussion specifically, and it cuts **in favour**
of that direction: the footprint is mostly compile-time constants that can be
tuned per deployment, not an inherent cost of linking a JIT. But the claim as
written points at the wrong subsystem, so anyone acting on it would optimise the
wrong thing.

### 2.3 `Makefile:24` — shell.c and http.c frame sizes

| | |
|---|---|
| **Says** | "shell.c and http.c are over budget today (276 KB and 266 KB)" |
| **Measured** | **10,224** and **5,456** bytes |

```bash
gcc -ffreestanding -O2 -mcmodel=small -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
    -fno-pie -fno-pic -fno-tree-vectorize -I. -Ikernel -Iarch/x86 -Inet \
    -Wframe-larger-than=2048 -c user/shell.c -o /dev/null
```

Both are now well under the 16,384-byte `-Wframe-larger-than` in `X86_CFLAGS`,
so the comment's justification — that making it an error "would block work" — no
longer applies. The two frames it names are no longer over budget at all.

### 2.4 `tcg-internal.h` citation has drifted

**Cross-ISA §2 cites:** `tcg/tcg-internal.h:37` — `#ifdef CONFIG_USER_ONLY /
#define tcg_use_softmmu false`

**Actually:** line 37 is now the start of an SLS comment block. The define is at
**line 54**, and the condition is:

```c
#if defined(CONFIG_USER_ONLY) || defined(SLS_IN_KERNEL)
#define tcg_use_softmmu false
```

The claim's substance holds. But the row is filed under "things upstream QEMU
already does that we rely on," and the tree now takes that path via
**`SLS_IN_KERNEL`** — this project's own switch, added for the §5c A/B. That is a
stronger fact than the one recorded, and it should be recorded as ours.

---

## 3. Runtime-only — real, but not reproducible from a checkout

These come from `qemu bench` on a live node. They are **not** in doubt; they
simply cannot be re-derived by reading the tree, so they need a node and a
recorded run.

| Claim | Source | Status |
|---|---|---|
| 86 bytes of host code per guest load (softmmu ON) | Cross-ISA §5b | Runtime. 9 samples, zero variance reported. |
| 16 bytes per guest load (softmmu OFF) | Cross-ISA §5c | **RE-VERIFIED 2026-08-05** on `88003e7`: `CODE 8003 bytes → 16 bytes/load`, exact match. |
| CODE 43,293 → 8,003 bytes, **5.41×** | Cross-ISA §5c | **Half re-verified.** The OFF term (8,003) reproduces exactly. The ON term was not re-run, so the *ratio* is not re-derived — that needs a softmmu=ON build. |
| EXEC ratio 7.56×, excess **~1.4×** real work | Cross-ISA §5c | Runtime, ±7% from CV 15.3%, n=5. 2026-08-05 cold EXEC was 5,939 cyc/load vs §5c's 6,774 — a −12.3% move, inside the reported CV. Consistent. |
| Node 2 silent halt in `map_guest_ram` | Cross-ISA §6 | **No longer reproduces (2026-08-05), not diagnosed.** `guest RAM mapped: 256 MiB` now prints. §8's gate required a root cause and was not met; see §6. |
| Arena leak, 10,353,840 bytes/launch | Phase2 App. | **Superseded.** 1,507,392 cold, **0** warm, 7 free/7 reuse. Leak fixed; see Phase2 appendix. |
| Translation cache survives reboot | Phase2 | **YES — verified end to end, 2026-08-05.** First launch after a reboot: no `flush_page`, `TCACHE 8 hit / 0 miss`, `TRANSLATE` 0 cycles / 0 blocks, `CODE` 0 bytes, arena 1,056 B against 1,508,448 B cold. Fixed in `551d9db` + `8fc55f2`; evidence in §3c. |

**Before any of these is published**, re-run on current `HEAD` and record the
build ID. The §5c A/B was taken 2026-08-04; the tree has moved since. Zero
variance across samples is not evidence of stability across *builds* — §5b makes
exactly this point about cross-boot versus within-boot noise.

---

## 3b. The cold/warm method — how to get an honest cycle number without KVM

**Established 2026-08-05, node 2. This is a measurement technique, not a
result, and it is the most reusable thing in this document.**

§5b of the Cross-ISA plan disqualified every cycle figure taken on this host:

> Our JIT emits fresh host code at fresh addresses, so the outer emulator must
> translate that code before running it, every time, because it is always cold.
> EXEC is timing **the outer emulator compiling our JIT's output**, not our
> output executing.

That reasoning is correct, and it has an exploitable hole: it only holds while
the code is *fresh*. Once the translation cache is warm, the same bytes sit at
the same address, so the **outer** emulator's TB cache hits too and no outer
translation occurs. Running the identical bench twice therefore separates the
two costs by subtraction:

| | cold (`TCACHE 0 hit, 8 miss`) | warm (`TCACHE 8 hit, 0 miss`) |
|---|---|---|
| `TRANSLATE` | 24,318,725 cyc, 8 blocks | **0 cyc, 0 blocks** |
| `CODE` | 8,003 bytes | 0 (nothing compiled) |
| `EXEC` | 2,969,646 cyc → **5,939 cyc/load** | 125,562 cyc → **251 cyc/load** |

- **cold EXEC** = guest code executing **+** outer emulator translating it
- **warm EXEC** = guest code executing, that cost already paid
- the difference is the outer-translation artifact itself

### The warm figure does NOT replicate — corrected 2026-08-05

An earlier revision of this section called 251 cyc/load "the first EXEC figure
on this hardware not dominated by the confound" and quoted it as a result. **A
second boot gave 455.** The correction matters more than the number:

| | boot A | boot B | move |
|---|---|---|---|
| cold EXEC | 5,939 cyc/load | 5,434 | **−8.5%** — inside §5b's 5–13% cross-boot band |
| **warm EXEC** | **251 cyc/load** | **455** | **+81%** — far outside it |
| cold/warm ratio | 23.7× | 11.9× | — |
| `CODE` | 8,003 B | 8,003 B | **0** |
| arena, cold launch | 1,507,392 B | 1,507,392 B | **0** |

The warm figure is the **noisiest** measurement in the set, not the cleanest.
It is a much smaller sample — 125K–228K cycles against 2.7–3.0M cold — so fixed
overheads and interrupt noise are a far larger fraction of it. `n=1` was quoted
as though it were settled, which is the error §5b spent two pages establishing
should not be made.

**The method stands; the value does not.** Warming the cache does remove outer
translation, and that is real. But any warm figure needs **five runs in one
boot**, with a CV, exactly as §5b did for the ON baseline, before it means
anything. Until then there is no publishable warm number.

**What replicated perfectly across both boots:** `CODE` at 8,003 bytes, arena at
1,507,392 cold and 0 warm, `TCACHE` 0/8 then 8/0, 502 instructions. Zero
variance on every one. §5b's original conclusion — build claims on emitted code
size, never on cycles — survived its own extension intact.

### The trap in the same data

**The cold-to-warm ratio must never be quoted as a speedup.** It is the
difference between paying outer-emulator translation and not paying it. On real
hardware there is no outer emulator and the ratio largely disappears. It
measures the test rig, not the system under test — the identical error §5b
caught, in a new form. That it came out 23.7× on one boot and 11.9× on the next
is the tell: a real property of the system would not move by half.

### Procedure

```bash
tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # cold: TCACHE 0/8
tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # warm: TCACHE 8/0
# repeat the warm run 5x and report a CV -- one sample is not a measurement
```

Check `TCACHE` on each run to confirm which regime you are in. A "warm" run
showing misses, or a "cold" run showing hits, invalidates the pair.

---

## 3c. Persistence across reboot — NOT YET TESTED

A reboot trial on 2026-08-05 produced `TCACHE 0 hit(s), 8 miss(es)` on the first
bench after boot, with 8 blocks recompiled and 8,003 bytes re-emitted. **This is
not a negative result, and must not be recorded as one.**

`qemu_sls_tcache_sync()` is the only writer of the on-NVMe cache. It has **two**
call sites, and only one of them is reachable:

| Call site | Reachable? |
|---|---|
| `kernel/checkpoint_mgr.c:182` | **Yes** — the `checkpoint` shell command |
| `kernel/qemu_sls_vm.c:51`, inside `qemu_sls_snapshot_save()` | **No** — see below |

The bench path calls neither; `sls-launcher.c` does not reference either symbol.
The trial ran `qemu bench` and then rebooted, so nothing was ever written and the
restore path had nothing to restore.

### Separate finding: `qemu_sls_snapshot_save()` is still unwired

`checkpoint_mgr.c:166` names **two** functions as having been called from
nowhere. Wiring `qemu_sls_tcache_sync()` into the checkpoint fixed one of them.
`qemu_sls_snapshot_save()` was not wired and remains dead:

```
grep -rn 'qemu_sls_snapshot_save\s*(' --include=*.c kernel/ user/ net/
  kernel/qemu_sls_vm.c:39        <- the definition
  kernel/checkpoint_mgr.c:166    <- a comment saying it is called from nowhere
```

No caller. So the guest CPU state it persists — `rip`, registers, sequence — is
never written by anything, and `qemu_sls_snapshot_restore()` has nothing to
restore for the same reason the tcache did not. The comment at `:166` reads as
though both halves were fixed. Only one was.

Whether that matters depends on intent: the tcache is keyed by `guest_pc`, so
restoring translated code without CPU state may be exactly right for a bench
that always enters at GPA 0. But it is currently undecided rather than decided,
and the dead function makes it look settled.

`checkpoint_mgr.c:166` describes this precise trap, from when the sync call was
genuinely unwired:

> `qemu_sls_tcache_sync()` and `qemu_sls_snapshot_save()` existed, were correct
> as far as anyone could tell, and were called from nowhere — so every boot has
> reported "no snapshot — cold start" for the simple reason that a snapshot had
> never once been produced. The restore path had nothing to restore, **and
> looked exactly like a restore path that did not work.**

### The boot log already contains the answer

`qemu_sls_tcache_init()` runs at `kernel/kernel.c:409` and prints exactly one of:

| Line | Meaning |
|---|---|
| `NVMe unavailable — cold start` | No device; nothing to do with the cache logic |
| `no snapshot — cold start` | Nothing was ever written — **expected for the 08-05 trial** |
| `snapshot is from a DIFFERENT BUILD … discarded, cold start` | Build ID moved; correct refusal |
| `warm start — codebuf_used=N, codebuf=0x…` | Restored |

It goes to the node console at boot, not to the `aeroslsctl shell` capture,
which is why the trial did not include it. **Read that line before drawing any
conclusion about persistence.**

### 2026-08-05, second trial — correctly sequenced, restore still did not fire

The sequence below was run. The write **succeeded** and the restore **did not**:

```
qemu bench 500   ->  TCACHE 0 hit, 8 miss   (cold, expected)
checkpoint       ->  [QEMU-SLS TCACHE] synced: 8 TBs, 8051 code bytes   <- written
<node restart>
qemu bench 500   ->  TCACHE 0 hit, 8 miss   <- NOT restored
```

The third run is a first-launch-after-boot, not a same-boot repeat: it re-prints
the `[QEMU-SLS MMU]` mapping and `TCG ready` lines, and `ALLOC` reads *13 call(s)
since boot* with *8 block(s) this launch* — identical to the first run. A second
launch in the same boot would have carried a higher cumulative `ALLOC`.

**So this is a real open defect, not a sequencing mistake.** What has been ruled
out, statically, from the tree:

| Hypothesis | Status |
|---|---|
| `tcache_init` runs before NVMe is up | **Eliminated.** `nvme_io_init()` at `kernel.c:387` precedes `qemu_sls_tcache_init()` at `:409` |
| `sync` was never called | **Eliminated.** It printed `synced: 8 TBs, 8051 code bytes` |
| Write and read use different LBAs or layouts | **Eliminated.** Both use `QEMU_TCACHE_HDR_LBA`; header field offsets match at `:345-354` and `:73-106` |
| The node's disk is recreated on restart | **Eliminated.** `run-cluster.sh:465` creates the image only when absent |
| Another subsystem overwrites the tcache LBAs | **Eliminated.** tcache spans 10000–18968; persist ends ~7664, `STREAM_DIR` 8192, stream data 65536+, VM state 20000. All disjoint. |

### The boot log answered it — the restore WORKED

```
[QEMU-SLS TCACHE] warm start — codebuf_used=8051, codebuf=0x0000000007944000
```

`codebuf_used=8051` matches `synced: 8 TBs, 8051 code bytes` exactly. The header
was found, the magic matched, the identity matched, the code buffer was read
back. **The storage layer is not the defect.** Every hypothesis in the table
that used to be here was wrong, including the one marked "most likely" — the
build ID matched fine.

The misses therefore happen *after* a successful restore, in
`qemu_sls_tcache_lookup()`, which has two silent miss paths: the descriptor is
absent, or `qemu_sls_page_gen[d->guest_page] != d->gen_expected`. (The third,
all-zero code bytes, prints `REFUSED` and did not appear.)

### Root cause: the loader invalidates the cache it just restored

`../qemu/sls/sls-launcher.c:542-550`:

```c
for (uint32_t off = 0; off < len; off += 4096) {
    int differs = 0;
    for (...) if (dst[off + i] != src[off + i]) { differs = 1; break; }
    if (!differs) continue;              /* identical: no copy, no flush */
    for (...) dst[off + i] = src[off + i];
    qemu_sls_tcache_flush_page(off);     /* bumps qemu_sls_page_gen[page] */
}
```

**The translation cache is persisted. The guest RAM it was compiled from is
not.** Nothing in any checkpoint or persist path covers guest RAM, and
`qemu_sls_mmu_map_guest_ram()` allocates fresh frames on every boot. So:

| | guest RAM at launch | `differs` | flush | result |
|---|---|---|---|---|
| **Post-reboot, 1st launch** | fresh frames | **1** | **yes, page 0** | gen bumped → all 8 TBs stale → **0 hit / 8 miss** |
| **Same boot, 2nd launch** | still holds the image | 0 | no | gen intact → **8 hit / 0 miss** |

The image is 3,006 bytes — one page — and all 8 TBs are compiled from GPA
`0x0..0xBBE`, so a single `flush_page(0)` invalidates the entire cache.

### Why this was hard to see

The comment directly above that loop names this exact failure mode:

> Comparing first costs a 4 KiB scan per page. Re-translating the blocks on that
> page costs, on this workload, about 25 million cycles. The comparison is not an
> optimisation so much as **the difference between a cache that works across
> launches and one that never survives its own loader.**

The compare-before-copy was added *specifically* to prevent this, and it works —
for the same-boot case. It cannot work across a reboot, because the reference it
compares against is guest RAM, and guest RAM is precisely what does not persist.

**The predicate is subtly wrong.** It asks *"do the bytes I am about to write
differ from what is in guest RAM?"* The invariant the cache actually needs is
*"do the bytes now in guest RAM differ from the bytes these TBs were compiled
from?"* Identical questions within a boot; different questions across one. Note
that **after** the copy the page holds exactly the bytes the TBs were compiled
from — so the flush fires on a page that ends up correct. The invalidation is
sound in mechanism and spurious in this instance.

### FIXED AND VERIFIED, 2026-08-05 — first cross-reboot warm start

Fix in `551d9db` (qemu) + `8fc55f2` (aerosls2). Re-run of the same sequence:

```
qemu bench 500   ->  flush_page page=0 gen->2 ;  TCACHE 0 hit / 8 miss
checkpoint       ->  synced: 8 TBs, 8051 code bytes
<node restart>
qemu bench 500   ->  NO flush_page line ;  TCACHE 8 hit(s) / 0 miss(es)
```

| | cold | warm after reboot |
|---|---|---|
| `flush_page` | `page=0 gen->2` | **absent** |
| `TCACHE` | 0 hit / 8 miss | **8 hit / 0 miss** |
| `TRANSLATE` | 39,210,486 cyc, 8 blocks | **0 cyc, 0 blocks** |
| `CODE` | 8,003 bytes | **0 compiled** |
| `ARENA` consumed this launch | 1,507,392 B | **0** |
| `ARENA` used, total | 1,508,448 B | **1,056 B** |
| `ALLOC` since boot | 13 calls | **4 calls** |

**Phase 2's premise is demonstrated end to end.** Translation is not merely
faster across a reboot, it does not happen: zero blocks compiled, zero bytes
emitted, and the 1.5 MB of TCG arena a cold launch consumes is never
allocated — 1,056 bytes across 4 calls instead of 1,508,448 across 13.

### What to quote from this, and what not to

**Quotable — countable and rig-independent**, the same class as `CODE`:
translation eliminated entirely (`TRANSLATE` 0 cycles, 0 blocks, `CODE` 0
bytes, 8 TB hits), and 1.5 MB of per-launch arena allocation eliminated.

**Not quotable:** the total-cycles ratio (48,289,941 → 4,559,082, ~10.6×).
`EXEC` on this run was **6,128 cyc/load**, against 100–455 for same-boot warm
runs — because restarting the node restarts the outer `qemu-system-x86_64`
too, so the outer emulator must translate our restored host code on first
execution even though *our* translator did not run.

That is a clean confirmation of §3b: a first launch after a boot always carries
outer-translation cost regardless of whether the inner cache hit, and `EXEC`
differences track the outer emulator rather than the guest load path. It also
kills the last reason to quote any cycle figure from this host.

### Fix direction (as implemented)

Persist a per-page digest of the bytes each TB was compiled from, alongside the
TB descriptors, and flush only when the **post-copy** bytes disagree with it.
Eight bytes per page for the handful of pages that actually back TBs — not the
256 MiB of guest RAM, which is the obvious alternative and much worse.

### CONFIRMED by measurement, 2026-08-05

The print was added (`qemu_sls_tcache_flush_page()`, commit `19798a8`) and the
prediction held exactly:

| Run | `flush_page` output | `TCACHE` |
|---|---|---|
| Boot A, 1st launch | `gpa=0x0 page=0 gen->2` — **once** | 0 hit / 8 miss |
| **Boot B, 1st launch — the test** | `gpa=0x0 page=0 gen->3` — **once** | **0 hit / 8 miss** |
| Boot B, 2nd launch — the control | **no line at all** | **8 hit / 0 miss** |

Every element of the prediction is present: exactly one flush, page 0 only, on
the first launch after a boot and not on the warm relaunch. The generation
climbs by one per boot — 2, then 3 — because `flush_page` persists the counter
synchronously, so restored TBs carry `gen_expected` from the previous boot and
are always exactly one behind.

That also settles **which** miss path is taken: the descriptors are present and
correct and are being rejected by the staleness check. It is a generation
mismatch, not an absent TB.

Both falsifiers were live and neither fired. A flush on the warm run would have
meant something else invalidates on every launch; no flush at all would have
meant the descriptors were missing rather than stale. **Root-caused.**

**For the record:** this began as a code-reading hypothesis, which §6 warns
against having watched three of them fail on a different bug. What changed the
outcome was writing down what each possible result would mean *before* running
it, so that a confirming result could not be quietly reinterpreted from a
disconfirming one.

### Minor, noted in passing

`synced: 8 TBs, **8051** code bytes` against `CODE **8003** bytes emitted` — a
48-byte difference between `qemu_sls_codebuf_used` and the bench's emitted
count. Probably prologue or alignment, but it is unexplained and both numbers
are supposed to describe the same code.

### The correct sequence

```bash
tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # populates in-memory
tools/aeroslsctl --host localhost:3002 shell "checkpoint"       # the ONLY thing that writes it
#   expect: [QEMU-SLS TCACHE] synced: N TBs, M code bytes
# reboot the node WITHOUT rebuilding
#   expect in the boot log: [QEMU-SLS TCACHE] warm start — codebuf_used=…
tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # expect TCACHE 8 hit(s)
```

**The reboot must not rebuild.** `AEROSLS_BUILD_ID` is `git rev-parse
--short=12 HEAD` and any commit invalidates the cache by design. If `deploy.sh`
runs as part of the restart, the result is `DIFFERENT BUILD … discarded` — a
correct refusal that once again looks identical to failure.

---

## 4. Cannot be verified here — needs hardware

| Claim | Blocker |
|---|---|
| Any end-to-end speed multiplier | No KVM on the build host; §9 forbids a timing claim without KVM or non-x86 hardware |
| That eliminating softmmu helps on a real ARM64 host | No ARM64 port exists; no ARM64 hardware |
| That qemu-user is measurably faster than qemu-system | `tests/softmmu_ceiling_measurement.sh` exists to establish this; not run here |

---

## 5. What is safe to say today

**Defensible, with the method attached:**

> Eliminating QEMU's software MMU reduces the host code emitted for a guest
> memory access by 5.4× — from 86 bytes to 16 — measured on AeroSLS, bit-identical
> across nine samples spanning five boots and three builds. On that host a further
> ~1.4× of genuine execution work is eliminated beyond what the code-size
> reduction alone accounts for.

**Structurally true and the strongest framing available** (Cross-ISA §10):

> An operating system can do something to an emulator that a process cannot.
> Shadowing guest page tables into host page tables requires owning the page
> tables. FEX-Emu, Box64 and qemu-user are user-mode and structurally cannot.

**Must not be said:**

- "3–5× faster QEMU" — retired in Cross-ISA §10; never measured here.
- Any end-to-end speed multiplier — §9.
- Anything implying an ARM64 build exists.
- "AeroSLS runs on Raspberry Pi" framed as the product. ARM-guest-on-ARM-host is
  the row where KVM wins and TCG never runs (Cross-ISA §1). The live claim is
  **x86-64 guest on ARM host**.

---

## 6. Reproducing this document

```bash
make x86-iso                          # staleness guards need a current binary
tests/stack_frame_budget_check.sh     # stack + frame budget, from the linked binary
size my_sls_kernel.bin                # text / data / bss
readelf -lW my_sls_kernel.bin         # memsz — the number that matters for footprint
nm --size-sort -S my_sls_kernel.bin | tail -20
```

Every row in §1 and §2 has its command inline. If a row cannot be reproduced by
running its command, the row is wrong and this document is the thing to fix.
