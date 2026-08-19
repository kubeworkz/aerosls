//! The POSIX sidecar's block cache — the ramdisk protocol *client*.
//!
//! This crate is the counterpart of the ramdisk driver sidecar
//! (`aerosls-ramdisk`): the driver is a dumb, passive `RD_*` block server;
//! this is the component inside the POSIX sidecar that talks to it. It
//! handshakes (`RD_INFO`), issues `RD_READ`/`RD_WRITE` with transient MEM
//! grants of client-owned buffers (W-only for reads, R-only for writes),
//! establishes a durable `RD_MAP` view, caches recently read blocks, and
//! flips the device to `STALE` when the driver dies — the device half of the
//! respawn state machine (`docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md`
//! §4.1). Recovery (respawn) is the respawn layer's job, not this crate's.
//!
//! The cache is written generically over `aerosls_proto::kabi::Kernel`, so
//! the same code runs against the host fake kernel (`aerosls-kernel-sim`,
//! see `tests/`) and the real kernel ABI in the sidecar image.
//!
//! - `cache` — `BlockCache`, `MappedView`, `BufferAlloc`, `State`, `Error`.
//! - `copy` — the crate's entire unsafe surface (raw memory copies).

#![cfg_attr(not(test), no_std)]

pub mod cache;
pub mod copy;

pub use cache::{BlockCache, BufferAlloc, DeviceInfo, Error, MappedView, State, NCACHE};
