//! The sidecar entry point over the real kernel ABI (cargo feature
//! `target`). Bootstrap per Phase 2 §6.2:
//!
//! 1. crt0 hands us the BootInfo pointer (a0/rdi).
//! 2. Parse the BIB; resolve budget/console/ramdisk caps by name.
//! 3. Init the heap over the budget region; bind `BudgetAlloc` to the
//!    budget cap for request buffers.
//! 4. `boot()`: connect the block cache to the ramdisk channel, mount
//!    `/`, `/dev`, `/tmp`, spawn init with console stdio.
//! 5. Run the scheduler; when it quiesces, park on the console channel
//!    (the event loop — typed input arrives there as messages; the ChanDev
//!    adapter that feeds the in-memory console is future work, so v1 just
//!    waits).
//!
//! Like the ramdisk driver's entry, this is forward-declared: it compiles
//! against the not-yet-existing kernel (`k_chan_*` externs in
//! `aerosls_proto::kabi`) and the init image is the image-build's business
//! (`PLACEHOLDER_INIT` below is replaced by the real shell).

use crate::allocator::BudgetAlloc;
use crate::boot::{boot, BootCaps};
use crate::heap::Bump;
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{Kernel, RealKernel, TIMEOUT_NONE};
use aerosls_procmgr::{Ctx, Program, Step};

/// Reserved heap over the budget region (single-threaded sidecar; access is
/// confined to `rust_entry`).
static mut HEAP: Bump = Bump::new();

/// A placeholder init: print a boot banner on the console and live forever.
/// The image build replaces this with the real init/shell.
fn placeholder_init(ctx: &mut Ctx<'_, RealKernel, BudgetAlloc>) -> Step {
    let task = ctx.task;
    if ctx.data.is_empty() {
        ctx.data.push(1);
        // fd 1 is the console stdout the bootstrap opened.
        let _ = ctx.vfs().write(task, 1, b"aerosls.posix.v1: init up\r\n");
        return Step::Yield;
    }
    Step::Yield
}

#[no_mangle]
// The `static mut` heap is deliberate on a bare-metal single-threaded
// target; the lint fires at this use site.
#[allow(static_mut_refs)]
pub extern "C" fn rust_entry(bib_ptr: *const u8) -> ! {
    let bib = unsafe { BootInfo::from_raw(bib_ptr) }.expect("corrupt boot info");
    let caps = BootCaps::from_bib(&bib).expect("missing initial caps");

    // Budget → heap (v1 reserves it for the image's real allocator) and
    // the request-buffer allocator over the same region.
    unsafe {
        HEAP.init(caps.budget_base as usize, caps.budget_len as usize);
    }
    let alloc = BudgetAlloc::new(caps.budget_slot, caps.budget_base, caps.budget_len);

    let console = alloc::sync::Arc::new(aerosls_vfs::CharNode::console());
    let init = Program::new("init", placeholder_init);
    let mut booted = boot(RealKernel, &caps, console, init, alloc)
        .unwrap_or_else(|e| panic!("sidecar boot failed: {e:?}"));

    loop {
        booted.proc.drain_wakes();
        if booted.proc.run_next().is_none() {
            // Quiesced — nothing runnable, nothing blocked. Park on the
            // console channel until the kernel delivers input (the event
            // loop). The kernel console channel is the same endpoint the
            // manifest named; a sidecar without one spins (v1 debug only).
            // `RealKernel` is a unit struct — a fresh handle is fine.
            let chan = [caps.console_chan.unwrap_or(u32::MAX)];
            let _ = RealKernel.wait(&chan, TIMEOUT_NONE);
        }
    }
}
