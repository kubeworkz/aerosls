/*
 * stdio.h — freestanding shim for the kernel's mbedTLS build.
 *
 * mps_reader.h and others include <stdio.h> unconditionally, but nothing in
 * the compiled set references a stdio symbol: every fprintf/printf/vsnprintf
 * call in the 107 library files goes through the mbedtls_* wrappers, which
 * the config routes to kernel functions by macro (FPRINTF_MACRO,
 * SNPRINTF_MACRO), and the FILE* family (fopen/fread/fwrite/…) lives
 * entirely under #if defined(MBEDTLS_FS_IO), which the config #undefs.
 *
 * So this header exists to satisfy the include, and defines FILE as an
 * opaque type — the FPRINTF_MACRO receives the stream as an unused macro
 * parameter, so no file object ever has to exist. A future mbedTLS bump
 * that calls a bare stdio function fails at link time naming it: the
 * config's own section heading says a missing symbol is a LINK error
 * rather than an accidental pull-in of a libc that is not there.
 */
#ifndef SLS_FREESTANDING_STDIO_H
#define SLS_FREESTANDING_STDIO_H

#include <stddef.h>

typedef struct sls_stdio_file FILE;

#endif /* SLS_FREESTANDING_STDIO_H */
