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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pci::{PciClass, PciDevice, PciBar};
    use crate::manifest::{e1000_manifest, virtio_net_manifest, MatchResult};
    use crate::device::{DeviceState, DeviceEvent, DeviceEntry};
    use crate::recovery::{RecoveryManager, RecoveryConfig, CrashReason};

    fn mock_device(bus: u8, slot: u8, vendor: u16, device: u16, class_base: u8) -> PciDevice {
        PciDevice {
            bus, slot, func: 0,
            vendor_id: vendor, device_id: device,
            class: PciClass { base: class_base, sub: 0x00, prog_if: 0x00 },
            header_type: 0, revision_id: 0,
            bars: [PciBar::default(); 6],
            interrupt_pin: 1, interrupt_line: 0,
            has_msi: true, has_msix: false, msi_offset: 0, msix_offset: 0,
            is_bridge: false, secondary_bus: 0,
        }
    }

    #[test]
    fn devmgr_error_display() {
        let cases = [
            (DevMgrError::PciEnumFailed, "PCI enumeration failed"),
            (DevMgrError::NoDriverMatch, "no driver matched device"),
            (DevMgrError::SpawnFailed, "driver spawn failed"),
            (DevMgrError::ManifestNotFound, "driver manifest not found"),
            (DevMgrError::DeviceNotFound, "device not found"),
            (DevMgrError::ChannelFull, "channel full"),
            (DevMgrError::Timeout, "timeout"),
            (DevMgrError::Internal, "internal error"),
        ];
        for (err, expected) in cases {
            assert_eq!(alloc::format!("{}", err), expected);
        }
    }

    #[test]
    fn devmgr_error_equality() {
        assert_eq!(DevMgrError::Timeout, DevMgrError::Timeout);
        assert_ne!(DevMgrError::Timeout, DevMgrError::Internal);
    }

    #[test]
    fn opcode_constants() {
        assert_eq!(opcode::DRIVER_READY, 0xD001);
        assert_eq!(opcode::DRIVER_SHUTDOWN, 0xD002);
        assert_eq!(opcode::DRIVER_INIT, 0xD003);
        assert_eq!(opcode::HEARTBEAT, 0xD004);
        assert_eq!(opcode::HEARTBEAT_ACK, 0xD005);
        assert_eq!(opcode::EVT_DEVICE_REMOVED, 0xE001);
        assert_eq!(opcode::EVT_DRIVER_FAILED, 0xE002);
        assert_eq!(opcode::EVT_DRIVER_RESTORED, 0xE003);
        assert_eq!(opcode::SUBSCRIBE_EVENTS, 0xE010);
    }

    #[test]
    fn constants() {
        assert_eq!(MAX_DEVICES, 64);
        assert_eq!(MAX_MANIFESTS, 32);
        assert_eq!(MAX_CLIENTS, 16);
        assert_eq!(HOTPLUG_POLL_NS, 5_000_000_000);
        assert_eq!(HEALTH_CHECK_NS, 2_000_000_000);
        assert_eq!(MAX_CRASH_RETRIES, 8);
    }

    #[test]
    fn devmgr_new() {
        let kernel = ();
        let dm = DeviceManager::new(&kernel, 0, 1);
        assert!(dm.devices.is_empty());
        assert!(dm.manifests.is_empty());
        assert!(dm.clients.is_empty());
        assert_eq!(dm.bus_cap, 0);
        assert_eq!(dm.parent_chan, 1);
    }

    #[test]
    fn register_manifest() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());
        dm.register_manifest(virtio_net_manifest());
        assert_eq!(dm.manifests.len(), 2);
    }

    #[test]
    fn register_manifest_limit() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        for _ in 0..MAX_MANIFESTS + 10 {
            dm.register_manifest(e1000_manifest());
        }
        assert_eq!(dm.manifests.len(), MAX_MANIFESTS);
    }

    #[test]
    fn subscribe_client() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.subscribe_client(10);
        dm.subscribe_client(20);
        assert_eq!(dm.clients, vec![10, 20]);
    }

    #[test]
    fn subscribe_client_no_duplicates() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.subscribe_client(10);
        dm.subscribe_client(10);
        dm.subscribe_client(10);
        assert_eq!(dm.clients, vec![10]);
    }

    #[test]
    fn subscribe_client_limit() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        for i in 0..MAX_CLIENTS + 5 {
            dm.subscribe_client(i as u32);
        }
        assert_eq!(dm.clients.len(), MAX_CLIENTS);
    }

    #[test]
    fn on_device_discovered() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        let dev = mock_device(0, 3, 0x8086, 0x100E, 0x02);
        dm.on_device_discovered(dev);
        assert_eq!(dm.devices.len(), 1);
        assert_eq!(dm.devices[0].device.vendor_id, 0x8086);
    }

    #[test]
    fn on_device_discovered_duplicate() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        let dev = mock_device(0, 3, 0x8086, 0x100E, 0x02);
        dm.on_device_discovered(dev.clone());
        dm.on_device_discovered(dev);
        assert_eq!(dm.devices.len(), 1);
    }

    #[test]
    fn on_device_discovered_with_matching_manifest() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());

        let dev = mock_device(0, 3, 0x8086, 0x100E, 0x02);
        dm.on_device_discovered(dev);
        assert_eq!(dm.devices.len(), 1);
        // State should be Failed (spawn_driver stub fails)
        assert_eq!(dm.devices[0].state, DeviceState::Failed);
    }

    #[test]
    fn on_device_discovered_no_matching_manifest() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());

        let dev = mock_device(0, 3, 0x1234, 0x9999, 0xFF); // unknown device
        dm.on_device_discovered(dev);
        assert_eq!(dm.devices.len(), 1);
        assert_eq!(dm.devices[0].state, DeviceState::Discovered);
    }

    #[test]
    fn on_driver_crashed() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        let dev = mock_device(0, 3, 0x8086, 0x100E, 0x02);
        dm.on_device_discovered(dev);

        // Simulate: device was in Failed state, crash it
        dm.devices[0].state = DeviceState::DriverRunning;
        dm.devices[0].driver_pid = 42;

        dm.on_driver_crashed(0, CrashReason::Signal(139), 139, 1000);
        assert_eq!(dm.devices[0].state, DeviceState::Failed);
        assert_eq!(dm.devices[0].crash_count, 1);
    }

    #[test]
    fn on_driver_crashed_out_of_bounds() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        // Should not panic
        dm.on_driver_crashed(99, CrashReason::Unknown, 0, 0);
    }

    #[test]
    fn on_device_removed() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        let dev = mock_device(0, 3, 0x8086, 0x100E, 0x02);
        dm.on_device_discovered(dev);
        dm.devices[0].state = DeviceState::DriverRunning;

        dm.on_device_removed(0, 1000);
        assert_eq!(dm.devices[0].state, DeviceState::RemovedWait);
    }

    #[test]
    fn on_device_removed_out_of_bounds() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        // Should not panic
        dm.on_device_removed(99, 0);
    }

    #[test]
    fn compute_backoff() {
        let kernel = ();
        let dm = DeviceManager::new(&kernel, 0, 1);

        let b1 = dm.compute_backoff(1);
        let b2 = dm.compute_backoff(2);
        let b5 = dm.compute_backoff(5);
        let b20 = dm.compute_backoff(20);

        assert!(b1 >= 100_000_000); // at least 100ms
        assert!(b2 > b1);
        assert!(b5 > b2);
        // Should not exceed max (5s + 25% jitter = 6.25s)
        assert!(b20 <= 6_250_000_000);
    }

    #[test]
    fn poll_returns_false_when_empty() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        assert!(!dm.poll());
    }

    #[test]
    fn match_driver_priority() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());
        dm.register_manifest(virtio_net_manifest());

        let dev = mock_device(0, 3, 0x8086, 0x100E, 0x02);
        let entry = DeviceEntry::new(dev);
        let idx = dm.match_driver(&entry.device);
        assert_eq!(idx, Some(0)); // e1000 matches first
    }

    #[test]
    fn match_driver_no_match() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());

        let dev = mock_device(0, 3, 0x1234, 0x9999, 0xFF);
        let entry = DeviceEntry::new(dev);
        assert!(dm.match_driver(&entry.device).is_none());
    }

    #[test]
    fn multiple_devices_tracked() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);

        dm.on_device_discovered(mock_device(0, 0, 0x8086, 0x100E, 0x02));
        dm.on_device_discovered(mock_device(0, 1, 0x1AF4, 0x1000, 0x02));
        dm.on_device_discovered(mock_device(0, 2, 0x10DE, 0x1234, 0x03));

        assert_eq!(dm.devices.len(), 3);
    }

    #[test]
    fn full_device_lifecycle() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());

        // Discover
        dm.on_device_discovered(mock_device(0, 3, 0x8086, 0x100E, 0x02));
        assert_eq!(dm.devices.len(), 1);

        // Device is in Failed state (spawn stub)
        assert_eq!(dm.devices[0].state, DeviceState::Failed);
        assert_eq!(dm.devices[0].crash_count, 1);

        // Simulate successful recovery
        dm.devices[0].transition(DeviceEvent::SpawnSucceeded, 1000);
        assert_eq!(dm.devices[0].state, DeviceState::DriverRunning);
        assert_eq!(dm.devices[0].crash_count, 0);

        // Crash again
        dm.on_driver_crashed(0, CrashReason::HeartbeatTimeout, 0, 2000);
        assert_eq!(dm.devices[0].state, DeviceState::Failed);
    }

    #[test]
    fn hotplug_lifecycle() {
        let kernel = ();
        let mut dm = DeviceManager::new(&kernel, 0, 1);
        dm.register_manifest(e1000_manifest());

        // Discover and spawn
        dm.on_device_discovered(mock_device(0, 3, 0x8086, 0x100E, 0x02));
        dm.devices[0].transition(DeviceEvent::SpawnSucceeded, 100);
        assert_eq!(dm.devices[0].state, DeviceState::DriverRunning);

        // Hotplug removal
        dm.on_device_removed(0, 200);
        assert_eq!(dm.devices[0].state, DeviceState::RemovedWait);
        assert!(dm.devices[0].was_removed);

        // Re-detection
        dm.devices[0].transition(DeviceEvent::DeviceRedetected, 300);
        assert_eq!(dm.devices[0].state, DeviceState::Matched);
        assert!(!dm.devices[0].was_removed);
    }
}
