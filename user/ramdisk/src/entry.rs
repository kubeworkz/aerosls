//! The sidecar entry point over the real kernel ABI (cargo feature
//! `target`). Bootstrap per implementation plan §3:
//!
//! 1. crt0 (`src/crt0.S`) hands us the BootInfo pointer.
//! 2. Parse the BIB; find budget/storage/console caps by name.
//! 3. Init the heap over the budget region (reserved; v1 is allocation-free).
//! 4. Validate storage geometry (raw blocks — no filesystem knowledge).
//! 5. Adopt every initial CHAN cap except console (respawn control channel).
//! 6. Serve forever; boot-time injection (NEW_CHANNEL) is handled by the
//!    server loop.

use crate::bootinfo::{BootCap, BootInfo};
use crate::endpoints::EndpointSet;
use crate::heap::Bump;
use crate::kapi::{Kernel, RealKernel, CAP_CHAN, CAP_MEM};
use crate::server::{self, Device};
use aerosls_proto::{BLOCK_SIZE, W};

/// Reserved heap (single-threaded sidecar; no atomics needed). Access is
/// confined to `rust_entry`.
static mut HEAP: Bump = Bump::new();

/// Global allocator: bump over the budget region.
#[cfg(all(feature = "target", target_os = "none"))]
struct HeapAlloc;

#[cfg(all(feature = "target", target_os = "none"))]
unsafe impl core::alloc::GlobalAlloc for HeapAlloc {
    #[allow(static_mut_refs)]
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        match unsafe { HEAP.alloc(layout.size(), layout.align()) } {
            Some(p) => p as *mut u8,
            None => core::ptr::null_mut(),
        }
    }
    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: core::alloc::Layout) {}
}

#[cfg(all(feature = "target", target_os = "none"))]
#[global_allocator]
static GLOBAL_ALLOC: HeapAlloc = HeapAlloc;

/* The crt0 (crt0.S) is assembled by rustc's LLVM integrated assembler
 * through global_asm — no external cross-GCC — and linked at address 0
 * by ramdisk.ld (ENTRY(_start)). It saves rdi (the BIB pointer) and
 * switches to the sidecar's own boot stack before calling rust_entry. */
#[cfg(all(feature = "target", target_arch = "x86_64", target_os = "none"))]
core::arch::global_asm!(include_str!("crt0.S"), options(att_syntax));

/// Panic handler: write to kernel serial log (SYS_SLS_SERIAL_WRITE = 165).
#[cfg(all(feature = "target", target_os = "none"))]
struct PanicBuf { buf: [u8; 256], pos: usize }

#[cfg(all(feature = "target", target_os = "none"))]
impl core::fmt::Write for PanicBuf {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let room = &mut self.buf[self.pos..];
        let n = s.len().min(room.len());
        room[..n].copy_from_slice(&s.as_bytes()[..n]);
        self.pos += n;
        Ok(())
    }
}

#[cfg(all(feature = "target", target_os = "none"))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    use core::fmt::Write as _;
    let mut b = PanicBuf { buf: [0u8; 256], pos: 0 };
    let _ = core::write!(&mut b, "[RAMDISK PANIC] {info}");
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,
            in("rdi") b.buf.as_ptr(),
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack),
        );
    }
    loop { core::hint::spin_loop(); }
}

#[no_mangle]
// The `static mut` heap is deliberate on a bare-metal single-threaded
// target; the lint fires at this use site.
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("corrupt boot info");
    let k = RealKernel;

    // Budget → heap (reserved for future use).
    let budget = bib.find_cap(CAP_MEM, "budget").expect("no budget cap");
    unsafe {
        HEAP.init(budget.base as usize, budget.len as usize);
    }

    // Storage: validate geometry against the cap, then resolve base/len.
    let storage = bib.find_cap(CAP_MEM, "storage").expect("no storage cap");
    assert!(
        storage.len >= BLOCK_SIZE as u64 && storage.len % BLOCK_SIZE as u64 == 0,
        "storage not block-aligned"
    );
    let storage_info = k
        .cap_info(storage.slot)
        .expect("storage cap_info failed");
    let dev = Device {
        storage_slot: storage.slot,
        storage_base: storage_info.base,
        storage_len: storage_info.len,
        storage_writable: storage_info.rights & W as u16 != 0,
    };

    // Console + initial endpoints (respawn control channel).
    let console = bib.find_cap(CAP_CHAN, "console").expect("no console cap");
    let mut eps = EndpointSet::new(console.slot);
    for c in bib.caps() {
        adopt_initial(console.slot, c, &mut eps);
    }

    // Scan cap table slots for wired CHAN endpoints that aren't in the BIB.
    // The POSIX sidecar's manifest declares a "ramdisk" CHAN wired to us;
    // the kernel places those endpoints in our cap table but they aren't in
    // our BIB (the BIB was built before POSIX was created). Scan all
    // reasonable slots to adopt them.
    for slot in 0u32..16 {
        if slot == console.slot { continue; }
        if let Ok(info) = k.cap_info(slot) {
            if info.ty == CAP_CHAN {
                eps.adopt(slot);
            }
        }
    }

    loop {
        // Only returns on a fatal kernel error; a healthy kernel never does.
        let _ = server::run(&k, &mut eps, &dev);
    }
}

fn adopt_initial(console_slot: u32, c: &BootCap<'_>, eps: &mut EndpointSet) {
    if c.ty == CAP_CHAN && c.slot != console_slot {
        eps.adopt(c.slot);
    }
}
