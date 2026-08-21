//! The POSIX sidecar's VFS — the filesystem layer above the block cache.
//!
//! This crate is the next layer up from `aerosls-blockcache` in the Phase 2
//! chain: the block cache is the ramdisk protocol *client*; this is the
//! component inside the POSIX sidecar that turns blocks into
//! `open`/`read`/`write`/`stat`. It owns the mount table (`/` ← aerofs-lite
//! over the ramdisk device, `/tmp` ← ramfs) and every file descriptor, with
//! the semantics pinned by `docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`
//! (§3.2, §3.4) and `docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md`
//! (§4.2, §6): no ambient namespace, fds as sidecar-local capabilities with
//! rights minted at open, shared offsets via `Arc<FileNode>`, stale mounts
//! failing `EIO` (never `ENOENT`), and pre-death fds failing permanently
//! across a remount (never silently reconnecting).
//!
//! Written generically over `aerosls_proto::kabi::Kernel` + `BufferAlloc`
//! like the block cache, so the identical code runs against the host fake
//! kernel (`aerosls-kernel-sim`) and the real kernel ABI in the sidecar
//! image.
//!
//! - `aerofs` — the aerofs-lite on-disk format + the `genrootfs` image
//!   builder (pure, no I/O).
//! - `ramfs` — the in-memory `/tmp` filesystem (never stale).
//! - `fileobj` — file-like objects: pipes (`pipe()`), character devices
//!   (`/dev/console`, `/dev/null`) — everything an fd can name that is not
//!   a file on a mount.
//! - `vfs` — mounts, path resolution, fd tables, the syscall surface.
//! - `errno` — the shared POSIX errno set.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod aerofs;
pub mod errno;
pub mod fileobj;
pub mod ramfs;
pub mod vfs;

pub use aerofs::{FileType, ImageBuilder, SuperblockRecord};
pub use aerosls_blockcache::BufferAlloc;
pub use errno::{DirEnt, Errno, Stat};
pub use fileobj::{CharNode, ConsoleIo, FileObj, PipeNode, PtyState, SocketMeta, SocketState, PIPE_CAP, Termios, TCGETS, TCSETS, TIOCGWINSZ, TIOCSWINSZ, TIOCSCTTY, WinSize};
pub use vfs::{
    rights_of, FileNode, FdEntry, MountState, Vfs, CLONE_FILES, O_ACCMODE, O_APPEND, O_CREAT,
    O_EXCL, O_RDONLY, O_RDWR, O_TRUNC, O_WRONLY, POLLIN, POLLOUT, POLLERR, POLLHUP,
    POLLNVAL, PollFd, SelectFdSet, SelectResult, FD_SETSIZE, SEEK_CUR, SEEK_END, SEEK_SET,
};
