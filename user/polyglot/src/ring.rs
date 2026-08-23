//! `ring` — the Polyglot Nexus shared-memory channel: a single-producer /
//! single-consumer ring buffer per direction, mmap'd from a file both
//! sidecars map (the channel file, created by `sls-kerneld`).
//!
//! This replaces the kernel in the message data path: a wasm→lisp call is
//! now a frame pushed into ring0 (wasm produces, Lisp consumes) and the
//! reply is a frame pushed into ring1 (Lisp produces, wasm consumes). The
//! arena bytes and the cap descriptors inside the frames travel by
//! reference (arena offset + len), never copied — the zero-copy gain the
//! bench legs measure. `sls-kerneld` still owns the arena bookkeeping
//! (alloc/free, syscalls 290/304) over TCP; only the message queues moved
//! into shared memory.
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
pub const RING0_OFFSET: usize = 0;
pub const RING1_OFFSET: usize = RING_HEADER_LEN + RING_CAPACITY;
pub const CHAN_FILE_SIZE: usize = RING_HEADER_LEN * 2 + RING_CAPACITY * 2;

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

/// Create (or truncate + init) the channel file with both rings' headers.
pub fn create_channel(path: &str) -> io::Result<()> {
    {
        use std::os::unix::fs::OpenOptionsExt;
        let f = OpenOptions::new()
            .create(true)
            .truncate(true)
            .read(true)
            .write(true)
            .custom_flags(libc::O_CLOEXEC)
            .open(path)
            .map_err(|e| io::Error::new(e.kind(), format!("create channel file {path}: {e}")))?;
        f.set_len(CHAN_FILE_SIZE as u64)
            .map_err(|e| io::Error::new(e.kind(), format!("ftruncate channel file {path}: {e}")))?;
        f.sync_all().ok();
    }
    let (base, len) = map_channel(path)?;
    unsafe { std::slice::from_raw_parts_mut(base, len) }.fill(0);
    for offset in [RING0_OFFSET, RING1_OFFSET] {
        unsafe {
            let b = base.add(offset);
            std::ptr::write_unaligned(b as *mut u32, RING_MAGIC);
            std::ptr::write_unaligned(b.add(4) as *mut u32, RING_VERSION);
            std::ptr::write_unaligned(b.add(8) as *mut u32, RING_CAPACITY as u32);
        }
    }
    unsafe { libc::munmap(base as *mut libc::c_void, len) };
    Ok(())
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
