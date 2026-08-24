//! Device lifecycle state machine and hotplug handling.
//!
//! Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §2.5-2.7.
//!
//! Each discovered PCI device has a lifecycle state tracked in a
//! DeviceEntry. State transitions are driven by events from the
//! hardware (hotplug), the driver (crash/shutdown), or the
//! Device Manager (spawn/respawn).

use super::pci::PciDevice;
use super::MAX_CRASH_RETRIES;

/// Device lifecycle states.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceState {
    /// PCI enumeration found device, no driver matched or not yet matched.
    Discovered,
    /// Driver manifest selected, capabilities being provisioned.
    Matched,
    /// Driver sidecar alive and serving, channels established with clients.
    DriverRunning,
    /// Driver crashed but retry budget not exhausted.
    Failed,
    /// PCIe device removed (hotplug or AER).
    Removed,
    /// Device removed, clients notified, awaiting re-detect.
    RemovedWait,
    /// Retry budget exhausted, no more restarts.
    Dead,
}

impl Default for DeviceState {
    fn default() -> Self {
        Self::Discovered
    }
}

/// Events that drive state transitions.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceEvent {
    /// Driver manifest matched to this device.
    Matched,
    /// create_sidecar() succeeded, driver is starting.
    SpawnSucceeded,
    /// create_sidecar() failed.
    SpawnFailed,
    /// Driver exited voluntarily (exit code 0).
    DriverExited(u32),
    /// Driver crashed (signal/fault).
    DriverCrashed(u32),
    /// PCIe device removed.
    DeviceRemoved,
    /// PCIe device re-detected (hotplug).
    DeviceRedetected,
    /// Retry budget check.
    RetryBudgetCheck,
}

impl DeviceEvent {
    /// Opcode for client notifications.
    pub fn opcode(&self) -> u32 {
        match self {
            Self::DeviceRemoved => 0xE001,  // EVT_DEVICE_REMOVED
            Self::DriverCrashed(_) => 0xE002,  // EVT_DRIVER_FAILED
            Self::SpawnSucceeded => 0xE003, // EVT_DRIVER_RESTORED
            _ => 0,
        }
    }
}

/// A tracked device with its lifecycle state.
#[derive(Clone, Debug)]
pub struct DeviceEntry {
    /// The PCI device information.
    pub device: PciDevice,
    /// Current lifecycle state.
    pub state: DeviceState,
    /// Index of the matched driver manifest.
    pub manifest_idx: Option<usize>,
    /// PID of the running driver sidecar (0 = none).
    pub driver_pid: u32,
    /// Capability handles held by the driver.
    pub driver_caps: DriverCaps,
    /// Number of consecutive crashes.
    pub crash_count: u32,
    /// Backoff duration in nanoseconds.
    pub backoff_ns: u64,
    /// Timestamp of last crash (for backoff timing).
    pub last_crash_ns: u64,
    /// Timestamp of last state change.
    pub last_transition_ns: u64,
    /// Whether the device was removed while driver was running.
    pub was_removed: bool,
}

/// Capability handles for a running driver.
#[derive(Clone, Copy, Debug, Default)]
pub struct DriverCaps {
    /// IO_PORT capability handle (BAR MMIO).
    pub io_port: u16,
    /// IRQ capability handle.
    pub irq: u16,
    /// DMA buffer capability handles.
    pub dma: [u16; 4],
    pub n_dma: u8,
    /// BUS_ACCESS capability handle (for enumeration).
    pub bus: u16,
    /// Control channel handle to Device Manager.
    pub control_chan: u32,
    /// Service channel handle to network stack (or other clients).
    pub service_chan: u32,
}

impl DeviceEntry {
    /// Create a new device entry from a discovered PCI device.
    pub fn new(device: PciDevice) -> Self {
        Self {
            device,
            state: DeviceState::Discovered,
            manifest_idx: None,
            driver_pid: 0,
            driver_caps: DriverCaps::default(),
            crash_count: 0,
            backoff_ns: 0,
            last_crash_ns: 0,
            last_transition_ns: 0,
            was_removed: false,
        }
    }

    /// Transition the device to a new state.
    ///
    /// Returns true if the transition was valid, false if invalid.
    pub fn transition(&mut self, event: DeviceEvent, now_ns: u64) -> bool {
        let old = self.state;

        match (self.state, event) {
            // DISCOVERED transitions
            (DeviceState::Discovered, DeviceEvent::Matched) => {
                self.state = DeviceState::Matched;
            }

            // MATCHED transitions
            (DeviceState::Matched, DeviceEvent::SpawnSucceeded) => {
                self.state = DeviceState::DriverRunning;
                self.crash_count = 0;
            }
            (DeviceState::Matched, DeviceEvent::SpawnFailed) => {
                self.state = DeviceState::Failed;
                self.crash_count += 1;
                self.last_crash_ns = now_ns;
            }

            // DRIVER_RUNNING transitions
            (DeviceState::DriverRunning, DeviceEvent::DriverExited(code)) => {
                if code == 0 {
                    self.state = DeviceState::Removed;
                } else {
                    self.state = DeviceState::Failed;
                    self.crash_count += 1;
                    self.last_crash_ns = now_ns;
                }
                self.driver_pid = 0;
            }
            (DeviceState::DriverRunning, DeviceEvent::DriverCrashed(_)) => {
                self.state = DeviceState::Failed;
                self.crash_count += 1;
                self.last_crash_ns = now_ns;
                self.driver_pid = 0;
            }
            (DeviceState::DriverRunning, DeviceEvent::DeviceRemoved) => {
                self.state = DeviceState::RemovedWait;
                self.was_removed = true;
                self.driver_pid = 0;
            }

            // FAILED transitions
            (DeviceState::Failed, DeviceEvent::SpawnSucceeded) => {
                self.state = DeviceState::DriverRunning;
                self.crash_count = 0;
            }
            (DeviceState::Failed, DeviceEvent::SpawnFailed) => {
                if self.crash_count >= MAX_CRASH_RETRIES {
                    self.state = DeviceState::Dead;
                }
                // else stays Failed, will retry on next poll
            }
            (DeviceState::Failed, DeviceEvent::DeviceRemoved) => {
                self.state = DeviceState::RemovedWait;
                self.was_removed = true;
            }

            // REMOVED transitions
            (DeviceState::Removed, DeviceEvent::DeviceRedetected) => {
                self.state = DeviceState::Matched;
                self.was_removed = false;
            }

            // REMOVED_WAIT transitions
            (DeviceState::RemovedWait, DeviceEvent::DeviceRedetected) => {
                self.state = DeviceState::Matched;
                self.was_removed = false;
                self.crash_count = 0;
            }

            // DEAD transitions (operator restart only)
            (DeviceState::Dead, DeviceEvent::Matched) => {
                self.state = DeviceState::Matched;
                self.crash_count = 0;
                self.was_removed = false;
            }

            // Invalid transitions
            _ => return false,
        }

        self.last_transition_ns = now_ns;
        let _ = old; // for future logging
        true
    }

    /// Check if the device is in a state that accepts driver messages.
    pub fn is_driver_active(&self) -> bool {
        self.state == DeviceState::DriverRunning && self.driver_pid != 0
    }

    /// Check if the device needs a respawn attempt.
    pub fn needs_respawn(&self, now_ns: u64) -> bool {
        if self.state != DeviceState::Failed {
            return false;
        }
        if self.crash_count >= MAX_CRASH_RETRIES {
            return false;
        }
        // Check if backoff has elapsed
        let elapsed = now_ns.saturating_sub(self.last_crash_ns);
        elapsed >= self.backoff_ns
    }

    /// Human-readable state name.
    pub fn state_name(&self) -> &'static str {
        match self.state {
            DeviceState::Discovered => "DISCOVERED",
            DeviceState::Matched => "MATCHED",
            DeviceState::DriverRunning => "DRIVER_RUNNING",
            DeviceState::Failed => "FAILED",
            DeviceState::Removed => "REMOVED",
            DeviceState::RemovedWait => "REMOVED_WAIT",
            DeviceState::Dead => "DEAD",
        }
    }
}

/// State transition table for validation.
pub fn is_valid_transition(from: DeviceState, event: DeviceEvent) -> bool {
    match (from, event) {
        (DeviceState::Discovered, DeviceEvent::Matched) => true,

        (DeviceState::Matched, DeviceEvent::SpawnSucceeded) => true,
        (DeviceState::Matched, DeviceEvent::SpawnFailed) => true,

        (DeviceState::DriverRunning, DeviceEvent::DriverExited(_)) => true,
        (DeviceState::DriverRunning, DeviceEvent::DriverCrashed(_)) => true,
        (DeviceState::DriverRunning, DeviceEvent::DeviceRemoved) => true,

        (DeviceState::Failed, DeviceEvent::SpawnSucceeded) => true,
        (DeviceState::Failed, DeviceEvent::SpawnFailed) => true,
        (DeviceState::Failed, DeviceEvent::DeviceRemoved) => true,

        (DeviceState::Removed, DeviceEvent::DeviceRedetected) => true,

        (DeviceState::RemovedWait, DeviceEvent::DeviceRedetected) => true,

        (DeviceState::Dead, DeviceEvent::Matched) => true,

        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pci::{PciClass, PciDevice, PciBar};

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

    #[test]
    fn happy_path_lifecycle() {
        let mut dev = DeviceEntry::new(mock_device());
        assert_eq!(dev.state, DeviceState::Discovered);

        dev.transition(DeviceEvent::Matched, 0);
        assert_eq!(dev.state, DeviceState::Matched);

        dev.transition(DeviceEvent::SpawnSucceeded, 100);
        assert_eq!(dev.state, DeviceState::DriverRunning);
        assert_eq!(dev.crash_count, 0);
    }

    #[test]
    fn crash_and_retry() {
        let mut dev = DeviceEntry::new(mock_device());
        dev.transition(DeviceEvent::Matched, 0);
        dev.transition(DeviceEvent::SpawnSucceeded, 100);

        dev.transition(DeviceEvent::DriverCrashed(139), 200);
        assert_eq!(dev.state, DeviceState::Failed);
        assert_eq!(dev.crash_count, 1);
    }

    #[test]
    fn dead_after_max_retries() {
        let mut dev = DeviceEntry::new(mock_device());
        dev.crash_count = MAX_CRASH_RETRIES;

        dev.transition(DeviceEvent::SpawnFailed, 0);
        assert_eq!(dev.state, DeviceState::Dead);
    }

    #[test]
    fn hotplug_removal_and_redetect() {
        let mut dev = DeviceEntry::new(mock_device());
        dev.transition(DeviceEvent::Matched, 0);
        dev.transition(DeviceEvent::SpawnSucceeded, 100);

        dev.transition(DeviceEvent::DeviceRemoved, 200);
        assert_eq!(dev.state, DeviceState::RemovedWait);
        assert!(dev.was_removed);

        dev.transition(DeviceEvent::DeviceRedetected, 300);
        assert_eq!(dev.state, DeviceState::Matched);
        assert!(!dev.was_removed);
    }

    #[test]
    fn invalid_transition_rejected() {
        let mut dev = DeviceEntry::new(mock_device());
        // Can't go directly from Discovered to DriverRunning
        let result = dev.transition(DeviceEvent::SpawnSucceeded, 0);
        assert!(!result);
        assert_eq!(dev.state, DeviceState::Discovered);
    }
}
