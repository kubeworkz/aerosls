//! Integration tests for the VFS, driven end to end: the *real* ramdisk
//! driver runs in its own thread against the fake kernel, the block cache
//! (the protocol client) connects to it, and the VFS mounts aerofs-lite on
//! top — so the full chain `VFS → block cache → wire → driver → storage` is
//! exercised for real. The respawn tests kill the driver and remount a
//! replacement, exactly as the device half of the respawn state machine
//! prescribes (respawn decision §5, §6).

use aerosls_blockcache::{BlockCache, BufferAlloc};
use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE};
use aerosls_proto::kabi::SendCap;
use aerosls_proto::*;
use aerosls_ramdisk::endpoints::EndpointSet;
use aerosls_ramdisk::server::{self, Device};
use aerosls_vfs::{
    Errno, ImageBuilder, MountState, Vfs, O_APPEND, O_CREAT, O_EXCL, O_RDONLY, O_RDWR,
    O_WRONLY, SEEK_SET,
};
use std::thread::JoinHandle;

const PASSWD: &[u8] = b"root:x:0:0:root:/root:/bin/sh\n";

/// A boot image with the layout the Phase 2 doc sketches
/// (`/etc/passwd`, `/bin/busybox`, ...).
fn sample_image() -> Vec<u8> {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_dir("/bin", 0o755);
    b.add_file("/etc/passwd", PASSWD, 0o644);
    b.add_file("/etc/group", b"root:x:0:\n", 0o644);
    b.add_file("/bin/busybox", &[0x7f; 3000], 0o755);
    b.build()
}

/// The same image but with different content — the "different device behind
/// the same name" case for revalidation.
fn other_image() -> Vec<u8> {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_file("/etc/passwd", b"DIFFERENT IMAGE\n", 0o644);
    b.build()
}

fn perms_image() -> Vec<u8> {
    let mut b = ImageBuilder::new();
    b.add_dir("/etc", 0o755);
    b.add_file("/etc/secret", b"top secret", 0o600);
    b.add_file("/etc/readme", b"public", 0o644);
    b.build()
}

/// Boot the real ramdisk driver the way `rust_entry` would.
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

/// Fake-side `BufferAlloc`: each request gets a fresh fake-kernel region.
#[derive(Clone)]
struct FakeAlloc(FakeClient);

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

/// Boot a driver over `img` and return (fake, client, driver thread).
fn boot(img: Vec<u8>) -> (FakeKernel, FakeClient, JoinHandle<()>) {
    let (fake, client) = FakeKernel::new(img, 1);
    let t = boot_driver(fake.clone());
    (fake, client, t)
}

/// A VFS with `/` mounted on the connected aerofs device and `/tmp` ramfs.
fn mount_root(client: &FakeClient) -> Vfs<FakeClient, FakeAlloc> {
    let cache = BlockCache::connect(client.clone(), 0, FakeAlloc(client.clone())).unwrap();
    let mut vfs = Vfs::new();
    vfs.mount_aerofs("/", cache).unwrap();
    vfs.mount_ramfs("/tmp").unwrap();
    vfs
}

#[test]
fn mount_and_open_passwd() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    let fd = vfs.open(0, "/etc/passwd", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 64];
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], PASSWD, "content matches the image");
    assert_eq!(vfs.offset_of(0, fd), Some(PASSWD.len() as u64));

    // stat agrees.
    let st = vfs.fstat(0, fd).unwrap();
    assert_eq!(st.size, PASSWD.len() as u64);
    let st = vfs.stat(0, "/etc/passwd").unwrap();
    assert_eq!(st.size, PASSWD.len() as u64);

    // Reading past EOF returns 0, not an error.
    let mut tail = [0u8; 8];
    assert_eq!(vfs.read(0, fd, &mut tail).unwrap(), 0);

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn path_resolution_and_dirs() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    // Root lists the image's top-level dirs (mount points are not overlaid
    // in v1 — a documented limitation).
    let root = vfs.read_dir(0, "/").unwrap();
    let names: Vec<&str> = root.iter().map(|e| e.name.as_str()).collect();
    assert!(names.contains(&"etc"), "root lists etc: {names:?}");
    assert!(names.contains(&"bin"));
    assert!(!names.contains(&"tmp"), "mount points are not overlaid in v1");

    let etc = vfs.read_dir(0, "/etc").unwrap();
    let names: Vec<&str> = etc.iter().map(|e| e.name.as_str()).collect();
    assert!(names.contains(&"passwd"), "etc lists passwd: {names:?}");
    assert!(names.contains(&"group"));

    // Errors.
    assert_eq!(vfs.open(0, "/nope", O_RDONLY, 0), Err(Errno::ENoent));
    assert_eq!(
        vfs.open(0, "/etc/passwd/x", O_RDONLY, 0),
        Err(Errno::ENotdir)
    );
    assert_eq!(vfs.open(0, "/etc", O_RDONLY, 0), Err(Errno::EIsdir));
    assert_eq!(vfs.read_dir(0, "/etc/passwd"), Err(Errno::ENotdir));

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn fd_semantics_shared_offset_rights_close() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    let fd = vfs.open(0, "/etc/passwd", O_RDONLY, 0).unwrap();
    let mut a = [0u8; 4];
    vfs.read(0, fd, &mut a).unwrap();
    assert_eq!(&a, b"root");

    // dup shares the offset (POSIX open file description).
    let d = vfs.dup(0, fd).unwrap();
    let mut b = [0u8; 4];
    vfs.read(0, d, &mut b).unwrap();
    assert_eq!(&b, b":x:0", "dup'd fd continues where the original left off");

    // lseek on one fd is visible through the other.
    vfs.lseek(0, fd, 0, SEEK_SET).unwrap();
    assert_eq!(vfs.offset_of(0, d), Some(0));

    // Rights minted at open: an O_RDONLY fd cannot write.
    assert_eq!(vfs.write(0, fd, b"x"), Err(Errno::EBadf));

    // dup2 replaces the target entry (the /etc/group fd is gone) and the
    // dup'd entry shares the source node/offset.
    let f2 = vfs.open(0, "/etc/group", O_RDONLY, 0).unwrap();
    vfs.dup2(0, fd, f2).unwrap();
    let mut c = [0u8; 8];
    let n = vfs.read(0, f2, &mut c).unwrap();
    assert_eq!(&c[..n], b"root:x:0", "dup2'd fd now names passwd at its offset");
    assert!(vfs.fd_ino(0, f2) == vfs.fd_ino(0, fd), "dup2 shares the node");

    // close then read → EBADF.
    vfs.close(0, fd).unwrap();
    assert_eq!(vfs.read(0, fd, &mut c), Err(Errno::EBadf));
    assert_eq!(vfs.close(0, fd), Err(Errno::EBadf));

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn ramfs_writes_and_mutations() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    // Create + write + read in /tmp.
    let fd = vfs.open(0, "/tmp/f", O_CREAT | O_RDWR, 0o644).unwrap();
    vfs.write(0, fd, b"hello").unwrap();
    vfs.lseek(0, fd, 0, SEEK_SET).unwrap();
    let mut buf = [0u8; 16];
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"hello");
    assert_eq!(vfs.fstat(0, fd).unwrap().size, 5);

    // O_APPEND (a second fd on the same file) writes at the end regardless
    // of the offset.
    let ap = vfs.open(0, "/tmp/f", O_RDWR | O_APPEND, 0o644).unwrap();
    vfs.lseek(0, fd, 0, SEEK_SET).unwrap();
    vfs.write(0, ap, b" world").unwrap();
    assert_eq!(vfs.fstat(0, fd).unwrap().size, 11);
    vfs.lseek(0, fd, 0, SEEK_SET).unwrap();
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"hello world");

    // O_EXCL rejects a second create.
    assert_eq!(
        vfs.open(0, "/tmp/f", O_CREAT | O_EXCL | O_RDWR, 0o644),
        Err(Errno::EExist)
    );

    // mkdir / rmdir / unlink.
    vfs.mkdir(0, "/tmp/d", 0o755).unwrap();
    vfs.rmdir(0, "/tmp/d").unwrap();
    vfs.unlink(0, "/tmp/f").unwrap();
    assert_eq!(vfs.open(0, "/tmp/f", O_RDONLY, 0), Err(Errno::ENoent));
    assert_eq!(vfs.open(0, "/tmp/d", O_RDONLY, 0), Err(Errno::ENoent));

    // ramfs never goes stale: it works even after the device died.
    client.kill_driver(0);
    t.join().unwrap();
    let fd = vfs.open(0, "/tmp/after", O_CREAT | O_RDWR, 0o644).unwrap();
    vfs.write(0, fd, b"still here").unwrap();
    let _ = fake;
}

#[test]
fn aerofs_is_read_only() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    assert_eq!(vfs.open(0, "/etc/passwd", O_WRONLY, 0), Err(Errno::ERofs));
    assert_eq!(vfs.open(0, "/etc/passwd", O_RDWR, 0), Err(Errno::ERofs));
    assert_eq!(
        vfs.open(0, "/etc/new", O_CREAT | O_WRONLY, 0o644),
        Err(Errno::ERofs)
    );
    assert_eq!(vfs.unlink(0, "/etc/passwd"), Err(Errno::ERofs));
    assert_eq!(vfs.mkdir(0, "/etc/newdir", 0o755), Err(Errno::ERofs));
    assert_eq!(vfs.rmdir(0, "/etc"), Err(Errno::ERofs));

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn stale_mount_fails_eio_not_enoent() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    // Hold an fd open across the death.
    let fd = vfs.open(0, "/etc/passwd", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 4];
    vfs.read(0, fd, &mut buf).unwrap();

    client.kill_driver(0);

    // The mount still resolves — EIO, never ENOENT (respawn §4.2, §6).
    assert_eq!(vfs.open(0, "/etc/passwd", O_RDONLY, 0), Err(Errno::EIo));
    assert_eq!(vfs.stat(0, "/etc/passwd"), Err(Errno::EIo));
    assert_eq!(vfs.read_dir(0, "/etc"), Err(Errno::EIo));
    // The held fd fails permanently.
    assert_eq!(vfs.read(0, fd, &mut buf), Err(Errno::EIo));

    // Observability: the device mount is stale, ramfs is active.
    let m = vfs.mounts();
    assert_eq!(m[0], (String::from("/"), MountState::Stale));
    assert_eq!(m[1], (String::from("/tmp"), MountState::Active));

    // And a stale mount must not shadow a path that never existed: a
    // non-existent path on the stale device is still EIO, not ENOENT —
    // the device is down, that's the honest answer.
    assert_eq!(vfs.open(0, "/definitely/not/here", O_RDONLY, 0), Err(Errno::EIo));

    t.join().unwrap();
    let _ = fake;
}

#[test]
fn open_fd_fails_permanently_across_remount() {
    let img = sample_image();
    let (fake, client, t) = boot(img.clone());
    let mut vfs = mount_root(&client);

    let old_fd = vfs.open(0, "/etc/passwd", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 4];
    vfs.read(0, old_fd, &mut buf).unwrap();
    assert_eq!(&buf, b"root");

    client.kill_driver(0);
    t.join().unwrap();
    assert_eq!(vfs.read(0, old_fd, &mut buf), Err(Errno::EIo));

    // Respawn: a *fresh* driver serving the *same* image, re-attached.
    let (fake2, client2, t2) = boot(img);
    let new_cache = BlockCache::connect(client2.clone(), 0, FakeAlloc(client2.clone())).unwrap();
    vfs.remount_aerofs(0, new_cache).unwrap();

    // The mount is live again; fresh opens work and see the same content.
    let new_fd = vfs.open(0, "/etc/passwd", O_RDONLY, 0).unwrap();
    let n = vfs.read(0, new_fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], &PASSWD[..n]);

    // But the pre-death fd fails permanently — it must never silently
    // reconnect to the replacement (respawn §6).
    assert_eq!(vfs.read(0, old_fd, &mut buf), Err(Errno::EIo));
    assert_eq!(vfs.lseek(0, old_fd, 0, SEEK_SET), Err(Errno::EIo));

    // ramfs is untouched by the respawn.
    let fd = vfs.open(0, "/tmp/x", O_CREAT | O_RDWR, 0o644).unwrap();
    vfs.write(0, fd, b"ok").unwrap();

    client2.kill_driver(0);
    t2.join().unwrap();
    let _ = (fake, fake2);
}

#[test]
fn remount_revalidation_rejects_different_device() {
    let img = sample_image();
    let (fake, client, t) = boot(img);
    let mut vfs = mount_root(&client);
    client.kill_driver(0);
    t.join().unwrap();

    // A replacement driver behind the same name serves a *different* image.
    let (_fake2, client2, t2) = boot(other_image());
    let new_cache = BlockCache::connect(client2.clone(), 0, FakeAlloc(client2.clone())).unwrap();
    assert_eq!(
        vfs.remount_aerofs(0, new_cache),
        Err(Errno::EIo),
        "superblock identity mismatch must fail the attach"
    );
    // The mount stays stale; nothing resolves.
    assert_eq!(vfs.open(0, "/etc/passwd", O_RDONLY, 0), Err(Errno::EIo));
    assert_eq!(vfs.open(0, "/etc/passwd", O_RDONLY, 0), Err(Errno::EIo));
    let m = vfs.mounts();
    assert_eq!(m[0].1, MountState::Stale);

    client2.kill_driver(0);
    t2.join().unwrap();
    let _ = fake;
}

#[test]
fn permission_checks_gate_open() {
    let (fake, client, t) = boot(perms_image());
    let mut vfs = mount_root(&client);

    // Task 0 is root: reads even a 0o600 file.
    let fd = vfs.open(0, "/etc/secret", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 16];
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"top secret");

    // An unprivileged task cannot.
    let t1 = vfs.add_task().unwrap();
    vfs.set_cred(t1, 1000, 1000).unwrap();    assert_eq!(vfs.open(t1, "/etc/secret", O_RDONLY, 0), Err(Errno::EAcces));

    // World-readable files work for anyone.
    let fd = vfs.open(t1, "/etc/readme", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 8];
    let n = vfs.read(t1, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"public");

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn relative_paths_and_cwd() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    // Default cwd is /.
    let fd = vfs.open(0, "etc/passwd", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 8];
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"root:x:0");

    vfs.set_cwd(0, "/etc").unwrap();
    let fd = vfs.open(0, "passwd", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 8];
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"root:x:0");
    // ".." climbs out of the cwd.
    let fd = vfs.open(0, "../etc/group", O_RDONLY, 0).unwrap();
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"root:x:0");

    // set_cwd validates the target.
    assert_eq!(vfs.set_cwd(0, "/nope"), Err(Errno::ENoent));
    assert_eq!(vfs.set_cwd(0, "/etc/passwd"), Err(Errno::ENotdir));

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn block_cache_is_used_under_the_vfs() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);

    let before = fake.driver_requests();
    // The mount already read the superblock; a fresh read of a small file
    // should reach the driver for its data block.
    let fd = vfs.open(0, "/etc/group", O_RDONLY, 0).unwrap();
    let mut buf = [0u8; 16];
    let n = vfs.read(0, fd, &mut buf).unwrap();
    assert_eq!(&buf[..n], b"root:x:0:\n");
    assert!(fake.driver_requests() > before, "the read must reach the driver");

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn mount_table_rejects_duplicates_and_longest_prefix_wins() {
    let (fake, client, t) = boot(sample_image());
    let cache = BlockCache::connect(client.clone(), 0, FakeAlloc(client.clone())).unwrap();
    let mut vfs = Vfs::new();
    vfs.mount_aerofs("/", cache).unwrap();
    let cache2 = BlockCache::connect(client.clone(), 0, FakeAlloc(client.clone())).unwrap();
    assert_eq!(vfs.mount_aerofs("/", cache2), Err(Errno::EExist));

    // /tmp goes to ramfs; a sibling /tmpx path does NOT match /tmp.
    vfs.mount_ramfs("/tmp").unwrap();
    let fd = vfs.open(0, "/tmp/a", O_CREAT | O_RDWR, 0o644).unwrap();
    vfs.write(0, fd, b"tmpfs").unwrap();
    // /tmpx isn't mounted → resolves to / → ENoent (aerofs has no tmpx).
    assert_eq!(vfs.open(0, "/tmpx", O_RDONLY, 0), Err(Errno::ENoent));

    client.kill_driver(0);
    t.join().unwrap();
    let _ = fake;
}

#[test]
fn mount_state_derived_from_device() {
    let (fake, client, t) = boot(sample_image());
    let mut vfs = mount_root(&client);
    assert_eq!(vfs.mounts()[0].1, MountState::Active);
    // A live device but no op yet: still Active (state is checked lazily).
    assert_eq!(vfs.mounts()[0].1, MountState::Active);
    client.kill_driver(0);
    // Force the close event to be observed (the sidecar event loop's job in
    // the real system; here the first op surfaces it).
    let _ = vfs.open(0, "/etc/passwd", O_RDONLY, 0);
    assert_eq!(vfs.mounts()[0].1, MountState::Stale);
    assert_eq!(vfs.mounts()[1].1, MountState::Active);
    t.join().unwrap();
    let _ = fake;
}
