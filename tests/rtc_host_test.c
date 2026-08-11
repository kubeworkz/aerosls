/*
 * rtc_host_test.c — kernel/rtc.c: calendar arithmetic, CMOS decode, and the
 * refusal rules that make a wrong clock safe.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -DRTC_HOST_TEST -DRTC_TEST_HOOKS \
 *       -I . -I kernel -o /tmp/rtc_host_test tests/rtc_host_test.c kernel/rtc.c
 *   /tmp/rtc_host_test
 *
 * ─── What is worth testing here ────────────────────────────────────────────
 * Not "does it read the chip" -- that needs a chip. The bugs in an RTC driver
 * that matter are the ones that produce a VALID-LOOKING WRONG TIME, and every
 * one of them lives in pure logic that can be driven from a host:
 *
 *   - a leap-year rule that is right in 2024 and wrong in 2100
 *   - BCD read as binary: 0x59 becomes 89, and every check still passes
 *   - the 12-hour PM bit ignored: every afternoon is twelve hours early
 *   - 31 February accepted and rolled silently into March
 *   - a dead CMOS battery reading 2000-01-01 accepted as truth
 *
 * The last is the one this file cares most about, because it is the one with
 * a security consequence: an expired certificate validates against a clock
 * that thinks it is 2000.
 *
 * The date arithmetic is checked against values computed with Python's
 * datetime -- an independent implementation, not this code's own output.
 *
 * That mattered immediately: three of the constants in this file were first
 * written from memory and three were wrong -- 2100-03-01, 2400-03-01 and the
 * reference timestamp. Two of them were in the leap-century cases, which is
 * precisely where a hand-checked value is least trustworthy and where a wrong
 * EXPECTED value would have been "fixed" by breaking the implementation to
 * match. Expected values do not come from recollection.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "rtc.h"

static int checks = 0, fails = 0;
static void ok(int cond, const char* what) {
    checks++;
    if (cond) printf("ok:   %s\n", what);
    else { printf("  FAIL  %s\n", what); fails++; }
}
static void eq_u64(uint64_t got, uint64_t want, const char* what) {
    checks++;
    if (got == want) printf("ok:   %s\n", what);
    else { printf("  FAIL  %s\n         got  %llu\n         want %llu\n",
                  what, (unsigned long long)got, (unsigned long long)want); fails++; }
}

/* Build epoch used throughout, so the sanity window is deterministic rather
 * than whatever the real build stamped in: 2026-01-01T00:00:00Z. */
#define TEST_EPOCH 1767225600ull

int main(void) {
    uint64_t t;

    printf("-- 1: days_from_civil against Python datetime --\n");

    eq_u64((uint64_t)rtc_days_from_civil(1970, 1, 1),      0, "epoch day is 0");
    ok(rtc_days_from_civil(1969,12,31) == -1,                 "the day before the epoch is -1");
    eq_u64((uint64_t)rtc_days_from_civil(2000, 3, 1),  11017, "2000-03-01 (after a leap 29 Feb)");
    eq_u64((uint64_t)rtc_days_from_civil(2024, 2, 29), 19782, "2024-02-29 exists");
    eq_u64((uint64_t)rtc_days_from_civil(2026, 8,11),  20676, "2026-08-11");
    eq_u64((uint64_t)rtc_days_from_civil(2100, 3, 1),  47541, "2100-03-01 (2100 is NOT a leap year)");
    eq_u64((uint64_t)rtc_days_from_civil(2400, 3, 1), 157114, "2400-03-01 (2400 IS a leap year)");

    /* The 100/400 rules, stated as differences so a naive `y%4` version is
     * visibly wrong: 2100 has 365 days, 2000 had 366. */
    ok(rtc_days_from_civil(2101,1,1) - rtc_days_from_civil(2100,1,1) == 365,
       "2100 is 365 days (the %100 rule)");
    ok(rtc_days_from_civil(2001,1,1) - rtc_days_from_civil(2000,1,1) == 366,
       "2000 was 366 days (the %400 exception)");

    printf("\n-- 2: rtc_compose range checking --\n");
    rtc_test_reset();
    rtc_test_set_build_epoch(TEST_EPOCH);

    ok(rtc_compose(2026, 8, 11, 12, 30, 45, &t) == RTC_OK, "a valid date composes");
    eq_u64(t, 1786451445ull, "2026-08-11T12:30:45Z is the right timestamp");

    ok(rtc_compose(2026, 13, 1, 0, 0, 0, &t) == RTC_E_INVALID, "month 13 rejected");
    ok(rtc_compose(2026,  0, 1, 0, 0, 0, &t) == RTC_E_INVALID, "month 0 rejected");
    ok(rtc_compose(2026,  2,29, 0, 0, 0, &t) == RTC_E_INVALID,
       "29 Feb 2026 rejected (2026 is not a leap year)");
    ok(rtc_compose(2028,  2,29, 0, 0, 0, &t) == RTC_OK,
       "29 Feb 2028 accepted (2028 is)");
    ok(rtc_compose(2026,  2,31, 0, 0, 0, &t) == RTC_E_INVALID,
       "31 Feb rejected, not rolled into March");
    ok(rtc_compose(2026,  4,31, 0, 0, 0, &t) == RTC_E_INVALID, "31 April rejected");
    ok(rtc_compose(2026,  8,11,24, 0, 0, &t) == RTC_E_INVALID, "hour 24 rejected");
    ok(rtc_compose(2026,  8,11,23,60, 0, &t) == RTC_E_INVALID, "minute 60 rejected");
    ok(rtc_compose(2026,  8,11,23,59,60, &t) == RTC_OK,        "second 60 allowed (leap second)");
    ok(rtc_compose(2026,  8,11,23,59,61, &t) == RTC_E_INVALID, "second 61 rejected");

    /* 0xFF in every register, the classic dead-hardware pattern, decoded as
     * binary. It must not produce a timestamp. */
    ok(rtc_compose(2255, 255, 255, 255, 255, 255, &t) == RTC_E_INVALID,
       "all-0xFF registers rejected");

    printf("\n-- 3: the refusal rules (the security-relevant part) --\n");

    /* A dead CMOS battery. This is the whole reason the floor exists: the
     * hardware reports it confidently and every field is in range. */
    ok(rtc_compose(2000, 1, 1, 0, 0, 0, &t) == RTC_E_NOT_SET,
       "2000-01-01 (dead battery) REFUSED, not accepted as truth");
    ok(rtc_compose(2025,12,31,23,59,59, &t) == RTC_E_NOT_SET,
       "one second before the build epoch is refused");
    ok(rtc_compose(2026, 1, 1, 0, 0, 0, &t) == RTC_OK,
       "the build epoch itself is accepted");
    ok(rtc_compose(2150, 1, 1, 0, 0, 0, &t) == RTC_E_NOT_SET,
       "a time far in the future is refused");

    /* The leap rule inside rtc_compose() is only reachable for years the
     * sanity window admits -- 2026 to 2046 -- and no century year falls there,
     * so y%4 and the full 100/400 rule agree on every legal input. Mutation
     * testing found exactly that: replacing the rule with y%4 killed nothing.
     *
     * Moving the build epoch makes a century year reachable, which turns a
     * correct-but-unverifiable branch into a tested one. Without this the
     * February length check is right by inspection only. */
    rtc_test_set_build_epoch(3976214400ull);      /* 2096-01-01T00:00:00Z */
    ok(rtc_compose(2096, 2, 29, 0, 0, 0, &t) == RTC_OK,
       "2096-02-29 accepted (divisible by 4, not a century)");
    ok(rtc_compose(2100, 2, 29, 0, 0, 0, &t) == RTC_E_INVALID,
       "2100-02-29 REJECTED (the %100 rule -- y%4 alone would accept it)");
    ok(rtc_compose(2100, 2, 28, 0, 0, 0, &t) == RTC_OK,
       "2100-02-28 is fine, so the rejection above is about the 29th");
    rtc_test_set_build_epoch(TEST_EPOCH);

    printf("\n-- 3b: civil_from_days, the inverse --\n");

    /* Added because mbedTLS's X.509 validity checks call gmtime_r, and
     * kernel/tls_platform.c builds it on this. Both directions of the
     * conversion now live together and are tested together.
     *
     * Round-tripping is the strong assertion: any single-day error in either
     * direction breaks it, and a certificate window shifted by one day is
     * exactly the kind of wrong that looks fine until a renewal. */
    {
        int64_t y; unsigned m, d;
        int rt_ok = 1;
        for (int64_t day = -25000; day <= 60000; day += 7) {
            rtc_civil_from_days(day, &y, &m, &d);
            if (rtc_days_from_civil(y, m, d) != day) { rt_ok = 0; break; }
        }
        ok(rt_ok, "round-trips for every 7th day across 1901..2134");

        rtc_civil_from_days(0, &y, &m, &d);
        ok(y == 1970 && m == 1 && d == 1, "day 0 is 1970-01-01");
        rtc_civil_from_days(-1, &y, &m, &d);
        ok(y == 1969 && m == 12 && d == 31, "day -1 is 1969-12-31");
        rtc_civil_from_days(19782, &y, &m, &d);
        ok(y == 2024 && m == 2 && d == 29, "day 19782 is 2024-02-29 (a leap day)");
        rtc_civil_from_days(47541, &y, &m, &d);
        ok(y == 2100 && m == 3 && d == 1, "day 47541 is 2100-03-01, not 02-29");
        rtc_civil_from_days(20676, &y, &m, &d);
        ok(y == 2026 && m == 8 && d == 11, "day 20676 is 2026-08-11");
    }

    printf("\n-- 4: CMOS decode --\n");
    {
        /* Status B: bit 1 = 24-hour, bit 2 = binary.
         * regs are [sec, min, hour, day, month, year2]. */
        uint8_t r[8] = {0};
        uint64_t got;

        /* BCD, 24-hour: 2026-08-11 12:30:45 */
        r[0]=0x45; r[1]=0x30; r[2]=0x12; r[3]=0x11; r[4]=0x08; r[5]=0x26;
        ok(rtc_test_decode_cmos(r, 0x02, &got) == RTC_OK, "BCD 24-hour decodes");
        eq_u64(got, 1786451445ull, "  ...to the right instant");

        /* Binary, 24-hour: the same moment. Both encodings must agree, which
         * is the assertion that catches treating one as the other. */
        r[0]=45; r[1]=30; r[2]=12; r[3]=11; r[4]=8; r[5]=26;
        ok(rtc_test_decode_cmos(r, 0x06, &got) == RTC_OK, "binary 24-hour decodes");
        eq_u64(got, 1786451445ull, "  ...to the SAME instant as the BCD form");

        /* BCD, 12-hour, PM: 12:30:45 PM is 12:30:45. Bit 7 set on hours. */
        r[0]=0x45; r[1]=0x30; r[2]=0x12 | 0x80; r[3]=0x11; r[4]=0x08; r[5]=0x26;
        ok(rtc_test_decode_cmos(r, 0x00, &got) == RTC_OK, "BCD 12-hour PM decodes");
        eq_u64(got, 1786451445ull, "  ...12 PM stays hour 12, it does not become 24");

        /* BCD, 12-hour, AM: 12:30:45 AM is 00:30:45 -- the case people get
         * backwards, and it is a twelve-hour error that passes every check. */
        r[2] = 0x12;
        ok(rtc_test_decode_cmos(r, 0x00, &got) == RTC_OK, "BCD 12-hour AM decodes");
        eq_u64(got, 1786451445ull - 12ull*3600ull, "  ...12 AM becomes hour 0");

        /* BCD, 12-hour, 3 PM = hour 15. */
        r[2] = 0x03 | 0x80;
        ok(rtc_test_decode_cmos(r, 0x00, &got) == RTC_OK, "BCD 12-hour 3 PM decodes");
        eq_u64(got, 1786451445ull + 3ull*3600ull, "  ...3 PM becomes hour 15");

        /* A year register of 0x99 windows to 2099 -- inside the ceiling? No:
         * it must be refused as too far in the future, which is the intended
         * behaviour of the sanity window rather than a decode failure. */
        r[0]=0x00; r[1]=0x00; r[2]=0x00; r[3]=0x01; r[4]=0x01; r[5]=0x99;
        ok(rtc_test_decode_cmos(r, 0x02, &got) == RTC_E_NOT_SET,
           "year 2099 refused by the future ceiling, not silently accepted");

        /* BCD register holding a non-BCD nibble (0x1A). bcd_to_bin turns the
         * low nibble 0xA into 10, so 0x1A reads as 20 -- in range, wrong, and
         * undetectable from the value alone. Documented rather than claimed
         * to be caught: this is a real limit of the decode. */
        r[0]=0x00; r[1]=0x00; r[2]=0x00; r[3]=0x11; r[4]=0x08; r[5]=0x26;
        ok(rtc_test_decode_cmos(r, 0x02, &got) == RTC_OK,
           "a well-formed BCD set decodes (malformed BCD nibbles are NOT detectable)");
    }

    printf("\n-- 5: rtc_set_unix and the trust flag --\n");
    rtc_test_reset();
    rtc_test_set_build_epoch(TEST_EPOCH);

    ok(rtc_is_trusted() == 0,        "no trusted time before anything is set");
    ok(rtc_get_unix(&t) != RTC_OK,   "rtc_get_unix() refuses before anything is set");

    ok(rtc_set_unix(1000000000ull) == RTC_E_NOT_SET,
       "an operator-supplied time before the build epoch is refused");
    ok(rtc_is_trusted() == 0, "  ...and does not mark the node as trusted");

    ok(rtc_set_unix(1786451445ull) == RTC_OK, "a sane operator-supplied time is accepted");
    ok(rtc_is_trusted() == 1, "  ...and the node now has trusted time");
    ok(rtc_get_source() == RTC_SRC_SET, "  ...attributed to the external source");
    ok(rtc_get_unix(&t) == RTC_OK && t == 1786451445ull, "  ...and reads back");

    /* Untouched-on-failure, the same contract entropy_get() has. */
    rtc_test_reset();
    rtc_test_set_build_epoch(TEST_EPOCH);
    t = 0xDEADBEEFull;
    rtc_get_unix(&t);
    eq_u64(t, 0xDEADBEEFull, "a refused rtc_get_unix() leaves the buffer untouched");

    printf("\n=== %d passed, %d failed ===\n", checks - fails, fails);
    return fails == 0 ? 0 : 1;
}
