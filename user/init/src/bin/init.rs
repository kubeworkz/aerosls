//! The init sidecar flat binary (feature `target`).
//!
//! The real entry point is `_start` — the crt0 in the LIB (crt0.S via
//! `global_asm!` in entry.rs), placed at address 0 by init.ld
//! (`ENTRY(_start)`). This crate root exists only so cargo links the
//! binary; it deliberately has no `main` (`#![no_main]`), and the linker
//! script's `ENTRY(_start)` resolves the crt0 out of the lib rlib.
//!
//! Build for the freestanding image:
//!
//! ```sh
//! cargo build -p aerosls-init --features target \
//!     --target x86_64-unknown-none --release --bin init
//! ```
//!
//! That produces `target/x86_64-unknown-none/release/init` (ELF); flatten
//! it with `aerosls-bootimage flatten` (or objcopy -O binary) to get the
//! flat image the kernel maps — `make selfhost-bootimage` does both.
//!
//! On the HOST this target is a std stub: the freestanding build needs
//! `no_std` + the crate's `#[panic_handler]` + crt0, none of which exist
//! for the host (whose core is precompiled with panic=unwind). The stub
//! exists so `cargo check --features target` — the documented way to
//! typecheck the real path — stays green; it is never run.

#![cfg_attr(target_os = "none", no_std)]
#![cfg_attr(target_os = "none", no_main)]

// Pull the lib (and its lang items — the `#[panic_handler]`) into this
// crate's dependency graph explicitly; an otherwise-empty root would let
// rustc skip the lib's lang-item registration.
#[cfg(target_os = "none")]
extern crate aerosls_init;

#[cfg(not(target_os = "none"))]
fn main() {
    panic!("the init sidecar binary is only buildable for x86_64-unknown-none");
}
