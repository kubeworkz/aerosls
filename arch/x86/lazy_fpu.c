#include "../../include/sls_mmu.h"
    void handle_device_not_available_fault(void) {
    asm volatile("clts");
    }