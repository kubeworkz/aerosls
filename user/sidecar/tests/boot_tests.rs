//! End-to-end tests for the sidecar bootstrap, driven against the *real*
//! ramdisk driver on the fake kernel: the BIB → caps resolution, the
//! manifest contract, and `boot()` itself — handshake, mounts, console
//! stdio, init spawn.

use aerosls_blockcache::BufferAlloc;
use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE};
use aerosls_proto::bootinfo::BootInfo;
use aerosls_proto::kabi::{SendCap, CAP_CHAN, CAP_MEM};
use aerosls_proto::{R, W};
use aerosls_ramdisk::endpoints::EndpointSet;
use aerosls_ramdisk::server::{self, Device};
use aerosls_sidecar::{boot, BootCaps};
use aerosls_vfs::{CharNode, ImageBuilder, O_RDONLY};
use std::sync::Arc;
use std::thread::JoinHandle;

type FC = FakeClient;
type FA = FakeAlloc;

/// The rootfs the booted sidecar runs: applet script files (first line
/// names the applet) plus the boot script init executes. The last line
/// names a nonexistent program — the child must exit 127 and init must
/// reap it and carry on.
fn image() -> Vec<u8> {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_file("/etc/passwd", b"root:x:0:0:root:/root:/bin/sh\n", 0o644);
    b.add_file("/etc/group", b"root:x:0:\n", 0o644);
    b.add_file("/bin/cat", b"cat\n", 0o755);
    b.add_file("/bin/echo", b"echo\n", 0o755);
    b.add_file(
        "/etc/init.rc",
        b"# Boot script - v1: path arg... one > redirect\n\n/bin/cat /etc/passwd\n/bin/echo booted > /tmp/out\n/bin/nope\n",
        0o644,
    );
    b.build()
}

fn boot_driver(fake: FakeKernel) -> JoinHandle<()> {
    let (base, len) = fake.storage_info();
    let dev = Device {
        storage_slot: DRIVER_STORAGE,
        storage_base: base,
        storage_len: len,
        storage_writable: false,
    };
    let mut eps = EndpointSet::new(DRIVER_CONSOLE);
    for h in fake.initial_chan_caps() {
        eps.adopt(h);
    }
    std::thread::spawn(move || {
        let _ = server::run(&fake, &mut eps, &dev);
    })
}

#[derive(Clone)]
struct FakeAlloc(FC);

impl BufferAlloc for FakeAlloc {
    fn alloc(&mut self, len: usize) -> Result<(SendCap, u64), i32> {
        let handle = self.0.new_region(vec![0u8; len], R | W);
        let info = self.0.cap_info(handle)?;
        Ok((
            SendCap {
                slot: handle,
                offset: 0,
                len: len as u32,
                rights: R | W,
                flags: 0,
            },
            info.base,
        ))
    }
}

/// Read a whole file through a *fresh* VFS task (the booted init has
/// exited, so task 0's fd table is gone).
fn read_all(vfs: &mut aerosls_vfs::Vfs<FC, FA>, path: &str) -> Vec<u8> {
    let t = vfs.add_task().unwrap();
    let fd = vfs.open(t, path, O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 128];
    let mut out = Vec::new();
    loop {
        match vfs.read(t, fd, &mut buf) {
            Ok(0) => break,
            Ok(n) => out.extend_from_slice(&buf[..n]),
            Err(_) => break,
        }
    }
    vfs.close(t, fd).unwrap();
    out
}

/// The full boot: BIB-shaped caps → handshake → mounts → the boot script
/// runner as init with console stdio → init forks one child per
/// `/etc/init.rc` line and runs the system to completion.
#[test]
fn boot_mounts_and_runs_init() {
    let (fake, client) = FakeKernel::new(image(), 1);
    let t = boot_driver(fake);

    // The sim's client table: no budget/console caps; the ramdisk endpoint
    // is the first wired handle (0).
    let caps = BootCaps::new(0, 0, 0, None, 0);
    let console = Arc::new(CharNode::console());
    let mut booted = boot(client.clone(), &caps, console.clone(), FakeAlloc(client.clone()))
        .expect("boot");

    booted.run(200);

    assert_eq!(booted.proc.exit_code(0), Some(0), "init exited cleanly");
    // The script's `/bin/cat /etc/passwd` wrote the passwd line to console
    // stdout (fd 1), read through the full chain: cache → driver → storage.
    assert_eq!(
        console.console_io().output(),
        b"root:x:0:0:root:/root:/bin/sh\n",
        "the boot script's cat carried the rootfs read to the console"
    );
    // `/bin/echo booted > /tmp/out`: the script runner's one `>` redirect
    // pointed the child's fd 1 at the ramfs file.
    assert_eq!(read_all(&mut booted.proc.vfs, "/tmp/out"), b"booted\n");
    // The root mount is a real aerofs on the cache, not a shadow.
    assert_eq!(
        read_all(&mut booted.proc.vfs, "/etc/passwd"),
        b"root:x:0:0:root:/root:/bin/sh\n"
    );
    // The bogus /bin/nope line cost a 127-exiting child; init reaped it and
    // still finished the script cleanly (proved by the exit above).
    assert!(booted
        .proc
        .wake_trace
        .iter()
        .any(|w| matches!(w, aerosls_procmgr::WakeEvent::Parked(0, aerosls_procmgr::BlockReason::WaitingChild(_)))),
        "init parked waiting for at least one script child"
    );

    client.kill_driver(0);
    t.join().unwrap();
}

/// `BootCaps::from_bib` resolves the initial caps by manifest name from a
/// kernel-filled Boot Info Block.
#[test]
fn from_bib_resolves_initial_caps() {
    let bib = build_bib(&[
        ("budget", CAP_MEM, 0x3, 0x1000_0000, 0x100_0000, 0),
        ("console", CAP_CHAN, 0x7, 0, 0, 1),
        ("ramdisk", CAP_CHAN, 0x7, 0, 0, 2),
    ]);
    let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
    let caps = BootCaps::from_bib(&info).unwrap();
    assert_eq!(caps.budget_slot, 0);
    assert_eq!(caps.budget_base, 0x1000_0000);
    assert_eq!(caps.budget_len, 0x100_0000);
    assert_eq!(caps.console_chan, Some(1));
    assert_eq!(caps.ramdisk_chan, 2);

    // A BIB missing the ramdisk channel must fail loudly.
    let bib = build_bib(&[("budget", CAP_MEM, 0x3, 0x1000_0000, 0x100_0000, 0)]);
    let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
    assert_eq!(
        BootCaps::from_bib(&info),
        Err(aerosls_sidecar::BootErr::MissingCap("ramdisk"))
    );
}

/// The manifest and the BIB are the same contract in two formats: the
/// manifest names the initial caps, the kernel builds the table in record
/// order, and the BIB reports it. Assert the names line up end to end.
#[test]
fn manifest_names_line_up_with_bib_caps() {
    use aerosls_proto::manifest::{
        build_manifest, parse_manifest, Budget, CapKind, Image, Limits, Manifest, ManifestCap,
        MAX_MANIFEST_CAPS,
    };
    let m = Manifest {
        version_major: 1,
        version_minor: 0,
        flags: 0,
        personality: Some("aerosls.posix.v1"),
        image: Some(Image {
            offset: 0x4000,
            size: 0xC000,
            entry: 0x4000,
        }),
        budget: Some(Budget {
            mem_bytes: 1 << 25,
            stack_bytes: 1 << 18,
            heap_initial: 1 << 23,
        }),
        cpu: None,
        limits: Some(Limits {
            max_tasks: 64,
            max_fds: 4096,
            max_channels: 128,
            max_open_files: 512,
            chan_queue_depth: 64,
        }),
        caps: [
            Some(ManifestCap {
                name: "budget",
                rights: 0x3,
                kind: CapKind::Mem {
                    base: 0x1000_0000,
                    size: 1 << 25,
                },
            }),
            Some(ManifestCap {
                name: "console",
                rights: 0x7,
                kind: CapKind::Chan {
                    peer: Some("kernel.debug.console"),
                    flags: 0,
                },
            }),
            Some(ManifestCap {
                name: "ramdisk",
                rights: 0x7,
                kind: CapKind::Chan {
                    peer: Some("drv.ramdisk.0"),
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
        ],
        n_caps: 3,
        bootstrap: None,
        flags_value: None,
        signature: None,
    };
    let blob = build_manifest(&m);
    let parsed = parse_manifest(&blob).unwrap();
    assert_eq!(parsed.personality, Some("aerosls.posix.v1"));

    // The kernel fills the table in record order; the BIB reports the same
    // order. Build the BIB from the manifest's caps and resolve them back.
    let slots: Vec<(String, u16, u16, u64, u64, u32)> = parsed
        .caps()
        .iter()
        .enumerate()
        .map(|(i, c)| {
            let c = c.unwrap();
            let (base, len) = match c.kind {
                CapKind::Mem { base, size } => (base, size),
                CapKind::Chan { .. } => (0, 0),
            };
            let ty = match c.kind {
                CapKind::Mem { .. } => CAP_MEM,
                CapKind::Chan { .. } => CAP_CHAN,
            };
            (c.name.to_string(), ty, c.rights, base, len, i as u32)
        })
        .collect();
    let bib = build_bib(
        &slots
            .iter()
            .map(|(n, ty, r, b, l, s)| (n.as_str(), *ty, *r, *b, *l, *s))
            .collect::<Vec<_>>(),
    );
    let info = unsafe { BootInfo::from_raw(bib.as_ptr()) }.unwrap();
    let caps = BootCaps::from_bib(&info).unwrap();
    assert_eq!(caps.budget_slot, 0);
    assert_eq!(caps.console_chan, Some(1));
    assert_eq!(caps.ramdisk_chan, 2);
    let _ = MAX_MANIFEST_CAPS;
}

/// Build a Boot Info Block with the given caps (name, ty, rights, base,
/// len, slot). Layout per `aerosls_proto::bootinfo`.
fn build_bib(caps: &[(&str, u16, u16, u64, u64, u32)]) -> Vec<u8> {
    const HEADER_LEN: usize = 32;
    let mut b = Vec::new();
    b.extend_from_slice(b"AERSLSB1");
    b.extend_from_slice(&1u16.to_le_bytes()); // version
    b.extend_from_slice(&(caps.len() as u16).to_le_bytes()); // cap_count
    b.extend_from_slice(&(1u64 << 20).to_le_bytes()); // budget_bytes
    b.extend_from_slice(&0x2000_0000u64.to_le_bytes()); // stack_top
    let total = HEADER_LEN
        + caps
            .iter()
            .map(|(n, _, _, _, _, _)| 2 + n.len() + 2 + 4 + 16)
            .sum::<usize>();
    b.extend_from_slice(&(total as u32).to_le_bytes()); // total_len
    for (name, ty, rights, base, len, slot) in caps {
        b.extend_from_slice(&(name.len() as u16).to_le_bytes());
        b.extend_from_slice(name.as_bytes());
        b.extend_from_slice(&(*slot as u16).to_le_bytes());
        b.push(*ty as u8);
        b.push(*rights as u8);
        b.extend_from_slice(&base.to_le_bytes());
        b.extend_from_slice(&len.to_le_bytes());
    }
    b
}
