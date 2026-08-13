/*
 * time.h — freestanding shim for the kernel's mbedTLS build.
 *
 * The x86_64-elf cross toolchain is built C-only with no libc headers
 * (setup-toolchain.sh: "no C++, no libc/headers"), but mbedTLS 3.6's public
 * headers include <time.h> whenever MBEDTLS_HAVE_TIME_DATE is set
 * (platform_util.h) and kernel/tls_platform.c includes it directly for its
 * mbedtls_platform_gmtime_r() implementation. Only two things are consumed:
 * the time_t typedef and struct tm — nothing in the linked set calls
 * localtime/mktime/asctime.
 *
 * mbedtls_time_t itself is NOT this time_t: sls_mbedtls_config.h defines
 * MBEDTLS_PLATFORM_TIME_TYPE_MACRO long long, so platform_time.h never reads
 * this typedef. It exists only so <time.h> has the standard shape.
 *
 * struct tm is the full C11 nine-field layout — tls_platform.c's gmtime_r
 * writes tm_year/tm_mon/tm_mday/tm_hour/tm_min/tm_sec/tm_wday/tm_yday/
 * tm_isdst, and mbedTLS's own x509 code reads the same fields.
 */
#ifndef SLS_FREESTANDING_TIME_H
#define SLS_FREESTANDING_TIME_H

typedef long long time_t;

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
