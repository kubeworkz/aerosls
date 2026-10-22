/*
 * elf64_load.c — E7 §5.1/§5.2 (the plan's increment 7): the tree's ONE
 * ELF64 PT_LOAD mapper, extracted from loader.c so loader_load_into_process
 * and cap_create_sidecar_in_personality share one truth about segment
 * flags, BSS zero-fill, partition charging and the §5.1 image contract.
 *
 * The loop is loader.c's own, moved verbatim in behavior: per PT_LOAD,
 * per page — a partition-charged frame, zero-filled, file bytes copied in,
 * flags from p_flags (W → WRITE, !X → NX). What this extraction ADDS on
 * top of the original loop is validation BEFORE the first allocation
 * (denial-before-side-effect, the posture the rest of the tree applies):
 *
 *   - §5.1's ET_EXEC gate, refused by name — the original loop mapped an
 *     ET_DYN at its linked addresses with no load bias, which cannot run;
 *     the census tool refuses the same artifact at the source, and the
 *     one mapper is where the rule becomes structural;
 *   - every LOAD's file span inside the blob (a malformed artifact read
 *     past the end of the image into whatever followed it);
 *   - every LOAD's vaddr inside the user-canonical range, through
 *     linux_fs_addr_is_user_canonical — the SAME bound the §5.3 wrmsr and
 *     the write plan filter through, because a segment linked (or
 *     crafted) into the kernel half would make user_map_page walk the
 *     shared kernel entries the child's PML4 holds by pointer;
 *   - the phdr table's own bounds and e_phentsize, for AT_PHDR's truth.
 *
 * phdr_vaddr is derived from the PT_LOAD that CONTAINS the phdr table
 * (p_vaddr + (e_phoff - p_offset)); 0 when no segment carries it — an
 * image whose own phdrs are not mapped cannot be handed a truthful
 * AT_PHDR, and cap.c's Linux create refuses that combination by name
 * rather than emitting an auxv entry that walks into nowhere.
 */
#include "elf64_load.h"
#include "cap.h"             /* cap_arch_map_page / CAP_PERM_W|X — the arch
                              * hook the segment mapping routes through */
#include "linux_abi.h"       /* linux_fs_addr_is_user_canonical — ONE bound */
#include "kernel_io.h"        /* kernel_serial_print/printf */
#include "../arch/x86/user_paging.h"
#include "frame_pool.h"

static void e6_memset(void* p, uint8_t v, uint64_t n)
{
    uint8_t* b = (uint8_t*)p;
    uint64_t i;
    for (i = 0; i < n; i++) b[i] = v;
}

static void e6_memcpy(void* d, const void* s, uint64_t n)
{
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    uint64_t i;
    for (i = 0; i < n; i++) dd[i] = ss[i];
}

int elf_blob_has_magic(const uint8_t* data, uint64_t size)
{
    return data && size >= 4 &&
           data[0] == ELF_MAGIC0 && data[1] == ELF_MAGIC1 &&
           data[2] == ELF_MAGIC2 && data[3] == ELF_MAGIC3;
}

int elf64_map_blob(const uint8_t* data, uint64_t size, uint64_t* pml4,
                   uint32_t partition_id, struct Elf64MapInfo* info)
{
    const struct ELF64Header* ehdr;
    const struct ELF64ProgramHeader* phdr;
    uint64_t ph_bytes;
    uint64_t phdr_vaddr = 0;
    uint16_t ph;

    if (!data || !pml4 || !info) return 0;

    /* ── validation, every check, before the first frame ── */
    if (size < sizeof(struct ELF64Header)) {
        kernel_serial_print("[ELF] refuse: blob too small for an ELF64 header\n");
        return 0;
    }
    ehdr = (const struct ELF64Header*)(const void*)data;
    if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3 ||
        ehdr->e_ident[4] != ELF_CLASS64) {
        kernel_serial_print("[ELF] refuse: not an ELF64 image\n");
        return 0;
    }
    if (ehdr->e_type == ELF_ET_DYN) {
        kernel_serial_print(
            "[ELF] refuse: ET_DYN (PIE) — §5.1's image contract: this "
            "mapper applies no load bias, so a PIE image cannot run here "
            "(build non-PIE)\n");
        return 0;
    }
    if (ehdr->e_type != ELF_ET_EXEC) {
        kernel_serial_printf("[ELF] refuse: e_type %u is neither ET_EXEC "
                             "nor ET_DYN\n", (unsigned)ehdr->e_type);
        return 0;
    }
    if (ehdr->e_phnum == 0 || ehdr->e_phoff == 0) {
        kernel_serial_print("[ELF] refuse: no program headers\n");
        return 0;
    }
    if (ehdr->e_phentsize != sizeof(struct ELF64ProgramHeader)) {
        kernel_serial_printf("[ELF] refuse: e_phentsize %u != %u\n",
                             (unsigned)ehdr->e_phentsize,
                             (unsigned)sizeof(struct ELF64ProgramHeader));
        return 0;
    }
    ph_bytes = (uint64_t)ehdr->e_phnum * sizeof(struct ELF64ProgramHeader);
    if (ehdr->e_phoff + ph_bytes < ehdr->e_phoff ||
        ehdr->e_phoff + ph_bytes > size) {
        kernel_serial_print("[ELF] refuse: program-header table outside "
                            "the blob\n");
        return 0;
    }
    phdr = (const struct ELF64ProgramHeader*)(const void*)(data + ehdr->e_phoff);

    /* Every LOAD checked first; only then does anything get mapped. */
    for (ph = 0; ph < ehdr->e_phnum; ph++) {
        const struct ELF64ProgramHeader* s = &phdr[ph];
        if (s->p_type != PT_LOAD) continue;
        if (s->p_filesz == 0) continue;   /* loader.c's established rule:
                                           * a filesz-0 LOAD carries nothing
                                           * to copy (its BSS-less pages are
                                           * not part of this v1) */
        if (s->p_offset + s->p_filesz < s->p_offset ||
            s->p_offset + s->p_filesz > size) {
            kernel_serial_printf(
                "[ELF] refuse: LOAD %u file span leaves the blob\n",
                (unsigned)ph);
            return 0;
        }
        if (s->p_memsz < s->p_filesz) {
            kernel_serial_printf(
                "[ELF] refuse: LOAD %u memsz < filesz\n", (unsigned)ph);
            return 0;
        }
        /* User-canonical from BOTH ends: non-canonical/kernel vaddrs are
         * refused through the one bound every other user-address filter
         * in this tree uses (§5.3, the write plan). Overflow-safe: the
         * (end - 1) form, same as linux_fd_write_plan's span check. */
        if (s->p_vaddr + s->p_memsz < s->p_vaddr ||
            !linux_fs_addr_is_user_canonical(s->p_vaddr) ||
            (s->p_vaddr + s->p_memsz - 1) > LINUX_USER_ADDR_MAX) {
            kernel_serial_printf(
                "[ELF] refuse: LOAD %u vaddr 0x%llx..0x%llx is not user-"
                "canonical\n", (unsigned)ph,
                (unsigned long long)s->p_vaddr,
                (unsigned long long)(s->p_vaddr + s->p_memsz));
            return 0;
        }
        /* The §5.2 AT_PHDR anchor: the table must live inside a LOAD. */
        if (phdr_vaddr == 0 &&
            ehdr->e_phoff >= s->p_offset &&
            ehdr->e_phoff + ph_bytes <= s->p_offset + s->p_filesz) {
            phdr_vaddr = s->p_vaddr + (ehdr->e_phoff - s->p_offset);
        }
    }

    /* ── two LOADs must not share a PAGE ────────────────────────────────
     * The mapper installs ONE frame per page, so a page covered by two
     * segments would be mapped twice and the second write would silently
     * discard the first segment's bytes and its permissions. Aligned
     * segments cannot share one; a non-aligned LOAD's page reaches BACK
     * into the previous segment's tail, which is the case the alignment
     * fix above makes possible. Refused by name, before the first frame,
     * like every other gate above. */
    for (ph = 0; ph < ehdr->e_phnum; ph++) {
        const struct ELF64ProgramHeader* a = &phdr[ph];
        uint64_t a_lo, a_hi;
        uint16_t q;
        if (a->p_type != PT_LOAD) continue;
        if (a->p_filesz == 0) continue;
        a_lo = a->p_vaddr & ~(uint64_t)4095u;
        a_hi = (a->p_vaddr + a->p_memsz + 4095) & ~(uint64_t)4095u;
        for (q = (uint16_t)(ph + 1); q < ehdr->e_phnum; q++) {
            const struct ELF64ProgramHeader* b = &phdr[q];
            uint64_t b_lo, b_hi;
            if (b->p_type != PT_LOAD) continue;
            if (b->p_filesz == 0) continue;
            b_lo = b->p_vaddr & ~(uint64_t)4095u;
            b_hi = (b->p_vaddr + b->p_memsz + 4095) & ~(uint64_t)4095u;
            if (a_lo < b_hi && b_lo < a_hi) {
                kernel_serial_printf(
                    "[ELF] refuse: LOAD %u (vaddr 0x%llx) and LOAD %u "
                    "(vaddr 0x%llx) share a page — one frame per page "
                    "cannot hold both\n",
                    (unsigned)ph, (unsigned long long)a->p_vaddr,
                    (unsigned)q, (unsigned long long)b->p_vaddr);
                return 0;
            }
        }
    }

    /* ── map: the original loop, now only reachable past every check ── */
    for (ph = 0; ph < ehdr->e_phnum; ph++) {
        const struct ELF64ProgramHeader* s = &phdr[ph];
        uint64_t n_pages, p;

        if (s->p_type != PT_LOAD) continue;
        if (s->p_filesz == 0) continue;

        {
            /* A PT_LOAD's p_vaddr is NOT required to be page-aligned (the
             * firstuser's RW segment links at 0x8077370 — 0x370 into its
             * page), and a page's frame holds the bytes of the PAGE, not of
             * the segment's start. Copying from segment offset 0 while
             * mapping p_vaddr lands the whole segment (p_vaddr & 0xFFF)
             * bytes early in vaddr space, so every read inside it returns
             * the file that many bytes further on. Measured (§10.13's
             * step-0 diagnosis): .init_array read back 0x28 instead of
             * 0x800ae00, __libc_start_init's `call *(%rbx)` jumped to 0x28,
             * and the firstuser died at rip=0x28 before main. The page
             * BASE is the mapping anchor; the segment's offset inside its
             * first page is where its bytes start in that frame. For an
             * aligned segment both reduce to the arithmetic this loop
             * always did — which is why increment 7's census never saw
             * this, and why the aligned images' load is unchanged. */
            uint64_t page_base = s->p_vaddr & ~(uint64_t)4095u;
            uint64_t lead      = s->p_vaddr - page_base;

            n_pages = (lead + s->p_memsz + 4095) / 4096;

            /* The segment's PERMS ride cap_arch_map_page's cap_perms word
             * (W→CAP_PERM_W, X→CAP_PERM_X — the same encoding the MEM/DEV
             * grants use), NOT user_map_page(): a real static binary links
             * its segments at LOW vaddrs (0x400000), squarely inside the
             * child's SHARED kernel identity map (supervisor 2 MiB huge
             * pages, cloned by pointer). get_or_alloc() would treat the
             * huge-page PD entry as a table pointer and write the leaf
             * through the kernel's own frames — the child's translation
             * would still resolve through the huge page and its FIRST
             * instruction fetch would #PF (measured: increment 7's first
             * census boot faulted the candidate at rip=0x400000, error
             * 0x15, with the [LOADER] lines claiming R-X). The hook's
             * safe walk re-points the CHILD's own path, replicating the
             * shared huge page at 4 KiB so the kernel keeps visibility of
             * the rest — the same fix create_sidecar's MEM caps got. */
            uint32_t cap_perms = ((s->p_flags & PF_W) ? CAP_PERM_W : 0u)
                               | ((s->p_flags & PF_X) ? CAP_PERM_X : 0u);

            kernel_serial_printf(
                "[LOADER] ELF segment: vaddr=0x%016llx  filesz=%llu  "
                "memsz=%llu  flags=%c%c%c\n",
                (unsigned long long)s->p_vaddr,
                (unsigned long long)s->p_filesz,
                (unsigned long long)s->p_memsz,
                (s->p_flags & PF_R) ? 'R' : '-',
                (s->p_flags & PF_W) ? 'W' : '-',
                (s->p_flags & PF_X) ? 'X' : '-');
            if (lead != 0) {
                /* Visible, like every other load decision here: a segment
                 * that starts mid-page is the case this mapper used to get
                 * wrong, so a boot log now says which one and by how much. */
                kernel_serial_printf(
                    "[LOADER] segment vaddr 0x%llx is 0x%llx into its page; "
                    "frames anchored at 0x%llx\n",
                    (unsigned long long)s->p_vaddr,
                    (unsigned long long)lead,
                    (unsigned long long)page_base);
            }

            for (p = 0; p < n_pages; p++) {
                /* This page's vaddr, and where the SEGMENT starts inside
                 * it: a page whose vaddr is below the segment's start
                 * carries lead bytes of nothing before the segment's first
                 * byte (zeros — they belong to no segment), and every
                 * later page starts at the segment offset that vaddr
                 * implies. Both are 0 for an aligned segment. */
                uint64_t page_vaddr = page_base + p * 4096;
                uint64_t seg_off    = (page_vaddr > s->p_vaddr)
                                    ? (page_vaddr - s->p_vaddr) : 0;
                uint64_t frame_off  = (page_vaddr < s->p_vaddr)
                                    ? (s->p_vaddr - page_vaddr) : 0;
                void* frame =
                    allocate_physical_ram_frame_for_partition(partition_id);
                if (!frame) {
                    kernel_serial_print("[ELF] frame alloc failed mid-load\n");
                    return 0;
                }
                e6_memset(frame, 0, 4096);   /* zero (BSS) fill: the pool
                                              * recycles frames — an unzeroed
                                              * BSS would read a former
                                              * tenant's bytes */
                if (seg_off < s->p_filesz) {
                    uint64_t copy_len = s->p_filesz - seg_off;
                    if (copy_len > 4096 - frame_off) copy_len = 4096 - frame_off;
                    e6_memcpy((uint8_t*)frame + frame_off,
                              data + s->p_offset + seg_off, copy_len);
                }
                if (cap_arch_map_page((uint64_t)(uintptr_t)pml4,
                                      page_vaddr,
                                      (uint64_t)(uintptr_t)frame,
                                      cap_perms) != 0) {
                    kernel_serial_print("[ELF] arch map failed mid-load\n");
                    return 0;
                }
            }
        }
    }

    info->entry     = ehdr->e_entry;
    info->phdr_vaddr = phdr_vaddr;
    info->phnum     = ehdr->e_phnum;
    info->phent     = (uint16_t)sizeof(struct ELF64ProgramHeader);
    kernel_serial_printf("[LOADER] ELF64 loaded. Entry: 0x%016llx\n",
                         (unsigned long long)info->entry);
    return 1;
}
