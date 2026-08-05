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
| Translation cache survives reboot | Phase2 | **Not shown.** Both 2026-08-05 runs were one boot — run 1 populated, run 2 hit. Proving persistence means rebooting and seeing `TCACHE` hits on the *first* bench. |

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
- the difference, ~5,688 cyc/load, is the outer-translation artifact itself

**251 cycles/load is the first EXEC figure on this hardware not dominated by
the confound.** It cross-checks: `qemu-system-x86_64` without KVM runs roughly
50× slower than native, a bare softmmu-off `MOV` is ~5 cycles native, and
251/5 ≈ 50. The number is consistent with the rig that produced it.

### The trap in the same data

**The 23.7× cold-to-warm ratio must never be quoted as a speedup.** It is the
difference between paying outer-emulator translation and not paying it. On real
hardware there is no outer emulator and the ratio largely disappears. It
measures the test rig, not the system under test — the identical error §5b
caught, in a new form. Quote **251 cyc/load** as a bounded execution cost on an
emulated host; quote the ratio for nothing.

### Procedure

```bash
tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # cold: TCACHE 0/8
tools/aeroslsctl --host localhost:3002 shell "qemu bench 500"   # warm: TCACHE 8/0
```

Check `TCACHE` on each run to confirm which regime you are in. A "warm" run
showing misses, or a "cold" run showing hits, invalidates the pair.

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
