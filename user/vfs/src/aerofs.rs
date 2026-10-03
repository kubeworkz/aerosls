//! aerofs-lite — the root filesystem format: v1 read-only, v2 writable
//! (Phase 2 design §6.3; POSIX-Environments v0.2 §5, P1b part 1).
//!
//! ─── The version rule (read v1, write v2, refuse the rest) ────────────────
//! v1 is the format every existing image is written in: the system rootfs and
//! every tenant ramdisk built by `ImageBuilder`. It is read-only, and it stays
//! exactly what it was — this module still parses and builds it byte-for-byte,
//! so `genrootfs` and the boot path do not change.
//!
//! v2 is what a WRITER formats: it is the same block size and the same block
//! address space, with three changes, all of which v1 predates:
//!
//!   1. a longer superblock fixed header carrying the allocation state a
//!      writable store needs (total blocks, the allocation-map extent);
//!   2. an inode layout with **ten direct pointers and two indirect blocks**
//!      (v1 has eleven and one) — 266 blocks, 136,192 bytes max per file,
//!      where v1's ceiling is 139 blocks and 71,168 bytes; and
//!   3. an allocation map (a block bitmap + an inode bitmap) in the blocks
//!      the superblock names, so a store can be written, not just read.
//!
//! A store whose version is neither 1 nor 2 is refused BY NAME
//! (`SuperblockRefusal::VERSION`), never mounted as if it were one of them:
//! a v3 written by a future build is not something this build may interpret.
//! The honest migration rule follows from that: **read v1, write v2, refuse
//! the rest**. v1 images (the system rootfs) mount read-only, exactly as
//! before; an empty store is formatted v2 and mounts writable (that is what a
//! tenant environment's private store gets). There is deliberately no
//! in-place v1 → v2 upgrade in this first cut: rewriting a store this build
//! did not format is a destructive operation, and it is not one to perform as
//! a side effect of a mount.
//!
//! The v1 on-disk layout, pinned exactly:
//!
//! ```text
//! block 0     superblock  (512 B): magic "AFSL", version u32=1,
//!                       block_size u32=512, inode_count u32,
//!                       inode_start u32 (block index), data_start u32
//!                       (block index), root_inode u32=2, sb_crc u32
//!                       (CRC-32 of everything before it), reserved
//! block S..   inode table 64 B each: { mode u16, uid u16, gid u16,
//!                       size u32, mtime u32, blocks[11] u32,
//!                       indirect u32, reserved u16 }  — inode i at
//!                       offset (i-1)*64, ino 0/1 unused, root=2
//! then        data blocks  dir entry 64 B each: { name[56], ino u32,
//!                       type u8, rec_len u8, reserved u16 }
//! ```
//!
//! Block addressing: 11 direct pointers plus one indirect block holding
//! `BLOCK_SIZE/4` u32 block pointers — 139 blocks, 71,168 bytes max per
//! file. Directory data is a run of 64 B entries (rec_len respected);
//! "." and ".." are real entries emitted by the builder.
//!
//! The v2 on-disk layout, pinned exactly:
//!
//! ```text
//! block 0     superblock  (512 B): magic "AFSL", version u32=2,
//!                       block_size u32=512, inode_count u32,
//!                       inode_start u32, data_start u32,
//!                       root_inode u32=2, total_blocks u32,
//!                       alloc_start u32, alloc_blocks u32, reserved,
//!                       sb_crc u32 at offset 44 (CRC-32 of bytes 0..44
//!                       with the CRC field zeroed)
//! block A..   allocation map: the block bitmap first (one bit per block,
//!                       LSB-first; set = in use), then the inode bitmap
//!                       (one bit per inode slot, ino 2 at bit 0), padded
//!                       to `alloc_blocks` whole 512 B blocks
//! block S..   inode table 64 B each: { mode u16, uid u16, gid u16,
//!                       size u32, mtime u32, blocks[10] u32,
//!                       indirect1 u32, indirect2 u32, reserved u16 }
//! then        data blocks  dir entry 64 B each: { name[56], ino u32,
//!                       type u8, rec_len u8, reserved u16 }
//! ```
//!
//! v2 block addressing: block `i` of a file is `blocks[i]` for `i < 10`; then
//! `indirect1`'s pointer `i - 10` for `10 <= i < 138`; then `indirect2`'s
//! pointer `i - 138` for `138 <= i < 266`. A deleted directory entry is a
//! tombstone: `ino` zero and the name zeroed, `rec_len` preserved so the walk
//! still steps over it; lookups and listings skip `ino == 0`.
//!
//! This module is pure (no kernel, no I/O): parsing/encoding the format
//! and building images. The VFS layer (`vfs.rs`) drives it through the
//! block cache — v2's allocation state is read and written through that
//! same cache, so the format module only defines the arithmetic. The
//! builder is the host-side `genrootfs` tool (implementation plan §7)
//! made testable.

use alloc::string::String;
use alloc::string::ToString;
use alloc::vec;
use alloc::vec::Vec;

/// The magic bytes "AFSL" (block 0, offset 0).
pub const AEROFS_MAGIC: [u8; 4] = *b"AFSL";
/// v1: read-only. Still parsed and still built (`ImageBuilder`) — every
/// existing image, including the system rootfs, is this.
pub const AEROFS_VERSION_V1: u32 = 1;
/// v2: writable. This is the version a FORMAT writes, and the only version
/// this build mounts read-write. See the module header for the version rule.
pub const AEROFS_VERSION_V2: u32 = 2;
/// The version this build writes when it formats an empty store.
pub const AEROFS_VERSION: u32 = AEROFS_VERSION_V2;

pub const INODE_SIZE: usize = 64;
pub const DIRENT_SIZE: usize = 64;
pub const NAME_MAX: usize = 56;
/// v1 inode: direct block pointers per inode (11 × u32), plus one indirect.
pub const NDIRECT: usize = 11;
/// Pointers per indirect block (512 / 4). Both versions share this; v2 just
/// has two levels of it.
pub const NINDIRECT: usize = 128;
/// v1: max blocks per file.
pub const MAX_BLOCKS: u64 = (NDIRECT + NINDIRECT) as u64;
/// v2 inode: direct pointers per inode (10 × u32) — one fewer than v1,
/// because the two indirect pointers below need the four bytes v1 spent on
/// its eleventh direct pointer.
pub const NDIRECT_V2: usize = 10;
/// v2: max blocks per file — 10 direct + two indirect blocks of 128 each.
pub const MAX_BLOCKS_V2: u64 = (NDIRECT_V2 + 2 * NINDIRECT) as u64;
/// The file-size ceilings each version's block count implies. These are the
/// numbers the P1b guard talks about: v1's 71,168 is the ceiling that made a
/// durable tenant filesystem unusable, and v2's 136,192 is the one that
/// moves it.
pub const MAX_FILE_BYTES_V1: u64 = MAX_BLOCKS * aerosls_proto::BLOCK_SIZE as u64;
pub const MAX_FILE_BYTES_V2: u64 = MAX_BLOCKS_V2 * aerosls_proto::BLOCK_SIZE as u64;
/// The inode count every freshly formatted v2 store gets. 64 inodes is four
/// times the eight environments a node runs, with room for a store to grow a
/// directory tree of its own; the table costs eight 512 B blocks.
pub const FORMAT_INODE_COUNT: u32 = 64;

/// Bit `i` of a bitmap byte array (LSB-first within each byte). Shared by the
/// v2 block and inode bitmaps, and pure so the host tests can exercise the
/// arithmetic without a device.
pub fn bitmap_get(bits: &[u8], idx: u64) -> bool {
    let byte = (idx / 8) as usize;
    byte < bits.len() && (bits[byte] >> (idx % 8)) & 1 != 0
}
pub fn bitmap_set(bits: &mut [u8], idx: u64) {
    let byte = (idx / 8) as usize;
    if byte < bits.len() {
        bits[byte] |= 1 << (idx % 8);
    }
}
pub fn bitmap_clear(bits: &mut [u8], idx: u64) {
    let byte = (idx / 8) as usize;
    if byte < bits.len() {
        bits[byte] &= !(1 << (idx % 8));
    }
}
/// The first clear bit in `[start, end)`, or None when the range is full.
/// This is the allocator's only search: a linear scan from zero, on a bitmap
/// that covers at most `total_blocks` bits (2,048 for the 1 MiB first-cut
/// store), which is why there is no free-list to keep consistent.
pub fn bitmap_find_clear(bits: &[u8], start: u64, end: u64) -> Option<u64> {
    (start..end).find(|&i| !bitmap_get(bits, i))
}

/// Directory entry type bytes (mirror Linux's DT_*). `DT_FIFO`/`DT_CHR`
/// never appear in aerofs-lite images (the builder never makes them); they
/// exist for in-memory dirents (`/dev`, pipe `fstat`). Values are local to
/// this format (note: not Linux's numbers — `DT_REG`/`DT_DIR` are baked
/// into every existing image, so they cannot change).
pub const DT_REG: u8 = 1;
pub const DT_DIR: u8 = 2;
pub const DT_FIFO: u8 = 3;
pub const DT_CHR: u8 = 4;

/// Mode type bits (mirror S_IFMT).
pub const S_IFMT: u16 = 0o170000;
pub const S_IFREG: u16 = 0o100000;
pub const S_IFDIR: u16 = 0o040000;
pub const S_IFCHR: u16 = 0o020000;
pub const S_IFIFO: u16 = 0o010000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileType {
    File,
    Dir,
    /// A pipe (never on disk; only in-memory objects, e.g. pipe fds).
    Fifo,
    /// A character device node (never on disk; only `/dev` entries).
    Char,
}

impl FileType {
    pub fn from_dt(dt: u8) -> Option<FileType> {
        match dt {
            DT_REG => Some(FileType::File),
            DT_DIR => Some(FileType::Dir),
            DT_FIFO => Some(FileType::Fifo),
            DT_CHR => Some(FileType::Char),
            _ => None,
        }
    }
    pub fn dt(self) -> u8 {
        match self {
            FileType::File => DT_REG,
            FileType::Dir => DT_DIR,
            FileType::Fifo => DT_FIFO,
            FileType::Char => DT_CHR,
        }
    }
}

/// The superblock (block 0). One struct for both versions: the v2-only
/// fields are zero on a v1 image, so a caller that only needs the shared
/// layout (mount, revalidation) never has to branch on the version.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Superblock {
    /// 1 (read-only, built) or 2 (writable, formatted by this build).
    pub version: u32,
    pub block_size: u32,
    pub inode_count: u32,
    /// Block index of the first inode-table block.
    pub inode_start: u32,
    /// Block index of the first data block.
    pub data_start: u32,
    pub root_inode: u32,
    pub crc: u32,
    /// v2 only: blocks in the store (the device's block count when it was
    /// formatted). 0 on v1 — a v1 image never allocates, so it never needs a
    /// device bound beyond what the reader's cache supplies.
    pub total_blocks: u32,
    /// v2 only: block index of the allocation map (the block bitmap, then
    /// the inode bitmap, packed into `alloc_blocks` blocks). 0 on v1.
    pub alloc_start: u32,
    /// v2 only: how many 512 B blocks the allocation map spans. 0 on v1.
    pub alloc_blocks: u32,
}

/// Why a block 0 is not an aerofs image this build will mount. A named
/// refusal, not just `None`: "a v3 store is refused by name" is a clause of
/// the P1b guard, and a caller (or a test) can assert which refusal it got
/// instead of only that one occurred.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SuperblockRefusal {
    /// Block 0 does not start with "AFSL".
    Magic,
    /// A version this build does not implement (`found` is what it read).
    /// Rule: read v1, write v2, refuse the rest.
    Version { found: u32 },
    /// Not 512 B blocks.
    BlockSize,
    /// The CRC does not cover the header.
    Crc,
    /// The header parses but describes a store that cannot work (no room
    /// for the inode table, no usable root, an allocation map that cannot
    /// hold one bit per block and inode).
    Layout,
}

/// An on-disk inode (64 B). One struct carries both versions' layouts: the
/// first 14 bytes are identical, and the pointer area is version-defined —
/// v1 has 11 direct pointers then one indirect (`indirect`, `indirect2` = 0),
/// v2 has 10 direct pointers then two (`blocks[10]` unused, `indirect` =
/// indirect1, `indirect2` set). The version comes from the superblock, so a
/// struct alone never has to carry it: `parse_inode`/`encode_inode` are v1,
/// `parse_inode_v2`/`encode_inode_v2` are v2, and the fs layer dispatches.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Inode {
    pub mode: u16,
    pub uid: u16,
    pub gid: u16,
    pub size: u32,
    pub mtime: u32,
    /// Direct data block pointers; block i of the file. Unused = 0.
    pub blocks: [u32; NDIRECT],
    /// Indirect block pointer (a data block of u32 block pointers). 0 = none.
    /// v2: this is the FIRST indirect block (blocks 10..138).
    pub indirect: u32,
    /// v2 only: the SECOND indirect block (blocks 138..266). 0 = none, and
    /// always 0 for a v1 inode — v1's byte 58..62 IS `indirect`, so a v1
    /// parse must never read it twice.
    pub indirect2: u32,
}

impl Inode {
    pub fn ty(&self) -> Option<FileType> {
        match self.mode & S_IFMT {
            S_IFREG => Some(FileType::File),
            S_IFDIR => Some(FileType::Dir),
            S_IFCHR => Some(FileType::Char),
            S_IFIFO => Some(FileType::Fifo),
            _ => None,
        }
    }
    /// Number of data blocks the file occupies.
    pub fn nblocks(&self) -> u64 {
        (self.size as u64).div_ceil(aerosls_proto::BLOCK_SIZE as u64)
    }
}

/// A directory entry (64 B).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DirEntry {
    pub name: [u8; NAME_MAX],
    pub ino: u32,
    pub ty: u8,
    pub rec_len: u8,
}

impl DirEntry {
    pub fn new(name: &str, ino: u32, ty: FileType) -> Option<DirEntry> {
        let nb = name.as_bytes();
        if nb.is_empty() || nb.len() > NAME_MAX {
            return None;
        }
        let mut name_b = [0u8; NAME_MAX];
        name_b[..nb.len()].copy_from_slice(nb);
        Some(DirEntry {
            name: name_b,
            ino,
            ty: ty.dt(),
            rec_len: DIRENT_SIZE as u8,
        })
    }
    pub fn name_str(&self) -> &str {
        let end = self
            .name
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.name.len());
        core::str::from_utf8(&self.name[..end]).unwrap_or("")
    }
}

// ── little-endian field access ──────────────────────────────────────────────

#[inline]
fn u16le(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}
#[inline]
fn u32le(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}
#[inline]
fn put_u16le(b: &mut [u8], v: u16) {
    b[..2].copy_from_slice(&v.to_le_bytes());
}
#[inline]
fn put_u32le(b: &mut [u8], v: u32) {
    b[..4].copy_from_slice(&v.to_le_bytes());
}

/// The inode-table byte offset of inode `ino` (1-based).
pub fn inode_offset(ino: u32) -> usize {
    (ino as usize - 1) * INODE_SIZE
}

/// Parse a v1 inode from its 64-byte table slot.
pub fn parse_inode(slot: &[u8]) -> Option<Inode> {
    if slot.len() < INODE_SIZE {
        return None;
    }
    let mut blocks = [0u32; NDIRECT];
    for i in 0..NDIRECT {
        blocks[i] = u32le(&slot[14 + i * 4..]);
    }
    Some(Inode {
        mode: u16le(slot),
        uid: u16le(&slot[2..]),
        gid: u16le(&slot[4..]),
        size: u32le(&slot[6..]),
        mtime: u32le(&slot[10..]),
        blocks,
        indirect: u32le(&slot[58..]),
        indirect2: 0,
    })
}

/// Encode a v1 inode into its 64-byte table slot.
pub fn encode_inode(slot: &mut [u8], ino: &Inode) {
    debug_assert!(slot.len() >= INODE_SIZE);
    put_u16le(slot, ino.mode);
    put_u16le(&mut slot[2..], ino.uid);
    put_u16le(&mut slot[4..], ino.gid);
    put_u32le(&mut slot[6..], ino.size);
    put_u32le(&mut slot[10..], ino.mtime);
    for i in 0..NDIRECT {
        put_u32le(&mut slot[14 + i * 4..], ino.blocks[i]);
    }
    put_u32le(&mut slot[58..], ino.indirect);
    // bytes 62..64 reserved, already zero.
    debug_assert_eq!(ino.indirect2, 0, "a v1 inode has one indirect block");
}

/// Parse a v2 inode from its 64-byte table slot: ten direct pointers at
/// 14..54 (v1's eleventh slot, 54..58, is indirect1 here), indirect1 at
/// 54..58, indirect2 at 58..62.
pub fn parse_inode_v2(slot: &[u8]) -> Option<Inode> {
    if slot.len() < INODE_SIZE {
        return None;
    }
    let mut blocks = [0u32; NDIRECT];
    for i in 0..NDIRECT_V2 {
        blocks[i] = u32le(&slot[14 + i * 4..]);
    }
    Some(Inode {
        mode: u16le(slot),
        uid: u16le(&slot[2..]),
        gid: u16le(&slot[4..]),
        size: u32le(&slot[6..]),
        mtime: u32le(&slot[10..]),
        blocks,
        indirect: u32le(&slot[54..]),
        indirect2: u32le(&slot[58..]),
    })
}

/// Encode a v2 inode into its 64-byte table slot. Mirror of
/// `parse_inode_v2`; the two are held together by the round-trip test.
pub fn encode_inode_v2(slot: &mut [u8], ino: &Inode) {
    debug_assert!(slot.len() >= INODE_SIZE);
    put_u16le(slot, ino.mode);
    put_u16le(&mut slot[2..], ino.uid);
    put_u16le(&mut slot[4..], ino.gid);
    put_u32le(&mut slot[6..], ino.size);
    put_u32le(&mut slot[10..], ino.mtime);
    for i in 0..NDIRECT_V2 {
        put_u32le(&mut slot[14 + i * 4..], ino.blocks[i]);
    }
    put_u32le(&mut slot[54..], ino.indirect);
    put_u32le(&mut slot[58..], ino.indirect2);
    // bytes 62..64 reserved, already zero. A v1 layout slot would have put
    // indirect1 at 58..62; a v2 encoder writing the tenth direct pointer is
    // the exact drift the round-trip test and the guard's layout clause pin.
    debug_assert_eq!(ino.blocks[NDIRECT_V2], 0, "v2 has ten direct pointers");
}

/// Parse one directory entry from its 64-byte slot.
pub fn parse_dirent(slot: &[u8]) -> Option<DirEntry> {
    if slot.len() < DIRENT_SIZE {
        return None;
    }
    let mut name = [0u8; NAME_MAX];
    name.copy_from_slice(&slot[..NAME_MAX]);
    Some(DirEntry {
        name,
        ino: u32le(&slot[56..]),
        ty: slot[60],
        rec_len: slot[61],
    })
}

/// Encode one directory entry into its 64-byte slot.
pub fn encode_dirent(slot: &mut [u8], e: &DirEntry) {
    debug_assert!(slot.len() >= DIRENT_SIZE);
    slot[..NAME_MAX].copy_from_slice(&e.name);
    put_u32le(&mut slot[56..], e.ino);
    slot[60] = e.ty;
    slot[61] = e.rec_len;
    // bytes 62..63 reserved, already zero.
}

// ── superblock ──────────────────────────────────────────────────────────────

const SB_V1_FIXED_LEN: usize = 32; // magic(4) + 7 × u32
const SB_V1_CRC_OFF: usize = 28;
// v2's fixed header is 16 bytes longer: after root_inode come total_blocks,
// alloc_start, alloc_blocks and a reserved word, and the CRC sits at 44 so
// it covers all of them.
const SB_V2_FIXED_LEN: usize = 48;
const SB_V2_CRC_OFF: usize = 44;

/// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320).
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

/// Validate a raw superblock block and parse it. `Err` names the refusal —
/// including a version this build does not implement, which is the refusal
/// the version rule requires to be visible rather than folded into "not an
/// image".
pub fn parse_superblock_refusing(block: &[u8]) -> Result<Superblock, SuperblockRefusal> {
    if block.len() < aerosls_proto::BLOCK_SIZE as usize || &block[..4] != &AEROFS_MAGIC {
        return Err(SuperblockRefusal::Magic);
    }
    let version = u32le(&block[4..]);
    let (fixed_len, crc_off, total_blocks, alloc_start, alloc_blocks) = match version {
        AEROFS_VERSION_V1 => (SB_V1_FIXED_LEN, SB_V1_CRC_OFF, 0, 0, 0),
        AEROFS_VERSION_V2 => (
            SB_V2_FIXED_LEN,
            SB_V2_CRC_OFF,
            u32le(&block[28..]),
            u32le(&block[32..]),
            u32le(&block[36..]),
        ),
        found => return Err(SuperblockRefusal::Version { found }),
    };
    if u32le(&block[8..]) != aerosls_proto::BLOCK_SIZE {
        return Err(SuperblockRefusal::BlockSize);
    }
    let crc = u32le(&block[crc_off..]);
    let mut zeroed = [0u8; SB_V2_FIXED_LEN];
    zeroed[..fixed_len].copy_from_slice(&block[..fixed_len]);
    zeroed[crc_off..crc_off + 4].copy_from_slice(&0u32.to_le_bytes());
    if crc32(&zeroed[..fixed_len]) != crc {
        return Err(SuperblockRefusal::Crc);
    }
    let sb = Superblock {
        version,
        block_size: u32le(&block[8..]),
        inode_count: u32le(&block[12..]),
        inode_start: u32le(&block[16..]),
        data_start: u32le(&block[20..]),
        root_inode: u32le(&block[24..]),
        crc,
        total_blocks,
        alloc_start,
        alloc_blocks,
    };
    // Sanity: sane layout, room for the table, a usable root.
    if sb.inode_count < 2
        || sb.inode_start < 1
        || sb.data_start < sb.inode_start + sb.inode_count.div_ceil(8)
        || sb.root_inode < 2
        || sb.root_inode > sb.inode_count
    {
        return Err(SuperblockRefusal::Layout);
    }
    if sb.version == AEROFS_VERSION_V2 {
        // A writable store's allocation map must be real: non-empty, inside
        // the store, and wide enough for one bit per block plus one per inode
        // slot. A map that cannot hold those bits makes every allocation
        // arithmetic silently wrong, which is worse than refusing the mount.
        let need_bits = sb.total_blocks as u64 + sb.inode_count as u64;
        if sb.alloc_start < 1
            || sb.alloc_blocks == 0
            || sb.total_blocks < sb.data_start + 1
            || sb.alloc_blocks as u64 * aerosls_proto::BLOCK_SIZE as u64 * 8 < need_bits
        {
            return Err(SuperblockRefusal::Layout);
        }
    }
    Ok(sb)
}

/// Validate a raw superblock block and parse it. Returns `None` on any
/// mismatch (bad magic/version/block size/CRC/layout) — the caller treats
/// that as "this is not an aerofs-lite image". Callers that need to say WHICH
/// mismatch (the guard, the version rule) call `parse_superblock_refusing`.
pub fn parse_superblock(block: &[u8]) -> Option<Superblock> {
    parse_superblock_refusing(block).ok()
}

// ── v2 formatting (the writer's half of the version rule) ───────────────────

/// Bytes the v2 block bitmap occupies at the start of the allocation map
/// (one bit per block, LSB-first). The inode bitmap follows it, one bit per
/// inode slot indexed by ino (bits 0 and 1 are the two unused slots).
pub fn v2_block_bitmap_len(total_blocks: u32) -> usize {
    ((total_blocks as u64 + 7) / 8) as usize
}

/// The v2 layout a store of `total_blocks` blocks and `inode_count` inode
/// slots gets: (alloc_start, alloc_blocks, inode_start, data_start). None
/// when the store is too small to hold the format. Pure arithmetic, so the
/// formatter, the parser's sanity check and the guard's layout clause all
/// agree by construction rather than by three parallel copies of the sum.
pub fn v2_layout(total_blocks: u32, inode_count: u32) -> Option<(u32, u32, u32, u32)> {
    let bs = aerosls_proto::BLOCK_SIZE;
    let table_blocks = (inode_count * INODE_SIZE as u32).div_ceil(bs);
    // A bit per block, plus a bit per inode slot 0..=inode_count.
    let map_bits = total_blocks as u64 + inode_count as u64 + 1;
    let map_bytes = (map_bits + 7) / 8;
    let alloc_blocks = (map_bytes as u32).div_ceil(bs);
    let alloc_start = 1; // right after the superblock
    let inode_start = alloc_start + alloc_blocks;
    let data_start = inode_start + table_blocks;
    // The root directory needs at least one data block of its own.
    if inode_count < 2 || total_blocks <= data_start + 1 {
        return None;
    }
    Some((alloc_start, alloc_blocks, inode_start, data_start))
}

/// Format an empty store as a raw v2 image: superblock, allocation map,
/// inode table (one root directory inode), and the root directory's one data
/// block. `total_blocks` is the store's size in 512 B blocks — the device's
/// own block count, which is what bounds v2's allocator. None when the store
/// cannot hold the format (the caller refuses the mount rather than writing a
/// partial image).
///
/// The returned image is `total_blocks` blocks long but almost entirely
/// zero: the caller writes only the non-zero blocks, which keeps a format of
/// a 1 MiB store at ~11 block writes instead of 2,048 — the tenant boot path
/// formats a store per environment, and this is the difference between an
/// eleven-request and a two-thousand-request mount. That is safe because
/// every block the allocation map reports FREE is either unwritten or
/// zeroed when it is allocated (`AerofsFs::alloc_block`), so stale bytes left
/// in a reused store are never reachable through the filesystem.
pub fn format_v2(total_blocks: u32) -> Option<Vec<u8>> {
    let bs = aerosls_proto::BLOCK_SIZE as usize;
    let inode_count = FORMAT_INODE_COUNT;
    let (alloc_start, alloc_blocks, inode_start, data_start) =
        v2_layout(total_blocks, inode_count)?;
    let root_block = data_start; // the empty root directory's one block
    let mut img = vec![0u8; total_blocks as usize * bs];

    // Superblock.
    let mut sb = [0u8; SB_V2_FIXED_LEN];
    sb[..4].copy_from_slice(&AEROFS_MAGIC);
    put_u32le(&mut sb[4..], AEROFS_VERSION_V2);
    put_u32le(&mut sb[8..], bs as u32);
    put_u32le(&mut sb[12..], inode_count);
    put_u32le(&mut sb[16..], inode_start);
    put_u32le(&mut sb[20..], data_start);
    put_u32le(&mut sb[24..], 2); // the root inode is always 2
    put_u32le(&mut sb[28..], total_blocks);
    put_u32le(&mut sb[32..], alloc_start);
    put_u32le(&mut sb[36..], alloc_blocks);
    let crc = crc32(&sb);
    put_u32le(&mut sb[SB_V2_CRC_OFF..], crc);
    img[..SB_V2_FIXED_LEN].copy_from_slice(&sb);

    // The root inode (ino 2): a directory with one data block, holding just
    // "." and ".." (both the root itself).
    let mut root = Inode {
        mode: S_IFDIR | 0o755,
        uid: 0,
        gid: 0,
        size: (2 * DIRENT_SIZE) as u32,
        mtime: 1,
        blocks: [0; NDIRECT],
        indirect: 0,
        indirect2: 0,
    };
    root.blocks[0] = root_block;
    let table_off = inode_start as usize * bs + inode_offset(2);
    encode_inode_v2(&mut img[table_off..table_off + INODE_SIZE], &root);
    let rb = root_block as usize * bs;
    let mut dot = [0u8; DIRENT_SIZE];
    encode_dirent(&mut dot, &DirEntry::new(".", 2, FileType::Dir).unwrap());
    let mut dotdot = [0u8; DIRENT_SIZE];
    encode_dirent(&mut dotdot, &DirEntry::new("..", 2, FileType::Dir).unwrap());
    img[rb..rb + DIRENT_SIZE].copy_from_slice(&dot);
    img[rb + DIRENT_SIZE..rb + 2 * DIRENT_SIZE].copy_from_slice(&dotdot);

    // Allocation map: every block through the root directory's is metadata or
    // the root itself, and inode slots 0, 1 (never used) and 2 (root) are
    // taken. Everything else is free.
    let bb_len = v2_block_bitmap_len(total_blocks);
    let map_off = alloc_start as usize * bs;
    let map_len = alloc_blocks as usize * bs;
    {
        let map = &mut img[map_off..map_off + map_len];
        for b in 0..=root_block as u64 {
            bitmap_set(&mut map[..bb_len], b);
        }
        for i in 0..=2u64 {
            bitmap_set(&mut map[bb_len..], i);
        }
    }
    Some(img)
}

/// The record a mount keeps for revalidation (respawn decision §5.9b):
/// identity of the image behind the device name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SuperblockRecord {
    pub magic: [u8; 4],
    pub block_size: u32,
    pub block_count: u64,
    pub sb_crc: u32,
}

impl SuperblockRecord {
    pub fn matches(&self, sb: &Superblock, block_count: u64) -> bool {
        self.magic == AEROFS_MAGIC
            && self.block_size == sb.block_size
            && self.block_count == block_count
            && self.sb_crc == sb.crc
    }
}

// ── image builder (the host-side genrootfs tool) ────────────────────────────

/// An image under construction: a tree of files and directories.
#[derive(Clone, Debug)]
pub struct ImageBuilder {
    nodes: Vec<(String, BuildNode)>,
    root: u32,
}

#[derive(Clone, Debug)]
struct BuildNode {
    ino: u32,
    inode: Inode,
    /// File data (files), or the raw directory-entry bytes (dirs), filled
    /// at build time.
    data: Vec<u8>,
}

impl ImageBuilder {
    pub fn new() -> ImageBuilder {
        let mut b = ImageBuilder {
            nodes: Vec::new(),
            root: 0,
        };
        b.root = b.insert_node(
            "/",
            Inode {
                mode: S_IFDIR | 0o755,
                uid: 0,
                gid: 0,
                size: 0,
                mtime: 1,
                blocks: [0; NDIRECT],
                indirect: 0,
                indirect2: 0,
            },
        );
        b
    }

    /// Add a directory. Missing parents are created implicitly.
    pub fn add_dir(&mut self, path: &str, mode: u16) -> &mut Self {
        self.ensure_parents(path);
        self.insert_node(
            path,
            Inode {
                mode: S_IFDIR | (mode & 0o7777),
                uid: 0,
                gid: 0,
                size: 0,
                mtime: 1,
                blocks: [0; NDIRECT],
                indirect: 0,
                indirect2: 0,
            },
        );
        self
    }

    /// Add a file with `data`. Missing parents are created implicitly.
    pub fn add_file(&mut self, path: &str, data: &[u8], mode: u16) -> &mut Self {
        self.ensure_parents(path);
        let ino = self.insert_node(
            path,
            Inode {
                mode: S_IFREG | (mode & 0o7777),
                uid: 0,
                gid: 0,
                size: data.len() as u32,
                mtime: 1,
                blocks: [0; NDIRECT],
                indirect: 0,
                indirect2: 0,
            },
        );
        self.node_mut(ino).data = data.to_vec();
        self
    }

    /// Resolve a path to its inode.
    pub fn resolve(&self, path: &str) -> Option<u32> {
        let norm = normalize_path(path);
        self.nodes
            .iter()
            .find(|(p, _)| p.as_str() == norm)
            .map(|(_, n)| n.ino)
    }

    /// Build the image: returns the raw 512-byte-block image.
    ///
    /// Layout: superblock (block 0), inode table (inode_count × 64 B,
    /// block-aligned), then data blocks (each inode's data in ino order,
    /// block-aligned). Directory data is built here from the child links.
    pub fn build(&mut self) -> Vec<u8> {
        self.nodes.sort_by_key(|(_, n)| n.ino);
        let inode_count = self
            .nodes
            .iter()
            .map(|(_, n)| n.ino)
            .max()
            .unwrap_or(2);
        // Pass 1 (immutable): compute each directory's entry bytes.
        let dir_data: Vec<(u32, Vec<u8>)> = self
            .nodes
            .iter()
            .filter(|(_, n)| n.inode.ty() == Some(FileType::Dir))
            .map(|(path, n)| {
                let mut entries = Vec::new();
                entries.push(DirEntry::new(".", n.ino, FileType::Dir).unwrap());
                entries.push(DirEntry::new("..", self.dotdot(path, n.ino), FileType::Dir).unwrap());
                for (cp, c) in self.nodes.iter() {
                    // parent_path("/") == "/", so exclude the node itself.
                    if cp != path && parent_path(cp) == *path {
                        let ty = c.inode.ty().unwrap();
                        entries.push(DirEntry::new(&last_comp(cp), c.ino, ty).unwrap());
                    }
                }
                let mut data = Vec::new();
                for e in &entries {
                    let mut slot = [0u8; DIRENT_SIZE];
                    encode_dirent(&mut slot, e);
                    data.extend_from_slice(&slot);
                }
                (n.ino, data)
            })
            .collect();
        // Pass 2 (mutable): apply directory sizes and contents.
        for (ino, data) in dir_data {
            let node = self.node_mut(ino);
            node.inode.size = data.len() as u32;
            node.data = data;
        }

        let bs = aerosls_proto::BLOCK_SIZE as usize;
        let inode_start: u32 = 1;
        let table_blocks = (inode_count * INODE_SIZE as u32).div_ceil(bs as u32);
        let data_start = inode_start + table_blocks;

        // Assign data block pointers in ino order.
        let mut next_block = data_start;
        for (_, n) in self.nodes.iter_mut() {
            let nb = (n.data.len() as u64).div_ceil(bs as u64);
            debug_assert!(nb <= MAX_BLOCKS, "file exceeds 11+128 blocks");
            let mut blocks = [0u32; NDIRECT];
            for i in 0..NDIRECT.min(nb as usize) {
                blocks[i] = next_block + i as u32;
            }
            n.inode.blocks = blocks;
            // The indirect block sits right after the direct run; its u32
            // pointer list covers blocks NDIRECT..nb and is filled at
            // emission time (each pointer = indirect + 1 + idx). The file's
            // total disk footprint is nb + 1 blocks (direct run + indirect).
            n.inode.indirect = if nb > NDIRECT as u64 {
                next_block + NDIRECT as u32
            } else {
                0
            };
            next_block += nb as u32 + if n.inode.indirect != 0 { 1 } else { 0 };
        }

        let total_blocks = next_block;
        let mut img = vec![0u8; total_blocks as usize * bs];

        // Superblock.
        let mut sb_raw = [0u8; SB_V1_FIXED_LEN];
        sb_raw[..4].copy_from_slice(&AEROFS_MAGIC);
        // The builder writes v1, deliberately: every existing image is v1,
        // the system rootfs is one, and a built image is mounted read-only.
        // v2 is what `format_v2` writes for a store the fs layer will modify.
        put_u32le(&mut sb_raw[4..], AEROFS_VERSION_V1);
        put_u32le(&mut sb_raw[8..], bs as u32);
        put_u32le(&mut sb_raw[12..], inode_count);
        put_u32le(&mut sb_raw[16..], inode_start);
        put_u32le(&mut sb_raw[20..], data_start);
        put_u32le(&mut sb_raw[24..], self.root);
        let crc = crc32(&sb_raw);
        put_u32le(&mut sb_raw[SB_V1_CRC_OFF..], crc);
        img[..SB_V1_FIXED_LEN].copy_from_slice(&sb_raw);

        // Inode table.
        for (_, n) in self.nodes.iter() {
            let off = inode_offset(n.ino);
            let slot = &mut img[inode_start as usize * bs + off..][..INODE_SIZE];
            encode_inode(slot, &n.inode);
        }

        // Data blocks.
        for (_, n) in self.nodes.iter() {
            let nblocks = (n.data.len() as u64).div_ceil(bs as u64);
            let mut copied = 0usize;
            for i in 0..nblocks {
                let bi = if i < NDIRECT as u64 {
                    n.inode.blocks[i as usize]
                } else {
                    n.inode.indirect + 1 + (i - NDIRECT as u64) as u32
                };
                let dst = &mut img[bi as usize * bs..][..bs];
                let take = core::cmp::min(bs, n.data.len() - copied);
                dst[..take].copy_from_slice(&n.data[copied..copied + take]);
                copied += take;
            }
            if n.inode.indirect != 0 {
                // Fill the indirect block: u32 pointers for blocks NDIRECT..nb.
                let off = n.inode.indirect as usize * bs;
                for i in NDIRECT as u64..nblocks {
                    let ptr = n.inode.indirect + 1 + (i - NDIRECT as u64) as u32;
                    let p = (i - NDIRECT as u64) as usize * 4;
                    img[off + p..off + p + 4].copy_from_slice(&ptr.to_le_bytes());
                }
            }
        }

        img
    }

    // ── internals ─────────────────────────────────────────────────────────────

    fn insert_node(&mut self, path: &str, inode: Inode) -> u32 {
        let ino = (self.nodes.len() + 2) as u32; // ino 1 reserved, root = 2
        self.nodes.push((
            normalize_path(path),
            BuildNode {
                ino,
                inode,
                data: Vec::new(),
            },
        ));
        ino
    }

    fn node_mut(&mut self, ino: u32) -> &mut BuildNode {
        self.nodes
            .iter_mut()
            .find(|(_, n)| n.ino == ino)
            .map(|(_, n)| n)
            .expect("inode exists")
    }

    /// Create any missing parent directories of `path`.
    fn ensure_parents(&mut self, path: &str) {
        let norm = normalize_path(path);
        let comps: Vec<&str> = norm
            .trim_start_matches('/')
            .split('/')
            .filter(|c| !c.is_empty())
            .collect();
        let mut acc = String::from("/");
        for c in comps.iter().take(comps.len().saturating_sub(1)) {
            acc.push_str(c);
            if self.resolve(&acc).is_none() {
                self.insert_node(
                    &acc,
                    Inode {
                        mode: S_IFDIR | 0o755,
                        uid: 0,
                        gid: 0,
                        size: 0,
                        mtime: 1,
                        blocks: [0; NDIRECT],
                        indirect: 0,
                        indirect2: 0,
                    },
                );
            }
            acc.push('/');
        }
    }

    fn dotdot(&self, path: &str, ino: u32) -> u32 {
        if path == "/" {
            ino
        } else {
            self.resolve(&parent_path(path)).unwrap_or(self.root)
        }
    }
}

// ── path helpers (shared with vfs.rs) ───────────────────────────────────────

/// Collapse a path: resolve "." and ".." (never above root), strip trailing
/// slashes, keep a leading slash.
pub fn normalize_path(path: &str) -> String {
    let mut out: Vec<&str> = Vec::new();
    for c in path.split('/') {
        match c {
            "" | "." => {}
            ".." => {
                out.pop();
            }
            c => out.push(c),
        }
    }
    let mut s = String::from("/");
    s.push_str(&out.join("/"));
    s
}

pub fn parent_path(path: &str) -> String {
    let norm = normalize_path(path);
    if norm == "/" {
        return String::from("/");
    }
    let idx = norm.rfind('/').unwrap();
    if idx == 0 {
        String::from("/")
    } else {
        norm[..idx].to_string()
    }
}

pub fn last_comp(path: &str) -> String {
    let norm = normalize_path(path);
    if norm == "/" {
        String::new()
    } else {
        norm.rsplit('/').next().unwrap().to_string()
    }
}

/// Split a normalized absolute path into components ("" for root).
pub fn path_comps(path: &str) -> Vec<&str> {
    path.split('/').filter(|c| !c.is_empty()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_known_vector() {
        // IEEE 802.3 check value for "123456789".
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn inode_roundtrip() {
        let ino = Inode {
            mode: S_IFREG | 0o644,
            uid: 0,
            gid: 0,
            size: 1234,
            mtime: 99,
            blocks: [3, 4, 5, 0, 0, 0, 0, 0, 0, 0, 0],
            indirect: 0,
            indirect2: 0,
        };
        let mut slot = [0u8; INODE_SIZE];
        encode_inode(&mut slot, &ino);
        assert_eq!(parse_inode(&slot), Some(ino));
        // Layout sanity: field offsets per the doc.
        assert_eq!(&slot[0..2], &0o100644u16.to_le_bytes());
        assert_eq!(&slot[14..18], &3u32.to_le_bytes());
        assert_eq!(&slot[58..62], &0u32.to_le_bytes());
        assert_eq!(slot[62..64], [0, 0], "reserved bytes stay zero");
    }

    #[test]
    fn dirent_roundtrip() {
        let e = DirEntry::new("passwd", 7, FileType::File).unwrap();
        assert_eq!(e.name_str(), "passwd");
        let mut slot = [0u8; DIRENT_SIZE];
        encode_dirent(&mut slot, &e);
        let p = parse_dirent(&slot).unwrap();
        assert_eq!(p, e);
        assert_eq!(p.name_str(), "passwd");
        assert_eq!(slot[56..60], 7u32.to_le_bytes());
        assert_eq!(slot[60], DT_REG);
        assert_eq!(slot[62..64], [0, 0]);
        assert!(DirEntry::new(&"x".repeat(NAME_MAX + 1), 1, FileType::File).is_none());
    }

    #[test]
    fn normalize_path_cases() {
        assert_eq!(normalize_path("/"), "/");
        assert_eq!(normalize_path("//"), "/");
        assert_eq!(normalize_path("/etc/passwd"), "/etc/passwd");
        assert_eq!(normalize_path("etc/./passwd"), "/etc/passwd");
        assert_eq!(normalize_path("/a/b/../c"), "/a/c");
        assert_eq!(normalize_path("../../.."), "/");
        assert_eq!(normalize_path("/tmp/"), "/tmp");
        assert_eq!(parent_path("/etc/passwd"), "/etc");
        assert_eq!(parent_path("/etc"), "/");
        assert_eq!(parent_path("/"), "/");
        assert_eq!(last_comp("/etc/passwd"), "passwd");
        assert_eq!(last_comp("/"), "");
        assert_eq!(path_comps("/"), Vec::<&str>::new());
        assert_eq!(path_comps("/a/b"), vec!["a", "b"]);
    }

    fn sample_image() -> (Vec<u8>, u32) {
        let mut b = ImageBuilder::new();
        b.add_dir("/etc", 0o755);
        b.add_dir("/bin", 0o755);
        b.add_file("/etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n", 0o644);
        b.add_file("/bin/busybox", &[0x7f; 6000], 0o755);
        let root = b.root;
        (b.build(), root)
    }

    #[test]
    fn image_parses_and_layout_is_sane() {
        let (img, root) = sample_image();
        let sb = parse_superblock(&img[..512]).expect("superblock must parse");
        assert_eq!(sb.root_inode, root);
        assert_eq!(sb.inode_count, 6); // root, etc, bin, passwd, busybox (+1)
        assert_eq!(sb.data_start, 1 + sb.inode_count.div_ceil(8));
        // Inode table fits before data_start.
        assert!(sb.data_start as usize * 512 >= 512 + sb.inode_count as usize * 64);
        // Root exists and is a directory.
        let table = &img[sb.inode_start as usize * 512..];
        let root_inode = parse_inode(&table[inode_offset(root)..][..INODE_SIZE]).unwrap();
        assert_eq!(root_inode.ty(), Some(FileType::Dir));
    }

    #[test]
    fn image_block_pointers_land_in_data_region() {
        let (img, _) = sample_image();
        let sb = parse_superblock(&img[..512]).unwrap();
        let table = &img[sb.inode_start as usize * 512..];
        for ino in 2..=sb.inode_count {
            let rec = parse_inode(&table[inode_offset(ino)..][..INODE_SIZE]).unwrap();
            let nb = rec.nblocks();
            for i in 0..NDIRECT.min(nb as usize) {
                let b = rec.blocks[i];
                assert!(b >= sb.data_start, "direct block in data region");
                assert!(b as usize * 512 < img.len(), "direct block in image");
            }
            if rec.indirect != 0 {
                assert!(rec.indirect >= sb.data_start);
                assert!(rec.indirect as usize * 512 < img.len());
                // The indirect block holds pointers for the remaining blocks,
                // each into the data region.
                let off = rec.indirect as usize * 512;
                for i in NDIRECT as u64..nb {
                    let p = (i - NDIRECT as u64) as usize * 4;
                    let ptr = u32::from_le_bytes([
                        img[off + p],
                        img[off + p + 1],
                        img[off + p + 2],
                        img[off + p + 3],
                    ]);
                    assert!(ptr >= sb.data_start, "indirect pointer in data region");
                }
            }
        }
    }

    #[test]
    fn image_dirs_contain_children_and_dotentries() {
        let (img, _) = sample_image();
        let sb = parse_superblock(&img[..512]).unwrap();
        let table = &img[sb.inode_start as usize * 512..];
        // /etc is ino 3 (root=2, then etc, bin, passwd, busybox).
        let etc = parse_inode(&table[inode_offset(3)..][..INODE_SIZE]).unwrap();
        assert_eq!(etc.ty(), Some(FileType::Dir));
        let dir_bytes = &img[etc.blocks[0] as usize * 512..][..etc.size as usize];
        let names: Vec<String> = dir_bytes
            .chunks(DIRENT_SIZE)
            .filter_map(parse_dirent)
            .map(|e| e.name_str().to_string())
            .collect();
        assert!(names.contains(&"passwd".to_string()), "etc lists passwd: {names:?}");
        assert!(names.contains(&".".to_string()));
        assert!(names.contains(&"..".to_string()));
        let dotdot = dir_bytes
            .chunks(DIRENT_SIZE)
            .filter_map(parse_dirent)
            .find(|e| e.name_str() == "..")
            .unwrap();
        assert_eq!(dotdot.ino, 2, "/etc/.. must be root");
    }

    #[test]
    fn file_spanning_two_blocks_reads_back() {
        let mut b = ImageBuilder::new();
        b.add_file("/big", &vec![0xAB; 700], 0o644); // 700 B → 2 blocks
        let img = b.build();
        let sb = parse_superblock(&img[..512]).unwrap();
        let table = &img[sb.inode_start as usize * 512..];
        let big = parse_inode(&table[inode_offset(3)..][..INODE_SIZE]).unwrap();
        assert_eq!(big.nblocks(), 2);
        let b0 = &img[big.blocks[0] as usize * 512..][..512];
        let b1 = &img[big.blocks[1] as usize * 512..][..512];
        assert_eq!(&b0[..], &[0xAB; 512]);
        assert_eq!(&b1[..188], &[0xAB; 188]);
    }

    #[test]
    fn file_over_eleven_blocks_uses_indirect() {
        let mut b = ImageBuilder::new();
        // 12 blocks (6144 B) → 11 direct + 1 indirect.
        b.add_file("/big", &vec![0xCD; 12 * 512], 0o644);
        let img = b.build();
        let sb = parse_superblock(&img[..512]).unwrap();
        let table = &img[sb.inode_start as usize * 512..];
        let big = parse_inode(&table[inode_offset(3)..][..INODE_SIZE]).unwrap();
        assert_eq!(big.nblocks(), 12);
        assert_ne!(big.indirect, 0, "block 11 must go through the indirect block");
        // Reassemble the file through the pointer list and check contents.
        let mut reassembled = Vec::new();
        for i in 0..12u64 {
            let bi = if i < NDIRECT as u64 {
                big.blocks[i as usize]
            } else {
                let p = (i - NDIRECT as u64) as usize * 4;
                let off = big.indirect as usize * 512 + p;
                u32::from_le_bytes([
                    img[off],
                    img[off + 1],
                    img[off + 2],
                    img[off + 3],
                ])
            };
            reassembled.extend_from_slice(&img[bi as usize * 512..][..512]);
        }
        assert!(reassembled.iter().all(|&b| b == 0xCD));
    }

    #[test]
    fn empty_dir_has_just_dot_and_dotdot() {
        let mut b = ImageBuilder::new();
        b.add_dir("/tmp", 0o777);
        let img = b.build();
        let sb = parse_superblock(&img[..512]).unwrap();
        let table = &img[sb.inode_start as usize * 512..];
        let tmp = parse_inode(&table[inode_offset(3)..][..INODE_SIZE]).unwrap();
        assert_eq!(tmp.size as usize, 2 * DIRENT_SIZE);
        assert_eq!(tmp.ty(), Some(FileType::Dir));
    }

    // ── v2: the writable format (P1b part 1) ─────────────────────────────────

    #[test]
    fn v2_inode_roundtrip_and_field_offsets() {
        let ino = Inode {
            mode: S_IFREG | 0o644,
            uid: 7,
            gid: 9,
            size: MAX_FILE_BYTES_V2 as u32,
            mtime: 12345,
            blocks: [10, 11, 12, 0, 0, 0, 0, 0, 0, 0, 0],
            indirect: 40,
            indirect2: 41,
        };
        let mut slot = [0u8; INODE_SIZE];
        encode_inode_v2(&mut slot, &ino);
        assert_eq!(parse_inode_v2(&slot), Some(ino));
        // The layout, byte-pinned: 54..58 is indirect1 in v2 (v1's eleventh
        // direct pointer lives there), 58..62 is indirect2 (v1's indirect).
        assert_eq!(&slot[54..58], &40u32.to_le_bytes());
        assert_eq!(&slot[58..62], &41u32.to_le_bytes());
        assert_eq!(slot[62..64], [0, 0]);
        // Reading the same bytes as v1 shows why the version gates the
        // parse: v1's single indirect lives at 58..62, so it would follow
        // 41 — v2's SECOND indirect — as its one indirect block and stop at
        // block 139. That mis-read is the drift the guard's layout clause
        // pins, and it is silent without the version check.
        let as_v1 = parse_inode(&slot).unwrap();
        assert_eq!(as_v1.indirect, 41);
        assert_eq!(as_v1.indirect2, 0, "v1 has no second indirect pointer");
    }

    #[test]
    fn v2_ceilings_moved() {
        assert_eq!(MAX_BLOCKS_V2, 266);
        assert_eq!(MAX_FILE_BYTES_V1, 71_168);
        assert_eq!(MAX_FILE_BYTES_V2, 136_192);
        assert!(MAX_FILE_BYTES_V2 > MAX_FILE_BYTES_V1, "the format's ceiling moved");
        assert!(MAX_FILE_BYTES_V2 > 71_200, "the guard's large file fits in one file");
    }

    #[test]
    fn bitmap_helpers() {
        let mut bits = [0u8; 4];
        for i in [0u64, 3, 9, 15, 16, 31] {
            assert!(!bitmap_get(&bits, i));
            bitmap_set(&mut bits, i);
            assert!(bitmap_get(&bits, i));
        }
        assert_eq!(bitmap_find_clear(&bits, 0, 32), Some(1));
        bitmap_clear(&mut bits, 1);
        assert_eq!(bitmap_find_clear(&bits, 0, 32), Some(1));
        assert_eq!(bitmap_find_clear(&bits, 0, 1), None);
        assert_eq!(bitmap_find_clear(&bits, 2, 2), None);
    }

    #[test]
    fn v2_format_layout_and_allocation_state() {
        // The first-cut tenant store: 1 MiB, 2,048 blocks.
        let total = 2048u32;
        let img = format_v2(total).expect("a 1 MiB store formats");
        let sb = parse_superblock(&img[..512]).expect("the v2 superblock parses");
        assert_eq!(sb.version, AEROFS_VERSION_V2);
        assert_eq!(sb.total_blocks, total);
        assert_eq!(sb.inode_count, FORMAT_INODE_COUNT);
        let (alloc_start, alloc_blocks, inode_start, data_start) =
            v2_layout(total, FORMAT_INODE_COUNT).unwrap();
        assert_eq!(
            (sb.alloc_start, sb.alloc_blocks, sb.inode_start, sb.data_start),
            (alloc_start, alloc_blocks, inode_start, data_start)
        );
        // Root: a directory whose one block holds "." and "..".
        let table = &img[inode_start as usize * 512..];
        let root = parse_inode_v2(&table[inode_offset(2)..][..INODE_SIZE]).unwrap();
        assert_eq!(root.ty(), Some(FileType::Dir));
        assert_eq!(root.size as usize, 2 * DIRENT_SIZE);
        let rb = root.blocks[0] as usize * 512;
        let dot = parse_dirent(&img[rb..rb + DIRENT_SIZE]).unwrap();
        let dotdot = parse_dirent(&img[rb + DIRENT_SIZE..rb + 2 * DIRENT_SIZE]).unwrap();
        assert_eq!((dot.name_str(), dot.ino), (".", 2));
        assert_eq!((dotdot.name_str(), dotdot.ino), ("..", 2));
        // The map: metadata and root marked used, the next block free.
        let map_off = alloc_start as usize * 512;
        let bb = v2_block_bitmap_len(total);
        let map = &img[map_off..map_off + alloc_blocks as usize * 512];
        for b in 0..=data_start as u64 {
            assert!(bitmap_get(&map[..bb], b), "block {b} is metadata or the root");
        }
        assert!(!bitmap_get(&map[..bb], data_start as u64 + 1));
        for i in 0..=2u64 {
            assert!(bitmap_get(&map[bb..], i), "inode slot {i} is used");
        }
        assert!(!bitmap_get(&map[bb..], 3));
        // The image is almost all zeroes — the mount writes only the non-zero
        // blocks, which is what keeps a 1 MiB format at ~11 writes.
        let nonzero = img.chunks(512).filter(|c| c.iter().any(|&b| b != 0)).count();
        assert!(nonzero <= 16, "a fresh format writes {nonzero} blocks, not {total}");
    }

    #[test]
    fn version_rule_refusals_are_named() {
        // v3 is refused BY NAME, not folded into "not an image".
        let mut img = format_v2(2048).unwrap();
        put_u32le(&mut img[4..], 3);
        assert_eq!(
            parse_superblock_refusing(&img[..512]),
            Err(SuperblockRefusal::Version { found: 3 })
        );
        assert!(parse_superblock(&img[..512]).is_none());
        // A v1 image (what the builder writes) still parses as v1: the read
        // half of the rule, exercised on every existing image in the tree.
        let v1 = ImageBuilder::new().build();
        assert_eq!(parse_superblock(&v1[..512]).unwrap().version, AEROFS_VERSION_V1);
        // A corrupt CRC and a corrupt magic each refuse by their own name.
        let mut bad = format_v2(2048).unwrap();
        bad[SB_V2_CRC_OFF] ^= 0xFF;
        assert_eq!(parse_superblock_refusing(&bad[..512]), Err(SuperblockRefusal::Crc));
        let mut bad = format_v2(2048).unwrap();
        bad[0] = b'X';
        assert_eq!(parse_superblock_refusing(&bad[..512]), Err(SuperblockRefusal::Magic));
        // A store too small to hold the format is not formatted at all.
        assert!(format_v2(8).is_none());
    }

    #[test]
    fn builder_still_writes_v1() {
        // genrootfs' output (the system rootfs) must not silently become v2:
        // a built image is read-only by the version rule.
        let img = ImageBuilder::new().build();
        assert_eq!(u32le(&img[4..]), AEROFS_VERSION_V1);
    }
}
