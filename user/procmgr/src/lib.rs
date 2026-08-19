//! The POSIX sidecar's proc manager — the layer above the VFS.
//!
//! Implements the task half of Phase 2 design §3.2–§3.3 and the fork/exec
//! semantics of §4: cooperative tasks over the VFS's per-task fd tables,
//! cwd and credentials; `fork` (fd-table copy with shared `FileNode`s, so
//! offsets are shared) and `fork_thread` (CLONE_FILES — the table object
//! itself is shared); exit/zombie/wait with orphan reparenting to init;
//! `exec` through the mount chain into a BusyBox-style applet registry;
//! and blocking reads (`Ctx::read_blocking`) that park a task on an empty
//! pipe or console instead of spinning, woken by the scheduler's
//! read-wake drain when data, EOF, or input arrives.
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
    is_child, BlockReason, Ctx, ProcManager, Program, ReadBlock, Step, TaskCtl, TaskState,
    WaitOutcome, FORK_MARKER,
};
