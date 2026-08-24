//! The driver's shared unsafe copy primitive (the same module the ramdisk
//! driver carries, kept here so the NVMe driver's unsafe surface is one
//! audited function).
//!
//! `memmove` semantics on purpose: in the shared-address-space MVP a client
//! grant could in principle alias the backing region or a DMA bounce buffer,
//! and `ptr::copy` is correct for overlapping ranges where
//! `copy_nonoverlapping` would be UB. Bounds are established by callers
//! (`server.rs` validates grants against the backend's geometry; the NVMe
//! backend bounds bounce copies by `sectors * 512`).
//!
//! Used by: `SimBackend` (storage ↔ grant copies) and the real NVMe backend
//! (bounce buffer ↔ grant copies).

/// Block copy with memmove semantics. Callers must have established that
/// `src[..bytes]` and `dst[..bytes]` are readable/writable; this function
/// itself performs no bounds checks (it is the audited primitive).
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
