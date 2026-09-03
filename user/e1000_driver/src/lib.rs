//! AeroSLS e1000 NIC driver sidecar (`drv.e1000.0`, personality
//! `aerosls.e1000.v1`) — Driver SDK ABI v0.1 §7.
//!
//! The kernel hands it a NIC whose assigned role is `none` (grub
//! `nic1=none` → `e1000_driver_handoff()`: bus master + uncached BAR,
//! nothing else — see docs/AeroSLS-Driver-SDK-ABI-v0.1.md §7), and the
//! driver's manifest mints `nic0.bar0` (CAP_TYPE_DEV over the MMIO BAR0)
//! plus a budget MEM cap for DMA memory. This sidecar:
//!
//! 1. `SYS_DEV_MMAP`s `nic0.bar0` → the register window (the devtest-proven
//!    path — device MMIO is reachable ONLY through the mediated mapping);
//! 2. reads the MAC the NIC loaded into RAL0/RAH0 at reset;
//! 3. programs TX/RX descriptor rings and buffers inside its budget region
//!    (identity-accessible at phys base, the POSIX-proven convention — the
//!    NIC DMA-reads/writes the same physical addresses);
//! 4. proves the data path with a PHY-loopback round trip: a frame to its
//!    own MAC is transmitted, the model routes it back into the RX ring,
//!    and the driver validates the received bytes (`[e1000] PASS` lines);
//! 5. then serves the NIC (polled RX drain; interrupt binding needs the
//!    PCI-INTx arch milestone, and frame forwarding to the network sidecar
//!    is the next composition step).
//!
//! Design posture mirrors the ramdisk/nvme drivers: dumb, passive, bounded.
//! The core (`device.rs`) is generic over an `Mmio` accessor and a raw DMA
//! base, so `cargo test` runs the identical register/ring/loopback logic
//! against an in-crate e1000 model that mirrors QEMU's `hw/net/e1000.c`
//! semantics for every register this driver touches — the verification
//! target for the on-target runs that CI's boot smoke will gate on.
//!
//! - `device` — register map, descriptor formats, the `Mmio` trait, the
//!   ring/DMA layout, and the driver core (bring-up + loopback self-test),
//!   all host-testable; `RealMmio` (feature `target`) over the dev-mapped
//!   window.
//! - `entry`  — the `extern "C"` `rust_entry` over the real kernel ABI
//!   (feature `target`); `crt0.S` is the assembly bootstrap (same shape as
//!   the network sidecar's: the kernel jumps to `_start` with the BIB
//!   pointer in `rdi`).
//!
//! v1 scope: the self-test + idle serve loop. Serving NET_* frames to the
//! network sidecar (replacing MockNetwork) and binding the NIC's real
//! interrupt are the next milestones; the watchdog/respawn policy lives
//! with the Device Manager, which spawns this sidecar from the registry.

#![cfg_attr(not(test), no_std)]

pub mod device;

#[cfg(feature = "target")]
mod entry;
