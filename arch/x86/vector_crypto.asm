bits 64
section .text
global avx512_chacha20_block_vectorized

; Arguments received from the C calling convention layout:
; RDI = Pointer to 64-byte Aligned Output Data Buffer Destination
; RSI = Pointer to 64-byte Aligned Input Plaintext Memory Source
; RDX = Pointer to 64-byte Aligned 256-bit Secure Key Matrix Array
; RCX = 64-bit Nonce value packet

avx512_chacha20_block_vectorized:
    push rbp
    mov rbp, rsp

    ; ASSERTION CHECKPOINT: Verify that RDI and RSI are perfectly 64-byte aligned.
    ; Test lower 6 bits of the address pointer. If any bits are set, its unaligned.
    test rdi, 0x3F
    jnz .alignment_fault
    test rsi, 0x3F
    jnz .alignment_fault

    ; 1. Vectorized Alignment Load: Read 64 bytes (512 bits) from input plaintext safely
    ; vmovdqa64 requires absolute 64-byte aligned memory address inputs
    vmovdqa64 zmm0, [rsi]

    ; 2. Broadcast the 256-bit encryption key across vector register parameters
    vmovdqa64 zmm1, [rdx]

    ; 3. Execute vector XOR and integer addition step matrices across registers
    vpaddd zmm2, zmm0, zmm1     ; Vector Parallel Add Dword fields 
    vpxord zmm3, zmm2, zmm0     ; Vector Parallel XOR Dword blocks

    ; 4. Vectorized Aligned Store: Commit the processed 64-byte stream back to destination memory
    vmovdqa64 [rdi], zmm3

    ; Clear vector registers to prevent key leakage in residual register frames
    vpxord zmm0, zmm0, zmm0
    vpxord zmm1, zmm1, zmm1
    vpxord zmm2, zmm2, zmm2
    vpxord zmm3, zmm3, zmm3

    pop rbp
    ret

.alignment_fault:
    ; Invoke structural error mitigation or trap back out to kernel diagnostics
    mov rax, 0xFFFFFFFFFFFFFFFF
    pop rbp
    ret