//! Arena object header with atomic reference counting.
//!
//! Every object allocated in the shared arena has a hidden 16-byte header
//! that precedes the user-visible data. The header is laid out so that
//! multiple sidecars can atomically increment/decrement the refcount
//! without additional synchronization.
//!
//! Memory layout (16 bytes, matches Phase 3 design spec §4.4):
//! ```text
//! offset  size  field
//! 0       4     refcount        atomic u32, starts at 1
//! 4       4     size            object size in bytes (excl. header)
//! 8       4     owner_sidecar   pid of the allocating sidecar
//! 12      4     flags           bit0 = STRING, bit1 = BINARY
//! ```
//!
//! The header resides in shared memory. When a sidecar calls `arena_free()`,
//! the kernel atomically decrements the refcount. When it reaches 0, the
//! arena pages are freed.

use core::sync::atomic::{AtomicU32, Ordering};

/// Object header size in bytes. Must be a power of two for alignment.
pub const HEADER_SIZE: usize = 16;

/// Alignment of arena objects (16 bytes ensures header is naturally aligned).
pub const OBJECT_ALIGN: usize = 16;

/// Flag: object contains UTF-8 string data.
pub const FLAG_STRING: u32 = 0x01;
/// Flag: object contains binary data.
pub const FLAG_BINARY: u32 = 0x02;

/// Refcount values.
pub const REFCOUNT_MAX: u32 = u32::MAX;

/// Invalid sidecar pid (no owner).
pub const OWNER_NONE: u32 = 0;

/// Arena object header — lives at the start of every allocated object.
///
/// # Safety
///
/// This struct is `repr(C)` and placed directly in shared memory.
/// The `refcount` field is accessed via atomic operations from multiple
/// sidecars. All other fields are written once (at allocation) and read-only
/// after that.
#[repr(C)]
#[derive(Debug)]
pub struct ArenaHeader {
    /// Atomic reference count. Starts at 1 when the object is first
    /// allocated. Incremented on ownership transfer; decremented on
    /// `arena_free()`. When it reaches 0, the pages are reclaimed.
    refcount: AtomicU32,
    /// Object size in bytes (excluding this header).
    size: u32,
    /// PID of the sidecar that allocated this object (for diagnostics).
    owner_sidecar: u32,
    /// Flags (FLAG_STRING, FLAG_BINARY).
    flags: u32,
}

impl ArenaHeader {
    /// Create a new header for a freshly allocated object.
    ///
    /// The refcount starts at 1 (the allocating sidecar owns it).
    pub fn new(size: u32, owner_sidecar: u32, flags: u32) -> Self {
        ArenaHeader {
            refcount: AtomicU32::new(1),
            size,
            owner_sidecar,
            flags,
        }
    }

    /// Get the current refcount (for diagnostics).
    pub fn refcount(&self) -> u32 {
        self.refcount.load(Ordering::Acquire)
    }

    /// Get the object size (excluding header).
    pub fn size(&self) -> u32 {
        self.size
    }

    /// Get the owning sidecar PID.
    pub fn owner(&self) -> u32 {
        self.owner_sidecar
    }

    /// Get the flags.
    pub fn flags(&self) -> u32 {
        self.flags
    }

    /// Check if the object is a string.
    pub fn is_string(&self) -> bool {
        self.flags & FLAG_STRING != 0
    }

    /// Check if the object is binary data.
    pub fn is_binary(&self) -> bool {
        self.flags & FLAG_BINARY != 0
    }

    /// Atomically increment the reference count.
    ///
    /// Called when:
    /// - A MEM cap with `@arena` semantics is sent over a channel
    /// - An `@owned` cap is transferred (refcount goes 1→1 on move, but
    ///   if both sides hold caps, it's 2)
    ///
    /// Returns the new refcount.
    pub fn inc_ref(&self) -> u32 {
        self.refcount.fetch_add(1, Ordering::AcqRel) + 1
    }

    /// Atomically decrement the reference count.
    ///
    /// Called when:
    /// - A sidecar calls `arena_free(cap)` on an `@arena` cap
    /// - A sidecar's table is torn down (process exit)
    /// - An `@owned` cap is revoked
    ///
    /// Returns the new refcount. If the refcount reaches 0, the caller
    /// should reclaim the arena pages.
    pub fn dec_ref(&self) -> u32 {
        let old = self.refcount.fetch_sub(1, Ordering::AcqRel);
        old - 1
    }

    /// Check if the refcount has reached zero (object is reclaimable).
    pub fn is_reclaimable(&self) -> bool {
        self.refcount.load(Ordering::Acquire) == 0
    }

    /// Get a pointer to the user data (after the header).
    ///
    /// # Safety
    ///
    /// The caller must ensure `base` points to a valid arena object header.
    pub unsafe fn data_ptr(base: *const ArenaHeader) -> *mut u8 {
        (base as *mut u8).add(HEADER_SIZE)
    }

    /// Get a pointer to the header from a user data pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure `data` points to the user data of an arena
    /// object (i.e., exactly HEADER_SIZE bytes after the header).
    pub unsafe fn from_data_ptr(data: *const u8) -> *const ArenaHeader {
        (data as *const ArenaHeader).sub(1)
    }

    /// Total allocation size including the header.
    pub fn total_size(size: u32) -> usize {
        (HEADER_SIZE + size as usize + OBJECT_ALIGN - 1) & !(OBJECT_ALIGN - 1)
    }
}

/// Helper: write a header to a byte buffer at a given offset.
///
/// # Safety
///
/// `buf` must have at least `offset + HEADER_SIZE` bytes.
pub unsafe fn write_header(buf: *mut u8, offset: usize, header: &ArenaHeader) {
    let dst = buf.add(offset) as *mut ArenaHeader;
    core::ptr::write_volatile(dst, core::ptr::read(header));
}

/// Helper: read a header from a byte buffer at a given offset.
///
/// # Safety
///
/// `buf` must have at least `offset + HEADER_SIZE` bytes, and the data
/// at that offset must be a valid ArenaHeader.
pub unsafe fn read_header(buf: *const u8, offset: usize) -> *const ArenaHeader {
    buf.add(offset) as *const ArenaHeader
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_size() {
        assert_eq!(core::mem::size_of::<ArenaHeader>(), 16);
    }

    #[test]
    fn header_new() {
        let h = ArenaHeader::new(1024, 42, FLAG_STRING);
        assert_eq!(h.refcount(), 1);
        assert_eq!(h.size(), 1024);
        assert_eq!(h.owner(), 42);
        assert!(h.is_string());
        assert!(!h.is_binary());
    }

    #[test]
    fn refcount_inc_dec() {
        let h = ArenaHeader::new(512, 1, 0);
        assert_eq!(h.refcount(), 1);

        let new = h.inc_ref();
        assert_eq!(new, 2);
        assert_eq!(h.refcount(), 2);

        let new = h.inc_ref();
        assert_eq!(new, 3);

        let new = h.dec_ref();
        assert_eq!(new, 2);

        let new = h.dec_ref();
        assert_eq!(new, 1);

        let new = h.dec_ref();
        assert_eq!(new, 0);
        assert!(h.is_reclaimable());
    }

    #[test]
    fn total_size_rounds_up() {
        // 16 (header) + 1 (data) = 17 → rounds to 32 (next multiple of 16)
        assert_eq!(ArenaHeader::total_size(1), 32);
        // 16 + 16 = 32 → already aligned
        assert_eq!(ArenaHeader::total_size(16), 32);
        // 16 + 17 = 33 → 48
        assert_eq!(ArenaHeader::total_size(17), 48);
    }

    #[test]
    fn data_ptr_roundtrip() {
        let mut buf = [0u8; 128];
        let header = ArenaHeader::new(64, 1, FLAG_BINARY);

        unsafe {
            write_header(buf.as_mut_ptr(), 0, &header);
            let h = read_header(buf.as_ptr(), 0);
            assert_eq!((*h).size(), 64);
            assert_eq!((*h).owner(), 1);
            assert!((*h).is_binary());

            let data = ArenaHeader::data_ptr(h);
            assert_eq!(data as usize - buf.as_ptr() as usize, HEADER_SIZE);

            let back = ArenaHeader::from_data_ptr(data);
            assert_eq!((*back).size(), 64);
        }
    }
}
