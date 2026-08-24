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
//! - `heap`     — bump allocator over the budget region (the global
//!   allocator `alloc` needs in the final binary).
//! - `entry`    — the `extern "C"` entry point (feature `target`), plus
//!   the crt0 (crt0.S via global_asm!) and the panic handler.
//!
//! v1 scope: the DM adopts devices (logs what it would spawn) and replies
//! ready; spawning the NVMe/e1000 driver sidecars per the registry is the
//! composition milestone (Phase 5 §2.2) and is deliberately not faked.

#![cfg_attr(not(test), no_std)]

pub mod heap;
pub mod server;

#[cfg(feature = "target")]
mod entry;

pub use server::{DmError, DmOutcome, DmServer, MSG_DEVICE_REGISTRY, MSG_DEVICES_READY};
