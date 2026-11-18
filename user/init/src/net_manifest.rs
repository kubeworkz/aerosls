//! Network driver sidecar manifest construction — the real `k_create_sidecar`
//! path for spawning the network driver from init.
//!
//! The network manifest declares budget (heap) and console (kernel serial).
//! The POSIX sidecar connects via a CHAN cap wired by the kernel to
//! `drv.network.0`.

use aerosls_proto::manifest::{
    Bootstrap, Budget, CapKind, Cpu, Image, Limits, Manifest, ManifestCap,
    pack_manifest,
};
use alloc::vec::Vec;

/// The network driver's identity — registered in the kernel's sidecar registry.
pub const NET_MANIFEST_NAME: &str = "drv.network.0";

/// Build the packed network manifest blob (header + records + image_kaddr
/// footer + patched total_len/CRC), ready for `Kernel::create_sidecar`.
///
/// `image_kaddr` is the physical address of the network binary (from the
/// `net.image` MEM cap), and `heap_base` is the physical address of
/// the network budget heap (page-aligned after the image).
pub fn build_net_manifest(image_kaddr: u64, image_size: u32, heap_base: u64) -> Vec<u8> {
    let heap_size = 512 * 1024; // 512 KiB — must match NET_HEAP_BYTES in layout.rs
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
            rights: 0x7, // R | W | send
            kind: CapKind::Chan {
                peer: Some("kernel.debug.console"),
                flags: 0,
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
        name: Some(NET_MANIFEST_NAME),
        personality: Some("aerosls.network.v1"),
        image: Some(Image {
            offset: 0,
            size: image_size,
            entry: 0,
        }),
        budget: Some(Budget {
            mem_bytes: heap_size,
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
            max_channels: 16,
            max_open_files: 0,
            chan_queue_depth: 16,
        }),
        caps,
        n_caps: 2,
        bootstrap: Some(Bootstrap {
            console: Some("console"),
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        argenv: None,
        interp: None,
        signature: None,
    };

    let blob = pack_manifest(&m, image_kaddr);
    blob
}

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::manifest::{
        parse_manifest, parse_manifest_packed, ManifestErr,
    };

    #[test]
    fn blob_is_packed_v1_and_roundtrips() {
        let image_kaddr = 0x3000_0000u64;
        let blob = build_net_manifest(image_kaddr, 0x4000, 0x3000_4000);

        // The footer is the last 8 bytes and is not a record; the
        // records-only parser must refuse a packed blob.
        assert_eq!(
            u64::from_le_bytes(blob[blob.len() - 8..].try_into().unwrap()),
            image_kaddr
        );
        assert_eq!(parse_manifest(&blob), Err(ManifestErr::BadLength));

        let parsed = parse_manifest_packed(&blob).expect("net blob is packed v1");
        assert_eq!(parsed.name, Some(NET_MANIFEST_NAME));
        assert_eq!(parsed.version_major, 1);
        assert_eq!(parsed.image.unwrap().size, 0x4000);
        assert_eq!(parsed.budget.unwrap().mem_bytes, 512 * 1024);
        assert!(parsed.interp.is_none());
    }
}
