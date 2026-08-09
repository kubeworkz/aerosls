# boot_riscv.S - Low-Level RISC-V Bootstrap Entry
.section .text.init
.global _start

_start:
    # 1. Disable all interrupts globally in the Supervisor Status Register (sstatus)
    csrw sie, zero

    # 2. Serialize Core Bootup (Symmetric Multiprocessing Isolation)
    # a0 contains the Hart ID. Only Hart 0 is allowed to initialize the main systems.
    # Secondary harts (APs) are parked in a spin loop awaiting synchronization markers.
    bnez a0, .park_secondary_hart

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

.park_secondary_hart:
    # Secondary cores spin here until Hart 0 configures the primary Sv39 page directories
    la   t0, flag_cores_synchronized
.spin_lock_check:
    ld   t1, 0(t0)
    bnez t1, .ap_boot_bridge
    nop                         # Conserve execution pipeline energy (hint)
    j    .spin_lock_check

.ap_boot_bridge:
    # Setup unique stack mapping offsets computed out of Hart IDs
    slli t2, a0, 12             # Multiply Hart ID by 4096 (4KB unique stack block)
    la   sp, bsp_stack_top
    sub  sp, sp, t2             # Assign unique offset stack context

    jal  ap_riscv_kernel_main

# Allocate memory areas inside the uninitialized BSS block data segments
.section .bss
.align 12                       # Force 4KB page alignment
bsp_stack_bottom:
    .space 4096 * 4             # 16KB total temporary boot stack allocation
bsp_stack_top:

.global flag_cores_synchronized
.align 3
flag_cores_synchronized:
    .quad 0                     # Set to 1 by Hart 0 to release parked secondary cores
