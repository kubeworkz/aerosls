// AeroSLS network sidecar crt0 — x86-64 entry stub.
//
// The kernel jumps to `_start` with a pointer to the Boot Info Block in
// rdi (the synthetic ring3_ctx frame fills rdi, kernel/cap.c step 9).
// Steps (Phase 2 §6.2):
//   1. save the BIB pointer before touching anything else
//   2. switch to the sidecar's own boot stack (16 KiB in .bss)
//   3. hand the BIB to `rust_entry` (never returns)

.section .text._start,"ax",@progbits
.globl _start
_start:
    mov   %rdi, %r13                  // 1. save BootInfo pointer
    lea   boot_stack_top(%rip), %rsp  // 2. own stack
    mov   %r13, %rdi                  // 3. first arg to rust_entry
    call  rust_entry                  // 4. never returns

.section .bss
.balign 4096
boot_stack:
    .skip 65536
boot_stack_top:
