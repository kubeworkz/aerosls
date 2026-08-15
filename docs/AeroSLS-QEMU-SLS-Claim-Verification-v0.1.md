# AeroSLS QEMU-SLS — Claim Verification v0.1

*Every quantitative claim in the QEMU-SLS documents, checked against the tree.
Each row carries the command that produced the measurement, so this document
can be re-run rather than believed.*

**Provenance:** each section carries its own commit and date, because a single
header stamp rots. This one said `88003e7` for most of a day after the document
had moved well past it — a stale figure at the top of the document about stale
figures. Sections §3b–§3d name the commit they were measured at; §1 and §2 name
the command that reproduces each row.

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
| ~~AeroSLS on ARM64 does not exist~~ | Cross-ISA §4 | **NO LONGER TRUE — superseded 2026-08-06.** `arch/arm64/` exists: `boot_arm64.S`, `gic.{c,h}`, `mmu.{c,h}`, `uart_pl011.{c,h}`, `linker_arm64.ld`. `make arm64-elf` and `arm64-run` are targets, and `all` builds it. A `simi_arm.c` backend of 3,902 lines exists host-side and kernel-side, byte-identity enforced by `arm_kernel_copy_rediff_check.sh`. | `ls arch/arm64/; grep -nE '^(all\|arm64-elf):' Makefile` |
| Kernel links 15 objects from `../qemu` | `boot.asm`, deploy.sh | **15** (`tci.x86.o` is deliberately excluded, in a comment) | `sed -n '145,160p' Makefile` |
| `QEMU_GUEST_RAM_PAGES` = 65,536 = 256 MiB | Cross-ISA §6 | **65536U**, `kernel/qemu_sls_mmu.h:74` → 256 MiB exactly | `grep -n QEMU_GUEST_RAM_PAGES kernel/qemu_sls_mmu.h` |
| GPA window at 32 TiB | Guest-Address-Space | **`0x200000000000`** = 32 TiB, `qemu_sls_mmu.h:70` | `grep -n QEMU_GPA_HOST_BASE kernel/qemu_sls_mmu.h` |
| `sls_code_buffer` is 32 MiB | Phase2 §1 | **32 MiB** declared, **32.00 MiB** in `.bss` | `tests/code_buffer_budget_check.sh` |
| `qemu_sls_codebuf` is 4 MiB | Phase2 §1 | **4 MiB** declared, **4.00 MiB** (`codebuf_storage`) in `.bss` | `tests/code_buffer_budget_check.sh` |
| The two code buffers are different buffers | Phase2 §1 | **Confirmed** — 32 MiB and 4 MiB, separate symbols, both present | `tests/code_buffer_budget_check.sh` (also asserts the buffers are distinct) |
| `QemuTBDesc` 32 bytes, `tb_table[4096]` | Phase2 §1 | **Consistent**: 4096 × 32 = 128 KiB, as the header states | `grep -n 'tb_table\|32 bytes' kernel/qemu_sls_tcache.h` |
| `qemu_sls_page_gen[65536]` | Phase2 §1 | **65536U** via `QEMU_TCACHE_GEN_PAGES` | `grep -n QEMU_TCACHE_GEN_PAGES kernel/qemu_sls_tcache.h` |
| `max_insns` bug is clamped | Cross-ISA App. A | **Fixed**, two places: refusal at `sls-launcher.c:1185`, hard clamp at `sls-x86-frontend.c:96` | `grep -n TCG_MAX_INSNS ../qemu/sls/*.c` |
| x86 guest frontend is minimal | Cross-ISA §4 | **18 case labels**; `0x8B`/`0x89` + ModRM as described | `grep -cE 'case 0x[0-9A-Fa-f]+:' ../qemu/sls/sls-x86-frontend.c` |
| 89 host test files | session record | **89** | `ls tests/*_host_test.c \| wc -l` |
| Stack is 1 MiB | `boot.asm` | **1,048,576 bytes**, read from the linked binary | `tests/stack_frame_budget_check.sh` |
| Two nodes booted from one image seed differently | `entropy_boot_diversity_check.sh` (GUARD-KIND: runtime — needs a live cluster, so it is owed on build hosts) | **3/3 distinct on 2026-08-15** — live 3-node run on current HEAD (512 MiB/node, TCG, no KVM, `--force`): `1afb842a…` / `b46e561a…` / `b15aade1…`; earlier: **3/3 distinct on 2026-08-13** — `cac65cf8…` / `bc2da14b…` / `091bb9b4…` | `./run-cluster.sh --nodes 3`, then `AEROSLS_TOKEN=<token> tests/entropy_boot_diversity_check.sh` |
| Leader failover: a dead leader's role AND partitions are taken over | `net/consensus.c` election + `kernel/failover.c` (failover_tick/failover_note_heartbeat/failover_live_checkpoint_broadcast) wired into the BSP sweep (net/http.c) and heartbeat RX, driven live via `tools/aeroslsctl` | **Live-wired + verified on 2026-08-15** — 3-node run (512 MiB/node, TCG, no KVM): node 3 was LEADER (term 1); `failover-live` (id=2) created on it; the leader broadcast its state tree every 100 ticks and both followers logged `[DSPP-CKPT] RX: COMPLETE`; node 3 SIGKILLed; node 1 was elected LEADER (term 2); at 300 ticks of silence node 3 was declared DEAD and node 1 recovered from the held checkpoint (seq=19549): adopted 2 partitions (ids 1+2 — id 1 persisted from an earlier run), `rc=0 (OK - adopted)`, and served `partition list` showing `recovered-1`/`recovered-2` owned by node 1. The follower (node 2) declared the death too but observed only — no split-brain. Host test 15/15. Honest boundary, updated 2026-08-15: partition-table ROWS now replicate over DSPP on every change (create/migrate/adopt/destroy announce, kernel/partition.c + net/dspp.c's DSPP_PARTITION_ANNOUNCE/WITHDRAW). Verified live on the same 3-node run: `repl-test` created on the leader appeared in a follower's `partition list` (follower logged `[PARTITION] sync: partition 2 'repl-test' (owner node 2) learned from node 2`), a destroy withdrew it from the follower, and after the leader was killed the adoption's owner handoff replicated to the other survivor (`partition 1 'recovered-1' (owner node 1) learned from node 1`). Periodic re-announce added 2026-08-15: each node re-broadcasts the rows it OWNS every 1000 ticks (partition_reannounce_tick, BSP sweep), so a node that boots after a create converges within one period without waiting for the next mutation. Verified live: a follower's log repeated `[PARTITION] sync: partition 4 'ra-test' (owner node 2) learned from node 2` on the schedule with no mutation between; then a follower was SIGKILLed while `ra-test` existed and, relaunched, converged to node 2's rows (ra-test/failover-test/adopt-repl) from the periodic broadcasts alone. Remaining boundaries: replicated rows are runtime state -- the RX apply runs in the timer ISR and does not persist, matching the service-registry remote-cache rule; catalog OBJECT data and stream bytes still move only via the migrate families; and the checkpoint carries table metadata. Earlier (pre-wiring) run: **2026-08-15** — node 2 LEADER, SIGKILLed, node 1 elected term 2, leadership only | `./run-cluster.sh --nodes 3`, `aeroslsctl shell "partition create <name>"` on the leader, `kill -9 <leader-pid>`, then read `cluster/node1.log` for `[FAILOVER] Adopted partition` and `aeroslsctl shell "partition list"` |
| Kernel image is freestanding — no glibc, no user space | checked at the link, not the headers | **0 undefined symbols, no `.dynamic`, no `PT_INTERP` on 2026-08-13** — verified on the shim-built `my_sls_kernel.bin` | `tests/no_hosted_link_check.sh` |

---

## 1b. This document went stale, in six days, on its most important row

**Recorded 2026-08-05 as verified: "AeroSLS on ARM64 does not exist — confirmed
absent." Untrue by 2026-08-06.**

An ARM64 port landed: `arch/arm64/` with boot, GIC, MMU, PL011 UART and its own
linker script; `arm64-elf` and `arm64-run` targets; `all` building it alongside
x86 and RISC-V. RISC-V grew to 18 files and four build variants. Six new guard
scripts arrived with them.

That row was not wrong when written — it was checked, with the command beside
it, and the command still reproduces the check. It went stale because the world
moved, which is the exact failure this document was created to catch. It is
worth stating plainly rather than quietly editing:

**The document written to catch stale claims went stale first, and on the single
row with the most strategic weight in it.** Cross-ISA's whole repositioning
turns on x86-64 guests running on non-x86 hosts, and "no ARM64 port exists" was
the sentence standing between the technique and the market.

Two things follow.

**The rule this document proposes survives the embarrassment intact.** Every row
here carries the command that reproduces it, so this took one command to detect
and one to correct. A row asserting "ARM64 does not exist" with no command
beside it would have been argued about instead.

**Nothing here should be read as current without re-running it.** The header
says each section carries its own date; treat those dates as the claim, not the
prose. A verification document is a photograph, not a mirror.

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
| 86 bytes of host code per guest load (softmmu ON) | Cross-ISA §5b | Runtime. 9 samples, zero variance reported. **RE-MEASURED 2026-08-13 on `c85ea56`: 90 bytes/load (45,041 bytes) — see §3e.** |
| 16 bytes per guest load (softmmu OFF) | Cross-ISA §5c | **RE-VERIFIED 2026-08-05** on `88003e7`: `CODE 8003 bytes → 16 bytes/load`, exact match. **Re-confirmed 2026-08-13 on `a031cb6` — see §3e** (`CODE 8105 bytes`, still **16 bytes/load**). |
| CODE 43,293 → 8,003 bytes, **5.41×** | Cross-ISA §5c | **FULLY RE-VERIFIED 2026-08-05** at `bebbc6e421df`. Both sides re-run from one flag apart: 43,293 and 8,003, ratio 5.4096×, all four figures bit-identical to 2026-08-04. See §3d. **RE-MEASURED 2026-08-13 on `c85ea56`: 45,041 → 8,105 = 5.56× (90 vs 16 bytes/load) — see §3e.** |
| EXEC ratio 7.56×, excess **~1.4×** real work | Cross-ISA §5c | **DID NOT REPRODUCE.** 2026-08-05 A/B gave exec_ratio 6.12× and excess **1.13×**, below §5c's own 1.30–1.50 range. n=1 per side against §5c's n=5, so not a refutation — but unreproduced, and not quotable until re-run at n=5. See §3d. |
| Node 2 silent halt in `map_guest_ram` | Cross-ISA §6 | **No longer reproduces (2026-08-05), not diagnosed.** `guest RAM mapped: 256 MiB` now prints. §8's gate required a root cause and was not met; see §6. |
| Arena leak, 10,353,840 bytes/launch | Phase2 App. | **Superseded.** 1,507,392 cold, **0** warm, 7 free/7 reuse. Leak fixed; see Phase2 appendix. |
| Translation cache survives reboot | Phase2 | **YES — verified end to end, 2026-08-05.** First launch after a reboot: no `flush_page`, `TCACHE 8 hit / 0 miss`, `TRANSLATE` 0 cycles / 0 blocks, `CODE` 0 bytes, arena 1,056 B against 1,508,448 B cold. Fixed in `551d9db` + `8fc55f2`; evidence in §3c. **Re-verified end to end 2026-08-13 on `a031cb6` — see §3e.** |

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

### The 8051-vs-8003 gap — EXPLAINED 2026-08-05

`synced: 8 TBs, **8051** code bytes` against `CODE **8003** bytes emitted`.
Both numbers are correct; they measure different things, and neither is a bug.

| | Source | Counts |
|---|---|---|
| `CODE` **8003** | `sls_last_code_bytes`, `sls-launcher.c:885` | sum of raw `gen_bytes` per block — what TCG actually emitted |
| `synced` **8051** | `qemu_sls_codebuf_used`, `qemu_sls_tcache.c:389` | high-water mark in the code buffer, **including alignment gaps** |

Blocks are placed 16-byte aligned (`qemu_sls_tcache.c:355`,
`off = (used + 15) & ~15`) because generated blocks are entered by an indirect
call and a misaligned entry costs a fetch penalty on every execution for the
life of the cache. Eight blocks means seven inter-block boundaries:

```
8051 - 8003 = 48 bytes over 7 boundaries = 6.86 avg
expected mean for 16-byte alignment on arbitrary sizes ≈ 7.5
```

Consistent, and deterministic — block sizes never vary, so the padding never
varies either, which is why both figures have been stable across every boot.

**Why this matters beyond curiosity:** it confirms `CODE` is the right metric
for the A/B and `codebuf_used` is not. `CODE` is pure code generation.
`codebuf_used` folds in an allocation policy, so it would move if someone
changed the alignment without a single byte of generated code changing. A
ratio built on it would be measuring the allocator.

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

## 3d. The softmmu A/B — COMPLETE, both sides reproducible

**2026-08-05, node 2, commit `bebbc6e421df`.** Both sides built from identical
sources, one documented flag apart (`SLS_SOFTMMU=on|off`), first launch after
boot so translation actually runs.

| | softmmu ON | softmmu OFF | ratio | §5c, 2026-08-04 |
|---|---|---|---|---|
| **`CODE` bytes emitted** | **43,293** | **8,003** | **5.4096×** | 5.41× ✓ |
| **bytes per guest load** | **86** | **16** | **5.375×** | 5.4× ✓ |
| instructions executed | 502 | 502 | — | 502 ✓ |

**All four numbers are bit-identical to the measurement taken a week earlier**,
across many commits, a Makefile rebuilt from scratch twice, and the
configuration-stamp and clean fixes. `CODE` has now never varied in any sample
this project has taken.

This is the first time the headline ratio rests on two builds that can be
reproduced on demand. Until `SLS_FORCE_SOFTMMU` existed the ON side required
hand-editing `tcg-internal.h`, so half the published figure came from a build
nobody could recreate.

### The `~1.4× real work` claim did NOT reproduce

§5c's more attractive claim — that beyond the code-size reduction a further
1.3–1.5× of genuine execution work is eliminated — does not hold at this
sample size:

```
code_ratio = 43,293 / 8,003  = 5.41x     (exact, zero variance)
exec_ratio = 45,470 / 7,426  = 6.12x     (n=1 per side)
excess     = 6.12 / 5.41     = 1.13x     (5c reported ~1.40x, range 1.30-1.50)
```

1.13× is **below** §5c's stated range. That is not a refutation — §5c used five
runs per side and `EXEC` carries a cold CV of ~10% on this host, so a single
sample each cannot settle it. But it does mean the excess figure is currently
**unreproduced**, and it must not be quoted until it has had the same n=5
treatment §5b gave the ON baseline.

The asymmetry is worth stating plainly: the metric with zero variance
reproduced to the digit; the metric derived from cycle counts did not. That is
the third independent confirmation of §5b's original conclusion.

### Also confirmed on the ON build

`TCACHE 8 hit(s), 0 miss(es)` on the warm relaunch, `TRANSLATE 0`. The
persistent cache works on the ON side too, and its identity stamp differs from
the OFF build's — so the two configurations cannot contaminate each other's
caches, which is what the softmmu term in `qemu_tcache_identity_of()` was added
to guarantee.

---

## 3e. Re-run on current HEAD — 2026-08-13 (`c85ea56`)

The rows above were measured at `bebbc6e421df`/`88003e7` (2026-08-04/05); the
tree has moved a week and the decoder frontend since. Per the note under the
§3 table, re-run before quoting. Both sides built from identical sources one
flag apart (`SLS_SOFTMMU=on|off`), first launch after boot, cold; 512
MiB/node, TCG, no KVM. Build ID `c85ea5683337`.

| Claim | 2026-08-04/05 | **2026-08-13 (`c85ea56`)** | Verdict |
|---|---|---|---|
| `CODE`, softmmu OFF | 8,003 bytes | **8,105 bytes** | moved +1.3%; still **16 bytes/load** |
| `CODE`, softmmu ON | 43,293 bytes | **45,041 bytes** | moved +4.0%; now **90 bytes/load** |
| **ratio, `CODE`** | **5.41×** | **5.56×** (45,041 / 8,105) | re-measured; moved UP |
| **ratio, bytes/load** | 5.38× | **5.63×** (90 / 16) | re-measured |
| instructions executed | 502 | **502** (both sides) | bit-identical |
| arena, cold launch | 1,507,392 B | **1,507,392 B** | bit-identical |
| TCACHE cold → warm | 0/8 → 8/0 | **0/8 → 8/0** | identical regimes |
| cache survives reboot | warm start; first bench 8/0, `TRANSLATE` 0, `CODE` 0 | **warm start `codebuf_used=8174` (= checkpoint's `synced: 8174`); first bench 8 hit / 0 miss, `TRANSLATE` 0, `CODE` 0, `ALLOC` 4 calls / 842 B** | re-verified end to end |
| cold EXEC | 5,939 / 5,434 cyc/load | 4,810 | inside §5b's cross-boot band; not a result |
| warm EXEC | 251 / 455 | 372 | inside the documented noisy band; still no quotable value |

**Why the ON side could not run on a bare-metal toolchain until now — and
what fixed it.** The ON side forces a full rebuild (AB_STAMP), which
recompiles mbedTLS, and this host's cross toolchain (`$HOME/opt/cross`,
bare-metal `x86_64-elf-gcc` 13.2.0) ships **no libc headers** — mbedTLS
`alignment.h`'s `#include <string.h>` failed the first attempt. The
freestanding shim dir (`vendor/mbedtls/shim/`) previously supplied only
`time.h`; it now also supplies `string.h`, `stdlib.h`, `assert.h` and
`stdio.h`, each declaring only the measured set of symbols the 107 compiled
library files reference — so a future mbedTLS bump that needs more fails at
compile/link time rather than silently. The config additionally defines
`MBEDTLS_PRINTF_MS_TIME "lld"` so debug.h skips `<inttypes.h>` (whose PRId64
would be wrong for the forced-`long long` `mbedtls_ms_time_t` anyway). Both
sides now build on the freestanding toolchain; the deploy host's
libc-equipped toolchain is unaffected — the shims shadow its headers with the
same declarations, exactly as shim/time.h already did.

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
> memory access by **5.41×** — from 86 bytes to 16. Measured on AeroSLS at
> commit `bebbc6e421df`, both sides built from identical sources one build flag
> apart (`SLS_SOFTMMU=on|off`), reproducible on demand. Bit-identical to the
> same measurement taken a week and many commits earlier; `CODE` has never
> varied in any sample this project has taken.

**Also defensible, and a better fit for constrained hardware:**

> Across a reboot, translation does not merely get faster — it does not happen.
> Zero blocks compiled, zero bytes emitted, all translation blocks served from a
> cache restored from NVMe, and the 1.5 MB of arena a cold launch consumes is
> never allocated: 1,056 bytes across 4 calls, against 1,508,448 across 13.

**No longer claimable — withdrawn 2026-08-05:** the "~1.4× of genuine execution
work eliminated beyond the code-size reduction." It came out at 1.13×, below
§5c's own 1.30–1.50 range, on a single sample per side. See §3d. Needs n=5 per
side before it can be used, and until then it is unreproduced rather than
merely unverified.

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
tests/no_hosted_link_check.sh     # freestanding image: 0 undefined symbols, no dynamic section, no interpreter
tests/code_buffer_budget_check.sh # code buffers link at their declared budgets (32 MiB / 4 MiB) and are distinct
size my_sls_kernel.bin                # text / data / bss
readelf -lW my_sls_kernel.bin         # memsz — the number that matters for footprint
nm --size-sort -S my_sls_kernel.bin | tail -20
./run-cluster.sh --nodes 3            # the entropy-diversity guard needs a LIVE
                                     # cluster (one image, N boots), not a build
AEROSLS_TOKEN=<token> \
    tests/entropy_boot_diversity_check.sh
python3 tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # §3 numbers; run twice for cold/warm
```

Every row in §1 and §2 has its command inline. If a row cannot be reproduced by
running its command, the row is wrong and this document is the thing to fix.
