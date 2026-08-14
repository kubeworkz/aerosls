# AeroSLS QEMU-SLS — Step 6 extension: the full AMD64 Guest Frontend, v0.1

*Extends `AeroSLS-QEMU-SLS-Step6-x86-Frontend-Plan-v0.1.md` (the "Step 6" plan) from
the 32-bit, paging-off guest it currently drives to **full AMD64 (64-bit mode)**.
Target unchanged from the Step 6 agreement of 2026-08-05: **arbitrary statically-linked
Linux x86-64 binaries.** Implementation environment constraint: freestanding, in-kernel,
no user space, no glibc — the same constraint every `sls/` file already honors.*

**Status: scoping plan — no code landed. Every claim below is checked against the tree
as of 2026-08-11; the ones that are hypotheses are labeled as such.**

---

## 0. The one fact that orders everything

**Long mode cannot exist without paging.** Entering 64-bit mode requires `CR0.PG=1`,
`CR4.PAE=1`, and `EFER.LME=1` (LMA is set the moment PG is set with LME=1), and every
instruction fetch after that is a paged fetch through the guest's CR3 tables. There is
no 64-bit execution with paging off, at all, for a single instruction.

This collides head-on with the current integration, where
`sls/sls-i386-codefetch.c` **refuses loudly when `qemu_sls_guest_paging_on` is set** —
its GPA-window read (`SLS_GPA_BASE + addr`) is only correct while guest virtual ==
guest physical. That refusal is not a bug; it is the honest boundary of the paging-off
path. But it means the entire "full AMD64" milestone order is forced by one constraint:

> **The shadow-walk / paging-on fetch+data path is not the last thing to build. It is
> the first thing. There is no intermediate paging-off 64-bit state to ship first.**

Everything else in this plan is depth on top of that.

---

## 1. Current state — what "the i386 guest frontend" actually is

The phrase is loaded: `sls/sls-x86-frontend.c` is 308 lines and 18 opcodes, but QEMU's
own decoder — `target/i386/tcg/translate.c` (3,620 lines) + `decode-new.c.inc` (3,149)
— is already compiling against the SLS shim behind `SLS_X86_FRONTEND=on`. ("i386" here
is QEMU's *target* name, which covers both 32- and 64-bit decode — the name is not a
32-bit limit, but the *machine state feeding it* is.)

| Piece | File | State (2026-08-11) |
|---|---|---|
| Hand-rolled frontend, 18 opcodes | `sls/sls-x86-frontend.c` | Default build. Deliberately rejects SIB (`rm==100`) and RIP-relative (`mod==00, rm==101`) addressing — structurally incapable of 64-bit code. |
| C dispatcher | `sls/sls-launcher.c` (~line 680+) | Hybrid loop: HLT, OUT, CALL, RET, LODSB, TEST, JZ/JNZ, MOV CR0/3/4, WRMSR/RDMSR (EFER only). Exits translated blocks to C for everything else. |
| Guest CPU state | `SLSCPUState` in `sls/sls-launcher.h` | `regs[16]`, `eip`, `eflags`, `cc_dst/cc_src/cc_op` (TCG cc state), `halted`, `cr0`, `cr3`, `cr4`, `efer`. Zero at reset — boots paging-off, as real hardware. |
| QEMU decoder (Step 6.1) | `target/i386/tcg/translate.c` + `decode-new.c.inc` | Compiles clean, 701,888-byte object, 919 functions. Full 64-bit decode machinery present: `CODE64`/`LMA`/`REX_W` (`translate.c:198-218`), `SYSCALL`/`SYSRET` decode entries (`decode-new.c.inc:1264-1266`), `swapgs` (`translate.c:3126`). |
| Build wiring (Step 6.2) | `make x86-iso SLS_X86_FRONTEND=on` | Object built by explicit path (never VPATH); flag off by default. |
| Execution loop (Step 6.3) | `sls-launcher.c` + one file from `accel/tcg`: `translator.c` | Decided: keep our launcher loop + TB management; `cputlb.c` deliberately excluded. |
| Helpers (Step 6.4) | `sls/sls-i386-helper-stubs.c` | All **765** `helper_*` defined via QEMU's own `DEF_HELPER` expansion, real signatures, **each halts and names itself**. Remaining unresolved symbols: 24, all AeroSLS kernel-side (0 `helper_*`, 0 code-fetch) — see `docs/step6/undefined-symbols.txt`. |
| Code fetch | `sls/sls-i386-codefetch.c` | GPA-window read; **refuses if guest paging is on** (the gate this plan exists to remove). |
| Guest paging machinery | `kernel/qemu_sls_mmu.c` (+ host test suite, 107 checks) | Shadow tables, `shadow_fault`, guest-paging enable/reset — real and tested; **not wired into instruction fetch or the TCG data path for GVA translation**. |

**Two things the decoder already gives us, so this plan does not spend any milestone on
them:** REX/66/67 prefix handling, and the 64-bit addressing forms (RIP-relative, SIB
with all 8 base/index registers, disp32 sign-extension). They are TCG-op work in
`translate.c`, not helpers, and they decode correctly today given the right mode state.
What is *not* given is that mode state, the paging translation behind every fetch and
load/store, the helper semantics, and the guest bootstrap.

---

## 2. The gap decomposition — five distinct pieces of work

"Full AMD64" decomposes into five gaps. They are independent enough to milestone
separately (M1–M6 in §5 map onto these 1:1), and the first one is the paging gate.

### Gap A — 64-bit mode state (who the decoder thinks it is)

Every `CODE64`/`LMA` decision in `translate.c` reads `dc->flags` — `HF_CS64_MASK`,
`HF_LMA_MASK` — plus the CS selector and EFER. Today the launcher drives a
32-bit-ish, paging-off guest. Full AMD64 needs the machine to be *born* in long mode:

- `CS.L=1, CS.D=0`, flat 64-bit code segment, CPL 0; `EFER.LME=LMA=1`; `CR4.PAE=1`;
  `CR0.PG=1` — constructed by the launcher as **boot state**, not earned by the guest
  executing real-mode → protected → long-mode transitions. This matches the agreed
  target: a statically-linked Linux binary never runs real-mode code; the OS sets up
  long mode and starts it there. Emulating the legacy boot sequence would be pure
  waste. State this choice explicitly in the code: the guest starts at its ELF entry
  with long mode already on.
- `CPUX86State` fields the decoder reads that `SLSCPUState` does not have: `eip` under
  `env`, the segment registers and their bases (FS/GS base matter — local-exec TLS
  resolves every `%fs:offset` access through the FS base, the thread pointer),
  `a20_mask`, the xmm/fp register files (Gap D), and the
  TCG env global layout `translate.c` binds via `offsetof`. The Step 6.1 shim already
  provides the real `CPUX86State`; the work is *initializing* it and keeping the two
  state structs coherent (or replacing `SLSCPUState` with `CPUX86State` outright —
  a decision for M1, see §5).
- Canonical-address semantics in the fetch/data path: 64-bit mode requires
  sign-extended addresses; a non-canonical address is a `#GP` (exit-to-C, Gap E).

### Gap B — paging-on fetch and data path (the gate)

Long mode forces paging on (see §0). The pieces exist but are not connected:

- **Fetch:** `sls-i386-codefetch.c` must translate guest VA → host pointer through the
  shadow tables instead of refusing. The launcher already runs the whole guest window
  under `qemu_sls_shadow_cr3` with `qemu_sls_guest_active=1`, and `shadow_fault()`
  populates the shadow on host #PF — so the fetch path can be a real GVA→host resolve
  that consults the shadow (walk it in software or fault-and-retry), not a special case
  for identity mappings. The refusal stays as the fallback for *foreign* guest tables
  until M2.
- **Data:** TCG `qemu_ld/st` in the softmmu=OFF build emit direct host accesses at
  `guest_base + gva`. With paging on, "gva" is a guest *virtual* address; the emitted
  access must land in shadow-mapped host memory. This is the unresolved design point
  from `AeroSLS-QEMU-SLS-Guest-Address-Space-Design-v0.1.md`: the GVA-direct scheme
  (map guest VAs directly, no base register) is **blocked by the shared-table defect**
  it documents — `qemu_sls_mmu_init()` copies kernel PML4 entries by value, so
  installing guest mappings at low addresses overwrites the kernel's live tables. The
  three ways out are on record there: (a) COW-clone page tables, (b) the guest_base
  window (shipped for paging-off; for paging-on it needs a per-access translation), (c)
  a separate address space with a world switch (shadow root maps only guest memory +
  a trampoline). **This plan makes the choice of (a)/(b)/(c) an explicit M2 decision
  record with the defect doc as the starting point**, because it is the single most
  consequential architectural call in the whole effort.
- The `guest_paging_on` flag and the reset/restore machinery
  (`AeroSLS-QEMU-SLS-Guest-Paging-Reset-Defect-v0.1.md`, fixed 2026-08-06) stay as-is;
  M1 builds on the *correct* reset, it does not re-litigate it.

### Gap C — helper semantics for compiled 64-bit code

The 765-helper census (from Step 6.1): **507 vector (66%), 59 x87 (7%), 5 flags (<1%),
194 other (25%)**. The decoder emits every one as a call; today every one halts. The
plan's ordering principle (inherited from Step 6.4) stays: *implement by what a target
binary actually calls, not by walking the list.* The compiled-64-bit-binary subset:

| Class | Helpers | Needed by |
|---|---|---|
| Flag computation | the 5 flag helpers (adc/sbb/rcr/rcl shift paths, `helper_cc_compute_all`) | essentially every compiled instruction stream that touches CF — M3 |
| Integer | div/idiv (all forms), one-operand mul, bswap, shift-by-cl edge cases, string ops (rep movs/stos/cmps), cmpxchg paths | any non-trivial -nostdlib binary — M3 |
| System | `helper_syscall` (`helper.h:55`, called from `emit.c.inc:4130`), cpuid, rdtsc, in/out, invlpg, cli/sti, hlt | any binary that touches the OS at all — M4 |
| SSE2 scalar | movsd/movss family, addsd/addss, subsd, mulsd, divsd, sqrtsd, xorpd, ucomisd/comisd, cvtsi2sd/cvtsi2ss, cvttsd2si/cvttss2si, movd/movq, mxcsr handling | compiler-generated double/float math (gcc -O2 emits SSE2 by default) — M6 |
| Everything else | ~600 (AVX/AVX2, most SSE4, MMX, all 59 x87, 3DNow, transactional) | **deferred or permanent-unsupported** — §4 |

The "stub halts loudly" discipline (Step 6.4) is the right instrument throughout: a
stub is the *correct permanent answer* for an instruction the agreed target never
executes, and the surviving stub list is the written definition of what this build does
not support. Nothing in this plan asks for silent-0 fallbacks.

### Gap D — syscall and system semantics (the architectural decision)

`SYSCALL` (0F 05) is decoded (`decode-new.c.inc:1264`) and emitted as
`gen_helper_syscall(tcg_env, insn_len)` (`emit.c.inc:4130`). Upstream's
`helper_syscall` raises `EXCP_SYSCALL` — "only for user emulation" (`cpu.h:1508`) —
or delivers a system-mode exception to a guest OS. **SLS is the guest OS.** There is no
Linux kernel underneath the guest, so the helper must route somewhere real:

1. **Exit-to-C and halt** — M4 gate, honest and trivial: the launcher records the
   syscall number + args and stops. Proves the decode/emit/helper path end-to-end.
2. **Route to the AeroSLS syscall surface** (`SYS_SLS_*`) — natural for SIMI-era
   guests, but the agreed target is *Linux* binaries that issue Linux syscall numbers.
3. **A minimal Linux-compat shim inside SLS** — `write`→serial,
   `exit`/`exit_group`, `brk`/`mmap`/`munmap`→SLS frames, plus `read`, `futex`,
   `clock_gettime` as the freestanding fixtures call them (stub-halt discipline:
   implement what the fixture demands, nothing else — there is no libc in the
   picture to demand a fixed surface). **M5.**

The plan's position: (1) is the first milestone, (3) is the target, (2) is a parallel
lane for SIMI guests that does not block the Linux-binary track. `swapgs` is already
translated inline (`translate.c:3126`) — it must at minimum swap the two GS bases in
the env without faulting; FS/GS base state rides along in Gap A.

### Gap E — guest bootstrap and the ELF boundary

Nothing in `sls/` parses ELF today (the `qemu run <hex>` shell command feeds raw
machine code at a fixed GPA). "Arbitrary statically-linked Linux x86-64 binaries"
requires a loader before execution even starts:

- Parse ELF64 (no interpreter — static means no PT_INTERP), place PT_LOAD segments,
  zero BSS, honor PT_TLS (the CPU has no TLS of its own: a fixture with `__thread`
  locals reads them through `%fs:offset` immediates the linker baked against a thread
  pointer at `tls_vaddr + round_up(memsz, align)`, so the loader must place the
  template, zero `.tbss`, and build the TCB with the self-pointer that layout demands).
- Build the initial stack in guest RAM: argc/argv/envp **and the auxv vector**
  (AT_PHDR/AT_PHENT/AT_PHNUM/AT_PAGESZ/AT_ENTRY/AT_UID/AT_GID/AT_RANDOM/AT_EXECFN) —
  the System V ABI every ELF64 entry point expects to find at RSP.
- Enter long mode at the ELF entry with the launcher-built identity tables covering the
  loaded segments and the stack (Gap A + B).

The M4 syscall shim and the M5 loader are one milestone: a loader is useless until
`write` and `exit_group` work, and the shim's test vehicle is a loaded binary.

---

## 3. Decoding strategy — the decision and the rejections

**Decision: the decoder is QEMU's own `target/i386/tcg/translate.c` +
`decode-new.c.inc`, already integrated (Steps 6.1–6.3). The AMD64 plan extends what
feeds it (machine state, paging, helpers, bootstrap) — it does not write a decoder.**

This is a decision, not an accident of history, and it is worth stating the rejections
explicitly, because "write our own AMD64 decoder" is the natural instinct and it is the
wrong one here:

- **Rejected: extend the 18-opcode `sls-x86-frontend.c` to 64-bit.** It is
  structurally incapable: it has no prefix/mode machinery, no REX, and its addressing
  decoder *deliberately rejects* SIB and RIP-relative (the comment says so — "a
  frontend that silently mis-decodes an address produces a guest that reads the wrong
  memory"). Extending it to 64-bit means writing REX/66/67 handling, a full ModRM+SIB
  decoder, and mode-state tracking from scratch — i.e., writing a second full AMD64
  decoder that already exists and already compiles. Rejected.
- **Rejected: a fresh hand-written decoder (Capstone-style opcode tables).** 6,769
  lines of battle-tested decode, with the entire QEMU helper ecosystem's IR contract
  already built around it, would be duplicated with zero integration benefit and a
  permanent divergence cost every time upstream fixes a decode bug. Rejected.
- **The 18-opcode frontend's fate (Step 6.5):** with translate.c carrying 64-bit, it
  cannot remain "the frontend." Resolution: retire it as a frontend, keep it (if at
  all) as a documented test fixture for the C-dispatcher hybrid, and flip the default
  build to `SLS_X86_FRONTEND=on` once M7's corpus passes — M8 in this plan.

**What "the decoder" therefore costs us:** the mode-state contract it assumes
(`dc->flags` from CPUX86State, Gap A) and the helpers it calls (Gap C/D). Both are
bounded and enumerated — the 765-helper census is a number, not a fog.

---

## 4. Instruction coverage — the full AMD64 surface, by class

### 4.1 Already free (decoder + TCG ops, no helpers, no mode-state work beyond Gap A)

MOV/LEA (all widths incl. REX.W 64-bit), push/pop, ADD/SUB/AND/OR/XOR/CMP/TEST on
registers and memory, NOT/NEG, shifts/rotates (most), JMP/Jcc/CALL/RET, Jcc/setcc/cmov
(most), MOVZX/MOVSX, **MOVSXD (63 /r — the i386 ARPL conflict is already resolved by
the decoder's mode-aware tables)**, NOP/PAUSE, RIP-relative and SIB addressing,
66h/67h/REX prefix matrix, 32-bit-write zero-extension semantics.

### 4.2 Helper-gated — must implement for the agreed target (≈30–60 of 765)

Flag computation (5); div/idiv/mul/bswap/shift-by-cl/string-op subset of the 194
"other"; syscall/sysret/cpuid/rdtsc/in/out (system subset of the 194). Ordered by what
the milestone binaries call (§5), with halting stubs for everything else.

### 4.3 SSE2 scalar — one deliberate, bounded slice (≈30–50 of 507)

movsd/movss + arithmetic/compare/convert/movd/movq + mxcsr, enough for
compiler-generated scalar double/float. This is *optional* for the agreed target
(compile fixtures with `-mno-sse -msoft-float` for M1–M5), but "arbitrary binaries"
means real ones eventually, and gcc defaults to SSE2. M6.

### 4.4 System — the small set that makes a Linux binary breathe

`syscall` → helper (Gap D), `sysret`, `swapgs`, `cpuid` (return a conservative leaf set
consistent with what helpers actually implement — do not advertise AVX before AVX is
real), `rdtsc` (scale host TSC), `in`/`out` (route to the SLS port model / serial,
per SCOPE.h CATEGORY 9), `cli`/`sti`/`hlt`, `invlpg`.

### 4.5 Deferred — with reasons, not silence

**Measured, not estimated (M7.5, iteration 29):** the census below is the
actual surviving halting-stub universe at qemu-sls `9b61143` — 676 declared
helpers, 48 with real bodies, **628 surviving halting stubs** — derived by
`sls/gen_unsupported_list.py` from the same sources the kernel links
(`target/i386/helper.h` + `ops_sse_header.h.inc` minus the real-body
renames in `sls-i386-helper-stubs.c`). The count moved from the plan's
"~600" guess to a number that regenerates on every commit; the vector
family split carries the usual glue-family imprecision, the x87/3DNow/SHA/
MPX/tail counts are exact.

| Class | Stub helpers | Why permanent |
|---|---|---|
| AVX/AVX-512 (ymm) | 131 | gcc -mno-avx output never emits a ymm op; the agreed target needs SSE2 scalar only |
| SSE vector (xmm) | 176 | the M6 scalar slice took the arithmetic/convert/compare the compiler emits; the packed family is not in its output |
| MMX | 135 | dead in 64-bit compiler output (x87-era); the 118 `_mmx` glue helpers exist only for QEMU's unified vector backend |
| x87 FPU | 80 | gcc emits x87 only for `long double`; fixtures use `-mlong-double-64` / `-msoft-float` (documented flag, no 80-bit math). The old "59" was per-instruction-class; the helper-name count is 80 |
| 3DNow! | 19 | never in any 64-bit compiler output (removed from hardware after AMD K6-2/K7) |
| SHA | 10 | gcc emits SHA extensions only under `-msha`; the agreed target does not |
| MPX bounds | 6 | Intel MPX was removed from hardware in 2021; never in modern compiler output |
| AVX-FMA | 8 | fma4 was AMD-only and never shipped in 64-bit gcc output; the agreed target is SSE2 |
| system/legacy (state save) | 2 | `fxsave`/`fxrstor` belong to the x87/MMX class the target never uses |
| system/legacy | 55 | sysenter/sysexit, monitor/mwait, pause, xsave/xrstor, SVM/VMX, in/out, BCD/bound, lar/lsl/lldt, lret, rsm, rdpmc/rdrand/rdpid, pkru/xgetbv, pdep/pext — never in static-Linux-binary output |
| Real-mode / protected-mode boot sequence | — | Guest starts in long mode (Gap A decision); the legacy path emulates nothing |

**One row is NOT permanent.** The signed/byte divide forms `divb_AL`,
`divw_AX`, `idivb_AL`, `idivl_EAX`, `idivq_EAX`, `idivw_AX` — the M3
integer debt the agreed target's compiler DOES emit (gcc `idiv`). The
classifier returns NULL for these so the halting message says "owed", never
§4.5. The M6 slice also left the SSE2 scalar `maxsd`/`maxss`/`minsd`/`minss`
unwritten (they classify as SSE, owed, and would be a two-line body each if
a fixture demanded them).

The permanent-unsupported list is a *documented output* of M7, not a hidden
assumption: the surviving halting stubs ARE the spec of what this build
does not run. Two mechanisms keep that spec honest and rot-proof:

- **The halting message names the class.** `sls_i386_helper_unimplemented`
  (sls-i386-helper-stubs.c) routes every stub name through
  `sls_i386_stub_class` (sls/sls-i386-stub-class.c) and prints the §4.5
  class: "Class: AVX/AVX-512 (ymm) -- permanent-unsupported (§4.5 of the
  AMD64 plan): the agreed target's compiler output never emits this." No
  stub reads as live-but-is-not — the old "the next thing to write" text is
  gone from the permanent classes, and the owed div/idiv family gets the
  honest "not implemented by this build (owed, not permanent)".
- **The classifier is pinned by a host test.** The classifier is pure C
  with no QEMU headers, so the same file compiles in the kernel AND in
  `tests/unsupported_class_host_test.c` (aerosls2), which asserts 39
  representative names per class plus the NULL family — the two can never
  drift, and the rules were validated against every one of the 628
  survivors.

---

## 5. Milestone order — each gated by a measurement, not an opinion

Every gate is a concrete binary and a concrete result. This is the project's standing
rule (a claim without a measurement is a hypothesis) applied to the plan itself.

### M1 — Long-mode entry, paging-on fetch through the shadow. *(the gate in §0)*

- Launcher constructs 64-bit boot state: `CS.L=1/CS.D=0`, `EFER.LME=LMA=1`,
  `CR4.PAE=1`, `CR0.PG=1`, guest CR3 → identity tables the launcher built covering the
  guest RAM window. Decide: initialize the real `CPUX86State` and stop maintaining
  `SLSCPUState` as a parallel struct (one source of truth for the decoder's
  `offsetof`-bound env).
- `sls-i386-codefetch.c`: replace the paging refusal with a real GVA→host resolve
  through the shadow for the launcher-built tables; keep the refusal for foreign CR3
  until M2. Remove the "i386" name's implication by renaming to
  `sls-x86-codefetch.c` or documenting in place — the file is mode-agnostic.
- **Gate:** a `gcc -static -nostdlib` 64-bit fixture entered directly in long mode —
  `movabs rax, imm64; mov rbx, [rip+disp]; mov rcx, [r12+r13*8+disp32]; add rax, rbx;
  add rax, rcx; ret` — runs and the launcher reads the expected rax. Must include REX,
  RIP-relative, SIB, and a 64-bit immediate: proves decode, mode state, addressing, and
  paging-on fetch in one binary.

### M2 — Guest-owned page tables; the fetch+data translation decision record.

- Guest (or its M5 loader) installs its own CR3 tables; fetch **and** TCG data-path
  accesses translate through them via the shadow. Resolve the Address-Space-Design
  (a)/(b)/(c) choice as a written decision record — the COW-clone (a) or world-switch
  (c) route fixes the shared-table defect; the window (b) route needs per-access
  translation. This is the deepest architecture in the plan; the defect doc's §1–§2
  analysis is the starting point.
- **Gate:** a fixture that maps a data page at a high non-identity GVA, writes a known
  value, reads it back, returns the checksum — and the identical fixture with a *bad*
  translation faults cleanly to C (exit, not corruption).

### M3 — Integer/flag helper subset for compiled code.

- Implement the 5 flag helpers + the div/idiv/mul/shift/string subset (§4.2) in the
  helper layer, replacing their halting stubs. Everything else still halts loudly.
- **Gate:** a `-nostdlib` fixture mixing add/adc/sub/cmp/div/idiv/imul/shifts/branches
  and returning a known 64-bit constant — the point where "a compiled 64-bit binary
  runs" stops being a promise and becomes a regression test.

### M4 — System instruction surface + the syscall decision.

- `cpuid` (conservative leaves), `rdtsc` (scaled), `in`/`out` (port model/serial),
  `swapgs` (real base-swap semantics in the env), and `syscall` → **exit-to-C with the
  number+args recorded** (option 1 of Gap D). The launcher reports the intercepted
  syscall and the guest state.
- **Gate:** a fixture executing cpuid + rdtsc + a `syscall` whose interception the
  launcher prints correctly. This proves the full decode→emit→helper→C boundary for
  the one instruction every Linux binary eventually executes.

**Status: CLOSED 2026-08-13 (iterations 12–14).** The compiled guest fixture now
executes all three: `syscall`/`sysret` (iteration 12, the CPL handoff with R11 flags
and RCX→RIP), `cpuid` (iteration 13, the seeded feature leaves LM|SYSCALL/LAHF_LM/
MMX|SSE|SSE2 returned exactly), and `rdtsc` (iteration 14, helper_rdtsc reading the
HOST TSC through lfence; the fixture proves nonzero, monotonic, strictly advancing
reads with the raw delta packed into the marker's high dword). The launcher's
print/compare block shows all three markers green on a single end-to-end run.

### M5 — ELF64 loader, guest bootstrap, and the Linux-compat syscall shim.

- `sls-elf64-loader.c`: parse ELF64 (static only — PT_INTERP rejected loudly), place
  segments, zero BSS, build PT_TLS TCB at FS:0, build initial stack with auxv. Long-mode
  entry per M1 covering loaded segments + stack (uses M2's translation).
- Syscall shim (option 3 of Gap D): `write`→serial, `exit`/`exit_group`→launcher,
  `brk`/`mmap`/`munmap`→SLS frames, plus `read`, `futex`, `clock_gettime` as the
  binary demands them (stub-halt discipline: implement what the fixture calls).
- **Gate — the agreed target, in one line:** a freestanding static ELF64 fixture
  (`gcc -static -nostdlib`, no libc and no user space — the payload obeys the same
  constraint as the implementation) prints to serial and exits 0, delivered back to
  the launcher, with its local-exec TLS reads resolving through the loader-built TCB.
  This is the milestone the whole plan exists for; M1–M4 are its prerequisites,
  and everything after is depth.

**Status: the loader milestone landed 2026-08-13 (iteration 15).** `sls-elf64-loader.c`
parses a static ELF64 (magic/class/endian/machine validated, ET_EXEC only), rejects
PT_INTERP loudly *before* any segment is placed, loads each PT_LOAD at its p_vaddr
(paging off: guest linear == guest physical, so p_vaddr IS the GPA), zeroes the BSS
tail (p_memsz − p_filesz) with tcache flush, and builds the System V initial stack:
argc/argv/envp plus the auxv vector (AT_PHDR/AT_PHENT/AT_PHNUM/AT_PAGESZ/AT_ENTRY/
AT_UID/AT_GID/AT_RANDOM/AT_EXECFN) with the strings and 16 AT_RANDOM bytes above the
pointer array, 16-aligned RSP. The M5 syscall shim (Gap D option 3) lives in
helper_syscall: a guest that never installed LSTAR is a Linux binary asking this build
to be its kernel — `write`→serial (fd 1/2, up to 512 bytes), `exit`/`exit_group`→the
launcher with the status in sls_last_guest_exit_code, anything else halts naming the
number. The gate fixture (sls/guest/hello.c, `gcc -static -nostdlib`, embedded as
hello-bytes.h) checks .data round-trip and .bss-zeroing internally, writes "hello from
the ELF loader" to serial, and exits 0; the companion dynhello fixture proves PT_INTERP
rejection. Both endpoints green in a single boot (22 guest insns, exit_code=0, and the
reject case rc=-1 with the interpreter named).

**Update 2026-08-13 (iteration 16): the PT_TLS TCB at FS:0 landed.** The loader's
Pass 3 copies the PT_TLS template (which the linker parks OUTSIDE every PT_LOAD — the
fixture's TLS segment is the canonical case), zeroes the .tbss tail, and builds the
tcbhead_t immediately above the aligned block: tcb=self, a minimal static DTV at
tp+0x100, self=tp, multiple_threads/gscope_flag zeroed, and fixed stack/pointer
guards — the exact spot the linker's local-exec tpoffs assume (TP = tls_vaddr +
round_up(memsz, align)). The launcher threads the loader's thread pointer into the
CPU reset via sls_guest_tls_fs_base and installs it as env.segs[R_FS].base, so every
%fs:offset access resolves from the first translated instruction. Two real defects
surfaced on the way, both of which the -nostdlib fixtures had masked: the loader was
handing the guest a HOST-space initial RSP (hello.c never touched the stack, so its
`push`-free _start ran fine; the TLS fixture's first `push %rbx` faulted at
guest_base + host_va − 8), and the launch-time CPU reset zeroed the entire static CPU
object, silently wiping a caller's pre-set FS base (the decoder then folded FS base 0
and the `mov %fs:-16` local-exec read faulted at guest_base − 16). Both fixed and
verified on hardware: the gate fixture (sls/guest/tls.c, embedded as tls-bytes.h)
checks .tdata reads back its link-time initializer, .tbss reads back zero, the TCB is
self-referential (tcb/self/dt at +0/+8/+0x10, block below TP), and exits 0 — 43 guest
insns, exit_code=0, PASS in the same boot as the elf/elf-reject gates, with the new
/api/qemu/tls endpoint added to the decoder-build CI gate list.

**Update 2026-08-13 (iteration 17): the guest-heap shim landed.** `brk`
(syscall 12), `mmap` (9, the 6-arg R10/R8/R9 form) and `munmap` (11) are real
cases in the M5 shim's helper_syscall. "SLS frames" is a guest-address carve of
the already-mapped 256 MiB emulator window: brk moves a single program break up
from 32 MiB, mmap bumps a page allocator up from 48 MiB, munmap releases an
exact recorded range (a double-unmap is -EINVAL), and both stay below a 1 MiB
stack-margin ceiling. State is per-launch (sls_shim_heap_reset, called beside
the sls_last_* resets), failure returns match Linux's raw-syscall convention
(negative errno in RAX; brk returns the unchanged break). The gate fixture
(sls/guest/brkmmap.c, embedded as brkmmap-bytes.h) grows/shrinks the break with
a pattern round-trip, maps an anonymous private page (zero-read, write,
read-back), unmaps it, maps again, and exits 0 - PASS in the same boot as the
compiled/elf/elf-reject/tls gates, with the new /api/qemu/brkmmap endpoint
added to the decoder-build CI gate list.

**Update 2026-08-13 (iteration 18): read and clock_gettime landed.** `read`
(syscall 0) serves a fixed boot-data stream — this kernel has no input devices
for the guest (the emulator's serial is output-only), so the only source of guest
input is the loader, and here it is bytes, not just addresses (the honest analogue
of AT_RANDOM's fixed pattern); nonzero fds are -EBADF (there are no files).
`clock_gettime` (228) supports CLOCK_MONOTONIC only: the build has no calibrated
TSC-to-wallclock ratio (kernel/auth.h), so the monotonic clock is ticks-since-launch
carried in a timespec - a real, strictly advancing counter, exactly what
CLOCK_MONOTONIC promises; any other clockid is -EINVAL with the reason named (no
wall clock). The gate fixture (sls/guest/rdclock.c, embedded as rdclock-bytes.h)
drains the stream in three reads (head, short-read tail, EOF), proves a nonzero fd
fails, takes two monotonic reads around a spin (sane nsec, non-decreasing total),
proves an unknown clockid fails, and exits 0 - PASS in the same boot as the
compiled/elf/elf-reject/tls/brkmmap gates, with the new /api/qemu/rdclock endpoint
added to the decoder-build CI gate list.

**Update 2026-08-13 (iteration 19): futex landed, closing the M5 shim surface.** `futex`
(202, the 6-arg form: RDI=uaddr, RSI=op, RDX=val, R10=timeout, R8=uaddr2, R9=val3)
models single-context honesty: this kernel runs exactly one execution context and has
no scheduler, so a wake is only ever observable as a value change — the wait loop is
Linux's own: check the word (mismatch = the release already happened -> -EAGAIN),
then poll the word and the tick deadline. WAKE reports the true count (0: no
concurrent waiter can exist), the op's low 7 bits are the command (the PRIVATE bit is
a hint), unknown ops are -EINVAL, and a wait with NO timeout halts naming that it
could never return (no waker can exist) instead of hanging the kernel. The gate
fixture (sls/guest/futex.c, embedded as futex-bytes.h) proves the fast path, the
private-hint equivalence, wake==0, -EINVAL, and a matched-word 20M-tick wait that
really sleeps: -ETIMEDOUT with the monotonic clock advanced by about the requested
interval — PASS in the same boot as the compiled/elf/elf-reject/tls/brkmmap/rdclock
gates, with the new /api/qemu/futex endpoint added to the decoder-build CI gate list.

**The M5 shim surface is now complete.** Every Linux-compat syscall the freestanding
fixtures demand — write, exit/exit_group, brk, mmap, munmap, read, clock_gettime,
futex — is real, verified on hardware, and gated in CI.

**Update 2026-08-13 (iteration 20, M6): the SSE2 scalar slice.** The M1-M5
-mno-sse -msoft-float crutch is dropped: sls/guest/sse2.c builds with the plain
x86-64 recipe (-fno-math-errno so __builtin_sqrt lowers to sqrtsd instead of a
libm call), and every operand is a runtime volatile so gcc emits real SSE2
instructions instead of constant-folding. The decoder already advertised
CPUID SSE/SSE2 and decodes the aligned 128-bit load/store path inline; the
helpers behind the scalar ops are implemented in the guest-helper layer with
host IEEE double math (this build has no softfloat, so the four basic ops +
sqrt of finite values round identically to round-to-nearest-even; MXCSR is
stored and round-tripped but the host FPU stays round-to-nearest — documented,
not silent). The gate fixture verifies the 128-bit aligned constant store (the
movdqa/movaps path that #UD'd under the futex fixture), scalar
add/sub/mul/div/sqrt with exact IEEE results, int<->double converts (32- and
64-bit sources), the float path + float->double widen, ucomisd branches, and
the MXCSR round-trip — PASS in the same boot as the compiled/elf/elf-reject/
tls/brkmmap/rdclock/futex gates, with the new /api/qemu/sse2 endpoint added to
the decoder-build CI gate list. M6's permanent-unsupported remainder (packed
vector, x87, AVX) stays as halting stubs, which become the documented spec of
what this build does not run at M7.

**Update 2026-08-13 (iteration 21, M7): fault semantics delivered to C.** The M7
gate is fault RECORDING: this build has no exception delivery (no IDT, no vectoring),
so a fault ends the launch with the fault class written into the launcher's record
(vector, error code, CR2, faulting RIP -- reset per launch beside the exit code),
distinguishable from a launch that ran to HLT. Four classes are wired and gated by
the faults fixture (sls/guest/faults.c, one entry per fault, launched separately):
ud2 raises #UD(6) through the decoder's gen_illegal_opcode path; DIV r/m64 by zero
raises #DE(0) through helper_divq_EAX; swapgs at CPL 3 (reached via the iteration-12
syscall/sysret round trip) raises #GP(13) through check_cpl0; and a paged guest's
store to an unmapped VA raises #PF(14) -- the shadow walk fails, and the kernel's
handle_page_fault hook (guest active + paging on + in-window) records the fault
with CR2 = the guest VA instead of panicking. All four record correctly in one boot
with the new /api/qemu/faults endpoint added to the decoder-build CI gate list;
the raise_exception body that previously named-and-halted is now the M7 record path,
and the #DE path routes through it instead of a kernel panic. M7's boundary, stated
in the plan: delivery (IDT, iretq, ring transitions) remains a later milestone.

**Update 2026-08-13 (iteration 22, M7): error codes carried in the record.** The
fault-record's error field is no longer silently 0 for the two classes whose
hardware error code is meaningful. #PF already carried the real faulting PTE bits
through the kernel hook (error=0x2 for the fixture's write-to-not-present store,
CPL 0) -- the gate now asserts them. #GP gains a real selector code: the fixture
adds a far call through memory (lcallq *mem, FF /3 -- the only far-call form long
mode allows) to a selector no descriptor table can resolve. This build maintains
no GDT/LDT, so hardware semantics are exactly #GP(selector), and the
helper_lcall_protected/helper_ljmp_protected stubs (previously halting) now
record #GP with the selector pushed as the error code -- the raise_exception_err
path M7 records. swapgs-at-CPL3 remains #GP(0), which is its true hardware code
and is asserted as such.

**Update 2026-08-13 (iteration 23, M8): guest IDT + iretq delivery.** M7's
fault-record was the foundation; this iteration turns a recorded fault into a
DELIVERED one when the guest has installed an IDT. The fixture now installs a
gate per vector (lidt, P bit set), and the fault paths -- helper_raise_exception
(#UD/#GP), the divide-error path (#DE), the far-transfer #GP path, and the
kernel's #PF hook -- first ask whether the guest's IDT has a present gate for
the vector (`sls_i386_try_deliver`). If it does, the fault is captured from the
committed CPU state (the TCG block unwinds via the longjmp, so env is synced)
and the exec loop pushes the hardware 64-bit interrupt frame -- [err]? RIP CS
RFLAGS RSP SS -- at the guest RSP and vectors into the handler (`CS` = the gate
selector, CPL from its RPL). The handler records its vector, stashes the
popped error code (and CR2 for #PF), skips the faulting instruction in the
saved RIP, and iretq's: `helper_iret_protected` pops the frame back (synthetic
flat segments, RPL from the selector -- the sysret approach) and the guest
resumes at the instruction after the fault, at its original CPL, with its
original stack. A gate-less fault still falls through to the M7 record.

Two decoder gaps surfaced and were fixed at the source: `gen_DIV` and
`gen_far_call`/`gen_far_jmp` never synced eip before their helper calls, so the
#DE and #GP-SEL faulting RIPs were block-start addresses -- `gen_update_eip_cur`
now precedes them. The kernel #PF hook's `saved_rip` is a HOST address for
mid-block faults (translating it needs TCG's search_pc machinery, which this
build does not use), so the fixture's #PF handler iretq's to a recovery label
recorded in guest memory rather than skipping a stale rip; the M7 #PF record's
rip field has the same limitation and is not asserted.

The gate: five launches (ud2, div-by-zero, swapgs-at-CPL3, unresolvable
far-call selector, unmapped paged store), each of which must now run to a clean
HLT with no recorded fault, `fault_status[vec]==2` (1 = handler entered, 2 =
recovered after iretq), the exact frame error codes (0, 0, 0, 0x10, 0x2), CR2 =
0x8000000 read by the #PF handler from env->cr[2], and recovery_ok set. The
swapgs-at-CPL3 recovery cannot hlt at CPL 3 (HLT is privileged), so it records
its status, sets a flag, and syscalls back to CPL 0 where the shared syscall
handler halts. All nine gates (compiled, elf, elf-reject, tls, brkmmap,
rdclock, futex, sse2, faults) are green in one boot.

**Update 2026-08-13 (iteration 24, M8.1): the TSS stack switch.** M8's
delivery pushed the frame at the faulting RSP for every delivery; this
iteration makes the privilege-changing case real. A fault at CPL 3 through a
gate whose CS is DPL 0 now switches stacks: `sls_i386_try_deliver` sees gate
RPL < faulting CPL, reads RSP0 from the loaded TSS (env->tr.base + 4), pushes
the frame THERE, and `sls_i386_deliver_fault` loads a null DPL-0 SS -- long-mode
hardware behavior -- so the handler genuinely runs at CPL 0 on the TSS stack
while the frame's RSP/SS slots carry the faulting CPL3 context for the iretq
to restore. The guest side needs a real TR: `helper_ltr` (formerly a halting
stub) now validates an available 64-bit TSS descriptor (S=0, type 9, present,
within the GDT limit) from the guest's GDT through the window, loads
env->tr, and marks the descriptor busy in the GDT -- LGDT was already inline
in translate.c. A privilege-changing delivery with no TSS loaded is recorded
as #TS(selector) (hardware semantics) rather than silently delivered on the
wrong stack, so a fixture that forgets the GDT/TSS fails the gate instead of
passing on a lie. The fixture's `_gp` entry (CPL3 swapgs) now sets up the
GDT/TSS too -- the same path, verified twice -- and the new `_tss` entry runs
the same fault through `tss_gp_handler`, which stashes its own entry RSP and
the frame's CS/RSP/SS. The gate asserts the handler RSP is exactly
TSS.RSP0 - 48 (the six-slot frame with the error code), the frame carries the
CPL3 selectors this build's sysretq produces (CS=0x13, SS=0xb for
STAR=0x00080008, measured identically by both CPL3 fixtures), and the frame
RSP is the loader stack, not the TSS stack.

One latent defect surfaced while wiring this: the M8 commit's faults-test doc
comment had lost its closing `*/` (a review-time sed), silently commenting out
`sls_test_guest_faults` -- the M8 object shipped without the symbol and the
kernel could not have linked the endpoint. It is fixed here (the comment now
closes), and the gate runs the function for real. All nine gates remain green
in one boot, with the faults gate now six cases: ud2, div-by-zero,
swapgs-at-CPL3 (TSS-delivered), the unresolvable far-call selector, the
CPL3->CPL0 TSS stack switch, and the unmapped paged store.

The plan's later milestones -- syscall
interception, and the permanent-unsupported list -- remain, with this delivery
path as their base.

**Update 2026-08-14 (iteration 25, M8.2): the TSS-family faults #TS and #NP
delivered as first-class classes.** M8.1's `helper_ltr` folded a busy TSS into
#GP, and #NP was only ever produced as a record; this iteration makes both
real deliveries with their selector error codes. `helper_ltr` now follows the
SDM exactly: an S=1 descriptor or a non-64-bit-TSS type is #GP(selector); a
BUSY 64-bit TSS (type 11 -- the M8.1 load marks the fixture's GDT[2] busy) is
#TS(selector) (upstream QEMU folds this into #GP; this build diverges toward
hardware); a NOT-present descriptor is #NP(selector) (now via the proper
EXCP0B_NOSEG macro). Two new fixture entries provoke them at CPL 0 and deliver
through guest IDT gates at vectors 10 and 11, mirroring the #GP-SEL flow:
`_ts` calls `setup_tss` (first ltr on 0x10 succeeds) then ltr's 0x10 again --
the descriptor is now type 11, so #TS(0x10); `_np` ltr's a second 64-bit TSS
descriptor the setup builds at GDT[4..5] (selector 0x20) with P=0 -- #NP(0x20).
A 64-bit TSS descriptor spans two GDT slots, so the non-present twin gets its
own 16 bytes rather than reusing GDT[3] (the live TSS's high half). The
decoder fix the faulting-RIP needed: `gen_LTR` never synced eip before the
helper call, so a faulting ltr recorded the block-start address -- the same
gap M8 closed for gen_DIV and the far transfers; `gen_update_eip_cur` now
precedes `gen_helper_ltr`, and the deliveries show the exact ltr addresses
(0x401609 / 0x401679). The gate asserts the frame error codes (0x10 for #TS,
0x20 for #NP), handler status 2, and recovery_ok -- all nine fixtures green in
one boot, the faults gate now eight cases.

**Update 2026-08-14 (iteration 26, M8.3): the double-fault #DF and the
stack fault #SS delivered as first-class classes.** The fault delivery path
gains Table 6-5's second-exception rules, and the last two hardware fault
classes of the M8 list become real deliveries with their exact error-code
semantics.

The mechanism: a `sls_delivery_in_progress` window, armed by
`sls_i386_try_deliver` the moment a delivery is decided and cleared by
`sls_i386_deliver_fault` when the frame is on the stack -- exactly the
delivery mechanism (gate fetch, TSS read, frame push), never the handler.
`sls_i386_fault_record` (the chokepoint every raise path funnels through)
now applies Table 6-5: a second exception inside the window that is #DF is a
TRIPLE FAULT (recorded and halted); #NP/#SS are masked (the original
delivery proceeds); #PF nests (the page-fault handler runs, the original is
abandoned); every other vector escalates to #DF(8) with error code 0. #DF
pushes an error code that is ALWAYS zero (Intel 386 manual §9.8; QEMU's
`exception_has_error_code` lists 8), which the fixture asserts.

That escalation exposed an M8.1 defect: the CPL3->CPL0 delivery with no TSS
loaded previously RECORDED #TS(selector) and halted, but the #TS is raised
while the delivery is pending, so Table 6-5 escalates it to #DF. The `_df`
fixture proves the whole chain: at CPL 3 (the syscall/sysret round trip) the
guest ud2's through a DPL-0 gate with no TSS loaded; the delivery's #TS
escalates to #DF(8); the #DF gate's CS is the CPL3 selector 0x1b, so the #DF
itself delivers same-CPL on the CPL3 stack (no TSS is needed -- or
available); the handler pops the always-zero error code, skips the ud2, and
iretq's back to the CPL3 recovery. The serial record shows the chain: "the
delivery's #TS(0x8) escalates to #DF(8) (Table 6-5)" then "delivery: vector
8 err=0x0 via IDT gate 0x001b ... frame from rip=0x401972" -- the exact ud2.

The #SS side: `helper_load_seg` (MOV Sreg under the decoder) was a halting
stub; it now has a real body (seg_helper.c's, with the descriptor read
through the guest window like `helper_ltr` and the faults routed through the
delivery path). The SDM's asymmetry is the trigger: a NOT-present SS
descriptor is #SS(selector) -- NOT #NP, which is the DS/ES/FS/GS case. The
`_ss` fixture loads a not-present writable data descriptor the setup builds
at GDT[6] (selector 0x30): `mov %eax,%ss` (8e d0) -> #SS(0x30), the
selector as the error code. `gen_movl_seg` now syncs eip before the helper
call -- the same gap M8.2 closed for gen_LTR -- so the delivered frame's RIP
is the exact mov (0x401759), and the handler's skip lands.

Two supporting fixes the new cases surfaced. First, a genuine launch-order
bug: the `_pf` case leaves guest paging enabled (the window's identity
mapping swapped for the guest's own tables), and `sls_elf64_load` runs
BEFORE `sls_launch_guest_at`'s paging reset -- so a load following a
paging-on launch faulted through the stale tables and kernel-#PF'd. The
loader now resets the guest address space itself (idempotent, no-op when
paging is off), the same rule launch_guest_at applies. Second, the
frontend-off (shipping) build's `compiled`/`elf` fixtures fail on the clean
tree with "unimplemented opcode 0xf3": the legacy C frontend never handled
REP/CET prefix bytes that the current fixture images emit (endbr64, rep
movsb), a pre-existing gap invisible to CI and deploy (neither exercises
those endpoints on the shipping build) -- recorded here, not fixed in this
iteration.

The gate: all ten fixtures green in one boot -- the faults gate is now ten
cases, #DF delivering err=0 (the always-zero double-fault error code) and
#SS delivering err=0x30 (the selector), each through a guest IDT gate with
the handler recovering to a clean HLT. The full M1-M8.2 regression
(compiled elf elf-reject tls brkmmap rdclock futex sse2 faults) passes on
the decoder build.

**Update 2026-08-14 (iteration 27, M8.4): the triple fault as a recorded
class -- and the recursion it exposed.** Designing the _tft fixture found a
real defect in iteration 26's escalation: a DPL-0 #DF gate with no TSS
loaded would NOT have triple-faulted -- the #DF's own delivery faults (the
delivery's #TS again), escalates again, calls try_deliver(8) again, forever.
A stack-overflow crash in the host kernel, not a shutdown.

The fix is Table 6-5 read precisely. Escalating to #DF is only valid when
the delivery already in progress is NOT the #DF's own; a fault raised while
delivering #DF is the triple fault (hardware shuts the machine down). The
escalation branch now checks `sls_pending_vector == EXCP08_DBLE` and, when
true, records the triple fault instead of escalating. The record (the
machine's "shutdown"): vector 8, error 0, the current RIP, exit code -1 --
no delivery attempt, because the guest's own IDT can no longer help. The
record logic is factored into `sls_i386_triple_fault`, shared by the direct
"#DF raised while a delivery is pending" row of Table 6-5 and the new
"fault raised while delivering #DF" case.

The `_tft` fixture proves the shutdown end to end: the same CPL3 ud2 entry
as `_df` (no TSS loaded), but the #DF gate's CS is KERNEL_CS (DPL 0), so
the escalated #DF's OWN delivery also needs the TSS -- the second
escalation is the triple fault. The serial record shows the full chain and
the new halt: the #UD delivery's #TS escalates, the #DF delivery's #TS
escalates, then "M8.4: TRIPLE FAULT -- fault raised while delivering #DF.
Halting (a real CPU shuts down here)." The launcher gate asserts the
shutdown shape: the launch ends halted with fault_vector 8 and error 0, and
the #DF handler NEVER ran (fault_status[8] stays 0, recovery_ok stays 0) --
proving the delivery was abandoned, not completed. The faults gate is now
eleven cases; the full M1-M8.3 regression stays green on the decoder build,
and the frontend-off (shipping) build still compiles and links.

**Update 2026-08-14 (iteration 28, M8.5): the nested #PF -- Table 6-5's
"second exception is #PF" row -- is now a delivered, gated class.** The row
already existed in the `sls_i386_fault_record` chokepoint (the M8.3
table), but it was unreachable from the one path that can actually raise a
#PF inside the delivery window: the kernel's guest-#PF hook. `sls_i386_guest_pf`
(sls-launcher.c) called `try_deliver(14)` directly, so a frame-push fault
during a delivery silently overwrote the pending state without the nested
row's bookkeeping. The hook now routes through a new
`sls_i386_raise_guest_pf` (sls-i386-helper-stubs.c) that funnels into the
SAME Table 6-5 chokepoint as the helper path -- the fix that made the row
reachable at all.

The `_pf_nested` fixture provokes the row with hardware's own mechanism:
the guest runs PAGED (the `_pf` tables: identity 2 MiB pages for the
code/.bss window and the loader stack, everything else unmapped), at CPL 3
via the syscall round trip, and swapgs raises #GP(0) through a DPL-0 gate.
The gate's delivery switches to TSS.RSP0 -- which the fixture points at the
top of an UNMAPPED page (0x0C000000). The delivery's first frame push (SS
at 0x0BFFFFF8) faults in the kernel's window walk, and the hook raises
guest #PF (error 0x2 -- write, not-present -- CR2 0x0BFFFFF8) WHILE the
#GP delivery is pending. Table 6-5 nests it: the DPL-3 #PF gate (CS 0x1b)
delivers same-CPL at the faulting CPL3 RSP (the only mapped stack
available -- the TSS.RSP0 the original delivery was pushing at is
deliberately unmapped), the `pf_handler` runs, iretq's to a recovery label,
and the guest halts cleanly. The launcher gate asserts the row's shape:
`status[14]=2` (the #PF handler ran and recovered), err=0x2 and
CR2=0x0BFFFFF8 came back exactly, recovery_ok=1 -- and, the row's point,
`status[13]=0`: the ORIGINAL #GP handler never ran, because `try_deliver`
overwrote the pending delivery. The serial record shows the whole chain:
the #GP delivery's `deliver-rsp=0xc000000`, the hook's
`guest #PF: cr2=0xbfffff8`, the nested row's log line, and the #PF's
same-CPL delivery through gate 0x1b.

The faults gate is now twelve cases, all green in one boot; the full
M1-M8.4 regression (compiled elf elf-reject tls brkmmap rdclock futex sse2
faults) passes on the decoder build, and the frontend-off (shipping) build
still compiles and links (the launcher hook change links in both configs).
The Table 6-5 matrix is complete: every row -- masked #NP/#SS, nested #PF,
escalation to #DF, the #DF-own-delivery triple fault -- now has a fixture
behind it, delivered through guest IDT gates on the decoder path.

**Update 2026-08-14 (iteration 29, M7.5): the §4.5 permanent-unsupported
list is now WRITTEN — measured, class-named, and machine-checked.** M7's
documented output (the plan's §4.5, "the surviving halting stubs are the
spec of what this build does not run") was counts, not names. Three
deliverables land it:

1. **The measured census.** `sls/gen_unsupported_list.py` (qemu-sls)
   derives the survivor universe from the same sources the kernel links
   (helper.h + ops_sse_header.h.inc minus the real-body renames): 676
   declared, 48 real, 628 surviving stubs — 131 AVX/AVX-512 (ymm), 176 SSE
   vector (xmm), 135 MMX, 80 x87, 19 3DNow!, 10 SHA, 6 MPX, 8 AVX-FMA, 57
   system/legacy (+2 state-save), regenerable at any commit. The plan's
   "~600" estimate is now a number that cannot rot.
2. **Honest halting.** `sls_i386_helper_unimplemented` now prints the §4.5
   class for every permanent stub ("Class: x87 FPU -- permanent-unsupported
   (§4.5)") instead of the old "the next thing to write", which read as
   live for classes the agreed target never emits. The one genuinely-owed
   family — the signed/byte divides `divb_AL`..`idivq_EAX`, the M3 integer
   debt — classifies NULL and gets "owed, not permanent". The audit also
   found the M6 slice left SSE2-scalar `maxsd`/`maxss`/`minsd`/`minss`
   unwritten (classified SSE, owed — two-line bodies if a fixture demands
   them).
3. **A pin.** The classifier is pure C (sls/sls-i386-stub-class.c, no QEMU
   headers), compiled both into the kernel and into
   `tests/unsupported_class_host_test.c`, which asserts 39 representative
   names per class + the NULL family — 96/96 host checks green, the rules
   validated against all 628 survivors. The decoder build links and boots
   the full M1–M8.5 gate green; the frontend-off build is untouched (the
   classifier object is frontend-only).

**Update 2026-08-14 (iteration 30, M6.5): the SSE2-scalar min/max gap the
iteration-29 audit flagged is CLOSED, and the census now measures 621
survivors.** The §4.5 audit named `maxsd`/`minsd`/`maxss`/`minss` as
"owed, not permanent" — the M6 slice had left them unwritten. The sse2
fixture (sls/guest/sse2.c) now drives them through the `<emmintrin.h>`
intrinsics (`_mm_max_sd` etc., which ARE the instructions — gcc will not
lower `a > b ? a : b` to maxsd without fast-math), plus the first
compare-family member: `cmpeqsd` read back through `movmskpd` (the
`_mm_cmpeq_sd` all-ones/all-zeros mask via `_mm_movemask_pd`), and a
NaN-aware fmax probe that stays libm-free via `__builtin_isnan`
(ucomisd + branch — ucomisd is real).

1. **Five new real helpers** (sls/sls-i386-helper-stubs.c): the min/max
   macro bodies keep all double arithmetic in locals — the kernel builds
   `-mno-sse`, so no helper may RETURN a double through XMM0, the exact
   ABI rule the M6 section states (the comparison is inlined into the
   macro rather than factored into a static function for that reason).
   `cmpeqsd` is the direct `==` with the upper lane UNCHANGED per the
   SDM (the intrinsic's mask read depends on it); `movmskpd_xmm/_ymm`
   extract the sign bits to EAX (int32, no SSE crossing) — the ymm form
   was surfaced by the rename covering the vex_l decode path too. The
   fixture's `-fno-math-errno` build flag is documented in the file
   header (it is what lets `__builtin_sqrt` inline instead of calling a
   libm that a `-nostdlib` guest does not have).
2. **A generator bug the new renames exposed.** `#define movmskpd` is a
   glue-BASE rename — in the stubs TU it lands on `glue(movmskpd,
   SUFFIX)`, killing the generated stub for movmskpd_mmx/xmm/ymm alike,
   but `gen_unsupported_list.py`'s `real_bodies()` subtracted only the
   bare token, so the census kept counting the two now-real bodies as
   halting stubs. `real_bodies()` now expands glue-base renames into
   their suffixed forms (only when the suffixed forms exist in the
   universe; cmpeqsd — whose CMP family is not enumerated — subtracts
   nothing, which is correct since it was never a row).
3. **The measured result:** 676 declared, 57 real bodies, **621 surviving
   halting stubs** (the seven new helpers — 4 min/max + movmskpd_xmm +
   movmskpd_ymm + the renamed-away movmskpd_mmx — leave the survivor
   set). The sse2 fixture runs 241 instructions and the gate passes:
   the full M1–M8.5 corpus (compiled elf elf-reject tls brkmmap rdclock
   futex sse2 faults) is 9/9 green in one boot on the decoder build, the
   frontend-off build still compiles and links, and the host suite stays
   96/96. The one SSE compare family member the slice has not taken is
   the vector (non-scalar) cmp forms — still §4.5-owed, still classified.

### M6 — SSE2 scalar slice.

- xmm register state in the env, the §4.3 instruction set, mxcsr flag helpers.
  Fixtures for M1–M5 may use `-mno-sse -msoft-float`; M6 removes that crutch.
- **Gate:** a `-nostdlib` double-math fixture (add/sub/mul/div/sqrt/compare/convert
  chain) compiled with default `-O2` returns the known value. Cross-check the same
  value on the host to pin the result.

### M7 — Fault semantics + hardening + the permanent-unsupported list.

- `#UD` on genuinely undefined encodings, `#GP` on canonical violations and bad
  segment state, `#PF` with guest CR2 set, `#DE` on div-by-zero — each delivered to C
  (exit with the fault recorded) per the launcher's hybrid model.
- The surviving halting stubs become the **written** permanent-unsupported list
  (§4.5), each with its reason. No stub left that reads as live but is not.
- **Gate:** a fixture that deliberately executes `ud2` and a div-by-zero; the launcher
  reports the correct fault class, not a hang or corruption. Plus: the full M1–M6
  corpus re-runs green (the start of the regression suite).

### M8 — Retire the 18-opcode frontend; flip the default build; measure.

- Step 6.5's decision, now forced by §3: retire `sls-x86-frontend.c` as a frontend
  (keep only as a documented fixture if the C-dispatcher tests need it). `make x86-iso`
  runs the real decoder without a flag.
- Measure image size (`readelf -lW … memsz` — Step 6's footprint warning: the image is
  already 221.6 MiB, TCG ~36 MiB), and verify the Phase 2 persistent tcache
  (`AeroSLS-QEMU-SLS-Phase2-Persistent-Translation-Plan-v0.1.md`) round-trips 64-bit
  TBs across a reboot.
- **Gate:** a fresh `x86-iso` boots, the M1–M7 corpus passes on the default build, and
  the size delta is recorded as a number.

---

## 6. Risks and honest unknowns

- **The shadow-walk completeness is the whole project and it is unverified.**
  `qemu_sls_mmu.c` is real (107 host-test checks) but the Repositioning plan records
  `map_guest_ram` as "currently halts a node silently" and shadow paging as
  UNVERIFIED. M1/M2 are the first time the shadow is load-bearing for *instruction
  fetch* — the highest-stakes code path in the kernel. If the shared-table defect or a
  walk bug shows up under real guest tables, it will look like misdecoded instructions
  or corrupted kernel memory, which is the most expensive failure mode this project
  has paid for. The M2 decision record must be written before the walk is extended,
  not after.
- **`CPUX86State` entanglement.** Step 6.1 proved the decoder parses against a shim;
  feeding it a *correctly initialized* full `CPUX86State` is a different claim. If the
  struct resists initialization outside QOM lifecycle, M1's "one source of truth"
  decision gets larger.
- **The helper subset estimate is an estimate.** §4.2's "≈30–60" is derived from the
  census plus what gcc emits; the real list is whatever the M3/M5 fixtures halt on.
  That is why the milestones are binary-gated rather than count-gated.
- **The shim surface is fixture-driven, not libc-driven.** There is no libc; the
  shim grows only by what the freestanding fixtures call — same discipline as
  6.4. The risk note that motivated this row is now moot by construction: no
  `__libc_start_main`, no hidden `brk`/`futex`/`clock_gettime` demand.
- **The market honesty note (from the Repositioning plan, unchanged):** this frontend's
  value is x86-64 guests on *non-x86* hosts. On x86 hosts, hardware virtualization
  owns the row. A complete AMD64 frontend on x86-only hardware demonstrates the
  technique; it does not reach the market the Repositioning plan identified. The ARM64
  host port is the independent track that completes the story — this plan is
  deliberately frontend-only and does not pretend otherwise.
- **"No user space or glibc" is one constraint, on both sides of the boundary.**
  The emulator is freestanding in-kernel code (as every `sls/` file already is), and
  the guest binaries the frontend runs are freestanding too — `gcc -static
  -nostdlib` fixtures compiled against the bare ABI, not a libc. A single-level storage
  architecture has no libc to host a libc-linked payload: the loader rejects
  PT_INTERP loudly for exactly this reason. This plan never changes that; glibc's
  runtime (its own TLS setup, `__libc_start_main`, its syscall envelope) is the kind
  of user space the architecture refuses, and the guest side stays on the same diet.

---

## 7. What this plan does not cover (deliberately)

- The ARM64/RISC-V *host* ports (separate track; §6).
- Real-mode/protected-mode boot emulation (Gap A decision — guests start in long mode).
- The SIMI layer and its x86-64 translator (`tools/simi/simi_x86.c`) — that is the
  SLIC-vs-native story, orthogonal to guest emulation.
- AVX/AVX-512, x87, MMX (permanent-unsupported or post-M7 depth; §4.5).
- Multi-vCPU guest execution (the launcher is single-vCPU by design; SCOPE.h CATEGORY 4
  elides locking for that reason, and M1–M8 keep it true).
