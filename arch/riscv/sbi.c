#include "sbi.h"

void sbi_putchar(char c) {
    // Invoke historical extension or modern debug console write strings
    sbi_call(SBI_EXT_0_1_CONSOLE_PUTCHAR, 0, c, 0);
}

int sbi_getchar(void) {
    struct SBIReturn ret = sbi_call(SBI_EXT_0_1_CONSOLE_GETCHAR, 0, 0, 0);
    // Value returns -1 if no character is currently waiting in the hardware UART buffer
    return (int)ret.error; 
}

// Global text canvas array used to buffer incoming shell commands from the virtual UART
#define SHELL_BUF_SIZE 256
static char riscv_shell_input_buffer[SHELL_BUF_SIZE];
static uint32_t buf_cursor = 0;

extern void route_sls_shell_command(const char* buffer);

// Registered inside the stvec (Supervisor Trap Vector Base Address Register) 
// to process asynchronous Supervisor Software / External Interrupts (scause Bit 63 = 1, Code = 9)
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