/*
 * assert.h — freestanding shim for the kernel's mbedTLS build.
 *
 * common.h includes <assert.h> unconditionally — every library file pulls
 * it in through common.h — yet nothing in the compiled set calls assert():
 * the measured count of `assert(` call sites in the 107 library files is
 * zero, because upstream's assert() users are the self-test and debug
 * paths, both removed by the config (#undef MBEDTLS_SELF_TEST /
 * MBEDTLS_DEBUG_C).
 *
 * The macro below keeps the standard contract — evaluate the expression,
 * abort() on false — so a future mbedTLS bump that calls assert() fails
 * loudly rather than silently: abort() is declared here but provided
 * nowhere in this link, so a caller would die at link time naming it.
 * NDEBUG is deliberately not special-cased: nothing in this build defines
 * it, and a kernel that compiled assertions away would keep teeth it
 * claims to have.
 */
#ifndef SLS_FREESTANDING_ASSERT_H
#define SLS_FREESTANDING_ASSERT_H

#include <stdlib.h>   /* abort() — declared; never linked (0 call sites) */

#define assert(expr) ((expr) ? (void)0 : abort())

#endif /* SLS_FREESTANDING_ASSERT_H */
