//! Boot Info Block parsing (Phase 2 §6.1).
//!
//! The kernel fills a BIB at `create_sidecar` time and jumps to `_start` with
//! a pointer to it. Layout (all little-endian):
//!
//! ```text
//! offset  size  field
//! 0       8     magic         "AERSLSB1"
//! 8       2     version       = 1
//! 10      2     cap_count
//! 12      8     budget_bytes
//! 20      8     stack_top
//! 28      4     total_len     (whole BIB, for bounds checking)
//! 32      n     caps[]        { name_len u16, name UTF-8,
//!                               slot u16, ty u8, rights u8,
//!                               base u64, len u64 }
//! ```
//!
//! `total_len` is a small extension over the Phase 2 doc's illustrative
//! layout: without it, a malformed `name_len` (u16, unbounded) could make the
//! parser read past the kernel's allocation. With it, every read is checked.
//! The BIB stays alive for the sidecar's lifetime; names are borrowed from it
//! (`&'a str`).

use core::str;

pub const BOOT_INFO_MAGIC: [u8; 8] = *b"AERSLSB1";
pub const BOOT_INFO_VERSION: u16 = 1;
pub const MAX_BOOT_CAPS: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BootErr {
    BadMagic,
    BadVersion,
    TooShort,
    BadUtf8,
    TooManyCaps,
    Truncated,
}

impl core::fmt::Display for BootErr {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            BootErr::BadMagic => write!(f, "bad BIB magic"),
            BootErr::BadVersion => write!(f, "bad BIB version"),
            BootErr::TooShort => write!(f, "BIB too short for header"),
            BootErr::BadUtf8 => write!(f, "BIB cap name not UTF-8"),
            BootErr::TooManyCaps => write!(f, "BIB has too many caps"),
            BootErr::Truncated => write!(f, "BIB truncated inside cap table"),
        }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct BootCap<'a> {
    pub name: &'a str,
    pub slot: u32,
    pub ty: u16,
    pub rights: u16,
    pub base: u64,
    pub len: u64,
}

/// Owns a fixed array of parsed caps (no allocation).
/// `#[repr(C)]` pins the layout so the in-kernel BIB writer (cap.c)
/// and the Rust parser agree on field offsets — especially `n_caps`
/// at offset 0x18 and `caps` at offset 0x20.
#[repr(C)]
#[derive(Debug)]
pub struct BootInfo<'a> {
    pub version: u16,
    pub budget_bytes: u64,
    pub stack_top: u64,
    pub n_caps: usize,
    pub caps: [BootCap<'a>; MAX_BOOT_CAPS],
}

const HEADER_LEN: usize = 32;
/// Fixed cap-entry bytes after the name: slot/ty/rights (4) + base/len (16).
/// (Name length is variable.) Used by the test's BIB builder.
#[cfg(test)]
const CAP_FIXED: usize = 2 + 4 + 16;

impl<'a> BootInfo<'a> {
    /// Parse the BIB at `p`. Unsafe only because the kernel handed us a raw
    /// pointer; every byte read is bounds-checked against `total_len` after
    /// the magic check, so a malformed BIB fails here instead of faulting.
    pub unsafe fn from_raw(p: *const u8) -> Result<BootInfo<'a>, BootErr> {
        if p.is_null() {
            return Err(BootErr::TooShort);
        }
        if read8(p, 0) != BOOT_INFO_MAGIC {
            return Err(BootErr::BadMagic);
        }
        let version = le_u16(p, 8);
        if version != BOOT_INFO_VERSION {
            return Err(BootErr::BadVersion);
        }
        let cap_count = le_u16(p, 10) as usize;
        if cap_count > MAX_BOOT_CAPS {
            return Err(BootErr::TooManyCaps);
        }
        let budget_bytes = le_u64(p, 12);
        let stack_top = le_u64(p, 20);
        let total_len = le_u32(p, 28) as usize;
        if total_len < HEADER_LEN {
            return Err(BootErr::TooShort);
        }

        let mut caps = [BootCap {
            name: "",
            slot: 0,
            ty: 0,
            rights: 0,
            base: 0,
            len: 0,
        }; MAX_BOOT_CAPS];

        let mut off: usize = HEADER_LEN;
        for i in 0..cap_count {
            // name
            if off + 2 > total_len {
                return Err(BootErr::Truncated);
            }
            let name_len = le_u16(p, off) as usize;
            off += 2;
            if off + name_len > total_len {
                return Err(BootErr::Truncated);
            }
            let name_bytes = core::slice::from_raw_parts(p.add(off), name_len);
            let name = str::from_utf8(name_bytes).map_err(|_| BootErr::BadUtf8)?;
            off += name_len;

            // slot, ty, rights
            if off + 4 > total_len {
                return Err(BootErr::Truncated);
            }
            let slot = le_u16(p, off) as u32;
            let ty = read_byte(p, off + 2) as u16;
            let rights = read_byte(p, off + 3) as u16;
            off += 4;

            // base, len
            if off + 16 > total_len {
                return Err(BootErr::Truncated);
            }
            let base = le_u64(p, off);
            let len = le_u64(p, off + 8);
            off += 16;

            caps[i] = BootCap {
                name,
                slot,
                ty,
                rights,
                base,
                len,
            };
        }

        Ok(BootInfo {
            version,
            budget_bytes,
            stack_top,
            n_caps: cap_count,
            caps,
        })
    }

    pub fn find_cap(&self, ty: u16, name: &str) -> Option<&BootCap<'a>> {
        self.caps[..self.n_caps]
            .iter()
            .find(|c| c.ty == ty && c.name == name)
    }

    pub fn caps(&self) -> &[BootCap<'a>] {
        &self.caps[..self.n_caps]
    }
}

unsafe fn read8(p: *const u8, i: usize) -> [u8; 8] {
    let mut m = [0u8; 8];
    unsafe { core::ptr::copy_nonoverlapping(p.add(i), m.as_mut_ptr(), 8) }
    m
}

unsafe fn read_byte(p: *const u8, i: usize) -> u8 {
    unsafe { *p.add(i) }
}

unsafe fn le_u16(p: *const u8, i: usize) -> u16 {
    u16::from_le_bytes([unsafe { read_byte(p, i) }, unsafe { read_byte(p, i + 1) }])
}

unsafe fn le_u32(p: *const u8, i: usize) -> u32 {
    u32::from_le_bytes([
        unsafe { read_byte(p, i) },
        unsafe { read_byte(p, i + 1) },
        unsafe { read_byte(p, i + 2) },
        unsafe { read_byte(p, i + 3) },
    ])
}

unsafe fn le_u64(p: *const u8, i: usize) -> u64 {
    let mut m = [0u8; 8];
    unsafe { core::ptr::copy_nonoverlapping(p.add(i), m.as_mut_ptr(), 8) }
    u64::from_le_bytes(m)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    fn build_bib(caps: &[(&str, u16, u16, u64, u64)]) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(&BOOT_INFO_MAGIC);
        b.extend_from_slice(&BOOT_INFO_VERSION.to_le_bytes());
        b.extend_from_slice(&(caps.len() as u16).to_le_bytes());
        b.extend_from_slice(&(1u64 << 20).to_le_bytes()); // budget_bytes
        b.extend_from_slice(&0x2000_0000u64.to_le_bytes()); // stack_top
        let total = HEADER_LEN
            + caps
                .iter()
                .map(|(n, _, _, _, _)| 2 + n.len() + CAP_FIXED)
                .sum::<usize>();
        b.extend_from_slice(&(total as u32).to_le_bytes());
        for (name, ty, rights, base, len) in caps {
            b.extend_from_slice(&(name.len() as u16).to_le_bytes());
            b.extend_from_slice(name.as_bytes());
            b.extend_from_slice(&0u16.to_le_bytes()); // slot placeholder
            b.push(*ty as u8);
            b.push(*rights as u8);
            b.extend_from_slice(&base.to_le_bytes());
            b.extend_from_slice(&len.to_le_bytes());
        }
        b
    }

    #[test]
    fn parse_ok_and_find() {
        let bib = build_bib(&[
            ("budget", 1, 0x3, 0x1000_0000, 0x10000),
            ("storage", 1, 0x1, 0x2000_0000, 0x200_0000),
            ("console", 2, 0x7, 0, 0),
        ]);
        let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
        assert_eq!(info.n_caps, 3);
        assert_eq!(info.budget_bytes, 1 << 20);
        let storage = info.find_cap(1, "storage").unwrap();
        assert_eq!(storage.base, 0x2000_0000);
        assert_eq!(storage.len, 0x200_0000);
        assert_eq!(storage.rights, 0x1);
        assert!(info.find_cap(1, "nope").is_none());
        assert!(info.find_cap(2, "console").is_some());
    }

    #[test]
    fn parse_rejects_garbage() {
        assert!(unsafe { BootInfo::from_raw(core::ptr::null()) }.is_err());
        let mut bad = build_bib(&[]);
        bad[0] = b'X';
        assert_eq!(
            unsafe { BootInfo::from_raw(bad.as_ptr()) }.unwrap_err(),
            BootErr::BadMagic
        );
    }

    #[test]
    fn parse_rejects_truncated() {
        let bib = build_bib(&[("storage", 1, 0x1, 0x2000_0000, 0x200_0000)]);
        // Chop the cap table off while keeping magic/version valid, and make
        // total_len agree with the truncation.
        let mut v = bib[..HEADER_LEN + 2].to_vec();
        let truncated_len = v.len() as u32;
        v[28..32].copy_from_slice(&truncated_len.to_le_bytes());
        assert_eq!(
            unsafe { BootInfo::from_raw(v.as_ptr()) }.unwrap_err(),
            BootErr::Truncated
        );
    }

    #[test]
    fn network_sidecar_bib_layout() {
        // The network sidecar's BIB (kernel/cap.c cap_create_sidecar) has:
        // Cap 0: messenger CHAN_R (unnamed, slot 6)
        // Cap 1: messenger CHAN_W (unnamed, slot 7)
        // Cap 2: budget MEM (named "budget", slot 8)
        // Cap 3: console CHAN_R (named "console", slot 9)
        // Cap 4: console CHAN_W (named "console", slot 10)
        let bib = build_bib(&[
            ("", 2, 0x2, 0, 0),       // CHAN_R messenger
            ("", 3, 0x4, 0, 0),       // CHAN_W messenger
            ("budget", 1, 0x7, 0x2200_0000, 0x100_0000), // MEM budget
            ("console", 2, 0x2, 0, 0), // CHAN_R console
            ("console", 3, 0x4, 0, 0), // CHAN_W console
        ]);
        let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
        assert_eq!(info.n_caps, 5);
        assert_eq!(info.budget_bytes, 1 << 20);

        // Verify struct layout: n_caps at offset 0x18, caps at offset 0x20
        assert_eq!(core::mem::offset_of!(BootInfo, n_caps), 0x18);
        assert_eq!(core::mem::offset_of!(BootInfo, caps), 0x20);

        // Find budget MEM cap
        let budget = info.find_cap(1, "budget").unwrap();
        assert_eq!(budget.base, 0x2200_0000);
        assert_eq!(budget.len, 0x100_0000);
        assert_eq!(budget.rights, 0x7);

        // Find console CHAN caps
        let console_r = info.find_cap(2, "console").unwrap();
        assert_eq!(console_r.rights, 0x2);
        let console_w = info.find_cap(3, "console").unwrap();
        assert_eq!(console_w.rights, 0x4);

        // Messenger caps are unnamed — can only find by type
        let messenger_r = &info.caps()[0];
        assert_eq!(messenger_r.ty, 2);
        assert_eq!(messenger_r.name, "");
        let messenger_w = &info.caps()[1];
        assert_eq!(messenger_w.ty, 3);
        assert_eq!(messenger_w.name, "");
    }
}
