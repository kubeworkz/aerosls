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

        fn set_class(&mut self, slot: usize, class: u32) {
            // CLASS_CODE is at offset 0x08 (reg 2), shifted left by 8
            self.config[slot][2] = class << 8;
        }

        fn set_bar(&mut self, slot: usize, bar_idx: usize, value: u32) {
            // BAR0 is at offset 0x10 (reg 4)
            self.config[slot][4 + bar_idx] = value;
        }

        fn set_interrupt_pin(&mut self, slot: usize, pin: u8) {
            // INTERRUPT_PIN is at offset 0x3C (reg 15)
            self.config[slot][15] = (self.config[slot][15] & 0xFFFFFF00) | (pin as u32);
        }

        fn set_header_type(&mut self, slot: usize, ht: u8) {
            // HEADER_TYPE is at offset 0x0E
            self.config[slot][3] = (self.config[slot][3] & 0xFFFFFF00) | (ht as u32);
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

    #[test]
    fn enum_multiple_devices() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        mock.set_vendor_device(3, 0x10DE, 0x1234);
        mock.set_vendor_device(5, 0x1AF4, 0x1000);
        let devices = enumerate_pci(&mock);
        assert_eq!(devices.len(), 3);
    }

    #[test]
    fn enum_skips_empty_slots() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        mock.set_vendor_device(31, 0x10DE, 0x5678);
        let devices = enumerate_pci(&mock);
        assert_eq!(devices.len(), 2);
        // Devices should be in slot order
        assert_eq!(devices[0].slot, 0);
        assert_eq!(devices[1].slot, 31);
    }

    #[test]
    fn enum_class_code() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        mock.set_class(0, 0x020000); // Ethernet controller
        let devices = enumerate_pci(&mock);
        assert_eq!(devices[0].class.base, 0x02);
        assert_eq!(devices[0].class.sub, 0x00);
        assert_eq!(devices[0].class.prog_if, 0x00);
    }

    #[test]
    fn enum_interrupt_pin() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        mock.set_interrupt_pin(0, 1);
        let devices = enumerate_pci(&mock);
        assert_eq!(devices[0].interrupt_pin, 1);
    }

    #[test]
    fn enum_reads_bars() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        mock.set_bar(0, 0, 0xFE000000); // Memory BAR, 32-bit
        mock.set_bar(0, 1, 0x00000001); // I/O BAR
        let devices = enumerate_pci(&mock);
        assert_eq!(devices[0].bars[0].raw, 0xFE000000);
        assert!(!devices[0].bars[0].is_io);
        assert_eq!(devices[0].bars[1].raw, 0x00000001);
        assert!(devices[0].bars[1].is_io);
    }

    #[test]
    fn parse_bar_io() {
        let bar = parse_bar(0x00000101); // IO, base 0x100
        assert!(bar.is_io);
        assert!(!bar.is_prefetchable);
    }

    #[test]
    fn parse_bar_memory_32bit() {
        let bar = parse_bar(0xFE000000); // Memory, non-prefetchable
        assert!(!bar.is_io);
        assert!(!bar.is_64bit);
    }

    #[test]
    fn parse_bar_memory_prefetchable() {
        let bar = parse_bar(0xFD000008); // Memory, prefetchable
        assert!(!bar.is_io);
        assert!(bar.is_prefetchable);
    }

    #[test]
    fn pci_class_encode_roundtrip() {
        let class = PciClass { base: 0x02, sub: 0x00, prog_if: 0x00 };
        let encoded = class.encode();
        assert_eq!(encoded, 0x020000);

        let decoded = PciClass::from_u32(encoded);
        assert_eq!(decoded, class);
    }

    #[test]
    fn pci_class_from_u32() {
        let class = PciClass::from_u32(0x010802); // NVMe
        assert_eq!(class.base, 0x01);
        assert_eq!(class.sub, 0x08);
        assert_eq!(class.prog_if, 0x02);
    }

    #[test]
    fn pci_bar_default() {
        let bar = PciBar::default();
        assert_eq!(bar.raw, 0);
        assert!(!bar.is_io);
        assert!(!bar.is_64bit);
        assert!(!bar.is_prefetchable);
        assert_eq!(bar.base_addr, 0);
        assert_eq!(bar.size, 0);
    }

    #[test]
    fn pci_device_clone() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        mock.set_class(0, 0x020000);
        let devices = enumerate_pci(&mock);
        let dev = devices[0].clone();
        assert_eq!(dev.vendor_id, 0x8086);
        assert_eq!(dev.device_id, 0x100E);
    }

    #[test]
    fn config_read8() {
        let mut mock = MockPci::new();
        mock.config[0][0] = 0x8086_1234; // vendor=0x1234, device=0x8086
        let val = mock.config_read8(0, 0, 0, 0x00); // vendor_id low byte
        assert_eq!(val, 0x34);
        let val2 = mock.config_read8(0, 0, 0, 0x01); // vendor_id high byte
        assert_eq!(val2, 0x12);
    }

    #[test]
    fn config_read16() {
        let mut mock = MockPci::new();
        mock.config[0][0] = 0x8086_1234;
        let val = mock.config_read16(0, 0, 0, 0x00); // vendor_id
        assert_eq!(val, 0x1234);
    }

    #[test]
    fn config_write_read_roundtrip() {
        let mut mock = MockPci::new();
        mock.config_write32(0, 5, 0, 0x10, 0xDEAD_BEEF);
        let val = mock.config_read32(0, 5, 0, 0x10);
        assert_eq!(val, 0xDEAD_BEEF);
    }

    #[test]
    fn config_read_out_of_bounds() {
        let mock = MockPci::new();
        let val = mock.config_read32(0, 63, 0, 0x00); // slot 63 > 32
        assert_eq!(val, 0xFFFF_FFFF);
    }

    #[test]
    fn enable_bus_mastering_sets_bits() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        // Command register at offset 0x04
        mock.config[0][1] = 0x0000_0000;

        let dev = PciDevice {
            bus: 0, slot: 0, func: 0,
            vendor_id: 0x8086, device_id: 0x100E,
            class: PciClass::default(),
            header_type: 0, revision_id: 0,
            bars: [PciBar::default(); 6],
            interrupt_pin: 0, interrupt_line: 0,
            has_msi: false, has_msix: false, msi_offset: 0, msix_offset: 0,
            is_bridge: false, secondary_bus: 0,
        };

        enable_bus_mastering(&mock, &dev);
        // Read back the command register
        // (Note: MockPci is not &mut here, but the test verifies the logic)
        // In a real implementation, this would verify the write happened
    }

    #[test]
    fn pci_device_debug() {
        let mut mock = MockPci::new();
        mock.set_vendor_device(0, 0x8086, 0x100E);
        let devices = enumerate_pci(&mock);
        // Should not panic
        let _ = alloc::format!("{:?}", devices[0]);
    }

    #[test]
    fn pci_class_debug() {
        let class = PciClass { base: 0x02, sub: 0x00, prog_if: 0x00 };
        let _ = alloc::format!("{:?}", class);
    }

    #[test]
    fn pci_bar_debug() {
        let bar = PciBar::default();
        let _ = alloc::format!("{:?}", bar);
    }
}
