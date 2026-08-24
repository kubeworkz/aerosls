//! Device Manager manifest construction — the "real `k_create_sidecar`"
//! path (kernel/cap.c `sys_sls_create_sidecar`, SYS_SLS_CREATE_SIDECAR).
//!
//! The init sidecar spawns the Device Manager by handing the kernel a
//! packed manifest blob: the records `build_manifest` emits (now on the
//! kernel's TAG_IMAGE 24-byte wire format) PLUS the 8-byte `image_kaddr`
//! footer, with `total_len` and the body CRC patched to cover the footer —
//! exactly what the kernel's parser and image mapper expect (the kernel
//! copies the DM binary from the footer's physical address into the
//! child's address space).
//!
//! The DM binary itself is loaded by the boot image (Phase 5 deliverable
//! #7); the init sidecar learns where via its `dm.image` MEM cap (see
//! entry.rs). This module only packs the metadata.

use aerosls_proto::manifest::{
    Bootstrap, Budget, CapKind, Cpu, Image, Limits, Manifest, ManifestCap,
    build_manifest, crc32, HEADER_LEN,
};
use alloc::vec::Vec;

/// The Device Manager's identity — the name registered in the kernel's
/// sidecar registry, so other manifests can wire channels to it.
pub const DM_MANIFEST_NAME: &str = "drv.device_manager.0";

/// Where the DM binary lives, per the boot image contract.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DmImage {
    /// Physical address of the DM binary (the manifest footer's image_kaddr).
    pub kaddr: u64,
    /// Binary size in bytes (image_size).
    pub size: u32,
    /// Entry offset within the binary (entry_offset).
    pub entry: u64,
}

/// The Device Manager's budget MEM cap base. The boot layout places the
/// DM's heap right after its code: image end, page-aligned. This is the
/// region the kernel mints into the DM's table as the "budget" cap and
/// what the DM sidecar stands its heap over (the same contract as init's
/// own budget cap).
pub fn dm_budget_base(image: &DmImage) -> u64 {
    (image.kaddr + image.size as u64 + 0xFFF) & !0xFFFu64
}

/// Build the packed DM manifest blob (header + records + image_kaddr
/// footer + patched total_len/CRC), ready to hand to `Kernel::create_sidecar`.
pub fn build_dm_manifest(image: &DmImage) -> Vec<u8> {
    let budget_base = dm_budget_base(image);
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem {
                base: budget_base,
                size: 256 * 1024, // 256 KiB heap
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
        name: Some(DM_MANIFEST_NAME),
        personality: Some("aerosls.device_manager.v1"),
        image: Some(Image {
            offset: 0, // the flat package places the image first
            size: image.size,
            entry: image.entry,
        }),
        budget: Some(Budget {
            mem_bytes: 256 * 1024,
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
        signature: None,
    };

    let mut blob = build_manifest(&m);
    // Append the image_kaddr footer, then patch total_len and the body CRC
    // so the kernel's bounds walk and CRC check cover records + footer.
    blob.extend_from_slice(&image.kaddr.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());
    blob
}

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::manifest::{TAG_IMAGE, TAG_NAME};

    fn le_u16(b: &[u8], i: usize) -> u16 {
        u16::from_le_bytes([b[i], b[i + 1]])
    }
    fn le_u32(b: &[u8], i: usize) -> u32 {
        u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
    }
    fn le_u64(b: &[u8], i: usize) -> u64 {
        u64::from_le_bytes([
            b[i], b[i + 1], b[i + 2], b[i + 3], b[i + 4], b[i + 5], b[i + 6], b[i + 7],
        ])
    }

    #[test]
    fn blob_matches_kernel_parser_layout() {
        let image = DmImage {
            kaddr: 0x3000_0000,
            size: 0x4000,
            entry: 0x1000,
        };
        let blob = build_dm_manifest(&image);

        // Header the kernel reads first.
        assert_eq!(&blob[..8], b"AERSLSM1");
        assert_eq!(le_u16(&blob, 8), 1); // version_major
        // record_count: name, personality, image, budget, cpu, limits,
        // budget-MEM cap, console-CHAN cap, bootstrap, flags = 10.
        assert_eq!(le_u16(&blob, 12), 10);
        assert_eq!(le_u32(&blob, 16), blob.len() as u32); // total_len incl. footer

        // Record walk exactly like kernel/cap.c, collecting the IMAGE
        // payload and the NAME.
        let mut off = HEADER_LEN;
        let mut name: Option<&[u8]> = None;
        let mut image_payload: Option<&[u8]> = None;
        let record_count = le_u16(&blob, 12);
        for _ in 0..record_count {
            assert!(off + 4 <= blob.len(), "record header within blob");
            let tag = le_u16(&blob, off);
            let rlen = le_u16(&blob, off + 2) as usize;
            assert!(off + 4 + rlen <= blob.len(), "record payload within blob");
            let p = &blob[off + 4..off + 4 + rlen];
            match tag {
                TAG_NAME => name = Some(p),
                TAG_IMAGE => image_payload = Some(p),
                _ => {}
            }
            off += 4 + rlen;
        }

        // NAME: name_len u16 + "drv.device_manager.0".
        let n = name.expect("TAG_NAME present");
        let nlen = le_u16(n, 0) as usize;
        assert_eq!(&n[2..2 + nlen], DM_MANIFEST_NAME.as_bytes());

        // IMAGE: 24 bytes, kernel offsets (entry u64, blob_offset u32,
        // image_size u32, image_kaddr u64).
        let p = image_payload.expect("TAG_IMAGE present");
        assert_eq!(p.len(), 24);
        assert_eq!(le_u64(p, 0), image.entry, "entry_offset at rp+0");
        assert_eq!(le_u32(p, 8), 0, "blob_offset at rp+8");
        assert_eq!(le_u32(p, 12), image.size, "image_size at rp+12");
        assert_eq!(le_u64(p, 16), 0, "image_kaddr in-record is 0");

        // Footer: 8 bytes right after the records, holding image_kaddr.
        assert_eq!(off + 8, blob.len(), "footer is the last 8 bytes");
        assert_eq!(le_u64(&blob, off), image.kaddr, "footer image_kaddr");

        // Body CRC covers records + footer, matching the header.
        assert_eq!(le_u32(&blob, 20), crc32(&blob[HEADER_LEN..]));
    }

    #[test]
    fn budget_sits_after_the_image() {
        let image = DmImage {
            kaddr: 0x3000_0000,
            size: 0x4000,
            entry: 0,
        };
        assert_eq!(dm_budget_base(&image), 0x3000_4000);
    }
}
