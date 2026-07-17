/* user/libsls/start.S — Ring-3 process entry point for AeroSLS programs
 *
 * The kernel enters this code via SYSRETQ with:
 *   RSP = top of user stack (16-byte aligned, no return address)
 *   all general-purpose registers zeroed
 *
 * We call main(0, NULL) and pass the return value to SYS_SLS_EXIT.
 */

.section .text
.global _start
_start:
    /* RSP is 16-byte aligned on entry (SYSRETQ does not push a return address).
     * x86-64 ABI: RSP must be 16-aligned before CALL, so we can call main directly. */
    xorq  %rdi, %rdi          /* argc = 0  */
    xorq  %rsi, %rsi          /* argv = NULL */
    call  main

    /* main() return value is in %rax — use it as exit code */
    movq  %rax, %rdi          /* exit_code argument */
    movq  $164, %rax          /* SYS_SLS_EXIT */
    syscall

    /* Should never reach here, but spin just in case */
.Lspin:
    jmp   .Lspin
