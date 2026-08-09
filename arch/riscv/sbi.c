#include "sbi.h"

void sbi_putchar(char c) {
    /* OpenSBI >= 0.9 removed the legacy console putchar extension
     * (SBI_EXT_0_1_CONSOLE_PUTCHAR) for S-mode guests -- the call is
     * silently dropped, so a kernel printing through it appears mute.
     * Use the Debug Console extension (SBI_DBCN) instead, with the
     * legacy call as a fallback for older firmwares. */
    char buf = c;
    unsigned long written = 0;
    struct SBIReturn ret = sbi_call(SBI_EXT_DBCN, SBI_DBCN_WRITE,
                                    1, (unsigned long)&buf, (unsigned long)&written);
    if (ret.error == 0) return;
    sbi_call(SBI_EXT_0_1_CONSOLE_PUTCHAR, 0, c, 0, 0);
}

int sbi_getchar(void) {
    /* Same story as sbi_putchar: legacy getchar is gone for S-mode
     * guests, so read via SBI_DBCN with a legacy fallback. Returns -1
     * when no character is currently waiting in the UART buffer. */
    char buf = 0;
    unsigned long got = 0;
    struct SBIReturn ret = sbi_call(SBI_EXT_DBCN, SBI_DBCN_READ,
                                    1, (unsigned long)&buf, (unsigned long)&got);
    if (ret.error == 0 && got > 0) return (int)(unsigned char)buf;
    struct SBIReturn legacy = sbi_call(SBI_EXT_0_1_CONSOLE_GETCHAR, 0, 0, 0, 0);
    return (int)legacy.error;
}

void sbi_system_reset(void) {
    /* System Reset Extension (SBI v0.3+/v1.0): an ecall to M-mode
     * OpenSBI, which performs the reset on the firmware's behalf. A
     * SHUTDOWN reset on QEMU -M virt powers the machine off -- the qemu
     * process exits rc=0 -- which is the real, honest "exit" for the
     * RISC-V port: the kernel has no process table to tear down (see
     * riscv_syscall_dispatch()'s comment in trap_riscv.c). Returns
     * (error in a0) only if the firmware lacks the extension; callers
     * must not assume the machine is gone just because the call returned. */
    sbi_call(SBI_EXT_SRST, SBI_SRST_RESET,
             SBI_SRST_RESET_TYPE_SHUTDOWN, SBI_SRST_RESET_REASON_NONE, 0);
}

// Global text canvas array used to buffer incoming shell commands from the virtual UART
#define SHELL_BUF_SIZE 256
static char riscv_shell_input_buffer[SHELL_BUF_SIZE];
static uint32_t buf_cursor = 0;

/* The x86 kernel routes UART shell lines through route_sls_shell_command();
 * the RISC-V port has no SLS shell yet, so the honest behavior is to
 * report the received line and continue. This definition closes the
 * dangling symbol that only becomes a hard link error once the trap
 * path actually links (see AeroSLS-SIMI-ISA-v0.1.md section 16 Phase 9c). */
void route_sls_shell_command(const char* buffer) {
    const char* prefix = "[SHELL] no SLS shell on the RISC-V port; received: \"";
    for (const char* p = prefix; *p; p++) sbi_putchar(*p);
    for (const char* p = buffer; *p; p++) sbi_putchar(*p);
    sbi_putchar('\"'); sbi_putchar('\n');
}

// Invoked from riscv_trap_dispatch (arch/riscv/trap_riscv.c) for
// asynchronous Supervisor External Interrupts (scause Bit 63 = 1, Code = 9)
void handle_riscv_supervisor_interrupt(uint64_t scause, uint64_t stval) {
    (void)stval; // Avoid unreferenced variable warnings
    
    // Check if the cause is a Supervisor External Interrupt (IRQ 9 from PLIC/UART)
    if ((scause & (1ULL << 63)) && (scause & 0xFF) == 9) {
        
        // Drain the virtual UART buffer using OpenSBI firmware getchar calls
        while (1) {
            int input_char = sbi_getchar();
            if (input_char == -1) break; // Buffer empty

            char c = (char)input_char;

            if (c == '\r' || c == '\n') {
                // Return / Enter Key: Terminate string and evaluate command
                riscv_shell_input_buffer[buf_cursor] = '\0';
                sbi_putchar('\n'); // Echo newline to terminal output window
                
                // Route the buffer straight to the Single-Level Storage shell execution matrix
                route_sls_shell_command(riscv_shell_input_buffer);
                
                // Reset buffer cursor for next input command stream loop
                buf_cursor = 0;
            } 
            else if (c == 0x7F || c == '\b') {
                // Backspace handling
                if (buf_cursor > 0) {
                    buf_cursor--;
                    sbi_putchar('\b'); sbi_putchar(' '); sbi_putchar('\b'); // Handle terminal visual wipe
                }
            } 
            else if (buf_cursor < (SHELL_BUF_SIZE - 1)) {
                // Standard ASCII payload character alphanumeric data parsing
                riscv_shell_input_buffer[buf_cursor++] = c;
                sbi_putchar(c); // Echo character back to user terminal display
            }
        }
    }
}