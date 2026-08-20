//! AeroSLS network driver sidecar (`aerosls.network.v1`).
//!
//! Design posture: **dumb, passive, connection-agnostic**. The driver has no
//! filesystem knowledge, no notion of names or peers, no timers, and never
//! initiates. It holds a network capability region and serves raw socket
//! operations (`NET_*`) to whoever holds a channel to it.
//!
//! - `kapi`    — the kernel ABI (re-exported from `aerosls_proto::kabi`),
//!   including the real `extern "C"` ABI behind the `target` feature.
//! - `bootinfo`— Boot Info Block parsing (Phase 2 §6.1).
//! - `heap`    — bump allocator over the budget region (reserved for future use).
//! - `endpoints` — NET_* endpoint set: adoption and per-endpoint handshake.
//! - `server`  — the `NET_*` dispatch and handlers.
//! - `mock`    — in-memory mock network backend for testing.
//!
//! The `target` feature adds `entry` (the real `rust_entry` over the extern
//! "C" kernel ABI) and is off by default so `cargo test` links the fake
//! kernel in `tests/` instead.

#![cfg_attr(not(test), no_std)]

pub use aerosls_proto::bootinfo;

pub mod endpoints;
pub mod heap;
pub mod kapi;
pub mod mock;
pub mod server;

#[cfg(feature = "target")]
mod entry;
