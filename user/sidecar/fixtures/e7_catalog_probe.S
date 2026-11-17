/* e7_catalog_probe.S — M5.2's live Linux-personality CAT_EXEC probe.
 *
 * Freestanding, position-independent, ONE .text section, _start at the
 * section's start — the kernel's flat sidecar-image mapper copies the file
 * at USER_PROC_CODE_BASE and enters at offset 0.
 *
 * Build (the committed .flat is this script's byte-exact output):
 *   gcc -nostdlib -static -no-pie -Wl,-Ttext=0x1000 -Wl,-e,_start \
 *       -o catalog.elf e7_catalog_probe.S
 *   objcopy -O binary -j .text catalog.elf e7_catalog_probe.flat
 *
 * The syscall is SYS_SLS_CATALOG_EXEC (334), a Linux-personality number:
 *   rdi = SQL pointer, rsi = bounded length, rdx = reserved flags (0).
 * The request carries NO uid or partition; the sidecar's registered
 * catalog channel is the identity. Every check below writes exactly one
 * marker at the end, so a boot's serial transcript says whether the whole
 * sequence held:
 *
 *   M52-CATALOG-OK    — bob's table accepted INSERT and UPDATE, dave's
 *                       table was EPERM, the foreign name was ENOENT, and
 *                       DDL was EINVAL.
 *   M52-CATALOG-FAIL  — at least one of those was not the answer the
 *                       boundary promises.
 *
 * The probe's own task still holds exactly its one sidecar channel (the
 * manifest asks for no caps), which the existing E7 cap-table guard checks.
 */
    .text
    .globl _start
_start:
    /* 1. INSERT into bob's table: affected_rows == 1. */
    lea     .Lsql_insert(%rip), %rdi
    mov     $.Lsql_insert_end-.Lsql_insert, %esi
    xor     %edx, %edx
    mov     $334, %eax
    syscall
    cmp     $1, %rax
    jne     .Lfail

    /* 2. UPDATE the row bob just wrote: affected_rows == 1. */
    lea     .Lsql_update(%rip), %rdi
    mov     $.Lsql_update_end-.Lsql_update, %esi
    xor     %edx, %edx
    mov     $334, %eax
    syscall
    cmp     $1, %rax
    je      .Lupdate_ok
    cmp     $0, %rax
    je      .Lupdate_zero
    cmp     $-22, %rax
    je      .Lupdate_einval
    cmp     $-1, %rax
    je      .Lupdate_eperm
    cmp     $-2, %rax
    je      .Lupdate_enoent
    cmp     $-5, %rax
    je      .Lupdate_eio
    cmp     $-11, %rax
    je      .Lupdate_busy
    cmp     $-71, %rax
    je      .Lupdate_eproto
    jmp     .Lfail
.Lupdate_ok:

    /* 3. DDL is outside the first write cut: CAT_ERR_TYPE -> EINVAL. */
    lea     .Lsql_ddl(%rip), %rdi
    mov     $.Lsql_ddl_end-.Lsql_ddl, %esi
    xor     %edx, %edx
    mov     $334, %eax
    syscall
    cmp     $-22, %rax
    jne     .Lfail

    /* 4. dave's table through bob's channel: CAT_ERR_PERM -> EPERM. */
    lea     .Lsql_denied(%rip), %rdi
    mov     $.Lsql_denied_end-.Lsql_denied, %esi
    xor     %edx, %edx
    mov     $334, %eax
    syscall
    cmp     $-1, %rax
    jne     .Lfail

    /* 5. a foreign partition's name: CAT_ERR_NOENT -> ENOENT. */
    lea     .Lsql_foreign(%rip), %rdi
    mov     $.Lsql_foreign_end-.Lsql_foreign, %esi
    xor     %edx, %edx
    mov     $334, %eax
    syscall
    cmp     $-2, %rax
    jne     .Lfail

    lea     .Lok_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lok_end-.Lok_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lfail:
    lea     .Lfail_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lfail_end-.Lfail_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_zero:
    lea     .Lupdate_zero_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_zero_end-.Lupdate_zero_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_einval:
    lea     .Lupdate_einval_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_einval_end-.Lupdate_einval_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_eperm:
    lea     .Lupdate_eperm_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_eperm_end-.Lupdate_eperm_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_enoent:
    lea     .Lupdate_enoent_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_enoent_end-.Lupdate_enoent_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_eio:
    lea     .Lupdate_eio_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_eio_end-.Lupdate_eio_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_busy:
    lea     .Lupdate_busy_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_busy_end-.Lupdate_busy_msg, %edx
    mov     $1, %eax
    syscall
    jmp     .Lexit
.Lupdate_eproto:
    lea     .Lupdate_eproto_msg(%rip), %rsi
    mov     $1, %edi
    mov     $.Lupdate_eproto_end-.Lupdate_eproto_msg, %edx
    mov     $1, %eax
    syscall
.Lexit:
    xor     %edi, %edi
    mov     $231, %eax          /* exit_group(status=0) */
    syscall
.Lhang:
    jmp     .Lhang

/* Every literal is inside .text so the flat image carries it and the
 * kernel's checked copy reads it from THIS task's own page tables. */
.Lsql_insert:
    .ascii  "INSERT INTO m52_bob (note) VALUES ('from-linux')"
.Lsql_insert_end:
.Lsql_update:
    .ascii  "UPDATE m52_bob SET note = 'updated'"
.Lsql_update_end:
.Lsql_ddl:
    .ascii  "DROP TABLE m52_bob"
.Lsql_ddl_end:
.Lsql_denied:
    .ascii  "INSERT INTO m52_dave (note) VALUES ('denied')"
.Lsql_denied_end:
.Lsql_foreign:
    .ascii  "INSERT INTO m52_foreign (note) VALUES ('foreign')"
.Lsql_foreign_end:
.Lok_msg:
    .ascii  "M52-CATALOG-OK\n"
.Lok_end:
.Lfail_msg:
    .ascii  "M52-CATALOG-FAIL\n"
.Lfail_end:
.Lupdate_zero_msg:
    .ascii  "M52-UPDATE-ZERO\n"
.Lupdate_zero_end:
.Lupdate_einval_msg:
    .ascii  "M52-UPDATE-EINVAL\n"
.Lupdate_einval_end:
.Lupdate_eperm_msg:
    .ascii  "M52-UPDATE-EPERM\n"
.Lupdate_eperm_end:
.Lupdate_enoent_msg:
    .ascii  "M52-UPDATE-ENOENT\n"
.Lupdate_enoent_end:
.Lupdate_eio_msg:
    .ascii  "M52-UPDATE-EIO\n"
.Lupdate_eio_end:
.Lupdate_busy_msg:
    .ascii  "M52-UPDATE-BUSY\n"
.Lupdate_busy_end:
.Lupdate_eproto_msg:
    .ascii  "M52-UPDATE-EPROTO\n"
.Lupdate_eproto_end:
