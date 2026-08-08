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
- **F4 (deferred, rides Phase 9)** — RV64 float codegen per Phase 10 D6 (the
  `sstatus.FS` lazy-save mechanism), a separate later pass, not this plan's work.

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
- **The NZCV unordered model, verified per D5** — FCMP sets N=1, Z=0, C=1, V=1 on
  NaN; from those flags EQ/NE/LT/LE come out IEEE-correct with a single cset,
  but **GT and GE are wrong with a single cset** (unordered → `!Z && N==V` and
  `N==V` are both true). F1 therefore compares the swapped operands
  (`fcmp b,a` + `cset lt/le`) for GT/GE — the exact x86 seta/setae-via-swap
  finding — and this executor's flag model is what makes both the direct and
  swapped paths right. The NaN pins in the test cover all six relations, not
  just float_ops's three.
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
- **CMP per the D5 finding** — EQ/NE/LT/LE emit `fcmp (a,b)` + a single
  `cset`; GT/GE emit `fcmp (b,a)` + `cset lt/le` (the swapped-operand trick F0
  verified, because a single `cset gt/ge` is wrong on NaN's N=1,Z=0,C=1,V=1).
  The unsigned relations (LTU..GEU) have no float meaning and return
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
  NEG, the D5 swapped-operand CMP, the two permanent rejection
  boundaries, and the four-engine parity numbers.

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

- **A0** — ISA: enum + `SIMI_OPS` + formats, assembler/disassembler, the §7/§16
  entries, and the two new test fixtures (`cas_simple.simi`: success, failure,
  returned-old cases; `atomic_add.simi`: a counter incremented N times, expected
  final value + returned olds).
- **A1** — interpreter + x86 (`lock cmpxchg`/`lock xadd`) + three-way parity
  (interp / x86 / RV64-reject), no existing test's result changed.
- **A2** — RV64 (`lr`/`sc`, `amo.add.d`) + `rv64_exec.c` + four-way parity.
- **A3** — A64 (`ldaxr`/`stlxr` loops) + `a64_exec.c` exclusive-monitor model +
  enc-check + size-gate rows + full four-way parity.
- **A4 (later, only if a consumer needs it)** — acquire/release ordering bits
  using the reserved flags slots, once a real kernel spinlock or queue needs
  non-SC semantics; v1 ships SC-only.

## 8. Honest verification caveats

- **The four-way parity harness is single-threaded.** It proves the FUNCTIONAL
  semantics — CAS success/failure and returned-old values, atomic-add results —
  bit-identical across all four engines. It CANNOT prove concurrency semantics:
  mutual exclusion, no lost updates under contention, ordering. That needs a
  real-hardware multi-threaded stress harness — x86 has real hardware, but ARM
  and RV64 need real boards or QEMU, neither available in this sandbox. Stated
  plainly: **this plan delivers bit-identical single-threaded semantics and
  documents the concurrency proof as a real-hardware follow-up, not glossed
  over** (the same honesty standard Phase 5 used for its decoder+executor
  evidence class).
- `a64_exec.c`'s exclusive monitor must be modeled correctly — `ldaxr` marks the
  address, `stlxr` succeeds only if still marked, else the loop retries — or the
  loop spins forever. A small, well-scoped executor addition, but a real one;
  the CAS fixture's failure-path check exercises the retry.
- The size gate: the two new programs get M0 baselines at git 1729f50; all 79
  existing rows must stay byte-identical (both parts are strictly additive).

---

## 9. Sequencing note

Both parts are independent of each other and of the in-flight M2.x chain work.
Part I's F0–F3 can proceed immediately in this workspace (all tooling exists; the
m0 worktree, gate, parity harness, and enc-check are all in place). Part II's
A0–A3 likewise. F4 (RV64 float) and A4 (ordering bits) are deferred by their own
scoping, not by this plan. The one cross-cutting rule from Phase 10 D1 carries
through both parts: **no ISA format surgery** — the type tags, opcode slots
(225 free), and flags bits (3 free) already reserve everything both parts need.
