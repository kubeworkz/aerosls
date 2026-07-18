#include "kernel_io.h"
#include "../arch/x86/vga.h"

// ─── x86 Port I/O ─────────────────────────────────────────────────────────────
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

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

// ─── Output primitives ────────────────────────────────────────────────────────
void kernel_serial_putchar(char c) {
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

// ─── read_line ────────────────────────────────────────────────────────────────
// Blocks until ENTER is pressed, echoes typed characters, stores in buf.
void read_line(char* buf) {
    int i = 0;
    for (;;) {
        // Wait for Data Ready bit in Line Status Register
        while (!(inb(SERIAL_COM1_BASE + 5) & 0x01))
            __asm__ volatile("pause");

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
    kernel_serial_print("\n[KERNEL PANIC] ");
    kernel_serial_print(msg);
    kernel_serial_print("\n-- System Halted --\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

// ─── free_kernel_memory ───────────────────────────────────────────────────────
// Stub — a full slab allocator is future work; lock-free map nodes are small
// and the kernel address space is large, so leaking is acceptable for the MVP.
void free_kernel_memory(void* ptr) {
    (void)ptr;
}
