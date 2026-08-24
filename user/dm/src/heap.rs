//! Minimal bump allocator over the budget MEM cap region.
//!
//! Matches the pattern in `user/sidecar/src/heap.rs`.  The bootstrap contract
//! (Phase 5 §1.5 step 1) stands the heap up over the budget region before any
//! allocation.  V1 is a bump allocator; a free-list allocator is future work.

/// Alignment must be a power of two.
pub struct Bump {
    base: usize,
    end: usize,
    cursor: usize,
}

impl Bump {
    pub const fn new() -> Bump {
        Bump {
            base: 0,
            end: 0,
            cursor: 0,
        }
    }

    /// Take ownership of `[base, base + len)` as the allocatable region.
    pub fn init(&mut self, base: usize, len: usize) {
        self.base = base;
        self.end = base + len;
        self.cursor = base;
    }

    /// Allocate `size` bytes aligned to `align` (power of two), or `None`
    /// when the region is exhausted.
    pub fn alloc(&mut self, size: usize, align: usize) -> Option<usize> {
        let a = align.max(1);
        debug_assert!(a.is_power_of_two());
        let aligned = (self.cursor + (a - 1)) & !(a - 1);
        if aligned.checked_add(size)? > self.end {
            return None;
        }
        self.cursor = aligned + size;
        Some(aligned)
    }

    /// Allocate a slice of `T` values (uninitialised).  The caller must
    /// initialise every element before reading.
    pub unsafe fn alloc_slice_uninit<T>(
        &mut self,
        count: usize,
    ) -> Option<&mut [T]> {
        let size = count * core::mem::size_of::<T>();
        let align = core::mem::align_of::<T>();
        let ptr = self.alloc(size, align)? as *mut T;
        Some(unsafe { core::slice::from_raw_parts_mut(ptr, count) })
    }

    pub fn used(&self) -> usize {
        self.cursor.saturating_sub(self.base)
    }

    pub fn remaining(&self) -> usize {
        self.end.saturating_sub(self.cursor)
    }
}

impl Default for Bump {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alloc_basic() {
        let mut bump = Bump::new();
        bump.init(0x1000, 0x1000);
        let a = bump.alloc(16, 8).unwrap();
        assert_eq!(a, 0x1000);
        let b = bump.alloc(32, 16).unwrap();
        assert_eq!(b, 0x1010); // 0x1000 + 16 = 0x1010, already 16-aligned
        assert_eq!(bump.used(), 16 + 32);
        assert_eq!(bump.remaining(), 0x1000 - 48);
    }

    #[test]
    fn alloc_exhausted() {
        let mut bump = Bump::new();
        bump.init(0x1000, 64);
        assert!(bump.alloc(32, 8).is_some());
        assert!(bump.alloc(32, 8).is_some());
        assert!(bump.alloc(1, 8).is_none());
    }
}
