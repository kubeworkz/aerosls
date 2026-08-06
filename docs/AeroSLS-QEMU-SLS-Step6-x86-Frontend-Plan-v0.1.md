# AeroSLS QEMU-SLS — Step 6: the x86-64 Guest Frontend, v0.1

*Cross-ISA Repositioning §8 Step 6. Target agreed 2026-08-05: **arbitrary
statically-linked Linux x86-64 binaries.***

---

## 0. Why this plan opens with a measurement

`sls/Makefile:50-52` in the qemu tree says:

```
# Layer 3: x86 Guest Frontend (translates x86 → TCG IR)
# NOTE: Large (~15K lines). Phase 0 milestone 1 uses TCI instead.
# TCG_GUEST_SRC = target/i386/tcg/translate.c (deferred)
```

That note is the only recorded reason we wrote `sls/sls-x86-frontend.c` (308
lines, 18 opcodes) instead of using QEMU's own frontend (`translate.c` 3,620
lines + `decode-new.c.inc` 3,149). It was a **sequencing** decision for
milestone 1, made when TCI was the execution path — not an architectural
rejection. TCI is now gone (`tci.x86.o` is deliberately not built) and
`-I target/i386` is already on the include path.

Deciding Step 6 from that note would be deciding from a stale estimate. So the
first thing done here was to compile `target/i386/tcg/translate.c` against the
SLS shim and read the errors.

### Reproducing the spike

```bash
cd aerosls2
CF="-ffreestanding -O2 -mcmodel=small -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
    -fno-pie -fno-pic -fno-tree-vectorize"
INC="-I ../qemu/sls/include -I ../qemu/sls -I ../qemu/include -I ../qemu/tcg \
     -I ../qemu/tcg/x86_64 -I ../qemu/accel/tcg -I ../qemu/target/i386"
DEF="-include ../qemu/sls/sls-osdep.h -DSLS_IN_KERNEL=1 -UCONFIG_PLUGIN -DCONFIG_TCG"
gcc -fsyntax-only $CF $INC $DEF ../qemu/target/i386/tcg/translate.c
```

---

## 1. What the spike found

| Step | Errors | What it revealed |
|---|---|---|
| Baseline | **92** | All from ONE undefined macro |
| + 3 QOM `OBJECT_DECLARE_*` stubs | **3** | |
| + `TARGET_LONG_BITS=64` | **1** | |
| + a stubbed QAPI generated header | **294** | The real surface, finally visible |

**The 92 was a mirage.** `target/i386/cpu-qom.h:31` uses
`OBJECT_DECLARE_CPU_TYPE`, which the shim does not define, so it parsed as an
implicit-int function with no semicolon and every declaration after it became
part of that function body. Three one-line macro stubs took 92 to 3.

The 294 that appear afterwards are the genuine dependency surface, and they are
**also** dominated by cascades:

| Count | Symbol | Root cause |
|---|---|---|
| 84 | `dh_ctype_tl` | one thing: the helper-declaration macros need `target_long` |
| 75 | `TCGv_dh_alias_tl` | same |
| 18 | `dh_retvar_decl_dh_alias_tl` | same |
| 18 | `X86CPU` | our own stub erased the macro that declares it |
| ~15 | `Notifier`, `OnOffAuto`, `CPUClass`, `DeviceRealize`, `VMChangeStateEntry`, `MemTxAttrs`, `qemu_irq`, `VMStateDescription`, `DumpState`, … | one each — QOM / device-model types `cpu.h` mentions |

By file: **237 of 294 in `include/exec/helper-head.h.inc`** (all the `tl`
plumbing), 42 in `target/i386/cpu.h`, 12 in `emit.c.inc`, 3 elsewhere.

### What that means

The blocker is **not** 15K lines of decoder. It is that `translate.c` needs
`CPUX86State`, which lives in `cpu.h`, which mentions QEMU's object and device
model throughout. The decoder itself touches almost none of that — `X86CPU` is
the QOM *wrapper*; `CPUX86State` is a plain C struct.

So the shape of the work is a **target shim**: a cut-down `cpu.h` providing
`CPUX86State` and the decoder's enums and constants, with the QOM half declared
as opaque types. That is the same technique `sls/sls-osdep.h` already applies to
`qemu/osdep.h`, one layer up. The precedent exists in this tree and works.

---

## 2. THE LIMIT OF THIS SPIKE — read before believing any of it

**`-fsyntax-only` proves the compiler can parse the file. Nothing else.**

It does not show that it links, and it emphatically does not show that a guest
runs. The gap is `sls/sls-helper-stubs.c`: ~130 helper functions that
**announce their own name and halt**. The x86 decoder emits calls to helpers
constantly — flag computation (`helper_cc_compute_all`), FPU, SSE, string
operations, control-register writes. A parsed decoder wired to stubs produces a
guest that halts on the first instruction whose semantics live in a helper,
which for compiler-generated code is roughly the first arithmetic instruction.

**Implementing those helpers is the actual body of work, and this spike does not
measure it at all.** Nothing below should be read as an estimate of it.

This warning exists because the pattern it guards against has recurred all week:
`http_route` looked fine while carrying an 18 KB struct, the tcache looked
restored while its own loader invalidated it, and the softmmu knob looked wired
while rebuilding two files of 121. A green compile is the same class of signal.

---

## 3. Sequenced plan

Each step has a gate that is a measurement, not an opinion.

**Step 6.1 — Target shim.** Create `sls/sls-target-i386.h` providing
`CPUX86State`, the decoder's enums/constants, and opaque declarations for the
QOM and device types. Wire `TARGET_LONG_BITS=64` and the helper `tl` plumbing.
*Gate:* `translate.c` and `decode-new.c.inc` compile to objects with zero
errors, using the real build's flags, not the Phase 0 makefile's.

**Step 6.2 — Link it.** Add the objects to `TCG_OBJS` and resolve what the
linker reports missing. Expect a long list of `helper_*` symbols; that list IS
the Step 6.4 work item, so capture it verbatim rather than summarising it.
*Gate:* the kernel links, and the missing-symbol list is written down.

**Step 6.3 — Decide the execution loop.** Our `sls-launcher.c` loop and QEMU's
`accel/tcg` translator loop are alternatives, and the kernel currently links
neither `cpu-exec.c` nor `translate-all.c`. Either adapt our loop to drive
`translator_loop`'s output, or adopt theirs.
*Gate:* an explicit decision, recorded, with the reason.

**Step 6.4 — Implement helpers, measured by a real binary.** Work the list from
6.2, prioritised by what a `gcc -static -nostdlib` hello world actually calls.
*Gate:* that binary runs to completion. Then widen: a static busybox applet is
the honest next rung.

**Step 6.5 — Retire or keep `sls-x86-frontend.c`.** If translate.c carries
everything, our 308-line frontend becomes dead code and should go, or be kept
deliberately as a fast path with that stated in its header.
*Gate:* a decision, and no file left that reads as live but is not.

**Steps 6.1–6.3 are days. Step 6.4 is the project.**

---

## 4. Risks and honest unknowns

**The helper surface is unmeasured.** §2. Everything about schedule beyond Step
6.2 is guesswork until that list exists.

**`cpu.h` may resist cutting down.** The spike stubbed types away to see past
them; a real shim must satisfy every *use*, not merely every mention. If
`CPUX86State` turns out to be genuinely entangled with QOM lifecycle, 6.1 gets
much larger and the fallback is growing our own frontend after all — with the
target scaled back from "arbitrary binaries" accordingly.

**Kernel image size.** The image is already 221.6 MiB, of which TCG is ~36 MiB.
`translate.c` plus helpers plus `CPUX86State` will add to that, and the edge
story is partly a footprint story. Measure with `readelf -lW … memsz` after 6.2,
not at the end.

**This does not need the frontend to be finished to matter.** Cross-ISA value
comes from x86-64 guests on non-x86 hosts, and there is still no ARM64 port.
A complete frontend on x86-only hardware demonstrates the technique; it does not
reach the market the Repositioning plan identified. Step 6 and the ARM64 port
are independent, and if only one gets done, the frontend is the one that leaves
us where we already are.
