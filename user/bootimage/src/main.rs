//! CLI for the Phase 5 boot-image builder.
//!
//! ```text
//! aerosls-bootimage --init <init.bin> --dm <dm.bin> --posix <posix.bin> -o sidecars.cpio
//! aerosls-bootimage flatten --input <init.elf> --output <init.bin>
//! ```
//!
//! The first form reads the three flat sidecar binaries, computes the
//! physical layout, packs all manifests, and writes the `newc` initrd
//! archive. The layout (every image's declared physical address) is
//! printed to stderr and embedded in the archive as `boot/layout`.
//!
//! `flatten` is the mini-objcopy: it converts the ELF that
//! `cargo build --target x86_64-unknown-none --bin init` produces into the
//! flat binary the kernel maps (`flatten.rs` — PT_LOAD segments with
//! zero-filled gaps/BSS, entry at offset 0). `make selfhost-bootimage`
//! uses it to turn the cross-built ELF into `init.bin`.

use aerosls_bootimage::{BootImageSpec, build_boot_image, flatten_elf};
use std::path::PathBuf;

fn usage() -> ! {
    eprintln!(
        "usage: aerosls-bootimage --init <init.bin> --dm <dm.bin> --posix <posix.bin> -o sidecars.cpio \\\n         [--init-entry <off>] [--dm-entry <off>] [--posix-entry <off>] [--base-phys <addr>]\n\n\
         aerosls-bootimage flatten --input <init.elf> --output <init.bin> \n         [--load-vaddr <hex>]  (default 0x400000000000 = USER_PROC_CODE_BASE)"
    );
    std::process::exit(2);
}

fn main() {
    let mut args = std::env::args().skip(1);
    if let Some(first) = args.next() {
        if first == "flatten" {
            return cmd_flatten(args);
        }
        // Re-queue for the shared parser below.
        let all = std::iter::once(first).chain(args);
        return cmd_build(all);
    }
    usage();
}

fn cmd_flatten(mut args: impl Iterator<Item = String>) {
    let mut input = None;
    let mut output = None;
    // The kernel maps sidecar images at USER_PROC_CODE_BASE (kernel/cap.c
    // cap_create_sidecar step 5) and the ELF is ET_DYN (static PIE), so
    // every relocation carries that runtime load bias. `flatten` must add
    // it or the image's GOT points into the low address space.
    let mut load_vaddr = 0x4000_0000_0000u64;
    while let Some(a) = args.next() {
        let mut next = || args.next().unwrap_or_else(|| {
            eprintln!("missing value for {a}");
            usage();
        });
        match a.as_str() {
            "--input" => input = Some(PathBuf::from(next())),
            "--output" | "-o" => output = Some(PathBuf::from(next())),
            "--load-vaddr" => {
                load_vaddr = u64::from_str_radix(
                    next().trim_start_matches("0x"),
                    16,
                )
                .unwrap_or_else(|_| {
                    eprintln!("bad --load-vaddr");
                    usage();
                })
            }
            "-h" | "--help" => usage(),
            other => {
                eprintln!("unknown argument: {other}");
                usage();
            }
        }
    }
    let (input, output) = match (input, output) {
        (Some(i), Some(o)) => (i, o),
        _ => usage(),
    };
    let elf = std::fs::read(&input).unwrap_or_else(|e| panic!("read {}: {e}", input.display()));
    let flat = flatten_elf(&elf, load_vaddr)
        .unwrap_or_else(|e| panic!("flatten {}: {e}", input.display()));
    std::fs::write(&output, &flat.image)
        .unwrap_or_else(|e| panic!("write {}: {e}", output.display()));
    eprintln!(
        "[BOOTIMAGE] flattened {} -> {} ({} bytes, entry offset 0x{:x})",
        input.display(),
        output.display(),
        flat.image.len(),
        flat.entry,
    );
}

fn cmd_build(mut args: impl Iterator<Item = String>) {
    let mut init = None;
    let mut dm = None;
    let mut posix = None;
    let mut ramdisk = None;
    let mut net = None;
    let mut out = None;
    let mut init_entry = 0u64;
    let mut dm_entry = 0u64;
    let mut posix_entry = 0u64;
    let mut ramdisk_entry = 0u64;
    let mut net_entry = 0u64;
    let mut base_phys = None;

    while let Some(a) = args.next() {
        let mut next = || args.next().unwrap_or_else(|| {
            eprintln!("missing value for {a}");
            usage();
        });
        match a.as_str() {
            "--init" => init = Some(PathBuf::from(next())),
            "--dm" => dm = Some(PathBuf::from(next())),
            "--posix" => posix = Some(PathBuf::from(next())),
            "--ramdisk" => ramdisk = Some(PathBuf::from(next())),
            "--net" => net = Some(PathBuf::from(next())),
            "-o" | "--output" => out = Some(PathBuf::from(next())),
            "--init-entry" => init_entry = parse_hex(&next(), &a),
            "--dm-entry" => dm_entry = parse_hex(&next(), &a),
            "--posix-entry" => posix_entry = parse_hex(&next(), &a),
            "--ramdisk-entry" => ramdisk_entry = parse_hex(&next(), &a),
            "--net-entry" => net_entry = parse_hex(&next(), &a),
            "--base-phys" => base_phys = Some(parse_hex(&next(), &a)),
            "-h" | "--help" => usage(),
            other => {
                eprintln!("unknown argument: {other}");
                usage();
            }
        }
    }

    let (init_path, dm_path, posix_path, ramdisk_path, net_path, out_path) = match (init, dm, posix, ramdisk, net, out) {
        (Some(i), Some(d), Some(p), Some(r), Some(n), Some(o)) => (i, d, p, r, n, o),
        _ => usage(),
    };

    let init_bin = std::fs::read(&init_path)
        .unwrap_or_else(|e| panic!("read {}: {e}", init_path.display()));
    let dm_bin = std::fs::read(&dm_path)
        .unwrap_or_else(|e| panic!("read {}: {e}", dm_path.display()));
    let posix_bin = std::fs::read(&posix_path)
        .unwrap_or_else(|e| panic!("read {}: {e}", posix_path.display()));
    let ramdisk_bin = std::fs::read(&ramdisk_path)
        .unwrap_or_else(|e| panic!("read {}: {e}", ramdisk_path.display()));
    let net_bin = std::fs::read(&net_path)
        .unwrap_or_else(|e| panic!("read {}: {e}", net_path.display()));

    let mut spec = BootImageSpec::new(init_bin, dm_bin, posix_bin, ramdisk_bin, net_bin);
    spec.init_entry = init_entry;
    spec.dm_entry = dm_entry;
    spec.posix_entry = posix_entry;
    spec.ramdisk_entry = ramdisk_entry;
    spec.net_entry = net_entry;
    if let Some(b) = base_phys {
        spec.base_phys = b;
    }

    let image = build_boot_image(&spec);
    std::fs::write(&out_path, &image.archive)
        .unwrap_or_else(|e| panic!("write {}: {e}", out_path.display()));

    eprintln!("[BOOTIMAGE] {} ({} bytes, newc initrd)", out_path.display(), image.archive.len());
    eprintln!("[BOOTIMAGE] base phys 0x{:x}", image.layout.base_phys);
    for (name, r) in image.layout.regions() {
        eprintln!("[BOOTIMAGE]   region {name:<11} 0x{:016x} + 0x{:x}", r.phys, r.size);
    }
    eprintln!(
        "[BOOTIMAGE] reserved span: 0x{:x}..0x{:x} ({} KiB)",
        image.layout.base_phys,
        image.layout.base_phys + image.layout.total_size(),
        image.layout.total_size() >> 10
    );
    eprintln!("[BOOTIMAGE]   init manifest -> boot/init.manifest (image @ 0x{:x})", image.layout.init_image.phys);
    eprintln!("[BOOTIMAGE]   dm   manifest -> boot/dm.manifest   (image @ 0x{:x})", image.layout.dm_image.phys);
    eprintln!("[BOOTIMAGE]   posix manifest -> boot/posix.manifest (image @ 0x{:x})", image.layout.posix_image.phys);
}

fn parse_hex(s: &str, arg: &str) -> u64 {
    u64::from_str_radix(s.trim_start_matches("0x"), 16)
        .unwrap_or_else(|_| {
            eprintln!("{arg}: not a hex number: {s}");
            usage();
        })
}
