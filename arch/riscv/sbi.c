#include "sbi.h"
#include "plic.h"
#include "trap_riscv.h"   /* rv_halt() -- the shell's `exit` command (Phase 9i) */
#include "context_riscv.h"   /* g_rv_slice_expired -- the Design B part 3 time-slice boundary */

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

/* Phase 9k: the kernel's first periodic interrupt-driven behavior — a
 * timer interrupt armed with NO firmware involvement in either mode.
 * `time` runs at QEMU virt's 10 MHz timebase, so
 * SBI_TIMER_TICKS_PER_SEC ticks = 1 second. The first call arms the
 * first tick at now+period; every later call advances the next tick
 * from the previously SCHEDULED time (not from the handler's entry
 * time), so a late handler cannot accumulate drift. The timer branch in
 * handle_riscv_supervisor_interrupt calls this to re-arm; timer
 * interrupts are level-sensitive, so the re-arm (moving the comparator
 * into the future) is what deasserts the pending bit — without it the
 * interrupt would re-fire immediately after sret/mret, exactly like the
 * timer fixture's ack-required tooth proved.
 *
 * S-mode: writes the stimecmp CSR directly (the Sstc extension this
 * platform exposes — QEMU virt + OpenSBI v1.3 advertise "time,sstc" in
 * the boot log). SBI_SET_TIMER is NOT used: on this OpenSBI both the
 * legacy EID=0 and the v0.2 TIME (EID=0x54494D45) call shapes return
 * error 0 but never actually fire a supervisor timer interrupt
 * (diagnosed empirically — see the comment on the csrw below), so the
 * direct Sstc write is the only mechanism that works there.
 *
 * M-mode (bare metal, no firmware): programs the CLINT's hart-0
 * mtimecmp MMIO register directly — physical 0x02004000 on QEMU virt
 * (CLINT base 0x02000000 + offset 0x4000). The kernel runs with paging
 * Bare, so the physical address is a direct pointer, exactly like
 * plic.c. mtimecmp and `time` share the same timebase, so the same
 * period constant and drift-free arithmetic apply unchanged. */
static uint64_t g_next_tick;

/* Design B part 3: a runtime override for the tick period, so the
 * two-task round-robin demo can run tick-cadenced slices at 100ms
 * without a rebuild. 0 (the default) = the compile-time
 * SBI_TIMER_TICK_PERIOD. The demo sets it, then restores 0. */
static uint64_t g_tick_period_override;

void sbi_set_tick_period(uint64_t period_ticks) {
    g_tick_period_override = period_ticks;
}

static uint64_t rv_rdtime(void) {
    uint64_t t;
    __asm__ volatile("csrr %0, time" : "=r"(t));
    return t;
}

#if defined(RISCV_MMODE)
#define CLINT_MTIMECMP 0x02004000UL   /* hart 0's mtimecmp: CLINT base + 0x4000 */
#endif

int sbi_arm_timer(uint64_t period_ticks) {
    uint64_t now = rv_rdtime();
    if (g_next_tick == 0 || g_next_tick <= now) {
        /* First arm, or the previous tick fired late (now already past
         * the scheduled time — a boot that took longer than the period):
         * resync to now+period rather than compounding the lateness. */
        g_next_tick = now + period_ticks;
    } else {
        g_next_tick += period_ticks;
    }
#if defined(RISCV_MMODE)
    /* Bare-metal M-mode: write the CLINT comparator directly. There is
     * no firmware and no SBI call to make — mtimecmp is plain MMIO. */
    *(volatile uint64_t*)CLINT_MTIMECMP = g_next_tick;
#else
    /* SBI_SET_TIMER on this OpenSBI (both the legacy EID=0 and the v0.2
     * TIME EID=0x54494D45 call shapes) returns error 0 but NEVER
     * produces a supervisor timer interrupt — the comparator write is
     * accepted and silently ignored, so no tick ever fires (diagnosed
     * empirically: error 0, then total STIP silence across 20s while
     * SEIP kept arriving). The direct stimecmp CSR write programs the
     * S-mode comparator that generates STIP, bypassing OpenSBI entirely.
     * (The trap-entry fixture's SBI_SET_TIMER(0) appeared to work only
     * because stimecmp is 0 at reset, so STIP was pending from the start
     * — time >= 0 — not because the SBI call programmed anything.) */
    __asm__ volatile("csrw stimecmp, %0" : : "r"(g_next_tick) : "memory");
#endif
    return 0;   /* both mechanisms are direct writes — neither can fail */
}

// Global text canvas array used to buffer incoming shell commands from the virtual UART
#define SHELL_BUF_SIZE 256
static char riscv_shell_input_buffer[SHELL_BUF_SIZE];
static uint32_t buf_cursor = 0;

/* Phase 9i interrupt-count tripwire: how many UART RX interrupts have
 * been claimed/drained/completed since boot. Only touched from interrupt
 * context (single hart, interrupts disabled inside the handler), so a
 * plain global is fine. The echo builds print it as [IRQ#N] AFTER the
 * complete() (see the handler), which is what makes per-keystroke
 * interrupt accounting a deterministic assertion. */
static uint64_t g_uart_rx_irq_count;

/* Phase 9k: how many timer interrupts (STIP in S-mode, MTIP in M-mode)
 * have been taken since boot. Same discipline as g_uart_rx_irq_count:
 * only touched from interrupt context (single hart, interrupts disabled
 * inside the handler), so a plain global is fine. The echo builds print
 * it as [TICK N] AFTER the re-arm (see the timer branch below), so a
 * client that has seen [TICK N] knows the next tick is already
 * scheduled — the deterministic proof that the timer is periodic, not
 * one-shot. Both modes count ticks now (the Phase 9k M-mode twin added
 * the CLINT/MTIP path). */
static uint64_t g_tick_count;

/* Phase 9l: preemptive time-slicing hook — the tick now doubles as a
 * round-robin scheduler. A small task table models runnable work; every
 * timer interrupt gives the ACTIVE task one bounded slice of real work
 * (the LCG in sls_task_run_slice, stepped exactly budget times — an
 * observable result that depends on the budget, so a "slice" provably
 * ran its work, not just a counter), then rotates to the next task.
 * This is the preemption mechanism a real scheduler is built on: the
 * tick preempts whatever the main loop is doing and hands the hart to
 * the next task for a bounded slice, with per-task slice accounting
 * that must stay fair (a round-robin guarantees |slices_a - slices_b|
 * <= 1 at every instant — the client asserts exactly that invariant).
 * Interrupts are disabled inside the handler (single hart), so the
 * table needs no locking. The tasks are stubs for now; the `tasks`
 * shell command exposes the table over the UART. */
#define SLS_NUM_TASKS 4

struct sls_task {
    const char* name;      /* printable task name */
    uint64_t slices;       /* how many slices this task has run */
    uint32_t budget;       /* per-slice work budget (LCG iterations) */
    uint32_t acc;          /* accumulated work result (observable) */
};

static struct sls_task g_tasks[SLS_NUM_TASKS] = {
    { "alpha", 0, 100, 0 },
    { "beta",  0, 200, 0 },
    { "gamma", 0, 300, 0 },
    { "delta", 0, 400, 0 },
};

static uint32_t g_active_task = 0;

/* One bounded slice of "work": a deterministic LCG (the Numerical
 * Recipes constants) stepped exactly budget times. The final state is
 * a function of the budget, so two tasks with different budgets cannot
 * silently "run" identical work — the acc column in the `tasks` output
 * is the observable proof each slice actually executed its iterations.
 * Bounded (max 400 iterations of 3 ops) so a slice costs microseconds,
 * never measurable drift on the 1s tick. */
static void sls_task_run_slice(struct sls_task* t) {
    uint32_t x = t->acc;
    for (uint32_t i = 0; i < t->budget; i++) {
        x = x * 1664525u + 1013904223u;
    }
    t->acc = x;
    t->slices++;
}

/* Phase 9i command loop: the real headless shell. Every line accumulated
 * by handle_riscv_supervisor_interrupt (with backspace editing and CR-only
 * line endings) is dispatched here. Commands:
 *   help               list the commands
 *   version            kernel version banner
 *   tasks              the round-robin task table (Phase 9l)
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
static void shell_print_udec(uint64_t v) {
    char buf[20];
    int i = 0;
    if (v == 0) { sbi_putchar('0'); return; }
    while (v > 0 && i < 20) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) sbi_putchar(buf[--i]);
}
void route_sls_shell_command(const char* buffer) {
    if (buffer[0] == '\0') { sbi_putchar('\r'); sbi_putchar('\n'); return; }

    if (shell_streq(buffer, "help")) {
        shell_print("[HELP] commands: help, version, tasks, echo <text>, exit\r\n");
        return;
    }
    if (shell_streq(buffer, "version")) {
        shell_print("[VER] AeroSLS RISC-V kernel (SIMI Phase 9i command loop)\r\n");
        return;
    }
    if (shell_streq(buffer, "tasks")) {
        /* Phase 9l: expose the round-robin task table. The active task
         * is the one the NEXT tick will slice; the slice counts prove
         * the rotation (a round-robin keeps them within 1 of each other
         * at every instant — the client asserts exactly that), and the
         * acc column is the observable work result (budget-dependent,
         * so a slice provably ran its LCG iterations). */
        shell_print("[TASKS] active=");
        shell_print(g_tasks[g_active_task].name);
        shell_print("\r\n");
        for (uint32_t i = 0; i < SLS_NUM_TASKS; i++) {
            shell_print("  ");
            shell_print(g_tasks[i].name);
            shell_print(" slices=");
            shell_print_udec(g_tasks[i].slices);
            shell_print(" acc=");
            shell_print_udec(g_tasks[i].acc);
            shell_print("\r\n");
        }
        /* The client waits for this deterministic end-of-table marker
         * (not a row prefix — the kernel prints char-by-char, so a
         * row's digits can straddle a socket segment and truncate the
         * last count). By the time [TASKS-END] appears, all four rows
         * are fully in the stream. */
        shell_print("[TASKS-END]\r\n");
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

    // Phase 9k: timer interrupt — STIP (cause 5) in the S-mode build,
    // MTIP (cause 7) in the bare-metal M-mode twin — the kernel's first
    // periodic interrupt-driven behavior. The build arms the timer at
    // boot (kernel_riscv.c's sbi_arm_timer call, mode-aware: stimecmp
    // CSR vs CLINT mtimecmp); each tick re-arms the next one and (echo
    // builds only) prints the running count. The re-arm is what
    // deasserts the level-pending timer bit, so it must happen on EVERY
    // tick — a one-shot arm would deliver [TICK 1] and then never fire
    // again, which the client's [TICK 2] assertion would catch.
    {
        uint64_t timer_cause;
#if defined(RISCV_MMODE)
        timer_cause = 7;   /* MTIP — machine timer */
#else
        timer_cause = 5;   /* STIP — supervisor timer */
#endif
        if ((scause & (1ULL << 63)) && (scause & 0xFF) == timer_cause) {
            g_tick_count++;
            /* Phase 9n: re-arm to SBI_TIMER_TICK_PERIOD — 1s on
             * production builds, 100ms on the echo builds (the
             * tick-vs-UART contention probe). The drift-free
             * arithmetic keeps the 100ms cadence exact even under
             * contention, and the level-pending bit guarantees no tick
             * is lost to an interrupt that arrives while disabled.
             * Design B part 3: the demo's sbi_set_tick_period override
             * (100ms) wins while it is set, so the two-task round-robin
             * gets fast, deterministic slice cadence. */
            sbi_arm_timer(g_tick_period_override ? g_tick_period_override
                                                  : SBI_TIMER_TICK_PERIOD);
            /* Design B part 3: mark the time-slice boundary. The
             * running task polls this flag and yields at its next
             * boundary — this is what turns the tick into a round-robin
             * scheduler handoff for the REAL register-context tasks
             * (vs. the modeled Phase 9l slice table below). Set
             * unconditionally: the echo build never polls it, so the
             * [TICK]/[SLICE] protocol is unaffected. */
            g_rv_slice_expired = 1;

            /* Phase 9l: preemptive time-slice — hand the hart to the
             * active task for one bounded slice, then rotate. The slice
             * runs here, inside the tick handler, which is exactly what
             * "preemptive" means: the tick preempts the main loop and
             * schedules the next task regardless of what the loop was
             * doing. Echo builds print [SLICE <name>] so the rotation is
             * observable (the client's [TICK 2] wait is unaffected — the
             * markers interleave in the stream). */
            uint32_t ran = g_active_task;
            sls_task_run_slice(&g_tasks[ran]);
            g_active_task = (g_active_task + 1) % SLS_NUM_TASKS;
#if defined(KERNEL_UART_ECHO)
            shell_print("[SLICE ");
            shell_print(g_tasks[ran].name);
            shell_print("]\r\n");
            /* Phase 9m: the tick line now carries wall-clock uptime in
             * seconds, derived from the same timebase that arms the
             * timer (`time` CSR in S-mode, `time`/mtime in M-mode —
             * rv_rdtime reads the shared 10 MHz clock, so seconds =
             * ticks / SBI_TIMER_TICKS_PER_SEC). The client asserts the
             * wall clock stays consistent with the tick count: at tick
             * N the machine has run at least N seconds, and the
             * reported uptime never goes backwards. */
            shell_print("[TICK ");
            shell_print_udec(g_tick_count);
            shell_print(" ");
            shell_print_udec(rv_rdtime() / SBI_TIMER_TICKS_PER_SEC);
            shell_print("s]\r\n");
#endif
            return;
        }
    }

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

        // Tripwire accounting. This prints AFTER the complete(), so a
        // client that has seen [IRQ#N] knows this interrupt's whole
        // claim/drain/complete round trip finished and the line is
        // re-armed -- which is exactly what makes "one isolated
        // keystroke -> exactly one interrupt" (and "a rapid batch drains
        // with no lost bytes") a deterministic assertion rather than a
        // timing guess. Echo builds only (KERNEL_UART_ECHO): the
        // shipping kernels stay quiet.
        g_uart_rx_irq_count++;
#if defined(KERNEL_UART_ECHO)
        shell_print("[IRQ#");
        shell_print_udec(g_uart_rx_irq_count);
        shell_print("]\r\n");
#endif
    }
}