//! The init sidecar entry point over the real kernel ABI (feature `target`).
//!
//! Bootstrap sequence (Phase 5 §1.5):
//!
//! 1. crt0 hands us the BootInfo pointer (a0/rdi).
//! 2. Parse the BIB; resolve budget, console, device_registry, spawn caps.
//! 3. Init the heap over the budget region.
//! 4. Read the device registry from the kernel-populated MEM cap.
//! 5. Spawn the Device Manager via CAP_SPAWN; receive parent channel.
//! 6. Send the device registry snapshot to the Device Manager.
//! 7. Wait for "devices ready" signal.
//! 8. Spawn the POSIX sidecar (channels wired by the Device Manager).
//! 9. Park in the scheduler event loop.

use crate::chan::{ChannelError, InitChannel, MSG_DEVICE_REGISTRY};
use crate::demo::{self, DM_READY_DEADLINE_NS, DM_READY_RETRIES};
use crate::devreg::DeviceRegistry;
use crate::heap::Bump;
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{RealKernel, SendCap, CAP_MEM, CAP_SPAWN};

/// Reserved heap over the budget region (single-threaded sidecar).
static mut HEAP: Bump = Bump::new();

/// Cap types from the capability-layer spec.
const CAP_MEM_TYPE: u16 = CAP_MEM;
const CAP_SPAWN_TYPE: u16 = CAP_SPAWN;

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
        .find_cap(CAP_MEM_TYPE, "budget")
        .expect("[INIT] missing 'budget' MEM cap");

    let console_cap = bib
        .find_cap(aerosls_proto::kabi::CAP_CHAN, "console")
        .expect("[INIT] missing 'console' CHAN cap");

    let devreg_cap = bib
        .find_cap(CAP_MEM_TYPE, "device_registry")
        .expect("[INIT] missing 'device_registry' MEM cap");

    let spawn_cap = bib
        .find_cap(CAP_SPAWN_TYPE, "spawn.init")
        .expect("[INIT] missing 'spawn.init' CAP_SPAWN cap");

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

    // Log discovered devices to the console channel.
    let console = InitChannel::new(RealKernel, console_cap.slot);
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

    // ── 5. Spawn the Device Manager ───────────────────────────────────────
    log(&console, "[INIT] spawning Device Manager...");

    // The Device Manager is created via `create_sidecar` which returns
    // a parent channel.  We use `CAP_SPAWN` to tell the kernel which
    // manifest to use.
    //
    // In the real kernel ABI, this would be a `k_create_sidecar` syscall
    // that takes the spawn cap and returns a channel handle.  For now
    // we use the real kernel's `send` on the spawn cap to trigger the
    // sidecar creation (the kernel interprets a message on a CAP_SPAWN
    // endpoint as a create request).
    //
    // The returned channel handle is our parent→child control channel.
    // The Device Manager's end of this channel is in its initial table
    // (transport spec §2.3, path 3: parent–child).

    // For the prototype, we use a simulated path. In the real kernel,
    // this would be:
    //   let dm_channel = k_create_sidecar(spawn_handle, "drv.device_manager.0");
    //
    // We'll use `send` on the spawn cap as a trigger (the kernel knows
    // to create the sidecar from the manifest name stored in the cap).
    let dm_channel = spawn_device_manager(&console, spawn_cap.slot);

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
                // CAP_ERR_TIMEOUT); retry, then give up and move on.
                log_fmt!(&console, "[INIT] DM not ready yet (attempt {}/3)", attempt);
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

    // ── 8. Spawn the POSIX sidecar ────────────────────────────────────────
    log(&console, "[INIT] spawning POSIX sidecar...");

    // In the full Phase 5 design, the init sidecar would spawn the POSIX
    // sidecar via a CAP_SPAWN for "aerosls.posix.v1".  For this prototype
    // we log the intent and park.
    //
    // The POSIX sidecar's manifest would declare channels to:
    //   - the filesystem sidecar (VFS channel)
    //   - the network stack sidecar (socket channel)
    //   - the console driver sidecar (console channel)
    //
    // These would be wired by the Device Manager via manifest path 2
    // (transport spec §2.2) when it creates the POSIX sidecar.

    log(&console, "[INIT] ── Phase 5 init sidecar complete ──");
    log(&console, "[INIT] system ready for POSIX sidecar creation.");

    // ── 9. The demo server loop ───────────────────────────────────────────
    // Blocking k_chan_wait (TIMEOUT_NONE) on the Device Manager channel:
    // this sidecar parks (cap_wait_chans) until the DM queues a message or
    // a control event, and a wake re-runs the wait to return it. Console
    // output inside the loop uses blocking sends (timeout 0). The loop
    // exits only when the DM channel closes — a real init would respawn
    // the DM from here; this demo parks forever instead.
    log(&console, "[INIT] entering the event loop (blocking wait on the Device Manager channel)...");
    match demo::run_event_loop(&console, &dm_channel) {
        Ok(()) => log(&console, "[INIT] event loop exited: Device Manager channel closed."),
        Err(e) => log_fmt!(&console, "[INIT] event loop error: {}", e),
    }
    loop {
        core::hint::spin_loop();
    }
}

/// Spawn the Device Manager sidecar and return a channel to it.
///
/// In the real kernel ABI, this calls `k_create_sidecar` which:
/// 1. Validates the CAP_SPAWN cap and the manifest name.
/// 2. Creates the Device Manager sidecar from the validated manifest.
/// 3. Returns a parent–child control channel.
///
/// For the prototype, we use the kernel's spawn mechanism.
fn spawn_device_manager(console: &InitChannel<RealKernel>, spawn_handle: u32) -> InitChannel<RealKernel> {
    // Send a "create" message on the spawn cap.  The kernel interprets
    // this as a create_sidecar request for the manifest named in the cap.
    //
    // The kernel returns the parent channel handle as the send result
    // (stored in the kernel's sidecar table for this sidecar).
    //
    // For the prototype, we use a placeholder approach: the kernel
    // assigns a channel handle and we wrap it.
    //
    // TODO: implement k_create_sidecar in kernel/cap.c
    log(console, "[INIT]   (create_sidecar: drv.device_manager.0)");

    // Placeholder: in the real implementation, this would be:
    //   let result = k_create_sidecar(spawn_handle, b"drv.device_manager.0");
    //   let dm_chan_handle = result.channel_handle;
    //
    // For now, we return a channel to a simulated handle.  The real
    // kernel would mint the channel and return the handle.
    //
    // When the kernel implements create_sidecar, this function becomes:
    //
    // ```rust
    // extern "C" {
    //     fn k_create_sidecar(
    //         spawn_handle: u32,
    //         manifest_name: *const u8,
    //         manifest_len: u32,
    //         out_chan: *mut u32,
    //     ) -> i32;
    // }
    //
    // let mut chan_handle: u32 = 0;
    // let rc = unsafe {
    //     k_create_sidecar(
    //         spawn_handle,
    //         b"drv.device_manager.0".as_ptr(),
    //         b"drv.device_manager.0".len() as u32,
    //         &mut chan_handle,
    //     )
    // };
    // assert_eq!(rc, 0, "create_sidecar failed: {rc}");
    // InitChannel::new(RealKernel, chan_handle)
    // ```

    // For this prototype, we use a dummy handle.  The tests use the
    // simulated kernel which provides real channel behavior.
    InitChannel::new(RealKernel, spawn_handle)
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

