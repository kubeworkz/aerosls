//! The Device Manager server core — the registry handshake and the event
//! loop, generic over `Kernel` so the exact same code runs against
//! `RealKernel` on the real machine (via `entry.rs`) and against the host
//! `FakeKernel` in `tests/handshake.rs`.
//!
//! The DM's job (Phase 5 §2, §1.5): init spawns it through the real
//! `k_create_sidecar` path and sends the device registry over the
//! messenger channel — the count (`u32` LE) as the payload plus the
//! registry table as a read-only MEM cap grant. The DM:
//!
//! 1. waits (blocking `k_chan_wait`, TIMEOUT_NONE — the park machinery)
//!    on its messenger receive end;
//! 2. receives `MSG_DEVICE_REGISTRY`, parses the registry from the
//!    granted cap (same `devreg.rs` wire format the kernel populated at
//!    boot and init read to log its devices);
//! 3. replies `MSG_DEVICES_READY` on the messenger send end — the reply
//!    init's finite-deadline handshake waits for;
//! 4. keeps serving the messenger (future messages: `MSG_SPAWN_POSIX`,
//!    ...), parking between events, until the channel closes (init death)
//!    — a respawned DM is a fresh process, so the loop then just ends.
//!
//! The grant lifecycle needs no handling here: the devreg cap init granted
//! stays in the DM's table for the channel's lifetime (transient grants die
//! with the channel on close), and this sidecar never sends caps itself.
//!
//! v1 scope: the DM adopts devices (logs what it would spawn) and replies
//! ready. Actually spawning the NVMe/e1000 driver sidecars per the registry
//! is the composition milestone (Phase 5 §2.2 — manifest path 2 channels
//! wired by the DM) and is future work, deliberately not faked here.

use aerosls_proto::devreg::{DevRegError, DeviceRegistry};
use aerosls_proto::kabi::{
    GrantedCap, Kernel, TIMEOUT_NONE, CAP_NONE, ERR_TIMEOUT,
};
use aerosls_proto::{CH_KIND_CLOSE, CH_KIND_MSG, R};

/// Protocol message IDs — must match `user/init/src/chan.rs` (the init
/// side of the same handshake). Kept in sync by hand, like every
/// cross-crate constant in this tree (the aeroidl-consistency gate greps
/// the kernel-facing ones).
pub const MSG_DEVICE_REGISTRY: u32 = 0x0001;
pub const MSG_DEVICES_READY: u32 = 0x0002;
pub const MSG_SPAWN_POSIX: u32 = 0x0003;
pub const MSG_POSIX_READY: u32 = 0x0004;

/// Server failures. A wedged init can never wedge the DM: a close event
/// or a malformed message is an error/exit, never a hang.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DmError {
    /// Kernel returned an error code.
    Kernel(i32),
    /// The messenger channel closed (init died or moved on). Carries the
    /// close reason + detail from the close body.
    Closed(u16, u32),
    /// A message arrived with an unexpected kind (not MSG / CLOSE).
    UnexpectedKind(u16),
    /// A message arrived with an unexpected tag for the protocol.
    UnexpectedTag(u32),
    /// The registry message's payload was shorter than the 4-byte count.
    PayloadTooShort,
    /// The registry message carried no MEM cap grant.
    MissingRegistryCap,
    /// The granted cap does not carry the READ right — refuse to touch it.
    RegistryCapNotReadable,
    /// The registry table at the granted cap is malformed.
    BadRegistry(DevRegError),
}

impl core::fmt::Display for DmError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            DmError::Kernel(code) => write!(f, "kernel error {code}"),
            DmError::Closed(reason, detail) => {
                write!(f, "messenger closed (reason={reason}, detail={detail})")
            }
            DmError::UnexpectedKind(k) => write!(f, "unexpected message kind {k}"),
            DmError::UnexpectedTag(t) => write!(f, "unexpected message tag 0x{t:08x}"),
            DmError::PayloadTooShort => write!(f, "registry payload too short"),
            DmError::MissingRegistryCap => write!(f, "registry message without a MEM cap"),
            DmError::RegistryCapNotReadable => {
                write!(f, "registry cap lacks the READ right")
            }
            DmError::BadRegistry(e) => write!(f, "bad device registry: {e}"),
        }
    }
}

/// One server event's outcome — what `serve_one` did, observable by tests.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DmOutcome {
    /// The registry handshake was served: `devices` entries adopted and
    /// `MSG_DEVICES_READY` replied.
    RegistryServed { devices: usize },
    /// A `k_chan_wait` under TIMEOUT_NONE reported the caller could not
    /// park (the real kernel returns CAP_ERR_TIMEOUT when every process is
    /// parked and nothing is runnable to switch to — a boot-time artifact,
    /// not a dead peer). This is NOT a server failure: the loop retries.
    /// Only reachable in real-kernel boots where the scheduler hands
    /// TIMEOUT back for an already-idle wait; host sims never see it.
    Idle,
    /// A future protocol message was acknowledged (logged, no reply — the
    /// reply tags are per-message protocol, and v1 defines none yet).
    Acknowledged { tag: u32 },
    /// The messenger channel closed; the server loop should exit.
    Closed(u16, u32),
}

/// The Device Manager sidecar: three channel ends resolved from its BIB by
/// `entry.rs` (messenger CHAN_R + CHAN_W are the two UNNAMED caps
/// `cap_create_sidecar` mints first; the console CHAN_W is the named
/// "console" cap wired to the kernel service).
pub struct DmServer<K: Kernel> {
    k: K,
    /// Send-only console channel (the kernel console service drains it).
    console_w: u32,
    /// Messenger receive end (init's registry and future requests).
    msg_r: u32,
    /// Messenger send end (the devices-ready reply).
    msg_w: u32,
}

/// A fixed stack buffer for formatted logs — the freestanding binary has
/// NO allocator, so `log_fmt!` formats into this instead of allocating.
struct LogBuf([u8; 256]);

impl core::fmt::Write for LogBuf {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(255); // byte 255 stays 0 (NUL terminator)
        self.0[..n].copy_from_slice(&s.as_bytes()[..n]);
        Ok(()) // truncates silently rather than failing
    }
}

impl LogBuf {
    fn as_str(&self) -> &str {
        let len = self.0.iter().position(|&b| b == 0).unwrap_or(256);
        core::str::from_utf8(&self.0[..len]).unwrap_or("")
    }
}

impl<K: Kernel> DmServer<K> {
    /// `console_w` may be `CAP_NONE` (a DM without console in tests); the
    /// messenger ends are required.
    pub fn new(k: K, console_w: u32, msg_r: u32, msg_w: u32) -> Self {
        Self {
            k,
            console_w,
            msg_r,
            msg_w,
        }
    }

    /// Fire-and-forget console log: blocking send (timeout 0 — a full
    /// console queue parks this sidecar until the kernel console drains
    /// it, nothing is dropped), no reply expected.
    pub fn log(&self, msg: &str) {
        if self.console_w == CAP_NONE as u32 {
            return;
        }
        let _ = self.k.send(self.console_w, 0, 0, msg.as_bytes(), &[], 0);
    }

    /// Format a log line into the stack buffer and send it (no alloc).
    pub fn log_fmt(&self, args: core::fmt::Arguments<'_>) {
        let mut b = LogBuf([0; 256]);
        let _ = core::fmt::write(&mut b, args);
        self.log(b.as_str());
    }

    /// Block (TIMEOUT_NONE park) for one event on the messenger, then serve
    /// it. Tests queue the event first so the sim's wait returns
    /// immediately; the real kernel parks until init sends.
    pub fn serve_one(
        &self,
        buf: &mut [u8],
        slots: &mut [GrantedCap; 4],
    ) -> Result<DmOutcome, DmError> {
        let mut chans = [self.msg_r];
        let (idx, kind) = match self.k.wait(&mut chans, TIMEOUT_NONE) {
            Ok(v) => v,
            Err(ERR_TIMEOUT) => {
                // `serve_one` always waits with TIMEOUT_NONE, so a returned
                // TIMEOUT can only mean the kernel could not park this
                // process (every process is blocked and nothing is runnable
                // to hand the CPU to). Yield to let peers run; the timer
                // ISR resumes us and the retry picks up any queued event.
                self.k.sched_yield();
                return Ok(DmOutcome::Idle);
            }
            Err(e) => return Err(DmError::Kernel(e)),
        };
        if idx != 0 {
            return Err(DmError::Kernel(-1));
        }
        match kind {
            CH_KIND_MSG => self.serve_msg(buf, slots),
            CH_KIND_CLOSE => {
                let (reason, detail) = self.recv_close_body(buf)?;
                Ok(DmOutcome::Closed(reason, detail))
            }
            other => Err(DmError::UnexpectedKind(other)),
        }
    }

    fn serve_msg(
        &self,
        buf: &mut [u8],
        slots: &mut [GrantedCap; 4],
    ) -> Result<DmOutcome, DmError> {
        let result = self
            .k
            .recv(self.msg_r, buf, slots)
            .map_err(DmError::Kernel)?;
        match result.tag {
            MSG_DEVICE_REGISTRY => self.serve_registry(result.len, result.n_caps, slots),
            tag => {
                // Future protocol messages: acknowledge (log) and move on.
                // No reply — v1 defines no reply tags beyond devices-ready.
                self.log_fmt(format_args!("[DM] acknowledged future message tag 0x{tag:08x}"));
                Ok(DmOutcome::Acknowledged { tag })
            }
        }
    }

    /// The registry handshake step: validate the payload (count) and the
    /// granted MEM cap, parse the table, reply devices-ready.
    fn serve_registry(
        &self,
        payload_len: usize,
        n_caps: usize,
        slots: &mut [GrantedCap; 4],
    ) -> Result<DmOutcome, DmError> {
        if payload_len < 4 {
            return Err(DmError::PayloadTooShort);
        }
        if n_caps == 0 {
            return Err(DmError::MissingRegistryCap);
        }
        let granted = slots[0];
        if granted.rights & R == 0 {
            return Err(DmError::RegistryCapNotReadable);
        }

        // The count travels both as the payload AND as the table's own
        // header (init sends both from the same devreg). The table is
        // authoritative — `from_raw_parts` validates the count against the
        // granted region's length, so a lying payload cannot overread.
        let reg = unsafe { DeviceRegistry::from_raw_parts(granted.base as *const u8, granted.len as usize) }
            .map_err(DmError::BadRegistry)?;

        // Reply devices-ready FIRST, before any console logging. The
        // adopt logs below are blocking console sends; if any of them
        // parks on a full console queue, the reply would be delayed
        // indefinitely and init's finite-deadline handshake would time out
        // (live QEMU failure). The reply is the protocol signal — get it
        // onto the messenger before spending time on diagnostic output.
        self.k
            .send(self.msg_w, MSG_DEVICES_READY, 0, &[], &[], 0)
            .map_err(DmError::Kernel)?;

        // Adopt (v1: log) the discovered devices.
        for (i, e) in reg.iter().enumerate() {
            let name = e.manifest_name().unwrap_or("?");
            self.log_fmt(format_args!(
                "[DM]   adopt [{}] {} class={:02x}:{:02x} vendor={:04x} dev={:04x} bar=0x{:08x}",
                i, name, e.class_code, e.subclass, e.vendor_id, e.device_id, e.bar0_phys
            ));
        }
        self.log_fmt(format_args!("[DM] registry: {} device(s) adopted", reg.len()));

        Ok(DmOutcome::RegistryServed { devices: reg.len() })
    }

    /// Drain the 8-byte close body after a CLOSE event.
    fn recv_close_body(&self, buf: &mut [u8]) -> Result<(u16, u32), DmError> {
        match self.k.recv(self.msg_r, buf, &mut [GrantedCap::default(); 4]) {
            Ok(_result) => {
                let reason = u16::from_le_bytes([buf[0], buf[1]]);
                let detail = u32::from_le_bytes([buf[2], buf[3], buf[4], buf[5]]);
                Ok((reason, detail))
            }
            // ERR_STATE (8): empty queue — the close event was queued
            // but the body wasn't (or was already consumed). Treat as
            // an ungraceful close with zeroed reason/detail.
            Err(8) => Ok((0, 0)),
            Err(e) => Err(DmError::Kernel(e)),
        }
    }

    /// The event loop: serve events until the messenger closes (init died),
    /// then return — `entry.rs` parks/spins after. `buf` is scratch.
    pub fn run(&self, buf: &mut [u8]) -> Result<(), DmError> {
        let mut slots = [GrantedCap::default(); 4];
        let mut idle_logged = false;
        loop {
            match self.serve_one(buf, &mut slots)? {
                DmOutcome::Closed(_reason, _detail) => return Ok(()),
                // Couldn't park (nothing runnable — idle demo boot tail):
                // note it once and keep retrying for future messages; the
                // retry is scheduler-tick-throttled, not a hot spin.
                DmOutcome::Idle => {
                    if !idle_logged {
                        self.log("[DM] messenger idle (all peers parked); waiting for events");
                        idle_logged = true;
                    }
                    // Yield the CPU so other processes (e.g. init) can run.
                    // Without this the DM busy-spins and starves peers.
                    self.k.sched_yield();
                }
                _ => {}
            }
        }
    }
}
