#ifndef SBI_H
#define SBI_H

#include <stdint.h>

// OpenSBI Extension ID (EID) and Function ID (FID) matching SBI v2.0 Specifications
#define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x01
#define SBI_EXT_0_1_CONSOLE_GETCHAR 0x02
#define SBI_EXT_DBCN                0x4442434E  // Debug Console Extension
#define SBI_DBCN_WRITE              0
#define SBI_DBCN_READ               1

struct SBIReturn {
    long error;
    long value;
};

// Raw architectural calling wrapper to pass parameters up to OpenSBI Machine Mode
static inline struct SBIReturn sbi_call(unsigned long ext, unsigned long fid, 
                                        unsigned long arg0, unsigned long arg1) {
    struct SBIReturn ret;
    register unsigned long a0 __asm__("a0") = arg0;
    register unsigned long a1 __asm__("a1") = arg1;
    register unsigned long a7 __asm__("a7") = ext;
    register unsigned long a6 __asm__("a6") = fid;

    __asm__ volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a6), "r"(a7)
        : "memory"
    );

    ret.error = a0;
    ret.value = a1;
    return ret;
}

void sbi_putchar(char c);
int sbi_getchar(void);

#endif