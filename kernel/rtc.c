/*
 * rtc.c — CMOS (x86) and PL031 (ARM) wall clock.
 *
 * See rtc.h for the contract and why a wrong answer is the failure this file
 * is built around. Implementation notes only below.
 */

#include "rtc.h"

#ifndef RTC_HOST_TEST
#include "kernel_io.h"
extern volatile uint64_t kernel_tick_counter;
#else
#include <stdio.h>
#define kernel_serial_print(s) fputs((s), stderr)
static uint64_t kernel_tick_counter;
#endif

/* ─── Sanity window ────────────────────────────────────────────────────────
 * SLS_BUILD_EPOCH is the build time in Unix seconds, passed by the Makefile.
 * A kernel cannot run before it was compiled, so anything earlier is a broken
 * clock rather than a surprising one.
 *
 * The default matters: if the Makefile ever stops passing it, the floor must
 * not silently become 0, because a floor of 0 accepts the dead-battery values
 * this whole mechanism exists to reject. 2026-01-01 is a date this source
 * demonstrably postdates. */
#ifndef SLS_BUILD_EPOCH
#define SLS_BUILD_EPOCH 1767225600ull      /* 2026-01-01T00:00:00Z */
#endif

/* Twenty years. Loose on purpose -- the goal is catching a register that reads
 * 0xFF or a year of 2255, not policing plausible clock drift. A tight ceiling
 * would reject a node legitimately running for a long time on a fast clock. */
#define RTC_MAX_FUTURE_SECONDS (20ull * 365ull * 24ull * 3600ull)

static uint64_t     build_epoch = SLS_BUILD_EPOCH;
static rtc_source_t source      = RTC_SRC_NONE;
static uint64_t     set_time;          /* base, from rtc_set_unix() */
static uint64_t     set_tick;          /* tick counter when it was set */
static uint8_t      trusted;

/* ─── Calendar arithmetic ──────────────────────────────────────────────────
 * Howard Hinnant's days_from_civil. Shifts the era to start in March so
 * February's variable length falls at the end of the year and the leap rule
 * needs no special case. Correct for any proleptic Gregorian date; in
 * particular it handles 1900 (not a leap year) and 2000 (a leap year), which
 * is where the naive `y % 4 == 0` version is wrong. */
int64_t rtc_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                    /* [0, 399] */
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;     /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

void rtc_civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d)
{
    /* Same era-shifted-to-March trick as days_from_civil, run backwards, so
     * February's variable length again falls at the end of the year and needs
     * no special case. The +719468 undoes the epoch shift. */
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);                  /* [0,146096] */
    const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    const int64_t  yy  = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);             /* [0,365] */
    const unsigned mp  = (5*doy + 2)/153;                               /* [0,11] */
    const unsigned dd  = doy - (153*mp+2)/5 + 1;                        /* [1,31] */
    const unsigned mm  = mp + (mp < 10 ? 3 : -9);                       /* [1,12] */

    if (y) *y = yy + (mm <= 2);
    if (m) *m = mm;
    if (d) *d = dd;
}

int rtc_compose(unsigned year, unsigned mon, unsigned day,
                unsigned hour, unsigned min, unsigned sec, uint64_t* out) {
    /* Field-range checks first. A CMOS register holding 0xFF decodes to
     * something like month 165, and rejecting that here is what stops it
     * becoming a confident timestamp two lines later.
     *
     * Seconds allow 60: a leap second is a real value the hardware can report
     * and is not a fault. */
    if (mon < 1u || mon > 12u)   return RTC_E_INVALID;
    if (day < 1u || day > 31u)   return RTC_E_INVALID;
    if (hour > 23u)              return RTC_E_INVALID;
    if (min  > 59u)              return RTC_E_INVALID;
    if (sec  > 60u)              return RTC_E_INVALID;
    if (year < 1970u || year > 2200u) return RTC_E_INVALID;

    /* Day-of-month against the actual month, so 31 February is rejected
     * rather than silently rolling into March. days_from_civil would happily
     * accept it and produce a real timestamp for the wrong day. */
    static const unsigned mdays[13] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned limit = mdays[mon];
    if (mon == 2u) {
        int leap = (year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u));
        if (leap) limit = 29u;
    }
    if (day > limit) return RTC_E_INVALID;

    int64_t days = rtc_days_from_civil((int64_t)year, mon, day);
    if (days < 0) return RTC_E_INVALID;

    uint64_t t = (uint64_t)days * 86400ull
               + (uint64_t)hour * 3600ull
               + (uint64_t)min  * 60ull
               + (uint64_t)sec;

    /* The rules that catch a plausible wrong answer. */
    if (t < build_epoch) return RTC_E_NOT_SET;
    if (t > build_epoch + RTC_MAX_FUTURE_SECONDS) return RTC_E_NOT_SET;

    *out = t;
    return RTC_OK;
}

/* ─── x86 CMOS / MC146818 ──────────────────────────────────────────────────
 * Registers 0x00 sec, 0x02 min, 0x04 hour, 0x07 day, 0x08 month, 0x09 year.
 * Status A 0x0A, Status B 0x0B.
 *
 * Three things this decode has to get right, each of which produces a
 * confident wrong answer if missed:
 *
 *  1. UPDATE IN PROGRESS. The chip updates its registers roughly once a
 *     second and they are not coherent while it does. Reading across that
 *     boundary yields e.g. 12:59:59 becoming 12:00:59 -- a valid-looking time
 *     an hour off. Status A bit 7 flags it, and the belt-and-braces approach
 *     is to also read twice and require agreement.
 *
 *  2. BCD OR BINARY. Status B bit 2 clear means the values are BCD, so 0x59
 *     is 59 and not 89. Assuming one is a whole-hour error at 0x10.
 *
 *  3. 12-HOUR MODE. Status B bit 1 clear means 12-hour, with bit 7 of the
 *     hours register as PM. Ignore it and every afternoon is twelve hours
 *     early, which passes every range check.
 *
 * The century register is deliberately NOT read. Its location is reported by
 * the ACPI FADT and this kernel does not parse ACPI, so the conventional
 * offset 0x32 is a guess that is wrong on real hardware. A two-digit year is
 * windowed into 2000-2099 instead, and the build-epoch floor catches the
 * error this could cause. Written down because "read 0x32" looks like an
 * obvious improvement until you know why it is not.
 */
#if defined(__x86_64__) && !defined(RTC_HOST_TEST)

static inline void cmos_out(uint8_t v, uint16_t p) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}
static inline uint8_t cmos_in(uint16_t p) {
    uint8_t r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(p)); return r;
}
static uint8_t cmos_read(uint8_t reg) {
    /* Bit 7 of 0x70 is the NMI-disable line. Preserving it as 0 keeps NMIs
     * enabled, which is what the rest of this kernel assumes. */
    cmos_out(reg & 0x7Fu, 0x70);
    return cmos_in(0x71);
}
static int cmos_updating(void) { return (cmos_read(0x0A) & 0x80u) != 0; }
#define HAVE_CMOS 1
#endif

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v & 0x0Fu) + ((v >> 4) * 10u)); }

/* Decodes an already-captured register set. Split out from the port reads so
 * the host test can drive every branch -- BCD, binary, 12-hour AM, 12-hour PM,
 * out-of-range garbage -- on a machine with no CMOS. */
static int decode_cmos(const uint8_t r[8], uint8_t status_b, uint64_t* out) {
    unsigned sec = r[0], min = r[1], hour_raw = r[2];
    unsigned day = r[3], mon = r[4], year2 = r[5];

    int binary   = (status_b & 0x04u) != 0;
    int hour24   = (status_b & 0x02u) != 0;
    /* The PM flag lives in bit 7 of the RAW register and must be taken off
     * before any conversion -- in BCD mode 0x92 is 12 PM, and running it
     * through bcd_to_bin with the flag still set gives 92. */
    int pm       = !hour24 && (hour_raw & 0x80u);
    unsigned hour = hour_raw & 0x7Fu;

    if (!binary) {
        sec = bcd_to_bin((uint8_t)sec);   min = bcd_to_bin((uint8_t)min);
        hour = bcd_to_bin((uint8_t)hour); day = bcd_to_bin((uint8_t)day);
        mon = bcd_to_bin((uint8_t)mon);   year2 = bcd_to_bin((uint8_t)year2);
    }

    if (!hour24) {
        /* 12-hour clocks number noon and midnight as 12, not 0. 12 AM is
         * hour 0; 12 PM is hour 12; everything else adds 12 only for PM. */
        if (hour == 12u) hour = pm ? 12u : 0u;
        else if (pm)     hour += 12u;
    }

    if (year2 > 99u) return RTC_E_INVALID;
    return rtc_compose(2000u + year2, mon, day, hour, min, sec, out);
}

#ifdef RTC_TEST_HOOKS
int rtc_test_decode_cmos(const uint8_t regs[8], uint8_t status_b, uint64_t* out) {
    return decode_cmos(regs, status_b, out);
}
void rtc_test_reset(void) {
    source = RTC_SRC_NONE; trusted = 0; set_time = 0; set_tick = 0;
    build_epoch = SLS_BUILD_EPOCH;
}
void rtc_test_set_build_epoch(uint64_t e) { build_epoch = e; }
#endif

#ifdef HAVE_CMOS
/* Reads until two consecutive captures agree and the chip is not mid-update.
 * Bounded: a wedged RTC must not hang the boot, and returning "no trustworthy
 * time" is a supported outcome. */
static int cmos_read_time(uint64_t* out) {
    uint8_t a[8], b[8], sb;
    for (int attempt = 0; attempt < 100; attempt++) {
        int guard = 0;
        while (cmos_updating() && guard++ < 100000) { }
        if (guard >= 100000) return RTC_E_INVALID;

        a[0]=cmos_read(0x00); a[1]=cmos_read(0x02); a[2]=cmos_read(0x04);
        a[3]=cmos_read(0x07); a[4]=cmos_read(0x08); a[5]=cmos_read(0x09);
        sb  = cmos_read(0x0B);

        while (cmos_updating() && guard++ < 200000) { }
        b[0]=cmos_read(0x00); b[1]=cmos_read(0x02); b[2]=cmos_read(0x04);
        b[3]=cmos_read(0x07); b[4]=cmos_read(0x08); b[5]=cmos_read(0x09);

        int same = 1;
        for (int i = 0; i < 6; i++) if (a[i] != b[i]) same = 0;
        if (same) return decode_cmos(a, sb, out);
    }
    return RTC_E_INVALID;
}
#endif

/* ─── ARM PL031 ────────────────────────────────────────────────────────────
 * QEMU virt maps a PrimeCell RTC at physical 0x09010000, adjacent to the
 * PL011 UART this kernel already drives at 0x09000000 -- same virtual offset
 * convention, see arch/arm64/uart_pl011.c.
 *
 * RTCDR at offset 0 is a 32-bit count of seconds since the epoch, so no BCD,
 * no 12-hour mode, no century question. Which also means no way to tell an
 * unset clock from a set one except by the sanity rules above -- QEMU seeds it
 * from the host, real hardware without a battery reads 0.
 *
 * Not present on a Raspberry Pi: the BCM SoCs have no RTC at all. That is not
 * a gap in this driver, it is the hardware, and rtc_set_unix() is the answer. */
#if defined(__aarch64__) && !defined(RTC_HOST_TEST)
#define PL031_BASE  0xFFFF000009010000UL
#define PL031_DR    (*(volatile uint32_t *)(PL031_BASE + 0x000))
#define PL031_CR    (*(volatile uint32_t *)(PL031_BASE + 0x00C))
#define HAVE_PL031  1
#endif

int rtc_init(void) {
    uint64_t t;

#ifdef HAVE_CMOS
    if (cmos_read_time(&t) == RTC_OK) {
        source = RTC_SRC_CMOS; trusted = 1;
        kernel_serial_print("[RTC] CMOS clock accepted.\n");
        return RTC_OK;
    }
    kernel_serial_print("[RTC] CMOS present but its reading failed the sanity "
                        "rules -- treating this node as having NO trusted time. "
                        "Certificate validity cannot be checked until it is set.\n");
    source = RTC_SRC_CMOS; trusted = 0;
    return RTC_E_NOT_SET;
#elif defined(HAVE_PL031)
    /* CR bit 0 is RTCEN; QEMU has it set out of reset, but a controller that
     * is disabled reports 0 forever and that must not read as 1970. */
    if ((PL031_CR & 1u) == 0u) {
        kernel_serial_print("[RTC] PL031 present but disabled -- no trusted time.\n");
        source = RTC_SRC_PL031; trusted = 0;
        return RTC_E_NO_SOURCE;
    }
    t = (uint64_t)PL031_DR;
    if (t < build_epoch || t > build_epoch + RTC_MAX_FUTURE_SECONDS) {
        kernel_serial_print("[RTC] PL031 reading is outside the sanity window "
                            "-- no trusted time on this node.\n");
        source = RTC_SRC_PL031; trusted = 0;
        return RTC_E_NOT_SET;
    }
    source = RTC_SRC_PL031; trusted = 1;
    kernel_serial_print("[RTC] PL031 clock accepted.\n");
    return RTC_OK;
#else
    (void)t;
    kernel_serial_print("[RTC] no clock hardware on this machine. Time must be "
                        "supplied at provisioning; certificate validity is "
                        "unverifiable until it is.\n");
    source = RTC_SRC_NONE; trusted = 0;
    return RTC_E_NO_SOURCE;
#endif
}

int rtc_get_unix(uint64_t* out) {
    if (!out) return RTC_E_INVALID;
    if (!trusted) return (source == RTC_SRC_NONE) ? RTC_E_NO_SOURCE : RTC_E_NOT_SET;

    if (source == RTC_SRC_SET) {
        /* Advance the supplied base by elapsed ticks. ~100 ticks per second,
         * matching net/http.c's own uptime conversion. Coarse, and honest
         * about it: this is good enough to decide whether a certificate
         * expired last month, which is what it is for. */
        uint64_t elapsed = (kernel_tick_counter - set_tick) / 100ull;
        *out = set_time + elapsed;
        return RTC_OK;
    }

#ifdef HAVE_CMOS
    return cmos_read_time(out);
#elif defined(HAVE_PL031)
    {
        uint64_t t = (uint64_t)PL031_DR;
        if (t < build_epoch || t > build_epoch + RTC_MAX_FUTURE_SECONDS)
            return RTC_E_NOT_SET;
        *out = t;
        return RTC_OK;
    }
#else
    return RTC_E_NO_SOURCE;
#endif
}

int rtc_set_unix(uint64_t t) {
    /* The same rules the hardware path is held to. An operator typing 2016 for
     * 2026 is exactly the wrong-but-plausible input this file exists to
     * refuse, and refusing it here is cheaper than debugging it later. */
    if (t < build_epoch) return RTC_E_NOT_SET;
    if (t > build_epoch + RTC_MAX_FUTURE_SECONDS) return RTC_E_NOT_SET;

    set_time = t;
    set_tick = kernel_tick_counter;
    source   = RTC_SRC_SET;
    trusted  = 1;
    kernel_serial_print("[RTC] time supplied externally and accepted.\n");
    return RTC_OK;
}

int rtc_is_trusted(void) { return trusted != 0; }

rtc_source_t rtc_get_source(void) { return source; }

/* --- One decomposition, two consumers -------------------------------------
 * Unix seconds to civil fields. The counterpart to rtc_compose(), and the
 * caller-facing form of rtc_civil_from_days(): that one stops at the date,
 * because the day count is the interesting half of the arithmetic, and every
 * caller then has to split the seconds-of-day itself.
 *
 * Two callers now do -- mbedtls_platform_gmtime_r() in tls_platform.c and
 * tls_cert_format_time() in tls_cert.c -- so the split lives here once rather
 * than in each of them. It was written in tls_platform.c first; this is that
 * code moved, not a second version of it. A project carrying two snprintfs by
 * accident should not acquire two calendars on purpose.
 *
 * Signed, because gmtime_r's contract admits times before 1970 and the floor
 * division below is the whole reason: C truncates toward zero, so -1 / 86400
 * is 0 and the last second of 1969 would decode as 1970-01-01T00:00:-1.
 *
 * Every out-parameter is optional; pass NULL for the fields you do not want. */
void rtc_break_down(int64_t t, int64_t* y, unsigned* mon, unsigned* day,
                    unsigned* hour, unsigned* min, unsigned* sec, int* wday) {
    int64_t days = t / 86400;
    int64_t rem  = t % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }   /* floor, not truncate */

    int64_t yy = 0; unsigned mm = 0, dd = 0;
    rtc_civil_from_days(days, &yy, &mm, &dd);

    if (y)    *y    = yy;
    if (mon)  *mon  = mm;
    if (day)  *day  = dd;
    if (hour) *hour = (unsigned)(rem / 3600);
    if (min)  *min  = (unsigned)((rem % 3600) / 60);
    if (sec)  *sec  = (unsigned)(rem % 60);
    /* 1970-01-01 was a Thursday, so day 0 is weekday 4. */
    if (wday) { int w = (int)((days + 4) % 7); if (w < 0) w += 7; *wday = w; }
}
