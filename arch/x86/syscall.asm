bits 64
global syscall_entry_stub
global enter_user_process
extern do_syscall          ; C dispatcher for every syscall number

; MSR Address Constants
IA32_KERNEL_GS_BASE equ 0xC0000102

section .text
syscall_entry_stub:
    swapgs
    mov [gs:0], rsp
    mov rsp, [gs:8]

    ; Save callee-saved regs (C ABI requires caller to preserve these).
    ; Also save RCX (user RIP) and R11 (user RFLAGS) which SYSCALL destroys.
    ; Caller-saved regs (R8-R10, RSI, RDX, RDI) are NOT on this push list —
    ; the kernel clobbers them mid-syscall (in-repo asm may still list them
    ; as clobbers) — but E7 increment 13 RESTORES them at
    ; syscall_return_path from the linux_args capture below, so they
    ; survive the round trip the way Linux promises (only RAX = result,
    ; RCX and R11 change across a syscall). A real musl binary holds
    ; live values across one — __libc_sigaction keeps `act` in R8
    ; through its unmask rt_sigprocmask — and it #PF'd at 0x8054bdf
    ; reading 0x88(R8) with R8 back as kernel scratch (0x90/0x91) until
    ; the restore landed.
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rcx       ; user RIP (set by CPU on SYSCALL)
    push r11       ; user RFLAGS

    ; Save the entry registers to per-CPU scratch ([gs:0x10..0x50]) right
    ; away. Park/resume capture reads these (proc_park_capture and the
    ; handoff/yield captures in process.c) — the copy on the syscall stack
    ; at [top-64..top-8] is NOT safe to read later: the -O2 call chain
    ; (do_syscall -> sys_sls_chan_* -> k_chan_* -> ...) may reuse that
    ; region for its own locals before the capture runs, so a late read can
    ; capture garbage and the resumed process sysrets with corrupted
    ; callee-saved regs / user RSP (caught live: resumed processes faulted
    ; at rip=0 and wrote to 0xf000ff53f000ff73). Must run BEFORE any call.
    mov [gs:0x10], r11
    mov [gs:0x18], rcx
    mov [gs:0x20], r15
    mov [gs:0x28], r14
    mov [gs:0x30], r13
    mov [gs:0x38], r12
    mov [gs:0x40], rbx
    mov [gs:0x48], rbp

    ; E7 (increment 2, Linux ABI Shim Design §3.4): the six Linux argument
    ; registers, to the SIBLING per-CPU block [gs:0x50..0x80), BEFORE any
    ; call and before the number chain — for the same reason the block
    ; above exists: a Linux-personality task's arguments live only in
    ; rdi/rsi/rdx/r10/r8/r9, and the entry path DESTROYS them by the time
    ; C could look (`mov rsi,rdi; mov rdi,rax` in .unknown_syscall). The
    ; SLS path pays six stores and reads nothing; capturing unconditionally
    ; is the honest first cut — a personality-gated capture would need the
    ; personality in a register here, and the fast path is deferred to a
    ; measurement (§3.7), not assumed.
    mov [gs:0x50], rdi
    mov [gs:0x58], rsi
    mov [gs:0x60], rdx
    mov [gs:0x68], r10
    mov [gs:0x70], r8
    mov [gs:0x78], r9

    ; Every syscall number reaches do_syscall, where the current process's
    ; personality decides what the number means BEFORE any SLS/Linux number
    ; chain.  There is deliberately no SLS fast-path table here: the SLS
    ; block overlaps Linux's (105, 110-124, 130-134, 140-145, 150-151),
    ; so a number chosen in this stub would decide for a Linux task too.
    ; The C legacy switch remains the one SLS implementation; the Linux
    ; branch remains the one Linux implementation.
    jmp .unknown_syscall

.unknown_syscall:
    ; The only dispatch: do_syscall applies the personality first, then the
    ; SLS legacy switch for an SLS task.
    ; do_syscall(uint64_t num, void* arg): rdi = num, rsi = arg
    mov  rsi, rdi          ; arg was in RDI
    mov  rdi, rax          ; syscall number from RAX
    call do_syscall
    ; return value already in RAX
    jmp  syscall_return_path

; Phase 1.5: cap_recv_resume() (process.c) re-runs a parked recv and then
; jumps HERE to reuse the canonical syscall-return sequence — the eight pops
; undo the entry pushes (r11, rcx, r15..rbp), [gs:0] holds the user RSP the
; resume path restored, the six-register restore below returns the
; argument registers, and the trailing swapgs+sysret lands back in Ring-3.
; (Note: this label is deliberately NOT named `.syscall_return` — a leading
; dot would scope it to the previous non-local label and every `jmp` above
; would break. It is the shared return path for both entry modes.)
global syscall_return_path
syscall_return_path:
    pop  r11
    pop  rcx
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    mov  rsp, [gs:0]
    ; E7 increment 13 (§10.13's register wall): return the six Linux
    ; argument registers from THIS entry's capture ([gs:0x50..0x80)) —
    ; Linux changes only RAX (result), RCX and R11 across a syscall, so
    ; a live value in any of them must survive. On the direct path the
    ; capture is this very entry's (only the entry stub writes that
    ; block, and no other syscall can have run on this core mid-flight);
    ; cap_recv_resume()/cap_sysret_resume() re-stamp it from park_ctx
    ; before they jump here, so a resume returns the PARKED task's own
    ; registers, not whoever last trapped. Never touch RAX — it carries
    ; the syscall's return value.
    mov  rdi, [gs:0x50]
    mov  rsi, [gs:0x58]
    mov  rdx, [gs:0x60]
    mov  r10, [gs:0x68]
    mov  r8,  [gs:0x70]
    mov  r9,  [gs:0x78]
    swapgs
    o64 sysret          ; return to Ring-3 (NASM 2.16: use o64 sysret, not sysretq)