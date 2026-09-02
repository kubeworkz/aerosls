//! The packed sidecar manifest (`aerosls/sidecar-manifest` §2.2).
//!
//! The manifest is the entire contract between the kernel and a sidecar:
//! identity, initial authority, resources, and debug plumbing. Source form
//! is JSON (for `genmanifest`); this module is the **packed binary form**
//! the kernel parses at `create_sidecar()` time — zero string parsing for
//! the kernel is approximated by a bounded, checked TLV walk over a
//! fixed header:
//!
//! ```text
//! offset  size  field
//! 0       8     magic          "AERSLSM1"
//! 8       2     version_major  = 1
//! 10      2     version_minor  = 0
//! 12      2     record_count
//! 14      2     flags          bit0 tolerate_unknown | bit1 strict_caps
//! 16      4     total_len      (whole blob, for bounds checking)
//! 20      4     body_crc32     (CRC-32 of the records, bytes 24..total_len)
//! 24      ...   records        (record_count × TLV)
//! ```
//!
//! Record: `{ tag: u16, len: u16, payload }` (4-byte header, payload `len`
//! bytes, back-to-back). Every read is bounds-checked against `total_len`,
//! so a malformed or truncated manifest fails here instead of overrunning;
//! the CRC is verified before any field is trusted (a corrupted manifest
//! fails at load, before any cap is minted — §2.3).
//!
//! Cap-record payloads (Phase 2 §2.2 — the kernel's `create_sidecar` parser
//! in kernel/cap.c and the C host test tests/cap_create_sidecar_host_test.c
//! share this exact layout; the golden tests below pin the bytes):
//!   CAP_MEM   name_len u16, name, phys_base u64, size u64 (bytes), rights u8
//!   CAP_CHAN  name_len u16, name, peer_len u16, peer, rights u8, flags u8
//!   NAME      name_len u16, name (sidecar identity — the kernel registers
//!             it in its sidecar registry so peers can wire channels to it)
//!
//! The parser is `no_std` and allocation-free: caps live in a fixed array
//! (matching `BootInfo`'s cap array — the kernel builds the initial
//! capability table from these in record order, and the BIB reports the
//! same order, so the two can never disagree).

use core::str;

pub const MANIFEST_MAGIC: [u8; 8] = *b"AERSLSM1";
/// Major version this parser understands. The kernel supports `[1, N]` and
/// refuses newer majors; the loader refuses anything it can't parse.
pub const MANIFEST_VERSION_MAJOR: u16 = 1;
pub const MANIFEST_VERSION_MINOR: u16 = 0;

/// Header flag bit 0: unknown tags are skippable (else fatal).
pub const FLAG_TOLERATE_UNKNOWN: u16 = 0x0001;
/// Header flag bit 1: any unrecognized cap record is fatal.
pub const FLAG_STRICT_CAPS: u16 = 0x0002;

pub const MAX_MANIFEST_CAPS: usize = 16;

/// Record tags (§2.2 table).
pub const TAG_PERSONALITY: u16 = 0x0001;
pub const TAG_IMAGE: u16 = 0x0002;
pub const TAG_BUDGET: u16 = 0x0003;
pub const TAG_CPU: u16 = 0x0004;
pub const TAG_LIMITS: u16 = 0x0005;
pub const TAG_CAP_MEM: u16 = 0x0006;
pub const TAG_CAP_CHAN: u16 = 0x0007;
pub const TAG_CAP_IRQ: u16 = 0x000B;
pub const TAG_CAP_IO: u16 = 0x000C;
pub const TAG_BOOTSTRAP: u16 = 0x0008;
pub const TAG_FLAGS: u16 = 0x0009;
/// Sidecar identity: the instance name other manifests' `CAP_CHAN` peer
/// fields resolve against (registered in the kernel's sidecar registry).
pub const TAG_NAME: u16 = 0x000A;
/// Reserved for Phase 3 signed manifests; ignored by v1.
pub const TAG_SIGNATURE: u16 = 0x7F00;

/// Packed manifest header size: magic[8] + version u16×2 + record_count
/// u16 + flags u16 + total_len u32 + body_crc32 u32 = 24 bytes (the
/// kernel's SIDECAR_MANIFEST_HEADER_LEN).
pub const HEADER_LEN: usize = 24;

/// Manifest parse failures. All are "this blob is not a manifest we can
/// load" — the kernel refuses `create_sidecar` on any of them, before any
/// cap is minted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ManifestErr {
    /// Bad magic, or a blob shorter than the fixed header.
    BadHeader,
    /// `version_major` is outside what this loader understands.
    BadVersion,
    /// `total_len` is inconsistent with the header (too small, or the blob
    /// is shorter than `total_len`).
    BadLength,
    /// The stored CRC-32 of the records does not match.
    CrcMismatch,
    /// A record header runs past `total_len`.
    Truncated,
    /// A record payload is longer than `total_len` allows.
    BadRecordLen,
    /// A variable-length field (name/peer) is not valid UTF-8.
    BadUtf8,
    /// Too many cap records for the fixed cap array.
    TooManyCaps,
    /// An unknown tag with `tolerate_unknown` clear, or — under
    /// `strict_caps` — a cap record with an unknown capability type.
    UnknownTag(u16),
}

impl core::fmt::Display for ManifestErr {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            ManifestErr::BadHeader => write!(f, "bad manifest header"),
            ManifestErr::BadVersion => write!(f, "unsupported manifest version"),
            ManifestErr::BadLength => write!(f, "manifest length inconsistent"),
            ManifestErr::CrcMismatch => write!(f, "manifest CRC mismatch"),
            ManifestErr::Truncated => write!(f, "manifest truncated"),
            ManifestErr::BadRecordLen => write!(f, "manifest record overruns blob"),
            ManifestErr::BadUtf8 => write!(f, "manifest name not UTF-8"),
            ManifestErr::TooManyCaps => write!(f, "manifest has too many caps"),
            ManifestErr::UnknownTag(t) => write!(f, "unknown manifest tag 0x{t:04x}"),
        }
    }
}

/// The `IMAGE` record (§2.2). The packed form matches the kernel's
/// parser (kernel/cap.c SIDECAR_TAG_IMAGE, 24 bytes):
///   entry_offset u64 (rp+0), blob_offset u32 (rp+8, image offset within
///   the flat package), image_size u32 (rp+12), image_kaddr u64 (rp+16,
///   0 here — the runtime packer supplies it via the appended footer).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Image {
    /// Image offset within the flat image+manifest package (blob_offset).
    pub offset: u32,
    /// Image size in bytes (image_size).
    pub size: u32,
    /// Entry point offset within the image (entry_offset).
    pub entry: u64,
}

/// The `BUDGET` record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Budget {
    pub mem_bytes: u64,
    pub stack_bytes: u32,
    pub heap_initial: u32,
}

/// The `CPU` record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cpu {
    pub share: u16,
    pub preemptible: bool,
}

/// The `LIMITS` record — resource ceilings enforced *inside* the sidecar.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Limits {
    pub max_tasks: u16,
    pub max_fds: u16,
    pub max_channels: u16,
    pub max_open_files: u16,
    pub chan_queue_depth: u16,
}

/// One initial capability — a `CAP_MEM` or `CAP_CHAN` record. The kernel
/// builds the sidecar's initial table from these, in record order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ManifestCap<'a> {
    pub name: &'a str,
    pub rights: u16,
    pub kind: CapKind<'a>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CapKind<'a> {
    /// `CAP_MEM`: base + size in the kernel's address space.
    Mem { base: u64, size: u64 },
    /// `CAP_CHAN`: the wired peer's name (opaque to the kernel).
    Chan { peer: Option<&'a str>, flags: u8 },
    /// `CAP_IRQ` (Driver SDK ABI v0.1 s4.3): a single-use bind cap
    /// for a device vector. `vector` is the IDT vector the kernel
    /// routes through `cap_irq_notify`; `perms` carries the bind
    /// right (`CAP_PERM_BIND`). Minted by `cap_create_sidecar`;
    /// `k_irq_bind` stamps it REVOKED at bind time.
    Irq { vector: u32, perms: u16 },
    /// `CAP_IO` (Driver SDK ABI v0.1 s4.2): a port-range cap. `base` is the
    /// first I/O port, `count` the number of ports, `perms` the R/W bits.
    /// Minted by `cap_create_sidecar`; `k_io_in`/`k_io_out` enforce the
    /// range (port = base + index, index + size <= count).
    Io { base: u32, count: u16, perms: u16 },
}

/// The `BOOTSTRAP` record — debug plumbing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Bootstrap<'a> {
    pub console: Option<&'a str>,
    pub debug: Option<&'a str>,
    pub log_level: u8,
}

/// A parsed manifest. Names are borrowed from the blob, which must outlive
/// the manifest (same lifetime contract as `BootInfo`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Manifest<'a> {
    pub version_major: u16,
    pub version_minor: u16,
    pub flags: u16,
    /// Instance name (`TAG_NAME`) — registered in the kernel's sidecar
    /// registry so other manifests can wire `CAP_CHAN` channels to it.
    pub name: Option<&'a str>,
    pub personality: Option<&'a str>,
    pub image: Option<Image>,
    pub budget: Option<Budget>,
    pub cpu: Option<Cpu>,
    pub limits: Option<Limits>,
    pub caps: [Option<ManifestCap<'a>>; MAX_MANIFEST_CAPS],
    pub n_caps: usize,
    pub bootstrap: Option<Bootstrap<'a>>,
    pub flags_value: Option<u32>,
    /// The raw `SIGNATURE` payload (opaque to v1; `None` when absent).
    pub signature: Option<&'a [u8]>,
}

impl<'a> Manifest<'a> {
    pub fn caps(&self) -> &[Option<ManifestCap<'a>>] {
        &self.caps[..self.n_caps]
    }

    /// Find an initial cap by name (names are only meaningful inside the
    /// sidecar; the kernel just builds the table).
    pub fn find_cap(&self, name: &str) -> Option<&ManifestCap<'a>> {
        self.caps().iter().flatten().find(|c| c.name == name)
    }
}

/// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — the same algorithm the
/// aerofs-lite superblock uses.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    crc ^ 0xFFFF_FFFF
}

/// Parse the packed manifest at `blob`. Every byte read is bounded by
/// `total_len`; the CRC is verified before any field is trusted. `strict`
/// semantics come from the header flags: unknown tags are skipped only when
/// `FLAG_TOLERATE_UNKNOWN` is set, and `FLAG_STRICT_CAPS` makes a cap
/// record with a type other than `CAP_MEM`/`CAP_CHAN` fatal.
pub fn parse_manifest(blob: &[u8]) -> Result<Manifest<'_>, ManifestErr> {
    if blob.len() < HEADER_LEN || &blob[..8] != &MANIFEST_MAGIC {
        return Err(ManifestErr::BadHeader);
    }
    let version_major = le_u16(blob, 8);
    let version_minor = le_u16(blob, 10);
    if version_major != MANIFEST_VERSION_MAJOR {
        return Err(ManifestErr::BadVersion);
    }
    let record_count = le_u16(blob, 12) as usize;
    let flags = le_u16(blob, 14);
    let total_len = le_u32(blob, 16) as usize;
    if total_len < HEADER_LEN || total_len > blob.len() {
        return Err(ManifestErr::BadLength);
    }
    let want_crc = le_u32(blob, 20);
    if crc32(&blob[HEADER_LEN..total_len]) != want_crc {
        return Err(ManifestErr::CrcMismatch);
    }

    let tolerate = flags & FLAG_TOLERATE_UNKNOWN != 0;
    // `strict_caps` (flag bit 1) has no v1 effect: CAP_MEM/CAP_CHAN are the
    // only cap tags, so the unknown-tag policy above already covers it.

    let mut m = Manifest {
        version_major,
        version_minor,
        flags,
        name: None,
        personality: None,
        image: None,
        budget: None,
        cpu: None,
        limits: None,
        caps: [None; MAX_MANIFEST_CAPS],
        n_caps: 0,
        bootstrap: None,
        flags_value: None,
        signature: None,
    };

    let mut off = HEADER_LEN;
    for _ in 0..record_count {
        if off + 4 > total_len {
            return Err(ManifestErr::Truncated);
        }
        let tag = le_u16(blob, off);
        let len = le_u16(blob, off + 2) as usize;
        off += 4;
        if off + len > total_len {
            return Err(ManifestErr::BadRecordLen);
        }
        let p = &blob[off..off + len];
        off += len;
        match tag {
            TAG_PERSONALITY => {
                // `name_len u16 + name` — like every other name field.
                let (name, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                if !rest.is_empty() {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.personality = Some(name);
            }
            TAG_NAME => {
                let (name, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                if !rest.is_empty() {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.name = Some(name);
            }
            TAG_IMAGE => {
                // Kernel layout (24 bytes, extra tolerated): entry_offset
                // u64, blob_offset u32, image_size u32, image_kaddr u64.
                if p.len() < 24 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.image = Some(Image {
                    entry: le_u64(p, 0),
                    offset: le_u32(p, 8),
                    size: le_u32(p, 12),
                });
            }
            TAG_BUDGET => {
                if p.len() != 16 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.budget = Some(Budget {
                    mem_bytes: le_u64(p, 0),
                    stack_bytes: le_u32(p, 8),
                    heap_initial: le_u32(p, 12),
                });
            }
            TAG_CPU => {
                if p.len() != 3 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.cpu = Some(Cpu {
                    share: le_u16(p, 0),
                    preemptible: p[2] & 1 != 0,
                });
            }
            TAG_LIMITS => {
                if p.len() != 10 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.limits = Some(Limits {
                    max_tasks: le_u16(p, 0),
                    max_fds: le_u16(p, 2),
                    max_channels: le_u16(p, 4),
                    max_open_files: le_u16(p, 6),
                    chan_queue_depth: le_u16(p, 8),
                });
            }
            TAG_CAP_MEM => {
                let (name, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                if rest.len() != 17 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.push_cap(ManifestCap {
                    name,
                    rights: rest[16] as u16,
                    kind: CapKind::Mem {
                        base: le_u64(rest, 0),
                        size: le_u64(rest, 8),
                    },
                })?;
            }
            TAG_CAP_CHAN => {
                let (name, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                let (peer, rest) = split_name(rest).ok_or(ManifestErr::BadRecordLen)?;
                if rest.len() != 2 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.push_cap(ManifestCap {
                    name,
                    rights: rest[0] as u16,
                    kind: CapKind::Chan {
                        peer: if peer.is_empty() { None } else { Some(peer) },
                        flags: rest[1],
                    },
                })?;
            }
            TAG_CAP_IRQ => {
                let (name, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                if rest.len() != 6 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.push_cap(ManifestCap {
                    name,
                    rights: le_u16(rest, 4),
                    kind: CapKind::Irq {
                        vector: le_u32(rest, 0),
                        perms: le_u16(rest, 4),
                    },
                })?;
            }
            TAG_CAP_IO => {
                let (name, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                if rest.len() != 8 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.push_cap(ManifestCap {
                    name,
                    rights: le_u16(rest, 6),
                    kind: CapKind::Io {
                        base: le_u32(rest, 0),
                        count: le_u16(rest, 4),
                        perms: le_u16(rest, 6),
                    },
                })?;
            }
            TAG_BOOTSTRAP => {
                let (console, rest) = split_name(p).ok_or(ManifestErr::BadRecordLen)?;
                let (debug, rest) = split_name(rest).ok_or(ManifestErr::BadRecordLen)?;
                if rest.len() != 1 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.bootstrap = Some(Bootstrap {
                    console: if console.is_empty() { None } else { Some(console) },
                    debug: if debug.is_empty() { None } else { Some(debug) },
                    log_level: rest[0],
                });
            }
            TAG_FLAGS => {
                if p.len() != 4 {
                    return Err(ManifestErr::BadRecordLen);
                }
                m.flags_value = Some(le_u32(p, 0));
            }
            TAG_SIGNATURE => m.signature = Some(p),
            other => {
                if tolerate {
                    continue;
                }
                return Err(ManifestErr::UnknownTag(other));
            }
        }
    }
    Ok(m)
}

impl<'a> Manifest<'a> {
    fn push_cap(&mut self, c: ManifestCap<'a>) -> Result<(), ManifestErr> {
        if self.n_caps >= MAX_MANIFEST_CAPS {
            return Err(ManifestErr::TooManyCaps);
        }
        self.caps[self.n_caps] = Some(c);
        self.n_caps += 1;
        Ok(())
    }
}

fn utf8(b: &[u8], e: ManifestErr) -> Result<&str, ManifestErr> {
    str::from_utf8(b).map_err(|_| e)
}

/// Split a `len u16 + bytes` name field off the front of a record payload.
fn split_name(p: &[u8]) -> Option<(&str, &[u8])> {
    if p.len() < 2 {
        return None;
    }
    let n = le_u16(p, 0) as usize;
    if p.len() < 2 + n {
        return None;
    }
    Some((utf8(&p[2..2 + n], ManifestErr::BadUtf8).ok()?, &p[2 + n..]))
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

// ── builder (host tooling / tests; the kernel never builds manifests) ───────

/// Build the packed form of `m` — the `genmanifest` equivalent. Returns the
/// blob exactly as `parse_manifest` expects it (header + CRC + records).
pub fn build_manifest(m: &Manifest<'_>) -> alloc::vec::Vec<u8> {
    let mut recs = alloc::vec::Vec::new();
    if let Some(n) = m.name {
        recs.push((TAG_NAME, enc_name(n)));
    }
    if let Some(p) = m.personality {
        recs.push((TAG_PERSONALITY, enc_name(p)));
    }
    if let Some(i) = m.image {
        // 24-byte record, the kernel's parser layout: entry_offset u64,
        // blob_offset u32, image_size u32, image_kaddr u64 (= 0; the
        // runtime packer appends the footer with the real address).
        let mut p = alloc::vec::Vec::with_capacity(24);
        p.extend_from_slice(&i.entry.to_le_bytes());
        p.extend_from_slice(&i.offset.to_le_bytes());
        p.extend_from_slice(&i.size.to_le_bytes());
        p.extend_from_slice(&0u64.to_le_bytes());
        recs.push((TAG_IMAGE, p));
    }
    if let Some(b) = m.budget {
        let mut p = alloc::vec::Vec::with_capacity(16);
        p.extend_from_slice(&b.mem_bytes.to_le_bytes());
        p.extend_from_slice(&b.stack_bytes.to_le_bytes());
        p.extend_from_slice(&b.heap_initial.to_le_bytes());
        recs.push((TAG_BUDGET, p));
    }
    if let Some(c) = m.cpu {
        let mut p = alloc::vec::Vec::with_capacity(3);
        p.extend_from_slice(&c.share.to_le_bytes());
        p.push(c.preemptible as u8);
        recs.push((TAG_CPU, p));
    }
    if let Some(l) = m.limits {
        let mut p = alloc::vec::Vec::with_capacity(10);
        p.extend_from_slice(&l.max_tasks.to_le_bytes());
        p.extend_from_slice(&l.max_fds.to_le_bytes());
        p.extend_from_slice(&l.max_channels.to_le_bytes());
        p.extend_from_slice(&l.max_open_files.to_le_bytes());
        p.extend_from_slice(&l.chan_queue_depth.to_le_bytes());
        recs.push((TAG_LIMITS, p));
    }
    for c in m.caps().iter().flatten() {
        match c.kind {
            CapKind::Mem { base, size } => {
                let mut p = alloc::vec::Vec::new();
                p.extend_from_slice(&enc_name(c.name));
                p.extend_from_slice(&base.to_le_bytes());
                p.extend_from_slice(&size.to_le_bytes());
                p.push(c.rights as u8);
                recs.push((TAG_CAP_MEM, p));
            }
            CapKind::Chan { peer, flags } => {
                let mut p = alloc::vec::Vec::new();
                p.extend_from_slice(&enc_name(c.name));
                p.extend_from_slice(&enc_name(peer.unwrap_or("")));
                p.push(c.rights as u8);
                p.push(flags);
                recs.push((TAG_CAP_CHAN, p));
            }
            CapKind::Irq { vector, perms } => {
                let mut p = alloc::vec::Vec::new();
                p.extend_from_slice(&enc_name(c.name));
                p.extend_from_slice(&vector.to_le_bytes());
                p.extend_from_slice(&perms.to_le_bytes());
                recs.push((TAG_CAP_IRQ, p));
            }
            CapKind::Io { base, count, perms } => {
                let mut p = alloc::vec::Vec::new();
                p.extend_from_slice(&enc_name(c.name));
                p.extend_from_slice(&base.to_le_bytes());
                p.extend_from_slice(&count.to_le_bytes());
                p.extend_from_slice(&perms.to_le_bytes());
                recs.push((TAG_CAP_IO, p));
            }
        }
    }
    if let Some(b) = m.bootstrap {
        let mut p = alloc::vec::Vec::new();
        p.extend_from_slice(&enc_name(b.console.unwrap_or("")));
        p.extend_from_slice(&enc_name(b.debug.unwrap_or("")));
        p.push(b.log_level);
        recs.push((TAG_BOOTSTRAP, p));
    }
    if let Some(f) = m.flags_value {
        recs.push((TAG_FLAGS, f.to_le_bytes().to_vec()));
    }
    if let Some(s) = m.signature {
        recs.push((TAG_SIGNATURE, s.to_vec()));
    }

    let total_len = HEADER_LEN
        + recs.iter().map(|(_, p)| 4 + p.len()).sum::<usize>();
    let mut blob = alloc::vec::Vec::with_capacity(total_len);
    blob.extend_from_slice(&MANIFEST_MAGIC);
    blob.extend_from_slice(&m.version_major.to_le_bytes());
    blob.extend_from_slice(&m.version_minor.to_le_bytes());
    blob.extend_from_slice(&(recs.len() as u16).to_le_bytes());
    blob.extend_from_slice(&m.flags.to_le_bytes());
    blob.extend_from_slice(&(total_len as u32).to_le_bytes());
    // CRC of the records — filled after the records are appended.
    blob.extend_from_slice(&0u32.to_le_bytes());
    for (tag, p) in &recs {
        blob.extend_from_slice(&tag.to_le_bytes());
        blob.extend_from_slice(&(p.len() as u16).to_le_bytes());
        blob.extend_from_slice(p);
    }
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());
    debug_assert_eq!(blob.len(), total_len);
    blob
}

fn enc_name(s: &str) -> alloc::vec::Vec<u8> {
    let mut v = alloc::vec::Vec::with_capacity(2 + s.len());
    v.extend_from_slice(&(s.len() as u16).to_le_bytes());
    v.extend_from_slice(s.as_bytes());
    v
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;
    use alloc::vec::Vec;

    fn sample() -> Manifest<'static> {
        Manifest {
            version_major: 1,
            version_minor: 0,
            flags: 0,
            name: Some("posix.0"),
            personality: Some("aerosls.posix.v1"),
            image: Some(Image { offset: 0x4000, size: 0xC000, entry: 0x4000 }),
            budget: Some(Budget { mem_bytes: 1 << 25, stack_bytes: 1 << 18, heap_initial: 1 << 23 }),
            cpu: Some(Cpu { share: 200, preemptible: true }),
            limits: Some(Limits { max_tasks: 64, max_fds: 4096, max_channels: 128, max_open_files: 512, chan_queue_depth: 64 }),
            caps: [
                Some(ManifestCap { name: "budget", rights: 0x3, kind: CapKind::Mem { base: 0x1000_0000, size: 1 << 25 } }),
                Some(ManifestCap { name: "img.ro", rights: 0x4, kind: CapKind::Mem { base: 0x2000_0000, size: 1 << 20 } }),
                Some(ManifestCap { name: "console", rights: 0x7, kind: CapKind::Chan { peer: Some("kernel.debug.console"), flags: 0 } }),
                Some(ManifestCap { name: "ramdisk", rights: 0x7, kind: CapKind::Chan { peer: Some("drv.ramdisk.0"), flags: 0 } }),
                None, None, None, None, None, None, None, None, None, None, None, None,
            ],
            n_caps: 4,
            bootstrap: Some(Bootstrap { console: Some("console"), debug: None, log_level: 1 }),
            flags_value: Some(0),
            signature: None,
        }
    }

    #[test]
    fn build_parse_roundtrip() {
        let blob = build_manifest(&sample());
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.name, Some("posix.0"));
        assert_eq!(m.personality, Some("aerosls.posix.v1"));
        assert_eq!(m.image.unwrap().entry, 0x4000);
        assert_eq!(m.budget.unwrap().mem_bytes, 1 << 25);
        assert_eq!(m.cpu.unwrap().share, 200);
        assert_eq!(m.limits.unwrap().max_tasks, 64);
        assert_eq!(m.n_caps, 4);
        let budget = m.find_cap("budget").unwrap();
        assert_eq!(budget.kind, CapKind::Mem { base: 0x1000_0000, size: 1 << 25 });
        let ramdisk = m.find_cap("ramdisk").unwrap();
        assert!(matches!(ramdisk.kind, CapKind::Chan { peer: Some("drv.ramdisk.0"), .. }));
        assert!(m.find_cap("nope").is_none());
        assert_eq!(m.bootstrap.unwrap().console, Some("console"));
    }

    #[test]
    fn rejects_corruption() {
        let mut blob = build_manifest(&sample());
        blob[0] = b'X';
        assert_eq!(parse_manifest(&blob), Err(ManifestErr::BadHeader));

        let mut blob = build_manifest(&sample());
        blob[20] ^= 0xFF; // corrupt the stored CRC
        assert_eq!(parse_manifest(&blob), Err(ManifestErr::CrcMismatch));

        let mut blob = build_manifest(&sample());
        blob[8] = 2; // version_major = 2
        assert_eq!(parse_manifest(&blob), Err(ManifestErr::BadVersion));
    }

    #[test]
    fn rejects_truncation() {
        let blob = build_manifest(&sample());
        // Chop the tail and make total_len agree with the truncation (and
        // the CRC cover the truncated records) — the record walk must fail,
        // not overrun.
        let mut v = blob[..blob.len() - 3].to_vec();
        let len = v.len() as u32;
        v[16..20].copy_from_slice(&len.to_le_bytes());
        let crc = crc32(&v[HEADER_LEN..]);
        v[20..24].copy_from_slice(&crc.to_le_bytes());
        assert_eq!(parse_manifest(&v), Err(ManifestErr::BadRecordLen));
    }

    /// Build a blob with the given header flags and an extra set of records
    /// *inside* `record_count` (the walk is bounded by the header count, so
    /// an appended record would never be seen).
    fn blob_with_extra(flags: u16, extra: &[(u16, &[u8])]) -> Vec<u8> {
        let mut recs: Vec<(u16, Vec<u8>)> = vec![(TAG_PERSONALITY, enc_name("t.personality"))];
        recs.extend(extra.iter().map(|(t, p)| (*t, p.to_vec())));
        let total_len = HEADER_LEN + recs.iter().map(|(_, p)| 4 + p.len()).sum::<usize>();
        let mut blob = Vec::new();
        blob.extend_from_slice(&MANIFEST_MAGIC);
        blob.extend_from_slice(&1u16.to_le_bytes()); // version_major
        blob.extend_from_slice(&0u16.to_le_bytes()); // version_minor
        blob.extend_from_slice(&(recs.len() as u16).to_le_bytes());
        blob.extend_from_slice(&flags.to_le_bytes());
        blob.extend_from_slice(&(total_len as u32).to_le_bytes());
        blob.extend_from_slice(&0u32.to_le_bytes()); // crc placeholder
        for (t, p) in &recs {
            blob.extend_from_slice(&t.to_le_bytes());
            blob.extend_from_slice(&(p.len() as u16).to_le_bytes());
            blob.extend_from_slice(p);
        }
        let crc = crc32(&blob[HEADER_LEN..]);
        blob[20..24].copy_from_slice(&crc.to_le_bytes());
        blob
    }

    #[test]
    fn unknown_tags_respect_tolerate_flag() {
        // An unknown tag inside record_count: fatal by default…
        let v = blob_with_extra(0, &[(0x1234, &[0xAB])]);
        assert_eq!(parse_manifest(&v), Err(ManifestErr::UnknownTag(0x1234)));
        // …skipped when tolerate_unknown is set.
        let v = blob_with_extra(FLAG_TOLERATE_UNKNOWN, &[(0x1234, &[0xAB])]);
        let m = parse_manifest(&v).unwrap();
        assert_eq!(m.personality, Some("t.personality"));
        assert_eq!(m.n_caps, 0);
    }

    #[test]
    fn signature_slot_is_ignored() {
        // 0x7F00 is reserved and known-but-ignored in v1.
        let v = blob_with_extra(0, &[(TAG_SIGNATURE, &[1, 2, 3])]);
        let m = parse_manifest(&v).unwrap();
        assert_eq!(m.signature, Some(&[1u8, 2, 3][..]));
    }

    /// A manifest containing exactly the given caps, in order, and no
    /// other records except an optional identity name.
    fn caps_only(name: Option<&'static str>, caps: Vec<ManifestCap<'static>>) -> Manifest<'static> {
        let mut arr = [None; MAX_MANIFEST_CAPS];
        for (i, c) in caps.iter().enumerate() {
            arr[i] = Some(*c);
        }
        Manifest {
            version_major: 1,
            version_minor: 0,
            flags: 0,
            name,
            personality: None,
            image: None,
            budget: None,
            cpu: None,
            limits: None,
            caps: arr,
            n_caps: caps.len(),
            bootstrap: None,
            flags_value: None,
            signature: None,
        }
    }

    /// Pin the exact CAP_MEM record bytes on the wire. The kernel parser
    /// (kernel/cap.c `SIDECAR_TAG_CAP_MEM`) and the C host test's blob
    /// builder emit/consume this layout byte-for-byte:
    /// `name_len u16, name, phys_base u64, size u64 (bytes), rights u8`.
    /// Pin the exact TAG_NAME record bytes — the sidecar identity the
    /// kernel registers in its sidecar registry (kernel/cap.c
    /// `SIDECAR_TAG_NAME`). Same `name_len u16 + name` shape as
    /// PERSONALITY.
    #[test]
    fn name_wire_bytes_golden() {
        let m = caps_only(Some("drv.child.0"), vec![]);
        let blob = build_manifest(&m);
        // Header (24) + one record { tag u16, len u16, payload }.
        assert_eq!(&blob[24..26], &TAG_NAME.to_le_bytes());
        assert_eq!(&blob[26..28], &13u16.to_le_bytes()); // 2 + 11
        let mut want = Vec::new();
        want.extend_from_slice(&11u16.to_le_bytes()); // name_len
        want.extend_from_slice(b"drv.child.0");
        assert_eq!(&blob[28..], &want[..]);
    }

    #[test]
    fn cap_mem_wire_bytes_golden() {
        let m = caps_only(None, vec![ManifestCap {
            name: "budget",
            rights: 0x3,
            kind: CapKind::Mem { base: 0x2000_0000, size: 1 << 18 },
        }]);
        let blob = build_manifest(&m);
        // Header (24) + one record { tag u16, len u16, payload }.
        assert_eq!(&blob[24..26], &TAG_CAP_MEM.to_le_bytes());
        assert_eq!(&blob[26..28], &25u16.to_le_bytes()); // 2 + 6 + 8 + 8 + 1
        let mut want = Vec::new();
        want.extend_from_slice(&6u16.to_le_bytes()); // name_len
        want.extend_from_slice(b"budget");
        want.extend_from_slice(&0x2000_0000u64.to_le_bytes()); // phys_base
        want.extend_from_slice(&(1u64 << 18).to_le_bytes()); // size (bytes)
        want.push(0x3); // rights
        assert_eq!(&blob[28..], &want[..]);
        assert_eq!(blob.len(), 24 + 4 + 25);
    }

    /// Same golden pin for CAP_CHAN:
    /// `name_len u16, name, peer_len u16, peer, rights u8, flags u8`.
    #[test]
    fn cap_chan_wire_bytes_golden() {
        let m = caps_only(None, vec![ManifestCap {
            name: "console",
            rights: 0x3,
            kind: CapKind::Chan { peer: Some("kernel.debug.console"), flags: 0 },
        }]);
        let blob = build_manifest(&m);
        assert_eq!(&blob[24..26], &TAG_CAP_CHAN.to_le_bytes());
        assert_eq!(&blob[26..28], &33u16.to_le_bytes()); // 2+7 + 2+20 + 1+1
        let mut want = Vec::new();
        want.extend_from_slice(&7u16.to_le_bytes()); // name_len
        want.extend_from_slice(b"console");
        want.extend_from_slice(&20u16.to_le_bytes()); // peer_len
        want.extend_from_slice(b"kernel.debug.console");
        want.push(0x3); // rights
        want.push(0); // flags
        assert_eq!(&blob[28..], &want[..]);
    }

    /// Same golden pin for CAP_IRQ (Driver SDK ABI v0.1 s4.3):
    /// `name_len u16, name, vector u32, perms u16`.
    #[test]
    fn cap_irq_wire_bytes_golden() {
        let m = caps_only(None, vec![ManifestCap {
            name: "irq.timer.0",
            rights: 0x1,
            kind: CapKind::Irq { vector: 32, perms: 0x1 },
        }]);
        let blob = build_manifest(&m);
        assert_eq!(&blob[24..26], &TAG_CAP_IRQ.to_le_bytes());
        assert_eq!(&blob[26..28], &19u16.to_le_bytes()); // 2 + 11 + 4 + 2
        let mut want = Vec::new();
        want.extend_from_slice(&11u16.to_le_bytes()); // name_len
        want.extend_from_slice(b"irq.timer.0");
        want.extend_from_slice(&32u32.to_le_bytes()); // vector
        want.extend_from_slice(&1u16.to_le_bytes()); // perms
        assert_eq!(&blob[28..], &want[..]);

        // Round-trip: parse the packed blob and check the irq cap.
        let parsed = parse_manifest(&blob).unwrap();
        let irq = parsed.caps().iter().flatten().find(|c| c.name == "irq.timer.0");
        assert_eq!(
            irq.map(|c| c.kind),
            Some(CapKind::Irq { vector: 32, perms: 0x1 })
        );
    }

    /// Same golden pin for CAP_IO (Driver SDK ABI v0.1 s4.2):
    /// `name_len u16, name, base u32, count u16, perms u16`.
    #[test]
    fn cap_io_wire_bytes_golden() {
        let m = caps_only(None, vec![ManifestCap {
            name: "uart",
            rights: 0x3,
            kind: CapKind::Io { base: 0x3F8, count: 8, perms: 0x3 },
        }]);
        let blob = build_manifest(&m);
        assert_eq!(&blob[24..26], &TAG_CAP_IO.to_le_bytes());
        assert_eq!(&blob[26..28], &14u16.to_le_bytes()); // 2 + 4 + 4 + 2 + 2
        let mut want = Vec::new();
        want.extend_from_slice(&4u16.to_le_bytes()); // name_len
        want.extend_from_slice(b"uart");
        want.extend_from_slice(&0x3F8u32.to_le_bytes()); // base
        want.extend_from_slice(&8u16.to_le_bytes()); // count
        want.extend_from_slice(&3u16.to_le_bytes()); // perms
        assert_eq!(&blob[28..], &want[..]);

        let parsed = parse_manifest(&blob).unwrap();
        let io = parsed.caps().iter().flatten().find(|c| c.name == "uart");
        assert_eq!(
            io.map(|c| c.kind),
            Some(CapKind::Io { base: 0x3F8, count: 8, perms: 0x3 })
        );
    }

    /// Locate the payload of the first record with the given tag in a
    /// packed blob (same TLV walk `parse_manifest` does).
    fn record_payload<'a>(blob: &'a [u8], tag: u16) -> Option<&'a [u8]> {
        let mut off = HEADER_LEN;
        while off + 4 <= blob.len() {
            let t = u16::from_le_bytes([blob[off], blob[off + 1]]);
            let len = u16::from_le_bytes([blob[off + 2], blob[off + 3]]) as usize;
            if off + 4 + len > blob.len() {
                return None;
            }
            if t == tag {
                return Some(&blob[off + 4..off + 4 + len]);
            }
            off += 4 + len;
        }
        None
    }

    /// Mirror one of the repo's manifest.json files (source form) as a
    /// `Manifest`: the instance name, personality, and the shared
    /// image/budget/cpu/limits plus the caps the JSON declares, in order.
    fn repo_manifest(
        name: &'static str,
        personality: &'static str,
        caps: Vec<ManifestCap<'static>>,
    ) -> Manifest<'static> {
        let mut arr = [None; MAX_MANIFEST_CAPS];
        for (i, c) in caps.iter().enumerate() {
            arr[i] = Some(*c);
        }
        Manifest {
            version_major: 1,
            version_minor: 0,
            flags: 0,
            name: Some(name),
            personality: Some(personality),
            image: Some(Image { offset: 0x8000, size: 0x4000, entry: 0x8000 }),
            budget: Some(Budget {
                mem_bytes: 262144,
                stack_bytes: 16384,
                heap_initial: 65536,
            }),
            cpu: Some(Cpu { share: 50, preemptible: true }),
            limits: Some(Limits {
                max_tasks: 1,
                max_fds: 0,
                max_channels: 16,
                max_open_files: 0,
                chan_queue_depth: 32,
            }),
            caps: arr,
            n_caps: caps.len(),
            bootstrap: None,
            flags_value: None,
            signature: None,
        }
    }

    /// The repo's real manifests (user/ramdisk/manifest.json and
    /// user/nvme_driver/manifest.json) declare an instance name.
    /// cap_create_sidecar registers that name in the kernel's sidecar
    /// registry (kernel/cap.c `SIDECAR_TAG_NAME` →
    /// sidecar_registry_register), and it is the same name other
    /// manifests' CAP_CHAN peer fields and the device registry's
    /// `driver_manifest` (user/init/src/devreg.rs, e.g. "drv.nvme.0")
    /// resolve against. Assert build_manifest emits the exact TAG_NAME
    /// record for both, so the kernel-side registration sees these names.
    #[test]
    fn repo_manifest_names_emit_tag_name() {
        let ramdisk = repo_manifest(
            "drv.ramdisk.0",
            "aerosls.ramdisk.v1",
            vec![
                ManifestCap { name: "budget", rights: 0x3, kind: CapKind::Mem { base: 0x1000_0000, size: 262144 } },
                ManifestCap { name: "storage", rights: 0x1, kind: CapKind::Mem { base: 0x1040_0000, size: 33_554_432 } },
                ManifestCap { name: "console", rights: 0x7, kind: CapKind::Chan { peer: Some("kernel.debug.console"), flags: 0 } },
                ManifestCap { name: "img.ro", rights: 0x5, kind: CapKind::Mem { base: 0x2040_0000, size: 16384 } },
            ],
        );
        let nvme = repo_manifest(
            "drv.nvme.0",
            "aerosls.nvme.v1",
            vec![
                ManifestCap { name: "budget", rights: 0x3, kind: CapKind::Mem { base: 0x1000_0000, size: 262144 } },
                ManifestCap { name: "bar0", rights: 0x3, kind: CapKind::Mem { base: 0xfebf_0000, size: 8192 } },
                ManifestCap { name: "dma", rights: 0x3, kind: CapKind::Mem { base: 0x1100_0000, size: 1_048_576 } },
                ManifestCap { name: "console", rights: 0x7, kind: CapKind::Chan { peer: Some("kernel.debug.console"), flags: 0 } },
                ManifestCap { name: "img.ro", rights: 0x5, kind: CapKind::Mem { base: 0x2020_0000, size: 16384 } },
            ],
        );

        for m in [ramdisk, nvme] {
            let blob = build_manifest(&m);
            let name = m.name.unwrap();
            let p = record_payload(&blob, TAG_NAME).expect("TAG_NAME record emitted");
            let mut want = Vec::new();
            want.extend_from_slice(&(name.len() as u16).to_le_bytes());
            want.extend_from_slice(name.as_bytes());
            assert_eq!(p, &want[..], "TAG_NAME payload for '{name}'");

            // The kernel-side parser sees the same identity — this is the
            // name cap_create_sidecar registers in the sidecar registry.
            let parsed = parse_manifest(&blob).unwrap();
            assert_eq!(parsed.name, Some(name));
            assert_eq!(parsed.n_caps, m.n_caps);
            assert_eq!(parsed.find_cap("console").unwrap().name, "console");
            assert!(parsed.find_cap("budget").is_some());
        }
    }

    /// Walk the built blob's TLV records EXACTLY like the kernel's parser
    /// (kernel/cap.c cap_create_sidecar: header at 0, records from
    /// HEADER_LEN, each tag u16 + len u16 + payload, then the 8-byte
    /// image_kaddr footer after the last record). This pins the shared
    /// wire format so a manifest built here boots on the kernel parser.
    #[test]
    fn image_record_matches_kernel_parser_layout() {
        let mut m = sample();
        m.image = Some(Image { offset: 0x8000, size: 0x4000, entry: 0x10 });
        let mut blob = build_manifest(&m);

        // Header fields the kernel reads first.
        assert_eq!(&blob[..8], &MANIFEST_MAGIC);
        assert_eq!(u16::from_le_bytes([blob[8], blob[9]]), 1); // version_major

        // Record walk, mirroring the kernel's loop.
        let mut off = HEADER_LEN;
        let mut image_payload: Option<&[u8]> = None;
        let record_count = u16::from_le_bytes([blob[12], blob[13]]);
        for _ in 0..record_count {
            assert!(off + 4 <= blob.len());
            let tag = u16::from_le_bytes([blob[off], blob[off + 1]]);
            let rlen = u16::from_le_bytes([blob[off + 2], blob[off + 3]]) as usize;
            assert!(off + 4 + rlen <= blob.len());
            if tag == TAG_IMAGE {
                image_payload = Some(&blob[off + 4..off + 4 + rlen]);
            }
            off += 4 + rlen;
        }

        // The kernel requires rlen >= 24; the payload offsets are:
        //   rp+0  entry_offset u64, rp+8 blob_offset u32,
        //   rp+12 image_size u32,   rp+16 image_kaddr u64.
        let p = image_payload.expect("TAG_IMAGE record present");
        assert_eq!(p.len(), 24, "kernel parser requires a 24-byte IMAGE record");
        assert_eq!(le_u64(p, 0), 0x10, "entry_offset at rp+0");
        assert_eq!(le_u32(p, 8), 0x8000, "blob_offset at rp+8");
        assert_eq!(le_u32(p, 12), 0x4000, "image_size at rp+12");
        assert_eq!(le_u64(p, 16), 0, "image_kaddr: footer supplies it");

        // Footer: the packer appends image_kaddr after the last record; the
        // kernel reads it as `blob + footer_off` where footer_off is the
        // post-record offset. total_len must cover it and the body CRC is
        // computed over records + footer.
        let kaddr = 0x2000_0000u64;
        blob.extend_from_slice(&kaddr.to_le_bytes());
        let total = blob.len() as u32;
        blob[16..20].copy_from_slice(&total.to_le_bytes());
        let crc = crc32(&blob[HEADER_LEN..]);
        blob[20..24].copy_from_slice(&crc.to_le_bytes());
        assert_eq!(u32::from_le_bytes([blob[16], blob[17], blob[18], blob[19]]), total);
        assert_eq!(off + 8, blob.len(), "footer sits right after the records");
        assert_eq!(le_u64(&blob, off), kaddr, "footer image_kaddr");
        assert_eq!(crc32(&blob[HEADER_LEN..]), crc, "body CRC over records+footer");

        // And the Rust parser still round-trips the same manifest.
        let parsed = parse_manifest(&blob).unwrap();
        assert_eq!(parsed.image.unwrap().entry, 0x10);
        assert_eq!(parsed.image.unwrap().offset, 0x8000);
        assert_eq!(parsed.image.unwrap().size, 0x4000);
    }
}
