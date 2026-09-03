//! The drv.e1000.0 driver manifest — the DM's first real spawn.
//!
//! Init spawns the DM; the DM spawns drivers. The e1000 driver is the
//! first one (Driver SDK ABI v0.1 §7): when the device registry shows a
//! NIC the kernel handed off (`driver_manifest == "drv.e1000.0"`, i.e. a
//! role-less NIC — grub `nicN=none` → `e1000_driver_handoff`), the DM
//! builds this sidecar's packed manifest and calls `Kernel::create_sidecar`.
//!
//! Everything the driver needs rides in the manifest:
//!
//! - the **image** — the kernel maps it from the footer `image_kaddr`, the
//!   e1000.image region the boot loader copied `boot/e1000.bin` into;
//!   the DM learns that address from the transient grant init attached to
//!   the registry message (never a mint — init already owns the region);
//! - the **budget** MEM cap — the e1000.heap region, which the layout
//!   places page-aligned immediately after the driver image (the same
//!   rule the POSIX/network spawns use, so `heap_base` is derived, not a
//!   second grant);
//! - the **DEV cap** (`nic0.bar0`) — the handed-off NIC's MMIO BAR0 base
//!   from the device registry, the same shape init mints into the POSIX
//!   manifest for devtest (Driver SDK ABI v0.1 §4.1). The driver maps it
//!   via SYS_DEV_MMAP.
//!
//! No console cap: the driver logs through the legacy SYS_SLS_SERIAL_WRITE
//! syscall (165) and only needs its rings/buffers (budget) plus the MMIO
//! window (DEV).

use aerosls_proto::devreg::DeviceRegistry;
use aerosls_proto::kabi::GrantedCap;
use aerosls_proto::manifest::{
    Bootstrap, Budget, CapKind, Cpu, Image, Limits, Manifest, ManifestCap,
    build_manifest, crc32, HEADER_LEN,
};
use alloc::vec::Vec;

/// The driver's registry identity — the device-registry `driver_manifest`
/// field of a handed-off NIC, and the name registered in the kernel's
/// sidecar registry when the DM spawns it. Must match `E1000_MANIFEST_NAME`
/// in user/bootimage/src/layout.rs and `BOOT_E1000_MANIFEST_NAME` in
/// kernel/boot_image.h.
pub const E1000_MANIFEST_NAME: &str = "drv.e1000.0";

/// The driver's budget heap size — must match `E1000_HEAP_BYTES` in
/// user/bootimage/src/layout.rs (the loader reserves the e1000.heap region
/// the manifest declares its budget against). 512 KiB: the driver's DMA
/// layout needs ~36 KiB (rings + RX buffers, device.rs DMA_REGION_BYTES);
/// the rest is headroom.
pub const E1000_HEAP_BYTES: u64 = 512 * 1024;

/// The e1000 MMIO BAR0 window the driver maps (the DEV cap's size). QEMU's
/// e1000 registers BAR0 as 128 KiB (0x20000); the *address* comes from the
/// device registry's PCI scan. Same constant as the POSIX sidecar's
/// devtest DEV cap (user/init/src/posix_manifest.rs E1000_BAR0_BYTES).
pub const E1000_BAR0_BYTES: u64 = 0x20000;

/// Everything the DM needs to spawn the driver: where the image lives, how
/// big it is, and which NIC's BAR0 to hand it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct E1000Spawn {
    /// Physical address of the driver binary (manifest footer image_kaddr).
    pub image_kaddr: u64,
    /// Binary size in bytes (image_size; the driver manifest's IMAGE
    /// record + the budget region's placement derive from it).
    pub image_size: u32,
    /// The handed-off NIC's MMIO BAR0 (from the device registry) — the
    /// DEV cap's physical base.
    pub bar0_phys: u64,
}

/// Decide whether — and with what — the DM should spawn drv.e1000.0 from a
/// registry message: `Some` when the registry holds a handed-off NIC
/// (driver_manifest == `E1000_MANIFEST_NAME`) AND the message carried the
/// driver-image grant init attaches second (`slots[1]`); `None` when there
/// is nothing to spawn or the message was incomplete (a grant-less boot
/// never spawns a driver that would then have nothing to map). Pure and
/// host-testable — the `Kernel::create_sidecar` call itself stays in
/// server.rs.
pub fn e1000_spawn_from_registry(
    reg: &DeviceRegistry,
    image_grant: Option<GrantedCap>,
) -> Option<E1000Spawn> {
    let entry = reg
        .iter()
        .find(|e| e.manifest_name() == Some(E1000_MANIFEST_NAME))?;
    let img = image_grant?;
    if img.base == 0 || img.len == 0 {
        return None;
    }
    Some(E1000Spawn {
        image_kaddr: img.base,
        image_size: img.len as u32,
        bar0_phys: entry.bar0_phys,
    })
}

/// The driver's budget MEM cap base: the boot layout places the e1000.heap
/// region page-aligned immediately after the driver image, so the base is
/// derived from the image grant — no second grant needed.
pub fn e1000_budget_base(spawn: &E1000Spawn) -> u64 {
    (spawn.image_kaddr + spawn.image_size as u64 + 0xFFF) & !0xFFFu64
}

/// Build the packed drv.e1000.0 manifest (header + records + image_kaddr
/// footer + patched total_len/CRC), ready for `Kernel::create_sidecar`.
pub fn build_e1000_manifest(spawn: &E1000Spawn) -> Vec<u8> {
    let heap_base = e1000_budget_base(spawn);
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W — the driver's DMA rings + buffers
            kind: CapKind::Mem {
                base: heap_base,
                size: E1000_HEAP_BYTES,
            },
        }),
        // The handed-off NIC's MMIO BAR0, mapped by the driver via
        // SYS_DEV_MMAP (Driver SDK ABI v0.1 §4.1). The base comes from the
        // kernel's PCI scan (device registry), never hardcoded.
        Some(ManifestCap {
            name: "nic0.bar0",
            rights: 0x3, // R | W
            kind: CapKind::Dev {
                base: spawn.bar0_phys,
                size: E1000_BAR0_BYTES,
            },
        }),
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
        None,
    ];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(E1000_MANIFEST_NAME),
        personality: Some("aerosls.e1000.v1"),
        image: Some(Image {
            offset: 0, // the flat package places the image first
            size: spawn.image_size,
            entry: 0,
        }),
        budget: Some(Budget {
            mem_bytes: E1000_HEAP_BYTES,
            stack_bytes: 16 * 1024,
            heap_initial: 64 * 1024,
        }),
        cpu: Some(Cpu {
            share: 50,
            preemptible: true,
        }),
        limits: Some(Limits {
            max_tasks: 1,
            max_fds: 0,
            max_channels: 8,
            max_open_files: 0,
            chan_queue_depth: 8,
        }),
        caps,
        n_caps: 2,
        // No console cap (the driver logs via SYS_SLS_SERIAL_WRITE), so no
        // bootstrap console channel either.
        bootstrap: Some(Bootstrap {
            console: None,
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        signature: None,
    };

    let mut blob = build_manifest(&m);
    // Append the image_kaddr footer, then patch total_len and body CRC so
    // the kernel's bounds walk and CRC check cover records + footer.
    blob.extend_from_slice(&spawn.image_kaddr.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());
    blob
}

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::devreg::{DRIVER_MANIFEST_LEN, MAX_DEVICES};
    use aerosls_proto::manifest::{parse_manifest, CapKind};

    fn entry(driver_manifest: &str, class: u8, subclass: u8) -> aerosls_proto::devreg::DeviceEntry {
        let mut manifest = [0u8; DRIVER_MANIFEST_LEN];
        manifest[..driver_manifest.len()].copy_from_slice(driver_manifest.as_bytes());
        aerosls_proto::devreg::DeviceEntry {
            class_code: class,
            subclass,
            vendor_id: 0x8086,
            device_id: 0x100E,
            pci_slot: 1,
            pci_bus: 0,
            bar0_phys: 0xFEBE_0000,
            irq_line: 11,
            is_64bit_bar: false,
            driver_manifest: manifest,
        }
    }

    fn reg_with(entries: &[aerosls_proto::devreg::DeviceEntry]) -> DeviceRegistry {
        let mut buf = Vec::with_capacity(4 + entries.len() * 64);
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
            buf.push(0);
            buf.push(0);
            buf.extend_from_slice(&e.driver_manifest);
        }
        unsafe { DeviceRegistry::from_raw_parts(buf.as_ptr(), buf.len()) }.unwrap()
    }

    fn image_grant() -> GrantedCap {
        GrantedCap {
            base: 0x2054_7000,
            len: 0x2000,
            ..GrantedCap::default()
        }
    }

    #[test]
    fn handed_off_nic_with_image_grant_yields_a_spawn() {
        let reg = reg_with(&[entry("drv.e1000.0", 0x02, 0x00)]);
        let s = e1000_spawn_from_registry(&reg, Some(image_grant())).expect("spawn");
        assert_eq!(s.image_kaddr, 0x2054_7000);
        assert_eq!(s.image_size, 0x2000);
        assert_eq!(s.bar0_phys, 0xFEBE_0000);
        // The budget sits page-aligned right after the image (the layout's
        // e1000.heap region).
        assert_eq!(e1000_budget_base(&s), 0x2054_9000);
    }

    #[test]
    fn kernel_owned_nic_is_not_a_spawn_target() {
        // A kernel-owned NIC carries no driver_manifest (role-aware
        // registry marking); nothing to spawn.
        let reg = reg_with(&[entry("", 0x02, 0x00)]);
        assert_eq!(e1000_spawn_from_registry(&reg, Some(image_grant())), None);
        // NVMe entries are not this DM's v1 spawn target either.
        let reg = reg_with(&[entry("drv.nvme.0", 0x01, 0x08)]);
        assert_eq!(e1000_spawn_from_registry(&reg, Some(image_grant())), None);
    }

    #[test]
    fn missing_image_grant_never_spawns() {
        let reg = reg_with(&[entry("drv.e1000.0", 0x02, 0x00)]);
        assert_eq!(e1000_spawn_from_registry(&reg, None), None);
        assert_eq!(e1000_spawn_from_registry(&reg, Some(GrantedCap::default())), None);
    }

    #[test]
    fn empty_registry_yields_no_spawn() {
        let reg = reg_with(&[]);
        assert_eq!(e1000_spawn_from_registry(&reg, Some(image_grant())), None);
    }

    #[test]
    fn blob_matches_the_kernel_parser_layout() {
        let s = E1000Spawn {
            image_kaddr: 0x2054_7000,
            image_size: 0x2000,
            bar0_phys: 0xFEBE_0000,
        };
        let blob = build_e1000_manifest(&s);

        // Header.
        assert_eq!(&blob[..8], b"AERSLSM1");
        assert_eq!(u16::from_le_bytes([blob[8], blob[9]]), 1);
        assert_eq!(
            u32::from_le_bytes([blob[16], blob[17], blob[18], blob[19]]),
            blob.len() as u32,
            "total_len includes the footer"
        );
        assert_eq!(
            u32::from_le_bytes([blob[20], blob[21], blob[22], blob[23]]),
            crc32(&blob[HEADER_LEN..])
        );

        // The whole blob re-parses with the proto parser.
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.name.unwrap(), E1000_MANIFEST_NAME);
        assert_eq!(m.n_caps, 2);
        let budget = m.find_cap("budget").expect("budget cap");
        assert_eq!(budget.rights, 0x3);
        assert_eq!(
            budget.kind,
            CapKind::Mem {
                base: e1000_budget_base(&s),
                size: E1000_HEAP_BYTES,
            }
        );
        let dev = m.find_cap("nic0.bar0").expect("nic0.bar0 cap");
        assert_eq!(dev.rights, 0x3);
        assert_eq!(
            dev.kind,
            CapKind::Dev {
                base: 0xFEBE_0000,
                size: E1000_BAR0_BYTES,
            }
        );
        // Footer holds the image kaddr.
        let n = blob.len();
        assert_eq!(
            u64::from_le_bytes(blob[n - 8..n].try_into().unwrap()),
            s.image_kaddr
        );
    }

    #[test]
    fn budget_region_fits_inside_the_registry_maximum() {
        let _ = MAX_DEVICES; // (compile-time sanity: devreg import used)
        let s = E1000Spawn {
            image_kaddr: 0x2054_7000,
            image_size: 0x2000,
            bar0_phys: 0,
        };
        assert!(e1000_budget_base(&s) + E1000_HEAP_BYTES <= 0x1_0000_0000);
    }
}
