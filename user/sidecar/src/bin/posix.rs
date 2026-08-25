//! The POSIX sidecar flat binary (feature `target`).
//!
//! The real entry point is `_start` — the crt0 in the LIB (crt0.S via
//! `global_asm!` in entry.rs), placed at address 0 by posix.ld
//! (`ENTRY(_start)`). This crate root exists only so cargo links the
//! binary; it deliberately has no `main` (`#![no_main]`), and the linker
//! script's `ENTRY(_start)` resolves the crt0 out of the lib rlib.
//!
//! Build for the freestanding image:
//!
//! ```sh
//! cargo build -p aerosls-sidecar --features target \
//!     --target x86_64-unknown-none --release --bin posix
//! ```
//!
//! That produces `target/x86_64-unknown-none/release/posix` (ELF); flatten
//! it with `aerosls-bootimage flatten` (or objcopy -O binary) for the
//! kernel. `make selfhost-bootimage` does both.

#![cfg_attr(target_os = "none", no_std)]
#![cfg_attr(target_os = "none", no_main)]

// Pull the lib (and its lang items — the `#[panic_handler]`) into this
// crate's dependency graph explicitly; an otherwise-empty root would let
// rustc skip the lib's lang-item registration.
#[cfg(target_os = "none")]
extern crate aerosls_sidecar;

#[cfg(not(target_os = "none"))]
fn main() {
    panic!("the posix sidecar binary is only buildable for x86_64-unknown-none");
}
