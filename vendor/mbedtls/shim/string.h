/*
 * string.h — freestanding shim for the kernel's mbedTLS build.
 *
 * The x86_64-elf cross toolchain is built C-only with no libc headers
 * (setup-toolchain.sh: "no C++, no libc/headers"), and mbedTLS library
 * sources include <string.h> unconditionally — alignment.h:15 is the first
 * one the build reaches, and it failed a freestanding rebuild of the
 * softmmu A/B side with "string.h: No such file or directory".
 *
 * The deploy host's toolchain HAS a real string.h, but the shim dir is
 * first on the include path, so this header shadows it there too. That is
 * the same arrangement shim/time.h already documents, and it is harmless
 * here because the declarations below are the plain C11 signatures: mbedTLS
 * compiles against the same text on both toolchains, and nothing in this
 * build takes the address of these functions or relies on a glibc
 * extension in them.
 *
 * The link resolves memcpy/memset & friends to the kernel's own
 * implementations — there is no libc in this link (the Makefile's
 * -U_FORTIFY_SOURCE exists precisely so gcc does not rewrite these calls
 * into __memcpy_chk and friends that would be absent). So this header
 * declares, and never provides: a symbol that does not resolve is a link
 * error, not a silent libc pull-in.
 *
 * The declared set is measured, not guessed: memcmp/memcpy/memmove/memset
 * and strchr/strcmp/strlen/strncmp/strncpy/strstr are every string.h symbol
 * the 107 compiled library files reference (grep of the tree). A future
 * mbedTLS bump that calls a function not listed here fails at compile time
 * on the freestanding toolchain — the correct failure mode, and the same
 * discipline the time_t-less time.h shim follows.
 */
#ifndef SLS_FREESTANDING_STRING_H
#define SLS_FREESTANDING_STRING_H

#include <stddef.h>

void  *memcpy(void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strstr(const char *haystack, const char *needle);

#endif /* SLS_FREESTANDING_STRING_H */
