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

use core::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlattenError {
    NotElf,
    Not64BitLe,
    NoLoadSegments,
    Truncated,
}

impl fmt::Display for FlattenError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            FlattenError::NotElf => write!(f, "not an ELF file"),
            FlattenError::Not64BitLe => write!(f, "not a 64-bit little-endian ELF"),
            FlattenError::NoLoadSegments => write!(f, "ELF has no PT_LOAD segments"),
            FlattenError::Truncated => write!(f, "ELF truncated"),
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

pub fn flatten_elf(elf: &[u8]) -> Result<Flattened, FlattenError> {
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

    Ok(Flattened {
        image,
        entry: entry - start,
    })
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
        let f = flatten_elf(&elf).unwrap();
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
        let f = flatten_elf(&elf).unwrap();
        assert_eq!(f.image.len(), 0x2000);
        assert_eq!(&f.image[0..2], &[0x90, 0xc3]);
        assert!(f.image[2..0x1000].iter().all(|&b| b == 0), "text BSS tail zero-filled");
        assert_eq!(&f.image[0x1000..0x1004], &[0x11, 0x22, 0x33, 0x44]);
        assert!(f.image[0x1004..0x2000].iter().all(|&b| b == 0), "data BSS tail zero-filled");
    }

    #[test]
    fn entry_offset_reported() {
        let elf = build_elf(&[(0x1000, &[0xc3], 1)]);
        let f = flatten_elf(&elf).unwrap();
        // e_entry is the first segment's vaddr in our builder, so the
        // flat offset of the entry is 0 (start = 0x1000).
        assert_eq!(f.entry, 0);
        assert_eq!(f.image, vec![0xc3]);
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(flatten_elf(&[]), Err(FlattenError::NotElf));
        assert_eq!(flatten_elf(&[0x7f, b'E', b'L', b'F']), Err(FlattenError::NotElf));
        let mut elf = build_elf(&[(0, &[0xc3], 1)]);
        elf[4] = 1; // ELFCLASS32
        assert_eq!(flatten_elf(&elf), Err(FlattenError::Not64BitLe));
        let elf = build_elf(&[]);
        assert_eq!(flatten_elf(&elf), Err(FlattenError::NoLoadSegments));
    }
}
