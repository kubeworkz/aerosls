/*
 * kernel_serial_tx_seam.h — force-included (via -include) into BOTH
 * tests/serial_tx_lock_host_test.c and the real kernel/kernel_io.c.
 *
 * ─── Why this seam exists ─────────────────────────────────────────────────
 * The property the E1 serial TX lock was repaired for is the AMOUNT OF ATOMIC
 * TRAFFIC ITS WAIT PATH GENERATES. That property is invisible to every other
 * kind of test: a lock that swaps on every waiting iteration and a lock that
 * swaps once per acquisition put identical bytes on the wire, return identical
 * codes, and produce identical boot logs. They differ only in how many atomic
 * read-modify-writes a waiter behind another writer performs — up to the whole
 * spin budget in the old shape, none at all in the current one.
 *
 * The only way to distinguish them is to count the atomics, and the only way to
 * count them is to sit in front of the one place they are issued. So
 * kernel_io.c wraps its kernel_serial_tx_try_acquire() in #ifndef, exactly as
 * it does for outb/inb (tests/kernel_io_panic_port.h) and for the same reason:
 * a seam is what makes an otherwise untestable property testable, and the
 * alternative — trusting the comment above the loop — is how the boot wedge got
 * in. The mechanism is a known one for this project: privileged port I/O and
 * hardware atomics are not available to a host process, so the seams are the
 * difference between a claim and a check.
 *
 * The substitution does not weaken what is tested. The hook below performs the
 * REAL compare-and-swap on kernel_io.c's own volatile word, so the lock behaves
 * under the test exactly as it does in the kernel; the counter is the only
 * addition. A hook that lied about the acquire (returning 1 without swapping)
 * would be caught by the test's own release-path checks, which read the flag
 * through the public lock.
 */
#ifndef KERNEL_SERIAL_TX_SEAM_H
#define KERNEL_SERIAL_TX_SEAM_H

/* Returns 1 when the caller may take the port, 0 when another writer holds it.
 * `busy` is kernel_io.c's serial_tx_busy; the hook must swap on it for real. */
int kio_test_try_acquire(volatile int* busy);

#define kernel_serial_tx_try_acquire(busy) kio_test_try_acquire(busy)

#endif /* KERNEL_SERIAL_TX_SEAM_H */
