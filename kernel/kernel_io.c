#include "kernel_io.h"
#include "smp.h"
#include "../arch/x86/vga.h"

// ─── x86 Port I/O ─────────────────────────────────────────────────────────────
/* Wrapped in #ifndef so tests/kernel_io_panic_port.h can substitute recording
 * hooks. Port I/O is privileged, so without a seam here the panic path is
 * untestable on the host -- and an untestable panic path is exactly how this
 * kernel came to have one that could not print. Same reasoning as
 * arch_read_cr3() and qemu_sls_invlpg(). */
#ifndef outb
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
#endif
#ifndef inb
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
#endif

// ─── Serial initialisation (9600 8N1, no IRQs) ────────────────────────────────
void serial_init(void) {
    outb(SERIAL_COM1_BASE + 1, 0x00); // Disable all interrupts
    outb(SERIAL_COM1_BASE + 3, 0x80); // Enable DLAB to set baud divisor
    outb(SERIAL_COM1_BASE + 0, 0x01); // Divisor low  byte (115200 baud)
    outb(SERIAL_COM1_BASE + 1, 0x00); // Divisor high byte
    outb(SERIAL_COM1_BASE + 3, 0x03); // 8-bit, no parity, 1 stop bit
    outb(SERIAL_COM1_BASE + 2, 0xC7); // FIFO enable, clear, 14-byte threshold
    outb(SERIAL_COM1_BASE + 4, 0x03); // RTS + DTR asserted
}

// ─── Output capture (see kernel_io.h's own header comment) ────────────────────
static char*  capture_buf = 0;
static size_t capture_len = 0;
static size_t capture_cap = 0;

void kernel_serial_capture_start(char* buf, size_t cap) {
    capture_buf = buf;
    capture_len = 0;
    capture_cap = cap;
    if (capture_cap > 0) capture_buf[0] = '\0';
}

size_t kernel_serial_capture_stop(void) {
    size_t n = capture_len;
    if (capture_buf && capture_cap > 0) {
        size_t term = n < capture_cap - 1 ? n : capture_cap - 1;
        capture_buf[term] = '\0';
    }
    capture_buf = 0;
    capture_len = 0;
    capture_cap = 0;
    return n;
}

/* ─── Panic-path output ────────────────────────────────────────────────────
 *
 * Everything below writes to the UART directly and touches NO global state.
 * That is the entire point, and it was bought at a cost worth recording.
 *
 * ─── What happened ────────────────────────────────────────────────────────
 * A `qemu bench 8` issued over HTTP took a kernel page fault and halted. The
 * serial log showed NOTHING: no [SLS-BENCH], no [QEMU-SLS MMU], and no [FAULT]
 * -- even though gdb later proved the CPU was stopped on the `cli; hlt` two
 * instructions PAST the kernel_serial_printf() that reports the fault.
 *
 * The reason is directly above: user/shell.c:457 calls
 * kernel_serial_capture_start() before running a command, so every character
 * the command produces is diverted into a memory buffer and never reaches the
 * UART. The command halted before the matching _stop() at shell.c:2812, so the
 * buffer was never flushed and the HTTP response was never sent. The output
 * existed; it just had nowhere to go.
 *
 * That cost an entire debugging session. Six successive hypotheses were built
 * on "the log does not show X, therefore X did not happen" -- and the log was
 * incapable of showing anything at all. This project already had the rule that
 * covers it: AN ABSENCE IS NOT A MEASUREMENT.
 *
 * ─── Why not simply call kernel_serial_capture_stop() first ───────────────
 * Because capture_buf, capture_len and capture_cap are in .bss, and .bss is
 * one of the things that can be unmapped or corrupted when the kernel is
 * panicking. A panic handler must not depend on the health of the machinery it
 * exists to report on. kernel_serial_putchar() reads capture_buf on EVERY
 * character, so it faults on its own first instruction in exactly the scenario
 * where its output matters most -- and a fault inside a fault handler is a
 * double fault, which reboots the machine and destroys the evidence.
 *
 * ─── The constraints these functions honour ───────────────────────────────
 *   - No .bss and no .data reads. SERIAL_COM1_BASE is a compile-time constant;
 *     every local here lives in a register.
 *   - No lookup tables. kernel_serial_print_hex64() indexes a static const
 *     char[]; the digit is computed arithmetically instead. (.rodata is
 *     normally mapped with .text, but the format strings are already an
 *     unavoidable .rodata dependency and there is no reason to add another.)
 *   - No VGA mirroring. vga_is_ready() reads driver state.
 *   - Minimal stack. RSP was 208 KiB outside the bootstrap stack when this was
 *     written, so the stack is not to be trusted either.
 *   - A BOUNDED wait for the transmitter, unlike kernel_serial_putchar()'s
 *     unbounded spin. A panic that hangs forever waiting on a UART that will
 *     never drain is strictly worse than a panic that drops a character: the
 *     first tells you nothing, the second tells you almost everything.
 *
 * Asserted by tests/kernel_panic_output_host_test.c, which drives these with a
 * capture buffer ACTIVE and checks the bytes arrive at the port rather than in
 * the buffer -- the far side of the boundary being crossed.
 */

/* Spin limit for the transmitter-holding-register-empty poll. At 115200 baud
 * one character is ~87 us; this is many thousands of character times, so it
 * expires only when the UART is genuinely not draining. */
#define PANIC_TX_SPIN 1000000UL

void kernel_panic_putchar(char c) {
    for (unsigned long i = 0; i < PANIC_TX_SPIN; i++) {
        if (inb(SERIAL_COM1_BASE + 5) & 0x20) break;
    }
    /* Written even if the poll expired -- see the bounded-wait note above. */
    outb(SERIAL_COM1_BASE, (uint8_t)c);
}

void kernel_panic_puts(const char* s) {
    if (!s) s = "(null)";
    while (*s) {
        if (*s == '\n') kernel_panic_putchar('\r');
        kernel_panic_putchar(*s++);
    }
}

void kernel_panic_hex64(uint64_t v) {
    kernel_panic_putchar('0');
    kernel_panic_putchar('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned nyb = (unsigned)((v >> shift) & 0xFu);
        kernel_panic_putchar(nyb < 10u ? (char)('0' + nyb)
                                       : (char)('a' + (nyb - 10u)));
    }
}

void kernel_panic_dec(uint64_t v) {
    char tmp[21];
    int  len = 0;
    if (v == 0) { kernel_panic_putchar('0'); return; }
    while (v && len < (int)sizeof tmp) { tmp[len++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (len-- > 0) kernel_panic_putchar(tmp[len]);
}

// ─── Output primitives ────────────────────────────────────────────────────────
void kernel_serial_putchar(char c) {
    if (capture_buf) {
        // Bounds-checked append; leave room for the NUL capture_stop() writes.
        if (capture_len + 1 < capture_cap) capture_buf[capture_len] = c;
        capture_len++;
        return;
    }
    // Wait until Transmit Holding Register Empty (bit 5 of LSR)
    while (!(inb(SERIAL_COM1_BASE + 5) & 0x20))
        __asm__ volatile("pause");
    outb(SERIAL_COM1_BASE, (uint8_t)c);
    // Mirror output to VGA text-mode HMI once the driver is initialised
    if (vga_is_ready()) vga_putchar(c);
}

void kernel_serial_print(const char* s) {
    while (*s) {
        if (*s == '\n') kernel_serial_putchar('\r');
        kernel_serial_putchar(*s++);
    }
}

void kernel_serial_print_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    kernel_serial_print(buf);
}
// ─── Minimal printf ───────────────────────────────────────────────────────────
// Handles: %s %-Ns %c %d %u %ld %lu %x %lx %016lx %04x %02x %Nx %%
// Does NOT require libc — uses GCC __builtin_va_* (available in -ffreestanding)

static void emit_uint(uint64_t val, int base, int width, char pad, int left) {
    char tmp[22];
    int  len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val) {
            uint32_t d = (uint32_t)(val % (uint32_t)base);
            tmp[len++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10);
            val /= (uint32_t)base;
        }
    }

    if (!left) {
        for (int i = len; i < width; i++) kernel_serial_putchar(pad);
    }
    for (int i = len - 1; i >= 0; i--) kernel_serial_putchar(tmp[i]);
    if (left) {
        for (int i = len; i < width; i++) kernel_serial_putchar(' ');
    }
}

static void emit_str(const char* s, int width, int left) {
    if (!s) s = "(null)";
    int len = 0;
    const char* p = s;
    while (*p++) len++;

    if (!left) for (int i = len; i < width; i++) kernel_serial_putchar(' ');
    while (*s) kernel_serial_putchar(*s++);
    if (left)  for (int i = len; i < width; i++) kernel_serial_putchar(' ');
}

void kernel_serial_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') { kernel_serial_putchar(*fmt); continue; }
        fmt++;
        if (!*fmt) break;
        if (*fmt == '%') { kernel_serial_putchar('%'); continue; }

        // Flags
        int left = 0;
        if (*fmt == '-') { left = 1; fmt++; }

        // Pad character
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }

        // Width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        // Length modifier
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; } // ll treated as l

        switch (*fmt) {
        case 's': {
            const char* sv = va_arg(ap, const char*);
            emit_str(sv, width, left);
            break;
        }
        case 'c':
            kernel_serial_putchar((char)va_arg(ap, int));
            break;
        case 'd': case 'i': {
            int64_t iv = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            if (iv < 0) { kernel_serial_putchar('-'); iv = -iv; width--; }
            emit_uint((uint64_t)iv, 10, width, pad, left);
            break;
        }
        case 'u':
            emit_uint(is_long ? va_arg(ap, uint64_t)
                              : (uint64_t)va_arg(ap, unsigned int),
                      10, width, pad, left);
            break;
        case 'x': case 'X':
            emit_uint(is_long ? va_arg(ap, uint64_t)
                              : (uint64_t)va_arg(ap, unsigned int),
                      16, width, pad, left);
            break;
        case 'p':
            kernel_serial_print("0x");
            emit_uint((uint64_t)(uintptr_t)va_arg(ap, void*), 16, 16, '0', 0);
            break;
        default:
            kernel_serial_putchar('%');
            kernel_serial_putchar(*fmt);
            break;
        }
    }
    va_end(ap);
}

// ─── Non-blocking console: the hardware half ──────────────────────────────────
// The line editor itself lives in kernel/console.c, deliberately: this file
// defines its own `static inline` inb()/outb(), so anything here executes real
// port I/O and cannot be exercised in a host process (it faults). Keeping the
// editing rules in a TU with no port access is what makes them testable --
// same split as boot_params.c's parser versus its multiboot2 tag walk.
#define CONSOLE_DRAIN_MAX 64   // bytes per poll; bounds a paste-flood's hold
                               // on the HTTP loop

int serial_console_poll(char* out, size_t cap) {
    for (int n = 0; n < CONSOLE_DRAIN_MAX; n++) {
        if (!(inb(SERIAL_COM1_BASE + 5) & 0x01)) return 0;   /* FIFO empty */
        if (console_feed((char)inb(SERIAL_COM1_BASE), out, cap)) return 1;
    }
    return 0;
}

// ─── read_line ────────────────────────────────────────────────────────────────
// Blocks until ENTER is pressed, echoes typed characters, stores in buf.
void read_line(char* buf) {
    int i = 0;
    for (;;) {
        /* Wait for Data Ready bit in Line Status Register.
         *
         * On a single-CPU boot with no NIC this poll IS the BSP's idle
         * loop -- http_server_run() is never entered, so without this call
         * the reconciler, flush daemon and tier manager would never run at
         * all while sitting at a prompt. No-op when an AP is online, and
         * self-rate-limiting either way (kernel/smp.h). */
        while (!(inb(SERIAL_COM1_BASE + 5) & 0x01)) {
            smp_uniprocessor_tick();
            __asm__ volatile("pause");
        }

        char c = (char)inb(SERIAL_COM1_BASE);

        if (c == '\r' || c == '\n') {
            kernel_serial_putchar('\r');
            kernel_serial_putchar('\n');
            buf[i] = '\0';
            return;
        }
        if ((c == 0x7F || c == '\b') && i > 0) {
            i--;
            kernel_serial_putchar('\b');
            kernel_serial_putchar(' ');
            kernel_serial_putchar('\b');
            continue;
        }
        if (c >= 0x20 && i < 254) {
            kernel_serial_putchar(c);  // echo
            buf[i++] = c;
        }
    }
}

// ─── kernel_panic ─────────────────────────────────────────────────────────────
void kernel_panic(const char* msg) {
    /* kernel_panic_puts(), not kernel_serial_print(). This function used the
     * latter, which means every panic raised while an HTTP shell command was
     * running went into that command's capture buffer and was never seen --
     * and a panic is precisely the event that prevents the buffer from ever
     * being flushed. The failure was silent and total. See the panic-path
     * header comment above. */
    kernel_panic_puts("\n[KERNEL PANIC] ");
    kernel_panic_puts(msg);
    kernel_panic_puts("\n-- System Halted --\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

// ─── free_kernel_memory ───────────────────────────────────────────────────────
// Stub — a full slab allocator is future work; lock-free map nodes are small
// and the kernel address space is large, so leaking is acceptable for the MVP.
void free_kernel_memory(void* ptr) {
    (void)ptr;
}
