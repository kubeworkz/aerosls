//! The boot-image builder: takes the init + Device Manager flat binaries,
//! assigns each a physical address from the layout, packs both manifests
//! (the same wire format `kernel/cap.c` parses, with the `image_kaddr`
//! footer), and assembles `sidecars.cpio` — a plain `newc` initrd that
//! U-Boot / GRUB / OpenSBI can load.
//!
//! # The "image at its manifest-declared physical address" contract
//!
//! The builder computes a deterministic physical layout (layout.rs), bakes
//! each image's address into its manifest's footer (`image_kaddr`) and into
//! the parent's MEM caps, and records every region + file offset in the
//! archive's `boot/layout` entry. The kernel's boot-time sidecar loader
//! (kernel/boot_image.h — the consumer) copies each image from the archive
//! to its declared address, reserves `[base_phys, base_phys + total_size)`
//! from the frame pool, zeroes the heap/registry regions, and creates the
//! init sidecar from `boot/init.manifest`.
//!
//! The two manifests cross-check each other: the init manifest's `dm.image`
//! MEM cap (what init's runtime `create_sidecar` call copies) points at the
//! same physical address the DM manifest's own footer declares.

use crate::layout::{BootImageSpec, BootLayout, DM_MANIFEST_NAME, INIT_MANIFEST_NAME, POSIX_MANIFEST_NAME, RAMDISK_MANIFEST_NAME};
use crate::newc;
use aerosls_proto::manifest::{
    Bootstrap, Budget, CapKind, Cpu, Image, Limits, Manifest, ManifestCap, build_manifest, crc32,
    HEADER_LEN,
};

/// Console channel peer — a kernel-owned service (kernel/cap.c wires
/// `kernel.*` peers to the kernel context, pid 0).
pub const CONSOLE_PEER: &str = "kernel.debug.console";

/// Ramdisk driver peer — the POSIX sidecar's block device endpoint.
pub const RAMDISK_PEER: &str = "drv.ramdisk.0";

/// The whole archive entry list, in order. The registry region is NOT an
/// entry: it is implicit memory the loader reserves and zeroes (devreg.rs
/// format), and the heap regions are implicit too — the archive carries no
/// 16 MiB of zeros.
pub const ENTRY_PATHS: [&str; 9] = [
    crate::layout::INIT_BIN_PATH,
    crate::layout::INIT_MANIFEST_PATH,
    crate::layout::DM_BIN_PATH,
    crate::layout::DM_MANIFEST_PATH,
    crate::layout::POSIX_BIN_PATH,
    crate::layout::POSIX_MANIFEST_PATH,
    crate::layout::RAMDISK_BIN_PATH,
    crate::layout::RAMDISK_MANIFEST_PATH,
    crate::layout::LAYOUT_PATH,
];

/// `build_manifest` + the 8-byte `image_kaddr` footer, with `total_len` and
/// the body CRC patched to cover records + footer — exactly what the
/// kernel's parser and image mapper expect (cap.c walks the records to find
/// the footer offset, then reads `image_kaddr` from it).
fn pack_with_footer(m: &Manifest<'_>, image_kaddr: u64) -> Vec<u8> {
    let mut blob = build_manifest(m);
    blob.extend_from_slice(&image_kaddr.to_le_bytes());
    let total = blob.len() as u32;
    blob[16..20].copy_from_slice(&total.to_le_bytes());
    let crc = crc32(&blob[HEADER_LEN..]);
    blob[20..24].copy_from_slice(&crc.to_le_bytes());
    blob
}

/// Build the init sidecar's packed manifest. Caps, in record order:
/// `budget` (its bump heap), `console` (kernel service), `device_registry`
/// (kernel-populated, read-only), `dm.image` (the DM binary's physical
/// address — what entry.rs hands `create_sidecar`), `posix.image`
/// (the POSIX sidecar binary), `posix.heap` (POSIX sidecar budget).
fn build_init_manifest(spec: &BootImageSpec, layout: &BootLayout, blob_offset: u32) -> Vec<u8> {
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem {
                base: layout.init_heap.phys,
                size: spec.init_heap_bytes,
            },
        }),
        Some(ManifestCap {
            name: "console",
            rights: 0x7, // R | W | send
            kind: CapKind::Chan {
                peer: Some(CONSOLE_PEER),
                flags: 0,
            },
        }),
        Some(ManifestCap {
            name: "device_registry",
            rights: 0x1, // R only (read-only view of the kernel's table)
            kind: CapKind::Mem {
                base: layout.registry.phys,
                size: spec.registry_bytes,
            },
        }),
        Some(ManifestCap {
            name: "dm.image",
            rights: 0x1, // R — init only reads the DM binary out of it
            kind: CapKind::Mem {
                base: layout.dm_image.phys,
                size: spec.dm_bin.len() as u64,
            },
        }),
        Some(ManifestCap {
            name: "posix.image",
            rights: 0x1, // R — init reads the POSIX binary out of it
            kind: CapKind::Mem {
                base: layout.posix_image.phys,
                size: spec.posix_bin.len() as u64,
            },
        }),
        Some(ManifestCap {
            name: "ramdisk.image",
            rights: 0x1, // R — init reads the ramdisk binary out of it
            kind: CapKind::Mem {
                base: layout.ramdisk_image.phys,
                size: spec.ramdisk_bin.len() as u64,
            },
        }),
        None, // ramdisk.heap — NOT in init's manifest; the ramdisk driver's own
              // manifest declares its budget at this address (avoids cap_create_mem overlap)
        None, // storage — NOT in init's manifest; only ramdisk needs it
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
        name: Some(INIT_MANIFEST_NAME),
        personality: Some("aerosls.init.v1"),
        image: Some(Image {
            offset: blob_offset,
            size: spec.init_bin.len() as u32,
            entry: spec.init_entry,
        }),
        budget: Some(Budget {
            mem_bytes: spec.init_heap_bytes,
            stack_bytes: 128 * 1024,
            heap_initial: 4 * 1024 * 1024,
        }),
        cpu: Some(Cpu {
            share: 300,
            preemptible: false,
        }),
        limits: Some(Limits {
            max_tasks: 1,
            max_fds: 32,
            max_channels: 64,
            max_open_files: 32,
            chan_queue_depth: 16,
        }),
        caps,
        n_caps: 6,
        bootstrap: Some(Bootstrap {
            console: Some("console"),
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        signature: None,
    };
    pack_with_footer(&m, layout.init_image.phys)
}

/// Build the Device Manager's packed manifest — the same records
/// `user/init/src/dm_manifest.rs` produces at runtime (the archive copy is
/// the boot-time reference; init re-packs from its `dm.image` cap, which
/// this builder pinned to the same address).
fn build_dm_manifest(spec: &BootImageSpec, layout: &BootLayout, blob_offset: u32) -> Vec<u8> {
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem {
                base: layout.dm_heap.phys,
                size: spec.dm_heap_bytes,
            },
        }),
        Some(ManifestCap {
            name: "console",
            rights: 0x7,
            kind: CapKind::Chan {
                peer: Some(CONSOLE_PEER),
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
            offset: blob_offset,
            size: spec.dm_bin.len() as u32,
            entry: spec.dm_entry,
        }),
        budget: Some(Budget {
            mem_bytes: spec.dm_heap_bytes,
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
    pack_with_footer(&m, layout.dm_image.phys)
}

/// Build the POSIX sidecar's packed manifest — budget + console, mirroring
/// the POSIX sidecar's expected BIB (budget is the heap region, console is
/// the kernel serial service).
fn build_posix_manifest(spec: &BootImageSpec, layout: &BootLayout, blob_offset: u32) -> Vec<u8> {
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem {
                base: layout.posix_heap.phys,
                size: spec.posix_heap_bytes,
            },
        }),
        Some(ManifestCap {
            name: "console",
            rights: 0x7, // R | W | send
            kind: CapKind::Chan {
                peer: Some(CONSOLE_PEER),
                flags: 0,
            },
        }),
        Some(ManifestCap {
            name: "ramdisk",
            rights: 0x7, // R | W | send
            kind: CapKind::Chan {
                peer: Some(RAMDISK_PEER),
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
    ];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(POSIX_MANIFEST_NAME),
        personality: Some("aerosls.posix.v1"),
        image: Some(Image {
            offset: blob_offset,
            size: spec.posix_bin.len() as u32,
            entry: spec.posix_entry,
        }),
        budget: Some(Budget {
            mem_bytes: spec.posix_heap_bytes,
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
        n_caps: 2,
        bootstrap: Some(Bootstrap {
            console: Some("console"),
            debug: None,
            log_level: 1,
        }),
        flags_value: Some(0),
        signature: None,
    };
    pack_with_footer(&m, layout.posix_image.phys)
}

/// Build the ramdisk driver's packed manifest — budget + storage + console.
fn build_ramdisk_manifest(spec: &BootImageSpec, layout: &BootLayout, blob_offset: u32) -> Vec<u8> {
    let caps = [
        Some(ManifestCap {
            name: "budget",
            rights: 0x3, // R | W
            kind: CapKind::Mem {
                base: layout.ramdisk_heap.phys,
                size: spec.ramdisk_heap_bytes,
            },
        }),
        Some(ManifestCap {
            name: "storage",
            rights: 0x1, // R only — read-only block device
            kind: CapKind::Mem {
                base: layout.storage.phys,
                size: spec.storage_bytes,
            },
        }),
        Some(ManifestCap {
            name: "console",
            rights: 0x7, // R | W | send
            kind: CapKind::Chan {
                peer: Some(CONSOLE_PEER),
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
    ];
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        name: Some(RAMDISK_MANIFEST_NAME),
        personality: Some("aerosls.ramdisk.v1"),
        image: Some(Image {
            offset: blob_offset,
            size: spec.ramdisk_bin.len() as u32,
            entry: spec.ramdisk_entry,
        }),
        budget: Some(Budget {
            mem_bytes: spec.ramdisk_heap_bytes,
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
            chan_queue_depth: 32,
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
    pack_with_footer(&m, layout.ramdisk_image.phys)
}

/// Byte span of one newc entry (header + padded name + padded data).
fn entry_span(off: usize, name: &str, data_len: usize) -> (usize, usize) {
    let name_len = name.len() + 1;
    let start = off;
    let end = start + 110 + align4(name_len) + align4(data_len);
    (start, end)
}

fn align4(n: usize) -> usize {
    (n + 3) & !3
}

/// Build the `boot/layout` text entry. Fixed-width (`{:016x}`) numeric
/// fields so the entry's size is computable without iteration, and the
/// kernel loader can parse it with fixed-width scans. Lines:
///
/// ```text
/// AEROSLS-BOOT-LAYOUT 1
/// base <phys>
/// region <name> <phys> <size>        (init.image, init.heap, dm.image,
///                                     dm.heap, registry — memory order)
/// file <path> <archive offset> <size> (every entry, incl. this file)
/// ```
///
/// `entry_offsets` must list EVERY entry with the layout file itself as
/// the LAST element: the first N−1 become `file` lines, and the last
/// element supplies this file's own offset (its size is `layout_size`, the
/// fixed total — a line's length never depends on its values).
fn build_layout_file(
    layout: &BootLayout,
    entry_offsets: &[(String, usize, usize)],
    layout_size: usize,
) -> String {
    let mut s = String::new();
    s.push_str("AEROSLS-BOOT-LAYOUT 1\n");
    s.push_str(&format!("base {:016x}\n", layout.base_phys));
    for (name, r) in layout.regions() {
        s.push_str(&format!("region {name} {:016x} {:016x}\n", r.phys, r.size));
    }
    for (path, off, size) in &entry_offsets[..entry_offsets.len() - 1] {
        s.push_str(&format!("file {path} {off:016x} {size:016x}\n"));
    }
    let (_, off, _) = entry_offsets.last().unwrap();
    s.push_str(&format!("file {} {off:016x} {layout_size:016x}\n", crate::layout::LAYOUT_PATH));
    s
}

/// The built boot image.
#[derive(Debug)]
pub struct BootImage {
    /// The `newc` initrd archive (`sidecars.cpio`).
    pub archive: Vec<u8>,
    /// The computed physical layout.
    pub layout: BootLayout,
    /// Every archive entry's `(path, offset, size)` — the ground truth the
    /// `boot/layout` entry is built from.
    pub entry_offsets: Vec<(String, usize, usize)>,
}

impl BootImage {
    /// The bytes of the archive entry at `path` (panics if absent).
    pub fn archive_entry(&self, path: &str) -> Vec<u8> {
        newc::parse_archive(&self.archive)
            .unwrap()
            .into_iter()
            .find(|e| e.name == path)
            .unwrap_or_else(|| panic!("archive entry {path} missing"))
            .data
    }
}

/// Assemble the boot image from a spec. Deterministic: the same inputs
/// produce the same archive byte-for-byte.
pub fn build_boot_image(spec: &BootImageSpec) -> BootImage {
    let layout = crate::layout::compute_layout(spec);

    // Manifest sizes are fixed regardless of the blob_offset value
    // (fixed-length records), so build once with a placeholder to learn the
    // sizes before computing any archive offsets.
    let init_manifest_ph = build_init_manifest(spec, &layout, 0);
    let dm_manifest_ph = build_dm_manifest(spec, &layout, 0);
    let posix_manifest_ph = build_posix_manifest(spec, &layout, 0);
    let ramdisk_manifest_ph = build_ramdisk_manifest(spec, &layout, 0);

    // All nine entries in order; each entry's span is
    // 110 (header) + padded name + padded data.
    let mut off = 0usize;
    let mut entry_offsets: Vec<(String, usize, usize)> = Vec::new();
    {
        let mut place = |path: &str, data_len: usize, off: &mut usize| {
            let (start, end) = entry_span(*off, path, data_len);
            entry_offsets.push((path.to_string(), start, data_len));
            *off = end;
        };
        place(crate::layout::INIT_BIN_PATH, spec.init_bin.len(), &mut off);
        place(crate::layout::INIT_MANIFEST_PATH, init_manifest_ph.len(), &mut off);
        place(crate::layout::DM_BIN_PATH, spec.dm_bin.len(), &mut off);
        place(crate::layout::DM_MANIFEST_PATH, dm_manifest_ph.len(), &mut off);
        place(crate::layout::POSIX_BIN_PATH, spec.posix_bin.len(), &mut off);
        place(crate::layout::POSIX_MANIFEST_PATH, posix_manifest_ph.len(), &mut off);
        place(crate::layout::RAMDISK_BIN_PATH, spec.ramdisk_bin.len(), &mut off);
        place(crate::layout::RAMDISK_MANIFEST_PATH, ramdisk_manifest_ph.len(), &mut off);
        let layout_size = layout_file_size();
        place(crate::layout::LAYOUT_PATH, layout_size, &mut off);
    }

    // Rebuild all manifests with the truthful blob_offset (the image's
    // archive offset). Sizes are unchanged.
    let init_bin_off = entry_offsets[0].1 as u32;
    let dm_bin_off = entry_offsets[2].1 as u32;
    let posix_bin_off = entry_offsets[4].1 as u32;
    let ramdisk_bin_off = entry_offsets[6].1 as u32;
    let init_manifest = build_init_manifest(spec, &layout, init_bin_off);
    let dm_manifest = build_dm_manifest(spec, &layout, dm_bin_off);
    let posix_manifest = build_posix_manifest(spec, &layout, posix_bin_off);
    let ramdisk_manifest = build_ramdisk_manifest(spec, &layout, ramdisk_bin_off);
    debug_assert_eq!(init_manifest.len(), init_manifest_ph.len());
    debug_assert_eq!(dm_manifest.len(), dm_manifest_ph.len());
    debug_assert_eq!(posix_manifest.len(), posix_manifest_ph.len());
    debug_assert_eq!(ramdisk_manifest.len(), ramdisk_manifest_ph.len());

    let layout_size = layout_file_size();
    // All nine entries, the layout file LAST.
    let layout_text = build_layout_file(&layout, &entry_offsets, layout_size);
    debug_assert_eq!(layout_text.len(), layout_size);

    // Assemble.
    let mut archive = Vec::new();
    newc::write_entry(&mut archive, crate::layout::INIT_BIN_PATH, &spec.init_bin);
    newc::write_entry(&mut archive, crate::layout::INIT_MANIFEST_PATH, &init_manifest);
    newc::write_entry(&mut archive, crate::layout::DM_BIN_PATH, &spec.dm_bin);
    newc::write_entry(&mut archive, crate::layout::DM_MANIFEST_PATH, &dm_manifest);
    newc::write_entry(&mut archive, crate::layout::POSIX_BIN_PATH, &spec.posix_bin);
    newc::write_entry(&mut archive, crate::layout::POSIX_MANIFEST_PATH, &posix_manifest);
    newc::write_entry(&mut archive, crate::layout::RAMDISK_BIN_PATH, &spec.ramdisk_bin);
    newc::write_entry(&mut archive, crate::layout::RAMDISK_MANIFEST_PATH, &ramdisk_manifest);
    newc::write_entry(&mut archive, crate::layout::LAYOUT_PATH, layout_text.as_bytes());
    newc::finish(&mut archive);

    BootImage {
        archive,
        layout,
        entry_offsets,
    }
}

/// The layout entry's total size: the header, base, 5 region lines and 5
/// file lines — every numeric field is 16 hex chars and every name/path is
/// a constant, so the size is a constant regardless of the values. Computed
/// once here and mirrored in `build_layout_file`'s last line.
fn layout_file_size() -> usize {
    let mut n = "AEROSLS-BOOT-LAYOUT 1\n".len();
    n += "base ".len() + 16 + 1;
    for name in ["init.image", "init.heap", "dm.image", "dm.heap", "posix.image", "posix.heap", "ramdisk.image", "ramdisk.heap", "storage", "registry"] {
        n += "region ".len() + name.len() + 1 + 16 + 1 + 16 + 1;
    }
    for p in ENTRY_PATHS {
        n += "file ".len() + p.len() + 1 + 16 + 1 + 16 + 1;
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::manifest::{
        TAG_BUDGET, TAG_CAP_CHAN, TAG_CAP_MEM, TAG_IMAGE, TAG_NAME, parse_manifest,
    };

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

    fn spec() -> BootImageSpec {
        BootImageSpec::new(vec![0xAA; 0x2000], vec![0xBB; 0x4000], vec![0xCC; 0x8000], vec![0xDD; 0x1000])
    }

    fn built() -> BootImage {
        build_boot_image(&spec())
    }

    /// Walk a packed manifest exactly like kernel/cap.c cap_create_sidecar:
    /// magic + version, then a bounded TLV walk, then the footer's
    /// image_kaddr at the end of the records. Returns the collected
    /// NAME/IMAGE/BUDGET payloads and the footer kaddr.
    fn kernel_walk(
        blob: &[u8],
    ) -> (Option<&[u8]>, Option<&[u8]>, Option<&[u8]>, u64) {
        assert_eq!(&blob[..8], b"AERSLSM1");
        assert_eq!(le_u16(blob, 8), 1, "version_major");
        let total_len = le_u32(blob, 16) as usize;
        assert_eq!(total_len, blob.len(), "total_len covers records + footer");
        assert_eq!(le_u32(blob, 20), crc32(&blob[HEADER_LEN..]), "body CRC");

        let mut name = None;
        let mut image = None;
        let mut budget = None;
        let mut off = HEADER_LEN;
        let record_count = le_u16(blob, 12);
        for _ in 0..record_count {
            assert!(off + 4 <= total_len, "record header within blob");
            let tag = le_u16(blob, off);
            let rlen = le_u16(blob, off + 2) as usize;
            assert!(off + 4 + rlen <= total_len, "record payload within blob");
            let p = &blob[off + 4..off + 4 + rlen];
            match tag {
                TAG_NAME => name = Some(p),
                TAG_IMAGE => image = Some(p),
                TAG_BUDGET => budget = Some(p),
                TAG_CAP_MEM | TAG_CAP_CHAN => { /* validated by parse_manifest */ }
                _ => {}
            }
            off += 4 + rlen;
        }
        // Footer: image_kaddr u64 right after the last record.
        assert_eq!(off + 8, blob.len(), "footer is the last 8 bytes");
        let kaddr = le_u64(blob, off);
        (name, image, budget, kaddr)
    }

    fn read_name(p: &[u8]) -> String {
        let nlen = le_u16(p, 0) as usize;
        String::from_utf8(p[2..2 + nlen].to_vec()).unwrap()
    }

    #[test]
    fn archive_is_valid_newc_with_all_entries() {
        let b = built();
        let es = crate::newc::parse_archive(&b.archive).unwrap();
        let paths: Vec<&str> = es.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(paths, ENTRY_PATHS);
        assert_eq!(es[0].data, spec().init_bin);
        assert_eq!(es[2].data, spec().dm_bin);
        assert_eq!(es[6].data, spec().ramdisk_bin);
        // Manifest + layout entries parse as non-empty text/blobs.
        assert!(es[1].data.len() > 24);
        assert!(es[3].data.len() > 24);
        assert!(es[5].data.len() > 24);
        assert!(es[7].data.len() > 24);
        assert!(es[8].data.starts_with(b"AEROSLS-BOOT-LAYOUT 1\n"));
        // The archive offsets recorded in entry_offsets match the parser's.
        let mut off = 0usize;
        for (i, e) in es.iter().enumerate() {
            assert_eq!(b.entry_offsets[i].1, off, "entry {} offset", i);
            assert_eq!(b.entry_offsets[i].2, e.data.len(), "entry {} size", i);
            off = off + 110 + align4(e.name.len() + 1) + align4(e.data.len());
        }
    }

    #[test]
    fn init_manifest_matches_kernel_parser_rules() {
        let b = built();
        let blob = b.archive_entry(ENTRY_PATHS[1]);
        let (name, image, budget, kaddr) = kernel_walk(&blob);
        assert_eq!(read_name(name.unwrap()), INIT_MANIFEST_NAME);
        // IMAGE: 24 bytes — entry u64, blob_offset u32, image_size u32,
        // image_kaddr u64 (in-record, 0 — the footer supplies it).
        let p = image.unwrap();
        assert_eq!(p.len(), 24);
        assert_eq!(le_u64(p, 0), spec().init_entry, "entry_offset");
        assert_eq!(le_u32(p, 8), b.entry_offsets[0].1 as u32, "blob_offset = init.bin's archive offset");
        assert_eq!(le_u32(p, 12), spec().init_bin.len() as u32, "image_size");
        assert_eq!(le_u64(p, 16), 0, "in-record image_kaddr is 0");
        // The footer: init's image at its declared physical address.
        assert_eq!(kaddr, b.layout.init_image.phys, "footer image_kaddr");
        assert!(kaddr >= 0x100000, "kernel's image_kaddr floor");
        // BUDGET: mem u64, stack u32, heap u32 — stack ≥ 4096 (kernel check).
        let bd = budget.unwrap();
        assert!(bd.len() >= 16);
        assert_eq!(le_u64(bd, 0), spec().init_heap_bytes);
        assert!(le_u32(bd, 8) >= 4096, "stack_bytes floor");
        // And the whole blob re-parses with the proto parser.
        let blob = b.archive_entry(ENTRY_PATHS[1]);
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.name.unwrap(), INIT_MANIFEST_NAME);
    }

    #[test]
    fn dm_manifest_matches_kernel_parser_rules() {
        let b = built();
        let blob = b.archive_entry(ENTRY_PATHS[3]);
        let (name, image, budget, kaddr) = kernel_walk(&blob);
        assert_eq!(read_name(name.unwrap()), DM_MANIFEST_NAME);
        let p = image.unwrap();
        assert_eq!(p.len(), 24);
        assert_eq!(le_u64(p, 0), spec().dm_entry, "entry_offset");
        assert_eq!(le_u32(p, 8), b.entry_offsets[2].1 as u32, "blob_offset = dm.bin's archive offset");
        assert_eq!(le_u32(p, 12), spec().dm_bin.len() as u32, "image_size");
        assert_eq!(kaddr, b.layout.dm_image.phys, "footer image_kaddr");
        let bd = budget.unwrap();
        assert!(bd.len() >= 16);
        assert_eq!(le_u64(bd, 0), spec().dm_heap_bytes);
        let blob = b.archive_entry(ENTRY_PATHS[3]);
        let m = parse_manifest(&blob).unwrap();
        assert_eq!(m.name.unwrap(), DM_MANIFEST_NAME);
    }

    #[test]
    fn dm_image_cap_agrees_with_dm_manifest_footer() {
        // The cross-manifest consistency the whole layout rests on: init's
        // `dm.image` MEM cap (what entry.rs hands `create_sidecar`) points
        // at the SAME address the DM's own manifest declares as its image.
        let b = built();
        let blob = b.archive_entry(ENTRY_PATHS[1]);
        let m = parse_manifest(&blob).unwrap();
        let cap = m.find_cap("dm.image").expect("dm.image cap");
        let (base, size) = match cap.kind {
            CapKind::Mem { base, size } => (base, size),
            _ => panic!("dm.image must be a MEM cap"),
        };
        assert_eq!(base, b.layout.dm_image.phys);
        assert_eq!(size, spec().dm_bin.len() as u64);
        let dm_blob = b.archive_entry(ENTRY_PATHS[3]);
        let (_, _, _, dm_kaddr) = kernel_walk(&dm_blob);
        assert_eq!(base, dm_kaddr, "init's dm.image cap == dm manifest footer");
    }

    #[test]
    fn budget_caps_match_layout_regions() {
        let b = built();
        let blob = b.archive_entry(ENTRY_PATHS[1]);
        let m = parse_manifest(&blob).unwrap();
        let (base, size) = match m.find_cap("budget").unwrap().kind {
            CapKind::Mem { base, size } => (base, size),
            _ => panic!(),
        };
        assert_eq!((base, size), (b.layout.init_heap.phys, b.layout.init_heap.size));

        let dm_blob = b.archive_entry(ENTRY_PATHS[3]);
        let dm = parse_manifest(&dm_blob).unwrap();
        let (base, size) = match dm.find_cap("budget").unwrap().kind {
            CapKind::Mem { base, size } => (base, size),
            _ => panic!(),
        };
        assert_eq!((base, size), (b.layout.dm_heap.phys, b.layout.dm_heap.size));
    }

    #[test]
    fn device_registry_cap_points_at_registry_region() {
        let b = built();
        let blob = b.archive_entry(ENTRY_PATHS[1]);
        let m = parse_manifest(&blob).unwrap();
        let (base, size) = match m.find_cap("device_registry").unwrap().kind {
            CapKind::Mem { base, size } => (base, size),
            _ => panic!(),
        };
        assert_eq!((base, size), (b.layout.registry.phys, b.layout.registry.size));
        // The registry region is big enough for MAX_DEVICES(16) × 64-byte
        // entries + the count (devreg.rs): 4 + 16 × 64 = 1028 bytes.
        assert!(b.layout.registry.size >= 4 + 16 * 64);
    }

    #[test]
    fn console_channel_peer_is_the_kernel_service() {
        let b = built();
        let blob = b.archive_entry(ENTRY_PATHS[1]);
        let m = parse_manifest(&blob).unwrap();
        let cap = m.find_cap("console").unwrap();
        assert_eq!(cap.name, "console");
        match cap.kind {
            CapKind::Chan { peer, .. } => assert_eq!(peer, Some(CONSOLE_PEER)),
            _ => panic!("console must be a CHAN cap"),
        }
    }

    #[test]
    fn layout_file_records_regions_and_files() {
        let b = built();
        let text = String::from_utf8(b.archive_entry(ENTRY_PATHS[8])).unwrap();
        let mut lines = text.lines();
        assert_eq!(lines.next(), Some("AEROSLS-BOOT-LAYOUT 1"));
        assert_eq!(lines.next().unwrap(), format!("base {:016x}", b.layout.base_phys));
        for (name, r) in b.layout.regions() {
            let want = format!("region {name} {:016x} {:016x}", r.phys, r.size);
            assert_eq!(lines.next().unwrap(), want);
        }
        for (path, off, size) in &b.entry_offsets {
            let want = format!("file {path} {off:016x} {size:016x}");
            assert_eq!(lines.next().unwrap(), want);
        }
        assert!(lines.next().is_none(), "no trailing lines");
    }

    #[test]
    fn archives_are_deterministic() {
        assert_eq!(built().archive, build_boot_image(&spec()).archive);
    }

    #[test]
    fn every_image_sits_at_its_declared_physical_address() {
        // The core contract, stated end to end: after the loader places the
        // initrd and copies each image to its declared address, the bytes
        // at `image_kaddr` are exactly the archive's image bytes.
        let b = built();
        assert_eq!(b.layout.init_image.phys, b.layout.base_phys);
        assert_eq!(b.layout.init_image.size, spec().init_bin.len() as u64);
        let init_blob = b.archive_entry(ENTRY_PATHS[1]);
        let dm_blob = b.archive_entry(ENTRY_PATHS[3]);
        let (_, _, _, init_kaddr) = kernel_walk(&init_blob);
        let (_, _, _, dm_kaddr) = kernel_walk(&dm_blob);
        assert_eq!(init_kaddr, b.layout.init_image.phys);
        assert_eq!(dm_kaddr, b.layout.dm_image.phys);
        // Both images live inside the reserved boot region, page-aligned,
        // and the whole region stays below the 4 GiB identity map.
        assert!(init_kaddr >= b.layout.base_phys);
        assert!(dm_kaddr >= b.layout.base_phys);
        assert!(b.layout.registry.end() <= 0x1_0000_0000);
    }
}
