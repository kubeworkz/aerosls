//! File-like objects: everything an fd can name that is *not* a file on a
//! mounted fs. The VFS's fd layer dispatches I/O on the object kind
//! (`FileObj`), so pipes and character devices live in core memory with
//! their own semantics instead of being forced through the mount table.
//!
//! - **`PipeNode`** — the shell-pipeline primitive (the `pipe()` syscall).
//!   A bounded buffer shared by read and write ends (`Arc`). End counts are
//!   tracked by the VFS as fds come and go, so reads see **EOF** once the
//!   last write end is gone, writes fail **`EPIPE`** once the last read end
//!   is gone, and empty/full reads/writes fail **`EAGAIN`** (the
//!   cooperative scheduler's "would block" — the shell polls or waits).
//! - **`CharNode`** — a character device (`/dev/console`, `/dev/null`).
//!   The console is a channel device: in the full sidecar its I/O goes over
//!   a channel to the kernel console driver; here it is an in-memory
//!   console (the sim harness the kernel channel plugs into later). I/O on
//!   a device node dispatches to the object, bypassing the fs layer.
//!
//! The enum keeps the core free of trait objects, same principle as `Fs`:
//! the concrete set of object kinds is closed, and every arm is explicit.

use alloc::collections::VecDeque;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell};

use crate::aerofs::{FileType, S_IFCHR, S_IFIFO};
use crate::errno::{Errno, Stat};

/// Pipe buffer capacity (bytes). The MVP's fixed size; the design's shell
/// pipeline walkthrough assumes small streams (a cat of a few lines).
pub const PIPE_CAP: usize = 4096;

// ── the file-like object ────────────────────────────────────────────────────

/// Everything an fd can name. `File(FileNode)` is the existing mount-table
/// path (fs + inode + shared offset); the other variants are in-core
/// objects whose I/O never touches a mount. `FdEntry` holds `Arc<FileObj>`
/// so `dup`/`fork` share the object (and, for pipes, the *ends*).
pub enum FileObj {
    /// A regular file or directory on a mounted fs (see `FileNode`).
    File(crate::vfs::FileNode),
    /// The read end of a pipe. Shares the `PipeNode` with its write end.
    PipeRead(Arc<PipeNode>),
    /// The write end of a pipe.
    PipeWrite(Arc<PipeNode>),
    /// A character device (`/dev/console`, `/dev/null`).
    Char(Arc<CharNode>),
}

// ── pipes ───────────────────────────────────────────────────────────────────

/// A pipe: the shared state behind a read/write end pair.
///
/// The VFS is the *only* place entries are created or destroyed (open,
/// close, dup, dup2, fork-copy, exit), and it bumps `readers`/`writers`
/// there — so EOF and EPIPE are exact even though an end may be dup'd many
/// times and fds may outlive their task (exit abandons a table).
pub struct PipeNode {
    buf: RefCell<VecDeque<u8>>,
    cap: usize,
    /// Live read ends (fds holding `FileObj::PipeRead` of this node).
    readers: Cell<u32>,
    /// Live write ends (fds holding `FileObj::PipeWrite` of this node).
    writers: Cell<u32>,
}

impl PipeNode {
    pub fn new(cap: usize) -> PipeNode {
        PipeNode {
            buf: RefCell::new(VecDeque::new()),
            cap,
            readers: Cell::new(0),
            writers: Cell::new(0),
        }
    }

    pub fn readers(&self) -> u32 {
        self.readers.get()
    }
    pub fn writers(&self) -> u32 {
        self.writers.get()
    }
    /// Buffered data exists (a read will return at least one byte).
    pub fn has_data(&self) -> bool {
        !self.buf.borrow().is_empty()
    }
    /// Free buffer space (a write of up to this many bytes succeeds).
    pub fn space(&self) -> usize {
        self.cap - self.buf.borrow().len()
    }

    /// Called by the VFS when an fd holding this end is created.
    pub fn bump_readers(&self, d: i32) {
        self.readers.set((self.readers.get() as i32 + d) as u32);
    }
    /// Called by the VFS when an fd holding this end is destroyed.
    pub fn bump_writers(&self, d: i32) {
        self.writers.set((self.writers.get() as i32 + d) as u32);
    }

    /// Read up to `buf.len()` bytes. `Ok(0)` is EOF: the buffer is empty
    /// and no write end remains. `EAGAIN` is "would block": empty but
    /// writers may still produce data.
    pub fn read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        if self.writers.get() == 0 && self.buf.borrow().is_empty() {
            return Ok(0); // EOF
        }
        let n = core::cmp::min(buf.len(), self.buf.borrow().len());
        if n == 0 {
            return Err(Errno::EAgain);
        }
        for b in buf[..n].iter_mut() {
            *b = self.buf.borrow_mut().pop_front().unwrap();
        }
        Ok(n)
    }

    /// Write up to `buf.len()` bytes (short writes allowed, POSIX). `EPIPE`
    /// when no read end remains; `EAGAIN` when the buffer is full.
    pub fn write(&self, buf: &[u8]) -> Result<usize, Errno> {
        if self.readers.get() == 0 {
            return Err(Errno::EPipe);
        }
        let space = self.cap - self.buf.borrow().len();
        if space == 0 {
            return Err(Errno::EAgain);
        }
        let n = core::cmp::min(buf.len(), space);
        self.buf.borrow_mut().extend(&buf[..n]);
        Ok(n)
    }

    pub fn stat(&self) -> Stat {
        Stat {
            mode: S_IFIFO | 0o600,
            uid: 0,
            gid: 0,
            size: self.buf.borrow().len() as u64,
            mtime: 0,
            ty: FileType::Fifo,
        }
    }
}

// ── character devices ───────────────────────────────────────────────────────

/// A character device node: `/dev/console` and `/dev/null`. Devices are
/// named by the `/dev` mount but their I/O bypasses the fs layer — an fd
/// opened on a device holds `FileObj::Char`, and read/write dispatch
/// straight to the node. (A device is not seekable: `lseek` → `ESPIPE`.)
pub struct CharNode {
    kind: CharKind,
    mode: u16,
    uid: u16,
    gid: u16,
}

enum CharKind {
    /// The console — a channel device (see `ConsoleIo`).
    Console(ConsoleIo),
    /// `/dev/null`: writes are discarded, reads are EOF.
    Null,
}

impl CharNode {
    /// The sidecar console, root-owned (`/dev/console`).
    pub fn console() -> CharNode {
        CharNode {
            kind: CharKind::Console(ConsoleIo::new()),
            mode: S_IFCHR | 0o600,
            uid: 0,
            gid: 0,
        }
    }

    /// `/dev/null`, world-writable.
    pub fn null() -> CharNode {
        CharNode {
            kind: CharKind::Null,
            mode: S_IFCHR | 0o666,
            uid: 0,
            gid: 0,
        }
    }

    pub fn mode(&self) -> u16 {
        self.mode
    }
    pub fn uid(&self) -> u16 {
        self.uid
    }
    pub fn gid(&self) -> u16 {
        self.gid
    }

    /// The readiness predicate for a blocked *reader*: a console read will
    /// return (input arrived, or the channel closed → EOF). Null is always
    /// ready — its reads never block (instant EOF).
    pub fn read_ready(&self) -> bool {
        match &self.kind {
            CharKind::Console(c) => !c.input_empty() || c.is_closed(),
            CharKind::Null => true,
        }
    }

    pub fn read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        match &self.kind {
            CharKind::Console(c) => c.read(buf),
            CharKind::Null => Ok(0), // EOF
        }
    }

    pub fn write(&self, buf: &[u8]) -> Result<usize, Errno> {
        match &self.kind {
            CharKind::Console(c) => c.write(buf),
            CharKind::Null => Ok(buf.len()), // discard
        }
    }

    pub fn stat(&self) -> Stat {
        Stat {
            mode: self.mode,
            uid: self.uid,
            gid: self.gid,
            size: 0,
            mtime: 0,
            ty: FileType::Char,
        }
    }

    /// Downcast to the console I/O (the sim harness reads what was typed /
    /// written). Panics on non-console nodes.
    pub fn console_io(&self) -> &ConsoleIo {
        match &self.kind {
            CharKind::Console(c) => c,
            CharKind::Null => panic!("not a console"),
        }
    }
}    /// The console's I/O. In the full sidecar this is the channel to the
    /// kernel's console driver (`ChanDev`); here it is an in-memory terminal:
    /// reads consume `input`, writes append to `output`. The bootstrap wires
    /// the real channel into this slot without the VFS changing.
    ///
    /// `close` is the **channel-close event**: the kernel console's channel
    /// went away. A closed console is EOF — reads return `Ok(0)` once the
    /// already-buffered input is drained, and parked readers are woken by
    /// the scheduler's drain (the readiness predicate includes the flag).
    pub struct ConsoleIo {
        input: RefCell<VecDeque<u8>>,
        output: RefCell<Vec<u8>>,
        closed: Cell<bool>,
    }

    impl ConsoleIo {
        pub fn new() -> ConsoleIo {
            ConsoleIo {
                input: RefCell::new(VecDeque::new()),
                output: RefCell::new(Vec::new()),
                closed: Cell::new(false),
            }
        }

        /// The console-close event: the kernel-console channel closed (the
        /// driver is gone or the channel was torn down). Delivered the way
        /// input is — from outside the scheduler, seen by the next wake
        /// drain. Buffered input already delivered over the channel stays
        /// readable; reads return `Ok(0)` (EOF) once it is drained.
        pub fn close(&self) {
            self.closed.set(true);
        }

        pub fn is_closed(&self) -> bool {
            self.closed.get()
        }

    /// Feed input as if typed at the console (the driver would push here).
    /// The sidecar core runs the scheduler after external input arrives,
    /// and its read-wake drain re-arms tasks blocked on the console.
    pub fn push_input(&self, bytes: &[u8]) {
        self.input.borrow_mut().extend(bytes);
    }

    pub(crate) fn input_empty(&self) -> bool {
        self.input.borrow().is_empty()
    }

    /// Everything written to the console so far (the driver would drain
    /// here; the sim harness reads it back).
    pub fn output(&self) -> Vec<u8> {
        self.output.borrow().clone()
    }

    fn read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        let n = core::cmp::min(buf.len(), self.input.borrow().len());
        if n > 0 {
            // Buffered input first — data already delivered over the
            // channel is consumed even after a close.
            for b in buf[..n].iter_mut() {
                *b = self.input.borrow_mut().pop_front().unwrap();
            }
            return Ok(n);
        }
        if self.closed.get() {
            return Ok(0); // channel closed — EOF
        }
        Err(Errno::EAgain) // nothing typed — "would block"
    }

    fn write(&self, buf: &[u8]) -> Result<usize, Errno> {
        if self.closed.get() {
            return Err(Errno::EIo); // the channel is dead
        }
        self.output.borrow_mut().extend_from_slice(buf);
        Ok(buf.len())
    }
}

impl Default for ConsoleIo {
    fn default() -> Self {
        Self::new()
    }
}
