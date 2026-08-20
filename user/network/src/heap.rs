//! Minimal bump allocator over the budget region.
//!
//! Reserved for future use: v1 of the driver is allocation-free (per-endpoint
//! state is a fixed array, `endpoints.rs`), but the bootstrap contract
//! initializes the heap, so it exists and is unit-tested.

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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bump_alloc_aligns_and_exhausts() {
        let mut backing = [0u8; 1024];
        let raw = backing.as_mut_ptr() as usize;
        let pad = (64 - raw % 64) % 64;
        let base = raw + pad;
        let mut h = Bump::new();
        h.init(base, 1024 - pad);

        let a = h.alloc(16, 8).unwrap();
        let b = h.alloc(64, 16).unwrap();
        let c = h.alloc(1, 32).unwrap();
        assert_eq!(a % 8, 0);
        assert_eq!(b % 16, 0);
        assert_eq!(c % 32, 0);
        assert!(a >= base && a < b && b < c);
        assert!(c + 1 <= base + (1024 - pad));
        assert_eq!(h.used(), c + 1 - base);
        assert_eq!(h.remaining(), (base + (1024 - pad)) - (c + 1));

        assert!(h.alloc(2048, 1).is_none());
        assert!(h.alloc(16, 1).is_some());
        assert_eq!(h.used(), c + 1 - base + 16);
    }
}
