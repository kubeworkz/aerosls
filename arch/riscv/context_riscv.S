# context_riscv.S - Low-Level RISC-V Callee-Saved Register Context Switcher
.text
.global perform_riscv_context_switch

# Arguments received via the RISC-V Calling Convention (lp64d ABI):
# a0 = Pointer to current running thread's Task structure (contains saved sp at offset 8)
# a1 = Pointer to incoming target thread's Task structure

perform_riscv_context_switch:
    # 1. Allocate space on the current thread's stack to store 14 64-bit registers
    # 14 registers * 8 bytes = 112 bytes. Must remain 16-byte aligned.
    addi sp, sp, -112

    # 2. Save the Return Address (ra) and Callee-Saved Data/Pointer Registers
    sd   ra,  0(sp)
    sd   s0,  8(sp)   # Frame pointer (fp)
    sd   s1,  16(sp)
    sd   s2,  24(sp)
    sd   s3,  32(sp)
    sd   s4,  40(sp)
    sd   s5,  48(sp)
    sd   s6,  56(sp)
    sd   s7,  64(sp)
    sd   s8,  72(sp)
    sd   s9,  80(sp)
    sd   s10, 88(sp)
    sd   s11, 96(sp)

    # 3. Save the active user-space thread local storage base pointer pointer register (tp)
    sd   tp,  104(sp)

    # 4. Save the outgoing stack pointer (sp) value into current thread's task struct descriptor
    # For this architecture layout, assume the 'rsp' or 'sp' tracking variable resides at offset 8
    sd   sp, 8(a0)

    # ==========================================================================
    # HARDWARE TRANSITION BOUNDARY: LOADING THE INCOMING TASK CONTEXT
    # ==========================================================================

    # 5. Extract the incoming thread's saved stack pointer from offset 8
    ld   sp, 8(a1)

    # 6. Pop the callee-saved registers off the newly loaded target stack pointer location
    ld   ra,  0(sp)
    ld   s0,  8(sp)
    ld   s1,  16(sp)
    ld   s2,  24(sp)
    ld   s3,  32(sp)
    ld   s4,  40(sp)
    ld   s5,  48(sp)
    ld   s6,  56(sp)
    ld   s7,  64(sp)
    ld   s8,  72(sp)
    ld   s9,  80(sp)
    ld   s10, 88(sp)
    ld   s11, 96(sp)
    ld   tp,  104(sp) # Restore target user thread storage base register

    # 7. Collapse the stack frame pointer allocation back to its clean state
    addi sp, sp, 112

    # 8. Return down the wire. 
    # Because 'ra' was overwritten with the incoming thread's return address, 
    # the register jump returns control directly into the target thread's execution path.
    ret