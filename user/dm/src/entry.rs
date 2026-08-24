//! The Device Manager's entry point over the real kernel ABI (feature
//! `target`). Bootstrap per the Phase 5 design (§1.5, §2):
//!
//! 1. crt0 (crt0.S) hands us the BootInfo pointer in rdi (filled by the
//!    kernel's synthetic ring3_ctx frame, kernel/cap.c step 9).
//! 2. Parse the BIB; resolve the messenger (the two UNNAMED CHAN caps
//!    `cap_create_sidecar` mints first — CHAN_R then CHAN_W), the
//!    console CHAN_W (named "console", wired to the kernel service), and
//!    the budget MEM cap.
//! 3. Serve the registry handshake and the event loop forever (`server`).
//!
//! A spawned DM is a FRESH process: after the messenger closes (init died
//! or moved on), `rust_entry` parks in a spin — the init watchdog's next
//! respawn creates a new DM process that re-runs this whole bootstrap.

use crate::heap::Bump;
use crate::server::DmServer;
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{RealKernel, CAP_CHAN, CAP_CHAN_W, CAP_MEM};

/* The budget MEM cap funds the heap; expose it as the crate's GLOBAL
 * allocator. proto links `alloc` (build_manifest and friends), and `alloc`
 * requires a global allocator in any final binary — the manifest's Budget
 * record (heap_initial 64 KiB over a 256 KiB budget) is exactly the
 * contract for this. Bump: alloc returns null on exhaustion, dealloc is a
 * no-op. Only this target build defines it — the host test harness keeps
 * std's allocator. */
struct BudgetAlloc;

#[global_allocator]
static GLOBAL_ALLOC: BudgetAlloc = BudgetAlloc;

#[allow(static_mut_refs)]
unsafe impl core::alloc::GlobalAlloc for BudgetAlloc {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        // SAFETY: rust_entry initializes HEAP over the budget cap before
        // any allocation runs; the sidecar is single-threaded.
        match unsafe { HEAP.alloc(layout.size(), layout.align()) } {
            Some(p) => p as *mut u8,
            None => core::ptr::null_mut(),
        }
    }

    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: core::alloc::Layout) {
        // Bump allocator: no free.
    }
}

/// Reserved heap over the budget region (single-threaded sidecar).
static mut HEAP: Bump = Bump::new();

/* The crt0 (crt0.S) is assembled by rustc's own LLVM integrated assembler
 * through global_asm — no external cross-GCC — and linked at address 0 by
 * dm.ld (ENTRY(_start)). It saves rdi (the BIB pointer from the kernel's
 * synthetic frame) before switching to the sidecar's own boot stack.
 *
 * `options(att_syntax)` is load-bearing: without it, rustc's global_asm
 * defaults to INTEL syntax when this crate is compiled as a LIBRARY (the
 * rlib the bin links against) but AT&T when linked as a bin — the same
 * file must parse in both builds, so the dialect is pinned here instead
 * of inside crt0.S (which also keeps crt0.S GAS-assemblable).
 *
 * `target_os = "none"` scopes all of this to the freestanding image: a
 * host `cargo check --features target` (the documented way to typecheck
 * the real path) builds the same crate for the host, where a custom
 * #[panic_handler] conflicts with the host's panic=unwind and the crt0
 * has no place. */
#[cfg(all(feature = "target", target_arch = "x86_64", target_os = "none"))]
core::arch::global_asm!(include_str!("crt0.S"), options(att_syntax));

/* ── panic handler (freestanding image only) ────────────────────────────────
 * Best effort: log the panic to the kernel serial log through the legacy
 * SYS_SLS_SERIAL_WRITE syscall (165, NUL-terminated string pointer in rdi
 * — dispatch.c case 165), then park forever. A panic in a panic=abort
 * sidecar must not unwind, and a visible halt beats a silent hang. */
#[cfg(all(feature = "target", target_os = "none"))]
struct PanicBuf([u8; 256]);

#[cfg(all(feature = "target", target_os = "none"))]
impl core::fmt::Write for PanicBuf {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        // At most 255 bytes written; byte 255 stays 0 (NUL terminator).
        let n = s.len().min(255);
        self.0[..n].copy_from_slice(&s.as_bytes()[..n]);
        Ok(()) // truncates silently rather than failing
    }
}

#[cfg(all(feature = "target", target_os = "none"))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    use core::fmt::Write as _;
    let mut b = PanicBuf([0; 256]);
    let _ = core::write!(&mut b, "[PANIC] {info}");
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,  // SYS_SLS_SERIAL_WRITE
            in("rdi") b.0.as_ptr(),
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack),
        );
    }
    loop {
        core::hint::spin_loop();
    }
}

/// A tiny stack-buffer writer for formatted logs (the freestanding binary
/// has NO allocator — `log_fmt!` formats into this fixed buffer).
struct StackBuf<const N: usize> {
    buf: [u8; N],
    len: usize,
}

impl<const N: usize> StackBuf<N> {
    fn new() -> Self {
        Self { buf: [0; N], len: 0 }
    }

    fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }
}

impl<const N: usize> core::fmt::Write for StackBuf<N> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let room = &mut self.buf[self.len..];
        let n = s.len().min(room.len());
        room[..n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(()) // truncates silently rather than failing
    }
}

/// Format a log line into a stack buffer and send it over the console
/// channel (no allocation).
macro_rules! log_fmt {
    ($server:expr, $fmt:literal $(, $arg:expr)* $(,)?) => {{
        use core::fmt::Write as _;
        let mut w = StackBuf::<256>::new();
        let _ = core::write!(&mut w, $fmt $(, $arg)*);
        $server.log(w.as_str());
    }};
}

#[no_mangle]
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    // ── 1. Parse the Boot Info Block ──────────────────────────────────────
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("[DM] corrupt boot info");

    // ── 2. Resolve initial capabilities by name ───────────────────────────
    // The messenger: `cap_create_sidecar` mints the parent↔child channel
    // FIRST, so its two ends are the BIB's two UNNAMED caps (CHAN_R then
    // CHAN_W, kernel/cap.c step 8); the manifest caps (budget, console)
    // follow named. Init sends on its CHAN_W and receives our replies on
    // its CHAN_R — we do the mirror image with `msg_r`/`msg_w`.
    let msg_r = bib
        .find_cap(CAP_CHAN, "")
        .expect("[DM] missing messenger CHAN_R (the unnamed first cap)");
    let msg_w = bib
        .find_cap(CAP_CHAN_W, "")
        .expect("[DM] missing messenger CHAN_W (the unnamed second cap)");

    // A wired channel appears in the BIB as TWO named entries (CHAN_R then
    // CHAN_W); the DM only SENDS to the console, so it needs the W end.
    let console_w = bib
        .find_cap(CAP_CHAN_W, "console")
        .expect("[DM] missing console CHAN_W cap");

    let budget = bib
        .find_cap(CAP_MEM, "budget")
        .expect("[DM] missing 'budget' MEM cap");

    // ── 3. Stand the heap over the budget region (the global allocator
    //        proto's `alloc` usage needs; the manifest declares this).
    unsafe {
        HEAP.init(budget.base as usize, budget.len as usize);
    }

    let server = DmServer::new(RealKernel, console_w.slot, msg_r.slot, msg_w.slot);
    server.log("[DM] ── AeroSLS Device Manager booting ──");
    log_fmt!(&server, "[DM] budget: {} KiB heap", budget.len >> 10);
    server.log("[DM] waiting for the device registry from init...");

    // ── 3. Serve the handshake and the event loop ─────────────────────────
    // run() returns only when the messenger closes (init died or moved on)
    // — a respawned DM is a fresh process, so park here. Console output
    // inside the loop uses blocking sends (timeout 0).
    let mut buf = [0u8; 256];
    match server.run(&mut buf) {
        Ok(()) => server.log("[DM] messenger closed; parking"),
        Err(e) => log_fmt!(&server, "[DM] server error: {e}"),
    }
    loop {
        core::hint::spin_loop();
    }
}
