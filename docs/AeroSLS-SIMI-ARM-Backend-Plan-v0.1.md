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

**M2 — kernel copy + honest kernel-side gap note — DONE (M3's kernel half, §10.180).**
Byte-identical `kernel/simi_arm.{c,h}` following the Phase 3/5 port convention
(shortened header pointing back at the host copy; re-diffed and confirmed
identical below it), compiled under freestanding flags with zero warnings.
The copy was staged when M3's real-execution leg landed, so it inherits M3's
verification rather than the plan's original "unverified" framing: the host
copy it is byte-identical to is now proven by qemu-aarch64 real execution
(§10.178/10.179), and its freestanding compile is a CI-enforced gate.
- **Gate:** the port compiles clean under the kernel's freestanding flags
  (zero warnings, enforced by the arm64-guards CI job), and the header records
  the open gap honestly: *executing* A64 in a kernel still requires an arm64
  kernel build — the copy is compiled nowhere and linked into nothing; there is
  no arm64 kernel target to wire it into.

**M3 — CLOSED (qemu-aarch64 leg), optional (arm64 kernel half).** The
real-execution proof landed via `qemu-aarch64` user-mode: `simi_arm_jit.c`
(§10.178) translates each corpus fixture with the exact `simi_arm.c`, mmaps
the emitted words executable, and ACTUALLY branches into them on a real A64
implementation (qemu-aarch64 user-mode running a static aarch64-linux-gnu
ELF — the same translator that will port into the kernel). The emitted `blr`
host calls land in real C functions under the real AAPCS64 convention, and
the whole corpus runs with results matching the four-way expected values,
closing the two decoder-blind gaps: an encoder+decoder that agree but are
every way wrong (M3's first run PROVED it — it found two such bugs, the
JMPR table's guest-relative addressing and the FCMP unordered flag model,
both fixed in §10.179), and A64 architectural rules (SP 16-byte alignment,
4-byte branch alignment) `a64_exec` is documented laxer about. The runner
(`run_arm64_tests.sh`) skips cleanly without the toolchain (the
"environment-dependent" premise) and the `arm64-guards` CI job installs it
and requires all-pass. The other half of M3's definition — an *arm64 kernel
target* if the roadmap ever grows one — is split: the kernel-side
**copy** of the translator (`kernel/simi_arm.{c,h}`, M2's milestone) is
done, staged and compile-gated (§10.180); the actual arm64 kernel
*build* and its wiring (translate glue, paging, activation, syscall
path) remains deliberately undone and honest.

**M4 — arm64 kernel target. M4a+M4b+M4c DONE (§10.188-10.190).**
The last row of the §10.184 matrix: a minimal bootable AArch64 kernel that
`kernel/simi_arm.c` finally links into. Scope in three sub-milestones,
mirroring the RISC-V kernel's Phase 9 shape (self-contained qemu `-M
virt` build, embedded .tmo boot smoke, clean power-off): M4a boots
(entry, PL011 console, banner, PSCI SYSTEM_OFF shutdown — the exact
analog of SBI_SRST, so QEMU exits rc=0); M4b links `kernel/simi_arm.c`
(the freestanding `{memcpy, memset}` pair the §10.181 gate permits must
be PROVIDED by the kernel — that contract becomes concrete here),
embeds a tiny .tmo exactly like `rv64_boot_smoke_tmo.h`, translates it
with `simi_arm_translate()`, and calls the entry as a real function
(result in x9, the M3-established convention) — the kernel leg's job is
the LINK, not re-verifying the encoder, which qemu-aarch64 already
proved; M4c (DONE, §10.190) is the arm64 MMU (VMSAv8-64: TCR/MAIR/TTBR,
a real 4-level 4 KiB walk, identity-mapped), then user paging +
interrupts + the syscall path — the Phase-9-equivalent honest
remainder.
- **Gate (M4a) — PASSED (§10.188):** `qemu-system-aarch64 -M
  virt,virtualization=on -cpu cortex-a53 -kernel sls_arm64_kernel.elf`
  exits rc=0 (PSCI powered it off) with the banner + the `[M4]`
  exception-level line in the serial log; enforced in the arm64-guards
  CI job (builds `make arm64-elf`, boots, asserts rc=0 + banner + EL1 +
  PSCI line). The working machine config was itself the milestone's
  riskiest unknown and is pinned empirically: virtualization=on (NOT
  secure) — see §10.188.
- **Gate (M4b) — PASSED (§10.189):** the log shows `[SIMI] entry
  returned (real machine code executed) -- result=0x…2a (expected 42)`,
  matching the four-way host value; `make arm64-elf` links
  `kernel/simi_arm.c` into the image (the kernel provides the
  freestanding `{memcpy, memset}` pair — the §10.181 contract,
  concrete). The first real fix M4b forced: the kernel's 16 KiB boot
  stack overflowed — `simi_arm_translate` alone has an 18,800-byte
  frame (measured with `-fstack-usage`), so the stack was raised to
  256 KiB (§10.189). The boot assertion (result=42) is CI-enforced in
  arm64-guards.
- **Gate (M4c) — PASSED (§10.190):** the log shows `[M4c] MMU on`,
  `SCTLR_EL1 M=1 C=1 I=1`, and the two walk results — the kernel page
  as normal WBWA (attr 1) and the UART page as device+PXN (attr 0) —
  then the SIMI smoke still returns 42 and PSCI powers off rc=0, all
  under the MMU. The MMU comes on BEFORE the banner, so the whole log
  is translated output. Two real bugs were found and fixed en route:
  the 1 GiB-boundary L1 index (kernel at 0x40000000 = exactly 1 GiB
  sits at L1[1], not L1[0]) and a walk whose block/table constants
  were both 0b11, letting the compiler dead-code-eliminate the L3
  descent — the walk printed L2 descriptors and lied (§10.190).
  CI-enforced in arm64-guards (rc=0 + banner + SCTLR + both walk
  lines + SIMI result + PSCI).
- Sizing: boot_arm64.S ~150, linker_arm64.ld ~40, uart_pl011.c ~80,
  kernel_arm64.c ~220 (banner + boot smoke + memcpy/memset + PSCI),
  mmu.c ~250 (tables, enable, walk), Makefile ~25, CI ~30.

**M5 — user-mode paging on the arm64 kernel. DONE (§10.191).**
The first of the M4 honest-remainder gaps (user paging, §10.190),
taken as its own milestone: the kernel relinks to TTBR1 high VAs
(VMA 0xFFFF000040080000, LMA 0x40080000, a physical entry stub at
0x4007F000) and the translated SIMI program runs in EL0 from a VA the
kernel's own tables do NOT map (TTBR0 user tree: code/stack/svc-stub/
scratch at 0x10000000/0x10001000/0x10004000/0x10005000). The blob's
trampoline `br x30`s into a user `svc #0` stub; the vector table's
sync-from-lower-A64 handler stores x9 (the result) and erets to a real
`noreturn` continuation function that prints the result and powers off.
- **Gate (M5) — PASSED (§10.191):** the log shows `[M5] eret into
  EL0...`, `[M5] returned from EL0 -- user result=0x000000000000002a
  (expected 0x2a = 42)`, then PSCI SYSTEM_OFF with qemu rc=0; the
  exception log contains exactly two exceptions (the SVC from EL0 and
  the PSCI smc); boots rc=0 on cortex-a57 (local) and cortex-a53
  (CI), deterministic. CI (arm64-guards) asserts the TTBR1-pure
  kernel lines, the unmapped-to-kernel / user-walk pair at
  0x10000000, the EL0-executable page, and the EL0 result line.
- The milestone's hardest bug was the continuation: a computed-goto
  label (`&&after_eret`) is NOT a stable eret target — GCC placed it
  at the top of the inlined body, so the handler's eret re-ran the
  whole EL0 excursion, and the EL0-clobbered register file then
  faulted on a bare physical address. The fix is structural: the
  continuation is a real `noreturn` function entered via eret
  (§10.191, with the five other boot-found bugs: the `--gc-sections`
  .text loss, a double-add in the high-VA jump, the unparenthesized
  `KERNEL_VIRT_OFF` macro, AP at bit 8 instead of [7:6], and the
  vector table's SError-first ordering).
- **SLS framing (recorded per the architecture note):** AeroSLS is a
  Single Level Storage architecture — there is no glibc and no user
  space; everything runs in kernel space. M5 is therefore NOT the
  start of a userland: it proves the CPU's isolation machinery
  (TTBR1/TTBR0, EL1/EL0, vector dispatch) as a fault-containment
  domain for code you do not trust (a SIMI guest, a network-facing
  tenant, a downloaded artifact), while the single-level store stays
  the shared object space underneath. The default configuration is
  EL1-only — the M4b direct-call shape is the system's real home, and
  the EL0 excursion is an optional sandbox, not the runtime's home.
  If a contained domain ever needs to touch the store, the interface
  is a capability-flavored object-catalog trap (ask for a capability,
  get a handle, access through it) — deliberately NOT a POSIX-style
  syscall ABI, which this architecture has no reason to carry.
- Honest remainder: the isolation machinery now exists; next are an
  activation cache, GIC interrupts, FP/SIMD context — and, only if a
  containment use-case lands, the capability-flavored object-catalog
  trap for foreign-code domains.

**M5.1 — the activation cache (translate-on-first-use). DONE
(§10.192-10.193).** The System/38 `CREATE PROGRAM` → activation split, on
arm64, mirroring `kernel/simi_translate.c`'s `g_activations[]` (ISA doc
§11): translate a `.tmo` once, cache the emitted A64 code, and re-enter
it any number of times WITHOUT retranslating. Today the arm64 kernel
already reuses one buffer across the M4b EL1 direct call and the M5 EL0
excursion, but that reuse is implicit — M5.1 makes it an explicit,
name+hash-keyed cache with an observable HIT path.
- **Design.** A bounded static array of slots (mirroring the x86
  `struct SimiActivation`: object name + FNV-1a hash + `.tmo` size +
  translated code length + entry offset); the code bytes stay in the
  existing `g_smoke_code_buf` (one slot per embedded object; the array
  shape generalizes, no allocator — the kernel's static-array-everywhere
  discipline). MISS: `simi_arm_translate()` + icache flush + record the
  slot. HIT: skip the translator entirely, reuse entry_off/len, no
  re-flush (bytes unchanged). The REGISTER FRAME IS NOT CACHED: the
  SIMI frame is SP-relative, carved per entry off the kernel stack
  (EL1) or a fresh user stack (EL0) — the arm64 equivalent of the x86
  cache's fresh-per-process scratch frame, but free: the arm64
  trampoline has no baked per-process address (unlike the x86 r7
  `movabs`), so nothing about the cached bytes is per-entry.
- **Gate (M5.1) — PASSED (§10.193):** occurrence counts on the boot
  log: exactly 1 `activation cache MISS` (translate once), exactly 3
  `activation cache HIT`s, exactly 2 EL1 direct results (42), exactly
  2 EL0 results (42) — the four entries (EL1, EL1, EL0, EL0), one
  translation, all 42, rc=0 on cortex-a57 (local) and cortex-a53
  (CI), with the exception log containing exactly the intended three
  (SVC, SVC, PSCI smc). The gate uses `grep -o | wc -l` occurrence
  counts, not `grep -c` line counts: the kernel's UART lines carry
  literal `\r\n` text, so logical lines share one grep line
  (§10.193). CI (arm64-guards) asserts the four counts + rc=0.
- **Honest caveats.** Single-object today (one embedded `.tmo` — no
  second object exists to exercise slot selection); re-upload
  invalidation is argued from the FNV-1a keying (the x86 kernel's
  hash-vs-collision testing, ISA §11) but not exercised — this kernel
  has no upload path; and a changed-bytes retranslate reuses the same
  fixed buffer, so the x86 Phase-4 frame-leak gap degenerates to
  nothing here (one static 4 KiB region, never grown). Sizing: ~80-120
  lines in `kernel_arm64.c` only — the translator core is untouched
  (the cache is a caller-side concern, exactly as x86's
  `simi_translate.c` wraps rather than modifies `simi_x86.c`).

**M5.2 — GIC interrupts: the arm64 kernel's first interrupt-driven
behavior. DONE (§10.194-10.195).** The deferred interrupt story from
§10.190/§10.191, as its own milestone: a periodic tick delivered by the
ARMv8 generic timer through the qemu virt GIC, handled in the vector
table's EL1h IRQ slot, with a tick line and an IRQ-count tripwire — the
RISC-V kernel's Phase 9k/9n `[TICK N]` + re-arm proof, mirrored (the
exact analog of `sbi_arm_timer` + STIP, but with no firmware: the
arm64 kernel programs the CNTP timer and the GIC itself).
- **Design.** Timer: the EL1 physical generic timer (CNTP_TVAL_EL0 +
  CNTP_CTL_EL0.ENABLE, re-armed in the handler — a one-shot never
  delivers `[TICK 2]`, the RISC-V re-arm proof). Interrupt routing:
  the qemu virt GIC (GICv3 expected on QEMU 8.2: distributor
  0x08000000, redistributor 0x080A0000, CPU interface via the
  ICC_*_EL1 sysregs — no MMIO CPU interface), programmed to put the
  EL1 physical timer PPI in group 1 and enable it (GICD_CTLR /
  GICD_IGROUPR0 / GICD_ISENABLER0 + ICC_SRE/IGRPEN1; the exact PPI
  number — SBSA says 26 — and the GIC version are pinned EMPIRICALLY
  at implementation, the spike-before-estimate discipline). The GIC
  MMIO is low-VA, and the kernel is TTBR1-pure: mmu.c gains device
  pages for the distributor/redistributor at high VAs, mirroring the
  UART page (attr 0, PXN). Vector table: the EL1h IRQ slot (VBAR+0x280)
  becomes the tick handler — acknowledge (ICC_IAR1), increment the
  counter, re-arm, print `[TICK N]`, EOIR (ICC_EOIR1). The lower-EL IRQ
  slot stays a stub: interrupts are masked (DAIF.I=1, SPSR 0x3c0/0x3c5)
  during the EL0 excursions and the handler, so no IRQ can trap from
  EL0 — honest note.
- **Gate (M5.2) — PASSED (§10.195):** the log shows `[TICK 1]`,
  `[TICK 2]`, `[TICK 3]` as an unbroken run (exactly 3), `[TICK 2]`
  proving the re-arm, then `[M5.2] tick gate reached: 3 ticks,
  unbroken run` and PSCI SYSTEM_OFF rc=0 — on cortex-a57 (local) and
  cortex-a53 (CI), deterministic. The M5.1 counts (1 MISS / 3 HITs /
  four 42s) are untouched, and the exception log is exactly the six
  intended exceptions: 3 IRQ (the ticks), 2 SVC (the EL0 excursions),
  1 PSCI smc. The spike overrode BOTH scope assumptions — the machine
  is GICv2 (not v3), and the timer's INTID is 30 (FDT PPI 14 + 16) —
  and three bring-up bugs were found by booting: `msr daifclr` #4
  clears A not I (IRQ unmask is #2), GICC_CTLR 0x1 enables group 0
  not group 1 (EnableGrp1 is bit 1 = 0x2), and the ISENABLER0 write
  targeted SGI 14 instead of PPI 14's INTID 30 (§10.195). CI
  (arm64-guards) asserts the tick run + `[TICK 2]` + the existing
  counts + rc=0.
- **Contention probe (M5.2 addendum, §10.196) — DONE:** the Phase 9n
  follow-up is now part of the gate. The embedded boot program is a
  1e8-iteration loop (still returns 42), the tick period is 100 ms
  (was 50 ms), and the timer is armed BEFORE the EL0 excursions — so a
  tick fires inside each EL0 window, pends against the EL0 SPSR's I
  mask, and is taken exactly once on the svc-return to EL1 (the
  continuation clears I at its entry). The gate is the unbroken
  `[TICK 1..4]` stream with the exact interleave — `eret → [TICK 1] →
  eret → [TICK 2] → [TICK 3] → [TICK 4] → gate reached` — zero lost
  ticks, zero missed re-arms, on a57 + a53; CI asserts the ordered
  stream itself. Measured EL0 windows ~760-790 ms vs the 100 ms period
  (host-speed-dependent margin; the TCG not-real-time caveat holds).
- **Nested-IRQ probe (M5.3, §10.197) — DONE:** the contention probe
  extends to interrupt NESTING. After the M5.2 gate, the next physical
  tick (TICK 5) opens a bounded unmasked window inside ITS OWN handler
  with the VIRTUAL timer (CNTV, a different GIC INTID at higher
  priority) armed for 10 ms; the virtual fire (TICK 6) preempts the
  physical handler mid-window — proving the EL1h IRQ entry's
  ELR_EL1/SPSR_EL1 save/restore. Same-PPI nesting is impossible
  (GICv2 running-priority: an interrupt cannot preempt its own active
  instance) and the spike showed qemu's lax same-PPI re-entry corrupts
  the GIC (double-activation/EOIR of one INTID → hang), so the
  virtual timer is the honest second source. The probe's deepest
  finding: the kernel boots in the SECURE world, and qemu's GICv2
  hides group-1 interrupts from a secure IAR read (returns 1022
  without GICC_CTLR.AckCtl) — M5.2's "group 1" ticks were spurious
  1022s all along; both timers moved to group 0 (the secure group),
  the honest config for a secure kernel. Gate: the M5.2 phase stays
  byte-identical, then `[TICK 5] → nesting window → [TICK 6] →
  nested marker → gate at 6` — deterministic across 5/5 runs on a57 +
  a53; CI asserts the full ordered stream.
- **Honest caveats.** TCG timing is not real-time: the tripwire
  asserts the tick COUNT and the re-arm, never wall-clock; the PPI
  number/GIC version are pinned empirically before the design is
  trusted (the RISC-V SBI_SRST and the M4a EL-ladder are the
  precedents for such spikes being real); nesting is proven only via
  the virtual timer (the same-PPI case is architecturally impossible)
  and only in the EL1h handler (the lower-EL IRQ slot stays a stub:
  SPSR masks IRQs during the EL0 excursions, as scoped in §10.194).
  FP/SIMD context: the zero-FP claim holds on three machine-checked
  legs (`-mgeneral-regs-only` codegen, a 0-instruction image census,
  and the CPACR_EL1.FPEN=0 trap, spike-verified under TCG), so the
  EL1h entry needs no SIMD save — the gates are DONE (§10.199): the
  census, the boot-time cpacr log, and the teeth smoke, all wired
  into arm64-guards CI.
  Sizing: ~150-200 lines
  — mmu.c GIC device pages (~20), a new arch/arm64/gic.c (~90:
  distributor + sysreg init + acknowledge/EOIR), timer arm + handler
  in kernel_arm64.c (~50), the vector slot (~10), CI asserts (~10).

**M0 and M1 are the project.** M2 is a port with a re-diff; M3 was
environment-dependent until the qemu-aarch64 leg landed. The ordering rule
from Step 6.4 applies equally here:
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

### 10.94 M2.42 — the flat walk cap matches the BIG cap (as built)

M2.41 raised TX_AR_CHAIN_MAX from 12 to 20; M2.42 raises it to 64,
equal to TX_AR_CHAIN_BIG, so a 21-64-value product now stays FLAT too
(no deferral). The key observation: a set of > 64 values can NEVER
chain — the emission gate rejects ncand > TX_AR_CHAIN_BIG, and the
walk's deferral only triggers on a flat-cap overflow — so with equal
caps every REACHABLE chain materializes from a flat set and the entire
M2.30-M2.41 record machinery (product defers, union records, the
flat+flat fallback, the in-place freeze, chain_def_live/chain_prewrite
flattening of >64 records) is UNREACHABLE: no record is ever created
whose true set fits the emission bound, and a union exceeding 64 is
rejected by chain_def_alloc_union's cap-check exactly where the plain
merge would collapse. The machinery is retained — it is the recorded
history and a safety net should the caps ever diverge again — and
annotated as vestigial in the code. (Equal-caps note: M2.61 later
raised both caps to 96, so this "> 64 can never chain" boundary moved
— sets of 65-96 values now gain the chain; see §10.132.) The raise is emission-invisible
for every pre-existing program: chain13-17's dispatch sets are
unchanged (chain16's 25-value product and 40-value union now flatten
instead of deferring, but the candidate SET — what the chain emits —
is identical), so it is strictly additive.

### 10.95 M2.42 gate results (measured)

Total emitted bytes across the now-66-program parity set: **M0
123188 → M1 104248, 18940 saved** (≈15.4%), up from M2.41's 18240.
One row added, zero moved:

- jmpr_chain18 3956 → 3256 (−700, new 66th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: r1 = r2 + r3 is FORTY values
  {42..120} (r2 in {2,12,...,72} — EIGHT values, r3 in
  {40,42,44,46,48} — FIVE values); the arm `BC r5, J` delivers it;
  then `LOADI r2, #1` WRITES a source. Under the M2.41 20-cap the
  product deferred and the write flattened it to UNKNOWN (the table
  ran); under 64 it stays flat, the write is a no-op for r1's slot,
  and the gate (324 < 528) emits the 40-pair chain. Runtime
  `72 + 48 = 120` takes the chain's FORTIETH b.eq (dump-verified:
  40 b.eq pairs, first → #100's block at offset 1556, fortieth →
  #4000's block at offset 3116, both byte-exact). The chain vs table
  delta is exactly 528 − 324 = +204; the rest of the 700 is the M1
  allocator on the 122-instruction body.
- Teeth: reverting the cap to 20 grows jmpr_chain18 back to exactly
  3460 (the table path — the product defers and the write kills it,
  still correct), proving the raised cap is what the chain dispatches
  through.
- Row-by-row accounting (gate tables diffed vs committed 2826afb,
  measured with the M2.41 verifier): **all 65 shared rows
  byte-identical** — the raise is emission-invisible for the whole
  M2.41 corpus, so M2.42 is strictly additive.
- Four-way parity: 264 PASS, 0 FAIL — all 66 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment; the 40-candidate
  flat chain executes at runtime on the ARM engine (PASS 4000,
  runtime index 120 taking the fortieth b.eq), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged.
  enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.96 M2.43 — the tracked-register closure above 4 (as built)

M2.26 sized TX_AR_CHAIN_REGS at 4 — the index register plus its
feeders — and a closure needing more feeders TRUNCATED silently: the
missing feeder was never tracked, its set stayed UNKNOWN, the index
set became UNKNOWN, and the dispatch fell to the table. M2.43 raises
the cap to 8, so a 5-8-feeder index chain now analyzes. Every
TX_AR_CHAIN_REGS use is an array size or loop bound (g_chain_arr is
REGS x 4096 ChainSets — the static footprint scales with the cap; 8
is a generous bound for realistic register chains while staying well
under TX_AR_MAX_REGS), so the change is a single define. The raise is
emission-invisible for every pre-existing program — none has a closure
between 5 and 8 (the chain-family tests were designed to fit the 4-cap
or decline) — so it is strictly additive.

### 10.97 M2.43 gate results (measured)

Total emitted bytes across the now-67-program parity set: **M0
126012 → M1 106548, 19464 saved** (≈15.4%), up from M2.42's 18940.
One row added, zero moved:

- jmpr_chain19 2824 → 2300 (−524, new 67th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: the index is built in TWO
  stages over joins — r2 = r4 + r5 (r4 in {0,10}, r5 in {0,10,20} →
  4 values {0,10,20,30}), then r1 = r2 + r3 (r3 in {40,42,44,46,48} →
  TWENTY values {40..78}). The tracked closure is {r1, r2, r3, r4,
  r5} — FIVE registers: under the 4-cap the closure stops at r4 and
  r5 is never tracked, so r1 falls to UNKNOWN and the table runs;
  under the 8-cap all five are tracked and the gate (164 < 360) emits
  the 20-pair chain. Runtime `10 + 20 + 48 = 78` takes the chain's
  TWENTIETH b.eq (dump-verified: 20 b.eq pairs, first → #100's block
  at offset 1400, twentieth → #2000's block at offset 2160, both
  byte-exact). The candidates ARE the block pcs (block0..block19 at
  40..78) — the JMPR jumps to pc == index, so the dispatch index
  doubles as the target pc. The chain vs table delta is exactly
  360 − 164 = +196; the rest of the 524 is the M1 allocator on the
  80-instruction body.
- Teeth: reverting the cap to 4 grows jmpr_chain19 back to exactly
  2496 (the table path — the closure truncates and r1 goes UNKNOWN,
  still correct), proving the raised cap is what the chain dispatches
  through.
- Row-by-row accounting (gate tables diffed vs committed e1fe7d1,
  measured with the M2.42 verifier): **all 66 shared rows
  byte-identical** — no pre-existing program has a 5-8-register
  closure, so M2.43 is strictly additive.
- Four-way parity: 268 PASS, 0 FAIL — all 67 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (this milestone's
  harness also captures the interpreter's OUTPUT — simi-run exits 0
  regardless — so the interp check is a real value comparison); the
  20-candidate two-stage chain executes at runtime on the ARM engine
  (PASS 2000, runtime index 78 taking the twentieth b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged. enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.98 M2.44 — a tracked feeder rewritten after the index (as built)

A PROOF milestone with NO code change. The chain walk stores a
PER-REGISTER set — an abstract interpretation, not a re-derivation:
when the index build computes cur[r1] = image(cur[r2], cur[r3]) at pc
A, that set is a STORED image, and a later WRITE to a feeder updates
only that feeder's slot. This mirrors the runtime exactly: r1 holds the
value computed at the build, not something re-derived from the
rewritten r2. The stress case is an OPAQUE rewrite — `SHL r2, r2, #1`
is a writer the walk cannot track, so r2's slot becomes UNKNOWN — and
the index set must still survive, because the dispatch reads only r1's
slot (chain_flatten_big of a flat slot is a direct copy; chain_prewrite
only flattens DEFERRED forms, and a flat index has none). The probe
proves the analysis is order-sensitive: build-then-rewrite chains with
the EXACT pre-rewrite candidate set, while rewrite-before-build
computes the index from the unknown feeder and falls to the table.

### 10.99 M2.44 gate results (measured)

Total emitted bytes across the now-68-program parity set: **M0
128876 → M1 108872, 20004 saved** (≈15.5%), up from M2.43's 19464.
One row added, zero moved — and simi_arm.c is BYTE-IDENTICAL to the
M2.43 commit (the milestone is a probe, not a change):

- jmpr_chain20 2864 → 2324 (−540, new 68th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: r1 = r2 + r3 over joins (r2 in
  {2,12,22,32}, r3 in {40,42,44,46,48} -> TWENTY values {42..80});
  the OPAQUE rewrite `SHL r2, r2, #1` sits BETWEEN the build (pc 18)
  and the dispatch (pc 20); the gate (164 < 368) emits the 20-pair
  chain. Runtime `32 + 48 = 80` (the SHL sets r2 = 64, but r1 was
  computed before it) takes the chain's TWENTIETH b.eq
  (dump-verified: 20 b.eq pairs with the FIRST subs = #42, first →
  #100's block at offset 1424, twentieth → #2000's block at offset
  2184, both byte-exact). The candidate set being EXACTLY the
  pre-rewrite image {42..80} — not fewer, not the SHL'd values — is
  the proof that the stored image was used.
- Teeth (order-sensitivity): swapping the two instructions (rewrite
  BEFORE the build) grows jmpr_chain20 back to exactly 2528 (the
  table path — the index is then computed from the unknown feeder,
  still correct), proving the analysis really is per-pc abstract
  interpretation, not a degenerate approximation.
- Row-by-row accounting (gate tables diffed vs committed 5de17cc,
  measured with the M2.43 verifier): **all 67 shared rows
  byte-identical** and simi_arm.c untouched — strictly additive.
- Four-way parity: 272 PASS, 0 FAIL — all 68 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the feeder-rewrite chain
  executes at runtime on the ARM engine (PASS 2000, runtime index 80
  taking the twentieth b.eq), and the documented float/mem skips and
  the no-expected jmpr_oob are unchanged. enc-check clean; jmpr_oob
  still faults (UDF, rc=1).

### 10.100 M2.45 — an opaque rewrite of the index register itself (as built)

A PROOF milestone with NO code change — the mirror of M2.44. M2.44
pinned that a write to a FEEDER leaves r1's stored image untouched;
M2.45 pins that a write to the INDEX register r1 itself REPLACES
cur[r1] entirely: a TRACKED writer installs the new image (the
M2.27/M2.28 family), an OPAQUE writer (SHR — not in the image family)
installs UNKNOWN, and the dispatch snapshot is then UNKNOWN — the
chain dies conservatively and the runtime table runs. This mirrors the
runtime exactly: the dispatch uses the register's CURRENT value, so a
chain built from a stale pre-rewrite image would be wrong (the runtime
index is no longer a member of the old candidate set).

### 10.101 M2.45 gate results (measured)

Total emitted bytes across the now-69-program parity set: **M0
131756 → M1 111416, 20340 saved** (≈15.4%), up from M2.44's 20004.
One row added, zero moved — simi_arm.c is BYTE-IDENTICAL to the M2.44
commit (a probe, not a change):

- jmpr_chain21 2880 → 2544 (−336, new 69th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: r1 = r2 + r3 over joins (r2 in
  {2,12,22,32}, r3 in {40,42,44,46,48} -> TWENTY values {42..80});
  the OPAQUE rewrite `SHR r1, r1, #1` sits BETWEEN the build (pc 18)
  and the dispatch (pc 20). The chain dies (dump-verified: ZERO b.eq
  in the body — the table path) and the runtime index `32 + 48 = 80`
  becomes 80 >> 1 = 40 — a REAL block (block_40 at pc 40, deliberately
  OUTSIDE the candidate set), which the table lands on
  (dump-verified: block_40's `movz x9, #10000` at word 321) -> PASS
  10000 on all four engines. The test DISCRIMINATES: a stale-image
  chain would dispatch 40 against {42..80}, miss every b.eq and
  UDF-trap, so a regression in the replace semantics fails the parity
  pin. The −336 delta is the accumulated M1 emission folds, not the
  chain — the chain is dead by design, and the row is a regression
  guard (a fired chain would shrink it below the honest table size).
- Teeth (op-sensitivity at the same pc): swapping SHR for a TRACKED
  rewrite `ADD r1, r1, #-32` re-derives the image {10..48} and the
  chain fires on the SHIFTED candidates (dump-verified: 20 b.eq — 6
  backward to the pre-chain body at candidates 10..20, 14 forward to
  the post-chain filler/blocks at 22..48 — with the first subs = #10
  and the last = #48), runtime 48 taking the twentieth b.eq -> block3
  -> 400 on all four engines. The replace is VALUE-ACCURATE in both
  directions, proven at the same pc with only the op differing.
- Row-by-row accounting (gate tables diffed vs committed 44d97b3,
  measured with the M2.44 verifier): **all 68 shared rows
  byte-identical** and simi_arm.c untouched — strictly additive.
- Four-way parity: 276 PASS, 0 FAIL — all 69 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the opaque-rewrite table
  dispatch executes at runtime on the ARM engine (PASS 10000, index
  40 landing on block_40), and the documented float/mem skips and the
  no-expected jmpr_oob are unchanged. enc-check clean; jmpr_oob
  still faults (UDF, rc=1).

### 10.102 M2.46 — two index generations over one tracked feeder (as built)

A PROOF milestone with NO code change. The chain machinery requires
exactly ONE dynamic JMPR (n_dyn == 1 — a second dynamic dispatch forces
naive table mode for the whole program), so the probe puts both index
generations in the SAME register with a single dispatch: build1 `r1 =
r2 + r3` at pc 18 (TWENTY values {42..80}, the stored image), a
TRACKED `LOADI r3, #100` at pc 19 (rewrites the feeder — cur[r3]
becomes {100}, and r1's stored image is untouched, M2.44's mechanism
now with a tracked writer), then build2 IN-PLACE `r1 = r1 + r3` at pc
20 — which reads the OLD r1 image and the NEW r3 -> TWENTY values
{142..180}. Each generation is computed from the feeder's value AT THAT
BUILD (build1 uses r3's join set {40..48}, build2 uses {100}); the two
generations COEXIST in one image. The dispatch chains on the SECOND
image — the walk's per-register abstract interpretation handles it by
construction (chain_img_alu reads both operand slots into a local
before writing the destination, so the in-place form is sound).

### 10.103 M2.46 gate results (measured)

Total emitted bytes across the now-70-program parity set: **M0
136620 → M1 114940, 21680 saved** (≈15.9%), up from M2.45's 20340.
One row added, zero moved — simi_arm.c is BYTE-IDENTICAL to the M2.45
commit (a probe, not a change):

- jmpr_chain22 4864 → 3524 (−1340, new 70th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: build1 {42..80} -> tracked
  LOADI r3,#100 -> build2 in-place {142..180}; the gate (164 <
  40+4·182 = 768) emits the 20-pair chain on the SECOND image.
  Runtime `32 + 48 = 80` then `+ 100 = 180` takes the chain's
  TWENTIETH b.eq (dump-verified: 20 b.eq pairs, first subs = #142 at
  word 257, the twentieth b.eq targeting +552 words = word 848 =
  block19's `movz x9, #2000` d280fa09, byte-exact) -> PASS 2000 on
  all four engines. The test DISCRIMINATES every failure mode of the
  two-generation independence: re-deriving r1's image at the LOADI
  (→ {202..232}), a stale r3 at build2 (→ {82..128}), or an in-place
  read-after-write (→ {202..232}) all move the candidate set off the
  runtime index 180 and UDF-trap — a regression in either generation
  fails the parity pin.
- Teeth (op-sensitivity at the same pc): swapping the TRACKED LOADI
  for an OPAQUE `SHL r3, r3, #1` makes build2's image UNKNOWN — the
  chain dies (dump-verified: 0 b.eq, table path, 4136 bytes = the
  honest naive size; the 612-byte delta vs the chain is the table
  768 minus the chain 164 plus the shared index-load/flush nuance)
  and the runtime index `80 + 96 = 176` lands on block17 -> 1800 on
  all four engines. Tracked vs opaque writer on the feeder, same pc.
- Row-by-row accounting (gate tables diffed vs committed 42e426a,
  measured with the M2.45 verifier): **all 69 shared rows
  byte-identical** and simi_arm.c untouched — strictly additive.
- Four-way parity: 280 PASS, 0 FAIL — all 70 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the two-generation chain
  executes at runtime on the ARM engine (PASS 2000, runtime index
  180 taking the twentieth b.eq), and the documented float/mem skips
  and the no-expected jmpr_oob are unchanged. enc-check clean;
  jmpr_oob still faults (UDF, rc=1).

### 10.104 M2.47 — the shifts join the chain image (as built)

A CODE-CHANGE milestone (the first since M2.43's cap raise):
SHL/SHR/SAR join the chain image family. Before M2.47 the walk's ALU
branch tracked only ADD/SUB/MUL/AND/OR/XOR (chain_alu_eval returned 0
for anything else), so a shift on the index path fell to the
opaque-writer branch and the chain died. M2.47 adds the three shifts
with the M2.3 argument: a constant-amount shift of a known set is
plain 64-bit, never faults and is type-agnostic, so the image
re-derives. The semantics mirror ar_const_step exactly: the AMOUNT is
masked mod 64 (& 0x3F — lslv/lsrv/asrv mask in hardware, the interp
masks with fetch_operand_b & 0x3F), SHR is LOGICAL (unsigned >>) and
SAR ARITHMETIC (sign-filling). The change is two lines of logic
(chain_alu_eval's switch + the walk's write-path condition); the
emission needed nothing (the runtime shift codegen was already there).

### 10.105 M2.47 gate results (measured)

Total emitted bytes across the now-71-program parity set: **M0
138868 → M1 116540, 22328 saved** (≈16.1%), up from M2.46's 21680.
This milestone deliberately moves ONE shared row — the first since the
airtight series began — and adds one:

- jmpr_chain23 2248 → 1804 (−444, new 71st row; M0 baseline measured
  at git 1729f50) — the dedicated pin: a SHL-BUILT index. r1 = r2 << 1
  over a TEN-way join (r2 in {20..29} -> TEN values {40,42,...,58});
  the gate (84 < 40 + 4·60 = 280) emits the 10-pair chain. Runtime
  `29 << 1 = 58` takes the chain's TENTH b.eq (dump-verified: 10 b.eq,
  first subs = #40, last = #58) -> block9 -> PASS 1000 on all four
  engines. The register-form amount (`SHL r1, r2, r5` with LOADI
  r5,#1 — the amount set is the singleton {1}, masked mod 64)
  re-derives identically (10 b.eq, 1804 bytes, PASS 1000, verified ad
  hoc). Teeth: reverting the M2.47 tracking (dropping the shifts from
  the walk's ALU branch) makes the SHL opaque again — cur[r1] goes
  UNKNOWN, the chain dies, and the row grows back to exactly 2000
  (the table path, 0 b.eq, still correct); the 196-byte delta is
  exactly (40 + 4·60) − (8·10 + 4).
- jmpr_chain21 2544 → 2340 (−204, MOVED — the deliberate
  supersession): the M2.45 pin's SHR is now TRACKED. The write still
  REPLACES cur[r1] (the M2.45 semantics hold), but with the SHIFTED
  image {42..80} >> 1 = {21..40} instead of UNKNOWN — so the chain
  fires on the shifted candidates and the row shrinks by exactly the
  chain-vs-table delta. The result is unchanged (10000: runtime 80 >>
  1 = 40 = the twentieth candidate -> block_40). The M2.45 "opaque
  dies" narrative is superseded; the teeth revert grows the row back
  to exactly 2544. jmpr_chain20's SHL (a FEEDER rewrite) is likewise
  now tracked (r2's slot re-derives to {4,24,44,64}), but its row is
  UNCHANGED at 2324 — the stored-image claim holds for tracked
  writers too, so the pin survives with only its prose updated. The
  M2.44/45 doc sections remain as-built history.
- Row-by-row accounting (gate tables diffed vs committed c640fb7):
  **exactly one of the 70 shared rows moved** (jmpr_chain21, above,
  deliberate and documented), the other 69 byte-identical, and
  jmpr_chain23 added — the shift tracking is emission-invisible
  everywhere except where a shift actually feeds an index path.
- Four-way parity: 284 PASS, 0 FAIL — all 71 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the SHL-built chain executes at
  runtime on the ARM engine (PASS 1000, runtime index 58 taking the
  tenth b.eq), and the documented float/mem skips and the no-expected
  jmpr_oob are unchanged. enc-check clean;  jmpr_oob still faults (UDF, rc=1).

### 10.106 M2.48 — the unary NOT/NEG join the chain image (as built)

A CODE-CHANGE milestone (a follow-on to M2.47): the unary NOT (~a) and
NEG (-a) join the image family. Before M2.48 the image covered only
the binary ADD/SUB/MUL/AND/OR/XOR plus the M2.47 shifts, so a unary op
on the index path fell to the walk's opaque-writer branch and the
chain died. M2.48 adds the UNARY image — { f(a) : a in S(a) } — with
the M2.2 argument (plain 64-bit, never faults, type-agnostic),
implemented as a dedicated walk branch that reuses the image helper's
immediate path with a dummy imm (the unary eval ignores bv). A
deferred source is unreachable under the M2.42 equal-caps proof (no
record is ever created), so that case falls to UNKNOWN to stay
conservative. The change is one eval pair + one walk branch; the
emission needed nothing.

### 10.107 M2.48 gate results (measured)

Total emitted bytes across the now-72-program parity set: **M0
141232 → M1 118460, 22772 saved** (≈16.1%), up from M2.47's 22328.
One row added, zero moved:

- jmpr_chain24 2364 → 1920 (−444, new 72nd row; M0 baseline measured
  at git 1729f50) — the dedicated pin: a NEG-BUILT index. r1 = -r2
  over a TEN-way join of NEGATIVE constants (r2 in {-58,-56,...,-40}
  -> TEN values {40,42,...,58} — the uint32 wrap of the negated
  negative constants is exact); the gate (84 < 40 + 4·60 = 280) emits
  the 10-pair chain. Runtime `-(-58) = 58` takes the chain's TENTH
  b.eq (dump-verified: 10 b.eq, first subs = #40, last = #58) ->
  block9 -> PASS 1000 on all four engines. The NOT side is
  out-of-range by construction (NOT of a small pc is huge), so it
  lands on the bare-UDF path: the analysis computes the NOT image
  exactly, finds no in-range candidate, and emits just the UDF
  (dump-verified: 0 b.eq, 0 table words, 1 UDF word; all four engines
  fault rc=1/2 — verified ad hoc on a NOT variant of jmpr_chain23).
  The size discriminates it from the UNKNOWN -> table path, so the
  NOT image is genuinely computed, not collapsed.
- Teeth: reverting the M2.48 tracking (disabling the unary branch)
  makes the NEG opaque again — cur[r1] goes UNKNOWN, the chain dies,
  and the row grows back to exactly 2116 (the table path, 0 b.eq,
  still correct); the 196-byte delta is exactly (40 + 4·60) − (8·10 +
  4).
- Row-by-row accounting (gate tables diffed vs committed fc49bde):
  **all 71 shared rows byte-identical** — no committed program uses
  NOT/NEG in a chain path (the only NOT/NEG instructions live in
  extra_ops/float_ops, neither of which has a JMPR, so the walk never
  runs on them) — and jmpr_chain24 added; strictly additive.
- Four-way parity: 288 PASS, 0 FAIL — all 72 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the NEG-built chain executes at
  runtime on the ARM engine (PASS 1000, runtime index 58 taking the
  tenth b.eq), and the documented float/mem skips and the no-expected
  jmpr_oob are unchanged. enc-check clean; jmpr_oob still faults (UDF, rc=1).

### 10.108 M2.49 — the register-form shift product image (as built)

A PROOF milestone with NO code change. M2.47 verified the shift image
with a SINGLETON amount (SHL r1, r2, r5 with LOADI r5, #1); this pin
exercises the full PRODUCT form: the amount register r5 carries a
MULTI-VALUE set, so the image is the pair product { a << (b & 0x3F) :
a in S(a), b in S(b) } — chain_img_alu's register path, exact until
its merge cap. The masking is part of the product: 66 & 0x3F = 2, so
an amount of 66 behaves identically to 2, exactly as the hardware
(lslv), the interpreter and ar_const_step. An unmasked analysis would
compute 12 << 66 out-of-range and emit a bare UDF — trapping the
runtime index 48 — so the pin discriminates.

### 10.109 M2.49 gate results (measured)

Total emitted bytes across the now-73-program parity set: **M0
144196 → M1 120644, 23552 saved** (≈16.3%), up from M2.48's 22772.
One row added, zero moved — simi_arm.c is BYTE-IDENTICAL to the M2.48
commit (a probe, not a change):

- jmpr_chain25 2964 → 2184 (−780, new 73rd row; M0 baseline measured
  at git 1729f50) — the dedicated pin: SHL r1, r2, r5 with r2 in
  {10,11,12} and r5 in {2,3,66}. The nine pairs collapse to SIX
  distinct candidates {40,44,48,80,88,96} (dump-verified: 6 b.eq,
  compares #40 #44 #48 #80 #88 #96) — the product is EXACT (dedup
  preserves all six) and the masking is applied (48 present, not a
  huge unmasked value). Runtime `12 << (66 & 0x3F) = 12 << 2 = 48`
  takes the b.eq for 48 -> block2 -> PASS 300 on all four engines.
  The gate (52 < 40 + 4·98 = 432) emits the 6-pair chain.
- Teeth — the exact-or-conservative cap discipline, both ways:
  (a) EXACT: a 65-PAIR product (r2 in {1..13} x r5 in {1..5}) whose
  DISTINCT set stays under the 64 cap chains on the exact
  deduplicated candidates (22 in-range b.eq; the runtime 13 << 5 =
  416 correctly misses all of them and faults — the analysis is
  exact, not prematurely collapsed); (b) CONSERVATIVE: a 240-pair
  product with >64 DISTINCT values (r2 in {1..40} x r5 in {1..6})
  overflows the merge cap and collapses to UNKNOWN — 0 b.eq, the
  full 10-word table (movz #132, subs/cset/cbz, li32 base, add-shift,
  ldr, br, UDF, dump-verified), and the runtime 40 << 6 = 2560 faults
  through the bounds check on all four engines. No truncated or
  wrong chain is ever emitted.
- Row-by-row accounting (gate tables diffed vs committed dbb6438):
  **all 72 shared rows byte-identical** and simi_arm.c untouched —
  strictly additive.
- Four-way parity: 292 PASS, 0 FAIL — all 73 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the product-image chain
  executes at runtime on the ARM engine (PASS 300, runtime index 48
  taking the b.eq for 48), and the documented float/mem skips and
  the no-expected jmpr_oob are unchanged. enc-check clean; jmpr_oob
  still faults (UDF, rc=1).

### 10.110 M2.50 — the NOT-built index, in range (as built)

A PROOF milestone with NO code change — and an honest premise
correction. The M2.50 brief said the unary branch "only handles a
singleton source set"; that is INACCURATE — the branch passes the
FULL source set to chain_img_alu's immediate path, which maps
{ f(a) : a in S(a) } over every element, and jmpr_chain24 (M2.48)
already pinned a TEN-VALUE NEG source. What M2.48 did NOT cover is
an IN-RANGE NOT image: chain24's NOT variant used POSITIVE
constants (NOT of a small pc is huge — out of range, so the
analysis computed the exact image, found no in-range candidate and
emitted a bare UDF). Over NEGATIVE constants the NOT image lands IN
RANGE: ~(-58) = 57, ~(-40) = 39 — the NOT of each negative constant
is a small positive pc.

### 10.111 M2.50 gate results (measured)

Total emitted bytes across the now-74-program parity set: **M0
146540 → M1 122552, 23988 saved** (≈16.4%), up from M2.49's 23552.
One row added, zero moved — simi_arm.c is BYTE-IDENTICAL to the
M2.49 commit (a probe, not a change):

- jmpr_chain26 2344 → 1908 (−436, new 74th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: r1 = ~r2 over a ten-way join
  of negative constants (r2 in {-58,-56,...,-40} -> TEN values
  {39,41,...,57}). The gate (84 < 40 + 4*60 = 280) emits the 10-pair
  chain; runtime ~(-58) = 57 takes the chain's TENTH b.eq (57 = 39 +
  2*9) -> block9's LOADI #1000 -> PASS 1000 on all four engines.
  Dump-verified: 10 b.eq, first subs #39, last #57; the tenth b.eq
  (imm19 140) lands byte-exact on block9's movz x9, #1000.
- Teeth — the cap discipline for the UNARY image (ad hoc): a
  65-DISTINCT join source through NEG (generated scratch program,
  152 instructions, runtime NEG(-150) = 150) overflows the merge cap
  inside chain_img_alu -> the image is UNKNOWN (0 b.eq, no chain
  words) and the full table dispatch runs -> PASS 777 on all four
  engines through the bounds check. The exact side is this pin's
  ten distinct candidates. No truncated or wrong chain is ever
  emitted — same discipline as the M2.49 product teeth, now
  demonstrated for the unary map.
- Row-by-row accounting (gate tables diffed vs committed 440ad69):
  **all 73 shared rows byte-identical** and simi_arm.c untouched —
  strictly additive.
- Four-way parity: 296 PASS, 0 FAIL — all 74 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the NOT-image chain executes
  at runtime on the ARM engine (PASS 1000, runtime index 57 taking
  the tenth b.eq), and the documented float/mem skips and the
  no-expected jmpr_oob are unchanged (jmpr_oob still faults via UDF
  at pc=0x320, rc=1). enc-check clean.

### 10.112 M2.51 — the unary collapse promoted to a committed pin (as built)

A PROOF milestone with NO code change: the conservative side of the
unary cap discipline — M2.50's ad-hoc teeth — is promoted into the
corpus as jmpr_chain27, so the collapse is regression-guarded exactly
like the exact side (chain24). The pin drives a SIXTY-FIVE-DISTINCT
join source through NEG (r2 in {-150,-149,...,-86} -> 65 values
{86..150}), which at the M2.51-era caps (64, TX_AR_CHAIN_MAX /
TX_AR_CHAIN_BIG) OVERFLOWED the image merge cap inside chain_img_alu
-> the image UNKNOWN -> the chain died -> the naive table dispatch
ran. M2.61's EQUAL-CAPS bump (MAX = BIG = 96) changed this pin's
meaning, exactly like chain33's: 65 <= 96 now FITS, so the image
materializes and the chain fires with the full 65 candidates
(Dump-verified: 65 b.eq, 0 table words; see §10.132). The test
DISCRIMINATES truncation: a wrong analysis emitting a chain over only
the first 64 candidates {86..149} would miss the runtime index 150,
miss every b.eq and UDF-trap — only the exact-or-conservative
emission passes. The collapse side of the unary discipline moved to
chain30 (100 values > 96, §10.118).

### 10.113 M2.51 gate results (measured)

Total emitted bytes across the now-75-program parity set: **M0 151040
→ M1 126436, 24604 saved** (≈16.3%), up from M2.50's 23988. One row
added, zero moved — simi_arm.c is BYTE-IDENTICAL to the M2.50 commit
(a promotion, not a change):

- jmpr_chain27 4500 → 3884 (−616, new 75th row; M0 baseline measured
  at git 1729f50) — the conservative-collapse pin: 152 instructions
  (65 join paths + NEG + JMPR + 17 filler + block0), runtime
  -(-150) = 150 landing on block0 at pc 150 -> LOADI #777 -> PASS 777
  on all four engines through the table's bounds check.
  Dump-verified: 0 b.eq and the full 10-word table dispatch (movz
  x11, #152 = num_instr, subs/cset/cbz bounds check, li32 base,
  add-shift ldr, br x11, UDF). The row was a dynamic-JMPR program
  (g_alloc = 0) at the M2.51-era caps, so the 616-byte M0→M1 delta
  was the accumulated emission folds, not the chain. (Equal-caps
  note: M2.61 changed this row — 65 <= 96 now fits, the chain fires
  with the full 65 candidates at M1 3760; see §10.132.)
- Row-by-row accounting (gate tables diffed vs committed 64bd805):
  **all 74 shared rows byte-identical** and simi_arm.c untouched —
  strictly additive.
- Four-way parity: 300 PASS, 0 FAIL — all 75 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the collapse-pin table path
  executes at runtime on the ARM engine (PASS 777 through the bounds
  check), and the documented float/mem skips and the no-expected
  jmpr_oob are unchanged (jmpr_oob still faults via UDF at
  pc=0x320, rc=1). enc-check clean.

### 10.114 M2.52 — the double-unary image, NOT then NEG (as built)

A PROOF milestone with NO code change: the composition of two unary
ops, where the second unary's source is the FIRST unary's STORED
IMAGE. The walk's unary branch (M2.48) maps { f(a) : a in S(a) } over
whatever cur[source] holds — for a second unary that is the previous
unary's image, in place (rd == ra) or not. The two maps compose
exactly in two's complement: NEG(NOT(a)) = -(~a) = a + 1, and the
uint32 wrap preserves it mod 2^32. jmpr_chain28 drives r1 = ~r2 over
a ten-way join (r2 in {39,41,...,57}, ten odd positives) followed by
in-place NEG r1, r1 -> the composed image {40,42,...,58} (a + 1 over
the source set) — in range.

### 10.115 M2.52 gate results (measured)

Total emitted bytes across the now-76-program parity set: **M0
153288 → M1 128240, 25048 saved** (≈16.3%), up from M2.51's 24604.
One row added, zero moved — simi_arm.c is BYTE-IDENTICAL to the
M2.51 commit (a probe, not a change):

- jmpr_chain28 2248 → 1804 (−444, new 76th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: NOT-then-NEG over the ten-way
  join. The gate (84 < 40 + 4*60 = 280) emits the 10-pair chain;
  runtime NEG(NOT(57)) = 58 takes the chain's TENTH b.eq (58 = 40 +
  2*9) -> block9's LOADI #1000 -> PASS 1000 on all four engines.
  Dump-verified: 10 b.eq, first subs #40, last #58; the tenth b.eq
  (imm19 140) lands byte-exact on block9's movz x9, #1000. The test
  DISCRIMINATES the stored-image read: if the second unary re-read
  the JOIN set, NEG over {39..57} would be {-57..-39} — uint32-huge,
  all out of range — and runtime 58 would miss every candidate and
  UDF-trap; only the correct stored-image composition chains.
- Teeth — both orders compose, and the chain is proven to come from
  the stored image (ad hoc): (a) the MIRROR composition NEG-then-NOT
  over {41..59} (NOT(NEG(a)) = a - 1) lands the SAME candidate set
  {40..58} — 10 b.eq, PASS 1000 on all four engines at the identical
  1804 bytes, so both orders re-derive and compose exactly;
  (b) disabling the unary tracking (temporary edit) grows the row to
  exactly 2000 bytes (table, 0 b.eq, still PASS 1000) — the 196-byte
  delta is exactly (40 + 4*60) − (8*10 + 4), the chain-vs-table
  difference, proving the fired chain came from the composed stored
  image and not from a coincidental table hit.
- Row-by-row accounting (gate tables diffed vs committed 67810c2):
  **all 75 shared rows byte-identical** and simi_arm.c untouched —
  strictly additive.
- Four-way parity: 304 PASS, 0 FAIL — all 76 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the composed-image chain
  executes at runtime on the ARM engine (PASS 1000, runtime index 58
  taking the tenth b.eq), and the documented float/mem skips and
  the no-expected  jmpr_oob are unchanged (jmpr_oob still faults via UDF at
  pc=0x320, rc=1). enc-check clean.

### 10.116 M2.53 — the unary-over-deferred boundary (as built)

A PROOF milestone with NO code change, and a HONEST CORRECTION of the
M2.42 "UNREACHABLE" comment. The unary branch's fallback
(`sa->def >= 0 -> UNKNOWN`) is reachable in PRINCIPLE: the M2.30 root
allocates a record whenever a register-form product OVERFLOWS the
merge cap with known FLAT sources (a >96-DISTINCT image) — the M2.42
comment means such records can never lead to a CHAIN (the dispatch
gate rejects ncand > TX_AR_CHAIN_BIG), NOT that they are never
allocated. All other record creators (M2.32/33/35-39/41) chase an
existing record, so the M2.30 root is the only first record. The
fallback is observationally conservative: the record-collapse and the
plain-UNKNOWN-collapse both give UNKNOWN -> the table, and the true
set (>96) can never chain anyway. Under CURRENT equal caps (96/96,
M2.61) the boundary is: sub-96 products keep their FLAT image (no
record — the unary re-derives), >96 products collapse (record or
plain UNKNOWN — the unary falls back). jmpr_chain29 pins the flat
side of the boundary: a product whose image EXCEEDED the OLD flat cap
(12, pre-M2.41) but fits the current 96.

### 10.117 M2.53 gate results (measured)

Total emitted bytes across the now-77-program parity set: **M0
157888 → M1 131668, 26220 saved** (≈16.6%), up from M2.52's 25048.
One row added, zero moved — simi_arm.c is BYTE-IDENTICAL to the
M2.52 commit (a probe, not a change):

- jmpr_chain29 4600 → 3428 (−1172, new 77th row; M0 baseline measured
  at git 1729f50) — the dedicated pin: r1 = -(r2 + r3) with r2, r3 in
  {-79,-77,...,-61} (ten odd negatives each). The ADD product is the
  19 even values {-158,-156,...,-122} (19 distinct <= 96 — FLAT, so
  no record is created and the in-place NEG RE-DERIVES to
  {122,124,...,158}); a 12-cap walker would defer this product. The
  gate (156 < 40 + 4*160 = 680) emits the 19-pair chain; runtime
  -( -61 + -61 ) = 122 takes the chain's FIRST b.eq -> block0's LOADI
  #100 -> PASS 100 on all four engines. Dump-verified: 19 b.eq,
  compares exactly {122..158 step 2}; the first b.eq (imm19 275)
  lands byte-exact on block0's movz x9, #100 (d2800c89).
- Teeth — the two sides of the boundary (ad hoc):
  (a) OVER-cap: a 20x20 MUL product (>96 distinct values) followed by
  in-place NEG — the image overflows the merge cap, the M2.30 root
  allocates a record (or plain UNKNOWN), the NEG hits the fallback
  -> 0 b.eq, the full table dispatch, and the runtime index
  -(20*20) = -400 (uint32-huge, out of range) faults through the
  bounds check (ARM rc=1 at the table's UDF word, interp rc=2
  "JMPR target out of range") — conservative, no wrong chain.
  (b) CAP-REVERT — temporarily setting TX_AR_CHAIN_MAX back to 12
  (the pre-M2.41 cap) makes the SAME jmpr_chain29 pin's product
  DEFER: the M2.30 root creates a record, the NEG reads
  cur[r1].def >= 0 and hits the unary-over-deferred fallback ->
  UNKNOWN -> the table, PASS 100 on all four engines at 3952 bytes
  (0 b.eq, +524 over the 3428-byte chain). This is the sharpest
  demonstration: the fallback fires correctly when records ARE
  reachable, and never emits a wrong chain. The cap is restored
  byte-identical (diff empty) and the committed pin chains again.
- Row-by-row accounting (gate tables diffed vs committed c2e6f2f):
  **all 76 shared rows byte-identical** and simi_arm.c untouched —
  strictly additive.
- Four-way parity: 308 PASS, 0 FAIL — all 77 expected-result
  programs on all four engines (interp, x86 JIT, RV64, ARM), each
  checked against its "Expected result:" comment (the harness
  captures the interpreter's output); the flat-product+NEG chain
  executes at runtime on the ARM engine (PASS 100, runtime index 122
  taking the first b.eq), and the documented float/mem skips and
  the no-expected jmpr_oob are unchanged (jmpr_oob still faults via
  UDF at pc=0x320, rc=1). enc-check clean.

### 10.118 M2.54 — the binary over-cap collapse pinned, and the flatten overflow fixed (as built)

A PROOF milestone that found and fixed a REAL latent bug. M2.51's
chain27 pinned the collapse for the UNARY image; this pin promotes
M2.49's ad-hoc binary teeth (a >64-distinct product collapsing to
UNKNOWN) into a committed corpus test — jmpr_chain30: r1 = r2 + r3
over TWO SEQUENTIAL 10-way joins (r2 in {0,10,...,90} at Ja, r3 in
{0..9} at Jb, the sets delivered independently), so the analysis's
image is the full 100-pair product {0..99} — 100 DISTINCT values, >
96 — which OVERFLOWS the image merge cap inside chain_img_alu ->
UNKNOWN -> the chain dies -> the naive table dispatch runs. Runtime
90 + 9 = 99 lands on block0 at pc 99 -> LOADI #999 -> PASS 999 on all
four engines, through the table's bounds check. The runtime index 99
is the LAST value of the 100-image, so the test DISCRIMINATES
truncation: a wrong analysis emitting a chain over only the first 96
candidates {0..95} would miss 99, miss every b.eq and UDF-trap
(rc=1) — only the conservative collapse passes. Dump-verified:
0 b.eq, the full table dispatch, 101 instructions.

**The latent bug the pin's positive control exposed.** The control
raised TX_AR_CHAIN_MAX to 128 (keeping TX_AR_CHAIN_BIG at 64) so the
walk would keep the 100-value image and the chain could fire — and
the translator CRASHED with stack smashing in both simi-arm-verify
and a64_dump. ASan pinned it: `chain_flatten_big` copies a FLAT set
(stored at TX_AR_CHAIN_MAX width, n <= 128) into a `struct ChainBig`
store (TX_AR_CHAIN_BIG width, v[64]) with NO cap check — the loop at
simi_arm.c:1430 (`tmp.v[i] = s->v[i]`) writes past the 260-byte stack
buffer whenever TX_AR_CHAIN_MAX > TX_AR_CHAIN_BIG. Under the
M2.42-era equal caps n <= 64 = BIG always, so the overflow was
unreachable — but it is a landmine for any future cap divergence.
(At the shipped 96/96 caps, n <= 96 = BIG always — same reasoning,
same unreachability; M2.61's bump preserved the invariant.)

**The fix — exact-or-conservative, byte-invisible under equal caps.**
`chain_flatten_big` now collapses to UNKNOWN when `s->unk || s->n >
TX_AR_CHAIN_BIG` instead of copying (never a truncated set — a
truncated candidate list could UDF-fault at runtime; UNKNOWN always
falls to the table). Under the shipped caps the guard never fires
(n <= 96), so the fix is byte-invisible: the gate's rows are
byte-identical. The control re-run proves it: with TX_AR_CHAIN_MAX
= 128 the translator no longer crashes — jmpr_chain30 PASS 999 at
2476 bytes, 0 b.eq, the full table dispatch (the 100-value flat set
collapses at the flatten boundary instead of overflowing).

### 10.119 M2.54 gate results (measured)

Total emitted bytes across the now-78-program parity set: **M0
168384 → M1 141436, 26948 saved** (≈16.0%). One row added, zero
moved — despite the simi_arm.c flatten fix, **all 81 shared rows are
byte-identical** (the fix only fires when TX_AR_CHAIN_MAX >
TX_AR_CHAIN_BIG, impossible under the shipped equal caps):

- jmpr_chain30 2888 → 2476 (−412, new 78th row; M0 baseline measured
  at git 1729f50) — the binary-collapse pin: r1 = r2 + r3 over the
  two sequential 10-way joins -> the 100-distinct image {0..99} ->
  UNKNOWN -> the table. Runtime 90 + 9 = 99 -> block0's LOADI #999
  -> PASS 999 on all four engines, through the table's bounds check.
  The row is a dynamic-JMPR program (g_alloc = 0), so the 412-byte
  M0->M1 delta is the accumulated emission folds, not the chain.
- Teeth — the fix's two sides (ad hoc): (a) the OVERFLOW control
  (TX_AR_CHAIN_MAX raised to 128) previously crashed with stack
  smashing; with the fix it PASSes conservatively (0 b.eq, table,
  2476 bytes — the flatten collapses instead of overflowing);
  (b) a TRUNCATION check — the emitted candidate path can never
  deliver a partial set, because every flatten/merge site caps to
  UNKNOWN (chain_merge_big, the dispatch's bigsnap.n <= BIG guard,
  and now chain_flatten_big).
- Row-by-row accounting (gate tables diffed vs the current committed
  simi_arm.c): **all 81 shared rows byte-identical** — strictly
  additive; the only simi_arm.c delta is the flatten guard.
- Four-way parity: 332 PASS, 0 FAIL — all 83 expected-result
  programs on all four engines (interp 83/83, x86 82/0/3, RV64
  82/0/3, ARM 82/0/3), each checked against its "Expected result:"
  comment; the collapse-pin table path executes at runtime on the
  ARM engine (PASS 999 through the bounds check), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged
  (jmpr_oob still faults via UDF, rc=1). enc-check clean.

### 10.120 M2.55 — the mirror cap divergence (BIG > MAX) pinned, and the freeze overflow fixed (as built)

The mirror of M2.54. M2.54 proved the FLAT->BIG direction
(TX_AR_CHAIN_MAX > TX_AR_CHAIN_BIG) is overflow-free by collapsing
at the flatten boundary; this milestone probes the opposite
divergence — TX_AR_CHAIN_BIG > TX_AR_CHAIN_MAX — where the union
store's frozen `g_chain_def_flat` (BIG-shaped) can hold more than the
walk's flat bound, and stresses that a >MAX deferred record still
materializes soundly through the BIG path (`chain_flatten_big_rec`,
the dispatch snapshot) while the walk path collapses conservatively
(the M2.41 guards: `st->n > TX_AR_CHAIN_MAX -> UNKNOWN`).

**The pin — the turn-over boundary at the M2.55-era caps.** Under the
M2.55-era EQUAL caps (64/64) the divergence was unreachable — a
record's store held at most 64, exactly the flat bound — so the
committed corpus test (jmpr_chain31) regression-guarded the boundary
itself: the largest set the machinery could represent (n == MAX ==
BIG, exactly 64 values), materializing through record creation, the
BIG store, and the walk flatten all at the same point. (M2.61's
EQUAL-CAPS bump to 96/96 moved the boundary: the same 64-value union
now sits BELOW the cap, stays FLAT — the record machinery never
engages, instrumented ndef=0 at M2.64 — and the fixture still fires
its 64-pair chain; the turn-over boundary itself moved to chain37
(96 values, §10.132) and chain30 (100 values, collapses).) The index
is the union of TWO deferred products (mirroring chain16's R-arm):
R1 = r2 + r3 (r2 in {44,60,76,92}, r3 in {0,2,...,14} -> 32 values
{44..106} step 2) and R2 = r2 + r6 (r6 in {108,110,...,122} -> 32
values {152..214}); the R-arm's `BC r5, J` (r5 = 0, not taken)
delivers R1 into J, the fall-through computes R2 as the carry, and J
forms the union record — 64 DISTINCT values, exactly MAX = BIG. The
cost gate (8*64+4 = 516 < 40 + 4*216 = 904) emits the 64-pair chain;
runtime 92 + 122 = 214 takes the chain's SIXTY-FOURTH b.eq, landing
on block63's LOADI #555 — so a truncated materialization (a store
one short, a guard that kept only 63) would miss it and UDF-trap:
the discriminator. Dump-verified: 64 b.eq, 65 br (64 block RETs +
1), 1 udf fall-through, 0 table words — the full candidate chain.

**The latent bug this pin's control exposed — the FREEZE copy, the
mirror of M2.54's flatten.** `chain_def_alloc`'s freeze path copies
a FLAT set (a `struct ChainSet` at TX_AR_CHAIN_MAX width) into the
record's BIG-shaped store (`g_chain_def_flat[ri].v[TX_AR_CHAIN_BIG]`)
with NO cap check — the mirror of the M2.54 flatten overflow, at the
in-place deferral sites (an aliased flat source frozen as a CD_FLAT
operand against a deferred other source). ASan-proven: with the caps
temporarily diverged the other way (MAX=20 / BIG=1 / DEFS=2, a
9-value aliased source frozen at the LAST record ri = DEFS-1), the
freeze loop at simi_arm.c:1246 fires an ASan
`global-buffer-overflow` (rc=1); the same program post-fix PASSes
conservatively (rc=0, the guard refuses -> UNKNOWN -> table). For
non-last records the overwrite lands in the NEXT record's header,
which its own later allocation overwrites (self-healing but silent
corruption of in-flight state); at the array end it is a real
out-of-bounds write. The fix is the exact-or-conservative discipline
at the freeze boundary: `freeze && (freeze->unk || freeze->n >
TX_AR_CHAIN_BIG) -> refuse (-1, UNKNOWN)` — never a truncated set.

**Control A — the mirror divergence itself (BIG=128 > MAX=64,
ASan-clean).** With TX_AR_CHAIN_BIG raised to 128 and the walk cap
at 64, a >MAX record's true set (up to 128) materializes through the
BIG store: jmpr_chain30 (100-value product index, previously the
collapse pin) and jmpr_chain31 both PASS their expected values
(999/555) — the >MAX record materializes soundly and the emission
gate chooses the cheaper shape (chain30 falls to the table by cost;
chain31 chains all 64). The full ARM suite at the control cap runs
83 passed / 0 failed / 3 skipped — every chain-series program, the
deferred joins, and the unions stay sound, and the walk path
collapses any >MAX record conservatively (the M2.41 guards).

### 10.121 M2.55 gate results (measured)

Total emitted bytes across the now-79-program parity set: **M0
174588 → M1 146380, 28208 saved** (≈16.2%). One row added, zero
moved — despite the simi_arm.c freeze guard, **all 83 shared rows
are byte-identical** (the guard only fires when the freeze set
exceeds TX_AR_CHAIN_BIG, impossible under the shipped equal caps):

- jmpr_chain31 6204 → 4944 (−1260, new 79th row; M0 baseline
  measured at git 1729f50) — the boundary pin: the 64-distinct
  union of two deferred products, runtime 214 -> the chain's
  sixty-fourth b.eq -> block63's LOADI #555 -> PASS 555 on all four
  engines. The 1260-byte M0->M1 delta is the accumulated emission
  folds on the program's large straight-line body. (Equal-caps
  note: at 96/96 the same 64-value union is below the cap and stays
  FLAT — the row still fires its 64-pair chain; see §10.120.)
- Teeth — the fix's two sides (ad hoc, not committed): (a) the
  FREEZE overflow control (MAX=20 / BIG=1 / DEFS=2, 9-value frozen
  aliased source at the last record) — ASan global-buffer-overflow
  at simi_arm.c:1246 pre-fix (rc=1), conservative PASS post-fix
  (rc=0); (b) Control A (BIG=128 > MAX=64) — the full ARM suite at
  the divergent caps: 83/0/3, ASan-clean, >MAX records materialize
  soundly through the BIG store.
- Row-by-row accounting (gate tables diffed vs the current committed
  simi_arm.c): **all 83 shared rows byte-identical** — strictly
  additive; the only simi_arm.c delta is the freeze guard.
- Four-way parity: 333 PASS, 0 FAIL — all 84 expected-result
  programs on all four engines (interp 84/84, x86 83/0/3, RV64
  83/0/3, ARM 83/0/3), each checked against its "Expected result:"
  comment; the boundary-pin chain executes at runtime on the ARM
  engine (PASS 555 through the chain's sixty-fourth b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged (jmpr_oob still faults via UDF, rc=1). enc-check clean.

### 10.122 M2.56 — the tracked-register closure pinned at its 8-cap (as built)

A boundary pin, no code change: M2.43 raised TX_AR_CHAIN_REGS from 4
to 8 (chain19 pinned the 5-register closure); this milestone pins the
NEW turn-over boundary — a closure needing EXACTLY 8 registers, the
largest the cap admits. (M2.62 later raised TX_AR_CHAIN_REGS to 12,
so the same 8-feeder closure now sits BELOW the cap — the fixture
still fires its 30-pair chain, and the turn-over boundary moved to
chain38 (12 feeders) / chain39 (13 feeders); see §10.134/10.136.)
The index is built in FOUR stages over joins (jmpr_chain32): r4 =
r8 + r8 (r8 in {0,10} — the self-add tracks r8 ONCE, which is what
keeps the closure at eight), r2 = r4 + r5 (r5 in {0,10,...,50}), r3 =
r6 + r7 (r6 in {80,90,100}, r7 in {0,200,400}), r1 = r2 + r3 ->
THIRTY distinct values {80..170} U {280..370} U {480..570} step 10.
The closure {r1,r2,...,r8} = EIGHT registers (<= TX_AR_CHAIN_REGS):
every feeder is tracked and the 30-pair chain fires (the gate:
8*30+4 = 244 < 40 + 4*572 = 2328). Runtime 10 + 10 + 50 + 100 + 400
= 570 is the LAST of the 30 candidates and takes the chain's
THIRTIETH b.eq, landing on block29's LOADI #3000 — a TRUNCATED image
(29 candidates, or one feeder dropped by a closure bug) misses 570
and UDF-traps (rc=1): the discriminator. Dump-verified: 30 b.eq, 31
br (30 block RETs + 1), 1 udf fall-through, 0 table words.

**The turn-over control — a NINTH feeder falls to the table,
conservatively and correctly.** The closure BFS adds feeders only
while `ntr < TX_AR_CHAIN_REGS`; a closure needing nine registers
stops growing at eight, the missing feeder (r9 in the ad-hoc
control, `ADD r4, r8, r9` instead of `r8 + r8`) is never tracked and
its set stays UNKNOWN, poisoning r4 -> r2 -> r1 to UNKNOWN — the
dispatch falls to the naive table. The control (not committed) runs
its runtime index through the table's bounds check and PASSes its
expected value with 0 b.eq in the dump: the over-cap side of the
boundary is sound (conservative, never a wrong chain). (The analog at
the new 12-cap is chain39, §10.136.) This is the same
exact-or-conservative discipline the cap series has held since
M2.26.

### 10.123 M2.56 gate results (measured)

Total emitted bytes across the now-80-program parity set: **M0
187416 → M1 154828, 32588 saved** (≈17.4%). One row added, zero
moved — **all 84 shared rows byte-identical** (the gate row is
strictly additive):

- jmpr_chain32 12828 → 8448 (−4380, new 80th row; M0 baseline
  measured at git 1729f50) — the 8-register-closure boundary pin:
  runtime 570 -> the chain's thirtieth b.eq -> block29's LOADI
  #3000 -> PASS 3000 on all four engines. The 4380-byte M0->M1
  delta is the accumulated emission folds on the program's large
  straight-line body (572 instructions). (Equal-caps note: REGS is
  12 since M2.62, so the 8-feeder closure is below the cap; the
  row still fires its 30-pair chain — see §10.122.)
- Teeth — the turn-over sides (ad hoc, not committed): (a) the pin
  itself discriminates truncation at runtime (570 is the last of
  30 candidates; a 29-candidate chain UDF-traps) and in the dump
  (30 b.eq, 0 table words); (b) the NINTH-feeder control (r4 = r8
  + r9, closure needs 9 > 8) — 0 b.eq, the table dispatch, PASSes
  its expected value through the bounds check (conservative and
  correct).
- Row-by-row accounting (gate tables diffed vs the current committed
  simi_arm.c): **all 84 shared rows byte-identical** — strictly
  additive; simi_arm.c is UNCHANGED in M2.56 (a pure pin + gate
  row + doc).
- Four-way parity: 337 PASS, 0 FAIL — all 85 expected-result
  programs on all four engines (interp 85/85, x86 84/0/3, RV64
  84/0/3, ARM 84/0/3), each checked against its "Expected result:"
  comment; the boundary-pin chain executes at runtime on the ARM
  engine (PASS 3000 through the chain's thirtieth b.eq), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged (jmpr_oob still faults via UDF, rc=1). enc-check clean.

### 10.124 M2.57 — the deferred-record pool boundary at its 64-cap (as built)

A boundary pin, no code change: the index was a 64-record DAG built
by the walk's deferred-product machinery (jmpr_chain33). The root
product r1 = r2 + r3 (r2 in {0,20,...,140}, r3 in {0..18 step 2} ->
ALL even numbers 0..158, 80 DISTINCT values > 64) OVERFLOWS the image
merge cap and DEFERS (M2.30: record 0, CD_SLOT/CD_SLOT); then 63
IN-PLACE ADDs r1 = r1 + r4 with r4 = {2} (a singleton — the closure
stays {r1,r2,r3,r4} = 4 <= TX_AR_CHAIN_REGS 8) each defer over the
DEFERRED source (M2.32/M2.41: record k = op(rec(k-1), slot(r4))) —
64 records total, EXACTLY TX_AR_CHAIN_DEFS, all allocating from the
per-iteration pool (instrumentation confirmed def = 63 on r1's slot
at the dispatch). The 64-deep DAG materializes through the BIG store
on dispatch (chain_flatten_big_rec recurses the whole record chain).

**The honest shipped-cap behavior — changed by M2.61, now flat.** At
the M2.57-era caps the record machinery could not chain (the M2.42
vestigial note was correct): the root's 80-value true set OVERFLOWED
the BIG store on materialization — chain_merge_big collapsed to
UNKNOWN (exact-or-conservative, it NEVER keeps a truncated 64-value
set) — so the dispatch fell to the TABLE: 0 b.eq, and runtime 252
(the joins' fall-through arms 120 + 6, plus 63*2; every BC falls
through, r0 == 0) dispatched through the table's bounds check to
block63's LOADI #7777. M2.61's EQUAL-CAPS bump (MAX = BIG = 96)
changed this row's meaning: the root's 80-value true set now FITS the
BIG store, so the walk keeps the whole chain FLAT — no records at all
(instrumented ndef = 0 at M2.64; the M2.57 "64-record DAG" is a
fossil) — and the chain FIRES with the 64 in-range candidates (even
126..252, < num_instr): 64 b.eq, 0 table words, runtime 252 = the
SIXTY-FOURTH b.eq -> block63's LOADI #7777 -> PASS 7777 on all four
engines (see §10.132). The pool boundary itself moved to the
M2.64/M2.65 pins: chain40 (96 records, at-cap) and chain41 (97
records, exhaustion).

**The two boundaries, proven by the ad-hoc controls (not committed).**
(A) TX_AR_CHAIN_BIG raised to 128 (MAX stays 64): the same DAG then
materialized its 80-value set (fits the 128 store) and the chain
FIRED with the 64 in-range candidates (even 126..252, the rest >=
num_instr are filtered), runtime 252 taking the chain's SIXTY-FOURTH
b.eq — the record machinery is SOUND when reachable, exactly the
cap-divergence story M2.54/M2.55's controls established (the 96/96
shipped state is that behavior, now regression-guarded). (B) a 65th
in-place ADD (same BIG=128): the pool was exhausted (g_chain_ndef >=
64 -> chain_def_alloc returns -1), r1's slot fell to UNKNOWN, and
the dispatch was the table again — conservative and correct, the
runtime 256 dispatching through the bounds check. At the M2.57-era
caps the pool held exactly 64 records and failed cleanly at 65; at
the shipped 96/96 state the same exhaustion boundary is pinned by
chain41 (97 records, §10.140).

### 10.125 M2.57 gate results (measured)

Total emitted bytes across the now-81-program parity set: **M0
194884 → M1 161272, 33612 saved** (≈17.2%). One row added, zero
moved — **all 85 shared rows byte-identical** (the gate row is
strictly additive):

- jmpr_chain33 7468 → 6444 (−1024, new 81st row; M0 baseline
  measured at git 1729f50) — the 64-record-DAG pool pin: runtime
  252 dispatches through the table's bounds check to block63's
  LOADI #7777 -> PASS 7777 on all four engines. The 1024-byte
  M0->M1 delta is the accumulated emission folds on the program's
  straight-line body (254 instructions, 63 in-place ADDs).
  (Equal-caps note: M2.61 changed this row — the 80-value root now
  fits 96, the walk keeps the DAG flat, and the chain fires 64
  b.eq at M1 5904; see §10.124/§10.132.)
- Teeth — the pool's two boundaries (ad hoc, not committed):
  (a) Control A (BIG=128 > MAX=64) — the same DAG materializes
  80 values and chains: 64 b.eq, runtime 252 = the sixty-fourth,
  PASS 7777 (sound when reachable); (b) Control B (a 65th in-place
  ADD, BIG=128) — the pool exhausts, r1 UNKNOWN, the table: 0 b.eq,
  PASS 7777 via the bounds check (conservative).
- Row-by-row accounting (gate tables diffed vs the current committed
  simi_arm.c): **all 85 shared rows byte-identical** — strictly
  additive; simi_arm.c is UNCHANGED in M2.57 (a pure pin + gate
  row + doc).
- Four-way parity: 341 PASS, 0 FAIL — all 86 expected-result
  programs on all four engines (interp 86/86, x86 85/0/3, RV64
  85/0/3, ARM 85/0/3), each checked against its "Expected result:"
  comment; the pool-pin's table path executes at runtime on the ARM
  engine (PASS 7777 through the bounds check), and the documented
  float/mem skips and the no-expected jmpr_oob are unchanged
  (jmpr_oob still faults via UDF, rc=1). enc-check clean.

### 10.126 M2.58 — the walk's fixpoint convergence pinned, and an unsound truncation fixed (as built)

A PROBE milestone that found and fixed a REAL soundness bug. The
chain walk iterates its linear-stream analysis to a fixpoint, capped
at 64 iterations (`for (iter = 0; changed && iter < 64; iter++)`);
`changed` is set solely by the dispatch-snapshot comparator. M2.58
probed the convergence behavior with a BACKWARD-branch join lattice
(jmpr_chain34): a loop that increments r2 by r3 = {1} and branches
BACK to its own head, delivering r2's loop-carried set to the
already-processed head. Because the walk is path-insensitive, the
head's set grows by ONE value per walk iteration ({100}, {100,101},
{100,101,102}, ...) — the fixpoint of an accumulator loop is
UNBOUNDED, so the walk NEVER converges, and the 64-iteration cap
stops it at iteration 63 with a truncated set.

**The bug.** The pre-fix snapshot at iteration 63 was the
post-increment {101..164}, and the emission fired the chain over it
(64 b.eq, dump-verified). But the runtime executes the loop 65 times
(r5 counts 65 -> 0), so the index at the JMPR is 165 — a LEGITIMATE
value (the loop can iterate any number of times) that the truncated
set does not cover. The chain fell through to UDF: the ARM engine
faulted (rc=1, "unimplemented or malformed instruction") while
interp/x86/rv64 — which execute the .tmo directly and have no chain
analysis — returned 8000. A real soundness break: the chain's
fall-through was documented to "fire exactly for indices the analysis
proves impossible", but the truncation proved 165 impossible when it
was not.

**The fix — the exact-or-conservative discipline at the cap.**
After the iteration loop, `if (changed) chain_unknown_big(&g_chain_bigsnap)`
— an UNCONVERGED walk collapses the snapshot to UNKNOWN and the
dispatch falls to the table, which dispatches every index correctly.
The discriminator control (guard temporarily removed) reproduces the
UDF exactly; with the guard, jmpr_chain34 PASSes 8000 through the
table's bounds check. **Byte-invisible**: every existing program's
lattice converges within the cap (the corpus is all forward join
lattices that settle in a handful of iterations) — the size gate's
85 shared rows are byte-identical with the fix in.

### 10.127 M2.58 gate results (measured)

Total emitted bytes across the now-82-program parity set: **M0
199680 → M1 165392, 34288 saved** (≈17.2%). One row added, zero
moved — despite the simi_arm.c unconverged guard, **all 86 shared
rows are byte-identical** (the guard only fires when the walk is
still moving at iteration 64, impossible for every converged
program):

- jmpr_chain34 4796 → 4120 (−676, new 82nd row; M0 baseline
  measured at git 1729f50) — the convergence pin: runtime 165
  dispatches through the table's bounds check to block32's LOADI
  #8000 -> PASS 8000 on all four engines. The 676-byte M0->M1
  delta is the accumulated emission folds on the program's small
  straight-line body (167 instructions, a 3-instruction loop).
- Teeth — the fix's two sides (ad hoc, not committed): (a) the
  UNSOUND chain — guard removed, the same fixture's truncated
  {101..164} chain fires (64 b.eq) and runtime 165 UDF-faults
  (rc=1), the exact bug the pin regression-guards; (b) the
  sound table — guard present, 0 b.eq, PASS 8000 through the
  bounds check.
- Row-by-row accounting (gate tables diffed vs the current committed
  simi_arm.c): **all 86 shared rows byte-identical** — the only
  simi_arm.c delta is the unconverged guard, provably invisible for
  every converged program.
- Four-way parity: 345 PASS, 0 FAIL — all 87 expected-result
  programs on all four engines (interp 87/87, x86 86/0/3, RV64
  86/0/3, ARM 86/0/3), each checked against its "Expected result:"
  comment; the convergence-pin's table path executes at runtime on
  the ARM engine (PASS 8000 through the bounds check), and the
  documented float/mem skips and the no-expected jmpr_oob are
  unchanged (jmpr_oob still faults via UDF, rc=1). enc-check clean.

### 10.128 M2.59 — the fold fixpoint's retroactive split pinned, and its interaction with the M2.58 guard (as built)

A pure probe + pin milestone (`simi_arm.c` unchanged). The rule-1
comment at the fold fixpoint's mark loop documents the RETROACTIVE
SPLIT — "a fold target landing inside another JMPR's chain, before
that JMPR's constant source" — but notes the corpus has no dedicated
test for it. M2.59 constructed it (jmpr_chain35): JMPR A (pc 3,
index 5) folds to target T = pc 5, which sits INSIDE JMPR B's chain
(B's source LOADI r2 at pc 2, then T, then B at pc 6). B's index
source is set BEFORE A on purpose: the runtime path into T is A's
dispatch (which skips pc 4), so B's source must precede A to be live
at B — while the ANALYSIS still sees source < T < B, exactly the
split shape the comment names.

**The convergence.** Pass 1 folds BOTH JMPRs (no target is marked
when the scan reaches pc 5 — marks land post-pass). The rule-1 mark
on pc 5 then resets the constant map at T in pass 2, killing r2 = 9
— B's constant source is before the split — so B UN-FOLDS; pass 3
confirms (no change). The fold set is monotone-decreasing (a fold
only ever dies; marks only ever accrete), so the fixpoint settles in
exactly 3 scans; instrumentation confirmed `passes` converged with
fold3=5 fold6=-1. The 512-pass safety net stays unreachable (it needs
the relaxation 2-cycle, which the corpus still cannot construct) —
consistent with the existing comments.

**The M2.58 interaction.** The converged state — A folded, B the
single dynamic JMPR — feeds the M2.25 chain walk: g_alloc = 0, the
walk's snapshot at B is the singleton {9} (the union at the head T
is {9} from the fall-through carry; the fold edge delivers the same
set, so the merge is exact), and B emits a 1-candidate inline chain
(cmp + b.eq -> pc 9, UDF fall-through) instead of the runtime table.
The fold fixpoint's convergence (its own passes) is separate from
the walk's 64-iteration cap: the walk here is forward-only — no
backward edge — so it converges in ONE iteration, and the M2.58
unconverged guard (changed still true at the cap -> UNKNOWN) stays
silent. The pin proves the two analyses compose: a fold that takes
multiple fixpoint passes to settle still hands the walk a converged,
sound input.

**Teeth.** Four-way parity (runtime 15 on all four engines: A -> T
-> B -> target9) proves the split path dispatches correctly through
the chain. The dump pins the emission shape: 1 unconditional branch
(A's fold, b pc 5), 1 b.eq (B's chain), 1 UDF (chain fall-through),
0 table words. The control (rule-1 mark disabled ad hoc, not
committed) keeps B folded -> g_alloc = 1 -> cache mode -> 984 bytes,
0 b.eq — a different, smaller emission the gate row discriminates.

### 10.129 M2.59 gate results (measured)

Total emitted bytes across the now-83-program parity set: **M0
200920 → M1 166460, 34460 saved** (≈17.1%). One row added, zero
moved — `simi_arm.c` is byte-identical to the M2.58 commit (this is
a pure pin + gate row + doc):

- jmpr_chain35 1240 → 1068 (−172, new 83rd row; M0 baseline
  measured at git 1729f50) — the retroactive-split pin: A's
  direct-branch fold, B's 1-candidate chain replacing its runtime
  table, and the naive-mode frame discipline. Runtime 15 = 10 + 5
  (r3 = r2+1 at T, r0 = r3+r1 at target9) on all four engines.
- Row-by-row accounting: **all 87 shared rows byte-identical** —
  zero simi_arm.c delta, so the gate proves nothing else moved.
- Four-way parity: 349 PASS, 0 FAIL — all 88 expected-result
  programs on all four engines (interp 88/88, x86 87/0/3, RV64
  87/0/3, ARM 87/0/3); the split path (A -> T -> B -> 9) executes
  at runtime on every engine. enc-check clean.

### 10.130 M2.60 — the relaxation 2-cycle driven into the 512-pass safety net (as built)

A pure probe + pin milestone (`simi_arm.c` unchanged). The M2.18
safety-net comment documents the last untested convergence
mechanism: "a fold ENABLED BY the relaxation can target the relaxed
head itself (a backward fold re-entering the head makes it a loop
head the relaxation must not apply to) — that shape 2-cycles the
fixpoint (relax -> fold -> reset -> un-fold)". M2.60 constructed it
(jmpr_chain36):

- The loop head V (pc 5) is RELAX-ELIGIBLE: its unique incoming
  edge is S's forward BR (pc 3), V-1 = pc 4 is a RET (a terminal —
  no fall-through), no CALL targets V, V is not an entry, and no
  backward BR/BC targets it (J is a JMPR — the eligibility scan
  counts only BR/BC/CALL edges, so J's back-edge is invisible to
  it).
- JMPR J (pc 8) dispatches on r2 = 5 = V's pc — a BACKWARD fold
  re-entering the relaxed head — and r2's constant (LOADI at pc 2)
  is established BEFORE V. V is a pre-marked block head (BR
  targets are heads from the start), so its reset kills r2 UNLESS
  the relaxation restores it from the S snapshot — the fold is
  genuinely ENABLED BY the relaxation, exactly as the comment says.

**The 2-cycle, instrumented.** Pass 1: snapshot at S, relaxation
restores r2 at V, J folds. Pass 2: V is a fold target (g_fold_tgt_prev
live-excluded) — the relaxation is refused, the reset kills r2, J
un-folds. Pass 3: V is no longer a fold target — relax again, J folds.
The fixpoint alternates forever: **NET FIRED at total pass 513** (the
>512 boundary), the safety net disables the relaxation, clears the
fold set, and restarts; the un-relaxed fixpoint is monotone-decreasing
and terminates at **total pass 514** with J un-folded (fold8=-1). The
post-net state is EXACTLY what the un-relaxed analysis would reach, so
the restart is sound — the pin guards TERMINATION, not emission.

**The M2.58 interaction.** The post-net state — J the single dynamic
JMPR — feeds the walk: g_alloc = 0, the S-branch delivers {5} at the
head V, and J emits a 1-candidate chain (cmp r2, #5; b.eq BACK to V;
UDF fall-through) instead of the runtime table. The loop's back-edge
is NOT an edge for the walk — an unfolded JMPR is a terminal — so the
walk converges in 2 iterations and the M2.58 unconverged guard stays
silent even though the PROGRAM loops. Runtime: the chain dispatches
r2 = 5 back to V, the loop runs three times (r1 3->2->1->0), the INV
BC exits at r1 == 0, r0 = 5 + 5 = 10 on all four engines.

**Teeth.** Control A (the head forced not relax-eligible, ad hoc):
the fixpoint takes 1 pass, no net, same 1056-byte emission, PASS 10
— the 2-cycle requires the eligibility. Control B (the net disabled,
ad hoc, timeout-bounded): the translator HANGS on the 2-cycle (exit
124) — the net is provably the termination mechanism. Dump-verified:
1 b (S's BR), 1 b.eq (J's chain, BACKWARD to V), 1 udf, 0 table
words.

### 10.131 M2.60 gate results (measured)

Total emitted bytes across the now-84-program parity set: **M0
202100 → M1 167516, 34584 saved** (≈16.9%). One row added, zero
moved — `simi_arm.c` is byte-identical to the M2.59 commit (pure
pin + gate row + doc):

- jmpr_chain36 1180 → 1056 (−124, new 84th row; M0 baseline
  measured at git 1729f50) — the 2-cycle pin: the 1-candidate
  chain replacing J's runtime table (the chain's b.eq loops back
  to V — the walk's backward candidate — which the M0 table's
  indirect branch also served) plus the accumulated emission
  folds on the small loop body. Runtime 10 on all four engines.
- Row-by-row accounting: **all 88 shared rows byte-identical** —
  zero simi_arm.c delta; the net never fires for any corpus
  program (all converge), so nothing else can move.
- Four-way parity: 353 PASS, 0 FAIL — all 89 expected-result
  programs on all four engines (interp 89/89, x86 88/0/3, RV64
  88/0/3, ARM 88/0/3); the loop body (S -> V -> J -> V ... ->
  EXIT) executes at runtime on every engine. enc-check clean.

### 10.132 M2.61 — the permanent EQUAL-CAPS regression: MAX and BIG bumped together 64 -> 96 (as built)

The cap series (M2.54/M2.55) found the discipline's two failure modes
when the caps DIVERGE — the flatten overflow (MAX > BIG) and the
freeze overflow (BIG > MAX) — and every pin since guarded the
equal-caps turn-over at 64 or a collapse side. This milestone makes
the equality itself a committed, re-measured fact: **TX_AR_CHAIN_MAX
and TX_AR_CHAIN_BIG are permanently bumped TOGETHER 64 -> 96**, the
gate is re-measured, and a new turn-over pin (jmpr_chain37) guards
the new boundary. Nothing in the code hardcodes 64 (the M2.55
controls already ran the whole suite at 128/20 and 128/64 — the caps
are pure macros), so the bump is a pure capability gain, monotone in
set size: sets <= 64 behave byte-identically, and 65-96-value sets
gain the chain.

**The new turn-over pin (jmpr_chain37).** The index is the union of
two flat products (chain31's R-arm shape scaled): R1 = r2 + r3 (r2 in
{52,68,84,100,116,132}, r3 in {0,2,...,14}) = 48 values {52..146}
step 2, and R2 = r2 + r6 (r6 in {200,202,...,214}) = 48 values
{252..346} step 2 — the union is **96 DISTINCT values, exactly the
new MAX = BIG**, materializing through the flat walk, the head union,
and the emission gate at the same point. Runtime 132 + 214 = 346 (all
join BCs fall through: r0 == 0, r5 == 0) takes the chain's
NINETY-SIXTH b.eq -> block95's LOADI #987 -> PASS 987 on all four
engines; a truncated materialization (a union one short, or a store
one short) misses 346 and UDF-traps. Dump-verified: **96 b.eq, 97 br,
1 udf, 0 table words**. The cost gate (8*96+4 = 772 < 40 + 4*348 =
1432) emits the full 96-pair chain.

**What the bump proves about the discipline.** Three rows moved, all
intended capability gains:

- jmpr_chain37 9348 -> 7288 (new 85th row) — the 96 turn-over.
- jmpr_chain33 6444 -> 5904 — its 80-value root now FITS the 96
  store, so the 64-record DAG materializes and chains (64 b.eq;
  runtime 252 = the sixty-fourth). The M2.57-era collapse at 64/64
  (the M2.42 "vestigial" note) was CAP-SPECIFIC, not structural —
  M2.57's own Control A (BIG=128) proved the same DAG chains when
  the caps admit it, and the shipped 96/96 state is exactly that
  behavior, now regression-guarded.
- jmpr_chain27 3884 -> 3760 — its 65-distinct NEG image now fits
  and chains (65 b.eq; runtime 150 = the sixty-fifth). Same story
  as chain33: the M2.51 collapse pin became the exact-side pin.
- jmpr_chain30 (100 values) STILL collapses to the table (100 >
  96) — the over-cap conservative side of the discipline is
  unchanged and still pinned.

### 10.133 M2.61 gate results (measured)

Total emitted bytes across the now-85-program parity set: **M0
211448 → M1 174140, 37308 saved** (≈17.6%). One row added, three
moved (the intended 65-96 capability gains above); the other **87
shared rows are byte-identical** — proving the bump is invisible
below 65 values:

- Row-by-row accounting (gate table diffed vs the M2.60 commit's
  build): only jmpr_chain27 (-124), jmpr_chain33 (-540), and the
  new jmpr_chain37 (+7288) differ — every other shared row is
  byte-for-byte the M2.60 emission.
- Four-way parity: 357 PASS, 0 FAIL — all 90 expected-result
  programs on all four engines (interp 90/90, x86 89/0/3, RV64
  89/0/3, ARM 89/0/3); chain37's 96-candidate chain and chain33's
  64-candidate chain both execute at runtime on every engine.
  enc-check clean, a64-f0 PASS, stress all-green.

### 10.134 M2.62 — the tracked-register closure raised to its 12-cap, and the turn-over pinned (as built)

M2.61 made the equal-caps invariant permanent (MAX = BIG = 96); this
milestone applies the same regression discipline to the register
dimension: **TX_AR_CHAIN_REGS is raised 8 -> 12**, and jmpr_chain38
pins the new turn-over — a closure needing EXACTLY TWELVE registers,
the largest the cap admits. The index is built in six stages over
joins (chain32's structure extended): r4 = r8 + r8 (the self-add
tracks r8 ONCE — that's what keeps the closure at twelve), r5 = r9 +
r10, r2 = r4 + r5, r6 = r11 + r12, r3 = r6 + r7, r1 = r2 + r3 -> the
closure {r1..r12} = 12 = TX_AR_CHAIN_REGS: every feeder tracked, and
the 33-candidate chain fires (cost 268 < table 2020).

**The turn-over.** Runtime 10 + (10+5) + (50+5) + 400 = 490 — the LAST
of the 33 candidates {40..90} U {240..290} U {440..490} step 5 — takes
the chain's THIRTY-THIRD b.eq -> block32 at pc 490 -> LOADI #3200 ->
PASS 3200 on all four engines; a truncated image (32 candidates, or
one dropped feeder) misses 490 and UDF-traps. Dump-verified: **33
b.eq, 34 br, 1 udf, 0 table words**. The control (REGS=8, ad hoc,
not committed): the closure stops growing at 8 — the untracked
feeders' sets stay UNKNOWN, poisoning r1 -> the dispatch falls to the
table: 0 b.eq, PASS 3200 through the bounds check (9376 bytes vs
7624 at the 12-cap) — the over-cap side of the boundary is
conservative and correct, never a wrong chain.

**Footprint note.** g_chain_arr is REGS x 4096 ChainSets, so the
static BSS scales with the cap (8 -> 12 adds ~6 MB at MAX = 96).
The ceiling semantics are unchanged: every existing fixture uses at
most 8 feeders, so nothing below the new cap can move.

### 10.135 M2.62 gate results (measured)

Total emitted bytes across the now-86-program parity set: **M0
222812 → M1 181764, 41048 saved** (≈18.4%). One row added, **zero
moved** — the totals delta is EXACTLY chain38's row (M0 +11364, M1
+7624), so all 89 shared rows are byte-identical; the REGS bump is a
pure ceiling:

- jmpr_chain38 11364 → 7624 (−3740, new 86th row; M0 baseline
  measured at git 1729f50) — the 12-register turn-over pin: the
  33-candidate chain replacing the runtime table, on a 495-
  instruction body (the 12-join header + the three 11-block
  candidate regions + fillers). Runtime 490 on all four engines.
- Row-by-row accounting: **all 89 shared rows byte-identical** —
  the gate total moved by exactly chain38's M1; the ceiling never
  changes emission below it.
- Four-way parity: 361 PASS, 0 FAIL — all 91 expected-result
  programs on all four engines (interp 91/91, x86 90/0/3, RV64
  90/0/3, ARM 90/0/3); chain38's 33-candidate chain executes at
  runtime on every engine. enc-check clean, a64-f0 PASS, stress
  all-green.

### 10.136 M2.63 — the 13-register OVER-cap boundary pinned (as built)

M2.62 pinned the at-cap turn-over (jmpr_chain38 — the closure
{r1..r12} = exactly TX_AR_CHAIN_REGS = 12); M2.63 pins the other
side of the same boundary: a closure needing EXACTLY THIRTEEN
registers — the smallest the cap rejects — must truncate
conservatively to the runtime table. This is a pure probe + pin:
**simi_arm.c is unchanged** (the truncation is the existing
`ntr < TX_AR_CHAIN_REGS` guard on the closure scan, first committed
at M2.26).

**The fixture (jmpr_chain39).** The index is chain38's six stages
plus one intermediate (r13 = r2 + r6, then r1 = r13 + r3) and a
2-way r7 join {0, 200} (narrowed from chain38's 3-way so the
COMPLETED image — the control's — fits the 96-cap). The closure
scan fills in stream order: round 1 {r1, r13, r3}, round 2 +{r2,
r6, r7}, round 3 +{r4, r5, r11, r12} (ntr=10), round 4 +r8 (11)
then +r9 (12 — the `ntr < cap` guard now blocks r10). Tracked =
{r1, r13, r3, r2, r6, r7, r4, r5, r11, r12, r8, r9} = TWELVE, and
r10 — the thirteenth feeder — is untracked. Its set stays UNKNOWN
in the walk, so r5 = r9 + r10 is UNKNOWN, poisoning r2 → r13 → r1:
no candidate set, no chain, the runtime table. Runtime 345 (all
join BCs fall through: 10 + 10 + 5 + 50 + 5 + 200 → r13 = 35 + 55
= 90, r3 = 55 + 200 = 255, r1 = 90 + 255 = 345) dispatches through
the table's bounds check to block33 at pc 345 → LOADI #3200 → PASS
on all four engines.

**Dump-verified (shipped):** 0 b.eq, 0 b — the M2.24 table
dispatch (cbz bounds-check, 2-word li32 base, add-shift, ldr W,
br, UDF) and the 350-entry table; 6556 bytes. The body UDF and the
dispatch UDF are the two 0x00000000 words, matching jmpr_dyn's
shape.

**The turn-over control (REGS=13, ad hoc, reverted):** the closure
completes — r10 tracked, r5 known, r13 = {40..90 step 5} (11),
r3 = 8 values, r1 = r13 + r3 = 28 distinct values (14 + 14 sums,
≤ TX_AR_CHAIN_MAX = 96) — and the **28-candidate chain fires**
(dump: 28 b.eq, 0 table words, 5344 bytes), still PASS 3200: 345
is the LAST candidate. The same fixture, the chain shape — the
discriminator is the dump + byte size, not the result. Reverting
the control restored the shipped 6556-byte table emission exactly.

### 10.137 M2.63 gate results (measured)

Total emitted bytes across the now-92-program parity set: **M0
230776 → M1 188320, 42456 saved**. One row added, **zero moved** —
the totals delta is EXACTLY chain39's row (M0 +7964, M1 +6556), so
all 90 shared rows are byte-identical; the pin changes nothing:

- jmpr_chain39 7964 → 6556 (−1408, new 91st row; M0 baseline
  measured at git 1729f50) — the 13-register over-cap pin: the
  table path itself is byte-identical to M0's shape (no chain,
  no fold); the −1408 is M2.24's table compaction (32-bit
  entries, 2-word base) plus the accumulated emission folds, on
  a 350-instruction body.
- Row-by-row accounting: **all 90 shared rows byte-identical** —
  the gate total moved by exactly chain39's M1.
- Four-way parity: 365 PASS, 0 FAIL — all 92 expected-result
  programs on all four engines (interp 92/92, x86 91/0/3, RV64
  91/0/3, ARM 91/0/3); chain39's table dispatch executes at
  runtime on every engine. enc-check clean, a64-f0 PASS, stress
  all-green.

### 10.138 M2.64 — the deferred-record pool raised to its 96-cap (as built)

M2.64 completes the EQUAL-CAPS regression's third dimension:
TX_AR_CHAIN_DEFS (the deferred-record pool) raised 64 → 96 to match
MAX and BIG (M2.61 did the set caps, M2.62 the register closure).
The code change is the one `#define` plus this milestone's comment.

The honest finding first: **at equal caps the deferred-record
machinery is observationally conservative, so the bump is a pure
ceiling.** Instrumenting the dispatch captured the current state:
jmpr_chain33 (the M2.57 64-record pin) is now **flat** — `ndef = 0`,
its 80-value root stays flat at MAX = 96 and never touches the pool
(the M2.61 MAX bump moved that boundary; the M2.57 "64-record DAG"
comment is a fossil). jmpr_chain30 (100-value index) is the ONLY
corpus program that creates a record (`ndef = 1`), and its
materialization collapses conservatively: a root product whose true
set exceeds 96 DEFERS (record 0), and its materialization at the
BIG store overflows chain_merge_big → UNKNOWN — the exact-or-
conservative discipline, never a truncated set. So the pool bounds
only DAG depth, and nothing below the cap moves.

**The pin (jmpr_chain40).** A DAG needing EXACTLY 96 records: the
root product r1 = r2 + r3 (r2 in {0,10,…,90} — 10 arms, r3 in
{0..9} — 10 arms → ALL 100 values {0..99} distinct, > MAX 96)
overflows the image merge cap and defers (record 0), then **95
in-place ADDs** r1 = r1 + r4 (r4 = {2}, a flat singleton; the
closure {r1,r2,r3,r4} = 4 ≤ REGS) each defer as record k =
op(rec(k-1), slot r4) — **96 records = TX_AR_CHAIN_DEFS exactly**
(instrumented: `ndef = 96`, r1's def = 95 at the dispatch). The
materialization of the 100-value root collapses conservatively, so
the dispatch is the TABLE — 0 b.eq — and runtime **289** (the
fall-through arms 90 + 9 plus 95×2, the **hundredth** value of the
true set {190..289}) passes the bounds check to block0 at pc 289 →
LOADI #7777 → PASS on all four engines. The discriminator is
soundness-shaped: any truncated materialization (96, 97, 98, or 99
candidates) would miss 289 and UDF-trap (rc=1) — the four-way PASS
proves the collapse is exact, not truncated.

**The boundary control (DEFS=95, ad hoc, reverted).** The 96th
allocation returns -1 (pool exhausted), r1 falls to FLAT UNKNOWN at
the walk, and the dispatch is the table again — the SAME 6324-byte
emission, a DIFFERENT mechanism (walk-state UNKNOWN vs
materialization collapse), proven by the walk state (`ndef = 95` vs
96). Reverting restored the shipped emission byte-identically.

### 10.139 M2.64 gate results (measured)

Total emitted bytes across the now-93-program parity set: **M0
238284 → M1 194644, 43640 saved**. One row added, **zero moved** —
the totals delta is EXACTLY chain40's row (M0 +7508, M1 +6324), so
all 92 shared rows are byte-identical; the DEFS bump is a pure
ceiling (chain30's single record and every other program's walk are
unchanged):

- jmpr_chain40 7508 → 6324 (−1184, new 92nd row; M0 baseline
  measured at git 1729f50) — the 96-record pool pin: the table
  path itself is byte-identical to M0's shape (no chain — the
  over-cap DAG collapses conservatively); the −1184 is M2.24's
  table compaction (32-bit entries, 2-word base) plus the
  accumulated emission folds, on a 294-instruction body.
- Row-by-row accounting: **all 92 shared rows byte-identical** —
  the gate total moved by exactly chain40's M1.
- Four-way parity: 369 PASS, 0 FAIL — all 93 expected-result
  programs on all four engines (interp 93/93, x86 92/0/3, RV64
  92/0/3, ARM 92/0/3); chain40's table dispatch executes at
  runtime on every engine (the ARM engine is the soundness check
  — a truncated candidate set would UDF-trap its runtime 289).
  enc-check clean, a64-f0 PASS, stress all-green.

### 10.140 M2.65 — the 97-record OVER-cap boundary pinned (as built)

M2.64 pinned the at-cap turn-over (jmpr_chain40 — a 96-record DAG
fits the pool, r1's def = 95 at the dispatch); M2.65 pins the other
side of the same boundary: a DAG needing EXACTLY NINETY-SEVEN
records — the smallest the pool rejects — must exhaust it
conservatively at the walk. This is a pure probe + pin: **simi_arm.c
is unchanged** (the exhaustion is the existing `g_chain_ndef >=
TX_AR_CHAIN_DEFS → -1` guard in chain_def_alloc, first committed at
M2.32).

**The fixture (jmpr_chain41).** chain40's shape plus ONE in-place
add: the root product r1 = r2 + r3 (r2 in {0,10,…,90}, r3 in {0..9}
→ all 100 values {0..99} distinct, > MAX 96) defers as record 0,
then 96 in-place ADDs r1 = r1 + r4 (r4 = {2}) defer — 97 records
needed. At DEFS = 96 the first 96 allocations succeed (records
0..95) and the 97th returns -1: r1 falls to FLAT UNKNOWN at the
walk (instrumented: `ndef = 96, r1's def = -1` — vs chain40's
`def = 95`, the fits-and-materializes side). The dispatch is the
TABLE — 0 b.eq — and runtime **291** (the fall-through arms 90 + 9
plus 96×2, the **100th** value of the true set {192..291}) passes
the bounds check to block0 at pc 291 → LOADI #8888 → PASS on all
four engines. The discriminator is soundness-shaped: a
non-conservative exhaustion fallback — one that kept a truncated
partial image instead of clearing to UNKNOWN — would miss 291 and
UDF-trap (rc=1), so the four-way PASS proves the -1 fallback is
exact-or-conservative, not truncated.

**The boundary control (DEFS=97, ad hoc, reverted).** The 97-record
DAG FITS the pool (instrumented: `ndef = 97, r1's def = 96`), and
the 100-value materialization then collapses conservatively at the
BIG store (chain_merge_big → UNKNOWN) — the SAME 6364-byte table, a
DIFFERENT mechanism (walk-state pool exhaustion vs materialization
collapse), proven by the walk state. Reverting restored the shipped
emission byte-identically. Together chain40/chain41 bracket the DEFS
boundary from both sides: at-cap fits, one-past exhausts.

### 10.141 M2.65 gate results (measured)

Total emitted bytes across the now-94-program parity set: **M0
245840 → M1 201008, 44832 saved**. One row added, **zero moved** —
the totals delta is EXACTLY chain41's row (M0 +7556, M1 +6364), so
all 93 shared rows are byte-identical (the DEFS cap is untouched —
M2.65 is a pure pin):

- jmpr_chain41 7556 → 6364 (−1192, new 93rd row; M0 baseline
  measured at git 1729f50) — the 97-record over-cap pin: the
  table path itself is byte-identical to M0's shape (no chain —
  the pool exhaustion falls back conservatively); the −1192 is
  M2.24's table compaction (32-bit entries, 2-word base) plus
  the accumulated emission folds, on a 296-instruction body.
- Row-by-row accounting: **all 93 shared rows byte-identical** —
  the gate total moved by exactly chain41's M1.
- Four-way parity: 373 PASS, 0 FAIL — all 94 expected-result
  programs on all four engines (interp 94/94, x86 93/0/3, RV64
  93/0/3, ARM 93/0/3); chain41's table dispatch executes at
  runtime on every engine (the ARM engine is the soundness check
  — a truncated exhaustion fallback would UDF-trap its runtime
  291). enc-check clean, a64-f0 PASS, stress all-green.

### 10.142 M2.66 — the deferred-UNION path probed: the cap check makes it unreachable at equal caps (as built)

M2.66 answers the last structural question the M2.61–M2.65 cap
series left open: can ANY UNION record survive the equal-caps
collapse and fire a chain, or is the union allocator's cap check
what makes it unreachable? This is a pure probe + pin: **simi_arm.c
is unchanged**.

**The structural argument.** At MAX = BIG = 96 the ONLY ways to
create a record are (i) a flat product whose image exceeds MAX —
its true set is > 96 by construction — and (ii) products over such
a source (monotone in cardinality for every ALU op the image
supports). Every product record therefore has a true set > 96, and
its materialization overflows chain_merge_big → UNKNOWN. A UNION
record (chain_def_alloc_union) must materialize each CD_REC operand
over the current cur[] — a product record's flatten collapses — and
the flat-flat union path (M2.41) only triggers when the merged walk
set exceeds MAX, which the allocator's own merge then rejects. So
no union record can be allocated at equal caps: the allocator's
BIG-store cap check is the binding constraint, and the deferred-
union machinery is structurally unreachable.

**The probe (jmpr_chain42).** chain13's M2.37 skeleton scaled so
the deferred side is a > 96-value product: R1 = r2 + r3 (r2 in
{0,10,…,90}, r3 in {0..9} → all 100 values {0..99} distinct) defers
as record 0; a BC (r5 = 0, not taken) delivers R1 at J1 while the
flat arm writes r1 = #200 — OUTSIDE {0..99} — so the M2.36
containment test fails and the M2.37 union path runs:
chain_def_alloc_union(CD_REC rec0, CD_FLAT {200}) flattens its
record side — rec0's 100-value true set OVERFLOWS the BIG store —
returns -1, and r1 falls to FLAT UNKNOWN at the walk (instrumented:
`ndef = 1, r1's def = -1`) — the dispatch is the TABLE, 0 b.eq, and
runtime **232** (99 at the root, the flat arm's 200, then +32)
passes the bounds check to block0 at pc 232 → LOADI #4200 → PASS
on all four engines.

**The control (BIG=128 ad hoc, the M2.55 direction, reverted).**
rec0's materialization now fits the store (100 ≤ 128), the union
record IS created (instrumented: `ndef = 3, r1's def = 2`), and the
dispatch materializes {32..131} ∪ {232} = 101 candidates → the
**101-pair chain fires** (dump: 101 b.eq, 0 table words, 4480 vs
4656 bytes), runtime 232 = the LAST b.eq. The probe therefore proves
the cap check is the binding constraint: the union path is REACHED,
and the allocator's BIG-store check is what makes it unreachable at
equal caps. 232 is the 101st candidate — any truncated
materialization misses it and UDF-traps, so the four-way PASS proves
the collapse is exact.

### 10.143 M2.66 gate results (measured)

Total emitted bytes across the now-95-program parity set: **M0
251452 → M1 205664, 45788 saved**. One row added, **zero moved** —
the totals delta is EXACTLY chain42's row (M0 +5612, M1 +4656), so
all 94 shared rows are byte-identical (simi_arm.c untouched):

- jmpr_chain42 5612 → 4656 (−956, new 94th row; M0 baseline
  measured at git 1729f50) — the deferred-union probe: the
  table path itself is byte-identical to M0's shape (no chain —
  the union allocator rejects the over-cap record side); the
  −956 is M2.24's table compaction (32-bit entries, 2-word
  base) plus the accumulated emission folds, on a 237-
  instruction body.
- Row-by-row accounting: **all 94 shared rows byte-identical** —
  the gate total moved by exactly chain42's M1. (A mid-
  milestone control-residue caught in review: the gate was
  first run against the BIG=128 control binary, moving
  chain40's row by −412 and reporting chain42 at the control's
  4480; rebuilding from the reverted source restored both —
  chain40 6324, chain42 4656 — and the accounting above is the
  shipped state.)
- Four-way parity: 377 PASS, 0 FAIL — all 95 expected-result
  programs on all four engines (interp 95/95, x86 94/0/3, RV64
  94/0/3, ARM 94/0/3); chain42's table dispatch executes at
  runtime on every engine (the ARM engine is the soundness check
  — a truncated union materialization would UDF-trap its
  runtime 232). enc-check clean, a64-f0 PASS, stress all-green.

### 10.144 M2.67 — stale fixture-header cleanup to the equal-caps reality (as built)

A comment-only pass (no code change) prompted by the M2.64/M2.66
findings: several chain fixtures' headers still narrated the
M2.54–M2.57-era mechanics and caps, which the equal-caps bumps
(M2.61 MAX/BIG 64→96, M2.62 REGS 8→12, M2.64 DEFS 64→96) had made
false as *current-behavior* claims. The instrumented ground truth
(M2.64: every fixture flat with ndef=0 except chain30's single
record) was the authority for each rewrite:

- jmpr_chain29: "fits the current 64" / ">64-distinct image" → 96
  (the unary-over-deferred boundary numbers; the flat-side
  mechanism claim was already correct).
- jmpr_chain30: merge-cap "(64, TX_AR_CHAIN_MAX / TX_AR_CHAIN_BIG)"
  and the truncated-64-candidate discriminator → 96, plus an
  explicit note that the >96 product DEFERS as record 0
  (instrumented ndef=1) and collapses at the dispatch BIG store.
- jmpr_chain31: the "TURN-OVER BOUNDARY at exactly 64" framing was
  false at 96/96 — the 64-value union now sits BELOW the cap and
  materializes flat (instrumented ndef=0, the "deferred products"
  narrative is a fossil); the fixture still fires its 64-pair
  chain, and the boundary moved to chain37 (96) / chain30 (100).
- jmpr_chain32: "AT its 8-cap" → the 8-feeder closure is now BELOW
  TX_AR_CHAIN_REGS 12 (M2.62); the fixture still fires its 30-pair
  chain, and the boundary moved to chain38 (12) / chain39 (13).
- jmpr_chain33: the "64-record DAG falls to the table — 0 b.eq"
  narrative was false — at 96/96 the 80-value root fits the BIG
  store, the walk keeps everything flat (ndef=0), and the chain
  fires 64 b.eq; the pool boundary itself moved to the
  chain40/chain41 pins (M2.64/M2.65).

The size_gate_arm.sh row comments for chain29–33 were aligned to
match (the M2.61-era chain33 gate comment was already current in
its emission claim; its "pool boundary pinned by a 65th ADD at
DEFS=64" tail was corrected to point at chain40/41). chain6–18
headers were left as historical milestone narratives (they describe
what each milestone did at its then-current caps; chain18's
"vestigial" note already frames the record machinery as history).

### 10.145 M2.67 gate results (measured)

No code change, so the gate is a no-op by construction: **94/94,
M0 251452 → M1 205664, 45788 saved — byte-identical to M2.66's
totals, all 94 rows unchanged** (the comment edits cannot move
emission). Four-way parity re-run for safety: interp 95/95, x86
94/0/3, RV64 94/0/3, ARM 94/0/3 — all green on the exact final
state. The six changed files are comment-only: five fixture
headers and the gate script's row comments.

### 10.146 M2.68 — doc-side sweep: stale cap numbers aligned to the equal caps (as built)

A comment-only follow-up to M2.67's fixture-header pass, extending the
same audit to the plan doc's §10.x milestone records and the ISA doc.
The ISA doc has NO chain-cap references (verified: zero TX_AR_CHAIN
hits), so this sweep is plan-doc + gate-comment only. The M2.40–49
"as built" sections were left as historical milestone narratives (like
chain6–18's headers — they record what each milestone did at its
then-current caps); the M2.51–57 sections' *current-behavior* claims
were aligned to the 96/96/12/96 reality, each with an explicit
pointer:

- §10.112/10.113 (M2.51, chain27): the pin's "OVERFLOWS (64) -> the
  table" narrative was stale — M2.61's bump made 65 <= 96 FITS, the
  chain now fires 65 b.eq (M1 3884 -> 3760), and the collapse side
  moved to chain30. The M2.61 record §10.132 already documented the
  row change; the earlier sections now say so too.
- §10.116/10.117 (M2.53, chain29): "sub-64 / >64" boundary prose
  and "fits the current 64" -> 96 (the flat-side mechanism claim
  was already correct).
- §10.118/10.119 (M2.54, chain30): "100 > 64" and the
  truncated-64-candidate discriminator -> 96; the "Under the shipped
  caps (M2.42) n <= 64 = BIG" latent-bug context was re-framed as
  M2.42-era (the 96/96 invariant now holds the same way).
- §10.120/10.121 (M2.55, chain31): "turn-over boundary at the
  shipped caps (64/64)" -> re-framed as the M2.55-era boundary with
  the equal-caps note (the 64-value union is now below the 96/96
  cap, stays FLAT, instrumented ndef=0 at M2.64, still fires its
  64-pair chain; the boundary itself moved to chain37/chain30).
- §10.122/10.123 (M2.56, chain32): "at its 8-cap" -> the 8-feeder
  closure is now below TX_AR_CHAIN_REGS 12 (M2.62); the turn-over
  moved to chain38 (12) / chain39 (13).
- §10.124/10.125 (M2.57, chain33): "64-record DAG falls to the
  table — 0 b.eq" -> the equal-caps reality (the 80-value root
  fits 96, the walk keeps the DAG flat with ndef=0, and the chain
  fires 64 b.eq at M1 5904); the pool boundary moved to the
  chain40/chain41 pins.
- §10.94 (M2.42, historical record): a pointer note added where the
  old "> 64 values can NEVER chain" boundary claim could be misread
  as current (65-96-value sets now chain since M2.61).
- gate: jmpr_chain27.simi's fixture header carried the same stale
  M2.51-era collapse narrative as §10.112 (missed by M2.67's
  chain29-33 scope) — aligned it too; and the M2.61-era chain37 row
  comment claimed "all 88 shared rows unchanged except jmpr_chain33"
  but chain27 ALSO moved (65 > 64 now fits 96) — corrected to name
  both rows, matching §10.133's accounting.

### 10.147 M2.68 gate results (measured)

No code change, so the gate is a no-op by construction: **94/94,
M0 251452 → M1 205664, 45788 saved — byte-identical to M2.66/67's
totals, all 94 rows unchanged**. Four-way parity re-run for safety:
interp 95/95, x86 94/0/3, RV64 94/0/3, ARM 94/0/3 — all green on
the exact final state. Three files changed, all comment-only: the
plan doc (§10.112-125 sweep + §10.146/10.147 records), the chain27
fixture header, and the gate script's chain37 row comment.

### 10.148 M2.69 — the fold fixpoint's safety net probed: convergence is ≤ 3 scans, so the 512-pass net fires only on the 2-cycle (as built)

M2.59 pinned the retroactive split (chain35, two JMPRs, 3 passes); M2.60
pinned the relaxation 2-cycle (chain36, the net firing). The untested
boundary was the "many scans" direction: can a fold analysis need many
passes to converge, approaching the 512-pass safety net without being a
divergence? The probe (jmpr_chain43) scales chain35's mechanism to a
SIX-JMPR cascade — J_k folds (pass 1) to T_k, where T_k sits INSIDE
J_{k+1}'s chain (between S_{k+1} and JMPR_{k+1}), so the pass-1 rule-1
mark at T_k resets r_{k+1} in pass 2.

The structural argument — the answer is that the case does not exist.
g_pc_target (the block-head marks) only ever ACCRETES inside the
fixpoint: the per-pass scan resets the constant map at every marked pc,
and the post-pass rule-1 only adds marks (never removes). A JMPR
un-folds iff a marked pc sits in its chain, and once un-folded it can
never re-fold (no mark is ever removed; a LOADI re-establishing a
constant is a fixed instruction with a fixed value). The fold set is
monotone-decreasing, and pass 1's marks are ALL already live in pass 2 —
so every JMPR whose chain contains ANY pass-1 target un-folds in the
SAME pass. Convergence is exactly 3 scans (fold-all, un-fold-all,
confirm) for any RULE-1-driven cascade size; the pass count never
scales with the number of JMPRs. The M2.18 relaxation is the only
non-monotone force (it can RESTORE a constant at a relax-eligible
head, re-folding a JMPR), and its per-pass exclusion (g_fold_tgt_prev,
the previous pass's complete fold set) makes a re-folding JMPR's own
target flip the head's eligibility back and forth — the 2-cycle
chain36 pins. M2.70 (chain44) corrects the "ANY": when a fold INTO a
relax-eligible head un-folds, the head drops out of the next pass's
exclusion and its JMPR re-folds once — a legitimate 4th scan
(passes=3). The 2-cycle is the only DIVERGENCE, so the 512-pass
safety net is reachable ONLY by the 2-cycle; a slow-converging fold
analysis cannot be constructed.

The instrumented probe (temporary pass counter, reverted, simi_arm.c
byte-identical): **chain43 FIXPOINT passes=2 relax=1 net=0** — 2
changed iterations + 1 confirm = 3 scans for six JMPRs, the net never
fired, the relaxation stayed enabled. The control at the same
instrumented level — **chain36 FIXPOINT passes=0 relax=0 net=1**: the
2-cycle drove the counter past 512, the net fired exactly once (relax
disabled, fold set cleared, restart), and the un-relaxed restart
converged on its first scan (passes=0 after the reset — no JMPR folds
without the relaxation) — the restart-and-terminate behavior, still
PASSing = 10. Emission dump-verified: chain43 emits exactly ONE direct
`b` (J1's fold survives — its chain (1, 7) contains no mark, all
pass-1 targets sit at pc ≥ 9), 0 b.eq, and 5 `br x11` table dispatches
(J2..J6 went dynamic; multiple dynamic JMPRs mean no inline chain, so
g_alloc = 0 and the naive path emits the compacted table). Runtime
9 → 12 → 15 → 18 → 21 → 24 → r0 = 1 on all four engines.

### 10.149 M2.69 gate results (measured)

**95/95, M0 253200 → M1 207220, 45980 saved** — the new jmpr_chain43
row (M0 1748, measured at git 1729f50: the naive translator, all six
JMPRs via table → M1 1556, −192: the M2.24 table compaction and
accumulated naive-path emission folds) and all **94 shared rows
byte-identical** (45788 + 192 = 45980 exactly). Four-way parity on the
exact final state: interp 96/96, x86 95/0/3, RV64 95/0/3, ARM 95/0/3
(+1 each = chain43). The teeth: reverting the rule-1 mark (constants
flowing through fold targets) keeps J2..J6 folded → g_alloc = 1 →
cache mode → a smaller, different emission, caught by the gate row.

### 10.150 M2.70 — the safety net tightened 512 → 16, and the "≤ 3 scans" bound corrected (as built)

M2.69 claimed convergence is exactly 3 scans for ANY cascade. The
instrumented corpus sweep confirmed the empirical max (passes=2 — 71
fixtures passes=0, 21 passes=1, chain35/43 passes=2, only chain36 fires
its net), but the structural claim needed one more case: the
RELAXATION-FLIP. The M2.18 per-pass exclusion (g_fold_tgt_prev,
recomputed from the PREVIOUS pass's fold set) can CLEAR: when a JMPR
that folded INTO a relax-eligible head un-folds, the head drops out of
the next pass's exclusion and its JMPR re-folds once. jmpr_chain44
constructs it: J2 (pc 8) folds forward into J3's head H3 (pc 10) in
pass 1 while J1 (pc 4) folds to T1 = 6 inside J2's chain. Pass 2: T1's
rule-1 mark kills r2 permanently (J2's fold to 10 leaves the fold set),
and H3's exclusion kills r3 at H3 (J3 un-folds). Pass 3: J2's fold to
10 is GONE from pass 2's fold set, so H3 is no longer excluded — the
relaxation restores r3 = 12 and J3 RE-FOLDS (an un-fold ENABLED a
re-fold). Pass 4: same → converge. Instrumented: FIXPOINT passes=3
relax=1 net=0 — a legitimate 4th scan, net silent. J3's re-fold is
stable (its target 12 is fall-through, not the head — no self-reentry),
so the constructible legit max is passes=3; the 2-cycle (a fold into
its own head, chain36) is the only DIVERGENCE.

The tightening: `++passes > 512` → `++passes > 16`. The trip count
only needs to sit above the legit max with margin — 512 was a 170×
overestimate of the corpus's 2; 16 gives 5× over the constructible
passes=3 and 8× over the corpus max, cutting the 2-cycle's scan
count 514 → 18 (28.6×) and chain36's end-to-end translate ~8-9× —
measured as committed numbers by bench_net (M2.71; the end-to-end
ratio is diluted below the scan ratio by the per-translate fixed
passes — array clears, pass A, the reachability BFS, the emission —
which are identical at both trip counts; chain36's net now fires at
pass 17 instead of 513, with the same un-relaxed restart and the
same emission). The restart's soundness is unchanged: the un-relaxed
fixpoint is monotone-decreasing in the fold set and provably
terminates. Also amended: the M2.69-era "ANY cascade" phrasings in
chain43's header, the gate's chain43 comment, and §10.148 now scope
the 3-scan bound to rule-1-driven cascades and point at chain44 for
the relaxation-flip.

### 10.151 M2.70 gate results (measured)

**96/96, M0 254532 → M1 208308, 46224 saved** — the new jmpr_chain44
row (M0 1332, measured at git 1729f50: the naive translator, all three
JMPRs via table → M1 1088, −244: the M2.24 table compaction, J1/J3's
folds, and J2's M2.25 1-candidate inline chain) and all **95 shared
rows byte-identical** (45980 + 244 = 46224 exactly — the tightening
cannot move emission: no corpus fixture's fixpoint runs past passes=2,
and chain36's net outcome is unchanged). Four-way parity on the exact
final state: interp 97/97, x86 96/0/3, RV64 96/0/3, ARM 96/0/3 (+1
each = chain44). Dump-verified chain44: 1 b.eq (J2's chain), 0 table
dispatches, J1/J3 direct b's. The teeth: making H3 non-eligible (H3-1
= ADD instead of RET) keeps J3 folded from pass 1 → a different
emission, caught by the gate row.

### 10.152 M2.71 — the safety-net translate-time win committed as a measurement (as built)

The M2.70 claim was "~30× faster translate". M2.71 makes it a
committed, measured number: bench_net.c translates chain36 — the only
input in the corpus whose fold fixpoint DIVERGES (the M2.60 relaxation
2-cycle) — N times at the shipped trip count (16, now the tunable
g_ar_net_trip exposed in simi_arm.h) and at the M2.69-era count (512)
in the SAME process (identical fixed overhead: same buffer, same
warmup, same arrays), timing both with CLOCK_MONOTONIC. The fixpoint's
scan count (g_ar_net_scans, incremented once per scan) is reported as
the machine-independent truth: **18 vs 514 scans = 28.6×** — the 2-cycle
burns 17 scans before the net fires (then the un-relaxed restart
converges in 1) instead of 513 + 1.

The honest end-to-end number: **~8-9× wall-clock on chain36** (25.7 µs
vs 221 µs per translate, measured on this sandbox; three runs 7.1-9.1×).
The end-to-end ratio is BELOW the scan ratio because the per-translate
FIXED passes — the 4096-entry array clears, pass A, the reachability
BFS, the emission — are identical at both trip counts and dominate the
16-side's 18 scans; the scan-work win (28.6×) is the fixpoint's own
cost and is what the translate spends when the 2-cycle runs. (An
ad-hoc 2011-instruction padded variant measured only 2.0× — the fixed
passes scale with program size too, so the end-to-end ratio is
smallest on large programs; the win is fundamentally the scan count.)
The committed gate: `make bench-net` FAILS if the scan ratio drops
below 20× or the wall-clock ratio below 4×. The wall-clock floor is
deliberately loose — measured 4.9-9.1× on this sandbox depending on
machine load, and its purpose is to guard FIXED-cost regressions (the
per-translate array clears, pass A, the reachability BFS, the
emission); the precise fixpoint guard is the deterministic scan ratio
above. The M2.70-era "~30× translate time" phrasings (§10.150, the
chain44 gate comment, the net comment in simi_arm.c) are corrected to
the precise numbers.

### 10.153 M2.71 gate results (measured)

bench_net (N=100, this sandbox): **trip=16 25.74 µs/translate / 18
scans; trip=512 220.95 µs/translate / 514 scans; 8.6× wall-clock,
28.6× scans — ALL CHECKS PASSED**. Emission unaffected (the trip count
is read per translate; the shipped default stays 16, and no corpus
fixture's fixpoint runs past passes=2), so the size gate and four-way
parity are unchanged: **96/96, 46224 saved** and interp 97/97,
x86/RV64/ARM 96/0/3, re-run on the exact final state. New files:
bench_net.c, the Makefile bench-net target (in `all`, ~0.5 s at 100
iters — the M2.27 stale-binary lesson), the gitignore entry, and the
two exposed globals g_ar_net_trip / g_ar_net_scans in simi_arm.c/.h
(the trip count becomes a tunable, default unchanged at 16).

### 10.154 M2.72 — corpus-wide translate-cost bench, the fixpoint series' regression guard (as built)

bench_net pins the safety-net win on chain36, the only net-firing
input. bench_corpus.c extends the discipline to the WHOLE corpus: it
translates every tests/*.simi fixture N times (default 50, at the
shipped trip count — g_ar_net_trip is set to 16 explicitly, since
bench-net may have left 512), timing with CLOCK_MONOTONIC, and records
per fixture the instruction count (object header), the fixpoint scan
count (g_ar_net_scans), and the per-translate / per-instruction time.
Two committed assertions gate it:

1. **Total scans == 144 (deterministic, machine-independent)** — the
   fixpoint's convergence across all 99 fixtures that assemble (the
   96 size-gate rows plus mem_ops, cap_forge_debug, straight_line_bench,
   which the parity runners skip for runtime reasons but which translate
   fine). The per-fixture distribution is the fixpoint series in one
   table: 1 scan for the plain fixtures, 2 for the M2-era chain/join
   fixtures whose fold set changes once, 3 for chain35/43's retroactive
   split, 4 for chain44's relaxation-flip, 18 for chain36's net-firing
   2-cycle. ANY change to convergence — more passes, a fold that now
   survives or dies — moves this number and fails the gate. Update the
   constant deliberately when the corpus changes (adding/removing a
   fixture, or a legitimate convergence change), exactly like the size
   gate's M0 baselines.

2. **Aggregate per-instruction time < 4.0 µs/instr** — the measured
   aggregate (0.965-0.991 µs/instr on this sandbox) is dominated by the
   per-translate FIXED cost (the array clears, pass A, the reachability
   BFS, the emission — identical per translate), which is what makes it
   a stable, comparable number; the 4x ceiling tolerates machine speed
   differences while still catching gross cost regressions (an O(n²)
   pass, a new per-pass 4096-walk, a 5x slower scan step). The precise
   fixpoint guard is the deterministic scan total above; this catches
   the cost side.

The Makefile target assembles each fixture on the spot but only when
stale/missing (a fresh checkout pays the full assembly once; incremental
builds re-assemble only changed fixtures), and runs in `all` with
bench-net (~1 s total at 50 iters — the M2.27 stale-binary lesson).

### 10.155 M2.72 gate results (measured)

bench_corpus (N=50, this sandbox): **99 fixtures x 50 iters, 6672
instr, 144 scans, 0.991 µs/instr aggregate (second run 0.965) — ALL
CHECKS PASSED**; the per-fixture table shows the expected scan
distribution (chain35/43=3, chain44=4, chain36=18). bench-net re-run
with the wall-clock floor relaxed 5x -> 4x (a loaded-machine run hit
4.9x, marginal noise; measured range 4.9-9.1x): **PASS**. Emission
untouched (M2.72 adds no simi_arm.c change — the globals came in at
M2.71), so the size gate stays **96/96, 46224 saved, all rows
byte-identical**; four-way parity interp 97/97, x86/RV64/ARM 96/0/3.
New files: bench_corpus.c, the Makefile bench-corpus target + gitignore
entry, and the §10.152 floor amendment. (M2.73 supersedes this record's
two aggregate assertions — the total-scan count and the per-instruction
ceiling — with the per-row committed baselines of §10.156/10.157.)

### 10.156 M2.73 — per-row translate-cost baselines, the size gate's per-fixture model (as built)

M2.72's aggregate assertions (total scans == 144, aggregate < 4.0
µs/instr) could not see a change confined to ONE fixture — two
fixtures trading scan counts keeps the total, and a single fixture's
cost regression is diluted in the aggregate. M2.73 tightens to
COMMITTED PER-ROW BASELINES: bench_corpus.c now carries a 99-row
BASELINES table (name → scans, name → µs/instr, measured 2026-08-09
at N=500 on this sandbox), and every fixture's row is asserted:

- **scans: EXACT** — the deterministic, machine-independent fixpoint
  work per fixture (1 for the plain fixtures, 2 for the M2-era
  chain/join fixtures, 3 for chain35/43, 4 for chain44, 18 for
  chain36). ANY convergence change in ANY fixture fails its row — the
  tight per-fixture guard, closing the total's trade-off hole.
- **us/instr: asserted at a 3.0x margin over the committed value.**
  Per-fixture wall-clock is inherently noisy — two N=500 runs swung up
  to 42% on individual fixtures (WSL timer granularity, load drift;
  the aggregate stayed within 2.3%) — so a tighter margin would flake.
  3.0 absorbs the noise and machine-speed differences while still
  failing a fixture whose translate cost grows 3x or more: the gross
  single-fixture regression the aggregate ceiling could not see. The
  printed table shows every fixture's measured value next to its
  baseline, so drifts below the margin stay visible for a human.

A fixture with no baseline row FAILS loudly — adding/removing a
corpus fixture requires updating the table deliberately, exactly the
size gate's M0_BASELINES discipline. The Makefile target now runs at
N=500 for per-row timing stability (~2-3 s total in `all`).

### 10.157 M2.73 gate results (measured)

bench_corpus (N=500, this sandbox): **99 fixtures, 6672 instr, 144
scans, 0.950 µs/instr aggregate, 0 failed — ALL CHECKS PASSED**, all
99 rows within their committed baselines (teeth: corrupting add.tmo's
committed scans 1 -> 2 fails the row "scans=1 != committed 2",
reverted to PASS). Emission untouched, so the size gate stays **96/96,
46224 saved, all rows byte-identical**; four-way parity interp 97/97,
x86/RV64/ARM 96/0/3. Changed: bench_corpus.c (the BASELINES table +
per-row assertions, replacing the aggregate constants), the Makefile
bench-corpus target (N=50 -> 500 + comment), §10.154/10.155
superseded.

### 10.158 M2.74 — the execution-cost bench, the runtime-side measurement (as built)

The measurement story's final axis: bench_corpus pins TRANSLATE cost;
bench_exec.c pins what the translated code COSTS TO RUN. Every fixture
is translated once (translate cost is bench-corpus's job), then executed
N=500 times through a64_exec.c — the same purpose-built A64
decoder/executor the four-way parity uses — with the same mock host
functions (obj_ops's catalog) and the same Expected result parsed from
the .simi comment (run_arm_tests.sh's rule: the LAST "Expected result:"
match; the Makefile target reuses the assemble-on-the-spot loop). Three
fixtures are skipped exactly as the runner skips them: mem_ops
(address-0 pointer is a Phase 1 interpreter-only convenience),
jmpr_oob and cap_forge_debug (no Expected result — jmpr_oob's whole
point is that execution must NOT produce one). In `all` with the other
benches (~1-2 s).

The per-row guard mirrors bench_corpus's two-tier model:

- **steps: EXACT** — a64_exec_run now counts executed instructions
  (`A64Cpu.steps`, one increment per decoded instruction; behavior-
  neutral, the other callers never read it). Deterministic and machine-
  independent — the same translated bytes over zeroed guest state
  always execute the same count — so ANY change in how much the code
  executes (a decode that now faults early, a fold that changes the
  emitted control flow, an a64_exec regression) moves the count and
  fails the row. The bench also ASSERTS determinism: every iteration
  must execute the same steps AND produce the expected x9, which
  catches the fault-early hazard (a fixture that now faults after 3
  instructions looks FAST on the clock but fails both checks).
- **ns/call: asserted at 4.0x** over the committed value (median of
  three N=500 passes). Execution runs are only a few microseconds, so
  the wall-clock tail is fatter than translate's: three passes swung up
  to **2.28x per fixture** (worst of 96; 5 fixtures over 2.0x; mean
  ~1.3x — WSL load). 4.0 absorbs that tail and machine-speed
  differences while still failing a fixture whose execution cost grows
  4x+ — the gross single-fixture regression. (bench_corpus's margin is
  3.0x because its runs are longer and less load-sensitive.)

Corpus totals: **96 fixtures, 35032 steps** executed across one run of
every fixture (jmpr_chain34=1034, aggregate_abi=1108 the heaviest; the
atomics fixtures run their trivial M=0/R=0 path — the harness-filled
scratch cells are zeroed here, so the loops don't spin). A fixture with
no baseline row FAILS loudly — the M0_BASELINES update discipline.
Re-measure deliberately with `BENCH_EXEC_MEASURE=1
./bench-exec <tests/ fixtures> 500` (prints the table rows without
asserting).

### 10.159 M2.74 gate results (measured)

bench-exec (N=500, this sandbox): **99 fixtures, 35032 steps, ~8-11
µs/step aggregate, 3 skipped, 0 failed — ALL CHECKS PASSED**, all 96
rows within their committed baselines (teeth: corrupting add.simi's
committed steps 241 -> 999 fails the row "steps=241 != committed 999",
reverted to PASS). Emission untouched (no simi_arm.c change — the
A64Cpu.steps counter is a64_exec-side and behavior-neutral), so the
size gate stays **96/96, 46224 saved, all rows byte-identical**;
four-way parity interp 97/97, x86/RV64/ARM 96/0/3; bench-net PASS;
bench-corpus PASS; enc-check clean; stress all-green. Changed:
bench_exec.c (new — the per-row BASELINES table), a64_exec.h/.c
(the steps counter), the Makefile (bench-exec target in `all`/`clean`)
and .gitignore.

### 10.160 M2.75 — the parity harness asserts the committed step counts (as built)

The M2.74 follow-up: bench-exec's committed per-fixture step counts now
fire in the PARITY HARNESS itself, before any bench runs. The 96-row
baseline table moved out of bench_exec.c into **bench_baselines.h** —
the single source of truth shared by bench-exec (the execution-cost
gate) and simi_arm_verify.c (the four-way parity ARM leg).
simi-arm-verify takes a new optional `--steps` flag: after a successful
run, it derives the fixture name from the .tmo path (basename with
.tmo -> .simi), looks up the committed count, and asserts
A64Cpu.steps equals it — failing loudly if the fixture has no
committed row (the M0_BASELINES update discipline) or if the count
moves (a decode that now faults early, a fold that alters the emitted
control flow, an a64_exec regression). The check is skipped when
execution does not complete (a fault is already reported as FAIL), and
the counts are for the "main" entry the runners execute. run_arm_tests.sh
passes `--steps` on every fixture; the Makefile's simi-arm-verify and
bench-exec rules gained bench_baselines.h as a dependency.

Why the harness, not just the bench: a step-count regression is a
DECODE/EMISSION regression — the four-way parity is the semantic
surface that must catch it first, and it runs on every `make test-arm`.
The bench's per-row steps guard remains (same table, same numbers) as
the translate-cost-adjacent check; the harness check is the earlier,
cheaper tripwire.

### 10.161 M2.75 gate results (measured)

ARM parity with `--steps` on all 96 fixtures: **96 passed, 0 failed, 3
skipped — every PASS prints "steps ok"**, proving every committed count
matches the harness's own execution exactly (teeth: corrupting add.simi's
committed steps 241 -> 999 fails the parity row "--steps: executed 241
instructions != committed 999 (decode/emission regression)", reverted to
PASS). bench-exec unchanged (same table via the header): **96 rows within
their committed baselines, ALL CHECKS PASSED**. Emission untouched (no
simi_arm.c change), so the size gate stays **96/96, 46224 saved, all
rows byte-identical**; four-way parity interp 97/97, x86/RV64/ARM
96/0/3; bench-net and bench-corpus PASS; enc-check clean. Changed:
bench_baselines.h (new — the shared table), simi_arm_verify.c (the
--steps check), bench_exec.c (consumes the header), run_arm_tests.sh
(--steps on every fixture), the Makefile (deps) and plan doc §10.158
superseded for the table's location.

### 10.162 M2.76 — the step-count tripwire extended to RV64 and x86 (as built)

The M2.75 follow-up, completing the four-way story. Both remaining
parity legs now assert committed per-fixture counts in their runners:

- **RV64 — dynamic steps (the full mirror).** rv64_exec_run() gained
  the same behavior-neutral per-instruction counter as a64_exec
  (RvCpu.steps, one increment per decoded instruction), and
  simi-riscv-verify takes the same `--steps` flag as simi-arm-verify:
  after a successful run it derives the fixture name from the .tmo
  path, looks up the committed count in the new bench_baselines_rv64.h
  (96 rows, measured via the new RV64_STEPS_MEASURE env mode — the
  documented re-measure tool), and asserts cpu.steps equals it. The
  counts are deterministic and machine-independent — same translated
  bytes over zeroed guest state always execute the same count — so ANY
  decode/emission regression (fault-early decode, a fold altering the
  control flow, an rv64_exec change) fails the fixture in
  run_riscv_tests.sh, before any bench runs.
- **x86 — emitted bytes (the honest native-leg count).** The x86 leg
  executes REAL machine code on the host CPU (mmap PROT_EXEC + a
  function-pointer call), so there is no executor to count retired
  instructions — and this sandbox has NO virtualized PMU to count them
  in hardware (probed: perf_event_open PERF_COUNT_HW_INSTRUCTIONS
  returns ENOENT on the WSL2 kernel — /proc/sys/kernel/perf_event_paranoid
  is 2, so the syscall is permitted, the counter just doesn't exist;
  the software counters measure time, not instructions). The
  deterministic, machine-independent count that IS available is the
  emitted translation's size (simi_x86_translate's out_len) — the x86
  analog of the ARM size gate's per-fixture byte baselines. simi-jit-test
  takes `--bytes`: it asserts out_len equals the committed count in the
  new bench_baselines_x86.h (96 rows, measured from the runner's own
  "%u bytes native code" field) — ANY emission change (a fold altering
  how much code is emitted, a codegen restructure) fails the fixture in
  run_native_tests.sh. Dynamic correctness on the native leg remains
  the execution itself + the result check + the crash handler.

So the four-way story is now complete in the sense the sandbox allows:
ARM and RV64 (the executor legs) assert dynamic executed-instruction
counts; x86 (the native leg) asserts the emitted-byte count, with the
no-vPMU limitation recorded honestly rather than papered over.

### 10.163 M2.76 gate results (measured)

Native parity with `--bytes` on all 96 fixtures: **96 passed, 0 failed,
3 skipped** — every PASS prints "bytes ok". RV64 parity with `--steps`
on all 96 fixtures: **96 passed, 0 failed, 3 skipped** — every PASS
prints "steps ok". Teeth: corrupting add.simi's committed RV64 steps
225 -> 999 fails "--steps: executed 225 instructions != committed 999";
corrupting its committed x86 bytes 442 -> 999 fails "--bytes: emitted
442 bytes != committed 999"; both reverted to PASS. Emission untouched
(no simi_arm.c change), so the size gate stays **96/96, 46224 saved,
all rows byte-identical**; four-way parity interp 97/97, x86/RV64/ARM
96/0/3; bench-net, bench-corpus and bench-exec PASS; enc-check clean;
stress all-green. Changed: rv64_exec.h/.c (RvCpu.steps),
bench_baselines_rv64.h + bench_baselines_x86.h (new — the committed
tables), simi_riscv_verify.c (--steps + RV64_STEPS_MEASURE),
simi_jit_test.c (--bytes), run_riscv_tests.sh + run_native_tests.sh
(the flags on every fixture), the Makefile (deps).

### 10.164 M2.77 — the RV64 execution-cost bench, the timing dimension (as built)

The M2.76 follow-up: bench_exec.c's model now has its RV64 counterpart.
bench_exec_rv64.c mirrors it exactly but drives rv64_exec.c — every
fixture is translated once (translate cost is bench-corpus's job), then
executed N=500 times through the same purpose-built RV64 decoder/executor
the four-way parity uses, with the same mock host functions and the
Expected result parsed from the .simi comment; skips mirror
run_riscv_tests.sh exactly (mem_ops, jmpr_oob, cap_forge_debug).

The per-row guard uses the SAME single source of truth as the parity
tripwire: bench_baselines_rv64.h gained the ns_per_call column (the
struct's steps column is the table the simi-riscv-verify --steps check
already reads), so the bench's steps assertions and the harness's are
one table, one truth. Two tiers, sized from measurement:

- **steps: EXACT** — the deterministic, machine-independent execution
  work per fixture. Cross-validated: the bench's three measurement
  passes produced EXACTLY the committed steps the M2.76 parity table
  carries (all 96 rows), independent confirmation that the harness and
  the bench agree on the execution path.
- **ns/call: asserted at 4.0x** over the committed median of three N=500
  passes. Measured tail: three passes swung up to **2.37x per fixture**
  (worst of 96; 7 fixtures over 2.0x) — the same WSL load profile
  bench_exec documented on the A64 side (2.28x), so the same 4.0x
  margin applies. The printed table shows measured vs committed.

Corpus totals: **96 fixtures, 32820 RV64 steps** across one run of
every fixture (aggregate_abi=1110 and jmpr_chain34=1083 the heaviest;
RV64 executes fewer steps than A64 — 32820 vs 35032 — because the
RV64 codegen uses a different instruction mix). In `all` with the
other benches (~1-2 s). Re-measure deliberately with
RV64_BENCH_MEASURE=1 ./bench-exec-rv64 <tests/ fixtures> 500.

### 10.165 M2.77 gate results (measured)

bench-exec-rv64 (N=500, this sandbox): **99 fixtures, 32820 steps,
~8.8 µs/step aggregate, 3 skipped, 0 failed — ALL CHECKS PASSED**, all
96 rows within their committed baselines (teeth: corrupting add.simi's
committed ns 5321 -> 100 fails the row "4841 ns/call > committed 100 x
4 (execution cost regression)", reverted to PASS). Emission untouched
(no simi_arm.c change — rv64_exec/rv64_exec.h steps came in M2.76), so
the size gate stays **96/96, 46224 saved, all rows byte-identical**;
four-way parity interp 97/97, x86/RV64/ARM 96/0/3; bench-net,
bench-corpus and bench-exec PASS; enc-check clean; stress all-green.
Changed: bench_exec_rv64.c (new), bench_baselines_rv64.h (the
ns_per_call column), the Makefile (bench-exec-rv64 target in
`all`/`clean`) and .gitignore.

### 10.166 M2.78 — the cross-ISA execution-work report, A64 vs RV64 pinned (as built)

The measurement series' derived view: cross_isa.c is a small tool over
the two COMMITTED step tables — bench_baselines.h (A64) and
bench_baselines_rv64.h — whose counts are already pinned EXACTLY by the
parity tripwires (simi-arm-verify --steps, simi-riscv-verify --steps).
It does no execution and no timing of its own: pure header computation,
deterministic and instant, in `all` so the two tables stay honest on
every build. It prints the per-fixture A64-vs-RV64 executed-instruction
ratio table sorted by ratio (so the codegen-density extremes are
visible), the totals and the aggregate ratio, and asserts two gates a
single-ISA check cannot see:

- **No orphan rows** — every fixture must have a committed count in
  BOTH tables. A row in one table missing from the other (or a
  fixture-set mismatch) FAILS: adding a fixture to one ISA's baselines
  and not the other's is a deliberate-update error.
- **The aggregate ratio within a +/-25% band** around the measured
  value. A codegen change that rebalances the two ISAs' instruction
  mixes (a fold that saves A64 steps but not RV64 steps, or vice
  versa) moves it; the band tolerates deliberate divergence while
  failing gross drift.

**The pinned number (measured 2026-08-09): aggregate ratio 1.0674** —
A64 executes 35032 instructions across the corpus vs RV64's 32820, so
the A64 codegen is ~6.7% instruction-heavier overall. The per-fixture
extremes: mem_pre.simi is the most RV64-heavy (0.874 — the A64
pre-indexed address fold is one instruction where RV64 needs a longer
address computation) and jmpr_chain37.simi the most A64-heavy (1.486 —
the M2.x chain/fold machinery's per-dispatch A64 instruction count is
higher); the jmpr_chain* fixtures cluster at the A64-heavy end.

### 10.167 M2.78 gate results (measured)

cross-isa: **96 paired fixtures, aggregate ratio 1.0674 within the
committed band — ALL CHECKS PASSED**. Teeth: commenting out add.simi's
A64 row fails "orphan RV64 row: add.simi has no A64 baseline" plus the
fixture-set mismatch; moving the committed ratio constant to 1.5 fails
"aggregate ratio 1.0674 outside committed band [1.1250, 1.8750]"; both
reverted to PASS. Emission untouched (no simi_arm.c change), so the
size gate stays **96/96, 46224 saved, all rows byte-identical**;
four-way parity interp 97/97, x86/RV64/ARM 96/0/3; all five `all`
checks (bench-net, bench-corpus, bench-exec, bench-exec-rv64,
cross-isa) PASS. Changed: cross_isa.c (new), the Makefile (cross-isa
target in `all`/`clean`) and .gitignore.

### 10.168 M2.79 — the cross-ISA emitted-size report, A64 vs x86 density pinned (as built)

The code-density dimension the M2.78 step report misses: cross_size.c
compares the two ISAs' EMITTED sizes per fixture. The two sides are
honestly asymmetric, exactly matching where their truth lives:

- **A64 — measured live.** The size gate (size_gate_arm.sh) measures the
  CURRENT emitted bytes (its "M1") live every run — there is no
  committed current-A64 table, because every codegen change moves it. So
  cross_size translates each fixture with the real simi_arm.c and reads
  out_len (translate-only — no execution, the bench-corpus pattern; the
  same measurement the size gate gates on with M1 <= M0).
- **x86 — from the committed --bytes table.** The CURRENT x86 emitted
  bytes are committed in bench_baselines_x86.h (the simi-jit-test
  --bytes tripwire's table), so the report reads them from there.

Skips mirror the parity runners exactly (mem_ops, jmpr_oob,
cap_forge_debug — no committed x86 row). Output: the per-fixture
a64/x86 emitted-byte ratio table sorted by density, the totals and the
aggregate ratio. Gates, mirroring cross_isa: **no orphans** (every
committed x86 row must be measured on the A64 side; a fixture without an
x86 baseline fails) and the **aggregate ratio within +/-25%** of the
measured value (code-size mix drift — a fold that shrinks one ISA's
emission without the other — fails).

**The pinned number (measured 2026-08-09): aggregate ratio 0.9092** —
A64 M1 emits 207808 bytes across the corpus vs x86's 228549, so the
folding-heavy A64 backend is ~9% smaller overall. The per-fixture
extremes are the mirror image of the step report's: the jmpr_chain*
fixtures are the most A64-lean (jmpr_chain32 0.502 — the M2.x chain
emission is a fraction of x86's per-candidate mov+compare words), while
the tiny fixtures are x86-lean (rv64_boot_smoke 2.388, add 2.172 —
x86's compact register forms beat A64's fixed 4-byte words once the
trampoline/entry overhead dominates).

### 10.169 M2.79 gate results (measured)

cross-size: **96 paired fixtures (3 skipped), aggregate ratio 0.9092
within the committed band — ALL CHECKS PASSED**. Teeth: commenting out
add.simi's x86 row fails "no committed x86 baseline"; moving the
committed ratio constant to 1.3 fails "aggregate ratio 0.9092 outside
committed band [0.9750, 1.6250]"; both reverted to PASS. Emission
untouched (no simi_arm.c change — cross_size only READS the emission),
so the size gate stays **96/96, 46224 saved, all rows byte-identical**;
four-way parity interp 97/97, x86/RV64/ARM 96/0/3; all six `all`
checks (bench-net, bench-corpus, bench-exec, bench-exec-rv64,
cross-isa, cross-size) PASS. Changed: cross_size.c (new), the Makefile
(cross-size target in `all`/`clean`) and .gitignore.

### 10.170 M2.80 — the interpreter leg gets its execution-cost gate (design)

M2.79 left the four-way measurement story one leg short: A64 and RV64
have execution-cost benches (bench-exec, bench-exec-rv64), x86 has its
emitted-byte gate, but the reference interpreter — the parity GROUND
TRUTH every other leg is checked against — had no committed cost or
work number of its own. M2.80 closes that with `bench-exec-interp`, the
same translate-once + run-N-times model as the other two benches.

The interpreter is a Phase-1 standalone program (the run loop and its
static frame/memory state lived inline in `simi_interp.c`'s `main()`,
with no library API — unlike a64_exec/rv64_exec). M2.80 extracts the
loop into a reusable entry point, behavior-identical:

- `simi_interp_run(SimiObject obj, entry_pc, &steps, &result)` — the
  exact loop simi-run used, moved verbatim into a non-static function
  (declared in new `simi_interp.h`); it resets the frame stack AND
  zeroes `mem` on entry, so repeated calls are independent and
  deterministic (a no-op change for the standalone, which always
  started from a fresh process). `main()` became a thin wrapper that
  parses args, loads the object, calls the run function, and prints
  `result`. `main` is guarded by `SIMI_INTERP_NO_MAIN` so the bench
  can link `simi_interp.c` without a main conflict. Guard: the interp
  parity stays 97/97 (verified).
- `bench_exec_interp.c` — reads each .tmo once with `simi_obj_read`
  (the "translate once" analog — the interpreter doesn't translate),
  then executes it N=500 times through `simi_interp_run`, asserting
  the expected r0 and step-count determinism every iteration, and
  timing ns/call. Fixture set is run_tests.sh's: every fixture with an
  "Expected result:" comment — INCLUDING mem_ops.simi (the address-0
  pointer is an interpreter-only convenience; the translated legs use
  mem_ops_native instead), so this table carries **97 rows** to the
  translated legs' 96. jmpr_oob (faults by design) and cap_forge_debug
  (no expected result) are skipped, exactly as the runner skips them.
- `bench_baselines_interp.h` — the committed per-row table (steps
  EXACT + ns/call at a 4x margin), the same two-tier model and
  discipline as the other benches (a fixture with no row FAILS loudly;
  re-measure deliberately with `SIMI_BENCH_MEASURE=1`).

The steps are the SIMI-instruction count the interpreter executes —
the raw SIMI stream, no translation — so this is the GROUND-TRUTH
work number the translated ISAs' counts are compared against by the
cross-ISA reports, and it is now itself a committed, asserted figure.

### 10.171 M2.80 gate results (measured)

**ALL CHECKS PASSED — 97 fixture rows within their committed
baselines: 2595 SIMI steps across one run of the whole corpus (2
skipped, 0 failed)** — the interp bench's first committed run. Noise
analysis over three N=500 measure passes: steps identical across all
three (determinism confirmed), ns/call tail **2.08x per fixture** (1/97
over 2.0x, 0/97 over 3.0x; median 1.07x) — the same WSL profile as the
A64/RV64 sides (2.28x/2.37x), so the 4.0x margin is comfortably sized.
The headline number is the expansion factor: the interpreter executes
**2595 SIMI instructions** where the translated code executes **35032
A64 / 32820 RV64** (~13.5x / ~12.7x) — the ISA-level work the
translators emit per SIMI op (fetch/cache machinery, chains, frame
prologues), now pinned as a committed quantity. Teeth: corrupting
add.simi's committed steps 6→99 fails "steps=6 != committed 99
(interpreter/emission work changed)"; corrupting its ns 3218→100
fails "3498 ns/call > committed 100 x 4 (execution cost regression)";
both reverted to PASS. Emission untouched (no simi_arm.c change — the
bench only READS the emission via the .tmo), so the size gate stays
**96/96, 46224 saved, all rows byte-identical**; four-way parity interp
97/97, x86/RV64/ARM 96/0/3; all **seven** `all` checks (bench-net,
bench-corpus, bench-exec, bench-exec-rv64, bench-exec-interp,
cross-isa, cross-size) PASS; enc-check clean; stress all-green.
Changed: simi_interp.c/simi_interp.h (the simi_interp_run extraction),
bench_exec_interp.c (new), bench_baselines_interp.h (new, 97 rows),
the Makefile (bench-exec-interp target in `all`/`clean`) and
.gitignore.

### 10.172 M2.81 — the interp leg gets its parity-suite tripwire (design)

M2.80 gave the interpreter leg its cost gate (bench-exec-interp) but,
unlike the translated legs, it had no tripwire in the PARITY SUITE
itself: M2.75/M2.76 wired `--steps`/`--bytes` into the ARM/RV64/x86
runners, so a decode or emission regression fails the fixture at the
earliest point in the pipeline — the interpreter runner (run_tests.sh)
still only checked the result value. M2.81 closes that with a `--steps`
flag on simi-run, the exact M2.75/M2.76 mirror:

- `simi-run [--steps] program.tmo entry_name` — with the flag, after a
  successful run (top-level RET) the executed SIMI-instruction count
  must equal the committed count in `bench_baselines_interp.h` (the
  same single source of truth bench-exec-interp asserts; the header is
  included only in the non-bench build, so the bench TU stays
  warning-free). The fixture name is derived from the .tmo path
  (basename, `.tmo` -> `.simi` — the runner's rule); a fixture with no
  committed row FAILS loudly (exit 1), the same deliberate-update
  discipline as every other table. The check prints `--steps: ok (N)`
  to stderr (the per-fixture .run.log) and exits 1 with
  `--steps: executed N instructions != committed M (...)` on any
  mismatch. Without the flag the standalone behaves exactly as before.
- `run_tests.sh` now invokes `simi-run --steps` on every fixture, so
  all 97 runner fixtures (the same set as the bench table) assert their
  committed count during the normal `make test` parity run. The result
  comparison is untouched — the check is stderr-only.
- Makefile: `simi-run` gains `bench_baselines_interp.h` as a
  dependency, so the tripwire table is always current with the build
  (the M2.27 stale-binary lesson).

### 10.173 M2.81 gate results (measured)

**Interp parity 97 passed, 0 failed — every fixture's .run.log shows
`--steps: ok (N)` (97/97)**, proving each committed count matches the
harness's own execution exactly. Teeth: corrupting add.simi's committed
6->99 fails the row "FAIL add (interpreter exited 1)" with the log
"--steps: executed 6 instructions != committed 99 (interpreter/decode/
emission regression)"; reverted to PASS (97/97, steps-ok 97/97). One
implementation bug caught in testing: the `.tmo`->`.simi` name swap
initially copied 5 bytes (missing the NUL), leaving a garbage byte on
uninitialized stack — 11 fixtures failed nondeterministically on the
lookup; fixed by copying 6 bytes, then deterministic 97/97. Emission
untouched (no simi_arm.c change), so the size gate stays **96/96,
46224 saved, all rows byte-identical**; four-way parity interp 97/97,
x86/RV64/ARM 96/0/3; all seven `all` checks PASS (bench-exec-interp
re-verified against the modified simi_interp.c). Changed:
simi_interp.c (the --steps check + guarded header include),
run_tests.sh (--steps on every fixture), the Makefile (simi-run dep),
plan doc §10.172/10.173.

### 10.174 M2.82 — the interpreter step count joins the cross-ISA report (design)

M2.80/M2.81 made the interpreter's SIMI-instruction counts a committed,
tripwire-pinned quantity (bench_baselines_interp.h, asserted EXACTLY by
the simi-run --steps check in the parity suite), so the cross-ISA
execution-work report gains its third axis: cross_isa.c now reads all
THREE committed step tables — interp (ground truth), A64, RV64 — and
reports, per fixture, the ISA/interp EXPANSION factors: how many
translated instructions one SIMI instruction costs on each ISA. The
existing A64-vs-RV64 section and its 1.0674 gate are untouched; the new
section adds:

- the per-fixture expansion table (interp / a64 / rv64 counts plus
  a64/interp and rv64/interp ratios, sorted by expansion so the
extremes are visible);
- the aggregate expansion factors with their own +/-25% committed
  bands (EXP_A64_MEASURED / EXP_RV64_MEASURED);
- the interp orphan gates: every A64 and RV64 row must have an interp
  row, and every interp row must be in the shared set EXCEPT
  mem_ops.simi — the deliberate interpreter-only fixture (address-0
  pointer; the translated legs use mem_ops_native). mem_ops's 10 steps
  are reported but EXCLUDED from the expansion denominators, so the
  factors are a fair shared-96 comparison.

The tool is still pure header computation — no execution, instant, in
`all` — and now keeps all three tables honest against each other on
every build.

### 10.175 M2.82 gate results (measured)

**ALL CHECKS PASSED — 96 paired fixtures, aggregate ratio 1.0674,
expansion a64/interp 13.5520 rv64/interp 12.6963 within their committed
bands.** The pinned expansion numbers: one SIMI instruction costs
**13.55 A64 / 12.70 RV64 instructions** on average across the corpus
(35032 / 2585 and 32820 / 2585 over the shared 96) — the ~13x
M2.80 headline, now a committed, band-gated quantity. Per-fixture
range: rv64_boot_smoke 77.3x (3 SIMI steps -> 232 A64 — the fixed
entry/exit machinery dominates the tiniest fixtures) down to
jmpr_chain34 5.09x (the loop-heavy fixture amortizes it), with the
call/ret and jmpr_callret* cluster at ~40-60x. Teeth: removing
add.simi's interp row fails "orphan A64/RV64 row: add.simi has no
interpreter baseline" plus both fixture-set mismatches; moving
EXP_A64_MEASURED to 10.0 fails "a64/interp expansion 13.5520 outside
committed band [7.5000, 12.5000] around 10.0000 (ISA expansion
drift)"; both reverted to PASS. Emission untouched (no simi_arm.c
change), so the size gate stays **96/96, 46224 saved, all rows
byte-identical**; four-way parity interp 97/97, x86/RV64/ARM 96/0/3;
all seven `all` checks PASS. Changed: cross_isa.c (the third axis),
the Makefile (cross-isa dep on bench_baselines_interp.h + comment),
plan doc §10.174/10.175.

### 10.176 M2.83 — the long-standing repo cleanup (design)

Two categories of files were tracked in the repository that should
never have been:

- **`.freebuff/` desktop app state** — `desktop-v2.db` (plus `-shm` /
  `-wal`) is the Freebuff desktop app's live SQLite database, churned
  on every session; `.freebuff/worktrees/<id>` is the app's per-worktree
  bookkeeping. Because they were tracked, `git status` showed constant
  churn and merges occasionally tripped over them (the M2.80 merge's
  transient "unstable object source data" stash failure was a race with
  the app writing them). They are removed from the index (kept on disk
  — the running app needs them) and `.freebuff/` is added to the root
  `.gitignore`, so the app state can never be re-added.
- **`tools/simi` build outputs** — five committed binaries (`simi-asm`,
  `simi-run`, `simi-dis`, `simi-jit-test`, `simi-riscv-verify`) were
  tracked; they went stale whenever the sources moved (the recurring
  "stale-toolchain quirk": a fresh checkout's tracked binaries predate
  the atomics/float work, so `make test` failed until a forced rebuild).
  They are removed from the index (kept on disk) — the directory's
  `.gitignore` already listed every build output, so untracking makes
  the ignore real.

The fresh-checkout contract is now honest: a new clone has NO binaries
and NO `.tmo` files, and needs one build before anything runs. That
step is documented in the `tools/simi/Makefile` top comment (`make`, or
`make clean && make` for a guaranteed-fresh build, then `make test`).

### 10.177 M2.83 cleanup result (verified)

`git ls-files` confirms zero tracked files in either category —
`.freebuff/` (4 files) and the `tools/simi` binaries (5 files) are all
untracked and ignored; the working-tree copies are intact (kept via
`git rm --cached`). No source, test, or measurement file changed, so
the size gate's 96/96, 46224-saved, byte-identical state and the
four-way parity (interp 97/97, x86/RV64/ARM 96/0/3) are untouched by
construction. Changed: root `.gitignore` (`.freebuff/` entry),
`tools/simi/Makefile` (fresh-checkout note), the index removals, and
this record.

### 10.178 M3 — the REAL AArch64 execution proof lands (qemu-aarch64 leg)

§6's M3 was the milestone the whole M-series deferred on the honest
premise that no AArch64 toolchain or QEMU existed in this environment
("optional, environment-dependent"). M3 is now CLOSED on the execution
half via `qemu-aarch64` **user-mode** — the strongest substitute for
real ARM hardware available on an x86 host, and the same class of proof
the x86 leg has had since Phase 3 (`simi_jit_test.c` executes real
x86-64 on the host CPU) and the RV64 leg has never had. The M3 design
reuses the x86 leg's shape wholesale: `tools/simi/simi_arm_jit.c`
translates each corpus fixture with the EXACT `simi_arm.c`, mmaps the
emitted words, flips the page to executable (W^X), and branches into
the trampoline for real — no decoder in the loop. The result register
is read out of x9 (t0, OP_RET's convention) by a three-instruction
naked stub (`blr x0; mov x0, x9; ret`); SIGSEGV/SIGILL/SIGBUS are
caught and reported as execution faults. The first full-corpus run was
**84 passed / 12 failed** — and the failures were exactly the
agreement risk §7 predicted: TWO bugs that a64_exec and simi_arm.c
shared, invisible to four-way parity, found only by real execution
(§10.179 records both fixes). The Makefile's `simi-arm-jit` target
cross-compiles the harness statically
(`aarch64-linux-gnu-gcc -static`, deliberately not in `all` — the
sandbox has no toolchain), `tools/simi/tests/run_arm64_tests.sh` loops
the same parity set as `run_arm_tests.sh` (same skip rules) and runs
every fixture under qemu-aarch64, skipping cleanly when the toolchain
is absent, and the `arm64-guards` CI job installs
`gcc-aarch64-linux-gnu` + `qemu-user`, builds, and requires all-pass.
What M3 closes: the two decoder-blind residual risks §7 documented —
(a) an encoder and decoder that AGREE but are both wrong pass four-way
parity invisibly (now execution-visible, and it happened), and (b) A64
architectural rules a64_exec is laxer about (SP 16-byte alignment,
4-byte branch alignment) are now enforced by the real A64
implementation.
Deliberately NOT done: the arm64 *kernel* target half of M3's
definition — no arm64 kernel build exists, and §6 keeps that honest.

### 10.179 M3's paydirt — the two agreeing-but-wrong bugs real A64 caught

The first M3 corpus run under qemu-aarch64 failed 12 of 96 fixtures:
all 12 dynamic-JMPR (table-dispatch) programs faulted, and float_ops
returned 16 instead of 15. Both were encoder/decoder pairs that agreed
with each other — the exact residual risk §7(a) — and both were
empirically pinned against real A64 before fixing.

**(1) The JMPR table was guest-relative.** The dynamic dispatch loaded
its table base with `movz`+`movk` of a bare *byte offset into out_buf*
and `br`'d to it — correct only under a64_exec's guest convention
(addresses are byte offsets), where a `br` to 0xNNN lands inside the
buffer. On real A64 that offset is an unmapped address and every
naive-mode fixture faulted (the same "identical trap" RV64's auipc
port avoided — the A64 port never hit it because a64_exec hid it). The
fix keeps the same 5-word dispatch and the same 4-byte table, so NO
fixture's size moves: the dispatch now loads the base PC-RELATIVELY
(`adr x11, #table_off − pos`, patched post-emit) and the table entries
become SIGNED RELATIVE offsets — `(g_instr_off[pc] − jmpr_table_off)`,
loaded with `ldrsw` (always negative: the table sits after the code) —
so `base + rel` resolves to the absolute target in BOTH conventions
(a64_exec's PC is a byte offset; real A64's is an address; `adr` is
PC-relative in both). a64_exec gained its first `adr` decode
(`0 00 10000 immlo immhi Rd`, 21-bit signed byte offset). The pre-M3
`patch_li32` (movz+movk, 2 words) became `patch_adr` (1 word); the
±1MB adr range is structurally safe under the 256 KiB CODE_CAP.

**(2) The FCMP unordered model was wrong, and the cset mappings were
tuned to it.** a64_exec modeled unordered FCMP as N=1, Z=0, C=1, V=1,
and F1's float-CMP codegen (EQ/NE/LT/LE direct, GT/GE via swapped
operands + cset lt/le) was derived from it. An empirical probe on real
A64 (fcmp_probe4.c, `mrs nzcv`, bits 31:28) pinned the truth: **NZCV
= N=0, Z=0, C=1, V=1** — so EQ/NE/GT/GE are IEEE-correct with a
single cset eq/ne/gt/ge and NO swap, while LT and LE are the classic
A64 NaN gotcha: naive cset lt/le read N!=V / Z||N!=V, both TRUE on
unordered (N=0, V=1), so they return 1 for NaN. F1 now emits **cset mi
(N)** for LT and **cset ls (!C || Z)** for LE, both 0 on unordered;
a64_exec's `fcmp_flags` was corrected to the real model; and
a64_f0_test.c's pins were rewritten to the exact sequences F1 emits
(the old pins asserted the wrong model — they passed because encoder
and decoder agreed). float_ops returns **15 on real A64**, matching
interp/x86/RV64 exactly.

Both fixes are size-neutral (the dispatch stays 5 words; fcmp+cset is
one pair either way), so the size gate's committed rows did not move,
and a64_exec's `adr` + the corrected flags keep the four-way parity
green. The corrected corpus is **96 passed / 0 failed / 3 skipped on
real A64** — the full chain series, the memory folds, obj_ops' real
hostfn `blr` calls, and stress_atomics' real ldaxr/stlxr under
contention all execute correctly on actual AArch64, and jmpr_oob still
faults at the bounds-check UDF in both legs. The two bugs are the
fulfillment of §6's promise that M3 would close "the one documented
residual-risk class the whole M2 series has been building against".

### 10.180 M2's kernel copy lands as M3's kernel half — staged, not wired

M2's milestone text defined the kernel copy (`kernel/simi_arm.{c,h}`,
byte-identical below a shortened header, compiled freestanding with zero
warnings) but it was never executed — the M2 series ballooned into the
size work instead. M3's kernel half picks it up, and it lands STRONGER
than the plan's original gate: the host copy it is byte-identical to is
now proven by real A64 execution (§10.178/10.179), and the freestanding
compile is a CI-enforced gate rather than a one-time check.

`kernel/simi_arm.c` (3862 lines) and `kernel/simi_arm.h` (110 lines)
follow the Phase 5 RV64 port convention exactly: a replaced header
carrying the kernel-copy framing and the honest gap note, then the
unmodified body — re-diffed at commit time and confirmed byte-identical
(the .c body below the header equals `tools/simi/simi_arm.c` lines
72-end; the .h body equals `tools/simi/simi_arm.h` line 29-end). One
pre-existing dead function (`e64`, zero call sites) was removed from the
HOST copy as part of this, so the copy compiles with zero warnings —
`aarch64-linux-gnu-gcc -ffreestanding -O2 -Wall -Wextra -ffunction-
sections -fdata-sections -c kernel/simi_arm.c` is clean — and that exact
command is now a step in the arm64-guards CI job (the M2 gate, enforced
on every push; a divergence from the host copy would surface as a
behavior difference under the real-A64 leg in the same job).

The honest boundary, stated in both headers: there is DELIBERATELY no
arm64 kernel build in this tree, so the copy is compiled nowhere and
linked into nothing — unlike `kernel/simi_riscv.c`, which sits in the
RISC-V kernel's `RV_C_SRC` because a RISC-V kernel exists. Wiring the
A64 translator into a live kernel — `kernel/simi_translate_arm.c`-
equivalent glue, an arm64 activation/spawn path, user-mode paging, an
object-catalog/syscall-dispatch/exit-stub story — all awaits an arm64
kernel target the roadmap does not grow. That is the remaining half of
M3, kept honest and undone (updated §6).

### 10.181 The kernel-copy gate grows teeth — and the teeth found a real hole

The arm64-guards CI gate for `kernel/simi_arm.c` is now a two-part check
with a dedicated teeth smoke, mirroring the repo's `tests/*_smoke.sh`
discipline. The smoke (`tools/simi/tests/arm64_kernel_copy_smoke.sh`,
run by the arm64-guards job) deliberately breaks both halves of the gate
on throwaway copies and asserts the gate FAILS each time, plus asserts
the clean copy still passes (so the teeth are meaningful, not trivially
red): an injected `printf` is caught by the undefined-symbol check, an
injected unused variable by the zero-warning check. A gate that has gone
blind fails the job.

Building the smoke exposed a real hole in the copy's no-libc claim: GCC
13 on AArch64 synthesizes `memcpy`/`memset` CALLS from the M2
chain-analysis struct copies (`struct ChainSet` is ~392 bytes after the
M2.64 cap bump; the aggregate-copy path lowers `*dst = *cur` to a
libcall), and no flag (`-fno-builtin`, `-fno-tree-loop-distribute-
patterns`, `-fno-tree-vectorize`, `-Os`) removes them. The RV64 and x86
kernel copies compile to ZERO undefined symbols on their toolchains, so
the ARM copy's honest contract is now stated precisely in its header:
the object's only undefined symbols may be the freestanding {memcpy,
memset} pair — exactly what a real kernel provides (Linux arm64 ships
memcpy.S/memset.S) — and ANY other symbol fails the gate. The gate's
`nm -u` check enforces exactly that: `memcpy`/`memset` are allowed,
everything else (printf, malloc, ...) is a hard failure.

### 10.182 The byte-identity contract becomes a CI tripwire (re-diff on every push)

Before this, kernel/simi_arm.{c,h} identity with the tools copies was
asserted once at commit time and never looked at again: a future edit to
tools/simi/simi_arm.c that forgot to re-derive the kernel copy would
silently diverge the two, and the arm64-guards compile gate would still
pass (it compiles whatever kernel/simi_arm.c happens to be). A new
toolchain-free guard closes that: `tests/arm_kernel_copy_rediff_check.sh`
re-runs the re-diff on every push (it rides the `tests/*_check.sh` glob
in tests/run_checks.sh, so the main CI job runs it — no aarch64
toolchain required). It extracts each pair's BODY from its marker line
(the `#include "simi_arm.h"` / `#ifndef SIMI_ARM_H` anchors) to EOF and
diffs — marker-anchored, not hardcoded line numbers, so header growth
(the kernel .c header has grown twice, §10.180/10.181) never breaks the
extraction. Empty-body extraction is an ABORT (exit 2), not a pass, so a
broken marker can't silently make the diff trivially green.

Teeth: `tests/arm_kernel_copy_rediff_smoke.sh` (rides the
`tests/*_smoke.sh` glob in run_guard_smokes.sh) copies the four files to
a temp dir, mutates one token in the KERNEL copy's body and then in the
HOST copy's body, and asserts the check FAILS each time — plus asserts
the pristine copies still PASS, so the teeth are meaningful. The real
files are never touched; the check's positional-path args exist
precisely to let the smoke point it at the throwaway copies. A check
that has gone blind (broken marker, empty diff) fails here.

### 10.183 The x86 and RV64 kernel-copy guards get the same audit as the ARM copy

The ARM copy audit (§10.181) proved the technique catches real holes: the
freestanding-compile gate saw the synthesized memcpy/memset, and the
no-libc claim became a precise, enforced contract. The other two kernel
translator copies -- kernel/simi_x86.c and kernel/simi_riscv.c -- now get
the same treatment.

The x86 copy: `tests/simi_x86_kernel_check.sh` (toolchain-free -- the
host gcc IS the x86 kernel's toolchain, the kernel-guards job builds with
X86_CC=gcc -- so it rides the `tests/*_check.sh` glob in run_checks.sh
and runs on every push). It compiles the copy with the exact X86_CFLAGS
(-ffreestanding -O2 -Wall -Wextra -mcmodel=small -mno-red-zone -mno-sse
-mno-sse2 -mno-mmx -fno-pie -fno-pic -fno-tree-vectorize
-Wframe-larger-than=16384) and asserts (a) zero warnings and (b) ZERO
undefined symbols. Teeth: `tests/simi_x86_kernel_smoke.sh` (rides the
`tests/*_smoke.sh` glob) injects printf and an unused variable into
throwaway copies and asserts the check fails each time.

The RV64 copy: `tools/simi/tests/simi_riscv_kernel_check.sh` -- needs the
riscv64-unknown-elf toolchain, so it is explicit, not globbed, and runs
in the riscv-guards CI job (the same job that builds `make riscv-elf`).
Same contract: exact RV_CFLAGS (-ffreestanding -O2 -Wall -Wextra
-mcmodel=medany -march=rv64gcv -mabi=lp64d -mno-relax), zero warnings,
ZERO undefined symbols. Teeth: `tools/simi/tests/simi_riscv_kernel_smoke.sh`,
same printf/unused-var injections, same clean-copy sanity tooth.

Both verified first: each copy compiles clean under its kernel's exact
flags with zero warnings and zero undefined symbols -- so the contract
for x86 and RV64 is strictly EMPTY, unlike the ARM copy's {memcpy,
memset} allowance (the AArch64/GCC13 aggregate-copy synthesis is the only
reason that pair is permitted at all). On both toolchains the M2 chain
struct copies lower to plain inline copies, so no libcalls are
synthesized and the -nostdlib link contract is literally no symbols.

One pre-existing CI regression surfaced and was fixed in the same pass:
tests/makefile_sources_check.sh flags every kernel/*.c not in X86_C_SRC
(its documented job), and kernel/simi_arm.c — added by the M3 kernel
half (§10.180) but deliberately linked into no image, since no arm64
kernel build exists — tripped it, so the guard had been failing since
that merge. It now carries the same documented-exclusion treatment as
kernel/kernel_riscv.c ("RISC-V port, not in the x86 image"):
"AArch64 translator staging copy — no arm64 kernel build exists to link
it; its honesty is enforced by the byte-identity re-diff +
freestanding-compile gates, not by the x86 image". The exclusion list is
the only place a "we meant to leave that out" can hide, and it has to
say why — this one points at the two gates that actually enforce the
copy's contract.

### 10.184 The kernel-copy coverage matrix (auditable at a glance)

The kernel-translator-copy discipline now spans three copies (ARM
§10.180/10.181, x86 and RV64 §10.183) and several gates. The guarantees
are scattered across records, so here they are as a single matrix: for
each kernel copy and each enforcement dimension, what exists and where.
A row is CI-enforced when the named check/smoke runs on every push or in
a toolchain job; "commit-time only" means the property was asserted when
the copy was made and has no automated re-check.

| dimension | kernel/simi_arm.c (host twin tools/simi/simi_arm.c) | kernel/simi_x86.c (host twin tools/simi/simi_x86.c) | kernel/simi_riscv.c (host twin tools/simi/simi_riscv.c) |
|---|---|---|---|
| byte-identity re-diff tripwire | CHECK tests/arm_kernel_copy_rediff_check.sh + smoke tests/arm_kernel_copy_rediff_smoke.sh (§10.182) -- marker-anchored body diff of BOTH .c and .h pairs, toolchain-free, runs every push via the tests/*_check.sh + *_smoke.sh globs | CHECK tests/x86_kernel_copy_rediff_check.sh + smoke tests/x86_kernel_copy_rediff_smoke.sh (§10.185) -- same marker-anchored mechanism, same globs | CHECK tests/riscv_kernel_copy_rediff_check.sh + smoke tests/riscv_kernel_copy_rediff_smoke.sh (§10.185) -- same mechanism; its FIRST run caught and re-derived a real divergence (the F4-era stale kernel .h) |
| freestanding compile gate (zero warnings) | CHECK arm64-guards CI job: aarch64-linux-gnu-gcc -ffreestanding -O2 -Wall -Wextra -ffunction-sections -fdata-sections -c kernel/simi_arm.c (§10.180) | CHECK tests/simi_x86_kernel_check.sh -- exact X86_CFLAGS mirror, host gcc, runs every push via the glob (§10.183) | CHECK tools/simi/tests/simi_riscv_kernel_check.sh -- exact RV_CFLAGS mirror (rv64gcv/lp64d), explicit step in the riscv-guards CI job (§10.183) |
| undefined-symbol gate | CHECK same arm64-guards step: nm -u must show nothing beyond {memcpy, memset} -- GCC 13/AArch64 synthesizes exactly that pair from the M2 chain struct copies (§10.181) | CHECK same simi_x86_kernel_check.sh: ZERO undefined symbols -- the -nostdlib link contract is strictly empty on x86 (§10.183) | CHECK same simi_riscv_kernel_check.sh: ZERO undefined symbols (§10.183) |
| teeth (deliberate-violation smokes) | CHECK arm64_kernel_copy_smoke.sh (printf + unused-var, 3 teeth, arm64-guards job) + the rediff smoke above (§10.181/10.182) | CHECK tests/simi_x86_kernel_smoke.sh (3 teeth, glob-run) (§10.183) + the x86 rediff smoke (kernel-mutation and host-mutation teeth) (§10.185) | CHECK tools/simi/tests/simi_riscv_kernel_smoke.sh (3 teeth, riscv-guards job) (§10.183) + the riscv rediff smoke (§10.185) |
| real-execution leg on the host twin | CHECK M3 qemu-aarch64 corpus: run_arm64_tests.sh translates and EXECUTES every fixture on real A64 (§10.178/10.179) -- the kernel copy is byte-identical to that proven encoder | CHECK native x86 JIT: run_native_tests.sh executes the host twin as real x86 (M2.76 steps/bytes tripwires) | CHECK rv64_exec parity (run_riscv_tests.sh) + the real RV64 kernel boot smoke in the riscv-guards job (the host twin runs as actual machine code) |
| kernel build/link | CHECK kernel/simi_arm.c is in AR_C_SRC -- linked into the arm64 kernel image (M4b, §10.189), with the kernel providing the freestanding {memcpy, memset} pair (§10.181). The matrix's last NONE row is closed | CHECK kernel/simi_x86.c is in X86_C_SRC -- linked into the x86 kernel image | CHECK kernel/simi_riscv.c is in RV_C_SRC -- linked into the RISC-V kernel image |
| matrix self-check | CHECK tests/kernel_copy_matrix_check.sh + smoke tests/kernel_copy_matrix_smoke.sh (§10.186) -- parses THIS table: every script named in a CHECK cell must exist and be wired (the tests/*_check.sh + *_smoke.sh globs, or a ci.yml/Makefile reference), every .c named must be referenced by the build/CI | same mechanism, all three columns | same |

Gap 1 (the byte-identity tripwires for x86 and RV64) is CLOSED (§10.185):
the marker-anchored check + teeth smoke pattern generalized to both
remaining pairs. The tripwire proved itself immediately -- its first run
on the RV64 pair caught a real divergence (the kernel .h had never been
re-derived after F4's float-codegen comment change) and the copy was
re-derived in the same pass. All three pairs now re-diff on every push.

The one remaining open gap:

1. The ARM copy has no kernel build/link (documented honest boundary,
   §10.180) -- the only row that is not a "TODO guard" but a "cannot
   exist yet" gap: it closes when the roadmap grows an arm64 kernel
   target, not by adding a check.

### 10.185 Gap #1 closed: the byte-identity tripwire generalizes to x86 and RV64 — and its first run caught a real divergence

The §10.184 matrix flagged x86 and RV64 as the two pairs without a
byte-identity re-diff tripwire. They now have one, mirroring the ARM
pair (§10.182) exactly: `tests/x86_kernel_copy_rediff_check.sh` and
`tests/riscv_kernel_copy_rediff_check.sh` are marker-anchored body diffs
(the `#include "simi_x86.h"` / `#ifndef SIMI_X86_H` and `#include
"simi_riscv.h"` / `#ifndef SIMI_RISCV_H` anchors), toolchain-free, so
they ride the `tests/*_check.sh` glob in run_checks.sh and run on every
push. Empty-body extraction remains an abort, not a pass. Each has its
own teeth smoke (`tests/x86_kernel_copy_rediff_smoke.sh`,
`tests/riscv_kernel_copy_rediff_smoke.sh`): pristine copies must pass,
a mutated kernel copy must fail, a mutated host copy must fail — 3 teeth
each, all verified green with the real files.

The tripwire proved its worth on the very first run. The RV64 check
failed against the pristine tree: the .c bodies were byte-identical, but
the .h bodies had diverged. The host tools/simi/simi_riscv.h carries the
F4-era comment (float codegen for ADD/SUB/MUL/DIV/NEG/CMP under
T_F32/T_F64 is REAL — the F/D GP-bounce in emit_instr; only float MOD
and FLAG_IMM forms reject with TX_RV_ERR_FLOAT_UNSUPPORTED), while
kernel/simi_riscv.h still carried the pre-F4 Phase 10 v1 claim (float
"scoped OUT... rejected outright"). F4 updated the host header and never
re-derived the kernel copy — exactly the silent divergence this class of
check exists to catch, and one the compile/undefined-symbol gates could
not (both files define TX_RV_ERR_FLOAT_UNSUPPORTED; only the surrounding
comment differs, and no compiler reads comments). The kernel copy's .h
was re-derived in the same pass (kernel-copy header prose preserved and
its stale "float-rejection rationale" pointer corrected, host body
spliced below the #ifndef marker), and the compile gate still passes
clean. The matrix row for RV64 now records this history.

With this, all three kernel translator copies re-diff against their host
twins on every push; the §10.184 matrix's only remaining open gap is
the ARM copy's no-kernel-build boundary (§10.180), which closes with an
arm64 kernel target, not a check.

### 10.186 The §10.184 matrix becomes self-verifying

A doc table goes stale silently: a guard renamed or deleted, a CI
reference dropped, a kernel file renamed — the prose keeps saying CHECK
while nothing enforces it. The matrix now carries its own machine check:
`tests/kernel_copy_matrix_check.sh` parses the §10.184 table and
asserts, for every script named in a CHECK cell, that the script EXISTS
and is WIRED, and for every .c file named anywhere in the table, that
the build/CI references it. It rides the `tests/*_check.sh` glob in
run_checks.sh, so it runs on every push — toolchain-free, pure text.

The wiring rules are exactly what the matrix text claims: `tests/*_check.sh`
scripts must match the run_checks.sh glob (`guards=(tests/*_check.sh)`),
`tests/*_smoke.sh` the run_guard_smokes.sh glob
(`smokes=(tests/*_smoke.sh)`), and `tools/simi/tests/` + bare script
names must be referenced by basename in ci.yml, the root Makefile, or
tools/simi/Makefile (the explicit toolchain jobs, X86_C_SRC/RV_C_SRC,
and the test-native/test-riscv/test-arm targets). .c files must appear
in the root Makefile or ci.yml. The table gained a self-referential
"matrix self-check" row — the check verifies its own existence and
wiring, closing the loop.

Teeth: `tests/kernel_copy_matrix_smoke.sh` (rides the `tests/*_smoke.sh`
glob) breaks each staleness class in throwaway copies of the doc and
ci.yml — a renamed script, a dropped run_arm64_tests.sh CI reference,
a renamed kernel .c — and asserts the check FAILS each time, plus
asserts the pristine copies still PASS. 4 teeth, all verified green
against the real tree.

The first real run already earned the check's keep: it confirmed every
one of the 16 script claims in the matrix as of this record, including
the two runners (run_native_tests.sh, run_riscv_tests.sh) whose wiring
is the tools/simi/Makefile test targets rather than a CI job — the
check's wiring vocabulary had to match that reality, which it now does.

### 10.187 M4 scoped: the minimal bootable arm64 kernel that links simi_arm.c

This is a scoping study, not an implementation — the last open row of the
§10.184 matrix (kernel build/link for the ARM copy) is "NONE" not
because a check is missing but because no arm64 kernel target exists.
The scope below is grounded in the closest precedent the repo already
has: the self-contained RISC-V kernel (kernel/kernel_riscv.c + arch/
riscv/* + Makefile riscv-elf + the riscv-guards CI job) is a qemu `-M
virt` build with an embedded .tmo boot smoke and a clean power-off. An
arm64 kernel is the same shape with different silicon.

#### The three sub-milestones

**M4a — bootable arm64 kernel.** The vertical slice with no SIMI yet:
- `arch/arm64/linker_arm64.ld` — mirror arch/riscv/linker_riscv.ld:
  `OUTPUT_ARCH("aarch64")`, `ENTRY(_start)`, load base 0x40080000 (2 MiB
  into qemu -M virt's 0x40000000 DRAM; the Linux arm64 convention),
  sections .text.init/.text/.rodata/.data/.bss, `_end` symbol.
- `arch/arm64/boot_arm64.S` (~150 lines) — `_start`: set up the stack,
  clear BSS, write VBAR_EL1 (a minimal 16-entry vector table; the smoke
  needs only the sync/panic entries), and handle the exception level
  reality: qemu -M virt with TCG enters the kernel at EL2 (the embedded
  boot path), so the head either stays at EL2 or drops to EL1 like
  Linux — the scope's decision point, documented, not a blocker.
- `arch/arm64/uart_pl011.c` (~80 lines) — the virt PL011 UART at
  0x09000000, polled TX (UARTFR bit 5), putc/puts/print-hex; the analog
  of the RV kernel's 16550 rv_boot_print.
- `kernel/kernel_arm64.c` (~220 lines) — kernel_arm64_main(): banner,
  the PL011 print path, and `psci_system_off()`: qemu -M virt emulates
  PSCI 0.2 through EL3, so `smc #0` with function id 0x84000008 powers
  the machine off and QEMU exits rc=0 — the EXACT analog of the RV
  kernel's SBI_SRST shutdown (Phase 9f), including the rc=0 assertion
  in the CI runner. No -bios needed (unlike RV, the PSCI monitor is
  emulated by the virt machine itself).
- Makefile: `arm64-elf` target — AR_CC=aarch64-linux-gnu-gcc,
  AR_CFLAGS="-ffreestanding -O2 -Wall -Wextra -march=armv8-a
  -mgeneral-regs-only -fno-stack-protector -ffunction-sections
  -fdata-sections" (the -mgeneral-regs-only is the arm64 analog of the
  x86 kernel's -mno-sse: no FP/SIMD anywhere in the kernel),
  AR_LD/AR_LDFLAGS with the linker script, AR_ELF=sls_arm64_kernel.elf.
- **Gate:** qemu-system-aarch64 -M virt -cpu cortex-a53 -m 1G -kernel
  sls_arm64_kernel.elf exits rc=0 with the banner in the serial log.

**M4b — SIMI translate + execute in-kernel.** The milestone the whole
scope exists for:
- Add `kernel/simi_arm.c` to the arm64 build's AR_C_SRC. This is the
  first time the §10.180 copy is LINKED anywhere, and it makes the
  §10.181 contract concrete: GCC 13 synthesizes memcpy/memset calls
  from the M2 chain struct copies, so kernel_arm64.c (or a small
  arch/arm64/mem.S) must PROVIDE the freestanding {memcpy, memset}
  pair — the exact symbols the undefined-symbol gate permits and no
  more. The matrix's ARM compile/undefined-symbol rows stay exactly as
  they are; the kernel simply satisfies them.
- Embed a fixture the way kernel/rv64_boot_smoke_tmo.h does:
  `tools/simi/simi-asm` a tiny .simi (straight_line_bench or a
  dedicated arm64_boot_smoke), convert to a committed
  kernel/arm64_boot_smoke_tmo.h. kernel_arm64.c mirrors
  rv64_boot_smoke_test(): call `simi_arm_translate(tmo, len,
  g_simi_code_buf, cap, entry_name, scratch, 0, 0, 0, &out_len,
  &entry_off)`, then call the translated entry as a real function and
  read the result from x9 (the M3-established return convention).
- **The honest division of labor:** the kernel leg's job is the LINK
  and the call — the encoder itself is already proven by the
  qemu-aarch64 corpus (M3, §10.178/10.179), and the kernel copy is
  byte-identical to that proven encoder (the re-diff tripwire, §10.182).
  A wrong result in-kernel would indict the glue (buffer, entry_off,
  hostfn routing), not the encoder.
- **Gate:** the serial log shows the banner, `[SIMI] entry returned
  (real machine code executed)`, and the expected result matching the
  four-way host value; then PSCI SYSTEM_OFF exits rc=0.

**M4c — the arm64 MMU and the honest remainder (deferred, optional).**
Boot runs with the MMU OFF (physical addressing — legitimate for a boot
smoke; the A64 words simi_arm.c emits are position-independent enough
for direct execution). When the MMU arrives it is VMSAv8-64, nothing
like Sv39: TCR_EL1 (IPS/T0SZ), MAIR_EL1 (device + normal WBWA), TTBR0/
TTBR1, a 4-level 4 KiB walk — ~250-400 lines, its own milestone. After
that come the Phase-9-equivalent honest gaps, verbatim from the RV64
kernel's scope: user-mode paging (per-process address spaces), an
activation cache, the syscall/object-catalog/exit-stub story, GIC
interrupts (the smoke is polled-only), and FP/SIMD context management
for in-kernel T_F32/T_F64 SIMI execution (the kernel itself compiles
-mgeneral-regs-only; the boot smoke uses an integer fixture).

#### The CI story

The arm64-guards job already installs gcc-aarch64-linux-gnu and runs
the real-execution corpus. It gains `binutils-aarch64-linux-gnu
qemu-system-arm`, then two steps mirroring riscv-guards: build
`make arm64-elf`, boot under qemu-system-aarch64, assert rc=0 + banner
(M4a) + the [SIMI] smoke line and expected value (M4b). Nothing about
the kernel-copy gates changes — they are exactly what makes the link
safe to land.

#### Sizing and risk

M4a+b is ~500 lines of new kernel code plus ~55 lines of Makefile/CI
wiring — the same scale as the M0-M3 milestones this plan has shipped.
The riskiest unknown is the exception-level entry dance (EL2 vs EL1)
and the PSCI call under TCG, both of which are one-file, one-boot-loop
problems. The MMU, interrupts, and user paging are honestly deferred,
not hidden: the kernel-copy matrix row flips NONE -> CHECK only when
M4b's gate passes, and this record stays as the scope it was written
against.

### 10.188 M4a as built: sls_arm64_kernel.elf boots and PSCI powers it off

The scope's two riskiest unknowns (§10.187) were real and were pinned
empirically before the milestone could pass its gate. What shipped:

- `arch/arm64/linker_arm64.ld` — ENTRY(_start), load base 0x40080000
  (2 MiB into the virt DRAM), .text.init/.text/.rodata/.data/.bss with
  the 16 KiB boot stack inside BSS (so the entry's BSS clear pre-zeros
  it and the ELF loader maps it), `_end` symbol.
- `arch/arm64/boot_arm64.S` — the head: BSS clear, the EL drop ladder
  (EL3 -> EL1 and EL2 -> EL1), stack, `bl kernel_arm64_main`, panic
  loop. adrp+add throughout, no literal pool, no stack use before
  el1_ready.
- `arch/arm64/uart_pl011.{c,h}` — the virt PL011 at 0x09000000, polled
  TX only, minimal init (QEMU resets it already enabled at 8n1; the
  divisor/line-control registers are a documented M4a scope cut).
- `kernel/kernel_arm64.c` — banner, the `[M4]` exception-level line
  (`mrs CurrentEL`), PSCI SYSTEM_OFF (0x84000008 via `smc #0`), panic
  fallback. Compiled -mgeneral-regs-only, no FP/SIMD, no libc.
- Makefile: `arm64-elf` (AR_CC=aarch64-linux-gnu-gcc, AR_CFLAGS with
  -march=armv8-a -mgeneral-regs-only, AR_LDFLAGS with the linker
  script) + `arm64-run`; added to `all` and `clean` like riscv-elf.
  `makefile_sources_check` gained the kernel/kernel_arm64.c exclusion
  ("AArch64 kernel main — built by make arm64-elf, not the x86 image").
- CI: the arm64-guards job installs qemu-system-arm + binutils, builds
  `make arm64-elf`, boots, and asserts rc=0 + the banner + the EL1 line
  + the PSCI farewell — the riscv-guards SBI_SRST discipline mirrored.

The empirical pins — the part the scope flagged as risky and was right
to flag:

1. **Entry EL depends on the machine config.** Default `-M virt` enters
   the -kernel payload at EL1; `secure=on` enters at EL3; `secure=on
   +virtualization=on` enters at EL3 and the EL3 eret is unreliable in
   TCG. The WORKING configuration is `-machine virt,virtualization=on`
   (secure OFF): the machine's PSCI conduit logic (QEMU 8.2.2
   hw/arm/virt.c: `secure && firmware_loaded` -> DISABLED, `virt` ->
   SMC, else HVC) makes the conduit SMC, and the payload enters at EL2,
   where the head's HCR_EL2.RW=0x80000000 + eret drop lands it at EL1.
2. **SCR_EL3 resets to 0, i.e. secure AArch32 lower ELs.** An EL3 -> EL1
   eret without `SCR_EL3 = NS|RW (0x401)` does not leave EL3 (the
   exception log: "from EL3 to EL3, ELR = the eret target"). With it
   set, the drop works — but under secure=on the subsequent smc is
   delivered to an EMPTY EL3 vector (VBAR_EL3=0, no firmware), not to
   QEMU's PSCI emulation, because arm_is_psci_call only fires when the
   conduit is SMC. Hence virtualization=on, not secure=on.
3. **PSCI SYSTEM_OFF is intercepted by QEMU at exception delivery**
   (target/arm/helper.c arm_cpu_do_interrupt: `arm_is_psci_call` ->
   arm_handle_psci_call when TCG + conduit match) — no firmware
   required. The kernel's `smc #0` with x0=0x84000008 from non-secure
   EL1 powers the machine off and qemu exits rc=0.

Verified: `make arm64-elf` builds clean (the RWX-segment ld warning is
expected — no MMU, so no NX in M4a); three consecutive boots each exit
rc=0 with the banner, `[M4] exception level: EL1`, and the PSCI
farewell in the serial log. M4a's gate is CI-enforced in arm64-guards.
M4b (link kernel/simi_arm.c + the embedded .tmo smoke) is next; M4c
(MMU, interrupts, user paging) remains the honest remainder.

---

### 10.189 M4b as built: kernel/simi_arm.c linked into the arm64 kernel, translating and executing at boot

M4b is the milestone the whole M4 scope exists for: `kernel/simi_arm.c`
— the AArch64 SIMI translator — is finally LINKED into a real kernel
image and its output executed as machine code inside that kernel. The
§10.184 matrix's last NONE row (ARM kernel build/link) flips to CHECK
here.

**What shipped.** `tools/simi/tests/arm64_boot_smoke.simi` (the mirror
of `rv64_boot_smoke.simi`: ENTER #4, LOADI r0 #42, RET — deliberately
trivial, no CALL/memory/RESOLVE, because the encoder is already proven
by the qemu-aarch64 M3 leg; the kernel leg proves the LINK and the
call), assembled to an 80-byte .tmo and embedded as
`kernel/arm64_boot_smoke_tmo.h` (hand-transcribed, then diffed
byte-for-byte against the .tmo — identical). `kernel/kernel_arm64.c`
gains the freestanding `{memcpy, memset}` pair the §10.181 contract
requires (the M2 chain struct copies in simi_arm.c synthesize calls to
them under GCC 13 — the linked ELF has zero undefined symbols), a
4 KiB 16-aligned code buffer, and `arm64_boot_smoke_test()`: translate
the embedded .tmo via `simi_arm_translate()`, then call the entry with
a plain `blr` (the emitted blob is a normal callable A64 subroutine)
and read the result out of x9 (t0 — OP_RET's register, the M3
convention) in inline asm with x9 declared live. The Makefile's
`AR_C_SRC` gains `kernel/simi_arm.c`; the arm64-guards CI boot step
gains a `result=0x000000000000002a` serial-log assertion.

**The first real fix M4b forced — a stack overflow, found by booting.**
The first boot after the link hung: the banner and `[SIMI] translating`
printed, then QEMU looped. `-d int` showed an Undefined Instruction at
ELR=0x0 — the kernel had branched to address 0. Root cause, measured
with `-fstack-usage`: `simi_arm_translate` alone has an **18,800-byte
stack frame** (the M2 chain machinery — chain walk, def pool, chain
emission — is far heavier than `simi_riscv_translate`'s), plus ~1.6 KB
frames in the chain helpers it calls, against a **16 KiB** boot stack
(mirroring the RV64 kernel's — which works there only because the RV64
translator's frame is small). The overflow clobbered the return
address, so the first `ret` landed at 0x0; with no MMU, address 0 is
real memory in qemu virt, the fetch of zeros is an undefined
instruction, and with VBAR_EL1=0 the vector at 0x200 was also zeros —
an infinite exception loop. Fix: the arm64 kernel's stack reservation
goes 16 KiB -> 256 KiB (linker_arm64.ld; BSS is hundreds of KiB, so
the image costs nothing). After the fix: `[SIMI] entry returned (real
machine code executed) -- result=0x000000000000002a (expected 0x2a =
42)`, then PSCI SYSTEM_OFF, qemu rc=0.

**Verified.** Two consecutive boots, each rc=0 with the full log
(banner, EL1, translate OK, result 42, PSCI farewell); zero undefined
symbols in the linked ELF; `makefile_sources_check` (the M4a
exclusion already named the M4b link), the kernel-copy matrix
self-check, and the re-diff tripwires all green. The M4c remainder
(MMU, interrupts, user paging — §10.187) stays deferred; the matrix
row it was the boundary for is now closed.

---


### 10.190 M4c as built: the VMSAv8-64 MMU, on and walking

M4c is the arm64 MMU milestone the scope (§10.187) sized at ~250-400
lines and flagged "nothing like Sv39". It landed as `arch/arm64/mmu.c`
(~250 lines): a genuine 4-level (L0/L1/L2/L3) 4 KiB-granule identity
map, enabled at EL1 BEFORE the UART is touched, so the banner and every
subsequent line in the boot log is translated output.

**Design.** MAIR_EL1 = device-nGnRnE (attr 0) | normal WBWA (attr 1);
TCR_EL1 = T0SZ=16 (48-bit VA) | TG0=4 KiB | IPS=40-bit (safe on both
cortex-a53 and -a57); TTBR0 -> a 20-table tree in BSS (80 KiB, inside
the mapped region): one L0/L1/L2 table, 16 L3 tables identity-mapping
0x40000000..0x42000000 (32 MiB at 4 KiB — the image alone is 21.2 MiB,
the M2 chain arrays make BSS ~21 MiB), plus one L3 table for the UART
device page at 0x09000000. The enable sequence follows the ARM ARM
transition discipline (dsb ish; program MAIR/TCR/TTBR0; isb; tlbi
vmalle1; dsb ish; isb; SCTLR_EL1.M|C|I; isb), and the SIMI code buffer
gets a dc cvau / ic ivau flush before execution (the M3
`__builtin___clear_cache` hygiene, now that I=1). TTBR1 is deliberately
unused — the deferred user-space half.

**Two real bugs, found by booting, both pinned empirically.**
1. **The 1 GiB-boundary L1 index.** The first boot after enabling faulted
   with a level-1 translation fault (ESR 0x86000005, FAR = the isb right
   after `msr sctlr_el1`): 0x40000000 is EXACTLY 1 GiB, so the kernel at
   0x40080000 lives at L1[1] ([1 GiB, 2 GiB)), not L1[0] ([0, 1 GiB) —
   where only the UART lives). L1[1] was empty (zeroed BSS). Fix: both
   L1 entries share one L2 table (L2[0..15] kernel, L2[72] UART).
2. **The walk that lied.** The selfcheck's walk printed L3 descriptors
   with attr=0 everywhere — because DESC_TABLE and DESC_PAGE were BOTH
   defined as 0b11, so the walk's "block vs table" checks folded into
   one condition and the compiler dead-code-eliminated the entire L3
   descent: the "walk" returned L2 table descriptors (whose low bits
   coincidentally read attr 0). The builder was always right — only the
   walk's constants were broken. Fix: distinct encodings (0b11 valid
   table/page, 0b01 block) and explicit block handling at L1/L2.

**Verified.** With the walk honest, the log shows the real descriptors:
`walk(0x09000000) L3=0x0020000009000403 attr=0 pxn=1 (device)` (PA
0x09000000, attr 0, PXN set) and `walk(0x40080000) L3=0x0000000040080407
attr=1 (normal)` (PA 0x40080000, attr 1, AF). Boots rc=0 on BOTH
cortex-a57 (local) and cortex-a53 (CI), deterministic across runs, with
the SIMI boot smoke still returning 42 under the MMU and PSCI powering
off. The arm64-guards CI boot step now asserts the `[M4c] MMU on` line,
`SCTLR_EL1 M=1 C=1 I=1`, both walk lines byte-exact, and the existing
SIMI-result + PSCI lines. The M4 honest remainder (user paging, an
activation cache, the syscall/object-catalog story, GIC interrupts,
FP/SIMD context) stays deferred — M4c's own gate (boot + walk + SIMI +
shutdown under the MMU) is closed.

### 10.191 M5 as built: user-mode paging — a TTBR1 kernel / TTBR0 user split, SIMI in EL0

M5 is the first of the M4 honest-remainder gaps (user paging, §10.190),
taken as its own milestone because it forces the kernel into a shape
the arm64 CPU actually has: kernel code at high VAs (TTBR1) and a
translated SIMI program executing in EL0 from a VA the kernel's own
tables do NOT map (TTBR0). The boot log's final act is now the
milestone's whole point:

```
[M5] eret into EL0...
[M5] returned from EL0 -- user result=0x000000000000002a (expected 0x2a = 42)
[M4] issuing PSCI SYSTEM_OFF                      → qemu rc=0
```

and the exception log contains exactly two exceptions, both intended:
`SVC from EL0 to EL1` (the blob's return trap, ELR 0x10004004 = the
user `svc #0` stub) and `Secure Monitor Call ... handled as PSCI call`
(the SYSTEM_OFF). A third exception anywhere fails the milestone.

**Design.** The kernel relinked to VMA 0xFFFF000040080000 with an LMA
split: `linker_arm64.ld` gives the `.text` an LMA of 0x40080000
(`AT(...)`), so qemu loads the body at its physical address while all
adrp/add references resolve at high VAs; a 64-byte entry stub parks at
0x4007F000 (a free page below the image) and jumps to a fixed physical
head whose `adrp`-relative math survives the MMU-off domain. The head
builds ONE 4-level table tree in BSS, programs MAIR/TCR (T0SZ=16,
T1SZ=16)/TTBR0_EL1+TTBR1_EL1 to the same tree, enables the MMU, and
jumps to its own high-VA alias (the transition that makes the kernel
TTBR1-pure; the boot tables are then reused as the user TTBR0 root).
`mmu.c` gains `mmu_build_user()` — a second tree identity-mapping the
code buffer's physical page (the translated blob, already icache-
flushed) plus a stack page, the `svc #0` stub page, and a scratch page
at user VAs 0x10000000/0x10001000/0x10004000/0x10005000 — with EL0
read/write AP and no UXN on the code page, and the user tree built as
physical descriptors (the walker reads physical memory). The kernel
then writes `svc #0` (0xD4000001) into the stub page, sets SP_EL0 =
user stack top, x30 = stub VA, ELR_EL1 = code VA + entry offset,
SPSR_EL1 = EL0t, switches TTBR0 to the user root, and erets. The
blob's trampoline runs in EL0, `br x30`s into the stub (x9 still
holding the RET result), `svc #0` traps to the vector table's
sync-from-lower-A64 slot, the handler stores x9 to `g_user_result` and
erets to `g_user_ret_addr` — the kernel continuation.

**The continuation bug that cost the most time.** The first version
used a GNU computed-goto label as the continuation
(`g_user_ret_addr = (uint64_t)&&after_eret;`). It failed twice over:
(1) GCC placed the `after_eret` label at the TOP of the inlined
function body, so the handler's eret re-ran the whole EL0 excursion —
the serial log visibly restarted at "[M5] user TTBR0 tree built"; (2)
even at the right address, the EL0 program clobbers the entire register
file, so the continuation ran with garbage in callee-saved registers
and faulted — a Data Abort at ELR uart_puts with FAR 0x4156f000, a
bare PHYSICAL address (the user root's PA) used as a VA. The fix is
structural: the continuation is now a real `noreturn` function
(`arm64_el0_done`), entered via the handler's eret like a hand-called
subroutine — it needs no pre-eret register state and no valid LR
(PSCI powers the machine off before any epilogue), and its prologue
pushes its own frame on the kernel SP (SP_EL1 never changed across the
excursion — the eret into EL0 switched to SP_EL0). The lesson is
recorded in the code comment: computed-goto labels in (possibly
inlined) functions are not a stable eret target.

**Five more real bugs, all found by booting and pinned empirically.**
1. **`--gc-sections` nuked the whole `.text`.** The entry stub's `br
   x0` jump to the physical head carries no relocation, so ld saw no
   reference from `_start` into `.text` and collected everything but
   the stub (objdump: zero instructions at 0x40080000). Fix:
   `KEEP()` the head section.
2. **A double-add overflow in the high-VA jump.** `movk` preserves the
   destination's low bits, so adding the virt offset onto a register
   still holding a physical address carried into bit 32 and produced
   0xFFFF0000_8010C14C — an L1[2] VA, unmapped. Fix: `movz` (zeroing
   the rest) before the add.
3. **`KERNEL_VIRT_OFF` without parentheses.** `p - KERNEL_VIRT_OFF`
   expanded to `p - 0xFFFF000040080000ULL - 0x40080000ULL` — the SUM
   of the two halves, producing a sign-extended garbage descriptor
   (0xFFFFFFFFC146E003) and a level-0 translation fault in the user
   walk. One-line fix, caught only because the selfcheck prints the
   descriptor.
4. **AP at the wrong bit.** `DESC_AP_EL0RW = 1 << 8` put EL0 access in
   the SHAREABILITY field (bit 8); the AP field is bits [7:6], so the
   stack page stayed EL1-only and EL0 took a permission fault. Fix:
   AP=01 at bits [7:6].
5. **The vector table's per-group order.** The first layout wrote the
   four slots per group as SError-first; the hardware order is
   Synchronous, IRQ, FIQ, SError, so the SVC from EL0 entered at
   VBAR+0x400 (a stub) instead of the handler — the table was fixed
   before the continuation bug was chased down.

**Verified.** rc=0 on both cortex-a57 (local) and cortex-a53 (CI),
deterministic across runs; the exception log is exactly the two
intended exceptions; the SIMI smoke still returns 42 on the M4b direct
path and again from EL0; all host gates green. CI (arm64-guards) now
asserts the TTBR1-pure kernel lines, the unmapped-to-kernel/user-walk
pair at 0x10000000, the EL0-executable page, and the EL0 result line.
The M5 honest remainder stays deferred. Per the architecture note
recorded in §6 M5: AeroSLS is Single Level Storage — no glibc, no user
space, everything in kernel space — so the EL0 split is NOT a userland:
it is a fault-containment domain for foreign/untrusted code (SIMI
guest, tenant, downloaded artifact), with the single-level store as the
shared object space beneath it. The default configuration is the
EL1-only M4b direct-call shape; the EL0 excursion is an optional
sandbox. Next items: an activation cache, GIC interrupts, FP/SIMD
context — and, only if a containment use-case lands, a
capability-flavored object-catalog trap for foreign-code domains
(deliberately not a POSIX-style syscall ABI).

### 10.192 M5.1 scope: the activation cache (translate-on-first-use)

M5.1 is the next of the M5 honest-remainder items (§6 M5, §10.191),
scoped as its own milestone in the established "scope before spike"
shape. It ports the x86 kernel's Phase 4 activation cache (ISA doc §11,
`kernel/simi_translate.c`'s `g_activations[]`) to the arm64 kernel: the
System/38 `CREATE PROGRAM` → activation split — translate a `.tmo` to
native A64 once, cache the emitted code, and re-enter it many times
without retranslating.

**Why this is a real (if small) milestone, not a no-op.** The arm64
kernel's two entry paths today already share one buffer: M4b's
`arm64_boot_smoke_test()` translates into `g_smoke_code_buf` and records
`g_code_len`/`g_entry_off`, and M5's `run_user_program()` reuses those
globals when it maps the buffer's physical page into the user TTBR0
tree — it does NOT retranslate. So the reuse exists but is implicit and
unobservable: nothing counts translations, nothing keys on content, and
nothing would stop a future change from silently re-translating on every
entry (the exact regression the x86 Phase 3→4 gap was). M5.1 makes the
property explicit and pinned.

**Design.** A bounded static array of activation slots in
`kernel_arm64.c`, mirroring the x86 shape:

```
struct Arm64Activation {
    char     name[PROC_NAME_LEN];   /* embedded object name ("arm64_boot_smoke") */
    uint32_t content_hash;          /* FNV-1a of the .tmo bytes (x86 keying) */
    uint32_t tmo_len;               /* byte size of the .tmo */
    uint32_t code_len;              /* translated A64 length (for flush + bounds) */
    uint32_t entry_off;             /* entry offset within g_smoke_code_buf */
    uint32_t valid;
};
```

- **MISS** (no slot, or name+hash+size mismatch): run
  `simi_arm_translate()` into `g_smoke_code_buf` (unchanged),
  `arm64_flush_icache()`, record the slot, increment `g_translate_count`,
  print `[SIMI] activation cache MISS (translated N bytes)`.
- **HIT** (name+hash+size all match): skip the translator entirely, take
  `entry_off`/`code_len` from the slot, do NOT re-flush (bytes
  unchanged), do NOT increment the counter, print `[SIMI] activation
  cache HIT (reusing entry_off)`.
- **The register frame is deliberately NOT cached.** The SIMI frame is
  SP-relative (carved off the caller's stack — the M5 finding), so each
  entry gets a fresh frame: the kernel stack for the EL1 direct path, a
  fresh user stack for the EL0 containment path. This is the arm64
  equivalent of the x86 cache's fresh-per-process scratch frame — but
  free, because the arm64 trampoline has no baked per-process address
  (unlike the x86 r7 `movabs` immediate §10 of the ISA doc), so nothing
  in the cached bytes is per-entry. The EL0 path must still build a
  FRESH user tree per excursion (fresh stack/stub/scratch pages — never
  shared, same discipline as x86's private scratch frame); only the code
  page is the shared cached one.
- **Invalidation.** Keyed on FNV-1a of the uploaded bytes + size, not
  just the name (x86 precedent): a re-upload with different bytes is a
  MISS. This kernel has no upload path, so the trigger is argued from
  the x86 kernel's tested keying (ISA §11: deterministic, zero pairwise
  collisions across the corpus, single-byte mutation changes the hash),
  not exercised here — stated plainly, per the project's honest-caveat
  discipline. And because the code region is one fixed 4 KiB static
  buffer, the x86 Phase-4 "re-upload orphans old frames" gap degenerates
  to nothing: there is no growing allocation to leak.

**Re-entry paths (both proven by M4b/M5, now cache-aware).**
1. **EL1 direct (M4b path):** `blr g_smoke_code_buf + entry_off`, result
   in x9 — a plain callable A64 subroutine, re-enterable any number of
   times; the M4b inline-asm call shape is unchanged.
2. **EL0 containment (M5 path):** map the cached buffer's physical page
   (kernel VA minus KERNEL_VIRT_OFF) into a fresh user TTBR0 tree at
   USER_CODE_VA, plus fresh stack/stub/scratch pages; set SP_EL0, x30 =
   stub VA, ELR_EL1 = USER_CODE_VA + entry_off, SPSR_EL1 = EL0t; eret.
   The blob runs, `br x30`s into the stub, `svc #0` traps, the handler
   stores x9 and erets into the noreturn continuation — unchanged from
   M5, with the same two-exception discipline.

**The honest gate (planned).** A translate counter + log lines make the
"once" property observable:
- (1) EL1 direct activation → MISS, translated N bytes, result 42;
- (2) EL1 direct re-entry → HIT, result 42, `g_translate_count`
  unchanged;
- (3) EL0 excursion → HIT (same cached page, fresh tree), result 42 via
  SVC;
- (4) EL0 excursion again → HIT, fresh tree, result 42.
Four entries, one translation, all returning 42. Optionally pin the
buffer's first words' hash unchanged across the HITs (byte-identity
discipline on the cache itself). CI (arm64-guards) asserts the MISS
line, at least one HIT line, the four result lines, and rc=0.

**Sizing and risk.** ~80-120 lines in `kernel_arm64.c` only: the FNV-1a
(borrowed from the x86 keying), the slot find/insert, and the HIT/MISS
branching in the two call paths. The translator core
(`kernel/simi_arm.c`) is untouched — the cache is a caller-side concern,
exactly as x86's `simi_translate.c` wraps rather than modifies
`simi_x86.c`, so the byte-identity re-diff tripwire and the
freestanding-compile gate are unaffected. The riskiest unknowns are
nothing new: the EL0 re-entry path is the M5-proven shape, and the only
new moving part is the counter/keying, both plain C.

### 10.193 M5.1 as built: the activation cache, four entries one translation

M5.1 implements the scope of §10.192: the System/38 `CREATE PROGRAM` →
activation split on the arm64 kernel. The boot gate — four entries
(EL1, EL1, EL0, EL0), ONE translation, all returning 42 — is closed,
and the boot log now makes the property explicit:

```
[SIMI] translating arm64_boot_smoke.tmo with kernel/simi_arm.c...
[SIMI] activation cache MISS (translated 0x…39c bytes)      ← entry 1 (EL1)
[SIMI] entry returned (real machine code executed) -- result=0x…2a
[SIMI] activation cache HIT (reusing entry_off=0x…33c)      ← entry 2 (EL1)
[SIMI] entry returned (real machine code executed) -- result=0x…2a
[SIMI] activation cache HIT (reusing entry_off=0x…33c)      ← entry 3 (EL0)
[M5] eret into EL0...
[M5] returned from EL0 -- user result=0x…2a
[SIMI] activation cache HIT (reusing entry_off=0x…33c)      ← entry 4 (EL0)
[M5] eret into EL0...
[M5] returned from EL0 -- user result=0x…2a
[M4] issuing PSCI SYSTEM_OFF                                 → qemu rc=0
```

**Design as built** (§10.192's shape, no drift): `struct
Arm64Activation` slots (name + FNV-1a `content_hash` + `tmo_len` +
`code_len` + `entry_off`), `g_act[ARM64_ACT_SLOTS]`, and
`g_translate_count`; `arm64_activate()` MISS/HITs through the existing
`g_smoke_code_buf` — on MISS it translates, flushes the I-cache,
records the slot, and bumps the counter; on HIT it reuses
`entry_off`/`len` with no translation, no flush, no bump. The two entry
paths then share the cache: `arm64_el1_entry()` (the M4b `blr` shape,
called twice) and `arm64_el0_activate()` (the M5 eret shape, called
from main and again from the continuation `arm64_el0_done`, which
launches the second EL0 excursion when `g_el0_excursions < 2` and
powers off after the second). Each EL0 excursion builds a FRESH user
tree (fresh stack/stub/scratch pages — only the cached code page is
shared), and the EL0 alias at USER_CODE_VA gets its own dc cvau / ic
ivau flush (a distinct VA from the kernel VA; no-op on qemu TCG, the
honest real-silicon hygiene). The register frame is not cached — it is
SP-relative, carved per entry — exactly as scoped.

**One measurement lesson, recorded because the CI depends on it:** the
kernel's UART lines carry literal `\r\n` TEXT (not carriage-return +
line-feed), so several logical lines share one grep line and `grep -c`
line counts undercount (the M5.1 gate's MISS/HIT/result counts read
1/2/1/2 with `-c` versus the true 1/3/2/2). The CI asserts therefore
use `grep -o | wc -l` occurrence counts — and the pre-M5.1 substring
asserts were immune either way. The literal-`\r\n` emission is
pre-existing (since M4a) and cosmetic; it is left untouched per the
fewest-changes discipline and documented here so no future gate trips
on it.

**Verified.** `make arm64-elf` clean (zero warnings), boots rc=0 on
cortex-a57 (local) and cortex-a53 (CI CPU), deterministic across runs:
1 MISS + 3 HITs, 2 EL1 results (42) + 2 EL0 results (42), and the
exception log is exactly the intended three exceptions — SVC (entry 3's
return trap), SVC (entry 4's return trap), and the PSCI Secure Monitor
Call — with no stray fault. The M5.1 gate is CI-enforced in
arm64-guards: the four occurrence counts + rc=0 + all pre-existing
asserts. The honest caveats from the scope hold as recorded
(single-object today; upload-invalidation argued from the x86-tested
FNV-1a keying, not exercised — no upload path; the frame-leak gap
degenerates to nothing in the fixed 4 KiB buffer). The translator core
was untouched — the re-diff tripwire and freestanding-compile gates are
unaffected.

### 10.194 M5.2 scope: GIC interrupts — the arm64 kernel's first interrupt-driven behavior

M5.2 is the deferred interrupt story from §10.190/§10.191, scoped as its
own milestone in the established "scope before spike" shape. It is the
arm64 analog of the RISC-V kernel's Phase 9k/9n periodic tick — but
with NO firmware: the arm64 kernel programs the timer and the interrupt
controller itself, the exact parallel of how the M-mode RISC-V twin
programmed the CLINT directly. The gate mirrors the RISC-V one exactly:
a periodic tick printed as `[TICK N]`, with `[TICK 2]` as the re-arm
proof (a one-shot timer delivers only `[TICK 1]`), and an unbroken run
with zero missed re-arms as the tripwire.

**Timer.** The ARMv8 generic timer at EL1: write CNTP_TVAL_EL0 and set
CNTP_CTL_EL0.ENABLE (IMASK cleared) to arm; the handler re-arms by
writing CNTP_TVAL_EL0 again before EOIR. The EL1 physical timer is the
right choice for an EL1 kernel (the EL2 virtual/hypervisor timers belong
to virtualization software). qemu TCG models the generic timer; CNTPCT
advances with the VM's virtual time.

**Interrupt routing — the GIC.** On qemu `-M virt` (QEMU 8.2.2), the
interrupt controller is a GIC; the expected config is GICv3
(gic-version=3 is the virt default since QEMU 5.0): distributor MMIO at
0x08000000, redistributor at 0x080A0000, and the CPU interface accessed
via the ICC_*_EL1 system registers (ICC_SRE_EL1 to force the sysreg
interface, ICC_IGRPEN1_EL1 to enable group 1, ICC_IAR1_EL1 to
acknowledge, ICC_EOIR1_EL1 to end). Programming a PPI into group 1
(non-secure) via GICD_IGROUPR0 and enabling it via GICD_ISENABLER0, plus
GICD_CTLR.EnableGrp1. The EL1 physical timer's PPI is 26 per the ARM
SBSA; the exact PPI number and the machine's GIC version are pinned
EMPIRICALLY at implementation time (the spike-before-estimate
discipline — the M4a EL-ladder and the RISC-V SBI_SRST work are the
precedents for such probes being genuinely surprising). If the machine
reports GICv2, the CPU interface is MMIO at 0x08010000 (GICC_CTLR/IAR/
EOIR) instead of sysregs — a contained alternative, decided by the
spike.

**TTBR1-pure integration.** The GIC distributor/redistributor live at
low physical addresses, and the kernel is TTBR1-pure (it never
dereferences a low VA — the M5 discipline). mmu.c therefore gains device
pages for 0x08000000 (distributor) and 0x080A0000 (redistributor) at
high VAs, mirroring the UART device page exactly (MAIR attr 0
device-nGnRnE, PXN). This is the same one-L3-table-per-device pattern
M4c established for the PL011 at 0x09000000.

**Vector table.** boot_arm64.S currently parks every IRQ slot on the
wfi stub. M5.2 replaces the EL1h IRQ slot (VBAR+0x280) with the tick
handler: acknowledge (read ICC_IAR1_EL1), increment a global tick
counter, re-arm the timer, print `[TICK N]` over the UART, end (write
ICC_EOIR1_EL1), eret. The lower-EL IRQ slot (VBAR+0x480) stays a stub:
interrupts are masked during the EL0 excursions (SPSR_EL1 0x3c0 and the
handler's 0x3c5 both set DAIF.I), so an IRQ can never trap from EL0 —
stated honestly rather than silently assuming it.

**Where the ticks fire.** The M5.1 boot flow runs the four entries with
DAIF masked throughout. M5.2 inserts a tick-wait phase after the second
EL0 excursion: the continuation clears DAIF.I (msr daifclr, #2), idles
until the target tick count is reached, then re-masks and issues PSCI
SYSTEM_OFF. The tick stream therefore lands strictly AFTER the last
`returned from EL0` line, so every pre-existing assertion (banner, MMU
walks, 1 MISS / 3 HITs, four 42s, PSCI rc=0) is untouched — the M5.1
gate stays intact by construction, which is the milestone's own
regression guard.

**The honest gate (planned).**
- The log shows `[TICK 1]`, `[TICK 2]`, ... as an unbroken run with
  count == target (no missed re-arms).
- The tripwire asserts `[TICK 2]` explicitly — the re-arm proof
  (mirroring the RISC-V runner's `[TICK 2 Us]`).
- Occurrence-count assertions (the §10.193 `grep -o | wc -l` lesson):
  `[TICK N` occurrences == target, and the M5.1 counts unchanged.
- qemu rc=0 (PSCI still powers off).
- CI (arm64-guards) asserts the tick run + the existing counts + rc=0,
  on cortex-a53.

**Honest caveats.**
- TCG timing is not real-time: the gate asserts the tick COUNT and the
  re-arm, never wall-clock. The period is a knob (e.g., 50-100ms of
  virtual time) tuned so the idle phase reliably delivers the target
  ticks without stretching the boot.
- The PPI number and GIC version are spiked empirically before the
  design is trusted; the spike is a contained, one-file probe (enable
  the timer, print whatever interrupt arrives), exactly how the M4a
  entry-EL and the RV64 SBI_SRST findings were pinned.
- No interrupt nesting: the handler runs with IRQs masked and erets
  back (the RISC-V handler discipline). If a tick ever fires during an
  EL0 excursion it would be masked and delivered on the next unmask —
  but it cannot trap from EL0 by construction (SPSR masks IRQs), and
  the excursions are microseconds of virtual time vs a 50ms+ period.
- The Phase 9n 100ms contention probe (tick stream interleaving with
  the UART with zero lost bytes) is a possible follow-up, not part of
  this gate: the arm64 boot has no concurrent UART traffic to contend
  with yet.

**Sizing.** ~150-200 lines: mmu.c GIC device pages (~20), a new
arch/arm64/gic.c (~90: distributor + sysreg init, acknowledge/EOIR,
the PPI-enable), the timer arm + tick handler + idle phase in
kernel_arm64.c (~50), the vector slot + DAIF clear in boot_arm64.S
(~10), CI asserts (~10). The translator core and the activation cache
are untouched.

### 10.195 M5.2 as built: GIC ticks, the arm64 kernel's first interrupt

M5.2 implements the scope of §10.194: a periodic tick from the ARMv8
EL1 physical generic timer (CNTP) through the qemu virt GIC, handled in
the vector table's EL1h IRQ slot. The boot log's final act is now:

```
[M5.2] waiting for 3 ticks (GICv2, CNTP PPI 14)...
[TICK 1]
[TICK 2]
[TICK 3]
[M5.2] tick gate reached: 3 ticks, unbroken run
[M4] issuing PSCI SYSTEM_OFF          → qemu rc=0
```

with the M5.1 gate (1 MISS / 3 HITs, two EL1 + two EL0 results, all 42)
fully intact, and the exception log exactly the six intended exceptions
(3 IRQ, 2 SVC, 1 PSCI smc) — no strays.

**The spike overrode BOTH scope assumptions — and that is the point of
the discipline.** (1) The machine defaults to **GICv2**, not GICv3:
qemu's `finalize_gic_version` picks v2 whenever v2 emulation is
supported and max_cpus <= 8 (GIC_NCPU), which is the case under TCG, so
the CPU interface is MMIO at 0x08010000 (GICC_CTLR/PMR/IAR/EOIR), not
the ICC_* sysregs. (2) The timer's **INTID is 30, not 14**: qemu's FDT
says `/timer interrupts = <1 14 260>` for the NS EL1 physical timer —
"PPI 14" is the PPI NUMBER, and a PPI's GIC INTID is ppi + 16 (SGIs
0-15, PPIs 16-31), so the enable/group/pending bits sit at bit 30.
Verified from qemu's own dumped device tree (§10.194's spike).

**Three bring-up bugs, all found by booting, all pinned empirically.**
1. **`msr daifclr` mask bits.** The first attempt unmasked with
   `daifclr, #4` — the DAIF immediate encoding is {D=8, A=4, I=2,
   F=1}, so #4 clears the async-abort mask, NOT the IRQ mask. The IRQ
   unmask is `daifclr, #2` (the exact immediate Linux's
   `local_irq_enable` uses). Symptom: the wait phase started, zero
   ticks, boot hung (timeout).
2. **GICC_CTLR bit 0 is EnableGrp0.** The CPU interface was programmed
   with 0x1 (EnableGrp0, the SECURE group) while the timer PPI was in
   group 1 — the interface silently disabled the very interrupt being
   set up. EnableGrp1 is bit 1 = 0x2. Symptom: with the unmask fixed,
   still zero ticks — the exception log showed no IRQ ever taken.
3. **SGI 14 vs PPI 14 (INTID 30).** The distributor was programmed with
   bit 14 — the SGI 14 enable — not the timer. The tell: reading back
   GICD_ISENABLER0 showed 0xffff, which is just the always-enabled
   SGIs (bits 0-15 read as one), so the timer IRQ line never asserted.
   Symptom: a poll loop (temp debug) showed the timer FIRED
   (CNTP_CTL_EL0.ISTATUS=1) but GICC_IAR stayed 1023 — the GIC never
   saw the interrupt. Fix: `GIC_NS_EL1_PHYS_TIMER_INTID 30u`.

**Design as built** (§10.194's shape, minus the overridden pins):
`arch/arm64/gic.c` — GICv2 init (distributor: GICD_CTLR.EnableGrp1,
IGROUPR0/ICPENDR0/ISENABLER0 at bit 30; CPU interface: GICC_CTLR 0x2,
GICC_PMR 0xFF) plus `gic_iar`/`gic_eoir`, and the generic timer
(`arm_timer_init` reads CNTFRQ_EL0, `arm_timer_arm` reloads
CNTP_TVAL_EL0 with a 50 ms period and sets CNTP_CTL_EL0.ENABLE). The GIC
MMIO is reached at high-VA device pages mapped in boot_arm64.S (L2[64]
L3 table: distributor at 0xFFFF000008000000, CPU interface at
0xFFFF000008010000 — the UART device-page pattern, attr 0 + PXN). The
EL1h IRQ slot (VBAR+0x280) is a real entry (`arm64_irq_el1h`) that saves
x0-x17 + x30 (the C handler preserves the callee-saved set per the ABI),
calls `arm64_tick_irq`, restores, and erets. `arm64_tick_irq`
acknowledges (1023 = spurious, nothing to EOIR), re-arms the timer
BEFORE the EOIR (the RISC-V discipline: minimize the window), bumps
`g_tick_count`, prints `[TICK N]`, and ends. After the second EL0
excursion the continuation runs `arm64_wait_ticks(3)`: arm the timer
first (no early tick), unmask IRQs (`daifclr #2`), busy-wait on the
volatile counter, re-mask, print the gate line. The tick phase sits
strictly AFTER the last EL0 result line, so every pre-existing
assertion is untouched by construction — the milestone's own regression
guard. The DAIF-immediate and GICC_CTLR gotchas are recorded in the
code comments so no future edit re-introduces them.

**Verified.** `make arm64-elf` clean, boots rc=0 on cortex-a57 (local)
and cortex-a53 (CI), deterministic across runs: exactly 3 `[TICK N]`
lines (unbroken), `[TICK 2]` present (the re-arm proof), the M5.1
counts unchanged, and the six-exception discipline. CI (arm64-guards)
asserts the 3-tick run + `[TICK 2]` + all existing counts + rc=0. The
honest caveats from the scope hold: the gate asserts the tick COUNT and
the re-arm, never wall-clock (TCG is not real-time); no interrupt
nesting (the handler runs masked); the lower-EL IRQ slot stays a stub
because SPSR masks IRQs during the EL0 excursions. The Phase 9n 100ms
contention probe remains a possible follow-up, not part of this gate.

### 10.196 M5.2 addendum as built: the Phase 9n-style contention probe

M5.2's scoped follow-up — the RISC-V Phase 9n probe mirrored — is now
part of the gate: the 100 ms `[TICK N]` stream INTERLEAVES with the EL0
excursion logs, with zero lost ticks and zero missed re-arms, asserted
by CI as an exact ordered token stream.

**The design.** (1) The embedded boot program (`arm64_boot_smoke.simi`,
still returning 42 — the M4b/M5.1 gates are unchanged) becomes a
1e8-iteration loop; the emitted A64 loop body is ~10 instructions per
SIMI iteration (the block-head frame reload), so one entry is ~1e9 A64
instructions — a ~760 ms EL0 window on the dev host, nearly 8x the
100 ms period (measured directly below). (2) The tick period goes 50 ms
→ 100 ms (`arm_timer_arm`, `g_cntfrq / 10`). (3) The timer is armed in
`kernel_arm64_main` BEFORE the first EL0 excursion (it used to be armed
only in the wait phase after both excursions), so the first fire lands
inside EL0 #1's window. (4) `arm64_el0_done` clears the I bit as its
FIRST statement: the svc handler erets back with SPSR 0x3c5 (DAIF
masked), but a tick pended during the EL0 window is pending right then
— unmask immediately so it is taken exactly once on that return.

**The lost-tick failure the probe exists to prove absent.** The GIC PPI
is LEVEL-sensitive: if a window ever ran shorter than the period, the
timer would fire during the masked EL1 phase, the handler's re-arm would
deassert the line before it was ever taken, and that tick would vanish
— the count breaks or (worse) the gate never reaches its target and the
boot hangs. The measured ~760-790 ms windows vs the 100 ms period are
the margin that makes the pended-through-EL0 path the one that actually
happens; the margin is host-speed-dependent (TCG, the standing caveat),
which is exactly why the gate asserts the ordered stream, not a count
of ticks that happen to land.

**Measured.** A temporary CNTPCT delta print in the tick handler
(removed after the measurement) read the EL0 windows directly: [TICK 1]
was taken 763 ms after the arm, [TICK 2] 1549 ms after — i.e. the two
windows were ~763 ms and ~786 ms. The final gate, identical on
cortex-a57 and cortex-a53 and deterministic across runs:

```
[M5.2] contention probe: 100 ms ticks armed before the EL0 excursions...
[M5] eret into EL0...
[TICK 1]                            ← pended through EL0 #1, taken on return
[M5] returned from EL0 -- result=0x2a
[M5] eret into EL0...
[TICK 2]                            ← pended through EL0 #2, taken on return
[M5] returned from EL0 -- result=0x2a
[TICK 3]
[TICK 4]                            ← idle ticks complete the count
[M5.2] tick gate reached: 4 ticks, unbroken run
[M4] issuing PSCI SYSTEM_OFF        → qemu rc=0
```

The M5.1 counts are untouched by construction (1 MISS / 3 HITs / two
EL1 + two EL0 results, all 42 — the EL1 entries now just take longer:
~1.9 s total boot). The exception log is exactly 4 IRQ + 2 SVC + 1 PSCI
smc, no strays.

**The verifier `--max-steps` knob.** The loop executes ~1e9 steps — far
beyond simi-arm-verify's 10M infinite-loop budget. `simi-arm-verify`
gains an optional `--max-steps N` (default unchanged at 10M — the tight
guard for every other fixture); the size gate passes
`--max-steps 1000000000` for this one row. The M0-era verifier at
1729f50 got the same knob (throwaway worktree, for the baseline
measurement only).

**The size-gate row.** The embedded program changed, so its M0 baseline
was re-measured with the M0-era translator (1729f50 worktree, the
documented re-measure procedure adapted for a post-M0 fixture): the
M0 emission is 1020 bytes (the trivial 42-return was 928), the current
emission is 1016 (-4: the M2.x imm12-fold family) — the loop's
block-head frame reload is the one shape that wins nothing.

**The execution benches skip the fixture.** `arm64_boot_smoke` is
kernel-embedded, not a parity-corpus fixture (no runner includes it),
and a 500x bench-exec run of the loop would take ~an hour — so
bench-exec, bench-exec-rv64, bench-exec-interp, bench-corpus, and
cross-size SKIP it with a documented reason. This also closes the
latent M4b-era breakage where the fixture had no committed baseline
rows in any bench table (rv64_boot_smoke had them all; arm64_boot_smoke
was never added) and `make all` failed on it. The ARM parity runner
(run_arm_tests.sh) asserts the same latent fix: it verifies
arm64_boot_smoke by RESULT with `--max-steps` (no `--steps` — the
fixture deliberately has no committed steps row, since bench-exec
skips it and a row no bench maintains would be a lie; the kernel boot
is the real gate for this program).

**Honest caveats.** The window margin is host-speed-dependent (TCG is
not real-time; the gate asserts the ordered tick stream and the re-arm,
never wall-clock); the position of [TICK 1] specifically depends on the
EL0 window exceeding the period, which the 1e8-iteration loop makes
~8x on the dev host and larger on slower CI hosts; the lower-EL IRQ
slot stays a stub (SPSR masks IRQs during the EL0 excursions, as
scoped in §10.194). The kernel stack is 256 KiB and the boot now takes
~2 s — no gate-impacting cost.

### 10.197 M5.3 as built: interrupt NESTING — the virtual timer preempts the physical handler

M5.3 extends the M5.2 contention probe to interrupt nesting: unmask
IRQs inside the tick handler and assert the handler re-enters cleanly
when a second tick fires during it. The spike-before-estimate
discipline found the design three times over — each finding a real
bug, one of them in M5.2 itself.

**The same-PPI spike: architecturally impossible AND empirically
broken.** The naive reading (unmask inside the physical handler, let
the same PPI fire again) was spiked first. qemu's GICv2 model DOES
re-enter the same PPI (its running-priority rule is lax), printing the
nested line — but the boot then hangs (rc=124): the nested handler
re-arms and the double-activation/double-EOIR of the same INTID
corrupts the GIC state, breaking the tick stream. The architecture
agrees: GICv2's running-priority rule means an interrupt cannot
preempt its own active instance. The honest nested source is a
DIFFERENT interrupt at a higher priority — the ARM generic timer's
VIRTUAL timer (CNTV), PPI 11 → INTID 27, at GIC priority 0x00 vs the
physical's 0x80.

**Finding 1 — M5.2's ticks were spurious 1022s all along (group
semantics).** The virtual timer stormed (13516 IRQs, the first
physical handler never completing), and instrumenting the IAR read
showed the storm entries returning 1022, not 27 or 30. qemu's
`gic_get_current_irq` returns 1022 — the "spurious" value normally
reserved for 1023-class reads — when the pending interrupt is group 1
and the access is SECURE without GICC_CTLR.AckCtl (bit 2) set. This
kernel boots at EL1 in the SECURE world (qemu's -kernel path leaves
SCR_EL3.NS=0), so every GIC MMIO access is secure. The consequence is
retroactive: M5.2's group-1 configuration never delivered a real
interrupt — every IAR read returned 1022, and the gate passed only
because the handler re-armed the timer and the level deasserted, so
the cadence looked right. The fix: move BOTH timer PPIs to group 0
(the secure group — the default), which a secure access acknowledges
directly with no AckCtl and which matches real-hardware semantics for
a secure kernel. Group 0 works in both qemu configs (see Finding 3).

**Finding 2 — the classic nested-exception bug: the EL1h IRQ entry
must save/restore ELR_EL1/SPSR_EL1.** With real delivery, the nested
flow almost worked — TICK 1 → window → TICK 2 (virtual) → nested
marker — but the continuation never resumed after TICK 1's handler.
When the virtual IRQ preempts the physical handler's spin, the
hardware overwrites ELR_EL1/SPSR_EL1 with the spin's PC/state; the
nested eret resumes the spin fine, but the OUTER entry's eret then
reads the stale spin PC and loops in the handler tail. The fix is the
textbook one: `arm64_irq_el1h` grows to save/restore ELR_EL1/SPSR_EL1
(the frame is now 20 x 8 = 160 bytes; the restore runs with I masked
— the handler re-masks before returning — so no IRQ can clobber the
restored values before the eret).

**Finding 3 — the local-vs-CI config split (and why the local PSCI
"hang" was a config artifact, not a kernel bug).** The nested build
hung at PSCI SYSTEM_OFF locally (rc=124) but the CI-equivalent config
passed. The cause: CI boots `-M virt,virtualization=on` (NON-secure
EL1 — the §10.188 finding, where qemu's PSCI emulation intercepts the
SMC), while the plain `-M virt` used for local bring-up boots SECURE
EL1, where the SMC is NOT intercepted and qemu never powers off. The
group-0 fix works in both configs; the local runs moved to the CI
config for honest parity with the gate.

**The window + the M5.2-phase determinism fix.** The first restructure
put the unmasked nesting window inside TICK 1's handler, and the slow
UART prints stretched that handler past the 100 ms physical period —
T3/T4 landed in the excursion-2 setup, a fragile run-order-dependent
pattern (5-run determinism check: 3 of 5 streams differed). Two
changes made it deterministic: (a) the M5.2 gate stays BYTE-IDENTICAL
(the window moved OUT of TICK 1's handler into a separate phase after
the gate — TICK 5 physical opens the window, TICK 6 virtual preempts,
gate at 6), and (b) `arm64_el0_done` now re-masks IMMEDIATELY after
taking the pended tick, so every tick's take is pinned to a daifclr
boundary and the continuation's prints + the next excursion's setup
run masked — the next physical fire pends deterministically regardless
of UART timing. The window is a 10 ms spin (`arm_timer_cntfrq() / 100`,
not a hardcoded count — CNTFRQ is 6.25 MHz under the CI config, and
the hardcoded 3125000 was a 500 ms window). 5/5 runs are byte-identical
on both a53 and a57.

**Gate (M5.3) — PASSED:** the full ordered stream is
`eret → [TICK 1] → eret → [TICK 2] → [TICK 3] → [TICK 4] → gate (4) →
[TICK 5] → nesting window → [TICK 6] → nested marker → gate (6) →
PSCI SYSTEM_OFF`, rc=0 on a57 + a53, deterministic across 5/5 runs.
The M5.1 counts are untouched (1 MISS / 3 HITs / four 42s) and the
exception discipline is exactly 6 IRQ (4 physical + TICK 5 + the
nested TICK 6) + 2 SVC + 1 PSCI smc, no strays. CI (arm64-guards)
asserts the full nested stream, the 6-tick count, and the M5.3
markers.

**Honest caveats.** Nesting is proven only for the virtual-timer
source (the same-PPI case is architecturally impossible and
empirically broken — recorded, not papered over); the nesting window
is an unmasked 10 ms spin inside the EL1h handler, so a stray IRQ in
that window re-enters the handler (bounded: the spin is well under the
physical period); the lower-EL IRQ slot remains a stub (EL0 runs
masked by design); and the group-0 finding means the kernel's GIC
programming now targets the secure group — correct for this
Secure-world kernel, and a documented divergence from a hypothetical
NS EL1 kernel's group-1 layout.

### 10.198 Scope: the M5 FP/SIMD remainder — does the zero-FP claim hold, and what should the EL1h entry do?

The last honest-remainder item on the M5 board is FP/SIMD context.
The question decomposes into two: does the kernel have any FP/SIMD
state to preserve across the nested interrupt, and — if not — what
gate keeps that true? The verdict is a zero-FP claim, but only as a
MACHINE-CHECKED invariant, not as an argument; the EL1h entry needs
no SIMD save, and the honest remainder is the check, not the code.

**Leg 1 — codegen: `-mgeneral-regs-only` on every C source.** The
arm64 kernel builds with `-mgeneral-regs-only` in AR_CFLAGS (the
x86 `-mno-sse` analog), so the compiler's register allocator cannot
touch v0-v31; every FP/SIMD operation in C becomes soft-float GPR
code. This constrains COMPILER output only — hand-written assembly
is outside its reach (hence Leg 2).

**Leg 2 — image: the shipped ELF contains zero FP/SIMD instructions.**
The census is a disassembly sweep of the linked kernel
(`sls_arm64_kernel.elf` — every section, including the hand-written
boot_arm64.S and the vector table): `aarch64-linux-gnu-objdump -d`
counts 0 matches for v/q register operands and the FP opcode family
(verified on the M5.3 build: count = 0). This closes the hole Leg 1
leaves open: even an asm routine cannot slip SIMD into the image.

**Leg 3 — architecture: CPACR_EL1.FPEN=0 makes any violation a TRAP,
not corruption.** The kernel never writes CPACR_EL1, so it stays at
its reset value (FPEN=0): an EL1 access to the FP/SIMD registers
traps (same-EL synchronous exception) instead of executing. The
spike proved the backstop is real even under qemu TCG — injecting a
single `fmov d0, xzr` into the boot path printed `[TEMP] before SIMD
access` and then hung at the EL1h sync stub (no "after" print,
rc=124), exactly the GICv2-running-priority precedent's lesson: TCG
can be lax, so the spike matters. The failure mode is LOUD (the
vector stub's wfi hang) on both TCG and real silicon — never silent
state corruption. The x86 asymmetry is recorded in the ISA doc §16
Phase 16: x86's `-mno-sse` flags have no LOUD backstop — the wired
`#NM` lazy handler (CR0.TS=1 on context switch, vector 7) silently
"rescues" an accidental SSE instruction with foreign state instead of
halting — so the ARM kernel's FPEN trap is strictly stronger.

**The nested-handler question resolves to: no save, by construction.**
The M5.3 handler opens a 10 ms unmasked window; if it (or anything
it calls) used SIMD, a nested fire would clobber its FP state — the
ELR/SPSR bug class, for FP. A v0-v31 + FPCR/FPSR save (32x16 + 2x4 =
520 bytes; the entry frame would grow 160 -> 680) would be a save of
DEAD state: legs 1+2 prove no FP instruction can exist in the image,
and leg 3 proves that if one ever slips in, it traps before it can
execute. Saving would protect nothing; the invariant to protect is
"no FP instruction in the image," which is machine-checkable.

**What the remainder actually is — three committed gates, no context
code:** (a) a CI check in arm64-guards that runs the census grep over
the freshly built ELF and fails on any FP/SIMD instruction (the M2
gate shape: a machine check, not an argument); (b) a teeth smoke that
injects a SIMD instruction into a throwaway build and asserts the
boot hangs before the probe's "after" print — proving the trap has
not gone blind (the kernel-copy teeth pattern); (c) optionally, a
boot-time `[M5] cpacr_el1=0x0 (FPEN=0: FP/SIMD traps)` log line so the
FPEN state is visible in the serial stream. Estimated size: ~25 lines
of CI + ~30 lines of teeth script + ~3 kernel lines — the smallest
honest close of the board's last item.

**Future trigger (recorded, not built):** if the kernel ever needs
real FP — a hardware-float decision, a SIMD memcpy, a crypto
extension — the EL1h entry MUST grow the v0-v31 + FPCR/FPSR save
(520 bytes) or adopt lazy switching (CPACR FPEN=1 + a
save-on-first-use handler). The M5.3 nesting window makes this a
correctness requirement for any future FP-using handler, not an
optimization. Until then, the zero-FP claim stands on three
independently machine-checked legs.

### 10.199 M5 FP/SIMD gates as built: the census, the trap, and their teeth

The §10.198 scope is implemented as three committed gates, closing the
M5 board's last item. All three were machine-verified locally before
wiring, and the census regex was itself spiked: the first attempt
(`\b[dsvqh][0-9]+\b` over the whole disassembly) false-positived 194
times on the clean ELF — the 8-hex-char ENCODING word column
(0xd5384242 = `mrs x2, currentel`) trivially matches `\bd[0-9]+\b`. The
census is therefore scoped to the mnemonic + operand COLUMNS of
objdump's tab-separated output (`ADDR:\tWORD\tMNEMONIC\tOPERANDS`):
mnemonic in the FP family, or a d/s/v/q/h register in the operand
column (allowing the vector-suffix forms like `v0.4s`). Validated: 0
matches on the clean ELF, 3/3 known-bad lines caught (`fmov d0, xzr`,
`ldp q0, q1, [x0]`, `add v0.4s, v1.4s, v2.4s`), and the hex-immediate
`#0x40d0` false positive correctly excluded.

**Gate 1 — the census (`tools/simi/tests/arm64_fp_census.sh`).** The
single source of truth for leg 2: disassembles the ELF and fails on
any FP/SIMD instruction (exit 1, printing the offending lines; exit 2
if the toolchain or ELF is missing). The arm64-guards job runs it
right after `make arm64-elf`, before the boot — a violation fails the
job at the image, before any boot could paper over it.

**Gate 2 — the visible backstop (kernel_arm64.c).** The boot now logs
`[M5] cpacr_el1=0x0000000000000000 (FPEN=0: FP/SIMD accesses trap to
EL1)` — the leg-3 state pinned in the serial stream, asserted by CI.
The kernel still never writes CPACR_EL1, so FPEN is the reset value;
if a future change sets FPEN=1, the line changes and the census +
teeth must move with it (recorded in the code comment).

**Gate 3 — the teeth (`tools/simi/tests/arm64_fp_gate_smoke.sh`).**
The kernel-copy teeth pattern applied to both machine-checkable legs:
- tooth 0: the clean ELF passes the census (sanity);
- tooth 1: a real `fmov d0, xzr` injected into kernel_arm64.c makes
  the census FAIL;
- tooth 2: booting the broken kernel under the CI config TRAPS — the
  probe's "before" line prints, the "after" never does, qemu times
  out (rc=124) at the EL1h sync stub: the FPEN backstop is loud, never
  silent corruption;
- restore: the injected kernel is reverted from a file backup (no git
  dependency — the smoke's restore must work in WSL, Git Bash, and CI
  alike) and rebuilt, and the census passes again.

**CI wiring.** Three additions to the arm64-guards job: the census
after the build, the cpacr log-line assert after the boot, and a new
"Teeth — FP/SIMD census + trap smokes" step asserting "4 passed, 0
failed". Verified locally: census 0/clean + 3/3-bad, the full
CI-equivalent boot block (banner, EL1, cpacr FPEN=0, PSCI rc=0, 6
unbroken ticks, M5.1 counts) passes on a fresh boot, and the smoke
fires 4/4 from a clean tree.

**Honest caveats.** The census is image-scoped — it sees the shipped
ELF, so it must stay wired into CI to remain true; the FP mnemonic
family list and register-class check are enumerations (an exotic
future FP instruction class would extend the list — the teeth smoke
only proves the CURRENT classes are caught); the trap's loudness is
the EL1h sync stub hang, which under qemu reads as rc=124 — the smoke
asserts exactly that shape.

### 10.200 M5.4 as built: the per-slice LCG teeth on the arm64 kernel

The fairness probe's "the work provably ran" proof lands on the
AArch64 kernel, mirroring the RISC-V kernel's per-slice LCG
(kernel_riscv.c, rv_fp_task_common). A new kernel-embedded fixture
(`tools/simi/tests/lcg_slice.simi`, embedded as
`kernel/arm64_lcg_slice_tmo.h` — the arm64_boot_smoke_tmo.h xxd
pattern, byte-diffed identical) runs ONE full per-slice budget of the
task LCG (acc = acc\*1664525 + 1013904223 mod 2^32, exactly
sp->work = 200,000 iterations from 0 — the same recurrence the
parity-corpus lcg_fairness.simi pins) and RETURNS the raw acc tooth:
0x0b6f2a40 = 191834688, printed by the kernel and asserted by CI as
`lcg slice executed (one per-slice budget) -- acc=0x000000000b6f2a40
(expected 0x0b6f2a40 = 191834688) PASS`. The RISC-V connection is
exact: five consecutive 200,000-iteration slices accumulate to the
committed all-heavy tooth 0xf2dc5340.

Design decisions, each pinned by the boot gate:
- **Kernel-embedded, NOT a parity-corpus fixture** (the
  arm64_boot_smoke §10.196 model): its real gate is the kernel boot's
translate + execute + serial assert, not the host parity tables. The
interp/x86/RV64 runners and all four bench tools skip it; the ARM
runner still executes it (its ~4.4M A64 steps fit the tight 10M
budget — no raised budget needed) without a steps row ("a row no
bench maintains would be a lie"), and the size gate pins its emission
(1092 bytes). Its host parity shape is covered by lcg_fairness.simi
in the corpus.
- **Direct translation, not the activation cache**: the program runs
once, so caching would serve no reuse — and routing it through
arm64_activate would have bumped the MISS count and broken the M5.1
gate (exactly one MISS at boot). It gets its OWN static code buffer
(`g_lcg_code_buf`, 16-aligned — EL1-only, no page mapping needed), so
the boot smoke's translated code survives untouched for the EL0
excursions.
- **Runs after the M5.1 four-entry gate, before the timer arms**: the
call sits between the second `arm64_el1_entry()` and
`arm_timer_arm()` in kernel_arm64_main, with IRQs still masked — no
tick can fire during the ~4.4M-instruction run, so the M5.2 tick
gate's count (exactly 6) and interleave are untouched by
construction. Boot-verified: rc=0 (PSCI), 1 MISS / 3 HIT / 2 EL1
entries / 2 EL0 excursions / 6 ticks — every pre-existing gate
byte-identical — plus the new `acc=0x000000000b6f2a40 ... PASS` line.
- **The discriminator is the value itself**: a wrong recurrence (missing
mod-2^32 mask, swapped constants, off-by-one count) changes the acc
and the kernel's PASS/FAIL verdict (and CI) fails; the host fixture's
"Expected result: 191834688" is verified on all four engines (ARM
1092 bytes, RV64 1116, x86 699, interp value-exact).

### 10.201 M5.6 as built: the LCG through the EL0 containment path

§10.200's EL1-only proof is extended to the containment path: the
per-slice LCG now takes the activation cache + EL0 excursion, so the
user-mode-containment proof executes the IDENTICAL work the EL1 boot
slice runs — at the byte level, not the equal-results level. The
cache gains a second slot (`ARM64_ACT_SLOTS 1 -> 2`) with a
per-slot `code_buf` (slot 0 -> g_smoke_code_buf, slot 1 ->
g_lcg_code_buf, now 4096-aligned so the user tree can map its page),
and the EL0 machinery is parameterized (`arm64_el0_activate_for(prog)`
+ a per-program continuation in `arm64_el0_done`, driven by
g_el0_program):

- **The EL1 LCG slice is now cache-routed** (§10.200's direct
translation is superseded): `arm64_lcg_slice_test` activates
"lcg_slice" (MISS, slot 1) and blr's the entry. The two LCG EL0
excursions then HIT the same slot — the SAME cached bytes are
re-executed at EL0, which is what makes "the EL0 run executes the
identical work" a byte-level claim (a discrepancy would be a
containment/eret fault, not a codegen change). The `acc=... PASS`
line shape is unchanged; one translation now serves all three LCG
executions.
- **The excursions run before the timer arms** (main launches the LCG
pair; the continuation's handoff arms the timer, prints the M5.2
probe line, and launches the smoke pair), so no tick can pend
through the LCG windows — the M5.2/M5.3 tick stream (exactly 6,
ordered) is unchanged by construction, and the new want= token
stream in CI is just the committed stream with two leading `eret
into EL0...` tokens (the LCG pair).
- **The boot gate is now six entries, two translations**: smoke EL1
MISS + HIT (42, 42), LCG EL1 MISS (0x0b6f2a40), LCG EL0 HIT + HIT
(0x0b6f2a40, 0x0b6f2a40), smoke EL0 HIT + HIT (42, 42). CI counts:
MISS = 2, HIT = 5, EL1 smoke = 2, EL0 smoke = 2, NEW LCG EL0 = 2,
ticks = 6, ordered stream with the two leading erets. Boot-verified:
rc=0 (PSCI), all counts exact, 0 FAIL lines, 4 user-tree builds, all
pre-existing asserts intact.

### 10.202 M5.7 as built: the containment path proven on DATA — the mem-touch fixture

§10.201's containment proofs (smoke, LCG) only ever exercised the
user tree's CODE page — their memory effects were the frame carve on
the user stack, which is infrastructure, not program. The mem-touch
fixture closes that: a new embedded program
(`tools/simi/tests/mem_touch.simi`, embedded as
`kernel/arm64_mem_touch_tmo.h`, byte-diffed identical, 240 bytes)
whose result depends on the user TTBR0 tree's DATA page. Its r7
scratch pointer is baked to **USER_SCRATCH_VA (0x10005000)** at
translate time — a VA the kernel's TTBR1 tables do NOT map — so the
program STOREs a 3-cell pattern into the scratch page, LOADs it
back, verifies every cell, and returns the tooth 0x0d15ea5e only if
all match (0 otherwise). A faulted access (unmapped user page) traps
to the EL1h sync vector and HANGS the boot (rc=124); a wrong
translation or a stale read changes the value and the PASS verdict
fails. The interpreter/x86/RV64 legs return 219540062 too (the
scratch-relative r7 convention is ISA-neutral), verified on all four
engines + real A64 (ARM a64_exec 1212 bytes, real A64 1208, RV64
1252, x86 832).

Design decisions:
- **EL0-only by construction** — the baked scratch is a user VA, so
  the program CANNOT run at EL1 (it would fault). That is why the
  mem slot's first excursion is its MISS (unlike the smoke/LCG,
  which warm at EL1): the translation happens on the first EL0 run
  and the second EL0 run HITs it. The slot records scratch as part
  of the emitted code's identity (name-keyed, fixed per program).
- **Third slot + third buffer** (`ARM64_ACT_SLOTS 2 -> 3`,
  `g_mem_code_buf`, 4096-aligned like the others); the EL0 machine's
  per-program state grows a MEM branch, and the continuation chain
  is LCG pair -> mem pair -> (arm the M5.2 timer, probe print) ->
  smoke pair -> ticks -> M5.3 -> PSCI. Everything before the timer
  arm, so the tick stream is untouched by construction.
- **The gate is now eight entries, three translations**: smoke EL1
  MISS+HIT, LCG EL1 MISS, LCG EL0 HIT+HIT, mem EL0 MISS+HIT, smoke
  EL0 HIT+HIT. CI: MISS = 3, HIT = 6, LCG EL0 = 2, NEW mem EL0 = 2,
  ticks = 6, ordered stream with FOUR leading erets. Boot-verified:
  rc=0 (PSCI), all counts exact, 0 FAIL lines, 6 user-tree builds.
  Runner treatment mirrors lcg_slice: skipped on the interp/x86/RV64
  legs + all bench tools, RUN on the ARM runner (a few hundred A64
  steps, no steps row) and the real-A64 leg; the size gate pins the
  1212-byte emission.

### 10.203 M5.7 teeth as built: the EL0-only claim machine-checked — the EL1 mem_touch attempt

§10.202's "EL0-only by construction" was a claim (the baked scratch is
USER_SCRATCH_VA, unmapped in TTBR1, so at EL1 it must fault) backed
only by the design argument. This teeth closes that: a THIRD arm64
kernel ELF, `sls_arm64_kernel_teeth.elf` (`make arm64-teeth`, the same
sources with `kernel_arm64.c` compiled under
`-DARM64_TEETH_EL1_MEMTOUCH`), whose main() skips the gate sequence
and instead attempts to run the mem_touch fixture at EL1 with the
SAME baked scratch (the slot's normal activation path, scratch
= USER_SCRATCH_VA). Its first STORE faults — Data Abort from current
EL, EC=0x25, far=0x10005000 — traps to the EL1h sync slot, and CI
asserts the HANG (rc=124), the [TEETH] attempt line, the [SYNC]
lines (ec=0x25, far=0x0000000010005000), and the ABSENCE of any
result or [TEETH] FAIL line. Boot-verified on real A64: rc=124, the
fault fires with exactly that syndrome/VA, and the hang is immediate
(the fault happens microseconds after the translation MISS).

Design decisions:
- **The EL1h sync slot gained a real handler** (boot_arm64.S
  arm64_sync_el1h): print ESR_EL1/FAR_EL1 through a C helper
  (`arm64_sync_el1h_c`) and halt. Previously the slot was the silent
  wfi stub; NO committed gate ever takes a sync-from-current-EL
  exception (the svc-return path is the sync-from-lower-EL slot, the
  tick path is IRQ), so the stock boot's log is byte-identical
  (re-verified: rc=0, MISS=3, HIT=6, ticks=6, mem EL0=2, ordered
  stream exact). A fault with any other EC prints the mismatch
  verdict and fails the teeth greps.
- **The teeth build is honest about dead code**: the gate-sequence
  functions (arm64_el1_entry/arm64_el0_done/arm64_wait_ticks/...) are
  never called in this variant, so its object rule adds
  `-Wno-unused-function` (documented in the Makefile — expected
  warnings, not bugs). The object is named `*.ar64.o` so the existing
  `make clean` pattern removes it.
- **Teeth-asserted, not just logged**: the CI step fails on rc=0
  (the program COMPLETED — the claim is false), on rc != 124 (early
  crash, wrong failure), on a missing [SYNC] line (no fault fired),
  on ec != 0x25 (wrong exception class), on far != 0x10005000 (wrong
  faulting VA), on [TEETH] FAIL, or on a mem_touch result line. The
  teeth ELF is censused like the stock one (same sources + one -D:
  the zero-FP/SIMD claim holds). This is the same teeth-in-CI
  discipline as the FP/SIMD and RV64 fp-save gates: a claim backed by
  a deliberately broken attempt that must fail loudly.

### 10.204 M5.8 as built: the EL1h backstop reports the taking-EL from SPSR — the EL0-future distinction stays honest

§10.203's handler assumed the fault was EL1-originated (the slot
*is* the same-EL backstop), so its verdict hard-coded "from EL1".
True for every committed gate — but a claim about the hardware's
SPSR made without reading it. This change makes the report read
SPSR_EL1: boot_arm64.S arm64_sync_el1h now also does
`mrs x2, spsr_el1` and passes it to arm64_sync_el1h_c, which decodes
M[3:0] (arm64_print_spsr_el: EL0t/EL1t/EL1h/EL2t/EL2h/EL3t/EL3h)
and prints `-- taken from <EL>` on the [SYNC] report line. Booted on
real A64, the teeth fault now reads:

    [SYNC] EL1h synchronous exception: esr=0x0000000096000044
      ec=0x0000000000000025 far=0x0000000010005000
      spsr=0x00000000600003c5 -- taken from EL1h (M=0x5, current EL, SP_EL1)
    [SYNC] Data Abort from EL1h (current EL, SPSR M=0x5): ... (teeth PASS)
    [SYNC] unhandled -- halting (the hang is the teeth: rc=124)

The verdicts are EL-aware instead of slot-assuming:
- **EC=0x25 AND M=0x5** (same-EL Data Abort taken from EL1h): the
  teeth PASS. BOTH conditions are required — a drift in SP selection
  or routing that changes the taking-EL fails the gate rather than
  passing silently.
- **EC=0x24** (lower-EL Data Abort): the canonical EL0-originated
  abort encoding. If a future EL0-fault path ever delivers one to
  this backstop, the report line names the taking EL (`taken from
  EL0t (M=0x0, lower EL, AArch64)`) and the verdict says so
  explicitly — no teeth verdict, but the honest story is on the log.
  Today a lower-EL fault goes to VBAR+0x400, so this branch is
  reachable only if routing changes.
- **EC=0x25 with M != 0x5**: same-EL encoding but a foreign
  taking-EL — an inconsistency hardware cannot produce; FAIL.
- **Any other EC**: the existing mismatch FAIL.

CI's teeth step gained three asserts: `-- taken from EL1h (M=0x5,
current EL, SP_EL1)` (the SPSR-derived report), the negative `taken
from EL0t` (the EL0-future line must NOT appear), and the verdict's
`Data Abort from EL1h (current EL, SPSR M=0x5)`. One LATENT BUG
surfaced and was fixed in the same step: the old `grep "ec=0x25"`
never matched the real log — print_u64 pads to 16 hex digits, so the
line carries `ec=0x0000000000000025` (the far grep already used the
padded form, which is why only EC was broken). The EC assert now
uses the padded form, with a comment recording the pitfall. The
stock boot never faults, so it is untouched (re-verified: rc=0,
MISS=3, HIT=6, ticks=6, mem EL0=2, ordered stream exact) and both
ELFs re-censused clean.

---


## Sources consulted

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
