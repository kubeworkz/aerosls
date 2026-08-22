//! # aerosls-shared-arena
//!
//! Cross-sidecar shared arena allocator for the AeroSLS Polyglot Nexus
//! (Phase 3). Enables zero-copy data sharing between language sidecars
//! via a shared memory region with atomic reference counting.
//!
//! ## Architecture
//!
//! ```text
//! ┌──────────────────────────────────────────────────────────────────┐
//! │                    Shared Arena (64 MiB)                         │
//! │                                                                  │
//! │  ┌──────────────────────────────────────────────────────┐       │
//! │  │ ArenaAllocator (bitmap, spinlock)                     │       │
//! │  ├──────────────────────────────────────────────────────┤       │
//! │  │ Object 1: [Header|refcount=2|size|owner|flags] [data]│       │
//! │  │ Object 2: [Header|refcount=1|size|owner|flags] [data]│       │
//! │  │ ...                                                  │       │
//! │  └──────────────────────────────────────────────────────┘       │
//! │                                                                  │
//! │  Sidecar A holds MEM cap ─────┐    ┌───── MEM cap Sidecar B     │
//! │  (refcount incremented)       │    │     (refcount incremented)  │
//! │                               ▼    ▼                             │
//! │                            Same object                           │
//! └──────────────────────────────────────────────────────────────────┘
//! ```
//!
//! ## Ownership Semantics
//!
//! | Annotation | Refcount | Lifetime | Freeing |
//! |------------|----------|----------|---------|
//! | `@borrowed` | stays at 1 | request duration | caller frees |
//! | `@owned` | stays at 1 (moved) | callee lifetime | callee frees |
//! | `@arena` | incremented on transfer | until refcount=0 | last holder frees |
//!
//! ## Thread Safety
//!
//! - The allocator bitmap uses an atomic spinlock for cross-sidecar safety.
//! - Object refcounts use `AtomicU32` operations.
//! - `#![no_std]` compatible — no `std` or `libc` required.

#![no_std]

extern crate alloc;

pub mod allocator;
pub mod arena;
pub mod header;

pub use allocator::ArenaAllocator;
pub use arena::{Arena, ArenaError};
pub use header::{ArenaHeader, FLAG_BINARY, FLAG_STRING, HEADER_SIZE};

#[cfg(test)]
mod tests {
    use crate::allocator::AlignedRegion;

    use super::*;

    #[test]
    fn full_lifecycle() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        // Allocate a string
        let msg = b"cross-sidecar log message";
        let (ptr1, len1) = arena.alloc_string(msg, 1).unwrap();
        assert_eq!(len1, msg.len() as u32);

        unsafe {
            let slice = core::slice::from_raw_parts(ptr1, msg.len());
            assert_eq!(slice, msg);
        }

        // @arena transfer
        unsafe {
            let new_ref = arena.inc_ref(ptr1 as *const u8);
            assert_eq!(new_ref, 2);
        }
        unsafe { arena.free(ptr1).unwrap(); }
        unsafe {
            assert_eq!(arena.header(ptr1 as *const u8).refcount(), 1);
        }
        unsafe { arena.free(ptr1).unwrap(); }

        // Allocate an f64 array (like sqrt_batch output)
        let count = 1000u32;
        let (ptr2, _) = arena.alloc(count * 8, 1, FLAG_BINARY).unwrap();

        unsafe {
            let floats = core::slice::from_raw_parts_mut(ptr2 as *mut f64, count as usize);
            for i in 0..count as usize {
                floats[i] = (i as f64) * 0.1;
            }
            assert!((floats[0] - 0.0).abs() < f64::EPSILON);
            assert!((floats[999] - 99.9).abs() < 0.01);
        }

        unsafe { arena.free(ptr2).unwrap(); }
    }

    #[test]
    fn arena_not_initialized() {
        let arena = Arena::new();
        assert_eq!(arena.alloc(64, 1, 0), Err(ArenaError::NotInitialized));
    }

    #[test]
    fn header_is_16_bytes() {
        assert_eq!(core::mem::size_of::<ArenaHeader>(), 16);
    }
}
