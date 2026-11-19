/* M2 dynamic hello — an arbitrary PT_INTERP image with no libc at all.
 *
 * The kernel maps the musl loader (the image's PT_INTERP) and enters the
 * loader; the loader transfers to this ET_EXEC's _start, which makes its
 * write/exit through raw syscalls. That keeps this fixture independent of
 * every committed musl object: the only thing it shares with the M2a pair
 * is the loader path, which is the contract under test. */
    .global _start
    .text
_start:
    mov     $1, %rax            /* write */
    mov     $1, %rdi            /* fd 1 */
    lea     msg(%rip), %rsi
    mov     $9, %rdx            /* "M2DYN-OK" + newline */
    syscall
    mov     $60, %rax           /* exit */
    xor     %rdi, %rdi
    syscall

    .section .rodata
msg:
    .ascii  "M2DYN-OK\n"
