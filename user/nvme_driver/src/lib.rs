//! AeroSLS NVMe driver sidecar (`aerosls.nvme.v1`) — the Phase 5 storage
//! stack's bottom layer (`docs/AeroSLS-Self-Hosted-Phase5-Design-v0.1.md` §3).
//!
//! Implements the `idl/blockdevice.aeroidl` interface — INFO / READ / WRITE /
//! FLUSH / MAP — as a passive, connection-agnostic channel server. The wire
//! realization is the shared RD_* protocol in `aerosls_proto` (the same one
//! the ramdisk driver serves): the IDL's `BlockOp` values 1–5 and
//! `BlockError` codes 0–8 are exactly `RD_INFO..RD_MAP` and `RD_OK..`
//! `RD_ERR_PROTO`, so one protocol serves both drivers. What differs from the
//! ramdisk is the backing store: this driver fronts a real NVMe controller,
//! so block I/O happens through DMA commands (PRP lists) instead of memory
//! copies.
//!
//! Design posture mirrors the ramdisk: **dumb, passive, connection-
//! agnostic**. The driver has no filesystem knowledge, no timers, and never
//! initiates; it serves raw sectors to whoever holds a channel to it. Per-
//! request I/O is bounded (`MAX_SECTORS` = 64 sectors = 32 KiB), matching
//! the RD_* protocol's window, so a request is at most 8 pages — one NVMe
//! command with a single PRP list.
//!
//! - `kapi`      — kernel ABI (re-exported from `aerosls_proto::kabi`).
//! - `copy`      — the driver's unsafe block-copy primitive (memmove).
//! - `heap`      — bump allocator over the budget region (bootstrap
//!   contract; v1 is allocation-free).
//! - `endpoints` — RD_* endpoint set (adoption + handshake state).
//! - `prp`       — pure PRP-address arithmetic, ported 1:1 from
//!   `drivers/nvme_io.c`'s `nvme_build_prp`/`nvme_build_prp_gather` and
//!   host-tested — the part where a mistake silently corrupts memory.
//! - `framepool` — page-aligned frame pool over the manifest's `dma` MEM
//!   cap (queues, PRP list page, bounce buffers).
//! - `backend`   — the `BlockBackend` trait (what the server talks to) and
//!   `SimBackend`, an arena-backed stand-in for host tests.
//! - `server`    — the RD_* dispatch loop and handlers.
//! - `nvme`      — the real NVMe MMIO backend (feature `target`): a Rust
//!   port of `drivers/nvme.c` + `nvme_admin.c` + `nvme_io.c`.
//! - `entry`     — the real `rust_entry` over the extern "C" kernel ABI
//!   (feature `target`); `crt0.S` is the assembly bootstrap (build artifact,
//!   like the ramdisk's).

#![cfg_attr(not(test), no_std)]

pub mod backend;
pub mod copy;
pub mod endpoints;
pub mod framepool;
pub mod heap;
pub mod kapi;
pub mod prp;
pub mod server;

#[cfg(feature = "target")]
pub mod nvme;

#[cfg(feature = "target")]
mod entry;
