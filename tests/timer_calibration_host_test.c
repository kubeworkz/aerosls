/*
 * timer_calibration_host_test.c — kernel/timer.c's calibration, pinned
 * against a modelled PIT, LAPIC and TSC.
 *
 * ─── What this exists to catch ──────────────────────────────────────────────
 * The incident this whole subsystem was written for: a guest that "stops
 * beating for 1.4-3 seconds while the host sits idle". Two independent
 * defects produced it, and this test refuses both:
 *
 *   1. PERIOD WAS A GUESS. init_timer programmed TICR=1,000,000 / divide-16
 *      and never measured, which on QEMU's APIC bus is 62.5 Hz, not the
 *      100 Hz every timeout constant in the tree assumes. The calibration
 *      now measures the LAPIC against the PIT's fixed 1193182 Hz and
 *      programs TICR from the measurement.
 *   2. THE COUNTER COUNTED INTERRUPTS SERVICED, not elapsed time. A periodic
 *      LAPIC coalesces interrupts it cannot deliver, so a busy guest's tick
 *      counter fell behind wall time by the stall itself (measured: 0.7
 *      ticks/s during a stall while wall time ran at 62.5). timer_advance()
 *      now derives the counter from elapsed TSC, so one interrupt arriving
 *      250 ms late credits 25 ticks, not 1.
 *
 * The numbers this test computes are exact, not approximate: the modelled
 * devices are driven by one fake TSC with integer counts-per-cycle ratios
 * (100 TSC cycles per PIT count, 10 per LAPIC count), and both calibration
 * windows telescope exactly (a read window's first/last latch bounds both
 * numerator and denominator). So the assertions say ==, not "~", and a
 * future edit that is off by even one count fails here.
 *
 * Also pinned: the PIT input frequency constant. A previous literal
 * (11931800) was ~10x the real 1193182, and because BOTH halves derive from
 * it, the errors would have agreed with each other at 10 Hz -- boot would
 * look healthy while every timeout silently took 10x time.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE -I . -I kernel -I net -I arch/x86 \
 *       -o /tmp/timer_calibration_host_test \
 *       tests/timer_calibration_host_test.c
 *   /tmp/timer_calibration_host_test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

/* ─── The modelled machine ──────────────────────────────────────────────────
 * One fake TSC is the only thing that moves; PIT and LAPIC counts are exact
 * integer ratios of it, so any measurement taken against them can be checked
 * with == rather than a tolerance. */
#define FAKE_TSC_HZ     119318200ULL   /* 100 TSC cycles per PIT count       */
#define FAKE_PIT_HZ     1193182ULL     /* == PIT_INPUT_HZ: the real i8253 Hz  */
#define FAKE_LAPIC_HZ   11931820ULL    /* 10 TSC cycles per LAPIC count       */
#define TSC_PER_PIT     (FAKE_TSC_HZ / FAKE_PIT_HZ)      /* 100               */
#define TSC_PER_LAPIC   (FAKE_TSC_HZ / FAKE_LAPIC_HZ)    /* 10               */

static uint64_t fake_tsc = 0;

/* Seams declared before kernel/timer.c sees them: timer.c calls through these
 * macros (its own #ifndef guards install real port I/O only when the macros
 * are absent). */
static uint64_t fake_tsc_read(void);
static uint8_t  fake_inb(uint16_t port);
static void     fake_outb(uint16_t port, uint8_t val);
static uint32_t fake_lapic_read(uint32_t reg);
static void     fake_lapic_write(uint32_t reg, uint32_t val);

#define outb(port, val)                fake_outb((port), (val))
#define inb(port)                      fake_inb((port))
#define TIMER_LAPIC_READ(reg)          fake_lapic_read(reg)
#define TIMER_LAPIC_WRITE(reg, val)    fake_lapic_write((reg), (val))
#define TIMER_READ_TSC()               fake_tsc_read()

#include "kernel/timer.c"

/* ─── Call recorders for the ISR's other halves ───────────────────────────── */
static int g_gates = 0;
static uint8_t g_gate_vec = 0;
static uint64_t g_gate_isr = 0;
static uint8_t g_gate_attr = 0;
void set_idt_gate(uint8_t vector, uint64_t isr_address, uint8_t attributes) {
    g_gates++; g_gate_vec = vector; g_gate_isr = isr_address; g_gate_attr = attributes;
}
void isr32_stub(void) { }

static int g_polls = 0;
void net_poll_tick(void) { g_polls++; }
static int g_parks = 0;
void cap_park_deadline_tick(void) { g_parks++; }
static int g_ctl = 0;
void proc_control_plane_tick(void) { g_ctl++; }
static int g_defers = 0;
void console_service_irq_defer(void) { g_defers++; }
static int g_notifies = 0;
static uint32_t g_last_notify_vec = 0;
void cap_irq_notify(uint32_t vector) { g_notifies++; g_last_notify_vec = vector; }

/* Captures the calibration's own report so claims like "calibrated" and
 * "CALIBRATION FAILED" can be checked rather than assumed. */
static char g_log[4096];
static int  g_log_len = 0;
void kernel_serial_print(const char* s) {
    while (*s && g_log_len < (int)sizeof(g_log) - 1) g_log[g_log_len++] = *s++;
    g_log[g_log_len] = '\0';
}
void kernel_serial_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    g_log_len += vsnprintf(g_log + g_log_len, sizeof(g_log) - (size_t)g_log_len, fmt, ap);
    va_end(ap);
    if (g_log_len > (int)sizeof(g_log) - 1) g_log_len = (int)sizeof(g_log) - 1;
}
static void log_reset(void) { g_log_len = 0; g_log[0] = '\0'; }
static int logged(const char* needle) { return strstr(g_log, needle) != NULL; }

/* ─── The PIT model (channel 2: latch + rate-generator programming) ───────── */
#define PORT_PIT_CMD   0x43
#define PORT_PIT_CH2   0x42
static uint64_t pit_epoch_tsc = 0;     /* when the count was (re)programmed  */
static uint64_t pit_reload = 65536;    /* uint64: 65536 does not fit uint16  */
static int      pit_prog_stage = 0;    /* 0 idle, 1 = low byte, 2 = high      */
static uint16_t pit_latched = 0;
static int      pit_byte_stage = 0;    /* 0 = next read is the low byte      */
static int      pit_dead = 0;          /* frozen counter: calibration must fail */
static int      g_pit_rate_gen = 0;    /* did the code PROGRAM channel 2?     */

static uint16_t pit_value_now(void) {
    if (pit_dead) return 0x1234;                   /* frozen: never advances  */
    uint64_t c = ((fake_tsc - pit_epoch_tsc) / TSC_PER_PIT) % pit_reload;
    return (uint16_t)(pit_reload - c);             /* counts down, then reloads */
}

static void fake_outb(uint16_t port, uint8_t val) {
    fake_tsc += 100;                                /* port-write latency       */
    if (port != PORT_PIT_CMD && port != PORT_PIT_CH2) return;
    if (port == PORT_PIT_CMD) {
        if (val == 0x80) {                          /* latch command            */
            pit_latched = pit_value_now();
            pit_byte_stage = 0;
        } else if (val == 0xB4) {                   /* ch2, lo/hi, mode 2, bin  */
            g_pit_rate_gen = 1;
            pit_prog_stage = 1;
        }
        return;
    }
    /* Data port: collect the two reload bytes, then restart the count. */
    static uint16_t prog_val = 0;
    if (pit_prog_stage == 1) { prog_val = val; pit_prog_stage = 2; return; }
    if (pit_prog_stage == 2) {
        prog_val |= (uint16_t)((uint16_t)val << 8);
        pit_reload = prog_val ? prog_val : 65536;   /* 0 means 65536          */
        pit_epoch_tsc = fake_tsc;
        pit_prog_stage = 0;
    }
}
static uint8_t fake_inb(uint16_t port) {
    fake_tsc += 100;
    if (port != PORT_PIT_CH2) return 0;
    if (pit_byte_stage == 0) { pit_byte_stage = 1; return (uint8_t)(pit_latched & 0xFF); }
    pit_byte_stage = 0; return (uint8_t)(pit_latched >> 8);
}

/* ─── The LAPIC model (TCCR downcounts from TICR, one-shot stops at 0) ────── */
static uint64_t lapic_epoch_tsc = 0;
static uint32_t lapic_ticr = 0;
static int      lapic_dead = 0;        /* TCCR frozen: calibration must fail */
static int      g_eois = 0;
static uint32_t g_last_ticr_write = 0;

static uint32_t fake_lapic_read(uint32_t reg) {
    fake_tsc += 100;
    if (reg == LAPIC_REG_SPURIOUS) return 0x1FF;    /* software-enabled        */
    if (reg == LAPIC_REG_TCCR) {
        if (lapic_dead) return 0x77777777u;         /* frozen                  */
        uint64_t elapsed = (fake_tsc - lapic_epoch_tsc) / TSC_PER_LAPIC;
        if (elapsed >= lapic_ticr) return 0;        /* one-shot expired         */
        return (uint32_t)(lapic_ticr - elapsed);
    }
    return 0;
}
static void fake_lapic_write(uint32_t reg, uint32_t val) {
    fake_tsc += 100;
    if (reg == LAPIC_REG_TICR) { lapic_ticr = val; lapic_epoch_tsc = fake_tsc; g_last_ticr_write = val; }
    if (reg == LAPIC_REG_EOI)  { g_eois++; }
}
static uint64_t fake_tsc_read(void) { return fake_tsc; }

/* ─── Harness ─────────────────────────────────────────────────────────────── */
static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

static void reset_scenario(void) {
    s_tsc_hz = 0; s_tsc_per_tick = 0; s_last_tsc = 0; s_ticr = 0;
    kernel_tick_counter = 0;
    g_gates = 0; g_polls = 0; g_parks = 0; g_ctl = 0; g_defers = 0;
    g_notifies = 0; g_eois = 0; g_pit_rate_gen = 0; g_last_ticr_write = 0;
    pit_dead = 0; lapic_dead = 0;
    pit_prog_stage = 0; pit_byte_stage = 0;
    pit_epoch_tsc = fake_tsc; pit_reload = 65536;
    lapic_epoch_tsc = fake_tsc; lapic_ticr = 0;
    log_reset();
}

int main(void) {
    /* ── 1. a healthy machine calibrates to the modelled rates, exactly ──── */
    reset_scenario();
    init_timer();

    CHECK(g_gates == 1 && g_gate_vec == 32 &&
          g_gate_isr == (uint64_t)(uintptr_t)isr32_stub && g_gate_attr == 0x8E,
          "init_timer registers isr32_stub on vector 32 before measuring");
    CHECK(g_pit_rate_gen == 1,
          "the TSC window programs channel 2 (a known rate, not a read-back guess)");
    CHECK(g_last_ticr_write == (uint32_t)(FAKE_LAPIC_HZ / KERNEL_TICK_HZ),
          "TICR = measured LAPIC rate / KERNEL_TICK_HZ, exactly");
    CHECK(s_tsc_per_tick == FAKE_TSC_HZ / KERNEL_TICK_HZ,
          "cycles-per-tick = measured TSC rate / KERNEL_TICK_HZ, exactly");
    CHECK(kernel_timer_tsc_hz() == FAKE_TSC_HZ,
          "measured TSC rate equals the modelled one exactly (1193182 Hz PIT input)");
    CHECK(kernel_timer_ticr() == (uint32_t)(FAKE_LAPIC_HZ / KERNEL_TICK_HZ),
          "kernel_timer_ticr() reports the programmed reload");
    CHECK(kernel_timer_calibrated() != 0, "kernel_timer_calibrated() reports armed");
    CHECK(logged("[TIMER] tick calibrated") && !logged("CALIBRATION FAILED"),
          "the boot log says calibrated and does not claim failure");

    /* ── 2. THE INCIDENT: one interrupt 250 ms late credits 25 ticks ────────
     * (250 ms is 25 ticks of KERNEL_TICK_NS, not 250 -- the number that used
     * to arrive as ONE.) */
    uint64_t per = (uint64_t)s_tsc_per_tick;
    fake_tsc += 25 * per;
    int calls = g_notifies;
    timer_irq_handler();
    CHECK(kernel_tick_counter == 25,
          "one interrupt arriving 250 ms after the last advances the counter by 25, not 1");
    CHECK(g_notifies == calls + 1 && g_last_notify_vec == 32,
          "cap_irq_notify(32) still runs on that interrupt");
    CHECK(g_eois > 0 && g_polls == 1 && g_parks == 1 && g_ctl == 1 && g_defers == 1,
          "every other ISR half runs even when the tick credit is unusual");

    /* ── 3. five coalesced interrupts account for the whole gap ───────────── */
    uint64_t before = kernel_tick_counter;
    for (int i = 0; i < 5; i++) { fake_tsc += 10 * per; timer_irq_handler(); }
    CHECK(kernel_tick_counter == before + 50,
          "five interrupts spaced 10 ticks apart credit 50 ticks total");

    /* ── 4. no phantom ticks, and truncation carry never drifts ─────────────
     * The exact landing point matters: timer_advance() must see a delta of
     * precisely per-1 cycles, so the partial period ALREADY carried in
     * s_last_tsc (a few hundred cycles of calibration/handler latency) is
     * subtracted out rather than assumed to be zero. */
    before = kernel_tick_counter;
    uint64_t carried = fake_tsc - s_last_tsc;   /* partial period in the carry */
    fake_tsc += (per - 1) - carried;            /* now exactly 1 cycle short   */
    timer_irq_handler();
    CHECK(kernel_tick_counter == before,
          "an interrupt one cycle short of a full period credits nothing");
    timer_irq_handler();
    CHECK(kernel_tick_counter == before + 1,
          "the carried partial period is credited by the next interrupt, not lost");

    /* ── 5. the death-watch premise: a long stall is reported as wall time ── */
    before = kernel_tick_counter;
    fake_tsc += 3ULL * 100ULL * per;                 /* 3 seconds of silence   */
    timer_irq_handler();
    CHECK(kernel_tick_counter == before + 300,
          "3 s of coalesced silence is credited as 300 ticks in one interrupt");

    /* ── 6. a TSC that jumps backwards rebases instead of wrapping ─────────
     * The jump has to stay small enough that the uint64 clock does not itself
     * underflow (that would fabricate a huge FORWARD delta, a different bug
     * from the one being pinned here): 5 ticks back, which is far more than
     * the few hundred cycles of residue the carry can hold. */
    before = kernel_tick_counter;
    fake_tsc -= 5ULL * per;
    timer_irq_handler();
    CHECK(kernel_tick_counter == before,
          "a backwards TSC read credits nothing and does not wrap the counter");

    /* ── 7. a frozen PIT degrades to the documented legacy behaviour ─────── */
    reset_scenario();
    pit_dead = 1;
    init_timer();                                   /* must RETURN, not hang   */
    CHECK(kernel_timer_calibrated() == 0,
          "a frozen PIT: kernel_timer_calibrated() reports 0");
    CHECK(kernel_timer_ticr() == TIMER_LEGACY_TICR,
          "a frozen PIT: the reload falls back to TIMER_LEGACY_TICR");
    CHECK(logged("CALIBRATION FAILED"),
          "a frozen PIT: the boot log says the measurement could not be taken");
    fake_tsc += 1000ULL * per;
    timer_irq_handler();
    CHECK(kernel_tick_counter == 1,
          "legacy fallback: one tick per interrupt, exactly as before the fix");

    /* ── 8. a frozen LAPIC likewise refuses to invent a rate ──────────────── */
    reset_scenario();
    lapic_dead = 1;
    init_timer();
    CHECK(kernel_timer_calibrated() == 0 && kernel_timer_ticr() == TIMER_LEGACY_TICR,
          "a frozen LAPIC: no fabricated rate, TIMER_LEGACY_TICR kept");
    CHECK(logged("CALIBRATION FAILED"),
          "a frozen LAPIC: the boot log says the measurement could not be taken");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
