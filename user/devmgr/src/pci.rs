//! PCIe bus enumeration and config space access.
//!
//! Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §2.3-2.4.
//!
//! The Device Manager holds a BUS_ACCESS capability granting it the right
//! to enumerate the bus and read/write config space. This module provides
//! the software interface over those hardware primitives.

use alloc::vec::Vec;

/// Maximum number of PCI functions per slot (0 = single-function only).
const MAX_FUNC: u8 = 8;

/// Maximum PCI bus number to scan.
const PCI_BUS_MAX: u8 = 255;

/// Maximum devices returned from a single enumeration.
const ENUM_MAX: usize = 64;

/// PCI configuration space register offsets.
pub mod reg {
    pub const VENDOR_ID: u8 = 0x00;
    pub const DEVICE_ID: u8 = 0x02;
    pub const COMMAND: u8 = 0x04;
    pub const STATUS: u8 = 0x06;
    pub const REVISION_ID: u8 = 0x08;
    pub const PROG_IF: u8 = 0x09;
    pub const SUBCLASS: u8 = 0x0A;
    pub const CLASS_CODE: u8 = 0x0B;
    pub const CACHE_LINE_SIZE: u8 = 0x0C;
    pub const LATENCY_TIMER: u8 = 0x0D;
    pub const HEADER_TYPE: u8 = 0x0E;
    pub const BAR0: u8 = 0x10;
    pub const BAR1: u8 = 0x14;
    pub const BAR2: u8 = 0x18;
    pub const BAR3: u8 = 0x1C;
    pub const BAR4: u8 = 0x20;
    pub const BAR5: u8 = 0x24;
    pub const INTERRUPT_PIN: u8 = 0x3C;
    pub const INTERRUPT_LINE: u8 = 0x3D;

    /// PCI capability list pointer (offset 0x34).
    pub const CAP_PTR: u8 = 0x34;

    /// Capability IDs.
    pub const CAP_MSI: u8 = 0x05;
    pub const CAP_MSIX: u8 = 0x11;
}

/// PCI class code breakdown.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PciClass {
    pub base: u8,
    pub sub: u8,
    pub prog_if: u8,
}

impl PciClass {
    /// Encode as a single 24-bit class code (base:sub:prog_if).
    pub fn encode(&self) -> u32 {
        ((self.base as u32) << 16) | ((self.sub as u32) << 8) | (self.prog_if as u32)
    }

    /// Create from a 24-bit encoded class code.
    pub fn from_u32(code: u32) -> Self {
        Self {
            base: ((code >> 16) & 0xFF) as u8,
            sub: ((code >> 8) & 0xFF) as u8,
            prog_if: (code & 0xFF) as u8,
        }
    }
}

/// PCI BAR (Base Address Register) info.
#[derive(Clone, Copy, Debug, Default)]
pub struct PciBar {
    pub raw: u32,
    pub is_io: bool,
    pub is_64bit: bool,
    pub is_prefetchable: bool,
    pub base_addr: u64,
    pub size: u64,
}

/// A discovered PCI device.
#[derive(Clone, Debug)]
pub struct PciDevice {
    pub bus: u8,
    pub slot: u8,
    pub func: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub class: PciClass,
    pub header_type: u8,
    pub revision_id: u8,
    pub bars: [PciBar; 6],
    pub interrupt_pin: u8,
    pub interrupt_line: u8,
    pub has_msi: bool,
    pub has_msix: bool,
    pub msi_offset: u16,
    pub msix_offset: u16,
    pub is_bridge: bool,
    pub secondary_bus: u8,
}

/// PCI config space reader trait.
///
/// The Device Manager implements this trait using its BUS_ACCESS capability.
/// The actual MMIO/port-I/O access is done by the kernel on behalf of the
/// sidecar via IO_PORT capabilities.
pub trait PciConfigReader {
    /// Read a 32-bit value from PCI config space at the given register.
    fn config_read32(&self, bus: u8, slot: u8, func: u8, offset: u8) -> u32;

    /// Write a 32-bit value to PCI config space.
    fn config_write32(&self, bus: u8, slot: u8, func: u8, offset: u8, value: u32);

    /// Read an 8-bit value from PCI config space.
    fn config_read8(&self, bus: u8, slot: u8, func: u8, offset: u8) -> u8 {
        let val = self.config_read32(bus, slot, func, offset & 0xFC);
        ((val >> ((offset & 3) * 8)) & 0xFF) as u8
    }

    /// Read a 16-bit value from PCI config space.
    fn config_read16(&self, bus: u8, slot: u8, func: u8, offset: u8) -> u16 {
        let val = self.config_read32(bus, slot, func, offset & 0xFE);
        ((val >> ((offset & 2) * 8)) & 0xFFFF) as u16
    }
}

/// Enumerate all PCI devices using the provided config reader.
pub fn enumerate_pci<R: PciConfigReader>(reader: &R) -> Vec<PciDevice> {
    let mut devices = Vec::new();

    for bus in 0..=PCI_BUS_MAX {
        for slot in 0..32 {
            for func in 0..MAX_FUNC {
                let vendor_id = reader.config_read16(bus, slot, func, reg::VENDOR_ID);
                if vendor_id == 0xFFFF {
                    // No device at this function
                    if func == 0 {
                        break; // Skip remaining functions
                    }
                    continue;
                }

                let device_id = reader.config_read16(bus, slot, func, reg::DEVICE_ID);
                let class_raw = reader.config_read32(bus, slot, func, reg::CLASS_CODE);
                let header_type = reader.config_read8(bus, slot, func, reg::HEADER_TYPE);
                let revision_id = reader.config_read8(bus, slot, func, reg::REVISION_ID);
                let interrupt_pin = reader.config_read8(bus, slot, func, reg::INTERRUPT_PIN);

                let class = PciClass::from_u32(class_raw >> 8);
                let is_bridge = (class_raw >> 24) == 0x06;

                let mut dev = PciDevice {
                    bus,
                    slot,
                    func,
                    vendor_id,
                    device_id,
                    class,
                    header_type,
                    revision_id,
                    bars: [PciBar::default(); 6],
                    interrupt_pin,
                    interrupt_line: 0,
                    has_msi: false,
                    has_msix: false,
                    msi_offset: 0,
                    msix_offset: 0,
                    is_bridge,
                    secondary_bus: 0,
                };

                // Read BARs
                for i in 0..6 {
                    let bar_offset = reg::BAR0 + (i as u8) * 4;
                    let raw = reader.config_read32(bus, slot, func, bar_offset);
                    dev.bars[i] = parse_bar(raw);
                }

                // Read secondary bus for bridges
                if is_bridge {
                    dev.secondary_bus = reader.config_read8(bus, slot, func, 0x19);
                }

                // Detect MSI/MSI-X capabilities
                if let Some(msi_off) = find_capability(reader, bus, slot, func, reg::CAP_MSI) {
                    dev.has_msi = true;
                    dev.msi_offset = msi_off;
                }
                if let Some(msix_off) = find_capability(reader, bus, slot, func, reg::CAP_MSIX) {
                    dev.has_msix = true;
                    dev.msix_offset = msix_off;
                }

                devices.push(dev);

                if devices.len() >= ENUM_MAX {
                    return devices;
                }

                // If not multi-function, skip remaining functions
                if func == 0 && (header_type & 0x80) == 0 {
                    break;
                }
            }
        }
    }

    devices
}

/// Parse a BAR register value.
fn parse_bar(raw: u32) -> PciBar {
    let is_io = (raw & 1) != 0;
    let is_prefetchable = (raw & 0x08) != 0;
    let is_64bit = !is_io && ((raw >> 1) & 0x03) == 0x02;

    PciBar {
        raw,
        is_io,
        is_64bit,
        is_prefetchable,
        base_addr: 0, // Would be computed from BAR write
        size: 0,      // Would be computed by BAR sizing
    }
}

/// Walk the PCI capability list to find a specific capability.
fn find_capability<R: PciConfigReader>(
    reader: &R,
    bus: u8,
    slot: u8,
    func: u8,
    cap_id: u8,
) -> Option<u16> {
    let mut offset = reader.config_read8(bus, slot, func, reg::CAP_PTR);
    let mut seen = 0u32;

    // Capabilities are in the lower 8 bits of the status register;
    // bit 4 of the capability pointer indicates if the list is valid.
    while offset != 0 && offset < 0xFF && seen < 64 {
        let next = reader.config_read8(bus, slot, func, offset);
        let id = reader.config_read8(bus, slot, func, offset + 1);

        if id == cap_id {
            return Some(offset as u16);
        }

        // Follow the linked list (low 8 bits of the "next" pointer)
        offset = next & 0xFC;
        seen += 1;
    }
    None
}

/// Enable bus mastering and memory space on a PCI device.
pub fn enable_bus_mastering<R: PciConfigReader>(reader: &R, dev: &PciDevice) {
    let cmd = reader.config_read32(dev.bus, dev.slot, dev.func, reg::COMMAND);
    let new_cmd = cmd | (1 << 1) | (1 << 2); // Memory Space + Bus Master
    reader.config_write32(dev.bus, dev.slot, dev.func, reg::COMMAND, new_cmd);
}

/// Read the PCI device's interrupt line register.
pub fn read_interrupt_line<R: PciConfigReader>(reader: &R, dev: &PciDevice) -> u8 {
    reader.config_read8(dev.bus, dev.slot, dev.func, reg::INTERRUPT_LINE)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Mock PCI config space for testing.
    struct MockPci {
        config: [[u32; 64]; 32], // [slot][register/4]
    }

    impl MockPci {
        fn new() -> Self {
            Self { config: [[0u32; 64]; 32] }
        }

        fn set_vendor_device(&mut self, slot: usize, vendor: u16, device: u16) {
            self.config[slot][0] = (device as u32) << 16 | vendor as u32;
        }
    }

    impl PciConfigReader for MockPci {
        fn config_read32(&self, _bus: u8, slot: u8, _func: u8, offset: u8) -> u32 {
            let reg = (offset / 4) as usize;
            if (slot as usize) < self.config.len() && reg < 64 {
                self.config[slot as usize][reg]
            } else {
                0xFFFF_FFFF
            }
        }

        fn config_write32(&mut self, _bus: u8, slot: u8, _func: u8, offset: u8, value: u32) {
            let reg = (offset / 4) as usize;
            if (slot as usize) < self.config.len() && reg < 64 {
                self.config[slot as usize][reg] = value;
            }
        }
    }

    #[test]
    fn enum_empty_bus() {
        let mock = MockPci::new();
        let devices = enumerate_pci(&mock);
        assert!(devices.is_empty());
    }

    #[test]
    fn enum_one_device() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        let devices = enumerate_pci(&mock);
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0].vendor_id, 0x8086);
        assert_eq!(devices[0].device_id, 0x100E);
    }
}
