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
use crate::kapi::{RealKernel, CAP_CHAN, CAP_MEM};
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
    }
    /* ud2 triggers #UD in ring 3 — the kernel must kill the process. */
    unsafe { core::arch::asm!("ud2"); }
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

/// LTO-proof serial write: uses a `#[used]` static buffer so the compiler
/// cannot eliminate it. Writes the ASCII digit `d` (0-9) to the serial log.
#[cfg(all(feature = "target", target_os = "none"))]
fn serial_digit(d: u8) {
    #[used]
    static mut DIGIT_BUF: [u8; 4] = [b'X', b'\n', 0, 0];
    unsafe {
        DIGIT_BUF[0] = b'0' + d;
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,
            in("rdi") DIGIT_BUF.as_ptr(),
            in("rdx") 2u64,
        );
    }
}

/// LTO-proof hex byte dump of the first 8 bytes at `ptr`.
#[cfg(all(feature = "target", target_os = "none"))]
fn serial_hex8(ptr: *const u8) {
    #[used]
    static mut HEX_BUF: [u8; 23] = *b"xx xx xx xx xx xx xx xx";
    unsafe {
        for i in 0..8u32 {
            let b = core::ptr::read_volatile(ptr.add(i as usize));
            let hi = b >> 4;
            let lo = b & 0xf;
            HEX_BUF[(i * 3) as usize] = if hi < 10 { b'0' + hi } else { b'a' + hi - 10 };
            HEX_BUF[1 + (i * 3) as usize] = if lo < 10 { b'0' + lo } else { b'a' + lo - 10 };
        }
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,
            in("rdi") HEX_BUF.as_ptr(),
            in("rdx") 23u64,
        );
    }
}

#[no_mangle]
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    serial_trace(b"[NET] rust_entry entered\n");
    serial_trace(b"[NET] BIB ptr=");
    serial_hex8(bib_ptr);

    // Dump first 8 raw bytes of BIB header before parsing.
    serial_trace(b"[NET] BIB raw:");
    serial_hex8(bib_ptr);

    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("corrupt boot info");
    serial_trace(b"[NET] BIB parsed\n");

    // Dump n_caps and key fields via LTO-proof writes.
    let n = bib.n_caps;
    serial_trace(b"[NET] n_caps=");
    if n < 10 {
        serial_digit(n as u8);
    } else {
        serial_digit((n / 10) as u8);
        serial_digit((n % 10) as u8);
    }
    serial_trace(b"\n");

    // Dump first cap entry to verify cap table.
    if bib.n_caps > 0 {
        serial_trace(b"[NET] cap0=");
        let c = &bib.caps[0];
        serial_trace(b"name='");
        serial_trace(c.name.as_bytes());
        serial_trace(b"' ty=");
        serial_digit(c.ty as u8);
        serial_trace(b" slot=");
        let s = c.slot;
        serial_digit((s / 100) as u8);
        serial_digit(((s / 10) % 10) as u8);
        serial_digit((s % 10) as u8);
        serial_trace(b"\n");
    }

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
