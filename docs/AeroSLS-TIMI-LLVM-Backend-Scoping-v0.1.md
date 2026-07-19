# AeroSLS TIMI: LLVM Backend Scoping — v0.1 (future work, not started)

**Status:** Design sketch only. Nothing in this document is implemented. Written in response to "would LLVM be a better fit for TIMI/SLIC" — the answer for the in-kernel translator was no (see rationale below); this document scopes the one place LLVM *could* plausibly help: giving TIMI a real compiler front end where today it has none.

---

## 1. The gap this addresses

Per `docs/AeroSLS-Gap-Analysis-v0.1.md`: **no compiler exists that targets TIMI.** Every `.timi` file in the repo is hand-written assembly, assembled by `tools/timi/timi-asm`. There is no path from C (or any higher-level source) to a `.timi` object today.

This document is *not* about running LLVM inside the kernel. TIMI's in-kernel translators (`kernel/timi_x86.c`, and the host-only `tools/timi/timi_riscv.c`) stay exactly as they are — freestanding, small, auditable, and already doing the one job they need to do. This is about a **standalone, host-side tool** that consumes LLVM IR and emits `.timi` object files in the exact format `timi-asm` already produces — a second front end, changing nothing downstream.

---

## 2. Two ways to build this, and why one is much more tractable here

**Option A — a real LLVM target backend** (`llvm/lib/Target/TIMI/`, TableGen instruction definitions, SelectionDAG or GlobalISel instruction selection, `llc -march=timi`, full integration as an LLVM target triple). This is the "proper" way or building an LLVM target, and the way to get it as `clang --target=timi-unknown-none`. It is also a genuinely large project — comparable in scope to standing up an out-of-tree LLVM backend for any real ISA (RISC-V's backend started this way and took a dedicated team a long time to reach production quality). Not recommended as a starting point.

**Option B — a standalone LLVM-IR-to-TIMI walker**, using LLVM only as a C-front-end-plus-optimizer library (`clang -S -emit-llvm` to produce `.ll`/`.bc`, then a small custom tool links against `LLVMCore`/`LLVMBitReader` and walks the IR directly, emitting TIMI opcodes). This skips SelectionDAG, TableGen, and everything LLVM's target-backend machinery is built for, in exchange for writing the IR→TIMI lowering by hand — which, per §3 below, turns out to be a much smaller job for TIMI specifically than it would be for a normal hardware target. **This is the recommended starting point.**

---

## 3. Why TIMI specifically makes Option B unusually tractable

Three properties of TIMI's actual design (confirmed by reading the ISA doc and the real translator code, not assumed) each remove a normally-hard part of writing a backend:

- **No real register allocation to replicate.** Both existing native translators (`kernel/timi_x86.c`, `tools/timi/timi_riscv.c`) do zero register allocation today — every one of TIMI's symbolic registers is simply an rbp-relative (or RV64 equivalent) stack slot, loaded before use and stored after ("naive load-operate-store codegen," `docs/AeroSLS-TIMI-ISA-v0.1.md` §10). This means an LLVM front end doesn't need a real allocator either — it only needs to rename each LLVM SSA value to a TIMI symbolic register 1:1 (TIMI supports up to 1024 per procedure via its 10-bit register fields). Real allocation, if ever wanted, is future work on the *translator* side (already flagged as such in the ISA doc, independent of this project), not something the LLVM front end is on the hook for.
- **A three-address, fixed-width instruction shape LLVM IR already resembles.** TIMI's design doc explicitly modeled the instruction *shape* on PL.8's W-code — "closer to LLVM IR... than a stack machine" — deliberately. Most of LLVM IR's core scalar instruction set (`add`, `sub`, `icmp`, `br`, `load`, `store`, `getelementptr` with constant offsets, `call`) has a near-direct one- or two-instruction TIMI equivalent already defined (§4 of the ISA doc: `ADD SUB MUL DIV MOD AND OR XOR NOT SHL SHR SAR NEG MOV LOADI LOADI64 CMP BR BC CALL RET LOAD STORE LEA PTRADD`, 27 opcodes, plus 3 object-typed opcodes added in Phase 6 — `RESOLVE OBJSIZE OBJTYPE` — 30 total, all in a 64-bit fixed encoding: 8-bit opcode, 4-bit type tag, 10-bit rD, 10-bit rA, 28-bit rB/immediate, 4-bit flags).
- **The activation cache is completely provenance-blind.** `kernel/timi_translate.c`'s activation cache keys purely on an FNV-1a hash + byte size of the compiled `.tmo` object's raw bytes (`tt_find_activation()`), and nothing in the translate path inspects debug info, compiler identity, or any other metadata. A `.tmo` file produced by this tool is, to every downstream system (`loader.c`, both native translators, the activation cache, the HTTP/shell introspection routes from Gap Remediation Phase G), indistinguishable from one produced by `timi-asm`. Zero downstream changes required.

What Option B does **not** get for free — named honestly, not glossed over — is discussed in §6.

---

## 4. Architecture sketch

A new host-side tool, tentatively `tools/timi/llvm-timi-cc` (name TBD), structured as roughly four passes over one `clang`-emitted LLVM `Module`:

1. **Normalize.** Run a small, fixed set of standard LLVM passes before touching TIMI at all: `mem2reg` (guarantees SSA form for anything not already `alloca`-free), `instcombine`/`simplifycfg` (conservative canonicalization only — see §7 on why the full -O2 pipeline is explicitly *not* used), and a legalization pass that rejects (with a clear diagnostic, not a silent miscompile) any IR construct with no TIMI equivalent yet (vectors, floating point, `invoke`/exception-handling terminators, variadic calls, large aggregates passed by value).
2. **Eliminate PHIs.** TIMI has no PHI-equivalent construct (confirmed: no SSA/PHI notion anywhere in `timi_isa.h` or the interpreter; TIMI registers are RISC-like and freely reassignable, not single-assignment). This pass runs standard out-of-SSA lowering — critical-edge splitting followed by copy insertion at each predecessor block for every PHI node (the same well-known algorithm LLVM's own `-reg2mem`/`DemoteRegToStack` approximates, though a purpose-built pass emitting TIMI `MOV`s directly is cleaner than round-tripping through `alloca`).
3. **Lower instruction-by-instruction.** Walk each basic block post-PHI-elimination; each LLVM SSA value gets a TIMI symbolic register (simple bump allocation per function, reset at each `ENTER` per §3's register-scoping rule); each instruction lowers to one or a short fixed sequence of TIMI opcodes from the existing 30-opcode set. Basic-block boundaries become TIMI's `BR`/`BC` targets.
4. **Emit.** Serialize to the exact `.tmo` binary layout `timi-asm`/`timi_isa.h`'s `TimiObject` struct defines (instruction array, literal pool, entry-point table, name pool) — reusing `tools/timi/timi_obj.c`'s existing object-writing code directly rather than reimplementing it, so the output path is shared with the hand-assembler and can't drift from the format the translators actually parse.

---

## 5. Phased plan (mirroring this roadmap's own phase convention — not started, not scheduled)

- **Phase 1 — feasibility spike.** Hand-write a few small `.ll` files (start from C sources whose hand-assembled `.timi` equivalents already exist as test fixtures, e.g. `tools/timi/tests/add.timi`'s source shape) and manually trace what `mem2reg`+`instcombine`-normalized IR looks like for them. Confirm the mapping in §3 holds for real `clang`-emitted IR, not just idealized examples. Enumerate every IR construct actually encountered that has no TIMI opcode yet.
- **Phase 2 — minimal integer-only subset.** Build the four-pass tool above against a deliberately small C subset: no floats (TIMI doesn't have them — see below), no structs passed by value, no varargs, no function pointers, no object-typed operations yet. Target: round-trip a handful of small integer/pointer functions through `clang -S -emit-llvm` → this tool → real TIMI translator (x86) → run, and get identical results to the hand-assembled equivalents.
- **Phase 3 — PHI elimination + control flow correctness.** Loops, nested conditionals, `switch` lowering (TIMI has no jump-table opcode — `switch` lowers to a `CMP`/`BC` chain) — enough real control flow to trust the PHI-elimination pass isn't just working on toy straight-line examples.
- **Phase 4 — calling convention / ABI mapping.** TIMI's `r0`–`r7` argument convention (§4.8 of the ISA doc) is documentation-only, not hardware-enforced — the LLVM side needs to actually honor it when lowering `call`/`ret`, including the >8-argument frame-memory spill path.
- **Phase 5 — object-typed operations.** Wire `RESOLVE`/`OBJSIZE`/`OBJTYPE` behind an explicit compiler intrinsic (not general C pointer syntax — see §6) so source code can name and query live SLS objects.
- **Phase 6 — floats.** Blocked on TIMI itself gaining float opcodes first (still "confirmed deferred" per the ISA doc, type tags reserved but unimplemented in every existing consumer) — this phase can't start until that's a separate, prior piece of work, not something this tool can front-run.

Each phase would get its own host-test-style verification pass, matching this project's established practice throughout Phases B–H of the main gap-remediation roadmap.

---

## 6. Open design questions, named honestly rather than assumed away

- **Capability tags don't have a C syntax to hang off of.** TIMI's Phase 7 capability model is a per-register shadow tag (`RESOLVE` sets it, `MOV` propagates it, every other register-writing opcode clears it) — there's nothing in ordinary C's type system that expresses "this value is a live, unforgeable object capability, don't let arbitrary arithmetic silently launder it into a fake one." Precedent exists for exactly this problem (CHERI-C adds `__capability`-qualified pointer types and dedicated builtins rather than trying to make plain C pointer arithmetic capability-safe) — the realistic path here is a compiler builtin (`__timi_resolve("name")` returning an opaque handle type) rather than trying to infer capability-ness from normal pointer code. This needs real design work, not just implementation.
- **`switch` and other multi-way control flow** have no direct TIMI opcode (no jump table) — every case lowers to a `CMP`/`BC` chain, which is correct but means large `switch` statements produce linear, not O(1), dispatch code. Acceptable for v1, worth flagging as a real (if minor) codegen-quality gap versus what a hardware `switch` opcode or a full LLVM target's jump-table lowering would give you.
- **Aggregates (structs/arrays) passed or returned by value** need a lowering convention (SLS-pointer-based, presumably, matching TIMI's existing SLS-native memory model) that doesn't exist yet and isn't a small addition — scoped out of Phase 2 deliberately rather than hand-waved into it.
- **What "conservative optimization only" actually means** needs a real policy, not just a phrase: which LLVM passes are safe to run before TIMI lowering without producing IR shapes the walker doesn't recognize yet is itself something Phase 1's feasibility spike needs to nail down empirically, not assume.

---

## 7. Explicitly out of scope for v1

Full C++ (exceptions, RTTI, virtual dispatch), any vector/SIMD lowering (no TIMI vector opcodes exist), relying on LLVM's full `-O2`/`-O3` pipeline (deliberately conservative passes only, so the IR shapes this tool has to handle stay predictable and auditable — consistent with this project's own preference for small, reviewable translation steps over black-box optimization, the same reasoning that ruled out embedding LLVM's JIT in the kernel in the first place), and RISC-V output (`kernel/timi_riscv.c` doesn't exist yet — only the host-only `tools/timi/timi_riscv.c` verify harness does; the kernel has no process/loader/object-catalog wiring for RV64 at all per the ISA doc's §12 findings, so x86-64 is the only currently-meaningful target for this tool's output regardless of what LLVM itself supports).

---

## 8. Honest effort sizing

This is a genuinely large project even scoped down to Option B — realistically weeks-to-months of focused work to reach Phase 2's "small integer functions round-trip correctly" milestone alone, before PHI elimination, real control flow, ABI handling, or object-typed operations are even attempted. It is additive and low-risk to the existing codebase (a new, isolated tool; zero changes to anything already shipped and host-tested through Phase H), but it should not be scheduled as if it were comparable in size to any single phase of the gap-remediation roadmap completed so far. Recommend treating Phase 1 (feasibility spike) as a standalone, cheap first step whenever this becomes a priority — it's pure investigation, produces no code that needs to ship, and would either validate or meaningfully revise every phase estimate above before committing to Phase 2's real implementation work.

---

## 9. Interaction with the existing toolchain (or lack thereof)

Nothing downstream changes. `timi-asm`, `timi-dis`, `timi-run` (interpreter), `kernel/timi_x86.c`, `kernel/timi_translate.c`'s activation cache, `loader_timi_info_query()`, and the Gap Remediation Phase G `GET /api/timi/<name>` route all operate on `.tmo` object bytes with zero awareness of what produced them. This tool would sit entirely alongside the existing hand-assembler as a second, optional way to produce the same object format — never a replacement, and never a dependency anything else in the kernel or toolchain needs to take on.
