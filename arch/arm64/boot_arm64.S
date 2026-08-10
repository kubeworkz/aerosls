/* arch/arm64/boot_arm64.S — M4a: the minimal arm64 kernel head.
 *
 * The exception-level reality of qemu -M virt was pinned empirically
 * (plan doc §10.188) and the shipped configuration is `-machine
 * virt,virtualization=on` — NOT secure: that gives the machine the SMC
 * PSCI conduit and enters the -kernel payload at EL2. The head drops
 * EL2 -> EL1 (HCR_EL2.RW must be set: it resets to 0, i.e. AArch32
 * EL1), and PSCI SYSTEM_OFF (`smc #0`) from EL1 is then intercepted by
 * QEMU's PSCI emulation — the machine powers off and qemu exits rc=0,
 * the SBI_SRST analog. The EL3 -> EL1 path is kept for robustness but
 * is NOT the shipped config: under secure=on the eret from EL3 does not
 * reliably leave EL3 in TCG (SCR_EL3 must get NS|RW=0x401 first; even
 * then the smc is delivered to an empty EL3 vector instead of the PSCI
 * emulation unless the conduit is SMC, which virtualization=on
 * guarantees).
 *
 * M4a has no MMU and no exception vectors beyond the panic loop; the
 * ladder uses no stack, so SP is only set at el1_ready where the C main
 * actually runs. Uses adrp+add (PC-relative) throughout. */
.section .text.init
.globl _start
_start:
    /* Clear BSS (__bss_start .. __bss_end, 16-byte strides). */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #16
    b       1b
2:
    /* The EL drop ladder. CurrentEL[3:2]: 0b11=EL3, 0b10=EL2, 0b01=EL1. */
    mrs     x2, CurrentEL
    cmp     x2, #0xc
    b.eq    3f                      /* EL3 -> EL2 -> EL1 */
    cmp     x2, #0x8
    b.eq    4f                      /* EL2 -> EL1 */
    b       5f                      /* already EL1 */
3:  /* EL3 -> EL1 directly (M4a has no use for EL2). Two prerequisites,
     * pinned empirically under qemu -M virt,secure=on (plan doc §10.188):
     * SCR_EL3 must get NS=1 (non-secure world) and RW=1 (lower ELs in
     * AArch64) — SCR_EL3 resets to 0, i.e. secure AArch32 lower ELs, and
     * the eret would not actually leave EL3. With SMD=0 (reset), smc from
     * the non-secure EL1 reaches the EL3 PSCI monitor. */
    mov     x3, #0x401             /* SCR_EL3: NS=bit0, RW=bit10 */
    msr     scr_el3, x3
    adr     x3, 5f
    msr     elr_el3, x3
    mov     x3, #0x3c5             /* SPSR_EL3: AArch64 EL1h */
    msr     spsr_el3, x3
    eret
4:  /* EL2 -> EL1 (only reached when the machine starts the payload at
     * EL2, e.g. virtualization=on without secure). HCR_EL2.RW=bit31 must
     * be set for the same AArch32-reset reason. */
    mov     x3, #0x80000000        /* HCR_EL2: RW=bit31 (AArch64 EL1) */
    msr     hcr_el2, x3
    adr     x3, 5f
    msr     elr_el2, x3
    mov     x3, #0x3c5             /* SPSR_EL2: AArch64 EL1h */
    msr     spsr_el2, x3
    eret
5:  /* EL1: stack, then the C main. */
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0
    bl      kernel_arm64_main

    /* Panic loop — reached only if the C main returns (it shouldn't). */
6:  wfi
    b       6b
