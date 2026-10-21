#ifndef ARCH_X86_FPU_ENABLE_H
#define ARCH_X86_FPU_ENABLE_H

/* fpu_enable.h — the SSE/FP control bits, split out of the boot path so a
 * host test can PIN them (tests/fpu_enable_host_test.c) instead of a comment
 * asserting them. A CR0/CR4 bit written wrong is not a compile error: with
 * CR4.OSFXSR clear, EVERY SSE instruction a ring-3 task executes takes #UD,
 * and with CR0.TS or CR0_EM left set the same instruction takes #NM or #UD
 * for the opposite reason. Both fail only at boot, against a binary the host
 * cannot run.
 *
 * Measured origin (§10.13's step-1 run, Linux-ABI design doc): the
 * designated first user reached `main`, then died inside musl's `abort()`
 * at `pxor %xmm0,%xmm0` — `rip=0x80525b0`, `[FAULT] Ring-3 fault error=0x0`
 * — because the tree wrote CR4.PAE at boot and nothing else, so the
 * OSFXSR/OSXMMEXCPT bits that make SSE legal were never set anywhere.
 *
 * What this does NOT do, named so it is not mistaken for it: CR0.TS stays
 * clear. Arming TS is the lazy-FPU switch's trigger (arch/x86/switch_lazy.asm
 * sets it, arch/x86/lazy_fpu.c's #NM handler services it — and that path is
 * unreachable today because perform_lazy_context_switch() has no caller), and
 * the handler's `xsave`/`xrstor` would themselves need CR4.OSXSAVE, which is
 * deliberately NOT set here. Leaving TS clear means #NM never fires and that
 * unfinished machinery stays safely dormant while ring-3 SSE becomes legal.
 * The consequence — no per-task XMM save/restore across a switch — is a
 * named gap, not a silent one.
 *
 * Bit positions: SDM Vol. 3A §2.5 (CR0) and §2.6 (CR4), Intel/AMD alike. */

#include <stdint.h>

/* CR0 */
#define FPU_CR0_MP   (1ull << 1)  /* monitor coprocessor — the bit the       */
                                  /* user's own request names; set with PE    */
#define FPU_CR0_EM   (1ull << 2)  /* x87/SSE emulation — set = #UD for both   */
#define FPU_CR0_TS   (1ull << 3)  /* task switched — set = #NM for both       */

/* CR4 */
#define FPU_CR4_OSFXSR     (1ull << 9)   /* enable FXSAVE/SSE state — without
                                          * it, every SSE instruction is #UD */
#define FPU_CR4_OSXMMEXCPT (1ull << 10)  /* SIMD FP exceptions deliver #XM
                                          * instead of #UD                    */

/* CPUID.1:EDX — the capability bits that say the CPU can do either at all.
 * Written to refuse-by-name rather than to write CR4 bits a CPU without SSE
 * would #GP on (a triple fault under -no-reboot, i.e. a silent reset). */
#define FPU_CPUID1_SSE  (1u << 25)
#define FPU_CPUID1_SSE2 (1u << 26)

/* MXCSR's reset value, SDM Vol. 2A: every SIMD exception masked. The gate
 * loads exactly this after fninit — FNINIT does NOT touch MXCSR (SDM), so
 * the separate ldmxcsr is what puts the SIMD unit into a defined state.
 * 0x1F80 = 8064 decimal, which is how it appears in a compiled .s (and how
 * tests/fpu_enable_host_test.c finds it there). */
#define FPU_SSE_MXCSR_DEFAULT 0x1F80u

/* Pure, no asm, no side effects — the half tests/fpu_enable_host_test.c
 * exercises: sets the two CR4 bits and preserves every other bit in the
 * word (PAE and any future one), so enabling SSE cannot silently drop a
 * feature the boot path already turned on. */
static inline uint64_t fpu_cr4_enable_sse(uint64_t cr4)
{
    return cr4 | FPU_CR4_OSFXSR | FPU_CR4_OSXMMEXCPT;
}

/* Pure the same way: forces MP on, clears TS and EM (the two bits that make
 * an FP/SSE instruction trap rather than execute), and preserves everything
 * else — PG, PE, CD, NW, WP ... — bit for bit. */
static inline uint64_t fpu_cr0_enable_fpu(uint64_t cr0)
{
    return (cr0 | FPU_CR0_MP) & ~(FPU_CR0_TS | FPU_CR0_EM);
}

/* Pure predicate over a CPUID.1:EDX value: SSE and SSE2 both present. The
 * caller supplies the register; the bit positions are this file's contract,
 * and a host test pins them against the SDM's own values. */
static inline int fpu_cpuid_has_sse(uint32_t edx)
{
    return (edx & (FPU_CPUID1_SSE | FPU_CPUID1_SSE2))
                == (FPU_CPUID1_SSE | FPU_CPUID1_SSE2);
}

/* Boot-path entry point (arch/x86/lazy_fpu.c): reads CPUID, refuses by name
 * if the CPU cannot do SSE, else writes CR4, writes CR0, `fninit`s the x87
 * core and loads the default (all-exceptions-masked) MXCSR. Call it on the
 * BSP before anything enters ring 3 — kernel.c's "4b" block does, ahead of
 * process_init(). */
void fpu_enable_sse(void);

#endif /* ARCH_X86_FPU_ENABLE_H */
