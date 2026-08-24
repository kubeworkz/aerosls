//! Host-side fake kernel for testing the init sidecar.
//!
//! Follows the pattern in `user/kernel-sim/`: implements the `Kernel` trait
//! with in-memory channels, so the init sidecar's bootstrap sequence can be
//! tested on the host without a real kernel.

use aerosls_proto::kabi::{CapInfo, GrantedCap, Kernel, SendCap};
use aerosls_proto::CH_KIND_NONE;
use alloc::collections::VecDeque;
use alloc::vec::Vec;
use core::cell::UnsafeCell;

/// Maximum number of channels in the simulation.
const SIM_CHANNELS: usize = 32;

/// A simulated channel endpoint (interior mutability for queue access).
struct SimEndpoint {
    queue: UnsafeCell<VecDeque<(u32, Vec<u8>, Vec<GrantedCap>)>>,
    cap: CapInfo,
}

// SAFETY: the sim kernel is only used in single-threaded test code.
unsafe impl Sync for SimEndpoint {}

impl SimEndpoint {
    fn new() -> Self {
        SimEndpoint {
            queue: UnsafeCell::new(VecDeque::new()),
            cap: CapInfo {
                ty: 0,
                rights: 0,
                flags: 0,
                base: 0,
                len: 0,
            },
        }
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
}

/// A simulated kernel that provides in-memory channels.
pub struct SimKernel {
    endpoints: Vec<SimEndpoint>,
    next_handle: u32,
}

impl SimKernel {
    pub fn new() -> Self {
        let mut endpoints = Vec::with_capacity(SIM_CHANNELS);
        for _ in 0..SIM_CHANNELS {
            endpoints.push(SimEndpoint::new());
        }
        SimKernel {
            endpoints,
            next_handle: 1,
        }
    }

    /// Register a capability on a handle (used by tests to set up initial state).
    pub fn register_cap(&mut self, handle: u32, cap: CapInfo) {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].cap = cap;
        }
    }

    /// Enqueue a message onto a channel (simulate kernel-to-sidecar delivery).
    pub fn inject_msg(&self, handle: u32, tag: u32, payload: &[u8]) {
        if (handle as usize) < self.endpoints.len() {
            self.endpoints[handle as usize].push(tag, payload.to_vec(), Vec::new());
        }
    }

    /// Allocate the next channel handle.
    pub fn alloc_handle(&mut self) -> u32 {
        let h = self.next_handle;
        self.next_handle += 1;
        h
    }
}

impl Default for SimKernel {
    fn default() -> Self {
        Self::new()
    }
}

impl Kernel for SimKernel {
    fn wait(&self, chans: &[u32], _timeout_ns: u64) -> Result<(usize, u16), i32> {
        for (i, &handle) in chans.iter().enumerate() {
            if (handle as usize) < self.endpoints.len() {
                if !self.endpoints[handle as usize].is_empty() {
                    return Ok((i, aerosls_proto::CH_KIND_MSG));
                }
            }
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
        let (tag, payload, caps) = self.endpoints[chan as usize]
            .pop()
            .ok_or(-1)?;

        let len = payload.len().min(buf.len());
        buf[..len].copy_from_slice(&payload[..len]);

        let n_caps = caps.len().min(slots.len());
        for i in 0..n_caps {
            slots[i] = caps[i];
        }

        Ok(aerosls_proto::kabi::RecvResult {
            kind: aerosls_proto::CH_KIND_MSG,
            flags: 0,
            tag,
            len,
            n_caps,
        })
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
        Ok(self.endpoints[handle as usize].cap)
    }
}
