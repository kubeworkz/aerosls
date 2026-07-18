# TIMI host toolchain

Host-side (Phase 1, 3, 5) tooling for AeroSLS's TIMI bytecode — the
translated-not-interpreted virtual ISA that SLIC compiles into native code,
modeled on IBM i's TIMI/SLIC/LPAR architecture. Full design rationale is in
`../../docs/AeroSLS-TIMI-ISA-v0.1.md`.

This directory is self-contained and builds with a plain host `cc` — no
cross-compiler needed. It's how you assemble, run, disassemble, and test
`.timi` programs against two different native targets without booting the
kernel.

## Build

```
cd tools/timi
make               # builds timi-asm, timi-run, timi-dis, timi-jit-test, timi-riscv-verify
make test          # assembles + interprets every tests/*.timi, checks expected results
make test-native   # assembles + translates to real x86-64 + executes natively, checks results
make test-riscv    # assembles + translates to RV64 + executes via a mini RV64 executor, checks results
```

## Tools

- `timi-asm <file>.timi` — two-pass assembler, emits a `.tmo` object file.
- `timi-run <file>.tmo` — reference interpreter (Phase 1). Simplified but
  unambiguous semantics: no width truncation, floats unimplemented, LOAD/STORE
  against a simulated 64KB memory array.
- `timi-dis <file>.tmo` — disassembler, resolves branch/call targets back to
  labels.
- `timi-jit-test <file>.tmo <entry> <expected>` — Phase 3 harness: translates
  the object to real x86-64 machine code via `timi_x86.c`, mmaps it
  executable, calls it through a function pointer, and checks the return
  value against `<expected>`. This is the same encoder the kernel uses.
- `timi-riscv-verify <file>.tmo <entry> <expected>` — Phase 5 harness:
  translates the object to RV64 machine code via `timi_riscv.c`, then
  executes it via `rv64_exec.c`, a small purpose-built RV64 decoder+
  executor — not real hardware or QEMU, neither is available in this
  project's dev sandbox (no riscv64 cross-compiler, no qemu-riscv64/
  qemu-system-riscv64, no root to install either). Deliberately not named
  "jit test" like its x86 counterpart, since it isn't one — see
  `timi_riscv_verify.c`'s top comment and §12 of the design doc for what
  that does and doesn't prove.

## Two native targets, one bytecode format

`timi-asm` produces the same `.tmo` regardless of which target you're
translating for — `timi_x86.c` and `timi_riscv.c` both consume the exact
same object format (`timi_isa.h`'s on-disk layout). Running the same
`tests/*.tmo` files through both `make test-native` and `make test-riscv`
and getting identical results is the concrete proof behind the roadmap's
Phase 5 claim ("second native target, to prove re-translation-on-migration
actually works") — not just an assertion that retargeting should work.

## Object-typed operations (v0.3, Phase 6)

`RESOLVE rD, "name"` / `OBJSIZE rD, rA, TYPE` / `OBJTYPE rD, rA, TYPE` look up
a live SLS object by name and introspect it (base address, byte size, object
type tag — a `T_OBJREF`-typed result, 0/0xFFFFFFFF sentinel on failure). The
`.tmo` format grew a fourth pool (object names, referenced by `RESOLVE`'s
operand) and the header a fifth field (`num_names`) — a breaking change from
v0.2, see `timi_isa.h`'s top comment.

These are the first opcodes where translated code makes a **real C-ABI call**
out to the host environment: every native translator now takes three extra
parameters (`rt_resolve_fn`/`rt_objsize_fn`/`rt_objtype_fn`, addresses of
`uint64_t fn(...)` functions) baked in as translate-time constants, plus a
new reserved register `r6` = `namepool_ptr` (mirrors `r7`'s existing
scratch-pointer convention, computed by the translator itself from the raw
`.tmo` bytes — never copied). Each environment binds those three function
pointers to something different:

- Phase 1 interpreter (`timi_interp.c`), the x86 JIT harness
  (`timi_jit_test.c`), and the RV64 verify harness (`timi_riscv_verify.c`)
  all bind them to small, identical mock catalogs — that's why
  `tests/obj_ops.timi` produces the same result (305) everywhere.
- `kernel/timi_runtime.c` binds them to the real, live `object_catalog[]`.

The RV64 verify harness has no real RV64 CPU to run these calls on, so
`rv64_exec.c` implements a host-callback sentinel mechanism: a reserved
address range that, when a `jalr` targets it, invokes a registered host C
function instead of faulting on a fake fetch address (see `rv64_exec.h`).
Worth reading if you're touching that file: an earlier version spaced
sentinel slots by 1, which real RV64 `jalr` semantics (target address's
low bit always cleared) silently corrupted — see §13 of the design doc
for the full bug writeup.

## Capability tags and authority-checked RESOLVE (v0.3, Phase 7)

Phase 6's `T_OBJREF` was a plain, forgeable `base_vaddr` — any register could
be made to hold one via `LOADI`, and nothing distinguished a real `RESOLVE`
result from a guessed address. Phase 7 adds two things that, together, give
`T_OBJREF` System/38-style "pointer" semantics:

- **Tagged, unforgeable capabilities.** Every symbolic register gets a
  shadow tag bit (interpreter: `Frame.cap_tag[]`; both native translators: a
  per-register byte region in the stack frame, directly below the register
  slots). `RESOLVE` tags the destination iff the result is non-zero; `MOV`
  propagates the source's tag; every other register-writing opcode
  (arithmetic, `LOADI`/`LOADI64`, `CMP`, `LEA`, `PTRADD`, typed `LOAD`)
  unconditionally clears the destination's tag. `OBJSIZE`/`OBJTYPE` require
  the operand's tag to be set, and reject with the pre-existing "not found"
  sentinel — **without ever calling the runtime catalog function** — if it
  isn't. `tests/cap_forge.timi` proves the forgery path is actually closed:
  it `RESOLVE`s a real object, then separately `LOADI`s the identical
  numeric address (never tagged) and confirms `OBJSIZE`/`OBJTYPE` on it come
  back rejected, not leaked. Passes identically (`3000304`) through
  `make test`, `make test-native`, and `make test-riscv`.
- **Authority-checked minting.** `kernel/timi_runtime.c`'s `timi_rt_resolve()`
  (the kernel-only binding — the host/mock catalogs have no permission
  model) now looks up the calling process's `owner_uid` via the new
  `process_find_current()` (`kernel/process.c`/`.h`) and gates the returned
  `base_vaddr` on the existing `catalog_check_access(uid, name, PERM_READ)`.
  Denial returns `0`, identical to "not found," so a process can't fish for
  which object names exist by comparing "not found" against "denied."

`CALL`/`RET` do **not** propagate tags across the call boundary in this v1 —
a capability passed as an argument or returned as a result arrives untagged
on the other side. A documented narrowing, not a bug; see §14 of the design
doc for why, and for what a v2 that closes it would need to do.

## Relationship to the kernel copy

`timi_x86.c` / `timi_x86.h` here are the copy of record. `kernel/timi_x86.c`
and `kernel/timi_x86.h` are meant to be **byte-identical** (modulo the doc
comments) to these — that's deliberate: it's what makes `timi-jit-test`'s
real-execution verification meaningful for the kernel build (same encoder,
same bugs or lack thereof). If you fix a bug in one, port it to the other and
diff to confirm they still match. `kernel/timi_runtime.c` has no host
equivalent — it's the kernel-only binding of the three v0.3 runtime-call
function pointers to the real `object_catalog[]` (the host toolchain's mock
catalogs live directly in `timi_interp.c`/`timi_jit_test.c`/
`timi_riscv_verify.c` instead).

`timi_riscv.c` / `timi_riscv.h` have **no kernel copy yet** — the RISC-V side
of the kernel (`kernel/kernel_riscv.c`) doesn't have a process/loader/object-
catalog subsystem at all yet (it's a ~10-line boot stub; `process.c`,
`loader.c`, `object_catalog.c` are x86-only in the root `Makefile`'s
`X86_C_SRC`). Porting the RV64 translator into the kernel is blocked on that
infrastructure existing first — a separate, non-TIMI undertaking. See §12.

`.tmo`, `.log`, `.txt` test artifacts and the compiled tool binaries are
gitignored — regenerate with `make`.
