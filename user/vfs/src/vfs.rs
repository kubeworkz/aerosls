//! The VFS — the POSIX sidecar's filesystem layer (Phase 2 design §3.2, §3.4;
//! respawn decision §4.2, §6).
//!
//! Owns the mount table and every file descriptor. Key semantics, pinned by
//! the docs:
//!
//! - **No ambient namespace.** A path is resolved only through the mount
//!   table, which is core code. `open("/etc/passwd")` can never reach
//!   anything but the aerofs-lite image the core mounted.
//! - **Fds are sidecar-local capabilities.** An fd is an index into a
//!   *task's* table of `{node, rights, flags}`. Rights are minted once at
//!   `open()` from the flags and re-checked on every operation — an
//!   `O_RDONLY` fd cannot write. There is no way to obtain an entry except
//!   the core-mediated paths (open/create, dup of an owned entry).
//! - **Shared offset.** `FileNode` (inode + offset) is `Arc`-shared by
//!   `dup`/`fork`, so dup'd fds correctly share the file offset, exactly as
//!   POSIX open file descriptions do.
//! - **Stale mounts fail `EIO`, never `ENOENT`.** Path resolution stays
//!   table-only (a stale mount still resolves); the first operation that
//!   needs the device fails `EIO` (respawn §4.2, §6).
//! - **Open fds across a device death fail permanently.** A `FileNode`
//!   pins the fs *generation* it was minted against; a remount replaces the
//!   fs in place with a fresh generation, so an fd opened before the death
//!   keeps its structure but every subsequent I/O fails `EIO` — it never
//!   silently reconnects to a different device (respawn §6).
//! - **`/tmp` (ramfs) never goes stale** — it is core memory, not a device.

use alloc::collections::BTreeMap;
use alloc::format;
use alloc::string::String;
use alloc::string::ToString;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::cell::Cell;

use aerosls_blockcache::{BlockCache, BufferAlloc, Error as CacheError};
use aerosls_proto::kabi::Kernel;
use aerosls_proto::{R, W};

use crate::aerofs::{
    parse_dirent, parse_inode, parse_superblock, path_comps, DirEntry, FileType, Inode,
    Superblock, SuperblockRecord, DIRENT_SIZE, NDIRECT, S_IFDIR,
};
use crate::errno::{DirEnt, Errno, Stat};
use crate::fileobj::{CharNode, FileObj, PipeNode, PIPE_CAP};
use crate::ramfs::RamFs;

/// Block size in bytes (a const for const-context array sizes).
pub const BS: usize = aerosls_proto::BLOCK_SIZE as usize;

/// Open flag values (Linux generic numbers).
pub const O_RDONLY: u16 = 0o0;
pub const O_WRONLY: u16 = 0o1;
pub const O_RDWR: u16 = 0o2;
pub const O_ACCMODE: u16 = 0o3;
pub const O_CREAT: u16 = 0o100;
pub const O_EXCL: u16 = 0o200;
pub const O_TRUNC: u16 = 0o1000;
pub const O_APPEND: u16 = 0o2000;

/// lseek whence values.
pub const SEEK_SET: u32 = 0;
pub const SEEK_CUR: u32 = 1;
pub const SEEK_END: u32 = 2;

/// `clone_task` flags: the child shares the parent's fd table object
/// itself (threads) instead of a copy (fork). A shared table means a
/// `close`/`dup2` in one task is visible in all holders — POSIX threads.
pub const CLONE_FILES: u32 = 1;

pub const MAX_TASKS: usize = 256;
pub const MAX_FDS: usize = 256;

/// The rights an open-access mode demands (proto's `R`/`W` bits).
pub fn rights_of(flags: u16) -> u8 {
    match flags & O_ACCMODE {
        O_WRONLY => W,
        O_RDWR => R | W,
        _ => R, // O_RDONLY
    }
}

/// POSIX mode-bit check: owner bits when euid matches, else group, else
/// other. `want` is the *owner-position* bits (0o400 read, 0o200 write,
/// 0o100 exec); the matched class's bits are shifted into that position
/// before the AND, so callers pass one canonical value regardless of class.
/// Root bypasses.
pub fn perm_ok(mode: u16, uid: u16, gid: u16, euid: u16, egid: u16, want: u16) -> bool {
    if euid == 0 {
        return true;
    }
    let bits = if euid == uid {
        mode & 0o700
    } else if egid == gid {
        (mode & 0o070) << 3
    } else {
        (mode & 0o007) << 6
    };
    bits & want == want
}

// ── mounts and filesystems ──────────────────────────────────────────────────

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MountState {
    Active,
    Stale,
}

/// One mount-table entry. `comps` is the mount point's path components
/// (empty for `/`).
struct Mount {
    path: String,
    comps: Vec<String>,
    fs: usize,
}

/// The concrete filesystems a mount can back. Enum dispatch keeps the core
/// free of trait objects and makes "the mount table is core-owned" literal.
pub enum Fs<K: Kernel, A: BufferAlloc> {
    Aerofs(AerofsFs<K, A>),
    Ram(RamFs),
    /// `/dev`: names character devices (`console`, `null`). The fs layer
    /// only *names* them — an fd opened on a device holds `FileObj::Char`
    /// and its I/O bypasses this enum entirely.
    Dev(DevFs),
}

impl<K: Kernel, A: BufferAlloc> Fs<K, A> {
    fn id(&self) -> u64 {
        match self {
            Fs::Aerofs(f) => f.id,
            Fs::Ram(_) => 0, // ramfs is never replaced; generation 0 is stable
            Fs::Dev(_) => 0, // devfs is core memory, never replaced
        }
    }
    fn stale(&self) -> bool {
        match self {
            Fs::Aerofs(f) => f.stale(),
            Fs::Ram(_) => false,
            Fs::Dev(_) => false,
        }
    }
    /// Observe a close event queued on the backing device without an
    /// outstanding request (respawn decision §5 steps 1–3): a cache hit
    /// must never come from a dead device. Ramfs/devfs never poll.
    fn poll_dead(&mut self) -> Result<(), Errno> {
        match self {
            Fs::Aerofs(f) => f.poll_dead(),
            Fs::Ram(_) => Ok(()),
            Fs::Dev(_) => Ok(()),
        }
    }
    fn read_only(&self) -> bool {
        match self {
            // aerofs-lite v1 is implemented read-only (the boot image is
            // immutable); writes live in ramfs until a writable FS lands.
            Fs::Aerofs(_) => true,
            Fs::Ram(_) => false,
            // The fs layer is read-only, but device *objects* are not —
            // their writes go through `FileObj::Char`, never here.
            Fs::Dev(_) => false,
        }
    }
    fn lookup(&mut self, comps: &[&str]) -> Result<(u64, FileType), Errno> {
        match self {
            Fs::Aerofs(f) => f.lookup(comps),
            Fs::Ram(f) => f.lookup(comps),
            Fs::Dev(f) => f.lookup(comps),
        }
    }
    fn read(&mut self, ino: u64, offset: u64, buf: &mut [u8]) -> Result<usize, Errno> {
        match self {
            Fs::Aerofs(f) => f.read(ino, offset, buf),
            Fs::Ram(f) => f.read(ino, offset, buf),
            // Device I/O never goes through the fs layer.
            Fs::Dev(_) => Err(Errno::EInval),
        }
    }
    fn write(&mut self, ino: u64, offset: u64, buf: &[u8]) -> Result<usize, Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.write(ino, offset, buf),
            Fs::Dev(_) => Err(Errno::EInval),
        }
    }
    fn truncate(&mut self, ino: u64, len: u64) -> Result<(), Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.truncate(ino, len),
            Fs::Dev(_) => Err(Errno::ERofs),
        }
    }
    fn stat(&mut self, ino: u64) -> Result<Stat, Errno> {
        match self {
            Fs::Aerofs(f) => f.stat(ino),
            Fs::Ram(f) => f.stat(ino),
            Fs::Dev(f) => f.stat(ino),
        }
    }
    fn read_dir(&mut self, ino: u64) -> Result<Vec<DirEnt>, Errno> {
        match self {
            Fs::Aerofs(f) => f.read_dir(ino),
            Fs::Ram(f) => f.read_dir(ino),
            Fs::Dev(f) => f.read_dir(ino),
        }
    }
    /// The device object an fd will name (only devfs has these).
    fn open_dev(&mut self, ino: u64) -> Result<Arc<FileObj>, Errno> {
        match self {
            Fs::Dev(f) => f.open_dev(ino),
            _ => Err(Errno::EInval),
        }
    }
    fn create_file(&mut self, parent: u64, name: &str, mode: u16) -> Result<u64, Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.create_file(parent, name, mode, 0, 0),
            Fs::Dev(_) => Err(Errno::ERofs),
        }
    }
    fn create_dir(&mut self, parent: u64, name: &str, mode: u16) -> Result<u64, Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.create_dir(parent, name, mode, 0, 0),
            Fs::Dev(_) => Err(Errno::ERofs),
        }
    }
    fn unlink(&mut self, parent: u64, name: &str) -> Result<(), Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.unlink(parent, name),
            Fs::Dev(_) => Err(Errno::ERofs),
        }
    }
    fn rmdir(&mut self, parent: u64, name: &str) -> Result<(), Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.rmdir(parent, name),
            Fs::Dev(_) => Err(Errno::ERofs),
        }
    }
}

// ── /dev (devfs) ────────────────────────────────────────────────────────────

/// The `/dev` filesystem: a flat directory naming character devices. It is
/// core memory (never stale), and it only *names* — I/O on an opened
/// device goes to the `FileObj::Char` the entry holds, never through this
/// enum's read/write.
pub struct DevFs {
    names: BTreeMap<String, u64>,
    nodes: BTreeMap<u64, DevEntry>,
    next_ino: u64,
}

struct DevEntry {
    obj: Arc<FileObj>,
    mode: u16,
    uid: u16,
    gid: u16,
}

const DEV_ROOT: u64 = 1;

impl DevFs {
    fn new() -> DevFs {
        DevFs {
            names: BTreeMap::new(),
            nodes: BTreeMap::new(),
            next_ino: DEV_ROOT + 1,
        }
    }

    /// Add a device node. The node's mode/uid/gid come from the `CharNode`.
    fn add(&mut self, name: &str, node: Arc<CharNode>) -> Result<(), Errno> {
        self.check_name(name)?;
        if self.names.contains_key(name) {
            return Err(Errno::EExist);
        }
        let ino = self.next_ino;
        self.next_ino += 1;
        self.nodes.insert(
            ino,
            DevEntry {
                obj: Arc::new(FileObj::Char(node.clone())),
                mode: node.mode(),
                uid: node.uid(),
                gid: node.gid(),
            },
        );
        self.names.insert(name.to_string(), ino);
        Ok(())
    }

    /// The node an entry names (fd minting: `open("/dev/console")`).
    fn open_dev(&self, ino: u64) -> Result<Arc<FileObj>, Errno> {
        self.nodes.get(&ino).map(|e| e.obj.clone()).ok_or(Errno::ENoent)
    }

    fn lookup(&self, comps: &[&str]) -> Result<(u64, FileType), Errno> {
        if comps.is_empty() {
            return Ok((DEV_ROOT, FileType::Dir));
        }
        if comps.len() > 1 {
            // devfs is flat; anything nested is not found.
            return Err(Errno::ENoent);
        }
        let ino = *self.names.get(comps[0]).ok_or(Errno::ENoent)?;
        let e = self.nodes.get(&ino).ok_or(Errno::EInval)?;
        Ok((ino, ty_of(e.mode)))
    }

    fn stat(&self, ino: u64) -> Result<Stat, Errno> {
        if ino == DEV_ROOT {
            return Ok(Stat {
                mode: S_IFDIR | 0o755,
                uid: 0,
                gid: 0,
                size: 0,
                mtime: 0,
                ty: FileType::Dir,
            });
        }
        let e = self.nodes.get(&ino).ok_or(Errno::ENoent)?;
        Ok(Stat {
            mode: e.mode,
            uid: e.uid,
            gid: e.gid,
            size: 0,
            mtime: 0,
            ty: ty_of(e.mode),
        })
    }

    fn read_dir(&self, ino: u64) -> Result<Vec<DirEnt>, Errno> {
        if ino != DEV_ROOT {
            return Err(Errno::ENotdir);
        }
        let mut out = Vec::new();
        for (name, cino) in &self.names {
            let e = self.nodes.get(cino).ok_or(Errno::EInval)?;
            out.push(DirEnt {
                name: name.clone(),
                ino: *cino,
                ty: ty_of(e.mode),
            });
        }
        Ok(out)
    }

    fn check_name(&self, name: &str) -> Result<(), Errno> {
        if name.is_empty() || name.contains('/') {
            return Err(Errno::EInval);
        }
        Ok(())
    }
}

fn ty_of(mode: u16) -> FileType {
    match mode & crate::aerofs::S_IFMT {
        S_IFDIR => FileType::Dir,
        crate::aerofs::S_IFCHR => FileType::Char,
        crate::aerofs::S_IFIFO => FileType::Fifo,
        _ => FileType::File,
    }
}

/// aerofs-lite over the ramdisk block cache: the root filesystem. Owns the
/// device (the block cache) and an inode cache; records the superblock
/// identity for revalidation on remount (respawn decision §5.9b).
pub struct AerofsFs<K: Kernel, A: BufferAlloc> {
    cache: BlockCache<K, A>,
    sb: Superblock,
    /// Identity of the image behind the device name, for revalidation.
    record: SuperblockRecord,
    /// Generation: bumped on every remount so pre-death fds fail
    /// permanently instead of silently reconnecting.
    id: u64,
    inodes: BTreeMap<u64, Inode>,
}

impl<K: Kernel, A: BufferAlloc> AerofsFs<K, A> {
    /// Mount: read the superblock through the cache and validate it. On
    /// success the fs owns the (already-connected) cache and records the
    /// image identity for later revalidation.
    pub fn mount(mut cache: BlockCache<K, A>, id: u64) -> Result<AerofsFs<K, A>, Errno> {
        let mut block = [0u8; aerosls_proto::BLOCK_SIZE as usize];
        cache
            .read_block(0, &mut block)
            .map_err(map_cache_err)?;
        let sb = parse_superblock(&block).ok_or(Errno::EInval)?;
        let record = SuperblockRecord {
            magic: crate::aerofs::AEROFS_MAGIC,
            block_size: sb.block_size,
            block_count: cache.blocks(),
            sb_crc: sb.crc,
        };
        Ok(AerofsFs {
            cache,
            sb,
            record,
            id,
            inodes: BTreeMap::new(),
        })
    }

    pub fn record(&self) -> SuperblockRecord {
        self.record
    }

    pub fn stale(&self) -> bool {
        self.cache.state() != aerosls_blockcache::State::Live
    }

    /// Observe a close event queued on the device without an outstanding
    /// request (the event loop's wake path; here the VFS polls before every
    /// op). Marks the cache stale on death — then `check_live` fails.
    pub fn poll_dead(&mut self) -> Result<(), Errno> {
        self.cache.poll_dead().map_err(map_cache_err)
    }

    fn check_live(&self) -> Result<(), Errno> {
        if self.stale() {
            return Err(Errno::EIo);
        }
        Ok(())
    }

    /// The op prologue: observe any queued close, then refuse if the device
    /// is known dead. Every device-touching entry point starts with this.
    fn op_guard(&mut self) -> Result<(), Errno> {
        self.poll_dead()?;
        self.check_live()
    }

    /// Look up a path (relative to this fs) from the root inode.
    pub fn lookup(&mut self, comps: &[&str]) -> Result<(u64, FileType), Errno> {
        self.op_guard()?;
        let mut ino = self.sb.root_inode as u64;
        let mut ty = FileType::Dir;
        for (i, c) in comps.iter().enumerate() {
            if *c == "." {
                continue;
            }
            let inode = self.inode(ino)?;
            if inode.ty() != Some(FileType::Dir) {
                return Err(Errno::ENotdir);
            }
            let ents = self.read_dir_entries(ino)?;
            let hit = if *c == ".." {
                ents.iter().find(|(n, _, _)| n == "..").ok_or(Errno::ENoent)?
            } else {
                ents.iter()
                    .find(|(n, _, _)| n == *c)
                    .ok_or(Errno::ENoent)?
            };
            ino = hit.1 as u64;
            ty = FileType::from_dt(hit.2).ok_or(Errno::EInval)?;
            if i + 1 < comps.len() && ty != FileType::Dir {
                return Err(Errno::ENotdir);
            }
        }
        Ok((ino, ty))
    }

    /// Read `buf.len()` bytes at `offset` of a regular file.
    pub fn read(&mut self, ino: u64, offset: u64, buf: &mut [u8]) -> Result<usize, Errno> {
        self.op_guard()?;
        let inode = self.inode(ino)?;
        if inode.ty() != Some(FileType::File) {
            return Err(Errno::EIsdir);
        }
        let size = inode.size as u64;
        if offset >= size {
            return Ok(0);
        }
        let n = core::cmp::min(buf.len() as u64, size - offset) as usize;
        let bs = BS;
        let mut done = 0usize;
        while done < n {
            let pos = offset + done as u64;
            let blk = pos / bs as u64;
            let in_blk = (pos % bs as u64) as usize;
            let take = core::cmp::min(bs - in_blk, n - done);
            let addr = self.block_addr(&inode, blk)?;
            let mut scratch = [0u8; BS];
            self.cache
                .read_block(addr as u64, &mut scratch)
                .map_err(map_cache_err)?;
            buf[done..done + take].copy_from_slice(&scratch[in_blk..in_blk + take]);
            done += take;
        }
        Ok(n)
    }

    pub fn stat(&mut self, ino: u64) -> Result<Stat, Errno> {
        self.op_guard()?;
        let inode = self.inode(ino)?;
        Ok(Stat {
            mode: inode.mode,
            uid: inode.uid,
            gid: inode.gid,
            size: inode.size as u64,
            mtime: inode.mtime,
            ty: inode.ty().ok_or(Errno::EInval)?,
        })
    }

    /// List a directory's entries (the fs-side primitive behind both
    /// `read_dir` and `lookup`).
    pub fn read_dir(&mut self, ino: u64) -> Result<Vec<DirEnt>, Errno> {
        let ents = self.read_dir_entries(ino)?;
        ents.into_iter()
            .map(|(name, ino, ty)| {
                FileType::from_dt(ty)
                    .map(|ty| DirEnt {
                        name,
                        ino: ino as u64,
                        ty,
                    })
                    .ok_or(Errno::EInval)
            })
            .collect()
    }

    // ── internals ─────────────────────────────────────────────────────────────

    /// The inode, from the cache or the inode table on the device.
    fn inode(&mut self, ino: u64) -> Result<Inode, Errno> {
        if let Some(i) = self.inodes.get(&ino) {
            return Ok(*i);
        }
        if ino < 2 || ino > self.sb.inode_count as u64 {
            return Err(Errno::ENoent);
        }
        let bs = BS;
        let byte_off = crate::aerofs::inode_offset(ino as u32);
        let blk = self.sb.inode_start as u64 + (byte_off / bs) as u64;
        let in_blk = byte_off % bs;
        let mut block = [0u8; BS];
        self.cache
            .read_block(blk, &mut block)
            .map_err(map_cache_err)?;
        let inode = parse_inode(&block[in_blk..]).ok_or(Errno::EInval)?;
        if inode.ty().is_none() {
            return Err(Errno::EInval);
        }
        self.inodes.insert(ino, inode);
        Ok(inode)
    }

    /// Resolve file block `blk` to a device block, following the indirect
    /// pointer when needed.
    fn block_addr(&mut self, inode: &Inode, blk: u64) -> Result<u32, Errno> {
        if blk < NDIRECT as u64 {
            let b = inode.blocks[blk as usize];
            if b == 0 {
                return Err(Errno::EInval); // sparse hole — builder never makes these
            }
            return Ok(b);
        }
        if inode.indirect == 0 {
            return Err(Errno::EInval);
        }
        let mut ib = [0u8; BS];
        self.cache
            .read_block(inode.indirect as u64, &mut ib)
            .map_err(map_cache_err)?;
        let idx = (blk - NDIRECT as u64) as usize;
        if idx >= crate::aerofs::NINDIRECT {
            return Err(Errno::EInval);
        }
        let b = u32::from_le_bytes([ib[idx * 4], ib[idx * 4 + 1], ib[idx * 4 + 2], ib[idx * 4 + 3]]);
        if b == 0 {
            return Err(Errno::EInval);
        }
        Ok(b)
    }

    /// Read a directory's full entry list. Reads the whole dir into a
    /// buffer (dirs are small) so entries may safely straddle blocks.
    fn read_dir_entries(&mut self, ino: u64) -> Result<Vec<(String, u32, u8)>, Errno> {
        self.op_guard()?;
        let inode = self.inode(ino)?;
        if inode.ty() != Some(FileType::Dir) {
            return Err(Errno::ENotdir);
        }
        let bs = BS;
        let mut data = vec![0u8; inode.size as usize];
        let mut done = 0usize;
        while done < data.len() {
            let blk = (done / bs) as u64;
            let in_blk = done % bs;
            let addr = self.block_addr(&inode, blk)?;
            let mut scratch = [0u8; BS];
            self.cache
                .read_block(addr as u64, &mut scratch)
                .map_err(map_cache_err)?;
            let take = core::cmp::min(bs - in_blk, data.len() - done);
            data[done..done + take].copy_from_slice(&scratch[in_blk..in_blk + take]);
            done += take;
        }
        let mut out = Vec::new();
        let mut p = 0usize;
        while p + DIRENT_SIZE <= data.len() {
            let e: DirEntry = parse_dirent(&data[p..]).ok_or(Errno::EInval)?;
            out.push((e.name_str().to_string(), e.ino, e.ty));
            let rec = if e.rec_len as usize >= DIRENT_SIZE {
                e.rec_len as usize
            } else {
                DIRENT_SIZE
            };
            p += rec;
            if rec < DIRENT_SIZE {
                break;
            }
        }
        Ok(out)
    }
}

/// Map a block-cache (device-level) failure to errno. Everything the cache
/// can produce is device-level: stale, driver status, kernel error,
/// protocol violation — all `EIO` (respawn §6: device gone or unreliable).
fn map_cache_err(e: CacheError) -> Errno {
    match e {
        CacheError::Stale { .. }
        | CacheError::Status(_)
        | CacheError::Kernel(_)
        | CacheError::Protocol => Errno::EIo,
    }
}

// ── fds ─────────────────────────────────────────────────────────────────────

/// The mount-table half of `FileObj`: which fs + inode, and the shared
/// offset. `Arc`-shared by dup/fork so dup'd fds share the offset. `fs_id`
/// pins the fs generation at open time — if the device died and was
/// remounted, the generation differs and every op fails `EIO` permanently
/// (respawn §6).
pub struct FileNode {
    pub fs: usize,
    pub fs_id: u64,
    pub ino: u64,
    offset: Cell<u64>,
}

/// One fd-table slot. `node` is the file-like object (file on a mount, a
/// pipe end, a device); `rights` are minted once at open/pipe from the
/// access mode and re-checked on every operation.
#[derive(Clone)]
pub struct FdEntry {
    pub node: Arc<FileObj>,
    /// Rights minted at open from the access mode (proto `R`/`W` bits).
    pub rights: u8,
    pub flags: u16,
}

impl FdEntry {
    /// A pipe end has entered the fd world (open of a pipe fd, dup, fork
    /// copy, dup2 placement). Bumps the end's liveness count so EOF/EPIPE
    /// stay exact. No-op for non-pipe objects.
    fn note_added(&self) {
        match &*self.node {
            FileObj::PipeRead(p) => p.bump_readers(1),
            FileObj::PipeWrite(p) => p.bump_writers(1),
            _ => {}
        }
    }

    /// A pipe end has left the fd world (close, dup2 overwrite, exit
    /// abandoning a table). No-op for non-pipe objects.
    fn note_removed(&self) {
        match &*self.node {
            FileObj::PipeRead(p) => p.bump_readers(-1),
            FileObj::PipeWrite(p) => p.bump_writers(-1),
            _ => {}
        }
    }
}

#[derive(Clone)]
struct FdTable {
    entries: Vec<Option<FdEntry>>,
}

impl FdTable {
    fn new() -> FdTable {
        FdTable {
            entries: Vec::new(),
        }
    }
    fn get(&self, fd: u32) -> Option<&FdEntry> {
        self.entries.get(fd as usize).and_then(|e| e.as_ref())
    }
    fn alloc(&mut self, entry: FdEntry) -> Result<u32, Errno> {
        for (i, slot) in self.entries.iter_mut().enumerate() {
            if slot.is_none() {
                *slot = Some(entry);
                return Ok(i as u32);
            }
        }
        if self.entries.len() >= MAX_FDS {
            return Err(Errno::EMfile);
        }
        self.entries.push(Some(entry));
        Ok((self.entries.len() - 1) as u32)
    }
}

struct Task {
    /// Index into `Vfs::table_pool`. `usize::MAX` = no table (exited).
    fds: usize,
    cwd: String,
    euid: u16,
    egid: u16,
}

// ── the VFS ─────────────────────────────────────────────────────────────────

pub struct Vfs<K: Kernel, A: BufferAlloc> {
    fss: Vec<Fs<K, A>>,
    mounts: Vec<Mount>,
    next_fs_id: u64,
    /// The fd-table registry. Tasks reference tables by index; a table is
    /// *shared* when two tasks carry the same index (CLONE_FILES threads)
    /// and *copied* when a fork pushed a fresh clone. Entries are never
    /// removed (indices must stay stable); the pool is bounded by the
    /// number of fork/clone events.
    table_pool: Vec<FdTable>,
    tasks: Vec<Task>,
}

impl<K: Kernel, A: BufferAlloc> Vfs<K, A> {
    pub fn new() -> Vfs<K, A> {
        Vfs {
            fss: Vec::new(),
            mounts: Vec::new(),
            next_fs_id: 1,
            table_pool: vec![FdTable::new()],
            tasks: vec![Task {
                fds: 0,
                cwd: String::from("/"),
                euid: 0,
                egid: 0,
            }],
        }
    }

    // ── mount management ───────────────────────────────────────────────────

    /// Mount an aerofs-lite device at `path`. The caller hands over an
    /// already-connected `BlockCache` (device attach + handshake are the
    /// respawn/device layer's job). Reads and validates the superblock and
    /// records the image identity for revalidation.
    pub fn mount_aerofs(&mut self, path: &str, cache: BlockCache<K, A>) -> Result<(), Errno> {
        let comps = mount_comps(path);
        self.check_mount_free(&comps)?;
        let id = self.next_fs_id;
        self.next_fs_id += 1;
        let fs = AerofsFs::mount(cache, id)?;
        let path = crate::aerofs::normalize_path(path);
        self.mounts.push(Mount {
            path: path.clone(),
            comps,
            fs: self.fss.len(),
        });
        self.fss.push(Fs::Aerofs(fs));
        Ok(())
    }

    /// Mount an in-memory ramfs at `path` (e.g. `/tmp`). Never stale.
    pub fn mount_ramfs(&mut self, path: &str) -> Result<(), Errno> {
        let comps = mount_comps(path);
        self.check_mount_free(&comps)?;
        let path = crate::aerofs::normalize_path(path);
        self.mounts.push(Mount {
            path: path.clone(),
            comps,
            fs: self.fss.len(),
        });
        self.fss.push(Fs::Ram(RamFs::new()));
        Ok(())
    }

    /// Mount an in-memory devfs at `path` (e.g. `/dev`) naming the given
    /// console and the always-present `/dev/null`. The console is owned by
    /// the caller (the bootstrap wires the kernel-console channel into it);
    /// the VFS only names it. Never stale.
    pub fn mount_devfs(&mut self, path: &str, console: Arc<CharNode>) -> Result<(), Errno> {
        let comps = mount_comps(path);
        self.check_mount_free(&comps)?;
        let mut dev = DevFs::new();
        dev.add("console", console)?;
        dev.add("null", Arc::new(CharNode::null()))?;
        let path = crate::aerofs::normalize_path(path);
        self.mounts.push(Mount {
            path: path.clone(),
            comps,
            fs: self.fss.len(),
        });
        self.fss.push(Fs::Dev(dev));
        Ok(())
    }

    /// Add a device node to an existing devfs mount (drivers register their
    /// channels here). `path` names the mount (e.g. `/dev`).
    pub fn add_dev_node(
        &mut self,
        path: &str,
        name: &str,
        node: Arc<CharNode>,
    ) -> Result<(), Errno> {
        let full = crate::aerofs::normalize_path(path);
        let (fs_idx, rel) = self.resolve(&full)?;
        if !rel.is_empty() {
            return Err(Errno::ENotdir);
        }
        match self.fss.get_mut(fs_idx) {
            Some(Fs::Dev(d)) => d.add(name, node),
            _ => Err(Errno::EInval),
        }
    }

    /// Re-attach a replacement device to an existing aerofs mount after a
    /// respawn (respawn decision §5.8–5.11). Revalidates the image identity
    /// against the record from the first mount; a mismatch means a
    /// *different device* behind the same name and the remount fails. On
    /// success the fs is replaced in place with a fresh generation, so
    /// pre-death fds fail permanently.
    pub fn remount_aerofs(&mut self, mount_idx: usize, cache: BlockCache<K, A>) -> Result<(), Errno> {
        let (fs_idx, record) = {
            let m = self.mounts.get(mount_idx).ok_or(Errno::EInval)?;
            match &self.fss[m.fs] {
                Fs::Aerofs(f) => (m.fs, f.record()),
                // Only device-backed mounts can respawn.
                Fs::Ram(_) | Fs::Dev(_) => return Err(Errno::EInval),
            }
        };
        let mut block = [0u8; aerosls_proto::BLOCK_SIZE as usize];
        let mut new_cache = cache;
        new_cache
            .read_block(0, &mut block)
            .map_err(map_cache_err)?;
        let sb = parse_superblock(&block).ok_or(Errno::EInval)?;
        if !record.matches(&sb, new_cache.blocks()) {
            // Different image behind the same device name: log loudly (the
            // caller's supervisor does that), fail the attach.
            return Err(Errno::EIo);
        }
        let id = self.next_fs_id;
        self.next_fs_id += 1;
        self.fss[fs_idx] = Fs::Aerofs(AerofsFs {
            cache: new_cache,
            sb,
            record,
            id,
            inodes: BTreeMap::new(),
        });
        Ok(())
    }

    /// The mount table for observability (device half of the respawn state
    /// machine; stale = the underlying device is gone).
    pub fn mounts(&self) -> Vec<(String, MountState)> {
        self.mounts
            .iter()
            .map(|m| {
                let st = if self.fss[m.fs].stale() {
                    MountState::Stale
                } else {
                    MountState::Active
                };
                (m.path.clone(), st)
            })
            .collect()
    }

    // ── tasks ──────────────────────────────────────────────────────────────

    /// Create a task (the proc manager's job in the full sidecar); returns
    /// its id.
    pub fn add_task(&mut self) -> Result<u32, Errno> {
        if self.tasks.len() >= MAX_TASKS {
            return Err(Errno::EAgain);
        }
        let id = self.tasks.len() as u32;
        let table = self.table_pool.len();
        self.table_pool.push(FdTable::new());
        self.tasks.push(Task {
            fds: table,
            cwd: String::from("/"),
            euid: 0,
            egid: 0,
        });
        Ok(id)
    }

    pub fn set_cred(&mut self, task: u32, euid: u16, egid: u16) -> Result<(), Errno> {
        let t = self.task_mut(task)?;
        t.euid = euid;
        t.egid = egid;
        Ok(())
    }

    pub fn set_cwd(&mut self, task: u32, path: &str) -> Result<(), Errno> {
        let norm = crate::aerofs::normalize_path(path);
        // Validate: the cwd must resolve through the mount table.
        let (fs_idx, rel) = self.resolve(&norm)?;
        let fs = self.fs_mut(fs_idx)?;
        let (_, ty) = fs.lookup(&rel)?;
        if ty != FileType::Dir {
            return Err(Errno::ENotdir);
        }
        self.task_mut(task)?.cwd = norm;
        Ok(())
    }

    // ── the syscall surface ────────────────────────────────────────────────

    /// `open(path, flags, mode)`.
    pub fn open(&mut self, task: u32, path: &str, flags: u16, mode: u16) -> Result<u32, Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        let want = rights_of(flags);
        let (ino, ty) = {
            let fs = self.fs_mut(fs_idx)?;
            if flags & O_CREAT != 0 {
                match fs.lookup(&rel) {
                    Ok(hit) => {
                        if flags & O_EXCL != 0 {
                            return Err(Errno::EExist);
                        }
                        hit
                    }
                    Err(Errno::ENoent) => {
                        if rel.is_empty() {
                            return Err(Errno::ENoent);
                        }
                        let parent = fs.lookup(&rel[..rel.len() - 1])?;
                        let name = rel.last().unwrap();
                        let ino = fs.create_file(parent.0, name, mode)?;
                        (ino, FileType::File)
                    }
                    Err(e) => return Err(e),
                }
            } else {
                fs.lookup(&rel)?
            }
        };
        if ty == FileType::Dir {
            return Err(Errno::EIsdir);
        }
        // Character device: mint the fd on the device object. Its I/O
        // bypasses the fs layer (see `FileObj::Char`), so the fs-level
        // read-only/truncate rules don't apply; perms are checked against
        // the node's mode.
        if ty == FileType::Char {
            let st = {
                let fs = self.fs_mut(fs_idx)?;
                fs.stat(ino)?
            };
            self.check_open_perms(want, st, task)?;
            let obj = {
                let fs = self.fs_mut(fs_idx)?;
                fs.open_dev(ino)?
            };
            let fd = {
                let idx = self.task_mut(task)?.fds;
                self.table_pool[idx].alloc(FdEntry {
                    node: obj,
                    rights: want,
                    flags,
                })?
            };
            return Ok(fd);
        }
        // Named pipes (an `S_IFIFO` in a real fs): no `mkfifo` in v1, so
        // only the `pipe()`-created objects exist; a fifo node can't be
        // opened yet.
        if ty == FileType::Fifo {
            return Err(Errno::EInval);
        }
        // O_TRUNC without write access is refused up front.
        if flags & O_TRUNC != 0 && want & W == 0 {
            return Err(Errno::EInval);
        }
        if want & W != 0 && self.fss[fs_idx].read_only() {
            return Err(Errno::ERofs);
        }
        let st = {
            let fs = self.fs_mut(fs_idx)?;
            fs.stat(ino)?
        };
        // Permission check against the task's credentials (the capability
        // layer — the fd — is minted only if this passes).
        self.check_open_perms(want, st, task)?;
        if flags & O_TRUNC != 0 {
            let fs = self.fs_mut(fs_idx)?;
            fs.truncate(ino, 0)?;
        }
        let fs_id = self.fss[fs_idx].id();
        let node = Arc::new(FileObj::File(FileNode {
            fs: fs_idx,
            fs_id,
            ino,
            offset: Cell::new(0),
        }));
        let fd = {
            let idx = self.task_mut(task)?.fds;
            self.table_pool[idx].alloc(FdEntry {
                node,
                rights: want,
                flags,
            })?
        };
        Ok(fd)
    }

    /// The open-time capability check: the rights demanded by the access
    /// mode against the object's mode bits and the task's credentials. The
    /// fd (a sidecar-local capability) is minted only if this passes.
    fn check_open_perms(&self, want: u8, st: Stat, task: u32) -> Result<(), Errno> {
        let (euid, egid) = {
            let t = self.task(task)?;
            (t.euid, t.egid)
        };
        if want & R != 0 && !perm_ok(st.mode, st.uid, st.gid, euid, egid, 0o400) {
            return Err(Errno::EAcces);
        }
        if want & W != 0 && !perm_ok(st.mode, st.uid, st.gid, euid, egid, 0o200) {
            return Err(Errno::EAcces);
        }
        Ok(())
    }

    /// `pipe()`: create a pipe and mint its read and write ends in the
    /// task's fd table (fd 0 = read, fd 1 = write). Both ends share the
    /// `PipeNode`; the end-count bookkeeping keeps EOF/EPIPE exact as the
    /// fds are dup'd, closed, forked, and dropped on exit.
    pub fn pipe(&mut self, task: u32) -> Result<(u32, u32), Errno> {
        let node = Arc::new(PipeNode::new(PIPE_CAP));
        let (r, w) = {
            let idx = self.task_mut(task)?.fds;
            let table = &mut self.table_pool[idx];
            let r = table.alloc(FdEntry {
                node: Arc::new(FileObj::PipeRead(node.clone())),
                rights: R,
                flags: 0,
            })?;
            let w = table.alloc(FdEntry {
                node: Arc::new(FileObj::PipeWrite(node)),
                rights: W,
                flags: 0,
            })?;
            (r, w)
        };
        // Count both ends as live (the entries already exist).
        {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(r).unwrap();
            e.note_added();
            let e = self.table_pool[idx].get(w).unwrap();
            e.note_added();
        }
        Ok((r, w))
    }

    pub fn close(&mut self, task: u32, fd: u32) -> Result<(), Errno> {
        let idx = self.task_mut(task)?.fds;
        let slot = self.table_pool[idx]
            .entries
            .get_mut(fd as usize)
            .ok_or(Errno::EBadf)?;
        if slot.is_none() {
            return Err(Errno::EBadf);
        }
        let gone = slot.take().unwrap();
        // A pipe end left the fd world: recount so EOF/EPIPE stay exact.
        gone.note_removed();
        Ok(())
    }

    pub fn dup(&mut self, task: u32, fd: u32) -> Result<u32, Errno> {
        let idx = self.task_mut(task)?.fds;
        let copy = {
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            FdEntry {
                node: e.node.clone(),
                rights: e.rights,
                flags: e.flags,
            }
        };
        let nfd = self.table_pool[idx].alloc(copy)?;
        // A new pipe end entered the fd world (dup shares the Arc — the
        // end stays alive as long as any of its fds does).
        let e = self.table_pool[idx].get(nfd).unwrap();
        e.note_added();
        Ok(nfd)
    }

    pub fn dup2(&mut self, task: u32, oldfd: u32, newfd: u32) -> Result<u32, Errno> {
        if oldfd == newfd {
            return Ok(newfd);
        }
        if newfd as usize >= MAX_FDS {
            return Err(Errno::EBadf);
        }
        let entry = {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(oldfd).ok_or(Errno::EBadf)?;
            FdEntry {
                node: e.node.clone(),
                rights: e.rights,
                flags: e.flags,
            }
        };
        let idx = self.task_mut(task)?.fds;
        let table = &mut self.table_pool[idx];
        // The slot being replaced loses its end; the placed one gains it.
        if (newfd as usize) < table.entries.len() {
            if let Some(prev) = table.entries[newfd as usize].take() {
                prev.note_removed();
            }
            table.entries[newfd as usize] = Some(entry);
        } else {
            while (table.entries.len() as u32) < newfd {
                table.entries.push(None);
            }
            table.entries.push(Some(entry));
        }
        let e = table.get(newfd).unwrap();
        e.note_added();
        Ok(newfd)
    }

    pub fn read(&mut self, task: u32, fd: u32, buf: &mut [u8]) -> Result<usize, Errno> {
        let obj = {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            if e.rights & R == 0 {
                return Err(Errno::EBadf);
            }
            e.node.clone()
        };
        match &*obj {
            FileObj::File(node) => {
                let n = {
                    let fs = self.fs_mut_id(node.fs, node.fs_id)?;
                    fs.read(node.ino, node.offset.get(), buf)?
                };
                node.offset.set(node.offset.get() + n as u64);
                Ok(n)
            }
            FileObj::PipeRead(p) => p.read(buf),
            FileObj::Char(c) => c.read(buf),
            // A write-only pipe end: the rights check above already
            // refused; this arm is defensive.
            FileObj::PipeWrite(_) => Err(Errno::EBadf),
        }
    }

    pub fn write(&mut self, task: u32, fd: u32, buf: &[u8]) -> Result<usize, Errno> {
        let (obj, append) = {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            if e.rights & W == 0 {
                return Err(Errno::EBadf);
            }
            (e.node.clone(), e.flags & O_APPEND != 0)
        };
        match &*obj {
            FileObj::File(node) => {
                // O_APPEND: the write always goes to the current end.
                let offset = if append {
                    let fs = self.fs_mut_id(node.fs, node.fs_id)?;
                    fs.stat(node.ino)?.size
                } else {
                    node.offset.get()
                };
                let n = {
                    let fs = self.fs_mut_id(node.fs, node.fs_id)?;
                    fs.write(node.ino, offset, buf)?
                };
                if !append {
                    node.offset.set(offset + n as u64);
                }
                Ok(n)
            }
            FileObj::PipeWrite(p) => p.write(buf),
            FileObj::Char(c) => c.write(buf),
            FileObj::PipeRead(_) => Err(Errno::EBadf),
        }
    }

    pub fn lseek(&mut self, task: u32, fd: u32, off: i64, whence: u32) -> Result<u64, Errno> {
        let obj = {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            e.node.clone()
        };
        let FileObj::File(node) = &*obj else {
            // Pipes and devices are not seekable.
            return Err(Errno::ESPipe);
        };
        // Every whence touches the device: lseek on a dead device fails
        // `EIO`, and a pre-death fd fails permanently (respawn §6).
        let size = if whence == SEEK_END {
            let fs = self.fs_mut_id(node.fs, node.fs_id)?;
            fs.stat(node.ino)?.size
        } else {
            let fs = self.fs_mut_id(node.fs, node.fs_id)?;
            fs.poll_dead()?;
            0
        };
        let base = match whence {
            SEEK_SET => 0i64,
            SEEK_CUR => node.offset.get() as i64,
            SEEK_END => size as i64,
            _ => return Err(Errno::EInval),
        };
        let new = base.checked_add(off).ok_or(Errno::EInval)?;
        if new < 0 {
            return Err(Errno::EInval);
        }
        node.offset.set(new as u64);
        Ok(new as u64)
    }

    pub fn stat(&mut self, task: u32, path: &str) -> Result<Stat, Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        let fs = self.fs_mut(fs_idx)?;
        let (ino, _) = fs.lookup(&rel)?;
        fs.stat(ino)
    }

    pub fn fstat(&mut self, task: u32, fd: u32) -> Result<Stat, Errno> {
        let obj = {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            e.node.clone()
        };
        match &*obj {
            FileObj::File(node) => {
                let fs = self.fs_mut_id(node.fs, node.fs_id)?;
                fs.stat(node.ino)
            }
            FileObj::PipeRead(p) | FileObj::PipeWrite(p) => Ok(p.stat()),
            FileObj::Char(c) => Ok(c.stat()),
        }
    }

    pub fn read_dir(&mut self, task: u32, path: &str) -> Result<Vec<DirEnt>, Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        let fs = self.fs_mut(fs_idx)?;
        let (ino, ty) = fs.lookup(&rel)?;
        if ty != FileType::Dir {
            return Err(Errno::ENotdir);
        }
        fs.read_dir(ino)
    }

    pub fn mkdir(&mut self, task: u32, path: &str, mode: u16) -> Result<(), Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        if rel.is_empty() {
            return Err(Errno::EExist);
        }
        let fs = self.fs_mut(fs_idx)?;
        let parent = fs.lookup(&rel[..rel.len() - 1])?;
        fs.create_dir(parent.0, rel.last().unwrap(), mode)?;
        Ok(())
    }

    pub fn unlink(&mut self, task: u32, path: &str) -> Result<(), Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        if rel.is_empty() {
            return Err(Errno::EIsdir);
        }
        let fs = self.fs_mut(fs_idx)?;
        let parent = fs.lookup(&rel[..rel.len() - 1])?;
        fs.unlink(parent.0, rel.last().unwrap())
    }

    pub fn rmdir(&mut self, task: u32, path: &str) -> Result<(), Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        if rel.is_empty() {
            return Err(Errno::EBusy);
        }
        let fs = self.fs_mut(fs_idx)?;
        let parent = fs.lookup(&rel[..rel.len() - 1])?;
        fs.rmdir(parent.0, rel.last().unwrap())
    }

    /// An fd's current offset (test/observability helper). Only seekable
    /// objects (files on a mount) have one.
    pub fn offset_of(&self, task: u32, fd: u32) -> Option<u64> {
        let idx = self.tasks.get(task as usize)?.fds;
        let e = self.table_pool.get(idx)?.get(fd)?;
        match &*e.node {
            FileObj::File(node) => Some(node.offset.get()),
            _ => None,
        }
    }

    /// The inode an fd names (test/observability helper: dup/dup2 must
    /// share the node). Only mount-table objects have one.
    pub fn fd_ino(&self, task: u32, fd: u32) -> Option<u64> {
        let idx = self.tasks.get(task as usize)?.fds;
        let e = self.table_pool.get(idx)?.get(fd)?;
        match &*e.node {
            FileObj::File(node) => Some(node.ino),
            _ => None,
        }
    }

    /// How many fds a task holds (0 after exit).
    pub fn fd_count(&self, task: u32) -> Option<usize> {
        let idx = self.tasks.get(task as usize)?.fds;
        Some(self.table_pool.get(idx)?.entries.len())
    }

    /// Fork support: a new task whose fd table is a *copy* of the parent's
    /// (entries share their `Arc<FileObj>`, so parent and child correctly
    /// share open-file offsets and pipe ends), with cwd and credentials
    /// copied. With `CLONE_FILES`, the child shares the parent's table
    /// *object* instead (threads: `close`/`dup2` in one is visible in all
    /// holders).
    pub fn clone_task(&mut self, parent: u32, flags: u32) -> Result<u32, Errno> {
        let (cwd, euid, egid, table_idx) = {
            let p = self.task(parent)?;
            (p.cwd.clone(), p.euid, p.egid, p.fds)
        };
        let table = if flags & CLONE_FILES != 0 {
            table_idx
        } else {
            // A fork-copied table holds fresh *entries* for the child's
            // pipe ends — recount them so EOF/EPIPE stay exact.
            let copy = self.table_pool[table_idx].clone();
            for e in copy.entries.iter().flatten() {
                e.note_added();
            }
            let idx = self.table_pool.len();
            self.table_pool.push(copy);
            idx
        };
        let id = self.tasks.len() as u32;
        self.tasks.push(Task {
            fds: table,
            cwd,
            euid,
            egid,
        });
        Ok(id)
    }

    /// Drop a task's fd table (the exit path). A table shared via
    /// `CLONE_FILES` survives as long as any holder remains; when the last
    /// holder exits, the table is cleared and its pipe ends recounted (the
    /// reader side of a dead shell sees EOF, a dead reader gets `EPIPE`).
    pub fn exit_task(&mut self, task: u32) -> Result<(), Errno> {
        let idx = {
            let t = self.task_mut(task)?;
            if t.fds == usize::MAX {
                return Ok(()); // already exited
            }
            t.fds
        };
        let shared = self
            .tasks
            .iter()
            .enumerate()
            .any(|(i, t)| i as u32 != task && t.fds == idx);
        if !shared {
            for slot in self.table_pool[idx].entries.iter_mut() {
                if let Some(e) = slot.take() {
                    e.note_removed();
                }
            }
        }
        self.task_mut(task)?.fds = usize::MAX;
        Ok(())
    }

    // ── internals ─────────────────────────────────────────────────────────────

    fn task(&self, task: u32) -> Result<&Task, Errno> {
        self.tasks.get(task as usize).ok_or(Errno::EInval)
    }
    fn task_mut(&mut self, task: u32) -> Result<&mut Task, Errno> {
        self.tasks.get_mut(task as usize).ok_or(Errno::EInval)
    }

    fn fs_mut(&mut self, idx: usize) -> Result<&mut Fs<K, A>, Errno> {
        self.fss.get_mut(idx).ok_or(Errno::EInval)
    }

    /// Like `fs_mut` but enforces the fail-permanently rule: the caller's
    /// node pins an fs generation; if it no longer matches (device died and
    /// was remounted), every op fails `EIO` — an fd must never silently
    /// change content (respawn §6). Also polls the device for a queued
    /// close, so an fd op on a freshly-dead device fails `EIO` instead of
    /// being served from cache (respawn §5 steps 1–3, §6).
    fn fs_mut_id(&mut self, idx: usize, expected: u64) -> Result<&mut Fs<K, A>, Errno> {
        let fs = self.fss.get_mut(idx).ok_or(Errno::EInval)?;
        if fs.id() != expected {
            return Err(Errno::EIo);
        }
        fs.poll_dead()?;
        Ok(fs)
    }

    /// Resolve a normalized absolute path to (fs index, remaining comps).
    /// Longest-prefix match on component boundaries: `/tmp/x` goes to the
    /// `/tmp` mount, `/tmpx` does not. Table-only — no device I/O — so a
    /// stale mount still resolves (respawn §4.2).
    fn resolve<'a>(&self, norm: &'a str) -> Result<(usize, Vec<&'a str>), Errno> {
        let comps = path_comps(norm);
        let mut best: Option<(usize, usize)> = None; // (mount idx, comps len)
        for (mi, m) in self.mounts.iter().enumerate() {
            if comps.len() >= m.comps.len()
                && comps[..m.comps.len()]
                    .iter()
                    .zip(m.comps.iter())
                    .all(|(a, b)| *a == b.as_str())
            {
                if best.map_or(true, |(_, bl)| m.comps.len() > bl) {
                    best = Some((mi, m.comps.len()));
                }
            }
        }
        let (mi, ml) = best.ok_or(Errno::EInval)?;
        let rel = comps[ml..].to_vec();
        Ok((self.mounts[mi].fs, rel))
    }

    fn full_path(&self, task: u32, path: &str) -> Result<String, Errno> {
        if path.starts_with('/') {
            Ok(crate::aerofs::normalize_path(path))
        } else {
            let t = self.task(task)?;
            let joined = format!("{}/{}", t.cwd, path);
            Ok(crate::aerofs::normalize_path(&joined))
        }
    }

    fn check_mount_free(&self, comps: &[String]) -> Result<(), Errno> {
        if self.mounts.iter().any(|m| m.comps == comps) {
            return Err(Errno::EExist);
        }
        Ok(())
    }
}

impl<K: Kernel, A: BufferAlloc> Default for Vfs<K, A> {
    fn default() -> Self {
        Self::new()
    }
}

fn mount_comps(path: &str) -> Vec<String> {
    crate::aerofs::path_comps(&crate::aerofs::normalize_path(path))
        .iter()
        .map(|c| c.to_string())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rights_of_access_modes() {
        assert_eq!(rights_of(O_RDONLY), R);
        assert_eq!(rights_of(O_WRONLY), W);
        assert_eq!(rights_of(O_RDWR), R | W);
        assert_eq!(rights_of(O_RDONLY | O_CREAT), R);
    }

    #[test]
    fn perm_ok_matrix() {
        // 0o640: owner rw, group r, other none.
        assert!(perm_ok(0o640, 1, 1, 1, 1, 0o400));
        assert!(perm_ok(0o640, 1, 1, 1, 1, 0o200));
        assert!(!perm_ok(0o640, 1, 1, 1, 1, 0o100));
        assert!(perm_ok(0o640, 1, 1, 2, 1, 0o400), "group read");
        assert!(!perm_ok(0o640, 1, 1, 2, 1, 0o200), "group cannot write");
        assert!(!perm_ok(0o640, 1, 1, 3, 3, 0o400), "other cannot read");
        assert!(perm_ok(0o000, 1, 1, 0, 0, 0o400), "root bypasses");
        assert!(perm_ok(0o444, 1, 1, 3, 3, 0o400), "world-readable");
    }

    #[test]
    fn resolve_longest_prefix() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        v.mounts.push(Mount {
            path: String::from("/"),
            comps: Vec::new(),
            fs: 0,
        });
        v.mounts.push(Mount {
            path: String::from("/tmp"),
            comps: vec![String::from("tmp")],
            fs: 1,
        });
        let (fs, rel) = v.resolve("/tmp/x").unwrap();
        assert_eq!(fs, 1);
        assert_eq!(rel, vec!["x"]);
        let (fs, rel) = v.resolve("/etc/passwd").unwrap();
        assert_eq!(fs, 0);
        assert_eq!(rel, vec!["etc", "passwd"]);
        // Longest prefix wins: /tmp/x goes to /tmp; /tmpx stays on /.
        let (fs, rel) = v.resolve("/tmpx").unwrap();
        assert_eq!(fs, 0);
        assert_eq!(rel, vec!["tmpx"]);
    }

    // ── pipes ────────────────────────────────────────────────────────────────

    #[test]
    fn pipe_roundtrip_and_eof() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        assert_eq!(v.write(0, w, b"hello").unwrap(), 5);
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 5);
        assert_eq!(&buf[..5], b"hello");
        // Empty but a writer remains: would-block.
        assert_eq!(v.read(0, r, &mut buf), Err(Errno::EAgain));
        // Last write end gone: EOF.
        v.close(0, w).unwrap();
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 0);
    }

    #[test]
    fn pipe_epipe_when_no_readers() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.close(0, r).unwrap();
        assert_eq!(v.write(0, w, b"x"), Err(Errno::EPipe));
    }

    #[test]
    fn pipe_dup_keeps_an_end_alive() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        // Two write ends; closing one must not EOF the reader.
        let w2 = v.dup(0, w).unwrap();
        v.close(0, w).unwrap();
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf), Err(Errno::EAgain), "writer still alive");
        v.close(0, w2).unwrap();
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 0, "now EOF");
    }

    #[test]
    fn pipe_dup2_replaces_and_recounts() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        // Overwrite the read end with a second copy of the write end: the
        // last reader is gone, so writes must EPIPE.
        v.dup2(0, w, r).unwrap();
        assert_eq!(v.write(0, r, b"x"), Err(Errno::EPipe), "no readers left");
        assert_eq!(v.write(0, w, b"y"), Err(Errno::EPipe));
        // And the original read slot is now a write end (rights checked).
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf), Err(Errno::EBadf));
    }

    #[test]
    fn pipe_rights_and_seekability() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        // Cross-direction ops are refused by the minted rights.
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, w, &mut buf), Err(Errno::EBadf));
        assert_eq!(v.write(0, r, b"x"), Err(Errno::EBadf));
        // Pipes are not seekable.
        assert_eq!(v.lseek(0, r, 0, SEEK_SET), Err(Errno::ESPipe));
        assert_eq!(v.lseek(0, w, 0, SEEK_CUR), Err(Errno::ESPipe));
        // fstat says FIFO with the buffered size.
        let st = v.fstat(0, r).unwrap();
        assert_eq!(st.ty, FileType::Fifo);
        assert_eq!(st.size, 0);
        v.write(0, w, b"abc").unwrap();
        assert_eq!(v.fstat(0, r).unwrap().size, 3);
        // No fs/inode identity for pipes.
        assert_eq!(v.offset_of(0, r), None);
        assert_eq!(v.fd_ino(0, r), None);
    }

    #[test]
    fn fork_copy_recounts_pipe_ends() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        let child = v.clone_task(0, 0).unwrap();
        // Parent drops its write end; the child's fork-copied one remains.
        v.close(0, w).unwrap();
        let mut buf = [0u8; 4];
        assert_eq!(
            v.read(0, r, &mut buf),
            Err(Errno::EAgain),
            "child's write end keeps the pipe open"
        );
        // Child exits: its table is cleared, last writer gone → EOF.
        v.exit_task(child).unwrap();
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 0);
    }

    #[test]
    fn exit_task_drops_pipe_ends() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        let child = v.clone_task(0, 0).unwrap();
        // Parent exits, abandoning both its ends. Had the counts not been
        // dropped, the child would still see two readers below.
        v.exit_task(0).unwrap();
        // Child's fork-copied ends work; the parent's are gone (not 2).
        assert_eq!(v.write(child, w, b"x").unwrap(), 1);
        // Child closes its read end — the last reader — so writes EPIPE.
        v.close(child, r).unwrap();
        assert_eq!(v.write(child, w, b"y"), Err(Errno::EPipe));
    }

    #[test]
    fn clone_files_shared_table_shares_pipe_ends() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        let thread = v.clone_task(0, CLONE_FILES).unwrap();
        // A thread's exit must NOT drop the shared ends (the parent still
        // holds them): write still succeeds.
        v.exit_task(thread).unwrap();
        assert_eq!(v.write(0, w, b"x").unwrap(), 1);
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 1);
        assert_eq!(&buf[..1], b"x");
    }

    // ── /dev ─────────────────────────────────────────────────────────────────

    #[test]
    fn console_roundtrip_and_null() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let console = Arc::new(CharNode::console());
        v.mount_devfs("/dev", console.clone()).unwrap();

        // Write to /dev/console → the console's output log.
        let w = v.open(0, "/dev/console", O_WRONLY, 0).unwrap();
        v.write(0, w, b"hi\n").unwrap();
        assert_eq!(console.console_io().output(), b"hi\n");
        v.close(0, w).unwrap();

        // Typed input is readable from the console.
        let r = v.open(0, "/dev/console", O_RDONLY, 0).unwrap();
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf), Err(Errno::EAgain), "nothing typed yet");
        console.console_io().push_input(b"ab");
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 2);
        assert_eq!(&buf[..2], b"ab");
        v.close(0, r).unwrap();

        // /dev/null: writes discarded, reads EOF.
        let n = v.open(0, "/dev/null", O_RDWR, 0).unwrap();
        assert_eq!(v.write(0, n, b"junk").unwrap(), 4);
        assert_eq!(v.read(0, n, &mut buf).unwrap(), 0);
        // Devices are not seekable.
        assert_eq!(v.lseek(0, n, 0, SEEK_SET), Err(Errno::ESPipe));
        // fstat: char device.
        assert_eq!(v.fstat(0, n).unwrap().ty, FileType::Char);
        // The device is listed in /dev.
        let names: Vec<String> = v
            .read_dir(0, "/dev")
            .unwrap()
            .iter()
            .map(|d| d.name.clone())
            .collect();
        assert!(names.contains(&String::from("console")) && names.contains(&String::from("null")));
    }

    #[test]
    fn console_perms_checked_against_creds() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        v.mount_devfs("/dev", Arc::new(CharNode::console())).unwrap();
        // Console is 0600 root; a non-root user cannot open it.
        v.set_cred(0, 1000, 1000).unwrap();
        assert_eq!(v.open(0, "/dev/console", O_RDWR, 0), Err(Errno::EAcces));
        // But /dev/null is 0666.
        assert!(v.open(0, "/dev/null", O_RDWR, 0).is_ok());
        // Root regains the console.
        v.set_cred(0, 0, 0).unwrap();
        assert!(v.open(0, "/dev/console", O_RDWR, 0).is_ok());
    }

    #[test]
    fn add_dev_node_registers_a_device() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        v.mount_devfs("/dev", Arc::new(CharNode::console())).unwrap();
        let bell = Arc::new(CharNode::null());
        v.add_dev_node("/dev", "bell", bell.clone()).unwrap();
        assert!(v.open(0, "/dev/bell", O_RDWR, 0).is_ok());
        assert_eq!(v.add_dev_node("/dev", "bell", bell.clone()), Err(Errno::EExist));
        // Not a devfs mount: rejected.
        assert_eq!(v.add_dev_node("/tmp", "x", bell), Err(Errno::EInval));
    }

    /// Kernelless stand-ins for unit tests that don't touch the device.
    mod dummy {
        use aerosls_proto::kabi::{CapInfo, GrantedCap, Kernel, RecvResult, SendCap};
        use aerosls_blockcache::BufferAlloc;

        pub struct NoKernel;
        pub struct NoAlloc;

        impl Kernel for NoKernel {
            fn wait(&self, _: &[u32], _: u64) -> Result<(usize, u16), i32> {
                Err(aerosls_proto::kabi::ERR_STATE)
            }
            fn recv(&self, _: u32, _: &mut [u8], _: &mut [GrantedCap]) -> Result<RecvResult, i32> {
                Err(aerosls_proto::kabi::ERR_STATE)
            }
            fn send(&self, _: u32, _: u32, _: u16, _: &[u8], _: &[SendCap], _: u64) -> Result<(), i32> {
                Err(aerosls_proto::kabi::ERR_STATE)
            }
            fn close(&self, _: u32, _: u16, _: u32) -> Result<(), i32> {
                Err(aerosls_proto::kabi::ERR_STATE)
            }
            fn cap_info(&self, _: u32) -> Result<CapInfo, i32> {
                Err(aerosls_proto::kabi::ERR_STATE)
            }
        }
        impl BufferAlloc for NoAlloc {
            fn alloc(&mut self, _: usize) -> Result<(SendCap, u64), i32> {
                Err(aerosls_proto::kabi::ERR_STATE)
            }
        }
    }
}
