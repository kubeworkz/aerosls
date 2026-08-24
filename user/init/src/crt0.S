// AeroSLS init sidecar crt0 — the Phase 5 boot sidecar's entry stub (x86-64).
//
// The kernel jumps to `_start` with a pointer to the Boot Info Block in
// rdi (the synthetic ring3_ctx frame fills rdi, kernel/cap.c step 9).
// Steps (Phase 2 §6.2, same as every sidecar crt0):
//   1. save the BIB pointer before touching anything else
//   2. switch to the sidecar's own boot stack (16 KiB in .bss — the
//      kernel-provided stack's top page holds the BIB and must not be
//      consumed)
//   3. hand the BIB to the Rust entry (`rust_entry`, never returns)
//
// This file is compiled by cargo through `global_asm!(include_str!(
// "crt0.S"))` in entry.rs (feature `target`) — rustc's LLVM integrated
// assembler — so the flat image builds with no external cross-GCC. It is
// x86-64-only because the kernel's `cap_create_sidecar` sidecar path is
// x86-only today; the RISC-V sidecar boot (a0 handoff) belongs to the
// sibling crt0.S files assembled by the sidecar image build's cross-GCC.
//
// Deliberately no `#ifdef` branches: rustc's global_asm preprocessing is
// unreliable with them (the RISC-V branch is assembled even on x86_64 in
// some crate-type/emit combinations), and the arch gate lives in Rust
// (`#[cfg(target_arch = "x86_64")]` around the include). Comments use `//`
// only — `#` is both the x86 comment char and the preprocessor marker, so
// `# ...` mid-line comments must not appear here.
//
// The assembly dialect is pinned to AT&T by the CALLER (entry.rs uses
// `global_asm!(include_str!("crt0.S"), options(att_syntax))`): rustc's
// global_asm defaults to INTEL syntax when this crate is compiled as a
// LIBRARY but AT&T when linked as a bin, so the dialect must be fixed
// outside the file for it to parse in both builds — and leaving it out of
// the file keeps crt0.S GAS-assemblable. `.balign 4096` (not the
// siblings' `.align 12`): LLVM MC's x86 `.align` takes bytes, GAS's takes
// a power-of-two exponent — `.balign` means the same thing in both, and
// the boot stack needs a page alignment.
//
// The linker script (init.ld) places `.text._start` at address 0, so the
// first byte of the flat binary is `_start`, matching the manifest's
// `image.entry = 0`.

.section .text._start,"ax",@progbits
.globl _start
_start:
    mov   %rdi, %r13         // 1. save BootInfo pointer in a callee-saved reg
    lea   boot_stack_top(%rip), %rsp   // 2. own stack
    mov   %r13, %rdi         // 3. first arg to rust_entry
    call  rust_entry         // 4. never returns

.section .bss
.balign 4096
boot_stack:
    .skip 16384
boot_stack_top:
