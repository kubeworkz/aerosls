#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

// ─── COM1 Serial Port (QEMU: -serial file:...) ────────────────────────────────
#define SERIAL_COM1_BASE  0x3F8u

void serial_init(void);

void kernel_serial_putchar(char c);
void kernel_serial_print(const char* s);
void kernel_serial_print_hex64(uint64_t v);
void kernel_serial_printf(const char* fmt, ...);

// ─── Output capture (Kernel-Side Shell Refactor, docs/AeroSLS-Web-Terminal-
// Plan-v0.1.md §10.2) ───────────────────────────────────────────────────────
// kernel_serial_putchar() is the single choke point every kernel_serial_
// print()/printf() call in the whole kernel bottoms out at (602 call sites
// across 38 files, confirmed by grep before this was added -- redirecting
// those call sites individually was the naive approach this deliberately
// avoids). When a capture buffer is active, kernel_serial_putchar() appends
// to it instead of touching the UART/VGA at all (bounds-checked, silently
// truncating past cap -- matches the existing hardware path's own lack of
// an overflow signal). Not reentrant -- this kernel has no concurrent
// shell execution today (same single-active-session assumption
// current_tx_id/current_session_uid in user/shell.c already make), so a
// single global capture slot is sufficient; do not call
// kernel_serial_capture_start() again before a matching _stop().
void   kernel_serial_capture_start(char* buf, size_t cap);
size_t kernel_serial_capture_stop(void);   // NUL-terminates buf, returns length written (excl. NUL)

// Blocking read of one line from COM1 into buf (max 255 chars + NUL)
void read_line(char* buf);

// Print message to serial and halt all cores
void kernel_panic(const char* msg);

// Kernel-mode helper stubs (no-op in ring-0 builds)
void free_kernel_memory(void* ptr);

#endif /* KERNEL_IO_H */
