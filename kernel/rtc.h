#ifndef SLS_RTC_H
#define SLS_RTC_H

#include <stdint.h>

/*
 * rtc.h — wall-clock time, and an honest answer when there isn't any.
 *
 * TLS Phase 1. See docs/AeroSLS-TLS-Design-v0.1.md §1.1.
 *
 * ─── Why this exists ───────────────────────────────────────────────────────
 * The kernel has kernel_tick_counter, which is monotonic time since boot. That
 * is the right thing for timeouts and the wrong thing for X.509: certificate
 * validity is expressed in calendar time, so without a wall clock an expired
 * or revoked certificate is indistinguishable from a valid one.
 *
 * ─── The failure this file is mostly about ─────────────────────────────────
 * Not "there is no clock" -- that case is easy, you refuse. The dangerous case
 * is a clock that answers CONFIDENTLY AND WRONGLY. A CMOS chip with a dead
 * battery does not report an error; it reports 2000-01-01, or 1980-01-01, or
 * whatever its registers power up to. Every certificate on the machine is then
 * "not yet valid", or -- far worse, depending which way the clock is wrong --
 * an expired one is accepted.
 *
 * So rtc_get_unix() is allowed to fail, and it fails on plausible-looking
 * garbage as well as on absent hardware:
 *
 *   - no RTC on this machine                  -> RTC_E_NO_SOURCE
 *   - registers that decode to nonsense       -> RTC_E_INVALID
 *   - a time BEFORE this kernel was built     -> RTC_E_NOT_SET
 *   - a time absurdly far in the future       -> RTC_E_NOT_SET
 *
 * The build-epoch floor is the interesting one and it is nearly free: a kernel
 * cannot legitimately be running before it was compiled. Any clock claiming
 * otherwise is wrong, and one comparison converts the most common hardware
 * failure in this area from a silent security hole into a refusal.
 *
 * ─── Callers ──────────────────────────────────────────────────────────────
 * Treat a failure exactly as entropy.h says to treat its own: do not fall
 * back, do not assume the epoch, do not "use boot time for now". Certificate
 * validation with no trusted time must decline to validate. That is a real
 * operational cost -- a Raspberry Pi has no RTC at all -- and the answer there
 * is rtc_set_unix() at provisioning or from NTP, not a default.
 */

#define RTC_OK             0
#define RTC_E_NO_SOURCE  (-1)   /* no RTC hardware on this machine */
#define RTC_E_INVALID    (-2)   /* hardware present, registers are nonsense */
#define RTC_E_NOT_SET    (-3)   /* decodes fine but cannot be true */

/* Probe for a clock. Safe to call more than once. Returns RTC_OK if a source
 * was found AND its current reading passes the sanity rules -- a machine whose
 * RTC exists but reads 1980 reports failure here, at boot, where somebody can
 * see it, rather than at the first handshake. */
int rtc_init(void);

/* Seconds since 1970-01-01T00:00:00Z. On failure *out is UNTOUCHED, same
 * contract as entropy_get(), and for the same reason. */
int rtc_get_unix(uint64_t* out);

/* Supply the time from outside: an operator at provisioning, or NTP once that
 * exists. This is the only path that works on hardware with no RTC.
 *
 * Rejects values that fail the same sanity rules, so a typo'd year cannot be
 * installed as truth. Once set, the value advances with kernel_tick_counter --
 * it does NOT re-read hardware, because on the machines that need this call
 * there is no hardware to re-read. */
int rtc_set_unix(uint64_t t);

/* Non-zero when rtc_get_unix() will currently succeed. Intended for "may we
 * validate certificates at all" decisions and for status display. */
int rtc_is_trusted(void);

/* Which source answered, for operator-facing status. Never used to decide
 * anything -- a caller should ask rtc_is_trusted(), not infer it from here. */
typedef enum {
    RTC_SRC_NONE = 0,
    RTC_SRC_CMOS,        /* x86 CMOS/MC146818 via ports 0x70/0x71 */
    RTC_SRC_PL031,       /* ARM PrimeCell RTC, QEMU virt */
    RTC_SRC_SET          /* supplied via rtc_set_unix() */
} rtc_source_t;

rtc_source_t rtc_get_source(void);

/* ─── Pure conversion, exposed because it is the part worth testing ────────
 * Days since the Unix epoch for a proleptic Gregorian date. Howard Hinnant's
 * days_from_civil: branch-free, no lookup tables, no loop over years, and
 * correct across the 100/400 rules that hand-rolled versions get wrong in
 * 1900 and 2100.
 *
 * Exposed in the header so the host test can drive it against an independent
 * implementation across thousands of dates, which is not possible if it is
 * static and only reachable through a hardware read. */
int64_t rtc_days_from_civil(int64_t y, unsigned m, unsigned d);

/* Assemble a Unix timestamp from decoded calendar fields, applying the sanity
 * rules. Returns RTC_OK or RTC_E_INVALID / RTC_E_NOT_SET. Also exposed for
 * testing: this is where a wrong century or a mis-decoded PM bit turns into a
 * wrong-but-plausible answer. */
int rtc_compose(unsigned year, unsigned mon, unsigned day,
                unsigned hour, unsigned min, unsigned sec, uint64_t* out);

#ifdef RTC_TEST_HOOKS
/* Feed raw CMOS register values instead of reading ports, so the decode path
 * -- BCD vs binary, 12- vs 24-hour, the PM bit -- is testable on a host with
 * no CMOS and no privilege to touch one. */
int  rtc_test_decode_cmos(const uint8_t regs[8], uint8_t status_b, uint64_t* out);
void rtc_test_reset(void);
void rtc_test_set_build_epoch(uint64_t e);
#endif

#endif /* SLS_RTC_H */
