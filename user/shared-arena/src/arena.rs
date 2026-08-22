//! High-level arena API for cross-sidecar shared memory allocation.
//!
//! `Arena` ties together the page allocator and object headers to provide
//! a safe(ish) interface for allocating, sharing, and freeing objects in
//! the shared arena.

use core::sync::atomic::{compiler_fence, Ordering};

use crate::allocator::{ArenaAllocator, PAGE_SIZE};
use crate::header::{ArenaHeader, FLAG_BINARY, FLAG_STRING, HEADER_SIZE};

/// Error codes for arena operations.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArenaError {
    /// Arena is not initialized.
    NotInitialized,
    /// Not enough memory for the requested allocation.
    OutOfMemory,
    /// Invalid pointer (not within the arena region).
    InvalidPointer,
}

impl core::fmt::Display for ArenaError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            ArenaError::NotInitialized => write!(f, "arena not initialized"),
            ArenaError::OutOfMemory => write!(f, "out of memory"),
            ArenaError::InvalidPointer => write!(f, "invalid arena pointer"),
        }
    }
}

/// High-level arena handle.
///
/// Wraps the raw allocator and provides object-level allocation with
/// reference counting. Designed for shared memory: all allocator state
/// lives at the base address, and operations use raw pointers.
pub struct Arena {
    base: *mut u8,
    region_size: usize,
    data_start: usize,
    initialized: bool,
}

unsafe impl Send for Arena {}
unsafe impl Sync for Arena {}

impl Arena {
    pub const fn new() -> Self {
        Arena {
            base: core::ptr::null_mut(),
            region_size: 0,
            data_start: 0,
            initialized: false,
        }
    }

    /// Initialize the arena over a memory region.
    ///
    /// # Safety
    ///
    /// `base` must point to a valid, writable, aligned memory region
    /// accessible to all sidecars that will share this arena.
    pub unsafe fn init(&mut self, base: *mut u8, region_size: usize) -> Result<(), ArenaError> {
        if base.is_null() || region_size < PAGE_SIZE * 2 {
            return Err(ArenaError::InvalidPointer);
        }

        self.base = base;
        self.region_size = region_size;
        self.data_start = ArenaAllocator::init(base, region_size);
        self.initialized = true;

        Ok(())
    }

    /// Attach to an already-initialized arena.
    ///
    /// # Safety
    ///
    /// `base` must point to an arena previously initialized via `init()`.
    pub unsafe fn attach(&mut self, base: *mut u8, region_size: usize) -> Result<(), ArenaError> {
        if base.is_null() || region_size < PAGE_SIZE * 2 {
            return Err(ArenaError::InvalidPointer);
        }

        let alloc = &*(base as *const ArenaAllocator);
        if alloc.total_pages == 0 {
            return Err(ArenaError::NotInitialized);
        }

        self.base = base;
        self.region_size = region_size;

        let bitmap_size = core::mem::size_of_val(&alloc.bitmap);
        self.data_start =
            (ArenaAllocator::STRUCT_SIZE + bitmap_size + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
        self.initialized = true;

        Ok(())
    }

    /// Allocate an object in the arena.
    ///
    /// Returns `(data_ptr, total_size)` where `data_ptr` points to the
    /// user-visible data (past the header).
    pub fn alloc(
        &self,
        size: u32,
        owner_sidecar: u32,
        flags: u32,
    ) -> Result<(*mut u8, u32), ArenaError> {
        if !self.initialized {
            return Err(ArenaError::NotInitialized);
        }

        let alloc = self.base as *mut ArenaAllocator;
        let total_bytes = ArenaHeader::total_size(size);
        let pages_needed = ((total_bytes + PAGE_SIZE - 1) / PAGE_SIZE) as u32;

        let page_index =
            unsafe { ArenaAllocator::alloc_pages(alloc, pages_needed) }.ok_or(ArenaError::OutOfMemory)?;

        let data_offset = self.data_start + (page_index as usize) * PAGE_SIZE + HEADER_SIZE;
        let data_ptr = unsafe { self.base.add(data_offset) };

        // Write the header (ensure alignment)
        let header = ArenaHeader::new(size, owner_sidecar, flags);
        unsafe {
            // data_ptr - HEADER_SIZE bytes = start of the header
            let hdr_ptr = data_ptr.sub(HEADER_SIZE) as *mut ArenaHeader;
            core::ptr::write_volatile(hdr_ptr, header);
        }

        compiler_fence(Ordering::Release);

        Ok((data_ptr, total_bytes as u32))
    }

    /// Allocate a string object (UTF-8) in the arena.
    pub fn alloc_string(
        &self,
        data: &[u8],
        owner_sidecar: u32,
    ) -> Result<(*mut u8, u32), ArenaError> {
        let (ptr, _) = self.alloc(data.len() as u32, owner_sidecar, FLAG_STRING)?;
        unsafe {
            core::ptr::copy_nonoverlapping(data.as_ptr(), ptr, data.len());
        }
        Ok((ptr, data.len() as u32))
    }

    /// Allocate a binary object in the arena.
    pub fn alloc_binary(
        &self,
        data: &[u8],
        owner_sidecar: u32,
    ) -> Result<(*mut u8, u32), ArenaError> {
        let (ptr, _) = self.alloc(data.len() as u32, owner_sidecar, FLAG_BINARY)?;
        unsafe {
            core::ptr::copy_nonoverlapping(data.as_ptr(), ptr, data.len());
        }
        Ok((ptr, data.len() as u32))
    }

    /// Increment the refcount of an arena object.
    ///
    /// # Safety
    ///
    /// `data_ptr` must point to valid user data of an arena object.
    pub unsafe fn inc_ref(&self, data_ptr: *const u8) -> u32 {
        let header = ArenaHeader::from_data_ptr(data_ptr) as *const ArenaHeader;
        (*header).inc_ref()
    }

    /// Decrement the refcount and free the object if it reaches 0.
    ///
    /// # Safety
    ///
    /// `data_ptr` must point to valid user data of an arena object.
    /// Must be called exactly once per reference held.
    pub unsafe fn free(&self, data_ptr: *mut u8) -> Result<(), ArenaError> {
        if !self.initialized {
            return Err(ArenaError::NotInitialized);
        }

        let header = ArenaHeader::from_data_ptr(data_ptr as *const u8) as *const ArenaHeader;

        let data_addr = data_ptr as usize;
        let arena_start = self.base as usize;
        let arena_end = arena_start + self.region_size;
        if data_addr < arena_start + self.data_start || data_addr >= arena_end {
            return Err(ArenaError::InvalidPointer);
        }

        let new_ref = (*header).dec_ref();

        if new_ref == 0 {
            self.reclaim(header as *mut ArenaHeader);
        }

        Ok(())
    }

    /// Reclaim pages for a zero-refcount object.
    unsafe fn reclaim(&self, header: *mut ArenaHeader) {
        let alloc = self.base as *mut ArenaAllocator;

        let obj_start = header as usize;
        let obj_size = ArenaHeader::total_size((*header).size()) as usize;
        let first_byte = obj_start - self.base as usize;
        let page_index = (first_byte / PAGE_SIZE) as u32;
        let page_count = ((first_byte % PAGE_SIZE + obj_size + PAGE_SIZE - 1) / PAGE_SIZE) as u32;

        ArenaAllocator::free_pages(alloc, page_index, page_count);
    }

    /// Get the header for an arena object from its data pointer.
    pub unsafe fn header(&self, data_ptr: *const u8) -> &ArenaHeader {
        &*(ArenaHeader::from_data_ptr(data_ptr))
    }

    pub fn is_initialized(&self) -> bool {
        self.initialized
    }

    pub fn base(&self) -> *mut u8 {
        self.base
    }

    pub fn region_size(&self) -> usize {
        self.region_size
    }
}

impl Default for Arena {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use crate::allocator::AlignedRegion;

    use super::*;

    #[test]
    fn arena_init_and_alloc() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        assert!(arena.is_initialized());

        let (ptr, total_size) = arena.alloc(64, 1, FLAG_STRING).unwrap();
        assert!(!ptr.is_null());
        assert!(total_size >= 64 + HEADER_SIZE as u32);

        unsafe {
            for i in 0..64 {
                *ptr.add(i) = i as u8;
            }
        }

        unsafe {
            let header = arena.header(ptr as *const u8);
            assert_eq!(header.refcount(), 1);
            assert_eq!(header.size(), 64);
            assert_eq!(header.owner(), 1);
            assert!(header.is_string());
        }
    }

    #[test]
    fn arena_alloc_string() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        let hello = b"Hello, shared arena!";
        let (ptr, len) = arena.alloc_string(hello, 1).unwrap();
        assert_eq!(len, hello.len() as u32);

        unsafe {
            let slice = core::slice::from_raw_parts(ptr, hello.len());
            assert_eq!(slice, hello);
        }
    }

    #[test]
    fn arena_refcount_transfer() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        let (ptr, _) = arena.alloc(32, 1, 0).unwrap();

        // Simulate @arena transfer
        unsafe {
            let new_ref = arena.inc_ref(ptr as *const u8);
            assert_eq!(new_ref, 2);
        }

        // First sidecar frees
        unsafe {
            arena.free(ptr).unwrap();
        }

        // Refcount should be 1
        unsafe {
            let header = arena.header(ptr as *const u8);
            assert_eq!(header.refcount(), 1);
        }

        // Second sidecar frees — reclaims
        unsafe {
            arena.free(ptr).unwrap();
        }
    }

    #[test]
    fn arena_multiple_allocations() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        let mut ptrs = alloc::vec::Vec::new();
        for i in 0..10 {
            let size = 32 + i * 16;
            let (ptr, _) = arena.alloc(size as u32, 1, 0).unwrap();
            ptrs.push((ptr, size));
        }

        for i in 0..ptrs.len() {
            for j in (i + 1)..ptrs.len() {
                assert_ne!(ptrs[i].0 as usize, ptrs[j].0 as usize);
            }
        }

        for (ptr, _) in ptrs {
            unsafe {
                arena.free(ptr).unwrap();
            }
        }
    }

    #[test]
    fn arena_page_reclamation() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        let alloc = region.as_mut_ptr() as *mut ArenaAllocator;
        let initial_free = unsafe { ArenaAllocator::free_count(alloc) };

        let (ptr, _) = arena.alloc(8192, 1, FLAG_BINARY).unwrap();
        assert!(unsafe { ArenaAllocator::free_count(alloc) } < initial_free);

        unsafe {
            arena.free(ptr).unwrap();
        }
        assert_eq!(unsafe { ArenaAllocator::free_count(alloc) }, initial_free);
    }

    #[test]
    fn arena_ownership_modes() {
        let region = AlignedRegion::new(256 * 1024);
        let mut arena = Arena::new();
        unsafe {
            arena.init(region.as_mut_ptr(), 256 * 1024).unwrap();
        }

        // @borrowed: refcount stays at 1
        let (ptr_borrowed, _) = arena.alloc(128, 1, FLAG_STRING).unwrap();
        unsafe {
            let h = arena.header(ptr_borrowed as *const u8);
            assert_eq!(h.refcount(), 1);
        }

        // @owned: cap moved, refcount stays at 1
        let (ptr_owned, _) = arena.alloc(256, 1, FLAG_BINARY).unwrap();
        unsafe {
            let h = arena.header(ptr_owned as *const u8);
            assert_eq!(h.refcount(), 1);
            arena.free(ptr_owned).unwrap();
        }

        // @arena: refcount managed by transfer
        let (ptr_arena, _) = arena.alloc(512, 1, 0).unwrap();
        unsafe {
            let new_ref = arena.inc_ref(ptr_arena as *const u8);
            assert_eq!(new_ref, 2);

            arena.free(ptr_arena).unwrap();
            let h = arena.header(ptr_arena as *const u8);
            assert_eq!(h.refcount(), 1);

            arena.free(ptr_arena).unwrap();
        }
    }

    #[test]
    fn arena_not_initialized() {
        let arena = Arena::new();
        assert_eq!(arena.alloc(64, 1, 0), Err(ArenaError::NotInitialized));
    }
}
