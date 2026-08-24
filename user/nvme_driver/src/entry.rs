//! The sidecar entry point over the real kernel ABI (cargo feature
//! `target`). Bootstrap per the Phase 5 design (§1.5) and the ramdisk
//! driver's plan (§3):
//!
//! 1. crt0 (`src/crt0.S`) hands us the BootInfo pointer.
//! 2. Parse the BIB; find budget / bar0 / dma / console caps by name.
//! 3. Init the heap over the budget region (reserved; v1 is allocation-free).
//! 4. Bring the NVMe controller up: BAR0 MMIO base from the `bar0` cap, DMA
//!    frames from the `dma` cap.
//! 5. Adopt every initial CHAN cap except console (respawn control channel).
//! 6. Serve forever; boot-time injection (NEW_CHANNEL) is handled by the
//!    server loop.
//!
//! The Device Manager decides whether this sidecar exists at all (it reads
//! the device registry — a missing controller means the NVMe driver is never
//! spawned), so a failed bring-up here is a kernel/hardware fault, not a
//! recovery case for this sidecar; the watchdog/restart policy lives with
//! the Device Manager (Phase 5 §8).

use crate::endpoints::EndpointSet;
use crate::heap::Bump;
use crate::kapi::{Kernel, RealKernel, CAP_CHAN, CAP_MEM};
use crate::nvme::NvmeDevice;
use crate::server;
use aerosls_proto::bootinfo::BootInfo;

/// Reserved heap (single-threaded sidecar; no atomics needed). Access is
/// confined to `rust_entry`.
static mut HEAP: Bump = Bump::new();

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

    // BAR0 MMIO base for the controller registers.
    let bar0 = bib.find_cap(CAP_MEM, "bar0").expect("no bar0 cap");
    let bar0_info = k.cap_info(bar0.slot).expect("bar0 cap_info failed");

    // DMA region for queues, the PRP list page, and bounce buffers.
    let dma = bib.find_cap(CAP_MEM, "dma").expect("no dma cap");
    let dma_info = k.cap_info(dma.slot).expect("dma cap_info failed");

    // Bring the controller up (disable → admin queues → enable → identify →
    // I/O queues).
    let mut dev = NvmeDevice::new(bar0_info.base, dma_info.base, dma_info.len)
        .expect("NVMe bring-up failed");

    // Console + initial endpoints (respawn control channel).
    let console = bib.find_cap(CAP_CHAN, "console").expect("no console cap");
    let mut eps = EndpointSet::new(console.slot);
    for c in bib.caps() {
        if c.ty == CAP_CHAN && c.slot != console.slot {
            eps.adopt(c.slot);
        }
    }

    loop {
        // Only returns on a fatal kernel error; a healthy kernel never does.
        let _ = server::run(&k, &mut eps, &mut dev);
    }
}
