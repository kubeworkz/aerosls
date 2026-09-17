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
///
/// `caps[0]` is the registry table grant (read-only); `caps[1]`, when
/// present, is the e1000 driver image grant (also read-only) — the
/// DM needs the driver binary's address+size to spawn drv.e1000.0, and a
/// grant (a derived copy of init's own cap) is the only way to share a
/// physical region that is already a MEM object without minting an
/// overlapping second one.
pub fn send_registry<K: Kernel>(
    dm: &InitChannel<K>,
    tag: u32,
    payload: &[u8],
    caps: &[SendCap],
) -> Result<(), ChannelError> {
    dm.send_with_caps(tag, payload, caps)
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
    let mut chans = [dm.r];
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
    // Wait on the dead peer's RECEIVE end: the close event was already
    // consumed, so nothing is ready and the finite deadline is the sleep.
    let _ = dm.kernel().wait(&mut [dm.r], ns);
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

/// The watchdog supervisor loop — init's event loop once it supervises
/// TWO children (the Device Manager and the POSIX sidecar). Blocks
/// (TIMEOUT_NONE park) on BOTH children's receive ends; whichever queued
/// an event is handled that iteration:
///
/// - **MSG** — a child notification, logged with a blocking console send.
/// - **CLOSE** — the child died (CLOSE_PEER_DEAD from the teardown scan,
///   or an explicit close): sleep the bounded backoff (the
///   deadline-wait-as-sleep trick on the dead endpoint), respawn the
///   child via its OWN closure, and re-enter the loop on the fresh
///   channel. Each child keeps its own restart budget; when one exhausts
///   its budget the loop returns `TooManyRestarts` (crash-loop breaker).
///
/// The POSIX sidecar is the driver-death demo's watchdog subject: when
/// irqtest kills its process mid-storm, the kernel teardown frees the
/// bound vectors and closes init's messenger endpoint — the CLOSE here is
/// what triggers the respawn, and the respawned process re-binds the
/// vectors cleanly (its manifest caps are fresh).
pub fn run_supervisor_loop<K: Kernel, FA, FB, FE>(
    console: &InitChannel<K>,
    mut a: InitChannel<K>,
    pa: &RespawnPolicy,
    mut respawn_a: FA,
    mut b: InitChannel<K>,
    pb: &RespawnPolicy,
    mut respawn_b: FB,
    env: InitChannel<K>,
    mut handle_env: FE,
) -> Result<(), ChannelError>
where
    FA: FnMut(u32) -> Result<InitChannel<K>, ChannelError>,
    FB: FnMut(u32) -> Result<InitChannel<K>, ChannelError>,
    // POSIX-Environments E4: dispatch an ENV request (its exact bytes) into the
    // reply buffer, returning the reply length. The caller wires this to the
    // environment manager (EnvManager::handle_request).
    FE: FnMut(&[u8], &mut [u8]) -> usize,
{
    let mut buf = [0u8; 128];
    let mut restarts_a: u32 = 0;
    let mut restarts_b: u32 = 0;
    loop {
        let mut chans = [a.r, b.r, env.r];
        let (idx, kind) = loop {
            match console.kernel().wait(&mut chans, TIMEOUT_NONE) {
                Ok(v) => break v,
                Err(code) if code == aerosls_proto::kabi::ERR_TIMEOUT => {
                    // Nothing arrived AND the park could not happen (no
                    // other process was runnable that tick — chan.c's
                    // k_chan_wait returns CAP_ERR_TIMEOUT = "retry" for
                    // that case). Give the scheduler a turn so a woken or
                    // freshly-spawned child can run, then re-enter the
                    // wait. Fatal only for genuine errors (revoked
                    // handles, protocol faults). Without this, a
                    // momentarily-unparkable wait would kill the
                    // supervisor loop at boot and the watchdog could never
                    // respawn a crashed POSIX sidecar.
                    console.kernel().sched_yield();
                }
                Err(code) => return Err(ChannelError::Kernel(code)),
            }
        };
        if idx == 0 {
            match kind {
                aerosls_proto::CH_KIND_MSG => match a.recv_msg(&mut buf) {
                    Ok(_tag) => {
                        let _ = console.request(0, b"[INIT] Device Manager notification");
                    }
                    Err(e) => return Err(e),
                },
                CH_KIND_CLOSE => {
                    let (_reason, _detail) = a.recv_close()?;
                    if restarts_a >= pa.max_restarts {
                        return Err(ChannelError::TooManyRestarts);
                    }
                    backoff_sleep(&a, pa.backoff_for(restarts_a + 1));
                    a = respawn_a(restarts_a + 1)?;
                    restarts_a += 1;
                }
                other => return Err(ChannelError::UnexpectedKind(other)),
            }
        } else if idx == 1 {
            match kind {
                aerosls_proto::CH_KIND_MSG => match b.recv_msg(&mut buf) {
                    Ok(_tag) => {
                        let _ = console.request(0, b"[INIT] POSIX sidecar notification");
                    }
                    Err(e) => return Err(e),
                },
                CH_KIND_CLOSE => {
                    let (_reason, _detail) = b.recv_close()?;
                    if restarts_b >= pb.max_restarts {
                        return Err(ChannelError::TooManyRestarts);
                    }
                    backoff_sleep(&b, pb.backoff_for(restarts_b + 1));
                    b = respawn_b(restarts_b + 1)?;
                    restarts_b += 1;
                }
                other => return Err(ChannelError::UnexpectedKind(other)),
            }
        } else {
            // idx == 2: the environment-manager control channel — a kernel
            // service (the kernel context holds the far end), so it never
            // respawns. Dispatch the request to the env manager and send its
            // reply back with the SAME tag, so env_service_create() on the
            // kernel side matches the answer to its request.
            match kind {
                aerosls_proto::CH_KIND_MSG => match env.recv_msg_len(&mut buf) {
                    Ok((tag, len)) => {
                        let mut reply = [0u8; 64];
                        let n = handle_env(&buf[..len], &mut reply);
                        if n > 0 {
                            let _ = env.request(tag, &reply[..n]);
                        }
                    }
                    Err(e) => return Err(e),
                },
                CH_KIND_CLOSE => {
                    let _ = env.recv_close();
                    let _ = console.request(0, b"[INIT] env control channel closed");
                }
                other => return Err(ChannelError::UnexpectedKind(other)),
            }
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
        let console = InitChannel::new_single(k.clone(), 1);
        let dm = InitChannel::new_single(k.clone(), 2);

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
        let console = InitChannel::new_single(k.clone(), 1);
        let dm = InitChannel::new_single(k.clone(), 2);

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
        let dm = InitChannel::new_single(k.clone(), 2);

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
        let dm = InitChannel::new_single(k.clone(), 2);

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
        let dm = InitChannel::new_single(k.clone(), 2);

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
        send_registry(&dm, MSG_DEVICE_REGISTRY, &payload, &[cap]).unwrap();
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
        let dm = InitChannel::new_single(k.clone(), 2);

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
        let console = InitChannel::new_single(k.clone(), 1);
        let dm = InitChannel::new_single(k.clone(), 2);

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
            Ok(InitChannel::new_single(k.clone(), 3))
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
    fn supervisor_loop_respawns_each_child_independently() {
        // ch1 = console, ch2 = DM, ch3 = respawned DM,
        // ch4 = POSIX, ch5 = respawned POSIX, ch6 = env control (idle here).
        let k = shared_with_chans(&[1, 2, 3, 4, 5, 6]);
        let console = InitChannel::new_single(k.clone(), 1);
        let dm = InitChannel::new_single(k.clone(), 2);
        let posix = InitChannel::new_single(k.clone(), 4);
        let env = InitChannel::new_single(k.clone(), 6);

        // Both children die (teardown scan emits CLOSE_PEER_DEAD + pid).
        k.sim().inject_close(2, CLOSE_PEER_DEAD, 42);
        k.sim().inject_close(4, CLOSE_PEER_DEAD, 104);

        let policy = RespawnPolicy {
            base_backoff_ns: 10,
            max_backoff_ns: 40,
            max_restarts: 3,
        };
        let mut dm_respawns = 0;
        let mut posix_respawns = 0;
        let result = run_supervisor_loop(
            &console,
            dm,
            &policy,
            |attempt| {
                dm_respawns += 1;
                assert_eq!(attempt, 1);
                k.sim().inject_msg(3, 0x99, b"dm alive after respawn");
                Ok(InitChannel::new_single(k.clone(), 3))
            },
            posix,
            &policy,
            |attempt| {
                posix_respawns += 1;
                assert_eq!(attempt, 1);
                k.sim().inject_msg(5, 0x77, b"posix alive after respawn");
                Ok(InitChannel::new_single(k.clone(), 5))
            },
            env,
            |_req: &[u8], _reply: &mut [u8]| 0, // env control not exercised here
        );

        // Both children died once and were each respawned by their own
        // closure — the watchdog supervised them independently.
        assert_eq!(dm_respawns, 1);
        assert_eq!(posix_respawns, 1);

        // Both respawned children's notifications were dispatched through
        // the live loop to the console (the blocking send after dispatch).
        assert_eq!(k.sim().queue_len(1), 2);

        // With nothing else queued the sim's non-blocking TIMEOUT_NONE
        // wait reports CH_KIND_NONE, which the loop surfaces as an error
        // exactly like the single-child loop does.
        assert_eq!(result, Err(ChannelError::UnexpectedKind(CH_KIND_NONE)));
    }

    #[test]
    fn supervisor_loop_dispatches_env_request_and_replies() {
        // POSIX-Environments E4: an ENV request on the env-control channel is
        // dispatched to the handler, and its reply is sent back with the
        // request's tag. The env channel uses SEPARATE recv/send handles
        // (as the real kernel wires CHAN_R/CHAN_W), so the reply does not loop
        // back into the wait as a new request.
        // ch1 = console, ch2 = DM, ch3 = POSIX, ch4 = env recv, ch5 = env send.
        let k = shared_with_chans(&[1, 2, 3, 4, 5]);
        let console = InitChannel::new_single(k.clone(), 1);
        let dm = InitChannel::new_single(k.clone(), 2);
        let posix = InitChannel::new_single(k.clone(), 3);
        let env = InitChannel::new(k.clone(), 4, 5);

        // The kernel's env_service sends an ENV request on the env channel.
        k.sim().inject_msg(4, 0xABCD, b"env-create-request-bytes");

        let policy = RespawnPolicy::default();
        let mut seen: Vec<u8> = Vec::new();
        let result = run_supervisor_loop(
            &console,
            dm,
            &policy,
            |_| Ok(InitChannel::new_single(k.clone(), 2)),
            posix,
            &policy,
            |_| Ok(InitChannel::new_single(k.clone(), 3)),
            env,
            |req: &[u8], reply: &mut [u8]| {
                seen.extend_from_slice(req);
                reply[..5].copy_from_slice(b"REPLY");
                5
            },
        );

        // The handler saw exactly the request bytes...
        assert_eq!(&seen, b"env-create-request-bytes");
        // ...and its reply went out on the SEND handle, tagged with the
        // request's tag so env_service_create() correlates it.
        assert_eq!(k.sim().queue_len(5), 1);
        assert_eq!(k.sim().peek_tag(5), Some(0xABCD));
        // The recv handle is empty afterwards (no loopback), so the loop's next
        // wait reports the sim's non-blocking CH_KIND_NONE.
        assert_eq!(k.sim().queue_len(4), 0);
        assert_eq!(result, Err(ChannelError::UnexpectedKind(CH_KIND_NONE)));
    }

    #[test]
    fn create_sidecar_spawns_usable_messenger() {
        // The REAL entry path (entry.rs step 5): pack the DM manifest and
        // hand it to Kernel::create_sidecar. The sim mints a fresh channel
        // for the spawned DM and returns both messenger ends.
        let k = shared_with_chans(&[1]); // 1 = console
        let console = InitChannel::new_single(k.clone(), 1);

        let image = crate::dm_manifest::DmImage {
            kaddr: 0x3000_0000,
            size: 0x4000,
            entry: 0x1000,
        };
        let manifest = crate::dm_manifest::build_dm_manifest(&image);
        let (r, w) = k.create_sidecar(&manifest).unwrap();

        // A fresh handle was minted for the DM and recorded in the spawn
        // log; both ends point at it (the sim's single-handle model).
        assert_eq!(k.sim().spawn_handles(), vec![r]);
        assert_eq!(w, r);

        // The messenger is usable end to end: send the registry to the DM
        // (blocking send on w), the DM "replies" with devices-ready on r.
        let dm = InitChannel::new(k.clone(), r, w);
        let cap = SendCap {
            slot: 5,
            offset: 0,
            len: 64,
            rights: 0x01,
            flags: 0,
        };
        send_registry(&dm, MSG_DEVICE_REGISTRY, &1u32.to_le_bytes(), &[cap]).unwrap();
        assert_eq!(k.sim().peek_tag(r), Some(MSG_DEVICE_REGISTRY));
        // The DM reads the registry...
        let mut buf = [0u8; 4];
        assert_eq!(dm.recv_msg(&mut buf).unwrap(), MSG_DEVICE_REGISTRY);
        // ...and sends devices-ready back to init.
        k.sim().inject_msg(r, MSG_DEVICES_READY, b"");
        let mut rb = [0u8; 128];
        assert_eq!(wait_devices_ready(&dm, &mut rb, DM_READY_DEADLINE_NS), Ok(()));

        // The watchdog's respawn closure calls the same path again — a
        // SECOND spawn mints a second handle (a fresh DM each time).
        let (r2, w2) = k.create_sidecar(&manifest).unwrap();
        assert_eq!(k.sim().spawn_handles(), vec![r, r2]);
        assert_eq!(w2, r2);
        assert_ne!(r2, r);
    }

    #[test]
    fn resilient_loop_crash_loop_breaker() {
        // ch1 = console, ch2 = DM, ch3 = respawn target.
        let k = shared_with_chans(&[1, 2, 3]);
        let console = InitChannel::new_single(k.clone(), 1);
        let dm = InitChannel::new_single(k.clone(), 2);

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
            Ok(InitChannel::new_single(k.clone(), 3))
        });

        assert_eq!(respawns, 1);
        assert_eq!(result, Err(ChannelError::TooManyRestarts));
    }
}
