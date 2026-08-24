//! ELF64 → flat binary ("objcopy -O binary" in miniature).
//!
//! The kernel's `cap_create_sidecar` copies `image_size` bytes from the
//! image's declared physical address, starting at offset 0, and enters at
//! `image.entry` (0 for a flat binary) — so the sidecar image must be a
//! flat blob whose FIRST byte is the entry code, not an ELF. `cargo build
//! --target x86_64-unknown-none --bin init` produces an ELF (the only
//! format lld can emit), and this module converts it exactly the way
//! binutils' `objcopy -O binary` does:
//!
//! 1. walk the ELF64 program headers, collecting the `PT_LOAD` segments;
//! 2. emit every byte from the lowest segment's `p_vaddr` to the highest
//!    segment's `p_vaddr + p_memsz`, copying each segment's `p_filesz`
//!    file bytes at its `p_vaddr` and zero-filling the rest (BSS tails,
//!    alignment gaps, the ELF headers themselves — all become zeros).
//!
//! This crate is the host tool, so it has `std` and can read the ELF
//! directly. Only the 64-bit little-endian ELF shape lld emits for
//! `x86_64-unknown-none` is supported; anything else is a `FlattenError`.
//!
//! **Relocations are applied.** rust-lld emits `.rela.dyn` with
//! `R_X86_64_RELATIVE` entries even for a static freestanding binary (the
//! GOT/`.data.rel.ro` pointer slots it lays out for the final link), and
//! `objcopy -O binary` would apply them. A flat image with zeroed GOT
//! slots makes the sidecar's first indirect call jump to address 0 —
//! observed as a ring-3 instruction-fetch #PF at rip=0 under QEMU.
//!
//! The binaries are ET_DYN (static PIE), so every relocation carries the
//! runtime load bias B: `R_X86_64_RELATIVE` resolves to `B + addend` and
//! the symbol-using forms to `B + st_value + addend`. The kernel loads the
//! image at `USER_PROC_CODE_BASE` (0x4000_0000_0000, kernel/cap.c), so the
//! caller passes that as `load_vaddr` — with B = 0 the GOT would point
//! into the low address space and the first indirect call would fault at
//! a low unmapped address (also observed under QEMU).

use core::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlattenError {
    NotElf,
    Not64BitLe,
    NoLoadSegments,
    Truncated,
    UnsupportedReloc(u32),
}

impl fmt::Display for FlattenError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FlattenError::NotElf => write!(f, "not an ELF file"),
            FlattenError::Not64BitLe => write!(f, "not a 64-bit little-endian ELF"),
            FlattenError::NoLoadSegments => write!(f, "ELF has no PT_LOAD segments"),
            FlattenError::Truncated => write!(f, "ELF truncated"),
            FlattenError::UnsupportedReloc(t) => {
                write!(f, "unsupported ELF relocation type {t}")
            }
        }
    }
}

/// The result of flattening: the flat image plus the entry offset within
/// it (so the caller can cross-check the manifest's `image.entry`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Flattened {
    pub image: Vec<u8>,
    /// Entry point as an offset into `image` (ELF `e_entry` − lowest
    /// `PT_LOAD` vaddr). Must be 0 for the flat sidecar images.
    pub entry: u64,
}

pub fn flatten_elf(elf: &[u8], load_vaddr: u64) -> Result<Flattened, FlattenError> {
    // ELF64 header: magic(4) class(1) data(1) ... e_entry(24, 8)
    // e_phoff(32, 8) e_phentsize(54, 2) e_phnum(56, 2).
    if elf.len() < 64 || &elf[..4] != b"\x7fELF" {
        return Err(FlattenError::NotElf);
    }
    if elf[4] != 2 || elf[5] != 1 {
        return Err(FlattenError::Not64BitLe);
    }
    let phoff = le_u64(elf, 32) as usize;
    let phentsize = le_u16(elf, 54) as usize;
    let phnum = le_u16(elf, 56) as usize;
    let entry = le_u64(elf, 24);

    if phnum == 0 || phentsize < 56 || phoff == 0 {
        return Err(FlattenError::NoLoadSegments);
    }

    // Collect PT_LOAD segments. Program header (56 bytes): p_type(0,4)
    // p_flags(4,4) p_offset(8,8) p_vaddr(16,8) p_paddr(24,8)
    // p_filesz(32,8) p_memsz(40,8) p_align(48,8).
    let mut segs: Vec<(u64, u64, u64, u64)> = Vec::new(); // offset, vaddr, filesz, memsz
    for i in 0..phnum {
        let off = phoff + i * phentsize;
        if off + 56 > elf.len() {
            return Err(FlattenError::Truncated);
        }
        if le_u32(elf, off) != 1 {
            continue; // not PT_LOAD
        }
        let (p_offset, p_vaddr) = (le_u64(elf, off + 8), le_u64(elf, off + 16));
        let (p_filesz, p_memsz) = (le_u64(elf, off + 32), le_u64(elf, off + 40));
        segs.push((p_offset, p_vaddr, p_filesz, p_memsz));
    }
    if segs.is_empty() {
        return Err(FlattenError::NoLoadSegments);
    }

    let start = segs.iter().map(|s| s.1).min().unwrap();
    let end = segs
        .iter()
        .map(|s| s.1.saturating_add(s.3))
        .max()
        .unwrap();
    let size = (end - start) as usize;
    let mut image = vec![0u8; size];

    for (p_offset, p_vaddr, p_filesz, _p_memsz) in segs {
        let fsz = p_filesz as usize;
        if (p_offset as usize) + fsz > elf.len() {
            return Err(FlattenError::Truncated);
        }
        let dst = (p_vaddr - start) as usize;
        image[dst..dst + fsz].copy_from_slice(&elf[p_offset as usize..p_offset as usize + fsz]);
    }

    apply_relocations(elf, &mut image, start, load_vaddr)?;

    Ok(Flattened {
        image,
        entry: entry - start,
    })
}

/* ─── Relocation application ──────────────────────────────────────────────────
 * Section header table (64-byte entries): sh_name(0,4) sh_type(4,4)
 * sh_flags(8,8) sh_addr(16,8) sh_offset(24,8) sh_size(32,8) sh_link(40,4)
 * sh_info(44,4) sh_addralign(48,8) sh_entsize(56,8). e_shoff at ELF offset
 * 40, e_shentsize at 58, e_shnum at 60. SHT_SYMTAB = 2, SHT_RELA = 4.
 * Rela entry (24 bytes): r_offset(0,8) r_info(8,8) r_addend(16,8);
 * reloc type = r_info & 0xffffffff, symbol index = r_info >> 32.
 * Symtab entry (24 bytes): st_name(0,4) st_info(4,1) st_other(5,1)
 * st_shndx(6,2) st_value(8,8) st_size(16,8). */
fn apply_relocations(
    elf: &[u8],
    image: &mut [u8],
    start: u64,
    load_vaddr: u64,
) -> Result<(), FlattenError> {
    let shoff = le_u64(elf, 40) as usize;
    let shentsize = le_u16(elf, 58) as usize;
    let shnum = le_u16(elf, 60) as usize;
    if shoff == 0 || shnum == 0 {
        return Ok(()); // no section table — nothing to relocate
    }
    if shoff + shnum * shentsize > elf.len() {
        return Err(FlattenError::Truncated);
    }

    // Symbol table (needed for the symbol-using relocation forms).
    let mut symtab: Option<(usize, usize, usize)> = None; // offset, entsize, count
    let mut relas: Vec<(usize, usize)> = Vec::new();       // offset, count
    for i in 0..shnum {
        let sh = shoff + i * shentsize;
        let stype = le_u32(elf, sh + 4);
        match stype {
            2 => {
                let entsize = le_u64(elf, sh + 56) as usize;
                let size = le_u64(elf, sh + 32) as usize;
                symtab = Some((le_u64(elf, sh + 24) as usize, entsize, size / entsize.max(1)));
            }
            4 => {
                let entsize = le_u64(elf, sh + 56) as usize;
                if entsize != 0 && entsize != 24 {
                    return Err(FlattenError::UnsupportedReloc(0xFFFF));
                }
                let size = le_u64(elf, sh + 32) as usize;
                relas.push((le_u64(elf, sh + 24) as usize, size / 24));
            }
            _ => {}
        }
    }

    let sym_value = |symtab: Option<(usize, usize, usize)>, idx: usize| -> Result<u64, FlattenError> {
        let (off, entsize, count) = symtab.ok_or(FlattenError::UnsupportedReloc(0xFFFF))?;
        if idx >= count || off + (idx + 1) * entsize > elf.len() {
            return Err(FlattenError::Truncated);
        }
        Ok(le_u64(elf, off + idx * entsize + 8))
    };

    for (rela_off, count) in relas {
        if rela_off + count * 24 > elf.len() {
            return Err(FlattenError::Truncated);
        }
        for i in 0..count {
            let r = rela_off + i * 24;
            let r_offset = le_u64(elf, r);
            let r_info = le_u64(elf, r + 8);
            let r_addend = le_u64(elf, r + 16); // addend is signed; two's complement carries
            let rtype = (r_info & 0xFFFF_FFFF) as u32;
            let symidx = (r_info >> 32) as usize;
            // ET_DYN (static PIE): the runtime value of every symbol is its
            // link-time offset + the load bias. RELATIVE has no symbol:
            // B + addend. The symbol-using forms: B + st_value + addend.
            let value = match rtype {
                8 => load_vaddr.wrapping_add(r_addend),
                1 | 6 | 7 => {
                    load_vaddr.wrapping_add(sym_value(symtab, symidx)?).wrapping_add(r_addend)
                }
                other => return Err(FlattenError::UnsupportedReloc(other)),
            };
            if r_offset < start || r_offset + 8 > start + image.len() as u64 {
                return Err(FlattenError::Truncated); // relocated slot outside the image
            }
            let dst = (r_offset - start) as usize;
            image[dst..dst + 8].copy_from_slice(&value.to_le_bytes());
        }
    }
    Ok(())
}

fn le_u16(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}

fn le_u32(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

fn le_u64(b: &[u8], i: usize) -> u64 {
    let mut m = [0u8; 8];
    m.copy_from_slice(&b[i..i + 8]);
    u64::from_le_bytes(m)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a minimal ELF64 with the given PT_LOAD segments. Each segment
    /// is (vaddr, bytes, memsz) — the file bytes are laid out consecutively
    /// after a 64-byte header + a 56-byte program header.
    fn build_elf(segs: &[(u64, &[u8], u64)]) -> Vec<u8> {
        let mut e = vec![0u8; 64 + 56 * segs.len()];
        e[..4].copy_from_slice(b"\x7fELF");
        e[4] = 2; // ELFCLASS64
        e[5] = 1; // ELFDATA2LSB
        e[6] = 1; // EV_CURRENT
        e[16] = 2; // e_type = ET_EXEC
        let entry = segs.first().map(|s| s.0).unwrap_or(0);
        e[24..32].copy_from_slice(&entry.to_le_bytes());
        e[32..40].copy_from_slice(&64u64.to_le_bytes()); // e_phoff
        e[54..56].copy_from_slice(&56u16.to_le_bytes()); // e_phentsize
        e[56..58].copy_from_slice(&(segs.len() as u16).to_le_bytes()); // e_phnum

        let mut file_off = e.len();
        let mut out = e;
        for (i, (vaddr, bytes, memsz)) in segs.iter().enumerate() {
            let ph = 64 + i * 56;
            out[ph..ph + 4].copy_from_slice(&1u32.to_le_bytes()); // PT_LOAD
            out[ph + 8..ph + 16].copy_from_slice(&(file_off as u64).to_le_bytes());
            out[ph + 16..ph + 24].copy_from_slice(&vaddr.to_le_bytes());
            out[ph + 24..ph + 32].copy_from_slice(&vaddr.to_le_bytes()); // p_paddr
            out[ph + 32..ph + 40].copy_from_slice(&(bytes.len() as u64).to_le_bytes());
            out[ph + 40..ph + 48].copy_from_slice(&memsz.to_le_bytes());
            out[ph + 48..ph + 56].copy_from_slice(&4096u64.to_le_bytes()); // p_align
            out.extend_from_slice(bytes);
            file_off += bytes.len();
        }
        out
    }

    #[test]
    fn flattens_single_segment() {
        let elf = build_elf(&[(0x0, &[0x90, 0xc3, 0x48, 0x89], 8)]);
        let f = flatten_elf(&elf, 0x4000_0000_0000).unwrap();
        assert_eq!(f.entry, 0);
        assert_eq!(f.image, vec![0x90, 0xc3, 0x48, 0x89, 0, 0, 0, 0]);
    }

    #[test]
    fn zero_fills_bss_and_gaps() {
        // Two segments: code at 0, then data at 0x1000 with memsz 0x1000
        // but filesz 4 (a 4-byte value + 0xffc bytes of BSS tail).
        let elf = build_elf(&[
            (0x0, &[0x90, 0xc3], 0x1000),                       // text, BSS tail
            (0x1000, &[0x11, 0x22, 0x33, 0x44], 0x1000),        // data + BSS
        ]);
        let f = flatten_elf(&elf, 0x4000_0000_0000).unwrap();
        assert_eq!(f.image.len(), 0x2000);
        assert_eq!(&f.image[0..2], &[0x90, 0xc3]);
        assert!(f.image[2..0x1000].iter().all(|&b| b == 0), "text BSS tail zero-filled");
        assert_eq!(&f.image[0x1000..0x1004], &[0x11, 0x22, 0x33, 0x44]);
        assert!(f.image[0x1004..0x2000].iter().all(|&b| b == 0), "data BSS tail zero-filled");
    }

    #[test]
    fn entry_offset_reported() {
        let elf = build_elf(&[(0x1000, &[0xc3], 1)]);
        let f = flatten_elf(&elf, 0x4000_0000_0000).unwrap();
        // e_entry is the first segment's vaddr in our builder, so the
        // flat offset of the entry is 0 (start = 0x1000).
        assert_eq!(f.entry, 0);
        assert_eq!(f.image, vec![0xc3]);
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(flatten_elf(&[], 0), Err(FlattenError::NotElf));
        assert_eq!(flatten_elf(&[0x7f, b'E', b'L', b'F'], 0), Err(FlattenError::NotElf));
        let mut elf = build_elf(&[(0, &[0xc3], 1)]);
        elf[4] = 1; // ELFCLASS32
        assert_eq!(flatten_elf(&elf, 0), Err(FlattenError::Not64BitLe));
        let elf = build_elf(&[]);
        assert_eq!(flatten_elf(&elf, 0), Err(FlattenError::NoLoadSegments));
    }

    /// Append a section header table + a `.rela.dyn` with `n` RELATIVE
    /// entries to a segment-built ELF, and point e_shoff/e_shnum at it.
    /// Each entry is (r_offset, addend); RELATIVE (type 8) needs no symtab.
    fn add_rela_dyn(mut elf: Vec<u8>, entries: &[(u64, u64)]) -> Vec<u8> {
        let rela_off = elf.len();
        for (off, add) in entries {
            let info = 8u64; // type 8 = R_X86_64_RELATIVE, symbol 0
            elf.extend_from_slice(&off.to_le_bytes());
            elf.extend_from_slice(&info.to_le_bytes());
            elf.extend_from_slice(&add.to_le_bytes());
        }
        // Section header table: null section + one SHT_RELA section.
        let shoff = elf.len();
        elf.extend_from_slice(&[0u8; 64]); // null section
        let mut sh = vec![0u8; 64];
        sh[4..8].copy_from_slice(&4u32.to_le_bytes()); // sh_type = SHT_RELA
        sh[24..32].copy_from_slice(&(rela_off as u64).to_le_bytes());
        sh[32..40].copy_from_slice(&((entries.len() * 24) as u64).to_le_bytes());
        sh[40..44].copy_from_slice(&0u32.to_le_bytes()); // sh_link: no symtab
        sh[44..48].copy_from_slice(&1u32.to_le_bytes()); // sh_info: target sect
        sh[56..64].copy_from_slice(&24u64.to_le_bytes()); // sh_entsize
        elf.extend_from_slice(&sh);
        elf[40..48].copy_from_slice(&(shoff as u64).to_le_bytes()); // e_shoff
        elf[58..60].copy_from_slice(&64u16.to_le_bytes()); // e_shentsize
        elf[60..62].copy_from_slice(&2u16.to_le_bytes()); // e_shnum
        elf
    }

    #[test]
    fn applies_relative_relocations() {
        // A data segment at vaddr 0x1000 holding one zeroed u64 pointer
        // slot; .rela.dyn patches it with the addend (base = 0).
        let elf = build_elf(&[(0x1000, &[0u8; 8], 8)]);
        let elf = add_rela_dyn(elf, &[(0x1000, 0xDEAD_BEEF)]);
        let f = flatten_elf(&elf, 0x4000_0000_0000).unwrap();
        assert_eq!(f.image.len(), 8);
        assert_eq!(
            u64::from_le_bytes(f.image[0..8].try_into().unwrap()),
            0x4000_0000_0000 + 0xDEAD_BEEF,
            "RELATIVE relocation = load bias + addend (ET_DYN static PIE)"
        );
    }

    #[test]
    fn applies_multiple_relocations_across_gap() {
        // Two slots in a 0x2000 image (one inside the segment, one in the
        // zero-filled BSS tail) both get patched.
        let elf = build_elf(&[(0x1000, &[0u8; 4], 0x2000)]);
        let elf = add_rela_dyn(elf, &[(0x1000, 0x1111), (0x1FF8, 0x2222)]);
        let f = flatten_elf(&elf, 0x4000_0000_0000).unwrap();
        assert_eq!(f.image.len(), 0x2000);
        assert_eq!(
            u64::from_le_bytes(f.image[0..8].try_into().unwrap()),
            0x4000_0000_0000 + 0x1111
        );
        assert_eq!(
            u64::from_le_bytes(f.image[0xFF8..0x1000].try_into().unwrap()),
            0x4000_0000_0000 + 0x2222
        );
    }

    #[test]
    fn rejects_unsupported_reloc_type() {
        // GLOB_DAT (6) with no symtab must fail loudly, not silently
        // produce a wrong image.
        let elf = build_elf(&[(0x1000, &[0u8; 8], 8)]);
        let mut e = add_rela_dyn(elf, &[(0x1000, 0x1234)]);
        // Rewrite the single entry's r_info to type 6 (GLOB_DAT).
        let shoff = u64::from_le_bytes(e[40..48].try_into().unwrap()) as usize;
        let rela_off = u64::from_le_bytes(e[shoff + 64 + 24..shoff + 64 + 32].try_into().unwrap()) as usize;
        e[rela_off + 8..rela_off + 16].copy_from_slice(&6u64.to_le_bytes());
        assert_eq!(
            flatten_elf(&e, 0x4000_0000_0000),
            Err(FlattenError::UnsupportedReloc(0xFFFF)),
            "symbol-using reloc without a symtab is refused (0xFFFF = no symtab)"
        );
    }
}
