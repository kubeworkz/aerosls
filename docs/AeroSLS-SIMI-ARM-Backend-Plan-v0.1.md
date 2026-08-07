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

### 10.5 M2.1/M2.2 amendment — folding arithmetic constants (as built)

The M2 fold accepted only index values the analysis could see directly
(LOADI/LOADI64 constants, propagated through MOV). M2.1 extends the
constant map to fold ADD/SUB; M2.2 adds MUL — the index is now
COMPUTED at translate time, e.g. `load; add; sub; dispatch` or `load;
mul; dispatch`, which is how real dispatchers shape their index. On the
emitted code side nothing changed: the ALU is a plain 64-bit add/sub/
mul for every SIMI type (the translator never truncates to the declared
width), `materialize_imm` sign-extends imm28 exactly as the analysis
does, and MUL emits `enc_madd(rh, X_T0, rhs, 31)` — `rd = rn*rm + xzr`,
a plain multiply that never faults — so the 64-bit wrap of `a+b`/`a-b`/
`a*b` here is bit-identical to runtime. The fold is deliberately limited
to ADD/SUB/MUL: DIV/MOD would change behavior on a translate-time
division by zero, and the rest of the ALU family (AND/OR/XOR/shifts) is
left to a later pass, a conservative omission rather than a soundness
one.

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
- M2.2 folded MUL, but the rest of the ALU family (AND/OR/XOR/shifts)
  still does not fold; a later pass can extend the same machinery.

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
