/* e7_linux_probe.S — E7's first Linux-personality task, as a payload
 * (AeroSLS-Linux-ABI-Shim-Design-v0.1.md §3.3 steps 5-7, boot proof).
 *
 * Freestanding, position-independent, ONE .text section, _start at the
 * section's start — the kernel's flat sidecar-image mapper
 * (kernel/cap.c cap_create_sidecar_in_personality step 5) copies the file
 * at USER_PROC_CODE_BASE and enters at image_entry = offset 0.
 *
 * Build (the committed .flat is the byte-exact output; build_probe.sh):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o probe.elf e7_linux_probe.S
 *   objcopy -O binary -j .text probe.elf e7_linux_probe.flat
 *
 * The sequence is a machine-checked argument about the round trip:
 *   218 (set_tid_address) — §5.6's store-and-ignore: the answer must be
 *       the argument echo itself (args[0] -> rax). A mismatch issues the
 *       MARKER number 391, whose ABSENCE the boot evidence asserts.
 *   39  — outside the census: the sidecar answers CAP_ENOSYS (-10) and
 *       §5.7's map must deliver -38 into rax, never the raw CAP code. A
 *       mismatch issues MARKER 392 (also asserted absent).
 *   40  — a third full round trip; its arrival is the sidecar's
 *       `[E7] linux: num=40 -> ENOSYS` line.
 *   231 (exit_group) — the sidecar reaps; no reply is owed and this task
 *       never runs again (the hang below is unreachable by design).
 *
 * Registers: immediates only, and the one preserved value lives in r12 —
 * callee-saved, pushed by the entry stub and restored by the park/resume
 * (park_ctx) — because the kernel's SLS ABI does NOT promise the argument
 * registers survive a syscall return.
 */
    .text
    .globl _start
_start:
    /* 1. set_tid_address(0x1DECAF): rax must return 0x1DECAF. */
    mov     $0x1DECAF, %r12
    mov     %r12, %rdi
    mov     $218, %eax
    syscall
    cmp     %r12, %rax
    je      .Ltid_ok
    mov     $391, %eax          /* MARKER: 218's ret did not echo the arg */
    syscall
.Ltid_ok:
    /* 2. syscall 39 (outside the census): rax must return -38 (-ENOSYS),
     *    which only happens if the sidecar's CAP status was mapped. */
    mov     $39, %eax
    syscall
    cmp     $-38, %rax
    je      .Lenosys_ok
    mov     $392, %eax          /* MARKER: the reply status was not mapped */
    syscall
.Lenosys_ok:
    /* 3. a third round trip: 40 must ARRIVE (the sidecar logs it). */
    mov     $40, %eax
    syscall
    /* 4. exit_group(42): the sidecar reaps this task. */
    mov     $42, %edi
    mov     $231, %eax
    syscall
.Lhang:
    jmp     .Lhang
