#ifndef FAULT_REPORT_H
#define FAULT_REPORT_H

/*
 * fault_report.h — the two predicates that turn a kernel fault line into a
 * diagnosis, kept here rather than inline in stubs.c so a host test can reach
 * them.
 *
 * They exist because of a specific incident. A node printed
 *
 *     [FAULT] Kernel fault  cs=0x8  error=0x0  rip=0xcdcdcdcdcdcdcdcd — Halting.
 *
 * and that one line contains two independent facts that took an entire
 * investigation to re-derive by hand:
 *
 *   1. 0xcdcdcdcdcdcdcdcd is NON-CANONICAL. On x86-64 an address is only
 *      usable if bits 63:48 are a sign-extension of bit 47. The CPU therefore
 *      never fetched an instruction from it -- it raised #GP(0) while LOADING
 *      rip. That is the signature of a smashed return address (or indirect
 *      target), not of a jump into bad code, and it is why the error code is
 *      zero with no selector. Without knowing this, error=0x0 reads like a
 *      nondescript protection fault and the search starts in the wrong place.
 *
 *   2. The value is one byte repeated eight times, so it is DATA, and the byte
 *      names the writer. In that incident 0xCD was the uploaded payload, which
 *      identified the whole class of bug in a single step.
 *
 * Neither fact is obscure and both are mechanical. Having the kernel state
 * them costs a handful of instructions on a path that is already halting
 * forever, and converts a line that needs an expert into a line that needs a
 * grep.
 */

#include <stdint.h>

/* The repeated byte, or -1 if `v` is not eight copies of one byte.
 *
 * Note that 0 is deliberately reported as the pattern 0x00 rather than as "no
 * pattern": a null return address is every bit as much a corruption report as
 * a poisoned one, and callers that want to say something different about it
 * can test for zero themselves. */
static inline int fault_poison_byte(uint64_t v) {
    unsigned char b = (unsigned char)(v & 0xFFu);
    for (int i = 1; i < 8; i++)
        if ((unsigned char)((v >> (i * 8)) & 0xFFu) != b) return -1;
    return (int)b;
}

/* Nonzero if `v` cannot be an instruction pointer on x86-64: bits 63:48 must
 * equal bit 47 sign-extended, i.e. v >> 47 must be all-zeros (low half) or
 * all-ones in its low 17 bits (high half). The two boundary values worth
 * remembering are 0x0000_7fff_ffff_ffff (last canonical low address) and
 * 0x0000_8000_0000_0000 (first non-canonical one). */
static inline int fault_noncanonical(uint64_t v) {
    uint64_t hi = v >> 47;
    return !(hi == 0ULL || hi == 0x1FFFFULL);
}

/* ─── Locating the interrupt frame ────────────────────────────────────────
 * x86-64 always pushes SS:RSP on an interrupt, even at the same privilege
 * level, so the frame is exactly five qwords: RIP, CS, RFLAGS, RSP, SS. Given
 * a value already known to be the saved RIP and CS (the handler receives both
 * as arguments), the frame can be found by scanning upward from the handler's
 * own frame for that adjacent pair.
 *
 * Scanning rather than computing an offset is deliberate. The offset depends
 * on what the assembly stub pushed and on whatever prologue the compiler chose
 * for the handler that day, and an offset that silently goes stale prints
 * confident nonsense at exactly the moment nobody can afford to double-check
 * it. A search either finds the pair or reports that it did not.
 *
 * Returns a pointer to the saved-RIP slot, or 0 if no match is in range. */
static inline const uint64_t* fault_find_iret_frame(const uint64_t* from,
                                                    const uint64_t* limit,
                                                    uint64_t saved_rip,
                                                    uint64_t saved_cs) {
    for (const uint64_t* p = from; p + 1 < limit; p++)
        if (p[0] == saved_rip && p[1] == saved_cs) return p;
    return 0;
}

/* ─── Extent of a poison run ──────────────────────────────────────────────
 * Given a stack address known to hold `pattern`, walks DOWNWARD (toward lower
 * addresses, i.e. into the region the faulting epilogue already popped, which
 * nothing has overwritten) for as long as the qwords keep matching, and
 * returns the lowest matching address.
 *
 * This is the measurement that names the bug. An overflowing buffer writes
 * upward from its own base through the saved registers and return address
 * above it, so the bottom of the poison run is the base of the buffer and the
 * length of the run is how far it overran. A run of about 4096 bytes is a page
 * buffer; about 1024, a DSPP fragment; 64, a name field. One number, and the
 * candidate list collapses.
 *
 * `floor` bounds the walk so a stack full of the pattern cannot send it off
 * the end of mapped memory -- this runs when memory is already known-bad, and
 * a diagnostic that faults while diagnosing a fault teaches nothing. */
static inline const uint64_t* fault_poison_run_base(const uint64_t* known,
                                                    const uint64_t* floor,
                                                    uint64_t pattern) {
    const uint64_t* p = known;
    while (p > floor && p[-1] == pattern) p--;
    return p;
}

/* The other end of the same run: walks UPWARD from `known` and returns the
 * first address that does NOT match. The returned pointer is one past the run,
 * so the run is [known, result).
 *
 * Both directions are needed, and the first version of this header only had
 * one. It anchored the walk at the qword below the interrupted rsp on the
 * theory that a local array overflows upward into the frame above it, so the
 * damage would be below the return point. Two things were wrong with that.
 *
 * The hardware wrote there. An interrupt pushes its five-qword frame starting
 * at the interrupted rsp and going DOWN, so the slots immediately below it --
 * including the very one the faulting `ret` had just popped -- are overwritten
 * with RIP, CS, RFLAGS, RSP and SS before the handler ever runs. Reading them
 * back reports the CPU's own values as if they were program data.
 *
 * And the interesting direction is up. Memory at and above the interrupted rsp
 * is the live stack of every frame that had not yet returned, and the hardware
 * does not touch it. If that is full of payload, the run's top edge is the
 * first surviving frame -- and if the run reaches the top of the stack, then
 * nothing survived, which is a different bug entirely from a local array
 * running off its end. */
static inline const uint64_t* fault_poison_run_end(const uint64_t* known,
                                                   const uint64_t* ceiling,
                                                   uint64_t pattern) {
    const uint64_t* p = known;
    while (p < ceiling && p[0] == pattern) p++;
    return p;
}

#endif /* FAULT_REPORT_H */
