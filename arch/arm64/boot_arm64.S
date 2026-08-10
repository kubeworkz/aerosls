/* arch/arm64/boot_arm64.S — M4a/M4c/M5: the arm64 kernel head.
 *
 * The exception-level reality of qemu -M virt was pinned empirically
 * (plan doc §10.188): `-machine virt,virtualization=on` (NOT secure)
 * gives the SMC PSCI conduit and enters the -kernel payload at EL2; the
 * head drops EL2 -> EL1 (HCR_EL2.RW must be set: it resets to 0, i.e.
 * AArch32 EL1), and PSCI SYSTEM_OFF (`smc #0`) from EL1 is then
 * intercepted by QEMU's PSCI emulation — the machine powers off and
 * qemu exits rc=0, the SBI_SRST analog.
 *
 * M5 (plan doc §6 M5 / §10.191) — the TTBR1 split. The kernel is
 * LINKED at a high virtual base (0xFFFF000040080000, the TTBR1 half)
 * with physical LMAs (linker_arm64.ld). The whole MMU-off phase runs
 * at the PHYSICAL address of that image: because the loaded image
 * preserves the link-time differences, every adrp/add reference made
 * while the PC is at (VMA - __virt_off) resolves to a PHYSICAL address
 * — exactly what the MMU-off head needs. The sole exception is the
 * entry stub (VMA=LMA=0x40080000, so the ELF's e_entry is physical and
 * qemu starts there): from its physical VMA domain, adrp of a high-VMA
 * symbol yields the HIGH VA, so the stub subtracts __virt_off.
 *
 * The head builds the boot page tables in assembly (physical stores to
 * BSS tables, pre-zeroed by the BSS clear): one L0/L1/L2 tree with
 * 2 MiB BLOCK descriptors covering 0x40000000..0x42000000 (the whole
 * image+stack) plus an L3 device page for the PL011 at 0x09000000 —
 * the same tree serves TTBR1 (kernel high VAs) and, during the
 * transition, TTBR0 (the identity low VAs). TCR uses T0SZ=T1SZ=16 with
 * TG1=4 KiB, so after enabling, the head jumps to the high VA, points
 * TTBR0 at an empty root (the kernel becomes TTBR1-pure: low VAs are
 * unmapped-to-kernel), and installs VBAR_EL1.
 *
 * The vector table (VBAR_EL1 + 0x580 = synchronous exception from a
 * lower AArch64 EL) is how the EL0 SIMI program returns: its trampoline
 * `br x30`s into a user-mode `svc #0` stub, and the handler saves the
 * result (still in t0/x9) to a kernel global and erets back to the
 * kernel continuation. All handler references are TTBR1 (high VA) —
 * TTBR0 is still the user tables at that moment, which is fine. */
.section .text.entry
.globl _start
_start:
    /* Physical 0x4007F000, MMU off. Branch to the kernel body's
     * physical base (linker-defined __head_phys = .text LMA = the
     * 0x40080000 the head physically runs at). */
    ldr     x0, =__head_phys
    br      x0

.section .text.init
.globl head_start
head_start:
    /* Clear BSS (physical addresses — PC is at (VMA - __virt_off), so
     * adrp/add resolves physically). 16-byte strides. */
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
3:  /* EL3 -> EL1 (see the §10.188 findings: SCR_EL3 needs NS|RW=0x401
     * or the eret never leaves EL3). */
    mov     x3, #0x401
    msr     scr_el3, x3
    adr     x3, 5f
    msr     elr_el3, x3
    mov     x3, #0x3c5
    msr     spsr_el3, x3
    eret
4:  /* EL2 -> EL1 (HCR_EL2.RW = AArch64 EL1). */
    mov     x3, #0x80000000
    msr     hcr_el2, x3
    adr     x3, 5f
    msr     elr_el2, x3
    mov     x3, #0x3c5
    msr     spsr_el2, x3
    eret
5:
    /* EL1, MMU off, PC physical. Build the boot page tables. */
    adrp    x0, g_boot_pg_l0
    add     x0, x0, :lo12:g_boot_pg_l0     /* x0 = phys L0 */
    adrp    x1, g_boot_pg_l1
    add     x1, x1, :lo12:g_boot_pg_l1     /* x1 = phys L1 */
    adrp    x2, g_boot_pg_l2
    add     x2, x2, :lo12:g_boot_pg_l2     /* x2 = phys L2 */
    adrp    x3, g_boot_pg_uart
    add     x3, x3, :lo12:g_boot_pg_uart   /* x3 = phys UART L3 */

    orr     x4, x1, #3              /* L0[0] = L1 table */
    str     x4, [x0]
    orr     x4, x2, #3              /* L1[0] and L1[1] = L2 table: */
    str     x4, [x1]                /*   L1[0] covers the UART's 0..1 GiB, */
    str     x4, [x1, #8]            /*   L1[1] the kernel's [1,2) GiB — */
                                    /*   0x40000000 is exactly 1 GiB (§10.190) */

    /* L2[0..15]: 2 MiB BLOCK descriptors for 0x40000000..0x42000000.
     * Block low bits: 0b01 | AF(1<<10) | attr1(1<<2) = 0x405, AP=00
     * (EL1-only — EL0 cannot touch the kernel map). */
    mov     x4, #0x405
    mov     x5, #0x40000000         /* base PA (movz x5, #0x4000, lsl #16) */
    orr     x6, x5, x4              /* first block descriptor */
    mov     x7, xzr
    mov     x8, #16
6:  str     x6, [x2, x7, lsl #3]
    add     x6, x6, #0x200000       /* next 2 MiB */
    add     x7, x7, #1
    subs    x8, x8, #1
    b.ne    6b

    orr     x4, x3, #3              /* L2[72] = UART L3 table */
    str     x4, [x2, #(72 * 8)]

    /* UART L3[0]: 0x09000000 | 0b11 | AF | PXN (attr 0 = device).
     * 0x403 (0b11 | AF) is not a logical-immediate bitmask, so add it. */
    mov     x4, #0x9000000          /* movz x4, #0x90, lsl #16 */
    add     x4, x4, #0x403          /* 0b11 | AF */
    movk    x4, #0x20, lsl #48      /* PXN = 1<<53 */
    str     x4, [x3]

    /* M5.2: the GICv2 device pages (the kernel is TTBR1-pure, so the
     * distributor 0x08000000 + CPU interface 0x08010000 are mapped at
     * high VAs, the UART pattern — one L3 table under L2[64], which
     * covers the 0x08000000 2 MiB region). L3[0] = distributor,
     * L3[16] = 0x08010000 CPU interface. Same device descriptor
     * (0b11 | AF | PXN) as the UART. */
    adrp    x3, g_boot_pg_gic
    add     x3, x3, :lo12:g_boot_pg_gic   /* x3 = phys GIC L3 */
    orr     x4, x3, #3
    str     x4, [x2, #(64 * 8)]           /* L2[64] = GIC L3 table */
    mov     x4, #0x8000000          /* 0x08000000 (assembler-synthesized) */
    add     x4, x4, #0x403          /* 0b11 | AF */
    movk    x4, #0x20, lsl #48      /* PXN = 1<<53 */
    str     x4, [x3]                /* L3[0]: 0x08000000 distributor */
    mov     x4, #0x8010000          /* 0x08010000 */
    add     x4, x4, #0x403          /* 0b11 | AF */
    movk    x4, #0x20, lsl #48      /* PXN = 1<<53 */
    str     x4, [x3, #(16 * 8)]     /* L3[16]: 0x08010000 CPU iface */

    /* Program the regime and enable (ARM ARM transition order). */
    mov     x0, #0xff00             /* MAIR: attr0=device, attr1=WBWA */
    msr     mair_el1, x0
    /* TCR: T0SZ=16 | (TG1=4K<<30 | T1SZ=16)<<16 | IPS=40bit<<32
     * = 0x280100010 — built explicitly, it is not a mov-able value. */
    movz    x0, #0x10
    movk    x0, #0x8010, lsl #16    /* TG1=0b10<<30 | T1SZ=16 */
    movk    x0, #0x2, lsl #32       /* IPS=0b010 (40-bit PA) */
    msr     tcr_el1, x0
    adrp    x0, g_boot_pg_l0
    add     x0, x0, :lo12:g_boot_pg_l0   /* physical root */
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0
    dsb     ish
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    orr     x0, x0, #1              /* M */
    orr     x0, x0, #(1 << 2)       /* C */
    orr     x0, x0, #(1 << 12)      /* I */
    msr     sctlr_el1, x0
    isb

    /* Jump to the high VA. adrp at the physical PC gives the PHYSICAL
     * address of 7f; add the PURE __virt_off to cross into the TTBR1
     * half. movz (not movk) is load-bearing: movk would keep x1's stale
     * low 32 bits and the add would double-count the 0x40000000 base —
     * the first M5 bug, a carry into bit 32 landing the jump at
     * L1[2] (0xFFFF00008010C14C) with a level-1 translation fault
     * (§10.191). */
    adrp    x0, 7f
    add     x0, x0, :lo12:7f
    movz    x1, #0xffff, lsl #48   /* x1 = exactly 0xFFFF000000000000 */
    add     x0, x0, x1
    br      x0
7:
    /* High PC: TTBR1 is live. Stack, vectors, then drop the identity
     * map — the kernel becomes TTBR1-pure. */
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0
    adrp    x0, arm64_vectors
    add     x0, x0, :lo12:arm64_vectors
    msr     vbar_el1, x0
    adrp    x0, g_boot_pg_empty
    add     x0, x0, :lo12:g_boot_pg_empty
    movz    x1, #0xffff, lsl #48   /* pure __virt_off (movz: no stale low bits) */
    sub     x0, x0, x1              /* physical empty root */
    msr     ttbr0_el1, x0
    tlbi    vmalle1
    dsb     ish
    isb
    bl      kernel_arm64_main

    /* Panic loop — reached only if the C main returns (it shouldn't). */
8:  wfi
    b       8b

/* ─── The boot page tables (BSS, pre-zeroed by the BSS clear) ─────── */
.section .bss
.balign  4096
.globl g_boot_pg_l0
g_boot_pg_l0:   .space 4096
.globl g_boot_pg_l1
g_boot_pg_l1:   .space 4096
.globl g_boot_pg_l2
g_boot_pg_l2:   .space 4096
.globl g_boot_pg_uart
g_boot_pg_uart: .space 4096
.globl g_boot_pg_gic
g_boot_pg_gic:  .space 4096
/* The kernel's TTBR0 root: all-invalid, so low VAs fault (the kernel
 * lives entirely in TTBR1; the user tables replace this for EL0 runs). */
.globl g_boot_pg_empty
g_boot_pg_empty: .space 4096

/* ─── The vector table (VBAR_EL1): 16 slots of 128 bytes ──────────── */
.macro VEC_SLOT target
    b       \target
    .fill   31, 4, 0
.endm

.section .text.vectors, "ax"
.balign  0x800
.globl arm64_vectors
/* Per-group order is Synchronous, IRQ, FIQ, SError (the first M5
 * vector bug was writing SError first, so the SVC landed on a stub:
 * sync-from-lower-A64 is at +0x400, not +0x580 — §10.191). */
arm64_vectors:
    VEC_SLOT arm64_vector_stub      /* 0x000 sync,  EL1t  */
    VEC_SLOT arm64_vector_stub      /* 0x080 IRQ,   EL1t  */
    VEC_SLOT arm64_vector_stub      /* 0x100 FIQ,   EL1t  */
    VEC_SLOT arm64_vector_stub      /* 0x180 SError, EL1t */
    VEC_SLOT arm64_vector_stub      /* 0x200 sync,  EL1h  */
    VEC_SLOT arm64_irq_el1h        /* 0x280 IRQ,   EL1h  ← M5.2 tick */
    VEC_SLOT arm64_vector_stub      /* 0x300 FIQ,   EL1h  */
    VEC_SLOT arm64_vector_stub      /* 0x380 SError, EL1h */
    VEC_SLOT arm64_svc_from_el0     /* 0x400 sync,  lower A64 ← SVC */
    VEC_SLOT arm64_vector_stub      /* 0x480 IRQ,   lower A64 */
    VEC_SLOT arm64_vector_stub      /* 0x500 FIQ,   lower A64 */
    VEC_SLOT arm64_vector_stub      /* 0x580 SError, lower A64 */
    VEC_SLOT arm64_vector_stub      /* 0x600 sync,  lower A32 */
    VEC_SLOT arm64_vector_stub      /* 0x680 IRQ,   lower A32 */
    VEC_SLOT arm64_vector_stub      /* 0x700 FIQ,   lower A32 */
    VEC_SLOT arm64_vector_stub      /* 0x780 SError, lower A32 */

/* M5.2 EL1h IRQ entry (VBAR+0x280): the generic-timer tick. The
 * interrupted C code (the idle loop in kernel_arm64.c) may hold live
 * values in any register, so the entry saves everything it will clobber
 * — the caller-saved x0-x17 plus x30 (the C handler preserves the
 * callee-saved x19-x28/x29 per the ABI, and SP is untouched) — calls
 * arm64_tick_irq, restores, erets. 18 * 8 = 144 bytes, 16-aligned. */
arm64_irq_el1h:
    sub     sp, sp, #(18 * 8)
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    str     x30, [sp, #136]
    bl      arm64_tick_irq
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldr     x30, [sp, #136]
    add     sp, sp, #(18 * 8)
    eret

/* SVC from the EL0 SIMI program: the trampoline `br x30`-ed into a
 * user-mode `svc #0` stub with the result still in t0 (x9). Save it to
 * the kernel global and eret back to the kernel continuation. All
 * references here are TTBR1 (high VA) — TTBR0 is still the user
 * tables, which is fine: the kernel is TTBR1-pure (§10.191). */
arm64_svc_from_el0:
    adrp    x0, g_user_result
    add     x0, x0, :lo12:g_user_result
    str     x9, [x0]
    adrp    x0, g_user_ret_addr
    add     x0, x0, :lo12:g_user_ret_addr
    ldr     x1, [x0]
    msr     elr_el1, x1
    mov     x1, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el1, x1
    eret

/* Stray-vector hang: any exception this kernel does not handle stalls
 * here (the boot smoke never takes one). */
arm64_vector_stub:
    wfi
    b       arm64_vector_stub
