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
///
/// `quiet` selects the gate image's script: the demo applets' IRQ storm
/// owns the serial port and starves the E7 gate tenant's own evidence, so
/// the gate build drops them. The default image keeps the full script —
/// the flag comes from the build line, so `make x86-iso` is unchanged.
pub fn build_rootfs(quiet: bool) -> Vec<u8> {
    let mut b = ImageBuilder::new();

    // Root directory structure
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_dir("/tmp", 0o755);
    b.add_dir("/dev", 0o755);

    // Boot script — the `init` applet reads this and forks one child per line.
    // The gate profile's line set stops at `caps`: no netcheck/nettest/irqtest/
    // devtest, whose IRQ storm and respawn churn the serial port while the
    // gate's own workload is running.
    const FULL_RC: &[u8] = b"echo AeroSLS Phase 5 booting\n\
          echo Root filesystem mounted\n\
          ls /\n\
          echo Testing network...\n\
          netcheck\n\
          nettest 8 7\n\
          irqtest\n\
          devtest\n\
          caps\n\
          echo System ready\n\
          echo\n\
          sh\n";
    const GATE_RC: &[u8] = b"echo AeroSLS Phase 5 booting\n\
          echo Root filesystem mounted\n\
          ls /\n\
          caps\n\
          echo System ready\n\
          echo\n\
          sh\n";
    b.add_file(
        "/etc/init.rc",
        if quiet { GATE_RC } else { FULL_RC },
        0o644,
    );

    b.build()
}
