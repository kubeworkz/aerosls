#ifndef SCHEDULER_LAZY_H
#define SCHEDULER_LAZY_H

#include "scheduler_extended.h"

// Per-core pointer tracking the thread whose data is *physically* loaded in the FPU/ZMM registers
// Initialised to NULL on core bootup initialization
static struct ExtendedTask* volatile fpu_hardware_owner = NULL;

void init_interrupt_7_handler(void);
void handle_device_not_available_fault(void);

#endif