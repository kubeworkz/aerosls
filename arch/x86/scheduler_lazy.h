#ifndef SCHEDULER_LAZY_H
#define SCHEDULER_LAZY_H

#include <stddef.h>
#include "scheduler_extended.h"

// Per-core pointer tracking the thread whose data is *physically* loaded in the FPU/ZMM registers
// Initialised to NULL on core bootup initialization.
// Gap Remediation SIMI Phase 10: the real definition now lives in
// lazy_fpu.c (a plain extern here, not `static` -- a file-scope static in
// a header gives every including translation unit its own private copy
// instead of one shared global; see lazy_fpu.c's comment for the full
// story).
extern struct ExtendedTask* volatile fpu_hardware_owner;

// Gap Remediation SIMI Phase 10: init_interrupt_7_handler() used to be
// declared here with no definition anywhere in the codebase (a dangling
// prototype that would fail to link if anything ever called it — nothing
// did). Vector 7 is now registered the same way every other exception
// vector already is, directly in arch/x86/idt.c's init_idt(); there is no
// separate per-vector init function for any of them, so this one didn't
// need to be the exception. Removed rather than implemented, to match
// the established pattern instead of adding a second one.
void handle_device_not_available_fault(void);

#endif