//! Device registry parser for the init sidecar.
//!
//! The kernel populates a `SidecarDeviceInfo` table at boot (Phase 5 §1.3)
//! after PCI enumeration.  The init sidecar receives this as a READ-ONLY
//! MEM cap and parses it to discover which devices are available and which
//! driver manifests should be spawned.
//!
//! Wire format (little-endian, packed):
//!
//! ```text
//! offset  size  field
//! 0       4     count        number of entries
//! 4       n × 64  entries    one DeviceEntry per PCI device
//! ```
//!
//! Each `DeviceEntry` is 64 bytes:
//!
//! ```text
//! offset  size  field
//! 0       1     class_code
//! 1       1     subclass
//! 2       2     vendor_id
//! 4       2     device_id
//! 6       1     pci_slot
//! 7       1     pci_bus
//! 8       8     bar0_phys
//! 16      1     irq_line
//! 17      1     is_64bit_bar
//! 18      2     reserved (0)
//! 20      44    driver_manifest (UTF-8, null-padded)
//! ```

use core::str;

pub const MAX_DEVICES: usize = 16;
pub const DEVICE_ENTRY_SIZE: usize = 64;
pub const DRIVER_MANIFEST_LEN: usize = 44;

/// Maximum length of a driver manifest name (null-terminated in the table).
pub const MAX_MANIFEST_NAME: usize = DRIVER_MANIFEST_LEN - 1;

/// One PCI device discovered by the kernel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceEntry {
    pub class_code: u8,
    pub subclass: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub pci_slot: u8,
    pub pci_bus: u8,
    pub bar0_phys: u64,
    pub irq_line: u8,
    pub is_64bit_bar: bool,
    /// Null-terminated UTF-8 driver manifest name (e.g. `"drv.nvme.0"`).
    pub driver_manifest: [u8; DRIVER_MANIFEST_LEN],
}

impl DeviceEntry {
    /// Returns the driver manifest name as a `&str`, if valid UTF-8.
    pub fn manifest_name(&self) -> Option<&str> {
        // Find the null terminator.
        let len = self
            .driver_manifest
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(DRIVER_MANIFEST_LEN);
        str::from_utf8(&self.driver_manifest[..len]).ok()
    }

    /// PCI class code + subclass as a single u16 for convenient matching.
    pub fn class(&self) -> u16 {
        ((self.class_code as u16) << 8) | (self.subclass as u16)
    }

    /// NVMe: class 0x01, subclass 0x08.
    pub fn is_nvme(&self) -> bool {
        self.class_code == 0x01 && self.subclass == 0x08
    }

    /// Ethernet: class 0x02, subclass 0x00.
    pub fn is_ethernet(&self) -> bool {
        self.class_code == 0x02 && self.subclass == 0x00
    }

    /// VGA: class 0x03, subclass 0x00.
    pub fn is_vga(&self) -> bool {
        self.class_code == 0x03 && self.subclass == 0x00
    }
}

/// Parsed device registry — owns the data (no lifetime needed).
#[derive(Debug)]
pub struct DeviceRegistry {
    entries: [DeviceEntry; MAX_DEVICES],
    count: usize,
}

impl DeviceRegistry {
    /// Parse the device registry from a raw byte slice (the MEM cap's region).
    ///
    /// # Safety
    ///
    /// The caller must ensure `data` points to valid memory that was populated
    /// by the kernel.  All reads are bounds-checked against `len`.
    pub unsafe fn from_raw_parts(data: *const u8, len: usize) -> Result<Self, DevRegError> {
        if data.is_null() || len < 4 {
            return Err(DevRegError::TooShort);
        }

        // Read the count (first u32).
        let count = unsafe { core::ptr::read_unaligned(data as *const u32) as usize };
        if count > MAX_DEVICES {
            return Err(DevRegError::TooManyDevices);
        }

        let table_size = 4 + count * DEVICE_ENTRY_SIZE;
        if len < table_size {
            return Err(DevRegError::Truncated);
        }

        // Parse each entry.
        let mut entries = [DeviceEntry {
            class_code: 0,
            subclass: 0,
            vendor_id: 0,
            device_id: 0,
            pci_slot: 0,
            pci_bus: 0,
            bar0_phys: 0,
            irq_line: 0,
            is_64bit_bar: false,
            driver_manifest: [0u8; DRIVER_MANIFEST_LEN],
        }; MAX_DEVICES];

        for i in 0..count {
            let off = 4 + i * DEVICE_ENTRY_SIZE;
            entries[i] = DeviceEntry {
                class_code: unsafe { *data.add(off) },
                subclass: unsafe { *data.add(off + 1) },
                vendor_id: le_u16(unsafe { data.add(off + 2) }),
                device_id: le_u16(unsafe { data.add(off + 4) }),
                pci_slot: unsafe { *data.add(off + 6) },
                pci_bus: unsafe { *data.add(off + 7) },
                bar0_phys: le_u64(unsafe { data.add(off + 8) }),
                irq_line: unsafe { *data.add(off + 16) },
                is_64bit_bar: unsafe { *data.add(off + 17) } != 0,
                driver_manifest: {
                    let mut m = [0u8; DRIVER_MANIFEST_LEN];
                    let src = unsafe {
                        core::slice::from_raw_parts(data.add(off + 20), DRIVER_MANIFEST_LEN)
                    };
                    m.copy_from_slice(src);
                    m
                },
            };
        }

        Ok(DeviceRegistry { entries, count })
    }

    /// Number of discovered devices.
    pub fn len(&self) -> usize {
        self.count
    }

    /// Whether the registry is empty.
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Get a device entry by index.
    pub fn get(&self, index: usize) -> Option<&DeviceEntry> {
        if index < self.count {
            Some(&self.entries[index])
        } else {
            None
        }
    }

    /// Iterator over all entries.
    pub fn iter(&self) -> core::slice::Iter<'_, DeviceEntry> {
        self.entries[..self.count].iter()
    }

    /// Find a device by class code and subclass.
    pub fn find(&self, class_code: u8, subclass: u8) -> Option<&DeviceEntry> {
        self.entries[..self.count]
            .iter()
            .find(|e| e.class_code == class_code && e.subclass == subclass)
    }

    /// Find all devices matching a predicate.
    pub fn find_all<F: Fn(&DeviceEntry) -> bool>(
        &self,
        pred: F,
    ) -> impl Iterator<Item = &DeviceEntry> {
        self.entries[..self.count].iter().filter(move |e| pred(e))
    }

    /// Count devices by class.
    pub fn count_nvme(&self) -> usize {
        self.entries[..self.count].iter().filter(|e| e.is_nvme()).count()
    }

    pub fn count_ethernet(&self) -> usize {
        self.entries[..self.count]
            .iter()
            .filter(|e| e.is_ethernet())
            .count()
    }
}

/// Device registry parse failures.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DevRegError {
    /// The table is too short to contain a count.
    TooShort,
    /// More than `MAX_DEVICES` entries claimed.
    TooManyDevices,
    /// The table is truncated (count claims more entries than the data holds).
    Truncated,
}

impl core::fmt::Display for DevRegError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            DevRegError::TooShort => write!(f, "device registry too short"),
            DevRegError::TooManyDevices => write!(f, "device registry has too many entries"),
            DevRegError::Truncated => write!(f, "device registry truncated"),
        }
    }
}

// ── little-endian helpers ────────────────────────────────────────────────────

unsafe fn le_u16(p: *const u8) -> u16 {
    u16::from_le_bytes([unsafe { *p }, unsafe { *p.add(1) }])
}

unsafe fn le_u64(p: *const u8) -> u64 {
    let mut m = [0u8; 8];
    unsafe {
        core::ptr::copy_nonoverlapping(p, m.as_mut_ptr(), 8);
    }
    u64::from_le_bytes(m)
}

// ── tests ────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a device registry blob with the given entries.
    fn build_registry(entries: &[DeviceEntry]) -> alloc::vec::Vec<u8> {
        let mut buf = alloc::vec::Vec::with_capacity(4 + entries.len() * DEVICE_ENTRY_SIZE);
        buf.extend_from_slice(&(entries.len() as u32).to_le_bytes());
        for e in entries {
            buf.push(e.class_code);
            buf.push(e.subclass);
            buf.extend_from_slice(&e.vendor_id.to_le_bytes());
            buf.extend_from_slice(&e.device_id.to_le_bytes());
            buf.push(e.pci_slot);
            buf.push(e.pci_bus);
            buf.extend_from_slice(&e.bar0_phys.to_le_bytes());
            buf.push(e.irq_line);
            buf.push(e.is_64bit_bar as u8);
            buf.push(0); // reserved
            buf.push(0); // reserved
            buf.extend_from_slice(&e.driver_manifest);
        }
        buf
    }

    fn nvme_entry() -> DeviceEntry {
        let mut manifest = [0u8; DRIVER_MANIFEST_LEN];
        manifest[..11].copy_from_slice(b"drv.nvme.0\0");
        DeviceEntry {
            class_code: 0x01,
            subclass: 0x08,
            vendor_id: 0x144D, // Samsung
            device_id: 0xA808,
            pci_slot: 0,
            pci_bus: 0,
            bar0_phys: 0xFEBF0000,
            irq_line: 11,
            is_64bit_bar: true,
            driver_manifest: manifest,
        }
    }

    fn e1000_entry() -> DeviceEntry {
        let mut manifest = [0u8; DRIVER_MANIFEST_LEN];
        manifest[..12].copy_from_slice(b"drv.e1000.0\0");
        DeviceEntry {
            class_code: 0x02,
            subclass: 0x00,
            vendor_id: 0x8086,
            device_id: 0x100E,
            pci_slot: 1,
            pci_bus: 0,
            bar0_phys: 0xFEBE0000,
            irq_line: 11,
            is_64bit_bar: false,
            driver_manifest: manifest,
        }
    }

    #[test]
    fn parse_two_devices() {
        let blob = build_registry(&[nvme_entry(), e1000_entry()]);
        let reg = unsafe { DeviceRegistry::from_raw_parts(blob.as_ptr(), blob.len()) }.unwrap();
        assert_eq!(reg.len(), 2);
        assert!(reg.get(0).unwrap().is_nvme());
        assert!(reg.get(1).unwrap().is_ethernet());
        assert_eq!(reg.count_nvme(), 1);
        assert_eq!(reg.count_ethernet(), 1);
    }

    #[test]
    fn manifest_name_roundtrip() {
        let e = nvme_entry();
        assert_eq!(e.manifest_name(), Some("drv.nvme.0"));
        let e = e1000_entry();
        assert_eq!(e.manifest_name(), Some("drv.e1000.0"));
    }

    #[test]
    fn find_by_class() {
        let blob = build_registry(&[nvme_entry(), e1000_entry()]);
        let reg = unsafe { DeviceRegistry::from_raw_parts(blob.as_ptr(), blob.len()) }.unwrap();
        assert!(reg.find(0x01, 0x08).is_some());
        assert!(reg.find(0x02, 0x00).is_some());
        assert!(reg.find(0x03, 0x00).is_none());
    }

    #[test]
    fn rejects_too_short() {
        let data = [0u8; 2];
        let result = unsafe { DeviceRegistry::from_raw_parts(data.as_ptr(), 2) };
        assert!(matches!(result, Err(DevRegError::TooShort)));
    }

    #[test]
    fn rejects_truncated() {
        let mut blob = build_registry(&[nvme_entry()]);
        // Truncate after the count but before the entry.
        blob.truncate(8);
        let result = unsafe {
            DeviceRegistry::from_raw_parts(blob.as_ptr(), blob.len())
        };
        assert!(matches!(result, Err(DevRegError::Truncated)));
    }

    #[test]
    fn empty_registry() {
        let blob = build_registry(&[]);
        let reg = unsafe { DeviceRegistry::from_raw_parts(blob.as_ptr(), blob.len()) }.unwrap();
        assert!(reg.is_empty());
        assert_eq!(reg.len(), 0);
        assert!(reg.find(0x01, 0x08).is_none());
    }
}
