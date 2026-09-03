// AeroSLS e1000 driver sidecar crt0 (drv.e1000.0) — x86-64 entry stub,
// same shape as the network sidecar's.
//
// The kernel jumps to `_start` with a pointer to the Boot Info Block in
// rdi (the synthetic ring3_ctx frame fills rdi, kernel/cap.c step 9).
// Steps (Phase 2 §6.2):
//   1. save the BIB pointer before touching anything else
//   2. switch to the driver's own boot stack (16 KiB in .bss)
//   3. hand the BIB to `rust_entry` (never returns)
//
// Included via global_asm!(include_str!("crt0.S"), options(att_syntax)) in
// entry.rs and placed at address 0 by e1000.ld, exactly like the network
// sidecar's crt0/network.ld pair.

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
    .skip 16384
boot_stack_top:
