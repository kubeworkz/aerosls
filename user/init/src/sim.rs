//! Host-side fake kernel for testing the init sidecar.
//!
//! Follows the pattern in `user/kernel-sim/`: implements the `Kernel` trait
//! with in-memory channels, so the init sidecar's bootstrap sequence can be
//! tested on the host without a real kernel.

use aerosls_proto::kabi::{CapInfo, GrantedCap, Kernel, SendCap, CAP_CHAN};
use aerosls_proto::{CH_KIND_CLOSE, CH_KIND_NONE};
use alloc::collections::VecDeque;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::cell::UnsafeCell;

/// Maximum number of channels in the simulation.
const SIM_CHANNELS: usize = 32;

/// E5: the first pid the sim hands a spawned sidecar. A non-zero start keeps
/// 0 meaning "no such sidecar" — the ABI's own convention for `sidecar_pid`.
const SIM_FIRST_PID: u32 = 100;

/// A simulated channel endpoint (interior mutability for queue access).
struct SimEndpoint {
    queue: UnsafeCell<VecDeque<(u32, Vec<u8>, Vec<GrantedCap>)>>,
    /// The cap backing this endpoint (interior mutability so
    /// `Kernel::create_sidecar(&self)` can mint a spawn's endpoint).
    cap: UnsafeCell<CapInfo>,
    /// Pending CLOSE event (reason, detail) — set by `inject_close` to
    /// model the kernel's close_evt state.
    closed: UnsafeCell<Option<(u16, u32)>>,
}

// SAFETY: the sim kernel is only used in single-threaded test code.
unsafe impl Sync for SimEndpoint {}

impl SimEndpoint {
    fn new() -> Self {
        SimEndpoint {
            queue: UnsafeCell::new(VecDeque::new()),
            cap: UnsafeCell::new(CapInfo {
                ty: 0,
                rights: 0,
                flags: 0,
                base: 0,
                len: 0,
            }),
            closed: UnsafeCell::new(None),
        }
    }

    fn set_cap(&self, cap: CapInfo) {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.cap.get() = cap }
    }

    fn cap(&self) -> CapInfo {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.cap.get() }
    }

    fn push(&self, tag: u32, payload: Vec<u8>, caps: Vec<GrantedCap>) {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.queue.get()).push_back((tag, payload, caps)) }
    }

    fn pop(&self) -> Option<(u32, Vec<u8>, Vec<GrantedCap>)> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.queue.get()).pop_front() }
    }

    fn is_empty(&self) -> bool {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.queue.get()).is_empty() }
    }

    fn queue_len(&self) -> usize {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.queue.get()).len() }
    }

    fn head_tag(&self) -> Option<u32> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.queue.get()).front().map(|(t, _, _)| *t) }
    }

    fn mark_closed(&self, reason: u16, detail: u32) {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.closed.get() = Some((reason, detail)); }
    }

    fn mark_closed_cleared(&self) {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.closed.get() = None; }
    }

    fn close_info(&self) -> Option<(u16, u32)> {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.closed.get() }
    }

    /// E5: put a handle back into a reusable state, the way the kernel's
    /// cap_table_teardown leaves the slot it frees — nothing of the previous
    /// owner survives into the next one.
    fn reset(&self) {
        // SAFETY: single-threaded test harness only.
        unsafe {
            (*self.queue.get()).clear();
            *self.closed.get() = None;
            *self.cap.get() = CapInfo { ty: 0, rights: 0, flags: 0, base: 0, len: 0 };
        }
    }
}

/// A simulated kernel that provides in-memory channels.
pub struct SimKernel {
    endpoints: Vec<SimEndpoint>,
    /// Next handle to hand out (interior mutability so `create_sidecar`
    /// can mint a spawn's channel through `&self`).
    next_handle: UnsafeCell<u32>,
    /// Handles minted by `create_sidecar` (the sim's spawn log — tests
    /// assert a spawn happened and how often).
    spawns: UnsafeCell<Vec<u32>>,
    /// POSIX-Environments E4: region allocations as `(nframes, align,
    /// charged_partition)`, and the next fake region base to hand out. When
    /// `regions_exhausted` is set, an allocation returns 0 (the frame pool
    /// cannot back the request). The partition is recorded so a test can prove
    /// an environment's heap and storage are charged to the TENANT: with the
    /// caller-charged `alloc_region` the third field is 0 ("the caller's"),
    /// with the E4 follow-on `alloc_region_in` it is the environment's own.
    regions: UnsafeCell<Vec<(u64, u64, u32)>>,
    next_region_base: UnsafeCell<u64>,
    regions_exhausted: UnsafeCell<bool>,
    /// POSIX-Environments E4: each `create_sidecar_in` as `(manifest bytes,
    /// target_partition)`, so tests see which partition a spawn targeted and
    /// what manifest it carried.
    created: UnsafeCell<Vec<(Vec<u8>, u32)>>,
    /// POSIX-Environments E5: the sim's sidecar registry as
    /// `(name, partition, pid, handle)`. The real kernel keeps one
    /// (`sidecar_registry_*`, name-scoped by partition since E2), and
    /// `sidecar_pid` is only meaningful against it, so the sim has to model it
    /// rather than invent pids: a name is registered when the sidecar is
    /// created from its manifest, and dropped when its teardown runs — which
    /// is what makes the destroy path's liveness test testable.
    sidecars: UnsafeCell<Vec<(Vec<u8>, u32, u32, u32)>>,
    /// E5: channel handles a torn-down sidecar gave back. The kernel closes a
    /// dead sidecar's channels (and reuses the slots), so the sim hands its
    /// handle back rather than burning SIM_CHANNELS on every destroy — which
    /// is what lets a recycle loop run past the channel budget at all.
    free_handles: UnsafeCell<Vec<u32>>,
    next_sim_pid: UnsafeCell<u32>,
    /// Pids `proc_kill` was called with, in order.
    killed: UnsafeCell<Vec<u32>>,
    /// When set, `proc_kill` records the pid but leaves the registry entry —
    /// the kernel's DEFERRED kill of a RUNNING target (kernel/process.c),
    /// which finishes at the next schedule. The destroy path must not reclaim
    /// an environment's frames in that window, so this is the sim's negative
    /// control for the ordering rule.
    kill_deferred: UnsafeCell<bool>,
    /// POSIX-Environments E5: regions released through `free_region_in`, as
    /// `(base, nframes, charged_partition)` — the environment's storage given
    /// back to its tenant partition, and the evidence the leak tests count.
    freed_regions: UnsafeCell<Vec<(u64, u64, u32)>>,
    /// POSIX-Environments E5: partitions a `partition destroy` has deactivated.
    /// `partition_destroy()` leaves the slot inactive, and
    /// `sys_sls_free_region()` refuses a release into a partition that is not
    /// active BEFORE it touches the frame bitmap — which is the whole reason
    /// the environment manager may attempt a release after a partition
    /// teardown ended an environment instead of leaking a double-decrement.
    dead_partitions: UnsafeCell<Vec<u32>>,
    /// POSIX-Environments E5: handles `cap_revoke` was called with, in order —
    /// the environment manager's own channel ends coming back. A destroy that
    /// stopped revoking them would stop growing this, and the kernel's channel
    /// space would drift back to being exhausted by the loop (which is how the
    /// real boot failed: `chan_create failed (-7)` at the twelfth environment).
    revoked: UnsafeCell<Vec<u32>>,
}

impl SimKernel {
    pub fn new() -> Self {
        let mut endpoints = Vec::with_capacity(SIM_CHANNELS);
        for _ in 0..SIM_CHANNELS {
            endpoints.push(SimEndpoint::new());
        }
        SimKernel {
            endpoints,
            next_handle: UnsafeCell::new(1),
            spawns: UnsafeCell::new(Vec::new()),
            regions: UnsafeCell::new(Vec::new()),
            next_region_base: UnsafeCell::new(0x1000_0000),
            regions_exhausted: UnsafeCell::new(false),
            created: UnsafeCell::new(Vec::new()),
            sidecars: UnsafeCell::new(Vec::new()),
            free_handles: UnsafeCell::new(Vec::new()),
            next_sim_pid: UnsafeCell::new(SIM_FIRST_PID),
            killed: UnsafeCell::new(Vec::new()),
            kill_deferred: UnsafeCell::new(false),
            freed_regions: UnsafeCell::new(Vec::new()),
            dead_partitions: UnsafeCell::new(Vec::new()),
            revoked: UnsafeCell::new(Vec::new()),
        }
    }

    /// POSIX-Environments E4: the `(nframes, align, charged_partition)` of each
    /// region allocation.
    pub fn regions(&self) -> Vec<(u64, u64, u32)> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.regions.get()).clone() }
    }

    /// POSIX-Environments E4: each `create_sidecar_in` as `(manifest, partition)`.
    pub fn created(&self) -> Vec<(Vec<u8>, u32)> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.created.get()).clone() }
    }

    /// POSIX-Environments E5: the live registry as
    /// `(name, partition, pid, handle)`.
    pub fn sidecars(&self) -> Vec<(Vec<u8>, u32, u32, u32)> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.sidecars.get()).clone() }
    }

    /// E5: drop a sidecar's registry entry and hand its channel back, the way
    /// cap_table_teardown() does ("a dying sidecar stops resolving" — its entry
    /// goes first, and its channels close with the rest of its capabilities).
    /// Returns true when there was such a sidecar.
    fn teardown_sidecar(&self, pid: u32) -> bool {
        let mut found = false;
        // SAFETY: single-threaded test harness only.
        unsafe {
            let reg = &mut *self.sidecars.get();
            let mut i = 0;
            while i < reg.len() {
                if reg[i].2 == pid {
                    let handle = reg[i].3;
                    reg.remove(i);
                    (*self.free_handles.get()).push(handle);
                    found = true;
                } else {
                    i += 1;
                }
            }
        }
        found
    }

    /// POSIX-Environments E5: the live registry's entry count — the number the
    /// leak tests hold stable across a recycle loop.
    pub fn sidecar_count(&self) -> usize {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.sidecars.get()).len() }
    }

    /// POSIX-Environments E5: handles `cap_revoke` was called with.
    pub fn revoked_handles(&self) -> Vec<u32> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.revoked.get()).clone() }
    }

    /// POSIX-Environments E5: pids `proc_kill` was called with, in order.
    pub fn killed_pids(&self) -> Vec<u32> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.killed.get()).clone() }
    }

    /// POSIX-Environments E5: regions released through `free_region_in`.
    pub fn freed_regions(&self) -> Vec<(u64, u64, u32)> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.freed_regions.get()).clone() }
    }

    /// POSIX-Environments E5: model the kernel's deferred kill of a RUNNING
    /// target — every subsequent `proc_kill` records the pid but leaves the
    /// sidecar registered, so `sidecar_pid` keeps resolving it.
    pub fn set_kill_deferred(&self, deferred: bool) {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.kill_deferred.get() = deferred };
    }

    /// POSIX-Environments E5: the schedule tick that finishes a deferred
    /// teardown — every killed pid's registry entry goes away now.
    pub fn complete_deferred_kill(&self) {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.kill_deferred.get() = false };
        for pid in self.killed_pids() {
            self.teardown_sidecar(pid);
        }
    }

    /// POSIX-Environments E5: model a `partition destroy`. The kernel kills
    /// every process in the partition (`process_kill_partition`, which drops
    /// each sidecar's registry entry) and deactivates the slot, after which
    /// `sys_sls_free_region()` refuses a release into it because
    /// `partition_reclaim_all_frames()` has already returned every frame that
    /// partition owned. Both halves are modelled, because both are what the
    /// environment manager's partition-teardown handling depends on.
    pub fn destroy_partition(&self, partition: u32) {
        // SAFETY: single-threaded test harness only.
        unsafe {
            let reg = &mut *self.sidecars.get();
            let mut i = 0;
            while i < reg.len() {
                if reg[i].1 == partition {
                    let handle = reg[i].3;
                    reg.remove(i);
                    (*self.free_handles.get()).push(handle);
                } else {
                    i += 1;
                }
            }
            (*self.dead_partitions.get()).push(partition);
        }
    }

    /// POSIX-Environments E4: make `alloc_region` return 0 (frame pool cannot
    /// back the request), so tests can drive the region-exhaustion path.
    pub fn set_regions_exhausted(&self, exhausted: bool) {
        // SAFETY: single-threaded test harness only.
        unsafe { *self.regions_exhausted.get() = exhausted };
    }

    /// Register a capability on a handle (used by tests to set up initial state).
    pub fn register_cap(&mut self, handle: u32, cap: CapInfo) {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].set_cap(cap);
        }
    }

    /// Handles minted by `create_sidecar` since construction, in order.
    pub fn spawn_handles(&self) -> Vec<u32> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.spawns.get()).clone() }
    }

    /// Enqueue a message onto a channel (simulate kernel-to-sidecar delivery).
    pub fn inject_msg(&self, handle: u32, tag: u32, payload: &[u8]) {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].push(tag, payload.to_vec(), Vec::new());
        }
    }

    /// Simulate the peer closing: a CLOSE event becomes pending on the
    /// endpoint (the real kernel sets close_evt on explicit close / death).
    pub fn inject_close(&self, handle: u32, reason: u16, detail: u32) {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].mark_closed(reason, detail);
        }
    }

    /// Queued-message count on a handle (tests verify blocking sends
    /// landed on the console endpoint).
    pub fn queue_len(&self, handle: u32) -> usize {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].queue_len()
        } else {
            0
        }
    }

    /// Tag of the head queued message on a handle, if any.
    pub fn peek_tag(&self, handle: u32) -> Option<u32> {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].head_tag()
        } else {
            None
        }
    }

    /// Allocate the next channel handle.
    pub fn alloc_handle(&mut self) -> u32 {
        let h = unsafe { *self.next_handle.get() };
        unsafe { *self.next_handle.get() = h + 1 };
        h
    }
}

impl Default for SimKernel {
    fn default() -> Self {
        Self::new()
    }
}

impl Kernel for SimKernel {
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32> {
        for (i, &handle) in chans.iter().enumerate() {
            if (handle as usize) >= self.endpoints.len() {
                continue;
            }
            let ep = &self.endpoints[handle as usize];
            if !ep.is_empty() {
                return Ok((i, aerosls_proto::CH_KIND_MSG));
            }
            if ep.close_info().is_some() {
                return Ok((i, CH_KIND_CLOSE));
            }
        }
        // The real kernel parks a TIMEOUT_NONE wait until an event and wakes
        // a finite-deadline wait at its deadline with ERR_TIMEOUT (kernel/
        // chan.c + process.c's cap_park_deadline_tick). The host fake cannot
        // block, so it models the two observable outcomes: a finite deadline
        // with nothing ready means the deadline has passed (ERR_TIMEOUT);
        // TIMEOUT_NONE with nothing ready means the wait has not yet been
        // woken (CH_KIND_NONE — a single-iteration test sees "no event").
        if timeout_ns != aerosls_proto::kabi::TIMEOUT_NONE {
            return Err(aerosls_proto::kabi::ERR_TIMEOUT);
        }
        Ok((0, CH_KIND_NONE))
    }

    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<aerosls_proto::kabi::RecvResult, i32> {
        if (chan as usize) >= self.endpoints.len() {
            return Err(-1);
        }
        let ep = &self.endpoints[chan as usize];
        if let Some((tag, payload, caps)) = ep.pop() {
            let len = payload.len().min(buf.len());
            buf[..len].copy_from_slice(&payload[..len]);

            let n_caps = caps.len().min(slots.len());
            for i in 0..n_caps {
                slots[i] = caps[i];
            }

            return Ok(aerosls_proto::kabi::RecvResult {
                kind: aerosls_proto::CH_KIND_MSG,
                flags: 0,
                tag,
                len,
                n_caps,
            });
        }
        if let Some((reason, detail)) = ep.close_info() {
            // Close body, the kernel's wire format (kernel/chan.c): reason
            // u16 LE, detail u32 LE, pad 2.
            let body = [
                (reason & 0xFF) as u8,
                (reason >> 8) as u8,
                (detail & 0xFF) as u8,
                ((detail >> 8) & 0xFF) as u8,
                ((detail >> 16) & 0xFF) as u8,
                ((detail >> 24) & 0xFF) as u8,
                0,
                0,
            ];
            // Delivered exactly once, matching the kernel's close_evt
            // contract (kernel/chan.c): a second recv sees an empty
            // endpoint and fails instead of re-delivering the CLOSE.
            ep.mark_closed_cleared();
            let len = body.len().min(buf.len());
            buf[..len].copy_from_slice(&body[..len]);
            return Ok(aerosls_proto::kabi::RecvResult {
                kind: CH_KIND_CLOSE,
                flags: 0,
                tag: 0,
                len,
                n_caps: 0,
            });
        }
        Err(-1)
    }

    fn send(
        &self,
        chan: u32,
        tag: u32,
        _flags: u16,
        payload: &[u8],
        _caps: &[SendCap],
        _timeout_ns: u64,
    ) -> Result<(), i32> {
        if (chan as usize) >= self.endpoints.len() {
            return Err(-1);
        }
        self.endpoints[chan as usize].push(tag, payload.to_vec(), Vec::new());
        Ok(())
    }

    fn close(&self, _chan: u32, _reason: u16, _detail: u32) -> Result<(), i32> {
        Ok(())
    }

    /// E5: give a sidecar's channel handle back to the sim's reuse pool — the
    /// kernel's cap_revoke drops the holder (the slot returns) and frees the
    /// channel at refcount 0 (the channel id returns), so a create/destroy
    /// loop must be able to reuse both. Handing the handle back is what makes
    /// the sim's channel budget exercised the way the kernel's is.
    fn cap_revoke(&self, handle: u32) -> Result<(), i32> {
        if (handle as usize) >= self.endpoints.len() {
            return Err(-1);
        }
        // SAFETY: single-threaded test harness only.
        unsafe {
            (*self.revoked.get()).push(handle);
            let free = &mut *self.free_handles.get();
            if !free.contains(&handle) {
                self.endpoints[handle as usize].reset();
                free.push(handle);
            }
        }
        Ok(())
    }

    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
        if (handle as usize) >= self.endpoints.len() {
            return Err(-1);
        }
        Ok(self.endpoints[handle as usize].cap())
    }

    fn create_sidecar(&self, manifest: &[u8]) -> Result<(u32, u32), i32> {
        // Mint a fresh channel endpoint for the spawned sidecar (the real
        // kernel creates a process and returns the parent's messenger
        // ends; the sim has no process model, so the new handle IS the
        // messenger — both ends, per the sim's single-handle channel).
        // E5: prefer a channel a torn-down sidecar gave back. The kernel frees
        // a dead sidecar's channels and reuses the slots, so a recycle loop
        // past the channel budget is only possible if the sim does the same.
        // A reused endpoint is reset first, so nothing of its previous owner
        // (a queued message, a pending close) survives into the next sidecar.
        let h = match unsafe { (*self.free_handles.get()).pop() } {
            Some(h) => {
                self.endpoints[h as usize].reset();
                h
            }
            None => {
                let h = unsafe { *self.next_handle.get() };
                unsafe { *self.next_handle.get() = h + 1 };
                h
            }
        };
        if (h as usize) >= self.endpoints.len() {
            return Err(-1);
        }
        self.endpoints[h as usize].set_cap(CapInfo {
            ty: CAP_CHAN,
            rights: 0x0003,
            flags: 0,
            base: 0,
            len: 0,
        });
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.spawns.get()).push(h) };
        let _ = manifest.len(); // the sim trusts the blob (kernel validates it)
        Ok((h, h))
    }

    fn alloc_region(&self, nframes: u64, align_frames: u64) -> u64 {
        // 0 in the partition slot is the ABI's "charge the caller" — the sim
        // does not model a caller's partition, so the two are distinguished by
        // value, exactly as the kernel sees them.
        self.alloc_region_in(nframes, align_frames, 0)
    }

    fn alloc_region_in(&self, nframes: u64, align_frames: u64,
                       target_partition: u32) -> u64 {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.regions.get()).push((nframes, align_frames, target_partition)) };
        if unsafe { *self.regions_exhausted.get() } {
            return 0;
        }
        let base = unsafe { *self.next_region_base.get() };
        // Advance by the request size so successive regions are distinct and
        // non-overlapping — the env manager relies on distinct storage bases.
        unsafe { *self.next_region_base.get() = base + nframes * 4096 };
        base
    }

    fn create_sidecar_in(&self, manifest: &[u8], target_partition: u32) -> Result<(u32, u32), i32> {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.created.get()).push((manifest.to_vec(), target_partition)) };
        let ends = self.create_sidecar(manifest)?;
        // E5: register the new sidecar under its manifest NAME in its target
        // partition, the way the real kernel's create path does, so
        // `sidecar_pid` can resolve it and the destroy path has something real
        // to look up. A manifest without a name record is skipped rather than
        // given a synthetic one: an unnamed sidecar cannot be resolved by the
        // real registry either, and inventing a name here would hide that.
        let name = aerosls_proto::manifest::parse_manifest(manifest)
            .ok()
            .and_then(|m| m.name.map(|n| n.as_bytes().to_vec()));
        if let Some(name) = name {
            let pid = unsafe { *self.next_sim_pid.get() };
            unsafe { *self.next_sim_pid.get() = pid + 1 };
            // SAFETY: single-threaded test harness only.
            unsafe { (*self.sidecars.get()).push((name, target_partition, pid, ends.0)) };
        }
        Ok(ends)
    }

    fn free_region_in(&self, base: u64, nframes: u64, target_partition: u32) -> bool {
        // A zero base is the allocator's own failure value, so there is nothing
        // to release; a zero-length region is refused the way the kernel's
        // free_contiguous_frames_for_partition refuses it.
        if base == 0 || nframes == 0 {
            return false;
        }
        // SAFETY: single-threaded test harness only.
        unsafe {
            // E5: refused — before the bitmap is touched — when the charged
            // partition is not active (sys_sls_free_region).
            if (*self.dead_partitions.get()).contains(&target_partition) {
                return false;
            }
            (*self.freed_regions.get()).push((base, nframes, target_partition));
        }
        true
    }

    fn sidecar_pid(&self, name: &str, partition: u32) -> u32 {
        let want = name.as_bytes();
        // SAFETY: single-threaded test harness only.
        unsafe {
            (*self.sidecars.get())
                .iter()
                .find(|(n, p, _, _)| n.as_slice() == want && *p == partition)
                .map(|(_, _, pid, _)| *pid)
                .unwrap_or(0)
        }
    }

    fn proc_kill(&self, pid: u32) {
        // SAFETY: single-threaded test harness only.
        unsafe { (*self.killed.get()).push(pid) };
        if unsafe { *self.kill_deferred.get() } {
            return; // the kernel defers a RUNNING target; see the field comment
        }
        self.teardown_sidecar(pid);
    }
}

/* Shared-kernel form: the demo runs ONE sidecar holding several channels
 * (console + Device Manager) over the same kernel instance, so tests share
 * one SimKernel between the channels. A bare `impl Kernel for Rc<SimKernel>`
 * would violate the orphan rule (Kernel and Rc are both foreign), so the
 * local newtype `SharedKernel` wraps the Rc. The real kernel (RealKernel)
 * is a Copy unit struct, so the entry path needs no wrapper. */
#[derive(Clone)]
pub struct SharedKernel(pub Rc<SimKernel>);

impl SharedKernel {
    pub fn new() -> Self {
        SharedKernel(Rc::new(SimKernel::new()))
    }

    /// Wrap an already-configured sim (tests register initial caps on the
    /// plain `SimKernel` before sharing it between channels — an `Rc`
    /// cannot be borrowed mutably).
    pub fn from_sim(sim: SimKernel) -> Self {
        SharedKernel(Rc::new(sim))
    }

    /// Borrow the underlying sim (tests inspect queues / inject events).
    pub fn sim(&self) -> &SimKernel {
        &self.0
    }
}

impl Default for SharedKernel {
    fn default() -> Self {
        Self::new()
    }
}

impl Kernel for SharedKernel {
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32> {
        self.0.wait(chans, timeout_ns)
    }

    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<aerosls_proto::kabi::RecvResult, i32> {
        self.0.recv(chan, buf, slots)
    }

    fn send(
        &self,
        chan: u32,
        tag: u32,
        flags: u16,
        payload: &[u8],
        caps: &[SendCap],
        timeout_ns: u64,
    ) -> Result<(), i32> {
        self.0.send(chan, tag, flags, payload, caps, timeout_ns)
    }

    fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32> {
        self.0.close(chan, reason, detail)
    }

    fn cap_revoke(&self, handle: u32) -> Result<(), i32> {
        self.0.cap_revoke(handle)
    }

    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
        self.0.cap_info(handle)
    }

    fn create_sidecar(&self, manifest: &[u8]) -> Result<(u32, u32), i32> {
        self.0.create_sidecar(manifest)
    }

    fn create_sidecar_in(&self, manifest: &[u8], target_partition: u32) -> Result<(u32, u32), i32> {
        self.0.create_sidecar_in(manifest, target_partition)
    }

    fn alloc_region(&self, nframes: u64, align_frames: u64) -> u64 {
        self.0.alloc_region(nframes, align_frames)
    }

    fn alloc_region_in(&self, nframes: u64, align_frames: u64,
                       target_partition: u32) -> u64 {
        self.0.alloc_region_in(nframes, align_frames, target_partition)
    }

    fn free_region_in(&self, base: u64, nframes: u64, target_partition: u32) -> bool {
        self.0.free_region_in(base, nframes, target_partition)
    }

    fn sidecar_pid(&self, name: &str, partition: u32) -> u32 {
        self.0.sidecar_pid(name, partition)
    }

    fn proc_kill(&self, pid: u32) {
        self.0.proc_kill(pid)
    }
}
