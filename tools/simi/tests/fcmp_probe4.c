#include <stdio.h>
#include <stdint.h>
#include <math.h>

static uint64_t out_nzcv_cmp(void) {
    uint64_t v;
    __asm__ volatile(
        "mov x9, #1\n"
        "mov x10, #2\n"
        "cmp x9, x10\n"
        "mrs x11, nzcv\n"
        "mov %0, x11\n"
        : "=r"(v) : : "x9", "x10", "x11", "cc");
    return v;
}
static uint64_t out_nzcv_fcmp(uint64_t abits, uint64_t bbits) {
    uint64_t v;
    __asm__ volatile(
        "fmov d0, %1\n"
        "fmov d1, %2\n"
        "fcmp d0, d1\n"
        "mrs x11, nzcv\n"
        "mov %0, x11\n"
        : "=r"(v) : "r"(abits), "r"(bbits) : "x11", "cc");
    return v;
}
static void show(const char* tag, uint64_t nzcv) {
    printf("%-14s NZCV=0b%d%d%d%d (0x%llx)\n", tag,
        (int)((nzcv >> 3) & 1), (int)((nzcv >> 2) & 1),
        (int)((nzcv >> 1) & 1), (int)(nzcv & 1), (unsigned long long)nzcv);
}
int main(void) {
    union { uint64_t u; double d; } dd;
    show("cmp 1,2 (expect N)", out_nzcv_cmp());
    dd.d = NAN; uint64_t nanbits = dd.u;
    dd.d = 1.0; uint64_t onebits = dd.u;
    dd.d = 2.0; uint64_t twobits = dd.u;
    show("fcmp NaN,1", out_nzcv_fcmp(nanbits, onebits));
    show("fcmp 1,NaN", out_nzcv_fcmp(onebits, nanbits));
    show("fcmp NaN,NaN", out_nzcv_fcmp(nanbits, nanbits));
    show("fcmp 1,2", out_nzcv_fcmp(onebits, twobits));
    show("fcmp 2,1", out_nzcv_fcmp(twobits, onebits));
    show("fcmp 1,1", out_nzcv_fcmp(onebits, onebits));
    return 0;
}
