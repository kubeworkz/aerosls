//! The sidecar entry point over the real kernel ABI (cargo feature
//! `target`). Same bootstrap shape as the network sidecar:
//!
//! 1. crt0 (`src/crt0.S`, placed at address 0 by e1000.ld) hands us the
//!    BootInfo pointer in `rdi`.
//! 2. Parse the BIB; find the budget MEM cap and the DEV cap
//!    (`nic0.bar0`) the kernel minted from the device registry for the
//!    NIC it handed off (grub `nic1=none` → `e1000_driver_handoff`).
//! 3. `SYS_DEV_MMAP` the DEV cap → the BAR0 MMIO window (device MMIO is
//!    reachable ONLY through the mediated map; devtest proved the path).
//! 4. Read the MAC from RAL0/RAH0, then run the bring-up + PHY-loopback
//!    self-test (rings + buffers live in the budget region, which is
//!    identity-mapped at its physical base — the POSIX heap convention —
//!    so `budget.base` is both the CPU access address and the address the
//!    NIC DMA-reads/writes).
//! 5. Serve forever: polled RX drain. Interrupt binding needs the PCI-INTx
//!    arch milestone; frame forwarding to the network sidecar is the next
//!    composition step. The Device Manager owns spawn/respawn policy and
//!    only spawns this sidecar when a NIC was actually handed off, so a
//!    failed bring-up here is a kernel/hardware fault — die loudly
//!    (`[e1000] FAIL`, then #UD so the kernel kills the process) rather
//!    than recover.

use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{CAP_MEM, CAP_TYPE_DEV};
use crate::device::{Dma, Device, RealMmio, run_selftest_retry, DMA_REGION_BYTES};

/// The flat binary links the whole dependency graph, some of which pulls in
/// `alloc` (proto), so a global allocator is required even though this
/// driver never allocates. Minimal bump over the budget region, initialized
/// in `rust_entry` after the budget cap is found; single-threaded sidecar,
/// so plain `static mut` is safe (the network sidecar's identical pattern).
#[cfg(all(feature = "target", target_os = "none"))]
struct Bump {
    base: usize,
    len: usize,
    used: usize,
}

#[cfg(all(feature = "target", target_os = "none"))]
impl Bump {
    const fn new() -> Self {
        Bump {
            base: 0,
            len: 0,
            used: 0,
        }
    }
    unsafe fn init(&mut self, base: usize, len: usize) {
        self.base = base;
        self.len = len;
        self.used = 0;
    }
    unsafe fn alloc(&mut self, size: usize, align: usize) -> *mut u8 {
        let a = align.max(1);
        let start = (self.base + self.used + a - 1) & !(a - 1);
        let end = start.checked_add(size).unwrap_or(usize::MAX);
        if self.base != 0 && end <= self.base + self.len {
            self.used = end - self.base;
            return start as *mut u8;
        }
        core::ptr::null_mut()
    }
}

#[cfg(all(feature = "target", target_os = "none"))]
static mut HEAP: Bump = Bump::new();

#[cfg(all(feature = "target", target_os = "none"))]
struct BumpAlloc;

#[cfg(all(feature = "target", target_os = "none"))]
unsafe impl core::alloc::GlobalAlloc for BumpAlloc {
    #[allow(static_mut_refs)]
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        unsafe { HEAP.alloc(layout.size(), layout.align()) }
    }
    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: core::alloc::Layout) {}
}

#[cfg(all(feature = "target", target_os = "none"))]
#[global_allocator]
static GLOBAL_ALLOC: BumpAlloc = BumpAlloc;

// The driver-SDK syscall shims we link against (proto's `abi` module,
// feature `target`). Declared here — rather than imported — so the entry
// stays explicit about exactly which kernel surface the driver touches
// (same convention as the sidecar applets' extern blocks).
extern "C" {
    // Driver SDK ABI v0.1 §4.1 — map a CAP_TYPE_DEV cap to a user-half
    // MMIO window.
    fn k_dev_mmap(handle: u32, vaddr_hint: u64, flags: u32, out_vaddr: *mut u64) -> i32;
    // SYS_YIELD — the v1 idle loop's pace (no channel to block on yet).
    fn k_yield();
}

#[cfg(all(feature = "target", target_arch = "x86_64", target_os = "none"))]
core::arch::global_asm!(include_str!("crt0.S"), options(att_syntax));

/// Kernel serial log (SYS_SLS_SERIAL_WRITE = 165) — the boot log the
/// `[e1000] PASS/FAIL` gates grep.
#[cfg(all(feature = "target", target_os = "none"))]
const SYS_SERIAL_WRITE: u64 = 165;

/// LTO-proof serial write — the network sidecar's exact pattern: copies
/// into a `#[used]` static buffer so the compiler cannot eliminate or
/// reorder the write, then one raw syscall.
#[cfg(all(feature = "target", target_os = "none"))]
#[allow(static_mut_refs)]
fn serial_write(msg: &[u8]) {
    #[used]
    static mut BUF: [u8; 384] = [0u8; 384];
    let n = msg.len().min(383);
    unsafe {
        core::ptr::copy_nonoverlapping(msg.as_ptr(), BUF.as_mut_ptr(), n);
        BUF[n] = 0;
        core::arch::asm!(
            "syscall",
            inlateout("rax") SYS_SERIAL_WRITE => _,
            inlateout("rdi") BUF.as_ptr() => _,
            inlateout("rdx") (n as u64) => _,
            lateout("rcx") _, lateout("r11") _, lateout("rsi") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
            options(nostack),
        );
    }
}

/// Formatted log: `core::fmt::write` into a stack buffer, flushed with one
/// syscall per line. `#[used]`/fence concerns are `serial_write`'s; the
/// buffer is fully written before the call.
#[cfg(all(feature = "target", target_os = "none"))]
fn klog_fmt(args: core::fmt::Arguments) {
    struct Out<'a> {
        buf: &'a mut [u8],
        pos: usize,
    }
    impl core::fmt::Write for Out<'_> {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            let free = self.buf.len() - self.pos;
            let n = s.len().min(free);
            self.buf[self.pos..self.pos + n].copy_from_slice(&s.as_bytes()[..n]);
            self.pos += n;
            Ok(())
        }
    }
    let mut b = [0u8; 384];
    let mut o = Out { buf: &mut b, pos: 0 };
    let _ = core::fmt::write(&mut o, args);
    let pos = o.pos;
    drop(o);
    serial_write(&b[..pos]);
}

#[cfg(all(feature = "target", target_os = "none"))]
macro_rules! klog {
    ($($t:tt)*) => {
        klog_fmt(core::format_args!($($t)*))
    };
}

/// Panic handler: dump to the kernel serial log, then #UD (ring 3) so the
/// kernel kills the process — the network sidecar's exact pattern.
#[cfg(all(feature = "target", target_os = "none"))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    use core::fmt::Write as _;
    struct PanicBuf {
        buf: [u8; 256],
        pos: usize,
    }
    impl core::fmt::Write for PanicBuf {
        fn write_str(&mut self, s: &str) -> core::fmt::Result {
            let room = &mut self.buf[self.pos..];
            let n = s.len().min(room.len());
            room[..n].copy_from_slice(&s.as_bytes()[..n]);
            self.pos += n;
            Ok(())
        }
    }
    let mut b = PanicBuf { buf: [0u8; 256], pos: 0 };
    let _ = core::write!(&mut b, "[e1000 PANIC] {info}");
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") SYS_SERIAL_WRITE => _,
            inlateout("rdi") b.buf.as_ptr() => _,
            inlateout("rdx") b.pos as u64 => _,
            lateout("rcx") _, lateout("r11") _, lateout("rsi") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
            options(nostack),
        );
    }
    unsafe { core::arch::asm!("ud2"); }
    loop {}
}

/// Fatal: log, then #UD in ring 3 — the kernel must kill the process (the
/// Device Manager's respawn policy owns recovery).
#[cfg(all(feature = "target", target_os = "none"))]
fn die() -> ! {
    klog!("[e1000] FAIL: dying (#UD)\n");
    unsafe { core::arch::asm!("ud2"); }
    loop {}
}

#[no_mangle]
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    klog!("[e1000] rust_entry (drv.e1000.0 / aerosls.e1000.v1)\n");

    let bib = match unsafe { BootInfo::from_raw(bib_ptr) } {
        Ok(b) => b,
        Err(e) => {
            klog!("[e1000] FAIL: BIB parse error: {e:?}\n");
            die();
        }
    };
    klog!("[e1000] BIB: n_caps={}\n", bib.n_caps);

    // ── Budget MEM cap → the DMA region ─────────────────────────────────
    // Sidecar MEM regions are identity-mapped at their physical base (the
    // POSIX heap convention), so `base` is both the CPU access address and
    // the address the NIC's DMA engine reads/writes.
    let Some(budget) = bib.find_cap(CAP_MEM, "budget") else {
        klog!("[e1000] FAIL: no budget MEM cap in BIB\n");
        die();
    };
    if budget.len < DMA_REGION_BYTES {
        klog!(
            "[e1000] FAIL: budget {:#x} < DMA layout need {:#x}\n",
            budget.len,
            DMA_REGION_BYTES
        );
        die();
    }
    klog!(
        "[e1000] budget MEM cap: base={:#x} len={:#x} ({} KiB)\n",
        budget.base,
        budget.len,
        budget.len / 1024
    );
    // Heap over the budget region is only a link-time requirement (see the
    // allocator above); the DMA layout below owns the region's start, so
    // carve the heap from the tail to keep the descriptor area contiguous.
    unsafe {
        let heap_base = (budget.base + DMA_REGION_BYTES).max(budget.base);
        let heap_len = budget.len.saturating_sub(DMA_REGION_BYTES);
        HEAP.init(heap_base as usize, heap_len as usize);
    }

    // ── DEV cap → map BAR0 ──────────────────────────────────────────────
    // Named lookup first (the registry-derived "nic0.bar0" the manifest
    // mints), then any DEV cap as a fallback — mirroring devtest's trial
    // scan semantics.
    let dev_cap = bib
        .find_cap(CAP_TYPE_DEV, "nic0.bar0")
        .or_else(|| bib.caps().iter().find(|c| c.ty == CAP_TYPE_DEV));
    let Some(dev_cap) = dev_cap else {
        klog!("[e1000] no DEV cap in BIB — no NIC handed off (idle)\n");
        idle();
    };
    let mut vaddr: u64 = 0;
    if unsafe { k_dev_mmap(dev_cap.slot, 0, 0, &mut vaddr) } != 0 {
        klog!("[e1000] FAIL: SYS_DEV_MMAP rejected slot {}\n", dev_cap.slot);
        die();
    }
    klog!(
        "[e1000] DEV cap '{}' slot {} → BAR0 window {:#x}\n",
        dev_cap.name,
        dev_cap.slot,
        vaddr
    );

    // ── Bring-up + self-test ────────────────────────────────────────────
    let mmio = unsafe { RealMmio::new(vaddr) };
    let dev = Device::new(mmio);

    let mac = dev.mac();
    let all_zero = mac.iter().all(|&b| b == 0);
    let all_ff = mac.iter().all(|&b| b == 0xff);
    if all_zero || all_ff || mac[0] & 0x01 == 1 {
        klog!("[e1000] FAIL: RAL0/RAH0 did not yield a valid MAC\n");
        die();
    }
    klog!(
        "[e1000] MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );

    let dma = Dma { base: budget.base };
    // QEMU drops RX deliveries for ~1 s after the RCTL (RX-enable) write —
    // its flush_queue_timer grace — so the loopback round trip is retried
    // with a wall-time settle between attempts (`bring_up_rings` runs once;
    // retries never re-enable RX and so never re-arm the window).
    let out = match run_selftest_retry(&dev, &dma, &mac, || qemu_rx_grace_settle(&dev)) {
        Ok(out) => out,
        Err(e) => {
            klog!("[e1000] FAIL: {e}\n");
            die();
        }
    };
    klog!(
        "[e1000] PASS: loopback round-trip {} bytes (MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}, rx slot {})\n",
        out.frame_len,
        out.mac[0], out.mac[1], out.mac[2], out.mac[3], out.mac[4], out.mac[5],
        out.rx_slot
    );

    // ── v1 serve loop: polled RX drain ──────────────────────────────────
    // Loopback is off; no peer traffic reaches a role-less NIC, so the
    // drain is dormant until the network data-plane milestone gives frames
    // somewhere to go. Yield between drains so the driver never spins.
    klog!("[e1000] entering polled RX-drain serve loop\n");
    loop {
        dev.rx_drain(&dma);
        unsafe { k_yield() }
    }
}

#[cfg(all(feature = "target", target_os = "none"))]
fn idle() -> ! {
    loop {
        unsafe { k_yield() }
    }
}

/// Burn wall time between loopback retries so QEMU's 1000 ms post-RCTL
/// RX grace (flush_queue_timer) expires. Every `MMIO_EXIT_PERIOD` spins a
/// STATUS register read forces a vCPU→main-loop exit, so QEMU evaluates
/// and fires the timer against wall time even in single-threaded TCG — a
/// pure guest spin would never let the main loop run.
#[cfg(all(feature = "target", target_os = "none"))]
fn qemu_rx_grace_settle(dev: &Device<RealMmio>) {
    const SPINS: u32 = 400_000_000;
    const MMIO_EXIT_PERIOD: u32 = 1_000_000;
    let mut i = 0u32;
    while i < SPINS {
        if i % MMIO_EXIT_PERIOD == 0 {
            // STATUS read: side-effect-free, forces an MMIO exit.
            let _ = dev.link_up();
        }
        core::hint::spin_loop();
        i += 1;
    }
}
