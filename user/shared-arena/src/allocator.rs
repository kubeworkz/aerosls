//! Bitmap-based page allocator for the shared arena.
//!
//! Manages 4 KiB pages within a contiguous physical region. Each page is
//! either free (bitmap bit = 0) or allocated (bitmap bit = 1).
//!
//! This allocator operates on raw pointers because it lives in shared
//! memory that is mapped by multiple sidecars. All mutation is serialized
//! through an atomic spinlock.

use core::sync::atomic::{AtomicBool, Ordering};

/// 4 KiB page size — matches the kernel's CAP_ARENA page granularity.
pub const PAGE_SIZE: usize = 4096;

/// Maximum number of pages the allocator can track.
pub const MAX_PAGES: usize = 16384;

/// Bits per u64 word in the bitmap.
const BITS_PER_WORD: usize = 64;

/// Spinlock for cross-sidecar allocation (atomic test-and-set).
pub struct ArenaSpinlock {
    locked: AtomicBool,
}

impl ArenaSpinlock {
    pub const fn new() -> Self {
        ArenaSpinlock {
            locked: AtomicBool::new(false),
        }
    }

    /// Acquire the lock, spinning until available.
    pub fn lock(&self) {
        while self
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
    }

    /// Release the lock.
    pub fn unlock(&self) {
        self.locked.store(false, Ordering::Release);
    }
}

/// Bitmap page allocator.
pub struct ArenaAllocator {
    pub(crate) total_pages: u32,
    pub(crate) free_pages: u32,
    pub(crate) lock: ArenaSpinlock,
    pub(crate) bitmap: [u64; MAX_PAGES / BITS_PER_WORD],
}

impl ArenaAllocator {
    /// Size of the allocator struct itself.
    pub const STRUCT_SIZE: usize = core::mem::size_of::<Self>();

    /// Bitmap size in bytes.
    pub const BITMAP_SIZE: usize = core::mem::size_of::<[u64; MAX_PAGES / BITS_PER_WORD]>();

    /// Compute data_start from the allocator base.
    /// data_start = round_up(STRUCT_SIZE + BITMAP_SIZE, PAGE_SIZE)
    pub const fn compute_data_start() -> usize {
        (Self::STRUCT_SIZE + Self::BITMAP_SIZE + PAGE_SIZE - 1) & !(PAGE_SIZE - 1)
    }

    /// Initialize an allocator over a contiguous memory region.
    ///
    /// # Safety
    ///
    /// `base` must point to at least `region_size` bytes of writable
    /// memory, aligned to at least 16 bytes.
    ///
    /// Returns the data_start offset (first usable page after the header).
    pub unsafe fn init(base: *mut u8, region_size: usize) -> usize {
        let allocator = &mut *(base as *mut ArenaAllocator);

        // Zero the bitmap
        for word in allocator.bitmap.iter_mut() {
            *word = 0;
        }

        let data_start = Self::compute_data_start();
        let data_size = region_size.saturating_sub(data_start);
        let total_pages = (data_size / PAGE_SIZE) as u32;

        allocator.total_pages = total_pages;
        allocator.free_pages = total_pages;
        allocator.lock = ArenaSpinlock::new();

        data_start
    }

    /// Allocate `count` contiguous pages.
    ///
    /// Returns the page index (relative to data_start) or `None`.
    ///
    /// # Safety
    ///
    /// `self_ptr` must point to a valid ArenaAllocator in writable memory.
    pub unsafe fn alloc_pages(self_ptr: *mut Self, count: u32) -> Option<u32> {
        if count == 0 || (*self_ptr).total_pages == 0 || count > (*self_ptr).total_pages {
            return None;
        }

        (*self_ptr).lock.lock();
        let result = Self::alloc_pages_inner(self_ptr, count);
        (*self_ptr).lock.unlock();
        result
    }

    /// Free `count` pages starting at `page_index`.
    ///
    /// # Safety
    ///
    /// `self_ptr` must point to a valid ArenaAllocator.
    pub unsafe fn free_pages(self_ptr: *mut Self, page_index: u32, count: u32) {
        if count == 0 || page_index + count > (*self_ptr).total_pages {
            return;
        }

        (*self_ptr).lock.lock();
        Self::free_pages_inner(self_ptr, page_index, count);
        (*self_ptr).lock.unlock();
    }

    /// Check if a page is allocated.
    pub unsafe fn is_allocated(self_ptr: *const Self, page_index: u32) -> bool {
        if page_index >= (*self_ptr).total_pages {
            return false;
        }
        let word = (page_index / BITS_PER_WORD as u32) as usize;
        let bit = page_index % BITS_PER_WORD as u32;
        (*self_ptr).bitmap[word] & (1u64 << bit) != 0
    }

    /// Number of free pages remaining.
    pub unsafe fn free_count(self_ptr: *const Self) -> u32 {
        (*self_ptr).free_pages
    }

    /// Total number of pages.
    pub unsafe fn total_count(self_ptr: *const Self) -> u32 {
        (*self_ptr).total_pages
    }

    // ── Internal (must hold lock) ───────────────────────────────────────

    unsafe fn alloc_pages_inner(self_ptr: *mut Self, count: u32) -> Option<u32> {
        let total = (*self_ptr).total_pages;
        if count > total {
            return None;
        }

        let mut run_start: Option<u32> = None;
        let mut run_len: u32 = 0;

        for page in 0..total {
            if !Self::is_page_set(self_ptr, page) {
                if run_start.is_none() {
                    run_start = Some(page);
                    run_len = 1;
                } else {
                    run_len += 1;
                }
                if run_len == count {
                    let start = run_start.unwrap();
                    for i in start..start + count {
                        Self::set_page(self_ptr, i, true);
                    }
                    (*self_ptr).free_pages -= count;
                    return Some(start);
                }
            } else {
                run_start = None;
                run_len = 0;
            }
        }

        None
    }

    unsafe fn free_pages_inner(self_ptr: *mut Self, page_index: u32, count: u32) {
        for i in page_index..page_index + count {
            Self::set_page(self_ptr, i, false);
        }
        (*self_ptr).free_pages += count;
    }

    unsafe fn is_page_set(self_ptr: *const Self, page: u32) -> bool {
        let word = (page / BITS_PER_WORD as u32) as usize;
        let bit = page % BITS_PER_WORD as u32;
        (*self_ptr).bitmap[word] & (1u64 << bit) != 0
    }

    unsafe fn set_page(self_ptr: *mut Self, page: u32, allocated: bool) {
        let word = (page / BITS_PER_WORD as u32) as usize;
        let bit = page % BITS_PER_WORD as u32;
        if allocated {
            (*self_ptr).bitmap[word] |= 1u64 << bit;
        } else {
            (*self_ptr).bitmap[word] &= !(1u64 << bit);
        }
    }
}

/// Aligned memory region for arena tests.
///
/// Uses `alloc_zeroed` with proper alignment and wraps the pointer in a
/// type that deallocates correctly on drop.
#[cfg(test)]
pub(crate) struct AlignedRegion {
    ptr: *mut u8,
    layout: core::alloc::Layout,
}

#[cfg(test)]
impl AlignedRegion {
    pub fn new(size: usize) -> Self {
        extern crate std;
        use std::alloc::{alloc_zeroed, Layout};
        let layout = Layout::from_size_align(size, 16).expect("invalid layout");
        let ptr = unsafe { alloc_zeroed(layout) };
        assert!(!ptr.is_null(), "allocation failed");
        assert!((ptr as usize) % 16 == 0, "pointer not 16-byte aligned: {:p}", ptr);
        AlignedRegion { ptr, layout }
    }

    pub fn as_mut_ptr(&self) -> *mut u8 {
        self.ptr
    }
}

#[cfg(test)]
impl Drop for AlignedRegion {
    fn drop(&mut self) {
        extern crate std;
        use std::alloc::dealloc;
        unsafe {
            dealloc(self.ptr, self.layout);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spinlock_basic() {
        let lock = ArenaSpinlock::new();
        assert!(!lock.locked.load(Ordering::Relaxed));
        lock.lock();
        assert!(lock.locked.load(Ordering::Relaxed));
        lock.unlock();
        assert!(!lock.locked.load(Ordering::Relaxed));
    }

    #[test]
    fn alloc_single_page() {
        let region = AlignedRegion::new(256 * 1024);
        let ptr = region.as_mut_ptr();
        let alloc_ptr = ptr as *mut ArenaAllocator;
        unsafe {
            ArenaAllocator::init(ptr, 256 * 1024);

            let tp = ArenaAllocator::total_count(alloc_ptr);
            let fp = ArenaAllocator::free_count(alloc_ptr);
            // data_start depends on actual struct size — just check pages > 0
            assert!(tp > 0, "total_pages should be > 0 (ptr={:p}, align={})", ptr, ptr as usize % 16);
            assert_eq!(fp, tp, "free_pages should equal total after init");

            let page = ArenaAllocator::alloc_pages(alloc_ptr, 1).unwrap();
            assert_eq!(page, 0);
            assert!(ArenaAllocator::is_allocated(alloc_ptr, 0));
            assert_eq!(ArenaAllocator::free_count(alloc_ptr), tp - 1);

            ArenaAllocator::free_pages(alloc_ptr, 0, 1);
            assert!(!ArenaAllocator::is_allocated(alloc_ptr, 0));
            assert_eq!(ArenaAllocator::free_count(alloc_ptr), tp);
        }
    }

    #[test]
    fn alloc_contiguous_pages() {
        let region = AlignedRegion::new(256 * 1024);
        let alloc_ptr = region.as_mut_ptr() as *mut ArenaAllocator;
        unsafe {
            ArenaAllocator::init(region.as_mut_ptr(), 256 * 1024);

            let page = ArenaAllocator::alloc_pages(alloc_ptr, 4).unwrap();
            assert_eq!(page, 0);
            assert!(ArenaAllocator::is_allocated(alloc_ptr, 0));
            assert!(ArenaAllocator::is_allocated(alloc_ptr, 3));
            assert!(!ArenaAllocator::is_allocated(alloc_ptr, 4));

            let page2 = ArenaAllocator::alloc_pages(alloc_ptr, 4).unwrap();
            assert_eq!(page2, 4);

            ArenaAllocator::free_pages(alloc_ptr, 0, 4);
            assert!(!ArenaAllocator::is_allocated(alloc_ptr, 0));
            assert!(ArenaAllocator::is_allocated(alloc_ptr, 4));
        }
    }

    #[test]
    fn first_fit_behavior() {
        let region = AlignedRegion::new(256 * 1024);
        let alloc_ptr = region.as_mut_ptr() as *mut ArenaAllocator;
        unsafe {
            ArenaAllocator::init(region.as_mut_ptr(), 256 * 1024);

            ArenaAllocator::alloc_pages(alloc_ptr, 1); // page 0
            ArenaAllocator::alloc_pages(alloc_ptr, 1); // page 1
            ArenaAllocator::free_pages(alloc_ptr, 0, 1); // free page 0

            let page = ArenaAllocator::alloc_pages(alloc_ptr, 1).unwrap();
            assert_eq!(page, 0); // first fit
        }
    }

    #[test]
    fn out_of_memory() {
        let region = AlignedRegion::new(64 * 1024);
        let alloc_ptr = region.as_mut_ptr() as *mut ArenaAllocator;
        unsafe {
            ArenaAllocator::init(region.as_mut_ptr(), 64 * 1024);

            let total = ArenaAllocator::total_count(alloc_ptr);
            for _ in 0..total {
                assert!(ArenaAllocator::alloc_pages(alloc_ptr, 1).is_some());
            }
            assert_eq!(ArenaAllocator::free_count(alloc_ptr), 0);
            assert!(ArenaAllocator::alloc_pages(alloc_ptr, 1).is_none());
        }
    }

    #[test]
    fn zero_size_alloc_fails() {
        let region = AlignedRegion::new(64 * 1024);
        let alloc_ptr = region.as_mut_ptr() as *mut ArenaAllocator;
        unsafe {
            ArenaAllocator::init(region.as_mut_ptr(), 64 * 1024);
            assert!(ArenaAllocator::alloc_pages(alloc_ptr, 0).is_none());
        }
    }

    #[test]
    fn alloc_too_large_fails() {
        let region = AlignedRegion::new(64 * 1024);
        let alloc_ptr = region.as_mut_ptr() as *mut ArenaAllocator;
        unsafe {
            ArenaAllocator::init(region.as_mut_ptr(), 64 * 1024);
            let total = ArenaAllocator::total_count(alloc_ptr);
            assert!(ArenaAllocator::alloc_pages(alloc_ptr, total + 1).is_none());
        }
    }
}
