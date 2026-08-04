/*
 * kernel_io_panic_port.h — force-included (via -include) into BOTH
 * tests/kernel_panic_output_host_test.c and the real kernel/kernel_io.c.
 *
 * Port I/O is privileged. Without a seam, kernel_io.c cannot be linked into a
 * host test at all, and the panic path stays untestable -- which is precisely
 * how this kernel came to ship a panic path that could not print. The bug it
 * hid (a kernel #PF whose entire report went into a shell capture buffer and
 * was discarded) survived an entire debugging session for exactly that reason.
 *
 * kernel_io.c wraps its outb/inb in #ifndef so these macros win. Note this is
 * a header rather than a pair of -D flags: a -D can define the macros, but the
 * DECLARATIONS the substituted functions need have to reach the translation
 * unit too. Same pattern, and same reasoning, as tests/qemu_sls_test_window.h.
 *
 * The substitution does not weaken what is tested. The production port number
 * (SERIAL_COM1_BASE) is untouched and is asserted in the test itself, so a
 * wrong port fails there rather than hiding here.
 */
#ifndef KERNEL_IO_PANIC_PORT_H
#define KERNEL_IO_PANIC_PORT_H

#include <stdint.h>

void    kio_test_outb(uint16_t port, uint8_t val);
uint8_t kio_test_inb(uint16_t port);

#define outb kio_test_outb
#define inb  kio_test_inb

#endif
