bits 64
    section .text
    global avx512_chacha20_block_vectorized
    avx512_chacha20_block_vectorized:
    push rbp
    mov rbp, rsp
    test rdi, 0x3F
    jnz .alignment_fault
    test rsi, 0x3F
    jnz .alignment_fault
    vmovdqa64 zmm0, [rsi]
    vmovdqa64 zmm1, [rdx]
    vpaddd zmm2, zmm0, zmm1
    vpxord zmm3, zmm2, zmm0
    vmovdqa64 [rdi], zmm3
    vpxord zmm0, zmm0, zmm0
    vpxord zmm1, zmm1, zmm1
    vpxord zmm2, zmm2, zmm2
    vpxord zmm3, zmm3, zmm3
    pop rbp
    ret
    .alignment_fault:
    mov rax, -1
    pop rbp
    ret