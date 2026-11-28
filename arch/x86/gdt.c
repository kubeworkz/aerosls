#include "gdt.h"
#include "user_paging.h"

// Define a 7-entry GDT (Null, KCode, KData, UData, UCode, and 2 slots for the 16-byte TSS)
static struct GDTEntry gdt[7];
struct GDTPointer gdt_ptr;   // exported for smp.c trampoline handshake
static struct TaskStateSegment tss;

// Static kernel stack dedicated strictly for handling exceptions/syscalls from user space.
// Retained as the pre-relocation TSS stack; after kernel_tables_map() every
// switch into a user task points TSS.RSP0 at that task's own kernel syscall
// stack (a low .bss stack would be shadowed by a low-linked ET_EXEC image).
static uint8_t interruption_stack[4096];

/* A low-linked ET_EXEC (0x400000 + segments) shadows every low kernel .bss
 * page its segments cover -- including the GDT, IDT and TSS at 0x440000.
 * Keep the CPU's descriptor tables at a high canonical supervisor alias, mapped
 * by static page tables and inherited by every user PML4 (clone_kernel_slots
 * copies supervisor PML4 entries by pointer). The aliased physical pages are
 * still the same objects, so init-time construction and tss_set_rsp0() need no
 * second copy. */
#define KERNEL_TABLES_VA 0xFFFF800000000000ULL
static uint64_t kt_pdpt[512] __attribute__((aligned(4096)));
static uint64_t kt_pd[512]   __attribute__((aligned(4096)));
static uint64_t kt_pt[512]   __attribute__((aligned(4096)));
static uint64_t g_idt_high;

extern uint64_t p4_table[512];
extern void idt_page_info(uint64_t* phys, uint64_t* off);

void set_gdt_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access      = access;
}

void set_gdt_tss(int num, uint64_t base, uint32_t limit, uint8_t access) {
    struct GDTSystemEntry* tss_entry = (struct GDTSystemEntry*)&gdt[num];
    
    set_gdt_gate(num, (uint32_t)base, limit, access, 0x00);
    tss_entry->base_upper = (uint32_t)(base >> 32);
    tss_entry->reserved   = 0;
}

void tss_set_rsp0(uint64_t top) {
    tss.rsp0 = top;
}

uint64_t kernel_tables_gdt_va(void) {
    return KERNEL_TABLES_VA + ((uint64_t)&gdt & 0xFFFULL);
}

uint64_t kernel_tables_tss_va(void) {
    return KERNEL_TABLES_VA + ((uint64_t)&tss & 0xFFFULL);
}

uint64_t kernel_tables_idt_va(void) {
    return g_idt_high;
}

static void kernel_tables_map(void) {
    uint64_t idt_phys = 0, idt_off = 0;
    idt_page_info(&idt_phys, &idt_off);
    (void)idt_off;   /* idt is page-aligned by idt.c */

    kt_pt[0] = ((uint64_t)&gdt & ~0xFFFULL) |
               USER_PTE_PRESENT | USER_PTE_WRITE;
    kt_pt[1] = idt_phys | USER_PTE_PRESENT | USER_PTE_WRITE;
    kt_pd[0] = (uint64_t)kt_pt | USER_PTE_PRESENT | USER_PTE_WRITE;
    kt_pdpt[0] = (uint64_t)kt_pd | USER_PTE_PRESENT | USER_PTE_WRITE;
    p4_table[256] = (uint64_t)kt_pdpt | USER_PTE_PRESENT | USER_PTE_WRITE;
    g_idt_high = KERNEL_TABLES_VA + 0x1000;

    __asm__ volatile("invlpg (%0)" : : "r"(KERNEL_TABLES_VA) : "memory");
}

void init_gdt(void) {
    kernel_tables_map();
    gdt_ptr.limit = (sizeof(struct GDTEntry) * 7) - 1;
    gdt_ptr.base  = kernel_tables_gdt_va();

    // Entry 0: Null Descriptor
    set_gdt_gate(0, 0, 0, 0, 0);

    // Entry 1: Kernel Code (Ring 0). Access: Present, Ring 0, Executable, Read/Write (0x9A)
    set_gdt_gate(1, 0, 0xFFFFF, 0x9A, 0xA0); // 0xA0 sets Long-Mode 64-bit flag

    // Entry 2: Kernel Data (Ring 0). Access: Present, Ring 0, Writable (0x92)
    set_gdt_gate(2, 0, 0xFFFFF, 0x92, 0xA0);

    // Entry 3: User Data (Ring 3). Access: Present, Ring 3, Writable (0xF2)
    set_gdt_gate(3, 0, 0xFFFFF, 0xF2, 0xA0);

    // Entry 4: User Code (Ring 3). Access: Present, Ring 3, Executable (0xFA)
    set_gdt_gate(4, 0, 0xFFFFF, 0xFA, 0xA0);

    // Set up the Task State Segment
    for (int i = 0; i < sizeof(struct TaskStateSegment); i++) ((uint8_t*)&tss)[i] = 0;
    tss.rsp0 = (uint64_t)&interruption_stack[4095]; // Top of stack
    tss.iomap_base = sizeof(struct TaskStateSegment);

    // Entry 5 & 6: TSS Descriptor (Takes 16 bytes, occupying two standard slots). Access: 0x89
    set_gdt_tss(5, kernel_tables_tss_va(),
                sizeof(struct TaskStateSegment) - 1, 0x89);

    // Flush and reload registers via inline assembly
    __asm__ volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t" // 0x10 points to Kernel Data Segment (Offset 16)
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq $0x08\n\t"     // 0x08 points to Kernel Code Segment (Offset 8)
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"           // Perform 64-bit far return to reload CS register
        "1:\n\t"
        "mov $0x2B, %%ax\n\t" // 0x2B points to TSS (Offset 40 + Task Attribute Ring 3 bits)
        "ltr %%ax\n\t"        // Load Task Register
        : : "m"(gdt_ptr) : "rax", "memory"
    );
}