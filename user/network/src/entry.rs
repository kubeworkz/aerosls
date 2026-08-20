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

#[no_mangle]
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("corrupt boot info");
    let k = RealKernel;

    // Budget → heap.
    let budget = bib.find_cap(CAP_MEM, "budget").expect("no budget cap");
    unsafe {
        HEAP.init(budget.base as usize, budget.len as usize);
    }

    // Network region (v1: the mock backend is in-process; the cap is a
    // placeholder for the real network stack's memory region).
    let _network = bib.find_cap(CAP_MEM, "network").expect("no network cap");

    // Console + initial endpoints.
    let console = bib.find_cap(CAP_CHAN, "console").expect("no console cap");
    let mut eps = EndpointSet::new(console.slot);
    let mut net = MockNetwork::new();
    for c in bib.caps() {
        adopt_initial(console.slot, c, &mut eps);
    }

    loop {
        let _ = server::run(&k, &mut eps, &mut net);
    }
}

fn adopt_initial(console_slot: u32, c: &BootCap<'_>, eps: &mut EndpointSet) {
    if c.ty == CAP_CHAN && c.slot != console_slot {
        eps.adopt(c.slot);
    }
}
