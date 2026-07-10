.text
    .global riscv_vector_save_state
    .global riscv_vector_load_state
    riscv_vector_save_state:
    vsetvli t0, zero, e64, m8, ta, ma
    vse64.v v0,  (a0)
    addi    a0, a0, 64
    vse64.v v8,  (a0)
    addi    a0, a0, 64
    vse64.v v16, (a0)
    addi    a0, a0, 64
    vse64.v v24, (a0)
    ret
    riscv_vector_load_state:
    vsetvli t0, zero, e64, m8, ta, ma
    vle64.v v0,  (a0)
    addi    a0, a0, 64
    vle64.v v8,  (a0)
    addi    a0, a0, 64
    vle64.v v16, (a0)
    addi    a0, a0, 64
    vle64.v v24, (a0)
    ret