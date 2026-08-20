//! Arc-wrapped kernel type for sharing across components.
//!
//! When two components (e.g. the block cache and the network client) need
//! the same `K: Kernel` instance, wrapping it in `Arc` and implementing
//! `Kernel` for the wrapper lets both hold a shared reference without
//! changing the generic `K: Kernel` bound everywhere.

use alloc::sync::Arc;
use spin::Mutex;

use crate::kabi::{CapInfo, GrantedCap, Kernel, RecvResult, SendCap};

/// Buffer acquisition for request grants. Moved here from blockcache so
/// both blockcache and net_client can share the same allocator type.
pub trait BufferAlloc {
    fn alloc(&mut self, len: usize) -> Result<(SendCap, u64), i32>;
}

/// An `Arc`-wrapped buffer allocator with interior mutability.
/// Uses `spin::Mutex` (not `RefCell`) so the wrapper is `Send + Sync`
/// and can be moved across threads (e.g. the block cache driver thread).
#[derive(Clone)]
pub struct AWrap<A: BufferAlloc>(pub Arc<Mutex<A>>);

impl<A: BufferAlloc> BufferAlloc for AWrap<A> {
    fn alloc(&mut self, size: usize) -> Result<(SendCap, u64), i32> {
        self.0.lock().alloc(size)
    }
}

/// An `Arc`-wrapped kernel. Implements `Kernel` by delegating to the
/// inner `K`. `Clone` is cheap (Arc pointer copy).
#[derive(Clone)]
pub struct KWrap<K: Kernel>(pub Arc<K>);

impl<K: Kernel> Kernel for KWrap<K> {
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32> {
        self.0.wait(chans, timeout_ns)
    }
    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<RecvResult, i32> {
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
    fn poll(&self, chan: u32) -> Result<u16, i32> {
        self.0.poll(chan)
    }
}
