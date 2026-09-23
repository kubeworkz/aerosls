#include <string.h>  // strlen() for the console line editor (x86-only TU)

#include "kernel_io.h"
#include "process.h"   /* E1: kernel_yield_to_ring3 (the shell's idle point) */
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

// ─── UART loopback ownership (irqtest demo interlock) ───────────────────────
// See kernel_io.h. While a ring-3 probe (the irqtest serial demo) holds
// COM1 in MCR loopback, the kernel must not transmit (every TX byte loops
// back as a foreign RX byte) and must not drain the RX FIFO (the bytes the
// probe deliberately leaves in RBR hold the IRQ4 line at a known level;
// draining them inside QEMU's RX-CTI window destroys the very edges the
// demo counts).
static volatile int g_serial_loopback_owned = 0;

// ─── Deferred output while the probe owns the port ────────────────────────
// The interlock's rule is "no bytes on the wire while the demo runs" -- not
// "no bytes ever". Dropping them instead loses evidence: the E3 multi-instance
// boot check counts one live-rootfs line per POSIX instance, and BOTH tenant
// sidecars emitted their `[POSIX] aero state=` line while the irqtest's serial
// demo held the port (measured with a kernel-side trace ring: 8 of 94
// SYS_SLS_SERIAL_WRITE calls in that boot ran inside the window, and they were
// exactly the tenants' diagnostic batches). The port was then accused of a
// failure it had never reported.
//
// Deferring keeps the demo's wire silent -- the bytes go out at release, when
// MCR loopback is already off, so they cannot re-enter the probe's own RX
// stream -- and keeps the evidence. Bounded, so a pathological writer cannot
// balloon kernel .bss; the overflow count is reported once, on the way out,
// rather than silently.
#define SERIAL_LB_DEFER_CAP 2048
static char   lb_defer_buf[SERIAL_LB_DEFER_CAP];
static size_t lb_defer_len;
static size_t lb_defer_dropped;

static void lb_defer_replay(void);
static void lb_defer_drain_if_free(void);

void serial_loopback_ownership_set(int owned) {
    g_serial_loopback_owned = owned ? 1 : 0;
    /* "Released" is not the same instant as "the port can carry bytes".
     * cap_io_write() calls this BEFORE its own outb, so at the release the
     * device is still in loopback and anything written now loops back into the
     * probe's RX stream instead of reaching the wire. Measured: flushing there
     * re-manufactured the irqtest's phantom `stuck-driver w=1 ch=leak` FAIL and
     * the replayed lines still never appeared. So the HARDWARE bit is the
     * authority here, and the console poll below is the reaper. */
    lb_defer_drain_if_free();
}
int  serial_loopback_ownership(void)          { return g_serial_loopback_owned; }

// ─── Output capture (see kernel_io.h's own header comment) ────────────────
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

// ─── Serial TX serialization (see kernel_io.h) ────────────────────────────────
// One global lock, not per-line state: the kernel has exactly one console.
// 0 = free, 1 = held. A failed acquire is NOT an error -- the caller prints
// unlocked (see the header); the spin bound is what keeps the kernel from ever
// parking on a printer.
//
// ─── Why the wait TESTS BEFORE IT SWAPS ───────────────────────────────────────
// This lock began as a compare-and-swap retry loop: every iteration of the wait
// issued `lock cmpxchg` on serial_tx_busy, up to SERIAL_TX_SPIN_LIMIT of them per
// failed acquisition. Under QEMU's multi-threaded TCG each of those is a helper
// call taking a lock shared with the other vCPU thread's own memory operations,
// so a writer waiting on a slow console line hammered the mechanism the core it
// was waiting for needed in order to make progress at all. That is the leading
// suspect for the E1 boot wedge -- a lost wake with the guest alive, the tick
// counter advancing and the BSP parked in cap_wait_chans for ever.
//
// The wait now reads serial_tx_busy with a PLAIN LOAD and issues the atomic only
// when that load reports the port free, so a held window contains no atomic
// read-modify-write at all, while an uncontended acquire still costs exactly
// one. Both halves of that sentence are asserted by
// tests/serial_tx_lock_host_test.c through the seam just below.
//
// The obvious way to remove the hammering -- give up after a SINGLE attempt (a
// "try-once" lock, so the waiter never spins) -- was built and measured, and
// rejected: with the loser printing immediately, two writers interleave BYTE by
// byte, and its torn-line count over 14 interleaved boots (57) was
// indistinguishable from an arm whose lock never acquires at all (62).
// Whole-line output is this lock's entire purpose, so the wait has to keep
// waiting; only its cost was allowed to change.
static volatile int serial_tx_busy = 0;
/* ~0.5-2 ms of pauses: far longer than a QEMU UART takes to shift out a
 * console line, and short enough that the pathological caller (interrupt
 * context on the core that already holds it) wastes a blink instead of a
 * frame. */
#define SERIAL_TX_SPIN_LIMIT 200000UL

/* The lock's one atomic, behind a seam for the same reason outb/inb are (see
 * the top of this file): the ATOMIC TRAFFIC the wait path generates is the
 * property this lock was repaired for, and it is invisible to every other kind
 * of test -- a lock that swaps per iteration and one that swaps once per
 * acquisition put identical bytes on the wire, return identical codes and
 * produce identical boot logs. tests/kernel_serial_tx_seam.h substitutes a
 * counting implementation, so a regression back to a swap-per-iteration wait
 * fails a host test instead of a QEMU boot. */
#ifndef kernel_serial_tx_try_acquire
static inline int kernel_serial_tx_try_acquire(volatile int* busy) {
    return __sync_bool_compare_and_swap(busy, 0, 1);
}
#endif

int kernel_serial_tx_lock(void) {
    for (unsigned long i = 0; i < SERIAL_TX_SPIN_LIMIT; i++) {
        /* Test: a plain load. While another writer holds the port -- the whole
         * reason to be in this loop -- this is ALL the loop does. */
        if (serial_tx_busy) {
            __asm__ volatile("pause");
            continue;
        }
        /* Then test-and-set: claim only what we just observed free. The port
         * may have been taken in between, and then this is one failed atomic
         * and the loop goes back to plain loads. */
        if (kernel_serial_tx_try_acquire(&serial_tx_busy)) return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

void kernel_serial_tx_unlock(void) {
    serial_tx_busy = 0;
}

// ─── Output primitives ────────────────────────────────────────────────────────
void kernel_serial_putchar(char c) {
    /* Loopback ownership: while the probe owns the port, kernel TX must not
     * reach the wire — every byte would loop back into the probe's own RX
     * stream (loopback internalizes TX→RX before the wire, so a wire print
     * during the demo is both lost AND an interference). Suppression ends at
     * ownership release (one release poll after the MCR bit clears, see
     * serial_console_poll below).
     *
     * Deferred rather than discarded: the byte is not the demo's to throw
     * away, and a boot guard reading this log cannot tell a swallowed line
     * from one that was never written ("an absence is not a measurement").
     * The window is short and the buffer bounded — see lb_defer_buf. */
    if (serial_loopback_ownership()) {
        if (lb_defer_len < SERIAL_LB_DEFER_CAP) lb_defer_buf[lb_defer_len++] = c;
        else lb_defer_dropped++;
        return;
    }
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
    int held = kernel_serial_tx_lock();
    while (*s) {
        if (*s == '\n') kernel_serial_putchar('\r');
        kernel_serial_putchar(*s++);
    }
    if (held) kernel_serial_tx_unlock();
}

void kernel_serial_print_hex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    kernel_serial_print(buf);
}

// ─── Deferred-output replay (see serial_loopback_ownership_set) ────────────
// Runs at ownership release, with loopback already off. The buffer is copied
// out and cleared BEFORE any byte is written, so a phase-2→3 re-entry mid-
// replay (the probe re-taking the port) defers the remainder again instead of
// having it written into the window it is meant to be silent in.
// Replay only when the port is genuinely free: ownership clear AND the device's
// own MCR loopback bit clear. Both conditions are needed -- see the setter.
static void lb_defer_drain_if_free(void) {
    if (g_serial_loopback_owned) return;
    if (inb(SERIAL_COM1_BASE + 4) & 0x10) return;
    lb_defer_replay();
}

static void lb_defer_replay(void) {
    static char scratch[SERIAL_LB_DEFER_CAP];
    size_t n = lb_defer_len;
    size_t dropped = lb_defer_dropped;
    if (!n && !dropped) return;
    for (size_t i = 0; i < n; i++) scratch[i] = lb_defer_buf[i];
    lb_defer_len = 0;
    lb_defer_dropped = 0;
    /* Held for the whole replay for the same reason print() holds it: the
     * deferred batch is several lines and a second writer (the AP's sidecar
     * drain in a unified boot) must not land inside one of them. Bounded and
     * non-reentrant by design -- called from a context that already holds it,
     * the wait expires and the bytes go out anyway, exactly as print() does. */
    int held = kernel_serial_tx_lock();
    for (size_t i = 0; i < n; i++) kernel_serial_putchar(scratch[i]);
    if (held) kernel_serial_tx_unlock();
    if (dropped) {
        /* Never silent: an overflowing window is itself a finding. Printed
         * after the unlock so the warning does not nest the lock the batch
         * just held (kernel_serial_print takes it itself). */
        kernel_serial_print("[serial] loopback window overflow: 0x");
        kernel_serial_print_hex64((uint64_t)dropped);
        kernel_serial_print(" byte(s) dropped\n");
    }
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
    /* One printf is one line of console text: hold the TX lock for all of
     * it, so a second writer (the AP's sidecar drain in a unified boot)
     * cannot land between two of its characters. */
    int held = kernel_serial_tx_lock();

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
            /* Two putchars, NOT kernel_serial_print("0x"): this printf
             * already holds the TX lock, and print() would try to take it
             * again. The lock is bounded rather than reentrant (see
             * kernel_io.h), so a nested acquisition would spin out its
             * whole budget before printing. Keep the print family flat. */
            kernel_serial_putchar('0');
            kernel_serial_putchar('x');
            emit_uint((uint64_t)(uintptr_t)va_arg(ap, void*), 16, 16, '0', 0);
            break;
        default:
            kernel_serial_putchar('%');
            kernel_serial_putchar(*fmt);
            break;
        }
    }
    if (held) kernel_serial_tx_unlock();
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
    /* UART loopback (MCR bit 4) is device-internal traffic, not console
     * input: the irqtest demo drives loopback edges whose bytes race this
     * poll and would otherwise be absorbed into console_feed's line
     * editor, then merge with the first real line typed after boot
     * (caught live: "Acaps" exec'd instead of "caps", silently dropping
     * the first typed command).
     *
     * Ownership discipline (see kernel_io.h): while the demo holds the
     * port, this poll does NOT drain the RX FIFO — the bytes the probe
     * deliberately leaves in RBR hold the IRQ4 line at a known level, and
     * draining them inside QEMU's RX-CTI window destroys the very edges
     * the demo counts (caught live: the old "discard while loopback is
     * on" rule turned phase 2 into 0/10 and manufactured the phantom
     * stuck-driver ch=leak FAIL). The MCR bit is re-checked FIRST on
     * every poll so a phase-2→3 re-entry during the release sequence is
     * caught; release takes two polls (residue flush, then ring dump +
     * ownership hand-back) so the probe's own post-clear drain gets a
     * poll window before kernel TX resumes. */
    static int prev_loopback = 0;
    /* The reaper for output a loopback window deferred. This runs every tick
     * from the console service (and each HTTP sweep), and it is the first
     * point at which the port is provably free again: the probe's own MCR-off
     * write cleared ownership one instruction before its outb, so the setter
     * could not flush there. Guarded on the hardware bit, so a window that is
     * still open (or re-opened for phase 3) keeps the bytes deferred. */
    lb_defer_drain_if_free();
    if (inb(SERIAL_COM1_BASE + 4) & 0x10) {
        serial_loopback_ownership_set(1);
        prev_loopback = 1;
        return 0;
    }
    if (prev_loopback || serial_loopback_ownership()) {
        /* Either the poll observed the loopback bit clear, or ownership
         * was left set by a path cap_io_write did not see. cap_io_write
         * normally clears ownership AT the probe's MCR-off write (the
         * probe's post-clear verdict prints are kernel TX and must reach
         * the wire immediately); this branch is the fallback. */
        while (inb(SERIAL_COM1_BASE + 5) & 0x01)
            (void)inb(SERIAL_COM1_BASE);
        console_reset_line();
        prev_loopback = 0;
        serial_loopback_ownership_set(0);
        return 0;
    }
    for (int n = 0; n < CONSOLE_DRAIN_MAX; n++) {
        if (!(inb(SERIAL_COM1_BASE + 5) & 0x01)) return 0;   /* FIFO empty */
        if (console_feed((char)inb(SERIAL_COM1_BASE), out, cap))
            return (int)strlen(out);   /* line length; the editor NUL-terminates */
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
            /* POSIX-Environments E1: the shell's idle point. On a unified
             * boot the sidecar world runs only while this loop is not
             * running (the timer's Ring-0 path never schedules), so the wait
             * for a keystroke is where the CPU is shared. No-op on every
             * other boot. */
            kernel_yield_to_ring3(PROC_CONTROL_PLANE_BUDGET_TICKS);
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
