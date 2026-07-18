#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include <stdint.h>
#include <stdarg.h>

// ─── COM1 Serial Port (QEMU: -serial file:...) ────────────────────────────────
#define SERIAL_COM1_BASE  0x3F8u

void serial_init(void);

void kernel_serial_putchar(char c);
void kernel_serial_print(const char* s);
void kernel_serial_print_hex64(uint64_t v);
void kernel_serial_printf(const char* fmt, ...);

// Blocking read of one line from COM1 into buf (max 255 chars + NUL)
void read_line(char* buf);

// Print message to serial and halt all cores
void kernel_panic(const char* msg);

// Kernel-mode helper stubs (no-op in ring-0 builds)
void free_kernel_memory(void* ptr);

#endif /* KERNEL_IO_H */
