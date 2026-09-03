//! The e1000 driver flat binary (feature `target`).
//!
//! The real entry point is `_start` — the crt0 in the LIB (global_asm! in
//! entry.rs), placed at address 0 by e1000.ld. This crate root exists only
//! so cargo links the binary.

#![cfg_attr(target_os = "none", no_std)]
#![cfg_attr(target_os = "none", no_main)]

#[cfg(target_os = "none")]
extern crate aerosls_e1000_driver;

#[cfg(not(target_os = "none"))]
fn main() {
    panic!("the e1000 driver sidecar binary is only buildable for x86_64-unknown-none");
}
