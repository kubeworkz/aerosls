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
    /// Received a message but the payload was too short for the expected type.
    PayloadTooShort,
    /// Received an unexpected message kind (not MSG).
    UnexpectedKind(u16),
}

impl core::fmt::Display for ChannelError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            ChannelError::Kernel(code) => write!(f, "kernel error {code}"),
            ChannelError::Closed(reason, detail) => {
                write!(f, "channel closed (reason={reason}, detail={detail})")
            }
            ChannelError::PayloadTooShort => write!(f, "payload too short"),
            ChannelError::UnexpectedKind(k) => write!(f, "unexpected message kind {k}"),
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

    /// Introspect this endpoint's capability.
    pub fn info(&self) -> Result<CapInfo, ChannelError> {
        self.k
            .cap_info(self.handle)
            .map_err(ChannelError::Kernel)
    }

    /// Send a message with no caps and wait for a reply.
    pub fn request(&self, tag: u32, payload: &[u8]) -> Result<(), ChannelError> {
        self.k
            .send(self.handle, tag, 0, payload, &[], TIMEOUT_NONE)
            .map_err(ChannelError::Kernel)
    }

    /// Send a message carrying a MEM cap (e.g. device registry snapshot).
    pub fn send_with_cap(
        &self,
        tag: u32,
        payload: &[u8],
        cap: &SendCap,
    ) -> Result<(), ChannelError> {
        self.k
            .send(self.handle, tag, 0, payload, core::slice::from_ref(cap), TIMEOUT_NONE)
            .map_err(ChannelError::Kernel)
    }

    /// Wait for a message (blocking).
    pub fn recv_msg(&self, buf: &mut [u8]) -> Result<u32, ChannelError> {
        let mut caps = [GrantedCap::default(); 4];
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
                let mut close_buf = [0u8; 8];
                let _result = self
                    .k
                    .recv(self.handle, &mut close_buf, &mut caps)
                    .map_err(ChannelError::Kernel)?;
                let reason = u16::from_le_bytes([close_buf[0], close_buf[1]]);
                let detail = u32::from_le_bytes([close_buf[2], close_buf[3], close_buf[4], close_buf[5]]);
                return Err(ChannelError::Closed(reason, detail));
            }
            other => return Err(ChannelError::UnexpectedKind(other)),
        }
        let result = self
            .k
            .recv(self.handle, buf, &mut caps)
            .map_err(ChannelError::Kernel)?;
        if result.len == 0 {
            return Ok(0);
        }
        Ok(result.tag)
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
