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
