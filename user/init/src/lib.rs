//! The AeroSLS init sidecar (`aerosls.init.v1`) — Phase 5 system orchestrator.
//!
//! This crate is the first sidecar the kernel spawns after boot.  It:
//!
//! 1. Parses the Boot Info Block to resolve initial capabilities.
//! 2. Initialises the heap from the budget MEM cap.
//! 3. Reads the device registry (kernel-populated PCI enumeration table).
//! 4. Spawns the Device Manager via `create_sidecar` (SYS_SLS_CREATE_SIDECAR).
//! 5. Sends the device registry snapshot over the parent channel.
//! 6. Waits for the Device Manager's "devices ready" signal.
//! 7. Spawns the POSIX sidecar (and optionally WASM/Lisp runtimes).
//! 8. Parks in the scheduler event loop.
//!
//! # Modules
//!
//! - `heap`       — bump allocator over the budget region.
//! - `devreg`     — device registry parser (reads the kernel-populated table).
//! - `chan`       — thin channel helpers over the `Kernel` trait.
//! - `demo`       — the demo server loop: blocking wait/send + the finite-
//!                 deadline Device Manager handshake, plus the watchdog
//!                 respawn (bounded backoff, crash-loop breaker) that
//!                 self-heals a dead DM (the Phase 5 "real sidecar
//!                 workload" proof).
//! - `entry`      — the `extern "C"` entry point (feature `target`).
//! - `sim`        — host-side fake kernel for testing.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod chan;
pub mod demo;
pub mod dm_manifest;
pub mod heap;

#[cfg(feature = "target")]
mod entry;

#[cfg(test)]
pub mod sim;

/// The device-registry wire format lives in `aerosls_proto` (shared with
/// the Device Manager sidecar, which parses the same table from the MEM
/// cap init grants it); re-export the module so `crate::devreg::*` keeps
/// working exactly as before.
pub use aerosls_proto::devreg;
pub use chan::{ChannelError, InitChannel};
pub use heap::Bump;
