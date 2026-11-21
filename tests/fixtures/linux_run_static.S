/* tests/fixtures/linux_run_static.S — the step 1 gate's uploaded payload.
 *
 * A static, non-PIE ET_EXEC with no libc and no interpreter: exactly the shape
 * the tenant `linux run` path accepts. It writes its marker with the raw
 * write(2) syscall and exits 0, so the only machinery between the gate's bytes
 * and the marker is the kernel's ELF mapper + Linux shim.
 *
 * Build (the gate does this itself):
 *   cc -nostdlib -static -no-pie -o linux_run_static.elf \
 *      tests/fixtures/linux_run_static.S
 */
    .global _start
    .text
_start:
    mov $1, %rax            /* SYS_write */
    mov $1, %rdi            /* stdout */
    lea marker(%rip), %rsi
    mov $marker_len, %rdx
    syscall

    mov $60, %rax           /* SYS_exit */
    xor %rdi, %rdi
    syscall

marker:
    .ascii "TENANT-EXEC-OK\n"
    .set marker_len, . - marker
