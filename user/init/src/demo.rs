//! The Phase 5 demo server loop — the "real sidecar workload" that proves
//! the blocking transport end to end (kernel/chan.c + process.c's
//! park/resume, transport spec §3):
//!
//! - **Blocking `k_chan_wait` (TIMEOUT_NONE)** — the event loop parks this
//!   sidecar (cap_wait_chans, park_syscall = SYS_SLS_CHAN_WAIT) until the
//!   Device Manager queues a message or a control event; a wake re-runs
//!   the wait, which returns the event. The sidecar never busy-polls.
//! - **Blocking `k_chan_send` (timeout 0)** — console output and the
//!   device-registry handshake send with timeout 0: a full peer queue
//!   parks the sender (park_syscall = SYS_SLS_CHAN_SEND) and a recv
//!   freeing a slot wakes it to re-run the send. A slow peer backpressures
//!   init; nothing is dropped.
//! - **Finite reply deadline** — the Device Manager "devices ready"
//!   handshake waits with an absolute deadline: the timer ISR wakes the
//!   parked wait when the deadline passes and the re-run returns
//!   CAP_ERR_TIMEOUT, so a wedged DM is handled by the deadline + bounded
//!   retries instead of unbounded blocking.
//! - **Watchdog respawn (self-healing)** — when the DM channel closes
//!   (`CLOSE_PEER_DEAD` from the teardown scan, or an explicit close),
//!   `run_resilient_loop` sleeps a bounded backoff (`RespawnPolicy`:
//!   base × 2^(n−1), capped) using the deadline-wait-as-sleep trick, then
//!   respawns a fresh DM and re-enters the loop on the new channel. The
//!   restart budget is bounded (crash-loop breaker: `TooManyRestarts`).
//!
//! The whole module is generic over `Kernel`, so the exact same code runs
//! against `RealKernel` on the real machine (via `entry.rs`) and against
//! the host `SimKernel` in tests (which model the kernel's blocking and
//! deadline behavior).

use crate::chan::{ChannelError, InitChannel, MSG_DEVICES_READY};
use aerosls_proto::kabi::{Kernel, SendCap, TIMEOUT_NONE};
use aerosls_proto::CH_KIND_CLOSE;

/// Reply deadline for the Device Manager handshake: 1 s — 100 kernel ticks
/// at the documented ~10 ms KERNEL_TICK_NS. The kernel rounds a deadline
/// UP to whole ticks, parks the wait, and wakes it with CAP_ERR_TIMEOUT
/// when the deadline passes.
pub const DM_READY_DEADLINE_NS: u64 = 1_000_000_000;

/// Bounded handshake retries before init parks in the event loop: a
/// wedged DM costs at most retries × deadline, never an unbounded block.
pub const DM_READY_RETRIES: u32 = 3;

/// Send the device registry snapshot to the Device Manager with a
/// BLOCKING send (timeout 0): if the DM's queue is full, the kernel parks
/// this sidecar until a slot frees — the handshake cannot drop the
/// registry.
pub fn send_registry<K: Kernel>(
    dm: &InitChannel<K>,
    tag: u32,
    payload: &[u8],
    cap: &SendCap,
) -> Result<(), ChannelError> {
    dm.send_with_cap(tag, payload, cap)
}

/// Wait for the DM's "devices ready" reply with a FINITE deadline. The
/// wait parks with the absolute deadline; if the DM does not reply in
/// time, the kernel wakes the park at the deadline and the re-run returns
/// CAP_ERR_TIMEOUT (mapped to `ChannelError::Timeout`) — the caller
/// retries or moves on.
pub fn wait_devices_ready<K: Kernel>(
    dm: &InitChannel<K>,
    buf: &mut [u8],
    deadline_ns: u64,
) -> Result<(), ChannelError> {
    match dm.recv_msg_deadline(buf, deadline_ns) {
        Ok(tag) if tag == MSG_DEVICES_READY => Ok(()),
        Ok(tag) => Err(ChannelError::UnexpectedTag(tag)),
        Err(e) => Err(e),
    }
}

/// Outcome of one event-loop iteration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventOutcome {
    /// A Device Manager notification was handled; keep blocking for the
    /// next event.
    Continue,
    /// The DM channel closed (explicit close or peer death).
    Closed(u16, u32),
    /// The DM channel misbehaved (kernel error, unexpected kind/tag).
    Err(ChannelError),
}

/// One iteration of the demo server loop: `k_chan_wait` (TIMEOUT_NONE —
/// the blocking park) on the DM channel, then dispatch the event. A MSG
/// is a DM notification, logged with a BLOCKING send (timeout 0 — a full
/// console queue parks the sender until the kernel console drains it); a
/// CLOSE is surfaced so the caller can respawn the DM.
pub fn dispatch_event<K: Kernel>(
    console: &InitChannel<K>,
    dm: &InitChannel<K>,
    buf: &mut [u8],
) -> EventOutcome {
    // k_chan_wait (TIMEOUT_NONE): the park machinery suspends this sidecar
    // until a message or control event is queued. The real kernel never
    // returns "no event" from this call (it blocks); the host fake returns
    // CH_KIND_NONE when nothing is queued so a single-iteration test sees
    // "no event" without hanging.
    let mut chans = [dm.handle];
    let (idx, kind) = match dm.kernel().wait(&mut chans, TIMEOUT_NONE) {
        Ok(v) => v,
        Err(code) => return EventOutcome::Err(ChannelError::Kernel(code)),
    };
    if idx != 0 {
        return EventOutcome::Err(ChannelError::Kernel(-1));
    }
    match kind {
        aerosls_proto::CH_KIND_MSG => match dm.recv_msg(buf) {
            Ok(_tag) => {
                // Blocking send (timeout 0): a full console queue parks
                // the sender; the kernel console's drain frees it.
                let _ = console.request(0, b"[INIT] Device Manager notification");
                EventOutcome::Continue
            }
            Err(e) => EventOutcome::Err(e),
        },
        CH_KIND_CLOSE => match dm.recv_close() {
            Ok((reason, detail)) => EventOutcome::Closed(reason, detail),
            Err(e) => EventOutcome::Err(e),
        },
        other => EventOutcome::Err(ChannelError::UnexpectedKind(other)),
    }
}

/// The demo server loop: block (TIMEOUT_NONE park) on the Device Manager
/// channel, dispatch notifications, and exit only when the DM channel
/// closes (a real init would respawn the DM from here).
pub fn run_event_loop<K: Kernel>(
    console: &InitChannel<K>,
    dm: &InitChannel<K>,
) -> Result<(), ChannelError> {
    let mut buf = [0u8; 128];
    loop {
        match dispatch_event(console, dm, &mut buf) {
            EventOutcome::Continue => {}
            EventOutcome::Closed(_reason, _detail) => return Ok(()),
            EventOutcome::Err(e) => return Err(e),
        }
    }
}

/// Bounded-restart policy for the Device Manager watchdog (crash-loop
/// breaker): each death costs `backoff_for(restart)` ns of sleep before a
/// respawn, and after `max_restarts` consecutive deaths the loop gives up
/// with `ChannelError::TooManyRestarts` instead of spinning forever.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RespawnPolicy {
    /// First backoff delay (ns), doubled per restart.
    pub base_backoff_ns: u64,
    /// Backoff cap (ns) — exponential growth saturates here.
    pub max_backoff_ns: u64,
    /// Restart budget before the loop returns `TooManyRestarts`.
    pub max_restarts: u32,
}

impl Default for RespawnPolicy {
    fn default() -> Self {
        Self {
            base_backoff_ns: 100_000_000,  // 100 ms
            max_backoff_ns: 5_000_000_000, // 5 s cap
            max_restarts: 5,
        }
    }
}

impl RespawnPolicy {
    /// The backoff delay for the `restart`-th respawn (1-based):
    /// base × 2^(restart-1), saturating at `max_backoff_ns`. A crash loop
    /// therefore ramps 100 ms → 200 ms → 400 ms → … → 5 s, never more.
    pub fn backoff_for(&self, restart: u32) -> u64 {
        if restart == 0 {
            return self.base_backoff_ns;
        }
        let shift = (restart - 1).min(62);
        self.base_backoff_ns
            .saturating_mul(1u64 << shift)
            .min(self.max_backoff_ns)
    }
}

/// Sleep `ns` before a respawn. There is no sleep syscall: the kernel parks
/// this sidecar on the DEAD peer's channel with the finite deadline (the
/// close event was already consumed, so nothing is ready), and the timer
/// ISR wakes the park when the deadline passes — the re-run then returns
/// ERR_TIMEOUT. In the host sim, a finite deadline with nothing ready is
/// ERR_TIMEOUT immediately, so tests do not actually wait.
pub fn backoff_sleep<K: Kernel>(dm: &InitChannel<K>, ns: u64) {
    let _ = dm.kernel().wait(&mut [dm.handle], ns);
}

/// The watchdog-respawned demo loop — the self-healing path (Phase 5
/// reliability): block (TIMEOUT_NONE park) on the Device Manager channel;
/// when the DM dies (`CLOSE_PEER_DEAD` from the teardown scan, or an
/// explicit close), sleep the bounded backoff, respawn a fresh DM via
/// `respawn(restart)`, and re-enter the loop on the new channel. When the
/// restart budget is exhausted the loop returns `TooManyRestarts` — the
/// crash-loop breaker.
pub fn run_resilient_loop<K: Kernel, F>(
    console: &InitChannel<K>,
    mut dm: InitChannel<K>,
    policy: &RespawnPolicy,
    mut respawn: F,
) -> Result<(), ChannelError>
where
    F: FnMut(u32) -> Result<InitChannel<K>, ChannelError>,
{
    let mut buf = [0u8; 128];
    let mut restarts: u32 = 0;
    loop {
        match dispatch_event(console, &dm, &mut buf) {
            EventOutcome::Continue => {}
            EventOutcome::Closed(_reason, _detail) => {
                if restarts >= policy.max_restarts {
                    return Err(ChannelError::TooManyRestarts);
                }
                backoff_sleep(&dm, policy.backoff_for(restarts + 1));
                dm = respawn(restarts + 1)?;
                restarts += 1;
            }
            EventOutcome::Err(e) => return Err(e),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::chan::MSG_DEVICE_REGISTRY;
    use crate::sim::{SharedKernel, SimKernel};
    use aerosls_proto::kabi::{CapInfo, CAP_CHAN};
    use aerosls_proto::{CH_KIND_NONE, CLOSE_PEER_DEAD};
    use alloc::vec::Vec;

    /// Build a shared sim with CHAN caps registered on the given handles
    /// (the kernel would mint these from the manifest's CHAN records; the
    /// sim registers them explicitly before sharing).
    fn shared_with_chans(handles: &[u32]) -> SharedKernel {
        let mut sim = SimKernel::new();
        for &handle in handles {
            sim.register_cap(
                handle,
                CapInfo {
                    ty: CAP_CHAN,
                    rights: 0x0003, // R | W
                    flags: 0,
                    base: 0,
                    len: 0,
                },
            );
        }
        SharedKernel::from_sim(sim)
    }

    #[test]
    fn blocking_wait_delivers_dm_notification_and_console_log() {
        // Two endpoints over ONE kernel instance — the demo sidecar holds
        // both the console channel and the Device Manager channel.
        let k = shared_with_chans(&[1, 2]); // 1 = console, 2 = Device Manager
        let console = InitChannel::new(k.clone(), 1);
        let dm = InitChannel::new(k.clone(), 2);

        // The real kernel enqueues this message and WAKES the parked wait
        // (cap_send_msg -> cap_wake_chan); the sim models the "already
        // ready" outcome, which is what the wake's re-run sees.
        k.sim().inject_msg(2, 0x77, b"device hotplug");

        let mut buf = [0u8; 128];
        let outcome = dispatch_event(&console, &dm, &mut buf);
        assert_eq!(outcome, EventOutcome::Continue);

        // The notification was consumed from the DM endpoint...
        assert_eq!(k.sim().queue_len(2), 0);
        // ...and the BLOCKING console send (timeout 0 — the park-if-full
        // path) landed on the console endpoint.
        assert_eq!(k.sim().queue_len(1), 1);
        assert_eq!(k.sim().peek_tag(1), Some(0));
    }

    #[test]
    fn peer_death_close_surfaces_and_loop_exits() {
        let k = shared_with_chans(&[1, 2]);
        let console = InitChannel::new(k.clone(), 1);
        let dm = InitChannel::new(k.clone(), 2);

        // Teardown scan (cap_table_teardown) sets close_evt with
        // CLOSE_PEER_DEAD + the dead pid; the sim models that signal.
        k.sim().inject_close(2, CLOSE_PEER_DEAD, 106);

        let mut buf = [0u8; 128];
        assert_eq!(
            dispatch_event(&console, &dm, &mut buf),
            EventOutcome::Closed(CLOSE_PEER_DEAD, 106)
        );

        // The close body is delivered EXACTLY ONCE (kernel/chan.c): a
        // second recv on the empty endpoint fails instead of re-delivering.
        assert!(dm.recv_close().is_err());

        // A full loop run exits cleanly on the close event (the sim can't
        // block forever, so re-inject the death signal; on the real kernel
        // the TIMEOUT_NONE wait simply parks until the event arrives).
        k.sim().inject_close(2, CLOSE_PEER_DEAD, 106);
        assert_eq!(run_event_loop(&console, &dm), Ok(()));
    }

    #[test]
    fn devices_ready_reply_within_deadline() {
        let k = shared_with_chans(&[2]);
        let dm = InitChannel::new(k.clone(), 2);

        k.sim().inject_msg(2, MSG_DEVICES_READY, b"");
        let mut buf = [0u8; 128];
        assert_eq!(
            wait_devices_ready(&dm, &mut buf, DM_READY_DEADLINE_NS),
            Ok(())
        );
    }

    #[test]
    fn deadline_elapses_with_timeout() {
        let k = shared_with_chans(&[2]);
        let dm = InitChannel::new(k.clone(), 2);

        // Nothing queued: the real kernel wakes the parked wait at its
        // absolute deadline (timer ISR -> cap_park_deadline_tick) and the
        // re-run returns ERR_TIMEOUT; the sim models that as a finite
        // deadline with nothing ready -> ERR_TIMEOUT.
        let mut buf = [0u8; 128];
        assert_eq!(
            wait_devices_ready(&dm, &mut buf, DM_READY_DEADLINE_NS),
            Err(ChannelError::Timeout)
        );
    }

    #[test]
    fn blocking_registry_send_lands_on_dm_endpoint() {
        let k = shared_with_chans(&[2]);
        let dm = InitChannel::new(k.clone(), 2);

        let cap = SendCap {
            slot: 5,
            offset: 0,
            len: 64,
            rights: 0x01,
            flags: 0,
        };
        let payload = 2u32.to_le_bytes(); // 2 devices

        // Blocking send (timeout 0): a full DM queue would park the sender
        // (SYS_SLS_CHAN_SEND); the sim's queue is never full, so the send
        // lands immediately.
        send_registry(&dm, MSG_DEVICE_REGISTRY, &payload, &cap).unwrap();
        assert_eq!(k.sim().peek_tag(2), Some(MSG_DEVICE_REGISTRY));

        // The DM reads it back with the payload intact.
        let mut buf = [0u8; 4];
        let tag = dm.recv_msg(&mut buf).unwrap();
        assert_eq!(tag, MSG_DEVICE_REGISTRY);
        assert_eq!(buf, payload);
    }

    #[test]
    fn wrong_reply_tag_is_rejected() {
        let k = shared_with_chans(&[2]);
        let dm = InitChannel::new(k.clone(), 2);

        k.sim().inject_msg(2, 0xDEAD, b"");
        let mut buf = [0u8; 128];
        assert_eq!(
            wait_devices_ready(&dm, &mut buf, DM_READY_DEADLINE_NS),
            Err(ChannelError::UnexpectedTag(0xDEAD))
        );
    }

    #[test]
    fn backoff_policy_doubles_and_caps() {
        // base 10 ns, cap 40 ns: 1→10, 2→20, 3→40, 4→40 (saturated).
        let p = RespawnPolicy {
            base_backoff_ns: 10,
            max_backoff_ns: 40,
            max_restarts: 5,
        };
        assert_eq!(p.backoff_for(1), 10);
        assert_eq!(p.backoff_for(2), 20);
        assert_eq!(p.backoff_for(3), 40);
        assert_eq!(p.backoff_for(4), 40);
        // Never wraps even at absurd restart counts.
        assert_eq!(p.backoff_for(u32::MAX), 40);
    }

    #[test]
    fn resilient_loop_respawns_and_serves_new_channel() {
        // ch1 = console, ch2 = DM, ch3 = the respawned DM's channel.
        let k = shared_with_chans(&[1, 2, 3]);
        let console = InitChannel::new(k.clone(), 1);
        let dm = InitChannel::new(k.clone(), 2);

        // The DM dies (teardown scan emits CLOSE_PEER_DEAD + the dead pid).
        k.sim().inject_close(2, CLOSE_PEER_DEAD, 42);

        // The respawn closure models a fresh DM coming up: it mints the
        // new channel AND sends a notification on it immediately (a real
        // DM's first act after init's registry handshake).
        let policy = RespawnPolicy {
            base_backoff_ns: 10,
            max_backoff_ns: 40,
            max_restarts: 3,
        };
        let mut respawns = 0;
        let mut attempts = Vec::new();
        let result = run_resilient_loop(&console, dm, &policy, |attempt| {
            respawns += 1;
            attempts.push(attempt);
            k.sim().inject_msg(3, 0x99, b"alive after respawn");
            Ok(InitChannel::new(k.clone(), 3))
        });

        // Exactly one respawn happened, with the 1-based attempt number.
        assert_eq!(respawns, 1);
        assert_eq!(attempts, vec![1]);

        // The new channel's notification was DISPATCHED through the live
        // loop: the console endpoint received the notification log (the
        // blocking send after dispatch).
        assert_eq!(k.sim().queue_len(1), 1);
        assert_eq!(k.sim().peek_tag(1), Some(0));

        // After the notification the new channel is empty; the sim cannot
        // block forever, so the next TIMEOUT_NONE wait reports CH_KIND_NONE
        // (the real kernel parks). The loop surfaces that as the sim's
        // documented non-blocking outcome — the respawn path itself
        // already succeeded.
        assert_eq!(result, Err(ChannelError::UnexpectedKind(CH_KIND_NONE)));
    }

    #[test]
    fn resilient_loop_crash_loop_breaker() {
        // ch1 = console, ch2 = DM, ch3 = respawn target.
        let k = shared_with_chans(&[1, 2, 3]);
        let console = InitChannel::new(k.clone(), 1);
        let dm = InitChannel::new(k.clone(), 2);

        // DM dies once, respawns to ch3, and the respawned DM ALSO dies
        // immediately (a crash loop) — the restart budget (1) is exhausted
        // and the loop gives up instead of respawning forever.
        k.sim().inject_close(2, CLOSE_PEER_DEAD, 42);
        let policy = RespawnPolicy {
            base_backoff_ns: 10,
            max_backoff_ns: 40,
            max_restarts: 1,
        };
        let mut respawns = 0;
        let result = run_resilient_loop(&console, dm, &policy, |attempt| {
            respawns += 1;
            assert_eq!(attempt, 1);
            k.sim().inject_close(3, CLOSE_PEER_DEAD, 43);
            Ok(InitChannel::new(k.clone(), 3))
        });

        assert_eq!(respawns, 1);
        assert_eq!(result, Err(ChannelError::TooManyRestarts));
    }
}
