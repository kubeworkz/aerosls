#ifndef SYSCALL_DISPATCH_H
#define SYSCALL_DISPATCH_H

#include <stdint.h>

/*
 * Kernel-mode syscall dispatcher.
 *
 * In the current build the shell runs in Ring-0 alongside the kernel, so
 * do_syscall() is a direct C dispatch rather than a hardware 'syscall'
 * instruction.  When a user-space ring-3 process eventually calls syscall(),
 * the assembly stub in arch/x86/syscall.asm routes execution to the same
 * handler functions — the ABI is therefore consistent.
 */
uint64_t do_syscall(uint64_t num, void* arg);

/* Legacy syscall numbers used by the shell (not in object_catalog.h) */
#define SYS_SLS_CHMOD      107
#define SYS_SLS_SET_USER   108

#endif /* SYSCALL_DISPATCH_H */
