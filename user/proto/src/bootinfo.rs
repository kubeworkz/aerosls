//! Boot Info Block parsing (Phase 2 §6.1).
//!
//! The kernel fills a BIB at `create_sidecar` time and jumps to `_start` with
//! a pointer to it. Layout (all little-endian):
//!
//! ```text
//! offset  size  field
//! 0       8     magic         "AERSLSB1"
//! 8       2     version       = 3
//! 10      2     cap_count
//! 12      8     budget_bytes
//! 20      8     stack_top
//! 28      4     total_len     (whole BIB, for bounds checking)
//! 32      4     flags         (BOOT_INFO_FLAG_*; 0 on a non-unified boot)
//! 36      4     own_pid       (this sidecar's pid; 0 = no identity)
//! 40      4     own_index     (its environment index; 0 = not an environment)
//! 44      4     reserved      (= 0; keeps caps[] 8-byte aligned)
//! 48      n     caps[]        { name_len u16, name UTF-8,
//!                               slot u16, ty u8, rights u8,
//!                               base u64, len u64 }
//! ```
//!
//! `flags` is version 2's addition (POSIX-Environments E1): the boot context
//! a sidecar cannot otherwise observe — today, whether the kernel is running
//! the UNIFIED boot, where the kernel owns the NICs and the console and the
//! sidecar world shares the CPU with the Ring-0 control plane instead of
//! being the boot (`unified=1`; see the POSIX Environments roadmap §4). The
//! version was bumped with it rather than appending the field silently, and
//! both sides (kernel/cap.c writes, this parser reads) ship in the same boot
//! image, so no version 1 BIB is ever parsed by a version 2 reader.
//!
//! version 3 (POSIX-Environments E6) adds `own_pid`/`own_index` and the
//! `ENV_CONSOLE` flag: the sidecar's OWN identity, which it cannot otherwise
//! observe — the cap table below names capabilities ("console", "budget"),
//! never the sidecar, so `aerosls.posix.2` was a name only the kernel knew. A
//! sidecar whose console is an environment console announces the pair on that
//! console at boot (user/sidecar/src/boot.rs), which is E6's in-band
//! self-report: the attach surface's last gap is a *consistent* crossing of the
//! two consoles' addressing, where every interaction through an address still
//! looks self-consistent, so only something the environment emits about itself
//! can expose it. It was version 2's `reserved` word plus one new word, and the
//! version was bumped with it rather than reusing the word silently.
//!
//! `total_len` is a small extension over the Phase 2 doc's illustrative
//! layout: without it, a malformed `name_len` (u16, unbounded) could make the
//! parser read past the kernel's allocation. With it, every read is checked.
//! The BIB stays alive for the sidecar's lifetime; names are borrowed from it
//! (`&'a str`).

use core::str;

pub const BOOT_INFO_MAGIC: [u8; 8] = *b"AERSLSB1";
pub const BOOT_INFO_VERSION: u16 = 3;
pub const MAX_BOOT_CAPS: usize = 16;

/// POSIX-Environments E1: this sidecar was created by the UNIFIED boot. The
/// kernel owns the NICs and the serial console there, and shares the CPU with
/// the Ring-0 control plane (kernel/process.c's kernel_yield_to_ring3), so a
/// sidecar must not assume the Phase-5 posture — where the sidecar world *is*
/// the boot and owns the hardware capabilities.
pub const BOOT_INFO_FLAG_UNIFIED: u32 = 1 << 0;

/// POSIX-Environments E6: this sidecar's console is an ENVIRONMENT console —
/// the kernel registered it with the per-environment console registry, so an
/// attach surface serves it. It is the precondition of `own_index`/`own_pid`
/// (see `identity()`): the two identity words are filled in for every sidecar
/// that has a name, but only a sidecar with an environment console announces
/// them, because an announcement is a claim about a console somebody can attach
/// to. Without the flag, "environment 0" (the system POSIX sidecar
/// `aerosls.posix.0`, whose console is the kernel transcript) and "not an
/// environment at all" (`aerosls.init.0`, `drv.ramdisk.0`) would be the same
/// statement.
pub const BOOT_INFO_FLAG_ENV_CONSOLE: u32 = 1 << 1;

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
    /// Wire-format flags (BOOT_INFO_FLAG_*). Placed here, inside what used to
    /// be the u16-to-u64 padding, so the parsed struct's layout — n_caps at
    /// 0x18, caps at 0x20, asserted by the layout test below — is unchanged.
    pub flags: u32,
    pub budget_bytes: u64,
    pub stack_top: u64,
    pub n_caps: usize,
    pub caps: [BootCap<'a>; MAX_BOOT_CAPS],
    /// v3 (E6): the pid the kernel gave THIS sidecar. 0 on a BIB that carried
    /// no identity (the host sim's `BootCaps::new`). It is filled in for every
    /// named sidecar; whether it is ANNOUNCED is `identity()`'s business (the
    /// pair with `BOOT_INFO_FLAG_ENV_CONSOLE`).
    pub own_pid: u32,
    /// v3 (E6): this sidecar's environment index, parsed by the kernel out of
    /// its own registry name (`aerosls.posix.<index>`) with the SAME function
    /// the per-environment console registry uses. It is 0 both for a sidecar
    /// whose name is not an environment's name and for the system POSIX sidecar
    /// `aerosls.posix.0` — an index, not a flag: "environment 0" is a real
    /// environment (the E3 boot spawn). What separates those two cases is
    /// `BOOT_INFO_FLAG_ENV_CONSOLE`, which is why the announcement is keyed on
    /// that flag rather than on this index being nonzero.
    pub own_index: u32,
}

/// v3 header: 40 bytes through `flags`, then own_pid + own_index + reserved.
const HEADER_LEN: usize = 48;
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
        let flags = le_u32(p, 32);
        let own_pid = le_u32(p, 36);
        let own_index = le_u32(p, 40);
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
            flags,
            budget_bytes,
            stack_top,
            n_caps: cap_count,
            caps,
            own_pid,
            own_index,
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

    /// POSIX-Environments E1: was this sidecar created by the unified boot
    /// (kernel owns the NICs and the console; the Ring-0 control plane and the
    /// sidecars share the CPU)? False on every other boot, including every
    /// Phase-5 boot, so a sidecar that does not care behaves as before.
    pub fn is_unified(&self) -> bool {
        self.flags & BOOT_INFO_FLAG_UNIFIED != 0
    }

    /// POSIX-Environments E6: is this sidecar's console an ENVIRONMENT console
    /// (one the attach surface serves)? See `BOOT_INFO_FLAG_ENV_CONSOLE`.
    pub fn is_env_console(&self) -> bool {
        self.flags & BOOT_INFO_FLAG_ENV_CONSOLE != 0
    }

    /// POSIX-Environments E6: this sidecar's own identity `(index, pid)` — the
    /// pair it announces on its own console at boot — or `None` when it has no
    /// identity to announce: `own_pid` 0 (a pre-v3 BIB or the host sim's
    /// constructor) or no environment console (see
    /// `BOOT_INFO_FLAG_ENV_CONSOLE`). This is the one thing a sidecar can say
    /// about itself that the kernel did not learn from what the sidecar did: it
    /// is handed to the sidecar at boot, so announcing it is a claim about the
    /// environment, not a read-back of the console the claim travels on.
    pub fn identity(&self) -> Option<(u32, u32)> {
        if self.own_pid == 0 || !self.is_env_console() {
            None
        } else {
            Some((self.own_index, self.own_pid))
        }
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
        build_bib_all(caps, 0, 0, 0)
    }

    fn build_bib_flags(caps: &[(&str, u16, u16, u64, u64)], flags: u32) -> Vec<u8> {
        build_bib_all(caps, flags, 0, 0)
    }

    fn build_bib_all(
        caps: &[(&str, u16, u16, u64, u64)],
        flags: u32,
        own_pid: u32,
        own_index: u32,
    ) -> Vec<u8> {
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
        b.extend_from_slice(&flags.to_le_bytes()); // v2: boot-context flags
        b.extend_from_slice(&own_pid.to_le_bytes()); // v3: own pid
        b.extend_from_slice(&own_index.to_le_bytes()); // v3: own env index
        b.extend_from_slice(&0u32.to_le_bytes()); // v3: reserved
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
    fn unified_flag() {
        // E1: the flag is what tells a sidecar it shares a boot with the Ring-0
        // control plane. A non-unified BIB must read false — every Phase-5
        // sidecar's behaviour depends on that default.
        let plain = build_bib(&[("console", 2, 0x7, 0, 0)]);
        let plain = unsafe { BootInfo::from_raw(plain.as_ptr()) }.unwrap();
        assert_eq!(plain.flags, 0);
        assert!(!plain.is_unified());

        let uni = build_bib_flags(&[("console", 2, 0x7, 0, 0)], BOOT_INFO_FLAG_UNIFIED);
        let uni = unsafe { BootInfo::from_raw(uni.as_ptr()) }.unwrap();
        assert_eq!(uni.flags, BOOT_INFO_FLAG_UNIFIED);
        assert!(uni.is_unified());
        // ...and the caps after the new header fields still parse.
        assert_eq!(uni.find_cap(2, "console").unwrap().rights, 0x7);
    }

    #[test]
    fn identity_survives_the_round_trip() {
        // E6: the sidecar's own index/pid ride in the header, so a sidecar can
        // announce them without asking the kernel anything — the property the
        // attach surface's last gap needs (see the module docs).
        let bib = build_bib_all(
            &[("console", 2, 0x7, 0, 0)],
            BOOT_INFO_FLAG_ENV_CONSOLE,
            13,
            2,
        );
        let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
        assert_eq!(info.own_pid, 13);
        assert_eq!(info.own_index, 2);
        assert!(info.is_env_console());
        assert_eq!(info.identity(), Some((2, 13)));
        // ...and the caps still parse after the wider header.
        assert_eq!(info.n_caps, 1);
        assert_eq!(info.find_cap(2, "console").unwrap().rights, 0x7);

        // A BIB that named no identity says so, rather than inventing pid 0
        // as an identity (index 0 alone is a real environment — `aerosls.posix.0`).
        let plain = build_bib(&[("console", 2, 0x7, 0, 0)]);
        let plain = unsafe { BootInfo::from_raw(plain.as_ptr()) }.unwrap();
        assert_eq!(plain.own_pid, 0);
        assert_eq!(plain.identity(), None);

        // And the flag is what gates the announcement, not the index: a sidecar
        // whose name IS an environment's but whose console is the kernel
        // transcript (`aerosls.posix.0`, the E3 boot spawn) carries index 0 and
        // no environment console — silence, which is NOT environment 0 being
        // announced. Both words are still reported, so a reader can tell the
        // two apart.
        let boot_spawn = build_bib_all(&[("console", 2, 0x7, 0, 0)], 0, 13, 0);
        let boot_spawn = unsafe { BootInfo::from_raw(boot_spawn.as_ptr()) }.unwrap();
        assert_eq!(boot_spawn.own_pid, 13);
        assert_eq!(boot_spawn.own_index, 0);
        assert!(!boot_spawn.is_env_console());
        assert_eq!(boot_spawn.identity(), None);
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
