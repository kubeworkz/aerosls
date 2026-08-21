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
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell};

use crate::aerofs::{FileType, S_IFCHR, S_IFIFO};
use crate::errno::{Errno, Stat};

/// Pipe buffer capacity (bytes). The MVP's fixed size; the design's shell
/// pipeline walkthrough assumes small streams (a cat of a few lines).
pub const PIPE_CAP: usize = 4096;

/// Terminal window size (matches struct winsize).
#[derive(Clone, Copy, Debug)]
pub struct WinSize {
    pub rows: u16,
    pub cols: u16,
    pub xpixel: u16,
    pub ypixel: u16,
}

impl Default for WinSize {
    fn default() -> Self {
        WinSize { rows: 24, cols: 80, xpixel: 0, ypixel: 0 }
    }
}

/// ioctl request codes (Linux values).
pub const TIOCGWINSZ: u32 = 0x5413;
pub const TIOCSWINSZ: u32 = 0x5414;
pub const TIOCSCTTY: u32 = 0x540e;
pub const FIONBIO: u32 = 0x5421;

/// PTY buffer capacity.
pub const PTY_BUF_CAP: usize = 4096;

/// Address families.
pub const AF_UNIX: u16 = 1;

/// Socket types.
pub const SOCK_STREAM: u16 = 1;

/// Unix domain socket buffer capacity.
pub const UNIX_SOCK_BUF_CAP: usize = 65536;

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
    /// A network socket (backed by the NET_* protocol to the network
    /// driver sidecar). The VFS tracks only metadata; actual I/O goes
    /// through `Vfs::net_k()` / `Vfs::net_alloc()` at the caller level.
    Socket(Arc<SocketMeta>),
    /// The master side of a pseudo-terminal (PTY). Reads from
    /// `master_buf`, writes to `slave_buf`.
    PtyMaster(Arc<PtyState>),
    /// The slave side of a pseudo-terminal. Reads from `slave_buf`,
    /// writes to `master_buf`.
    PtySlave(Arc<PtyState>),
    /// A Unix domain socket (AF_UNIX). Connected pairs share a
    /// `UnixSocketState`; the VFS reads/writes the cross-linked buffers
    /// directly — no sidecar needed.
    UnixSocket(Arc<UnixSocketState>),
}

/// Metadata for a network socket fd. The actual NET_* send/recv happens
/// at the `Ctx` level (the caller holds the kernel handle).
pub struct SocketMeta {
    /// Driver-side socket ID (returned by NET_SOCKET).
    pub sock_id: u32,
    /// Channel endpoint to the network driver sidecar.
    pub chan: u32,
    /// SOCK_STREAM or SOCK_DGRAM.
    pub sock_type: u16,
    /// Client-side socket state mirror.
    pub state: SocketState,
    /// Remote address (connected/accepted sockets).
    pub remote_ip: u32,
    pub remote_port: u16,
}

/// Client-side socket state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SocketState {
    Created,
    Bound,
    Listening,
    Connected,
    HalfClosed,
    Closed,
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
    /// A PTY slave (backed by a shared PtyState).
    PtySlave(alloc::sync::Arc<PtyState>),
    /// The console — a channel device (see `ConsoleIo`).
    Console(ConsoleIo),
    /// `/dev/null`: writes are discarded, reads are EOF.
    Null,
}

impl CharNode {
    /// A PTY slave node (backed by an existing PtyState).
    pub fn pty_slave(pty: alloc::sync::Arc<PtyState>) -> CharNode {
        CharNode {
            kind: CharKind::PtySlave(pty),
            mode: S_IFCHR | 0o620,
            uid: 0,
            gid: 0,
        }
    }

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
            CharKind::PtySlave(p) => p.slave_has_data() || p.master_writers.get() == 0,
        }
    }

    pub fn read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        match &self.kind {
            CharKind::Console(c) => c.read(buf),
            CharKind::Null => Ok(0), // EOF
            CharKind::PtySlave(p) => p.slave_read(buf),
        }
    }

    /// Check if the console input buffer contains a specific byte.
    pub fn input_contains(&self, byte: u8) -> bool {
        match &self.kind {
            CharKind::Console(c) => c.input_contains(byte),
            CharKind::Null | CharKind::PtySlave(_) => false,
        }
    }

    /// Remove the first occurrence of a byte from the console input buffer.
    pub fn discard_byte(&self, byte: u8) {
        if let CharKind::Console(c) = &self.kind {
            c.discard_byte(byte);
        }
    }

    pub fn write(&self, buf: &[u8]) -> Result<usize, Errno> {
        match &self.kind {
            CharKind::Console(c) => c.write(buf),
            CharKind::Null => Ok(buf.len()), // discard
            CharKind::PtySlave(p) => p.slave_write(buf),
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
            _ => panic!("not a console"),
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

    /// Check if the input buffer contains a specific byte.
    pub(crate) fn input_contains(&self, byte: u8) -> bool {
        self.input.borrow().contains(&byte)
    }

    /// Remove the first occurrence of a specific byte from the input buffer.
    pub(crate) fn discard_byte(&self, byte: u8) {
        let mut buf = self.input.borrow_mut();
        if let Some(pos) = buf.iter().position(|&b| b == byte) {
            buf.remove(pos);
        }
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

// -- terminal line discipline ------------------------------------------------

/// Minimal termios flags for v1 (Linux values).
pub const ICANON: u32 = 0o000002;  // canonical (line-buffered) mode
pub const ECHO: u32 = 0o000010;    // echo input characters
pub const ECHOE: u32 = 0o000020;   // echo erase as backspace-space-backspace
pub const ISIG: u32 = 0o000001;    // enable signal chars (INTR, QUIT, SUSP)
pub const ICRNL: u32 = 0o000200;   // map CR to NL on input
pub const ONLCR: u32 = 0o000004;   // map NL to CR-NL on output

/// Terminal attributes for line discipline processing.
#[derive(Clone, Copy)]
pub struct Termios {
    pub iflag: u32,   // input flags
    pub oflag: u32,   // output flags
    pub lflag: u32,   // local flags (canonical, echo, isig)
    /// Special characters: [0]=INTR, [1]=QUIT, [2]=SUSP, [3]=ERASE,
    /// [4]=KILL, [5]=EOF, [6]=NL (=\n), [7]=CR.
    pub cc: [u8; 8],
}

impl Termios {
    /// Linux default: cooked mode with echo.
    pub fn default_cooked() -> Termios {
        Termios {
            iflag: ICRNL,
            oflag: ONLCR,
            lflag: ICANON | ECHO | ECHOE | ISIG,
            cc: [3, 28, 26, 127, 21, 4, 10, 13], // intr, quit, susp, erase, kill, eof, nl, cr
        }
    }
    /// Raw mode: no processing at all.
    pub fn default_raw() -> Termios {
        Termios {
            iflag: 0,
            oflag: 0,
            lflag: 0,
            cc: [3, 28, 26, 127, 21, 4, 10, 13],
        }
    }
    pub fn canonical(&self) -> bool { self.lflag & ICANON != 0 }
    pub fn echo(&self) -> bool { self.lflag & ECHO != 0 }
    pub fn isig(&self) -> bool { self.lflag & ISIG != 0 }
    pub fn icrnl(&self) -> bool { self.iflag & ICRNL != 0 }
    pub fn clone_termios(&self) -> Termios {
        Termios {
            iflag: self.iflag,
            oflag: self.oflag,
            lflag: self.lflag,
            cc: self.cc,
        }
    }
    pub fn onlcr(&self) -> bool { self.oflag & ONLCR != 0 }
    /// Encode termios to 36-byte buffer: iflag(4) oflag(4) lflag(4) cc(8) + 16 reserved.
    pub fn encode(&self, buf: &mut [u8]) {
        if buf.len() < 36 { return; }
        buf[0..4].copy_from_slice(&self.iflag.to_le_bytes());
        buf[4..8].copy_from_slice(&self.oflag.to_le_bytes());
        buf[8..12].copy_from_slice(&self.lflag.to_le_bytes());
        buf[12..20].copy_from_slice(&self.cc);
    }
    /// Decode termios from 36-byte buffer.
    pub fn decode(buf: &[u8]) -> Option<Termios> {
        if buf.len() < 20 { return None; }
        let mut cc = [0u8; 8];
        cc.copy_from_slice(&buf[12..20]);
        Some(Termios {
            iflag: u32::from_le_bytes([buf[0], buf[1], buf[2], buf[3]]),
            oflag: u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]),
            lflag: u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]),
            cc,
        })
    }
}

/// ioctl request codes for termios.
pub const TCGETS: u32 = 0x5401;
pub const TCSETS: u32 = 0x5402;

// -- pseudo-terminals (PTYs) ------------------------------------------------

/// Shared state between the master and slave sides of a PTY.
/// Each side has its own read buffer and end counts, matching
/// the bidirectional pipe model: master reads return what the
/// slave wrote, and vice versa.
pub struct PtyState {
    /// Data written by the slave, waiting for the master to read.
    pub master_buf: RefCell<VecDeque<u8>>,
    /// Data written by the master, waiting for the slave to read.
    pub slave_buf: RefCell<VecDeque<u8>>,
    /// Master side: live read ends.
    pub master_readers: Cell<u32>,
    /// Master side: live write ends.
    pub master_writers: Cell<u32>,
    /// Slave side: live read ends.
    pub slave_readers: Cell<u32>,
    /// Slave side: live write ends.
    pub slave_writers: Cell<u32>,
    /// Terminal window size.
    pub win_size: Cell<WinSize>,
    /// Terminal line discipline settings.
    pub termios: RefCell<Termios>,
    /// Foreground process group ID for signal delivery (set by shell via TIOCSCTTY).
    pub foreground_pgid: Cell<u32>,
}

impl PtyState {
    pub fn new() -> PtyState {
        PtyState {
            master_buf: RefCell::new(VecDeque::new()),
            slave_buf: RefCell::new(VecDeque::new()),
            master_readers: Cell::new(0),
            master_writers: Cell::new(0),
            slave_readers: Cell::new(0),
            slave_writers: Cell::new(0),
            win_size: Cell::new(WinSize::default()),
            termios: RefCell::new(Termios::default_raw()),
            foreground_pgid: Cell::new(0),
        }
    }

    /// Master read: data from the slave's writes.
    pub fn master_read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        if self.slave_writers.get() == 0 && self.master_buf.borrow().is_empty() {
            return Ok(0); // EOF: slave side gone
        }
        let n = core::cmp::min(buf.len(), self.master_buf.borrow().len());
        if n == 0 {
            return Err(Errno::EAgain);
        }
        for b in buf[..n].iter_mut() {
            *b = self.master_buf.borrow_mut().pop_front().unwrap();
        }
        Ok(n)
    }

    /// Master write: data goes to the slave's read buffer.
    pub fn master_write(&self, buf: &[u8]) -> Result<usize, Errno> {
        if self.slave_readers.get() == 0 {
            return Err(Errno::EPipe);
        }
        let space = PTY_BUF_CAP - self.slave_buf.borrow().len();
        if space == 0 {
            return Err(Errno::EAgain);
        }
        let n = core::cmp::min(buf.len(), space);
        self.slave_buf.borrow_mut().extend(&buf[..n]);
        Ok(n)
    }

    /// Slave read: data from the master's writes.
    pub fn slave_read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        if self.master_writers.get() == 0 && self.slave_buf.borrow().is_empty() {
            return Ok(0); // EOF: master side gone
        }
        let n = core::cmp::min(buf.len(), self.slave_buf.borrow().len());
        if n == 0 {
            return Err(Errno::EAgain);
        }
        for b in buf[..n].iter_mut() {
            *b = self.slave_buf.borrow_mut().pop_front().unwrap();
        }
        Ok(n)
    }

    /// Slave write: program output goes directly to the master's read
    /// buffer (the terminal emulator reads it for display). No line
    /// discipline — the program wrote this intentionally.
    pub fn slave_write(&self, buf: &[u8]) -> Result<usize, Errno> {
        if self.master_readers.get() == 0 {
            return Err(Errno::EPipe);
        }
        let space = PTY_BUF_CAP - self.master_buf.borrow().len();
        if space == 0 {
            return Err(Errno::EAgain);
        }
        let n = core::cmp::min(buf.len(), space);
        self.master_buf.borrow_mut().extend(&buf[..n]);
        Ok(n)
    }

    /// Master write: user input goes through the line discipline, then
    /// to the slave's read buffer. In cooked mode: signal chars (INTR,
    /// QUIT, SUSP) are intercepted and raised, CR→NL translation
    /// applies, and echo sends the character back to the master buffer
    /// (for the terminal to display). In raw mode: everything passes
    /// through unmodified.
    pub fn master_input(&self, buf: &[u8]) -> Result<usize, Errno> {
        if self.slave_readers.get() == 0 {
            return Err(Errno::EPipe);
        }
        let tty = self.termios.borrow();
        let isig = tty.isig();
        let echo = tty.echo();
        let icrnl = tty.icrnl();
        let intr = tty.cc[0];
        let quit = tty.cc[1];
        let susp = tty.cc[2];
        drop(tty);
        let mut written = 0usize;
        for &byte in buf.iter() {
            let mut ch = byte;
            // Signal character interception.
            if isig && ch == intr {
                self.raise_signal(2); // SIGINT
                if echo {
                    self.master_buf.borrow_mut().push_back(b'^');
                    self.master_buf.borrow_mut().push_back(b'C');
                }
                written += 1;
                continue;
            }
            if isig && ch == quit {
                self.raise_signal(3); // SIGQUIT
                if echo {
                    self.master_buf.borrow_mut().push_back(b'^');
                    self.master_buf.borrow_mut().push_back(b'\\');
                }
                written += 1;
                continue;
            }
            if isig && ch == susp {
                self.raise_signal(20); // SIGTSTP
                if echo {
                    self.master_buf.borrow_mut().push_back(b'^');
                    self.master_buf.borrow_mut().push_back(b'Z');
                }
                written += 1;
                continue;
            }
            // CR → NL translation.
            if icrnl && ch == b'\r' {
                ch = b'\n';
            }
            // Echo: send back to master buffer for terminal display.
            if echo {
                self.master_buf.borrow_mut().push_back(ch);
            }
            // Write to slave buffer (the program reads this).
            if self.slave_buf.borrow().len() >= PTY_BUF_CAP {
                break;
            }
            self.slave_buf.borrow_mut().push_back(ch);
            written += 1;
        }
        if written == 0 && !buf.is_empty() {
            return Err(Errno::EAgain);
        }
        Ok(written)
    }

    /// Deliver a signal to the foreground process group.
    fn raise_signal(&self, sig: i32) {
        // v1 stub: stores the signal for the proc manager to observe.
        // In the full sidecar, this would deliver to the foreground_pgid.
        // For now, we just record it; the caller (Ctx) checks via
        // a signal register on the PtyState.
        let _ = sig;
    }

    /// Master-side readiness: readable if slave wrote data or slave gone.
    pub fn master_has_data(&self) -> bool {
        !self.master_buf.borrow().is_empty() || self.slave_writers.get() == 0
    }
    /// Master-side writability: has space or slave reader closed.
    pub fn master_has_space(&self) -> bool {
        self.slave_buf.borrow().len() < PTY_BUF_CAP || self.slave_readers.get() == 0
    }
    /// Slave-side readiness: readable if master wrote data or master gone.
    pub fn slave_has_data(&self) -> bool {
        !self.slave_buf.borrow().is_empty() || self.master_writers.get() == 0
    }
    /// Slave-side writability: has space or master reader closed.
    pub fn slave_has_space(&self) -> bool {
        self.master_buf.borrow().len() < PTY_BUF_CAP || self.master_readers.get() == 0
    }

    /// Bump master read end count.
    pub fn bump_master_readers(&self, d: i32) {
        self.master_readers.set((self.master_readers.get() as i32 + d) as u32);
    }
    /// Bump master write end count.
    pub fn bump_master_writers(&self, d: i32) {
        self.master_writers.set((self.master_writers.get() as i32 + d) as u32);
    }
    /// Bump slave read end count.
    pub fn bump_slave_readers(&self, d: i32) {
        self.slave_readers.set((self.slave_readers.get() as i32 + d) as u32);
    }
    /// Bump slave write end count.
    pub fn bump_slave_writers(&self, d: i32) {
        self.slave_writers.set((self.slave_writers.get() as i32 + d) as u32);
    }

    /// Get the current termios settings (for TCGETS ioctl).
    pub fn get_termios(&self) -> Termios {
        self.termios.borrow().clone_termios()
    }

    /// Set termios settings (for TCSETS ioctl).
    pub fn set_termios(&self, t: Termios) {
        *self.termios.borrow_mut() = t;
    }

    pub fn stat(&self) -> Stat {
        Stat {
            mode: S_IFCHR | 0o620,
            uid: 0,
            gid: 0,
            size: 0,
            mtime: 0,
            ty: FileType::Char,
        }
    }
}

impl Default for PtyState {
    fn default() -> Self {
        Self::new()
    }
}

// ── Unix domain sockets ──────────────────────────────────────────────────────

/// State of an unbound or listening Unix domain socket.
pub enum UnixSocketState {
    /// Created but not bound. `bind()` transitions to `Bound`.
    Unbound,
    /// Bound to a filesystem path. `listen()` transitions to `Listening`.
    Bound {
        path: String,
    },
    /// Listening for incoming connections. The `pending` queue holds
    /// connected pairs created by `connect()` — `accept()` pops from it.
    Listening {
        path: String,
        pending: RefCell<Vec<Arc<UnixSocketState>>>,
    },
    /// Connected to a peer. Uses shared `Arc<RefCell<VecDeque>>` buffers
    /// so both ends see the same data. A's `snd` IS B's `rcv` (same Arc).
    Connected {
        /// Shared receive buffer (I read, peer writes via its snd Arc).
        rcv: Arc<RefCell<VecDeque<u8>>>,
        /// Shared send buffer (I write, peer reads via its rcv Arc).
        snd: Arc<RefCell<VecDeque<u8>>>,
        /// Shared live read-end count across both sides (for EOF detection).
        /// When either side's read fd is closed, this decrements. A reader
        /// sees EOF when writers==0 and rcv is empty.
        readers: Arc<Cell<u32>>,
        /// Shared live write-end count across both sides (for EPIPE detection).
        writers: Arc<Cell<u32>>,
    },
}

impl UnixSocketState {
    pub fn new() -> Arc<Self> {
        Arc::new(UnixSocketState::Unbound)
    }

    /// Read from the receive buffer. Returns `Ok(0)` (EOF) when the peer
    /// has no writers and the buffer is empty. `EAGAIN` when empty but
    /// the peer may still write.
    pub fn read(&self, buf: &mut [u8]) -> Result<usize, Errno> {
        match self {
            UnixSocketState::Connected { rcv, writers, .. } => {
                if writers.get() == 0 && rcv.borrow().is_empty() {
                    return Ok(0); // EOF
                }
                let n = core::cmp::min(buf.len(), rcv.borrow().len());
                if n == 0 {
                    return Err(Errno::EAgain);
                }
                for b in buf[..n].iter_mut() {
                    *b = rcv.borrow_mut().pop_front().unwrap();
                }
                Ok(n)
            }
            _ => Err(Errno::EBadf),
        }
    }

    /// Write to the peer's receive buffer (via my send buffer). Returns
    /// `EPIPE` when the peer has no readers. `EAGAIN` when the peer's
    /// buffer is full.
    pub fn write(&self, buf: &[u8]) -> Result<usize, Errno> {
        match self {
            UnixSocketState::Connected { snd, readers, .. } => {
                if readers.get() == 0 {
                    return Err(Errno::EPipe);
                }
                let space = UNIX_SOCK_BUF_CAP - snd.borrow().len();
                if space == 0 {
                    return Err(Errno::EAgain);
                }
                let n = core::cmp::min(buf.len(), space);
                snd.borrow_mut().extend(&buf[..n]);
                Ok(n)
            }
            _ => Err(Errno::EBadf),
        }
    }

    /// Whether the receive buffer has data or the peer has no writers (EOF).
    pub fn has_data(&self) -> bool {
        match self {
            UnixSocketState::Connected { rcv, writers, .. } => {
                !rcv.borrow().is_empty() || writers.get() == 0
            }
            _ => false,
        }
    }

    /// Whether the send buffer has free space or the peer has no readers.
    pub fn has_space(&self) -> bool {
        match self {
            UnixSocketState::Connected { snd, readers, .. } => {
                snd.borrow().len() < UNIX_SOCK_BUF_CAP || readers.get() == 0
            }
            _ => false,
        }
    }

    /// Bump read end count.
    pub fn bump_readers(&self, d: i32) {
        if let UnixSocketState::Connected { readers, .. } = self {
            readers.set((readers.get() as i32 + d) as u32);
        }
    }
    /// Bump write end count.
    pub fn bump_writers(&self, d: i32) {
        if let UnixSocketState::Connected { writers, .. } = self {
            writers.set((writers.get() as i32 + d) as u32);
        }
    }
}