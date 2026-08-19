//! The kernel ABI for sidecars, expressed as a trait.
//!
//! Sidecar cores (the ramdisk driver, the POSIX sidecar's block cache, ...)
//! are written against `Kernel` (static dispatch), which makes them
//! host-testable: `aerosls-kernel-sim` implements the same semantics as a
//! fake, and the real ABI (`RealKernel` in the ramdisk crate) is selected by
//! that crate's `target` feature.
//!
//! The extern "C" declarations in the ramdisk crate's `kapi` are the *kernel
//! side of the contract* that `kernel/cap.c` and `kernel/chan.c` must
//! implement per `docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md` and
//! `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`.

/// Capability types (capability-layer spec §1.1, respawn decision §2).
pub const CAP_MEM: u16 = 1;
pub const CAP_CHAN: u16 = 2;
pub const CAP_SPAWN: u16 = 3;

/// Kernel error codes (capability-layer spec §7 + transport spec §7).
pub const ERR_OK: i32 = 0;
pub const ERR_NOTFOUND: i32 = 1;
pub const ERR_REVOKED: i32 = 2;
pub const ERR_RIGHTS: i32 = 3;
pub const ERR_RANGE: i32 = 4;
pub const ERR_BUDGET: i32 = 5;
pub const ERR_SPACE: i32 = 6;
pub const ERR_TARGET: i32 = 7;
pub const ERR_STATE: i32 = 8;
pub const ERR_TYPE: i32 = 9;
pub const ERR_PROTO: i32 = 10;
pub const ERR_NOMEM: i32 = 11;
pub const ERR_BUFSZ: i32 = 12;
pub const ERR_TIMEOUT: i32 = 13;
/// Fake-kernel only: the sidecar was torn down (driver "death").
pub const ERR_SHUTDOWN: i32 = 14;

/// Block forever (real kernel) / wait without a deadline.
pub const TIMEOUT_NONE: u64 = u64::MAX;

/// A capability a sidecar received on a message, already minted in its own
/// table by the kernel (capability-layer spec §4.1).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GrantedCap {
    /// Handle in the receiver's table.
    pub handle: u32,
    /// Rights the kernel actually minted (`min(held, requested)`).
    pub rights: u8,
    /// Base of the sub-region (sender base + offset).
    pub base: u64,
    /// Length of the sub-region.
    pub len: u64,
}

/// A capability argument to attach to a message a sidecar sends.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SendCap {
    /// Handle in the *sender's* table.
    pub slot: u32,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    /// `CAP_PERSIST | CAP_MOVE | CAP_MAP` from `aerosls_proto`.
    pub flags: u8,
}

/// Result of a receive: a message, a close event, or a NEW_CHANNEL event.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RecvResult {
    /// `CH_KIND_MSG | CH_KIND_CLOSE | CH_KIND_NEW_CHANNEL`.
    pub kind: u16,
    /// Envelope flags (`F_REPLY | F_NO_REPLY`).
    pub flags: u16,
    /// The request id (echoed in replies).
    pub tag: u32,
    /// Payload bytes written into the buffer.
    pub len: usize,
    /// Granted caps written into the slots array.
    pub n_caps: usize,
}

/// `cap_info` result (capability-layer spec §3.5).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CapInfo {
    pub ty: u16,
    pub rights: u16,
    pub flags: u16,
    pub base: u64,
    pub len: u64,
}

/// The kernel operations a sidecar can perform. All blocking calls honor
/// either a timeout or a close event; a wedged peer can never wedge the
/// caller (transport spec §4 guarantee 7).
pub trait Kernel {
    /// Block until a message or control event is queued on any of `chans`,
    /// or `timeout_ns` elapses. Returns the index into `chans` and the kind
    /// of the head entry. `TIMEOUT_NONE` blocks forever.
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32>;

    /// Receive one message/event from `chan`, filling `buf` (payload) and
    /// `slots` (granted caps). A failed recv (undersize, `ERR_BUFSZ`) does
    /// **not** consume the message (transport spec §3.4).
    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<RecvResult, i32>;

    /// Send one message. `caps` are validated against the sender's table and
    /// minted (derived) in the receiver's table — never amplified.
    fn send(
        &self,
        chan: u32,
        tag: u32,
        flags: u16,
        payload: &[u8],
        caps: &[SendCap],
        timeout_ns: u64,
    ) -> Result<(), i32>;

    /// Close an endpoint with a reason; idempotent (transport spec §6.1).
    fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32>;

    /// Introspect one of the caller's own caps (capability-layer spec §3.5).
    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32>;

    /// Non-blocking peek: is a message or control event already queued on
    /// `chan`? Returns the head's kind (`CH_KIND_*`, or `CH_KIND_NONE`).
    /// Peeks only — the event is consumed by the next `recv`. This is how
    /// a sidecar learns a peer died *without* an outstanding request
    /// (respawn decision §5 steps 1–3: the kernel queues the close and the
    /// block cache polls before serving a cache hit — a cache hit must
    /// never come from a dead device). Default: nothing queued, for
    /// implementations that never poll.
    fn poll(&self, chan: u32) -> Result<u16, i32> {
        let _ = chan;
        Ok(crate::CH_KIND_NONE)
    }
}
