# boot_riscv.S - Low-Level RISC-V Bootstrap Entry
.section .text.init
.global _start

_start:
    # 1. Disable all interrupts globally in the Supervisor Status Register (sstatus)
    csrw sie, zero

#if defined(RISCV_MMODE)
    # 1b. M-mode direct boot (QEMU `-bios none -kernel`): every hart runs
    #     the reset vector, so hart 0 is the boot hart and the rest park
    #     right here, before any shared state (stack, BSS) is touched.
    #     This is the one place the two build variants (Phase 9g) differ:
    #     the S-mode build compiles this whole block out -- OpenSBI
    #     delivers the payload to exactly one boot hart, whose id is NOT
    #     necessarily 0, and parks the secondaries itself. The variant is
    #     baked in at build time (-DRISCV_MMODE) rather than detected at
    #     runtime, because reading mstatus (the MPP-based detection)
    #     traps as illegal_instruction from S-mode -- see §16 Phase 9g.
    bnez  a0, .park_secondary
#endif

    # 3. Setup a clean, page-aligned temporary compilation bootstrap stack
    la   sp, bsp_stack_top      # Load address of the top of the stack

    # 4. Clear the BSS uninitialized data segment to guarantee standard zero-state rules
    la   t0, __bss_start
    la   t1, __bss_end
.clear_bss_loop:
    bgeu t0, t1, .jump_to_c_kernel
    sd   zero, 0(t0)            # Store 8 bytes of zeros
    addi t0, t0, 8
    j    .clear_bss_loop

.jump_to_c_kernel:
    # Set up arguments for the C entry point function matching the calling convention
    # a0 already holds Hart ID, a1 already holds FDT physical pointer
    jal  kernel_riscv_main      # Leap directly into C subsystem logic

.halt:
    wfi                         # Wait For Interrupt loop safeguard
    j    .halt

# Secondary harts in a direct M-mode boot (QEMU -bios none) park here, before
# touching the shared boot stack or clearing BSS. (Under OpenSBI they never
# enter this code at all -- OpenSBI parks them.)
.park_secondary:
    wfi
    j    .park_secondary

# Allocate memory areas inside the uninitialized BSS block data segments
.section .bss
.align 12                       # Force 4KB page alignment
bsp_stack_bottom:
    .space 4096 * 4             # 16KB total temporary boot stack allocation
bsp_stack_top:
