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
