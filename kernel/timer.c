#include "timer.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/lapic.h"
#include "net_event.h"
#include "cap.h"   /* Phase 5: cap_park_deadline_tick (weak default / process.c override) */
#include "process.h"  /* E1: proc_control_plane_tick (unified-boot yield budget) */
#include "console_service.h"  /* BSP console drain when AP is offline */
#include "dashboard.h"  /* read_tsc() -- the wall-clock source this file calibrates against */
#include "kernel_io.h"  /* kernel_serial_printf, for the calibration's own report */

/* ─── Port I/O seams ───────────────────────────────────────────────────────
 * Same shape as kernel_io.c's, for the same reason: port I/O is privileged
 * and untestable on the host, and an untestable timing path is how this
 * kernel came to have a tick rate nobody had ever measured. A host test
 * defines outb/inb before including this file and drives the real
 * calibration arithmetic against a modelled PIT. */
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

/* LAPIC and TSC seams, substituted by the same kind of host test. */
#ifndef TIMER_LAPIC_READ
#define TIMER_LAPIC_READ(reg)        lapic_read(reg)
#define TIMER_LAPIC_WRITE(reg, val)  lapic_write(reg, val)
#endif
#ifndef TIMER_READ_TSC
#define TIMER_READ_TSC()             read_tsc()
#endif

volatile uint64_t kernel_tick_counter = 0;

/* ─── The two numbers this file exists to get right ────────────────────────
 *
 * kernel_tick_counter is treated as a WALL CLOCK by the rest of the kernel:
 * net/consensus.h says its own election arithmetic "depend[s] on it being
 * wall-clock rather than a call count", kernel/chan.c converts nanosecond
 * deadlines with KERNEL_TICK_NS, kernel/auth.h measures token TTL in it, and
 * kernel/failover.h sizes a peer's death in it. Every one of those constants
 * is written against ~100 Hz.
 *
 * Neither half of that contract was ever true, and the difference showed up
 * live as a node that "stops beating for 1.4-3 seconds while the host sits
 * idle" (measured 2026-10-25, 3-node TCG cluster, /api/health's uptime_ticks
 * sampled at 20 Hz):
 *
 *   1. THE PERIOD WAS A GUESS. init_timer programmed TICR=1,000,000 with a
 *      divide-by-16 and a comment reading "fires roughly every 10 ms at a
 *      ~1.6 GHz effective LAPIC clock; exact rate is calibration-dependent".
 *      Nothing ever calibrated it. On QEMU's 1 GHz APIC bus that config is
 *      16 ms, i.e. 62.5 Hz -- and the observed ceiling on an idle node was
 *      63.5 ticks/s, matching it. So LIVENESS_BEAT_TICKS (60, documented
 *      "600 ms") really fired every 960 ms before a single tick was lost.
 *
 *   2. THE COUNTER COUNTED SERVICED INTERRUPTS, not elapsed time. QEMU's
 *      LAPIC is periodic: an interrupt that cannot be delivered promptly is
 *      coalesced into the next one, so kernel_tick_counter++ records "one
 *      interrupt I got around to" rather than "ten milliseconds that passed".
 *      Measured over 120 s of an idle cluster: node 1 averaged 53 ticks/s,
 *      node 3 averaged 31, and node 3 bottomed at 0.7 ticks/s inside a
 *      stall -- while wall time ran at 62.5. Every timeout in the tree
 *      therefore meant a different duration on every node, and stretched by
 *      whatever the guest happened to be doing.
 *
 * Together those are the reported symptom: a follower's 60-tick liveness
 * beat, intended at 600 ms, reached its peers 1.1 s apart on node 1 and
 * 1.9-3 s apart on node 3, with no traffic and an idle host. The peers are
 * not watching a slow network; they are watching a clock that runs slow.
 *
 * The fix has two halves and they are load-bearing on each other:
 *
 *   - timer_calibrate() MEASURES the LAPIC's real decrement rate against the
 *     PIT's own fixed 1.193182 MHz clock, then programs TICR so one period IS
 *     KERNEL_TICK_NS. No frequency is assumed anywhere; if the measurement
 *     cannot be taken, this file says so on the console and keeps the
 *     behaviour it had before.
 *   - timer_advance() derives the counter from elapsed TSC instead of counting
 *     interrupts, so an interrupt that is late brings the counter UP TO DATE
 *     rather than leaving it behind. A busy guest still tells the truth about
 *     how much time passed; a stalled sweep still declares the silence it
 *     really had.
 *
 * tests/timer_calibration_host_test.c pins both halves, including the exact
 * live shape: one interrupt arriving 250 ms after the last one advances the
 * counter by 25, not by 1. */
static uint64_t s_tsc_hz;          /* measured TSC rate, 0 = calibration failed */
static uint64_t s_tsc_per_tick;     /* TSC ticks in one KERNEL_TICK_NS period, 0 = uncalibrated */
static uint64_t s_last_tsc;         /* TSC reading the counter is already up to date with */
static uint32_t s_ticr;             /* the reload value that was actually programmed */

/* ─── The PIT: this machine's only fixed-frequency timebase ────────────────
 * The i8253/8254 input is 1193182 Hz by construction (QEMU implements that
 * exact value in hw/timer/i8254.c), so it is the one clock here that cannot
 * be a guess. Channel 2 is used because channel 0 feeds IRQ0 and
 * reprogramming it would move an interrupt source this kernel does not own;
 * channel 2's only product is the PC speaker.
 *
 * THE VALUE IS LOAD-BEARING FOR EVERYTHING BELOW: s_tsc_hz and the measured
 * lapic_hz are both computed as `delta * PIT_INPUT_HZ / counts`, so an
 * input frequency that is 10x off makes the TSC rate 10x off, which makes
 * s_tsc_per_tick 10x too long and, independently, makes the programmed TICR
 * 10x too large -- and the two errors then AGREE with each other at 10 Hz.
 * Nothing about the boot would look wrong; every timeout would silently take
 * 10x its documented time. (The previous literal here was 11931800ULL, a
 * 10x-inflated reading of "1.193182 MHz".) */
#define PIT_COMMAND          0x43
#define PIT_CH2_DATA         0x42
#define PIT_INPUT_HZ         1193182ULL
#define PIT_CMD_CH2_RATE_GEN 0xB4    /* channel 2, lo/hi access, mode 2, binary */
#define PIT_CMD_LATCH_CH2    0x80
/* ~100 ms of PIT time per calibration spin (120000 / 1193182 s): long enough
 * that a one-count read error is ~8 ppm, short enough not to be felt in a
 * boot that already takes minutes under TCG. The LAPIC half spins over half
 * of this, ~50 ms, matching its own comment below. */
#define PIT_CAL_COUNTS       120000ULL

static void timer_pit_start(void) {
    outb(PIT_COMMAND, PIT_CMD_CH2_RATE_GEN);
    outb(PIT_CH2_DATA, 0x00);
    outb(PIT_CH2_DATA, 0x00);       /* reload 0 means 65536 */
}

/* Latched read of the down-counting channel 2 value. */
static uint16_t timer_pit_read(void) {
    outb(PIT_COMMAND, PIT_CMD_LATCH_CH2);
    uint8_t lo = inb(PIT_CH2_DATA);
    uint8_t hi = inb(PIT_CH2_DATA);
    return (uint16_t)((uint16_t)lo | (uint16_t)((uint16_t)hi << 8));
}

/* Spin until the PIT has counted `counts_wanted` of its own ticks; return how
 * many it counted, and hand back `sample()` taken at the FIRST and LAST latch
 * of that same window.
 *
 * The two sample points are the whole point of the signature. If the caller
 * sampled around the loop instead, its window would also contain the prologue
 * (the first latch and everything before it) while the PIT count would not,
 * and the two would disagree by one read interval -- a several-percent error
 * measured as a calibration. Sampling inside, at the two latches that
 * actually bound the counted interval, makes the numerator and the
 * denominator cover identical time.
 *
 * The counter counts DOWN and wraps at 65536, so the delta is taken in uint16
 * arithmetic -- correct across any number of wraps as long as consecutive
 * reads are less than a full wrap apart, which the callers keep true by
 * holding the window's read interval well under 5.5 ms of PIT time.
 *
 * A PIT that never moves must return 0 rather than spin forever. Consecutive
 * reads are ~1 us apart and the counter advances every 84 ns, so a zero delta
 * is not noise -- 1000 of them in a row means the counter is frozen, and a
 * calibration cannot be taken from a frozen clock (the callers treat 0 as
 * "measurement failed" and keep the documented legacy behaviour). Without
 * this bound, a machine whose channel 2 is dead hangs in boot instead of
 * reporting it. */
static uint64_t timer_pit_spin(uint64_t counts_wanted,
                               uint64_t (*sample)(void),
                               uint64_t* s0, uint64_t* s1) {
    uint16_t prev = timer_pit_read();      /* window opens at this latch */
    *s0 = sample();
    uint64_t got = 0;
    uint32_t frozen = 0;
    while (got < counts_wanted) {
        uint16_t now = timer_pit_read();
        uint64_t d = (uint64_t)(uint16_t)(prev - now);
        if (d == 0) {
            if (++frozen > 1000) return 0;   /* channel 2 is not counting */
        } else {
            frozen = 0;
            got += d;
        }
        prev = now;
    }
    *s1 = sample();                        /* window closes at the last latch */
    return got;
}

static uint64_t timer_sample_tsc(void) {
    return TIMER_READ_TSC();
}
static uint64_t timer_sample_lapic(void) {
    return (uint64_t)TIMER_LAPIC_READ(LAPIC_REG_TCCR);
}

/* ─── Calibration ──────────────────────────────────────────────────────────
 * Returns the TSC rate in Hz, or 0 if it could not be measured. The caller
 * treats 0 as "keep the documented behaviour rather than invent a number". */
static uint64_t timer_calibrate_tsc(void) {
    timer_pit_start();
    uint64_t t0, t1;
    uint64_t counts = timer_pit_spin(PIT_CAL_COUNTS, timer_sample_tsc, &t0, &t1);
    if (counts == 0 || t1 <= t0) return 0;
    return ((t1 - t0) * PIT_INPUT_HZ) / counts;
}

/* Returns the LAPIC timer's post-divide decrement rate in Hz, or 0.
 *
 * Read from the Current Count Register across a PIT-measured window, which
 * measures whatever this machine's APIC bus actually does instead of the 1 GHz
 * QEMU happens to implement. Three details are load-bearing:
 *
 *   - ONE-SHOT with a 32-bit reload, so no interrupt can land while we are
 *     measuring. At tens of millions of ticks/s a 0xFFFFFFFF reload takes
 *     tens of seconds to expire; the window is ~50 ms.
 *   - Software-enable the APIC first if it is not already. The timer does not
 *     run otherwise, and a Current Count Register that never moves is
 *     indistinguishable from a broken one unless we know we enabled it.
 *   - Sample TCCR at the two latches that bound the counted interval (the
 *     same contract as the TSC half above), so the count and the elapsed time
 *     describe the same window. */
static uint64_t timer_calibrate_lapic(void) {
    uint32_t spurious = TIMER_LAPIC_READ(LAPIC_REG_SPURIOUS);
    if ((spurious & (1u << 8)) == 0)
        TIMER_LAPIC_WRITE(LAPIC_REG_SPURIOUS, spurious | (1u << 8));

    TIMER_LAPIC_WRITE(LAPIC_REG_TDCR, 0x03);            /* divide by 16 */
    TIMER_LAPIC_WRITE(LAPIC_REG_LVT_TMR, 0x20);         /* one-shot, vector 32, unmasked */
    TIMER_LAPIC_WRITE(LAPIC_REG_TICR, 0xFFFFFFFFu);

    uint64_t c0, c1;
    uint64_t counts = timer_pit_spin(PIT_CAL_COUNTS / 2, timer_sample_lapic, &c0, &c1);
    TIMER_LAPIC_WRITE(LAPIC_REG_LVT_TMR, 0x10000);      /* mask until init_timer is done */

    uint32_t delta = (uint32_t)(c0 - c1);   /* it counts DOWN; no wrap in this window */
    if (delta == 0 || counts == 0) return 0;
    return ((uint64_t)delta * PIT_INPUT_HZ) / counts;
}

/* Measure everything, then program the timer. Leaves s_tsc_per_tick at 0 when
 * either half failed -- that is the switch the ISR reads to choose between
 * calibrated and legacy behaviour. */
static void timer_calibrate(void) {
    s_tsc_hz = timer_calibrate_tsc();
    uint64_t lapic_hz = timer_calibrate_lapic();

    if (lapic_hz >= KERNEL_TICK_HZ && s_tsc_hz >= KERNEL_TICK_HZ) {
        s_ticr = (uint32_t)(lapic_hz / KERNEL_TICK_HZ);
        if (s_ticr == 0) s_ticr = 1;
        s_tsc_per_tick = s_tsc_hz / KERNEL_TICK_HZ;
        if (s_tsc_per_tick == 0) s_tsc_per_tick = 1;
    } else {
        /* Honest degradation: the pre-calibration reload, one tick per
         * interrupt, exactly as this file behaved before. A labelled fallback
         * that reproduces known behaviour beats a fabricated rate that
         * nobody can tell from a real one -- see kernel/timer.h. */
        s_ticr = TIMER_LEGACY_TICR;
        s_tsc_per_tick = 0;
    }
    /* s_last_tsc is stamped here and the counter is deliberately NOT reset:
     * kernel_tick_counter only ever moves forward, and a calibration that
     * zeroed it would move it backwards for anything that had already read
     * it. At this point in boot nothing has (the ISR is not installed yet),
     * but "never move the clock backwards" is cheaper to keep than to reason
     * about per call site. */
    s_last_tsc = TIMER_READ_TSC();

    TIMER_LAPIC_WRITE(LAPIC_REG_TICR, s_ticr);
    TIMER_LAPIC_WRITE(LAPIC_REG_LVT_TMR, 0x20000u | 0x20u);   /* periodic | vector 32 */

    kernel_serial_printf(
        "[TIMER] tick calibrated: tsc=%lu Hz, lapic=%lu Hz, ticr=%u, tick=%lu ns%s\n",
        (unsigned long)s_tsc_hz, (unsigned long)lapic_hz, (unsigned)s_ticr,
        (unsigned long)KERNEL_TICK_NS,
        s_tsc_per_tick ? "" : " (CALIBRATION FAILED: one tick per interrupt)");
}

/* Advance the counter to what the clock says it should be.
 *
 * Not `kernel_tick_counter++`: that records interrupts SERVICED, and under a
 * busy or stalled guest the periodic LAPIC coalesces them, so the counter
 * falls behind wall time by exactly the amount of the stall -- which is the
 * bug. Elapsed TSC divided by the calibrated period is the truth whether one
 * interrupt or five elapsed during the interval.
 *
 * The remainder is carried in s_last_tsc rather than discarded, so truncation
 * never accumulates: the counter is never permanently ahead of or behind the
 * clock by more than one tick. An interrupt arriving before a full period has
 * passed advances nothing (no phantom ticks) and leaves its partial period in
 * the carry for the next one.
 *
 * Named limit, because the difference between this and a free-running clock is
 * real: the counter still updates at interrupt granularity, so a section that
 * runs with interrupts OFF sees the value it had on entry. That is unchanged
 * from before and is not the defect here -- the live stalls were the opposite
 * case (interrupts on, busy guest, periodic interrupts coalescing), and those
 * now account for every elapsed millisecond. */
static void timer_advance(void) {
    uint64_t now = TIMER_READ_TSC();

    if (s_tsc_per_tick == 0) {              /* calibration failed: legacy path */
        kernel_tick_counter++;
        return;
    }
    if (now < s_last_tsc) {                 /* counter moved backwards: rebase */
        s_last_tsc = now;
        return;
    }
    uint64_t ticks = (now - s_last_tsc) / s_tsc_per_tick;
    s_last_tsc += ticks * s_tsc_per_tick;
    kernel_tick_counter += ticks;
}

void timer_irq_handler(void) {
    timer_advance();
    net_poll_tick();
    /* Phase 5 channel transport: wake any process parked in k_chan_wait /
     * k_chan_send whose deadline has passed (cap_park_deadline_tick, the
     * strong override in process.c; weak no-op elsewhere). Runs here — in
     * the ISR, BEFORE isr32_stub's schedule_ring3 call — so a woken
     * process is schedulable this very tick (~10 ms granularity, matching
     * KERNEL_TICK_NS). This is a pure proc_table state flip (BLOCKED →
     * SUSPENDED + resume_kernel), takes NO cap locks, and runs on the BSP
     * only (init_timer is BSP-only; the AP cores busy-wait on
     * kernel_sleep_ticks) — the same CPU that schedule_ring3 scans, so
     * there is no cross-CPU race (unlike the AP core's console-service
     * wake, which is pre-existing and separate). */
    cap_park_deadline_tick();
    /* POSIX-Environments E1: the same kind of wake for the Ring-0 control
     * plane. A unified boot's foreground loop yields the CPU to Ring-3 work
     * (kernel_yield_to_ring3) with a tick budget; this flips the parked
     * pseudo-process back to runnable once that budget expires, so the
     * schedule_ring3() call below resumes it. Pure proc_table state flip —
     * no locks, BSP only, exactly like the park-deadline wake above. On
     * every boot that is not unified there is no control plane and this is
     * one NULL-check. */
    proc_control_plane_tick();
    /* BSP-side console drain: when the AP core never comes online
     * (uniprocessor QEMU), microkernel_service_poll() never runs,
     * so the console sidecar channels must still be drained here.
     * console_service_tick takes cap spinlocks — running it directly in
     * this IRQ deadlocks when the timer fires while a process-context cap
     * op holds the same lock (the ISR spins on its own interrupted
     * holder). Latch a pending flag instead; smp_uniprocessor_tick
     * consumes it in process context. */
    console_service_irq_defer();
    /* Driver SDK: the LAPIC timer is a real device edge. When a sidecar
     * has bound vector 32 (k_irq_bind), deliver the tick as a channel
     * notification; unbound, cap_irq_notify is a silent no-op. Woken
     * drivers are picked up by isr32_stub's schedule_ring3 this tick. */
    cap_irq_notify(32);
    TIMER_LAPIC_WRITE(LAPIC_REG_EOI, 0);
}

void kernel_sleep_ticks(uint32_t ticks) {
    uint64_t target = kernel_tick_counter + ticks;
    while (kernel_tick_counter < target)
        __asm__ volatile("pause");
}

/* Calibration results, for boot logs and guards that want the number rather
 * than an inference drawn from behaviour. */
uint64_t kernel_timer_tsc_hz(void) {
    return s_tsc_hz;
}
uint32_t kernel_timer_ticr(void) {
    return s_ticr;
}
int kernel_timer_calibrated(void) {
    return s_tsc_per_tick != 0;
}

#include "../arch/x86/isr_stubs.h"

void init_timer(void) {
    /* Register ISR32 in the IDT */
    set_idt_gate(32, (uint64_t)isr32_stub, 0x8E);

    /* Measure first, then program. The old code programmed a guessed reload
     * and never measured anything, which is why the tree's "~100 Hz" was
     * really 62.5 Hz and fell further behind whenever the guest was busy. */
    timer_calibrate();
}
