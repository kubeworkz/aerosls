#include "../../include/sls_mmu.h"
    #define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x01
    #define SBI_EXT_0_1_CONSOLE_GETCHAR 0x02
    void sbi_putchar(char c) {
        register unsigned long a0 asm("a0") = c;
        register unsigned long a7 asm("a7") = SBI_EXT_0_1_CONSOLE_PUTCHAR;
        asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    }
    int sbi_getchar(void) {
        register unsigned long a0 asm("a0");
        register unsigned long a7 asm("a7") = SBI_EXT_0_1_CONSOLE_GETCHAR;
        asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
        return (int)a0;
    }