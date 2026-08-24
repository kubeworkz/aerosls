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
        }
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
        let h = unsafe { *self.next_handle.get() };
        unsafe { *self.next_handle.get() = h + 1 };
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

    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
        self.0.cap_info(handle)
    }

    fn create_sidecar(&self, manifest: &[u8]) -> Result<(u32, u32), i32> {
        self.0.create_sidecar(manifest)
    }
}
