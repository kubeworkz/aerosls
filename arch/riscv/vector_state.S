.text
.global riscv_vector_save_state
.global riscv_vector_load_state

# riscv_vector_save_state: Dump v0-v31 out to memory buffer
# a0 = Physical base target pointer to vector_state_buffer
riscv_vector_save_state:
    # Initialize vector configuration element size to 64-bit, maximum multiplier grouping (m8)
    vsetvli t0, zero, e64, m8, ta, ma
    
    # Execute vectorized sequential baseline save operations
    # vse64.v writes full 64-bit element structures consecutively out via hardware DMA paths
    vse64.v v0,  (a0)
    addi    a0, a0, 64   # Increment address pointer by base register footprint
    vse64.v v8,  (a0)
    addi    a0, a0, 64
    vse64.v v16, (a0)
    addi    a0, a0, 64
    vse64.v v24, (a0)
    ret

# riscv_vector_load_state: Pull data out of memory buffer into v0-v31
# a0 = Physical base source pointer to vector_state_buffer
riscv_vector_load_state:
    vsetvli t0, zero, e64, m8, ta, ma
    
    # Execute vectorized sequential baseline read operations
    vle64.v v0,  (a0)
    addi    a0, a0, 64
    vle64.v v8,  (a0)
    addi    a0, a0, 64
    vle64.v v16, (a0)
    addi    a0, a0, 64
    vle64.v v24, (a0)
    ret