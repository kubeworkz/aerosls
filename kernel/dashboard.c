#include "dashboard.h"

struct ActiveTransactionTracker {
    uint16_t token;
    uint64_t start_tsc;
    uint32_t is_active;
};

#define MAX_TRACKED_IOS 128
static struct ActiveTransactionTracker active_ios[MAX_TRACKED_IOS];

void dashboard_log_fault_start(uint16_t token) {
    global_telemetry.total_page_faults++;
    global_telemetry.dynamic_pending_ios++;

    for (int i = 0; i < MAX_TRACKED_IOS; i++) {
        if (!active_ios[i].is_active) {
            active_ios[i].token = token;
            active_ios[i].start_tsc = read_tsc();
            active_ios[i].is_active = 1;
            break;
        }
    }
}

void dashboard_log_fault_end(uint16_t token) {
    uint64_t current_tsc = read_tsc();
    
    for (int i = 0; i < MAX_TRACKED_IOS; i++) {
        if (active_ios[i].is_active && active_ios[i].token == token) {
            uint64_t duration = current_tsc - active_ios[i].start_tsc;
            
            // Rolling average calculation to avoid integer overflow
            if (global_telemetry.average_fault_latency_cycles == 0) {
                global_telemetry.average_fault_latency_cycles = duration;
            } else {
                global_telemetry.average_fault_latency_cycles = (global_telemetry.average_fault_latency_cycles * 7 + duration) / 8;
            }

            active_ios[i].is_active = 0;
            if (global_telemetry.dynamic_pending_ios > 0) {
                global_telemetry.dynamic_pending_ios--;
            }
            break;
        }
    }
}

// Emits an ANSI clear screen string over the serial line to render a clean real-time status matrix
void stream_realtime_dashboard(void) {
    // ANSI code: Clear screen (\033[2J) and move cursor to top-left (\033[H)
    kernel_serial_print("\033[2J\033[H");
    kernel_serial_print("===================================================\n");
    kernel_serial_print("       SINGLE LEVEL STORAGE PERFORMANCE TELEMETRY  \n");
    kernel_serial_print("===================================================\n");
    kernel_serial_printf(" Cumulative Demand Page Faults : %ld\n", global_telemetry.total_page_faults);
    kernel_serial_printf(" Core Physical RAM Evictions   : %ld\n", global_telemetry.total_evictions);
    kernel_serial_printf(" Current Active NVMe Flight IOs: %ld\n", global_telemetry.dynamic_pending_ios);
    kernel_serial_print("---------------------------------------------------\n");
    kernel_serial_printf(" Avg Page Resolution Latency  : %ld CPU Cycles\n", global_telemetry.average_fault_latency_cycles);
    
    // Estimate processing latency in microseconds (Assuming a 2.5GHz QEMU emulated CPU baseline)
    uint64_t microseconds = global_telemetry.average_fault_latency_cycles / 2500;
    kernel_serial_printf(" Estimated Media Access Scale  : %ld us\n", microseconds);
    kernel_serial_print("===================================================\n");
}