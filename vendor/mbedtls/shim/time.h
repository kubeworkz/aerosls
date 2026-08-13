/*
 * time.h — freestanding shim for the kernel's mbedTLS build.
 *
 * The x86_64-elf cross toolchain is built C-only with no libc headers
 * (setup-toolchain.sh: "no C++, no libc/headers"), but mbedTLS 3.6's public
 * headers include <time.h> whenever MBEDTLS_HAVE_TIME_DATE is set
 * (platform_util.h) and kernel/tls_platform.c includes it directly for its
 * mbedtls_platform_gmtime_r() implementation. The ONLY thing either consumes
 * is struct tm: nothing in the linked set calls localtime/mktime/asctime, and
 * nothing uses the bare time_t type.
 *
 * Deliberately NO time_t typedef. mbedtls_time_t is long long via
 * MBEDTLS_PLATFORM_TIME_TYPE_MACRO in sls_mbedtls_config.h, so platform_time.h
 * never reads time_t from this header; and on a build with a real libc (the
 * deploy server's host gcc), stdlib.h -> sys/types.h defines time_t itself, so
 * a typedef here would collide with it. With no typedef at all, the shim is a
 * no-op on a libc-equipped toolchain and exactly the struct tm supplier on a
 * freestanding one.
 *
 * struct tm is the full C11 nine-field layout — tls_platform.c's gmtime_r
 * writes tm_year/tm_mon/tm_mday/tm_hour/tm_min/tm_sec/tm_wday/tm_yday/
 * tm_isdst, and mbedTLS's own x509 code reads the same fields. It is safe to
 * define unconditionally: this header is the only <time.h> these translation
 * units see (the shim dir is first on the include path), so glibc's struct tm
 * never appears alongside it.
 */
#ifndef SLS_FREESTANDING_TIME_H
#define SLS_FREESTANDING_TIME_H

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#endif /* SLS_FREESTANDING_TIME_H */
