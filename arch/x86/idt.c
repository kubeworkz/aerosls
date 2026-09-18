#include "idt.h"

// ─── PIC remapping + masking ─────────────────────────────────────────────────
// The legacy 8259A PIC maps IRQ0-7 to INT 0x08-0x0F by default.
// In long mode 0x08 = #DF (Double Fault) → triple fault when timer fires.
// Remap both PICs to 0x20-0x2F and mask all lines; LAPIC handles timing.
static inline void _outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static void pic_remap_and_mask(void) {
    _outb(0x20, 0x11); _outb(0xA0, 0x11);  // ICW1: init
    _outb(0x21, 0x20); _outb(0xA1, 0x28);  // ICW2: vector offsets 0x20/0x28
    _outb(0x21, 0x04); _outb(0xA1, 0x02);  // ICW3: cascade
    _outb(0x21, 0x01); _outb(0xA1, 0x01);  // ICW4: 8086 mode
    _outb(0x21, 0xFF); _outb(0xA1, 0xFF);  // OCW1: mask ALL IRQs
}

// Define array space for all 256 interrupts
__attribute__((aligned(0x10))) 
static struct IDTEntry idt[256];
static struct IDTPointer idt_ptr;

// External reference to the assembly wrappers for the fault/IRQ handlers
#include "isr_stubs.h"

void set_idt_gate(uint8_t vector, uint64_t isr_address, uint8_t attributes) {
    idt[vector].isr_low    = (uint16_t)(isr_address & 0xFFFF);
    idt[vector].kernel_cs  = 0x08; // Matches code segment defined in boot.asm GDT
    idt[vector].ist        = 0;
    idt[vector].attributes = attributes;
    idt[vector].isr_mid    = (uint16_t)((isr_address >> 16) & 0xFFFF);
    idt[vector].isr_high   = (uint32_t)((isr_address >> 32) & 0xFFFFFFFF);
    idt[vector].reserved   = 0;
}

void init_idt(void) {
    idt_ptr.limit = (sizeof(struct IDTEntry) * 256) - 1;
    idt_ptr.base  = (uint64_t)&idt;

    // 0x8E: Present, Ring 0, 64-bit Interrupt Gate
    //
    // #DF is the gate whose ABSENCE is silently fatal. Any fault raised while
    // another fault is being delivered escalates to #DF; with no gate for it
    // the CPU cannot deliver that either and triple-faults, which is a reset --
    // and under -no-reboot a QEMU exit with no [FAULT] line, no panic and no
    // trace of where it happened. With the gate in place the same event prints a
    // report through handle_ring3_fault()'s panic path and halts.
    //
    // Caveat, stated because it is not fixed here: this gate has ist=0 (see
    // set_idt_gate), so a #DF caused by a damaged/kernel-invalid RSP still
    // cannot run -- the handler's first pushes fault again. Catching THAT case
    // needs a dedicated IST stack, which is a larger change than this one.
    set_idt_gate( 8, (uint64_t)isr8_stub,  0x8E);  // #DF Double Fault
    set_idt_gate( 6, (uint64_t)isr6_stub,  0x8E);  // #UD Invalid Opcode
    // #NM Device Not Available — Gap Remediation SIMI Phase 10. Before this,
    // any SSE/AVX instruction (kernel or, going forward, SIMI-JIT-emitted
    // float codegen) executed after a context switch would trap here with
    // no handler installed — an unhandled #NM with no IDT entry, the exact
    // reason -mno-sse is load-bearing for this kernel's own C code today.
    // See arch/x86/lazy_fpu.c and docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 10.
    set_idt_gate( 7, (uint64_t)isr7_stub,  0x8E);  // #NM Device Not Available
    set_idt_gate(11, (uint64_t)isr11_stub, 0x8E);  // #NP Segment Not Present
    set_idt_gate(12, (uint64_t)isr12_stub, 0x8E);  // #SS Stack-Segment Fault
    set_idt_gate(13, (uint64_t)isr13_stub, 0x8E);  // #GP General Protection
    set_idt_gate(14, (uint64_t)isr14_stub, 0x8E);  // #PF Page Fault

    // Driver SDK: one generic gate per device vector (33..255), each
    // delivering to cap_irq_notify (arch/x86/device_irq.c + interrupt.asm).
    init_device_irqs();

    // Remap and silence the legacy 8259A PIC BEFORE enabling interrupts.
    // Without this, IRQ0 fires as INT 0x08 (#DF) → triple fault.
    pic_remap_and_mask();

    // Load table pointer directly into the processor (this core's IDTR)
    idt_load_this_cpu();
    __asm__ volatile("sti"); // Re-enable interrupts globally
}

/* See idt.h. Split out of init_idt() because it is the half an application
 * processor needs: the table itself is built once, is shared read-only, and
 * lives in the identity-mapped kernel image, so an AP loads the SAME table --
 * it must simply load it, which is what nothing did before. */
void idt_load_this_cpu(void) {
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}