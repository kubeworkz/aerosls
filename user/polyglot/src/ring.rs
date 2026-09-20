//! `ring` — the Polyglot Nexus shared-memory channel: a single-producer /
//! single-consumer ring buffer per direction, mmap'd from a file both
//! sidecars map (the channel file, created by `sls-kerneld`).
//!
//! This replaces the kernel in the entire sidecar-to-kernel data path:
//! a wasm→lisp call is a frame pushed into ring0 (wasm produces, Lisp
//! consumes) and the reply is a frame pushed into ring1 (Lisp produces,
//! wasm consumes). The arena bytes and the cap descriptors inside the
//! frames travel by reference (arena offset + len), never copied — the
//! zero-copy gain the bench legs measure. The arena bookkeeping (alloc /
//! free, syscalls 290/304) also lives on rings: each sidecar gets its own
//! request/reply pair against `sls-kerneld` (ring2 wasm→kerneld, ring3
//! kerneld→wasm, ring4 lisp→kerneld, ring5 kerneld→lisp) so the kernel's
//! bump cursor + refcounts are reached through shared memory, not TCP.
//! SPSC is preserved per ring: each ring has exactly one producer and one
//! consumer, so the release/acquire cursor protocol never contends.
//!
//! Synchronization is the textbook SPSC protocol with release/acquire
//! cursors (plain movs on x86, so the SBCL side can speak the same wire
//! with raw 64-bit loads/stores under TSO):
//!
//!   producer: write bytes at write % cap, then publish write (Release)
//!   consumer: read write (Acquire) until write != read, read bytes, then
//!             publish read (Release)
//!
//! Frame format in the ring: `[body_len u32][body...]` — the body is the
//! exact 302/303 wire body the TCP transport used, so the generated client
//! stubs and the Lisp dispatch parse ring frames and TCP frames
//! identically. Only the transport seam (the fake syscall / chan-send)
//! switches.

use std::ffi::CString;
use std::fs::OpenOptions;
use std::io;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicU64, Ordering};

/// Channel file layout (created by sls-kerneld; both sidecars map it).
pub const RING_MAGIC: u32 = 0x534C_5352; // "SLSR"
pub const RING_VERSION: u32 = 1;
pub const RING_HEADER_LEN: usize = 32;
/// Data region size per direction.
pub const RING_CAPACITY: usize = 4 * 1024 * 1024;
/// ring0: wasm→lisp requests. ring1: lisp→wasm replies.
/// ring2: wasm→kerneld arena requests. ring3: kerneld→wasm arena replies.
/// ring4: lisp→kerneld arena requests. ring5: kerneld→lisp arena replies.
/// ring6: wasm→lisp async requests (heavy_reduce). ring7: lisp→wasm async
/// results (the dedicated result channel — T13/T14 in the design matrix).
pub const RING0_OFFSET: usize = 0;
pub const RING1_OFFSET: usize = RING_HEADER_LEN + RING_CAPACITY;
pub const RING2_OFFSET: usize = 2 * (RING_HEADER_LEN + RING_CAPACITY);
pub const RING3_OFFSET: usize = 3 * (RING_HEADER_LEN + RING_CAPACITY);
pub const RING4_OFFSET: usize = 4 * (RING_HEADER_LEN + RING_CAPACITY);
pub const RING5_OFFSET: usize = 5 * (RING_HEADER_LEN + RING_CAPACITY);
pub const RING6_OFFSET: usize = 6 * (RING_HEADER_LEN + RING_CAPACITY);
pub const RING7_OFFSET: usize = 7 * (RING_HEADER_LEN + RING_CAPACITY);
pub const CHAN_FILE_SIZE: usize = RING_HEADER_LEN * 8 + RING_CAPACITY * 8;

/// Header layout: magic u32 @0, version u32 @4, capacity u32 @8, pad u32
/// @12, write u64 @16, read u64 @24. 32 bytes, page-aligned by mmap.
const CURSOR_WRITE: usize = 16;
const CURSOR_READ: usize = 24;

pub struct Ring {
    /// Base of this ring's header within the mapping.
    base: *mut u8,
    /// `data` region (header + capacity).
    data: *mut u8,
    capacity: usize,
}

// The mapping is shared mutable state, accessed only through atomics /
// memcpy of the caller's own bytes — Send/Sync are what the sidecars
// actually do (each sidecar uses one Ring per direction from a single
// thread, so no races beyond the protocol).
unsafe impl Send for Ring {}
unsafe impl Sync for Ring {}

fn open_file(path: &str) -> io::Result<std::fs::File> {
    OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .map_err(|e| io::Error::new(e.kind(), format!("open channel file {path}: {e}")))
}

/// Map the whole channel file (both rings) and return the base pointer.
fn map_channel(path: &str) -> io::Result<(*mut u8, usize)> {
    let file = open_file(path)?;
    map_whole_file(&file, path)
}

/// Map an already-open file (the creation path has no name to open yet).
fn map_whole_file(file: &std::fs::File, path: &str) -> io::Result<(*mut u8, usize)> {
    let len = file
        .metadata()
        .map_err(|e| io::Error::new(e.kind(), format!("stat channel file {path}: {e}")))?
        .len() as usize;
    if len < CHAN_FILE_SIZE {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("channel file {path} is {len} bytes, need {CHAN_FILE_SIZE}"),
        ));
    }
    let base = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            file.as_raw_fd(),
            0,
        )
    };
    if base == libc::MAP_FAILED {
        return Err(io::Error::last_os_error());
    }
    Ok((base as *mut u8, len))
}

/// A shared-memory file that exists but has not been given its name yet.
///
/// The structural half of the shm leak fix. `open_unnamed` creates the inode
/// anonymously (`O_TMPFILE`), so the target path does not exist while the file
/// is being sized, mapped and initialized — the expensive part, and the part a
/// signal can interrupt. Nothing is left behind by a signal arriving during
/// creation, nor by a SIGKILL, which no handler can catch. `publish` links the
/// finished file into place, so the name also never refers to a
/// half-initialized channel (a reader that opened it early would fail its
/// header check).
pub struct PendingShm {
    file: std::fs::File,
    target: String,
    /// True until `publish` gives the file its name. False from the start on
    /// the fallback path, where the file was created named.
    unnamed: bool,
}

impl PendingShm {
    /// The unnamed file, for `ftruncate` / `mmap` / header initialization.
    pub fn file(&self) -> &std::fs::File {
        &self.file
    }

    /// Give the file its name. A second call is a no-op. On the fallback path
    /// (a filesystem without `O_TMPFILE`) there is nothing to publish.
    pub fn publish(&mut self) -> io::Result<()> {
        if !self.unnamed {
            return Ok(());
        }
        // `/proc/self/fd/N` + AT_SYMLINK_FOLLOW is the documented way to name
        // an O_TMPFILE inode: the magic link resolves to the unnamed dentry.
        let link = format!("/proc/self/fd/{}", self.file.as_raw_fd());
        let c_link = CString::new(link.clone())
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, format!("{link}: {e}")))?;
        let c_target = CString::new(self.target.clone()).map_err(|e| {
            io::Error::new(io::ErrorKind::InvalidInput, format!("{}: {e}", self.target))
        })?;
        // SAFETY: both pointers are NUL-terminated for the duration of the
        // call and linkat only reads them.
        let rc = unsafe {
            libc::linkat(
                libc::AT_FDCWD,
                c_link.as_ptr(),
                libc::AT_FDCWD,
                c_target.as_ptr(),
                libc::AT_SYMLINK_FOLLOW,
            )
        };
        if rc != 0 {
            let e = io::Error::last_os_error();
            return Err(io::Error::new(
                e.kind(),
                format!("link {link} -> {}: {e}", self.target),
            ));
        }
        self.unnamed = false;
        Ok(())
    }
}

/// Open the shared-memory file for `path` without giving it a name yet.
///
/// `O_TMPFILE` is tried first, in the target's own directory so the late link
/// stays on one filesystem. A filesystem that does not support anonymous
/// files (or a target that cannot be replaced) falls back to the previous
/// create+truncate, where `publish` is a no-op and the caller's own cleanup
/// covers the named window.
pub fn open_unnamed(path: &str) -> io::Result<PendingShm> {
    use std::os::unix::fs::OpenOptionsExt;
    let target = std::path::Path::new(path);
    let dir = match target.parent() {
        Some(p) if !p.as_os_str().is_empty() => p,
        _ => std::path::Path::new("."),
    };
    match OpenOptions::new()
        .read(true)
        .write(true)
        .mode(0o600)
        .custom_flags(libc::O_TMPFILE | libc::O_CLOEXEC)
        .open(dir)
    {
        Ok(file) => {
            // `linkat` refuses an existing target, and the previous
            // incarnation reused such a file by truncating it. A file at this
            // path is dead by construction (the port is unique per run), so
            // drop it and keep the anonymous path; if it cannot be removed,
            // fall through to the named create rather than guess.
            match std::fs::remove_file(target) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::NotFound => {}
                Err(_) => return open_named(path),
            }
            Ok(PendingShm {
                file,
                target: path.to_string(),
                unnamed: true,
            })
        }
        Err(_) => open_named(path),
    }
}

/// Create the file by name (the pre-anonymous behavior). Split out so the
/// fallback is visible where it is taken.
fn open_named(path: &str) -> io::Result<PendingShm> {
    use std::os::unix::fs::OpenOptionsExt;
    let file = OpenOptions::new()
        .create(true)
        .truncate(true)
        .read(true)
        .write(true)
        .custom_flags(libc::O_CLOEXEC)
        .open(path)
        .map_err(|e| io::Error::new(e.kind(), format!("create channel file {path}: {e}")))?;
    Ok(PendingShm {
        file,
        target: path.to_string(),
        unnamed: false,
    })
}

/// Create + init the channel file with both rings' headers.
pub fn create_channel(path: &str) -> io::Result<()> {
    let mut pending = open_unnamed(path)?;
    pending
        .file()
        .set_len(CHAN_FILE_SIZE as u64)
        .map_err(|e| io::Error::new(e.kind(), format!("ftruncate channel file {path}: {e}")))?;
    pending.file().sync_all().ok();
    let (base, len) = map_whole_file(pending.file(), path)?;
    unsafe { std::slice::from_raw_parts_mut(base, len) }.fill(0);
    for offset in [
        RING0_OFFSET,
        RING1_OFFSET,
        RING2_OFFSET,
        RING3_OFFSET,
        RING4_OFFSET,
        RING5_OFFSET,
        RING6_OFFSET,
        RING7_OFFSET,
    ] {
        unsafe {
            let b = base.add(offset);
            std::ptr::write_unaligned(b as *mut u32, RING_MAGIC);
            std::ptr::write_unaligned(b.add(4) as *mut u32, RING_VERSION);
            std::ptr::write_unaligned(b.add(8) as *mut u32, RING_CAPACITY as u32);
        }
    }
    unsafe { libc::munmap(base as *mut libc::c_void, len) };
    // Publish last: the name appears only once every header is written, so a
    // reader (or a crash) can never see a half-initialized channel.
    pending.publish()
}

impl Ring {
    /// Open one direction of an existing channel file and verify its header.
    pub fn open(path: &str, offset: usize) -> io::Result<Ring> {
        let (base, _len) = map_channel(path)?;
        let hdr = unsafe { base.add(offset) };
        let magic = unsafe { std::ptr::read_unaligned(hdr as *const u32) };
        let version = unsafe { std::ptr::read_unaligned(hdr.add(4) as *const u32) };
        let capacity = unsafe { std::ptr::read_unaligned(hdr.add(8) as *const u32) } as usize;
        if magic != RING_MAGIC || version != RING_VERSION || capacity != RING_CAPACITY {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("channel {path}@{offset}: bad header magic={magic:#x} v={version} cap={capacity}"),
            ));
        }
        Ok(Ring {
            base: hdr,
            data: unsafe { hdr.add(RING_HEADER_LEN) },
            capacity,
        })
    }

    /// Producer: push one frame `[len u32][body]`, waiting for space.
    pub fn send(&self, body: &[u8]) -> io::Result<()> {
        let total = 4usize
            .checked_add(body.len())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "frame too large"))?;
        if total > self.capacity {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("frame {total} B exceeds ring capacity {}", self.capacity),
            ));
        }
        let write_ptr = unsafe { &*(self.base.add(CURSOR_WRITE) as *const AtomicU64) };
        let read_ptr = unsafe { &*(self.base.add(CURSOR_READ) as *const AtomicU64) };
        // Wait for the consumer to free `total` bytes.
        let write = loop {
            let w = write_ptr.load(Ordering::Acquire);
            let r = read_ptr.load(Ordering::Acquire);
            if w >= r && w - r <= (self.capacity - total) as u64 {
                break w;
            }
            std::hint::spin_loop();
            std::thread::yield_now();
        };
        // Write [len u32] then body, both wrap-aware; publish after.
        let len_field = (body.len() as u32).to_le_bytes();
        self.copy_in(write, &len_field[..]);
        self.copy_in(write + 4, body);
        write_ptr.store(write + total as u64, Ordering::Release);
        Ok(())
    }

    /// Consumer: block until a frame is available, return its body.
    pub fn recv(&self) -> io::Result<Vec<u8>> {
        let write_ptr = unsafe { &*(self.base.add(CURSOR_WRITE) as *const AtomicU64) };
        let read_ptr = unsafe { &*(self.base.add(CURSOR_READ) as *const AtomicU64) };
        let read = loop {
            let r = read_ptr.load(Ordering::Acquire);
            let w = write_ptr.load(Ordering::Acquire);
            if w != r {
                break r;
            }
            std::hint::spin_loop();
            std::thread::yield_now();
        };
        let len = self.copy_len(read) as usize;
        if len > self.capacity - 4 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("ring frame len {len} exceeds capacity"),
            ));
        }
        let mut body = vec![0u8; len];
        self.copy_out(read + 4, &mut body);
        read_ptr.store(read + 4 + len as u64, Ordering::Release);
        Ok(body)
    }

    /// Read the 4-byte LE length field at `pos` (wrap-aware).
    fn copy_len(&self, pos: u64) -> u32 {
        let mut field = [0u8; 4];
        self.copy_out(pos, &mut field[..]);
        u32::from_le_bytes(field)
    }

    /// Copy `buf` into the ring at `pos` mod capacity, wrapping once.
    fn copy_in(&self, pos: u64, buf: &[u8]) {
        let off = (pos as usize) % self.capacity;
        let first = (self.capacity - off).min(buf.len());
        unsafe {
            std::ptr::copy_nonoverlapping(buf.as_ptr(), self.data.add(off), first);
            if first < buf.len() {
                std::ptr::copy_nonoverlapping(buf.as_ptr().add(first), self.data, buf.len() - first);
            }
        }
    }

    /// Copy `n` bytes from the ring at `pos` mod capacity into `buf`.
    fn copy_out(&self, pos: u64, buf: &mut [u8]) {
        let off = (pos as usize) % self.capacity;
        let first = (self.capacity - off).min(buf.len());
        unsafe {
            std::ptr::copy_nonoverlapping(self.data.add(off), buf.as_mut_ptr(), first);
            if first < buf.len() {
                std::ptr::copy_nonoverlapping(self.data, buf.as_mut_ptr().add(first), buf.len() - first);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(tag: &str) -> String {
        std::env::temp_dir()
            .join(format!("sls-ring-test-{tag}-{}.bin", std::process::id()))
            .to_string_lossy()
            .to_string()
    }

    /// The leak fix in one assertion: while the file is unnamed the path does
    /// not exist, so a signal during creation — or a SIGKILL, which no handler
    /// can catch — has nothing to leave behind. `publish` is what makes it
    /// appear, and a second call must be a no-op rather than an EEXIST.
    #[test]
    fn unnamed_until_published() {
        let path = scratch("a");
        let _ = std::fs::remove_file(&path);
        let mut pending = open_unnamed(&path).expect("open unnamed");
        assert!(
            !std::path::Path::new(&path).exists(),
            "the path must not exist while the file is unnamed"
        );
        pending.file().set_len(4096).expect("ftruncate");
        pending.publish().expect("publish");
        let md = std::fs::metadata(&path).expect("published file must exist");
        assert_eq!(md.len(), 4096, "the linked file is the one we sized");
        pending.publish().expect("publish must be idempotent");
        std::fs::remove_file(&path).expect("cleanup");
    }

    /// Negative control for the test above: the *named* path is visible the
    /// moment it is created, so `unnamed_until_published` is asserting a real
    /// difference and would fail if creation ever went back to create+truncate.
    #[test]
    fn named_creation_is_visible_immediately() {
        let path = scratch("b");
        let _ = std::fs::remove_file(&path);
        let named = open_named(&path).expect("open named");
        assert!(
            std::path::Path::new(&path).exists(),
            "the fallback names the file up front (which is why it needs cleanup)"
        );
        drop(named);
        std::fs::remove_file(&path).expect("cleanup");
    }

    /// `linkat` refuses an existing target, and a previous incarnation reused
    /// such a file by truncating it. A stale file must therefore be replaced
    /// rather than turning the anonymous path into an error.
    #[test]
    fn stale_file_is_replaced() {
        let path = scratch("c");
        std::fs::write(&path, b"stale").expect("write stale file");
        let mut pending = open_unnamed(&path).expect("open unnamed over a stale file");
        assert!(
            !std::path::Path::new(&path).exists(),
            "the stale file is gone, and the new one is not published yet"
        );
        pending.file().set_len(128).expect("ftruncate");
        pending.publish().expect("publish over the stale name");
        assert_eq!(
            std::fs::metadata(&path).expect("stat").len(),
            128,
            "the published file replaced the stale one"
        );
        std::fs::remove_file(&path).expect("cleanup");
    }

    /// The published channel must be a valid channel: `create_channel` sizes,
    /// zeroes and header-stamps the file while it is still unnamed, so this is
    /// also the check that the late link does not disturb any of that.
    #[test]
    fn published_channel_opens_on_every_ring() {
        let path = scratch("d");
        let _ = std::fs::remove_file(&path);
        create_channel(&path).expect("create channel");
        assert!(std::path::Path::new(&path).exists(), "create must publish");
        assert_eq!(
            std::fs::metadata(&path).expect("stat").len() as usize,
            CHAN_FILE_SIZE,
            "the channel file is fully sized"
        );
        for offset in [
            RING0_OFFSET,
            RING1_OFFSET,
            RING2_OFFSET,
            RING3_OFFSET,
            RING4_OFFSET,
            RING5_OFFSET,
            RING6_OFFSET,
            RING7_OFFSET,
        ] {
            Ring::open(&path, offset).unwrap_or_else(|e| panic!("ring @{offset}: {e}"));
        }
        std::fs::remove_file(&path).expect("cleanup");
    }

    /// An error path must not create a name either: a target in a directory
    /// that does not exist fails, and leaves nothing behind.
    #[test]
    fn missing_directory_creates_nothing() {
        let path = format!(
            "/tmp/sls-ring-test-missing-{}/arena.bin",
            std::process::id()
        );
        assert!(open_unnamed(&path).is_err(), "no such directory must error");
        assert!(!std::path::Path::new(&path).exists(), "and create nothing");
    }
}
