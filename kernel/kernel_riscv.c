#include "../include/sls_mmu.h"

extern void sbi_putchar(char c);
    
void kernel_riscv_main(unsigned long hart_id, unsigned long fdt) {
    const char* msg = "AeroSLS RISC-V Supervisor Node Kernel Online!";
    for(int i = 0; msg[i] != '\0'; i++) sbi_putchar(msg[i]);
    while(1) { asm volatile("wfi"); }
}    

void ap_riscv_kernel_main(void) { while(1); }