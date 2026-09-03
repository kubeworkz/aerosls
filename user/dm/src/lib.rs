//! The AeroSLS Device Manager sidecar (`aerosls.device_manager.v1`, kernel
//! registry name `drv.device_manager.0`) — Phase 5 system composition's
//! orchestration point (design doc §2).
//!
//! Init spawns it through the real `k_create_sidecar` path (the manifest
//! is built by `user/init/src/dm_manifest.rs`; the messenger channel
//! `cap_create_sidecar` mints appears in the DM's BIB as the two unnamed
//! CHAN caps) and sends the device registry over the messenger — the
//! count as the payload plus the table as a read-only MEM cap grant. This
//! sidecar:
//!
//! 1. parses the Boot Info Block and resolves its caps (messenger R/W,
//!    console, budget);
//! 2. blocks (park) on the messenger until the registry arrives;
//! 3. parses the registry from the granted cap (`aerosls_proto::devreg` —
//!    the shared wire format) and replies `MSG_DEVICES_READY`;
//! 4. keeps serving the messenger until it closes (init death).
//!
//! # Modules
//!
//! - `server`   — the DM core: the registry handshake + event loop,
//!   generic over `Kernel` (host-tested against the kernel-sim fake).
//! - `drv_manifest` — the drv.e1000.0 driver manifest builder + the
//!   registry-driven spawn decision (pure, host-tested).
//! - `heap`     — bump allocator over the budget region (the global
//!   allocator `alloc` needs in the final binary).
//! - `entry`    — the `extern "C"` entry point (feature `target`), plus
//!   the crt0 (crt0.S via global_asm!) and the panic handler.
//!
//! v1 scope: the DM adopts devices, replies ready, and spawns the e1000
//! driver sidecar when the registry shows a NIC the kernel handed off
//! (role-less, driver_manifest `drv.e1000.0`); NVMe drivers (also
//! class-marked) remain future spawns — not faked.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod drv_manifest;
pub mod heap;
pub mod server;

#[cfg(feature = "target")]
mod entry;

pub use drv_manifest::{E1000_MANIFEST_NAME, E1000Spawn, build_e1000_manifest, e1000_budget_base, e1000_spawn_from_registry};
pub use server::{
    DmError, DmOutcome, DmServer, DriverOutcome, MSG_DEVICE_REGISTRY, MSG_DEVICES_READY,
};
