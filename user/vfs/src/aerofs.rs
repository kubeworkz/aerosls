//! aerofs-lite — the read-only root filesystem format (Phase 2 design §6.3).
//!
//! The on-disk layout, pinned exactly:
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
//! This module is pure (no kernel, no I/O): parsing/encoding the format
//! and building images. The VFS layer (`vfs.rs`) drives it through the
//! block cache. The builder is the host-side `genrootfs` tool
//! (implementation plan §7) made testable.

use alloc::string::String;
use alloc::string::ToString;
use alloc::vec;
use alloc::vec::Vec;

/// The magic bytes "AFSL" (block 0, offset 0).
pub const AEROFS_MAGIC: [u8; 4] = *b"AFSL";
pub const AEROFS_VERSION: u32 = 1;

pub const INODE_SIZE: usize = 64;
pub const DIRENT_SIZE: usize = 64;
pub const NAME_MAX: usize = 56;
/// Direct block pointers per inode (11 × u32), plus one indirect block.
pub const NDIRECT: usize = 11;
/// Pointers per indirect block (512 / 4).
pub const NINDIRECT: usize = 128;
/// Max blocks per file.
pub const MAX_BLOCKS: u64 = (NDIRECT + NINDIRECT) as u64;

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

/// The superblock (block 0).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Superblock {
    pub block_size: u32,
    pub inode_count: u32,
    /// Block index of the first inode-table block.
    pub inode_start: u32,
    /// Block index of the first data block.
    pub data_start: u32,
    pub root_inode: u32,
    pub crc: u32,
}

/// An on-disk inode (64 B).
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
    pub indirect: u32,
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

/// Parse an inode from its 64-byte table slot.
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
    })
}

/// Encode an inode into its 64-byte table slot.
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

const SB_FIXED_LEN: usize = 32; // magic(4) + 7 × u32
const SB_CRC_OFF: usize = 28;

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

/// Validate a raw superblock block and parse it. Returns `None` on any
/// mismatch (bad magic/version/block size/CRC) — the caller treats that as
/// "this is not an aerofs-lite image".
pub fn parse_superblock(block: &[u8]) -> Option<Superblock> {
    if block.len() < aerosls_proto::BLOCK_SIZE as usize || &block[..4] != &AEROFS_MAGIC {
        return None;
    }
    if u32le(&block[4..]) != AEROFS_VERSION || u32le(&block[8..]) != aerosls_proto::BLOCK_SIZE {
        return None;
    }
    let crc = u32le(&block[SB_CRC_OFF..]);
    let mut zeroed = [0u8; SB_FIXED_LEN];
    zeroed.copy_from_slice(&block[..SB_FIXED_LEN]);
    zeroed[SB_CRC_OFF..SB_CRC_OFF + 4].copy_from_slice(&0u32.to_le_bytes());
    if crc32(&zeroed) != crc {
        return None;
    }
    let sb = Superblock {
        block_size: u32le(&block[8..]),
        inode_count: u32le(&block[12..]),
        inode_start: u32le(&block[16..]),
        data_start: u32le(&block[20..]),
        root_inode: u32le(&block[24..]),
        crc,
    };
    // Sanity: sane layout, room for the table, a usable root.
    if sb.inode_count < 2
        || sb.inode_start < 1
        || sb.data_start < sb.inode_start + sb.inode_count.div_ceil(8)
        || sb.root_inode < 2
        || sb.root_inode > sb.inode_count
    {
        return None;
    }
    Some(sb)
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
        let mut sb_raw = [0u8; SB_FIXED_LEN];
        sb_raw[..4].copy_from_slice(&AEROFS_MAGIC);
        put_u32le(&mut sb_raw[4..], AEROFS_VERSION);
        put_u32le(&mut sb_raw[8..], bs as u32);
        put_u32le(&mut sb_raw[12..], inode_count);
        put_u32le(&mut sb_raw[16..], inode_start);
        put_u32le(&mut sb_raw[20..], data_start);
        put_u32le(&mut sb_raw[24..], self.root);
        let crc = crc32(&sb_raw);
        put_u32le(&mut sb_raw[SB_CRC_OFF..], crc);
        img[..SB_FIXED_LEN].copy_from_slice(&sb_raw);

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
}
