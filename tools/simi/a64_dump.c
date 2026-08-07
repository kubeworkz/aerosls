/* a64_dump.c — M0 verification tool: translate a .tmo and print every
 * emitted 32-bit word, one per line, feeding the independent Python
 * encoder cross-check (a64_enc_check.py, make a64-enc-check). */
#include "simi_arm.h"
#include <stdio.h>
#include <stdlib.h>

#define CODE_CAP 262144u

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* obj = malloc((size_t)sz);
    if (fread(obj, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    uint8_t* buf = calloc(1, CODE_CAP);
    uint32_t out_len = 0, entry_off = 0;
    int rc = simi_arm_translate(obj, (uint32_t)sz, buf, CODE_CAP, "main",
                                 (uint64_t)0x55550000,  /* fixed scratch for determinism */
                                 (uint64_t)0x11110000,  /* fixed hostfns (not executed) */
                                 (uint64_t)0x22220000,
                                 (uint64_t)0x33330000,
                                 &out_len, &entry_off);
    printf("rc=%d len=%u entry=%u\n", rc, out_len, entry_off);
    for (uint32_t i = 0; i + 4 <= out_len; i += 4) {
        uint32_t w = (uint32_t)buf[i] | ((uint32_t)buf[i+1]<<8) |
                     ((uint32_t)buf[i+2]<<16) | ((uint32_t)buf[i+3]<<24);
        printf("%08x\n", w);
    }
    return 0;
}
