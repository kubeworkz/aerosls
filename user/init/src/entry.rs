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

use crate::chan::{ChannelError, InitChannel, MSG_DEVICE_REGISTRY, MSG_DEVICES_READY};
use crate::devreg::DeviceRegistry;
use crate::heap::Bump;
use aerosls_proto::bootinfo::{BootInfo, BOOT_INFO_MAGIC};
use aerosls_proto::kabi::{
    Kernel, RealKernel, SendCap, CAP_MEM, CAP_SPAWN, TIMEOUT_NONE,
};

/// Reserved heap over the budget region (single-threaded sidecar).
static mut HEAP: Bump = Bump::new();

/// Cap types from the capability-layer spec.
const CAP_MEM_TYPE: u16 = CAP_MEM;
const CAP_SPAWN_TYPE: u16 = CAP_SPAWN;

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
    log_fmt(&console, "[INIT] budget: {} MiB", budget_cap.len >> 20);
    log_fmt(
        &console,
        "[INIT] found {} PCI device(s)",
        devreg.len(),
    );
    for (i, e) in devreg.iter().enumerate() {
        let name = e.manifest_name().unwrap_or("?");
        log_fmt(
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

    dm_channel
        .send_with_cap(MSG_DEVICE_REGISTRY, &payload, &devreg_send_cap)
        .unwrap_or_else(|e| panic!("[INIT] failed to send device registry: {e}"));

    // ── 7. Wait for "devices ready" ──────────────────────────────────────
    log(&console, "[INIT] waiting for Device Manager to initialise devices...");

    let mut reply_buf = [0u8; 256];
    match dm_channel.recv_msg(&mut reply_buf) {
        Ok(tag) if tag == MSG_DEVICES_READY => {
            log(&console, "[INIT] all devices ready.");
        }
        Ok(tag) => {
            log_fmt(
                &console,
                "[INIT] unexpected reply tag 0x{:08x} (expected MSG_DEVICES_READY)",
                tag,
            );
        }
        Err(e) => {
            panic!("[INIT] Device Manager closed unexpectedly: {e}");
        }
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

    // ── 9. Park in the scheduler event loop ───────────────────────────────
    //    Wait on the console channel for input / events.
    loop {
        let mut chans = [console_cap.slot];
        let _ = RealKernel.wait(&mut chans, TIMEOUT_NONE);
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

/// Send a formatted log message (simplified — no fmt in no_std).
fn log_fmt(console: &InitChannel<RealKernel>, _fmt: &str, _args: impl core::fmt::Display) {
    // In a real no_std environment, we'd write to a stack buffer.
    // For the prototype, just log the format string.
    let _ = console.request(0, _fmt.as_bytes());
}
