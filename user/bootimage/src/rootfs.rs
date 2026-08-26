//! Build a minimal aerofs rootfs image for the Phase 5 ramdisk.
//!
//! The image is embedded in the CPIO and copied to the ramdisk's storage
//! region by the kernel's `launch_init_sidecar`.  The POSIX sidecar reads
//! block 0 (superblock) to mount `/`.

use aerosls_vfs::aerofs::ImageBuilder;

/// Build a minimal aerofs rootfs with the files the POSIX sidecar needs
/// to boot to a shell.
///
/// Minimum contents:
/// - `/etc/init.rc` — boot script (run by the `init` applet)
/// - `/bin/` directory (for future applets)
/// - `/tmp` directory (mounted as ramfs by the sidecar)
pub fn build_rootfs() -> Vec<u8> {
    let mut b = ImageBuilder::new();

    // Root directory structure
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_dir("/tmp", 0o755);
    b.add_dir("/dev", 0o755);

    // Boot script — the `init` applet reads this and forks one child per line.
    b.add_file(
        "/etc/init.rc",
        b"echo AeroSLS Phase 5 booting\n\
          echo Root filesystem mounted\n\
          echo System ready\n",
        0o644,
    );

    b.build()
}
