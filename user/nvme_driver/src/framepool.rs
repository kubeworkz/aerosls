//! Page-aligned frame pool over the manifest's `dma` MEM cap.
//!
//! The real NVMe backend needs physically addressable, 4 KiB-aligned memory
//! for three things the C driver used to take from `allocate_physical_ram_frame()`:
//! the admin queues, the I/O queues + shared PRP list page, and bounce
//! buffers for client grants that are not page-aligned. In the sidecar model
//! that memory is a capability: the kernel hands the driver a `dma` MEM
//! region (physical RAM, identity-mapped in the MVP) and this pool carves
//! 4 KiB frames out of it. The pool is a bump allocator — the driver has no
//! free-list because its allocations are all for the device's lifetime
//! (queues, PRP list) or for the duration of one request (bounce buffers),
//! and a request completes before the next starts (window = 1).
//!
//! Pure arithmetic over caller-supplied addresses, so it is host-tested.

use crate::prp::NVME_PAGE_SIZE;

pub struct FramePool {
    base: u64,
    end: u64,
    cursor: u64,
}

impl FramePool {
    pub const fn new() -> FramePool {
        FramePool {
            base: 0,
            end: 0,
            cursor: 0,
        }
    }

    /// Take ownership of `[base, base + len)` as the frame region.
    pub fn init(&mut self, base: u64, len: u64) {
        self.base = base;
        self.end = base + len;
        self.cursor = base;
    }

    /// Allocate one 4 KiB-aligned frame, or `None` when exhausted.
    pub fn alloc_page(&mut self) -> Option<u64> {
        self.alloc_pages(1)
    }

    /// Allocate `n` contiguous, 4 KiB-aligned frames. `n == 0` is a no-op
    /// success returning `Some(0)` (no frames touched); callers never pass 0
    /// in practice (a zero-sector request is rejected by the server before
    /// it reaches the backend).
    pub fn alloc_pages(&mut self, n: u32) -> Option<u64> {
        if n == 0 {
            return Some(0);
        }
        let page = NVME_PAGE_SIZE as u64;
        // Overflow-safe alignment: a cursor near u64::MAX must refuse, not
        // wrap (the region's `end` is exclusive and can never exceed
        // u64::MAX, so this only fires on a malformed pool).
        let aligned = self.cursor.checked_add(page - 1)? & !(page - 1);
        let bytes = n as u64 * page;
        let next = aligned.checked_add(bytes)?;
        if next > self.end {
            return None;
        }
        self.cursor = next;
        Some(aligned)
    }

    pub fn remaining(&self) -> u64 {
        self.end.saturating_sub(self.cursor)
    }
}

impl Default for FramePool {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PAGE: u64 = NVME_PAGE_SIZE as u64;

    /// A leaky, page-aligned arena the pool can own (tests only). The box
    /// is padded so the aligned base has `bytes` usable behind it.
    fn arena(bytes: usize) -> (u64, u64) {
        let b = Box::leak(vec![0u8; bytes + PAGE as usize].into_boxed_slice());
        let raw = b.as_ptr() as u64;
        let aligned = (raw + (PAGE - 1)) & !(PAGE - 1);
        (aligned, bytes as u64)
    }

    #[test]
    fn pages_are_aligned_contiguous_and_monotonic() {
        let (base, len) = arena(64 * 1024);
        let mut p = FramePool::new();
        p.init(base, len);

        let a = p.alloc_page().unwrap();
        let b = p.alloc_pages(3).unwrap();
        let c = p.alloc_page().unwrap();

        assert_eq!(a % PAGE, 0);
        assert_eq!(b % PAGE, 0);
        assert_eq!(c % PAGE, 0);
        // Contiguous run and strictly increasing, no overlap.
        assert_eq!(a + PAGE, b);
        assert_eq!(b + 3 * PAGE, c);
        assert!(c + PAGE <= base + len);
        assert_eq!(p.remaining(), (base + len) - (c + PAGE));
    }

    #[test]
    fn exhaustion_returns_none() {
        // Exactly two pages: first two succeed, the third is refused, and
        // nothing is left.
        let (base, len) = arena(2 * PAGE as usize);
        let mut p = FramePool::new();
        p.init(base, len);
        assert!(p.alloc_page().is_some());
        assert!(p.alloc_page().is_some());
        assert!(p.alloc_page().is_none());
        assert_eq!(p.remaining(), 0);
    }

    #[test]
    fn uninitialized_pool_refuses() {
        let mut p = FramePool::new();
        assert_eq!(p.alloc_page(), None);
        assert_eq!(p.remaining(), 0);
    }

    #[test]
    fn overflow_safe() {
        // The largest page-aligned region that can still hold one full page:
        // base + PAGE fits in u64, but base + 2*PAGE would wrap. Both the
        // alignment step and the extent check must refuse rather than wrap.
        let base = u64::MAX - 2 * PAGE + 1; // == 0xFFFF_FFFF_FFFF_E000
        let mut p = FramePool::new();
        p.init(base, PAGE);
        assert_eq!(p.alloc_pages(2), None, "two pages would wrap past u64::MAX");
        assert_eq!(p.alloc_page(), Some(base));
        assert_eq!(p.alloc_page(), None, "exhausted at the top of the address space");
        assert_eq!(p.remaining(), 0);
    }
}
