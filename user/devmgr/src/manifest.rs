//! Driver manifest registry and matching.
//!
//! Design: docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §5.
//!
//! The Device Manager maintains a registry of driver manifests loaded from
//! the kernel's validated manifest store. Matching uses ordered priority:
//! 1. Exact vendor/device ID match
//! 2. Class code fallback
//! 3. Compatible string match

use super::pci::PciDevice;

/// Maximum length of a driver manifest name.
pub const MANIFEST_NAME_MAX: usize = 32;

/// Maximum length of a compatible string.
pub const COMPATIBLE_MAX: usize = 64;

/// Maximum number of capability requirements per manifest.
pub const MAX_CAP_REQUIREMENTS: usize = 8;

/// Maximum number of exported services per manifest.
pub const MAX_EXPORTS: usize = 8;

/// Match result from manifest matching.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MatchResult {
    /// No match.
    NoMatch,
    /// Matched by compatible string.
    Compatible,
    /// Matched by class code.
    ClassCode,
    /// Exact vendor/device ID match.
    Exact,
}

/// Capability requirement in a driver manifest.
#[derive(Clone, Copy, Debug, Default)]
pub struct CapRequirement {
    pub cap_type: u8,     // IO_PORT, IRQ, DMA_MEM, BUS_ACCESS
    pub perm: u8,         // required permissions
    pub count: u8,        // number of this cap needed
    pub detail0: u32,     // type-specific parameter 0
    pub detail1: u32,     // type-specific parameter 1
}

/// Resource limits for a driver sidecar.
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceLimits {
    pub max_memory_bytes: u32,
    pub max_dma_frames: u32,
    pub max_channels: u16,
    pub max_irqs: u16,
    pub max_cpu_us_per_sec: u32,
    pub stack_pages: u32,
}

/// An exported service from a driver.
#[derive(Clone, Copy, Debug, Default)]
pub struct ExportedService {
    pub name: [u8; 16],   // null-terminated service name
    pub port: u16,         // IPC port
    pub version: u16,      // version number
}

/// A driver manifest loaded from the kernel's validated registry.
///
/// Binary layout: matches the packed binary format defined in
/// docs/AeroSLS-Device-Driver-SDK-Phase4-Design-v0.1.md §5.2.
#[derive(Clone, Debug)]
pub struct DriverManifest<'a> {
    /// Driver name (e.g., "drv.e1000.0").
    pub name: &'a str,
    /// PCI vendor ID (0 = any).
    pub vendor_id: u16,
    /// PCI device ID (0 = any).
    pub device_id: u16,
    /// PCI class code (0 = any).
    pub class_code: u32,
    /// Compatible string (e.g., "intel,e1000").
    pub compatible: &'a str,
    /// Driver binary image offset and size.
    pub image_offset: u32,
    pub image_size: u32,
    pub entry_offset: u32,
    /// Capability requirements.
    pub caps: [CapRequirement; MAX_CAP_REQUIREMENTS],
    pub n_caps: u8,
    /// Resource limits.
    pub limits: ResourceLimits,
    /// Exported services.
    pub exports: [ExportedService; MAX_EXPORTS],
    pub n_exports: u8,
}

impl<'a> DriverManifest<'a> {
    /// Try to match a PCI device against this manifest.
    pub fn try_match(&self, device: &PciDevice) -> MatchResult {
        // Priority 1: Exact vendor/device ID match
        if self.vendor_id != 0 && self.vendor_id == device.vendor_id {
            if self.device_id != 0 && self.device_id == device.device_id {
                return MatchResult::Exact;
            }
        }

        // Priority 2: Class code match
        if self.class_code != 0 && self.class_code == device.class.encode() {
            return MatchResult::ClassCode;
        }

        // Priority 3: Compatible string match
        // In a real implementation, this would check the device's
        // compatible strings from ACPI/DT. For PCI, we do a simple
        // vendor/device ID → compatible string lookup.
        if !self.compatible.is_empty() {
            // For now, we can't do real compatible string matching
            // without device tree integration. The matching happens
            // at a higher level when the manifest registry is populated.
            // This is a placeholder for future DT/ACPI integration.
        }

        MatchResult::NoMatch
    }

    /// Check if this manifest requires a specific capability type.
    pub fn requires_cap(&self, cap_type: u8) -> bool {
        self.caps[..self.n_caps as usize]
            .iter()
            .any(|c| c.cap_type == cap_type)
    }

    /// Get the DMA buffer requirements.
    pub fn dma_requirements(&self) -> (u32, u32) {
        // Sum up DMA_BUF requirements: (total_pages, max_align_pages)
        let mut total_pages = 0u32;
        let mut max_align = 1u32;
        for c in &self.caps[..self.n_caps as usize] {
            if c.cap_type == 0x03 { // DMA_BUF
                total_pages += c.detail0;
                if c.detail1 > max_align {
                    max_align = c.detail1;
                }
            }
        }
        (total_pages, max_align)
    }
}

/// Create a manifest for the e1000 NIC driver.
pub fn e1000_manifest<'a>() -> DriverManifest<'a> {
    DriverManifest {
        name: "drv.e1000.0",
        vendor_id: 0x8086,
        device_id: 0x100E,
        class_code: 0x020000, // Ethernet controller
        compatible: "intel,e1000",
        image_offset: 0,
        image_size: 0,
        entry_offset: 0,
        caps: [
            CapRequirement { cap_type: 0x01, perm: 0x03, count: 1, detail0: 0, detail1: 0 }, // IO_PORT
            CapRequirement { cap_type: 0x02, perm: 0x07, count: 1, detail0: 0, detail1: 0 }, // IRQ
            CapRequirement { cap_type: 0x03, perm: 0x03, count: 3, detail0: 160, detail1: 16 }, // DMA_BUF
            CapRequirement { cap_type: 0x05, perm: 0x03, count: 1, detail0: 0, detail1: 0 }, // CHAN_R
            CapRequirement { cap_type: 0x06, perm: 0x01, count: 1, detail0: 0, detail1: 0 }, // CHAN_W
            CapRequirement::default(),
            CapRequirement::default(),
            CapRequirement::default(),
        ],
        n_caps: 5,
        limits: ResourceLimits {
            max_memory_bytes: 4 * 1024 * 1024,
            max_dma_frames: 160,
            max_channels: 8,
            max_irqs: 2,
            max_cpu_us_per_sec: 100_000,
            stack_pages: 8,
        },
        exports: [ExportedService::default(); MAX_EXPORTS],
        n_exports: 0,
    }
}

/// Create a manifest for the virtio-net NIC driver.
pub fn virtio_net_manifest<'a>() -> DriverManifest<'a> {
    DriverManifest {
        name: "drv.virtio-net.0",
        vendor_id: 0x1AF4,
        device_id: 0x1000,
        class_code: 0x020000, // Ethernet controller
        compatible: "virtio,net",
        image_offset: 0,
        image_size: 0,
        entry_offset: 0,
        caps: [
            CapRequirement { cap_type: 0x01, perm: 0x03, count: 1, detail0: 0, detail1: 0 }, // IO_PORT
            CapRequirement { cap_type: 0x02, perm: 0x07, count: 1, detail0: 0, detail1: 0 }, // IRQ
            CapRequirement { cap_type: 0x03, perm: 0x03, count: 4, detail0: 70, detail1: 16 }, // DMA_BUF
            CapRequirement { cap_type: 0x05, perm: 0x03, count: 1, detail0: 0, detail1: 0 }, // CHAN_R
            CapRequirement { cap_type: 0x06, perm: 0x01, count: 1, detail0: 0, detail1: 0 }, // CHAN_W
            CapRequirement::default(),
            CapRequirement::default(),
            CapRequirement::default(),
        ],
        n_caps: 5,
        limits: ResourceLimits {
            max_memory_bytes: 2 * 1024 * 1024,
            max_dma_frames: 70,
            max_channels: 8,
            max_irqs: 1,
            max_cpu_us_per_sec: 50_000,
            stack_pages: 8,
        },
        exports: [ExportedService::default(); MAX_EXPORTS],
        n_exports: 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::pci::PciDevice;

    fn e1000_device() -> PciDevice {
        PciDevice {
            bus: 0, slot: 3, func: 0,
            vendor_id: 0x8086, device_id: 0x100E,
            class: crate::pci::PciClass { base: 0x02, sub: 0x00, prog_if: 0x00 },
            header_type: 0, revision_id: 0x03,
            bars: [crate::pci::PciBar::default(); 6],
            interrupt_pin: 1, interrupt_line: 0,
            has_msi: true, has_msix: false, msi_offset: 0, msix_offset: 0,
            is_bridge: false, secondary_bus: 0,
        }
    }

    fn virtio_device() -> PciDevice {
        PciDevice {
            bus: 0, slot: 5, func: 0,
            vendor_id: 0x1AF4, device_id: 0x1000,
            class: crate::pci::PciClass { base: 0x02, sub: 0x00, prog_if: 0x00 },
            header_type: 0, revision_id: 0,
            bars: [crate::pci::PciBar::default(); 6],
            interrupt_pin: 1, interrupt_line: 0,
            has_msi: true, has_msix: false, msi_offset: 0, msix_offset: 0,
            is_bridge: false, secondary_bus: 0,
        }
    }

    fn unknown_device() -> PciDevice {
        PciDevice {
            bus: 0, slot: 7, func: 0,
            vendor_id: 0xFFFF, device_id: 0x0001,
            class: crate::pci::PciClass { base: 0xFF, sub: 0xFF, prog_if: 0xFF },
            header_type: 0, revision_id: 0,
            bars: [crate::pci::PciBar::default(); 6],
            interrupt_pin: 0, interrupt_line: 0,
            has_msi: false, has_msix: false, msi_offset: 0, msix_offset: 0,
            is_bridge: false, secondary_bus: 0,
        }
    }

    #[test]
    fn exact_match() {
        let m = e1000_manifest();
        let dev = e1000_device();
        assert_eq!(m.try_match(&dev), MatchResult::Exact);
    }

    #[test]
    fn exact_match_virtio() {
        let m = virtio_net_manifest();
        let dev = virtio_device();
        assert_eq!(m.try_match(&dev), MatchResult::Exact);
    }

    #[test]
    fn no_match_wrong_vendor() {
        let mut m = e1000_manifest();
        m.vendor_id = 0x1234;
        let dev = e1000_device();
        assert_eq!(m.try_match(&dev), MatchResult::NoMatch);
    }

    #[test]
    fn no_match_wrong_device() {
        let mut m = e1000_manifest();
        m.device_id = 0x9999;
        let dev = e1000_device();
        assert_eq!(m.try_match(&dev), MatchResult::NoMatch);
    }

    #[test]
    fn class_code_match() {
        let mut m = e1000_manifest();
        m.vendor_id = 0;
        m.device_id = 0;
        let dev = e1000_device();
        assert_eq!(m.try_match(&dev), MatchResult::ClassCode);
    }

    #[test]
    fn class_code_match_storage() {
        let mut m = e1000_manifest();
        m.vendor_id = 0;
        m.device_id = 0;
        m.class_code = 0x010802; // NVMe
        let mut dev = e1000_device();
        dev.class = crate::pci::PciClass { base: 0x01, sub: 0x08, prog_if: 0x02 };
        assert_eq!(m.try_match(&dev), MatchResult::ClassCode);
    }

    #[test]
    fn no_match_unknown_device() {
        let m = e1000_manifest();
        let dev = unknown_device();
        assert_eq!(m.try_match(&dev), MatchResult::NoMatch);
    }

    #[test]
    fn no_match_different_class() {
        let m = e1000_manifest();
        let mut dev = e1000_device();
        dev.class = crate::pci::PciClass { base: 0x01, sub: 0x06, prog_if: 0x00 }; // SATA
        assert_eq!(m.try_match(&dev), MatchResult::NoMatch);
    }

    #[test]
    fn manifest_requires_cap() {
        let m = e1000_manifest();
        assert!(m.requires_cap(0x01)); // IO_PORT
        assert!(m.requires_cap(0x02)); // IRQ
        assert!(m.requires_cap(0x03)); // DMA_BUF
        assert!(m.requires_cap(0x05)); // CHAN_R
        assert!(m.requires_cap(0x06)); // CHAN_W
        assert!(!m.requires_cap(0xFF)); // non-existent
    }

    #[test]
    fn manifest_dma_requirements() {
        let m = e1000_manifest();
        let (pages, align) = m.dma_requirements();
        assert_eq!(pages, 160); // detail0 of DMA_BUF cap
        assert_eq!(align, 16);  // detail1 of DMA_BUF cap
    }

    #[test]
    fn manifest_dma_requirements_virtio() {
        let m = virtio_net_manifest();
        let (pages, align) = m.dma_requirements();
        assert_eq!(pages, 70);
        assert_eq!(align, 16);
    }

    #[test]
    fn manifest_dma_requirements_none() {
        let mut m = e1000_manifest();
        m.n_caps = 0; // no cap requirements
        let (pages, align) = m.dma_requirements();
        assert_eq!(pages, 0);
        assert_eq!(align, 1);
    }

    #[test]
    fn e1000_manifest_fields() {
        let m = e1000_manifest();
        assert_eq!(m.name, "drv.e1000.0");
        assert_eq!(m.vendor_id, 0x8086);
        assert_eq!(m.device_id, 0x100E);
        assert_eq!(m.compatible, "intel,e1000");
        assert_eq!(m.n_caps, 5);
        assert_eq!(m.limits.max_memory_bytes, 4 * 1024 * 1024);
        assert_eq!(m.limits.max_dma_frames, 160);
        assert_eq!(m.limits.max_channels, 8);
        assert_eq!(m.limits.max_irqs, 2);
    }

    #[test]
    fn virtio_manifest_fields() {
        let m = virtio_net_manifest();
        assert_eq!(m.name, "drv.virtio-net.0");
        assert_eq!(m.vendor_id, 0x1AF4);
        assert_eq!(m.device_id, 0x1000);
        assert_eq!(m.compatible, "virtio,net");
        assert_eq!(m.n_caps, 5);
        assert_eq!(m.limits.max_memory_bytes, 2 * 1024 * 1024);
        assert_eq!(m.limits.max_dma_frames, 70);
    }

    #[test]
    fn cap_requirement_default() {
        let cap = CapRequirement::default();
        assert_eq!(cap.cap_type, 0);
        assert_eq!(cap.perm, 0);
        assert_eq!(cap.count, 0);
    }

    #[test]
    fn resource_limits_default() {
        let limits = ResourceLimits::default();
        assert_eq!(limits.max_memory_bytes, 0);
        assert_eq!(limits.max_dma_frames, 0);
        assert_eq!(limits.max_channels, 0);
    }

    #[test]
    fn exported_service_default() {
        let svc = ExportedService::default();
        assert_eq!(svc.name, [0u8; 16]);
        assert_eq!(svc.port, 0);
        assert_eq!(svc.version, 0);
    }

    #[test]
    fn manifest_clone() {
        let m = e1000_manifest();
        let m2 = m.clone();
        assert_eq!(m.name, m2.name);
        assert_eq!(m.vendor_id, m2.vendor_id);
        assert_eq!(m.n_caps, m2.n_caps);
    }

    #[test]
    fn manifest_match_priority() {
        // Exact should be preferred over ClassCode
        let m = e1000_manifest();
        let dev = e1000_device();
        // Manifest has both vendor/device AND class code matching
        assert_eq!(m.try_match(&dev), MatchResult::Exact, "exact should win over classcode");
    }

    #[test]
    fn multiple_manifests_selection() {
        let manifests = [e1000_manifest(), virtio_net_manifest()];
        let dev = e1000_device();

        let mut matched = None;
        for (i, m) in manifests.iter().enumerate() {
            if m.try_match(&dev) == MatchResult::Exact {
                matched = Some(i);
                break;
            }
        }
        assert_eq!(matched, Some(0)); // e1000 manifest matches
    }
}
