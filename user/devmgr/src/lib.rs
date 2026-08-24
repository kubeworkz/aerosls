//! AeroSLS Device Manager sidecar (`aerosls.devmgr.v1`).
//!
//! Handles PCIe bus enumeration, driver manifest matching, device lifecycle
//! state machine, hotplug detection, and driver sidecar spawn/supervision.
//!
//! Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §2.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use core::fmt;

use aerosls_proto::kabi::{Kernel, ERR_OK};

pub mod pci;
pub mod manifest;
pub mod device;
pub mod recovery;

pub use pci::{PciDevice, PciBar, PciClass};
pub use manifest::{DriverManifest, MatchResult};
pub use device::{DeviceState, DeviceEvent, DeviceEntry};
pub use recovery::{RecoveryManager, RecoveryConfig, CrashReason, RecoveryEvent};

/// Maximum number of devices tracked by the Device Manager.
pub const MAX_DEVICES: usize = 64;

/// Maximum number of driver manifests in the registry.
pub const MAX_MANIFESTS: usize = 32;

/// Maximum number of registered clients (for hotplug notifications).
pub const MAX_CLIENTS: usize = 16;

/// Hotplug poll interval in nanoseconds (5 seconds).
pub const HOTPLUG_POLL_NS: u64 = 5_000_000_000;

/// Health check interval in nanoseconds (2 seconds).
pub const HEALTH_CHECK_NS: u64 = 2_000_000_000;

/// Maximum crash retries before declaring a driver DEAD.
pub const MAX_CRASH_RETRIES: u32 = 8;

/// Device Manager error codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DevMgrError {
    PciEnumFailed,
    NoDriverMatch,
    SpawnFailed,
    ManifestNotFound,
    DeviceNotFound,
    ChannelFull,
    Timeout,
    Internal,
}

impl fmt::Display for DevMgrError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::PciEnumFailed => write!(f, "PCI enumeration failed"),
            Self::NoDriverMatch => write!(f, "no driver matched device"),
            Self::SpawnFailed => write!(f, "driver spawn failed"),
            Self::ManifestNotFound => write!(f, "driver manifest not found"),
            Self::DeviceNotFound => write!(f, "device not found"),
            Self::ChannelFull => write!(f, "channel full"),
            Self::Timeout => write!(f, "timeout"),
            Self::Internal => write!(f, "internal error"),
        }
    }
}

/// Channel message opcodes for Device Manager protocol.
pub mod opcode {
    /// Driver → Device Manager: driver is ready.
    pub const DRIVER_READY: u32 = 0xD001;
    /// Driver → Device Manager: driver is shutting down.
    pub const DRIVER_SHUTDOWN: u32 = 0xD002;
    /// Device Manager → Driver: initialize with device config.
    pub const DRIVER_INIT: u32 = 0xD003;
    /// Device Manager → Driver: heartbeat ping.
    pub const HEARTBEAT: u32 = 0xD004;
    /// Driver → Device Manager: heartbeat pong.
    pub const HEARTBEAT_ACK: u32 = 0xD005;
    /// Device Manager → Clients: device removed notification.
    pub const EVT_DEVICE_REMOVED: u32 = 0xE001;
    /// Device Manager → Clients: driver failed notification.
    pub const EVT_DRIVER_FAILED: u32 = 0xE002;
    /// Device Manager → Clients: driver restored notification.
    pub const EVT_DRIVER_RESTORED: u32 = 0xE003;
    /// Client → Device Manager: subscribe to device events.
    pub const SUBSCRIBE_EVENTS: u32 = 0xE010;
}

/// The Device Manager's main state.
pub struct DeviceManager<'a, K: Kernel> {
    /// Kernel ABI for syscalls.
    pub kernel: &'a K,
    /// Discovered PCI devices.
    pub devices: Vec<DeviceEntry>,
    /// Driver manifest registry.
    pub manifests: Vec<DriverManifest<'a>>,
    /// Registered client PIDs for event notifications.
    pub clients: Vec<u32>,
    /// BUS_ACCESS capability handle.
    pub bus_cap: u32,
    /// Control channel to parent (POSIX core).
    pub parent_chan: u32,
}

impl<'a, K: Kernel> DeviceManager<'a, K> {
    /// Create a new Device Manager.
    pub fn new(
        kernel: &'a K,
        bus_cap: u32,
        parent_chan: u32,
    ) -> Self {
        Self {
            kernel,
            devices: Vec::new(),
            manifests: Vec::new(),
            clients: Vec::new(),
            bus_cap,
            parent_chan,
        }
    }

    /// Register a driver manifest in the registry.
    pub fn register_manifest(&mut self, manifest: DriverManifest<'a>) {
        if self.manifests.len() < MAX_MANIFESTS {
            self.manifests.push(manifest);
        }
    }

    /// Subscribe a client for device event notifications.
    pub fn subscribe_client(&mut self, pid: u32) {
        if self.clients.len() < MAX_CLIENTS && !self.clients.contains(&pid) {
            self.clients.push(pid);
        }
    }

    /// Run one iteration of the Device Manager event loop.
    /// Returns true if work was done, false if idle.
    pub fn poll(&mut self) -> bool {
        let mut did_work = false;

        // 1. Check for driver messages (heartbeat responses, shutdown, etc.)
        did_work |= self.poll_driver_messages();

        // 2. Process devices in DRIVER_RUNNING state (health checks)
        did_work |= self.poll_health();

        // 3. Process devices in FAILED state (retry with backoff)
        did_work |= self.poll_failed_devices();

        // 4. Check for hotplug events (periodic re-enumeration)
        did_work |= self.poll_hotplug();

        did_work
    }

    /// Handle a device discovery from PCI enumeration.
    pub fn on_device_discovered(&mut self, device: PciDevice) {
        // Check if already tracked
        if self.devices.iter().any(|d| {
            d.bus == device.bus
                && d.slot == device.slot
                && d.func == device.func
        }) {
            return;
        }

        let entry = DeviceEntry::new(device);
        kernel_serial_printf(
            "[DM] discovered: PCI %02x:%02x.%x vendor=%04x device=%04x class=%06x\n",
            entry.device.bus, entry.device.slot, entry.device.func,
            entry.device.vendor_id, entry.device.device_id,
            entry.device.class_code,
        );

        // Try to match a driver
        if let Some(idx) = self.match_driver(&entry.device) {
            entry.state = DeviceState::Matched;
            entry.manifest_idx = Some(idx);
            // Attempt to spawn the driver
            self.spawn_driver(self.devices.len() - 1);
        }

        self.devices.push(entry);
    }

    /// Match a device against registered driver manifests.
    fn match_driver(&self, device: &PciDevice) -> Option<usize> {
        for (i, manifest) in self.manifests.iter().enumerate() {
            match manifest.try_match(device) {
                MatchResult::Exact => return Some(i),
                _ => {}
            }
        }
        // Fallback: class code match
        for (i, manifest) in self.manifests.iter().enumerate() {
            match manifest.try_match(device) {
                MatchResult::ClassCode => return Some(i),
                _ => {}
            }
        }
        // Fallback: compatible string match
        for (i, manifest) in self.manifests.iter().enumerate() {
            match manifest.try_match(device) {
                MatchResult::Compatible => return Some(i),
                _ => {}
            }
        }
        None
    }

    /// Spawn a driver sidecar for a device.
    fn spawn_driver(&mut self, device_idx: usize) {
        let entry = &mut self.devices[device_idx];
        entry.state = DeviceState::Spawning;

        // In a real implementation, this would call create_sidecar()
        // with the matched manifest. For now, mark as failed (no spawn
        // capability in this stub).
        entry.state = DeviceState::Failed;
        entry.crash_count += 1;
        entry.backoff_ns = self.compute_backoff(entry.crash_count);
        kernel_serial_printf(
            "[DM] driver spawn for PCI %02x:%02x.%x (attempt %u)\n",
            entry.device.bus, entry.device.slot, entry.device.func,
            entry.crash_count,
        );
    }

    /// Handle a driver crash for a tracked device.
    /// Performs quarantine (mask IRQs, revoke caps, notify clients)
    /// and transitions the device to FAILED state.
    pub fn on_driver_crashed(&mut self, device_idx: usize, reason: CrashReason, exit_code: u32, now_ns: u64) {
        if device_idx >= self.devices.len() {
            return;
        }

        // 1. Quarantine: mask IRQs, revoke caps
        recovery::quarantine_driver(self.kernel, &mut self.devices[device_idx]);

        // 2. Notify clients
        self.notify_clients(
            opcode::EVT_DRIVER_FAILED,
            device_idx,
            &[reason as u32, exit_code],
        );

        // 3. Transition to FAILED
        self.devices[device_idx].transition(
            DeviceEvent::DriverCrashed(exit_code),
            now_ns,
        );

        // 4. Compute backoff for retry
        self.devices[device_idx].backoff_ns = self.compute_backoff(
            self.devices[device_idx].crash_count,
        );
    }

    /// Handle a device removed event (hotplug).
    pub fn on_device_removed(&mut self, device_idx: usize, now_ns: u64) {
        if device_idx >= self.devices.len() {
            return;
        }

        // 1. Mask IRQs (via cap revoke)
        recovery::quarantine_driver(self.kernel, &mut self.devices[device_idx]);

        // 2. Notify clients
        self.notify_clients(
            opcode::EVT_DEVICE_REMOVED,
            device_idx,
            &[],
        );

        // 3. Transition to REMOVED_WAIT
        self.devices[device_idx].transition(
            DeviceEvent::DeviceRemoved,
            now_ns,
        );
    }

    /// Compute exponential backoff with jitter.
    pub fn compute_backoff(&self, attempt: u32) -> u64 {
        let base_ns: u64 = 100_000_000; // 100ms
        let max_ns: u64 = 5_000_000_000; // 5s
        let shift = core::cmp::min(attempt.saturating_sub(1), 16);
        let backoff = core::cmp::min(base_ns << shift, max_ns);
        // Simple jitter: add up to 25% of the backoff
        let jitter = backoff / 4;
        backoff + jitter
    }

    /// Poll driver messages from all active drivers.
    fn poll_driver_messages(&mut self) -> bool {
        // Placeholder: in a real implementation, this would iterate
        // driver control channels and process messages.
        false
    }

    /// Poll health checks for running drivers.
    fn poll_health(&mut self) -> bool {
        // Placeholder: send heartbeat to running drivers, detect timeouts.
        false
    }

    /// Process failed devices (retry with backoff).
    fn poll_failed_devices(&mut self) -> bool {
        let mut did_work = false;
        for i in 0..self.devices.len() {
            if self.devices[i].state == DeviceState::Failed
                && self.devices[i].crash_count < MAX_CRASH_RETRIES
            {
                // Backoff timer would fire here; for now, mark as needing retry
                did_work = true;
            }
        }
        did_work
    }

    /// Check for hotplug events (periodic re-enumeration).
    fn poll_hotplug(&mut self) -> bool {
        // Placeholder: re-enumerate PCI bus and detect new/removed devices.
        false
    }

    /// Notify all subscribed clients of a device event.
    fn notify_clients(&self, event: DeviceEvent, device_idx: usize) {
        let entry = &self.devices[device_idx];
        for &client in &self.clients {
            let _ = self.kernel.send(
                client,
                event.opcode(),
                0,
                &[device_idx as u32, entry.device.vendor_id as u32],
                &[],
                0,
            );
        }
    }
}

/// Printf-like macro for kernel serial output (no_std).
/// In a real sidecar this would use the console channel.
macro_rules! kernel_serial_printf {
    ($($arg:tt)*) => {
        // Stub: in production, format and send to console channel.
        // For now, this is a no-op in the sidecar build.
        let _ = core::format_args!($($arg)*);
    };
}
pub(crate) use kernel_serial_printf;
