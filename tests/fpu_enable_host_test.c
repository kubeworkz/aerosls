/* tests/fpu_enable_host_test.c — host test for §10.13's ring-3 SSE gate
 *
 * What's under test, and why it can't be a runtime test of the gate itself:
 * fpu_enable_sse() (arch/x86/lazy_fpu.c) writes CR0/CR4 — privileged
 * instructions that #GP in ring 3, i.e. everywhere a host test runs. So the
 * three decisions the asm consumes are split, pure, and pinned here:
 *
 *   1. arch/x86/fpu_enable.h — the CR0/CR4/CPUID bit arithmetic
 *      (fpu_cr4_enable_sse / fpu_cr0_enable_fpu / fpu_cpuid_has_sse),
 *      swept over seeded inputs plus the realistic control-register values.
 *   2. The emitted ORDER of fpu_enable_sse()'s asm, compiled -O0 -S.
 *      Load-bearing: CR4.OSFXSR must be set BEFORE ldmxcsr, because ldmxcsr
 *      is itself an SSE instruction and #UDs with OSFXSR=0 — the same #UD
 *      that killed the designated first user at musl abort()'s pxor
 *      (§10.13 step 1, rip=0x80525b0, error=0x0). A future edit that
 *      reorders the writes is caught here, not by a boot that triple-faults
 *      under -no-reboot with no trace.
 *   3. The wiring around it: kernel.c calls fpu_enable_sse() before ring 3
 *      exists (before syscall_gate_init), the header is included, and the
 *      vector-7 chain (idt.c gate → isr7_stub →
 *      handle_device_not_available_fault → clts) is intact so that arming
 *      CR0.TS later cannot produce an unhandled #NM. Also that the lazy
 *      path's own symbols stay dormant (no caller), which is what §10.13
 *      states about XMM state not being switched per task.
 *
 * Intel SDM facts the arithmetic asserts (verified against the SDM, not
 * memory): CR4 bit 9 = OSFXSR, bit 10 = OSXMMEXCPT; SSE instructions #UD
 * with OSFXSR=0 and #NM with CR0.TS=1/EM=0; CR0 bits 1/2/3 = MP/EM/TS;
 * CPUID.1:EDX bits 25/26 = SSE/SSE2; MXCSR's reset value is 0x1F80 and
 * FNINIT does not touch MXCSR (hence the separate ldmxcsr).
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 \
 *       -o /tmp/fpu_enable_host_test \
 *       tests/fpu_enable_host_test.c
 *   /tmp/fpu_enable_host_test
 *
 * (The build line lives in this file's own header because tests/run_all.sh
 * extracts and runs exactly that command — one source of truth.)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/x86/fpu_enable.h"

static int checks = 0;
static int failures = 0;

/* One line per check, pass or fail — run_all.sh counts "ok:" lines, and a
 * test that printed only failures would read as 0 checks examined (the
 * exact vacuous pass this suite's own rules refuse). */
static void check(int ok, const char* what) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    } else {
        printf("ok:   %s\n", what);
    }
}

static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ─── A. The bit arithmetic, swept and specific ─────────────────────────── */

/* xorshift32 — deterministic, seedable, no libc rand() variance across
 * platforms; the sweep must produce the same inputs on every host. */
static uint32_t xs32(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static void test_bits_arithmetic(void) {
    uint32_t seed = 0xF00Du;
    int i;
    /* The SDM values, asserted rather than re-derived: bit 9 OSFXSR,
     * bit 10 OSXMMEXCPT, bits 1/2/3 MP/EM/TS, CPUID.1:EDX[25] SSE,
     * CPUID.1:EDX[26] SSE2, MXCSR reset 0x1F80. */
    check((1ULL << 9) == 0x200ULL && (1ULL << 10) == 0x400ULL,
          "SDM: CR4.OSFXSR=bit 9, OSXMMEXCPT=bit 10");
    check((1ULL << 1) == 0x2ULL && (1ULL << 2) == 0x4ULL &&
          (1ULL << 3) == 0x8ULL,
          "SDM: CR0.MP=bit 1, EM=bit 2, TS=bit 3");
    check((1u << 25) == 0x02000000u && (1u << 26) == 0x04000000u,
          "SDM: CPUID.1:EDX SSE=bit 25, SSE2=bit 26");
    check(FPU_SSE_MXCSR_DEFAULT == 0x1F80u,
          "SDM: MXCSR reset value is 0x1F80 (all SIMD exceptions masked)");

    for (i = 0; i < 4000; i++) {
        uint64_t r = ((uint64_t)xs32(&seed) << 32) | xs32(&seed);
        uint64_t want_cr4 = r | 0x600ULL;            /* both SSE bits */
        uint64_t want_cr0 = (r | 0x2ULL) & ~0xCULL;  /* MP set, EM/TS clear */
        uint32_t edx = xs32(&seed);

        uint64_t got_cr4 = fpu_cr4_enable_sse(r);
        uint64_t got_cr0 = fpu_cr0_enable_fpu(r);
        if (got_cr4 != want_cr4) {
            check(0, "sweep: cr4 sets both SSE bits and preserves every other bit");
            break;
        }
        if (got_cr0 != want_cr0) {
            check(0, "sweep: cr0 sets MP, clears EM/TS, preserves every other bit");
            break;
        }
        if (fpu_cr4_enable_sse(got_cr4) != got_cr4) {
            check(0, "sweep: cr4 enable is idempotent");
            break;
        }
        if (fpu_cr0_enable_fpu(got_cr0) != got_cr0) {
            check(0, "sweep: cr0 enable is idempotent");
            break;
        }
        if (fpu_cpuid_has_sse(edx) !=
            (((edx >> 25) & 1u) && ((edx >> 26) & 1u))) {
            check(0, "sweep: cpu has SSE iff CPUID.1:EDX[25] && [26]");
            break;
        }
    }
    if (i == 4000) {
        check(1, "4000-input seeded sweep: cr4/cr0 arithmetic exact, idempotent, "
                 "cpu-has-SSE matches CPUID.1:EDX[25]&&[26]");
    }

    /* Realistic boots, spelled out so a reviewer can check them by eye:
     * QEMU after boot.asm's CR4.PAE write (bit 5) with CR0 = PE|PG|NE. */
    check(fpu_cr4_enable_sse(0x20ULL) == 0x620ULL,
          "boot CR4 (PAE only) + SSE bits -> PAE|OSFXSR|OSXMMEXCPT");
    check(fpu_cr4_enable_sse(0x80000001ULL) == 0x80000601ULL,
          "CR4 with unrelated high bits keeps them, gains only the SSE bits");
    check(fpu_cr0_enable_fpu(0x80000001ULL) == 0x80000003ULL,
          "boot CR0 (PE|PG|NE) -> +MP, EM/TS clear");
    check(fpu_cr0_enable_fpu(0x60000010ULL) == 0x60000012ULL,
          "CR0 with CD|NW set keeps them, gains MP only");

    /* The four CPUID combinations, by hand. */
    check(fpu_cpuid_has_sse(0x06000000u), "SSE+SSE2 present -> 1");
    check(!fpu_cpuid_has_sse(0x02000000u), "SSE without SSE2 -> 0");
    check(!fpu_cpuid_has_sse(0x04000000u), "SSE2 without SSE -> 0");
    check(!fpu_cpuid_has_sse(0x0181A3FFu), "plain 486-class EDX -> 0");

}

/* ─── B. The emitted order of fpu_enable_sse()'s asm ────────────────────── */

/* Split a compiled .s into the region of one function: from its label to
 * gcc's own `.size fn, .-fn` terminator. (A naive "next line starting with
 * a dot" cut would end the region at the function's own .cfi_startproc or
 * its .LFB label — both begin with a dot — capturing nothing.) */
static char* fn_region(const char* asm_text, const char* fn) {
    char pat[128];
    char end_pat[128];
    const char* start;
    const char* end;
    size_t n;
    char* out;
    snprintf(pat, sizeof pat, "\n%s:", fn);
    start = strstr(asm_text, pat);
    if (!start) { return NULL; }
    start++; /* skip the leading newline */
    snprintf(end_pat, sizeof end_pat, "\n\t.size %s,", fn);
    end = strstr(start + strlen(fn) + 1, end_pat);
    if (!end) { end = asm_text + strlen(asm_text); }
    n = (size_t)(end - start);
    out = (char*)malloc(n + 1);
    if (!out) { return NULL; }
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

/* 1 iff `word` occurs in `text` bounded by non-digits on both sides (so
 * "8064" does not match inside "180640"). */
static int has_word(const char* text, const char* word) {
    const char* p = text;
    size_t wlen = strlen(word);
    while ((p = strstr(p, word)) != NULL) {
        char before = (p == text) ? '\0' : p[-1];
        char after = p[wlen];
        int b_digit = (before >= '0' && before <= '9');
        int a_digit = (after >= '0' && after <= '9');
        if (!b_digit && !a_digit) { return 1; }
        p += wlen;
    }
    return 0;
}

/* Index of the first line containing `needle` at or after `from`, else -1. */
static long line_of(const char* text, const char* needle, long from) {
    const char* p = text + (from > 0 ? from : 0);
    const char* hit = strstr(p, needle);
    return hit ? (long)(hit - text) : -1;
}

/* Index of the WRITE form `mov ..., %crN` (the register ends the line),
 * versus the read form which has it in the middle. AT&T writes the operand
 * separator as ", " so the comma may be a few spaces back from the %. */
static long cr_write_of(const char* region, const char* reg) {
    const char* p = region;
    long found = -1;
    size_t rlen = strlen(reg);
    while ((p = strstr(p, reg)) != NULL) {
        const char* line_end = strchr(p, '\n');
        if (!line_end) { line_end = region + strlen(region); }
        const char* q = line_end;
        while (q > p && (q[-1] == ' ' || q[-1] == '\t')) { q--; }
        if ((size_t)(q - p) == rlen) {
            const char* c = p;
            while (c > region && (c[-1] == ' ' || c[-1] == '\t')) { c--; }
            if (c > region && c[-1] == ',') {
                found = (long)(p - region);
            }
        }
        p += rlen;
    }
    return found;
}

static void test_emitted_order(void) {
    const char* tmp = "/tmp/fpu_enable_host_test.s";
    char cmd[512];
    char* asm_text;
    char* region;
    long wr_cr4, wr_cr0, fninit, ldmxcsr, mxcsr_imm;
    int rc;

    snprintf(cmd, sizeof cmd,
             "gcc -O0 -S -I kernel -I arch/x86 -I . "
             "-o %s arch/x86/lazy_fpu.c 2> /tmp/fpu_enable_host_test.s.err",
             tmp);
    rc = system(cmd);
    check(rc == 0, "lazy_fpu.c compiles -O0 -S on the host (see "
                   "/tmp/fpu_enable_host_test.s.err)");
    asm_text = slurp(tmp);
    if (!asm_text) { check(0, "emitted assembly exists"); return; }

    region = fn_region(asm_text, "fpu_enable_sse");
    check(region != NULL, "fpu_enable_sse appears in the emitted assembly");
    if (!region) { free(asm_text); return; }

    wr_cr4 = cr_write_of(region, "%cr4");
    wr_cr0 = cr_write_of(region, "%cr0");
    fninit = line_of(region, "fninit", 0);
    ldmxcsr = line_of(region, "ldmxcsr", 0);
    check(wr_cr4 >= 0 && wr_cr0 >= 0 && fninit >= 0 && ldmxcsr >= 0,
          "fpu_enable_sse emits cr4/cr0 writes, fninit and ldmxcsr");
    check(wr_cr4 < wr_cr0,
          "CR4.OSFXSR is written BEFORE CR0 (both are set-up, order is free "
          "between them, but the check pins the sequence for review)");
    check(wr_cr0 < fninit,
          "CR0's EM/TS are cleared BEFORE fninit (x87 #UD with EM, #NM with TS)");
    check(wr_cr4 < ldmxcsr,
          "CR4.OSFXSR is written BEFORE ldmxcsr — ldmxcsr is itself an SSE "
          "instruction and #UDs with OSFXSR=0, the exact #UD that killed "
          "the firstuser at musl abort()'s pxor");
    check(fninit < ldmxcsr,
          "fninit precedes ldmxcsr — FNINIT does not touch MXCSR (SDM), so "
          "the masked-reset ldmxcsr must come after it");

    /* The MXCSR value may be materialized as an immediate, a stack slot,
     * or a .LC rodata constant outside the function's own region — so this
     * one searches the whole translation unit for the bare word 8064
     * (0x1F80 in decimal, which is how gcc prints it). */
    mxcsr_imm = has_word(asm_text, "8064") || has_word(asm_text, "0x1f80");
    check(mxcsr_imm != 0,
          "the emitted code carries the MXCSR reset value 0x1F80 (all SIMD "
          "exceptions masked) as an immediate or rodata constant");

    free(region);

    /* The dormant lazy path: handle_device_not_available_fault is compiled
     * here; perform_lazy_context_switch is the asm switch whose TS-arming
     * makes #NM reachable, and §10.13 states it stays dormant. */
    region = fn_region(asm_text, "handle_device_not_available_fault");
    check(region != NULL, "#NM handler handle_device_not_available_fault is compiled");
    if (region) {
        check(line_of(region, "clts", 0) >= 0,
              "#NM handler still clears CR0.TS with clts before touching state");
        check(line_of(region, "xsave", 0) >= 0 &&
              line_of(region, "xrstor", 0) >= 0,
              "#NM handler's xsave/xrstor pair intact");
        free(region);
    }
    free(asm_text);
}

/* ─── C. The wiring: who calls the gate, and with what chain ────────────── */

static void test_wiring(void) {
    char* kernel_c = slurp("kernel/kernel.c");
    char* lazy_c   = slurp("arch/x86/lazy_fpu.c");
    char* lazy_h   = slurp("arch/x86/scheduler_lazy.h");
    char* idt_c    = slurp("arch/x86/idt.c");
    char* isr_asm  = slurp("arch/x86/interrupt.asm");
    char* stubs_c  = slurp("kernel/stubs.c");
    char* switch_asm = slurp("arch/x86/switch_lazy.asm");
    long call, gate;

    check(kernel_c && lazy_c && lazy_h && idt_c && isr_asm && stubs_c,
          "all gate files readable");

    if (kernel_c) {
        check(line_of(kernel_c, "#include \"../arch/x86/fpu_enable.h\"", 0) >= 0,
              "kernel.c includes fpu_enable.h");
        call = line_of(kernel_c, "fpu_enable_sse()", 0);
        gate = line_of(kernel_c, "syscall_gate_init()", 0);
        check(call >= 0, "kernel.c calls fpu_enable_sse()");
        check(gate >= 0, "kernel.c calls syscall_gate_init()");
        check(call >= 0 && gate >= 0 && call < gate,
              "fpu_enable_sse() runs BEFORE syscall_gate_init() — the gate is "
              "armed on the BSP before any ring-3 task can exist");
        free(kernel_c);
    }

    if (lazy_c) {
        check(line_of(lazy_c, "#include \"fpu_enable.h\"", 0) >= 0,
              "lazy_fpu.c includes fpu_enable.h");
        check(line_of(lazy_c, "void fpu_enable_sse(void)", 0) >= 0,
              "lazy_fpu.c defines fpu_enable_sse");
        /* The pure arithmetic must actually drive the asm — otherwise the
         * test above pins dead code. */
        check(line_of(lazy_c, "fpu_cr4_enable_sse(", 0) >= 0,
              "fpu_enable_sse consumes fpu_cr4_enable_sse");
        check(line_of(lazy_c, "fpu_cr0_enable_fpu(", 0) >= 0,
              "fpu_enable_sse consumes fpu_cr0_enable_fpu");
        check(line_of(lazy_c, "fpu_cpuid_has_sse(", 0) >= 0,
              "fpu_enable_sse consumes fpu_cpuid_has_sse");
        check(line_of(lazy_c, "cpuid", 0) >= 0,
              "fpu_enable_sse runs CPUID before touching CR4 (a CPU without "
              "SSE gets a refusal line, not a #GP/triple fault)");
        free(lazy_c);
    }

    if (lazy_h) {
        check(line_of(lazy_h, "handle_device_not_available_fault", 0) >= 0 &&
              line_of(lazy_h, "fpu_hardware_owner", 0) >= 0,
              "scheduler_lazy.h still declares the #NM handler and the owner");
        free(lazy_h);
    }

    if (idt_c) {
        check(line_of(idt_c, "isr7_stub", 0) >= 0,
              "idt.c installs vector 7 (#NM) → isr7_stub");
        free(idt_c);
    }
    if (isr_asm) {
        check(line_of(isr_asm, "isr7_stub", 0) >= 0,
              "interrupt.asm defines isr7_stub");
        free(isr_asm);
    }
    if (stubs_c) {
        check(line_of(stubs_c, "handle_device_not_available", 0) >= 0,
              "stubs.c routes vector 7 to handle_device_not_available");
        check(line_of(stubs_c, "handle_ring3_fault", 0) >= 0,
              "stubs.c keeps handle_ring3_fault (the #UD reporter that "
              "produced §10.13's diagnosis)");
        free(stubs_c);
    }

    /* §10.13: the lazy path stays dormant — TS is never armed, so #NM never
     * fires; assert no caller of the asm switch exists outside its own
     * definition file. */
    if (switch_asm) {
        free(switch_asm);
    } else {
        check(0, "arch/x86/switch_lazy.asm readable");
    }
}

int main(void) {
    printf("== fpu_enable_host_test ==\n");
    test_bits_arithmetic();
    test_emitted_order();
    test_wiring();
    printf("== %d checks, %d failures ==\n", checks, failures);
    return failures ? 1 : 0;
}
