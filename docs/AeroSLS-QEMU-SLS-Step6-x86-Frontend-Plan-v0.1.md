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

**Step 6.1 — Target shim. ✅ DONE 2026-08-05. Gate met.**

`target/i386/tcg/translate.c` now compiles to a **701,888-byte object with 919
functions**, zero errors, using the real build's flags. The existing TCG core
compiles with zero errors throughout — checked after every edit, not at the
end.

It did not need a new `sls-target-i386.h`. The blockers were all in headers the
shim *already shadows*, so the work was thickening four of them:

| File | Change |
|---|---|
| `sls/include/qom/object.h` | the `OBJECT_DECLARE_*` family, emitting typedefs only |
| `sls/include/hw/core/cpu.h` | pull in `qom/object.h`; add `enum CacheType`, `TranslateForDebugResult`, `CPUState::cc` |
| `sls/include/qemu/typedefs.h` | ~12 QOM/device types; `Notifier`/`CPUClass`/`ResettablePhases` complete because `cpu.h` embeds them by value |
| `sls/include/qapi/qapi-types-machine-common.h` | new — stubs a QAPI-generated header |

Plus two build flags, both upstream's own mechanism rather than invention:
`-DTARGET_LONG_BITS=64` (added to `QEMU_DEFS`) and `-DCOMPILING_PER_TARGET`,
which is what gates the helper `tl` plumbing in `helper-head.h.inc`.

**Two mistakes worth recording**, both the same mistake: stub definitions of
`MemTxAttrs` and `TCGCPUOps` collided with the real headers, which the include
graph already reaches. The shim's job is to fill holes, never to duplicate a
header that resolves — every duplicate is a second definition free to drift.

**The error counts along the way were almost all cascade**, and each drop came
from one hole: 92 → 4 (the `OBJECT_DECLARE_CPU_TYPE` macro) → 276 (real surface
revealed) → 8 (`COMPILING_PER_TARGET` + typedefs) → 0. At no point was the
number an estimate of remaining work.

**Step 6.2 — Wire it into the build and capture the work list. ✅ DONE
2026-08-05.** The symbol list is captured verbatim in
`docs/step6/undefined-symbols.txt` (935 entries). The kernel does **not** link
yet and cannot until 6.4, so the build integration is behind a flag:

```
make x86-iso                      # unchanged, our 18-opcode frontend
make x86-iso SLS_X86_FRONTEND=on  # QEMU's decoder; will not link yet
```

Off by default so `deploy.sh` is untouched. Verified: the default build never
mentions the new object, `=on` adds it, `COMPILING_PER_TARGET` lands on exactly
one compile line (`translate.c`) and not on the generic TCG core, and an
invalid value is a hard error.

The object is built by an **explicit-path rule, not VPATH**. There are 20+
files named `translate.c` in the QEMU tree, one per guest architecture;
resolving through VPATH and a `%`-stem would make which decoder we compile
depend on VPATH search order, so adding an unrelated directory later could
silently swap in another architecture's frontend and still build clean. The
object is named `i386-translate` so the target is visible in the build log.

### What the 765 actually are

**None of them are already stubbed.** `sls-helper-stubs.c` defines 131 helper
names and the overlap with the decoder's 765 is **zero** — not an artifact.
The existing stubs are TCG's *atomic memory* helpers (`helper_atomic_add_fetchb`
…); the decoder wants *x86 instruction semantics* (`helper_aaa`,
`helper_addpd_xmm`, `helper_sha1rnds4`). Two unrelated families.

Approximate breakdown by category (regex-classified, so treat as indicative —
some scalar SSE lands in "other"):

| Category | Count | Share |
|---|---|---|
| Vector — SSE/AVX/MMX | 507 | 66% |
| x87 FPU | 59 | 7% |
| Flags | 5 | <1% |
| Other — integer, system, string | 194 | 25% |

**Roughly three quarters is vector and FPU long tail** that a
`gcc -static -nostdlib` binary never executes, and for which a stub that halts
loudly is a correct answer. The critical path for a first running binary is the
flag helpers plus a subset of the integer group — a far smaller number than 765,
and the reason 6.4 must be ordered by what a target binary calls rather than by
working down the list.

**Step 6.3 — Execution loop. ✅ DECIDED 2026-08-05.**

> **We keep `sls-launcher.c`'s execution loop and TB management, and take
> exactly one file from `accel/tcg`: `translator.c`.**

`x86_translate_code()` (translate.c:3613) drives `i386_tr_ops` through
`translator_loop()`, which lives in `accel/tcg/translator.c` — 522 lines. That
is the only part of `accel/tcg` required. `sls-launcher.c` already does what
`tb_gen_code()` would: it sets `tcg_ctx->gen_tb`, calls `tcg_gen_code()`, and
threads `qemu_sls_tcache_reserve()` through the result.

| | Ours + `translator.c` | Full `accel/tcg` |
|---|---|---|
| New code | **522 lines** | ~5,150 lines |
| Pulls in `cputlb.c` | no | **yes — 2,902 lines** |
| Persistent tcache | untouched | `translate-all.c` owns `tb_gen_code`, which it is built around |
| softmmu=OFF | preserved | adopts what we removed |

**The reason, and it is not primarily size.** `cputlb.c` *is* the software TLB —
95 softmmu symbols, defines `tlb_flush`, `tlb_set_page`, `tlb_fill`. Eliminating
it is the entire content of `tcg_use_softmmu false` and of the 5.41× result.
Adopting the full loop would mean linking the thing this project exists to
bypass. And `translate-all.c` owns `tb_gen_code()`, which is where our
cross-reboot translation cache is integrated — the one feature verified working
end to end this week.

**Cost of the choice, stated honestly:** we stay diverged from upstream, so
future QEMU updates need manual reconciliation in the launcher, and edge cases
that `cpu-exec.c` handles from years of real guests are ours to discover. If
Step 6.4 shows our loop's simplifications failing under real guest code, this
decision is the one to revisit first.

**Done as part of the decision:** `translator.c` compiles clean (3 shim gaps
closed — `AccelState`, the `IcountDecr.u16` layout, and `target_disas`'s
signature) and its object defines `translator_loop`, resolving the decoder's
reference. Both objects are in the build behind `SLS_X86_FRONTEND=on`.

One of those three is worth noting: our `icount_decr` stub had only the `u32`
arm, but `translator.c:81` takes `offsetof(CPUState, neg.icount_decr.u16.low)`
to emit the instruction-budget check into every block. A member that must exist
*at the right offset*, in a struct generated code indexes into. It compiled
everywhere else and would have failed exactly there.

**LANDED 2026-08-12 — the loop runs the decoder, and the fixtures pass on
hardware.** `sls-launcher.c`'s exec loop now calls `x86_translate_code`
through `translator_loop()` under `SLS_X86_FRONTEND=on`, with `SLSCPUState`
becoming the real `CPUX86State` and boot state (hflags, CRs, segments, CPUID
incl. long mode) set so the decoder sees a 64-bit machine. The first helpers
are real — `helper_write_crN` (CR0/CR3/CR4, shadow-MMU semantics) and
`helper_hlt` — and `qemu invl`/`paging`/`selfmod` all pass on the decoder
build with the paging-off bench running. The one real defect found in
verification: the launcher's `g_once_init_enter` stub was a **global
one-shot flag**, so TCG's lazy per-helper call-layout init ran for exactly one
helper and every later helper (e.g. `flush_page`) emitted its calls with zero
argument setup — the INVLPG helper received the prologue's leftover TB pointer
instead of the guest address. Fixed to per-address once semantics (the guest
is single-threaded). The next milestone is Step 6.4's helper long tail,
ordered by what a running guest actually calls.

**Step 6.4 — Implement helpers, measured by a real binary.** *In progress.*

**First milestone reached 2026-08-05: every QEMU-side symbol resolves.** The
unresolved list went from 943 to 24, and all 24 are AeroSLS kernel functions or
libc that the kernel objects supply — each checked against `kernel/` and found.
Zero `helper_*`, zero code-fetch.

Two new files did it:

`sls-i386-helper-stubs.c` defines all **765** helpers, each halting with its own
name. It is not a generated *list* — it expands `target/i386/helper.h` through
QEMU's own `DEF_HELPER` machinery, exactly as `helper-proto.h.inc` does for
prototypes. So every stub carries the real signature and tracks upstream
automatically; there is nothing here to go stale. 764 came out of `helper.h`
directly and the 765th, `helper_info_memset`, is metadata already supplied by
`tcg-runtime-gvec`.

`sls-i386-codefetch.c` supplies guest instruction fetch — `cpu_ld{b,w,l,q}_code_mmu`
and `get_page_addr_code_hostp`. Upstream has these only in `cputlb.c` (the soft
MMU we exclude) or `user-exec.c` (1,271 lines of qemu-user process model:
`mmap_lock`, `TaskState`, signal-based faults). Ours reads straight through the
GPA window, which **is** what softmmu=OFF means, at the fetch path rather than
the data path. It refuses loudly if the guest has paging enabled, since that
needs the shadow walk and returning the wrong bytes would decode as plausible
instructions and fail far from the cause.

It also supplies `tb_lock_page1`/`tb_unlock_page1`/`tb_unlock_pages` as no-ops.
They exist upstream to stop concurrent translation of a guest page; translation
here runs on the BSP one block at a time and there is no `PageDesc` tree to
protect. That is a simplification following from 6.3 and is flagged in place as
the first thing to revisit if guest code misbehaves.

**Two bugs the decoder found in the existing shim**, both worth recording:

- `QEMU_IS_ALIGNED` is a *macro* in the real `qemu/osdep.h`, which our shim
  shadows. Missing, it did not fail the compile — `translator.c:344` turned it
  into an implicit function call that compiled and only failed at link. **A
  missing macro degrades into a plausible function**, which is worse than a
  missing header.
- `abort()` is called directly by `translate.c`; the shim had `sls_abort()` but
  not the standard spelling.

*Remaining gate, unchanged:* a `gcc -static -nostdlib` binary runs to
completion. **The work list is no longer a file.** It is whatever helper a
running guest halts on first — launch, read the name, implement it, repeat.
Ordering by anything else is guessing at what a guest executes.

**First compiled guest reached 2026-08-12 — the loop works.**
`sls/guest/guest.c` is a real `gcc -static -nostdlib` x86-64 binary (71
bytes, `-mno-sse -mno-sse2` so the first milestone measures the integer
path, born in long mode with a stack from the launcher's boot state, paging
off) launched through the decoder by the `qemu compiled` fixture. It divides
`u64/u64` and `u32/u32` from a parameter block and writes the result to a
fixed GPA, so the result is checked against the same arithmetic in C — a
wrong helper fails, not just a missing one. The run sequence was exactly the
predicted one: halt on `helper_divq_EAX`, implement (int_helper.c semantics,
128/64 via `__int128`/libgcc, #DE as a halting kernel panic since guest
exception delivery does not exist yet), re-run, halt on `helper_divl_EAX`,
implement, re-run — **16 instructions to HLT, result matches C, all other
fixtures still pass.** The next halt is whichever helper the next guest
crosses; growing the guest is what finds the next one.

**Iteration 2 (2026-08-12): `rep movsb` + a counted backward-branch loop —
no new helper needed, one launcher defect found.** REP string ops and the
loop are both inline TCG here (`do_gen_rep`, translate.c), so the run passed
without implementing anything — 353 instructions to HLT, the 64-iteration
loop re-entering through the tcache (62 hits), and the result matching C.
The run did surface a real launcher bug: the boot state zeroed `env->df`,
but the decoder stores it as +1/−1 (`tcg-cpu.c`'s `1 - 2*((eflags>>10)&1)`),
so `dshift = df << ot` was 0 and every `rep movsb` byte landed at the same
address (`copied` read back as 0x88, the low byte only). The launcher now
sets `df = 1` (forward) at boot, matching EFLAGS.DF=0 on a fresh machine.
This is the first guest with control flow and the first string op under the
decoder, and both held up.

**Iteration 3 (2026-08-12): `rep stosb` + the REPZ/REPNZ flag-consuming path
— the string ops are still helper-free, and the flag core became real.** The
first run halted exactly where the design predicts pushfq lands:
`helper_read_eflags`. CMPS/SCAS are inline TCG in this QEMU (`do_gen_rep`
with a per-iteration `gen_jcc_noeob` on ZF — there is no `repz_cmps`
helper), so the *string* path needed nothing; what the guest needed was
`pushfq` to capture ZF, and PUSHF emits `gen_helper_read_eflags`. That
helper materializes EFLAGS from the lazy cc state through
`cpu_cc_compute_all`, which lives in `cc_helper.c` (not in this link) — so
the whole flag-computation core came over in one unit: `helper_read_eflags`
plus `helper_cc_compute_all` / `helper_cc_compute_c` / `helper_cc_compute_nz`
(translate.c's `gen_prepare_cc` emits all three for condition codes it
cannot express inline, so the next guest with a CF-consuming or complex jcc
would have hit those stubs anyway), with the upstream
`cc_helper_template.h.inc` expanded through a relative include. Result: 385
instructions to HLT; `rep stosb` filled all 16 bytes; the repz run over
equal strings left ECX=0 with ZF set, the repnz run over strings differing
in bytes 0..4 left ECX=10 with ZF set — the packed ECX/ZF result matched the
C expectation bit-for-bit, so the F2/F3 prefix plumbing and the lazy-flag
machinery are both right. `invl`/`paging`/`selfmod` still pass and the bench
still runs.

**Iteration 4 (2026-08-12): `popfq` — the write side of the flags, one
helper, and the cross-block handoff proven.** The halt was exactly the
predicted `helper_write_eflags` (POPF is one of the two flag helpers
`emit.c.inc`'s `gen_POPF` emits, with a CPL-dependent update mask, followed
by `set_cc_op(CC_OP_EFLAGS)` and `DISAS_EOB_NEXT`). Its body is misc_helper.c's
`cpu_load_eflags` verbatim — CC bits into `cc_src` with `CC_OP = EFLAGS`, the
DF bit decoded into `env->df` as +1/−1 (`1 - 2*DF`), the update mask routing
non-CC bits into `env->eflags` with bit 1 forced — so `read_eflags` (iteration
3) and `write_eflags` are now both real and the flags are a closed loop. The
interesting gate was not the helper itself but what it makes observable: each
popfq ends its TB, so every `jz`/`jc` after it decodes in a fresh block
against the env state the helper left — the guest proves that handoff five
ways (jz taken after ZF=1, jz skipped after ZF=0, jc taken after CF=1, a
pushfq read-back of the popped ZF, and the sharpest one: popfq with DF set
followed by `rep movsb` from src+7/dst+7 copies BACKWARD, which is only true
if the DF bit really reached `env->df`). 423 instructions to HLT; the packed
result and the backward-copied value match C exactly, and
`invl`/`paging`/`selfmod` still pass with the bench running. One fixture
mistake on the way (the expected constant dropped the CF bit — the helper
was right, the fixture was wrong) was caught by the failing read-back, not by
a guest hang.

**Iteration 5 (2026-08-12): `lahf`/`sahf` — no new helper, but the first
structural bug of the integration: the CPUState/CPUArchState alignment gap.**
LAHF/SAHF are INLINE TCG in this QEMU (gen_LAHF/gen_SAHF in emit.c.inc) —
there is no `helper_lahf`/`helper_sahf` — but two real gates surface: in
64-bit mode both decode is gated on CPUID_EXT3_LAHF_LM (else gen_illegal_opcode
→ #UD), and SAHF must leave the flags readable as EFLAGS. The launcher's
"conservative feature set" never seeded FEAT_8000_0001_ECX, so the first run
#UD'd at the very first sahf (guest eip 0x1a4) even after the boot state was
fixed — the diagnostic that named the fault (a temporary print in
helper_raise_exception) showed `feats[ECX]=0x1` at execute time but
`ext3=0x0` at translate time, and the launcher's own X-DIAG prints finally
pinned the cause: **env_ptr itself was corrupted by a TCG-generated store.**
`sizeof(CPUState)` is 0x10888, CPUArchState is 16-byte aligned, so in
SLSX86CPU the env landed at cpu+0x10890 — an 8-byte gap — and every
negative-offset field access from env computed its address 8 bytes too far:
the `can_do_io` byte store (`offsetof` 0x10870, emitted as env-0x18) wrote
0x01 into env_ptr. Iterations 1–4 never noticed because a broken env_ptr only
matters when a later translation reads feature bits through it; iteration 5's
sahf is the first feature-gated instruction. The fix is a contract the
launcher's comment already stated but nothing enforced: `CPUState` now pads
itself to a 16-byte multiple (sls/include/hw/core/cpu.h), so env sits exactly
at cpu+sizeof(CPUState).

Second, `gen_SAHF` in this tree's emit.c.inc never set cc_op — after the
preceding ALU op the next jcc/lahf computed flags from the stale cc_dst
instead of the sahf-written cc_src — so `set_cc_op(CC_OP_EFLAGS)` was added,
matching gen_POPF and upstream. Third, the guest test itself was wrong twice:
the asm loaded `mov $0x40` into **AL** (AH was 0 everywhere, so every sahf
wrote ZF=CF=0 and the fixture's real jcc semantics were never exercised), and
the regenerated guest-bytes.h never actually contained the new binary (a
regen that produced a stale, differently-compiled 71-byte guest — caught by
reading the "launching guest: N bytes" line and fixed by regenerating from
the verified guest.bin). Result: 460 instructions to HLT,
`ahf=0x4047010101` bit-for-bit the C expectation (jz taken after AH=0x40,
jz skipped after AH=0, jc taken after AH=0x01, lahf read-back of AH=0x45 is
0x47 with bit 1 forced, pushfq reads the sahf'd ZF back), every prior check
intact (movsb, stos, cmps ECX/ZF, popf, DF-set backward copy, 62 tcache
hits), and `invl`/`paging`/`selfmod` still pass with the bench running. All
diagnostics removed before commit.

**Iteration 6 (2026-08-12): `cli`/`sti` — the interrupt-flag write path,
and a second no-helper round.** Like LAHF/SAHF, CLI/STI are INLINE TCG in
this QEMU: gen_CLI/gen_STI (emit.c.inc) call gen_reset_eflags/gen_set_eflags
(translate.c), a read-modify-write of the IF bit (0x200) in env->eflags —
there is no `helper_cli`/`helper_sti` anywhere in target/ or sls/. STI
additionally ends the TB (DISAS_EOB_INHIBIT_IRQ), which i386_tr_tb_stop
handles inline (gen_set_hflag(HF_INHIBIT_IRQ_MASK) + exit_tb); the launcher
ignores is_jmp and just executes, so the only effect is that everything
after an sti decodes in a fresh block. The gate is the STATIC-flag
plumbing: IF lives in env->eflags while the CC bits live in cc_src under
cc_op, so a pushfq read-back (helper_read_eflags, real since iteration 3)
must see the written IF bit, and a jcc after cli/sti must still compute
from the lazy cc state. The guest proves four things: IF=1 after sti and
IF=0 after cli (both read back through pushfq), and a ZF=1 from `test`
still driving jz across sti (which ends the block) and across cli (which
does not). 489 instructions to HLT, `if=0x01010101` bit-for-bit the C
expectation, every prior check intact (movsb, stos, cmps ECX/ZF, popf,
sahf/lahf, DF-set backward copy, 62 tcache hits), and
`invl`/`paging`/`selfmod` still pass with the bench running. One fixture
mistake on the way, the same shape as iteration 4's: the expected constant
dropped the cli_zf bit (0x10101 vs the correct 0x01010101) — the machine
was right and the fixture was wrong, caught by the failing read-back.

**Build note — the TLS kernel build was broken on `main` by the user's
in-flight TLS commits, independent of this work:** mbedTLS 3.6.7's public
headers include `<time.h>` (platform_util.h, under HAVE_TIME_DATE) and
`<inttypes.h>` (platform_time.h, for mbedtls_ms_time_t), and the
x86_64-elf freestanding toolchain ships neither — the config comment's
"the cross-compiler provides one" was never true in this environment. The
kernel would not link at all (http.c's fresh object referenced the two new
tls_server renewal symbols). Unblocked by two additive changes in the
user's TLS area, flagged for their review: `MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO long long`
in sls_mbedtls_config.h (skips inttypes.h; long long == int64_t on LP64, so
mbedtls_ms_time_t is ABI-identical), and a freestanding shim
`vendor/mbedtls/shim/time.h` (struct tm + time_t — the full C11 nine-field
layout tls_platform.c's gmtime_r writes) wired into MBEDTLS_INC. The TLS
files now compile in the kernel build. (Landed as aerosls2 3481908, a
separate commit so it is a one-line revert if the user prefers to handle
that area themselves.)

**Iteration 7 (2026-08-12): `clts` — the first CR0 helper.** Unlike
CLI/STI, CLTS DOES cross a helper: gen_CLTS (emit.c.inc) calls
helper_clts with env only, then ends the TB (DISAS_EOB_NEXT). The
surrounding CR0 plumbing was already real: MOV CR0, r64 goes through
helper_write_crN (Step 6.3 — stores cr[0], keeps hflags PE coherent) and
MOV r64, CR0 is an inline tcg_gen_ld_tl of env->cr[0] in gen_load, no
helper. So the only stub to implement was helper_clts itself, and it is
cc_helper.c's verbatim body: `env->cr[0] &= ~CR0_TS_MASK;
env->hflags &= ~HF_TS_MASK;`. The guest writes CR0 = PE|TS (0x9), reads
it back to prove the write took, clts, and reads again — 506
instructions to HLT, `clts=0x10101` bit-for-bit the C expectation (TS
present before, TS clear after, PE intact), every prior check intact
(movsb, stos, cmps, popf, sahf/lahf, cli/sti, DF-set backward copy, 62
tcache hits), and `invl`/`paging`/`selfmod` still pass with the bench
running. All three instructions (MOV r64,CR0 / MOV CR0,r64 / CLTS) are
chk(cpl0) in the decoder; the guest runs at CPL 0, and the write->read->
clear round-trip through env->cr[0] crosses two block ends.

**Iteration 8 (2026-08-12): the carry path — helper_cc_compute_c on the
DYNAMIC boundary, another no-helper round.** The cc core is fully real
(iterations 3-4), and gen_prepare_eflags_c (translate.c) computes CF
INLINE for every concrete CC_OP — SUB/ADD via LTU compares, SHL via the
top bit of cc_src, SAR via bit 0 — so helper_cc_compute_c runs only on
CC_OP_DYNAMIC. That is exactly what a jcc sees as the FIRST
flag-consuming instruction of a fresh TB: i386_tr_init_disas_context
seeds cc_op=DYNAMIC with cc_op_dirty=false, so gen_update_cc_op does not
overwrite the env's stored op, and the helper resolves CF against the
previous block's cc_dst/cc_src/cc_op. The guest forces it: `shl` that
sets CF (CC_OP_SHLQ in env), `clts` to end the block, then a jc at the
head of the next TB. Three markers in RESULT_CF: jc taken with CF=1,
jnc taken with CF=0 (both through the helper on DYNAMIC), and an
add-wrap (0xFFFF..+1, CF=1) feeding `adc $0` in the same block for the
inline carry-IN path. 528 instructions to HLT, `cf=0x10101` bit-for-bit
the C expectation, every prior check intact (movsb, stos, cmps, popf,
sahf/lahf, cli/sti, clts, DF-set backward copy, 62 tcache hits), and
`invl`/`paging`/`selfmod` still pass with the bench running.

**Iteration 9 (2026-08-12): the debug-register move — helper_get_dr /
helper_set_dr (bpt_helper.c), the first system-helper round since
clts.** MOV r64, DRn reads through gen_load -> gen_helper_get_dr and
MOV DRn, r64 writes through gen_store -> gen_helper_set_dr
(emit.c.inc) — both halting stubs until now. bpt_helper.c's bodies
have three parts this build deliberately trims: the DR7.GD #DB path,
the hw_breakpoint_insert/remove dance for DR0-3 (bpt_helper.c is not
part of the link), and the DR6/7 reserved-mask #GP — none of which
have guest-exception delivery or a link surface here, so they would
halt anyway. What IS kept is the DR4/5 aliasing, verbatim: with
CR4.DE clear (the boot state — CR4 = PAE only) DR4 accesses address
DR6 and DR5 address DR7; with DE set the access is a #UD, which halts
naming the fault. The guest does a DR0 write->read round-trip and a
DR4->DR6 aliasing round-trip. 543 instructions to HLT, `dr=0x101`
bit-for-bit the C expectation, every prior check intact, and
`invl`/`paging`/`selfmod` still pass with the bench running. (One
fixture-editing slip on the way, same shape as iteration 6's: the enum
replacement dropped the closing RESULT64/RESULT32 lines and the
launcher object failed to compile — caught by the build, fixed in
seconds.)

**Iteration 10 (2026-08-12): the first MSR instruction — `rdmsr` on
MSR_EFER, helper_rdmsr (misc_helper.c).** gen_RDMSR (emit.c.inc) emits
gen_helper_rdmsr, which was a macro-generated halting stub (the
DEF_HELPER machinery emits every unlisted helper with a body that halts
naming itself). Upstream's body routes through
cpu_svm_check_intercept_param and the APIC; this build has neither, so
the modelled set is the plain env-backed MSRs — EFER, the SYSENTER
triplet, STAR, PAT, PKRS, VM_HSAVE_PA, PERF_STATUS (a constant), and
under TARGET_X86_64 the long-mode pointers (LSTAR/CSTAR/FMASK/FSBASE/
GSBASE/KERNELGSBASE) and TSC_AUX — and anything else halts naming the
MSR rather than upstream's silent val=0. The exit convention is the
verbatim one: env->regs[R_EAX] = lo, env->regs[R_EDX] = hi. The guest
(v10, 1116 bytes) reads EFER three times — the launcher seeds
EFER = LME|LMA (0x500) — checking LMA (bit 10), LME (bit 8), and a zero
high dword. 630 instructions to HLT, `msr=0x10101` bit-for-bit, every
prior check intact, and `invl`/`paging`/`selfmod` still pass with the
bench running.

Two build-flow slips on the way, both now fixed for good:
- The launcher object rule (tcg-objs/%.x86.o: %.c + stamps) did not
  depend on guest-bytes.h, so a header-only regen silently shipped the
  previous guest — the fixture printed the new msr= field with the old
  1001-byte guest (garbage in the unwritten RESULT_MSR slot, caught
  instantly). Added a dependency-only rule
  `tcg-objs/sls-launcher.x86.o: ../qemu/sls/guest/guest-bytes.h`;
- a mid-flow cp clobbered the freshly regenerated header with the stale
  staging copy. The regen now writes the real path and staging is
  synced FROM it.

**Iteration 11 (2026-08-12): the write side — `wrmsr` on EFER and STAR,
helper_wrmsr (misc_helper.c).** gen_WRMSR (emit.c.inc) emits
helper_wrmsr and ends the TB (DISAS_EOB_NEXT). val is assembled from
EDX:EAX as upstream; the modelled set mirrors the rdmsr side and the
default halts naming the MSR (upstream's `goto error` is #GP(0), no
delivery here). The EFER write keeps the feature-mask semantics
verbatim — the boot seeds CPUID LM alone among the EFER features, so
the update mask is LME, and the value goes through cpu_load_efer's
body (helper.c, not in this link — inlined) so hflags' LMA/SVME stay
coherent and the post-write TB still decodes long mode. The guest
(v11, 1399 bytes) proves six things: writing 0 clears LME, LMA
survives the write across a TB boundary (not directly writable), a
write carrying SCE (0x1) has it dropped, writing LME back restores the
0x500 seed, and a full 64-bit STAR write/read round-trips the EDX:EAX
halves. 691 instructions to HLT, `wmsr=0x010101010101` bit-for-bit,
every prior check intact, and `invl`/`paging`/`selfmod` still pass
with the bench running. First run, no slips.

**Step 6.5 — Retire or keep `sls-x86-frontend.c`.** If translate.c carries
everything, our 308-line frontend becomes dead code and should go, or be kept
deliberately as a fast path with that stated in its header.
*Gate:* a decision, and no file left that reads as live but is not.

**Steps 6.1–6.3 are days. Step 6.4 is the project.**

---

## 4. Risks and honest unknowns

**The helper surface — now measured, and it is the project.** §2 said this was
unknown. Step 6.1's object answers it:

```
nm -u translate.o | grep -c helper_     ->  765
nm -u translate.o | wc -l               ->  935
```

**765 undefined `helper_*` symbols.** Against `sls/sls-helper-stubs.c`'s ~130
stubs that announce their name and halt. That gap is Step 6.4, and it is the
first honest measure of it anyone has had.

Not all 765 need real implementations to run a first binary — many are SSE,
FPU and MMX that a simple static binary never touches, and a stub that halts
loudly is a correct answer for an instruction the guest never executes. But the
flag-computation helpers alone (`helper_cc_compute_all` and friends) are on the
path of essentially every compiled instruction, and nothing runs until they are
real. Step 6.4 should be ordered by what a target binary actually calls, not by
walking the list.

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
