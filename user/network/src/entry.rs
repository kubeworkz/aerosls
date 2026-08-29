//! The sidecar entry point over the real kernel ABI (cargo feature `target`).
//!
//! Bootstrap:
//! 1. crt0 hands us the BootInfo pointer.
//! 2. Parse the BIB; find budget/network/console caps by name.
//! 3. Init the heap over the budget region (reserved; v1 is allocation-free).
//! 4. Initialize the mock network backend.
//! 5. Adopt every initial CHAN cap except console.
//! 6. Serve forever; boot-time injection is handled by the server loop.

use crate::bootinfo::{BootCap, BootInfo};
use crate::endpoints::EndpointSet;
use crate::heap::Bump;
use crate::kapi::{Kernel, RealKernel, CAP_CHAN, CAP_MEM};
use crate::mock::MockNetwork;
use crate::server;

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
    let _ = core::write!(&mut b, "[NET PANIC] {info}");
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,
            in("rdi") b.buf.as_ptr(),
            in("rdx") b.pos as u64,
        );
        core::arch::asm!("cli", "hlt");
    }
    loop {}
}

#[cfg(all(feature = "target", target_os = "none"))]
fn serial_trace(msg: &[u8]) {
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,
            in("rdi") msg.as_ptr(),
            in("rdx") msg.len() as u64,
        );
    }
}

#[no_mangle]
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    serial_trace(b"[NET] rust_entry entered\n");
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("corrupt boot info");
    serial_trace(b"[NET] BIB parsed\n");
    let k = RealKernel;

    // Budget → heap.
    let budget = bib.find_cap(CAP_MEM, "budget").expect("no budget cap");
    serial_trace(b"[NET] budget cap found\n");
    unsafe {
        HEAP.init(budget.base as usize, budget.len as usize);
    }
    serial_trace(b"[NET] heap initialized\n");

    // Console + initial endpoints.
    let console = bib.find_cap(CAP_CHAN, "console").expect("no console cap");
    serial_trace(b"[NET] console cap found\n");
    let mut eps = EndpointSet::new(console.slot);
    serial_trace(b"[NET] EndpointSet created\n");
    let mut net = MockNetwork::new();
    serial_trace(b"[NET] MockNetwork created\n");
    for c in bib.caps() {
        adopt_initial(console.slot, c, &mut eps);
    }
    serial_trace(b"[NET] entering server loop\n");

    loop {
        let _ = server::run(&k, &mut eps, &mut net);
    }
}

fn adopt_initial(console_slot: u32, c: &BootCap<'_>, eps: &mut EndpointSet) {
    if c.ty == CAP_CHAN && c.slot != console_slot {
        eps.adopt(c.slot);
    }
}
