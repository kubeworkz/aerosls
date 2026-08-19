//! Minimal bump allocator over the budget region.
//!
//! The bootstrap contract (Phase 2 §6.2 step 3) stands the heap up over the
//! budget MEM cap. V1 is reserved, mirroring the ramdisk driver's heap: the
//! sidecar image's real allocator (free-list over the budget) is future
//! work, but the region is claimed and the Bump is exercised by the entry
//! point so the contract is not silently dropped.

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
