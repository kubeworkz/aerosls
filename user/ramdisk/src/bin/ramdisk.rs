//! The ramdisk driver flat binary (feature `target`).
//!
//! The real entry point is `_start` — the crt0 in the LIB, placed at
//! address 0 by ramdisk.ld. This crate root exists only so cargo links
//! the binary.

#![cfg_attr(target_os = "none", no_std)]
#![cfg_attr(target_os = "none", no_main)]

#[cfg(target_os = "none")]
extern crate aerosls_ramdisk;

#[cfg(not(target_os = "none"))]
fn main() {
    panic!("the ramdisk sidecar binary is only buildable for x86_64-unknown-none");
}
