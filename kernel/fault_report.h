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

#endif /* FAULT_REPORT_H */
