# AeroSLS SIMI — AArch64 (A64) Native Backend, v0.1

*Third SIMI native target, mirroring Phase 5's RV64 proof. Plan only — nothing
implemented yet. When shipped, this needs a §7 roadmap row and a §16 phase
entry (likely "Phase 15") in `docs/AeroSLS-SIMI-ISA-v0.1.md`, following the
exact conventions Phase 5 and Phase 14 established.*

---

## 0. Where this fits

SIMI's core claim is that one `.tmo` bytecode object is **technology-independent
and re-translatable** — the System/38 TIMI idea this whole project is built on
(`AeroSLS-SIMI-ISA-v0.1.md` §1, §7). Today that claim rests on two targets:

| Target | Proof strength | Where |
|---|---|---|
| x86-64 (Phase 3) | **Real hardware** — the translated bytes are mmap'd `PROT_EXEC` and executed by the host CPU | `tools/simi/simi_x86.c` + `simi_jit_test.c` |
| RV64 (Phase 5) | **Purpose-built decoder+executor** — `rv64_exec.c` stands in for a real RV64 CPU, because the sandbox has none | `tools/simi/simi_riscv.c` + `simi_riscv_verify.c` |

Two points define a line; the re-translatability claim deserves a third. This
plan scopes an **AArch64 (A64) backend** — `tools/simi/simi_arm.c` emitting
real ARMv8-A 64-bit machine code, verified by a new `a64_exec.c` decoder+
executor in the exact image of `rv64_exec.c`. It is the direct successor to
Phase 5's deliverable and inherits everything Phase 6/7/12/14 added since:
RESOLVE/OBJSIZE/OBJTYPE, capability tags, the argument-tag mask, and JMPR.

**What this is NOT.** This is not the Step-6 ARM *guest frontend* — the QEMU
`target/arm` decoder behind an `SLS_ARM_FRONTEND` flag, which is the ARM-flavored
twin of the in-flight x86-64 guest emulator (`docs/AeroSLS-QEMU-SLS-Step6-x86-
Frontend-Plan-v0.1.md`). That project translates *x86/ARM guest instructions to
TCG IR* so the kernel can run foreign binaries; this project translates *SIMI
bytecode to A64 machine code* so one SIMI program can run on ARM hardware. They
share the word "ARM" and nothing else — different input, different output,
different file set. A future ARM64 *kernel* build could host both, but neither
depends on the other.

**Host-toolchain scope, same as Phase 5.** Phase 5's honest deliverable was the
host toolchain — the kernel had no RISC-V process/loader/object-catalog
infrastructure to wire into, and the sandbox had no riscv64 toolchain, no
`qemu-system-riscv64`, and no root to install either (§12). The same constraint
applies here: **a byte-correct A64 translator and its host-side verifier are the
milestone, executed only by `a64_exec.c` until real ARM hardware (or a QEMU
aarch64 target, if a toolchain becomes available) exists.** That is not a
weakening — it is precisely how Phase 5 shipped, and the RV64 work only became a
kernel deliverable later, in Phase 9's sub-phases.

---

## 1. Scope

**What the backend is:** a portable, no-libc encoder that consumes the existing
`.tmo` object format (`SimiObjHdr`/`TxEntryRec`/`TxNameRec`, the same on-disk
layout `SIMI_X_MAGIC`/`SIMI_RV_MAGIC` describe) and emits A64 machine code,
mirroring `simi_riscv.c` function-for-function — same `CodeBuf`, same two-pass
`g_fixups`, same memory-resident symbolic-register slots, same capability-tag
region. The **object format and the ISA change not one byte**; that immutability
*is* the retargeting claim, and it is non-negotiable.

**In scope for v1:**
- The full current opcode surface as `simi_riscv.c` implements it: all 31
  opcodes including MUL/DIV/MOD (via `madd`/`msub`/`udiv`/`sdiv`), shifts,
  typed load/store (all 10 widths×signedness), PTRADD, LEA, CALL/RET, CMP's ten
  relations, JMPR **with the §16 Phase 14 non-negotiable bounds check**, and the
  v0.3 capability-tag mechanism (Phase 7) including the argument-tag mask
  (Phase 12).
- The same "narrow v1, flag it, move on" discipline Phase 5 used for
  `TX_RV_MAX_REGS`, Phase 6 for the 31-character name limit: the A64 port makes
  the same scope cuts as `simi_riscv.c`, no more, no less.

**Out of scope for v1** (each identical to the RV64 precedent):
- **Float codegen.** Rejected outright with `TX_AR_ERR_FLOAT_UNSUPPORTED`,
  mirroring RV64's Phase 10 exclusion — a translator with no type dispatch must
  not read an IEEE `F64` bit pattern as a plain integer and silently produce a
  wrong answer. A64 *hardware* float is trivially available on real ARM cores;
  the cut is a scoping decision, not a capability limit, exactly as documented
  in `simi_riscv.h`'s error enum comment.
- **NEON/SIMD, atomics, system instructions, AArch32, Thumb.** The decoder and
  the translator's encodings cover base A64 integer only. "ARM" here means
  strictly **A64 (ARMv8-A, 64-bit)** — the file name `simi_arm.c` is the
  architecture-family name the way `simi_riscv.c` is for RISC-V; its header
  states the A64-only narrowing up front, mirroring `simi_riscv.h`.
- **Real register allocation** (Phase 11). RV64 audited and deferred the
  x86-side linear-scan allocator; the A64 port does the same. 64 symbolic
  registers stay memory-resident — A64 has 31 GPRs, so they cannot be physical
  registers anyway; the RV64 slot model transfers directly (see §4.2).
- **Kernel wiring and kernel execution.** There is no `simi_translate_arm.c`
  and no arm64 kernel target. `kernel/simi_arm.c` arrives as a byte-identical
  port compiled under freestanding flags (the Phase 3/5 convention) once the
  host-side translator is proven — but *executing* A64 inside this kernel needs
  an arm64 kernel build or real ARM hardware, the exact shape of Phase 9e's
  still-open gap for RV64.

---

## 2. Verification strategy: why `a64_exec.c` exists

Phase 3's verification method — mmap the translated bytes `PROT_EXEC` and call
them as a real function on the host CPU — is unavailable for the same reason it
was for RV64: **the host CPU is x86-64; there is no A64 execution unit to hand
the bytes to.** A riscv64 toolchain was never available in this sandbox, and the
RV64 workaround was documented, not fixed: a small purpose-built decoder+
executor covering *exactly* the encodings the translator emits, no CSRs, no FP,
no traps (§12). `a64_exec.c` is that same tool for A64.

The honest strength ranking is unchanged from §12: **real-hardware execution
(Phase 3) > purpose-built decoder execution (Phase 5, and this plan) > static
review.** A bug in both the encoder *and* the decoder that happens to agree
would slip through — but that is materially stronger than review,which is exactly what missed the x86 ModRM bug in Phase 3, the RV64 `reg_disp()` slot-0 aliasing in Phase 5, and the RV64 `jalr`-clears-bit-0 host-call corruption in Phase 6 (§13). Every one of those was caught only because emitted bytes were actually
executed against a CPU model. The A64 port gets the same safety net, built
first, before the encoder grows beyond what the decoder can run.

**The four-way discipline.** Every `.simi` test in `tools/simi/tests/` is graded
by expected result. The ARM backend turns the existing three-way regression
(interpreter / x86-native / RV64) into a four-way one; a test is only "PASS"
when all four engines agree on the same number. The pre-existing skip
conventions carry over untouched (`cap_forge_debug`, `mem_ops`, `float_ops` —
the last because float is out of RV64's and A64's v1 scope alike).

---

## 3. The `a64_exec.c` decode+execute subset

Mirror of `rv64_exec.c` (211 lines) — a guest CPU, a step budget, a memory
buffer, and a small set of status codes:

```c
enum { A64_EXEC_OK = 0,          /* pc reached the sentinel return address */
       A64_EXEC_STEP_LIMIT,      /* step budget exceeded — infinite loop or decode bug */
       A64_EXEC_BAD_INSTR,       /* encoding this executor doesn't implement */
       A64_EXEC_MEM_FAULT };     /* load/store/fetch outside the guest buffer */

struct A64Cpu { uint64_t x[31];   /* x0..x30 — x31 is XZR/SP, not a GPR */
                uint64_t pc;
                uint8_t* mem;
                uint32_t mem_size; };

int a64_exec_run(struct A64Cpu* cpu, uint64_t max_steps);
const char* a64_exec_strerror(int code);
```

Same sentinel machinery as `rv64_exec.h`:

- `A64_EXEC_SENTINEL_RA 0xFFFFFFFFFFFFFFF0ull` — the caller preloads `x30`
  (link register) with it; when a `ret` (`br x30`) fires it, the "top-level call
  has returned" signal has occurred.
- `A64_EXEC_HOSTFN_BASE 0xFFFFFFFE00000000ull` + a registered host-callback
  table — `a64_exec_set_hostfn(idx, fn)` / `a64_exec_hostfn_addr(idx)` — so
  RESOLVE/OBJSIZE/OBJTYPE's `bl` to a "function address" invokes a real host C
  function instead of faulting. **One RV64 lesson does not transfer**: RV64's
  `jalr` unconditionally clears the target's bit 0, which corrupted every
  odd-numbered sentinel slot until the stride-16 fix (§13). A64's `br` performs
  no bit masking — branch targets are simply required to be 4-byte aligned —
  so sentinel slots can be dense. The sentinel base above is 16-aligned and
  satisfies that requirement. The mechanism (reserved range → callback table)
  transfers as-is.

**The instruction set the decoder implements — the exact encodings
`simi_arm.c` emits, M0 subset first:**

| Class | Encodings | Notes |
|---|---|---|
| Integer ALU | `add`/`sub` (shifted-register + 12-bit immediate), `and`/`orr`/`eor`, `movz`/`movk` | `movz`+`movk` in up to 4×16-bit pieces materializes any 64-bit constant — §4.1 |
| Shifts | `lsl`/`lsr`/`asr` (register + immediate forms) | |
| Loads/stores | `ldr`/`str` (8/16/32/64), `ldrsb`/`ldrsh`/`ldrsw` sign-extending, `ldrb`/`ldrh` zero — 12-bit unsigned scaled offsets | Frame slots off the base register, §4.2 |
| Control | `b` (imm26, ±128 MB), `bl`, `br`, `ret` (`br x30`), `cbz`/`cbnz` (imm19, ±1 MB) | `cbz`/`cbnz` cover SIMI's `BC` zero/nonzero test — **no NZCV flags in M0**, §4.3 |
| Host calls | `bl` to the sentinel range | → registered host callback, §3 above |

**M0 has no NZCV at all** — the decoder's flag path does not exist until M1's
CMP relation synthesis needs it (§4.3). Status codes, budget, fault handling:
all direct analogs of `rv64_exec.c`'s.

---

## 4. A64 design decisions

This is where the port is not a mechanical copy. Each decision below is stated
the way this repo documents per-target wrinkles — with the exact numbers that
make them checkable.

**4.1 — `li64` is simpler than RV64's, not harder.** RV64 needed the
`auipc`+`ld` literal-pool technique to stand in for x86's `movabs`. A64 has a
native 64-bit materialization primitive: `movz Xd, #imm16, LSL #shift` (sets one
16-bit field, zeroing the rest) followed by up to three `movk` (set a field
without clearing others). Any 64-bit constant is ≤ 4 instructions, straight-line,
no literal pool, no `%pcrel_hi`/`%pcrel_lo` split to patch. The JMPR runtime
table (M1) still wants a base address, but a PC-relative literal `ldr Xt, label`
handles that without resurrecting the pool machinery.

**4.2 — Frame slots: the one real structural divergence from the RV64 port.**
RV64's slots live at *negative* 12-bit signed offsets from `s0`
(`reg_disp(i) = -(8(i+1)+16)`, tags down to `s0-592`) — RV64's I-type immediate
is signed, so that just works. A64's scaled-offset `ldr`/`str` immediate is
**unsigned** (12-bit, ×8 for 64-bit → 0..32760), and the unscaled `ldur`/`stur`
covers only ±256 — too small for a 592-byte frame. The settlement: **one
`sub xBASE, xFP, #592` in the prologue**, after which every slot and tag sits at
a *positive* scaled offset from `xBASE`:

```
frame (s0 = sp at entry):  xFP = s0, saved x30/xFP pair at [s0-16, s0)
                           xBASE = xFP - 592        (1 sub)
slot i  at  xBASE + (568 - 8i)    → offsets 64..568, all 8-aligned, all ≥ 0
tag  i  at  xBASE + i             → offsets 0..63, byte loads/stores
SP      decremented by 592 after xBASE is set  → total frame 608 bytes,
                                                 16-aligned (608 = 38×16, §4.6)
```

`xBASE` is a fixed dedicated register (`x22`, outside the scratch set) — the
`x0-x30` GPR file is otherwise fully available, unlike RV64 where `s0` did
double duty as frame base. This mirrors the RV64 `reg_disp()` comment's
explicitness deliberately: the slot-0-aliasing bug that cost Phase 5 a cycle was
a geometry error exactly like the ones the offset table above prevents by
writing it down.

**4.3 — `BC` needs no flags; CMP relation synthesis is where NZCV first
appears.** SIMI's `BC` tests a register against zero — A64's `cbz`/`cbnz`
(imm19, ±1 MB) are the native form of exactly that test, so the M0 subset ships
flag-free. The ten CMP relations are a different story: RV64 synthesizes them
from `slt`/`sltu`/`xori`/`sub` because it has flag-free compare primitives.
**A64 has no `slt`** — the flag-free relation set doesn't exist; the honest
mapping is `cmp` + `cset`/`csinc`/`csel`, which *read NZCV*. So M1's `emit_cmp`
is the single place the decoder grows a flag model (N/Z/C/V from `cmp`/`subs`,
evaluated exactly like real hardware: borrow = C for subtraction, etc.), and the
translator lowers each relation to the canonical `cmp`+`cset` pair (e.g.
`REL_LT` → `cmp xn,xm; cset xd, lt`). This is bounded, real work, and it is
deliberately excluded from M0 so the first decoder stays flag-free and auditable.

**4.4 — CALL/RET and the trampoline.** A64's `bl` links to `x30` and `ret` is
`br x30` — there is no `jalr`-with-explicit-rd, so the port is *simpler* than
RV64's `jal`/`jalr` pair. The trampoline still must save its incoming `x30`
before its `bl` clobbers it and restore it before its final `ret` — the exact
shape of RV64's trampoline-saves-ra note, with `ret` in place of `jalr
x0,0(ra)`. Result register: `x0` (the real A64 ABI arg0/return register),
mirroring RV64's `x10`/`a0` choice for RESOLVE/OBJSIZE/OBJTYPE calls and the
`t0` result convention elsewhere.

**4.5 — Host-call sentinels need no stride.** See §3: A64 `br` does not mask
target bits, so the §13 stride-16 fix is unnecessary; keep the same reserved
base + callback table, and keep the sentinel base 4-byte aligned (it is).

**4.6 — SP must stay 16-byte aligned at all times.** An A64 architectural rule
RV64 didn't impose (RV64 `sp` needs only natural alignment). The prologue and
trampoline keep it by construction — every stack delta in §4.2 is a multiple of
16 — and the decoder should not enforce it (the *real* hardware run later will,
and the decoder being laxer than the hardware is a documented, known-laxer
direction — see §7).

**4.7 — Branch fixups: whole-word, same two-pass.** `B`/`CBZ`/`CBNZ` scatter
their offsets across imm26/imm19 in non-contiguous bit fields, exactly like
RV64's J/B-type — so the port keeps RV64's `patch32`-rewrite-the-whole-word
approach and its `g_fixups` table (no contiguous `rel32` to patch in place the
way x86 has).

**4.8 — Zero-shared-header policy, inherited.** `simi_arm.h` names its own
structs and macros (`SimiObjHdrAR`, `TxEntryRecAR`, `SIMI_AR_MAGIC`,
`TX_AR_*`) even though the on-disk layout is identical — so a future tool that
wants to compare all three targets' output in one translation unit can include
all three headers without redefinition errors (the TxEntryRec collision class
this project hit once already; see `simi_riscv.h`'s top comment).

**4.9 — Width/signedness mapping is 1:1, like RV64.** A64 has a native load for
every SIMI width×signedness combination (`ldrsb`/`ldrsh`/`ldrsw`/`ldrb`/`ldrh`
/`ldr`/`ldrsw`), so `load_typed`/`store_typed` port as a direct table — no x86's
`movsx`/`movzx` zoo to hand-pick.

**4.10 — `x31` is not a GPR.** Register 31 means `XZR` (reads as zero, writes
discarded) in ALU contexts and `SP` in load/store/add base contexts. The
translator uses fixed dedicated registers (`x29` fp, `x22` frame base, `x30`
lr, `x9`-`x11` scratch) so the distinction never bites, but the decoder must
honor both meanings when it decodes the encoding — a per-instruction
context-sensitive rule the RV64 executor never had (RV64 `x0` is unconditionally
zero, no second meaning).

---

## 5. Files and interfaces

**New files (host toolchain only — no kernel change until M2):**

| File | Content | Analog |
|---|---|---|
| `tools/simi/simi_arm.h` | Header: `SimiObjHdrAR`/`TxEntryRecAR`/`SIMI_AR_MAGIC`/`TX_AR_*`, `TX_AR_MAX_REGS` (64), `TX_AR_SCRATCH_ARG_IDX` (7), `TX_AR_NAMEPOOL_ARG_IDX` (6), the `TX_AR_ERR_*` enum (incl. `TX_AR_ERR_FLOAT_UNSUPPORTED`), and the `simi_arm_translate(...)` contract | `simi_riscv.h` |
| `tools/simi/simi_arm.c` | The encoder, ~900 lines: `CodeBuf`/`e32`/`patch32`, the §4.2 frame layout, `emit_cmp` via `cmp`+`cset` (M1), `g_fixups` two-pass, capability tags + arg-tag mask, JMPR with bounds check + literal-`ldr` table base, float rejection up front | `simi_riscv.c` |
| `tools/simi/a64_exec.h` / `.c` | The decoder+executor, ~300 lines: `A64Cpu`, `a64_exec_run`, `a64_exec_set_hostfn`/`hostfn_addr`, sentinel constants (§3) | `rv64_exec.h`/`.c` |
| `tools/simi/simi_arm_verify.c` | Harness: `simi-arm-verify program.tmo entry_name expected_value`, mock catalog for RESOLVE/OBJSIZE/OBJTYPE, `x0` result check | `simi_riscv_verify.c` |
| `tools/simi/tests/run_arm_tests.sh` | Four-way regression over the shared corpus | `run_riscv_tests.sh` |

**Modified files:**

| File | Change |
|---|---|
| `tools/simi/Makefile` | `simi-arm-verify: simi_arm_verify.c simi_arm.c simi_arm.h a64_exec.c a64_exec.h` added to `all` and `clean`; `test-arm` alias |
| `Makefile` (root) | `simi-tools`/`simi-test` targets (lines ~481-486) gain the arm target; nothing in `X86_C_SRC`/`RV_C_SRC` moves until M2 |
| `docs/AeroSLS-SIMI-ISA-v0.1.md` | §7 roadmap row + §16 phase entry (likely Phase 15) once implemented, per §0 |
| `kernel/simi_arm.{c,h}` | **M2 only** — byte-identical kernel copies, the Phase 3/5 port convention; compile-checked under freestanding flags, never executed in-kernel (§1) |

**Interfaces that must not change — the point of the whole exercise:**

- The `.tmo` object format (`SimiObjHdr`/`TxEntryRec`/`TxNameRec`, the v0.3
  `num_names` field). Zero changes, on any target.
- The translate contract — `simi_arm_translate(obj_data, obj_size, out_buf,
  out_cap, entry_name, scratch_ptr, rt_resolve_fn, rt_objsize_fn, rt_objtype_fn,
  &out_len, &entry_off)`, byte-for-byte the `simi_riscv_translate` signature.
- The verifier contract — `a64_exec_run`/`a64_exec_set_hostfn`/
  `a64_exec_hostfn_addr` mirror `rv64_exec`'s, so `simi_arm_verify.c` is a
  mechanical clone of `simi_riscv_verify.c` with `struct A64Cpu` swapped in.

---

## 6. Milestones

Each gate is a measurement, not an opinion — the Step-6 plan's rule, adopted
here.

**M0 — the decode+execute subset, first executable vertical slice.**
The §3 instruction set (no NZCV, no MUL/DIV/MOD, no JMPR yet) plus a `simi_arm.c`
that emits exactly that subset — enough to translate and run
`straight_line_bench.simi` and `branch_cmp.simi` end to end.
- **Gate:** `make -C tools/simi simi-arm-verify` builds with zero warnings;
  both tests produce the **identical expected result four ways** — interpreter,
  x86-native (real CPU), RV64 (`rv64_exec`), A64 (`a64_exec`).
- Sizing: `a64_exec.c` ~300 lines, `simi_arm.c` ~500, verify harness ~150,
  Makefile wiring a dozen.

**M1 — full opcode parity with `simi_riscv.c`.**
MUL/DIV/MOD (`madd`/`msub`/`udiv`/`sdiv`), shifts, typed loads/stores, PTRADD,
LEA, CALL/RET, CMP's ten relations via `cmp`+`cset` (**NZCV enters the decoder
here**), JMPR with its bounds check, capability tags + argument-tag mask, the
aggregate ABI (a pure convention — zero codegen, as Phase 14 established).
- **Gate:** the entire shared corpus passes four-way with the same skip set as
  RV64; `float_ops.simi` is *rejected* by the translator with
  `TX_AR_ERR_FLOAT_UNSUPPORTED`, not silently mis-executed; the JMPR
  out-of-range test (`jmpr_oob`) traps rather than jumping wild, on all four
  engines — the §16 Phase 14 verification discipline, applied to A64.

**M2 — kernel copy + honest kernel-side gap note.**
Byte-identical `kernel/simi_arm.{c,h}` following the Phase 3/5 port convention
(shortened header pointing back at the host copy; re-diffed and confirmed
identical below it), compiled under freestanding flags with zero warnings.
- **Gate:** the port compiles clean under the kernel's freestanding flags, and
  the doc records the open gap in Phase 9e's exact shape: *executing* A64 in a
  kernel requires an arm64 kernel build or real ARM hardware/QEMU — not claimed,
  filed as honest unverified.

**M3 — optional, environment-dependent.** Real-hardware or `qemu-aarch64`
execution proof once a toolchain is available (the riscv64 constraint §12
documented has no reason to be permanent), or an arm64 kernel target if the
roadmap ever grows one.

**M0 and M1 are the project.** M2 is a port with a re-diff; M3 is
environment-dependent. The ordering rule from Step 6.4 applies equally here:
implement what a running test halts on first, not by walking a list.

---

## 7. Risks and honest unknowns

**Decoder-based evidence is the ceiling, not a stopgap.** Two independent bugs
that agree (encoder and decoder both wrong the same way) pass a four-way run.
§12 documents this exact residual risk for RV64; it is unchanged here. The
mitigation is the same as it was: the decoder is written *independently* of the
encoder, from the ARM ARM's encoding tables, and the repo's history — ModRM,
`reg_disp()`, `jalr`-bit-0 — says bit-packing mistakes are the expected bug
class, review-blind, execution-visible.

**The decoder is probably laxer than real hardware.** The §4.6 SP-alignment and
4-byte-branch-alignment rules are A64 architectural requirements; `a64_exec.c`
enforcing them costs decoder complexity that buys nothing for the host-side
proof. If the emitted code ever violates one, the four-way regression cannot
see it — only a real ARM core can. This is the *reverse* of the usual
"decoder catches what review misses" direction, and it is why M3 (real
execution) is a milestone, not a nicety. The fix is cheap discipline — every
stack delta a multiple of 16, every branch target aligned by construction — but
it must be stated, as it is here.

**NZCV is the one piece of real new semantics.** M1's `emit_cmp` gives the
decoder its first flag model. Getting borrow (C) and overflow (V) exactly right
for 64-bit operands is precisely the class of subtle, review-blind bug this
project's own history is full of; `branch_cmp.simi`'s ten relations plus
signed/unsigned pairs are the test surface, and the four-way result is the
arbiter. Not a blocker — bounded, flaggable work.

**No A64 toolchain/QEMU confirmation yet.** The riscv64 constraint (§12) is
documented fact for this sandbox; the ARM equivalent should be re-checked at
implementation time. Either way the decoder path is needed for host-side
testing, so M0/M1 do not depend on the answer — M3 does.

**One divergence from the RV64 port to keep honest.** §4.2's `xBASE` register is
new state the RV64 file doesn't have (RV64 folds the frame base into `s0`). The
trampoline must initialize it, and the prologue must set it *before* any slot
access. It is the one place a mechanical RV64→A64 port would silently differ.

**If "ARM guest" is the actual ask, this is the wrong document.** Re-read §0's
"what this is NOT": an ARM *guest emulator* is QEMU's `target/arm` decoder
behind an `SLS_ARM_FRONTEND` build flag, with an execution loop and helper-stub
story in the image of the Step-6 x86 plan — a different file set, a different
milestone shape, and a much larger surface (the 765-helper lesson of Step 6.4
scales to whatever ARM's `helper.h` expands to). This plan is the SIMI backend;
if the goal is running ARM binaries on this kernel, say so and the plan gets
re-scoped, not stretched.

---

## 8. M0 as-built addendum

M0 shipped as planned with two deliberate deviations from the plan above,
one encoding bug caught in review that the verification net had copied,
and one infrastructure fix. Recorded here so the phase record stays honest;
all gates are the measurements below, not opinions.

### 8.1 What shipped

- `tools/simi/a64_exec.h` / `a64_exec.c` — the §3 decoder+executor, extended
  with the full NZCV model (see 8.2) and the `rx()`/`set_x()` XZR/SP
  split required by A64's dual meaning of register 31.
- `tools/simi/simi_arm.h` / `simi_arm.c` — the translator, zero-shared-header
  policy intact (`SimiObjHdrAR`, `TX_AR_*`, `TX_AR_ERR_*`).
- `tools/simi/simi_arm_verify.c` + `tests/run_arm_tests.sh` — the §2 harness.
- Two extra verification files beyond §5's list, added because the four-way
  gate alone cannot catch an encoder+decoder agreeing on a wrong constant:
  `tools/simi/a64_dump.c` (translate a `.tmo`, print every emitted word) and
  `tools/simi/a64_enc_check.py` (a from-scratch Python re-encoder of the same
  bit layouts, checked against the dump). Wired as `make a64-enc-check`.
- `tools/simi/Makefile`: targets `simi-arm-verify`, `a64_dump`, `a64-enc-check`.
- `tools/simi/.gitignore`: fixed the `timi-`→`simi-` typo that had left the
  five Phase 1/3/5 pre-built binaries tracked against the file's stated
  intent, and added `simi-arm-verify`/`a64_dump`. The five binaries are
  still tracked (gitignore does not untrack); whether to `git rm --cached`
  them is a maintainer call, not part of M0.

### 8.2 Deviations from the plan

1. **SP-relative frame, not the §4.2 xBASE trick.** The plan's `sub xBASE,
   xFP, #592` sketch needs a dedicated register that every callee's prologue
   necessarily re-bases and no epilogue restores (A64's x28 is callee-
   clobbered). The build instead addresses the frame from `sp` directly:
   slot i at `sp + (568-8i)` (imm12 `71-i`), tag i at `sp + i`, with `sp ==
   entry-592` throughout a body — byte-identical guest addresses to RV64's
   layout, one less register, no recompute-after-every-call, no audit
   burden. The one wrinkle is at CALL sites, which decrement `sp` by 80 to
   open the outgoing-arg area: the slot/tag loads there are re-anchored to
   the decremented `sp` (imm12 `81-i` / `80+i`). RV64 has no such wrinkle
   because its slots are s0-relative and s0 never moves — and the missing
   re-anchor was the second real bug the execution gate caught (8.4).
2. **CMP + NZCV entered M0 scope.** The §4 note that "M0 ships flag-free"
   was amended at build time: `branch_cmp.simi` and `loop_sum.simi` both
   need CMP, so M0 models the full NZCV flags. The synthesis is the
   compiler idiom `cmp xN, xM` (`subs xzr, xN, xM`) + `cset xN, <cond>`
   for all 10 relations. CBZ/CBNZ are still used for BC (SIMI's
   test-a-register-and-jump fits A64's compare-and-branch family exactly).
3. **Constants without a literal pool.** movz/movk materialize any 64-bit
   constant in ≤4 fixed words, deleting RV64's g_literals/auipc+ld machinery
   wholesale; only JMPR's table-base offset (not known at emit time) gets a
   fixed 4-word placeholder patched in a tiny final pass.
4. **enc_li_disp has no `addi x0, disp` shortcut.** A64's add/sub-immediate
   rn=31 means SP, not XZR, so small displacements use `movz` (correct
   1-instruction path) and fall back to li64 otherwise.
5. **Trampoline zeroes r0..r5, sets r6=namepool_ptr, r7=scratch_ptr**, and
   zeroes the argument-tag mask, then `bl`s entry — the `bl` clobbers x30,
   so the incoming x30 (the verifier's sentinel) is explicitly saved and
   restored around it, mirroring RV64's trampoline around its `jal`.

### 8.3 Gate results

- `make a64-enc-check`: 9/9 sections OK across `straight_line_bench`,
  `loop_sum`, `extra_ops` (prologue + trampoline skeleton + body words all
  independently re-encoded from the Python bit layouts).
- ARM suite: **15 passed, 0 failed, 4 skipped** (skips identical to RV64's:
  `mem_ops` address-0 convention, `float_ops` scoped out, `jmpr_oob` and
  `cap_forge_debug` have no expected result).
- Four-way parity on the same unmodified `.tmo` files: interpreter 17/17,
  x86-64 native 16/0/3, RV64 15/0/4, ARM 15/0/4.
- Build is warning-clean for every M0 file (`-Wall -Wextra`; the only
  warnings in `make all` are the pre-existing `simi_isa.h` unused-variable
  ones in the Phase 1 tools).

### 8.4 Bugs the gates caught

- **XZR/SP dual meaning (decoder):** CSEL-family, ORN, NEG and MUL reads of
  register 31 initially returned SP's value; a trace of `loop_sum` exposed
  the `cset` writing `0x4FD40` instead of 0. Data-processing families now
  read 31 as XZR; only add/sub-immediate and load/store address regs read
  it as SP. STR with rt=31 likewise stores zero.
- **CALL-site frame re-anchoring (encoder):** after `sub sp, sp, #80`, the
  slot/tag loads were 80 bytes too low (SP-relative frame, RV64-immune
  bug). Fixed to `81-i` / `80+i`.
- **MVN encoding (encoder+Python net):** `enc_orn` emitted `ORN xd, x0, xm`
  (Rn=0) instead of `ORN xd, xzr, xm` (Rn=31). It produced the right
  answer only because the executor kept host x0 at zero — on real silicon
  x0 is the caller's register, and this translator's hostfn ABI uses x0 for
  args/results. The Python cross-check had copied the same error, so it was
  no net at all for this word; both were corrected to `0xAA200000 | (rm<<16)
  | (31<<5) | rd`, and both file headers' "canonical" `mvn x0,x1 ==
  0xAA200020` reference (which decodes to `orn x0, x1, x0`) was corrected
  to `0xAA213E0`.
- **`is_li9` mask (Python net):** `0xFFFF001F` kept imm16 high bits, so
  `movk x9, #imm, lsl #16` with nonzero imm failed the match; `0xFF80001F`
  (fixed prefix bits 31:23 + rd) handles all hw values including the
  3-movk namepool li64.

### 8.5 Honest limits that stand after M0

- No real ARM execution unit exists in this sandbox — M0 evidence is
  decoder-based, the same §12 caveat the RV64 phase already carried. The
  XZR/SP and Rn=31 class of bug is exactly what M3's real execution (or a
  cross-check against an aarch64 assembler's output) will finally settle.
- `jmpr_oob`'s UDF-fault path is skipped by the runner's "no expected
  result" rule, same as RV64; the JMPR bounds check is code-reviewed but
  not asserted at runtime.
- Relations EQ/LT/GT are exercised; NE/LE/GE and the unsigned HS/LO/HI/LS
  codes are a direct cond-code lookup validated only by the mechanism, not
  by the corpus.
- The verify harness's hostfn stubs are plain C functions behind sentinel
  addresses; the real A64 ABI result convention (x0) is exercised by
  `cap_call_ret` through the trampoline but unproven against a real runtime.

## 9. M1 as-built addendum (register allocator + size gate)

M1 is the first performance milestone: M0's naive load-operate-store
codegen — every SIMI instruction fetched its operands from the frame and
stored its result back — became a tiny three-register allocator that
keeps arithmetic chains resident across instruction boundaries. The
surface area is unchanged (all 31 opcodes, all 10 CMP relations, JMPR
bounds check); only the shape of the emitted body changed.

### 9.1 What shipped

- **`simi_arm.c`** — an x9/x10/x11 cache with a compile-time directory
  (`g_cache_guest[]`), round-robin eviction, per-instruction
  reservation state (`g_resv_*`), and cache-aware codegen for ALU,
  LOAD/STORE, MOV, LOADI64, LEA/PTRADD, CMP (subs+cset, cached result),
  and the call-site return value. `get_operand` takes an `rd_live` hint
  (rd aliases a source operand) so a fetch that clobbers a REUSED result
  register spills the old value only when it is live — a dead
  destination's spill is skipped (9.2). Flush points are exactly the
  guest-observing boundaries: BR/BC/CALL/RET/JMPR/hostfn/ENTER.
- **`tests/rd_star.simi`** — the allocator edge-case test (`ADD rd,ra,rd`
  with rd resident): fails without the reserved-slot downgrade fix
  (10 instead of 42) and runs on all four engines.
- **`tests/dead_reuse.simi`** — the complement: `ADD rd,ra,rb` with rd
  resident but not an operand, ten forced occurrences. Reverting the
  `rd_live` skip grows the test by exactly 40 bytes (1152, back to M0's
  size); it pins the optimization the gate measures.
- **`tests/size_gate_arm.sh`** + Makefile `size-gate-arm` — the
  measurement gate.
- **`a64_enc_check.py`** — rewritten from M0's exact-body expectations
  to allocation-agnostic checks (see 9.3).

### 9.2 Design decisions worth recording

- **The cache must be empty at every block head.** A branch target that
  lands mid-chain would read garbage registers, so a pre-pass marks all
  branch/call target pcs (`g_pc_target[]`) and the main loop flushes the
  cache at the end of any instruction whose successor is a target — the
  flush word becomes part of the block head, and branch fixups (resolved
  after the loop) land on it naturally.
- **Result-in-place vs phantom.** When rd is already resident its slot is
  reused and the result computed in place; a *fresh* claim's register is
  garbage until the result lands, so operand reads of rd go to memory
  while the claim is a "phantom". When an operand fetch must clobber a
  reused result register (e.g. `ADD rd, ra, rd`), the old value is
  spilled and the reservation downgraded to a phantom — this is the
  rd_star fix.
- **JMPR disables the cache entirely** (`g_alloc=0`, naive M0 path). A
  JMPR can land on any pc, so no compile-time cache discipline survives
  it; jmpr_basic/jmpr_oob therefore measure the allocator's floor, not
  its peak, and their M0 sizes are unchanged.
- **The `rd_live` hint kills the dead-value spill.** When an operand
  fetch must clobber a REUSED result register, the old value is spilled
  only if rd is a source operand of the instruction (`rd_live`, e.g.
  `ADD rd, ra, rd` — a later fetch of rd must reload it from memory);
  if rd is not an operand (`ADD rd, ra, rb` with rd resident) the old
  value is dead and the spill is skipped — the result overwrites the
  register in place. The reservation downgrades to a phantom either
  way, so the register is garbage until the result lands. The cost is
  one boolean per call site; the win is one store per dead-resident
  reuse, measured by dead_reuse.simi (40 bytes) and add.simi (4).

### 9.3 Gate results (measured)

Total emitted bytes across the 17-program parity set — the 15 M0-corpus
programs plus the two M1 allocator edge-case tests (rd_star, dead_reuse),
whose baselines were measured by running the committed M0 translator on
the new .simi files: **M0 25020 → M1 24684, 336 saved** (≈1.3%). The
distribution is the honest picture:

- Arithmetic-heavy programs win big: aggregate_abi −152, dead_reuse −64
  (40 of it the rd_live skips — five first-fetch and five second-fetch
  shapes — the rest LOADI residency), mem_ops_native −36, rd_star −24.
- `straight_line_bench` — the register-pressure worst case (six live
  values vs a 3-register pool, so everything spills) — shrinks only 4
  bytes. This is the gate's point: the allocator is a measured ceiling,
  not a promise of a specific win.
- Branch-heavy programs (branch_cmp −8, cap_forge −4) win little; the
  cache is empty at every block head by design.
- `size-gate-arm` fails any test that regresses past its M0 baseline
  (hardcoded, with a documented re-measure procedure for corpus edits).

`a64_enc_check.py` changed what it can assert: M1's body is
allocation-dependent (register choice varies by eviction order), so exact
body words would just restate the allocator. It now requires (1) every
emitted body word to decode to a legal A64 class via the independent
Python bit layout, and (2) every movz/movk chain to carry the exact
LOADI values — prologue and trampoline keep exact-word checks. The net
is deliberately register-blind: a wrong operand register still decodes
as legal, so allocation correctness rests on the four-way execution
parity, not this net.

### 9.4 Bugs the gates caught

Three real allocator bugs, all fixed before commit:

1. **`g_pc_target` wipe** — the pre-pass zeroed each entry as it walked,
   wiping the marks earlier branch instructions had set; every block-head
   flush silently vanished and branch fixups landed on stale offsets
   (branch_cmp corrupting a live slot). Zeroed once before the walk.
2. **Stale `g_resv_slot` across instructions** — a LOADI left its
   reservation set; the next STORE's base-address fetch then skipped the
   spill of the "reserved" register and clobbered a resident value
   (aggregate_abi, mem_ops_native). Reservation state is reset at the
   top of every translate-loop iteration.
3. **Reserved-slot reuse with rd as operand** — `ADD rd, ra, rd` with rd
   resident read garbage into the second operand because the first fetch
   had claimed rd's register. The downgrade-to-phantom in `get_operand`
   is the fix; rd_star.simi proves it fails without (10 vs 42).

### 9.5 Honest limits that stand after M1

- The §8.5 caveats (decoder-based evidence, unproven ABI, unasserted
  JMPR fault path) stand unchanged — M1 added no real execution.
- Register correctness is covered only by execution parity, not the
  encoder net (9.3); the corpus is 17 programs, so an allocation bug
  that the corpus never exercises could still hide.
- The block-head flush is a structural cost (every branch target pays
  an empty cache), bounded and measured by the gate; the dead-value
  spill is gone but the corpus now includes dead_reuse.simi so a
  regression of the rd_live hint is caught by size, not just missed
  bytes.
- JMPR programs get no allocator benefit at all — the naive path is the
  price of a runtime-indirect branch, and no JMPR-bearing test exists
  that would measure a smarter middle ground.

## 10. M2 as-built addendum (constant-index JMPR folding)

M1 gave every JMPR-bearing program the worst of both worlds: the cache
was disabled for the whole program (a runtime JMPR can land on any pc,
so every pc would have to be a block head — no cache at all), and the
JMPR itself paid the full runtime machinery (4-word table-base constant,
table lookup, bounds check, UDF path). M2 folds the common case —
`load index; dispatch`, where the index is a compile-time constant —
into a plain direct branch, and only a genuinely runtime index keeps
the old dynamic path.

### 10.1 What shipped

- **`simi_arm.c`** — a per-register compile-time constant map
  (`g_const_val`/`g_const_known`) in the pre-pass, a JMPR fold target
  per pc (`g_jmpr_fold`), and OP_JMPR moved from `emit_instr` into
  `translate()`'s main loop where it can see the pc: folded →
  `cache_flush` + `op_b(target)`; else the original dynamic
  table+bounds-check path (word-identical, moved verbatim).
- **`tests/jmpr_mid.simi`** — a constant-index dispatch after a real
  arithmetic chain: the middle-ground measurement (the cache stays
  live AND the JMPR folds).
- **`tests/jmpr_dyn.simi`** — a runtime-computed index: pins the
  dynamic SUCCESS path, which jmpr_basic used to cover before it
  folded (jmpr_oob pins only the fault path).
- **`tests/jmpr_join.simi`** — the fold-soundness pin: an index set to
  different constants on two paths joining at a branch target, which
  the analysis must NOT fold.

### 10.2 Design decisions worth recording

- **The fold is only sound if the index is provably constant on every
  path that reaches the JMPR.** The analysis resets the constant map at
  every block head (branch/call target), so a chain between heads is
  executed identically on every path that enters it — a constant
  attributed there holds on every arrival. The fold targets are
  themselves merged into the block-head set, so their arrivals get
  rule-1 flushes too — a fold target landing inside another JMPR's
  chain (before its constant source) retroactively splits that chain,
  which is why the analysis runs to a fixpoint: the fold set is
  monotone decreasing and the added reset un-folds the affected JMPR.
- **g_alloc is 1 only when every JMPR folds.** One runtime-indexed
  JMPR still forces the whole program back to the naive path — its
  targets are any pc, and any pc reachable without a compile-time
  discipline makes every pc a potential block head. Folded JMPRs are
  plain branches and fold regardless of g_alloc.
- **The folded JMPR needs no bounds check** — the range `[0,
  num_instr)` is proved at fold time; the dynamic path's UDF #0 CFI
  machinery exists only for non-folded JMPRs.

### 10.3 Gate results (measured)

Total emitted bytes across the 20-program parity set: **M0 28500 → M1
27892, 608 saved** (≈2.1%). The JMPR row is the honest picture:

- jmpr_basic 1088 → 984 (−104): the fold + a live cache.
- jmpr_mid 1200 → 1032 (−168): dispatch after an arithmetic chain.
- jmpr_dyn 1136 → 1136 (0) and jmpr_join 1144 → 1144 (0): runtime or
  join-split indices keep the dynamic path, byte-identical to M0's
  naive codegen — the floor, exactly as designed.
- Four-way parity: interp 22/0, x86 21/0/3, RV64 20/0/4, ARM 20/0/4.
  jmpr_oob still faults (UDF, verified by hand: rc=1, "execution
  error"). jmpr_join's teeth: reverting the block-head constant reset
  makes it return 1 instead of 222.

### 10.5 M2.1/M2.2/M2.3 amendment — folding arithmetic constants (as built)

The M2 fold accepted only index values the analysis could see directly
(LOADI/LOADI64 constants, propagated through MOV). M2.1 extends the
constant map to fold ADD/SUB; M2.2 adds MUL; M2.3 completes the ALU
family with AND/OR/XOR and SHL/SHR/SAR — the index is now COMPUTED at
translate time, e.g. `load; add; sub; dispatch`, `load; mul; dispatch`
or `load; and; shl; xor; dispatch`, which is how real dispatchers shape
their index. On the emitted code side nothing changed: the ALU is a
plain 64-bit op for every SIMI type (the translator never truncates to
the declared width), `materialize_imm` sign-extends imm28 exactly as
the analysis does, MUL emits `enc_madd(rh, X_T0, rhs, 31)` — `rd =
rn*rm + xzr`, a plain multiply that never faults — and the shifts mask
their AMOUNT mod 64 (`& 0x3F`) exactly like A64's `lslv/lsrv/asrv`
hardware, the interpreter's `fetch_operand_b & 0x3F`, and RV64's
`v2 & 0x3F`, with SAR arithmetic (sign-filling) like the interpreter's
`(int64)>>` — so the 64-bit wrap of `a+b`/`a-b`/`a*b`/`a&b`/`a|b`/
`a^b`/`a<<amt`/`a>>amt` here is bit-identical to runtime. The fold is
deliberately limited to these nine: DIV/MOD would change behavior on a
translate-time division by zero, and NOT (a foldable `~a`, plain
64-bit) is left out as a conservative omission — unary, never a
dispatch index in practice.

- **`tests/jmpr_calc.simi`** — reworked to compute its index through all
  four fold shapes (ADD reg+reg, SUB reg+imm, SUB reg+reg, ADD reg+imm,
  with ADD reg+reg appearing twice): r1 = 5+3−2+3−2+6 = 13, folding to
  pc 13. The M0 baseline re-measured (the program grew by one LOADI):
  **1288 → 1076, 212 saved**; disabling the fold grows it back to
  exactly 1288 while the dynamic path still returns 222.
- **`tests/jmpr_mix.simi`** — the mixed case the corpus lacked: ONE
  runtime JMPR (index = 5^2, XOR deliberately not in the fold set) and
  ONE constant JMPR in the same program. The runtime index forces
  g_alloc=0 — the whole program runs naive codegen — yet the constant
  index still folds to a direct branch. This pins the folded-
  branch-under-naive-codegen interaction across all four engines.
  **1300 → 1252, 48 saved** (the fold under naive codegen; everything
  else byte-identical to M0).
- **`tests/jmpr_dyn.simi`** — its index arithmetic switched ADD → XOR.
  With M2.1's fold, `ADD r1, r1, r0` with a constant r0 would fold and
  jmpr_dyn would silently stop exercising the dynamic path; XOR is not
  in the fold set, so the test stays a genuine runtime index.

### 10.6 M2.1 gate results (measured)

Total emitted bytes across the 22-program parity set: **M0 31088 → M1
30220, 868 saved** (≈2.8%). The M2.1 rows on top of M2's 608:

- jmpr_calc 1288 → 1076 (−212): the computed index folds.
- jmpr_mix 1300 → 1252 (−48): folded branch under naive codegen.
- jmpr_dyn 1136 → 1136 (0): still a runtime index, still the floor.
- Four-way parity: interp 24/0, x86 23/0/3, RV64 22/0/4, ARM 22/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: disabling the ADD/SUB fold
  grows jmpr_calc back to exactly its M0 1288 while remaining correct
  via the dynamic path.

### 10.7 M2.1 honest limits

- The fold's in-range check is an unsigned comparison
  (`g_const_val[ra] < num_instr`), which also correctly refuses
  sign-extended negative constants (they land ≥ 2^63) — sound, but no
  test currently computes a negative index to pin the refusal.
- The only ALU ops left unfoldable are DIV/MOD (translate-time
  division by zero would diverge from the runtime fault) and NOT (a
  foldable `~a` left out as a conservative omission — unary, never a
  dispatch index in practice).

### 10.8 Honest limits that stand after M2

- The general case — a genuinely runtime index — still disables the
  cache for the whole program, and always will: the JMPR table can
  reach any pc, and no compile-time discipline survives that.
- The fixpoint's retroactive-split case (a fold target un-folding
  another JMPR) is covered by the reset argument and the convergence
  proof but has no dedicated test — constructing it needs two JMPRs
  tangled with branches in a way the corpus does not contain. Noted
  in the code; a future JMPR-heavy stress test may close it.
- Translate time grew by one O(n) constant-analysis scan per program
  (plus fixpoint iterations); negligible at these sizes, unbounded
  only by the JMPR count in the worst case.
- The §8.5/§9.5 caveats stand: decoder-based evidence, unproven ABI,
  and the encoder net's register-blindness all still apply.

### 10.9 M2.2 amendment — folding MUL (as built)

M2.1 folded ADD/SUB; M2.2 adds MUL to the same fold branch. The sound-
ness argument carries over verbatim: the ALU emits `enc_madd(rh, X_T0,
rhs, 31)` — a plain 64-bit multiply with the XZR addend, never faults,
type-agnostic, no truncation — and the imm28 sign-extension matches
`materialize_imm`, so `a*b` with 64-bit wrap is bit-identical to run-
time. MUL joins the fold in the same FLAG_IMM/register-form handling;
DIV/MOD stay excluded (translate-time division by zero would diverge
from the runtime fault).

- **`tests/jmpr_calc_mul.simi`** — index computed through a MULTIPLY
  chain: MUL reg+reg (3*4 = 12), MUL reg+imm (12*2 = 24), SUB reg+imm
  (24−11 = 13) — folds to pc 13. The M0 baseline measured against the
  committed M0 translator: **1272 → 1064, 208 saved**; disabling the
  MUL fold grows it back to exactly 1272 while the dynamic path still
  returns 222 (the teeth check).

### 10.10 M2.2 gate results (measured)

Total emitted bytes across the 23-program parity set: **M0 32360 → M1
31284, 1076 saved** (≈3.3%). The M2.2 row on top of M2.1's 868:

- jmpr_calc_mul 1272 → 1064 (−208): the computed multiply index folds.
- Four-way parity: interp 25/0, x86 24/0/3, RV64 23/0/4, ARM 23/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: disabling the MUL fold
  grows jmpr_calc_mul back to exactly its M0 1272 while remaining
  correct via the dynamic path.

### 10.11 M2.3 amendment — folding the bitwise + shift ALU family (as built)

M2.1/M2.2 folded ADD/SUB/MUL; M2.3 completes the ALU family in the
same fold branch: AND/OR/XOR (plain 64-bit, no masking, no fault) and
SHL/SHR/SAR. The shifts are the subtle part — the AMOUNT is masked mod
64 (`& 0x3F`) identically in the fold, the interpreter
(`fetch_operand_b & 0x3F`), A64 hardware (`lslv/lsrv/asrv`), and RV64
(`v2 & 0x3F`), so an amount of 65 shifts by 1; SAR is arithmetic
(sign-filling) via `(uint64_t)((int64_t)a >> amt)`, the interpreter's
exact expression. DIV/MOD stay excluded (translate-time division by
zero would diverge); NOT is left out as a conservative omission.

- **`tests/jmpr_calc_bit.simi`** — index computed through the new
  shapes with real teeth on both subtleties: r1 =
  0x8000000000000000|5 (sign bit set via the LOADI64 literal
  `#-9223372036854775808` — the assembler's strtoll clamps a positive
  2^63 to LLONG_MAX, so the negative literal is required to reach
  0x8000000000000000); SAR #2 = 0xE000000000000001 (a logical SAR
  would give 0x2000000000000001 — the fold MUST be arithmetic); SHR
  #61 = 7; SHL by a REGISTER amount 65 (masked to 1) = 14; AND 5 = 4;
  XOR #8 = 12; ADD #4 = 16 — folds to pc 16. A logical-SAR fold bug
  lands on pc 12 (55), distinct and caught by parity; an unmasked
  amount un-folds instead (dynamic path, correct but a size
  regression, caught by the teeth check). The M0 baseline measured
  against the committed M0 translator: **1384 → 1136, 248 saved**;
  disabling the fold grows it back to exactly 1384 while the dynamic
  path still returns 222.

### 10.12 M2.3 gate results (measured)

Total emitted bytes across the 24-program parity set: **M0 33744 → M1
32140, 1604 saved** (≈4.8%). The M2.3 row on top of M2.2's 1076:

- jmpr_calc_bit 1384 → 1136 (−248): the computed bitwise+shift index
  folds.
- Four-way parity: interp 26/0, x86 25/0/3, RV64 24/0/4, ARM 24/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: disabling the fold grows
  jmpr_calc_bit back to exactly its M0 1384 while remaining correct
  via the dynamic path.

### 10.13 M2.4 amendment — source-resident reservation (as built)

The M1 allocator's cache_reserve picked its eviction victim by
round-robin alone, so in a chain that re-reads the same source
(`ADD r4, r1, r2; ADD r5, r1, r2`) the victim could be a source
register: the instruction spilled it and then reloaded it from memory
when get_operand fetched it a few words later — a wasted store+load on
exactly the pattern that should benefit from residency. M2.4 passes the
instruction's source registers (ra, and rb when the operand is a
register) into cache_reserve, which now prefers a victim slot that
holds neither source. The preference is a two-pass scan: any EMPTY slot
first (a free slot costs no spill — empties occur transiently when
clobber_scratch or a prior spill leaves -1), then a non-source
occupant. Soundness: with rd not resident, at most 2 of the 3 slots
hold the ≤2 distinct sources, so a non-source slot always exists; the
plain round-robin victim remains only as a defensive fallback (the
cursor still advances every call, so nothing starves). All 11 call
sites pass their real source sets (CMP is always register form; the
call site and LOADI/LOADI64 pass none).

- **`tests/src_resident.simi`** — five ADDs re-reading r1/r2 as
  sources, results accumulated into r0 (expected 35). The M0 baseline
  measured against the committed M0 translator: **1120 → 1080, 40
  saved**; reverting to the blind round-robin grows it to 1108 (28
  over the hint) while staying correct. The hint also shaved 8 more
  off aggregate_abi and a few bytes elsewhere — the preference
  compounds across every arithmetic chain, not just this test.

### 10.14 M2.4 gate results (measured)

Total emitted bytes across the 25-program parity set: **M0 34864 → M1
33196, 1668 saved** (≈4.8%). The M2.4 rows on top of M2.3's 1604:

- src_resident 1120 → 1080 (−40): the source-reuse chain keeps its
  sources resident.
- aggregate_abi 4440 → 4432 (−8 additional): the preference compounds
  on an existing arithmetic-heavy program.
- Four-way parity: interp 27/0, x86 26/0/3, RV64 25/0/4, ARM 25/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: reverting the source
  preference grows src_resident to exactly 1108 while remaining
  correct.

### 10.15 M2.4 honest limits

- The reservation preference does not address the FETCH-ORDER clobber:
  when a source lives in the other operand's fetch target slot (e.g.
  ra resident in slot 1 but fetched into X_T0), get_operand still
  spills the co-resident source and reloads it. **Closed by M2.5
  (§10.16)**: fetch-target selection is now aware of both operands,
  compounding 264 bytes beyond M2.4 (1668 → 1932).
- The defensive round-robin fallback is argued unreachable (3 slots,
  ≤2 sources, rd not resident) and untested by construction.

### 10.16 M2.5 amendment — fetch-order clobber fixed (as built)

M2.4's first honest limit is now closed: get_operand's fetch-target
selection is aware of both operands. The default host assignment
(operand1→x9, operand2→x10) is overridden per instruction by two new
helpers:

- `cache_fetch_hosts(g1, g2, &h1, &h2)` — used by every two-register
  form (ALU, DIV/MOD, CMP, PTRADD, STORE's base+value): when g1 is
  resident at slot 1 or g2 at slot 0 (the CROSSED shape — e.g.
  `ADD r5, r2, r1` after `ADD r4, r1, r2` leaves r1@slot0, r2@slot1),
  the targets swap so each operand fetches into its own resident slot.
  Otherwise the first fetch spills the second operand, then the second
  fetch spills the first and reloads it — two spills and a reload for
  operands that were both resident. The swap is never larger than the
  default (checked per residency combination) and usually 2-4 words
  smaller. Naive mode keeps x9/x10 so M0 codegen stays byte-identical
  (gate baselines untouched).
- `cache_single_host(g)` — for the one-register-operand + immediate
  forms (ALU/DIV/MOD imm, LEA, PTRADD imm, LOAD): the operand takes x10
  when resident at slot 1, so the immediate's materialization (into x9)
  never spills it. `materialize_imm` now takes its target host
  explicitly; the M1-era X_T2 fallback for g_resv_slot==1 is gone,
  argued safe: the one live-reuse collision (rd==ra reused at slot 1)
  always triggers the slot-1 swap, so the immediate never overwrites a
  reused result register still holding rd's live old value.
- `emit_cmp` gained rn/rm host parameters, so CMP's `subs xzr, rn, rm`
  keeps the relation's operand order under the swap.

- **`tests/fetch_cross.simi`** — a chain that alternates operand order
  (`ADD rX, r2, r1` after `ADD r3, r1, r2`) so every instruction is
  crossed (ra@slot1, rb@slot0), plus crossed CMP and PTRADD variants
  (expected 96). M0 baseline measured against the committed M0
  translator: **1216 → 1112, 104 saved**; the teeth check (reverting
  the swap) grows it to 1160 — the M2.4 state — while remaining
  correct. The swap compounds corpus-wide: src_resident 1080 → 1056,
  aggregate_abi 4432 → 4420, mem_ops_native −8 more.

### 10.17 M2.5 gate results (measured)

Total emitted bytes across the 26-program parity set: **M0 36080 → M1
34148, 1932 saved** (≈5.4%). The M2.5 rows on top of M2.4's 1668:

- fetch_cross 1216 → 1112 (−104): the crossed chain fetches each
  operand into its own resident slot (1-2 words per instruction
  instead of 5-6).
- src_resident 1080 → 1056 (−24 additional), aggregate_abi 4432 →
  4420 (−12 additional): the swap compounds on existing
  arithmetic-heavy programs.
- Four-way parity: interp 28/0, x86 27/0/3, RV64 26/0/4, ARM 26/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: reverting the swap grows
  fetch_cross to exactly 1160 while remaining correct.

### 10.18 M2.6 amendment — small add/sub immediates fold (as built)

The ALU immediate-form codegen now folds small non-negative ADD/SUB
immediates into A64's 12-bit add-imm/sub-imm encodings — one word
instead of the movz(+movk)+add_shift materialization, exactly what
LEA/PTRADD/LOAD already do for their displacements. The fold fires in
cache mode (g_alloc) after the first operand is fetched, when the
sign-extended imm28 is in [0, 4095]; the instruction is then consumed
by a single `add/sub xd, xn, #imm` + store_result (the `break` exits
the outer switch, skipping the inner ALU switch and the common
store_result — no double or missed store). Gated on g_alloc so the
naive (JMPR) path stays byte-identical to M0, preserving the gate's
jmpr baselines and its "0 saved, the honest floor" rows.

Deliberate non-folds, stated in the code comment: values past imm12
(> 4095) and negative immediates keep the materialized path; A64's
add-imm also has an `imm12 << 12` shifted form (multiples of 4096 up
to 0xFFFFFF) recorded as a known next extension; AND/OR/XOR immediates
are bitmask encodings, not plain 12-bit, so the rest of the ALU family
is untouched.

- **`tests/alu_imm.simi`** — an immediate-heavy chain: five folded rows
  (5, 3, 100, 55, and the exact 4095 boundary) then three rows that
  must NOT fold (4096 just past imm12, a negative #-5, and a
  register-form ADD) so parity exercises both the fold and the
  materialized fallback (expected 8268). **M2.7 closed the 4096 and
  #-5 rows** (see §10.20): 4096 folds via the sh=1 shifted form and
  #-5 via the sign inversion, so only the register-form row remains a
  non-fold here. M0 baseline measured against the committed M0
  translator: **1132 → 1080, 52 saved** at M2.6; the M2.7 fold brings
  it to 1060 (72 below, teeth 1100 = the full materialize state).

### 10.19 M2.6 gate results (measured)

Total emitted bytes across the 27-program parity set: **M0 37212 → M1
35208, 2004 saved** (≈5.4%). The M2.6 rows on top of M2.5's 1932:

- alu_imm 1132 → 1080 (−52): the folded immediate rows are one word
  each instead of two.
- jmpr_calc 1076 → 1064 (−12), jmpr_calc_mul 1064 → 1060 (−4),
  jmpr_calc_bit 1136 → 1132 (−4): the three computed-index chains use
  SUB #imm under the cache (their JMPRs fold, so g_alloc=1), so the
  fold compounds on the M2.1-M2.3 machinery — their totals vs M0 are
  now 224/212/252 below (up from 212/208/248).
- Row-by-row M2.5-vs-M2.6 comparison: no test grew; these four are the
  only changes.
- Four-way parity: interp 29/0, x86 28/0/3, RV64 27/0/4, ARM 27/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: disabling the fold grows
  alu_imm to exactly 1100 while remaining correct.

### 10.20 M2.7 amendment — negative and shifted immediates fold (as built)

M2.6's two remaining immediate families are now folded, closing the
"known next extension" note in §10.18. The fold's direction flips with
the sign — `ADD #-v` emits `sub xd, xn, #|v|` and `SUB #-v` emits
`add xd, xn, #|v|` (bit-identical to the 64-bit wrap materialization:
ra + (2^64 − |v|) ≡ ra − |v|) — and the magnitude folds into either
form of A64's add/sub immediate: the plain imm12 (sh=0, 0..4095) or
the shifted imm12<<12 (sh=1, magnitudes that are multiples of 4096 up
to 0xFFFFFF, imm12 = mag>>12 ≤ 4095). New encoders
enc_add_imm_sh/enc_sub_imm_sh set bit 22; a64_exec.c already applied
sh on decode (its comment is updated), and a64_enc_check.py needs no
change (the body check is class-based and its 0xFF000000 mask ignores
bit 22 — a note records that). The fold stays gated on g_alloc, so the
naive JMPR path remains byte-identical to M0.

- **`tests/alu_imm_ext.simi`** — the negative and shifted rows:
  ADD #-7 → sub #7, SUB #-12 → add #12, ADD #4096 → add #1 lsl 12,
  ADD #16773120 → add #4095 lsl 12 (the exact shifted max, 0xFFF000 —
  a first draft used 16777215 = 0xFFFFFF, which is NOT a multiple of
  4096 and silently didn't fold; caught by dumping the emitted words),
  SUB #8192 → sub #2 lsl 12, and ADD #-4096 → sub #1 lsl 12 (negative
  + shifted in one row). Two rows must still materialize: 16777216
  (0x1000000, past 0xFFFFFF) and #-200000 (|v| = 0x30D40, not a
  multiple of 4096), plus a register-form ADD (expected 33742162). M0
  baseline measured against the committed M0 translator: **1196 →
  1096, 100 saved**; disabling the fold grows it back to exactly 1160.
  alu_imm.simi's instructions are unchanged (its M0 baseline 1132
  stays valid); only its 4096 and #-5 rows' comments moved from "no
  fold" to "fold" — its size drops 1080 → 1060.

### 10.21 M2.7 gate results (measured)

Total emitted bytes across the 28-program parity set: **M0 38408 → M1
36284, 2124 saved** (≈5.4%). The M2.7 rows on top of M2.6's 2004:

- alu_imm_ext 1196 → 1096 (−100): the negative and shifted rows are
  one word each instead of the 2-5 word materializations.
- alu_imm 1080 → 1060 (−20 additional): the 4096 row (was movz+add)
  and the #-5 row (was a 5-word movz+3×movk+sub) now fold.
- Row-by-row M2.6-vs-M2.7 accounting: grep confirms no other test
  contains a negative or 5+-digit ADD/SUB immediate, so these two rows
  are the only changes (the M1 delta 1096 − 20 reconciles exactly).
- Four-way parity: interp 30/0, x86 29/0/3, RV64 28/0/4, ARM 28/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: disabling the fold grows
  alu_imm to 1100 and alu_imm_ext to 1160 while remaining correct.

### 10.22 M2.8 amendment — LOAD/STORE displacement address math (as built)

§10.21 closed with the natural next target: a negative LOAD/STORE
displacement fell to the movz(+movk)+add_shift materialization even at
|disp| ≤ 4095, because A64's scaled load/store immediate is unsigned.
M2.8 extends the M2.7 add/sub-imm machinery to the memory-operand
address math. The LOAD and STORE codegen's non-scaled fallback (any
disp that is negative, unaligned for the access width, or too large)
now, gated on g_alloc, folds |disp| through the SAME shared
imm12_split helper the ALU fold uses — plain imm12 (0..4095) or the
shifted imm12<<12 form for multiples of 4096 up to 0xFFFFFF — emitting
a single `add/sub xB, xB, #imm` in place on the base register (one
word) instead of materialize + add_shift (2-5 words). STORE's
materialize fallback keeps its M2.6-era X_T2 + emit_li64 shape (both
x9/x10 are fetch targets there, so there is no h_imm to borrow). The
in-place base modification is gated by clobber_scratch before the fold
in both paths: without it, a base resident at its fetch slot would
leave a stale directory entry claiming a register that now holds
base±disp, poisoning every later read of that base (load-bearing —
mem_neg re-reads its base registers across the fold rows and would
compute wrong addresses otherwise).

- **`tests/mem_neg.simi`** — the negative-displacement rows: [r6-8]
  (fold sub #8), [r5-4096] (fold sub #1, lsl #12), [r5-4101] (honest
  materialize — |disp| = 4101 > 4095 and not a multiple of 4096, so
  the fold must decline), and [r6+5] (fold add #5 — positive but
  unaligned for i32, so the scaled fast path declines too). The bases
  r5 = r7 + 4661 (LEA #4095 + ADD #566) and r6 = r7 + 4087 (LEA
  #4087) are pure arithmetic, never dereferenced, so every effective
  address lands in r7 + [560, 4092] — the band portable across all
  four engines' r7 scratch conventions: above ARM/RV64's 576-byte
  frame (which occupies r7 + [−16, 560)), inside x86's 4096-byte mmap
  window (a 4-byte i32 access must end before offset 4096), and small
  enough for the interpreter's zeroed r7 + 64 KiB mem. The four i32
  addresses (565, 560, 4079, 4092) are all ≥ 4 bytes apart. A first
  draft used −4096 and −4097 — one byte apart — and the i32 stores
  OVERLAPPED (the 4-byte store at r7+1 clobbered the store at r7+0's
  high bytes); the four-way parity caught it as a wrong result (400
  vs 600), the same class of address-overlap bug the scaled path's
  alignment requirement guards against by construction. Expected
  600. M0 baseline measured against the committed M0 translator:
  **1308 → 1200, 108 saved**; disabling the fold grows it back to
  exactly 1272. mem_ops_native's displacements (0, 8) were already in
  the scaled fast path, so it is unchanged at 1004 — the audit the
  milestone asked for.

### 10.23 M2.8 gate results (measured)

Total emitted bytes across the 29-program parity set: **M0 39716 →
M1 37484, 2232 saved** (≈5.6%). The M2.8 row on top of M2.7's 2124:

- mem_neg 1308 → 1200 (−108): six of its eight memory ops fold to a
  single add/sub-imm (the −4096 rows through the sh=1 form); the
  −4101 store+load round-trip stays on the honest materialize path.
- Row-by-row M2.7-vs-M2.8 accounting (gate tables diffed, mem_neg
  excluded): all 28 shared rows byte-identical — nothing else grew or
  shrank. The totals move by exactly the mem_neg row (M0 +1308, M1
  +1200).
- Four-way parity: interp 31/0, x86 30/0/3, RV64 29/0/4, ARM 29/0/4.
  jmpr_oob still faults (UDF, rc=1). Teeth: disabling the address-math
  fold grows mem_neg to 1272 while remaining correct.

### 10.24 M2.9 amendment — pre-indexed load/store fold (as built)

§10.23 closed with the natural next target: a negative displacement
that is *aligned* to the access width (e.g. −16 for i64) still paid the
M2.8 sub-imm + zero-offset access (two words) even when it was tiny,
because A64's scaled immediate is unsigned and the M2.8 address math
had no smaller form. M2.9 adds the missing single-word form: the
pre-indexed load/store with its signed 9-bit *unscaled* immediate,
`ldr/str xt, [xb, #imm]!` (bits 31:30 size, 29:27 111, 26 0, 25:24 00,
23:22 opc, 21 0, 20:12 imm9 signed, 11:10 11 pre-index, 9:5 Rn, 4:0 Rt
— the 00/01 values of bits 11:10 are the unscaled ldur/stur and
post-indexed forms, neither emitted). In both LOAD and STORE address
math, a displacement that is negative, aligned to the access width,
and fits imm9 (|disp| ≤ 256) now emits ONE word instead of sub + a
zero-offset access, gated on g_alloc like every fold before it. The
writeback updates only the base's HOST register — guest memory is
touched only at the effective address, and clobber_scratch already
poisoned the base slot before the fold (the M2.8 load-bearing detail,
now load-bearing for a second reason), so later reads of the base
re-fetch from memory. Two aliasing facts make the fold safe without
extra guards: for LOAD, the emulator applies the writeback before the
load result (the ARM pseudocode order), so rd==ra (`ldr x9, [x9,
#-256]!`) ends with the loaded value in the result register; for
STORE, the value and base hosts are always distinct (cache_fetch_hosts
returns x9/x10), so the emitted rt never equals rn and the value is
never the written-back address even when the guest value register
aliases the base.

A first draft double-emitted: the pre-indexed word was followed by the
pre-existing trailing load_typed/store_typed, and for the rt==rn loads
the follow-up zero-offset load re-read `Mem[loaded value]` as an
address — the four-way parity caught it as a garbage ARM result
(-1008467667301860836 vs 1144 on the other three engines). The
translator now skips the trailing access on the pre-indexed branch
(the load_typed/store_typed calls moved into the imm12_split and
materialize branches). The same draft also used an i8 row loaded with
T_I8: 200 sign-extends to −56, and the three non-ARM engines agreed on
1144 — the row is u8 now.

The emulator (a64_exec.c) gained the pre-indexed decode, fenced by
`(w & 0x3B000000) == 0x38000000 && (w & 0xC00) == 0xC00` (bits 29:27
= 111, 26 = 0, 25:24 = 00, 21 = 0, 11:10 = 11 — the scaled class's
25:24 = 01 and the register-offset/atomic bit 21 = 1 are both
excluded; ldur and post-indexed still fall through to BAD_INSTR as
always). The writeback happens before the Rt read/store, matching the
ARM pseudocode. a64_enc_check.py gained the 11 classes (ldr/str/ldrb/
strb/ldrh/strh/ldr_w/str_w/ldrsb/ldrsh/ldrsw, all `_pre`) in ALLOWED
and decoders masked 0xFFC00C00 so the 11:10 = 11 marker is asserted,
not just the top byte. The encoding was pinned against QEMU's
a64.decode `@ldst_imm_pre` and the canonical `str x29, [sp, #-16]!` ==
0xF81F0FFD (no assembler available in this environment — the
verification caveats of §2 apply; the parity net plus the independent
Python bit layout are the cross-checks, exactly as for every other
emitted class).

- **`tests/mem_pre.simi`** — ten rows across all four widths: i64 at
  [r5-16] and [r5-256] (the exact imm9 boundary — −256 is the most
  negative 9-bit signed value), i32 at [r5-8] and [r5-4], i16 at
  [r6-6], u8 at [r6-1], the sign-extending i8 (ldrsb) at [r6-9], i64
  at [r6-24], and the two honest non-folds [r5-264] and [r6-258]
  (|disp| > 256 yet still aligned, so they take the M2.8 sub path).
  Every row round-trips store→load at the same address and re-reads
  its base (r5/r6) across rows, pinning the clobber_scratch
  poisoning the writeback depends on. Effective addresses stay in
  r7 + [1784, 3047] — inside the same portable band as mem_neg, ≥ 4
  bytes apart. Expected 1500. M0 baseline (committed M0 translator):
  **1860 → 1392, 468 saved**; disabling the pre-indexed fold grows it
  back to exactly 1456. mem_neg's [r6-8] store and load now fold too:
  1200 → 1192, revising §10.22's "sub #8" row description and §10.23's
  mem_neg total from 108 to 116 below M0.

### 10.25 M2.9 gate results (measured)

Total emitted bytes across the now-30-program parity set: **M0 41576 →
M1 38868, 2708 saved** (≈6.5%), up from M2.8's 2232. The M2.9 rows on
top of M2.8's 2232:

- mem_pre 1860 → 1392 (−468): its sixteen foldable memory ops (eight
  stores + eight loads) are each one pre-indexed word instead of
  sub + access; the −264/−258 round-trips stay on the M2.8 sub path.
- mem_neg 1308 → 1192 (−116, revised from −108): the [r6-8] i32 store
  and load fold to `str/ldr w, [x, #-8]!`, two more words saved.
- Row-by-row M2.8-vs-M2.9 accounting (gate tables diffed): all 28
  shared rows byte-identical — nothing else grew or shrank. The
  totals move by exactly mem_neg (−8) and the new mem_pre row.
- Four-way parity: interp 32/0, x86 31/0/3, RV64 30/0/4, ARM 30/0/4.
  enc-check 12/12 OK; jmpr_oob still faults (UDF, rc=1). Teeth:
  disabling the pre-indexed fold grows mem_pre to 1456 and mem_neg to
  1200, both still correct.

The M-line has now compounded to 2708 bytes saved. The immediate
family folds are complete across ALU, LOAD, and STORE — both signs,
both shift forms, and now the pre-indexed writeback form for aligned
negative displacements up to 256. The remaining lever on this axis is
structural rather than another fold: a negative displacement that is
*aligned but larger* than imm9 (e.g. −1024 for i64) still pays
sub + zero-offset, where a post-indexed writeback could not help, and
the two-word shape is the honest floor for magnitudes the pre-indexed
imm9 cannot express.

### 10.26 M2.10 amendment — the unscaled ldur/stur fold (as built)

§10.25 closed by asking whether the post-indexed writeback
(`ldr/str xt, [xb], #imm`) could fold another shape. The analysis
answered no for post-indexed specifically: its access is at the
UNMODIFIED base, so it can never fold the displacement into the same
instruction — only a cross-instruction fusion could use it, and the
translator emits one SIMI instruction at a time. But the same
imm9-family inspection turned up the form post-indexed's comment had
been calling out all along: the **unscaled ldur/stur** (bits 11:10 =
00), which needs NO writeback and NO alignment requirement. That makes
it strictly more general than the M2.9 pre-indexed form: it covers
every displacement in the same signed 9-bit imm9 ([-256, 255]),
*including unaligned ones* — the class that previously fell to the
M2.8 sub-imm + zero-offset access — and it does so with no base
register modification at all. M2.10 therefore replaces the pre-indexed
fold with the unscaled fold; the pre-indexed and post-indexed forms
(bits 11:10 = 11/01) are never emitted and decode to BAD_INSTR in the
emulator, which the M2.9 comment already reserved for them.

The translator change is small and simplifying. The fold condition
became `g_alloc && disp >= -256 && disp <= 255` (both signs, any
alignment); the unscaled word IS the access, so the trailing
load_typed/store_typed is skipped exactly as in M2.9; and — the
simplification — **clobber_scratch moved into the add/sub-imm
else-branch only**, because the unscaled form never modifies the base
host register. The M2.8/M2.9 load-bearing poison argument applied to
the *in-place* add/sub; with no writeback, the base's cache directory
entry stays truthful and a later fetch of the base can reuse the
register. The rd==ra case (`ldr x9, [x9, #imm]` with rh == h_a) reads
the base before overwriting it with the loaded value — no writeback
ordering to reason about, identical to the scaled fast path's existing
behavior. STORE's value/base hosts remain distinct by construction
(cache_fetch_hosts returns x9/x10).

The encoders are the imm9 family at bits 20:12 with bits 11:10 = 00
(bases 0xF8400000/0xF8000000 ldur/stur, 0x38400000/0x38000000
ldurb/sturb, 0x78400000/0x78000000 ldurh/sturh, 0xB8400000/0xB8000000
ldur_w/stur_w, and the sign-extending ldursb/ldursh/ldursw at
0x38800000/0x78800000/0xB8800000 — the M2.9 bases minus the 0xC00
writeback marker), plus the load_unscaled/store_unscaled width tables.
The emulator's decode fence became `(w & 0x3B000000) == 0x38000000 &&
(w & 0xC00) == 0x0000` with the writeback line deleted; pre/post-indexed
words fall through every other class to BAD_INSTR. a64_enc_check.py's
11 classes renamed `_pre` → the bare ldur/stur names in the encoders,
the decoders (mask 0xFFC00C00 with bits 11:10 = 00 asserted in both
mask and target), and ALLOWED. The canonical reference is now
`stur x29, [sp, #-16]` == 0xF81F03FD (0xF81F0FFD decodes back to the
pre-indexed form — the bit that changed is exactly the 11:10 field).
The QEMU @ldst_imm9 cite and the §2 verification caveats apply as
before; the parity net plus the independent Python bit layout are the
cross-checks.

- **`tests/mem_pre.simi`** — rewritten for the unscaled form with two
  NEW rows that only it can fold, which are the reason the form
  supersedes M2.9: [r5+5] (positive but UNALIGNED for i32 — the
  scaled fast path rejects it for alignment, and the M2.9 aligned-only
  pre-indexed fold rejected it too) and [r5-45] (negative unaligned
  — M2.9's alignment test excluded it as well). Both round-trip
  store→load and fold to a single stur_w/ldursw word. The other ten
  rows carry over ([r5-16], [r5-256] the imm9 boundary, the honest
  non-folds [r5-264]/[r6-258], i32 [r5-8]/[r5-4], i16 [r6-6], u8
  [r6-1], i8 ldursb [r6-9], i64 [r6-24]); expected 1800. M0 baseline
  re-measured for the 12-row file: **2012 → 1472, 540 saved**;
  disabling the unscaled fold grows it back to exactly 1552.
- **`tests/mem_neg.simi`** — comment-only change; its [r6+5] row (the
  positive-unaligned case that M2.8 folded as add #5 + access) now
  folds to a single stur_w/ldursw #5: 1192 → 1184, revising §10.24's
  mem_neg total from 116 to 124 below M0 (the [r6-8] row emits the
  unscaled word instead of the pre-indexed one — same size, new
  encoding).

### 10.27 M2.10 gate results (measured)

Total emitted bytes across the 30-program parity set: **M0 41728 → M1
38940, 2788 saved** (≈6.7%), up from M2.9's 2708. The M2.10 rows on
top of M2.9's 2708:

- mem_pre 2012 → 1472 (−540, re-baselined for the 12-row file): its
  20 foldable memory ops are each one unscaled word — including the
  four that were two words under M2.9's aligned-only fold (the +5/−45
  unaligned rows) — and its base grew by the M0 cost of two rows (152)
  minus what the unscaled fold recovers (80).
- mem_neg 1308 → 1184 (−124, revised from −116): the [r6+5] i32
  store and load fold to `stur_w/ldursw w, [x, #5]`, two more words
  saved.
- Row-by-row M2.9-vs-M2.10 accounting (gate tables diffed): all 28
  shared rows byte-identical — nothing else grew or shrank. The
  totals move by exactly mem_neg (−8) and the mem_pre re-baseline.
- Four-way parity: interp 32/0, x86 31/0/3, RV64 30/0/4, ARM 30/0/4.
  enc-check 12/12 OK; jmpr_oob still faults (UDF, rc=1). Teeth:
  disabling the unscaled fold grows mem_pre to 1552 and mem_neg to
  1200, both still correct.

The M-line has now compounded to 2788 bytes saved. The imm9 family is
complete in its canonical form: the unscaled ldur/stur is the single
word for EVERY displacement in [-256, 255], aligned or not, either
sign — strictly more general than the pre-indexed form it replaced,
and strictly simpler (no writeback, no clobber). The remaining lever
on this axis is magnitudes past imm9: a displacement like −1024 for
i64 still pays the two-word sub + zero-offset shape, where the
register-offset forms (`ldr xt, [xb, xm]` with the magnitude in a
register, or a base adjusted once for a run of accesses) are the
natural next candidate.

### 10.28 M2.11 amendment — register-offset access and run-reuse (as built)

§10.27 closed by naming the next structural lever: magnitudes past the
imm9 window (|disp| > 255) still paid the materialize path — li64 +
add_shift + zero-offset access, three words — and asked whether the
register-offset forms (`ldr/str xt, [xb, xm]`) could do better, either
by materializing the displacement once for a run of accesses or when
the base is dead after the access. M2.11 answers with two folds, both
gated on g_alloc like every fold before them:

1. **Per-instruction register-offset access** on the materialize path
   (the class mem_materialize_class returns true for: |disp| > 4095
   and not an imm12-split multiple of 4096, either sign). The
displacement is materialized into a scratch register (h_imm for LOAD,
X_T2 for STORE — clobbered first) and the access is a SINGLE
`ldr/str xt, [xb, xm]` word instead of li64 + add_shift +
zero-offset access (three words). Nothing modifies h_base, so — the
M2.10 simplifying fact again — clobber_scratch(h_a/h_base) lives in
the imm12 branch only. mem_neg's −4101 i64 pair is the corpus
instance: 1184 → 1176 (−8).

2. **Run-reuse**: a pre-pass (also g_alloc-gated) marks maximal
   straight-line runs of ≥ 2 consecutive LOAD/STORE with the SAME
   materialize-class displacement (the class is purely a function of
   (disp, type), so sharing a displacement means sharing a class). The
   run head materializes the displacement ONCE into **X_DR = x12** — a
   register deliberately outside the x9/x10/x11 cache, verified unused
   by the prologue, trampoline, cache, and hostfn call sites — and
each access in the run emits one register-offset word reusing x12:
   run of N accesses = 1 li64 + N words vs. the per-instruction 3N.
   Runs break at g_pc_target (block heads, which folded-JMPR targets
   seed) and at control-flow boundaries. The dynamic-JMPR hazard is
   structural, not special-cased: g_alloc is 0 whenever any JMPR fails
   to fold to a direct branch (the M0 comment at its definition), so a
   function containing a non-folded JMPR gets NO run-reuse at all — a
   dynamic jump can never land mid-run on an un-materialized x12.

   The STORE alias analysis is worth recording. cache_fetch_hosts only
   ever assigns x9/x10 for the value and base hosts (the M2.5 swap is
   x9↔x10), so h_val can never be X_T2 — the per-instruction
   displacement li64 into X_T2 cannot overwrite a live store value, and
   clobber_scratch(X_T2) is a pure safety net. X_DR's clobber_scratch
   is a no-op by construction (slot 3 fails the cache guard); the run
   head's li64 lands in a register nothing else touches between the
   head and the run's last access, because runs never span a block
   head or a control-flow boundary. The rd==ra and value-aliases-base
   cases need no extra guards: the register-offset word has no
   writeback, so the base host is never modified, and the M2.10
   distinct-hosts argument carries over unchanged.

The encoding is the A64 load/store register (register offset) form:
bit 21 = 1, bits 11:10 = 10, option = 011 (LSL#0) for 64-bit
accesses, option = 110 (SXTW) for the narrow sign-extension case
(negative displacements on i32/i16/i8), S = 0 — pinned against QEMU's
a64.decode `@ldst .. ... . .. .. . rm:5 opt:3 s:1 .. rn:5 rt:5` (the
same derivation the M2.x encoders used; no assembler available, §2
caveats apply, the parity net plus the Python bit layout are the
cross-checks). The emulator's decode fence is `(w & 0x3F20FC00) ==
0x38206800` (option 011) or `== 0x3820C800` (option 110) — bits
29:24 = 111000 (29:27 = 111, V = 0, opc = 00), bit 21 = 1, bits 11:10
= 10, S = 0, rn[9:8] = 00. The fence cannot collide with any other
emitted class: scaled (25:24 = 01) and the imm9 family (bit 21 = 0)
are excluded by pinned bits, and MOVZ/MOVK (29:27 = 010), CBZ/CBNZ
(110), B/BL (001), B.cond (010), BR/BLR/RET (010), and the ALU
shifted/logical ops (100/001/010/101) all differ in 29:27. The load
side fences (opc = 01 → 0x3920…, opc = 10 → 0x3A20…) are distinct
from the store fences and from each other. a64_enc_check.py gained
the 11 `_reg` classes in the encoders, the decoders, and ALLOWED.

- **`tests/mem_reg.simi`** — three runs plus a run-breaker and
  unchanged controls: an i64 run at [r7-4101] (the mem_neg magnitude,
  now exercising run-reuse), an i64 run at [r7+8193], and an i32 run
  at [r7-4101] (the SXTW form — str_w_reg/ldrsw_reg with rm = x12).
  The run-breaker is a [r15+4101] store/load pair splitting the i64
  run in two, verifying runs break and re-materialize. Expected 1300.
  M0 baseline (committed M0 translator): **1764 → 1440, 324 saved**;
  disabling run-reuse (run-length gate forced to 9999) grows it back
  to exactly 1580. mem_neg's −4101 pair folds per-instruction: 1184 →
  1176.

### 10.29 M2.11 gate results (measured)

Total emitted bytes across the now-31-program parity set (the gate's
own M0/M1 totals now include the new mem_reg row): **M0 43492 → M1
40372, 3120 saved** (≈7.2%), up from M2.10's 2788. The M2.11 rows on
top of M2.10's 2788:

- mem_reg 1764 → 1440 (−324, new 31st row): the three runs' heads
  materialize x12 once each (li64 count 17 across the file — one per
  run head), every run access is one register-offset word, and the
  standalone pair uses per-instruction scratches (rm = x11/x10).
- mem_neg 1184 → 1176 (−8): its −4101 i64 pair now emits
  li64 + `ldr/str x, [x, xm]` (two words) instead of li64 + add +
  zero-offset access (three).
- Row-by-row M2.10-vs-M2.11 accounting (gate tables diffed): all 29
  shared rows byte-identical — nothing else grew or shrank. The
  totals move by exactly mem_neg (−8) and the new mem_reg row.
- Four-way parity: interp 33/0, x86 32/0/3, RV64 31/0/4, ARM 31/0/4.
  enc-check 12/12 OK; jmpr_oob still faults (UDF, rc=1). Teeth:
  disabling run-reuse grows mem_reg to 1580, still correct.

The M-line has now compounded to 3120 bytes saved. The immediate
family (ALU, LOAD, STORE) is folded through both imm9 forms and now
past imm12 via the register-offset word, with run-reuse amortizing the
materialize across a straight-line run. The remaining levers are the
same structural ones §10.27 named: register-offset forms for narrow
magnitudes a run cannot amortize (a lone |disp| > 4095 access still
pays 2 words — the honest floor for a magnitude no immediate can
express), and — beyond the memory folds — the branch-side
optimizations (block coalescing, tail reuse) that the JMPR folding has
been leaving on the table.

### 10.30 M2.12 amendment — block coalescing for folded JMPRs (as built)

§10.29 closed by naming the branch-side lever the memory folds had
been ignoring: every folded JMPR emitted `cache_flush; b target`, and
every block head paid rule 1's end-of-loop flush — so a program whose
JMPR index happens to fold to the VERY NEXT pc emitted a branch to the
next instruction (dead: control falls through to it anyway) and then
spilled/reloaded the register frame across a block boundary that was
never really a join. M2.12 adds a block-coalescing pre-pass that
eliminates both, gated on g_alloc like every fold before it:

1. **The dead branch is dropped.** A folded JMPR whose target is
   pc+1 (`g_jmpr_fold[pc] == pc + 1`) is marked `g_fold_fall[pc]`; the
   main loop then emits NOTHING for it (previously `cache_flush` +
   `op_b`). Falling through to pc+1 IS the fold's target, so the
   branch is pure waste — one word saved on every fall-through fold.
2. **The blocks fuse when the fold is the only way in.** The pre-pass
   counts pc+1's other incoming edges: BR/BC/CALL (target formula
   pc+1+imm28, the same one pass A uses), other folded JMPRs (the scan
   skips q == pc — the fold at pc is the edge being dropped, not an
   "other"), and entry trampolines (`entries[i].offset`). When NONE
   exist, `g_pc_target[pc+1]` is CLEARED: the end-of-loop flush rule 1
   would otherwise owe at the boundary is skipped, the x9/x10/x11
   cache survives the join, and the register frame is loaded once, not
   once per block. When the target keeps other edges, the branch is
   still dropped but the flush stays — the other paths land with an
   empty cache exactly as before.

The soundness argument is the straight-line invariant made explicit.
Clearing a block-head mark is only sound when the only path to that pc
is the fall-through, which carries precisely the cache state the
previous instruction's emission left (the same invariant every
non-block-head pc already relies on). With g_alloc = 1 every JMPR
folds, so no dynamic JMPR can land mid-block; the incoming-edge scan
closes the direct-branch and entry paths. The end-of-loop flush for a
still-marked target is unchanged, so the other edges' arrivals are
byte-for-byte what they were. The M2.11 run-reuse pre-pass runs AFTER
this pass, so a LOAD/STORE run may now START at a fused target — the
cleared mark is exactly what the run walk checks — a compounding win
noted but not separately tested. One conservative ceiling: clearing
marks happens after the fold fixpoint converges, so a chain that a
fused boundary would newly enable to fold is missed (sound — no fold
is invalidated — just not maximally folded).

- **`tests/jmpr_fall.simi`** — the first fall-through fold in the
  corpus (all prior jmpr tests fold two or more pcs ahead). Live path:
  r1 = 7 folds to pc 7 = pc+1, fused (no other edge), so the chain
  r0/r1/r2 built at pcs 1-5 stays resident into the target's ADD —
  the register frame is loaded once. Dead path (pcs 9-14, after pc 8's
  RET, never executed — its EMISSION is what the gate measures): a
  fall-through fold at pc 10 whose target pc 11 has a SECOND incoming
  edge (the backward fold at pc 13 also targets pc 11), pinning the
  NON-fused half — branch dropped, flush kept. Expected 133. M0
  baseline (committed M0 translator, dynamic JMPR paths): **1376 →
  1064, 312 saved**; disabling the coalescing (fold-target check
  pc+1 → pc+9999) grows it back to exactly 1080, so the fusion itself
  is worth 16 of the 312.

### 10.31 M2.12 gate results (measured)

Total emitted bytes across the now-32-program parity set (the gate's
own M0/M1 totals now include the new jmpr_fall row): **M0 44868 → M1
41436, 3432 saved** (≈7.6%), up from M2.11's 3120. The M2.12 rows on
top of M2.11's 3120:

- jmpr_fall 1376 → 1064 (−312, new 32nd row): the fused live path
  drops the branch AND the boundary flush (the target ADD reuses
  resident x9/x10/x11 — dump-verified: no spill/reload between the
  chain and the target), and the dead path pins the non-fused branch-
  drop (no branch word, flush retained).
- Row-by-row M2.11-vs-M2.12 accounting (gate tables diffed): all 31
  shared rows byte-identical — nothing else grew or shrank. The
  totals move by exactly the new jmpr_fall row.
- Four-way parity: interp 34/0, x86 33/0/3, RV64 32/0/4, ARM 32/0/4.
  enc-check 12/12 OK; jmpr_oob still faults (UDF, rc=1). Teeth:
  disabling the coalescing grows jmpr_fall to 1080, still correct.

The M-line has now compounded to 3432 bytes saved. The JMPR side of
the branch ledger is complete: constant-index dispatches fold, dead
fall-through branches vanish, and genuinely-empty block joins fuse
away their prologue flush. The remaining branch-side levers §10.29
named are still on the table — tail reuse (a block whose epilogue is
identical to its successor's prologue) and coalescing chains across
more than one fall-through fold — plus the fixpoint-vs-fusion
interaction the §10.30 ceiling describes.

### 10.32 M2.13 amendment — cascading fall-through folds (as built)

The §10.30 ceiling named exactly the interaction M2.13 now removes:
"clearing marks happens after the fold fixpoint converges, so a chain
that a fused boundary would newly enable to fold is missed." A chain
of fall-through folds collapsed under M2.12 because the rule-1 merge
marked EVERY fold target as a block head — including a fall-through
fold's own target (pc+1). The mark reset the constant map at the NEXT
JMPR in the chain, un-folding it; a JMPR that fails to fold is
DYNAMIC, which kills g_alloc for the whole function. Three chained
fall-through folds: only the first folds, the other two fall back to
the runtime table + bounds check, and the function loses its cache
entirely.

The fix is a two-line change plus the coalescing pass taking over
mark authority:

1. **The rule-1 merge skips fall-through folds.** `g_jmpr_fold[pc] >= 0
   && g_jmpr_fold[pc] != (int)(pc + 1)` — a fall-through fold's branch
   is the dead one M2.12 drops, so it owes no mark. The constant map
   flows through the join, the next JMPR in the chain still sees its
   index constant, folds, and so on down the whole chain.
2. **The coalescing pass is the authority on fall-through targets.**
   For each fall-through fold it counts the target's OTHER incoming
   edges as before; when NONE exist it fuses (`g_pc_target[t] = 0`),
   and when they DO it now RE-MARKS (`g_pc_target[t] = 1`) — the
   merge no longer did, and the boundary flush plus the analysis-side
   constant reset must be exactly what they were.

The final `g_pc_target` state is IDENTICAL to M2.12's for every shape
the earlier pass handled (fused and non-fused single folds) — the
only behavioral difference is that chains now fold all the way
through, and a g_alloc=0 function with a folded fall-through JMPR
stops emitting the (dead, zero-width) boundary flush after its
unconditional branch. Pass A also now marks entry pcs: the trampoline
branches to them, so constants cannot survive into an entry — this
hardens the corner where an entry pc is also a fall-through fold
target (without it, such a target would be unmarked in the fixpoint
and a JMPR after it could fold on a fall-through-only constant; the
trampoline arrival marshals the arg slots and guarantees nothing).
Byte-neutral for the corpus — every entry sits at pc 0, where the
scan starts with an empty constant map anyway.

Soundness of the cascade rests on the same single-edge argument as
§10.30's fusion: a fall-through target is only left unmarked when it
has no other way in, so the only path to it is the fall-through,
which carries exactly the constants the linear walk attributed. When
it DOES have other edges, the edge that marks it (pass-A branch, real
fold merge) is sticky from the fixpoint's early iterations, and the
fixpoint re-scans un-fold any JMPR that folded across the join. The
coalescing pass never invents a fold — it only decides marks after
the fixpoint has converged.

- **`tests/jmpr_fall2.simi`** — the first chain in the corpus. Three
  JMPRs in a row, each folding to its own next pc (r1=5→pc 5,
  r2=6→pc 6, r3=7→pc 7), with all three index constants loaded
  BEFORE the chain (pcs 1-3) — the shape that actually exercises the
  cascade (loading each index right before its JMPR would fold even
  under the old merge). Expected 18; dump-verified: three movz + tag
  writes, NO branch words, no boundary flush, x9/x10/x11 resident
  straight into the target ADDs — the register frame is loaded once,
  not four times.

### 10.33 M2.13 gate results (measured)

Total emitted bytes across the now-33-program parity set (the gate's
own M0/M1 totals now include the new jmpr_fall2 row): **M0 46096 →
M1 42416, 3680 saved** (≈8.0%), up from M2.12's 3432. The M2.13 row
on top of M2.12's 3432:

- jmpr_fall2 1228 → 980 (−248, new 33rd row). The cascade is the
  bulk of it: the old mark-all merge (M2.12's rule-1) measures 1180 —
  the cascade alone is worth 200 of the 248, since a mark-all chain
  leaves JMPRs 5/6 dynamic, which kills g_alloc and drops the whole
  function to memory-slot codegen. Disabling the coalescing (fold-
  target check pc+1 -> pc+9999) grows it to 996, still correct —
  the branch-drop + fusion is worth 16 of the 248.
- Row-by-row M2.12-vs-M2.13 accounting (gate tables diffed): all 32
  shared rows byte-identical — the pass-A entry marking and the merge
  change perturbed nothing in the existing corpus. The totals move
  by exactly the new jmpr_fall2 row.
- Four-way parity: interp 35/0, x86 34/0/3, RV64 33/0/4, ARM 33/0/4.
  enc-check 12/12 OK; jmpr_oob still faults (UDF, rc=1). Teeth:
  mark-all merge 1180 and coalescing-off 996 both still verify 18.

The M-line has now compounded to 3680 bytes saved, and the §10.30
ceiling is gone: fused boundaries now participate in the fold
analysis, so chains fold all the way through instead of stopping at
the first fused join. The remaining §10.29 levers are tail reuse and
block coalescing's sibling — epilogue/prologue merging across
backward folds — plus the entry-corner hardening note in §10.32.

### 10.34 M2.14 amendment — tail reuse: one return sequence per function (as built)

The first of the §10.29 branch-side levers, and the one the dump
revealed most plainly: in jmpr_basic, the dead-path RET and the target
RET emitted byte-identical 7-word return tails (ldr x9 slot0; ldrb
x10 tag0; add sp,#TOTAL_FRAME; ldr x30; ldr x29; add sp,#16; br x30).
The tail is cache-independent — the RET's cache_flush emptied
x9/x10/x11 first, so its use of t0/t1 cannot collide with a resident
guest — and pc-independent, so every RET in the function emits the
same bytes. M2.14 emits ONE copy: the first RET in pc order emits it
in place and records the tail's first word in g_tail_ret_off; every
later RET emits just `cache_flush; b tail` — a backward branch, since
the owner is earlier in layout, whose target is exact at emit time (no
fixup needed; the offset arithmetic is a byte-delta / 4 with enc_b's
signed imm26 masking). N RETs share one copy; N-1 copies are deleted,
6 words (24 bytes) each. Gated on g_alloc like every fold: the naive
path stays byte-identical to M0, and jmpr_dyn keeps its documented
0-saved honest floor.

Soundness is the straight-line invariant again. Each RET's own flush
writes ITS resident set to slots before branching; the shared tail
then reads r0's slot+tag (sp-relative — correct for whichever frame
sp points at, which is also why the tail would stay correct even if a
.tmo ever carried multiple exported entries, since the frame layout
TX_AR_TOTAL_FRAME_BYTES is uniform) and returns. The branch crosses
no SIMI block boundary: the cache is empty after the flush and the
tail touches only t0/t1, so every arrival sees exactly the state its
own RET left. The CALL-return protocol is preserved identically: the
tail leaves r0's value in t0 and tag in t1 for the call site's
post-call code, whether the RET is the owner or a sharer.

This milestone also repaired a test-suite regression that M2.3 had
introduced silently: jmpr_dyn and jmpr_mix made their "runtime" JMPR
index with r1 = 5 ^ r0, and M2.3 added XOR to the fold set — folding
the index to 7, flipping both tests to g_alloc=1, and leaving the
dynamic (and mixed) dispatch paths unexercised since then (only
jmpr_oob's fault path remained). Both now read the index back from
guest memory (`LOAD r1, r7, #0` after a `STORE`), which the
constant-fold analysis never tracks (LOAD writes rd and hits the
constant-reset branch), so g_alloc=0 is genuinely restored — and the
gate's "honest floor" prose is true again: jmpr_dyn's dynamic JMPR
emission is byte-identical to M0's, its 16 bytes below M0 coming from
the zero-displacement STORE/LOAD folds, which apply to every program
regardless of g_alloc.

- **`tests/tail_ret.simi`** — three RET blocks via a BC dispatch (no
  JMPR), pinning that tail reuse is independent of the JMPR folding
  and of g_alloc's folds in general. Expected 33; dump-verified: one
  full tail, two `cache_flush; b tail` sharers.
- **call_ret** — dump-verified the sharer ON a call-return path:
  add2 (callee, pcs 0-2) owns the tail; main (pcs 3-7) calls add2 and
  its own RET is the sharer — `b` back over the callee's code to the
  shared tail, which pops MAIN's frame and returns, handing r0's
  value+tag to the harness through t0/t1.

### 10.35 M2.14 gate results (measured)

Total emitted bytes across the now-34-program parity set (the gate's
own M0/M1 totals now include the new tail_ret row): **M0 47168 →
M1 43352, 3816 saved** (≈8.1%), up from M2.13's 3680. The M2.14
movement on top of M2.13's 3680:

- Tail reuse: every multi-RET g_alloc=1 row shrank by exactly
  24 bytes (6 words) per sharing RET — aggregate_abi −72 (4 RETs),
  cap_call_ret −48, jmpr_calc_bit −48, jmpr_fall −48, branch_cmp
  −24, call_ret −24, jmpr_basic −24, jmpr_calc −24, jmpr_calc_mul
  −24, jmpr_mid −24; tail_ret 1048 → 1000 (−48, new 34th row). The
  row-by-row M2.13-vs-M2.14 accounting (gate tables diffed): every
  OTHER row byte-identical — the tail-sharing deltas account for all
  408 bytes, with no collateral movement anywhere.
- jmpr_dyn 1136/1004 → 1148/1132 and jmpr_mix 1300/1080 → 1312/1248:
  the two restored dynamic tests (see §10.34) — their M0 baselines
  re-measured at 1729f50 with the new content. The honest-floor row
  is genuine again (16 saved, all from the memory folds).
- Four-way parity: interp 36/0, x86 35/0/3, RV64 34/0/4, ARM 34/0/4.
  enc-check 12/12 OK; jmpr_oob still faults (UDF, rc=1). Teeth:
  disabling the sharing (`0 && g_alloc && ...`) grows tail_ret back
  to exactly 1048 = its M0 baseline and jmpr_basic to 984, both still
  correct.

The M-line has now compounded to 3816 bytes saved. Tail reuse closes
out the branch-side ledger §10.29 opened: dispatches fold, dead
fall-through branches vanish, empty joins fuse away their prologue
flush, chains cascade, and now N blocks ending in the same terminal
sequence share one copy of it. The remaining lever is §10.29's other
named item — epilogue/prologue merging across backward folds — plus
the fixpoint-vs-coalescing interactions the §10.30 ceiling
replacement (§10.32) describes.

### 10.36 M2.15 amendment — epilogue/prologue merging across backward folds (as built)

The last §10.29 lever, and the one the earlier milestones could not
touch: a folded JMPR to an EARLIER pc emits `cache_flush; b target`,
and the target block's head RELOADS its operand registers from the
frame (rule 1 gives a marked head an empty cache) — while the source
block's tail may have reloaded the SAME registers a few words earlier.
The key fact that makes the round trip partially dead is M1's
fetch discipline: `get_operand`'s miss path ld_slots WITHOUT claiming
the directory (only results — `cache_reserve` claims — ever become
resident). A tail fetch of g1/g2 into x9/x10 is therefore TRANSIENT,
and the fold's flush stores only the directory's resident RESULTS —
it never touches x9/x10's transient values. So when control lands at
the head, x9 still holds g1 and x10 still holds g2: the head's matching
reloads would re-read the same slot values into the same hosts — dead
code. The merge DROPS them, so the register frame is loaded once (at
the tail), not once per block.

Soundness is the single-edge invariant, sharper than §10.30's: the
head's code is SHARED by every incoming path, but the dropped reads
consume whatever is in x9/x10, which only the fold's path guarantees.
Any other edge (a fall-through from T-1, a BR/BC/CALL, another JMPR,
an entry trampoline) arrives with its own unknown x9/x10 state, so
the target must be reached ONLY via the fold: no BR/BC/CALL edge (the
pass-A formula q+1+imm28), no other folded JMPR, no entry, and T-1
must be an unconditional terminal — RET, BR, or a JMPR that does not
fold to T (this excludes BC's fall-through and CALL's return path).
This makes the shape inherently loop-free-dead at the target — a live
loop's head is also reached by its own entry — which is why the test
is structured so the LIVE path flows through the fold anyway: pc 3's
BR jumps INTO the fold source, so tail → fold → head all execute at
runtime, and the head's pattern writes r0 = r2 + r1, making the
expected value flow THROUGH the dropped reads (the ARM engine executes
them; a wrong drop diverges from the three interpreters).

The analysis must know the emitted words between the head and the
fold statically, so the layout is rigid: pattern [ALU-2src @ T] —
terminal [RET/BR @ T+1] — two result-only LOADIs [@ T+2, T+3] —
matching tail [ALU-2src @ T+4 reading the SAME guests in the SAME
order] — fold [JMPR @ T+5]. The LOADIs' destinations must be distinct
and not the pattern's sources (the tail's fetches are then real
reloads, misses against a cache holding exactly those two), and the
pre-T result registers must be pairwise distinct — the directory only
ever holds result registers, so a never-before-written rd cannot be
resident, every result op's claim is fresh, and the round-robin
cursor at the tail is exactly (result-op count) mod 3. The two LOADIs
occupy cursor+1 and cursor+2, so the tail's reserve picks the cursor
slot: the x9 fetch's value survives iff that slot is not x9, and the
x10 fetch's iff it is not x10 — the per-fetch drop flags. Gated on
g_alloc like every fold (the naive JMPR path stays byte-identical to
M0); `ar_is_result_op` mirrors the reserve call sites exactly
(ADD..SAR, DIV/MOD, NOT/NEG, MOV, LOADI, LOADI64, CMP, LEA, PTRADD,
LOAD, CALL-with-r0 — OP_RESOLVE/OBJSIZE/OBJTYPE flush and store
without claiming, and OP_LEAVE is a no-op, so neither advances the
cursor).

- **`tests/epi_merge.simi`** — the first single-edge backward fold in
  the corpus, and the M-line's first LIVE-executed merge test: the
  live path runs pc 0-2 (r2 = 3, r1 = 4), BRs into the fold source at
  pc 6, flows LOADI → LOADI → tail (the frame reloads) → fold → head
  (the dropped reads) → RET, and the head's `ADD r0, r2, r1` returns
  7. Expected 7 — the value propagates through the dropped reads, so
  the four-way parity self-validates the merge. The live region has
  TWO distinct results, so the result count mod 3 is 2, the tail's
  result lands in x11, and BOTH head fetches drop (the flags=3 case).
  M0 baseline (committed M0 translator, naive codegen): **1140 →
  1004, 136 saved**, of which the merge itself is the 8 bytes the
  teeth check isolates: disabling the fetch-drop hook grows the file
  back to exactly 1012 (the two reload words), still correct.
- **`tests/epi_merge2.simi`** — the ASYMMETRIC half (flags=2): a
  third distinct live result (r6) makes the count mod 3 = 0, so the
  tail's result lands in x9 and CLOBBERS the r2 transient — only the
  r1 fetch survives the fold's flush, the head drops ONLY its second
  fetch, and its first reload of r2 stays (reading the unchanged slot,
  which the live flush stored). A bug that drops both fetches reads
  x9 = the tail's result (7) as r2 and computes 11 instead of 7 — the
  parity fails on its own, pinning the asymmetric branch of the flags
  formula. **1160 → 1020, 140 saved**; disabling the merge grows it
  back to exactly 1024 (the one kept fetch, 4 bytes), still correct.

### 10.37 M2.15 gate results (measured)

Total emitted bytes across the now-36-program parity set: **M0 49468
→ M1 45376, 4092 saved**, up from M2.14's 3816. The M2.15 rows on top
of M2.14's 3816:

- epi_merge 1140 → 1004 (−136, new 35th row): the head's two reloads
  dropped (the tail's transient fetches survive the fold's flush in
  x9/x10) — dump-verified: the head emits a bare `add x11, x9, x10`
  (no `ldr` words), the tail emits the two frame reloads plus its two
  resident spills, and the fold's flush stores only the result.
- epi_merge2 1160 → 1020 (−140, new 36th row): the asymmetric
  flags=2 half — the tail's result lands in x9 and clobbers the r2
  transient, so only the r1 fetch drops and the head's r2 reload
  stays (dump-verified: `ldr x9, slot2` then `add x9, x9, x10` — the
  dropped read comes from x10's surviving transient).
- Row-by-row M2.14-vs-M2.15 accounting (gate tables diffed): all 34
  shared rows byte-identical — nothing else grew or shrank (no corpus
  program matches the rigid single-edge layout; jmpr_fall's backward
  fold targets a RET block head with a second edge). The totals move
  by exactly the new rows' 8 + 4 byte merge deltas.
- Four-way parity: interp 38/0, x86 37/0/3, RV64 36/0/4, ARM 36/0/4.
  enc-check clean; jmpr_oob still faults (UDF, rc=1). Teeth:
  disabling the merge grows epi_merge to exactly 1012 and epi_merge2
  to exactly 1024, both still correct.

The M-line has now compounded to 4092 bytes saved, and §10.29's
branch-side ledger is complete: dispatches fold, dead fall-through
branches vanish, empty joins fuse, chains cascade, terminal sequences
share one copy, and now a single-edge backward fold's head reuses the
source tail's reloads instead of paying for them twice. The honest
ceiling is the shape itself: the single-edge condition restricts the
merge to fold targets with no other incoming path (a live loop's head
always has an entry edge too), and the emission-order constraint — the
head is compiled before the source, so the runtime register state at
the head must be derivable from a rigid, statically-known layout — is
what keeps the analysis from generalizing to arbitrary backward folds.
### 10.38 M2.16 amendment — the fixpoint-vs-coalescing interaction (as designed)

M2.16 closes the §10.37 frontier by DESIGN, not by a code change — the
pass order already achieves what a re-ordering would, and the milestone
proves it. The interaction §10.30 named: "clearing marks happens after
the fold fixpoint converges, so a chain that a fused boundary would
newly enable to fold is missed." The question is whether the
coalescing pass's mark changes can ever change WHICH JMPRs fold — and
whether a re-run of the fold analysis after coalescing would emit
different code.

The answer is no, by the following mark-identity argument. The
fixpoint's block-head marks come from exactly three sources: pass-A
BR/BC/CALL targets, entry trampolines, and rule-1 fold targets — and
M2.13's rule-1 skip means a fall-through fold's OWN target is never
one of them. The coalescing pass clears `g_pc_target[t]` only when the
fall-through target t has NO other incoming edge, where "other edges"
are counted with the IDENTICAL formulas: BR/BC/CALL (q+1+imm28,
same bounds check), other folded JMPRs (`g_jmpr_fold[q] == t`, with
q == pc excluded — and a fall-through fold into t would require q ==
t-1 == pc, so any counted fold is non-fall-through and rule-1 marked
t), and entry trampolines. Every mark the fixpoint sets is therefore
an "other edge" that forces the coalescing pass to RE-MARK, never to
clear; every mark it clears was never set. The post-coalescing
fold-relevant mark set is IDENTICAL to the fixpoint's, so a re-run of
the fold analysis (the literal pass-order fix) reproduces g_jmpr_fold
exactly — byte-neutral. The identity is exact because the fixpoint's
convergence is exact: the rule-1 marks are a deterministic function of
the fold set (M = f(S)), and convergence S_k == S_{k-1} forces
M_k == M_{k-1}, so the final scan — the one that produced the
converged folds — already saw every mark a re-run would see. This was
corroborated empirically: temporary instrumentation comparing
g_pc_target before and after the coalescing pass printed "marks
identical after coalescing" for all 39 corpus programs that carry an
expected result (the full parity set).

The invariant to preserve is stated once, for future pass authors:
**the coalescing pass changes no fold-relevant mark.** M2.13's rule-1
skip is the mechanism that keeps it true (fall-through fold targets
are never marked, so the chain across them folds regardless of whether
the target is fused — the fusion is a flush-removal win, not a
fold-enabler). A future change that re-introduces mark-all, or adds a
new mark source that the coalescing scan does not count as an edge,
re-opens the interaction; the general fix would then be the joint
(marks, folds) convergence loop — fixpoint, coalescing, re-run,
re-run-coalescing — which terminates because folds are monotone in the
mark set. jmpr_cross pins the current closed state.

- **`tests/jmpr_cross.simi`** — the first corpus test of a chain
  CROSSING a fused fall-through fold's target. Fold A (pc 3 -> pc 4,
  target == pc+1) is dropped and fused (pc 4 has no other incoming
  edge); r2 (fold B's index) is loaded at pc 1, BEFORE pc 4, and read
  at pc 5. The rule-1 skip lets the constant survive the unmarked
  boundary, B folds to pc 7, and g_alloc stays 1 — dump-verified: the
  ADD at pc 4 reads r2 straight from the resident cache (no reload),
  no boundary flush, and ZERO dynamic-path words (no runtime table,
  no bounds check). jmpr_fall2 pins the CASCADE; jmpr_cross pins the
  GENERAL shape §10.30 named. Expected 19.

### 10.39 M2.16 gate results (measured)

Total emitted bytes across the now-37-program parity set: **M0 50680
→ M1 46368, 4312 saved** (≈8.5%), up from M2.15's 4092. The M2.16
row on top of M2.15's 4092:

- jmpr_cross 1212 → 992 (−220, new 37th row; M0 baseline measured at
  git 1729f50 like every other row — the committed M0 translator
  emits both JMPRs as dynamic table + bounds-check paths, g_alloc=0,
  naive codegen). The teeth isolate the two mechanisms' own
  contributions (they do not partition the 220 — the rest is the M1
  pipeline's codegen win vs M0's naive path, present regardless of the
  folds): reverting M2.13's rule-1 skip to mark-all grows it to
exactly 1164 (+172 — fold B's chain resets at the fused target, B
goes dynamic, g_alloc dies, the runtime table returns), and
disabling the coalescing (fold-target check pc+1 → pc+9999) grows it
to exactly 1000 (+8 — fold A's dead branch + flush, the branch-drop
side of coalescing; the fusion itself is worth 0 here because the
rule-1 skip left pc 4 unmarked anyway, which is precisely why the
fusion is not a fold-enabler); the same two teeth move jmpr_fall2 by
its exact M2.13 measurements (+200 and +16).
  Both teeth states still verify 19 — correctness is preserved either
  way, which is what the four-way parity proves.
- Row-by-row M2.15-vs-M2.16 accounting (gate tables diffed): all 36
  shared rows byte-identical — nothing else grew or shrank. The
  totals move by exactly the new jmpr_cross row: the byte-neutral
  re-run claim holds for the whole corpus, not just the new test.
- Four-way parity: interp 39/0, x86 38/0/3, RV64 37/0/4, ARM 37/0/4.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

The M-line has now compounded to 4312 bytes saved, and §10.37's last
frontier — the fixpoint-vs-coalescing interaction — is closed by the
mark-identity argument, pinned by jmpr_cross, and protected by the
two teeth (mark-all and coalescing-off) whose deltas are recorded
above. The remaining named levers on the branch ledger are the
emission-order constraint on epi-merge generalization (§10.37) and
the flags=1 cursor class (the mirror of epi_merge2, unexercised — the
formula and hook are shared, so it is a follow-up pin rather than a
gap). §10.40 closes that last pin.

### 10.40 M2.15 follow-up — the flags=1 cursor class (as pinned)

§10.39's "follow-up pin" — the third cursor class of the M2.15
epilogue/prologue merge, flags = 1 — is now exercised by
**`tests/epi_merge3.simi`**. simi_arm.c is unchanged: the pass's flag
formula `((c != 0) ? 1 : 0) | ((c != 1) ? 2 : 0)` and the main-loop
hook already handle the class, which is why it was a pin and not a
code change. The layout is the rigid epi_merge shape (pattern@T,
RET@T+1, two LOADIs@T+2/T+3, matching tail@T+4, fold@T+5) with FOUR
distinct live results (r2, r1, r6, r7) — the result count mod 3 is 1,
so the tail's result lands in x10, CLOBBERING the g2 (r1) transient:
only the g1 (r2) fetch survives the fold's flush (x9), and the head
drops ONLY its FIRST fetch (flags = 1), keeping the r1 reload — the
mirror of epi_merge2 (count mod 3 = 0, flags = 2, the second fetch
drops). The three classes are now all pinned: c=0 → flags=2
(epi_merge2), c=1 → flags=1 (epi_merge3), c=2 → flags=3 (epi_merge)
— the cursor-class ledger is closed.

Live-executed like epi_merge: the live path BRs (pc 5, the T-1
terminal) into the fold source, flows LOADI r4=6 → LOADI r5=9 → tail
ADD r9 (r2+r1=7) → JMPR r4 (folds backward to T=6) → head ADD r0
(r2+r1) → RET, returning 7. The dropped r2 read comes from x9's
surviving transient; the kept r1 reload reads the unchanged slot (the
live region's flush stored r1 = 4 at pc 5's BR). The tail's r2 fetch
into x9 evicts the resident r5 (a decoy LOADI result, standard
clobber behavior — the same eviction epi_merge's tail performs on its
LOADI residents), so the fold's flush stores only r4 and r9 and x9's
r2 transient survives — dump-verified: the head emits exactly ONE
ldr (the r1 reload into x10) then `add x10, x9, x10` = 7. A bug that
computes flags = 2 or 3 for this shape reads x10 = the tail's result
(7) as r1 and computes 10 instead of 7 — the four-way parity fails on
its own, pinning the formula's `c != 1` branch.

### 10.41 M2.15-follow-up gate results (measured)

Total emitted bytes across the now-38-program parity set: **M0 51860
→ M1 47400, 4460 saved** (≈8.5%), up from M2.16's 4312. The new row
on top of M2.16's 4312:

- epi_merge3 1180 → 1032 (−148, new 38th row; M0 baseline measured
  at git 1729f50). Disabling the merge (the fetch-drop hook) grows it
  back to exactly 1036 — the one kept fetch, 4 bytes — the exact
  mirror of epi_merge2's teeth (+4), while epi_merge's two dropped
  fetches measure +8. All teeth states still verify 7.
- Row-by-row M2.15-vs-now accounting (gate tables diffed): all 36
  shared rows byte-identical — the totals move by exactly the two new
  rows (jmpr_cross +220, epi_merge3 +148).
- Four-way parity: interp 40/0, x86 39/0/3, RV64 38/0/4, ARM 38/0/4.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.42 M2.17 amendment — relaxing the epi-merge rigid layout (as built)

M2.15's layout made the two dead-region intermediates at T+2/T+3
rigidly LOADI/LOADI64 — the head is compiled before the source, so the
runtime register state at the head has to come from a statically known
shape. M2.17 relaxes that to **any plain result-op** whose emission is
"reserve + transient fetches + compute + store" — an arithmetic chain,
CMP, LEA or PTRADD result merges exactly like a LOADI did.

Why this is sound: the merge's invariants depend only on the **claim
count**, not on what computes the values. Each accepted op calls
cache_reserve exactly once (ar_is_result_op's contract), so the
pattern's claim plus the two intermediates' claims still net +3 ≡ 0
mod 3, the tail's result host stays the pre-T cursor value cnt%3, and
the drop flags are unchanged. The destination check (d1/d2 distinct
and neither equal to the pattern's sources a1/b1) is what makes the
tail's fetches *real reloads* — a miss against a cache holding exactly
the two intermediate claims. That is the execution-order subtlety: the
head executes *after* the intermediates via the backward fold, but is
program-order *before* them, so "dead result" must be read as "not
claimed onto a guest the head (or tail) fetches" — the d1/d2 ≠ a1/b1
distinctness check, not a program-order liveness scan. The folded
JMPR's index register is deliberately exempt: it may be the computed
intermediate's destination (the flagship epi_merge4 shape), and a
folded JMPR never fetches its index at runtime.

The accepted set is a strict whitelist (ar_is_interm_op), not "any
result-op": **OP_CALL is excluded** (no plain claim — the call-site
scratch clobbers the cache state the invariant argument assumes) and
**OP_LOAD is excluded** (its address math's clobber_scratch paths and
the M2.8–M2.10 folds aren't covered by the invariant). Every other
result-op's emission is plain.

Why a post-hoc fixup variant can't work (the §10.37 emission-order
constraint, restated): the head's drop decisions are baked into
*already-emitted* bytes — the ldr count and host assignments at the
head are fixed by the time the fold source is reached. Patching the
head's fetch layout after the fact is recompilation, which is exactly
the constraint being relaxed; the only sound directions are (a) widen
what the *source* may emit while keeping the head's shape fixed (this
milestone), or (b) compile the head twice against a placeholder — not
worth it for a dead-path shape. The remaining frontier named in §10.37
(the general joint fixpoint) is still open; the emission-order
constraint on epi-merge is unchanged, only its layout is wider.

### 10.43 M2.17 gate results (measured)

Total emitted bytes across the now-39-program parity set: **M0 53008
→ M1 48404, 4604 saved** (≈8.7%), up from M2.16's 4460. The new row
on top of M2.16's 4460:

- epi_merge4 1148 → 1004 (−144, new 39th row; M0 baseline measured
  at git 1729f50). The dead region computes its fold index with an
  arithmetic chain (seed LOADI + ADD) instead of a LOADI; the
  constant-fixpoint analysis folds it, the epi-merge fires with both
  fetches dropped (flags=3 class), and the head is a bare add with
  zero ldr words. Reverting the widening to LOADI-only grows it back
  to exactly 1012 (+8 — the merge no longer fires, head reloads both
  sources), still correct; the other three epi_merge rows are
  byte-identical (their T+3 is still a LOADI).
- Row-by-row accounting (gate tables diffed): all 38 shared rows
  byte-identical — the totals move by exactly the new row.
- Four-way parity: interp 41/0, x86 40/0/3, RV64 39/0/4, ARM 39/0/4.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).
- Coverage pin (follow-up, mirroring how epi_merge3 was teed up in
  §10.39): epi_merge4 exercises exactly **one** computed intermediate
  (T+2 is still a LOADI seed) in the flags=3 class. The
  two-computed-intermediate shape and the flags=1/2 classes with
  computed ops remain unpinned; the formula is shared, so they are
  pins rather than gaps.

### 10.44 M2.17-follow-up — the computed fold index in the flags=2 class, and the two-computed ceiling (as pinned)

This amendment closes the §10.43 coverage pin — **partially**, and
precisely: the flags=2-with-computed clause is now pinned; the
flags=1-with-computed clause remains a follow-up (epi_merge3 pins
flags=1 with LOADI intermediates, so the formula's first branch still
lacks a computed-op witness); and the two-computed-intermediate
clause is closed as a **proven-impossible ceiling**, not a pin.

epi_merge5 is the epi_merge2 mirror with a computed fold index: three
live results (r2, r1, r6) make the pre-T result count 3 ≡ 0 mod 3, so
the tail's result lands in x9 and clobbers the r2 transient — the
head KEEPS its r2 reload and DROPS only the r1 fetch (flags = 2),
with r1 surviving as the tail's transient in x10. The dead region
computes the fold index with an arithmetic chain (seed LOADI r4, #2
then ADD r5, r4, #3 → r5 = 5 = T) — the M2.17 widening at work in a
second cursor class. Dump-verified: the head is `ldr x9, slot2; add
x9, x9, x10` — exactly one ldr. Teeth: disabling the merge grows it
back to exactly 1024 (+4 — the one kept fetch), the exact mirror of
epi_merge2's teeth; all epi_merge rows still verify 7. Four-way
parity: interp 42/0, x86 41/0/3, RV64 40/0/4, ARM 40/0/4; enc-check
clean; jmpr_oob still faults (UDF, rc=1).

**The two-computed ceiling.** The M2.17 widening is *permissive* —
ar_is_interm_op accepts any plain result-op at T+2, and the claim-count
invariant (the pattern's claim plus two intermediates' claims net +3 ≡
0 mod 3) would be satisfied by two computed ops as readily as by one.
The ceiling is the constant-fixpoint analysis, not the invariant: T+2
is necessarily the fold-source BR target — a block head where the
fold fixpoint resets every constant — and every pre-T constant is
additionally wiped at T (rule-1 fold-target mark, from the second
scan onward) and at T+1 (RET). A computed T+2 therefore has no known
source; its result cannot fold; the JMPR's index is unknown, so it
goes DYNAMIC and kills g_alloc for the whole function. A scratch
probe (probe_2c, T+2 = ADD sourcing a pre-T constant) proved it
empirically: the JMPR did not fold and the function emitted the naive
path. The probe was deleted after the measurement — its un-folded
step-budget loop hangs the test runners, so it cannot live in the
corpus. The seed at T+2 must be a LOADI (or LOADI64, the only
constant-establishing ops); **at most one dead-region intermediate
can be computed**. Relaxing this would require the fixpoint to carry
constants across the fold-source entry — the §10.38 joint-fixpoint
future work, not a widening change.

### 10.45 M2.17-follow-up gate results (measured)

Total emitted bytes across the now-40-program parity set: **M0 54176
→ M1 49424, 4752 saved** (≈8.8%), up from M2.17's 4604. The new row
on top of M2.17's 4604:

- epi_merge5 1168 → 1020 (−148, new 40th row; M0 baseline measured
  at git 1729f50). 148 = epi_merge4's 144 plus the extra pre-T decoy
  (r6) the flags=2 cursor class requires. Disabling the merge grows
  it back to exactly 1024 (+4, the one kept fetch — the mirror of
  epi_merge2's teeth, while epi_merge's two dropped fetches measure
  +8). All teeth states still verify 7.
- Row-by-row accounting (gate tables diffed vs committed 8b61abd):
  all 38 shared rows byte-identical — the totals move by exactly the
  two new rows (epi_merge4 1012→1004, M2.17's widening delta;
  epi_merge5 1024→1020).
- Four-way parity: interp 42/0, x86 41/0/3, RV64 40/0/4, ARM 40/0/4.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).
- Remaining follow-up pin: flags=1-with-computed (mirror of how
  epi_merge3 was teed up; the formula and hook are shared, so it is
  a pin rather than a gap).

### 10.46 M2.18 — the two-computed dead region (the §10.44 ceiling, broken)

§10.44 claimed a structural ceiling: T+2 is the fold-source BR target,
and the constant fixpoint resets every constant at that block head, so
a computed T+2 has no known source, the index chain cannot fold, and
at most one dead-region intermediate can be computed. M2.18 breaks
that ceiling with a **targeted fixpoint relaxation** — but only after
the probe proved the ceiling's mechanism first: probe_2c (T+2 = ADD
sourcing the pre-T constant r2, T+3 = SUB of it) emitted the naive
path (1156 bytes) because the JMPR went dynamic and g_alloc died.

**The relaxation rule (as built).** A block head pc X whose ONLY
incoming path is a single **forward** BR/BC edge — X−1 is a terminal
(RET or unconditional BR, so the fall-through edge is dead), no CALL
targets X, X is not an entry pc (the trampoline arrival guarantees
nothing), and no backward branch targets it — is **not a join**: the
constant map as it was at that branch holds at X, so the scan restores
the branch-time snapshot instead of resetting. The eligibility is
computed once (static); the snapshot is taken per-scan at the branch
and restored at the head. Two exclusions keep it sound:

- **Fold targets are excluded per-scan** (g_fold_tgt_prev, built from
the previous scan's COMPLETE fold set and live-updated as the scan
discovers folds): a fold edge into X is another incoming path, so X
must reset. A fold DISCOVERED during the scan at q < X is caught
within the same scan; a backward fold (q > X) is caught one scan
late, exactly like the existing rule-1 machinery.
- **A hard safety cap**: a fold ENABLED BY the relaxation can target
the relaxed head itself (a backward fold re-entering the head makes
it a loop head the relaxation must not apply to) — that shape
2-cycles the fixpoint (relax → fold → reset → un-fold). The corpus
does not exercise it, but a translator must terminate on any input:
if the fixpoint exceeds 512 passes it disables the relaxation and
re-runs the un-relaxed fixpoint from scratch, which is
monotone-decreasing in the fold set and provably terminates.

Why the relaxation is sound where it fires: the scan's constant map at
the branch is valid on every path into the branch, the branch writes
no registers, and the branch edge is X's only incoming path — so the
restored map is valid on every path into X. The deeper structural
reason this stays sound in the presence of loops is the **once-
execution property**: X−1 is a terminal (RET or unconditional BR), so
X can never be re-entered by fall-through, and X has no fold edge
(the per-scan exclusion) — the unique forward BR is X's ONLY path, so
X's block executes AT MOST ONCE per call and the restored constants
are consumed at most once, on the unique BR path, where they hold.
Any loop that could re-enter X's region would have to pass through
X−1 (a terminal), which either returns or jumps away; and any
loop-creating fold derived from X's constants has its target marked,
which severs the X-derived chain on the next scan (target between X
and the JMPR) or re-initializes the chain root (target at/before the
root) — so the shape either oscillates into the cap or self-corrects.
The relaxation inherits (but does not widen) the pre-existing latent
gap of the fixpoint: a constant-index fold into a loop whose index is
loop-variant would be unsound — the corpus avoids such programs and
four-way parity guards them, exactly as before M2.18; folds fed by
relaxed constants are one-shot by the once-execution property, so the
relaxation adds no new exposure. The change is also byte-neutral
across the pre-existing corpus: the row-by-row diff shows every
shared row byte-identical (no eligible head among them changed any
emission — the totals move by exactly the three epi_merge rows).

**epi_merge6 (as pinned).** The same single-edge backward-fold shape
as epi_merge4/5, but with a **dependent two-op chain** computing the
index: ADD r4, r2, #2 then SUB r5, r4, #1 → r5 = (3+2)−1 = 4 = T.
With the relaxation, T+2's block head restores r2 = 3 (the snapshot
at the pc-3 BR), the chain folds, and the JMPR folds BACKWARD to T
with both head fetches dropped (flags=3 — the claim count is still
three, so the invariants are unchanged). Dump-verified: the head is a
bare `add x11, x9, x10` (zero ldr words), the dead region is
`add x9, x9, #2; sub x10, x9, #1`, and the fold branches back to the
head; the tail's fetches evict the computed intermediates and
re-establish the r2/r1 transients exactly as in the LOADI shapes.
This closes the §10.44 pin: the joint-fixpoint direction (§10.38) is
no longer the only path to a two-computed dead region.

### 10.47 M2.18 gate results (measured)

Total emitted bytes across the now-41-program parity set: **M0 55332
→ M1 50432, 4900 saved** (≈8.9%), up from M2.17-follow-up's 4752.
The new row on top of 4752:

- epi_merge6 1156 → 1008 (−148, new 41st row; M0 baseline measured
  at git 1729f50). 148 = the full two-computed dead region folding
  with both head fetches dropped. Disabling the relaxation grows it
  back to exactly 1156 (the JMPR dynamic, g_alloc = 0, the whole
  function naive) — the same 148 the M0 baseline measures, and the
  probe's pre-fix size. All teeth states still verify 7.
- Row-by-row accounting (gate tables diffed vs committed 8b61abd):
  all 38 pre-M2.17 shared rows byte-identical — the totals move by
exactly the three epi_merge rows (epi_merge4 1012→1004 and
epi_merge5 1024→1020, M2.17's widening deltas; epi_merge6
1156→1008, this milestone's relaxation). The relaxation itself is
byte-neutral across the pre-existing corpus.
- Four-way parity: interp 43/0, x86 42/0/1, RV64 42/0/1, ARM 42/0/1
  (the three skips are the pre-existing float_ops RV64/ARM and
  mem_ops x86 limitations). enc-check clean; jmpr_oob still faults
  (UDF, rc=1).

### 10.48 M2.19 — the leaf-callee return value (as built)

M2.18's relaxation excluded CALL targets: a call edge was treated as
carrying an unknown callee-modified map. M2.19 shows that framing was
over-conservative — but the win is at the RETURN POINT, not the call
target, and it rests on a structural fact of this SIMI's fresh-frame
model. The interpreter's CALL zeroes the callee's frame, copies r0-r7
as arguments, and on RET propagates ONLY r0 (the return value) back to
the caller; the emitted code matches (the callee's ENTER prologue
zeroes all 64 slots + tags, then the call site marshals r0-r7 from the
outgoing-arg area). So the caller's register map is **structurally
preserved by a call except r0** — a probe confirmed a JMPR keyed on r2
survives a call and folds with no new machinery. What was genuinely
unknown was r0 itself: the fixpoint cleared it (the return register),
so a JMPR keyed on the RETURN VALUE never folded.

The first cut: **leaf-callee return-value analysis**. For each CALL
site, if the callee is a straight-line leaf — starts with ENTER, no
BR/BC/JMPR/CALL before its first RET — the fixpoint's own constant
step (factored into ar_const_step, shared with the main scan) is run
over the callee body against the fresh-frame map (r0-r7 unknown, r8+ =
0, zeroed by the prologue). If r0 at the RET is a compile-time
constant, the caller's r0 after the call is that constant, and a JMPR
keyed on it folds. The analysis is per-callee with the args unknown, so
a leaf returning a value computed from its ARGS is conservatively not
folded (the call-site-dependent seeding of the arg constants is the
natural follow-up); the ENTER requirement is what makes the r8+ = 0
seeding sound in the EMITTED code (a callee without ENTER would read
the caller's stale frame there, diverging from the interpreter).

Why this is sound: the callee is straight-line, so every execution of
it takes the same path and computes the same r0 from the same
constant/zeroed inputs — independent of the caller's state; and the
fresh-frame model makes the caller's other registers unaffected, so the
post-call map is exactly the pre-call map with r0 replaced by the
analyzed constant. jmpr_callret pins it: ENTER, LOADI r0 = 3, RET; the
JMPR r0 folds to pc 3 (a fall-through fold — the branch is dropped and
the call site flows directly into the target's ADD), g_alloc stays 1
for the whole function.

### 10.49 M2.19 gate results (measured)

Total emitted bytes across the now-42-program parity set: **M0 57368
→ M1 52316, 5052 saved** (≈8.8%), up from M2.18's 4900. The new row
on top of 4900:

- jmpr_callret 2036 → 1884 (−152, new 42nd row; M0 baseline measured
  at git 1729f50). 152 = the whole function's naive-to-cache delta:
  the JMPR's dynamic dispatch (runtime table + bounds check) is gone,
  the branch is a dropped fall-through, and g_alloc stays 1. Disabling
  the leaf analysis grows it back to exactly 2036 (the JMPR dynamic,
  g_alloc = 0, the runtime dispatch table), still correct — the teeth.
- Row-by-row accounting (gate tables diffed vs committed abc0a46):
  all 41 shared rows byte-identical — the totals move by exactly the
  new row. The analysis is byte-neutral across the pre-existing
  corpus: no existing call test's fold decisions change (call_ret,
  cap_call_ret and aggregate_abi all measure identically).
- Four-way parity: interp 44/0, x86 43/0/1, RV64 43/0/1, ARM 43/0/1
  (the three skips are the pre-existing float_ops RV64/ARM and
  mem_ops x86 limitations). enc-check clean; jmpr_oob still faults
  (UDF, rc=1).
- Open follow-up (recorded, not a gap): the args-unknown restriction —
  seeding the callee's r0-r7 with the CALLER's constants at each call
  site would fold leaves that return a function of their (constant)
  arguments, at the cost of making the analysis call-site-dependent.

### 10.50 M2.20 — the call-site-aware leaf analysis (as built)

The recorded M2.19 follow-up, built. The analysis is now
call-site-dependent: the precompute shrinks to a per-callee LEAF MARKER
(ar_leaf_ret_pc — ENTER start, straight-line body, first RET; the
M2.19 reviewer hardening against mid-body ENTERs and the
BR/BC/JMPR/CALL rejection are preserved), and the fixpoint's CALL
branch runs the SAME ar_const_step body walk at each call site against
an r8+ = 0 scratch map (file-scope, like the M2.18 snapshots) whose
r0-r7 are seeded from the CALLER's constant map. Sound because the
call-site marshaling copies the caller's live r0-r7 verbatim (in both
the interpreter and the emitted code), so a constant the caller's map
attributes to an argument register is a constant the callee reads; the
once-execution and block-head disciplines that make the caller's map
sound at any pc make it sound here. Two ordering lessons, both in the
code comments: the seed must snapshot the caller's r0 BEFORE the
"return value is unknown" clear (the same snapshot-before-clear trap
as M2.18's branch snapshot — the first build cleared r0 first, killing
arg0, and the probe caught it), and the args-unknown case is just the
all-unknown seed — so jmpr_callret's fold survives byte-identically.

### 10.51 M2.20 gate results (measured)

Total emitted bytes across the now-43-program parity set: **M0 60712
→ M1 55332, 5380 saved** (≈8.9%), up from M2.19's 5052. The new row
on top of 5052:

- jmpr_callret_arg 3344 → 3016 (−328, new 43rd row; M0 baseline
  measured at git 1729f50). TWO call sites to the SAME leaf pin the
  per-site dependence: the leaf returns arg0 + arg1; main passes
  3 + 5 = 8 and its JMPR folds to pc 8, other passes 8 + 10 = 18 and
  its JMPR folds to pc 18. Both folds are REAL branches (unreachable
  filler at pc 7 / pc 17, so neither target is pc+1; the coalescing
  pre-pass does not fire; rule 1 marks pc 8 and pc 18), and both are
  load-bearing — g_alloc requires EVERY JMPR in the stream to fold,
  so a single failed site would throw the whole program back to the
  naive path. Disabling ONLY the arg seeding (args stay unknown — the
  M2.19 behavior) grows it back to exactly 3344 while jmpr_callret
  stays byte-identical at 1884, both still correct — the teeth,
  isolating the arg-seeding delta (2 × 164) exactly.
- Row-by-row accounting (gate tables diffed vs committed c4ee7fc):
  all 42 shared rows byte-identical — the totals move by exactly the
  new row. The analysis is byte-neutral across the pre-existing
  corpus: no existing call test's fold decisions change (jmpr_callret
  folds identically under the all-unknown seed; call_ret, cap_call_ret
  and aggregate_abi measure identically).
- Four-way parity: all 46 common tests pass on all four engines (the
  three skips are the pre-existing float_ops RV64/ARM and mem_ops x86
  limitations). enc-check clean; jmpr_oob still faults (UDF, rc=1).
- Convergence (honest framing): the call-site analysis joins the
  pre-existing latent 2-cycle class — a backward fold into a region
  that seeds another fold can oscillate (fold -> reset -> un-fold ->
  re-fold), and the M2.18 cap's fallback re-runs the UN-relaxed
  fixpoint, which is monotone-decreasing in the fold set and provably
  terminates under the accumulating block-head marks; the call-site
  seeds only shrink as marks accumulate, and the walk never invents a
  value — a partially-known walk result is UNKNOWN, never a different
  constant — so a call's r0 result is monotone known -> unknown, and
  the same argument covers them. The corpus exercises neither shape;
  the cap is the translator's termination guarantee on any input.
- Remaining restriction (recorded, not a gap): the walk is
  straight-line only, so a leaf with a branch, a nested call, or a
  load/store-dependent return is conservatively not folded; and the
  call-site walk re-runs every fixpoint pass (deterministic and cheap
  — the leaf is rejected upfront unless straight-line).

### 10.52 M2.21 — reachability-scoped g_alloc (as built)

The g_alloc gate was "every JMPR in the linear stream folds" — but the
fixpoint scans the whole stream, including functions that are never
entered, so a single non-folding JMPR in an UNREACHABLE function threw
the whole program back to the naive path. M2.21 scopes the gate to
REACHABLE JMPRs. The reachability pre-pass is a worklist BFS from the
entries. It follows the static control edges — BR/BC targets +
fall-through, CALL target + fall-through (the call returns), RET is a
terminal — AND the FOLD edges, because it runs AFTER the fold fixpoint:
a FOLDED JMPR's target is a runtime edge (a direct branch; the M2.12
fall-through fold's target pc+1 is reached by falling through), while
an UNFOLDED JMPR is a terminal (its runtime target is data-dependent —
only whether ITS OWN pc is reachable matters to the gate). The gate
then requires every reachable JMPR to fold; unreachable ones keep their
dynamic dispatch path in the emission (correct code, never executed).

The FOLD EDGES are the soundness-critical part (M2.21 reviewer
finding). A reachable fold can dispatch into a statically-unreachable-
looking region — the fold index is just a constant, any pc. Without
the fold edge in the pre-pass, a non-folding JMPR THERE would be marked
unreachable, g_alloc would stay 1, and the runtime dispatch from that
region could land mid-chain with a stale cache directory — the exact
hazard the gate exists to prevent. Rule 1 makes the region's ENTRY
cache-safe (the fold target is a block head), but the region's own
non-folding JMPR still dispatches to arbitrary pcs at runtime, so it
must gate. With the fold edges, the reachable set is complete for the
gating question. The cache correctness argument is otherwise PER-PATH
and independent of reachability (rule 1 re-fetches at every branch/call
/fold target), so reachability only changes the OPTIMIZATION gate,
never the emitted code's correctness; and the conservative direction
holds — a reachable non-folding JMPR still gates (jmpr_oob, jmpr_dyn,
their JMPRs in main, measure byte-identically).

### 10.53 M2.21 gate results (measured)

Total emitted bytes across the now-45-program parity set: **M0 66848
→ M1 61324, 5524 saved** (≈8.3%), up from M2.20's 5380. Two rows
added:

- jmpr_unreach 3044 → 2948 (−96, new 44th row; M0 baseline measured
  at git 1729f50). `dead` (pc 10) is an unreachable function whose
  JMPR r7 reads an ENTER argument — unknown, so it NEVER folds; before
  M2.21 it killed g_alloc for the whole program, and main's own
  foldable JMPR (r0 = 8 after the M2.20 leaf call) was thrown away.
  With the scoped gate, main's JMPR folds to pc 8 (dump-verified: a
  direct `b` over the filler; dead's dispatch path — table + br + UDF
  — is still emitted, never executed). Reverting the reachability gate
  grows it back to exactly 2996 (main's JMPR dynamic, g_alloc = 0,
  whole function naive), still correct — the teeth. The remaining 48
  of the 96 below M0 is M2.14 tail-reuse across the program's three
  RETs (M0 has none).
- jmpr_foldreach 3092 → 3044 (−48, new 45th row; M0 baseline measured
  at git 1729f50) — the SOUNDNESS pin, byte-identical to the M2.20
  tree's row: main's JMPR r0 = 10 folds to pc 10 INSIDE `dead`, and
  dead's JMPR (index = DIV 14/2 = 7, which never folds) is reachable
  VIA THE FOLD, so it gates. Under M2.20's all-JMPRs rule it gated by
  the old rule's conservatism (every JMPR in the stream); the closure
  is what keeps it gated under the scoped rule — removing the fold
  edge from the pre-pass drops the row to exactly 2988 (−56, the
  buggy g_alloc=1 emission where dead's JMPR hides from the gate),
  still passing in this construct but unsound in general — the teeth.
  The 48 below M0 is the naive-path machinery (pc 6's fold as a
  direct branch, M2.14 tail-reuse), not a cache win.
- Row-by-row accounting (gate tables diffed vs committed 2b2e8e6):
  all 43 pre-existing rows byte-identical and jmpr_foldreach
  byte-identical (3044) — exactly ONE row moved, jmpr_unreach (2996
  → 2948), the M2.21 delta (+48 saved). The scoping is byte-neutral
  across the pre-existing corpus: every pre-existing non-folding JMPR
  is reachable (jmpr_oob, jmpr_dyn), so no fold decision changes.
- Four-way parity: all 48 common tests pass on all four engines (the
  three skips are the pre-existing float_ops RV64/ARM and mem_ops x86
  limitations). enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.54 M2.22 — per-pc live-predecessor reachability in the emission (as built)

M2.21 put reachability in the GATE (only reachable non-folding JMPRs
forfeit the cache). M2.22 puts it in the EMISSION: the same fold-aware
BFS now counts, per pc, the number of LIVE incoming edges
(`g_npred[]` — every edge the BFS follows increments its target's
count; entries are roots, reachable with count 0). A block with ZERO
live predecessors is entered by no runtime path under g_alloc=1 (the
closure argument of §10.52: with the cache on, every reachable JMPR
folds, so the reachable set is exact — a dynamic dispatch would imply
a reachable non-folding JMPR that gates). The emission therefore treats
dead code as dead:

- **Dead-region start invariant.** The first unreachable pc after a
  live one (or the stream start) is compiled with an EMPTY initial
  cache directory — the same state any cold entry sees. In practice
  the directory is already empty there (a dead region's linear
  predecessor is a terminal whose own flush spilled it), so the reset
  never emits a spill: it makes the contract structural rather than
  incidental, so a dead region's bytes cannot drift with the reachable
  residue preceding it, and a region that later becomes reachable
  (analysis change) is already emitted in its correct cold shape.
- **The frame load is dropped.** An UNREACHABLE function's ENTER never
  runs, so its `cache_flush + emit_prologue` (~197 words of frame
  setup — sub/str/str/add/sub, 64 slot zeroes, 64 tag zeroes, arg
  marshaling) is dead code and is not emitted. Dump-verified:
  `off[ENTER] == off[ENTER+1]` — the ENTER emits ZERO words. The
  frame the prologue would have built is the caller's, already in
  place for any fold that lands past this ENTER (jmpr_foldreach's
  shape); a fold landing exactly ON the ENTER keeps it reachable, so
  the drop never fires for an entered frame.
- **Dead flushes are skipped.** Every `cache_flush` whose pc is dead
  (BR/BC/CALL/JMPR/RET/RESOLVE/OBJSIZE) or whose target block is dead
  (the rule-1 boundary flush) is dead weight and is skipped, via one
  guard (`flush_owed(pc) = !g_alloc || g_reach[pc]`; naive mode is a
  no-op either way). The spill words are CONSERVED when reachable code
  follows (the first flush at a live head after the region spills the
  deferred residents — same count), and genuinely SAVED when the dead
  region ends the stream (the deferred spill never fires).

Soundness rests on the same per-path argument as §10.52: reachability
only decides whether dead words are emitted, never the emitted code's
correctness. The conservative direction holds — reachable code is
emitted exactly as before (flush_owed is 1 for every reachable pc), and
the dead-region reset only touches dead pcs. Naive-mode programs
(g_alloc=0) are byte-identical (all guards are no-ops there) —
jmpr_foldreach measures 3044 unchanged.

### 10.55 M2.22 gate results (measured)

Total emitted bytes across the now-46-program parity set: **M0 69924
→ M1 61912, 8012 saved** (≈11.5%), up from M2.21's 5524. One row
added, five moved:

- jmpr_deadmult 3076 → 2188 (−888, new 46th row; M0 baseline measured
  at git 1729f50) — the dedicated pin. `dead` (pc 13) is an
  unreachable MULTI-BLOCK function at the stream end (leaf's RET at pc
  12 is a terminal), with an internal BR to a second block — so its
  whole body (ENTER frame-load prologue, the BR's spill of r4, the
  label's dynamic JMPR path) is correct-but-dead code. M2.22 drops
  the frame load (the ENTER emits ZERO words) and the dead flushes
  (the BR's first word is the b itself — no spill; r4 is never read
  reachable and the region is at the stream end, so the deferred spill
  never fires). The 792 below the M2.21 emission is exactly the
  197-word prologue + the dead BR's spill; the remaining 96 below M0
  is M2.14 tail-reuse + fold machinery. Reverting the
  reachability-aware emission (flush_owed → always flush) grows it
  back to exactly 2980 (the M2.21 bytes, still correct) — the teeth.
- jmpr_unreach 3044 → 2160 (−884, from 2948): M2.22 drops `dead`'s
  ENTER frame-load prologue (788 = 197 words) — the M2.21 win's
  emission side. Teeth: revert only the emission → back to exactly
  2948, the M2.21 bytes.
- jmpr_callret_arg 3344 → 2224 (−1120, from 3016): `other` (pc 10),
  the second caller to the leaf, is UNREACHABLE (main's RET exits,
  nothing targets pc 10) — its ENTER prologue (788) and its folded
  JMPR's flush (4) drop. This is a pre-existing row shrinking under
  the new rule, not a fold change: other's fold edges stay dead (the
  BFS is source-gated, so a dead fold does not resurrect its target).
- jmpr_calc/jmpr_calc_bit/jmpr_calc_mul 1040→1036 / 1084→1080 /
  1036→1032 (−4 each): each has an unreachable dispatch block after
  its fold target (the LOADI/RET dead paths); a dead RET's flush is
  skipped.
- jmpr_fall 1016 → 1008 (−8): the dead path at the stream end (pcs
  9-14) loses two flushes — the rule-1 boundary spill of r4 (the
  fall-through fold's target has another edge, so its head is live in
  g_pc_target but dead in g_reach) and the backward fold's spill of
  r5, both at the stream end so never deferred.
- Row-by-row accounting (gate tables diffed vs committed e9e3eb2,
  measured with the M2.21 verifier): all other 40 shared rows
  byte-identical — M2.22 is byte-neutral over every program with no
  dead region (and over every naive-mode program).
- Four-way parity: 144 PASS, 0 FAIL across all engines (the three
  pre-existing skips unchanged). enc-check clean; jmpr_oob still
  faults (UDF, rc=1).

### 10.56 M2.23 — whole-body dead-code elimination (as built)

M2.22 treated dead code as dead for the FRAME LOAD and the FLUSHES but
still emitted the dead bodies. M2.23 goes the rest of the way: a pc
with ZERO live predecessors never executes under g_alloc=1 (the §10.52
closure argument again: with the cache on, every reachable JMPR folds,
so the reachable set is exact), so its ENTIRE body is not emitted. The
emission loop's first act is the elimination skip —
`if (g_alloc && !g_reach[pc] && g_npred[pc] == 0) { g_instr_off[pc] =
cb.len; continue; }` — after which only live pcs reach the dispatch:
ENTER frame loads, arithmetic, branches, CALL sites, dynamic JMPR
table paths, and RETs of dead functions simply do not appear.

The reachable-emission machinery SIMPLIFIES rather than accumulates:

- The rule-1 boundary flush moves from the end of the loop to the
  head of each block (`if (pc >= 1 && g_pc_target[pc]) cache_flush`),
  landing at the same byte position (directly after the previous
  instruction's code) and byte-identical for every reachable path.
  M2.22's dead-head skip needs no guard: a dead head never reaches
  the line (eliminated), and a live head always owes the flush
  (g_reach is exact under the closure).
- M2.22's `flush_owed` guards and its region-start reset are REMOVED
  as subsumed: with elimination, every emitted pc is live, so every
  internal flush is owed and every dead region is simply absent (no
  cold shape to compile). M2.23 minus the elimination is
  byte-identical to M2.21 (measured), which is also the teeth.
- The JMPR jump table disappears from every g_alloc=1 program: a
  dynamic path is only emitted by a non-folding JMPR, and under
  g_alloc=1 every EMITTED JMPR folds (the non-folding ones are
  dead — eliminated, table and all). The table machinery survives
  only for naive mode, byte-identical to M0.
- The M2.14 tail ownership naturally stays on live RETs: dead RETs
  never reach the OP_RET case (they are eliminated), so the first
  LIVE RET emits the shared return tail in place and the rest branch
  to it. This is what keeps jmpr_calc correct — its dead RET was
  previously the tail's owner.

`g_instr_off[pc]` still records a byte position for a dead pc (the
next live code) so the table and patch passes stay well-defined;
nothing ever dispatches to it. Naive mode is untouched (a dynamic
dispatch can land anywhere, so no pc is provably dead). The pc→offset
mapping, the patch pass, and the fixpoint marks are all pc-based and
need no adjustment — only the emission skips.

### 10.57 M2.23 gate results (measured)

Total emitted bytes across the now-47-program parity set: **M0 73240
→ M1 63032, 10208 saved** (≈13.9%), up from M2.22's 8012. One row
added, ten moved:

- jmpr_deadfull 3316 → 1964 (−1352, new 47th row; M0 baseline measured
  at git 1729f50) — the dedicated pin. `deadfull` (pc 10) is an
  unreachable function with a rich body (arithmetic chain computing
  600, a CALL to the leaf, a BR to a second block, and a JMPR whose
  600 index is out of range, so its dynamic path — and, as the only
  dynamic JMPR in the stream, the whole 176-byte table — is dead).
  M2.23 drops the entire function: 1964 = main + leaf + trampoline.
  Teeth: disabling the elimination grows it back to exactly 3204 (the
  M2.21 emission — M2.23 minus the elimination is byte-identical to
  M2.21, since the hoisted flush and the reverted guards are
  byte-neutral; the 1240-byte delta is the elimination alone).
- The four dead-function rows now land on the same 1964-byte live
  core (main + leaf + trampoline): jmpr_unreach 2160 → 1964 (−196:
  dead body + 128-byte table), jmpr_deadmult 2188 → 1964 (−224: dead
  body + 144-byte table), jmpr_callret_arg 2224 → 1964 (−260: the
  whole `other` body — no table, its JMPR folds), and jmpr_deadfull.
- Partial dead regions shrink to nothing: jmpr_calc 1036 → 1012
  (−24: the dead LOADI/RET dispatch blocks), jmpr_calc_bit 1080 →
  1052 (−28), jmpr_calc_mul 1032 → 984 (−48), jmpr_fall 1008 → 980
  (−28: the whole dead path at the stream end), jmpr_mid 1008 → 992
  (−16: its dead path), jmpr_basic 960 → 944 (−16), jmpr_cross 992 →
  988 (−4: its dead RET).
- Row-by-row accounting (gate tables diffed vs committed 7783483,
  measured with the M2.22 verifier): all other 36 shared rows
  byte-identical — M2.23 is byte-neutral over every program with no
  dead code (and over every naive-mode program, jmpr_foldreach 3044
  unchanged). The elimination is the entire M2.23 delta: with it
  disabled, every row returns to its M2.21 byte count exactly.
- Four-way parity: 147 PASS, 0 FAIL across all engines (the three
  pre-existing skips unchanged). enc-check clean; jmpr_oob still
  faults (UDF, rc=1).

### 10.58 M2.24 — the compact JMPR table (as built)

M2.23 made the JMPR jump table disappear from every g_alloc=1 program
(no dynamic path is emitted, so no table is needed). The table that
remains lives only under g_alloc=0 (naive) — a reachable non-folding
JMPR's runtime index is data, so every pc in [0, num_instr) is a
possible target and all num_instr entries are required. M2.24 shrinks
that table and its access paths:

- **4-byte entries.** M2.23's 8-byte entries mirrored RV64's 64-bit
  slots, but the entry is a byte offset into out_buf — `g_instr_off[pc]`
  and `cb.len` are `uint32_t` by construction, and the harness maps the
  code buffer at VA 0, so one 32-bit `ldr W` per entry suffices. The
  dispatch becomes `t2 = base + (target << 2); t2 = (u32)[t2]; br t2`
  (the 32-bit load zero-extends — offsets are positive). The table is
  num_instr x 4 bytes, exactly half of M2.23's.
- **2-word base load.** The table-base placeholder shrinks from a
  4-word movz+3xmovk li64 to a 2-word movz+movk (the base is also a
  32-bit offset — `cb.len` at the table's position, < 2^32 always).
  Saves 8 bytes per dynamic JMPR.
- **Bounds check unchanged.** `t0 < num_instr; cset lo; cbz -> UDF #0`
  is the ISA §16 CFI requirement, untouched — jmpr_oob still faults.

The g_alloc=1 elimination of the table (M2.23) is now the two-sided
story: when every reachable JMPR folds there is nothing to dispatch
with, and when one doesn't, the dispatch path is minimal. The naive
path stays byte-identical to M0 except for the table's width and the
base load's width — both pure size wins, no semantic change (verified
by the four-way parity executing the compact table at runtime).

### 10.59 M2.24 gate results (measured)

Total emitted bytes across the now-48-program parity set: **M0 74584
→ M1 64032, 10552 saved** (≈14.1%), up from M2.23's 10208. One row
added, four moved:

- jmpr_table 1344 → 1236 (−108, new 48th row; M0 baseline measured at
  git 1729f50) — the dedicated pin. Two DYNAMIC JMPRs (indices stored
  to and re-loaded from guest memory — never fold, g_alloc=0)
  dispatch SEQUENTIALLY through two distinct table entries in one
  run: table[7] -> block A, table[13] -> block B, each with a passing
  bounds check. The M2.23-era 8-byte table measures 1312 (+76 = 15x4
  table + 2x8 base) — the teeth, isolating the width delta exactly.
- jmpr_dyn 1132 → 1088 (−44 = 9x4 + 8), jmpr_mix 1248 → 1184 (−64 =
  14x4 + 8), jmpr_foldreach 3044 → 2964 (−80 = 18x4 + 8), jmpr_join
  1144 → 1096 (−48 = 10x4 + 8) — the naive-mode table-bearing rows,
  each shrinking by exactly num_instr x 4 (table) + 8 (base load).
- Row-by-row accounting (gate tables diffed vs committed 3d8d52e,
  measured with the M2.23 verifier): all other 43 shared rows
  byte-identical — M2.24 touches ONLY the naive-mode dynamic dispatch
  (every g_alloc=1 row, including the 1964-byte dead-code core,
  unchanged).
- Four-way parity: 150 PASS, 0 FAIL across all engines (the compact
  table executes at runtime on the ARM engine; the three pre-existing
  skips unchanged). enc-check clean; jmpr_oob still faults (UDF,
  rc=1, now at a slightly earlier pc — the table is smaller).

### 10.60 M2.25 — the inline compare-and-branch dispatch chain (as built)

M2.24 left the naive-mode table at its floor: num_instr x 4 bytes plus
an 8-byte base load per dynamic JMPR. M2.25 removes the table and the
indirect load entirely when the dispatch is provably small — a naive
mode program (g_alloc=0) with EXACTLY ONE dynamic (non-folding) JMPR
whose index register's possible values are a small, provable constant
set. The dispatch becomes an inline chain — `ld t0, slot(ra); cmp
subs xzr, t0, #c0; b.eq pc(c0); cmp ...; b.eq pc(ck); UDF #0` — one
cmp + b.eq pair per candidate, straight to the target's code, with
UDF #0 as the fall-through.

- **The candidate-set analysis (new).** A forward walk over the linear
  stream tracks only the index register's constant set, with the SAME
  join discipline as the fold fixpoint (constants die at block heads)
  — but a head's set is the UNION of its incoming edges' sets, because
  the chain must cover every path's value, not fold a single one
  (jmpr_join's index is 8 on the branch path and 9 on the fall-through:
  set = {8, 9}). Branch deliveries accumulate per-head arrival sets;
  the walk iterates to a fixpoint (sets only grow, collapsing to
  UNKNOWN at the TX_AR_CHAIN_MAX=8 cap, so it converges; a backward
  branch's delivery to an already-walked head is picked up next pass).
  Any writer that is not a known constant — LOAD, CALL result,
  register-form ALU, MUL/DIV/AND/OR/XOR, shifts — marks the register
  UNKNOWN and no chain fires (jmpr_dyn/mix/foldreach, whose indices
  come from LOAD/DIV, keep the table; jmpr_table has two dynamic
  sites, which disqualifies it outright). ADD/SUB-immediate and CMP
  (0/1) are set-computable.
- **Soundness.** The chain's fall-through UDF fires exactly for indices
  the analysis proves impossible — any deviation would contradict a
  per-edge constant set — and candidates >= num_instr get no branch
  and land on the UDF, matching the table's bounds-check fault. The
  walk's linear state is built only from real edges (a non-head pc
  after a terminal is either a branch target whose union resets it, or
  unreachable; the single dynamic JMPR is reachable by construction —
  it is what forced g_alloc=0), so a dead region cannot taint the
  dispatch set. The B.cond opcode (0x54000000, cond@15:12) and the
  subs-xzr cmp are new to the emitted set — a64_exec.c decodes and
  executes both, a64_enc_check.py re-derives the encodings
  independently, and the fixup pass gained a FIX_B_COND kind.
- **Degenerate chains.** jmpr_oob's index is the constant 999 —
  provably out of range, zero in-range candidates: the chain collapses
  to a bare UDF #0 (the bounds check and table are gone; the fault is
  identical, verified rc=1).

The table under g_alloc=0 is now conditional: it exists only when the
single dynamic JMPR's set is unknowable or too large, or when there
are two or more dynamic sites. When the chain fires, g_njmpr_li_pos
stays 0 and no table is emitted at all.

### 10.61 M2.25 gate results (measured)

Total emitted bytes across the now-49-program parity set: **M0 75840
→ M1 65088, 10752 saved** (≈14.2%), up from M2.24's 10552. One row
added, one moved:

- jmpr_chain 1256 → 1116 (−140, new 49th row; M0 baseline measured at
  git 1729f50) — the dedicated pin. The index's provable set is {10,
  12} — the union of the never-taken BC's branch-path value (10,
  delivered at the join) and the taken fall-through's (12) — and the
  runtime index 12 takes the chain's SECOND b.eq, proving the chain
  dispatches past the first candidate (jmpr_join's runtime 8 is the
  first candidate of {8, 9}).
- jmpr_join 1096 → 1036 (−60) — the only shared row that moved. The
  chain replaced the M2.24 table (10x4 + 8) and the bounds-check
  dispatch words; the teeth (chain activation disabled) restores
  exactly 1096, and jmpr_chain's teeth version (table path) measures
  1192 (+76).
- Row-by-row accounting (gate tables diffed vs committed b945e16,
  measured with the M2.24 verifier): all other 47 shared rows
  byte-identical — jmpr_dyn/mix/foldreach/table keep their M2.24
  bytes exactly (unknown index sets / two dynamic sites), and every
  g_alloc=1 row is untouched.
- Four-way parity: 196 PASS, 0 FAIL across all engines on the 49
  gated programs (the b.eq chain executes at runtime on the ARM
  engine; the three documented float/mem engine skips unchanged).
  enc-check clean (B.cond now decodes in the independent net);
  jmpr_oob still faults (UDF, rc=1, now at the bare chain word).

### 10.62 M2.26 — the chain follows a register-built index (as built)

M2.25's candidate-set walk tracked ONLY the index register, so an
index built by a register-form ADD/SUB — `LOADI r2, #c; ADD r1, r1, r2`
— was opaque: the ADD's source was untracked, r1's set collapsed to
UNKNOWN, and the program kept the table. M2.26 generalizes the walk to
multiple registers without touching the emission:

- **The tracked-register closure.** Before the walk, a pre-scan
  computes the registers that can TRANSITIVELY feed ra: ra, plus every
  operand of an instruction whose rd is tracked (iterated to
  stability, capped at TX_AR_CHAIN_REGS=4 — beyond that the analysis
  declines). The operand tests are opcode-gated (ar_has_reg_ra /
  ar_has_reg_rb): LOADI/LOADI64/BR/CALL carry no register source in
  w_ra, and rb is a register only in the non-immediate binary-ALU
  forms, so the immediate bits are never mistaken for registers. Each
  addition is individually capped — the first closure round can name
  several feeders at once, and the per-head arrival arrays and the
  walk state are sized to the cap (the teeth caught this as a
  would-be out-of-bounds).
- **Per-register walk state.** cur[] and the per-head arrivals become
  one ChainSet per tracked slot; the head-join unions every slot, and
  BR/BC/folded-JMPR deliveries carry every slot. The set-update rules
  now apply to any tracked rd: LOADI {imm}, LOADI64 {literal},
  MOV copies its source's set, CMP {0, 1}, and ADD/SUB compute the
  image over the SOURCES — S(rd) = {a + imm} for the immediate form,
  or the pair product {a + b : a in S(ra), b in S(rb)} for the
  register form, with the cap collapse if either source is untracked
  or unknown. Every other writer (LOAD, MUL/DIV/AND/OR/XOR, shifts,
  RESOLVE...) still marks its set UNKNOWN — conservative, and the
  reason jmpr_dyn/mix/foldreach keep the table. (STORE is now
  correctly not treated as a writer — its rd is the value source — a
  small soundness improvement over M2.25's over-conservative guard,
  byte-neutral on the corpus.)
- **Unchanged soundness argument.** The union-at-joins discipline, the
  real-edges-only linear state, the UDF fall-through, and the
  out-of-range collapse are exactly M2.25's; only the tracked
  register count grew, and a closure of size 1 reproduces M2.25
  byte-for-byte (verified: all 49 shared gate rows byte-identical).

### 10.63 M2.26 gate results (measured)

Total emitted bytes across the now-50-program parity set: **M0 77088
→ M1 66196, 10892 saved** (≈14.1%), up from M2.25's 10752. One row
added, zero moved:

- jmpr_chain2 1248 → 1108 (−140, new 50th row; M0 baseline measured
  at git 1729f50) — the dedicated pin. The dispatch index is r1 = 2 +
  r2 with the join-dependent base r2 in {8, 10} (never-taken branch
  path / taken fall-through), so the candidate set is the pair product
  {2} + {8, 10} = {10, 12} across the two tracked registers, and the
  runtime index 12 takes the chain's SECOND b.eq — jmpr_chain's
  second-candidate shape, now through a register ADD instead of a
  bare LOADI. The teeth (register-form image disabled) grows it back
  to exactly 1184 (the table path, still correct) — and exposed the
  closure's per-round over-cap, fixed before it could overflow.
- Row-by-row accounting (gate tables diffed vs committed 579c5ad,
  measured with the M2.25 verifier): **all 49 shared rows
  byte-identical** — M2.26 is strictly additive; the closure is
  {ra} alone for every pre-existing row, so nothing else moved.
- Four-way parity: 200 PASS, 0 FAIL across all engines on the 50
  gated programs (the register-ADD chain executes at runtime on the
  ARM engine; the three documented float/mem engine skips unchanged).
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.64 M2.27 — the chain follows a MUL-built index (as built)

M2.26's pair-product image covered ADD/SUB only, so an index built by
multiplication — `MUL r1, r2, #k` — was still opaque: the MUL writer
marked its set UNKNOWN and the program kept the table. M2.27 extends
the image to MUL with the same argument the M2.2 fold used: plain
64-bit multiply never faults and is type-agnostic, so its value set
is exactly the product of its sources' sets.

- **chain_img_alu, shared.** The ADD/SUB and MUL images are now one
  helper: for each operation, S(rd) = {a op imm} for the immediate
  form and {a op b : a in S(ra), b in S(rb)} for the register form,
  with the cap collapse if either source is untracked or unknown.
  The closure already followed MUL (its operands are registers, so
  the transitive feeder scan picks them up — no closure change
  needed). Every other writer still marks UNKNOWN.
- **jmpr_chain3, the pin.** `r1 = r2 * 2` with the join-dependent
  base r2 in {5, 6} — the candidate set is the MUL image {5, 6} × 2
  = {10, 12}, and the runtime index 12 takes the chain's SECOND
  b.eq (dump-verified: `madd x9, x9, x10, xzr` computes r2*2, then
  `subs xzr,x9,#10 / b.eq`, `subs xzr,x9,#12 / b.eq`, `udf #0`).
- **Unchanged soundness argument.** Union-at-joins, real-edges-only
  linear state, UDF fall-through, out-of-range collapse — exactly
  M2.25/M2.26's; only the image's opcode set grew, and the teeth
  (MUL dropped from the image) grows jmpr_chain3 back to exactly
  1184 (the table path, still correct).

### 10.65 M2.27 gate results (measured)

Total emitted bytes across the now-51-program parity set: **M0 78336
→ M1 67304, 11032 saved** (≈14.1%), up from M2.26's 10892. One row
added, zero moved:

- jmpr_chain3 1248 → 1108 (−140, new 51st row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The teeth (MUL image
  disabled) grows it back to exactly 1184 (the table path, still
  correct).
- Row-by-row accounting (gate tables diffed vs committed 119bcda,
  measured with the M2.26 verifier): **all 50 shared rows
  byte-identical** — M2.27 is strictly additive; MUL appears in the
  corpus only inside jmpr_calc_mul (whose constant fold predates
  M2.26 and does not feed a chain), so nothing else moved.
- Four-way parity: 204 PASS, 0 FAIL — all 51 expected-result programs
  on all four engines (interp, x86 JIT, RV64, ARM), each checked
  against its "Expected result:" comment; the register-MUL chain
  executes at runtime on the ARM engine (PASS 333), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean (the canonical make a64-enc-check, a
  fresh assemble + dump + re-derive); jmpr_oob still faults (UDF,
  rc=1).

### 10.66 M2.28 — the chain follows a bitwise-built index (as built)

M2.27's chain_img_alu image covered ADD/SUB/MUL only, so a bitwise-built
index — `XOR r1, r2, #k` — was still opaque: the XOR writer marked its
set UNKNOWN and the program kept the table. M2.28 extends the image to
AND/OR/XOR with the same argument the M2.3 bitwise fold used: plain
64-bit bitwise ops never fault and are type-agnostic, so the value set
is exactly the op of its sources' sets.

- **chain_alu_eval, shared.** The six images now funnel through one
  int64 evaluator (op, a, b), replacing M2.27's growing nested
  ternary; the imm form passes the sign-extended imm28, the register
  form the source value. AND/OR/XOR join ADD/SUB/MUL in both forms,
  and the writer rule in the walk matches. Every other writer still
  marks UNKNOWN.
- **jmpr_chain4, the pin.** `r1 = r2 ^ 8` with the join-dependent
  base r2 in {2, 4} — the candidate set is the XOR image {2, 4} ^ 8
  = {10, 12}, and the runtime index 12 takes the chain's SECOND
  b.eq (dump-verified: `eor x9, x9, x10` computes r2^8, then
  `subs xzr,x9,#10 / b.eq`, `subs xzr,x9,#12 / b.eq`, `udf #0`).
- **Unchanged soundness argument.** Union-at-joins, real-edges-only
  linear state, UDF fall-through, out-of-range collapse — exactly
  M2.25-M2.27's; only the image's opcode set grew, and the teeth
  (bitwise ops dropped from the writer rule) grows jmpr_chain4 back
  to exactly 1184 (the table path, still correct).

### 10.67 M2.28 gate results (measured)

Total emitted bytes across the now-52-program parity set: **M0 79584
→ M1 68412, 11172 saved** (≈14.0%), up from M2.27's 11032. One row
added, zero moved:

- jmpr_chain4 1248 → 1108 (−140, new 52nd row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The teeth (bitwise
  image disabled) grows it back to exactly 1184 (the table path,
  still correct).
- Row-by-row accounting (gate tables diffed vs committed 11352bf,
  measured with the M2.27 verifier): **all 51 shared rows
  byte-identical** — M2.28 is strictly additive; AND/OR/XOR appear
  in the corpus only inside jmpr_calc_bit (whose constant fold
  predates the chain and does not feed a dispatch), so nothing else
  moved.
- Four-way parity: 208 PASS, 0 FAIL — all 52 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the register-XOR
  chain executes at runtime on the ARM engine (PASS 333), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean (the canonical make a64-enc-check, a
  fresh assemble + dump + re-derive); jmpr_oob still faults (UDF,
  rc=1).

### 10.68 M2.29 — the adaptive chain cap (as built)

M2.25-M2.28 emitted the inline chain only when the provable candidate
set fit the hard cap of 8 — a 9-11-candidate set kept the runtime table
EVEN when the chain would have been smaller. M2.29 makes the decision
adaptive:

- **The cost function.** Chain: one subs+b.eq pair per in-range
  candidate (2 words) plus the UDF fall-through (1 word); the index
  load and the pre-dispatch cache flush are shared with the table
  path, so they cancel. Table: the 10-word dispatch (movz num_instr,
  subs cmp, cset, cbz, the 2-word li32 base, add-shift, ldr W, br,
  UDF) plus num_instr 4-byte entries (M2.24's compaction). The chain
  fires when 8·n + 4 < 40 + 4·num_instr. Every in-range candidate is
  < num_instr ≤ 4096, so all fit the imm12 of subs — no movz+subs
  form is ever needed; ncand==0 (jmpr_oob's constant 999) always
  wins (a bare UDF), byte-identical to M2.25.
- **The 8 → 12 raise.** TX_AR_CHAIN_MAX grows to 12 so 9-11-candidate
  sets can be COLLECTED (the merge no longer collapses them to
  UNKNOWN at 9) and emitted. The static BSS cost is bounded (~290
  KiB more across g_chain_arr); the corpus's sets stay tiny.
- **jmpr_chain5, the pin.** The index is the pair product r1 = r2 + r3
  over a 2-way join ({0, 10}) and a 5-way join ({18..26 evens}) — TEN
  in-range candidates {18..36 evens} on the 38-instruction program.
  The old cap kept the 152-byte table + dispatch (1664 bytes); the
  adaptive check (84 < 192) emits the 10-pair chain instead, and the
  runtime index 10 + 26 = 36 takes the chain's TENTH b.eq (block9,
  LOADI #1000) — the full walk dispatches. (First test draft landed
  the candidates 2 pcs off — labels do not consume pcs — and candidate
  38 fell out of range, faulting via the chain UDF exactly as designed;
  the corrected layout uses the blocks' real even pcs.)
- **Unchanged soundness argument.** Union-at-joins, real-edges-only
  linear state, UDF fall-through, out-of-range collapse — exactly
  M2.25-M2.28's; only the emission decision grew from a hard cap to
  a cost comparison, and the teeth (cap forced back to 8) grows
  jmpr_chain5 back to exactly 1664 (the table path, still correct).

### 10.69 M2.29 gate results (measured)

Total emitted bytes across the now-53-program parity set: **M0 81408
→ M1 69968, 11440 saved** (≈14.1%), up from M2.28's 11172. One row
added, zero moved:

- jmpr_chain5 1824 → 1556 (−268, new 53rd row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The teeth (cap forced to
  8) grows it back to exactly 1664 (the table path, still correct).
- Row-by-row accounting (gate tables diffed vs committed 649fbff,
  measured with the M2.28 verifier): **all 52 shared rows
  byte-identical** — M2.29 is strictly additive; no pre-existing
  program has a candidate set above 2, so nothing else moved
  (jmpr_oob's degenerate ncand==0 chain still fires).
- Four-way parity: 212 PASS, 0 FAIL — all 53 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 10-candidate
  chain executes at runtime on the ARM engine (PASS 1000), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean (the canonical make a64-enc-check, a
  fresh assemble + dump + re-derive); jmpr_oob still faults (UDF,
  rc=1).

### 10.70 M2.30 — the deferred pair product (as built)

M2.29's adaptive gate could emit a 13-32-candidate chain, but the walk
itself capped flat sets at 12 — a pair product that exceeded it
collapsed to UNKNOWN at the 13th merge and kept the table. M2.30 lets
the walk DEFER an oversized register-form product:

- **The deferred form.** A register-form ADD/SUB/MUL/AND/OR/XOR whose
  image overflowed the flat cap is recorded in the walk state as
  (op, source-slot-a, source-slot-b) instead of UNKNOWN. The dispatch
  then re-computes the product into a BIG candidate set (cap 32) and
  the M2.29 gate decides. The imm form can never overflow (one source),
  and rd == ra/rb is refused (a self-referencing deferred form could
  not be materialized).
- **Soundness — the invalidation discipline.** A deferred product's
  value is fixed at its instruction, so its sources must be untouched:
  every WRITE of a source slot, every head-UNION, and every DELIVERY
  flattens (and caps) any deferred form reading it eagerly — over the
  pre-write/pre-union source values. The dispatch materialization
  therefore sees exactly the sources the product saw. Every merge
  output and every constructor is flat by construction (def = 0), so
  a deferred form can never leak into an accumulator.
- **jmpr_chain6, the pin.** The index is r1 = r2 + r3 over a 3-way
  join ({0,2,4}) and a 5-way join ({20,30,40,50,60}) — FIFTEEN
  distinct in-range candidates {20,22,...,64} on the 66-instruction
  program, beyond the flat cap of 12. The walk defers; the dispatch
  materializes all 15; the gate (124 < 304) emits the 15-pair chain;
  the runtime index 4 + 60 = 64 takes the chain's FIFTEENTH b.eq
  (block14, LOADI #1500).
- **A latent decoder bug the pin exposed.** The M2.25-era B.cond
  decode in a64_exec.c read the cond field from bits 15:12 instead of
  3:0 — grabbing imm19's top nibble. Every M2.25-M2.29 chain stayed
  under the 512-byte b.eq offset threshold where the misread nibble is
  zero, so EQ decoded correctly by luck; jmpr_chain6's 15-pair chain
  pushed the later offsets past it and the branches silently became NE
  (the runtime index fell through to the wrong block). The fix is one
  line (cond = w & 0xF); the airtight diff proves all prior rows
  byte-identical, and the enc-check's bcond class check was unaffected
  (it masks only the top byte).
- **Unchanged emission.** The M2.29 cost gate and the chain shape are
  untouched — only the walk's set representation grew, and the teeth
  (deferral disabled) grows jmpr_chain6 back to exactly 2188 (the
  table path, still correct).

### 10.71 M2.30 gate results (measured)

Total emitted bytes across the now-54-program parity set: **M0 83868
→ M1 71976, 11892 saved** (≈14.2%), up from M2.29's 11440. One row
added, zero moved:

- jmpr_chain6 2460 → 2008 (−452, new 54th row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The teeth (deferral
  disabled) grows it back to exactly 2188 (the table path, still
  correct; the +180 delta is exactly the 66-entry table + 10-word
  dispatch minus the 15-pair chain).
- Row-by-row accounting (gate tables diffed vs committed 5298fcd,
  measured with the M2.29 verifier): **all 53 shared rows
  byte-identical** — M2.30 is strictly additive; no pre-existing
  program's product exceeds 12, so nothing else moved, and the B.cond
  fix is byte-invisible on every prior row (their offsets were all
  under the 512-byte threshold).
- Four-way parity: 216 PASS, 0 FAIL — all 54 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 15-candidate
  chain executes at runtime on the ARM engine (PASS 1500), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean (the canonical make a64-enc-check, a
  fresh assemble + dump + re-derive); jmpr_oob still faults (UDF,
  rc=1).

### 10.72 M2.31 — the deferred pair product, generalized (as built)

The M2.30 deferral machinery (§10.70) shipped op-generic without a
pin: the walk's product branch records `cur[s].def_op = op` for the
whole six-op branch (ADD/SUB/MUL/AND/OR/XOR — the same set the M2.2
fold and the M2.27/28 image justify as plain 64-bit, never-faulting,
type-agnostic), and both materialization sites (the walk-side capped
`chain_flatten` and the dispatch-side BIG `chain_flatten_big`) evaluate
through the shared `chain_alu_eval`, which already handled all six
ops. jmpr_chain6 pinned only ADD, so a bitwise- or multiply-built index
had no proof that the deferred form round-trips. M2.31 closes that
with jmpr_chain7: a **deferred XOR product**.

- **The pin.** `r1 = r2 ^ r3` over a 3-way join (`r2 ∈ {0,32,64}`)
  and a 5-way join (`r3 ∈ {20,22,24,26,28}`) — FIFTEEN distinct
  products `{20,22,...,92}` (bit-6/7 toggles over an even low run,
  all ≥ 2 apart, all real block pcs). The flat image overflows the
  cap of 12, so the walk defers: `def_op = OP_XOR` plus the two source
  slots, invalidated on any write/union (jmpr_chain6's discipline,
  untouched). The dispatch re-merges into the BIG set and the cost
  gate (124 < 416) emits the 15-pair chain; runtime `64 ^ 28 = 92`
  takes the FIFTEENTH b.eq, landing on block14's LOADI #1500.
- **Zero codegen delta.** The emission is byte-identical to M2.30's;
  the walk's deferred-form record was already op-agnostic. M2.31's
  code change is nil — the milestone is the proof, the teeth, and the
  measurement (below). The one build note surfaced: `e64()` in
  simi_arm.c is dead (zero callers, predating M2.31; -Wunused-function
  warns on every build) — left untouched as out of scope.

### 10.73 M2.31 gate results (measured)

Total emitted bytes across the now-55-program parity set: **M0 86888
→ M1 74320, 12568 saved** (≈14.5%), up from M2.30's 11892. One row
added, zero moved:

- jmpr_chain7 3020 → 2344 (−676, new 55th row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The chain vs table delta
  is exactly 416 − 124 = +292 (94-entry table + 10-word dispatch
  minus the 15-pair chain); the remaining 384 of the 676 is the M1
  allocator on the 94-instruction body. The teeth (deferral
  restricted to ADD-only) grows it back to exactly 2636 (the table
  path, still correct) — proving the XOR path is the deferral doing
  the work, not an accident of the walk.
- Row-by-row accounting (gate tables diffed vs committed 0920071,
  measured with the M2.30 verifier): **all 54 shared rows
  byte-identical** — M2.31 is strictly additive; no pre-existing
  program's product exceeds 12, so nothing else moved.
- Four-way parity: 220 PASS, 0 FAIL — all 55 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 15-candidate
  XOR chain executes at runtime on the ARM engine (PASS 1500), and
  the documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean (the canonical make a64-enc-check, a
  fresh assemble + dump + re-derive); jmpr_oob still faults (UDF,
  rc=1).

### 10.74 M2.32 — the deferred pair product, nested (as built)

The M2.30/M2.31 deferral records the op plus two SOURCE SLOTS, so a
product over a deferred source had to be flattened eagerly — and the
capped walk-side flatten collapses (a deferred source's true set always
exceeds 12) to UNKNOWN, killing the chain. jmpr_chain8's shape makes
that visible: an in-place second product `ADD r1, r1, r4` over a
15-value deferred `r1` died to the table. Two design facts block the
naive fixes:

- **Four-tracked-register cap.** The closure tracks the JMPR register
  plus its transitive feeders (≤ 4). A second product in a fresh
  register would need a fifth (the JMPR reg, the first product's two
  sources, the second product's source, and the second dst), so the
  nested form must write `r1` in place — `rd == ra`.
- **No self-cycle.** The in-place record cannot reference its own slot
  (materialization would recurse forever), so it must reference the
  OLD value of `r1` — which is exactly what the deferral overwrites.

M2.32 solves both with an immutable **def-record pool**: `cur[].def`
became an index into `g_chain_defs`, and each record's two operands
are either a tracked slot (`CD_SLOT`) or an OLDER record (`CD_REC`).
The in-place product defers as `op(rec(R1), slot(r4))` — the left
operand is the immutable record for `r1`'s old value, not `cur[r1]`
itself, so there is no cycle. Records only reference older records, so
the DAG is acyclic; the pool resets each walk iteration (cap 64,
overflow → UNKNOWN). Two supporting changes keep the deferral sound:

- **Transitive invalidation.** `chain_prewrite` now flattens every
  deferred form whose record DAG TRANSITIVELY reaches the written
  slot — a nested deferral reads its source's source, so writing a
  leaf must invalidate the whole chain above it (a `CD_REC` operand
  is immune, being immutable).
- **In-place rule.** The deferral allows `rd == ra`/`rd == rb` only
  when that source is itself deferred (referenced as a record); an
  in-place FLAT source still falls to UNKNOWN exactly as M2.30 did.

A layout caveat the pin surfaced: a deferred source cannot cross a
block head — the head-union flattens every deferred form it mutates —
so jmpr_chain8 runs all three joins first, then the two products and
the JMPR in one straight-line run. (An imm-form product over a
deferred source also stays UNKNOWN — the record's two operands are
slot/record refs, no imm kind; noted as future work.)

### 10.75 M2.32 gate results (measured)

Total emitted bytes across the now-56-program parity set: **M0 89952
→ M1 76908, 13044 saved** (≈14.5%), up from M2.31's 12568. One row
added, zero moved:

- jmpr_chain8 3064 → 2588 (−476, new 56th row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The chain vs table delta
  is exactly 376 − 244 = +132 (84-entry table + 10-word dispatch
  minus the 30-pair chain); the remaining 344 of the 476 is the M1
  allocator on the 84-instruction body. The teeth (nested record path
  disabled) grows it back to exactly 2720 (the table path, still
  correct) — proving the 30-candidate walk dispatches through the
  nested DAG, not an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed a74f00f,
  measured with the M2.31 verifier): **all 55 shared rows
  byte-identical** — the def-record-pool rewrite is emission-invisible
  on every pre-existing program (chain6/7's two-slot deferrals became
  two-operand records with identical materialization; nothing else
  touches the deferred form), so M2.32 is strictly additive.
- Four-way parity: 224 PASS, 0 FAIL — all 56 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 30-candidate
  nested chain executes at runtime on the ARM engine (PASS 3000,
  runtime index 82 taking the thirtieth b.eq, dump-verified 30 b.eq
  pairs from #100's block to #3000's block), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean (the canonical make a64-enc-check, a fresh assemble
  + dump + re-derive); jmpr_oob still faults (UDF, rc=1).

### 10.76 M2.33 — the deferred product, imm form (as built)

M2.32's record operands were slots or older records, so an IMM-form
product over a deferred source (`ADD r1, r1, #32`) still fell to the
flatten path — the capped flatten collapses (the source's true set
always exceeds 12) to UNKNOWN, killing the chain. M2.33 adds a third
operand kind, `CD_IMM`: the imm-form product defers as `op(rec(R1),
#32)` — the source referenced as its immutable record (a record ref is
safe even in-place, per M2.32's design) and the immediate baked in as a
constant operand (the record's operand fields widened to int32 for the
imm28). The materializers gained a shared operand-resolution step that
handles all three kinds, and the invalidation is untouched (`CD_IMM`
never touches a slot, so `chain_def_touches` ignores it). Because the
imm image has exactly the source's cardinality, the imm branch fires
whenever the source is deferred — there is no flat overflow to check
(a flattened source is already UNKNOWN), and deferring is always
sound since the materialization computes the true image (even for
non-injective imm ops like `MUL r1, r1, #0`).

### 10.77 M2.33 gate results (measured)

Total emitted bytes across the now-57-program parity set: **M0 92780
→ M1 79140, 13640 saved** (≈14.7%), up from M2.32's 13044. One row
added, zero moved:

- jmpr_chain9 2828 → 2232 (−596, new 57th row; M0 baseline measured
  at git 1729f50) — the dedicated pin above. The chain vs table delta
  is exactly 376 − 124 = +252 (84-entry table + 10-word dispatch
  minus the 15-pair chain); the remaining 344 of the 596 is the M1
  allocator on the 84-instruction body. The teeth (CD_IMM path
  disabled) grows it back to exactly 2484 (the table path, still
  correct) — proving the imm product dispatches through the record
  DAG, not an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed 7cb4a17,
  measured with the M2.32 verifier): **all 56 shared rows
  byte-identical** — no pre-existing program has an imm-form product
  over a deferred source, so M2.33 is strictly additive.
- Four-way parity: 228 PASS, 0 FAIL — all 57 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 15-candidate
  imm chain executes at runtime on the ARM engine (PASS 1500,
  runtime index 82 taking the fifteenth b.eq, dump-verified 15 b.eq
  pairs from #100's block to #1500's block), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean (the canonical make a64-enc-check, a fresh assemble
  + dump + re-derive); jmpr_oob still faults (UDF, rc=1).

### 10.78 M2.34 — the deferred-preserving join (as built)

M2.32/M2.33's head-union flattened every deferred form it mutated, so
a deferred source could not cross a join: a product followed by a join
(chain8's shape with the r4 join moved *between* the products) killed
R1's deferred form at the head, and the second product fell back to
UNKNOWN. The soundness premise that makes a fix possible: every
record's true set exceeds the flat cap (12), so its contribution to a
union is UNKNOWN **unless** every incoming path carries the *same*
record — in which case the union is a no-op and the deferred form can
be kept. M2.34 rewrites `chain_deliver` to preserve: same record →
no-op (keep deferred), first arrival → keep the record, any mix →
UNKNOWN; and the head-union's second loop gains the same-record
preserve with the UNKNOWN fallback. This also removes the need to
flatten delivered records on delivery entirely — the unsound trap
being that flattening a stale delivered record reads slots rewritten
since the record was created.

### 10.79 M2.34 gate results (measured)

Total emitted bytes across the now-58-program parity set: **M0 95844
→ M1 81728, 14116 saved** (≈14.7%), up from M2.33's 13640. One row
added, zero moved:

- jmpr_chain10 3064 → 2588 (−476, new 58th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: index = `(r2 + r3) + r4` with
  the r4 join *between* the two products. R1's 15-value deferred
  product crosses the join (every incoming path carries the same
  record), then the in-place second product composes through the
  record DAG to the thirty candidates `{22..82}`; runtime
  `(20+30)+32 = 82` takes the chain's thirtieth b.eq (dump-verified:
  30 b.eq pairs, first → #100's block, thirtieth → #3000's block).
  The chain vs table delta is exactly 376 − 244 = +132; the rest of
  the 476 is the M1 allocator on the 112-instruction body.
- Teeth: the preserve had to be disabled in *both* places (the
  head-union's same-record branch **and** `chain_deliver`'s
  preservation — disabling only the head-union branch does not bite,
  because deliver's preservation alone still keeps R1 alive through
  the join). With both off, jmpr_chain10 grows to exactly 2720 (the
  table path, still correct) — proving the join-preserved record is
  what the chain dispatches through, not an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed 88e0709,
  measured with the M2.33 verifier): **all 57 shared rows
  byte-identical** — the deliver/head-union rewrite is
  emission-invisible on every pre-existing program (no pre-existing
  program carries the same record into a join), so M2.34 is strictly
  additive.
- Four-way parity: 232 PASS, 0 FAIL — all 58 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 30-candidate
  join-crossing chain executes at runtime on the ARM engine (PASS
  3000, runtime index 82 taking the thirtieth b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.80 M2.35 — the deferred-over-flat join (as built)

M2.34 preserved a deferred form across a join only when every path
carried the SAME record. The mirror case — a deferred carry meeting a
FLAT arrival (a branch path whose index register is a constant) —
still collapsed to UNKNOWN. M2.35's rule: when a join mixes a live
deferred form with a flat set, the union is still exactly the record
when the flat set is CONTAINED in the record's true set (the flat
path contributes nothing new); otherwise the union exceeds the record
(still > 12) and cannot be a flat walk set. The containment test
(`chain_flat_in_def`) materializes the CARRY record BIG — sound for
the M2.34 reason: the carry record is live (chain_prewrite flattens
it on any write to its DAG), so its CD_SLOT operands still read the
values the product saw. Two scoping decisions keep it sound and
strictly additive:

- **Empty-arrival gate**: an empty (delivery-less) arrival returns
  "not contained", so iteration 1 of the fixpoint still collapses the
  deferred carry exactly as M2.34 did — the change fires only when a
  real flat arrival exists, preserving every pre-existing program's
  iteration path byte-for-byte.
- **Deliver-side untouched**: `chain_deliver`'s flat-then-deferred
  order is unreachable — any flat r1 implies a prior r1 write, which
  flattens the record out of `cur[]`, so a deferred delivery always
  precedes a flat one into the same accumulator. Only the head-union
  (deferred CARRY vs flat arrival) needed the change.

The probe (jmpr_chain11) also surfaced two layout constraints worth
recording: (a) the flat delivery must be FORWARD (a backward BR's
arrival is read one iteration late, after the fixpoint has already
exited — iteration 0's UNKNOWN matches the initial UNKNOWN snapshot);
and (b) a BC delivery carries EVERY tracked slot, so the flat arm
must sit after all joins — an early arm delivers UNKNOWN for the
not-yet-joined registers and poisons them at the head. The final
shape: all joins first, then the flat arm (`r1 = #34`, a value inside
R1's set), then the product (R1 defers), then J1.

### 10.81 M2.35 gate results (measured)

Total emitted bytes across the now-59-program parity set: **M0 98944
→ M1 84336, 14608 saved** (≈14.8%), up from M2.34's 14116. One row
added, zero moved:

- jmpr_chain11 3100 → 2608 (−492, new 59th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: index = `(r2 + r3) + r4` over
  three joins plus the flat arm's forward BC delivering `{r1:34}`
  into J1. R1's 15-value deferred product (the carry) survives the
  flat join via containment, and the in-place second product composes
  to the thirty candidates `{24..84}`; runtime `(20+32)+32 = 84`
  takes the chain's thirtieth b.eq (dump-verified: 30 b.eq pairs,
  first → #100's block, thirtieth → #3000's block). The chain vs
  table delta is exactly 384 − 244 = +140; the rest of the 492 is the
  M1 allocator on the 86-instruction body.
- Teeth: disabling the containment keep grows jmpr_chain11 back to
  exactly 2748 (the table path, still correct) — proving the
  flat-arrival containment is what the chain dispatches through, not
  an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed d4d7323,
  measured with the M2.34 verifier): **all 58 shared rows
  byte-identical** — no pre-existing program has a non-empty flat
  arrival meeting a deferred carry (the empty-arrival gate keeps
  iteration 1 identical), so M2.35 is strictly additive.
- Four-way parity: 236 PASS, 0 FAIL — all 59 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 30-candidate
  flat-join chain executes at runtime on the ARM engine (PASS 3000,
  runtime index 84 taking the thirtieth b.eq), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.82 M2.36 — the deferred-arrival-over-flat join (as built)

M2.35 handled the join where the deferred form is the CARRY (sound
because the carry record is live). The mirror — a flat carry meeting
a deferred ARRIVAL — stayed UNKNOWN because an arrival record can be
STALE: its DAG slots may have been written since the product, and
materializing it would read replaced values (a chain over the wrong
set could UDF-fault on a true dispatch value). M2.36 adds a
record-LIVENESS check: `chain_def_live` walks the record's transitive
DAG and fails if any CD_SLOT leaf was WRITTEN after the record's
creation pc (tracked via `g_chain_def_pc`, set by
`chain_def_alloc`, and `g_chain_last_write`, updated at every write
site — the ar_writes branch, the CALL r0 clobber, and the ENTER
frame reset). The key soundness observation: head-unions are
DELIBERATELY not writes — a union only ever widens a slot to a
SUPERSET of what the record saw (it includes the arrival path's own
delivery), so materializing over a widened leaf is a sound
over-approximation; only a WRITE replaces a set and kills the
record. The keep then applies M2.35's containment in the other
direction: the flat carry must be contained in the record's
materialized true set (`chain_flat_in_def_idx`), so the carry path
contributes nothing new. The liveness + containment together make
keeping the arrival record EXACT (the union is exactly the record,
modulo the sound superset widening). No pre-existing program has a
flat carry meeting a deferred arrival, so the branch is dead for the
entire M2.35 corpus — strictly additive.

### 10.83 M2.36 gate results (measured)

Total emitted bytes across the now-60-program parity set: **M0
102084 → M1 86968, 15116 saved** (≈14.8%), up from M2.35's 14608.
One row added, zero moved:

- jmpr_chain12 3140 → 2632 (−508, new 60th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: index = `(r2 + r3) + r4` over
  three joins, with the R-arm's `BC r5, J1` (taken at runtime,
  r5 = 1) delivering the DEFERRED R1 into J1 forward, and the
  fall-through flat arm (`r1 = #34`, a value inside R1's set)
  providing the flat carry {34}. R1's 15-value deferred record
  survives the flat join via liveness + containment, and the in-place
  second product composes to the thirty candidates `{26..86}`;
  runtime `(22+32)+32 = 86` takes the chain's thirtieth b.eq
  (dump-verified: 30 b.eq pairs, first → #100's block, thirtieth →
  #3000's block). The chain vs table delta is exactly 392 − 244 =
  +148; the rest of the 508 is the M1 allocator on the 88-instruction
  body.
- Teeth: disabling the liveness keep grows jmpr_chain12 back to
  exactly 2780 (the table path, still correct) — proving the
  deferred-arrival liveness is what the chain dispatches through,
  not an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed d3a441d,
  measured with the M2.35 verifier): **all 59 shared rows
  byte-identical** — no pre-existing program has a flat carry
  meeting a deferred arrival, so M2.36 is strictly additive.
- Four-way parity: 240 PASS, 0 FAIL — all 60 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 30-candidate
  flat-carry chain executes at runtime on the ARM engine (PASS 3000,
  runtime index 86 taking the thirtieth b.eq), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.84 M2.37 — the non-contained deferred-over-flat join (as built)

M2.35/M2.36 made a deferred record survive a flat join only when the
flat side was CONTAINED in the record's true set (then the union is
exactly the record). A flat value outside the record's set — the
flat arm carrying #70 against R1's `{26..54}` — still collapsed the
union to UNKNOWN (the record's >12 values dominate the walk's flat
bound), killing the chain. M2.37 closes that: when containment
fails in EITHER orientation, the union is represented as a **UNION
record** — a new record shape, `op(rec(R), flat(F))`, whose
materialization MERGES the record's true set with the flat set,
capped at TX_AR_CHAIN_BIG (32). The flat side is a new operand
kind, CD_FLAT: the ≤12-value set is frozen out-of-line in
`g_chain_def_flat[rec]` at creation and is immune to later writes
and unions. The record side is a CD_REC reference to the existing
record, so the DAG stays acyclic and the existing invalidation
machinery needs no change: `chain_def_touches`/`chain_def_live`
already recurse CD_REC and ignore unknown kinds, and `chain_prewrite`
flattens the union like any other record when one of its transitively
read slots is written.

Soundness of the union record: its materialization at the dispatch
merges `flatten(rec)` over the CURRENT cur[] with the frozen flat
set. The caller gates make this exact-or-superset — in the carry
orientation the record is live by construction (any write to its DAG
flattened it out of cur[]), and in the arrival orientation the
M2.36 liveness check (chain_def_live) is required before the union
fires; head-unions can only widen the leaves (a sound superset), so
the dispatch materialization can only grow, never lose, values. The
eager cap-check in `chain_def_alloc_union` materializes the union
at the head and refuses to create the record when the merged set
exceeds 32 — exact-or-conservative, because a later head-union can
only widen (a union that fits at the head provably fits at the
dispatch; one that does not fit there can never fit later). Two
scoping decisions keep it strictly additive: an EMPTY flat side
still collapses (the M2.34 first-iteration behavior is unchanged),
and `chain_deliver` is untouched (a mix of record and flat arrivals
in the accumulator still collapses — the union fires only on the
head-union's single flat side). The union composes: products over
the union record defer with a CD_REC reference, and the flattened
union merges transitively (a union record inside another union's
record side is handled by the same flatten case).

### 10.85 M2.37 gate results (measured)

Total emitted bytes across the now-61-program parity set: **M0
105576 → M1 89832, 15744 saved** (≈14.9%), up from M2.36's 15116.
One row added, zero moved:

- jmpr_chain13 3492 → 2864 (−628, new 61st row; M0 baseline measured
  at git 1729f50) — the dedicated pin: chain12's shape (index =
  `(r2 + r3) + r4` over three joins) with the flat arm carrying #70
  — OUTSIDE R1's 15-value set. The R-arm's `BC r5, J1` (NOT taken
  at runtime, r5 = 0) delivers the deferred R1 forward; the
  fall-through flat arm provides the flat carry {70}. Containment
  fails in the M2.36 orientation, so the head creates the UNION
  record U = R1 ∪ {70} (16 values), and the in-place second product
  composes R2 = op(rec(U), slot(r4)) to the THIRTY-ONE candidates
  `{26,28,...,54, 58,60,...,86, 102}` (56 is a gap — R1 starts at
  26 — and 70 duplicates); runtime `70 + 32 = 102` takes the
  chain's thirty-first b.eq (dump-verified: 31 b.eq pairs, first →
  #100's block at offset 1316, thirty-first → #3200's block at
  offset 2724, both byte-exact). The chain vs table delta is
  exactly 456 − 252 = +204; the rest of the 628 is the M1
  allocator on the 104-instruction body.
- Teeth: disabling BOTH union fallbacks (the M2.35- and M2.36-
  orientation heads) grows jmpr_chain13 back to exactly 3068 (the
  table path, still correct) — proving the union record is what the
  chain dispatches through, not an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed 13822f7,
  measured with the M2.36 verifier): **all 60 shared rows
  byte-identical** — no pre-existing program has a deferred record
  meeting a non-contained flat set, so M2.37 is strictly additive.
- Four-way parity: 244 PASS, 0 FAIL — all 61 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 31-candidate
  union chain executes at runtime on the ARM engine (PASS 3200,
  runtime index 102 taking the thirty-first b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.86 M2.38 — the record-vs-record join (as built)

M2.37 made a deferred record survive a flat join; the remaining gap
was a join mixing a deferred record with a DIFFERENT deferred record
— the union of two >12-value sets cannot be represented by either
record, so the first loop's different-record unknowning collapsed it
to UNKNOWN and killed the chain. M2.38 generalizes the UNION record
so its second side can itself be a record: `op(rec(R2), rec(R1))`
(kb = CD_REC in the second operand slot, where M2.37 used CD_FLAT).
The flatten helpers need no change — `chain_flatten_op` already
resolves CD_REC on either operand, so the union's materialization
merges `flatten(rec2)` with `flatten(rec1)` exactly as it merged the
flat side. `chain_def_alloc_union` gained a second-side kind: CD_REC
(runs the same eager 32-cap merge over the second record) or CD_FLAT
(the M2.37 path, unchanged). The first loop's different-record
unknowning is DELETED — every record-vs-record case now reaches the
deferred-carry branch, where the union can fire: the carry record is
live by construction and the ARRIVAL record must pass the M2.36
liveness check (no write to its transitive DAG since creation) for
its materialization at the dispatch to be exact-or-superset. Records
only reference older records (both R2 and R1 predate the union at the
head), so the DAG stays acyclic and the existing invalidation needs
no change: `chain_prewrite`/`chain_def_touches`/`chain_def_live`
already recurse both CD_REC operands. The deletion is
emission-invisible — a record-vs-record union that does NOT fit (or
an unlive arrival) falls through to the same UNKNOWN the first loop
produced, and no pre-existing program has the shape that now fires.

### 10.87 M2.38 gate results (measured)

Total emitted bytes across the now-62-program parity set: **M0
108944 → M1 92596, 16348 saved** (≈15.0%), up from M2.37's 15744.
One row added, zero moved:

- jmpr_chain14 3368 → 2764 (−604, new 62nd row; M0 baseline measured
  at git 1729f50) — the dedicated pin: the dispatch index r1 is
  computed on two arms. The R-arm computes R1 = r2 + r3 (FIFTEEN
  values {40..68}, DEFERS) and its `BC r5, J` delivers R1 into J's
  accumulator; the fall-through S-arm (taken at runtime, r5 = 0)
  computes R2 = r2 + r6 over the SHARED r2 join (FIFTEEN DISJOINT
  values {70..98}, DEFERS — replacing R1 in cur, already delivered)
  and flows into J. So J mixes a deferred CARRY R2 with a deferred
  ARRIVAL R1 of a DIFFERENT record; the M2.38 union op(rec(R2),
  rec(R1)) fires (the merged 30-value set fits the 32-cap; R1 passes
  the liveness check) and the chain dispatches the THIRTY candidates
  `{40..98}` — runtime `22 + 76 = 98` takes the thirtieth b.eq
  (dump-verified: 30 b.eq pairs, first → #100's block at offset
  1464, thirtieth → #3000's block at offset 2624, both byte-exact).
  The chain vs table delta is exactly 440 − 244 = +196; the rest of
  the 604 is the M1 allocator on the 100-instruction body.
- Teeth: disabling the M2.38 union grows jmpr_chain14 back to
  exactly 2960 (the table path, still correct) — proving the
  record-vs-record union is what the chain dispatches through, not
  an accident of the layout. The probe also documents two layout
  constraints the debugging surfaced: a BC delivery carries EVERY
  tracked slot, so all joins must precede the delivering arm (an
  early arm delivers UNKNOWN for the not-yet-joined r6, poisoning
  the head and the union's materialization); and both products share
  the r2 join so the tracked-register closure {r1,r2,r3,r6} stays
  within the 4-register cap.
- Row-by-row accounting (gate tables diffed vs committed 34da96a,
  measured with the M2.37 verifier): **all 61 shared rows
  byte-identical** — the first-loop deletion is emission-invisible
  and no pre-existing program has the record-vs-record shape, so
  M2.38 is strictly additive.
- Four-way parity: 248 PASS, 0 FAIL — all 62 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 30-candidate
  record-vs-record chain executes at runtime on the ARM engine (PASS
  3000, runtime index 98 taking the thirtieth b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.88 M2.39 — the nested union join (as built)

M2.38 made the union record's second side a CD_REC, so its
materialization recurses through `chain_flatten_big_rec` (the UNION
special case resolves CD_REC operands via `chain_flatten_big_op`),
and `chain_def_live` recurses both CD_REC operands. That means a
union record meeting a THIRD distinct deferred record should compose
with NO new code: the M2.38 head-union branch fires with the arrival
being the union record (liveness recurses through it), and
`chain_def_alloc_union` merges `flatten(rec3)` with `flatten(union)`
— the union special case inside the flatten recursion — when the
merged true set fits the 32-cap. M2.39 is therefore a PROOF
milestone: the probe pins the composition, and no translator code
changed (the working tree's only deltas are the test, the gate row,
and this section). The cap-check stays exact-or-conservative for
nested unions: both the union and the new record materialize over
the live cur[], a later head-union can only widen, and a nested
union that fits at the head provably fits at the dispatch.

### 10.89 M2.39 gate results (measured)

Total emitted bytes across the now-63-program parity set: **M0
112436 → M1 95436, 17000 saved** (≈15.1%), up from M2.38's 16348.
One row added, zero moved:

- jmpr_chain15 3492 → 2840 (−652, new 63rd row; M0 baseline measured
  at git 1729f50) — the dedicated pin: three arms compute three
  DISTINCT deferred records over the four-register closure
  {r1,r2,r3,r6}: R1 = r2 + r3 (FIFTEEN values {42..70}), R2 = r2 + r6
  (FIFTEEN values {42,46,50,...,78}), R3 = r6 + r3 (THIRTEEN values
  {80..104} — the sparse-vs-contiguous steps of r6/r3 spread R3's
  sums past 12). The R-arm's `BC r5, J1` delivers R1; the
  fall-through computes R2 and flows in as the carry, so J1 forms
  the union U = op(rec(R2), rec(R1)) = {42..70, 74, 78} (17 values).
  The next `BC r5, JN` delivers U into JN; the fall-through computes
  R3, so JN forms the NESTED union U2 = op(rec(R3), rec(U)) to
  THIRTY candidates {42..104}; runtime `56 + 48 = 104` takes the
  thirtieth b.eq (dump-verified: 30 b.eq pairs, first → #100's block
  at offset 1492, thirtieth → #3000's block at offset 2700, both
  byte-exact). The chain vs table delta is exactly 464 − 244 = +220;
  the rest of the 652 is the M1 allocator on the 106-instruction
  body.
- Teeth: disabling the M2.38 union (the ONLY thing that could make
  this work — the nested union fires through the same branch) grows
  jmpr_chain15 back to exactly 3060 (the table path, still correct),
  proving the nested composition dispatches through the union
  machinery, not an accident of the layout. The probe also documents
  the design constraint: R3's sums must SPREAD past 12 — two
  contiguous 5-value ranges give only 9 distinct sums — so the probe
  uses sparse r6 (step 4) against contiguous r3 (step 2).
- Row-by-row accounting (gate tables diffed vs committed d998e8c,
  measured with the M2.38 verifier): **all 62 shared rows
  byte-identical** — simi_arm.c is untouched, so M2.39 is trivially
  additive; the only delta is the new jmpr_chain15 row at −652.
- Four-way parity: 252 PASS, 0 FAIL — all 63 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 30-candidate
  nested-union chain executes at runtime on the ARM engine (PASS
  3000, runtime index 104 taking the thirtieth b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.90 M2.40 — beyond the 32-cap (as built)

M2.30-M2.39 capped the BIG candidate set — the walk's
DEFERRED-materialization set (chain_flatten_big), the union record's
merged set (chain_merge_big / chain_def_alloc_union's eager
cap-check), and the emitted chain (g_chain_cand) — at TX_AR_CHAIN_BIG
= 32. A dispatch whose union of records exceeded 32 collapsed to
UNKNOWN and fell to the table. M2.40 raises TX_AR_CHAIN_BIG to 64, so
a 33-64-candidate dispatch now chains when the honest cost gate
(8n + 4 < 40 + 4·num_instr) says it beats the table. The LINEAR chain
is the right emission shape at any n: every candidate needs its own
cmp + b.eq pair (2 words) regardless, so "two chained segments" would
add a selector without reducing comparisons (8n + 8 > 8n + 4), and a
compare-and-branch tree costs ~4 words per internal node (cmp + b.eq
+ b.lo + b.hi) for n−1 nodes — both strictly worse on bytes. The
raise is sound with no other change: the flat walk set stays capped
at 12 (TX_AR_CHAIN_MAX — deferral still handles >12), the eager
cap-check stays exact-or-conservative at the new bound, the chain's
candidates are still < num_instr ≤ 4096 (the subs imm12), and the
b.eq imm19 range is far beyond 64. The raise is emission-invisible
for every pre-existing program — none has a union exceeding 32 — so
it is strictly additive.

### 10.91 M2.40 gate results (measured)

Total emitted bytes across the now-64-program parity set: **M0
116380 → M1 98680, 17700 saved** (≈15.2%), up from M2.39's 17000.
One row added, zero moved:

- jmpr_chain16 3944 → 3244 (−700, new 64th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: a union of two deferred
  records is FORTY values. R1 = r2 + r3 (r2 in {2,12,22,32}, r3 in
  {40,42,44,46,48} — TWENTY values {42..80}) and R2 = r2 + r6 (r6 in
  {70,72,...,88} — TWENTY-FIVE values {72..120}) mix at J into
  op(rec(R2), rec(R1)) = {42..120} evens — accepted at the 64 cap,
  rejected at 32 (the R-arm's `BC r5, J` delivers R1 forward, the
  fall-through computes R2 as the carry). The gate (324 < 528) emits
  the 40-pair chain; runtime `32 + 88 = 120` takes the fortieth
  b.eq (dump-verified: 40 b.eq pairs, first → #100's block at offset
  1544, fortieth → #4000's block at offset 3104, both byte-exact).
  The chain vs table delta is exactly 528 − 324 = +204; the rest of
  the 700 is the M1 allocator on the 122-instruction body.
- Teeth: reverting the cap to 32 grows jmpr_chain16 back to exactly
  3448 (the table path — the 40-value union collapses, still
  correct), proving the raised cap is what the chain dispatches
  through, not an accident of the layout.
- Row-by-row accounting (gate tables diffed vs committed 5a63ee6,
  measured with the M2.39 verifier): **all 63 shared rows
  byte-identical** — no pre-existing program has a union exceeding
  32, so M2.40 is strictly additive.
- Four-way parity: 256 PASS, 0 FAIL — all 64 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 40-candidate
  over-cap chain executes at runtime on the ARM engine (PASS 4000,
  runtime index 120 taking the fortieth b.eq), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.92 M2.41 — the flat walk cap above 12 (as built)

M2.25-M2.40 capped the walk's FLAT set (TX_AR_CHAIN_MAX) at 12: a
register-form product whose set exceeded 12 DEFERRED (a record) and
re-computed at the dispatch. M2.41 raises the cap to 20, so a
13-20-value product stays FLAT — and a flat set SURVIVES a write to
one of its SOURCE registers: chain_prewrite only flattens DEFERRED
forms (a write to r2 leaves the flat set in r1's slot untouched), while
a >12-value deferred record was flattened to UNKNOWN by the same write
(the eager flatten caps at the walk bound), killing the chain. The
raise is not free — it needs two companions to stay airtight:

- (a) **Flat+flat union fallback.** Raising the cap changes which sets
  are flat vs deferred, and a flat+flat head-union whose merged set
  exceeds the new cap has no representation (records only enter via
  products). chain14/15's 30-value joins would have collapsed. The
  fix generalizes chain_def_alloc_union so BOTH sides may be CD_FLAT:
  the union record now PRE-MERGES its true set into its store at
  creation — exact-or-conservative (record sides materialize over the
  current cur[] — the carry record is live by construction, the
  arrival record under chain_def_live; flat sides are frozen; later
  head-unions only widen) — and materializing is a single store copy.
  This actually SIMPLIFIES the flatten code (the UNION special cases
  no longer resolve operands).
- (b) **In-place-over-flat freeze.** chain13's second product (ADD
  r1, r1, r4 over its 16-value union U) previously deferred because U
  was a RECORD (the CD_REC indirection makes in-place sound). Under
  the 20-cap U is FLAT, and an in-place product over a flat source
  cannot reference a CD_SLOT (self-cycle) — it would collapse to
  UNKNOWN and chain13 would regress. The fix freezes the aliased flat
  source as a CD_FLAT operand (the set as it stands at the product
  instruction — exactly the runtime value rd holds) in both the M2.32
  deferred-source branch and the M2.30 overflow branch (the latter
  snapshots the aliased slot before the image write clobbers it).

Soundness is the M2.37 CD_FLAT story: frozen sets are immutable,
immune to writes and unions (chain_def_touches/live ignore
non-CDSLOT operands), and a product's materialization over a frozen
source is exact because the runtime value is fixed at the instruction.
The cap raise is emission-invisible for every pre-existing program —
no 13-20-value flat set survives in the M2.40 corpus (chain13-16's
products were designed around the record machinery and dispatch the
same candidate sets) — so it is strictly additive.

### 10.93 M2.41 gate results (measured)

Total emitted bytes across the now-65-program parity set: **M0
119232 → M1 100992, 18240 saved** (≈15.3%), up from M2.40's 17700.
One row added, zero moved:

- jmpr_chain17 2852 → 2312 (−540, new 65th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: r1 = r2 + r3 is TWENTY values
  {42..80} (r2 in {2,12,22,32}, r3 in {40,42,44,46,48}); the arm
  `BC r5, J` delivers it; then `LOADI r2, #1` WRITES a source. Under
  the old 12-cap the product deferred and the write flattened it to
  UNKNOWN (the table ran); under 20 it stays flat, the write is a
  no-op for r1's slot, and the gate (164 < 368) emits the 20-pair
  chain. Runtime `32 + 48 = 80` takes the chain's TWENTIETH b.eq
  (dump-verified: 20 b.eq pairs, first → #100's block at offset
  1412, twentieth → #2000's block at offset 2172, both byte-exact).
  The chain vs table delta is exactly 368 − 164 = +204; the rest of
  the 540 is the M1 allocator on the 82-instruction body.
- Teeth: reverting the cap to 12 grows jmpr_chain17 back to exactly
  2516 (the table path — the product defers and the write kills it,
  still correct), proving the raised cap is what the chain dispatches
  through.
- Row-by-row accounting (gate tables diffed vs committed 32e33f8,
  measured with the M2.40 verifier): **all 64 shared rows
  byte-identical** — the cap raise, the freeze, and the flat+flat
  fallback are emission-invisible for the whole M2.40 corpus
  (chain13/14/15/16 keep their exact bytes), so M2.41 is strictly
  additive.
- Four-way parity: 260 PASS, 0 FAIL — all 65 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 20-candidate
  flat chain executes at runtime on the ARM engine (PASS 2000,
  runtime index 80 taking the twentieth b.eq), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

---

## Sources consulted

- `docs/AeroSLS-SIMI-ISA-v0.1.md` — §7 phased roadmap, §12 Phase 5 findings (the
  RV64 verification tradeoff this plan inherits), §13 Phase 6 (the host-call
  sentinel history), §16 Phases 9/14 (kernel-wiring scope and the JMPR bounds-
  check requirement).
- `tools/simi/simi_riscv.c` / `simi_riscv.h` — the function-for-function mirror
  target, and the zero-shared-header policy it establishes.
- `tools/simi/rv64_exec.c` / `rv64_exec.h` — the decoder+executor shape
  `a64_exec.c` clones, including the sentinel-address machinery and the
  stride-16 lesson.
- `tools/simi/simi_riscv_verify.c` — the harness shape `simi_arm_verify.c`
  clones.
- `docs/AeroSLS-QEMU-SLS-Step6-x86-Frontend-Plan-v0.1.md` — the plan-format
  template and the "gates are measurements, not opinions" discipline adopted in
  §6.
- ARM Architecture Reference Manual (ARMv8-A, A64 instruction set) — encoding
  layouts cited in §3/§4 (imm26/imm19/imm16, scaled vs. unscaled offsets,
  `XZR`/`SP` dual meaning of register 31, `cset`/`csinc` relation synthesis,
  SP-alignment rules). Exact bit layouts to be re-verified against the manual at
  implementation time, per this repo's own "spike before estimate" habit.
