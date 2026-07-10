import os

# Define the file paths and content blocks to build the architecture tree
workspace_files = {}

# -------------------------------------------------------------------------
# GLOBAL BUILD CONFIGURATION & BOOT MANAGEMENT
# -------------------------------------------------------------------------

workspace_files["Makefile"] = """# ==============================================================================
#           AEROSLS UNIFIED CROSS-PLATFORM HARDWARE ARCHITECTURE MATRIX
# ==============================================================================

HOST_CXX    = g++
LLVM_CONFIG = llvm-config
ASN         = nasm
OBJCOPY     = objcopy

PLUGIN_CXXFLAGS = $(shell $(LLVM_CONFIG) --cxxflags) -fPIC -shared -fno-rtti
PLUGIN_LDFLAGS  = $(shell $(LLVM_CONFIG) --ldflags) -Wl,-z,defs
ALLOC_PLUGIN    = libSLSAllocationPassV2.so

# --- x86_64 Toolchain ---
X86_CC      = x86_64-elf-gcc
X86_LD      = x86_64-elf-ld
X86_CFLAGS  = -ffreestanding -O2 -Wall -Wextra -mcmodel=kernel -mno-red-zone -msse -mavx512f
X86_LDFLAGS = -T arch/x86/linker.ld -nostdlib

X86_ASM_SRC = arch/x86/boot.asm arch/x86/interrupt.asm arch/x86/switch_lazy.asm arch/x86/syscall.asm arch/x86/vector_crypto.asm
X86_C_SRC   = kernel/kernel.c arch/x86/idt.c arch/x86/gdt.c kernel/scheduler.c arch/x86/lazy_fpu.c \\
              kernel/lockfree_map.c drivers/ahci.c drivers/pci.c drivers/nvme.c drivers/nvme_admin.c \\
              kernel/frame_pool.c kernel/dashboard.c user/shell.c kernel/smp.c drivers/io_prio.c \\
              net/consensus.c net/prefetch.c kernel/secure_api.c kernel/pte_migrate.c

X86_OBJECTS = $(X86_ASM_SRC:.asm=.x86.o) $(X86_C_SRC:.c=.x86.o) arch/x86/trampoline.o
X86_BIN     = my_sls_kernel.bin
X86_ISO     = sls_operating_system.iso

# --- RISC-V 64-Bit Toolchain ---
RV_CC       = riscv64-unknown-elf-gcc
RV_LD       = riscv64-unknown-elf-ld
RV_CFLAGS   = -ffreestanding -O2 -Wall -Wextra -mcmodel=medany \\
              -march=rv64gc -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections
RV_LDFLAGS  = -T arch/riscv/linker_riscv.ld -nostdlib --gc-sections

RV_ASM_SRC  = arch/riscv/boot_riscv.S arch/riscv/context_riscv.S arch/riscv/vector_state.S
RV_C_SRC    = kernel/kernel_riscv.c arch/riscv/walk_page_tables_riscv.c drivers/pci.c \\
              kernel/frame_pool.c kernel/dashboard.c kernel/pte_migrate.c arch/riscv/sbi.c \\
              arch/riscv/plic.c arch/riscv/lazy_vector.c

RV_OBJECTS  = $(RV_ASM_SRC:.S=.rv.o) $(RV_C_SRC:.c=.rv.o)
RV_ELF      = sls_riscv_kernel.elf

.PHONY: all clean x86-run riscv-run plugins

all: plugins x86-iso riscv-elf

plugins: compiler/SLSAllocationPassV2.cpp
	$(HOST_CXX) $(PLUGIN_CXXFLAGS) $(PLUGIN_LDFLAGS) $< -o $(ALLOC_PLUGIN) $(shell $(LLVM_CONFIG) --libs)

%.x86.o: %.asm
	$(ASN) -f elf64 $< -o $@

%.x86.o: %.c
	$(X86_CC) $(X86_CFLAGS) -c $< -o $@

arch/x86/trampoline.o: arch/x86/trampoline.asm
	$(ASN) -f bin $< -o arch/x86/trampoline.bin
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \\
		--redefine-sym _binary_arch_x86_trampoline_bin_start=trampoline_start \\
		--redefine-sym _binary_arch_x86_trampoline_bin_end=trampoline_end \\
		arch/x86/trampoline.bin arch/x86/trampoline.o

$(X86_BIN): $(X86_OBJECTS)
	$(X86_LD) $(X86_LDFLAGS) $(X86_OBJECTS) -o $(X86_BIN)

x86-iso: $(X86_BIN)
	mkdir -p isodir/boot/grub
	cp $(X86_BIN) isodir/boot/
	cp grub.cfg isodir/boot/grub/
	grub-mkrescue -o $(X86_ISO) isodir
	rm -rf isodir

x86-run: x86-iso
	@if [ ! -f sls_storage.img ]; then qemu-img create -f raw sls_storage.img 10G; fi
	qemu-system-x86_64 -cdrom $(X86_ISO) -drive id=disk,file=sls_storage.img,if=none,format=raw \\
		-device nvme,drive=disk,serial=slsdev0 -m 4G -smp 4 -boot d -serial file:sls_kernel_debug.log

%.rv.o: %.S
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

%.rv.o: %.c
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

$(RV_ELF): $(RV_OBJECTS)
	$(RV_LD) $(RV_LDFLAGS) $(RV_OBJECTS) -o $(RV_ELF)

riscv-elf: $(RV_ELF)

riscv-run: riscv-elf
	@if [ ! -f sls_storage_rv64.img ]; then qemu-img create -f raw sls_storage_rv64.img 10G; fi
	qemu-system-riscv64 -M virt -bios default -kernel $(RV_ELF) \\
		-drive id=disk0,file=sls_storage_rv64.img,if=none,format=raw \\
		-device virtio-blk-device,drive=disk0 \\
		-m 4G -smp 4 -nographic -serial stdio

clean:
	rm -f *.o *.bin *.iso *.elf *.img *.log $(ALLOC_PLUGIN)
	find . -name "*.o" -type f -delete
	find . -name "*.bin" -type f -delete
"""

workspace_files["grub.cfg"] = """set timeout=0
set default=0
menuentry "Single Level Storage OS" {
    multiboot2 /boot/my_sls_kernel.bin
    boot
}"""

workspace_files["deploy.json"] = """{
  "os_name": "Single Level Storage OS",
  "version": "1.0.0-Distributed",
  "disk_layout": {
    "sector_size_bytes": 512,
    "god_anchor_sector": 1024,
    "kernel_start_sector": 2048,
    "system_matrix_start_sector": 4096,
    "user_space_start_sector": 32768
  },
  "payloads": [
    {
      "name": "kernel_binary",
      "source_file": "my_sls_kernel.bin",
      "target_sector_offset": 2048
    }
  ]
}"""

workspace_files["deploy.py"] = """import struct, json, os, subprocess
GOD_MAGIC = 0x534C524F4F544F44
def build_god_anchor(layout):
    raw_anchor = struct.pack("<QQQII", GOD_MAGIC, 1, 2, 0, 0)
    return raw_anchor + b"\\x00" * (layout["sector_size_bytes"] - len(raw_anchor))
def create_deployable_media():
    with open("deploy.json", "r") as f: config = json.load(f)
    layout = config["disk_layout"]
    subprocess.run(["make", "clean"], check=True)
    subprocess.run(["make", "my_sls_kernel.bin"], check=True)
    target_disk = "sls_dist_release.img"
    with open(target_disk, "wb") as disk: disk.truncate(4 * 1024 * 1024 * 1024)
    with open(target_disk, "r+b") as disk:
        for payload in config["payloads"]:
            with open(payload["source_file"], "rb") as pf: binary_data = pf.read()
            disk.seek(payload["target_sector_offset"] * layout["sector_size_bytes"])
            disk.write(binary_data)
        anchor_payload = build_god_anchor(layout)
        disk.seek(layout["god_anchor_sector"] * layout["sector_size_bytes"])
        disk.write(anchor_payload)
    print(f"[DEPLOY] Generated deployable asset: {target_disk}")
if __name__ == "__main__": create_deployable_media()"""

# -------------------------------------------------------------------------
# HARDWARE ABSTRACTION CORE (HAL)
# -------------------------------------------------------------------------

workspace_files["include/sls_mmu.h"] = """#ifndef SLS_MMU_H
#define SLS_MMU_H
#include <stdint.h>
struct SLSPte { uint64_t* raw_entry_ptr; };
#if defined(__x86_64__)
    #define SLS_PTE_VALID       (1ULL << 0)
    #define SLS_PTE_WRITABLE    (1ULL << 1)
    #define SLS_PTE_USER        (1ULL << 2)
    #define SLS_PTE_ACCESSED    (1ULL << 5)
    #define SLS_PTE_DIRTY       (1ULL << 6)
    #define SLS_PTE_SLS_DISK    (1ULL << 9)
    #define SLS_FRAME_MASK      0x000FFFFFFFFFF000ULL
    static inline uint64_t sls_extract_physical_address(uint64_t pte_val) { return pte_val & SLS_FRAME_MASK; }
    static inline uint64_t sls_compile_pte(uint64_t phys_addr, uint64_t flags) { return (phys_addr & SLS_FRAME_MASK) | flags; }
#elif defined(__riscv) || defined(__riscv_xlen)
    #define SLS_PTE_VALID       (1ULL << 0)
    #define SLS_PTE_READABLE    (1ULL << 1)
    #define SLS_PTE_WRITABLE    (1ULL << 2)
    #define SLS_PTE_USER        (1ULL << 4)
    #define SLS_PTE_ACCESSED    (1ULL << 6)
    #define SLS_PTE_DIRTY       (1ULL << 7)
    #define SLS_PTE_SLS_DISK    (1ULL << 8)
    static inline uint64_t sls_extract_physical_address(uint64_t pte_val) { return ((pte_val >> 10) << 12); }
    static inline uint64_t sls_compile_pte(uint64_t phys_addr, uint64_t flags) {
        uint64_t base_pte = (phys_addr >> 12) << 10;
        if (flags & SLS_PTE_WRITABLE) base_pte |= (1ULL << 1);
        return base_pte | flags;
    }
#endif
static inline void sls_mark_page_disk_resident(uint64_t* pte_entry_ptr, uint64_t block_id) {
    *pte_entry_ptr = sls_compile_pte((block_id << 12), SLS_PTE_SLS_DISK);
}
static inline int sls_is_page_dirty(uint64_t pte_val) { return (pte_val & SLS_PTE_DIRTY) ? 1 : 0; }
#endif"""

# -------------------------------------------------------------------------
# X86_64 PLATFORM AND ASSEMBLY CODE BLOCKS
# -------------------------------------------------------------------------

workspace_files["arch/x86/linker.ld"] = """ENTRY(_start)
SECTIONS {
    . = 1M;
    .text : { KEEP(*(.multiboot2)) *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .bss : { *(COMMON) *(.bss) *(.bootstrap_stack) }
}"""

workspace_files["arch/x86/boot.asm"] = """section .multiboot2
align 8
multiboot_start:
    dd 0xe85250d6, 0, multiboot_end - multiboot_start, -(0xe85250d6 + 0 + (multiboot_end - multiboot_start))
    dw 0, 0
    dd 8
multiboot_end:
section .bootstrap_stack, nobits
align 16
stack_bottom: resb 4096 * 4
stack_top:
section .text
bits 32
global _start
_start:
    mov esp, stack_top
    lgdt [gdt64.pointer]
    jmp gdt64.code:_start64
bits 64
_start64:
    xor ax, ax
    mov ss, ax
    mov ds, ax
    extern kernel_main
    call kernel_main
.halt: hlt
    jmp .halt
section .rodata
gdt64: dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.pointer: dw $ - gdt64 - 1
    dq gdt64"""

workspace_files["arch/x86/interrupt.asm"] = """bits 64
global isr14_stub
extern handle_page_fault
isr14_stub:
    push rbp
    mov rbp, rsp
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov rdi, [rbp + 8] 
    call handle_page_fault
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    pop rbp
    add rsp, 8
    iretq"""

workspace_files["arch/x86/switch_lazy.asm"] = """bits 64
global perform_lazy_context_switch
perform_lazy_context_switch:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov [rdi + 8], rsp
    mov rsp, [rsi + 8]
    mov rax, cr0
    or rax, 0x08
    mov cr0, rax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ret"""

workspace_files["arch/x86/syscall.asm"] = """bits 64
global syscall_entry_stub
extern sys_sls_allocate
    syscall_entry_stub:
    swapgs
    mov [gs:0], rsp
    mov rsp, [gs:8]
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rcx
    push r11
    cmp rax, 105
    jne .unknown_syscall
    call sys_sls_allocate
    jmp .syscall_return
    .unknown_syscall:
    xor rax, rax
    .syscall_return:
    pop r11
    pop rcx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    mov rsp, [gs:0]
    swapgs
    sysretq"""

workspace_files["arch/x86/trampoline.asm"] = """bits 16
    section .text
    global trampoline_start
    global trampoline_end
    trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    lgdt [0x08000 + (ap_gdt_ptr - trampoline_start)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:(0x08000 + (ap_protected_mode - trampoline_start))
    bits 32
    ap_protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    mov eax, [0x07000]
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)
    mov cr0, eax
    lgdt [0x07010]
    jmp 0x08:ap_kernel_entry_bridge
    bits 64
    ap_kernel_entry_bridge:
    mov rax, [0x07020]
    mov rsp, [0x07030]
    jmp rax
    align 16
    ap_gdt: dq 0, 0x00CF9A000000FFFF, 0x00CF92000000FFFF
    ap_gdt_ptr: dw $ - ap_gdt - 1
    dd 0x08000 + (ap_gdt - trampoline_start)
    trampoline_end:"""

workspace_files["arch/x86/vector_crypto.asm"] = """bits 64
    section .text
    global avx512_chacha20_block_vectorized
    avx512_chacha20_block_vectorized:
    push rbp
    mov rbp, rsp
    test rdi, 0x3F
    jnz .alignment_fault
    test rsi, 0x3F
    jnz .alignment_fault
    vmovdqa64 zmm0, [rsi]
    vmovdqa64 zmm1, [rdx]
    vpaddd zmm2, zmm0, zmm1
    vpxord zmm3, zmm2, zmm0
    vmovdqa64 [rdi], zmm3
    vpxord zmm0, zmm0, zmm0
    vpxord zmm1, zmm1, zmm1
    vpxord zmm2, zmm2, zmm2
    vpxord zmm3, zmm3, zmm3
    pop rbp
    ret
    .alignment_fault:
    mov rax, -1
    pop rbp
    ret"""

workspace_files["arch/x86/gdt.c"] = """#include "../../include/sls_mmu.h"
    struct GDTEntry { uint16_t limit_low; uint16_t base_low; uint8_t base_mid; uint8_t access; 
    uint8_t granularity; uint8_t base_high; } attribute((packed));
    struct GDTPointer { uint16_t limit; uint64_t base; } attribute((packed));
    static struct GDTEntry gdt;
    static struct GDTPointer gdt_ptr;
    void init_gdt(void) {
    gdt_ptr.limit = (sizeof(struct GDTEntry) * 7) - 1; gdt_ptr.base = (uint64_t)&gdt;
    asm volatile("lgdt %0" : : "m"(gdt_ptr));
    }"""

workspace_files["arch/x86/idt.c"] = """#include "../../include/sls_mmu.h"
    struct IDTEntry { uint16_t isr_low; uint16_t kernel_cs; uint8_t ist; uint8_t attributes; uint16_t isr_mid; uint32_t isr_high; uint32_t reserved; } attribute((packed));
    struct IDTPointer { uint16_t limit; uint64_t base; } attribute((packed));
    static struct IDTEntry idt;
    static struct IDTPointer idt_ptr;
    extern void isr14_stub(void);
    void init_idt(void) {
    idt_ptr.limit = (sizeof(struct IDTEntry) * 256) - 1; idt_ptr.base = (uint64_t)&idt;
    idt.isr_low = (uint16_t)((uint64_t)isr14_stub & 0xFFFF); idt.kernel_cs = 0x08; idt.attributes = 0x8E;
    idt.isr_mid = (uint16_t)(((uint64_t)isr14_stub >> 16) & 0xFFFF); idt.isr_high = (uint32_t)(((uint64_t)isr14_stub >> 32) & 0xFFFFFFFF);
    asm volatile("lidt %0; sti" : : "m"(idt_ptr));
    }"""

workspace_files["arch/x86/lazy_fpu.c"] = """#include "../../include/sls_mmu.h"
    void handle_device_not_available_fault(void) {
    asm volatile("clts");
    }"""

"""     -------------------------------------------------------------------------
     RISC-V ARCHITECTURE AND ASSEMBLY SPECIFICS
    ------------------------------------------------------------------------- """

workspace_files["arch/riscv/linker_riscv.ld"] = """OUTPUT_ARCH( "riscv" )
    ENTRY(_start)
    SECTIONS {
    . = 0x80000000;
    .text : ALIGN(4K) { *(.text.init) (.text .text.) }
    .rodata : ALIGN(4K) { (.rodata .rodata.) }
    .data : ALIGN(4K) { (.data .data.) }
    .bss : ALIGN(4K) { __bss_start = .; (.sbss .sbss.) (.bss .bss.) *(COMMON) __bss_end = .; }
    _end = .;
    }"""

workspace_files["arch/riscv/boot_riscv.S"] = """.section .text.init
    .global _start
    _start:
    csrw sie, zero
    bnez a0, .park_secondary_hart
    la   sp, bsp_stack_top
    la   t0, __bss_start
    la   t1, __bss_end
    .clear_bss_loop:
    bgeu t0, t1, .jump_to_c_kernel
    sd   zero, 0(t0)
    addi t0, t0, 8
    j    .clear_bss_loop
    .jump_to_c_kernel:
    extern kernel_riscv_main
    jal  kernel_riscv_main
    .halt:
    wfi
    j    .halt
    .park_secondary_hart:
    la   t0, flag_cores_synchronized
    .spin_lock_check:
    ld   t1, 0(t0)
    bnez t1, .ap_boot_bridge
    .pause:
    j    .spin_lock_check
    .ap_boot_bridge:
    slli t2, a0, 12
    la   sp, bsp_stack_top
    sub  sp, sp, t2
    extern ap_riscv_kernel_main
    jal  ap_riscv_kernel_main
    .section .bss
    .align 12
    bsp_stack_bottom: .space 4096 * 4
    bsp_stack_top:
    .global flag_cores_synchronized
    .align 3
    flag_cores_synchronized: .quad 0"""

workspace_files["arch/riscv/context_riscv.S"] = """.text
    .global perform_riscv_context_switch
    perform_riscv_context_switch:
    addi sp, sp, -112
    sd   ra,  0(sp)
    sd   s0,  8(sp)
    sd   s1,  16(sp)
    sd   s2,  24(sp)
    sd   s3,  32(sp)
    sd   s4,  40(sp)
    sd   s5,  48(sp)
    sd   s6,  56(sp)
    sd   s7,  64(sp)
    sd   s8,  72(sp)
    sd   s9,  80(sp)
    sd   s10, 88(sp)
    sd   s11, 96(sp)
    sd   tp,  104(sp)
    sd   sp, 8(a0)
    ld   sp, 8(a1)
    ld   ra,  0(sp)
    ld   s0,  8(sp)
    ld   s1,  16(sp)
    ld   s2,  24(sp)
    ld   s3,  32(sp)
    ld   s4,  40(sp)
    ld   s5,  48(sp)
    ld   s6,  56(sp)
    ld   s7,  64(sp)
    ld   s8,  72(sp)
    ld   s9,  80(sp)
    ld   s10, 88(sp)
    ld   s11, 96(sp)
    ld   tp,  104(sp)
    addi sp, sp, 112
    ret"""

workspace_files["arch/riscv/vector_state.S"] = """.text
    .global riscv_vector_save_state
    .global riscv_vector_load_state
    riscv_vector_save_state:
    vsetvli t0, zero, e64, m8, ta, ma
    vse64.v v0,  (a0)
    addi    a0, a0, 64
    vse64.v v8,  (a0)
    addi    a0, a0, 64
    vse64.v v16, (a0)
    addi    a0, a0, 64
    vse64.v v24, (a0)
    ret
    riscv_vector_load_state:
    vsetvli t0, zero, e64, m8, ta, ma
    vle64.v v0,  (a0)
    addi    a0, a0, 64
    vle64.v v8,  (a0)
    addi    a0, a0, 64
    vle64.v v16, (a0)
    addi    a0, a0, 64
    vle64.v v24, (a0)
    ret"""

workspace_files["arch/riscv/sbi.c"] = """#include "../../include/sls_mmu.h"
    #define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x01
    #define SBI_EXT_0_1_CONSOLE_GETCHAR 0x02
    void sbi_putchar(char c) {
        register unsigned long a0 asm("a0") = c;
        register unsigned long a7 asm("a7") = SBI_EXT_0_1_CONSOLE_PUTCHAR;
        asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    }
    int sbi_getchar(void) {
        register unsigned long a0 asm("a0");
        register unsigned long a7 asm("a7") = SBI_EXT_0_1_CONSOLE_GETCHAR;
        asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
        return (int)a0;
    }"""

workspace_files["arch/riscv/plic.c"] = """#include "../../include/sls_mmu.h"
    #define PLIC_BASE_VIRT 0xFFFFFFFF40003000ULL
    void init_riscv_plic(uint32_t target_hart_id) {
        (volatile uint32_t)(PLIC_BASE_VIRT + (10 * 4)) = 5;
        uint32_t s_enable_offset = 0x2000 + ((target_hart_id * 2 + 1) * 0x80);
        (volatile uint32_t)(PLIC_BASE_VIRT + s_enable_offset) |= (1 << 10);
        uint32_t s_threshold_offset = 0x200000 + ((target_hart_id * 2 + 1) * 0x1000);
        (volatile uint32_t)(PLIC_BASE_VIRT + s_threshold_offset) = 0;
    }"""

workspace_files["arch/riscv/walk_page_tables_riscv.c"] = """#include "paging_riscv.h"
    extern void* allocate_physical_ram_frame(void);
    uint64_t* walk_page_tables_sv39(uint64_t root_pt_phys, uint64_t virtual_address) {
    uint64_t* current_table = (uint64_t*)root_pt_phys;
    size_t idx2 = VPN2_INDEX(virtual_address);
    if (!(current_table[idx2] & PTE_V)) {
    uint64_t* new_table = (uint64_t*)allocate_physical_ram_frame();
    if (!new_table) return 0;
    for (int i = 0; i < 512; i++) new_table[i] = 0;
    current_table[idx2] = PA_TO_PTE((uint64_t)new_table) | PTE_V;
    }
    current_table = (uint64_t*)PTE_TO_PA(current_table[idx2]);
    size_t idx1 = VPN1_INDEX(virtual_address);
    if (!(current_table[idx1] & PTE_V)) {
        uint64_t* new_table = (uint64_t*)allocate_physical_ram_frame();
        if (!new_table) return 0;
        for (int i = 0; i < 512; i++) new_table[i] = 0;
        current_table[idx1] = PA_TO_PTE((uint64_t)new_table) | PTE_V;
    }
    current_table = (uint64_t*)PTE_TO_PA(current_table[idx1]);
    return &current_table[VPN0_INDEX(virtual_address)];
    }"""

workspace_files["arch/riscv/paging_riscv.h"] = """#ifndef PAGING_RISCV_H
    #define PAGING_RISCV_H
    #include <stdint.h>
    #define VPN2_INDEX(va) (((va) >> 30) & 0x1FF)
    #define VPN1_INDEX(va) (((va) >> 21) & 0x1FF)
    #define VPN0_INDEX(va) (((va) >> 12) & 0x1FF)
    #define PTE_V     (1ULL << 0)
    #define PTE_R     (1ULL << 1)
    #define PTE_W     (1ULL << 2)
    #define PTE_X     (1ULL << 3)
    #define PTE_U     (1ULL << 4)
    #define PTE_A     (1ULL << 6)
    #define PTE_D     (1ULL << 7)
    #define PTE_SLS_DISK (1ULL << 8)
    #define PTE_TO_PA(pte) (((pte) >> 10) << 12)
    #define PA_TO_PTE(pa)  (((pa) >> 12) << 10)
    #endif"""

workspace_files["arch/riscv/lazy_vector.c"] = """#include "paging_riscv.h"
    void handle_riscv_vector_disabled_trap(void) {
    // Vector lazy logic goes here...
    }"""

"""     -------------------------------------------------------------------------
    INDEPENDENT CORE METADATA ENGINE MODULES
    ------------------------------------------------------------------------- """

workspace_files["kernel/kernel.c"] = """#include "../include/sls_mmu.h"
    void kernel_main(void) {
    volatile char* vga = (volatile char*)0xB8000;
    const char* msg = "SLS Kernel Booted Successfully!";
    for (int i = 0; msg[i] != '\0'; i++) {
    vga[i * 2] = msg[i]; vga[i * 2 + 1] = 0x0F;
    }
    while (1) { asm volatile("hlt"); }
    }"""

workspace_files["kernel/kernel_riscv.c"] = """#include "../include/sls_mmu.h"
    extern void sbi_putchar(char c);
    void kernel_riscv_main(unsigned long hart_id, unsigned long fdt) {
    const char* msg = "AeroSLS RISC-V Supervisor Node Kernel Online!\n";
    for(int i = 0; msg[i] != '\0'; i++) sbi_putchar(msg[i]);
    while(1) { asm volatile("wfi"); }
    }
    void ap_riscv_kernel_main(void) { while(1); }"""

workspace_files["kernel/scheduler.c"] = """#include "../include/sls_mmu.h"
    #define MAX_TASKS 64
    enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED };
    struct Task { uint32_t id; enum TaskState state; uint64_t blocked_on_vaddr; uint64_t rsp; };
    static struct Task task_table[MAX_TASKS];
    static uint32_t current_task_idx = 0;
    uint32_t kernel_get_current_thread_id(void) { return task_table[current_task_idx].id; 
    }"""

workspace_files["kernel/lockfree_map.c"] = """#include "../include/sls_mmu.h"
    #define HASH_BUCKETS 2048
    struct SLSObjectNode { uint64_t unique_object_id; uint64_t global_virtual_address; struct SLSObjectNode* next; };
    static struct SLSObjectNode* volatile concurrent_object_directory[HASH_BUCKETS];
    struct SLSObjectNode* lockfree_lookup_object(uint64_t object_id) {
        uint32_t bucket = object_id % HASH_BUCKETS;
        struct SLSObjectNode* current = concurrent_object_directory[bucket];
        while (current != NULL) {
            if (current->unique_object_id == object_id) return current;
            current = current->next;
        }
        return 0;
    }"""

workspace_files["kernel/frame_pool.c"] = """#include "../include/sls_mmu.h"
    #define TOTAL_FRAMES 1048576
    static uint64_t physical_memory_bitmap[TOTAL_FRAMES / 64];
    void* allocate_physical_ram_frame(void) {
        for (size_t i = 0; i < (TOTAL_FRAMES / 64); i++) {
            if (physical_memory_bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                for (int bit = 0; bit < 64; bit++) {
                    if (!(physical_memory_bitmap[i] & (1ULL << bit))) {
                        physical_memory_bitmap[i] |= (1ULL << bit);
                        return (void*)(((i * 64) + bit) * 4096);
                    }
                }
            }
        }
        return 0;
    }"""

workspace_files["kernel/dashboard.c"] = """#include "../include/sls_mmu.h"
    extern void kernel_serial_print(const char* str);
    void stream_realtime_dashboard(uint32_t high, uint32_t med, uint32_t low) {
        kernel_serial_print("\033[2J\033[H");
    }"""

workspace_files["kernel/smp.c"] = """#include "../include/sls_mmu.h"
    void boot_application_processors(uint8_t target_apic_id) {
        // SMP core execution loops...
    }"""

workspace_files["kernel/secure_api.c"] = """#include "../include/sls_mmu.h"
    void derive_user_key(const char* password, uint32_t len, uint32_t* out_key) {
        // Cryptographic derived key math...
    }"""

workspace_files["kernel/pte_migrate.c"] = """#include "../include/sls_mmu.h"
    void migrate_x86_table_to_riscv(uint64_t* x86_src, uint64_t* riscv_dest) {
        // Page directory mapping loops...
    }"""

"""     -------------------------------------------------------------------------
    HARDWARE PERIPHERAL DRIVERS & PACKET NETWORKING
    ------------------------------------------------------------------------- """

workspace_files["drivers/pci.c"] = """#include "../include/sls_mmu.h"
    uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((uint32_t)1 << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) 
    | ((uint32_t)func << 8) | (offset & 0xFC);
    asm volatile("outl %0, $0xCF8" : : "a"(address));
    uint32_t ret;
    asm volatile("inl $0xCFC, %0" : "=a"(ret));
    return ret;
    }"""

workspace_files["drivers/ahci.c"] = """#include "../include/sls_mmu.h"
    void storage_read_block(uint64_t disk_block_id, void* ram_frame) {
    // Fallback block sector logic...
    }"""

workspace_files["drivers/nvme.c"] = """#include "../include/sls_mmu.h"
    int init_nvme_controller(uint64_t bar0_phys) {
        return 1;
    }"""

workspace_files["drivers/nvme_admin.h"] = """#ifndef NVME_ADMIN_H
    #define NVME_ADMIN_H
    #include <stdint.h>
    struct NVMeCmd { uint8_t opcode; uint8_t flags; uint16_t command_id; uint32_t nsid; 
    uint64_t rsvd; uint64_t metadata; uint64_t prp1; uint64_t prp2; uint32_t cdw; } 
    attribute((packed));
    #define IO_QUEUE_SIZE 256
    #endif"""

workspace_files["drivers/nvme_admin.c"] = """#include "nvme_admin.h" """
workspace_files["drivers/io_prio.c"] = """#include "nvme_admin.h" """
workspace_files["net/consensus.c"] = """#include "../include/sls_mmu.h" """
workspace_files["net/prefetch.c"] = """#include "../include/sls_mmu.h" """
workspace_files["user/shell.c"] = """#include "../include/sls_mmu.h" """

workspace_files["compiler/SLSAllocationPassV2.cpp"] = """#include 
    "llvm/IR/PassManager.h"
    #include "llvm/IR/IRBuilder.h"
    #include "llvm/Passes/PassPlugin.h"
    #include "llvm/Passes/PassBuilder.h"
    #include "llvm/ADT/Triple.h"
    using namespace llvm;
    namespace {
        struct SLSAllocationPassV2 : public PassInfoMixin<SLSAllocationPassV2> {
            PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
                return PreservedAnalyses::all();
            }
        };
    }
    extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
        return { LLVM_PLUGIN_API_VERSION, "SLSAllocationPassV2", "v2.0", [](PassBuilder &) {} };
    }"""

"""     -------------------------------------------------------------------------
    DIRECTORY TREE GENERATION LOOP
    ------------------------------------------------------------------------- """

def execute_workspace_generation():
    print("==================================================================")
    print("           AEROSLS AUTOMATED WORKSPACE INITIALIZER               ")
    print("==================================================================")

    for filepath, content in workspace_files.items():
        normalized_path = os.path.normpath(filepath)
        parent_directory = os.path.dirname(normalized_path)    

        # Recursively construct directory steps
        if parent_directory and not os.path.exists(parent_directory):
            os.makedirs(parent_directory, exist_ok=True)
            print(f" -> Created directory path layout: {parent_directory}/")

        # Write clean source assets into their active positions
        with open(normalized_path, "w") as f:
            f.write(content.strip())
        print(f" [+] Generated file: {normalized_path}")    

    print("------------------------------------------------------------------")
    print(" SUCCESS: AeroSLS Platform Workspace generated cleanly.")
    print(" You can now open this parent folder directly inside your IDE.")
    print("==================================================================")

if __name__ == "__main__":
    execute_workspace_generation()    