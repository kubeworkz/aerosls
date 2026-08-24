//! Thin channel helpers for the init sidecar.
//!
//! Wraps `aerosls_proto::kabi::Kernel` with init-specific convenience
//! methods: send a typed request and wait for a typed reply.  Follows
//! the same pattern as `user/sidecar/src/net_client.rs`.

use aerosls_proto::kabi::{
    CapInfo, GrantedCap, Kernel, SendCap, TIMEOUT_NONE,
};
use aerosls_proto::{CH_KIND_CLOSE, CH_KIND_MSG, CLOSE_PEER};

/// Channel operations errors.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelError {
    /// Kernel returned an error code.
    Kernel(i32),
    /// Peer closed the channel (reason, detail).
    Closed(u16, u32),
    /// A finite-deadline wait elapsed with nothing ready (the kernel woke
    /// the parked wait at its deadline and the re-run returned
    /// `ERR_TIMEOUT`) — the caller retries or moves on.
    Timeout,
    /// Received a message but the payload was too short for the expected type.
    PayloadTooShort,
    /// Received an unexpected message kind (not MSG).
    UnexpectedKind(u16),
    /// Received a message with an unexpected tag for the expected protocol.
    UnexpectedTag(u32),
}

impl core::fmt::Display for ChannelError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            ChannelError::Kernel(code) => write!(f, "kernel error {code}"),
            ChannelError::Closed(reason, detail) => {
                write!(f, "channel closed (reason={reason}, detail={detail})")
            }
            ChannelError::Timeout => write!(f, "reply deadline elapsed"),
            ChannelError::PayloadTooShort => write!(f, "payload too short"),
            ChannelError::UnexpectedKind(k) => write!(f, "unexpected message kind {k}"),
            ChannelError::UnexpectedTag(t) => write!(f, "unexpected message tag 0x{t:08x}"),
        }
    }
}

/// A channel endpoint with convenience methods.
pub struct InitChannel<K: Kernel> {
    k: K,
    /// The kernel handle for this endpoint.
    pub handle: u32,
}

impl<K: Kernel> InitChannel<K> {
    pub fn new(k: K, handle: u32) -> Self {
        Self { k, handle }
    }

    /// The underlying kernel (for callers that need to wait on this
    /// endpoint directly, e.g. the demo event loop).
    pub(crate) fn kernel(&self) -> &K {
        &self.k
    }

    /// Introspect this endpoint's capability.
    pub fn info(&self) -> Result<CapInfo, ChannelError> {
        self.k
            .cap_info(self.handle)
            .map_err(ChannelError::Kernel)
    }

    /// Send a message with no caps and wait for a reply.
    ///
    /// The send is BLOCKING (timeout_ns = 0, transport spec §3.4): if the
    /// peer's queue is full, the kernel parks this sidecar (cap_wait_chans,
    /// park_syscall = SYS_SLS_CHAN_SEND) and a recv freeing a slot wakes it
    /// to re-run the send — a slow peer backpressures init, nothing is
    /// dropped. Console logs use this path (fire-and-forget, no reply).
    pub fn request(&self, tag: u32, payload: &[u8]) -> Result<(), ChannelError> {
        self.k
            .send(self.handle, tag, 0, payload, &[], 0)
            .map_err(ChannelError::Kernel)
    }

    /// Send a message carrying a MEM cap (e.g. device registry snapshot).
    /// Blocking send, same semantics as `request`.
    pub fn send_with_cap(
        &self,
        tag: u32,
        payload: &[u8],
        cap: &SendCap,
    ) -> Result<(), ChannelError> {
        self.k
            .send(self.handle, tag, 0, payload, core::slice::from_ref(cap), 0)
            .map_err(ChannelError::Kernel)
    }

    /// Wait for a message (blocking): the inner `k_chan_wait` with
    /// TIMEOUT_NONE parks this sidecar until a message or control event is
    /// queued (the demo event loop's blocking park).
    pub fn recv_msg(&self, buf: &mut [u8]) -> Result<u32, ChannelError> {
        let mut chans = [self.handle];
        let (idx, kind) = self
            .k
            .wait(&mut chans, TIMEOUT_NONE)
            .map_err(ChannelError::Kernel)?;
        if idx != 0 {
            return Err(ChannelError::Kernel(-1));
        }
        match kind {
            CH_KIND_MSG => {}
            _ if kind == CH_KIND_CLOSE => {
                let (reason, detail) = self.recv_close()?;
                return Err(ChannelError::Closed(reason, detail));
            }
            other => return Err(ChannelError::UnexpectedKind(other)),
        }
        let result = self
            .k
            .recv(self.handle, buf, &mut [GrantedCap::default(); 4])
            .map_err(ChannelError::Kernel)?;
        // The tag is the protocol discriminator and must survive even an
        // empty payload (e.g. the "devices ready" signal has no body).
        Ok(result.tag)
    }

    /// Wait for a message with a FINITE deadline: the `k_chan_wait` parks
    /// with an absolute deadline and the kernel wakes it at the deadline
    /// (timer ISR) whose re-run returns `ERR_TIMEOUT` — mapped to
    /// `ChannelError::Timeout`. This is the handshake path: a slow peer is
    /// handled by the deadline + bounded retries, never unbounded blocking.
    pub fn recv_msg_deadline(
        &self,
        buf: &mut [u8],
        deadline_ns: u64,
    ) -> Result<u32, ChannelError> {
        let mut chans = [self.handle];
        let (idx, kind) = self
            .k
            .wait(&mut chans, deadline_ns)
            .map_err(|code| {
                if code == aerosls_proto::kabi::ERR_TIMEOUT {
                    ChannelError::Timeout
                } else {
                    ChannelError::Kernel(code)
                }
            })?;
        if idx != 0 {
            return Err(ChannelError::Kernel(-1));
        }
        match kind {
            CH_KIND_MSG => {}
            _ if kind == CH_KIND_CLOSE => {
                let (reason, detail) = self.recv_close()?;
                return Err(ChannelError::Closed(reason, detail));
            }
            other => return Err(ChannelError::UnexpectedKind(other)),
        }
        let result = self
            .k
            .recv(self.handle, buf, &mut [GrantedCap::default(); 4])
            .map_err(ChannelError::Kernel)?;
        // Same as `recv_msg`: the tag survives an empty payload.
        Ok(result.tag)
    }

    /// Drain a queued CLOSE event (the 8-byte body: reason u16 LE, detail
    /// u32 LE, pad 2) after the peer closed or died.
    pub fn recv_close(&self) -> Result<(u16, u32), ChannelError> {
        let mut close_buf = [0u8; 8];
        let _result = self
            .k
            .recv(self.handle, &mut close_buf, &mut [GrantedCap::default(); 4])
            .map_err(ChannelError::Kernel)?;
        let reason = u16::from_le_bytes([close_buf[0], close_buf[1]]);
        let detail = u32::from_le_bytes([close_buf[2], close_buf[3], close_buf[4], close_buf[5]]);
        Ok((reason, detail))
    }

    /// Send a CLOSE event.
    pub fn close(&self) -> Result<(), ChannelError> {
        self.k
            .close(self.handle, CLOSE_PEER, 0)
            .map_err(ChannelError::Kernel)
    }
}

/// Protocol message IDs for init ↔ Device Manager communication.
pub const MSG_DEVICE_REGISTRY: u32 = 0x0001;
pub const MSG_DEVICES_READY: u32 = 0x0002;
pub const MSG_SPAWN_POSIX: u32 = 0x0003;
pub const MSG_POSIX_READY: u32 = 0x0004;
