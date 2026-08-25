//! The Phase 5 boot image layout — the contract between the boot-image
//! builder (this crate), the bootloader, and the kernel's boot-time
//! sidecar loader (`kernel/boot_image.h` mirrors these constants).
//!
//! # Model
//!
//! The kernel's `cap_create_sidecar` copies each sidecar's image from a
//! physical address (`image_kaddr`, the manifest blob's 8-byte footer) into
//! freshly allocated frames, and mints the manifest's MEM caps against
//! their declared physical bases as-is (`cap_create_mem`: page-aligned,
//! inside the 4 GiB identity map, disjoint from the kernel image, the
//! arena, and every other MEM object). So the boot image must:
//!
//!   1. assign every sidecar a **physical address** the builder can bake
//!      into the manifest footer (`image_kaddr`) and the parent's MEM caps,
//!   2. reserve those regions from the frame pool before any sidecar is
//!      created (the loader's job — `kernel/boot_image.h` documents the
//!      reservation span `[base_phys, base_phys + total_size)`),
//!   3. lay the regions out page-aligned and non-overlapping, below 4 GiB.
//!
//! The archive (a plain newc CPIO) carries the *bytes*; the loader copies
//! each image from the archive to its declared address. The heap and
//! registry regions are implicit — the loader reserves and zeroes them
//! (the archive carries no 16 MiB of zeros). `boot/layout` inside the
//! archive records every region and file so the loader needs no knowledge
//! beyond this module's mirror in `kernel/boot_image.h`.
//!
//! # Regions (in order, each page-aligned, contiguous)
//!
//! ```text
//! base_phys (0x2000_0000 = 512 MiB — above the kernel image, below 4 GiB)
//!   ├─ init.image  the init sidecar flat binary        (size = init_bin.len)
//!   ├─ init.heap   init's budget MEM cap (bump heap)   (16 MiB, §1.4)
//!   ├─ dm.image    the Device Manager flat binary      (size = dm_bin.len)
//!   ├─ dm.heap     the DM's budget MEM cap             (256 KiB, dm_manifest.rs)
//!   └─ registry    kernel-populated SidecarDeviceInfo  (4 KiB, devreg.rs)
//! ```

/// Physical base of the reserved boot-image region (512 MiB). Must satisfy
/// `cap_create_mem`: ≥ 0x100000, disjoint from the kernel image
/// `[0x100000, _kernel_image_end)` and from the arena. Mirrored in
/// `kernel/boot_image.h` (`BOOT_IMAGE_BASE_PHYS`).
pub const BOOT_IMAGE_BASE_PHYS: u64 = 0x2000_0000;

/// Init's budget heap (design doc §1.4 `mem_bytes` = 16 MiB).
pub const INIT_HEAP_BYTES: u64 = 16 * 1024 * 1024;

/// The Device Manager's budget heap — must match `dm_budget_base`'s 256 KiB
/// in `user/init/src/dm_manifest.rs`.
pub const DM_HEAP_BYTES: u64 = 256 * 1024;

/// The POSIX sidecar's budget heap — 4 MiB, enough for the VFS caches,
/// process table, and applet scripts.
pub const POSIX_HEAP_BYTES: u64 = 4 * 1024 * 1024;

/// The ramdisk driver's budget heap — 256 KiB, same as the DM.
pub const RAMDISK_HEAP_BYTES: u64 = 256 * 1024;

/// The ramdisk storage region — 32 MiB of block data that the POSIX sidecar
/// reads via the block cache. In a real deployment this would contain an
/// aerofs-lite or ext2 rootfs image; for the Phase 5 demo it is zero-filled
/// (the POSIX shell boots in console-only mode until a real image is placed
/// here).
pub const STORAGE_BYTES: u64 = 16 * 1024 * 1024;

/// The device registry region: `4 + MAX_DEVICES(16) × 64` = 1028 bytes
/// (devreg.rs), rounded to one page.
pub const REGISTRY_BYTES: u64 = 4096;

/// Init's registry name (the manifest's TAG_NAME; the kernel registers it
/// in the sidecar registry). Mirrors the design doc's `aerosls.init.v1`
/// personality; the instance name follows the `drv.*`/`aerosls.*` pattern.
pub const INIT_MANIFEST_NAME: &str = "aerosls.init.0";
/// The Device Manager's registry name — must match
/// `dm_manifest::DM_MANIFEST_NAME` in `user/init/src/dm_manifest.rs`.
pub const DM_MANIFEST_NAME: &str = "drv.device_manager.0";
/// The POSIX sidecar's registry name.
pub const POSIX_MANIFEST_NAME: &str = "aerosls.posix.0";
/// The ramdisk driver's registry name.
pub const RAMDISK_MANIFEST_NAME: &str = "drv.ramdisk.0";

/// Archive entry paths (the loader walks the CPIO for `INIT_MANIFEST_PATH`).
pub const INIT_BIN_PATH: &str = "boot/init.bin";
pub const INIT_MANIFEST_PATH: &str = "boot/init.manifest";
pub const DM_BIN_PATH: &str = "boot/dm.bin";
pub const DM_MANIFEST_PATH: &str = "boot/dm.manifest";
pub const POSIX_BIN_PATH: &str = "boot/posix.bin";
pub const POSIX_MANIFEST_PATH: &str = "boot/posix.manifest";
pub const RAMDISK_BIN_PATH: &str = "boot/ramdisk.bin";
pub const RAMDISK_MANIFEST_PATH: &str = "boot/ramdisk.manifest";
pub const LAYOUT_PATH: &str = "boot/layout";

/// One reserved physical region.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
    /// Physical base (page-aligned).
    pub phys: u64,
    /// Size in bytes.
    pub size: u64,
}

impl Region {
    pub const fn end(&self) -> u64 {
        self.phys + self.size
    }
}

/// The computed layout for one boot image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootLayout {
    pub base_phys: u64,
    pub init_image: Region,
    pub init_heap: Region,
    pub dm_image: Region,
    pub dm_heap: Region,
    pub posix_image: Region,
    pub posix_heap: Region,
    pub ramdisk_image: Region,
    pub ramdisk_heap: Region,
    pub storage: Region,
    pub registry: Region,
}

impl BootLayout {
    /// The whole reserved span `[base_phys, base_phys + total_size)` — the
    /// loader reserves exactly this from the frame pool.
    pub fn total_size(&self) -> u64 {
        self.registry.end() - self.base_phys
    }

    /// Every region, in memory order, for iteration in tests and the
    /// `boot/layout` file.
    pub fn regions(&self) -> [(&'static str, Region); 10] {
        [
            ("init.image", self.init_image),
            ("init.heap", self.init_heap),
            ("dm.image", self.dm_image),
            ("dm.heap", self.dm_heap),
            ("posix.image", self.posix_image),
            ("posix.heap", self.posix_heap),
            ("ramdisk.image", self.ramdisk_image),
            ("ramdisk.heap", self.ramdisk_heap),
            ("storage", self.storage),
            ("registry", self.registry),
        ]
    }
}

/// The inputs to one boot-image build.
#[derive(Clone, Debug)]
pub struct BootImageSpec {
    /// The init sidecar flat binary (entry at `init_entry`).
    pub init_bin: Vec<u8>,
    /// The Device Manager flat binary (entry at `dm_entry`).
    pub dm_bin: Vec<u8>,
    /// The POSIX sidecar flat binary.
    pub posix_bin: Vec<u8>,
    /// The ramdisk driver flat binary.
    pub ramdisk_bin: Vec<u8>,
    /// Size of init's budget heap region.
    pub init_heap_bytes: u64,
    /// Size of the DM's budget heap region.
    pub dm_heap_bytes: u64,
    /// Size of the POSIX sidecar's budget heap region.
    pub posix_heap_bytes: u64,
    /// Size of the ramdisk driver's budget heap region.
    pub ramdisk_heap_bytes: u64,
    /// Size of the ramdisk storage region.
    pub storage_bytes: u64,
    /// Size of the device-registry region.
    pub registry_bytes: u64,
    /// Physical base of the whole boot-image region.
    pub base_phys: u64,
    /// Entry-point offset within the init binary.
    pub init_entry: u64,
    /// Entry-point offset within the DM binary.
    pub dm_entry: u64,
    /// Entry-point offset within the POSIX binary.
    pub posix_entry: u64,
    /// Entry-point offset within the ramdisk binary.
    pub ramdisk_entry: u64,
}

impl BootImageSpec {
    /// A spec with the documented defaults.
    pub fn new(init_bin: Vec<u8>, dm_bin: Vec<u8>, posix_bin: Vec<u8>, ramdisk_bin: Vec<u8>) -> Self {
        Self {
            init_bin,
            dm_bin,
            posix_bin,
            ramdisk_bin,
            init_heap_bytes: INIT_HEAP_BYTES,
            dm_heap_bytes: DM_HEAP_BYTES,
            posix_heap_bytes: POSIX_HEAP_BYTES,
            ramdisk_heap_bytes: RAMDISK_HEAP_BYTES,
            storage_bytes: STORAGE_BYTES,
            registry_bytes: REGISTRY_BYTES,
            base_phys: BOOT_IMAGE_BASE_PHYS,
            init_entry: 0,
            dm_entry: 0,
            posix_entry: 0,
            ramdisk_entry: 0,
        }
    }
}

fn align_up(v: u64, a: u64) -> u64 {
    debug_assert!(a.is_power_of_two());
    (v + a - 1) & !(a - 1)
}

/// Compute the layout from a spec: five contiguous page-aligned regions in
/// memory order. The result is deterministic for a given spec, so the
/// builder can bake the addresses into both manifests before writing the
/// archive.
pub fn compute_layout(spec: &BootImageSpec) -> BootLayout {
    let mut next = spec.base_phys;
    let mut take = |size: u64| {
        let r = Region {
            phys: next,
            size,
        };
        next = align_up(r.end(), 4096);
        r
    };
    BootLayout {
        base_phys: spec.base_phys,
        init_image: take(spec.init_bin.len() as u64),
        init_heap: take(spec.init_heap_bytes),
        dm_image: take(spec.dm_bin.len() as u64),
        dm_heap: take(spec.dm_heap_bytes),
        posix_image: take(spec.posix_bin.len() as u64),
        posix_heap: take(spec.posix_heap_bytes),
        ramdisk_image: take(spec.ramdisk_bin.len() as u64),
        ramdisk_heap: take(spec.ramdisk_heap_bytes),
        storage: take(spec.storage_bytes),
        registry: take(spec.registry_bytes),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec() -> BootImageSpec {
        BootImageSpec::new(vec![0xAA; 0x2000], vec![0xBB; 0x4000], vec![0xCC; 0x8000], vec![0xDD; 0x1000])
    }

    #[test]
    fn regions_are_aligned_contiguous_and_in_order() {
        let l = compute_layout(&spec());
        assert_eq!(l.base_phys, BOOT_IMAGE_BASE_PHYS);
        let mut prev_end = l.base_phys;
        for (_, r) in l.regions() {
            assert_eq!(r.phys % 4096, 0, "region page-aligned");
            assert_eq!(r.phys, prev_end, "regions are contiguous");
            assert!(r.size > 0);
            prev_end = r.end();
        }
        assert!(l.total_size() > 0);
    }

    #[test]
    fn regions_stay_below_the_4gib_identity_map() {
        let l = compute_layout(&spec());
        assert!(l.registry.end() <= 0x1_0000_0000, "within 4 GiB");
    }

    #[test]
    fn posix_image_sits_after_dm_heap() {
        let l = compute_layout(&spec());
        assert!(l.posix_image.phys >= l.dm_heap.end());
    }

    #[test]
    fn ramdisk_image_sits_after_posix_heap() {
        let l = compute_layout(&spec());
        assert!(l.ramdisk_image.phys >= l.posix_heap.end());
    }

    #[test]
    fn heap_regions_match_the_sidecar_contracts() {
        let l = compute_layout(&spec());
        assert_eq!(l.init_heap.size, INIT_HEAP_BYTES);
        assert_eq!(l.dm_heap.size, DM_HEAP_BYTES);
        assert_eq!(l.posix_heap.size, POSIX_HEAP_BYTES);
        assert_eq!(l.ramdisk_heap.size, RAMDISK_HEAP_BYTES);
        assert_eq!(l.storage.size, STORAGE_BYTES);
        assert_eq!(l.registry.size, REGISTRY_BYTES);
    }

    #[test]
    fn dm_heap_sits_immediately_after_dm_image_page_aligned() {
        // The same rule dm_manifest::dm_budget_base applies: the DM's heap
        // starts at page_align(image.kaddr + image.size).
        let l = compute_layout(&spec());
        let want = align_up(l.dm_image.end(), 4096);
        assert_eq!(l.dm_heap.phys, want);
    }
}
