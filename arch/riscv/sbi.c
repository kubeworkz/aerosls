#include "sbi.h"
#include "plic.h"
#include "trap_riscv.h"   /* rv_halt() -- the shell's `exit` command (Phase 9i) */

/* QEMU virt 16550 UART (Phase 9g) -- the console for the M-mode build
 * (QEMU `-bios none -kernel`): there is no OpenSBI, so no SBI console
 * extension exists at all, and the console is the raw 16550 the virt
 * machine maps at physical 0x10000000 (the standard first-UART base;
 * paging is Bare in M-mode -- satp is never written -- so the MMIO
 * access is a plain volatile load/store). The S-mode build (no
 * RISCV_MMODE) keeps using SBI_DBCN below. The variant is baked in at
 * build time rather than detected at runtime, because reading mstatus
 * (the only CSR that would reveal the privilege mode) traps as an
 * illegal instruction from S-mode -- see AeroSLS-SIMI-ISA-v0.1.md §16
 * Phase 9g. */
#define VIRT_UART_BASE 0x10000000UL

/* Phase 9i: read one byte from the 16550 RECEIVER BUFFER directly -- the
 * device-driven contract for the interrupt handler. The byte that raised
 * the UART's interrupt line (and thus SEIP through the PLIC) is in the
 * hardware FIFO; reading it here is what empties the FIFO and lets the
 * line deassert. sbi_getchar() (the SBI console) is the firmware's
 * polling view of the SAME UART -- equivalent bytes, but the
 * interrupt-driven path is supposed to talk to the device, not the
 * firmware. Available in both modes (the UART is at the same physical
 * address with paging Bare). */
static int uart_rx_byte(void) {
    volatile uint8_t* lsr = (volatile uint8_t*)(VIRT_UART_BASE + 5);
    volatile uint8_t* rbr = (volatile uint8_t*)(VIRT_UART_BASE + 0);
    if ((*lsr & 0x01) == 0) return -1;   /* LSR bit 0: RX data ready */
    return (int)(unsigned char)*rbr;
}

#if defined(RISCV_MMODE)

static void uart_putchar(char c) {
    volatile uint8_t* lsr = (volatile uint8_t*)(VIRT_UART_BASE + 5);
    volatile uint8_t* thr = (volatile uint8_t*)(VIRT_UART_BASE + 0);
    while ((*lsr & 0x20) == 0) { }   /* LSR bit 5: THR empty -- wait for it */
    *thr = (uint8_t)c;
}

static int uart_getchar(void) {
    return uart_rx_byte();
}

void sbi_putchar(char c) {
    uart_putchar(c);
}

int sbi_getchar(void) {
    return uart_getchar();
}

#else /* S-mode under OpenSBI */

/* SBI DBCN (v2.0 spec 4.2) carries its buffer addresses as lo/hi 32-bit
 * pairs: a1=base_lo, a2=base_hi, a3=written/read_lo, a4=written/read_hi.
 * The generic 3-arg sbi_call() cannot express this -- passing the
 * written-count pointer as arg2 would put it in a2 (base_hi), corrupting
 * the base address so the call always errors. That was the actual state
 * of the code below until the trap-entry fixture exposed it (ISA doc
 * §16 Phase 9h): DBCN always failed and printing rode on the legacy
 * fallback, which this OpenSBI build (v1.3) still serves despite the
 * spec deprecation. On RV64/Sv39 the hi halves are 0. */
static struct SBIReturn sbi_dbcn(unsigned long fid, unsigned long num_bytes,
                                 unsigned long base_addr, unsigned long count_addr) {
    struct SBIReturn ret;
    register unsigned long a0 __asm__("a0") = num_bytes;
    register unsigned long a1 __asm__("a1") = base_addr & 0xFFFFFFFFUL; /* base_lo */
    register unsigned long a2 __asm__("a2") = 0;                        /* base_hi */
    register unsigned long a3 __asm__("a3") = count_addr & 0xFFFFFFFFUL; /* count_lo */
    register unsigned long a4 __asm__("a4") = 0;                        /* count_hi */
    register unsigned long a6 __asm__("a6") = fid;
    register unsigned long a7 __asm__("a7") = SBI_EXT_DBCN;
    __asm__ volatile("ecall"
                     : "+r"(a0), "+r"(a1)
                     : "r"(a2), "r"(a3), "r"(a4), "r"(a6), "r"(a7)
                     : "memory");
    ret.error = a0;
    ret.value = a1;
    return ret;
}

void sbi_putchar(char c) {
    /* Debug Console extension write; falls back to the legacy SBI v0.1
     * console putchar (still served by OpenSBI v1.3 despite the spec
     * deprecation) for firmwares without DBCN. Only the RETURN is
     * trusted: OpenSBI v1.3 emits the byte and returns error 0 but
     * does not reliably write the byte count back to the a3 address
     * (observed empirically by the trap-entry fixture, ISA doc §16
     * Phase 9h), so checking the count would double-print via the
     * fallback. */
    char buf = c;
    unsigned long written = 0;
    struct SBIReturn ret = sbi_dbcn(SBI_DBCN_WRITE, 1,
                                    (unsigned long)&buf, (unsigned long)&written);
    if (ret.error == 0) return;
    sbi_call(SBI_EXT_0_1_CONSOLE_PUTCHAR, 0, c, 0, 0);
}

int sbi_getchar(void) {
    /* Same shape as sbi_putchar: DBCN read with a legacy fallback.
     * Returns -1 when no character is currently waiting in the UART
     * buffer. */
    char buf = 0;
    unsigned long got = 0;
    struct SBIReturn ret = sbi_dbcn(SBI_DBCN_READ, 1,
                                    (unsigned long)&buf, (unsigned long)&got);
    if (ret.error == 0 && got > 0) return (int)(unsigned char)buf;
    struct SBIReturn legacy = sbi_call(SBI_EXT_0_1_CONSOLE_GETCHAR, 0, 0, 0, 0);
    return (int)legacy.error;
}

#endif

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

/* Phase 9i command loop: the real headless shell. Every line accumulated
 * by handle_riscv_supervisor_interrupt (with backspace editing and CR-only
 * line endings) is dispatched here. Commands:
 *   help               list the commands
 *   version            kernel version banner
 *   echo <text>        repeat the text
 *   exit               power the machine off (SBI_SRST in S-mode; a
 *                      reported halt in bare M-mode, where no firmware
 *                      exists to power off with)
 * Anything else is reported as an unknown command. This definition also
 * closes the dangling symbol that only became a hard link error once the
 * trap path actually linked (see AeroSLS-SIMI-ISA-v0.1.md §16 Phase 9c). */
static int shell_streq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a++ != *b++) return 0;
    }
    return *a == *b;
}
static void shell_print(const char* s) {
    while (*s) sbi_putchar(*s++);
}
void route_sls_shell_command(const char* buffer) {
    if (buffer[0] == '\0') { sbi_putchar('\r'); sbi_putchar('\n'); return; }

    if (shell_streq(buffer, "help")) {
        shell_print("[HELP] commands: help, version, echo <text>, exit\r\n");
        return;
    }
    if (shell_streq(buffer, "version")) {
        shell_print("[VER] AeroSLS RISC-V kernel (SIMI Phase 9i command loop)\r\n");
        return;
    }
    if (shell_streq(buffer, "exit")) {
#if defined(RISCV_MMODE)
        shell_print("[EXIT] no firmware to power off -- halting hart.\r\n");
        rv_halt();
#else
        shell_print("[EXIT] powering off via OpenSBI SBI_SRST (SHUTDOWN).\r\n");
        sbi_system_reset();
        shell_print("[EXIT] SBI_SRST unsupported or failed -- halting hart instead.\r\n");
        rv_halt();
#endif
        return;
    }
    if (buffer[0] == 'e' && buffer[1] == 'c' && buffer[2] == 'h' && buffer[3] == 'o' &&
        buffer[4] == ' ') {
        shell_print("[ECHO] ");
        for (const char* p = buffer + 5; *p; p++) sbi_putchar(*p);
        sbi_putchar('\r'); sbi_putchar('\n');
        return;
    }

    shell_print("[ERR] unknown command: \"");
    for (const char* p = buffer; *p; p++) sbi_putchar(*p);
    shell_print("\"\r\n");
}

// Invoked from riscv_trap_dispatch (arch/riscv/trap_riscv.c) for
// asynchronous external interrupts (scause/mcause Bit 63 = 1). Cause 9 is
// SEIP (supervisor external, the PLIC's S-mode context line); cause 11
// is MEIP (machine external, the PLIC's M-mode context line -- the
// Phase 9i M-mode twin, where the bare-metal build has no firmware and
// the interrupt is taken in M-mode at mtvec). Both are the same PLIC/
// UART device path; plic.c's plic_claim/complete resolve to the mode's
// own context.
void handle_riscv_supervisor_interrupt(uint64_t scause, uint64_t stval) {
    (void)stval; // Avoid unreferenced variable warnings
    
    // Check if the cause is an external interrupt (IRQ 9 from PLIC/UART in
    // S-mode, IRQ 11 in M-mode)
    if ((scause & (1ULL << 63)) &&
        ((scause & 0xFF) == 9 || (scause & 0xFF) == 11)) {
        // Phase 9i: claim the PLIC source for hart 0's S-mode context --
        // returns 10 (the UART). Claiming masks the source while the
        // handler drains, so a re-asserted line cannot double-deliver;
        // the matching complete() at the end unmasks it for the next
        // character.
        uint32_t irq = plic_claim_interrupt(0);

        // Drain the 16550 receiver buffer DIRECTLY (uart_rx_byte, not
        // the SBI console): the bytes that raised the line are in the
        // hardware FIFO, and emptying it is what lets the line (and
        // thus SEIP) deassert. Level-triggered RX: any bytes that
        // arrive while the handler runs simply stay in the FIFO and
        // re-assert the line after the complete() -- the next interrupt
        // drains them.
        while (1) {
            int input_char = uart_rx_byte();
            if (input_char == -1) break; // Buffer empty

            char c = (char)input_char;

            if (c == '\r' || c == '\n') {
                // Line end (a real terminal sends CR-only on Enter):
                // terminate the buffer and dispatch it. Echo a proper
                // CRLF so the terminal cursor returns to column 0.
                riscv_shell_input_buffer[buf_cursor] = '\0';
                sbi_putchar('\r'); sbi_putchar('\n');
                
                // Route the buffer to the command dispatcher
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

        // Acknowledge the PLIC: writing the claimed source back unmasks
        // it for the next interrupt (Phase 9i -- without this the line
        // would be stuck claimed and no further UART interrupt could
        // ever fire).
        plic_complete_interrupt(0, irq);
    }
}