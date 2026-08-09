# boot_riscv.S - Low-Level RISC-V Bootstrap Entry
.section .text.init
.global _start

_start:
    # 1. Disable all interrupts globally in the Supervisor Status Register (sstatus)
    csrw sie, zero

    # 2. OpenSBI (fw_dynamic) delivers the payload to exactly one hart -- the
    #    boot hart -- whose id is NOT necessarily 0 (default: the last hart).
    #    The former a0==0 gate silently parked the only hart that ever enters,
    #    so the kernel never ran under `-smp > 1` (riscv-run's own config).
    #    The delivered hart is the boot hart by construction; proceed straight
    #    to the C entry. (Secondary harts are parked inside OpenSBI; nothing
    #    in this port starts them via HSM, so no AP path exists yet.)

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

# Allocate memory areas inside the uninitialized BSS block data segments
.section .bss
.align 12                       # Force 4KB page alignment
bsp_stack_bottom:
    .space 4096 * 4             # 16KB total temporary boot stack allocation
bsp_stack_top:
