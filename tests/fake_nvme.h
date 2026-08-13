#ifndef SLS_TESTS_FAKE_NVME_H
#define SLS_TESTS_FAKE_NVME_H

/* fake_nvme.h — a host-side NVMe that can be made to fail on command.
 *
 * ─── What this is for ──────────────────────────────────────────────────────
 * SQLite's testing notes describe inserting "a new Virtual File System object
 * that is specially rigged to simulate an I/O error after a set number of I/O
 * operations", then running the whole suite in a loop with the threshold
 * raised each time. Every path that touches storage gets its error return
 * exercised, including the ones no test would ever think to reach.
 *
 * This kernel has the same seam and never used it: nvme_read_sync() and
 * nvme_write_sync() are the only two ways anything reaches the disk, they both
 * return a status, and until now nothing on the host could make that status
 * non-zero. So the error paths below them have never executed. Eight call
 * sites do not even look at the value.
 *
 * ─── It enforces the real driver's contracts, not just its shape ───────────
 * A mock that merely stores bytes would let a caller pass an unaligned buffer
 * and pass on the host while failing on hardware. drivers/nvme_io.h requires
 * every buffer to be 4 KiB aligned, because NVMe cannot describe an unaligned
 * one with a PRP entry at all. This checks it, loudly, and treats a violation
 * as a test failure rather than quietly copying the bytes anyway.
 *
 * ─── Sparse, because the disk is not ───────────────────────────────────────
 * run-cluster.sh makes a 10 GiB image. Frames are stored in a hash table and
 * only when written, so a test that touches four frames costs four frames.
 * Reading a frame that was never written yields zeroes, which is what a fresh
 * image does.
 */

#include <stdint.h>
#include <stddef.h>

#define FAKE_NVME_FRAME    4096u
#define FAKE_NVME_SECTORS  8u      /* per frame; nvme_io.c sets NLB = 7 */

/* Wipe the disk, zero the counters, clear any injected fault. */
void fake_nvme_reset(void);

/* Fail the Nth I/O operation since the last reset (1-based). op == 0 disables.
 *
 * `persistent` distinguishes the two failure shapes SQLite separates for OOM,
 * and they ask different questions:
 *   0  only that one operation fails -- does the caller notice and recover?
 *   1  that operation and every one after it fails -- does the caller give up
 *      cleanly, or retry forever?
 * A module can pass the first and hang on the second. */
void fake_nvme_fail_at(unsigned long op, int persistent);

/* Write only the first `sectors` of the 8 in the Nth operation's frame, then
 * report success. This is a TORN WRITE, and it is not hypothetical here: a
 * 4 KiB write is one command covering 8 sectors, and power lost partway leaves
 * some written and some not. Provided now because the crash-testing work needs
 * it and the seam is the place it belongs; the I/O-error sweep does not use
 * it. op == 0 disables. */
void fake_nvme_tear_at(unsigned long op, unsigned sectors);

/* Counters since reset. `ops` is reads + writes and is what a sweep loops
 * over: run once to learn the total, then fail each one in turn. */
unsigned long fake_nvme_ops(void);
unsigned long fake_nvme_reads(void);
unsigned long fake_nvme_writes(void);

/* Contract violations seen since reset -- unaligned buffers, and frames the
 * store could not hold. Non-zero means the test learned nothing and must say
 * so rather than report a pass. */
unsigned long fake_nvme_violations(void);

/* Read a frame back without counting as an I/O or tripping an injected fault:
 * how a test inspects the disk. Returns 1 if the frame was ever written. */
int fake_nvme_peek(uint64_t lba, unsigned char out[FAKE_NVME_FRAME]);

/* 1 if the frame was written and every byte matches `expect`. */
int fake_nvme_frame_equals(uint64_t lba, const unsigned char *expect, size_t len);

#endif /* SLS_TESTS_FAKE_NVME_H */
