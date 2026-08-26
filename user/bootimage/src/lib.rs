//! The Phase 5 boot-image builder (`aerosls-bootimage`).
//!
//! Packages the init and Device Manager sidecar binaries into an initrd
//! CPIO archive (`sidecars.cpio`, `newc` format — loadable by GRUB as a
//! Multiboot2 module, by U-Boot/OpenSBI via `-initrd`/`bootm`, and by
//! QEMU's `-initrd`) with each image at its manifest-declared physical
//! address:
//!
//! - [`layout`] computes the deterministic physical layout (init image,
//!   init heap, DM image, DM heap, device registry — page-aligned and
//!   contiguous below 4 GiB).
//! - [`builder::build_boot_image`] packs both manifests on the kernel's
//!   wire format (records + the `image_kaddr` footer) and assembles the
//!   archive, including a self-describing `boot/layout` entry.
//! - [`newc`] is the minimal newc CPIO writer + parser (the parser is the
//!   test-side ground truth and the reference for the kernel loader).
//!
//! The kernel's boot-time sidecar loader (the consumer — see
//! `kernel/boot_image.h`) copies each image from the archive to its
//! declared address, reserves `[base, base + total_size)` from the frame
//! pool, zeroes the heap/registry regions, and creates the init sidecar
//! from `boot/init.manifest`.

pub mod builder;
pub mod flatten;
pub mod layout;
pub mod newc;
pub mod rootfs;

pub use builder::{BootImage, build_boot_image};
pub use flatten::{FlattenError, Flattened, flatten_elf};
pub use layout::{BootImageSpec, BootLayout, compute_layout};
