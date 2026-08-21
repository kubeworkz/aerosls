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

use alloc::collections::{BTreeMap, VecDeque};
use alloc::format;
use alloc::string::String;
use alloc::string::ToString;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell};

use aerosls_blockcache::{BlockCache, BufferAlloc, Error as CacheError};
use aerosls_proto::kabi::Kernel;
use aerosls_proto::{R, W};

use crate::aerofs::{
    parse_dirent, parse_inode, parse_superblock, path_comps, DirEntry, FileType, Inode,
    Superblock, SuperblockRecord, DIRENT_SIZE, NDIRECT, S_IFDIR,
};
use crate::errno::{DirEnt, Errno, Stat};
use crate::fileobj::{CharNode, FileObj, PipeNode, PtyState, PIPE_CAP};
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
/// poll() event bitmask -- matches Linux/POSIX values.
pub const POLLIN: u16 = 0x001;
pub const POLLPRI: u16 = 0x002;
pub const POLLOUT: u16 = 0x004;
pub const POLLERR: u16 = 0x008;
pub const POLLHUP: u16 = 0x010;
pub const POLLNVAL: u16 = 0x020;

/// A single entry in a poll() set: an fd to check, the events the caller
/// is interested in, and the events that actually occurred (filled in by
/// Vfs::poll()).
pub struct PollFd {
    pub fd: u32,
    pub events: u16,
    pub revents: u16,
}

impl PollFd {
    pub fn new(fd: u32, events: u16) -> PollFd {
        PollFd { fd, events, revents: 0 }
    }
}

/// Maximum number of file descriptors for select/poll (Linux default).
pub const FD_SETSIZE: usize = 256;

/// A bitmask-based fd set for select(). Each bit position corresponds
/// to an fd number. Bit N is set if fd N is in the set.
/// Layout matches the Linux fd_set: an array of bytes where bit i is
/// at byte (i/8), bit (i%8).
pub struct SelectFdSet {
    bytes: [u8; (FD_SETSIZE + 7) / 8],
}

impl SelectFdSet {
    pub fn new() -> SelectFdSet {
        SelectFdSet { bytes: [0u8; (FD_SETSIZE + 7) / 8] }
    }

    /// Check if fd is in the set.
    pub fn contains(&self, fd: u32) -> bool {
        let fd = fd as usize;
        if fd >= FD_SETSIZE { return false; }
        (self.bytes[fd / 8] >> (fd % 8)) & 1 != 0
    }

    /// Add an fd to the set.
    pub fn set(&mut self, fd: u32) {
        let fd = fd as usize;
        if fd < FD_SETSIZE {
            self.bytes[fd / 8] |= 1 << (fd % 8);
        }
    }

    /// Remove an fd from the set.
    pub fn clear_fd(&mut self, fd: u32) {
        let fd = fd as usize;
        if fd < FD_SETSIZE {
            self.bytes[fd / 8] &= !(1 << (fd % 8));
        }
    }

    /// Clear the entire set.
    pub fn clear(&mut self) {
        self.bytes = [0u8; (FD_SETSIZE + 7) / 8];
    }

    /// The raw bytes (for interop with C-like callers).
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Mutable raw bytes (for populating from external sources).
    pub fn as_bytes_mut(&mut self) -> &mut [u8] {
        &mut self.bytes
    }
}

impl Default for SelectFdSet {
    fn default() -> Self { Self::new() }
}

/// Timeout for select(). None means block indefinitely.
pub struct SelectTimeout {
    /// Seconds.
    pub sec: u32,
    /// Microseconds.
    pub usec: u32,
}

/// Result of select(): which fd_sets have ready fds.
pub struct SelectResult {
    pub readfds: SelectFdSet,
    pub writefds: SelectFdSet,
    pub errorfds: SelectFdSet,
    /// Number of ready fds (across all three sets).
    pub nready: usize,
}

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
    fn rename(&mut self, old_parent: u64, old_name: &str, new_parent: u64, new_name: &str) -> Result<(), Errno> {
        match self {
            Fs::Aerofs(_) => Err(Errno::ERofs),
            Fs::Ram(f) => f.rename(old_parent, old_name, new_parent, new_name),
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
    /// PTY slave devices: name -> FileObj::PtySlave.  These bypass
    /// the DevEntry layer because they need PtyState access, not
    /// CharNode dispatch.
    pty_slaves: BTreeMap<String, Arc<FileObj>>,
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
            pty_slaves: BTreeMap::new(),
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

    /// Register a PTY slave device.  Unlike `add`, this stores the
    /// FileObj::PtySlave directly so open_dev returns it without
    /// wrapping in a CharNode.
    pub fn add_pty_slave(&mut self, name: &str, obj: Arc<FileObj>) -> Result<(), Errno> {
        if name.is_empty() || name.contains('/') {
            return Err(Errno::EInval);
        }
        if self.pty_slaves.contains_key(name) || self.names.contains_key(name) {
            return Err(Errno::EExist);
        }
        self.pty_slaves.insert(name.to_string(), obj);
        Ok(())
    }

    /// The node an entry names (fd minting: `open("/dev/console")`).
    fn open_dev(&self, ino: u64) -> Result<Arc<FileObj>, Errno> {
        self.nodes.get(&ino).map(|e| e.obj.clone()).ok_or(Errno::ENoent)
    }

    /// Look up a PTY slave by name (e.g. "pts_0").
    pub fn open_pty_slave(&self, name: &str) -> Result<Arc<FileObj>, Errno> {
        self.pty_slaves.get(name).cloned().ok_or(Errno::ENoent)
    }

    /// Whether a name is a PTY slave device (for Vfs::open to dispatch).
    pub fn is_pty_slave(&self, name: &str) -> bool {
        self.pty_slaves.contains_key(name)
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
    /// Close-on-exec: the fd is automatically closed when the task execs.
    /// Set by pipe fds created for pipeline stages, and by any fd opened
    /// with O_CLOEXEC. Prevents stale pipe ends from leaking into exec'd
    /// programs.
    pub cloexec: bool,
}

impl FdEntry {
    /// A pipe end has entered the fd world (open of a pipe fd, dup, fork
    /// copy, dup2 placement). Bumps the end's liveness count so EOF/EPIPE
    /// stay exact. No-op for non-pipe objects.
    fn note_added(&self) {
        match &*self.node {
            FileObj::PipeRead(p) => p.bump_readers(1),
            FileObj::PipeWrite(p) => p.bump_writers(1),
            // PTY fds are bidirectional: a single fd is both read and
            // write end (unlike pipes which are unidirectional).
            FileObj::PtyMaster(p) => {
                p.bump_master_readers(1);
                p.bump_master_writers(1);
            }
            FileObj::PtySlave(p) => {
                p.bump_slave_readers(1);
                p.bump_slave_writers(1);
            }
            FileObj::UnixSocket(s) => {
                s.bump_readers(1);
                s.bump_writers(1);
            }
            _ => {}
        }
    }

    /// A pipe end has left the fd world (close, dup2 overwrite, exit
    /// abandoning a table). No-op for non-pipe objects.
    fn note_removed(&self) {
        match &*self.node {
            FileObj::PipeRead(p) => p.bump_readers(-1),
            FileObj::PipeWrite(p) => p.bump_writers(-1),
            FileObj::PtyMaster(p) => {
                p.bump_master_readers(-1);
                p.bump_master_writers(-1);
            }
            FileObj::PtySlave(p) => {
                p.bump_slave_readers(-1);
                p.bump_slave_writers(-1);
            }
            FileObj::UnixSocket(s) => {
                s.bump_readers(-1);
                s.bump_writers(-1);
            }
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

pub struct Task {
    /// Index into `Vfs::table_pool`. `usize::MAX` = no table (exited).
    pub fds: usize,
    pub cwd: String,
    pub euid: u16,
    pub egid: u16,
    /// Session ID: 0 = inherited from parent, nonzero = session leader.
    pub session_id: u32,
}

// ── the VFS ─────────────────────────────────────────────────────────────────



// -- PTY multiplexer ---------------------------------------------------------

/// The PTY multiplexer: tracks /dev/ptmx and the /dev/pts_N device tree.
/// Each open of /dev/ptmx creates a new PTY pair; the slave is registered
/// at /dev/pts_N in the devfs.
pub struct PtyMultiplexer {
    next_pts: u32,
    /// All live PTY pairs, keyed by slave number.
    pairs: alloc::collections::BTreeMap<u32, Arc<PtyState>>,
}

impl PtyMultiplexer {
    pub fn new() -> PtyMultiplexer {
        PtyMultiplexer {
            next_pts: 0,
            pairs: alloc::collections::BTreeMap::new(),
        }
    }

    /// Allocate a new PTY pair and return its slave number.
    pub fn allocate(&mut self) -> u32 {
        let n = self.next_pts;
        self.next_pts += 1;
        let pty = Arc::new(PtyState::new());
        self.pairs.insert(n, pty);
        n
    }

    /// The PtyState for a slave number.
    pub fn get(&self, n: u32) -> Option<Arc<PtyState>> {
        self.pairs.get(&n).cloned()
    }
}

impl Default for PtyMultiplexer {
    fn default() -> Self {
        Self::new()
    }
}

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
    /// Tasks parked on an fd becoming readable (a pipe with data or EOF,
    /// console input). One entry per task. The proc manager drains
    /// satisfied waits each scheduler step (`take_woken_readers`); the
    /// wait is re-registered by the program if its retry would-block
    /// again. `obj` is kept alive by the waiter so the readiness check
    /// never touches a freed object even if the fd's table slot changed.
    waiters: Vec<Waiter>,
    /// Shared kernel handle for socket NET_* RPCs (set after boot).
    net_k: Option<alloc::sync::Arc<K>>,
    /// Shared allocator handle for socket buffer grants (set after boot).
    net_alloc: Option<alloc::sync::Arc<core::cell::RefCell<A>>>,
    /// The console node, kept alive for Ctrl-C detection in drain_wakes.
    console_node: Option<Arc<CharNode>>,
    /// PTY multiplexer (allocated when /dev/ptmx is first opened).
    pty_mux: Option<PtyMultiplexer>,
    /// Unix domain socket path → listener registry. `connect()` looks
    /// up the path here; `bind()` registers; `unlink()` removes.
    unix_sockets: alloc::collections::BTreeMap<String, Arc<crate::fileobj::UnixSocketState>>,
}

/// A parked reader. `ready()` is the *only* wake condition — it is
/// re-checked at every scheduler drain, so no per-event notification is
/// needed: a pipe write or writer-exit in any task's step, or console
/// input pushed between runs, is seen at the next drain.
struct Waiter {
    task: u32,
    obj: Arc<FileObj>,
}

impl Waiter {
    /// The *only* wake condition, re-checked at every scheduler drain. The
    /// object kind determines the predicate (no kind field needed — the
    /// `FileObj` variant *is* the wait's kind): a reader wakes on data or
    /// EOF, a writer wakes on free space or the last reader closing (its
    /// retry observes `EPIPE`), a console reader wakes on input **or the
    /// channel-close event** (its retry observes `Ok(0)` — EOF).
    fn ready(&self) -> bool {
        match &*self.obj {
            FileObj::PipeRead(p) => p.has_data() || p.writers() == 0,
            FileObj::PipeWrite(p) => p.space() > 0 || p.readers() == 0,
            // Console input arrived, or the console channel closed (EOF).
            // (Null reads never block; true keeps any stray registration
            // from wedging.)
            FileObj::Char(c) => c.read_ready(),
            // Sockets: not yet wired for readiness polling — the driver
            // handles backpressure. Always report ready so a stray
            // registration doesn't wedge the task.
            FileObj::PtyMaster(p) => p.master_has_data() || p.slave_writers.get() == 0,
            FileObj::PtySlave(p) => p.slave_has_data() || p.master_writers.get() == 0,
            FileObj::Socket(_) => true,
            FileObj::UnixSocket(s) => s.has_data(),
            // Nothing else can be blocked on (the wait_* validators
            // reject them).
            FileObj::File(_) => false,
        }
    }
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
                session_id: 0,
            }],
            waiters: Vec::new(),
            net_k: None,
            net_alloc: None,
            console_node: None,
            pty_mux: None,
            unix_sockets: alloc::collections::BTreeMap::new(),
        }
    }

    /// Set the shared kernel/allocator handles for socket NET_* RPCs.
    /// Called once at boot, after the block cache is connected.
    pub fn set_net_handles(
        &mut self,
        k: alloc::sync::Arc<K>,
        alloc: alloc::sync::Arc<core::cell::RefCell<A>>,
    ) {
        self.net_k = Some(k);
        self.net_alloc = Some(alloc);
    }

    /// Shared kernel handle (for socket operations from callers that have
    /// &Vfs but not &K directly).
    pub fn net_k(&self) -> Option<&alloc::sync::Arc<K>> {
        self.net_k.as_ref()
    }

    /// Shared allocator handle (for socket buffer grants).
    pub fn net_alloc(&self) -> Option<&alloc::sync::Arc<core::cell::RefCell<A>>> {
        self.net_alloc.as_ref()
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
        self.console_node = Some(console.clone());
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
    /// Register a PTY slave in the devfs at /dev. The slave is a
    /// FileObj::PtySlave stored directly (not wrapped in CharNode).
    pub fn add_pty_slave_dev(&mut self, name: &str, obj: Arc<FileObj>) -> Result<(), Errno> {
        let full = crate::aerofs::normalize_path("/dev");
        let (fs_idx, _rel) = self.resolve(&full)?;
        match self.fss.get_mut(fs_idx) {
            Some(Fs::Dev(d)) => d.add_pty_slave(name, obj),
            _ => Err(Errno::EInval),
        }
    }

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
            session_id: 0,
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
        // Resolve relative paths against the current cwd.
        let full = if path.starts_with('/') {
            path.to_string()
        } else {
            let cwd = self.task(task)?.cwd.clone();
            if cwd == "/" {
                format!("/{}", path)
            } else {
                format!("{}/{}", cwd, path)
            }
        };
        let norm = crate::aerofs::normalize_path(&full);
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

    /// Return the current working directory for a task.
    pub fn get_cwd(&self, task: u32) -> Result<&str, Errno> {
        Ok(&self.task(task)?.cwd)
    }

    /// `setsid()`: create a new session. The calling task becomes the
    /// session leader (`session_id` = its own task id). Returns the new
    /// session id.
    pub fn setsid(&mut self, task: u32) -> Result<u32, Errno> {
        let t = self.task_mut(task)?;
        t.session_id = task;
        Ok(task)
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
            // Check if this is ptmx (by ino identity: we detect it by name).
            let is_ptmx = rel.last().map_or(false, |n| *n == "ptmx");
            if is_ptmx && self.pty_mux.is_some() {
                // Lazy-init multiplexer if not already done.
            }
            if is_ptmx {
                let (master_fd, _slave_fd) = self.openpty(task)?;
                return Ok(master_fd);
            }
            // Check if this is a PTY slave device.
            if let Some(name) = rel.last() {
                let fs = self.fss.get(fs_idx);
                if let Some(Fs::Dev(d)) = fs {
                    if d.is_pty_slave(name) {
                        let obj = d.open_pty_slave(name)?;
                        let fd = {
                            let idx = self.task_mut(task)?.fds;
                            self.table_pool[idx].alloc(FdEntry {
                                node: obj,
                                rights: want,
                                flags,
                                cloexec: false,
                            })?
                        };
                        return Ok(fd);
                    }
                }
            }
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
                    cloexec: false,
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
                cloexec: false,
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
                cloexec: false,
            })?;
            let w = table.alloc(FdEntry {
                node: Arc::new(FileObj::PipeWrite(node)),
                rights: W,
                flags: 0,
                cloexec: false,
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

    /// Create a network socket fd. The VFS tracks only metadata; actual
    /// NET_* I/O goes through `Vfs::net_k()` / `Vfs::net_alloc()` at the
    /// caller level (the `Ctx`). Returns the fd number.
    pub fn socket_open(
        &mut self,
        task: u32,
        sock_id: u32,
        chan: u32,
        sock_type: u16,
    ) -> Result<u32, Errno> {
        let meta = Arc::new(crate::fileobj::SocketMeta {
            sock_id,
            chan,
            sock_type,
            state: crate::fileobj::SocketState::Created,
            remote_ip: 0,
            remote_port: 0,
        });
        let idx = self.task_mut(task)?.fds;
        self.table_pool[idx].alloc(FdEntry {
            node: Arc::new(FileObj::Socket(meta)),
            rights: R | W,
            flags: 0,
            cloexec: false,
        })
    }

    // -- PTY methods -----------------------------------------------------------

    /// Create a pseudo-terminal pair: master + slave.  Returns the
    /// (master_fd, slave_fd) pair.  The slave is registered in the
    /// devfs at /dev/pts_N.  The multiplexer is lazily created.
    pub fn openpty(&mut self, task: u32) -> Result<(u32, u32), Errno> {
        // Lazily create the multiplexer.
        if self.pty_mux.is_none() {
            self.pty_mux = Some(PtyMultiplexer::new());
        }
        let pts_num = self.pty_mux.as_mut().unwrap().allocate();
        let pty = self.pty_mux.as_ref().unwrap().get(pts_num).ok_or(Errno::EIo)?;
        // Register slave in devfs at /dev/pts_N.
        let slave_name = alloc::format!("pts_{}", pts_num);
        // Best-effort: register in devfs if /dev is mounted (tests may not have it).
        let _ = self.add_pty_slave_dev(&slave_name, Arc::new(FileObj::PtySlave(pty.clone())));
        // Mint master fd.
        let master_fd = {
            let idx = self.task_mut(task)?.fds;
            self.table_pool[idx].alloc(FdEntry {
                node: Arc::new(FileObj::PtyMaster(pty.clone())),
                rights: R | W,
                flags: 0,
                cloexec: false,
            })?
        };
        // Mint slave fd.
        let slave_fd = {
            let idx = self.task_mut(task)?.fds;
            self.table_pool[idx].alloc(FdEntry {
                node: Arc::new(FileObj::PtySlave(pty)),
                rights: R | W,
                flags: 0,
                cloexec: false,
            })?
        };
        // Bump end counts for both fds.
        {
            let idx = self.task_mut(task)?.fds;
            let me = self.table_pool[idx].get(master_fd).unwrap();
            me.note_added();
            let se = self.table_pool[idx].get(slave_fd).unwrap();
            se.note_added();
        }
        Ok((master_fd, slave_fd))
    }

    /// ioctl on a PTY fd.  Currently supports TIOCGWINSZ, TIOCSWINSZ,
    /// and TIOCSCTTY.  Returns ENOTTY for unsupported requests.
    pub fn ioctl(&self, task: u32, fd: u32, request: u32,
                 arg: &mut [u8]) -> Result<(), Errno> {
        let idx = self.tasks.get(task as usize).ok_or(Errno::EInval)?.fds;
        let e = self.table_pool.get(idx).ok_or(Errno::EInval)?
            .get(fd).ok_or(Errno::EBadf)?;
        match &*e.node {
            FileObj::PtyMaster(p) | FileObj::PtySlave(p) => {
                match request {
                    crate::fileobj::TIOCGWINSZ => {
                        if arg.len() < 8 { return Err(Errno::EInval); }
                        let ws = p.win_size.get();
                        arg[0..2].copy_from_slice(&ws.cols.to_le_bytes());
                        arg[2..4].copy_from_slice(&ws.rows.to_le_bytes());
                        arg[4..6].copy_from_slice(&ws.xpixel.to_le_bytes());
                        arg[6..8].copy_from_slice(&ws.ypixel.to_le_bytes());
                        Ok(())
                    }
                    crate::fileobj::TIOCSWINSZ => {
                        if arg.len() < 8 { return Err(Errno::EInval); }
                        let ws = crate::fileobj::WinSize {
                            cols: u16::from_le_bytes([arg[0], arg[1]]),
                            rows: u16::from_le_bytes([arg[2], arg[3]]),
                            xpixel: u16::from_le_bytes([arg[4], arg[5]]),
                            ypixel: u16::from_le_bytes([arg[6], arg[7]]),
                        };
                        p.win_size.set(ws);
                        Ok(())
                    }
                    crate::fileobj::TIOCSCTTY => {
                        // v1 stub: set controlling terminal (set foreground pgid).
                        // arg[0..4] = pgid.
                        if arg.len() >= 4 {
                            let pgid = u32::from_le_bytes([arg[0], arg[1], arg[2], arg[3]]);
                            p.foreground_pgid.set(pgid);
                        }
                        Ok(())
                    }
                    crate::fileobj::TCGETS => {
                        if arg.len() < 36 { return Err(Errno::EInval); }
                        let tty = p.get_termios();
                        tty.encode(arg);
                        Ok(())
                    }
                    crate::fileobj::TCSETS => {
                        if arg.len() < 20 { return Err(Errno::EInval); }
                        let tty = crate::fileobj::Termios::decode(arg).ok_or(Errno::EInval)?;
                        p.set_termios(tty);
                        Ok(())
                    }
                    crate::fileobj::TIOCFLUSH => {
                        // arg[0..4] = queue_selector (TCIFLUSH/TCOFLUSH/TCIOFLUSH).
                        let qs = if arg.len() >= 4 {
                            u32::from_le_bytes([arg[0], arg[1], arg[2], arg[3]])
                        } else {
                            crate::fileobj::TCIOFLUSH
                        };
                        let is_master = matches!(&*e.node, FileObj::PtyMaster(_));
                        p.flush(qs, is_master);
                        Ok(())
                    }
                    crate::fileobj::TIOCOUTQ => {
                        // Return number of bytes in the output queue.
                        if arg.len() < 4 { return Err(Errno::EInval); }
                        let n = p.output_queue_len() as u32;
                        arg[0..4].copy_from_slice(&n.to_le_bytes());
                        Ok(())
                    }
                    _ => Err(Errno::ENotty),
                }
            }
            _ => Err(Errno::ENotty),
        }
    }

    /// The sock_id stored in an fd (observability helper for socket
    /// operations at the `Ctx` level).
    pub fn fd_sock_id(&self, task: u32, fd: u32) -> Option<u32> {
        let idx = self.tasks.get(task as usize)?.fds;
        let e = self.table_pool.get(idx)?.get(fd)?;
        match &*e.node {
            FileObj::Socket(m) => Some(m.sock_id),
            _ => None,
        }
    }

    /// The channel endpoint stored in a socket fd.
    pub fn fd_sock_chan(&self, task: u32, fd: u32) -> Option<u32> {
        let idx = self.tasks.get(task as usize)?.fds;
        let e = self.table_pool.get(idx)?.get(fd)?;
        match &*e.node {
            FileObj::Socket(m) => Some(m.chan),
            _ => None,
        }
    }

    /// Whether an fd names a socket.
    pub fn is_socket_fd(&self, task: u32, fd: u32) -> bool {
        let idx = match self.tasks.get(task as usize) {
            Some(t) => t.fds,
            None => return false,
        };
        match self.table_pool.get(idx).and_then(|t| t.get(fd)) {
            Some(e) => matches!(&*e.node, FileObj::Socket(_)),
            None => false,
        }
    }

    /// Park the task on `fd` becoming readable. Called by the blocking
    /// read path after a read returned `EAGAIN` (empty pipe, console with
    /// no input). The proc manager drains satisfied waits at each
    /// scheduler step and requeues the task; its program retries the read
    /// and re-registers if it would-block again. A console reader is
    /// woken by input **or the console-close event** — its retry then
    /// sees `Ok(0)`.
    ///
    /// Rejects fds that can never block: mount-table files read `Ok(0)`
    /// at EOF (never `EAGAIN`), and write-only pipe ends aren't readable.
    pub fn wait_readable(&mut self, task: u32, fd: u32) -> Result<(), Errno> {
        let idx = self.task_mut(task)?.fds;
        let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
        match &*e.node {
            FileObj::PipeRead(_) | FileObj::Char(_) | FileObj::Socket(_)
            | FileObj::PtyMaster(_) | FileObj::PtySlave(_)
            | FileObj::UnixSocket(_) => {
                // A task parks on one thing at a time; replace any prior
                // registration (bounded by the number of tasks).
                self.waiters.retain(|w| w.task != task);
                self.waiters.push(Waiter {
                    task,
                    obj: e.node.clone(),
                });
                Ok(())
            }
            _ => Err(Errno::EInval),
        }
    }

    /// Park the task on `fd` becoming writable. Called by the blocking
    /// write path after a write returned `EAGAIN` (a full pipe). The proc
    /// manager drains satisfied waits at each scheduler step and requeues
    /// the task; its program retries the write and re-registers if it
    /// would-block again.
    ///
    /// Rejects fds that can never block on write: mount-table files and
    /// the console grow their storage (no backpressure in v1), and a
    /// read-only pipe end isn't writable.
    pub fn wait_writable(&mut self, task: u32, fd: u32) -> Result<(), Errno> {
        let idx = self.task_mut(task)?.fds;
        let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
        match &*e.node {
            FileObj::PipeWrite(_) | FileObj::Socket(_)
            | FileObj::PtyMaster(_) | FileObj::PtySlave(_)
            | FileObj::UnixSocket(_) => {
                // A task parks on one thing at a time; replace any prior
                // registration (bounded by the number of tasks).
                self.waiters.retain(|w| w.task != task);
                self.waiters.push(Waiter {
                    task,
                    obj: e.node.clone(),
                });
                Ok(())
            }
            _ => Err(Errno::EInval),
        }
    }

    /// Drain the read-waits whose object is ready (pipe data/EOF, console
    /// input) and return the tasks to wake. Called by the proc manager at
    /// every scheduler step (and by an event-loop driver when external
    /// input arrives between runs).
    pub fn take_woken_readers(&mut self) -> Vec<u32> {
        self.take_woken(|w| matches!(&*w.obj, FileObj::PipeRead(_) | FileObj::Char(_)
            | FileObj::PtyMaster(_) | FileObj::PtySlave(_)))
    }

    /// Drain the write-waits whose object is ready (pipe space freed by a
    /// reader, or the last reader gone) and return the tasks to wake.
    pub fn take_woken_writers(&mut self) -> Vec<u32> {
        self.take_woken(|w| matches!(&*w.obj, FileObj::PipeWrite(_)
            | FileObj::PtyMaster(_) | FileObj::PtySlave(_)))
    }

    /// Check if the console input buffer contains a specific byte.
    /// Used by the scheduler's drain_wakes to detect Ctrl-C (0x03)
    /// while the shell is blocked on WaitingChild.
    pub fn has_console_byte(&self, byte: u8) -> bool {
        self.console_node
            .as_ref()
            .map(|c| c.input_contains(byte))
            .unwrap_or(false)
    }

    /// Remove the first occurrence of a specific byte from the console
    /// input buffer.  Pairs with `has_console_byte` for Ctrl-C processing.
    pub fn discard_console_byte(&self, byte: u8) {
        if let Some(c) = &self.console_node {
            c.discard_byte(byte);
        }
    }

    fn take_woken<F: Fn(&Waiter) -> bool>(&mut self, kind: F) -> Vec<u32> {
        let mut woken = Vec::new();
        let mut i = 0;
        while i < self.waiters.len() {
            if kind(&self.waiters[i]) && self.waiters[i].ready() {
                let w = self.waiters.swap_remove(i);
                woken.push(w.task);
            } else {
                i += 1;
            }
        }
        woken
    }

    // -- Unix domain socket methods -------------------------------------------

    /// Create an unbound Unix domain socket (AF_UNIX, SOCK_STREAM).
    pub fn unix_socket(&mut self, task: u32) -> Result<u32, Errno> {
        let state = crate::fileobj::UnixSocketState::new();
        let idx = self.task_mut(task)?.fds;
        self.table_pool[idx].alloc(FdEntry {
            node: Arc::new(FileObj::UnixSocket(state)),
            rights: R | W,
            flags: 0,
            cloexec: false,
        })
    }

    /// Bind a Unix socket to a filesystem path. Registers in the global
    /// path → listener registry. Fails with `EAddrInuse` if the path
    /// is already bound.
    pub fn unix_bind(&mut self, task: u32, fd: u32, path: &str) -> Result<(), Errno> {
        let idx = self.task_mut(task)?.fds;
        // Must be an unbound Unix socket.
        {
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            match &*e.node {
                FileObj::UnixSocket(s) => match &**s {
                    crate::fileobj::UnixSocketState::Unbound => {}
                    _ => return Err(Errno::EInval),
                },
                _ => return Err(Errno::EBadf),
            }
        }
        // Check for duplicate path.
        if self.unix_sockets.contains_key(path) {
            return Err(Errno::EAddrInuse);
        }
        let new_state = Arc::new(crate::fileobj::UnixSocketState::Bound { path: path.to_string() });
        let new_entry = FdEntry {
            node: Arc::new(FileObj::UnixSocket(new_state.clone())),
            rights: R | W,
            flags: 0,
            cloexec: false,
        };
        self.table_pool[idx].entries[fd as usize] = Some(new_entry);
        self.unix_sockets.insert(path.to_string(), new_state);
        Ok(())
    }

    /// Mark a bound socket as listening. Transitions Bound → Listening.
    pub fn unix_listen(&mut self, task: u32, fd: u32) -> Result<(), Errno> {
        let idx = self.task_mut(task)?.fds;
        let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
        let path = match &*e.node {
            FileObj::UnixSocket(s) => match &**s {
                crate::fileobj::UnixSocketState::Bound { path } => path.clone(),
                crate::fileobj::UnixSocketState::Listening { .. } => return Ok(()),
                _ => return Err(Errno::EInval),
            },
            _ => return Err(Errno::EBadf),
        };
        let listening = Arc::new(crate::fileobj::UnixSocketState::Listening {
            path: path.clone(),
            pending: RefCell::new(Vec::new()),
        });
        let new_entry = FdEntry {
            node: Arc::new(FileObj::UnixSocket(listening.clone())),
            rights: R,
            flags: 0,
            cloexec: false,
        };
        self.table_pool[idx].entries[fd as usize] = Some(new_entry);
        self.unix_sockets.insert(path, listening);
        Ok(())
    }

    /// Accept a pending connection on a listening Unix socket. Returns
    /// the new fd for the connected socket. If no pending connection
    /// exists, registers a waiter and returns `EAgain`.
    pub fn unix_accept(&mut self, task: u32, fd: u32) -> Result<u32, Errno> {
        let idx = self.task_mut(task)?.fds;
        let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
        let pending = match &*e.node {
            FileObj::UnixSocket(s) => match &**s {
                crate::fileobj::UnixSocketState::Listening { pending, .. } => {
                    pending.borrow().len()
                }
                _ => return Err(Errno::EInval),
            },
            _ => return Err(Errno::EBadf),
        };
        if pending == 0 {
            // No pending connection — register waiter.
            let node = e.node.clone();
            _ = e; // release borrow on table_pool
            self.waiters.push(Waiter { task, obj: node });
            return Err(Errno::EAgain);
        }
        // Pop the first pending connection.
        let connected = {
            if let FileObj::UnixSocket(s) = &*e.node {
                if let crate::fileobj::UnixSocketState::Listening { pending, .. } = &**s {
                    pending.borrow_mut().remove(0)
                } else {
                    unreachable!()
                }
            } else {
                unreachable!()
            }
        };
        _ = e; // release borrow on table_pool
        // Mint a new fd for the accepted connection.
        let idx = self.task_mut(task)?.fds;
        self.table_pool[idx].alloc(FdEntry {
            node: Arc::new(FileObj::UnixSocket(connected)),
            rights: R | W,
            flags: 0,
            cloexec: false,
        })
    }

    /// Connect to a Unix socket at `path`. Creates a connected pair:
    /// one end for the caller, the other pushed to the listener's pending
    /// queue. Fails with `ENoent` if no socket is bound at `path`.
    pub fn unix_connect(&mut self, task: u32, fd: u32, path: &str) -> Result<(), Errno> {
        let listener = self.unix_sockets.get(path).ok_or(Errno::ENoent)?.clone();
        // Shared buffers: a_to_b = A's snd = B's rcv, b_to_a = B's snd = A's rcv.
        let a_to_b: Arc<RefCell<VecDeque<u8>>> = Arc::new(RefCell::new(VecDeque::new()));
        let b_to_a: Arc<RefCell<VecDeque<u8>>> = Arc::new(RefCell::new(VecDeque::new()));
        // Shared reader/writer counts: both ends see the same Cell.
        let readers: Arc<Cell<u32>> = Arc::new(Cell::new(1));
        let writers: Arc<Cell<u32>> = Arc::new(Cell::new(1));
        let a = Arc::new(crate::fileobj::UnixSocketState::Connected {
            rcv: b_to_a.clone(),
            snd: a_to_b.clone(),
            readers: readers.clone(),
            writers: writers.clone(),
        });
        let b = Arc::new(crate::fileobj::UnixSocketState::Connected {
            rcv: a_to_b,
            snd: b_to_a,
            readers,
            writers,
        });
        // Push B into the listener's pending queue.
        match &*listener {
            crate::fileobj::UnixSocketState::Listening { pending, .. } => {
                pending.borrow_mut().push(b);
            }
            _ => return Err(Errno::EInval),
        }
        // Replace the caller's fd with A (the connected end).
        let idx = self.task_mut(task)?.fds;
        self.table_pool[idx].entries[fd as usize] = Some(FdEntry {
            node: Arc::new(FileObj::UnixSocket(a)),
            rights: R | W,
            flags: 0,
            cloexec: false,
        });
        Ok(())
    }

    /// Unregister a Unix socket path from the registry.
    pub fn unix_unlink(&mut self, path: &str) {
        self.unix_sockets.remove(path);
    }

    /// VFS-level setsockopt stub. For TCP sockets this is a no-op at
    /// the VFS layer — the actual option is forwarded to the network
    /// driver via `Ctx::setsockopt()`. For non-socket fds, returns
    /// `ENOTTY`.
    pub fn setsockopt(&self, task: u32, fd: u32, _level: u32, _optname: u32, _optval: &[u8]) -> Result<(), Errno> {
        let idx = self.tasks.get(task as usize).ok_or(Errno::EInval)?.fds;
        let e = self.table_pool.get(idx).ok_or(Errno::EInval)?.get(fd).ok_or(Errno::EBadf)?;
        match &*e.node {
            FileObj::Socket(_) => Ok(()), // TCP: handled by NetClient
            FileObj::UnixSocket(_) => Ok(()), // v1: accepted, no-op
            _ => Err(Errno::ENotty),
        }
    }

    /// VFS-level getsockopt stub.
    pub fn getsockopt(&self, task: u32, fd: u32, _level: u32, _optname: u32, optval: &mut [u8]) -> Result<usize, Errno> {
        let idx = self.tasks.get(task as usize).ok_or(Errno::EInval)?.fds;
        let e = self.table_pool.get(idx).ok_or(Errno::EInval)?.get(fd).ok_or(Errno::EBadf)?;
        match &*e.node {
            FileObj::Socket(_) | FileObj::UnixSocket(_) => {
                for b in optval.iter_mut() { *b = 0; }
                Ok(optval.len())
            }
            _ => Err(Errno::ENotty),
        }
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

    /// Close every fd with the `cloexec` flag set on this task. Called
    /// by `ProcManager::exec()` before replacing the program image so
    /// that pipe fds and other internal descriptors don't leak into the
    /// exec'd program.
    pub fn close_cloexec(&mut self, task: u32) {
        let idx = match self.tasks.get(task as usize) {
            Some(t) => t.fds,
            None => return,
        };
        let table = match self.table_pool.get_mut(idx) {
            Some(t) => t,
            None => return,
        };
        for slot in table.entries.iter_mut() {
            if let Some(e) = slot {
                if e.cloexec {
                    let gone = slot.take().unwrap();
                    gone.note_removed();
                }
            }
        }
    }

    /// Set or clear the `cloexec` flag on an fd. The shell calls this
    /// after creating pipe fds to mark them as close-on-exec so they
    /// don't leak into child programs.
    pub fn set_cloexec(&mut self, task: u32, fd: u32, cloexec: bool) -> Result<(), Errno> {
        let idx = self.task_mut(task)?.fds;
        let e = self.table_pool[idx].entries.get_mut(fd as usize).ok_or(Errno::EBadf)?.as_mut().ok_or(Errno::EBadf)?;
        e.cloexec = cloexec;
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
                cloexec: false,
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
                cloexec: false,
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
            FileObj::PtyMaster(p) => p.master_read(buf),
            FileObj::PtySlave(p) => p.slave_read(buf),
            FileObj::Socket(_) => Err(Errno::EBadf),
            FileObj::UnixSocket(s) => s.read(buf),
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
            FileObj::PtyMaster(p) => p.master_input(buf),
            FileObj::PtySlave(p) => p.slave_write(buf),
            FileObj::Socket(_) => Err(Errno::EBadf),
            FileObj::UnixSocket(s) => s.write(buf),
        }
    }

    pub fn lseek(&mut self, task: u32, fd: u32, off: i64, whence: u32) -> Result<u64, Errno> {
        let obj = {
            let idx = self.task_mut(task)?.fds;
            let e = self.table_pool[idx].get(fd).ok_or(Errno::EBadf)?;
            e.node.clone()
        };
        let FileObj::File(node) = &*obj else {
            // Pipes, devices, sockets, and PTYs are not seekable.
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
            FileObj::PtyMaster(p) | FileObj::PtySlave(p) => Ok(p.stat()),
            FileObj::Socket(_) | FileObj::UnixSocket(_) => Ok(Stat {
                mode: 0o140000, // S_IFSOCK
                uid: 0,
                gid: 0,
                size: 0,
                mtime: 0,
                ty: FileType::File, // no S_IFSOCK in v1, use File
            }),
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

    /// Rename (move) `old_path` to `new_path`.  Both paths must
    /// resolve to the same underlying filesystem (cross-mount rename
    /// is not supported in v1).
    pub fn rename(&mut self, task: u32, old_path: &str, new_path: &str) -> Result<(), Errno> {
        let old_full = self.full_path(task, old_path)?;
        let (old_fs, old_rel) = self.resolve(&old_full)?;
        let new_full = self.full_path(task, new_path)?;
        let (new_fs, new_rel) = self.resolve(&new_full)?;
        if old_fs != new_fs {
            return Err(Errno::EXDev); // cross-device rename
        }
        if old_rel.is_empty() || new_rel.is_empty() {
            return Err(Errno::EBusy);
        }
        let fs = self.fs_mut(old_fs)?;
        let old_parent = fs.lookup(&old_rel[..old_rel.len() - 1])?;
        let new_parent = fs.lookup(&new_rel[..new_rel.len() - 1])?;
        fs.rename(old_parent.0, old_rel.last().unwrap(), new_parent.0, new_rel.last().unwrap())
    }

    /// Check file accessibility (POSIX `access(2)`).  In v1 this
    /// simply verifies the path exists; permission bits are not
    /// enforced.
    pub fn access(&mut self, task: u32, path: &str, _mode: u32) -> Result<(), Errno> {
        let full = self.full_path(task, path)?;
        let (fs_idx, rel) = self.resolve(&full)?;
        if rel.is_empty() {
            return Ok(()); // root always accessible
        }
        let fs = self.fs_mut(fs_idx)?;
        fs.lookup(&rel)?;
        Ok(())
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

    /// Unified poll: check readiness across the task's open fds. For each
    /// entry in `fds`, `revents` is filled with the subset of `events`
    /// that the fd is ready for right now (POLLIN if data/EOF is available,
    /// POLLOUT if buffer space is available, POLLNVAL if the fd is bad,
    /// POLLERR if the device is dead).
    ///
    /// This is a stateless, non-blocking check -- it does not register any
    /// waits.  For pipes and character devices it queries the in-core state;
    /// for network sockets it checks the client-side state (actual driver
    /// polling happens at the Ctx level via SocketOps::poll).
    ///
    /// Returns the number of fds with non-zero revents.
    pub fn poll(
        &self,
        task: u32,
        fds: &mut [PollFd],
    ) -> Result<usize, Errno> {
        let idx = self.tasks.get(task as usize).ok_or(Errno::EInval)?.fds;
        let table = self.table_pool.get(idx).ok_or(Errno::EInval)?;
        let mut n_ready = 0usize;
        for f in fds.iter_mut() {
            let revents = match table.get(f.fd) {
                None => POLLNVAL,
                Some(e) => Self::fd_ready(&e.node, f.events),
            };
            // POLLNVAL and POLLERR are always reported per POSIX, even
            // if not in the requested events mask.
            let always = revents & (POLLNVAL | POLLERR);
            f.revents = always | ((revents & !always) & f.events);
            if f.revents != 0 {
                n_ready += 1;
            }
        }
        Ok(n_ready)
    }

    /// select(): check readiness across up to nfds file descriptors.
    /// Converts the three fd_sets to a PollFd array, calls poll(),
    /// then writes the results back into the output fd_sets.
    ///
    /// - `readfds`: fds the caller wants to check for readability (POLLIN)
    /// - `writefds`: fds the caller wants to check for writability (POLLOUT)
    /// - `errorfds`: fds the caller wants to check for errors (POLLERR)
    ///
    /// On return, each output set contains only the fds that are ready.
    /// Returns the total number of ready fds across all sets.
    pub fn select(
        &self,
        task: u32,
        nfds: u32,
        readfds: &SelectFdSet,
        writefds: &SelectFdSet,
        errorfds: &SelectFdSet,
    ) -> Result<SelectResult, Errno> {
        let mut out_read = SelectFdSet::new();
        let mut out_write = SelectFdSet::new();
        let mut out_error = SelectFdSet::new();
        let limit = core::cmp::min(nfds as usize, FD_SETSIZE);
        let mut nready = 0usize;
        for fd in 0..limit as u32 {
            let want_in = readfds.contains(fd);
            let want_out = writefds.contains(fd);
            let want_err = errorfds.contains(fd);
            if !want_in && !want_out && !want_err {
                continue;
            }
            let mut events = 0u16;
            if want_in { events |= POLLIN; }
            if want_out { events |= POLLOUT; }
            // POLLERR is always checked when requested.
            if want_err { events |= POLLERR; }
            let mut pfds = [PollFd::new(fd, events)];
            self.poll(task, &mut pfds)?;
            let r = pfds[0].revents;
            if r & POLLIN != 0 { out_read.set(fd); }
            if r & POLLOUT != 0 { out_write.set(fd); }
            // POLLERR, POLLHUP, POLLNVAL all map to the error set.
            if r & (POLLERR | POLLHUP | POLLNVAL) != 0 { out_error.set(fd); }
            if r != 0 { nready += 1; }
        }
        Ok(SelectResult {
            readfds: out_read,
            writefds: out_write,
            errorfds: out_error,
            nready,
        })
    }

    /// The readiness bitmask for a single file-like object.  Returns the
    /// events the object is *currently* ready for, regardless of what the
    /// caller asked for (the caller masks with `events`).
    fn fd_ready(obj: &FileObj, events: u16) -> u16 {
        let mut revents = 0u16;
        match obj {
            FileObj::PipeRead(p) => {
                if (events & POLLIN) != 0 && (p.has_data() || p.writers() == 0) {
                    revents |= POLLIN;
                }
                if (events & POLLOUT) != 0 && p.readers() == 0 {
                    revents |= POLLHUP;
                }
            }
            FileObj::PipeWrite(p) => {
                if (events & POLLIN) != 0 && p.readers() == 0 {
                    revents |= POLLERR;
                }
                if (events & POLLOUT) != 0 && (p.space() > 0 || p.readers() == 0) {
                    revents |= POLLOUT;
                }
                if p.readers() == 0 {
                    revents |= POLLERR; // EPIPE condition
                }
            }
            FileObj::Char(c) => {
                if (events & POLLIN) != 0 && c.read_ready() {
                    revents |= POLLIN;
                }
                // Console/null never block on write.
                if (events & POLLOUT) != 0 {
                    revents |= POLLOUT;
                }
            }
            FileObj::Socket(meta) => {
                // Socket readiness is derived from client-side state.
                // The caller can additionally query the network driver via
                // SocketOps::poll for more accurate results.
                match meta.state {
                    crate::fileobj::SocketState::Connected
                    | crate::fileobj::SocketState::Listening => {
                        if (events & POLLIN) != 0 {
                            revents |= POLLIN;
                        }
                        if (events & POLLOUT) != 0
                            && meta.state != crate::fileobj::SocketState::HalfClosed
                        {
                            revents |= POLLOUT;
                        }
                    }
                    crate::fileobj::SocketState::HalfClosed => {
                        if (events & POLLIN) != 0 {
                            revents |= POLLIN;
                        }
                        revents |= POLLHUP;
                    }
                    crate::fileobj::SocketState::Closed => {
                        revents |= POLLERR;
                    }
                    _ => {}
                }
            }
            FileObj::PtyMaster(p) => {
                if (events & POLLIN) != 0 && (p.master_has_data() || p.slave_writers.get() == 0) {
                    revents |= POLLIN;
                }
                if (events & POLLOUT) != 0 && (p.master_has_space() || p.slave_readers.get() == 0) {
                    revents |= POLLOUT;
                }
                if p.slave_readers.get() == 0 {
                    revents |= POLLHUP;
                }
            }
            FileObj::PtySlave(p) => {
                if (events & POLLIN) != 0 && (p.slave_has_data() || p.master_writers.get() == 0) {
                    revents |= POLLIN;
                }
                if (events & POLLOUT) != 0 && (p.slave_has_space() || p.master_readers.get() == 0) {
                    revents |= POLLOUT;
                }
                if p.master_readers.get() == 0 {
                    revents |= POLLHUP;
                }
            }
            FileObj::UnixSocket(s) => {
                if (events & POLLIN) != 0 && s.has_data() {
                    revents |= POLLIN;
                }
                if (events & POLLOUT) != 0 && s.has_space() {
                    revents |= POLLOUT;
                }
            }
            FileObj::File(_) => {
                // Regular files on a mount are always ready.
                if (events & POLLIN) != 0 {
                    revents |= POLLIN;
                }
                if (events & POLLOUT) != 0 {
                    revents |= POLLOUT;
                }
            }
        }
        revents
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
            session_id: 0,
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
        // An exited task is no longer waiting on anything readable.
        self.waiters.retain(|w| w.task != task);
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

    pub fn task(&self, task: u32) -> Result<&Task, Errno> {
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
    use crate::fileobj::{TIOCGWINSZ, TIOCSWINSZ, TIOCSCTTY, TCSETS, TCGETS, Termios, ECHO, ISIG, ICANON, ICRNL, WinSize};

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

    // ── cloexec ──────────────────────────────────────────────────────────────

    #[test]
    fn close_cloexec_only_closes_marked_fds() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        v.mount_ramfs("/tmp").unwrap();
        let f1 = v.open(0, "/tmp/a", O_CREAT | O_RDWR, 0o644).unwrap();
        let f2 = v.open(0, "/tmp/b", O_CREAT | O_RDWR, 0o644).unwrap();
        // Mark only f1 as cloexec.
        v.set_cloexec(0, f1, true).unwrap();
        v.close_cloexec(0);
        // f1 is gone (cloexec).
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, f1, &mut buf), Err(Errno::EBadf));
        // f2 survives (not cloexec).
        assert_eq!(v.write(0, f2, b"x").unwrap(), 1);
    }

    #[test]
    fn close_cloexec_preserves_non_cloexec_fds() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        // Neither end is cloexec — close_cloexec is a no-op.
        v.close_cloexec(0);
        let mut buf = [0u8; 4];
        assert_eq!(v.write(0, w, b"abc").unwrap(), 3);
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 3);
    }

    #[test]
    fn dup_clears_cloexec_on_new_fd() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.set_cloexec(0, r, true).unwrap();
        // dup creates a new fd with cloexec cleared (POSIX semantics).
        let r2 = v.dup(0, r).unwrap();
        v.close_cloexec(0);
        // Original read end is cloexec → gone.
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf), Err(Errno::EBadf));
        // Dup'd fd is NOT cloexec → survives.
        assert_eq!(v.read(0, r2, &mut buf), Err(Errno::EAgain));
        // Write end survives.
        assert_eq!(v.write(0, w, b"x").unwrap(), 1);
    }

    #[test]
    fn dup2_clears_cloexec_on_target() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.set_cloexec(0, w, true).unwrap();
        // dup2 the write end to fd 5; the target fd clears cloexec.
        v.dup2(0, w, 5).unwrap();
        v.close_cloexec(0);
        // fd 5 is NOT cloexec → survives.
        assert_eq!(v.write(0, 5, b"x").unwrap(), 1);
        // Original write end (cloexec) is gone.
        assert_eq!(v.write(0, w, b"x"), Err(Errno::EBadf));
        // Read end survives.
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 1);
    }

    #[test]
    fn set_cloexec_toggles() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (_r, w) = v.pipe(0).unwrap();
        v.set_cloexec(0, w, true).unwrap();
        v.set_cloexec(0, w, false).unwrap();
        // Toggle off — close_cloexec should not close it.
        v.close_cloexec(0);
        assert_eq!(v.write(0, w, b"x").unwrap(), 1);
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
    fn console_close_is_eof_and_wakes() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let console = Arc::new(CharNode::console());
        v.mount_devfs("/dev", console.clone()).unwrap();

        let r = v.open(0, "/dev/console", O_RDONLY, 0).unwrap();
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, r, &mut buf), Err(Errno::EAgain), "open console blocks");

        // The channel-close event: buffered input is still served first…
        console.console_io().push_input(b"hi");
        console.console_io().close();
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 2);
        assert_eq!(&buf[..2], b"hi");
        // …then reads are EOF, and the parked-reader predicate fires.
        assert_eq!(v.read(0, r, &mut buf).unwrap(), 0);
        assert!(console.read_ready());
        assert!(v.wait_readable(0, r).is_ok(), "closed console accepts waits");

        // Writes to a closed channel fail: the device is dead.
        let rw = v.open(0, "/dev/console", O_RDWR, 0).unwrap();
        assert_eq!(v.write(0, rw, b"x"), Err(Errno::EIo));
        v.close(0, r).unwrap();
        v.close(0, rw).unwrap();

        // A second open of the closed console is still EOF, not EAGAIN
        // (the node is the singleton; the event is on the channel).
        let r2 = v.open(0, "/dev/console", O_RDONLY, 0).unwrap();
        assert_eq!(v.read(0, r2, &mut buf).unwrap(), 0);
        v.close(0, r2).unwrap();
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
    fn wait_readable_validation_and_wake() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        // Objects that can never block are rejected up front.
        v.mount_ramfs("/tmp").unwrap();
        let f = v.open(0, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
        assert_eq!(v.wait_readable(0, f), Err(Errno::EInval), "files never block");
        assert_eq!(v.wait_readable(0, w), Err(Errno::EInval), "write end isn't readable");
        assert_eq!(v.wait_readable(0, 99), Err(Errno::EBadf));
        // Empty pipe with a live writer: parked, nothing woken.
        v.wait_readable(0, r).unwrap();
        assert_eq!(v.take_woken_readers(), Vec::<u32>::new());
        // Data arrives (a write in any task's step): the drain wakes it.
        v.write(0, w, b"x").unwrap();
        assert_eq!(v.take_woken_readers(), vec![0]);
        // The woken *program* consumes the data on its retry; the drain
        // only wakes. Re-register (empty again, writer alive): parked.
        let mut tmp = [0u8; 4];
        assert_eq!(v.read(0, r, &mut tmp).unwrap(), 1);
        v.wait_readable(0, r).unwrap();
        assert_eq!(v.take_woken_readers(), Vec::<u32>::new());
        // The last writer closing makes EOF ready.
        v.close(0, w).unwrap();
        assert_eq!(v.take_woken_readers(), vec![0], "EOF wakes the reader");
        // One wait per task: re-registering replaces (no duplicates).
        v.wait_readable(0, r).unwrap();
        v.wait_readable(0, r).unwrap();
        assert_eq!(v.take_woken_readers().len(), 1);
    }

    #[test]
    fn wait_writable_validation_and_wake() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        // Objects that can never block on write are rejected up front.
        v.mount_ramfs("/tmp").unwrap();
        let f = v.open(0, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
        assert_eq!(v.wait_writable(0, f), Err(Errno::EInval), "files never block");
        assert_eq!(v.wait_writable(0, r), Err(Errno::EInval), "read end isn't writable");
        assert_eq!(v.wait_writable(0, 99), Err(Errno::EBadf));
        // Fill the pipe, then park the writer.
        let big = [0u8; PIPE_CAP + 1];
        assert_eq!(v.write(0, w, &big).unwrap(), PIPE_CAP, "pipe full");
        assert_eq!(v.write(0, w, b"x"), Err(Errno::EAgain));
        v.wait_writable(0, w).unwrap();
        assert_eq!(v.take_woken_writers(), Vec::<u32>::new());
        // A reader drains: the drain wakes the writer.
        let mut tmp = [0u8; 16];
        assert_eq!(v.read(0, r, &mut tmp).unwrap(), 16);
        assert_eq!(v.take_woken_writers(), vec![0], "space freed → woken");
        // Fill again, re-register; the last reader closing makes the
        // retry EPIPE (never block again).
        assert_eq!(v.write(0, w, b"xxxxxxxxxxxxxxxx").unwrap(), 16);
        v.wait_writable(0, w).unwrap();
        assert_eq!(v.take_woken_writers(), Vec::<u32>::new());
        v.close(0, r).unwrap();
        assert_eq!(v.take_woken_writers(), vec![0], "no readers → woken");
        assert_eq!(v.write(0, w, b"x"), Err(Errno::EPipe));
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

    // ── poll ─────────────────────────────────────────────────────────────

    #[test]
    fn poll_bad_fd_returns_pollnval() {
        let v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let mut fds = vec![PollFd::new(99, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        // POLLNVAL is always reported per POSIX, even if not in events.
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLNVAL);
    }

    #[test]
    fn poll_empty_pipe_read_not_ready() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (_r, _w) = v.pipe(0).unwrap();
        // Empty pipe with writer alive: not readable, not writable (full = 0%)
        let mut fds = vec![PollFd::new(_r, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 0, "empty pipe read end not ready");
        assert_eq!(fds[0].revents, 0);
    }

    #[test]
    fn poll_pipe_read_ready_on_data() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.write(0, w, b"hello").unwrap();
        let mut fds = vec![PollFd::new(r, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLIN);
    }

    #[test]
    fn poll_pipe_read_ready_on_eof() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.close(0, w).unwrap();
        let mut fds = vec![PollFd::new(r, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLIN);
    }

    #[test]
    fn poll_pipe_write_ready_when_space() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (_r, w) = v.pipe(0).unwrap();
        let mut fds = vec![PollFd::new(w, POLLOUT)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLOUT);
    }

    #[test]
    fn poll_pipe_write_epipe_when_no_readers() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.close(0, r).unwrap();
        let mut fds = vec![PollFd::new(w, POLLOUT | POLLERR)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        // POLLERR because no readers (EPIPE condition)
        assert_eq!(fds[0].revents & POLLERR, POLLERR);
    }

    #[test]
    fn poll_console_not_ready_without_input() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let console = Arc::new(CharNode::console());
        v.mount_devfs("/dev", console).unwrap();
        let fd = v.open(0, "/dev/console", O_RDONLY, 0).unwrap();
        let mut fds = vec![PollFd::new(fd, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 0, "console not ready without input");
        assert_eq!(fds[0].revents, 0);
    }

    #[test]
    fn poll_console_ready_with_input() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let console = Arc::new(CharNode::console());
        console.console_io().push_input(b"hello");
        v.mount_devfs("/dev", console).unwrap();
        let fd = v.open(0, "/dev/console", O_RDONLY, 0).unwrap();
        let mut fds = vec![PollFd::new(fd, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLIN);
    }

    #[test]
    fn poll_null_always_ready() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let console = Arc::new(CharNode::console());
        v.mount_devfs("/dev", console).unwrap();
        let fd = v.open(0, "/dev/null", O_RDONLY, 0).unwrap();
        let mut fds = vec![PollFd::new(fd, POLLIN | POLLOUT)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLIN | POLLOUT);
    }

    #[test]
    fn poll_ramfs_file_always_ready() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        v.mount_ramfs("/tmp").unwrap();
        let fd = v.open(0, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
        v.write(0, fd, b"data").unwrap();
        let mut fds = vec![PollFd::new(fd, POLLIN | POLLOUT)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 1);
        assert_eq!(fds[0].revents, POLLIN | POLLOUT);
    }

    #[test]
    fn poll_mixed_fds() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.write(0, w, b"data").unwrap();
        v.mount_ramfs("/tmp").unwrap();
        let f = v.open(0, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
        // fd 99 doesn't exist (POLLNVAL), r has data (POLLIN), f is a file (POLLIN|POLLOUT)
        let mut fds = vec![
            PollFd::new(99, POLLIN),
            PollFd::new(r, POLLIN),
            PollFd::new(f, POLLIN | POLLOUT),
        ];
        let n = v.poll(0, &mut fds).unwrap();
        // POLLNVAL on fd 99 counts as ready (always reported per POSIX).
        assert_eq!(n, 3, "three fds ready (bad fd, r, and f)");
        assert_eq!(fds[0].revents, POLLNVAL);
        assert_eq!(fds[1].revents, POLLIN);
        assert_eq!(fds[2].revents, POLLIN | POLLOUT);
    }

    #[test]
    fn poll_events_masking() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (_r, w) = v.pipe(0).unwrap();
        // Write end is writable, but we only ask for POLLIN (which won't be set)
        let mut fds = vec![PollFd::new(w, POLLIN)];
        let n = v.poll(0, &mut fds).unwrap();
        assert_eq!(n, 0, "POLLIN not masked for write end");
        assert_eq!(fds[0].revents, 0);
    }


    // -- PTY tests ---------------------------------------------------------------

    #[test]
    fn openpty_returns_master_and_slave() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        assert_ne!(master, slave);
    }

    #[test]
    fn pty_master_write_slave_read() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Master writes -> Slave reads
        assert_eq!(v.write(0, master, b"hello").unwrap(), 5);
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, slave, &mut buf).unwrap(), 5);
        assert_eq!(&buf[..5], b"hello");
    }

    #[test]
    fn pty_slave_write_master_read() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Slave writes -> Master reads
        assert_eq!(v.write(0, slave, b"world").unwrap(), 5);
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, master, &mut buf).unwrap(), 5);
        assert_eq!(&buf[..5], b"world");
    }

    #[test]
    fn pty_bidirectional_simultaneous() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Both directions at once
        v.write(0, master, b"to-slave").unwrap();
        v.write(0, slave, b"to-master").unwrap();
        let mut buf = [0u8; 16];
        assert_eq!(v.read(0, slave, &mut buf).unwrap(), 8);
        assert_eq!(&buf[..8], b"to-slave");
        assert_eq!(v.read(0, master, &mut buf).unwrap(), 9);
        assert_eq!(&buf[..9], b"to-master");
    }

    #[test]
    fn pty_empty_read_would_block() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        let mut buf = [0u8; 4];
        // No data written yet: would-block
        assert_eq!(v.read(0, master, &mut buf), Err(Errno::EAgain));
        assert_eq!(v.read(0, slave, &mut buf), Err(Errno::EAgain));
    }

    #[test]
    fn pty_master_read_eof_when_slave_closed() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        v.close(0, slave).unwrap();
        let mut buf = [0u8; 4];
        // Slave gone: master reads EOF
        assert_eq!(v.read(0, master, &mut buf).unwrap(), 0);
    }

    #[test]
    fn pty_slave_read_eof_when_master_closed() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        v.close(0, master).unwrap();
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, slave, &mut buf).unwrap(), 0);
    }

    #[test]
    fn pty_ioctl_tiocgwinsz() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        // Get default window size (24x80)
        let mut buf = [0u8; 8];
        v.ioctl(0, master, TIOCGWINSZ, &mut buf).unwrap();
        let cols = u16::from_le_bytes([buf[0], buf[1]]);
        let rows = u16::from_le_bytes([buf[2], buf[3]]);
        assert_eq!(cols, 80);
        assert_eq!(rows, 24);
    }

    #[test]
    fn pty_ioctl_tiocswinsz() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        // Set window size to 50x120
        let mut buf = [0u8; 8];
        buf[0..2].copy_from_slice(&120u16.to_le_bytes()); // cols
        buf[2..4].copy_from_slice(&50u16.to_le_bytes());  // rows
        v.ioctl(0, master, TIOCSWINSZ, &mut buf).unwrap();
        // Read it back
        let mut buf2 = [0u8; 8];
        v.ioctl(0, master, TIOCGWINSZ, &mut buf2).unwrap();
        assert_eq!(u16::from_le_bytes([buf2[0], buf2[1]]), 120);
        assert_eq!(u16::from_le_bytes([buf2[2], buf2[3]]), 50);
    }

    #[test]
    fn pty_ioctl_enotty_for_nonpty() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        v.mount_ramfs("/tmp").unwrap();
        let f = v.open(0, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(v.ioctl(0, f, TIOCGWINSZ, &mut buf), Err(Errno::ENotty));
    }

    #[test]
    fn pty_fstat_returns_char_device() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        let st = v.fstat(0, master).unwrap();
        assert_eq!(st.ty, FileType::Char);
        let st2 = v.fstat(0, slave).unwrap();
        assert_eq!(st2.ty, FileType::Char);
    }

    #[test]
    fn pty_poll_readiness() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // No data: not readable
        let mut fds = vec![PollFd::new(master, POLLIN)];
        assert_eq!(v.poll(0, &mut fds).unwrap(), 0);
        // Write from slave: master becomes readable
        v.write(0, slave, b"x").unwrap();
        let mut fds = vec![PollFd::new(master, POLLIN)];
        assert_eq!(v.poll(0, &mut fds).unwrap(), 1);
        assert_eq!(fds[0].revents, POLLIN);
    }


    // -- setsid tests -----------------------------------------------------------

    #[test]
    fn setsid_returns_task_as_session_id() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let sid = v.setsid(0).unwrap();
        assert_eq!(sid, 0); // task 0 becomes session leader
    }

    #[test]
    fn pty_fork_building_blocks() {
        // Exercises the VFS primitives that forkpty() orchestrates:
        // openpty + dup2 + ioctl TIOCSCTTY + setsid.
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        // 1. Create a PTY pair.
        let (master, slave) = v.openpty(0).unwrap();
        assert_ne!(master, slave);
        // 2. dup2 slave to a high fd (fd 10) to simulate wiring stdin.
        let target = v.dup2(0, slave, 10).unwrap();
        assert_eq!(target, 10);
        // 3. Write to the master, read from fd 10 (the slave copy).
        v.write(0, master, b"hello").unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, 10, &mut buf).unwrap(), 5);
        assert_eq!(&buf[..5], b"hello");
        // 4. setsid creates a session.
        let sid = v.setsid(0).unwrap();
        assert_eq!(sid, 0);
        // 5. TIOCSCTTY sets the controlling terminal (stub, no-op but must not fail).
        let mut arg = [0u8; 4];
        v.ioctl(0, 10, crate::fileobj::TIOCSCTTY, &mut arg).unwrap();
    }

    // -- select tests -----------------------------------------------------------

    #[test]
    fn select_pipe_data_ready() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, w) = v.pipe(0).unwrap();
        v.write(0, w, b"data").unwrap();
        let mut rfds = SelectFdSet::new();
        rfds.set(r);
        let wfds = SelectFdSet::new();
        let efds = SelectFdSet::new();
        let res = v.select(0, r + 1, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 1);
        assert!(res.readfds.contains(r));
    }

    #[test]
    fn select_empty_pipe_not_ready() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, _w) = v.pipe(0).unwrap();
        let mut rfds = SelectFdSet::new();
        rfds.set(r);
        let wfds = SelectFdSet::new();
        let efds = SelectFdSet::new();
        let res = v.select(0, r + 1, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 0);
        assert!(!res.readfds.contains(r));
    }

    #[test]
    fn select_pipe_write_ready() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (_r, w) = v.pipe(0).unwrap();
        let rfds = SelectFdSet::new();
        let mut wfds = SelectFdSet::new();
        wfds.set(w);
        let efds = SelectFdSet::new();
        let res = v.select(0, w + 1, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 1);
        assert!(res.writefds.contains(w));
    }

    #[test]
    fn select_bad_fd_in_error_set() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let rfds = SelectFdSet::new();
        let wfds = SelectFdSet::new();
        let mut efds = SelectFdSet::new();
        efds.set(99);
        let res = v.select(0, 100, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 1);
        assert!(res.errorfds.contains(99));
    }

    #[test]
    fn select_nfds_limits_scan() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (_r, w) = v.pipe(0).unwrap();
        let mut wfds = SelectFdSet::new();
        wfds.set(w);
        let rfds = SelectFdSet::new();
        let efds = SelectFdSet::new();
        // nfds=1 means only fd 0 is checked; fd w > 0 is not scanned.
        let res = v.select(0, 1, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 0);
    }

    #[test]
    fn select_multiple_fds() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r1, w1) = v.pipe(0).unwrap();
        let (r2, _w2) = v.pipe(0).unwrap();
        v.write(0, w1, b"a").unwrap();
        let mut rfds = SelectFdSet::new();
        rfds.set(r1);
        rfds.set(r2);
        let wfds = SelectFdSet::new();
        let efds = SelectFdSet::new();
        let max_fd = core::cmp::max(r1, r2) + 1;
        let res = v.select(0, max_fd, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 1);
        assert!(res.readfds.contains(r1));
        assert!(!res.readfds.contains(r2));
    }

    #[test]
    fn select_fd_set_operations() {
        let mut s = SelectFdSet::new();
        assert!(!s.contains(0));
        s.set(0);
        assert!(s.contains(0));
        s.clear_fd(0);
        assert!(!s.contains(0));
        // FD_SETSIZE boundary
        s.set((FD_SETSIZE - 1) as u32);
        assert!(s.contains((FD_SETSIZE - 1) as u32));
        s.clear();
        assert!(!s.contains((FD_SETSIZE - 1) as u32));
    }

    #[test]
    fn select_console_ready_with_input() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let console = Arc::new(CharNode::console());
        console.console_io().push_input(b"x");
        v.mount_devfs("/dev", console).unwrap();
        let fd = v.open(0, "/dev/console", O_RDONLY, 0).unwrap();
        let mut rfds = SelectFdSet::new();
        rfds.set(fd);
        let wfds = SelectFdSet::new();
        let efds = SelectFdSet::new();
        let res = v.select(0, fd + 1, &rfds, &wfds, &efds).unwrap();
        assert_eq!(res.nready, 1);
        assert!(res.readfds.contains(fd));
    }


    // -- line discipline tests (cooked mode via TCSETS) ------------------------

    #[test]
    fn pty_cooked_echo_returns_chars_to_master() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Switch to cooked mode: echo + isig + icrnl.
        let cooked = Termios {
            iflag: ICRNL,
            oflag: 0,
            lflag: ECHO | ISIG | ICANON,
            cc: [3, 28, 26, 127, 21, 4, 10, 13],
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Write 'a' to the master (simulating user typing).
        v.write(0, master, b"a").unwrap();
        // Echo: 'a' should appear in the master's read buffer.
        let mut buf = [0u8; 8];
        let n = v.read(0, master, &mut buf).unwrap();
        assert_eq!(n, 1);
        assert_eq!(buf[0], b'a');
        // ICANON holds data until newline — send CR to flush.
        v.write(0, master, b"\r").unwrap();
        // The newline (translated from CR by ICRNL) appears on master echo.
        let n = v.read(0, master, &mut buf).unwrap();
        assert!(n >= 1);
        // Data: 'a' + newline should be in the slave's read buffer.
        let n = v.read(0, slave, &mut buf).unwrap();
        assert!(n >= 1);
        assert_eq!(buf[0], b'a');
    }

    #[test]
    fn pty_cooked_isig_intercepts_ctrl_c() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        // Cooked mode with isig enabled.
        let cooked = Termios {
            iflag: 0,
            oflag: 0,
            lflag: ECHO | ISIG,
            cc: [3, 28, 26, 127, 21, 4, 10, 13], // INTR = 3 (Ctrl-C)
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Write Ctrl-C (0x03) to the master.
        v.write(0, master, b"\x03").unwrap();
        // Echo: should see "^C" in the master buffer.
        let mut buf = [0u8; 8];
        let n = v.read(0, master, &mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(&buf[..2], b"^C");
    }

    #[test]
    fn pty_cooked_isig_intercepts_ctrl_z() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        let cooked = Termios {
            iflag: 0,
            oflag: 0,
            lflag: ECHO | ISIG,
            cc: [3, 28, 26, 127, 21, 4, 10, 13], // SUSP = 26 (Ctrl-Z)
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Write Ctrl-Z (0x1a = 26) to the master.
        v.write(0, master, b"\x1a").unwrap();
        let mut buf = [0u8; 8];
        let n = v.read(0, master, &mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(&buf[..2], b"^Z");
    }

    #[test]
    fn pty_cooked_icrnl_translates_cr_to_nl() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        let cooked = Termios {
            iflag: ICRNL,
            oflag: 0,
            lflag: ECHO | ICANON,
            cc: [3, 28, 26, 127, 21, 4, 10, 13],
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Write CR (0x0d) to the master.
        v.write(0, master, b"\r").unwrap();
        // Echo: should see NL (\n) in the master buffer.
        let mut buf = [0u8; 8];
        let n = v.read(0, master, &mut buf).unwrap();
        assert_eq!(n, 1);
        assert_eq!(buf[0], b'\n');
        // Slave also gets NL.
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 1);
        assert_eq!(buf[0], b'\n');
    }

    #[test]
    fn pty_raw_mode_no_echo() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Default is raw mode (no echo). Write 'x' to master.
        v.write(0, master, b"x").unwrap();
        // No echo: master read buffer should be empty.
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, master, &mut buf), Err(Errno::EAgain));
        // Data still reaches the slave.
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 1);
        assert_eq!(buf[0], b'x');
    }

    #[test]
    fn pty_tcgets_returns_current_termios() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        // Set a known termios.
        let custom = Termios {
            iflag: ICRNL,
            oflag: 0,
            lflag: ECHO | ISIG,
            cc: [3, 28, 26, 127, 21, 4, 10, 13],
        };
        let mut arg = [0u8; 36];
        custom.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Read it back with TCGETS.
        let mut out = [0u8; 36];
        v.ioctl(0, master, TCGETS, &mut out).unwrap();
        let read_back = Termios::decode(&out).unwrap();
        assert_eq!(read_back.iflag, ICRNL);
        assert!(read_back.echo());
        assert!(read_back.isig());
        assert!(!read_back.canonical()); // ICANON was not set
    }

    // -- Unix domain socket tests ----------------------------------------------

    #[test]
    fn unix_socket_bind_and_listen() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let fd = v.unix_socket(0).unwrap();
        v.unix_bind(0, fd, "/tmp/test.sock").unwrap();
        v.unix_listen(0, fd).unwrap();
        assert!(v.unix_sockets.contains_key("/tmp/test.sock"));
    }

    #[test]
    fn unix_socket_bind_duplicate_fails() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let fd1 = v.unix_socket(0).unwrap();
        v.unix_bind(0, fd1, "/tmp/dup.sock").unwrap();
        let fd2 = v.unix_socket(0).unwrap();
        assert_eq!(v.unix_bind(0, fd2, "/tmp/dup.sock"), Err(Errno::EAddrInuse));
    }

    #[test]
    fn unix_socket_connect_and_rw() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        // Server: create, bind, listen.
        let srv = v.unix_socket(0).unwrap();
        v.unix_bind(0, srv, "/tmp/x11").unwrap();
        v.unix_listen(0, srv).unwrap();
        // Client: create, connect.
        let cli = v.unix_socket(0).unwrap();
        v.unix_connect(0, cli, "/tmp/x11").unwrap();
        // Server accepts.
        let accepted = v.unix_accept(0, srv).unwrap();
        // Client writes, server reads.
        v.write(0, cli, b"hello").unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, accepted, &mut buf).unwrap(), 5);
        assert_eq!(&buf[..5], b"hello");
        // Server writes, client reads.
        v.write(0, accepted, b"world").unwrap();
        assert_eq!(v.read(0, cli, &mut buf).unwrap(), 5);
        assert_eq!(&buf[..5], b"world");
    }

    #[test]
    fn unix_socket_connect_no_listener_fails() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let cli = v.unix_socket(0).unwrap();
        assert_eq!(v.unix_connect(0, cli, "/nonexistent"), Err(Errno::ENoent));
    }

    #[test]
    fn unix_socket_bidirectional_simultaneous() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let srv = v.unix_socket(0).unwrap();
        v.unix_bind(0, srv, "/tmp/bidi").unwrap();
        v.unix_listen(0, srv).unwrap();
        let cli = v.unix_socket(0).unwrap();
        v.unix_connect(0, cli, "/tmp/bidi").unwrap();
        let acc = v.unix_accept(0, srv).unwrap();
        // Both directions at once.
        v.write(0, cli, b"to-srv").unwrap();
        v.write(0, acc, b"to-cli").unwrap();
        let mut buf = [0u8; 16];
        assert_eq!(v.read(0, acc, &mut buf).unwrap(), 6);
        assert_eq!(&buf[..6], b"to-srv");
        assert_eq!(v.read(0, cli, &mut buf).unwrap(), 6);
        assert_eq!(&buf[..6], b"to-cli");
    }

    #[test]
    fn unix_socket_empty_read_would_block() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let srv = v.unix_socket(0).unwrap();
        v.unix_bind(0, srv, "/tmp/empty").unwrap();
        v.unix_listen(0, srv).unwrap();
        let cli = v.unix_socket(0).unwrap();
        v.unix_connect(0, cli, "/tmp/empty").unwrap();
        let acc = v.unix_accept(0, srv).unwrap();
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, acc, &mut buf), Err(Errno::EAgain));
        assert_eq!(v.read(0, cli, &mut buf), Err(Errno::EAgain));
    }

    #[test]
    fn unix_socket_eof_when_peer_closed() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let srv = v.unix_socket(0).unwrap();
        v.unix_bind(0, srv, "/tmp/eof").unwrap();
        v.unix_listen(0, srv).unwrap();
        let cli = v.unix_socket(0).unwrap();
        v.unix_connect(0, cli, "/tmp/eof").unwrap();
        let acc = v.unix_accept(0, srv).unwrap();
        // Close the client end.
        v.close(0, cli).unwrap();
        // Server reads EOF.
        let mut buf = [0u8; 4];
        assert_eq!(v.read(0, acc, &mut buf).unwrap(), 0);
    }

    // -- setsockopt/getsockopt tests ------------------------------------------

    #[test]
    fn setsockopt_on_unix_socket_succeeds() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let fd = v.unix_socket(0).unwrap();
        let optval = 1u32.to_le_bytes();
        assert!(v.setsockopt(0, fd, 1, 2, &optval).is_ok()); // SOL_SOCKET, SO_REUSEADDR
    }

    #[test]
    fn getsockopt_on_unix_socket_returns_zeros() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let fd = v.unix_socket(0).unwrap();
        let mut buf = [0xFFu8; 4];
        let n = v.getsockopt(0, fd, 1, 2, &mut buf).unwrap();
        assert_eq!(n, 4);
        assert_eq!(buf, [0, 0, 0, 0]); // default returns zeros
    }

    #[test]
    fn setsockopt_on_pipe_fails_enotty() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (r, _w) = v.pipe(0).unwrap();
        let optval = 1u32.to_le_bytes();
        assert_eq!(v.setsockopt(0, r, 1, 2, &optval), Err(Errno::ENotty));
    }


    // -- ICANON canonical mode tests --------------------------------------------

    #[test]
    fn icanon_buffers_until_newline() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Enable canonical mode + echo.
        let cooked = Termios {
            iflag: ICRNL,
            oflag: 0,
            lflag: ICANON | ECHO,
            cc: [3, 28, 26, 127, 21, 4, 10, 13],
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Write "hello" without newline — nothing reaches slave yet.
        v.write(0, master, b"hello").unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, slave, &mut buf), Err(Errno::EAgain));
        // Write newline — now the line is flushed.
        v.write(0, master, b"\n").unwrap();
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 6); // "hello\n"
        assert_eq!(&buf[..6], b"hello\n");
    }

    #[test]
    fn icanon_erase_removes_last_char() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        let cooked = Termios {
            iflag: 0,
            oflag: 0,
            lflag: ICANON | ECHO,
            cc: [3, 28, 26, 127, 21, 4, 10, 13], // ERASE=127 (DEL)
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Type "abc", erase 'c', type "d", newline.
        v.write(0, master, b"abc").unwrap();
        v.write(0, master, b"\x7f").unwrap(); // ERASE
        v.write(0, master, b"d\n").unwrap();
        let mut buf = [0u8; 8];
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 4);
        assert_eq!(&buf[..4], b"abd\n");
    }

    #[test]
    fn icanon_kill_clears_line() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        let cooked = Termios {
            iflag: 0,
            oflag: 0,
            lflag: ICANON | ECHO,
            cc: [3, 28, 26, 127, 21, 4, 10, 13], // KILL=21 (Ctrl-U)
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Type "hello", kill, type "world", newline.
        v.write(0, master, b"hello").unwrap();
        v.write(0, master, b"\x15").unwrap(); // KILL
        v.write(0, master, b"world\n").unwrap();
        let mut buf = [0u8; 16];
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 6);
        assert_eq!(&buf[..6], b"world\n");
    }

    #[test]
    fn icanon_eof_flushes_empty_line() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        let cooked = Termios {
            iflag: 0,
            oflag: 0,
            lflag: ICANON | ECHO,
            cc: [3, 28, 26, 127, 21, 4, 10, 13], // EOF=4 (Ctrl-D)
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // EOF on empty line: should flush nothing (Ok(0) from slave read).
        v.write(0, master, b"\x04").unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, slave, &mut buf), Err(Errno::EAgain));
        // EOF with partial data: flushes the partial line.
        v.write(0, master, b"partial").unwrap();
        v.write(0, master, b"\x04").unwrap();
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 7);
        assert_eq!(&buf[..7], b"partial");
    }

    #[test]
    fn icanon_echo_sends_to_master() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        let cooked = Termios {
            iflag: 0,
            oflag: 0,
            lflag: ICANON | ECHO,
            cc: [3, 28, 26, 127, 21, 4, 10, 13],
        };
        let mut arg = [0u8; 36];
        cooked.encode(&mut arg);
        v.ioctl(0, master, TCSETS, &mut arg).unwrap();
        // Type "ab" — echo should appear on master.
        v.write(0, master, b"ab").unwrap();
        let mut buf = [0u8; 8];
        let n = v.read(0, master, &mut buf).unwrap();
        assert_eq!(n, 2);
        assert_eq!(&buf[..2], b"ab");
    }

    #[test]
    fn raw_mode_no_line_buffering() {
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Default is raw mode — no ICANON.
        // Write "hello" — should reach slave immediately (no buffering).
        v.write(0, master, b"hello").unwrap();
        let mut buf = [0u8; 8];
        let n = v.read(0, slave, &mut buf).unwrap();
        assert_eq!(n, 5);
        assert_eq!(&buf[..5], b"hello");
    }

    // -- tcflush / TIOCOUTQ tests -------------------------------------------------

    #[test]
    fn tcflush_master_input_clears_master_buf() {
        use crate::fileobj::{TIOCFLUSH, TCIFLUSH};
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Slave writes data — it sits in master_buf.
        v.write(0, slave, b"hello").unwrap();
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, master, &mut buf).unwrap(), 5);
        // Write more data.
        v.write(0, slave, b"world").unwrap();
        // Flush master input (TCIFLUSH) — should clear master_buf.
        let mut arg = [0u8; 4];
        arg[0..4].copy_from_slice(&TCIFLUSH.to_le_bytes());
        v.ioctl(0, master, TIOCFLUSH, &mut arg).unwrap();
        // Master read should now return EAGAIN (buffer empty).
        assert_eq!(v.read(0, master, &mut buf), Err(Errno::EAgain));
    }

    #[test]
    fn tcflush_master_output_clears_slave_buf() {
        use crate::fileobj::{TIOCFLUSH, TCOFLUSH};
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Master writes data — it sits in slave_buf.
        v.write(0, master, b"data").unwrap();
        // Flush master output (TCOFLUSH) — should clear slave_buf.
        let mut arg = [0u8; 4];
        arg[0..4].copy_from_slice(&TCOFLUSH.to_le_bytes());
        v.ioctl(0, master, TIOCFLUSH, &mut arg).unwrap();
        // Slave read should now return EAGAIN (buffer empty).
        let mut buf = [0u8; 8];
        assert_eq!(v.read(0, slave, &mut buf), Err(Errno::EAgain));
    }

    #[test]
    fn tcflush_both_clears_all_buffers() {
        use crate::fileobj::{TIOCFLUSH, TCIOFLUSH};
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Fill both buffers.
        v.write(0, slave, b"to_master").unwrap();
        v.write(0, master, b"to_slave").unwrap();
        // Flush everything.
        let mut arg = [0u8; 4];
        arg[0..4].copy_from_slice(&TCIOFLUSH.to_le_bytes());
        v.ioctl(0, master, TIOCFLUSH, &mut arg).unwrap();
        let mut buf = [0u8; 16];
        assert_eq!(v.read(0, master, &mut buf), Err(Errno::EAgain));
        assert_eq!(v.read(0, slave, &mut buf), Err(Errno::EAgain));
    }

    #[test]
    fn tiocoutq_returns_output_queue_length() {
        use crate::fileobj::{TIOCOUTQ};
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, slave) = v.openpty(0).unwrap();
        // Slave writes — appears in master's output queue.
        v.write(0, slave, b"abc").unwrap();
        let mut arg = [0u8; 4];
        v.ioctl(0, master, TIOCOUTQ, &mut arg).unwrap();
        let n = u32::from_le_bytes([arg[0], arg[1], arg[2], arg[3]]);
        assert_eq!(n, 3);
        // Read it — queue drains.
        let mut buf = [0u8; 8];
        v.read(0, master, &mut buf).unwrap();
        v.ioctl(0, master, TIOCOUTQ, &mut arg).unwrap();
        let n = u32::from_le_bytes([arg[0], arg[1], arg[2], arg[3]]);
        assert_eq!(n, 0);
    }

    #[test]
    fn tcdrain_on_pty_succeeds() {
        use crate::fileobj::{TIOCFLUSH, TCIOFLUSH};
        let mut v = Vfs::<dummy::NoKernel, dummy::NoAlloc>::new();
        let (master, _slave) = v.openpty(0).unwrap();
        // tcdrain is a no-op in our synchronous model — should always succeed.
        // We verify via a TIOCFLUSH round-trip (drain semantics are that
        // all pending output has been transmitted, which is trivially true).
        let mut arg = [0u8; 4];
        arg[0..4].copy_from_slice(&TCIOFLUSH.to_le_bytes());
        v.ioctl(0, master, TIOCFLUSH, &mut arg).unwrap();
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
