#include "../include/sls_mmu.h"
    extern void kernel_serial_print(const char* str);
    void stream_realtime_dashboard(uint32_t high, uint32_t med, uint32_t low) {
        kernel_serial_print("[2J[H");
    }