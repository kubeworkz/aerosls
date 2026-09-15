//! POSIX sidecar manifest construction — the real `k_create_sidecar` path
//! for spawning the POSIX sidecar from init.
//!
//! Follows the same pattern as dm_manifest.rs: init reads the POSIX
//! image's physical address from its `posix.image` MEM cap and the budget
//! from `posix.heap`, then packs a manifest blob for `Kernel::create_sidecar`.

use aerosls_proto::manifest::{
    Bootstrap, Budget, CapKind, Cpu, Image, Limits, Manifest, ManifestCap,
    build_manifest, crc32, HEADER_LEN,
};
use alloc::vec::Vec;

/// The POSIX sidecar's identity — registered in the kernel's sidecar registry.
pub const POSIX_MANIFEST_NAME: &str = "aerosls.posix.0";

/// The e1000 MMIO BAR0 window the devtest applet maps (Driver SDK ABI
/// v0.1 s4.1 acceptance demo). QEMU's e1000 model registers BAR0 as
/// 128 KiB (0x20000); every register the applet reads (CTRL at 0x00,
/// STATUS at 0x08, RAL0/RAH0 at 0x5400) sits well inside it. The BAR0
/// *address* comes from the kernel's PCI scan via the device registry —
/// never hardcoded — but the registry carries only the base, so the
/// size is the device model's known constant (the same way the budget
/// heap sizes are layout constants).
pub const E1000_BAR0_BYTES: u64 = 0x20000;

/// Build the packed POSIX manifest blob (header + records + image_kaddr
/// footer + patched total_len/CRC), ready for `Kernel::create_sidecar`.
///
/// `image_kaddr` is the physical address of the POSIX binary (from the
/// `posix.image` MEM cap), and `heap_base` is the physical address of
/// the POSIX budget heap (from the `posix.heap` MEM cap).
///
/// `e1000_bar0_phys` is the MMIO BAR0 base of the first e1000 NIC from
/// the device registry (init reads it from the kernel's PCI scan); when
/// the boot config has no e1000 (e.g. the smoke/guard QEMU invocations
/// without `-device e1000`), pass `None` and the manifest simply omits
/// the DEV cap — the devtest applet then skips cleanly instead of
/// failing the boot script.
pub fn build_posix_manifest(
    image_kaddr: u64,
    image_size: u32,
    heap_base: u64,
    e1000_bar0_phys: Option<u64>,
) -> Vec<u8> {
    let heap_size = 4 * 1024 * 1024; // 4 MiB — must match POSIX_HEAP_BYTES in layout.rs
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem {
                base: heap_base,
                size: heap_size,
            },
        }),
        Some(ManifestCap {
            name: "console",
            rights: 0x7, // R | W | (send)
            kind: CapKind::Chan {
                peer: Some("kernel.debug.console"),
                flags: 0,
            },
        }),
        Some(ManifestCap {
            name: "ramdisk",
            rights: 0x7, // R | W | send — block I/O channel to the ramdisk driver
            kind: CapKind::Chan {
                peer: Some("drv.ramdisk.0"),
                flags: 0,
            },
        }),
        Some(ManifestCap {
            name: "network",
            rights: 0x7, // R | W | send — network socket channel to the network driver
            kind: CapKind::Chan {
                peer: Some("drv.network.0"),
                flags: 0,
            },
        }),
        // Driver SDK ABI v0.1 s4.3 — single-use bind cap for the LAPIC
        // timer vector (32); the irqtest applet binds it to prove
        // edge-to-channel delivery on real hardware.
        Some(ManifestCap {
            name: "irq.timer.0",
            rights: 0x1, // CAP_PERM_BIND
            kind: CapKind::Irq {
                vector: 32,
                perms: 0x1, // CAP_PERM_BIND
            },
        }),
        // s4.3 — single-use bind cap for the 16550's IRQ4 (IO-APIC pin 4,
        // remapped to vector 0x24 = 36 by idt.c). k_irq_bind lazily
        // unmask the RTE; irqtest drives the UART in loopback to prove a
        // non-timer device edge reaches the channel.
        Some(ManifestCap {
            name: "irq.serial.0",
            rights: 0x1, // CAP_PERM_BIND
            kind: CapKind::Irq {
                vector: 36,
                perms: 0x1, // CAP_PERM_BIND
            },
        }),
        // s4.2 — the 16550's port range (COM1, 8 ports, R | W). irqtest
        // uses k_io_out/k_io_in to configure loopback + RDA interrupts.
        Some(ManifestCap {
            name: "uart",
            rights: 0x3, // R | W
            kind: CapKind::Io {
                base: 0x3F8,
                count: 8,
                perms: 0x3,
            },
        }),
        // Driver SDK ABI v0.1 s4.1 — the e1000's MMIO BAR0, mapped by the
        // devtest applet via SYS_DEV_MMAP to read the device registers
        // (the acceptance proof that CAP_TYPE_DEV works on the real
        // target). The base comes from the kernel's PCI scan (device
        // registry), NOT hardcoded — this is the DM-generates-manifest
        // direction in miniature (SDK doc §5). The DEV cap is
        // object-backed: k_dev_mmap maps it on demand.
        e1000_bar0_phys.map(|base| ManifestCap {
            name: "nic0.bar0",
            rights: 0x3, // R | W
            kind: CapKind::Dev {
                base,
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
    ];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(POSIX_MANIFEST_NAME),
        personality: Some("aerosls.posix.v1"),
        image: Some(Image {
            offset: 0,
            size: image_size,
            entry: 0,
        }),
        budget: Some(Budget {
            mem_bytes: heap_size,
            stack_bytes: 64 * 1024,
            heap_initial: 1024 * 1024,
        }),
        cpu: Some(Cpu {
            share: 100,
            preemptible: true,
        }),
        limits: Some(Limits {
            max_tasks: 64,
            max_fds: 128,
            max_channels: 32,
            max_open_files: 128,
            chan_queue_depth: 16,
        }),
        caps,
        n_caps: if e1000_bar0_phys.is_some() { 8 } else { 7 },
        bootstrap: Some(Bootstrap {
            console: Some("console"),
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        signature: None,
    };

    let mut blob = build_manifest(&m);
    // Append the image_kaddr footer, then patch total_len and body CRC.
    blob.extend_from_slice(&image_kaddr.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());
    blob
}

/// Build a **tenant-profile** POSIX manifest (POSIX-Environments E3): the
/// runtime a per-partition environment gets. It carries only what a tenant
/// needs — a budget MEM region, a console channel, and a block channel to
/// its OWN ramdisk (`ramdisk_peer`) — and NONE of the system profile's
/// direct-hardware caps: no network, no UART port I/O, no timer/serial IRQ
/// binds, no NIC BAR. E2's kernel-side gate (cap_create_sidecar) already
/// rejects a hardware cap in a non-system-partition manifest; this profile
/// is the builder-side half — the two together are defence in depth.
///
/// `name` is the instance's registry identity (e.g. "aerosls.posix.1"), so
/// several environments coexist; `ramdisk_peer` is that environment's own
/// ramdisk name (e.g. "drv.ramdisk.1"), resolved within the environment's
/// partition by the E2 scoped registry. `image_kaddr`/`image_size` point at
/// the shared, read-only POSIX image; `heap_base` is this instance's private
/// budget heap.
pub fn build_posix_manifest_tenant(
    name: &str,
    ramdisk_peer: &str,
    image_kaddr: u64,
    image_size: u32,
    heap_base: u64,
) -> Vec<u8> {
    let heap_size = 4 * 1024 * 1024; // 4 MiB — matches the system profile's heap
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem { base: heap_base, size: heap_size },
        }),
        Some(ManifestCap {
            name: "console",
            rights: 0x7, // R | W | send
            kind: CapKind::Chan {
                peer: Some("kernel.debug.console"),
                flags: 0,
            },
        }),
        Some(ManifestCap {
            name: "ramdisk",
            rights: 0x7, // R | W | send — block I/O to THIS env's ramdisk
            kind: CapKind::Chan {
                peer: Some(ramdisk_peer),
                flags: 0,
            },
        }),
        None, None, None, None, None,
        None, None, None, None, None, None, None, None,
    ];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(name),
        personality: Some("aerosls.posix.v1"),
        image: Some(Image { offset: 0, size: image_size, entry: 0 }),
        budget: Some(Budget {
            mem_bytes: heap_size,
            stack_bytes: 64 * 1024,
            heap_initial: 1024 * 1024,
        }),
        cpu: Some(Cpu { share: 100, preemptible: true }),
        limits: Some(Limits {
            max_tasks: 64,
            max_fds: 128,
            max_channels: 32,
            max_open_files: 128,
            chan_queue_depth: 16,
        }),
        caps,
        n_caps: 3,
        bootstrap: Some(Bootstrap {
            console: Some("console"),
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        signature: None,
    };

    let mut blob = build_manifest(&m);
    blob.extend_from_slice(&image_kaddr.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());
    blob
}

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::manifest::{parse_manifest, CapKind};

    fn build(e1000: Option<u64>) -> Vec<u8> {
        build_posix_manifest(0x3000_0000, 0x40000, 0x3040_0000, e1000)
    }

    #[test]
    fn no_e1000_omits_the_dev_cap() {
        let blob = build(None);
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.n_caps, 7);
        assert!(m.find_cap("nic0.bar0").is_none());
    }

    #[test]
    fn tenant_profile_has_only_budget_console_and_its_own_ramdisk() {
        // E3: a per-environment POSIX instance. Its own name and its own
        // ramdisk peer; exactly three caps; NONE of the system profile's
        // hardware or network caps.
        let blob = build_posix_manifest_tenant(
            "aerosls.posix.1", "drv.ramdisk.1",
            0x3000_0000, 0x40000, 0x3040_0000);
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.name, Some("aerosls.posix.1"));
        assert_eq!(m.n_caps, 3);
        // The three tenant caps are present...
        assert!(m.find_cap("budget").is_some());
        assert!(m.find_cap("console").is_some());
        let rd = m.find_cap("ramdisk").expect("ramdisk chan present");
        assert_eq!(rd.kind, CapKind::Chan { peer: Some("drv.ramdisk.1"), flags: 0 });
        // ...and every system-profile hardware/network cap is absent.
        for absent in ["network", "uart", "irq.timer.0", "irq.serial.0", "nic0.bar0"] {
            assert!(m.find_cap(absent).is_none(),
                    "tenant profile must not carry '{absent}'");
        }
    }

    #[test]
    fn two_tenant_instances_get_distinct_names_and_ramdisks() {
        // Two environments' manifests are independent: distinct identities,
        // each wired to its own ramdisk — the coexistence E2's scoped
        // registry makes resolvable.
        let blob_a = build_posix_manifest_tenant(
            "aerosls.posix.1", "drv.ramdisk.1", 0x3000_0000, 0x40000, 0x3040_0000);
        let blob_b = build_posix_manifest_tenant(
            "aerosls.posix.2", "drv.ramdisk.2", 0x3000_0000, 0x40000, 0x3800_0000);
        let a = parse_manifest(&blob_a).unwrap();
        let b = parse_manifest(&blob_b).unwrap();
        assert_ne!(a.name, b.name);
        assert_eq!(a.find_cap("ramdisk").unwrap().kind,
                   CapKind::Chan { peer: Some("drv.ramdisk.1"), flags: 0 });
        assert_eq!(b.find_cap("ramdisk").unwrap().kind,
                   CapKind::Chan { peer: Some("drv.ramdisk.2"), flags: 0 });
    }

    #[test]
    fn e1000_bar0_becomes_a_dev_cap_from_the_registry_base() {
        // The registry base (e.g. QEMU's assignment for the standard
        // -device e1000 slot) becomes the DEV cap's physical base — never
        // a hardcoded address, and the cap round-trips the packed blob.
        let base = 0xFEBE_0000u64;
        let blob = build(Some(base));
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.n_caps, 8);
        let dev = m.find_cap("nic0.bar0").unwrap();
        assert_eq!(dev.rights, 0x3); // R | W
        assert_eq!(
            dev.kind,
            CapKind::Dev {
                base,
                size: E1000_BAR0_BYTES,
            }
        );
    }
}
