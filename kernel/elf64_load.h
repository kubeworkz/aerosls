#ifndef ELF64_LOAD_H
#define ELF64_LOAD_H

/*
 * elf64_load.h — E7 §5.1/§5.2 (the plan's increment 7): the tree's ONE
 * ELF64 image header set and ONE PT_LOAD mapper.
 *
 * ─── Why this exists ───────────────────────────────────────────────────────
 * Two places in the tree map an ELF image into a process's page table:
 * loader.c's loader_load_into_process (uploaded program objects) and, as of
 * increment 7, cap_create_sidecar_in_personality's image step (a Linux-
 * personality child planted with a real static binary — the census
 * candidate, tools/linux_syscall_census.txt §5.1's designated first user).
 * Two PT_LOAD loops would be two truths about segment flags, BSS zero-fill
 * and partition charging — so the loop lives HERE, once, and both call it.
 * The minimal ELF64 structs used to live in loader.h; they moved here so
 * cap.c (and its host test) can include this light header without pulling
 * loader.h's process/simi dependency chain. loader.h includes this file, so
 * every existing consumer of the structs keeps compiling, against ONE
 * definition.
 *
 * ─── The image contract §5.1 makes structural (not just documentary) ───────
 * ET_EXEC only. The mapper refuses an ET_DYN (PIE) image BY NAME: it maps
 * segments at their absolute p_vaddr and does NO load bias or relocation, so
 * a PIE image mapped here cannot run — the census tool refuses the candidate
 * artifact at the source for the same reason (measured: x86_64-unknown-
 * linux-musl produces ET_DYN by default; backup/aerobackup/.cargo/config.toml
 * forces relocation-model=static). Refusing at the one mapper turns §5.1's
 * rule into something no caller can forget.
 *
 * ─── What it guarantees ────────────────────────────────────────────────────
 *   - every PT_LOAD's pages are partition-charged frames
 *     (allocate_physical_ram_frame_for_partition — Phase 13's quota, the
 *     same hard ceiling every unbounded, per-tenant frame allocation passes);
 *   - every frame is ZERO-FILLED before content is copied, so p_memsz >
 *     p_filesz tails (BSS) read as zeros, never a recycled frame's bytes;
 *   - page flags come from p_flags (W → USER_PTE_WRITE, !X → NX);
 *   - phdr_vaddr is the mapped vaddr of the program-header table, for the
 *     §5.2 stack image's AT_PHDR — derived as p_vaddr + (e_phoff - p_offset)
 *     of the PT_LOAD that contains e_phoff, 0 when no segment carries the
 *     header (an image whose own phdrs are not mapped cannot be handed a
 *     truthful AT_PHDR; cap.c's Linux create refuses that case by name).
 *
 * Validation happens BEFORE the first frame is allocated, so a refused
 * image maps nothing (denial-before-side-effect). A frame-allocation
 * failure mid-loop returns 0 after logging — the frames already mapped
 * leak, matching every other create-time failure path in cap.c's image
 * step, which predates this file.
 *
 * Freestanding: no libc; the byte copies are local statics with the same
 * shapes loader.c's own ld_* helpers are.
 */
#include <stdint.h>

/* ─── Minimal ELF64 structures (moved from loader.h — one definition) ────── */
#define ELF_MAGIC0  0x7F
#define ELF_MAGIC1  'E'
#define ELF_MAGIC2  'L'
#define ELF_MAGIC3  'F'
#define ELF_CLASS64 2
#define PT_LOAD     1
#define PF_X        1       // segment execute permission
#define PF_W        2       // segment write permission
#define PF_R        4       // segment read permission

/* e_type values — ELF gABI: ET_EXEC is loadable-at-linked-addresses,
 * ET_DYN is PIE/DSO and needs a load bias this mapper does not do (§5.1). */
#define ELF_ET_EXEC 2
#define ELF_ET_DYN  3

struct ELF64Header {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;       // virtual address of entry point
    uint64_t e_phoff;       // program header table offset
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;       // number of program headers
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct ELF64ProgramHeader {
    uint32_t p_type;        // 1 = PT_LOAD
    uint32_t p_flags;       // PF_X | PF_W | PF_R
    uint64_t p_offset;      // offset of segment in binary
    uint64_t p_vaddr;       // target virtual address
    uint64_t p_paddr;
    uint64_t p_filesz;      // bytes to copy from binary
    uint64_t p_memsz;       // bytes to allocate (p_memsz - p_filesz = BSS)
    uint64_t p_align;
} __attribute__((packed));

/* What the caller needs to know about the mapped image. phent is
 * sizeof(struct ELF64ProgramHeader) for any image this mapper accepts;
 * phnum is e_phnum. */
struct Elf64MapInfo {
    uint64_t entry;       /* e_entry — an absolute vaddr (ET_EXEC, no bias) */
    uint64_t phdr_vaddr;  /* vaddr of the mapped program-header table, or 0 */
    uint16_t phnum;
    uint16_t phent;
};

/* The path-dispatch predicate: 1 iff the blob BEGINS with the four ELF
 * magic bytes. Checked BEFORE any validation, and only by the magic — a
 * real ELF that then fails the gates below (32-bit class, ET_DYN, bad
 * spans) is REFUSED by name, never silently flat-mapped instead; a flat
 * image whose machine code collided with the magic (2^-32 per image) is
 * refused loudly rather than mis-entered, which is the honest direction
 * to fail. The class check itself lives in elf64_map_blob, not here:
 * "has ELF magic" is this function's whole contract. Pure — the host
 * test drives it over headers, short blobs and non-ELF bytes. */
int elf_blob_has_magic(const uint8_t* data, uint64_t size);

/* Map every PT_LOAD of `data` (a flat, identity-mapped blob of `size`
 * bytes) into `pml4` at its linked p_vaddr, charging `partition_id`.
 * Returns 1 and fills *info on success; 0 (with a serial log naming the
 * reason) on any refusal: not ELF64, ET_DYN (§5.1), no program headers,
 * out-of-bounds phoff/phnum, or frame exhaustion mid-map. */
int elf64_map_blob(const uint8_t* data, uint64_t size, uint64_t* pml4,
                   uint32_t partition_id, struct Elf64MapInfo* info);

#endif /* ELF64_LOAD_H */
