#ifndef KERNEL_IO_H
#define KERNEL_IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
// No <string.h>: kernel sources build freestanding, and this header is
// pulled by riscv-built files (frame_pool.c etc.) whose toolchain has no
// hosted libc include. kernel_io.c (x86-only) includes <string.h>
// itself for the strlen() it uses in the console line editor.

// ─── COM1 Serial Port (QEMU: -serial file:...) ────────────────────────────────
#define SERIAL_COM1_BASE  0x3F8u

void serial_init(void);

void kernel_serial_putchar(char c);
void kernel_serial_print(const char* s);
void kernel_serial_print_hex64(uint64_t v);
void kernel_serial_printf(const char* fmt, ...);

/* ─── Serial TX serialization (POSIX-Environments E1) ─────────────────────
 * Until E1 the kernel was the machine's ONLY writer to COM1 after the boot
 * hand-over, so no line could ever come out interleaved with another's. The
 * unified boot has two writers by construction: the Ring-0 foreground loop
 * on the BSP (control plane prints) and the console-service drain, which on
 * an SMP boot runs on the AP core and prints the Ring-3 sidecars' log lines.
 * Both bottom out at kernel_serial_putchar(), and two writers inside one
 * putchar loop emit byte-interleaved text (observed: `[INIT] UNIF<kernel
 * line>IED boot`). That garbles the boot log for humans and makes every
 * grep-based boot guard — which reads exactly that log — flaky.
 *
 * kernel_serial_print()/printf()/print_hex64() and the sidecar console drain
 * (kernel/console_service.c) bracket their bytes with this lock, so one line
 * — or one sidecar console message — is written whole.
 *
 * The wait costs NO atomic read-modify-write while another writer holds the
 * port: it spins on a plain load and issues a single compare-and-swap only
 * when the load reports the port free, so an uncontended acquire still costs
 * exactly one. A swap per waiting iteration — what this began as — hammers a
 * lock the other vCPU's TCG thread needs in order to make progress at all,
 * which is the leading suspect for the E1 boot wedge; kernel/kernel_io.c has
 * the measurement, including why the cheaper "give up after one attempt"
 * shape is NOT an acceptable substitute (it tears lines as badly as no lock).
 *
 * The wait is BOUNDED on purpose: a caller that cannot acquire within
 * kernel_serial_tx_lock()'s spin limit prints anyway and releases nothing.
 * That makes a parked core impossible (no cross-core lock cycle can hang the
 * kernel) and degrades a print from interrupt context, where the same core
 * may already hold it, to the pre-E1 unlocked behaviour instead of
 * self-deadlocking. Callers must use the return value: only the acquirer
 * unlocks. */
int  kernel_serial_tx_lock(void);   /* 1 = acquired (must unlock), 0 = print anyway */
void kernel_serial_tx_unlock(void);

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

/* ─── Non-blocking console, for the loop that cannot block ────────────────
 * read_line() above spins on the UART until ENTER. That is fine for
 * sls_shell_loop(), which has nothing else to do -- but kernel.c enters
 * http_server_run() and never returns when a NIC is present, so on a
 * networked boot the blocking reader is never reached and the console
 * shows output with no prompt, ever. A cluster node had no control path at
 * all: no keyboard driver, and no host port forward available because
 * net/e1000.c binds a single NIC.
 *
 * These two give the HTTP loop a console it can poll between sweeps.
 */

/* Feed ONE received byte. Returns 1 when `c` completed a line, in which
 * case the line (without its terminator) is written to `out`; 0 otherwise.
 *
 * Pure apart from echo -- no port I/O -- so the editing rules are testable
 * without a UART. Handles CR and LF, backspace and DEL, and silently
 * refuses input past the line limit rather than wrapping: the echo stops,
 * which is the operator's signal, and a truncated command that ran anyway
 * would be worse than one that visibly did not fit. */
int console_feed(char c, char* out, size_t cap);
void console_reset_line(void);

/* ─── UART loopback ownership (irqtest demo interlock) ──────────────────────
 * While a ring-3 probe drives COM1 in MCR-loopback mode (the irqtest
 * serial demo), the RX path IS the experiment: the bytes deliberately
 * left in RBR hold the IRQ4 line at a known level, and every byte the
 * kernel injects (TX) or drains (console poll) manufactures or destroys
 * line transitions the probe is trying to count (caught live: phase 2
 * 0/10, ch=edge, and the stuck-driver ch=leak FAIL — all caused by the
 * kernel's loopback-drain eating bait bytes inside QEMU's RX-CTI window
 * and by kernel banner prints looping back as foreign RX bytes).
 *
 * serial_console_poll() sets ownership when it sees the MCR loopback bit
 * and releases it when the bit reads clear; cap_io_write() sets it the
 * instant a user-space MCR write turns the bit on and clears it at the
 * MCR-off write — the probe's post-clear verdict prints are kernel TX
 * (sidecar output routes through the console service) and must reach the
 * wire from that instruction onward. While owned, the kernel does NOT
 * transmit and does NOT touch the RX FIFO. Purely x86-hardware state —
 * host builds never set the flag. */
/* Weak declarations: the state lives in kernel_io.c, but arch TUs that
 * consume the hooks (cap_io_write's MCR hand-off) are also linked by host
 * tests WITHOUT kernel_io.c — the same host-link pattern cap.c's arch
 * hooks use. Call sites outside kernel_io.c must null-check. */
void serial_loopback_ownership_set(int owned) __attribute__((weak));
int  serial_loopback_ownership(void)          __attribute__((weak));

/* Drain whatever the UART has, up to a bounded number of bytes, feeding
 * each to console_feed(). Returns the LENGTH of the completed line in
 * `out` (NUL-terminated by the editor, terminator stripped) as soon as a
 * line completes -- any remaining bytes stay in the FIFO for the next
 * call. Returns 0 when the FIFO empties without a line completing. The
 * bound matters: without it a paste of a large block would hold the HTTP
 * loop for as long as bytes kept arriving. */
int serial_console_poll(char* out, size_t cap);

/* ─── Panic-path output ────────────────────────────────────────────────────
 * Use these, NOT kernel_serial_print/printf, from any path that is reporting a
 * fault, a panic, or anything else that halts the machine.
 *
 * kernel_serial_putchar() consults capture_buf on every character (see the
 * capture note above). While a shell command is running, that pointer is
 * non-NULL, so output goes to a memory buffer instead of the UART -- and a
 * halting path never reaches the kernel_serial_capture_stop() that would flush
 * it. The message is written, and then discarded, and the log shows nothing.
 *
 * That is not hypothetical: it hid a kernel page fault for an entire debugging
 * session. gdb showed the CPU stopped two instructions past the printf that
 * was supposed to have reported it, while the serial log had no [FAULT] line
 * at all.
 *
 * These functions touch no globals -- no capture state, no VGA state, no
 * lookup tables -- so they also work when .bss is what got corrupted, which is
 * a real possibility in the situations where they get called. They wait a
 * bounded time for the transmitter and then write anyway: a dropped character
 * beats a panic handler that hangs.
 *
 * See kernel/kernel_io.c for the full rationale and
 * tests/kernel_panic_output_host_test.c for the assertions.
 */
void kernel_panic_putchar(char c);
void kernel_panic_puts(const char* s);
void kernel_panic_hex64(uint64_t v);      /* prints 0x + 16 hex digits */
void kernel_panic_dec(uint64_t v);

// Print message to serial and halt all cores
void kernel_panic(const char* msg);

// Kernel-mode helper stubs (no-op in ring-0 builds)
void free_kernel_memory(void* ptr);

#endif /* KERNEL_IO_H */
