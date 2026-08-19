//! The driver's entire unsafe surface (implementation plan §7).
//!
//! `memmove` semantics on purpose: in the shared-address-space MVP a client
//! grant could in principle alias the storage region (whose address layout is
//! not secret), and `ptr::copy` is correct for overlapping ranges where
//! `copy_nonoverlapping` would be UB. Bounds and rights are established by
//! the caller in `server.rs` before this runs:
//!
//! 1. `src` is inside the storage region: `storage_base + lba * 512`, with
//!    `lba + count <= blocks` checked against the storage cap.
//! 2. `dst` is inside a kernel-minted grant: the kernel validated the region
//!    against the client's table, and the driver re-checked
//!    `grant.len >= bytes` and the rights bit.
//! 3. `bytes <= MAX_IO * BLOCK_SIZE` (32 KiB), so the copy is bounded by
//!    construction.

/// Block copy with memmove semantics. Both call sites double-checked their
/// ranges; this function itself does not (it is the audited primitive).
pub unsafe fn copy_blocks(src: *const u8, dst: *mut u8, bytes: usize) {
    unsafe { core::ptr::copy(src, dst, bytes) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn copy_is_memmove_semantics() {
        // Overlapping ranges: dst [0,4) <- src [4,8); must read the *old*
        // src bytes (memmove), which ptr::copy guarantees.
        let mut buf = [0u8, 1, 2, 3, 4, 5, 6, 7, 8, 9];
        unsafe {
            copy_blocks(buf[4..8].as_ptr(), buf[0..4].as_mut_ptr(), 4);
        }
        assert_eq!(&buf[0..4], &[4, 5, 6, 7]);
        // src region unchanged
        assert_eq!(&buf[4..8], &[4, 5, 6, 7]);
    }

    #[test]
    fn copy_basic() {
        let src = [9u8; 512];
        let mut dst = [0u8; 512];
        unsafe {
            copy_blocks(src.as_ptr(), dst.as_mut_ptr(), 512);
        }
        assert_eq!(dst, src);
    }
}
