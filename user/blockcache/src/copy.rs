//! The block cache's entire unsafe surface: two raw memory copies.
//!
//! Mirrors the driver's `copy` module, for the same reason: in the shared
//! address-space model the buffer the client grants for `RD_READ` is real
//! client memory at a kernel-minted `base`, so the client reads it back with
//! a raw pointer, exactly as the driver wrote into it. `ptr::copy` (memmove
//! semantics, deliberately not `copy_nonoverlapping`) because in shared space
//! the source and destination can in principle alias — the same rationale as
//! the driver's copy. In the future isolated model (per-sidecar page tables)
//! the `base` is simply the mapped view's address and this code is unchanged.

/// Copy `dst.len()` bytes from client memory at `base`.
///
/// # Safety
/// The caller must guarantee `base..base + dst.len()` is valid, mapped,
/// readable client-owned memory (a buffer from `BufferAlloc`, or a
/// `MappedView` region), and that no reference aliasing the destination is
/// live concurrently.
pub unsafe fn copy_from(base: u64, dst: &mut [u8]) {
    unsafe {
        core::ptr::copy(base as *const u8, dst.as_mut_ptr(), dst.len());
    }
}

/// Copy `src.len()` bytes into client memory at `base`.
///
/// # Safety
/// The caller must guarantee `base..base + src.len()` is valid, mapped,
/// writable client-owned memory, and that no reference aliasing the source
/// is live concurrently.
pub unsafe fn copy_to(base: u64, src: &[u8]) {
    unsafe {
        core::ptr::copy(src.as_ptr(), base as *mut u8, src.len());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let mut mem = [0u8; 32];
        let base = mem.as_mut_ptr() as u64;
        let src = b"hello, aeroSLS";
        unsafe { copy_to(base, src) };
        let mut dst = [0u8; 14];
        unsafe { copy_from(base, &mut dst) };
        assert_eq!(&dst[..], &src[..]);
    }

    #[test]
    fn memmove_semantics() {
        // dst overlapping the source (shift right by 2): memmove must keep
        // the original bytes, not clobber them.
        let mut mem = b"abcdefgh".to_vec();
        let base = mem.as_mut_ptr() as u64;
        unsafe { copy_from(base, &mut mem[2..]) };
        assert_eq!(&mem, b"ababcdef");
    }
}
