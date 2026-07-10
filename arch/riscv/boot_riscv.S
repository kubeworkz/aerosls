.section .text.init
    .global _start
    _start:
    csrw sie, zero
    bnez a0, .park_secondary_hart
    la   sp, bsp_stack_top
    la   t0, __bss_start
    la   t1, __bss_end
    .clear_bss_loop:
    bgeu t0, t1, .jump_to_c_kernel
    sd   zero, 0(t0)
    addi t0, t0, 8
    j    .clear_bss_loop
    .jump_to_c_kernel:
    extern kernel_riscv_main
    jal  kernel_riscv_main
    .halt:
    wfi
    j    .halt
    .park_secondary_hart:
    la   t0, flag_cores_synchronized
    .spin_lock_check:
    ld   t1, 0(t0)
    bnez t1, .ap_boot_bridge
    .pause:
    j    .spin_lock_check
    .ap_boot_bridge:
    slli t2, a0, 12
    la   sp, bsp_stack_top
    sub  sp, sp, t2
    extern ap_riscv_kernel_main
    jal  ap_riscv_kernel_main
    .section .bss
    .align 12
    bsp_stack_bottom: .space 4096 * 4
    bsp_stack_top:
    .global flag_cores_synchronized
    .align 3
    flag_cores_synchronized: .quad 0