#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

// Multiboot2 magic value in eax when GRUB hands control to the kernel
#define MULTIBOOT2_MAGIC  0x36d76289UL

// ─── Tag types we care about ──────────────────────────────────────────────────
#define MB2_TAG_END        0   // last tag
#define MB2_TAG_MMAP       6   // memory map
#define MB2_TAG_ACPI_OLD  14   // ACPI RSDP v1
#define MB2_TAG_ACPI_NEW  15   // ACPI RSDP v2

// ─── Multiboot2 fixed header (at the physical address in ebx) ────────────────
struct mb2_info {
    uint32_t total_size;
    uint32_t reserved;
    // followed by tags
} __attribute__((packed));

// ─── Generic tag header ───────────────────────────────────────────────────────
struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

// ─── Memory map tag (type 6) ─────────────────────────────────────────────────
struct mb2_tag_mmap {
    uint32_t type;          // = 6
    uint32_t size;
    uint32_t entry_size;    // = 24
    uint32_t entry_version; // = 0
    // followed by (size - 16) / entry_size entries
} __attribute__((packed));

// Memory map entry — one per region
struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;    // 1=available, 2=reserved, 3=ACPI, 4=ACPI NVS, 5=bad
    uint32_t zero;
} __attribute__((packed));

// mb2_mmap_entry.type values
#define MB2_MEM_AVAILABLE  1
#define MB2_MEM_RESERVED   2
#define MB2_MEM_ACPI       3
#define MB2_MEM_NVS        4
#define MB2_MEM_BAD        5

// ─── ACPI RSDP (embedded in tag types 14/15) ─────────────────────────────────
struct mb2_rsdp {
    char     signature[8];  // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;      // 0=ACPI 1.0, 2=ACPI 2.0+
    uint32_t rsdt_addr;
    // ACPI 2.0+ fields follow (revision >= 2):
    uint32_t length;
    uint64_t xsdt_addr;
} __attribute__((packed));

// ─── Inline iterator helpers ──────────────────────────────────────────────────
// Align a pointer up to the next 8-byte boundary (tags are 8-byte aligned)
static inline const struct mb2_tag* mb2_tag_next(const struct mb2_tag* t) {
    uint64_t addr = (uint64_t)(uintptr_t)t + t->size;
    addr = (addr + 7) & ~(uint64_t)7;
    return (const struct mb2_tag*)(uintptr_t)addr;
}

#endif /* MULTIBOOT2_H */
