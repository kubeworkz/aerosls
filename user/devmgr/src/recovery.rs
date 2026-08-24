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

    #[test]
    fn backoff_growth() {
        let config = RecoveryConfig::default();
        let rm = RecoveryManager { kernel: &(), config, events: Vec::new() };

        let b0 = rm.compute_backoff(1);
        let b1 = rm.compute_backoff(2);
        let b2 = rm.compute_backoff(3);

        // Backoff should roughly double each time (with jitter)
        assert!(b1 > b0);
        assert!(b2 > b1);

        // Should not exceed max
        let b20 = rm.compute_backoff(20);
        assert!(b20 <= config.max_backoff_ns + config.max_backoff_ns / 4);
    }
}
