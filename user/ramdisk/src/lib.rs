//! AeroSLS ramdisk driver sidecar (`aerosls.ramdisk.v1`).
//!
//! Implementation of `docs/AeroSLS-Ramdisk-Driver-Implementation-Plan-v0.1.md`.
//!
//! Design posture: **dumb, passive, connection-agnostic**. The driver has no
//! filesystem knowledge, no notion of names or peers, no timers, and never
//! initiates. It holds a read-only view of the ramdisk region and serves raw
//! blocks (`RD_*`) to whoever holds a channel to it.
//!
//! - `kapi`    — the kernel ABI (re-exported from `aerosls_proto::kabi`,
//!   including the real `extern "C"` ABI behind the `target` feature), so
//!   the core is host-testable.
//! - `bootinfo`— Boot Info Block parsing (Phase 2 §6.1; lives in
//!   `aerosls_proto::bootinfo`, re-exported here).
//! - `heap`    — bump allocator over the budget region (reserved for future
//!   use; v1 is allocation-free).
//! - `endpoints` — RD_* endpoint set: adoption (initial-table scan +
//!   `NEW_CHANNEL`) and per-endpoint handshake state.
//! - `server`  — the `RD_*` dispatch and handlers.
//! - `copy`    — the driver's entire unsafe surface (memmove block copy).
//!
//! The `target` feature adds `entry` (the real `rust_entry` over the extern
//! "C" kernel ABI) and is off by default so `cargo test` links the fake
//! kernel in `tests/` instead.

#![cfg_attr(not(test), no_std)]

pub use aerosls_proto::bootinfo;

pub mod copy;
pub mod endpoints;
pub mod heap;
pub mod kapi;
pub mod server;

#[cfg(feature = "target")]
mod entry;
