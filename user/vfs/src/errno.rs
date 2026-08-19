//! POSIX errno codes, shared by the fs layers and the VFS syscall surface.
//!
//! The VFS is the *only* place that maps internal failures to errno; the fs
//! layers below return these directly. `EIO` is the one the respawn spec
//! (§6) cares about most: a stale device must fail `EIO`, never `ENOENT`
//! (as if the file never existed) and never silently succeed.

/// A POSIX errno. The numeric codes match Linux (asm-generic).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Errno {
    /// No such file or directory.
    ENoent,
    /// I/O error — the device is stale/dead, or a driver/transport failure.
    EIo,
    /// Permission denied.
    EAcces,
    /// Bad file descriptor.
    EBadf,
    /// Is a directory.
    EIsdir,
    /// Not a directory.
    ENotdir,
    /// File exists.
    EExist,
    /// Directory not empty.
    ENotempty,
    /// No space left on device.
    ENospc,
    /// Read-only filesystem.
    ERofs,
    /// Invalid argument.
    EInval,
    /// File name too long.
    ENametoolong,
    /// Out of memory.
    ENomem,
    /// Directory — the operation requires a regular file.
    EAgain,
    /// Device or resource busy.
    EBusy,
    /// Too many open files.
    EMfile,
    /// Exec format error (the file is not a runnable image).
    ENoexec,
    /// Illegal seek — the fd names a non-seekable object (pipe, device).
    ESPipe,
    /// Broken pipe — a write to a pipe whose read ends are all closed.
    EPipe,
}

impl Errno {
    pub const fn code(self) -> i32 {
        match self {
            Errno::ENoent => 2,
            Errno::EIo => 5,
            Errno::EAcces => 13,
            Errno::EBadf => 9,
            Errno::EIsdir => 21,
            Errno::ENotdir => 20,
            Errno::EExist => 17,
            Errno::ENotempty => 39,
            Errno::ENospc => 28,
            Errno::ERofs => 30,
            Errno::EInval => 22,
            Errno::ENametoolong => 36,
            Errno::ENomem => 12,
            Errno::EAgain => 11,
            Errno::EBusy => 16,
            Errno::EMfile => 24,
            Errno::ENoexec => 8,
            Errno::ESPipe => 29,
            Errno::EPipe => 32,
        }
    }
}

/// File metadata (struct stat, the fields aerofs-lite and ramfs carry).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stat {
    pub mode: u16,
    pub uid: u16,
    pub gid: u16,
    pub size: u64,
    pub mtime: u32,
    pub ty: crate::aerofs::FileType,
}

/// One directory entry as returned by `read_dir`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DirEnt {
    pub name: alloc::string::String,
    pub ino: u64,
    pub ty: crate::aerofs::FileType,
}
