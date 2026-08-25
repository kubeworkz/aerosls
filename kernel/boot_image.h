/* boot_image.h — Phase 5 self-hosted boot: the initrd boot-image contract.
 *
 * The boot image is BUILT by the host tool `aerosls-bootimage`
 * (user/bootimage/), which packages the init and Device Manager sidecar
 * binaries into a `newc` CPIO initrd (`sidecars.cpio`) with each image at
 * its manifest-declared physical address. The constants below are the
 * kernel side of that contract — the mirror of user/bootimage/src/layout.rs
 * (keep the two in sync).
 *
 * ─── Loading (firmware → kernel) ─────────────────────────────────────────
 * GRUB loads the archive as a Multiboot2 module (`module /boot/sidecars.cpio`),
 * U-Boot/OpenSBI via `-initrd`/`bootm` (or the device tree `/chosen/module`).
 * The kernel finds it through its boot path (multiboot2 module tag on x86;
 * the device tree on RISC-V) — wherever the bootloader placed it. The
 * archive is ordinary data; the loader copies images out of it.
 *
 * ─── Archive layout (newc, in order) ────────────────────────────────────
 *   boot/init.bin       the init sidecar flat binary
 *   boot/init.manifest  init's packed manifest (records + image_kaddr footer)
 *   boot/dm.bin         the Device Manager flat binary
 *   boot/dm.manifest    the DM's packed manifest (records + image_kaddr footer)
 *   boot/layout         ASCII region/file table (see BOOT_LAYOUT_* below)
 *   TRAILER!!!          newc terminator
 *
 * The images are NOT at their declared addresses inside the archive: each
 * manifest's footer `image_kaddr` is the physical address the LOADER must
 * copy the image to. The registry and heap regions are implicit — the
 * loader reserves and zeroes them.
 *
 * ─── Memory layout (boot-image region, page-aligned, contiguous) ────────
 *   base_phys                    (BOOT_IMAGE_BASE_PHYS, 0x20000000 = 512 MiB)
 *     init.image                 size = the init binary's size
 *     init.heap                  BOOT_INIT_HEAP_BYTES   (init's budget MEM cap)
 *     dm.image                   size = the DM binary's size
 *     dm.heap                    BOOT_DM_HEAP_BYTES     (the DM's budget cap)
 *     registry                   BOOT_REGISTRY_BYTES    (SidecarDeviceInfo)
 *   The whole span [base_phys, base_phys + total_size) must be reserved
 *   from the frame pool before any sidecar is created (the arena carve in
 *   cap_init runs after the loader, so the reservation must win).
 *
 * The init sidecar then spawns the DM itself at runtime via its `dm.image`
 * cap (SYS_SLS_CREATE_SIDECAR), re-packing the same manifest records the
 * archive's boot/dm.manifest carries — the addresses agree because this
 * builder pinned them to the same layout. The consumer is implemented in
 * kernel/boot_image.c (see the "The loader" section below).
 */

#ifndef BOOT_IMAGE_H
#define BOOT_IMAGE_H

#include <stdint.h>

/* ─── The boot-image region ─────────────────────────────────────────────── */
/* Physical base (512 MiB): above the kernel image, below the 4 GiB identity
 * map, page-aligned, disjoint from the arena. Mirrors
 * BOOT_IMAGE_BASE_PHYS in user/bootimage/src/layout.rs. */
#define BOOT_IMAGE_BASE_PHYS        0x20000000ULL

/* Init's budget heap (design doc §1.4 mem_bytes = 16 MiB). */
#define BOOT_INIT_HEAP_BYTES        (16u * 1024u * 1024u)
/* The DM's budget heap — matches dm_budget_base's 256 KiB in
 * user/init/src/dm_manifest.rs. */
#define BOOT_DM_HEAP_BYTES          (256u * 1024u)
/* The POSIX sidecar's budget heap — 4 MiB for VFS caches, process
 * table, and applet scripts. */
#define BOOT_POSIX_HEAP_BYTES       (4u * 1024u * 1024u)
/* The device registry region: 4 + MAX_DEVICES(16) × 64 = 1028 bytes
 * (devreg.rs), rounded to one page. */
#define BOOT_REGISTRY_BYTES         4096u
/* Registry entry cap (devreg.rs): MAX_DEVICES. */
#define BOOT_REGISTRY_MAX_DEVICES   16u

/* ─── Archive entry paths (newc names) ──────────────────────────────────── */
#define BOOT_INIT_BIN_PATH          "boot/init.bin"
#define BOOT_INIT_MANIFEST_PATH     "boot/init.manifest"
#define BOOT_DM_BIN_PATH            "boot/dm.bin"
#define BOOT_DM_MANIFEST_PATH       "boot/dm.manifest"
#define BOOT_POSIX_BIN_PATH         "boot/posix.bin"
#define BOOT_POSIX_MANIFEST_PATH    "boot/posix.manifest"
#define BOOT_LAYOUT_PATH            "boot/layout"

/* ─── boot/layout text format ───────────────────────────────────────────── */
/* Fixed-width ASCII, one record per line (the producer in
 * user/bootimage/src/builder.rs; every numeric field is 16 lowercase hex
 * digits):
 *
 *   AEROSLS-BOOT-LAYOUT 1
 *   base <phys>
 *   region <name> <phys> <size>         7 lines, memory order:
 *                                       init.image init.heap dm.image
 *                                       dm.heap posix.image posix.heap
 *                                       registry
 *   file <path> <offset> <size>         one line per archive entry
 *                                       (init.bin init.manifest dm.bin
 *                                       dm.manifest posix.bin
 *                                       posix.manifest boot/layout)
 *
 * The loader may walk the CPIO by name instead of parsing this file; the
 * file exists so the loader can verify its region computation and so a
 * human can read the image addresses without disassembling the archive. */
#define BOOT_LAYOUT_MAGIC_LINE        "AEROSLS-BOOT-LAYOUT 1"

/* ─── newc header (110 bytes) — reference for the loader's CPIO walk ────── */
/* Offset 0: magic "070701" (6 bytes). Fields are 8-hex-char ASCII:
 * c_ino(6) c_mode(14) c_uid(22) c_gid(30) c_nlink(38) c_mtime(46)
 * c_filesize(54) c_devmajor(62) c_devminor(70) c_rdevmajor(78)
 * c_rdevminor(86) c_namesize(94, INCLUDES the trailing NUL) c_check(102, =0).
 * Then the NUL-terminated name padded to a 4-byte boundary, then the file
 * data padded to a 4-byte boundary. The archive ends with a "TRAILER!!!"
 * entry (filesize 0). */

/* ─── The loader (kernel/boot_image.c) ───────────────────────────────────────
 * boot_image_capture_mb2() runs early (right after boot_params_scan_mb2) to
 * remember the first Multiboot2 module (the initrd). launch_init_sidecar()
 * runs as kernel_main step 7d, before the HTTP server / shell:
 *
 *   1. boot_image_parse() walks the newc archive, extracts boot/init.manifest
 *      into a kernel buffer, and reads the init image kaddr (manifest
 *      footer), the dm.image + device_registry MEM caps, and the
 *      init.bin/dm.bin archive offsets.
 *   2. frame_pool_reserve_range(boot_base, boot_base + boot_total) — the
 *      whole boot-image span, before any sidecar can allocate from it.
 *   3. Copies init.bin -> init.image's kaddr and dm.bin -> dm.image's kaddr
 *      (cap_create_sidecar later copies from there into fresh frames).
 *   4. boot_build_registry() writes the SidecarDeviceInfo table (devreg.rs
 *      format) into the registry region.
 *   5. Plants a synthetic kernel-bootstrap parent descriptor (the kernel
 *      context has none) and calls cap_create_sidecar with the init
 *      manifest; the child is released PROC_SUSPENDED and runs on the next
 *      scheduler tick. */

/* One PCI device as the init sidecar's registry sees it — the 64-byte
 * packed layout of user/init/src/devreg.rs DeviceEntry:
 * class(0) subclass(1) vendor u16(2) device u16(4) slot(6) bus(7)
 * bar0 u64(8) irq(16) is64(17) reserved u16(18) driver_manifest[44](20).
 * The registry region itself is `u32 count` followed by count × 64 bytes. */
#define BOOT_DEVICE_ENTRY_SIZE   64u
#define BOOT_DRIVER_MANIFEST_LEN 44u
struct BootDeviceEntry {
    uint8_t  class_code;
    uint8_t  subclass;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  pci_slot;
    uint8_t  pci_bus;
    uint64_t bar0_phys;
    uint8_t  irq_line;
    uint8_t  is_64bit_bar;
    uint8_t  reserved[2];
    char     driver_manifest[BOOT_DRIVER_MANIFEST_LEN];
} __attribute__((packed));

/* boot_image_parse() result. Everything is derived from the INIT manifest
 * (the single source of truth the builder produced): the init image address
 * is the manifest footer's image_kaddr; the DM image and registry addresses
 * are the dm.image / device_registry MEM caps; boot_base/boot_total is the
 * contiguous span the loader must reserve. */
struct BootImageInfo {
    uint64_t init_kaddr, init_size;
    uint64_t dm_kaddr,   dm_size;
    uint64_t reg_kaddr,  reg_size;
    uint64_t boot_base,  boot_total;
    uint32_t init_bin_off, init_bin_len;   /* archive offsets of the images */
    uint32_t dm_bin_off,   dm_bin_len;
};

/* boot_image.c error codes (returned by the pure core; the glue logs and
 * skips the launch on any of them). */
#define BOOT_ERR_BADARCHIVE (-1)   /* not a newc archive */
#define BOOT_ERR_TRUNCATED  (-2)   /* archive/entry truncated */
#define BOOT_ERR_NOTFOUND   (-3)   /* entry not in the archive */
#define BOOT_ERR_NOMANIFEST (-4)   /* boot/init.manifest missing */
#define BOOT_ERR_NOIMAGE    (-5)   /* an image entry missing */
#define BOOT_ERR_BADMANIFEST (-6)  /* init manifest fails the header/CRC walk */
#define BOOT_ERR_MISMATCH   (-7)   /* manifest sizes disagree with the archive */
#define BOOT_ERR_RANGE      (-8)   /* addresses outside the 4 GiB identity map */

/* ─── Pure core (host-testable; no kernel deps) ───────────────────────────── */

/* Walk the newc archive for the entry `name`; on success store its data
 * offset/size and return 0. Negative BOOT_ERR_* otherwise. */
int boot_newc_find(const uint8_t* archive, uint32_t len, const char* name,
                   uint32_t* out_off, uint32_t* out_size);

/* Parse the initrd: locate + copy out boot/init.manifest, read the image /
 * cap / footer fields, cross-check the archive's image entries against the
 * manifest's declared sizes, and fill *info. Negative BOOT_ERR_* on any
 * inconsistency (the launch is refused, never partially applied). */
int boot_image_parse(const uint8_t* archive, uint32_t archive_len,
                     uint8_t* manifest_out, uint32_t manifest_cap,
                     uint32_t* manifest_len_out, struct BootImageInfo* info);

/* Build the device registry at dst: `u32 count` then count × 64-byte
 * entries, from a caller-supplied per-slot scan (returns 0 and fills *e for
 * a present device, non-zero for an empty slot). Returns the count, or -1
 * on a null dst/cap < 4. Matches the devreg.rs wire format exactly. */
int boot_build_registry(uint8_t* dst, uint32_t cap,
                        int (*scan)(int slot, struct BootDeviceEntry* e));

/* The driver-manifest name for a (class, subclass) pair — the design doc's
 * compile-time vendor/device table, kept class-based for now. */
const char* boot_driver_for_class(uint8_t class_code, uint8_t subclass);

/* ─── Boot glue (kernel-only) ──────────────────────────────────────────────── */

/* Remember the first Multiboot2 module (the initrd). Call early, right
 * after boot_params_scan_mb2(), while the mb2 info block is fresh. */
void boot_image_capture_mb2(uint32_t mb2_magic, uint32_t mb2_phys);

/* kernel_main step 7d: reserve the boot-image span, copy the images to
 * their declared addresses, build the device registry, and create the init
 * sidecar. Non-fatal: with no initrd the kernel boots as today. */
void launch_init_sidecar(void);

#endif /* BOOT_IMAGE_H */
