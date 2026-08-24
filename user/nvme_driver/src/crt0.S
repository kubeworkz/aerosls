// AeroSLS sidecar crt0 — shared by the ramdisk driver, the POSIX sidecar,
// and the NVMe driver.
//
// The kernel jumps to `_start` with a pointer to the Boot Info Block in
// a0 (RISC-V) / rdi (x86-64). Contract (Phase 2 §6.2):
//   1. save the BIB pointer before touching anything else
//   2. switch to the sidecar's own boot stack (a MEM cap from the manifest)
//   3. hand the BIB to the Rust entry (`rust_entry`, never returns)
//
// This file is assembled and linked ahead of the Rust rlib by the sidecar
// image build (linker script places `_start` at the manifest entry point;
// `boot_stack_top` is a manifest-sized stack, 16 KiB here). It is a build
// artifact, not compiled by cargo.

#ifdef __riscv
.section .text._start,"ax",@progbits
.globl _start
_start:
    mv   s0, a0              # 1. save BootInfo pointer in a callee-saved reg
    la   sp, boot_stack_top  # 2. own stack
    mv   a0, s0              # 3. first arg to rust_entry
    call rust_entry          # 4. never returns

.section .bss
.align 12
boot_stack:
    .skip 16384
boot_stack_top:
#endif

#ifdef __x86_64__
.section .text._start,"ax",@progbits
.globl _start
_start:
    mov   %rdi, %r13         # 1. save BootInfo pointer in a callee-saved reg
    lea   boot_stack_top(%rip), %rsp   # 2. own stack
    mov   %r13, %rdi         # 3. first arg to rust_entry
    call  rust_entry         # 4. never returns

.section .bss
.align 12
boot_stack:
    .skip 16384
boot_stack_top:
#endif
