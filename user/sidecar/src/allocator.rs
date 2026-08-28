//! The sidecar's request-buffer allocator.
//!
//! Every `RD_READ`/`RD_WRITE` needs a client-owned buffer to grant the
//! driver. `BudgetAlloc` carves them from the sidecar's own **budget MEM
//! cap** (manifest `budget`): the buffer is a sub-region of that cap, sent
//! as `SendCap { slot: budget_slot, offset, len, rights }` — the kernel
//! mints the grant from the sidecar's own cap, so there is no
//! amplification (capability-layer spec §1.2). A single reusable buffer
//! suffices (window = 1 per endpoint), but the bump gives the cache room
//! for its pool.
//!
//! On the host, tests use the fake kernel's regions instead; this allocator
//! is what the sidecar image links.

use aerosls_proto::kabi::{SendCap, ERR_BUDGET};
use aerosls_proto::{R, W};

use crate::heap::Bump;

/// Carve request buffers out of the budget cap. `budget_slot` is the cap's
/// handle in the sidecar's own table; `budget_base` its mapped address (for
/// the block cache's copies).
///
/// The block cache is strictly window=1 and single-threaded, so a SINGLE
/// fixed buffer (the region at `budget_base`, offset 0) is all it ever
/// needs. The transport MOVES a cap when it is sent (capability-layer
/// move, not mint), so every request must hand out a currently-valid source
/// cap slot; `reclaim()` re-adopts the slot the peer returns in its reply.
pub struct BudgetAlloc {
    slot: u32,
    base: u64,
    bump: Bump,
}

impl BudgetAlloc {
    /// Bind to the budget cap and claim `[base, base + len)` as the
    /// allocatable region (the entry point calls this once at boot).
    pub fn new(slot: u32, base: u64, len: u64) -> BudgetAlloc {
        let mut a = BudgetAlloc {
            slot,
            base,
            bump: Bump::new(),
        };
        a.bump.init(base as usize, len as usize);
        a
    }
}

impl aerosls_blockcache::BufferAlloc for BudgetAlloc {
    fn alloc(&mut self, len: usize) -> Result<(SendCap, u64), i32> {
        // Single fixed buffer: offset 0 of the budget cap, base == the
        // cap's own base. window=1 means one grant is in flight at a time;
        // it returns via the move-return (`reclaim` sets `self.slot`).
        Ok((
            SendCap {
                slot: self.slot,
                offset: 0,
                len: len as u32,
                rights: R | W,
                flags: 0,
            },
            self.base,
        ))
    }

    fn reclaim(&mut self, slot: u32) {
        // The peer returned our grant; it landed at a fresh slot in our
        // table. Adopt it so the next request sends a VALID source cap.
        self.slot = slot;
    }
}
