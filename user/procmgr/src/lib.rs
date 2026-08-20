//! The POSIX sidecar's proc manager — the layer above the VFS.
//!
//! Implements the task half of Phase 2 design §3.2–§3.3 and the fork/exec
//! semantics of §4: cooperative tasks over the VFS's per-task fd tables,
//! cwd and credentials; `fork` (fd-table copy with shared `FileNode`s, so
//! offsets are shared) and `fork_thread` (CLONE_FILES — the table object
//! itself is shared); exit/zombie/wait with orphan reparenting to init;
//! `exec` through the mount chain into a BusyBox-style applet registry,
//! with the new image's argv set by the caller (design §6.2's `ExecSpec`
//! — `Program::with_argv` / `Ctx::argv`); and blocking I/O
//! (`Ctx::read_blocking` / `Ctx::write_blocking`) that
//! park a task on an empty pipe or console, or on a full pipe, instead of
//! spinning — woken by the scheduler's wake drain when data, EOF, input,
//! the console-close event, or free space arrives.
//!
//! Blocked-task liveness is observable through `wake_trace` (`WakeEvent::
//! Parked`/`Woken`, each carrying the `BlockReason`) — every park records
//! why a task left the run queue, every wake records the reason it was
//! blocked on; a park without a later wake is a wedged task.
//!
//! The manager **owns the VFS** (the in-core call path); the
//! architecture's internal-bus message exchange between components is the
//! future split. Like every other crate in the chain it is generic over
//! `aerosls_proto::kabi::Kernel` + `BufferAlloc`, so the same code runs
//! against `aerosls-kernel-sim` and the real kernel ABI.
//!
//! - `procmgr` — `ProcManager`, `Program`/step model, `Ctx`, scheduler.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod procmgr;

#[cfg(test)]
mod tests;

pub use procmgr::{
    is_child, signal_exit_code, BlockReason, Ctx, ProcManager, Program, ReadBlock,
    SIGHUP, SIGINT, SIGQUIT, SIGKILL, SIGPIPE, SIGTERM, SignalSet, Step, TaskCtl,
    TaskState, WaitOutcome, WakeEvent, WriteBlock, WNOHANG, FORK_MARKER,
};
