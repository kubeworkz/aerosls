//! Crash recovery: detect driver failure, revoke caps, restart, reconnect.
//!
//! Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §7.
//!
//! The Device Manager monitors driver sidecars via three mechanisms:
//! 1. Channel close events (driver exit/crash)
//! 2. Health check timeouts (heartbeat)
//! 3. Process exit notifications from the kernel
//!
//! On driver failure:
//! 1. Quarantine: mask IRQs, revoke caps, notify clients
//! 2. Recovery: exponential backoff, respawn, revalidate, re-establish channels

use alloc::vec::Vec;
use core::fmt;

use super::device::{DeviceEntry, DeviceEvent, DeviceState, DriverCaps};
use super::manifest::DriverManifest;
use super::{MAX_CRASH_RETRIES, opcode};
use aerosls_proto::kabi::Kernel;

/// Recovery configuration.
#[derive(Clone, Copy, Debug)]
pub struct RecoveryConfig {
    /// Maximum consecutive crashes before declaring DEAD.
    pub max_retries: u32,
    /// Base backoff duration in nanoseconds (100ms).
    pub base_backoff_ns: u64,
    /// Maximum backoff duration in nanoseconds (5s).
    pub max_backoff_ns: u64,
    /// Health check timeout in nanoseconds (5s).
    pub health_timeout_ns: u64,
}

impl Default for RecoveryConfig {
    fn default() -> Self {
        Self {
            max_retries: MAX_CRASH_RETRIES,
            base_backoff_ns: 100_000_000,     // 100ms
            max_backoff_ns: 5_000_000_000,    // 5s
            health_timeout_ns: 5_000_000_000, // 5s
        }
    }
}

/// Recovery event emitted during the recovery process.
#[derive(Clone, Debug)]
pub enum RecoveryEvent {
    /// Driver crash detected.
    DriverCrashed {
        device_idx: usize,
        reason: CrashReason,
        exit_code: u32,
    },
    /// Recovery attempt starting.
    RecoveryStarted {
        device_idx: usize,
        attempt: u32,
        backoff_ns: u64,
    },
    /// Recovery attempt failed.
    RecoveryFailed {
        device_idx: usize,
        attempt: u32,
        error: RecoveryError,
    },
    /// Driver successfully restored.
    DriverRestored {
        device_idx: usize,
        attempt: u32,
    },
    /// Device declared dead (max retries exceeded).
    DeviceDead {
        device_idx: usize,
        total_crashes: u32,
    },
    /// Client notified of device event.
    ClientNotified {
        client_pid: u32,
        event: u32,
        device_idx: usize,
    },
}

/// Why the driver crashed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CrashReason {
    /// Driver exited with non-zero code.
    ExitCode(u32),
    /// Driver process faulted (signal).
    Signal(u32),
    /// Heartbeat timeout (driver unresponsive).
    HeartbeatTimeout,
    /// Driver closed its control channel.
    ChannelClosed,
    /// Unknown reason.
    Unknown,
}

impl fmt::Display for CrashReason {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ExitCode(code) => write!(f, "exit code {}", code),
            Self::Signal(sig) => write!(f, "signal {}", sig),
            Self::HeartbeatTimeout => write!(f, "heartbeat timeout"),
            Self::ChannelClosed => write!(f, "channel closed"),
            Self::Unknown => write!(f, "unknown"),
        }
    }
}

/// Recovery error.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryError {
    /// create_sidecar() failed.
    SpawnFailed,
    /// New driver failed to initialize.
    InitFailed,
    /// New driver failed revalidation.
    RevalidationFailed,
    /// Channel reconnection failed.
    ChannelReconnectFailed,
    /// Internal error.
    Internal,
}

/// Recovery manager handles the crash/restart lifecycle for all devices.
pub struct RecoveryManager<'a, K: Kernel> {
    kernel: &'a K,
    config: RecoveryConfig,
    /// Pending recovery events for processing.
    events: Vec<RecoveryEvent>,
}

impl<'a, K: Kernel> RecoveryManager<'a, K> {
    pub fn new(kernel: &'a K, config: RecoveryConfig) -> Self {
        Self {
            kernel,
            config,
            events: Vec::new(),
        }
    }

    /// Detect a driver crash for a device.
    ///
    /// Called when the Device Manager learns a driver has died (channel close,
    /// process exit notification, or heartbeat timeout).
    pub fn detect_crash(
        &mut self,
        device: &mut DeviceEntry,
        device_idx: usize,
        reason: CrashReason,
        exit_code: u32,
        now_ns: u64,
    ) {
        let crash_count = device.crash_count + 1;

        self.events.push(RecoveryEvent::DriverCrashed {
            device_idx,
            reason,
            exit_code,
        });

        // Quarantine phase: the DeviceManager's on_driver_crashed() handles:
        // 1. Mask IRQs (via IRQ cap revoke)
        // 2. Revoke all driver caps
        // 3. Notify clients
        // 4. Transition to FAILED state

        // Check if we've exceeded the retry budget
        if crash_count >= self.config.max_retries {
            self.events.push(RecoveryEvent::DeviceDead {
                device_idx,
                total_crashes: crash_count,
            });
        }
    }

    /// Attempt to recover a failed device.
    ///
    /// Called by the DeviceManager when a FAILED device's backoff timer fires.
    pub fn attempt_recovery(
        &mut self,
        device: &mut DeviceEntry,
        device_idx: usize,
        manifest: Option<&DriverManifest>,
        now_ns: u64,
    ) -> Result<(), RecoveryError> {
        let attempt = device.crash_count;

        self.events.push(RecoveryEvent::RecoveryStarted {
            device_idx,
            attempt,
            backoff_ns: device.backoff_ns,
        });

        // Phase 2.1: Create new driver sidecar
        // In a real implementation, this calls create_sidecar() with
        // the matched manifest. The kernel validates the manifest and
        // creates the replacement with fresh capabilities.
        //
        // For now, simulate the spawn:
        if manifest.is_none() {
            self.events.push(RecoveryEvent::RecoveryFailed {
                device_idx,
                attempt,
                error: RecoveryError::SpawnFailed,
            });
            return Err(RecoveryError::SpawnFailed);
        }

        // Phase 2.2: Validate the new driver
        // Send DRIVER_INIT, wait for DRIVER_READY
        // This happens in the main event loop

        // Phase 2.3: Re-establish client connections
        // Create new service channels to each registered client

        // Phase 2.4: Unmask IRQs
        // The IRQ cap is re-created with the new driver's IRQ cap

        // Phase 2.5: Mark live
        device.state = DeviceState::DriverRunning;
        device.crash_count = 0;

        self.events.push(RecoveryEvent::DriverRestored {
            device_idx,
            attempt,
        });

        Ok(())
    }

    /// Check if a device's health check has timed out.
    pub fn check_health_timeout(
        &mut self,
        device: &DeviceEntry,
        device_idx: usize,
        now_ns: u64,
        last_heartbeat_ns: u64,
    ) -> bool {
        if device.state != DeviceState::DriverRunning {
            return false;
        }

        let elapsed = now_ns.saturating_sub(last_heartbeat_ns);
        if elapsed > self.config.health_timeout_ns {
            self.detect_crash(
                &mut DeviceEntry::clone(device),
                device_idx,
                CrashReason::HeartbeatTimeout,
                0,
                now_ns,
            );
            return true;
        }
        false
    }

    /// Notify all clients of a device event.
    ///
    /// Sends the event message to each subscribed client's event channel.
    pub fn notify_clients(
        &self,
        clients: &[u32],
        event: u32,
        device_idx: usize,
        extra: &[u32],
    ) {
        for &client_pid in clients {
            let _ = self.kernel.send(
                client_pid,
                event,
                0,
                &{
                    let mut v = alloc::vec![device_idx as u32];
                    v.extend_from_slice(extra);
                    v
                },
                &[],
                0,
            );

            self.events.push(RecoveryEvent::ClientNotified {
                client_pid,
                event,
                device_idx,
            });
        }
    }

    /// Drain and return all pending recovery events.
    pub fn drain_events(&mut self) -> Vec<RecoveryEvent> {
        let mut out = Vec::new();
        core::mem::swap(&mut out, &mut self.events);
        out
    }

    /// Compute exponential backoff with jitter.
    pub fn compute_backoff(&self, attempt: u32) -> u64 {
        let shift = core::cmp::min(attempt.saturating_sub(1), 16);
        let backoff = core::cmp::min(
            self.config.base_backoff_ns << shift,
            self.config.max_backoff_ns,
        );
        // Add up to 25% jitter
        let jitter = backoff / 4;
        backoff + jitter
    }
}

/// Quarantine a crashed driver: mask IRQs, revoke caps.
///
/// This is the immediate response to a crash detection, before any
/// recovery attempt. Called from the DeviceManager's crash handler.
pub fn quarantine_driver<K: Kernel>(
    kernel: &K,
    device: &mut DeviceEntry,
) {
    let caps = &device.driver_caps;

    // Mask the IRQ at the interrupt controller
    if caps.irq != 0xFFFF {
        // Revoke the IRQ cap — this masks the interrupt and unregisters
        // from the IRQ delivery registry (cap_revoke -> irq_unregister)
        let _ = kernel.close(caps.irq, 0, 0);
    }

    // Revoke DMA caps — unmaps IOMMU pages
    for i in 0..caps.n_dma as usize {
        if caps.dma[i] != 0xFFFF {
            let _ = kernel.close(caps.dma[i], 0, 0);
        }
    }

    // Revoke IO_PORT cap
    if caps.io_port != 0xFFFF {
        let _ = kernel.close(caps.io_port, 0, 0);
    }

    // Revoke control channel
    if caps.control_chan != 0 {
        let _ = kernel.close(caps.control_chan, 0, 0);
    }

    // Clear driver state
    device.driver_pid = 0;
    device.driver_caps = DriverCaps::default();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pci::{PciDevice, PciBar, PciClass};

    fn mock_device() -> PciDevice {
        PciDevice {
            bus: 0, slot: 3, func: 0,
            vendor_id: 0x8086, device_id: 0x100E,
            class: PciClass { base: 0x02, sub: 0x00, prog_if: 0x00 },
            header_type: 0, revision_id: 0,
            bars: [PciBar::default(); 6],
            interrupt_pin: 1, interrupt_line: 0,
            has_msi: true, has_msix: false, msi_offset: 0, msix_offset: 0,
            is_bridge: false, secondary_bus: 0,
        }
    }

    fn mock_entry() -> crate::device::DeviceEntry {
        crate::device::DeviceEntry::new(mock_device())
    }

    fn make_rm() -> RecoveryManager<'static, ()> {
        RecoveryManager {
            kernel: &(),
            config: RecoveryConfig::default(),
            events: Vec::new(),
        }
    }

    #[test]
    fn backoff_growth() {
        let rm = make_rm();

        let b0 = rm.compute_backoff(1);
        let b1 = rm.compute_backoff(2);
        let b2 = rm.compute_backoff(3);

        // Backoff should roughly double each time (with jitter)
        assert!(b1 > b0);
        assert!(b2 > b1);

        // Should not exceed max
        let b20 = rm.compute_backoff(20);
        assert!(b20 <= rm.config.max_backoff_ns + rm.config.max_backoff_ns / 4);
    }

    #[test]
    fn backoff_first_attempt() {
        let rm = make_rm();
        let b = rm.compute_backoff(1);
        // Base is 100ms, jitter up to 25%
        assert!(b >= 100_000_000);
        assert!(b <= 125_000_000);
    }

    #[test]
    fn backoff_exponential_growth() {
        let rm = make_rm();
        let mut prev = 0u64;
        for attempt in 1..=10 {
            let b = rm.compute_backoff(attempt);
            // Should be >= previous (ignoring jitter)
            assert!(b >= prev / 2, "attempt {}: {} < prev/2 {}", attempt, b, prev/2);
            prev = b;
        }
    }

    #[test]
    fn backoff_max_cap() {
        let rm = make_rm();
        for attempt in 1..=32 {
            let b = rm.compute_backoff(attempt);
            // Max is 5s + 25% jitter = 6.25s
            assert!(b <= 6_250_000_000,
                "attempt {}: backoff {} exceeds max", attempt, b);
        }
    }

    #[test]
    fn backoff_zero_attempt() {
        let rm = make_rm();
        // saturating_sub(0, 1) = 0, so shift = 0, backoff = 100ms
        let b = rm.compute_backoff(0);
        assert!(b >= 100_000_000);
    }

    #[test]
    fn recovery_config_defaults() {
        let cfg = RecoveryConfig::default();
        assert_eq!(cfg.max_retries, 8);
        assert_eq!(cfg.base_backoff_ns, 100_000_000);
        assert_eq!(cfg.max_backoff_ns, 5_000_000_000);
        assert_eq!(cfg.health_timeout_ns, 5_000_000_000);
    }

    #[test]
    fn crash_reason_display() {
        let reasons = [
            (CrashReason::ExitCode(1), "exit code 1"),
            (CrashReason::Signal(139), "signal 139"),
            (CrashReason::HeartbeatTimeout, "heartbeat timeout"),
            (CrashReason::ChannelClosed, "channel closed"),
            (CrashReason::Unknown, "unknown"),
        ];
        for (reason, expected) in reasons {
            assert_eq!(alloc::format!("{}", reason), expected);
        }
    }

    #[test]
    fn crash_reason_equality() {
        assert_eq!(CrashReason::ExitCode(1), CrashReason::ExitCode(1));
        assert_ne!(CrashReason::ExitCode(1), CrashReason::ExitCode(2));
        assert_ne!(CrashReason::Signal(139), CrashReason::HeartbeatTimeout);
        assert_eq!(CrashReason::Unknown, CrashReason::Unknown);
    }

    #[test]
    fn recovery_error_equality() {
        assert_eq!(RecoveryError::SpawnFailed, RecoveryError::SpawnFailed);
        assert_ne!(RecoveryError::SpawnFailed, RecoveryError::InitFailed);
    }

    #[test]
    fn detect_crash_emits_event() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.state = crate::device::DeviceState::DriverRunning;

        rm.detect_crash(&mut entry, 0, CrashReason::Signal(139), 139, 1000);

        let events = rm.drain_events();
        assert_eq!(events.len(), 1);
        match &events[0] {
            RecoveryEvent::DriverCrashed { device_idx, reason, exit_code } => {
                assert_eq!(*device_idx, 0);
                assert_eq!(*reason, CrashReason::Signal(139));
                assert_eq!(*exit_code, 139);
            }
            _ => panic!("expected DriverCrashed event"),
        }
    }

    #[test]
    fn detect_crash_emits_dead_when_max_retries() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.crash_count = 7; // one less than max (8)

        rm.detect_crash(&mut entry, 0, CrashReason::HeartbeatTimeout, 0, 1000);

        let events = rm.drain_events();
        // Should have DriverCrashed + DeviceDead
        assert_eq!(events.len(), 2);
        assert!(matches!(&events[1], RecoveryEvent::DeviceDead { device_idx: 0, total_crashes: 8 }));
    }

    #[test]
    fn detect_crash_no_dead_under_budget() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.crash_count = 5;

        rm.detect_crash(&mut entry, 0, CrashReason::ChannelClosed, 0, 1000);

        let events = rm.drain_events();
        // Only DriverCrashed, no DeviceDead
        assert_eq!(events.len(), 1);
        assert!(matches!(&events[0], RecoveryEvent::DriverCrashed { .. }));
    }

    #[test]
    fn attempt_recovery_no_manifest_fails() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.crash_count = 1;
        entry.backoff_ns = 150_000_000;

        let result = rm.attempt_recovery(&mut entry, 0, None, 2000);
        assert!(result.is_err());
        assert_eq!(result.unwrap_err(), RecoveryError::SpawnFailed);

        let events = rm.drain_events();
        assert!(events.iter().any(|e| matches!(e, RecoveryEvent::RecoveryFailed { .. })));
    }

    #[test]
    fn attempt_recovery_with_manifest_succeeds() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.state = crate::device::DeviceState::Failed;
        entry.crash_count = 1;

        let manifest = crate::manifest::e1000_manifest();
        let result = rm.attempt_recovery(&mut entry, 0, Some(&manifest), 3000);
        assert!(result.is_ok());

        assert_eq!(entry.state, crate::device::DeviceState::DriverRunning);
        assert_eq!(entry.crash_count, 0);

        let events = rm.drain_events();
        assert!(events.iter().any(|e| matches!(e, RecoveryEvent::RecoveryStarted { .. })));
        assert!(events.iter().any(|e| matches!(e, RecoveryEvent::DriverRestored { .. })));
    }

    #[test]
    fn attempt_recovery_records_attempt() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.crash_count = 3;

        let manifest = crate::manifest::e1000_manifest();
        rm.attempt_recovery(&mut entry, 0, Some(&manifest), 3000).unwrap();

        let events = rm.drain_events();
        match events.iter().find(|e| matches!(e, RecoveryEvent::RecoveryStarted { .. })) {
            Some(RecoveryEvent::RecoveryStarted { attempt, .. }) => {
                assert_eq!(*attempt, 3);
            }
            _ => panic!("expected RecoveryStarted event"),
        }
    }

    #[test]
    fn notify_clients_sends_messages() {
        let rm = make_rm();
        let clients = [1, 2, 3];
        rm.notify_clients(&clients, 0xE001, 0, &[42]);

        let events = rm.drain_events();
        assert_eq!(events.len(), 3);
        for (i, event) in events.iter().enumerate() {
            match event {
                RecoveryEvent::ClientNotified { client_pid, event: evt, device_idx } => {
                    assert_eq!(*client_pid, (i + 1) as u32);
                    assert_eq!(*evt, 0xE001);
                    assert_eq!(*device_idx, 0);
                }
                _ => panic!("expected ClientNotified event"),
            }
        }
    }

    #[test]
    fn notify_clients_empty() {
        let rm = make_rm();
        rm.notify_clients(&[], 0xE001, 0, &[]);
        let events = rm.drain_events();
        assert!(events.is_empty());
    }

    #[test]
    fn drain_events_clears() {
        let mut rm = make_rm();
        rm.events.push(RecoveryEvent::DeviceDead { device_idx: 0, total_crashes: 8 });
        rm.events.push(RecoveryEvent::DriverCrashed {
            device_idx: 0, reason: CrashReason::Unknown, exit_code: 0
        });

        let events = rm.drain_events();
        assert_eq!(events.len(), 2);
        assert!(rm.events.is_empty());
    }

    #[test]
    fn health_timeout_not_running() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.state = crate::device::DeviceState::Failed;

        assert!(!rm.check_health_timeout(&entry, 0, 1000, 0));
        assert!(rm.events.is_empty());
    }

    #[test]
    fn health_timeout_within_budget() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.state = crate::device::DeviceState::DriverRunning;

        // Heartbeat was 1s ago, timeout is 5s
        assert!(!rm.check_health_timeout(&entry, 0, 1_000_000_000, 0));
    }

    #[test]
    fn health_timeout_exceeded() {
        let mut rm = make_rm();
        let mut entry = mock_entry();
        entry.state = crate::device::DeviceState::DriverRunning;

        // Heartbeat was 10s ago, timeout is 5s
        assert!(rm.check_health_timeout(&entry, 0, 10_000_000_000, 0));
        let events = rm.drain_events();
        assert!(events.iter().any(|e| matches!(e, RecoveryEvent::DriverCrashed { reason: CrashReason::HeartbeatTimeout, .. })));
    }

    #[test]
    fn quarantine_clears_driver_state() {
        let mut entry = mock_entry();
        entry.driver_pid = 42;
        entry.driver_caps.io_port = 10;
        entry.driver_caps.irq = 11;
        entry.driver_caps.dma = [12, 13, 14, 15];
        entry.driver_caps.n_dma = 3;
        entry.driver_caps.control_chan = 20;

        quarantine_driver(&(), &mut entry);

        assert_eq!(entry.driver_pid, 0);
        assert_eq!(entry.driver_caps.io_port, 0xFFFF);
        assert_eq!(entry.driver_caps.irq, 0xFFFF);
        assert_eq!(entry.driver_caps.n_dma, 0);
        assert_eq!(entry.driver_caps.control_chan, 0);
    }

    #[test]
    fn quarantine_no_caps_is_noop() {
        let mut entry = mock_entry();
        // Default caps are all 0 / 0xFFFF
        quarantine_driver(&(), &mut entry);
        assert_eq!(entry.driver_pid, 0);
    }

    #[test]
    fn recovery_config_clone() {
        let cfg = RecoveryConfig::default();
        let cfg2 = cfg;
        assert_eq!(cfg.max_retries, cfg2.max_retries);
        assert_eq!(cfg.base_backoff_ns, cfg2.base_backoff_ns);
    }

    #[test]
    fn recovery_event_clone() {
        let event = RecoveryEvent::DriverCrashed {
            device_idx: 0,
            reason: CrashReason::Signal(139),
            exit_code: 139,
        };
        let event2 = event.clone();
        assert!(matches!(event2, RecoveryEvent::DriverCrashed { .. }));
    }
}
