//! The init sidecar entry point over the real kernel ABI (feature `target`).
//!
//! Bootstrap sequence (Phase 5 §1.5):
//!
//! 1. crt0 hands us the BootInfo pointer (a0/rdi).
//! 2. Parse the BIB; resolve budget, console, device_registry, spawn caps.
//! 3. Init the heap over the budget region.
//! 4. Read the device registry from the kernel-populated MEM cap.
//! 5. Spawn the Device Manager via create_sidecar; receive the messenger.
//! 6. Send the device registry snapshot to the Device Manager.
//! 7. Wait for "devices ready" signal.
//! 8. Spawn the POSIX sidecar (channels wired by the Device Manager).
//! 9. Park in the scheduler event loop.

use crate::chan::{ChannelError, InitChannel, MSG_DEVICE_REGISTRY};
use crate::demo::{self, DM_READY_DEADLINE_NS, DM_READY_RETRIES};
use crate::devreg::DeviceRegistry;
use crate::dm_manifest::{self, DmImage};
use crate::heap::Bump;
use crate::posix_manifest;
use crate::ramdisk_manifest;
use crate::net_manifest;
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{Kernel, RealKernel, SendCap, CAP_CHAN_W, CAP_MEM, CAP_NONE};

extern "C" {
    fn k_yield();
}

/* The crt0 (crt0.S) is assembled by rustc's own LLVM integrated assembler
 * through global_asm — no external cross-GCC — and linked at address 0 by
 * init.ld (ENTRY(_start)). It saves rdi (the BIB pointer from the kernel's
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

/// Reserved heap over the budget region (single-threaded sidecar).
static mut HEAP: Bump = Bump::new();

/* ── panic handler (freestanding image only) ────────────────────────────────
 * rust_entry and the demo loop panic on unrecoverable contract failures
 * (`.expect()` on missing caps, `create_sidecar` failure, ...). The host
 * test harness keeps std's handler; the sidecar image needs its own, and
 * without one the target build does not link. Best effort: log the panic
 * to the kernel serial log through the legacy SYS_SLS_SERIAL_WRITE syscall
 * (165, NUL-terminated string pointer in rdi — dispatch.c case 165), then
 * park forever. A panic in a panic=abort sidecar must not unwind, and a
 * visible halt beats a silent hang: the watchdog/console path can see the
 * [PANIC] line even though this sidecar is done.
 *
 * `target_os = "none"`: the host test harness keeps std's handler and the
 * host build links with panic=unwind — a custom handler is only valid in
 * the freestanding image. */
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
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    use core::fmt::Write as _;
    let mut b = PanicBuf { buf: [0u8; 256], pos: 0 };
    let _ = core::write!(&mut b, "[PANIC] {info}");
    unsafe {
        core::arch::asm!(
            "syscall",
            inlateout("rax") 165u64 => _,  // SYS_SLS_SERIAL_WRITE
            in("rdi") b.buf.as_ptr(),
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack),
        );
    }
    loop {
        core::hint::spin_loop();
    }
}

/* The budget MEM cap funds the heap; expose it as the crate's GLOBAL
 * allocator so the sidecar can use `alloc` (build_manifest in
 * dm_manifest.rs). Only this target build defines it — the host test
 * harness keeps std's allocator. Bump: alloc returns null on exhaustion,
 * dealloc is a no-op (the heap is never reclaimed). */
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

/// A tiny stack-buffer writer for formatted logs. The freestanding binary
/// has NO global allocator (the bump heap is used explicitly), so logging
/// must not allocate — `log_fmt!` formats into this fixed buffer.
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
/// channel (no allocation; the bump heap is not a global allocator).
macro_rules! log_fmt {
    ($console:expr, $fmt:literal $(, $arg:expr)* $(,)?) => {{
        use core::fmt::Write as _;
        let mut w = StackBuf::<256>::new();
        let _ = core::write!(&mut w, $fmt $(, $arg)*);
        log($console, w.as_str());
    }};
}

#[no_mangle]
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    // ── 1. Parse the Boot Info Block ──────────────────────────────────────
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }
        .expect("[INIT] corrupt boot info");

    // ── 2. Resolve initial capabilities by name ───────────────────────────
    //    These names match the init sidecar's manifest (Phase 5 §1.4).
    let budget_cap = bib
        .find_cap(CAP_MEM, "budget")
        .expect("[INIT] missing 'budget' MEM cap");

    // A wired channel appears in the BIB as TWO named entries (CHAN_R then
    // CHAN_W, kernel/cap.c). Init only SENDS to the console, so the W end
    // is the one it needs — `find_cap(CAP_CHAN, ...)` would return the
    // (unused) R end.
    let console_w = bib
        .find_cap(CAP_CHAN_W, "console")
        .expect("[INIT] missing console CHAN_W cap");

    let devreg_cap = bib
        .find_cap(CAP_MEM, "device_registry")
        .expect("[INIT] missing 'device_registry' MEM cap");

    // The boot image (Phase 5 §7) places the Device Manager binary in
    // memory and grants init a MEM cap to it; k_create_sidecar copies the
    // image from that physical address.
    let dm_image_cap = bib
        .find_cap(CAP_MEM, "dm.image")
        .expect("[INIT] missing 'dm.image' MEM cap (boot image must place the DM binary)");

    // ── 3. Init the heap ──────────────────────────────────────────────────
    unsafe {
        HEAP.init(
            budget_cap.base as usize,
            budget_cap.len as usize,
        );
    }

    // ── 4. Read the device registry ───────────────────────────────────────
    let devreg = unsafe {
        DeviceRegistry::from_raw_parts(
            devreg_cap.base as *const u8,
            devreg_cap.len as usize,
        )
    }
    .expect("[INIT] bad device registry");

    // Log discovered devices to the console channel (send-only: r = CAP_NONE).
    let console = InitChannel::new(RealKernel, CAP_NONE as u32, console_w.slot);
    log(&console, "[INIT] ── AeroSLS init sidecar booting ──");
    log_fmt!(&console, "[INIT] budget: {} MiB", budget_cap.len >> 20);
    log_fmt!(&console, "[INIT] found {} PCI device(s)", devreg.len());
    for (i, e) in devreg.iter().enumerate() {
        let name = e.manifest_name().unwrap_or("?");
        log_fmt!(
            &console,
            "[INIT]   [{}] {} class={:02x}:{:02x} vendor={:04x} dev={:04x} bar=0x{:08x}",
            i,
            name,
            e.class_code,
            e.subclass,
            e.vendor_id,
            e.device_id,
            e.bar0_phys,
        );
    }

    // ── 5. Spawn the Device Manager (SYS_SLS_CREATE_SIDECAR) ──────────────
    log(&console, "[INIT] spawning Device Manager...");

    // Real kernel path: build the DM's packed manifest (records + the
    // image_kaddr footer pointing at the boot-loaded binary), hand it to
    // `create_sidecar`, and get back the parent end of the messenger
    // channel — CHAN_R (receive the DM's replies) and CHAN_W (send to
    // the DM). The kernel creates the process, maps the image, builds the
    // child's initial table from the manifest's caps, and returns both
    // ends (kernel/cap.c sys_sls_create_sidecar).
    let dm_image = DmImage {
        kaddr: dm_image_cap.base,
        size: dm_image_cap.len as u32,
        entry: 0, // flat binary: entry at offset 0
    };
    let dm_channel = spawn_device_manager(&console, dm_image);

    // Yield to give the DM time to start and park on its messenger channel
    // before we send the registry. Without this, init may send before the
    // DM enters k_chan_wait, and cap_wake_chan finds no parked process — the
    // message sits in the queue but the DM's first-poll might race.
    unsafe { k_yield(); }

    // ── 6. Send the device registry to the Device Manager ─────────────────
    log(&console, "[INIT] sending device registry to Device Manager...");

    // We send the device registry as a payload with the devreg MEM cap
    // attached.  The Device Manager receives both the data and a read-only
    // view of the registry memory.
    let devreg_send_cap = SendCap {
        slot: devreg_cap.slot,
        offset: 0,
        len: devreg_cap.len as u32,
        rights: 0x01, // R only
        flags: 0x00,  // transient (default)
    };

    // Payload: count of devices (u32 LE).
    let mut payload = [0u8; 4];
    let count = devreg.len() as u32;
    payload[..4].copy_from_slice(&count.to_le_bytes());

    // Blocking send (timeout 0): if the DM's queue is full, the kernel
    // parks this sidecar until a slot frees — the registry cannot be
    // dropped (transport spec §3.4).
    demo::send_registry(&dm_channel, MSG_DEVICE_REGISTRY, &payload, &devreg_send_cap)
        .unwrap_or_else(|e| panic!("[INIT] failed to send device registry: {e}"));

    // ── 7. Wait for "devices ready" (finite deadline + bounded retries) ──
    // The reply wait parks WITH an absolute deadline (1 s): if the DM is
    // slow, the kernel wakes the parked wait at the deadline and the re-run
    // returns CAP_ERR_TIMEOUT — a wedged DM costs at most
    // DM_READY_RETRIES × DM_READY_DEADLINE_NS, never an unbounded block.
    // On final failure init parks in the event loop anyway (the DM can
    // still signal readiness later via a notification).
    log(&console, "[INIT] waiting for Device Manager to initialise devices...");

    let mut reply_buf = [0u8; 256];
    let mut devices_ready = false;
    for attempt in 1..=DM_READY_RETRIES {
        match demo::wait_devices_ready(&dm_channel, &mut reply_buf, DM_READY_DEADLINE_NS) {
            Ok(()) => {
                devices_ready = true;
                break;
            }
            Err(ChannelError::Timeout) => {
                // The deadline elapsed (kernel woke the park with
                // CAP_ERR_TIMEOUT); yield to let the DM run, then retry.
                log_fmt!(&console, "[INIT] DM not ready yet (attempt {}/3)", attempt);
                unsafe { k_yield(); }
            }
            Err(e) => {
                log_fmt!(&console, "[INIT] DM handshake failed: {}", e);
                break;
            }
        }
    }
    if devices_ready {
        log(&console, "[INIT] all devices ready.");
    } else {
        log(&console, "[INIT] DM did not signal ready in time; parking in the event loop");
    }

    // ── 8. Spawn the ramdisk driver ───────────────────────────────────────
    log(&console, "[INIT] spawning ramdisk driver...");

    let ramdisk_image_cap = bib
        .find_cap(CAP_MEM, "ramdisk.image")
        .expect("[INIT] missing 'ramdisk.image' MEM cap");

    // ramdisk.heap is NOT in init's manifest (avoids cap_create_mem overlap).
    // Compute the budget and storage addresses from the layout: budget sits
    // page-aligned after the ramdisk image; storage sits after the budget.
    let ramdisk_budget_base = (ramdisk_image_cap.base + ramdisk_image_cap.len + 4095) & !4095u64;
    let ramdisk_budget_size = 256 * 1024; // 256 KiB — matches RAMDISK_HEAP_BYTES
    let storage_base = (ramdisk_budget_base + ramdisk_budget_size + 4095) & !4095u64;
    let storage_len = 16 * 1024 * 1024; // 16 MiB — matches STORAGE_BYTES in layout.rs (cap word 12-bit limit)
    log_fmt!(&console, "[INIT]   ramdisk budget @ 0x{:x}, storage @ 0x{:x} ({} MiB)",
             ramdisk_budget_base, storage_base, storage_len >> 20);

    spawn_ramdisk_driver(
        &console,
        ramdisk_image_cap,
        ramdisk_budget_base,
        ramdisk_budget_size,
        storage_base,
        storage_len,
    );
    // Yield to let the ramdisk sidecar start and park.
    unsafe { k_yield(); }

    // ── 9. Spawn the network driver FIRST ────────────────────────────────
    // The network driver must be registered in the kernel's sidecar
    // registry before the POSIX sidecar, because the POSIX manifest
    // declares a 'network' CHAN cap wired to 'drv.network.0'.
    log(&console, "[INIT] spawning network driver...");

    let net_image_cap = bib
        .find_cap(CAP_MEM, "net.image")
        .expect("[INIT] missing 'net.image' MEM cap");

    // net.heap is NOT in init's manifest (avoids MEM overlap with
    // the network sidecar's own budget cap).  Compute its base from the
    // image cap: heap sits immediately after the image, page-aligned.
    let net_heap_base = (net_image_cap.base + net_image_cap.len + 4095) & !4095u64;
    log_fmt!(&console, "[INIT]   net.heap computed @ 0x{:x}", net_heap_base);

    spawn_network_driver(&console, net_image_cap, net_heap_base);
    // Yield to let the network sidecar start and park.
    for _ in 0..3 {
        unsafe { k_yield(); }
    }

    // ── 10. Spawn the POSIX sidecar ───────────────────────────────────────
    log(&console, "[INIT] spawning POSIX sidecar...");

    let posix_image_cap = bib
        .find_cap(CAP_MEM, "posix.image")
        .expect("[INIT] missing 'posix.image' MEM cap");

    // posix.heap is NOT in init's manifest (avoids MEM overlap with
    // the POSIX sidecar's own budget cap).  Compute its base from the
    // image cap: heap sits immediately after the image, page-aligned.
    let posix_heap_base = (posix_image_cap.base + posix_image_cap.len + 4095) & !4095u64;
    log_fmt!(&console, "[INIT]   posix.heap computed @ 0x{:x}", posix_heap_base);

    // The POSIX sidecar's manifest declares budget + console + ramdisk + network caps.
    // The network CHAN cap is wired by the kernel to drv.network.0 (must be registered first).
    // The ramdisk CHAN cap is wired to drv.ramdisk.0. We KEEP the
    // messenger channel: its CLOSE event (on process death) is what the
    // watchdog below parks on to respawn a crashed POSIX sidecar.
    //
    // Driver SDK v0.1 s4.1 — mint a CAP_TYPE_DEV cap for the e1000's MMIO
    // BAR0 from the device registry (the kernel's PCI scan recorded
    // bar0_phys; no hardcoded address). devtest in the POSIX sidecar
    // maps it via SYS_DEV_MMAP and reads the device registers — the
    // on-target proof that CAP_TYPE_DEV works. Boots without an e1000
    // (no `-device e1000`) simply omit the cap and devtest skips.
    let e1000_bar0 = devreg.find(0x02, 0x00).map(|e| e.bar0_phys);
    match e1000_bar0 {
        Some(base) => log_fmt!(
            &console,
            "[INIT] e1000 BAR0 @ 0x{:x} → POSIX manifest DEV cap 'nic0.bar0'",
            base
        ),
        None => log(&console, "[INIT] no e1000 in registry — POSIX manifest omits the DEV cap"),
    }
    let posix_channel = spawn_posix_sidecar(
        &console,
        posix_image_cap.base,
        posix_image_cap.len as u32,
        posix_heap_base,
        e1000_bar0,
    );
    // Yield multiple times to give the ramdisk sidecar time to
    // re-scan its cap table, discover the POSIX ramdisk channel,
    // and be ready to handle RD_INFO before POSIX sends it.
    for _ in 0..3 {
        unsafe { k_yield(); }
    }

    log(&console, "[INIT] ── Phase 5 init sidecar complete ──");
    log(&console, "[INIT] system ready for POSIX sidecar creation.");

    // ── 10. The demo server loop (watchdog) ───────────────────────────────
    // Blocking k_chan_wait (TIMEOUT_NONE) on the Device Manager channel:
    // this sidecar parks (cap_wait_chans) until the DM queues a message or
    // a control event, and a wake re-runs the wait to return it. Console
    // output inside the loop uses blocking sends (timeout 0).
    //
    // Self-healing: when the DM channel closes (CLOSE_PEER_DEAD from the
    // teardown scan, or an explicit close), the watchdog sleeps a bounded
    // backoff (the deadline-wait-as-sleep trick: park on the dead channel
    // with a finite deadline, the timer ISR wakes us, the re-run returns
    // ERR_TIMEOUT), respawns a fresh DM, re-runs the registry handshake,
    // and re-enters the loop on the new channel. The restart budget is
    // bounded — a crash-looping DM makes the loop give up (crash-loop
    // breaker) instead of respawning forever.
    log(&console, "[INIT] entering the event loop (blocking wait + watchdog respawn)...");
    let dm_policy = demo::RespawnPolicy::default();
    let dm_image_copy = dm_image; // scalars, owned by the closure below
    let respawn_dm = |attempt: u32| -> Result<InitChannel<RealKernel>, ChannelError> {
        log_fmt!(
            &console,
            "[INIT] Device Manager died; respawning (restart {}, backoff {} ns)...",
            attempt,
            dm_policy.backoff_for(attempt),
        );
        let dm = spawn_device_manager(&console, dm_image_copy);
        // Re-run the registry handshake against the fresh DM: blocking
        // send (timeout 0 — a full queue parks us), then the
        // finite-deadline wait for "devices ready".
        demo::send_registry(&dm, MSG_DEVICE_REGISTRY, &payload, &devreg_send_cap)?;
        let mut rb = [0u8; 256];
        match demo::wait_devices_ready(&dm, &mut rb, DM_READY_DEADLINE_NS) {
            Ok(()) => log(&console, "[INIT] respawned DM signalled ready."),
            Err(ChannelError::Timeout) => {
                log(&console, "[INIT] respawned DM not ready yet; serving anyway.")
            }
            Err(e) => log_fmt!(&console, "[INIT] respawn handshake failed: {}", e),
        }
        Ok(dm)
    };
    // POSIX sidecar watchdog: when irqtest's driver-death phase kills the
    // process mid-storm, the kernel teardown closes this messenger and
    // frees the bound vectors; the respawn below creates a FRESH POSIX
    // process from the same manifest (fresh single-use IRQ caps), which
    // re-binds vector 36 and re-passes the irqtest phases.
    let posix_policy = demo::RespawnPolicy {
        base_backoff_ns: 100_000_000,  // 100 ms
        max_backoff_ns: 1_000_000_000, // 1 s cap (one restart expected)
        max_restarts: 3,
    };
    let (posix_kaddr, posix_size, posix_heap) = (
        posix_image_cap.base,
        posix_image_cap.len as u32,
        posix_heap_base,
    );
    let respawn_posix = |attempt: u32| -> Result<InitChannel<RealKernel>, ChannelError> {
        log_fmt!(
            &console,
            "[INIT] POSIX sidecar died; respawning (restart {}, backoff {} ns)...",
            attempt,
            posix_policy.backoff_for(attempt),
        );
        let p = spawn_posix_sidecar(&console, posix_kaddr, posix_size, posix_heap, e1000_bar0);
        log_fmt!(
            &console,
            "[INIT] respawned POSIX messenger: CHAN_R={} CHAN_W={}",
            p.r,
            p.w,
        );
        Ok(p)
    };
    match demo::run_supervisor_loop(
        &console,
        dm_channel,
        &dm_policy,
        respawn_dm,
        posix_channel,
        &posix_policy,
        respawn_posix,
    ) {
        Ok(()) => log(&console, "[INIT] supervisor event loop exited."),
        Err(ChannelError::TooManyRestarts) => {
            log(&console, "[INIT] a supervised sidecar crash-looped; giving up respawns.")
        }
        Err(e) => log_fmt!(&console, "[INIT] supervisor event loop error: {}", e),
    }
    loop {
        core::hint::spin_loop();
    }
}

/// Spawn the Device Manager sidecar through the real kernel path
/// (`SYS_SLS_CREATE_SIDECAR`): pack the DM's manifest (records + the
/// image_kaddr footer pointing at the boot-loaded binary), call
/// `Kernel::create_sidecar`, and wrap the returned parent messenger ends
/// (CHAN_R for receiving the DM's replies, CHAN_W for sending to it) in
/// an `InitChannel`. Called both at boot (step 5) and by the watchdog's
/// respawn closure (step 9) — each call creates a FRESH DM process.
fn spawn_device_manager(
    console: &InitChannel<RealKernel>,
    image: DmImage,
) -> InitChannel<RealKernel> {
    log_fmt!(
        console,
        "[INIT]   (create_sidecar: {} image @ 0x{:x}, {} bytes)",
        dm_manifest::DM_MANIFEST_NAME,
        image.kaddr,
        image.size,
    );
    let manifest = dm_manifest::build_dm_manifest(&image);
    let (r, w) = RealKernel
        .create_sidecar(&manifest)
        .unwrap_or_else(|e| panic!("[INIT] create_sidecar failed: {e}"));
    log_fmt!(console, "[INIT]   messenger: CHAN_R={r} CHAN_W={w}");
    InitChannel::new(RealKernel, r, w)
}

/// Spawn the ramdisk driver through the real kernel path (`SYS_SLS_CREATE_SIDECAR`).
/// The ramdisk binary address comes from the boot image's `ramdisk.image` MEM cap;
/// budget and storage are computed from the layout (not BIB caps).
fn spawn_ramdisk_driver(
    console: &InitChannel<RealKernel>,
    image_cap: &aerosls_proto::bootinfo::BootCap<'_>,
    budget_base: u64,
    budget_size: u64,
    storage_base: u64,
    storage_len: u64,
) {
    log_fmt!(
        console,
        "[INIT]   (create_sidecar: {} image @ 0x{:x}, {} bytes)",
        ramdisk_manifest::RAMDISK_MANIFEST_NAME,
        image_cap.base,
        image_cap.len,
    );
    let manifest = ramdisk_manifest::build_ramdisk_manifest(
        image_cap.base,
        image_cap.len as u32,
        budget_base,
        storage_base,
        storage_len,
    );
    let (r, w) = RealKernel
        .create_sidecar(&manifest)
        .unwrap_or_else(|e| panic!("[INIT] ramdisk create_sidecar failed: {e}"));
    log_fmt!(console, "[INIT]   ramdisk messenger: CHAN_R={r} CHAN_W={w}");
}

/// Spawn the POSIX sidecar through the real kernel path (`SYS_SLS_CREATE_SIDECAR`).
/// The POSIX binary address comes from the boot image's `posix.image` MEM cap;
/// the budget heap base is computed from the image cap (page-aligned after image).
fn spawn_posix_sidecar(
    console: &InitChannel<RealKernel>,
    image_kaddr: u64,
    image_size: u32,
    heap_base: u64,
    e1000_bar0: Option<u64>,
) -> InitChannel<RealKernel> {
    log_fmt!(
        console,
        "[INIT]   (create_sidecar: {} image @ 0x{:x}, {} bytes)",
        posix_manifest::POSIX_MANIFEST_NAME,
        image_kaddr,
        image_size,
    );
    let manifest = posix_manifest::build_posix_manifest(image_kaddr, image_size, heap_base, e1000_bar0);
    let (r, w) = RealKernel
        .create_sidecar(&manifest)
        .unwrap_or_else(|e| panic!("[INIT] POSIX create_sidecar failed: {e}"));
    log_fmt!(console, "[INIT]   POSIX messenger: CHAN_R={r} CHAN_W={w}");
    InitChannel::new(RealKernel, r, w)
}

/// Spawn the network driver through the real kernel path (`SYS_SLS_CREATE_SIDECAR`).
/// The network binary address comes from the boot image's `net.image` MEM cap;
/// the budget heap base is computed from the image cap (page-aligned after image).
fn spawn_network_driver(
    console: &InitChannel<RealKernel>,
    image_cap: &aerosls_proto::bootinfo::BootCap<'_>,
    heap_base: u64,
) {
    log_fmt!(
        console,
        "[INIT]   (create_sidecar: {} image @ 0x{:x}, {} bytes)",
        net_manifest::NET_MANIFEST_NAME,
        image_cap.base,
        image_cap.len,
    );
    let manifest = net_manifest::build_net_manifest(
        image_cap.base,
        image_cap.len as u32,
        heap_base,
    );
    let (r, w) = RealKernel
        .create_sidecar(&manifest)
        .unwrap_or_else(|e| panic!("[INIT] network create_sidecar failed: {e}"));
    log_fmt!(console, "[INIT]   network messenger: CHAN_R={r} CHAN_W={w}");
}

// ── logging helpers ──────────────────────────────────────────────────────────

/// Send a log message over the console channel (fire-and-forget).
fn log(console: &InitChannel<RealKernel>, msg: &str) {
    // In the real kernel, this sends a NO_REPLY message on the console
    // channel.  The kernel console endpoint delivers it to the serial
    // port / VGA output.
    //
    // For the prototype, the kernel's serial output handles this via
    // the console channel's kernel peer (transport spec §2.1).
    let _ = console.request(0, msg.as_bytes());
}

