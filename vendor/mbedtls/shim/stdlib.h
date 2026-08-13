/*
 * stdlib.h — freestanding shim for the kernel's mbedTLS build.
 *
 * The x86_64-elf cross toolchain ships no libc headers, and mbedTLS's
 * alignment.h — included by common.h, hence by every library file —
 * includes <stdlib.h> next to <string.h>. The freestanding rebuild of the
 * softmmu A/B side failed on string.h first; stdlib.h is the second
 * unconditional libc include in the same file.
 *
 * Like shim/string.h this shadows a real libc's stdlib.h on toolchains
 * that have one, harmlessly: the set below is measured from the tree
 * (grep of the 107 compiled files). malloc/calloc/free are declared but
 * never linked — the config's MBEDTLS_PLATFORM_MEMORY routes every
 * allocation to the fixed memory-buffer pool, and its own section heading
 * says the point: "no standard library at all ... a missing one is a LINK
 * error rather than an accidental pull-in of a libc that is not there."
 * abort/exit resolve to sls_mbedtls_exit via MBEDTLS_PLATFORM_EXIT_MACRO.
 */
#ifndef SLS_FREESTANDING_STDLIB_H
#define SLS_FREESTANDING_STDLIB_H

#include <stddef.h>

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void   free(void *ptr);
void   abort(void);
void   exit(int status);
int    abs(int j);

#endif /* SLS_FREESTANDING_STDLIB_H */
