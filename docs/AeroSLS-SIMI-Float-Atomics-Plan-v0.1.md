# AeroSLS SIMI — Float & Atomics Gap Plan, v0.1

*Plan only — nothing new implemented yet. Part I (float on A64) completes Phase 10's
third-target hole and inherits the already-landed Phase 10 design; Part II (atomics)
is a new ISA phase. When shipped, Part II needs a §7 roadmap row and a §16 phase
entry in `docs/AeroSLS-SIMI-ISA-v0.1.md`, following the exact conventions Phase 5
and Phase 14 established. Both parts are independent of the in-flight M2.x chain
work (which is pure integer pc-arithmetic analysis and touches neither float nor
atomics).*

---

## 0. Status table — where the gaps actually are

| Capability | Interpreter | x86 JIT | RV64 | A64 (ARM) |
|---|---|---|---|---|
| Integer (31 opcodes) | ✅ | ✅ | ✅ | ✅ |
| Float `T_F32`/`T_F64` (Phase 10) | ✅ landed | ✅ landed | ❌ explicit reject (`TX_RV_ERR_FLOAT_UNSUPPORTED`), deferred to ride Phase 9 | ❌ explicit reject (`TX_AR_ERR_FLOAT_UNSUPPORTED`) — **Part I's gap** |
| Atomics / concurrency | ❌ | ❌ | ❌ | ❌ — **Part II's gap** (no ISA support at all) |

Direct reads that ground this table: `tools/simi/simi_interp.c` (float branches on
`ADD`/`SUB`/`MUL`/`DIV`/`NEG`/`CMP`, `MOD`'s permanent float `die()`), the Phase 10
§16 entry's own findings (x86 landed with the `#NM` IDT fix + GP-bounce; RV64 and
A64 reject), `tools/simi/simi_arm.c`'s `TX_AR_ERR_FLOAT_UNSUPPORTED`, and the ISA
word's flags field (§4 line 56: "signed/unsigned or volatile/atomic markers
elsewhere" — the encoding reserves room but no atomic op exists). The opcode field
is 8 bits (256 slots, 31 used); the type field is 4 bits (13 of 16 used, `T_F32`/
`T_F64` among them); flags bits 1–3 are free (`FLAG_IMM`/`FLAG_INVERT` share bit 0).

---

# Part I — Float on A64 (completes Phase 10's third target)

## 1. The gap, precisely

Phase 10 landed float on the interpreter (native C `double`/`float` semantics, the
cheap correct reference) and the x86 translator (SSE2 via the `#NM` fix + GP
bounce), and explicitly scoped RV64 out to ride alongside Phase 9's RISC-V kernel
wiring (`sstatus.FS` lazy-save twin of `lazy_vector.c`). **A64 was never touched**:
`simi_arm.c` rejects every float-typed instruction with `TX_AR_ERR_FLOAT_UNSUPPORTED`
and `tests/float_ops.simi` is skipped on the ARM harness exactly as it is on RV64.
The ISA-level groundwork is already done and does not need re-doing: `T_F32`/`T_F64`
tags in the word, the Phase 10 assembler literal syntax (`#3.14f64`/`#3.14f32`
flowing through the existing `LOADI64` literal pool), and the seventeen-check
`float_ops.simi` fixture (expected 15) with its three NaN unordered-compare cases.

## 2. Design decisions (A64-specific; Phase 10's D1–D6 carry over unchanged)

**D1 — no new opcodes; dispatch on the type field already in the word.**
`ADD`/`SUB`/`MUL`/`DIV`/`NEG`/`CMP` grow a `T_F32 || T_F64` branch in
`simi_arm.c`'s `emit_instr()`, exactly as Phase 10 D1 did for x86. `MOD` keeps its
permanent float rejection (no single hardware float-remainder instruction on A64
either). `FLAG_IMM` + float stays rejected.

**D2 — representation: bit-reinterpret the 64-bit slot, no conversion.**
`T_F64` is an IEEE-754 double in a `d` register; `T_F32` is an IEEE-754 single in
an `s` register (the low 32 bits, high 32 zeroed on write — the ISA's existing
narrow-type convention). `regs[]` stays a unified 64-bit cell file; a float value
is the same bits read as float. This matches the interpreter's union trick and
Phase 10's decision 2 exactly.

**D3 — which A64 encoders are new, and which are NOT.**
The 66 existing encoders are all integer — float needs a small new family:
`enc_fadd`/`enc_fsub`/`enc_fmul`/`enc_fdiv` (both `d` and `s` forms, opcode `0x1E`/
`0x1E`-family "floating-point arithmetic"), `enc_fcmp` (fp compare, sets NZCV),
`enc_fmov` (move between a `d`/`s` register and the integer cache, opcode `0x9E`),
and — the one genuinely unplanned finding — **`LOAD`/`STORE` under float types
need NO new encoder at all**: A64's `ldr`/`str` (immediate) encodings carry `rt` as
a plain 5-bit register *number* with no register-class bit, so `ldr x9, [x10]` and
`ldr d0, [x10]` are the same encoding differing only in which register the number
names. The existing `enc_ldr`/`enc_str` (and the scaled/unscaled/pre/post/reg-offset
family) are reused by passing `d`-register numbers, and `a64_exec.c` already
decodes `rt` as a number. `NEG`'s float case needs no float instruction at all —
it is a sign-bit flip, exactly Phase 10's x86 finding: XOR the slot with
`0x8000000000000000` (f64) / `0x80000000` (f32) through the integer cache.
`fcvt` is NOT needed in v1: SIMI ops are typed per-instruction, so an `ADD` is
either f32 or f64, never mixed — the ISA has no conversion op.

**D4 — float register allocation: a d0/d1/d2 cache mirroring the M1 integer
cache.** The M1 three-register cache (`x9`/`x10`/`x11`) machinery
(`cache_reserve`/`cache_fetch_hosts`/eviction) is host-class-agnostic in shape;
float v1 mirrors it with `d0`/`d1`/`d2`, so arithmetic chains stay resident and
the size gate measures the same discipline. The x86 GP-bounce (Phase 10's simpler
choice) is the fallback if the mirror proves invasive — float ops read their bits
through the integer cache, compute in `d0`/`d1`, write back — at the cost of two
extra moves per float op, the same simplicity-over-speed trade x86 made.

**D5 — `CMP` float: IEEE-754 unordered semantics via `fcmp` + `cset`.**
Phase 10 D4 is non-negotiable: `LT`/`LE`/`GT`/`GE` all false on NaN, `NE` true,
`EQ` false. The x86 finding (seta/setae via operand swap for the ordered
relations, NaN folding into the flag encoding with no separate parity check) maps
onto A64 `fcmp` + `cset` — **but the exact NZCV mapping for the unordered case
must be verified against real A64 `fcmp` semantics with the NaN test before
writing the code**, not assumed from the x86 analog: this is exactly the kind of
cross-architecture assumption that silently produces a wrong answer on exactly
the case a parity test catches. The three NaN checks already in `float_ops.simi`
are the discriminator.

**D6 — the chain image (M2.x candidate-set walk) does NOT fold float ops.**
The M2.2/M2.3 argument ("plain 64-bit, never faults, type-agnostic") is about
integer pc arithmetic; a float ALU feeding a dispatch index is pathological, and
the walk's `chain_alu_eval` stays integer-only. This is a scoping statement, not a
code change — float instructions simply never enter the candidate-set machinery,
exactly as `OP_LOAD`/`OP_STORE` already don't.

**D7 — `LOADI` under float: reuse the literal pool.** Phase 10's assembler work
already emits float literals through `LOADI64`'s literal pool; the A64 side loads
the bits with the existing `li64` (movz/movk) and moves them into `d` with
`fmov` (or a plain GP-register reinterpret — the value never leaves the integer
cache for a pure constant).

## 3. Milestones

- **F0** — `a64_exec.c`: decode+execute the new encoders (`fadd`/`fsub`/`fmul`/
  `fdiv`/`fneg`-via-XOR is already executable/`fcmp`/`fmov`, d and s forms). The
  gate: `float_ops.simi` executes on the A64 executor and returns 15. Parity pin:
  interpreter vs A64-executor bit-identical on all 17 checks.
- **F1 — LANDED** — `simi_arm.c` float codegen, naive (d-cache mirror or GP
  bounce per D4), float `CMP` per D5. Gate: `float_ops.simi` PASS on the ARM
  engine; new size-gate row (naive baseline — see §3.2); the 77 existing rows
  byte-identical (float codegen is additive — no integer path changes).
- **F2 — LANDED** — the fold/allocator interaction: confirm the integer
  corpus's byte identity holds (D6's scoping means the chain machinery is
  untouched), and add `a64_enc_check.py` rows for the new encoders.
- **F3 — LANDED** — unskip `float_ops` on ARM: four-way parity (interp / x86 /
  RV64-skip / ARM), enc-check clean, gate re-measure, doc §16 Phase 10
  addendum (A64 landed).
- **F4 — LANDED** — RV64 float codegen: the F/D GP-bounce in
  `simi_riscv.c` (fmv-d/x moves, fadd/fsub/fmul/fdiv, feq/flt/fle, the
  sign-XOR NEG), `rv64_exec.c` F/D decode + f file, un-skip `float_ops`
  on the RV64 runner, four-way float parity (interp / x86 / RV64 /
  ARM all 15). The kernel-side `sstatus.FS` lazy-save (saving f0-f31
  across context switches) remains Phase 9's RISC-V kernel wiring.

### 3.1 F0 status — LANDED (a64_exec.c float decode+execute)

F0 is done: `a64_exec.c` decodes and executes the float family, and the gate
passes. The evidence, all measured:

- **Encodings verified against QEMU's `a64.decode`** (the same authoritative
  source the executor's integer families cite), cross-checked against the
  canonical S/D constants (`fadd s0,s1,s2` = 0x1E222820 vs `fadd d0,d1,d2` =
  0x1E622820 — the only difference is bit 22, the sz bit):
  - 3-same: `0001 1110 0 <sz> 1 Rm <opc6> Rn Rd` — FMUL 000010, FDIV 000110,
    FADD 001010, FSUB 001110; bit 22 = sz (0=S, 1=D), bit 23 = 1 is fp16
    (rejected).
  - FCMP: `00011110 .. 1 Rm 001000 Rn e z 000` — e@4 (quiet/signaling, same
    NZCV here), z@3 (register vs #0.0 form, both implemented).
  - FMOV scalar copy: `0001 1110 0 <sz> 1 00000 010000 Rn Rd` (s-form zeroes
    the high 32 bits, per real A64).
  - FMOV general: `sf 0011110 <type> 1 100110/100111 000000 Rn Rd` — opcode
    0x06 = FP→GP (`fmov x0,d1` = 0x9E660020, `fmov w0,s1` = 0x1E260020),
    0x07 = GP→FP (`fmov d0,x1` = 0x9E670020, `fmov s0,w1` = 0x1E270020).
- **The NZCV unordered model, verified per D5 EMPIRICALLY (M3)** — real A64
  FCMP on NaN sets **N=0, Z=0, C=1, V=1** (pinned on actual A64 via
  qemu-aarch64, `mrs nzcv` bits 31:28 — the pre-M3 N=1,Z=0 assumption was
  wrong and F1's codegen was tuned to it). From the real flags EQ/NE/GT/GE are
  IEEE-correct with a single cset eq/ne/gt/ge and NO operand swap (Z and N==V
  are both false on unordered), while **LT and LE are the classic A64 NaN
  gotcha**: naive cset lt/le read N!=V / Z||N!=V, both TRUE on unordered (N=0,
  V=1), returning 1 for NaN. F1 emits **cset mi (N)** for LT and **cset ls
  (!C || Z)** for LE, both 0 on unordered. The NaN pins in the test cover all
  six relations, not just float_ops's three.
- **The gate** (`make a64-f0-test`): a hand-assembled A64 transcription of
  `float_ops.simi`'s exact semantics (104 words, every word annotated with the
  decode pattern it came from — simi_arm.c can't emit float until F1, so this
  proves the decoder before the encoder exists). It executes to **AR_EXEC_OK,
  x0 = 15**, with every one of the 10 arithmetic results **bit-identical** to
  the C-IEEE patterns the harness derives (5.5 = 0x4016000000000000, …, -3.5f =
  0xC0600000), all **17 check values** exactly the expected 0/1s, and the two
  extra NaN GT/GE pins at 0. `NEG` runs through the sign-bit XOR path (plan D3),
  exercising `fmov x0,d0`/`eor`/`fmov d6,x0` rather than a float instruction.
- **Triple confirmation of the 15**: reference interpreter `simi-run` → 15;
  x86 JIT on real hardware (SSE, Phase 10 codegen) → 15; A64 executor → 15.
- **No regression**: the executor's new `f[32]` SIMD&FP file and decode blocks
  leave the integer corpus untouched — ARM suite 77/77 (4 skipped incl.
  float_ops), size gate byte-identical (77 rows, 26220 saved), and the interp /
  x86 / RV64 suites all green.

### 3.2 F1 status — LANDED (simi_arm.c float codegen)

F1 is done: `simi_arm.c` emits the float family and `float_ops.simi` PASSes on
A64. The evidence, all measured:

- **The D4 GP-bounce, as designed** — float ADD/SUB/MUL/DIV move the operand
  bits from the x9/x10/x11 integer cache into the FP scratch registers d0/s0
  and d1/s1 via `fmov` (GP→FP), compute there, and move the result back
  (FP→GP) into the reserved cache host: four words per float op, and d0/d1 are
  never used by the integer codegen, so there is no cross-instruction FP state
  to manage. The s-form `fmov` (GP→FP) zeroes the high 32 bits exactly like
  the interpreter's `bits_of_f32`. LOAD/STORE under float types reuse the
  integer encoders unchanged (D3: `rt` is a plain register number).
- **NEG stays a sign-bit flip (D3)** — no float instruction: the mask
  (`0x8000...0` for f64, `0x80000000` for f32) is materialized into X_T1 and
  XORed through the integer cache. The 32-bit mask flips bit 31 and leaves the
  zero-extended upper half zero, matching `bits_of_f32`.
- **CMP per the D5 finding, corrected by M3** — the real unordered model is
  N=0, Z=0, C=1, V=1 (empirically pinned), so ALL six relations emit
  `fcmp (a,b)` + a single `cset` with NO operand swap: EQ→eq, NE→ne, GT→gt,
  GE→ge (all IEEE-correct directly), LT→**mi (N)** and LE→**ls (!C || Z)**
  (the naive lt/le read N!=V / Z||N!=V, both true on unordered — the A64 NaN
  gotcha the pre-M3 swapped-operand trick was built around). The unsigned
  relations (LTU..GEU) have no float meaning and return
  `TX_AR_ERR_BAD_OPCODE`.
- **The permanent rejection boundaries, narrowed honestly** — the M0-era blanket
  rejection is gone. What remains: float MOD (no float instruction on any
  target — compose DIV+MUL+SUB), and a float op with an immediate operand
  (no float meaning — x86 parity). AND/OR/XOR/SHL/SHR/SAR under float types
  are deliberately NOT rejected: they operate on the raw slot bits exactly
  like the reference interpreter's plain integer path.
- **The gate** — `simi-arm-verify float_ops.tmo main 15` → **PASS, 15, at 2156
  bytes** (F1 cached emission). Reference interpreter → 15; x86 real hardware
  → 15.
- **The size-gate row, with the honest baseline** — M0 (git 1729f50) rejected
  float outright, so there is no real M0 byte count to measure; the row's
  baseline is the NAIVE emission of the F1 translator measured with the cache
  forced off (`g_alloc=0` — the documented invariant that the naive path
  degrades the cache helpers to exactly the M0 sequences means this is
  byte-identical to what M0 would have emitted had float been in scope).
  Measured: naive 2340, cached 2156 (−184). Gate is now **78/78 — 26404
  saved**, and the airtight diff vs the F0 commit shows **all 77 existing
  rows byte-identical** — float codegen is strictly additive, no integer path
  changed.
- **No regression** — ARM suite 77/77 (4 skipped: float_ops — the runner
  un-skip is F3's milestone — plus the standing cap_forge_debug/jmpr_oob/
  mem_ops skips), interp 79/79, x86 native 78/78, RV64 77/77, `a64-f0-test`
  still PASS.

### 3.3 F2 status — LANDED (a64_enc_check.py float rows + a real finding)

F2 is done: the independent transcription net now covers the float
encoders, and it found a genuine latent bug in its own constant walker.
The evidence, all measured:

- **The float rows** — `a64_enc_check.py` gains the scalar-FP families
  exactly as verified in F0: the 3-same FADD/FSUB/FMUL/FDIV d+s (mask
  0xFFE0FC00 pins sf+family+sz+the mandatory 1@21 and opc6; Rm/Rn/Rd
  free), FCMP d+s (opc6=001000 with e@4=0, z@3=0 and 000@2:0 pinned),
  and the four FMOV-general forms (0xFFF0FC00 pins sf/type/opc5 and the
  000000@15:10). All 14 go into `ALLOWED`; `main()` gains a `float_ops`
  branch whose 15 expected constants are derived INDEPENDENTLY from
  Python's own IEEE-754 (`struct.pack`), never transcribed from
  simi_arm.c — the f64 literals as full 64-bit patterns, the f32
  literals as the raw 32-bit pattern zero-extended to 64 (the
  assembler's `(uint64_t)c.u` storage), plus the NaN loaded as i64.
- **The real finding — the checker's own latent bug.** The M0-era
  `li64_chains` assumed a constant chain's movk halves are CONTIGUOUS
  (`h2 == j - i`). That held for every pre-F2 corpus constant (low-half
  values like 1/2/10, or full-width values like -8 whose halves are all
  nonzero) — but `emit_li64` actually SKIPS ZERO HIGH HALVES (movz
  already zeroed the register), so float_ops's f64 literals (e.g.
  3.5 = 0x400C000000000000, whose only nonzero halves are hw2/hw3)
  silently reconstructed the wrong value. The float_ops row tripped it
  on the first run (8 of 15 constants "missing"); the f32 values were
  found only because their chains are contiguous. Fixed: accept any
  strictly-increasing movk half, matching emit_li64's real emission.
  This is exactly the error class the net exists for — and the fix is
  in the CHECKER, not the translator (simi_arm.c was never wrong here).
- **The teeth (ad hoc)** — (b) replacing one emitted FP word with a
  non-instruction fails decode (MISMATCH body: 1 word, `00000000 ->
  None`); (c) flipping one movk half inside an f64 chain so the chain
  still DECODES but encodes the wrong value is caught by the constant
  check (`missing 0x400c000000000000` = 3.5). The net has teeth on both
  the class side and the value side for the float encoders.
- **The gate** — `make a64-enc-check`: all four programs green (16 OK
  rows: trampoline/prologue/body-structure/body-constants each),
  including float_ops's 539-word body with all 15 constants. The integer
  rows are byte-identical (the checker change is additive: new decode
  classes + the li64_chains fix, which provably changes nothing for
  contiguous chains). Size gate byte-identical (78/78, 26404 saved —
  simi_arm.c untouched), ARM suite 77/77, interp 79/79, x86 native
  78/78, RV64 77/77, `a64-f0-test` PASS, float_ops on ARM = 15 at 2156.

### 3.4 F3 status — LANDED (float_ops runs on ARM; four-way parity)

F3 is done: the ARM runner un-skips `float_ops`, and the float program now
has a genuine four-way execution parity. The evidence, all measured:

- **The un-skip** — `tests/run_arm_tests.sh` removes the `float_ops`
  skip case and its header rationale (M0-era "scoped out" text),
  replacing it with the F1/F2 story: simi_arm.c emits real IEEE-754
  words, a64_exec.c decodes/executes them, so ARM joins the float
  parity. The runner is now 78 passed / 0 failed / 3 skipped (the
  standing cap_forge_debug, jmpr_oob, and mem_ops skips).
- **The four-way parity** — the SAME `.tmo` and SAME `Expected result:
  15`: interpreter → 15; real x86-64 JIT (SSE, Phase 10 codegen) → 15
  at 2426 bytes; A64 executor → 15 at 2156 bytes; RV64 → its expected
  explicit rejection (`TX_RV_ERR_FLOAT_UNSUPPORTED`, scoped out of
  Phase 10 v1 — the documented fourth leg, not a regression). Every
  arithmetic result, all 17 checks, and the three NaN unordered-compare
  cases agree across the engines that execute it.
- **No regression** — size gate byte-identical (78/78, 26404 saved —
  simi_arm.c untouched; the runner change is test-side only), interp
  79/79, x86 native 78/78 (float_ops among them), RV64 77/77, enc-check
  clean, `a64-f0-test` PASS.
- **ISA doc §16 Phase 10 addendum** — records the A64 landing (F0-F3)
  next to the x86/interpreter findings: the GP-bounce, the sign-XOR
  NEG, the D5 real-model CMP (mi/ls for LT/LE after M3 corrected the
  unordered flags — the swapped-operand design is retired, see §10.179
  of the ARM plan), the two permanent rejection boundaries, and the
  four-engine parity numbers.

### 3.5 F4 status — LANDED (RV64 F/D codegen; four-way float parity complete)

F4 is done: `simi_riscv.c` emits the scalar F/D GP-bounce, `rv64_exec.c`
decodes and executes it against a new f file, and `float_ops` runs on the
RV64 engine — the four-way float parity (interp / x86 / RV64 / ARM) is
now complete on every engine. The evidence, all measured:

- **The codegen (GP-bounce)** — same choice x86 (XMM0/XMM1) and A64
  (d0/d1) made: operand bits arrive in the integer scratch regs t0/t1,
  bounce into f10/f11 (`fmv.d.x` / `fmv.w.x`), compute with the real
  F/D instruction (`fadd`/`fsub`/`fmul`/`fdiv`, d and s forms), bounce
  back (`fmv.x.d`). f32 results go through `fmv.x.w` then a
  `slli`/`srli` zero-extend, because FMV.X.W sign-extends per the
  RV64 ABI convention while SIMI's f32 convention is high-32-zeroed
  (the interpreter's raw-bits contract). `NEG` is a pure sign-bit XOR
  through the integer cache (the x86/A64 finding). `CMP` maps 1:1 onto
  `feq`/`flt`/`fle` — which RETURN an integer 0/1 with exactly the
  IEEE-754 unordered semantics the interpreter's C operators give:
  feq is 0 for NaN, flt/fle are 0 for any unordered pair; GT/GE swap
  the operands, NE is feq then xori 1. `rv64_exec.c` gains the f file
  and the OP-FP (0x53) decode: the arithmetic/compare subset with the
  D-vs-S fmt bit, and the four fmv moves (FMV.X.W sign-extending per
  spec).
- **Two latent sign-extension bugs found by the F4 tooth, one on each
  of the two targets** — the interpreter, x86, and the RV64 translator
  all zero-extend f32 loads (raw-bits convention), but `simi_riscv.c`'s
  `load_typed` mapped T_F32 to `i_lw` (SIGN-extends) and `simi_arm.c`'s
  three load forms (scaled/unscaled/register-offset) mapped T_F32 to
  LDRSW/LDURSW/LDRSW-reg (also sign-extending). Both were invisible
  because `float_ops` never LOADs an f32 from memory (only LOADI64 +
  arithmetic + CMP); a new f32 store→load→raw-bits tooth (negative
  -3.5f32, checked as i64 against 0x00000000C0600000, plus NEG-after-
  load) exposed both. Fixed: RV64 T_F32 → `i_lwu`, ARM T_F32 → the
  zero-extending `ldr w`/`ldur w`/`ldr w, reg` forms — one word for
  one word, so no existing program's emission changes. (The tooth's
  first draft failed on every engine including the interpreter — the
  expected constant was wrong, 3224371200 vs the true 3227516928 =
  0xC0600000; the engines were all correct, exactly the false alarm
  the cross-check isolates.)
- **The un-skip** — `run_riscv_tests.sh` removes the A0-era skip
  block; `float_ops` now RUNS on the RV64 engine through
  `simi-riscv-verify`: **float_ops = 15 at 2380 bytes** of RV64 code.
- **The four-way float parity** — interp 15, x86 15 (2426 bytes),
  RV64 15 (2380 bytes), ARM 15 (2156 bytes) — every arithmetic result,
  relation, and the three NaN unordered-compare cases bit-identical
  across all four engines. The f32-load tooth: 2/2/2/2.
- **The rejection teeth** — float MOD, FLAG_IMM + float arithmetic,
  and unsigned float CMP relations all reject cleanly at translate
  time on every native translator (the ARM strerror's stale
  "scoped out of M0" message was updated to match x86/RV64's
  "operand combination has no float meaning (immediate operand, or
  float MOD)").
- **No regression** — interp 81/81, x86 native 80/80, RV64 **80/80**
  (+1, float_ops now runs), ARM 80/80, size gate byte-identical
  (80/80, 26472 saved — the f32-load fixes are word-for-word), enc-
  check clean (24 OK / 0 MISMATCH), a64-f0-test PASS.
- **Kernel side** — `kernel/simi_riscv.c` mirrors the body changes
  byte-identical below its differing header. The remaining kernel
  work is the `sstatus.FS` lazy-save (saving f0-f31 across context
  switches, the `lazy_vector.c` twin) — Phase 9's RISC-V kernel
  wiring, honestly out of this tools-side pass.

## 4. Honest verification caveats

- `a64_exec.c` is a purpose-built decoder+executor, **not real ARM hardware** —
  the same evidence class as RV64's `rv64_exec.c`, weaker than x86's
  real-CPU `PROT_EXEC` run. A matching bug in the encoder and the executor would
  not be caught; the interpreter cross-check (host C float is the IEEE reference)
  is the independent third opinion.
- The `fcmp` NaN flag mapping (D5) is verified by the three NaN checks in
  `float_ops.simi` — the plan requires this to be demonstrated, not assumed.
- Rounding: default round-to-nearest on both the interpreter (host C) and A64 —
  bit-identical for normal values; subnormal/rounding-edge cases are covered by
  the existing fixture's scope, not exhaustively.
- The size gate gains new rows with M0 baselines measured at git 1729f50, like
  every other row; the 77 existing rows must stay byte-identical (F1's gate).

---

# Part II — Atomics (a new ISA phase)

## 5. The gap, precisely

SIMI has no way to express shared-memory synchronization: no compare-and-swap, no
atomic read-modify-write, no ordering hints. The ISA word already hints at the
intent — §4 line 56 reserves "volatile/atomic markers" in the flags field — and
the flags field has three free bits (bits 1–3), but no op, no flag, and no
semantics exist. For a kernel IR (the AeroSLS context), the first real workloads
that hit this are lock-free queues, reference counts, and cross-CPU
coordination — none of which can be written today, and all of which need at most
two primitives to compose.

## 6. Design decisions

**D1 — the op set, minimal and shaped like x86's `cmpxchg`/`xadd` (which every
target maps onto cleanly).**
- `OP_CAS`: `CAS rD, rA, rB, TYPE` — atomically: `old = [rA]`; if `old == rB`,
  `[rA] = rD`; `rD = old`. The `rD` slot is both input (the new value) and output
  (the old value) — the x86 `cmpxchg` operand shape, which is exactly how A64's
  `cas` and RV64's `lr`/`sc` loops and x86's `lock cmpxchg` all express the
  operation. No displacement in v1 (the `imm` field is unused/zero) — address is
  the full `rA` cell.
- `OP_ATOMIC_ADD`: `ATOMIC_ADD rD, rA, rB, TYPE` — atomically: `old = [rA]`;
  `[rA] = old + rB`; `rD = old` (the returned old value makes it composable:
  fetch-and-add for ticket locks, `old + rB` for counters).
- Types: `T_I32`/`T_I64` in v1 (and the unsigned views); the `TYPE` field already
  carries them. No float atomics, no byte atomics in v1 (composable via masking if
  ever needed — keep v1 minimal).
- **Ordering: SC (sequentially consistent) in v1** — no acquire/release bit yet.
  The free flags bits are reserved for a later ordering extension; SC is the
  conservative default and the only thing the four-way parity harness could
  observe anyway.

**D2 — A64: exclusive-monitor loops (`ldaxr`/`stlxr`), NOT the ARMv8.1 LSE single
instructions.** `CAS` is `ldaxr`/`cmp`/`b.ne`/`stlxr`/`cbnz` retry; `ATOMIC_ADD`
is `ldaxr`/`add`/`stlxr`/`cbnz` retry. Rationale: the exclusive path works on
ARMv8.0 (no LSE dependency), the loop is small and well-understood, and —
decisively for this project's verification model — `a64_exec.c` decodes the loop
as two ordinary load/store encodings (`enc_ldaxr`/`enc_stlxr`) with a tiny
exclusive-monitor model, rather than needing the whole LSE instruction family in
the decoder. (LSE `ldadd`/`cas` is a later single-instruction optimization, not
v1.)

**D3 — x86: `lock cmpxchg` (CAS) and `lock xadd` (ATOMIC_ADD)** — the two
instructions the op set is literally shaped after. The JIT harness runs on real
hardware, so the lock prefix semantics are real.

**D4 — RV64: `lr.d`/`sc.d` loop (CAS) and `amo.add.d` (ATOMIC_ADD)** — the `A`
extension is already implied by the kernel's `rv64gcv` march. `rv64_exec.c` gains
the four encodings (`lr.d`, `sc.d`, `amo.add.d`, plus the `sc.d` failure path).

**D5 — interpreter: trivial single-threaded direct semantics** — the parity
reference. `CAS` and `ATOMIC_ADD` are plain load-compare-store / load-add-store in
one thread; correctness of the *semantics* (return values, success/failure) is
what the cross-check proves.

**D6 — assembler/ISA plumbing**: two new enum entries + `SIMI_OPS` rows + formats
(`FMT_RRR` covers both: rD/rA/rB are all registers), the `FLAG_ATOMIC`-style
marker is *not* needed since the ops are explicit opcodes (the free flags bits
stay reserved for the future ordering extension), `a64_enc_check.py` rows, and a
`§7` roadmap row + `§16` phase entry (next free phase number per §16's
Phases 9–14 numbering — the exact number follows the convention the ARM backend's
§16 entry established).

## 7. Milestones

- **A0 — LANDED** — ISA: enum + `SIMI_OPS` + formats, assembler/disassembler,
  the §7/§16 entries, the interpreter semantics, and the two new test fixtures
  (`cas_simple.simi`: success, failure, returned-old cases; `atomic_add.simi`:
  a counter incremented N times, expected final value + returned olds).
- **A1 — LANDED** — x86 translator: `lock cmpxchg` for CAS, `lock xadd` for
  ATOMIC_ADD, un-skipped in the native runner, three-way parity
  (interp / x86 / RV64-reject), no existing test's result changed.
- **A2 — LANDED** — RV64 (`lr`/`sc`, `amo.add.d`) + `rv64_exec.c` +
  four-way parity (interp / x86 / RV64 / ARM-reject).
- **A3 — LANDED** — A64 (`ldaxr`/`stlxr` loops) + `a64_exec.c`
  exclusive-monitor model + enc-check + size-gate rows + full four-way
  parity (interp / x86 / RV64 / ARM).
- **A4 (later, only if a consumer needs it)** — acquire/release ordering bits
  using the reserved flags slots, once a real kernel spinlock or queue needs
  non-SC semantics; v1 ships SC-only.

### 7.1 A0 status — LANDED (ISA + interpreter + fixtures)

A0 is done: the atomics phase has real ISA plumbing and interpreter
semantics, verified by two permanent fixtures. The evidence, all
measured:

- **The ISA plumbing, as designed (D6)** — `OP_CAS`/`OP_ATOMIC_ADD`
  appended before `OP_COUNT` in `simi_isa.h` (append, never insert — the
  enum values are the wire encoding), two `SIMI_OPS` rows both
  `FMT_RRR`, and a doc comment recording the v1 scope (4/8-byte types
  only, register operands only, SC ordering, the three free flags bits
  reserved for the future acquire/release extension). No new format was
  needed — `FMT_RRR` already parses/prints `OP rD, rA, rB, TYPE` in the
  assembler and disassembler unchanged.
- **The interpreter semantics (D5), as the single-threaded parity
  reference** — `OP_CAS` and `OP_ATOMIC_ADD` in `simi_interp.c`:
  width-aware cell access via `width_of` (4/8-byte only; byte/float
  atomics `die()` with a clear message), `FLAG_IMM` rejected (register
  operands only — there is no displacement form), bounds-checked against
  the simulated memory, rD written with the returned old value (raw
  bits, zero-extended — "the cell is compared and returned as stored",
  no sign extension). CAS compares with a width mask, so an i32 CAS
  against an expected value whose high bits are garbage still matches
  on the cell's bits.
- **The fixtures** — `cas_simple.simi` (5 checks → **5**: success
  updates the cell and returns the old value, failure leaves the cell
  unchanged and returns the cell's value, a second success returns 200)
  and `atomic_add.simi` (2 checks → **2**: a counter incremented 10
  times, final cell 10, sum of the ten returned olds 0+..+9 = 45). Both
  address memory via `LEA r6, r7, #0` — the r7 scratch-pointer
  convention — so they are engine-agnostic (interp r7=0, natives
  r7=scratch_ptr), ready for A1-A3 unchanged.
- **The gate** — interpreter suite **81 passed / 0 failed** (was 79;
  both fixtures joined the permanent corpus). x86 / RV64 / ARM suites
  stay green with the two fixtures skipped (translate-time `BAD_OPCODE`
  rejection until A1/A2/A3 land — the float_ops skip discipline); no
  existing test's result changed anywhere.
- **The teeth (ad hoc)** — an immediate-operand CAS traps cleanly
  (`CAS has no immediate form in v1`), an i32-width CAS round-trips
  (cell 100 → CAS 1000 → returned 100, cell 1000, sum 2), and all three
  native translators reject the fixtures at translate time as expected
  before the skips were added.
- **ISA doc §16** — Phase 15 entry records the design decisions (the
  cmpxchg/xadd-shaped op set, the SC ordering + reserved-bits note) and
  this A0 status; the §7 roadmap row shows Phase 15 as 🟡 (A0 done, A1-A3
  pending).

### 7.2 A1 status — LANDED (x86 `lock cmpxchg` / `lock xadd`)

A1 is done: the x86 translator emits real locked atomic instructions and
both fixtures join the native suite. The evidence, all measured:

- **The codegen, as designed (D1/D5)** — `simi_x86.c` gains `OP_CAS` and
  `OP_ATOMIC_ADD` cases, and the kernel copy (`kernel/simi_x86.c`) is
  re-synced byte-for-byte below its differing header comment. The base
  pointer is staged in `rdx` (the fixed scratch register DIV/MOD already
  clobber freely — never in the allocation pool); CAS puts the expected
  value in `rax` (cmpxchg's implicit accumulator) and the new value in
  `rcx`, then `lock cmpxchg [rdx], rcx`; ATOMIC_ADD puts the addend in
  `rcx`, then `lock xadd [rdx], rcx`. No displacement (FMT_RRR's rb
  field is a register — disp = 0). `st_rax_untag`/`st_rcx_untag` clear
  rD's capability tag, matching the interpreter. The 64-bit forms emit
  REX.W; the 32-bit forms compare/store on EAX/ECX only, so high garbage
  bits in the expected/new values are ignored exactly like the
  interpreter's width-masked compare.
- **The real finding (a genuine x86 semantics bug, caught by the i32
  tooth)** — on a *successful* `cmpxchg`, the accumulator is **not
  written**, so the 32-bit form's RAX still carries the expected value's
  garbage high bits and the returned "old" would be wrong. Fixed by
  zero-extending EAX (`mov eax,eax`, a no-op on the mismatch path where
  EAX is already loaded from memory) after the 32-bit cmpxchg. The
  interpreter's width-masked returned-old semantics are now reproduced
  bit-for-bit.
- **The fixtures un-skipped** — `run_native_tests.sh` drops the A0-era
  skip block; both fixtures now RUN on real x86-64 hardware (the mmap'd
  scratch region stands in for process memory): **cas_simple = 5 at 995
  bytes, atomic_add = 2 at 822 bytes** of native code.
- **The three-way parity** — interp 5/2, x86 5/2, RV64 clean
  translate-time rejection (`unsupported or malformed opcode`, the
  documented fourth leg until A2).
- **The teeth (ad hoc)** — an i32-width CAS with garbage high bits in
  the expected value (0x10000000064 vs a cell holding 100) matches on
  the low 32 bits, stores the new value, and returns the zero-extended
  old (the width-mask semantics, now on the native path too); an
  immediate-operand CAS is rejected at translate time (`BAD_OPCODE`).
- **No regression** — interp 81/81, x86 native 80/80 (+2, 3 pre-existing
  skips), RV64 77/77, ARM 78/78, size gate byte-identical (78/78, 26404
  saved — simi_arm.c untouched), enc-check clean.

### 7.3 A2 status — LANDED (RV64 `lr`/`sc` + `amo.add.d`)

A2 is done: the RV64 translator emits the A-extension atomics the plan's
D4 design specified — the `lr`/`sc` loop for CAS, `amo.add` for
ATOMIC_ADD — and `rv64_exec.c` decodes them, so both fixtures run on the
RV64 engine and the four-way parity is complete except the A3 ARM leg.
The evidence, all measured:

- **The codegen (D4)** — `simi_riscv.c` gains `OP_CAS`/`OP_ATOMIC_ADD`
  cases; the kernel copy (`kernel/simi_riscv.c`) is re-synced
  byte-identical below its differing header. `t0` holds the base
  pointer, `t1` the new value (CAS) or addend (ATOMIC_ADD, rB — the
  rD-old-out-only shape matches the interpreter); CAS's expected value
  (rB) lives in `a1` (the one documented fourth-scratch exception —
  the `lr` would clobber it otherwise) and the loop is
  `lr.d`/`cmp`/`bne .done`/`sc.d`/`bne .retry`, with the success path
  recovering `old` from `a1` (they're equal there). ATOMIC_ADD is the
  single `amo.add.d`. The three AMO encoders emit aq=rl=1. The 32-bit
  forms (`lr.w`/`sc.w`/`amo.add.w`) mask the expected value and
  zero-extend the returned old via `slli`+`srli` — `lr.w`/`amo.add.w`
  sign-extend per spec, and the interpreter's contract is "the cell's
  raw bits, zero-extended," so the A1 i32 shape (garbage high bits in
  the expected value) is reproduced exactly.
- **`rv64_exec.c` gains the AMO opcode (0x2F)** — `lr.w/d`, `sc.w/d`,
  `amo.add.w/d` decode with funct3 width (0x2/0x3), sign-extending
  `lr.w`/`amo.add.w` loads per spec; the single-threaded model means
  `sc` always succeeds (`rd = 0`), so the retry path never executes
  here — the plan's "sc failure path" is implemented but unexercised
  until real hardware (the documented caveat).
- **The fixtures un-skipped** — `run_riscv_tests.sh` drops the A0-era
  skip block; both fixtures now RUN on the RV64 engine through
  `simi-riscv-verify`: **cas_simple = 5 at 1472 bytes, atomic_add = 2
  at 1244 bytes** of RV64 code.
- **The four-way parity** — interp 5/2, x86 5/2 (unchanged from A1:
  995/822 bytes), RV64 5/2, ARM clean translate-time rejection (`the
  documented fourth leg until A3`).
- **The teeth (ad hoc)** — an i32-width tooth (garbage-high-bits
  expected CAS + a 3×7 i32 ATOMIC_ADD counter at a distinct base —
  ATOMIC_ADD has no displacement, so the counter lives at `r7+4` via a
  second LEA): interp 4, x86 4, RV64 4, ARM-reject. (The tooth's first
  draft failed identically on interp and x86 — it stored the counter at
  `[r6+4]` but `ATOMIC_ADD` always operates at `[rA+0]`; the engines
  were all correct.)
- **No regression** — interp 81/81, x86 native 80/80, RV64 **79/79**
  (+2, the two fixtures), ARM 78/78 (+2 skipped until A3), size gate
  byte-identical (78/78, 26404 saved — simi_arm.c untouched), enc-check
  clean.

### 7.4 A3 status — LANDED (A64 `ldaxr`/`stlxr` loops + exclusive-monitor model)

A3 is done: the A64 translator emits the exclusive-monitor loops the
D2 design specified, `a64_exec.c` models the monitor, and both fixtures
run on the ARM engine — the four-way parity is now complete on all
four engines. The evidence, all measured:

- **The codegen (D2)** — `simi_arm.c` gains `OP_CAS`/`OP_ATOMIC_ADD`
  cases (mirror enum + the shared reg-range check, `FLAG_IMM`/width
  rejects). The instruction needs FOUR persistent hosts, one more than
  the 3-slot cache, so the cache flushes first (like RESOLVE's runtime
  call), the result host is reserved for rd, base and new/addend take
  the two non-rh slots, and the expected (CAS only) lives in x12
  (outside the cache — no run-reuse displacement is live at an atomics
  pc). CAS is `ldaxr`/`cmp`/`b.ne .done`/`stlxr`/`cbnz .retry`, with
  the success path recovering `old` from x12 via one `mov` — the
  `stlxr` status register doubles the result host (on success old ==
  expected; on retry the next ldaxr overwrites it), zero extra words
  for the plumbing. ATOMIC_ADD is `ldaxr`/`add`/`stlxr`/`cbnz .retry`
  with the sum in x12 and old staying in rh untouched — no recovery
  mov at all. Width masking: the 32-bit expected is masked with
  `mov w12, w12` (orr-32) and the `stlxr w` store truncates to the low
  32 bits, matching the interpreter's width-masked compare + wdt-byte
  store exactly (the A1 i32 tooth's shape).
- **A latent `enc_b_cond` bug, found and fixed** — the encoder wrote
  cond at bits 12–15 but ARM (and `a64_exec.c`) read it at bits 3:0.
  Every pre-A3 caller used cond = 0 (`b.eq`), where the two layouts
  agree, so the wrong placement was invisible for months; the atomics'
  `b.ne` (cond = 1) exposed it — the CAS mismatch branch decoded as
  `b.eq` on the executor and the compare fell through into the stlxr.
  Fixed at the source; the enc-check's independent `bcond` decoder
  carried the same latent 15:12 reading and was fixed there too.
- **The exclusive-monitor model** — `A64Cpu` gains `excl_valid` /
  `excl_addr`; LDAXR arms the monitor, STLXR succeeds iff it is valid
  and matches then clears it, and every ordinary STR path clears it
  too. The single-threaded guest's atomics loops emit no store between
  the pair, so stlxr succeeds first try here — the retry path is
  implemented but unexercised until real hardware (the documented
  caveat). `a64_exec.c` decodes the whole exclusive family; the i32
  tooth's `mov w12, w12` (orr-32) decode was added with it.
- **The fixtures un-skipped** — `run_arm_tests.sh` drops the A0-era
  skip block; both fixtures now RUN on the ARM engine through
  `simi-arm-verify`: **cas_simple = 5 at 1372 bytes, atomic_add = 2 at
  1196 bytes** of A64 code.
- **The four-way parity** — interp 5/2, x86 5/2 (unchanged: 995/822
  bytes), RV64 5/2 (unchanged: 1472/1244 bytes), ARM 5/2. The i32
  tooth (garbage-high-bits CAS + 3×7 ATOMIC_ADD counter) passes on all
  four: interp 4, x86 4, RV64 4, ARM 4.
- **Enc-check** — `a64_enc_check.py` gains independent `ldaxr`/`stlxr`
  W/X encoders + decode rules + ALLOWED classes and two new program
  rows (`cas_simple`, `atomic_add` — the Makefile assembles/dumps
  them); 6 programs, 24 OK / 0 MISMATCH. The decode verifies every
  emitted word — including the exclusive words — classifies into a
  legal A64 class.
- **Size gate** — the two new rows use honest NAIVE baselines (the
  programs didn't exist at M0; measured with the A3 translator forced
  to `g_alloc=0`, the float_ops precedent): cas_simple 1416→1372
  (**-44**), atomic_add 1220→1196 (**-24**). **80/80 rows, saved
  26472** (was 26404 — the +68 is exactly the two new rows; all 78
  existing rows byte-identical).
- **No regression** — interp 81/81, x86 native 80/80, RV64 79/79, ARM
  **80/80** (both fixtures now run), enc-check clean, a64-f0-test PASS.

## 8. Honest verification caveats

- **The four-way parity harness is single-threaded — CLOSED for x86 by the
  real-hardware stress harness (`tools/simi/stress_atomics.c`).** The parity
  harness proves the FUNCTIONAL semantics (CAS success/failure and returned-old
  values, atomic-add results) bit-identical across all four engines; it cannot
  prove concurrency semantics (mutual exclusion, no lost updates under
  contention, ordering). That gap is now closed for the x86 leg by
  `stress-atomics`: it translates `tests/stress_atomics.simi` (entries `main`
  and `consumer`, sharing one scratch region) and runs it under pthreads, where
  the A1 codegen's `lock cmpxchg` / `lock xadd` contend on REAL CPU cores —
  M producer threads hammer a shared ATOMIC_ADD counter and CAS-claim the next
  free slot of a bounded lock-free queue (writing their tid), while a single
  consumer thread drains the queue concurrently. Every run checks seven
  invariants: the counter equals M×R exactly (no lost updates), the CAS claim
  index equals M×R (every claim succeeds exactly once), each of the M×R slots
  holds exactly one producer's tid with a per-tid histogram of exactly R
  (no double-claim, no skip, no lost write), the concurrent drainer sees all
  M×R items (deq == M×R, 0 out-of-range values), and the M producer returns are
  a permutation of 0..M-1 (the ATOMIC_ADD ticket claim gives each thread a
  unique tid). Measured on this machine (12 WSL vCPUs): 8×20000 → 160000,
  16×16000 / 32×8000 / 64×4000 → 256000, 64×10000 → 640000 slots, up to
  **1,920,064 lock ops per run, every count exact at every thread count, all
  checks passed**. The negative control proves the checks have teeth: a plain
  non-atomic `cnt++` under the same load shape (8 threads × 2M) landed at
  **7,398,330 of 16,000,000 — 53% of updates lost** — so an exact-count pass on
  the SIMI-emitted `lock` codegen is meaningful, not trivially reachable. The
  concurrent drainer's spin pattern (observe the enqueue counter, then read the
  slot) is sound on x86 TSO — the producer's value store precedes its `lock
  xadd` (a full barrier) — and is the documented ARM/RV64-unsound-without-A4
  shape: **the ARM and RV64 legs still need real boards or QEMU (neither
  available in this sandbox), or the deferred A4 acquire/release extension,
  before their concurrency semantics can be claimed**. Stated plainly: this
  plan delivers bit-identical single-threaded semantics everywhere, and the
  concurrency proof on real hardware for x86 — the one target with real
  hardware available (the same honesty standard Phase 5 used for its
  decoder+executor evidence class).
- `a64_exec.c`'s exclusive monitor must be modeled correctly — `ldaxr` marks the
  address, `stlxr` succeeds only if still marked, else the loop retries — or the
  loop spins forever. It is (A3): `excl_valid`/`excl_addr`, LDAXR arms, STLXR
  succeeds iff valid and matching then clears, every ordinary STR to the
  watched address clears too. The single-threaded guest emits no store between
  the pair, so stlxr succeeds first try here — the retry loop is implemented
  but its failure path is exercised only on real hardware; the fixtures' CAS
  failure-path checks exercise the `cmp`/`b.ne` mismatch path instead.
- The size gate: the two new programs get NAIVE (`g_alloc=0`) baselines
  measured with the A3 translator forced to g_alloc=0 — they did not exist at
  M0, and the naive path degrades to exactly the M0 sequences (the float_ops
  precedent); all 78 existing rows byte-identical, the two new rows additive
  (80 rows total, both parts strictly additive).

### 7.5 A-stress status — LANDED (real-hardware x86 stress harness)

The §8 concurrency gap is closed for the x86 leg: `tools/simi/stress_atomics.c`
(+ `tests/stress_atomics.simi`, + the `stress-atomics` Makefile target, in
`all`) runs the atomics under pthreads on real hardware — M producers hammer
an ATOMIC_ADD shared counter and CAS-claim slots of a bounded lock-free queue
while one consumer drains concurrently. Measured: every count exact at
8/16/32/64 threads (up to 1,920,064 lock ops/run), the ticket leg yields a
unique tid per thread, the slot walk shows exactly R writes per thread, and a
plain non-atomic `cnt++` control loses 53% of updates on the same machine —
the exact-count checks are meaningful. `stress_atomics` also joined the
four-way parity corpus (single-threaded result 0) and the size gate
(naive 2632 → cached 2568, -64). A4 (acquire/release on the reserved flags
bits) stays deferred; the ARM/RV64 legs of the concurrency proof still need
real hardware.

---

## 9. Sequencing note

Both parts are independent of each other and of the in-flight M2.x chain work.
Part I's F0–F4 can proceed immediately in this workspace (all tooling exists; the
m0 worktree, gate, parity harness, and enc-check are all in place). Part II's
A0–A3 likewise. A4 (ordering bits) remains deferred by its own scoping, not by
this plan. The one cross-cutting rule from Phase 10 D1 carries
through both parts: **no ISA format surgery** — the type tags, opcode slots
(225 free), and flags bits (3 free) already reserve everything both parts need.
