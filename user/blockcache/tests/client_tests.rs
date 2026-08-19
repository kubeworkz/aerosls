//! Integration tests for the block cache, driven end to end: the *real*
//! ramdisk driver runs in its own thread against the fake kernel, and the
//! block cache (the client) talks to it through the same fake — the wire
//! protocol, grant minting/revocation, and window=1 are all exercised for
//! real.
//!
//! One test (`in_flight_read_aborts_with_close`) replaces the driver with a
//! tiny scripted one that never replies to `RD_READ`, so the client's
//! blocked recv and the driver's death can be ordered deterministically.

use aerosls_blockcache::{BlockCache, BufferAlloc, Error, State};
use aerosls_kernel_sim::{
    FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE,
};
use aerosls_proto::kabi::{GrantedCap, Kernel, SendCap, ERR_REVOKED, ERR_SHUTDOWN, TIMEOUT_NONE};
use aerosls_proto::*;
use aerosls_ramdisk::endpoints::EndpointSet;
use aerosls_ramdisk::server::{self, Device};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

/// Deterministic, non-trivial storage content: blocks of 512 bytes.
fn storage_image(blocks: usize) -> Vec<u8> {
    let n = blocks * BLOCK_SIZE as usize;
    (0..n).map(|i| (i % 251) as u8).collect()
}

/// Boot the real ramdisk driver the way `rust_entry` would: adopt every
/// initial CHAN cap except console, then run the server loop.
/// `writable` selects the Device's view of the storage cap (rw vs r-only).
fn boot_driver(fake: FakeKernel, writable: bool) -> JoinHandle<()> {
    let (base, len) = fake.storage_info();
    let dev = Device {
        storage_slot: DRIVER_STORAGE,
        storage_base: base,
        storage_len: len,
        storage_writable: writable,
    };
    let mut eps = EndpointSet::new(DRIVER_CONSOLE);
    for h in fake.initial_chan_caps() {
        eps.adopt(h);
    }
    std::thread::spawn(move || {
        let _ = server::run(&fake, &mut eps, &dev);
    })
}

/// Fake-side `BufferAlloc`: each request gets a fresh fake-kernel region
/// (R|W held; the cache requests the subset it needs on the wire).
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

/// One endpoint, real driver, connected block cache.
fn setup(blocks: usize) -> (FakeKernel, FakeClient, JoinHandle<()>, Vec<u8>) {
    let storage = storage_image(blocks);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_driver(fake.clone(), false);
    (fake, client, t, storage)
}

/// Like `setup`, but with a writable storage cap (the writable-driver
/// variant) — for the write-path tests.
fn setup_writable(blocks: usize) -> (FakeKernel, FakeClient, JoinHandle<()>, Vec<u8>) {
    let storage = storage_image(blocks);
    let (fake, client) = FakeKernel::new_writable(storage.clone(), 1);
    let t = boot_driver(fake.clone(), true);
    (fake, client, t, storage)
}

fn connect(client: &FakeClient) -> BlockCache<FakeClient, FakeAlloc> {
    BlockCache::connect(client.clone(), 0, FakeAlloc(client.clone())).unwrap()
}

#[test]
fn connect_handshake_reads_geometry() {
    let (fake, client, t, _storage) = setup(8);
    let cache = connect(&client);
    assert_eq!(cache.state(), State::Live);
    let info = cache.info();
    assert_eq!(info.block_size, BLOCK_SIZE);
    assert_eq!(info.blocks, 8);
    assert!(info.read_only, "the read-only manifest must report read-only");
    assert_eq!(cache.blocks(), 8);
    assert_eq!(fake.driver_requests(), 1, "exactly the handshake reached the driver");
    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_returns_storage_data() {
    let (_fake, client, t, storage) = setup(8);
    let mut cache = connect(&client);
    let mut d = [0u8; BLOCK_SIZE as usize];
    cache.read_block(2, &mut d).unwrap();
    assert_eq!(d, storage[2 * 512..3 * 512]);
    cache.read_block(7, &mut d).unwrap();
    assert_eq!(d, storage[7 * 512..8 * 512]);
    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn repeat_read_serves_from_cache() {
    let (fake, client, t, _storage) = setup(8);
    let mut cache = connect(&client);

    let before = fake.driver_requests();
    let mut d1 = [0u8; BLOCK_SIZE as usize];
    cache.read_block(2, &mut d1).unwrap();
    let after_miss = fake.driver_requests();
    assert_eq!(after_miss - before, 1, "a miss reaches the driver");

    let mut d2 = [0u8; BLOCK_SIZE as usize];
    cache.read_block(2, &mut d2).unwrap();
    let after_hit = fake.driver_requests();
    assert_eq!(after_hit - after_miss, 0, "a hit must not reach the driver");
    assert_eq!(d1, d2);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_multiblock() {
    let (_fake, client, t, storage) = setup(8);
    let mut cache = connect(&client);
    let mut dst = [0u8; 2 * BLOCK_SIZE as usize];
    cache.read(1, &mut dst).unwrap();
    assert_eq!(dst, storage[512..3 * 512]);
    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn write_updates_storage_and_invalidates_cache() {
    let (fake, client, t, storage) = setup_writable(8);
    let mut cache = connect(&client);

    // Prime the cache slot for block 3.
    let mut d = [0u8; BLOCK_SIZE as usize];
    cache.read_block(3, &mut d).unwrap();
    assert_eq!(fake.driver_requests(), 2);

    // Overwrite block 3.
    let new_block = [0x5Au8; BLOCK_SIZE as usize];
    cache.write_block(3, &new_block).unwrap();
    let st = client.storage_bytes();
    assert_eq!(&st[3 * 512..4 * 512], &new_block[..]);
    assert_eq!(&st[..3 * 512], &storage[..3 * 512], "only block 3 changed");

    // The cached copy was invalidated: the next read refetches.
    let n = fake.driver_requests();
    cache.read_block(3, &mut d).unwrap();
    assert_eq!(
        fake.driver_requests(),
        n + 1,
        "read after write must refetch, not serve the stale copy"
    );
    assert_eq!(d, new_block);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn write_rejected_on_read_only_device() {
    let (_fake, client, t, storage) = setup(8);
    let mut cache = connect(&client);

    let err = cache.write_block(0, &[0xAB; BLOCK_SIZE as usize]).unwrap_err();
    assert_eq!(err, Error::Status(RD_ERR_RO));
    // The device is alive — a driver error reply is not a stale.
    assert_eq!(cache.state(), State::Live);
    assert_eq!(client.storage_bytes(), storage, "storage untouched");

    // Reads still work after the rejected write.
    let mut d = [0u8; BLOCK_SIZE as usize];
    cache.read_block(0, &mut d).unwrap();

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_out_of_range_fails() {
    let (_fake, client, t, _storage) = setup(8);
    let mut cache = connect(&client);

    let mut d = [0u8; BLOCK_SIZE as usize];
    let err = cache.read_block(8, &mut d).unwrap_err();
    assert_eq!(err, Error::Status(RD_ERR_RANGE));
    assert_eq!(cache.state(), State::Live);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn flush_ok() {
    let (_fake, client, t, _storage) = setup(8);
    let mut cache = connect(&client);
    cache.flush().unwrap();
    assert_eq!(cache.state(), State::Live);
    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn map_establishes_durable_view() {
    let (_fake, client, t, storage) = setup(8);
    let mut cache = connect(&client);

    let view = cache.map().unwrap();
    assert_eq!(cache.view(), Some(view));
    assert_eq!(view.len, client.storage_len());
    assert_eq!(view.rights, R, "read-only storage maps read-only");

    let mut d = [0u8; BLOCK_SIZE as usize];
    view.read(1, &mut d).unwrap();
    assert_eq!(d, storage[512..2 * 512]);

    // The view survives subsequent requests on the endpoint.
    cache.read_block(0, &mut d).unwrap();
    view.read(2, &mut d).unwrap();
    assert_eq!(d, storage[2 * 512..3 * 512]);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn map_view_dies_with_driver() {
    let (_fake, client, t, _storage) = setup(8);
    let mut cache = connect(&client);

    let view = cache.map().unwrap();
    assert!(client.cap_info(view.handle).is_ok());

    // Driver death: deep revocation kills the persist grant through its
    // lineage to the driver's storage cap (plan §8.1, cap spec §3.4).
    client.kill_driver(9);
    assert_eq!(client.cap_info(view.handle), Err(ERR_REVOKED));

    // The cache is passive: it observes the death lazily. The next op
    // fails and flips the device stale; the view is dropped.
    let mut d = [0u8; BLOCK_SIZE as usize];
    assert!(cache.read_block(0, &mut d).is_err());
    assert_eq!(cache.state(), State::Stale { reason: CLOSE_PEER, detail: 0 });
    assert_eq!(cache.view(), None, "the mapped view is dropped on stale");

    t.join().unwrap();
}

#[test]
fn idle_close_marks_stale() {
    let (_fake, client, t, _storage) = setup(8);
    let mut cache = connect(&client);

    // The driver dies while the cache is idle (no request in flight).
    // The close event sits in the queue; the cache learns on the next op,
    // which fails at the send with the kernel's teardown error.
    client.kill_driver(4);
    let mut d = [0u8; BLOCK_SIZE as usize];
    let err = cache.read_block(0, &mut d).unwrap_err();
    assert_eq!(err, Error::Kernel(ERR_SHUTDOWN));
    assert_eq!(cache.state(), State::Stale { reason: CLOSE_PEER, detail: 0 });

    // From here on every op fails immediately with Stale — never touches
    // the kernel again.
    let err2 = cache.read_block(0, &mut d).unwrap_err();
    assert_eq!(err2, Error::Stale { reason: CLOSE_PEER, detail: 0 });

    t.join().unwrap();
}

/// A scripted driver that answers the `RD_INFO` handshake but never replies
/// to `RD_READ` — it just records that the request arrived. Combined with
/// `kill_driver`, this makes the client's blocked recv and the driver's
/// death deterministic.
fn scripted_stall_driver(fake: FakeKernel, seen_read: Arc<AtomicBool>) -> JoinHandle<()> {
    let (_base, len) = fake.storage_info();
    let blocks = len / BLOCK_SIZE as u64;
    let ep = fake.initial_chan_caps()[0];
    std::thread::spawn(move || {
        let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
        let mut caps = [GrantedCap::default(); 8];
        loop {
            let list = [DRIVER_CONSOLE, ep];
            let (idx, _kind) = match fake.wait(&list, TIMEOUT_NONE) {
                Ok(x) => x,
                // kill_driver: shutdown → the driver thread exits.
                Err(_) => return,
            };
            let h = list[idx];
            let rr = fake.recv(h, &mut buf, &mut caps).unwrap();
            if rr.kind != CH_KIND_MSG {
                continue;
            }
            let frame = RdFrame::parse(&buf[..rr.len]).unwrap();
            match frame.ty {
                RD_INFO => {
                    let mut p = [0u8; 32];
                    p[..16].copy_from_slice(&RdFrame::new(RD_INFO, false).encode());
                    p[16..32].copy_from_slice(&encode_info_body(BLOCK_SIZE, blocks, 1));
                    fake.send(h, rr.tag, F_REPLY, &p, &[], TIMEOUT_NONE)
                        .unwrap();
                }
                // Stall: acknowledge receipt, never reply. The client stays
                // blocked in recv until the close event arrives.
                RD_READ => seen_read.store(true, Ordering::SeqCst),
                _ => {}
            }
        }
    })
}

#[test]
fn in_flight_read_aborts_with_close() {
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new(storage, 1);
    let seen_read = Arc::new(AtomicBool::new(false));
    let t = scripted_stall_driver(fake.clone(), seen_read.clone());

    let mut cache = BlockCache::connect(client.clone(), 0, FakeAlloc(client.clone())).unwrap();
    let mut d = [0u8; BLOCK_SIZE as usize];
    let reader = std::thread::spawn(move || cache.read_block(1, &mut d).map(|()| d));

    // Deterministic handoff: wait until the driver has the RD_READ (so the
    // client is provably blocked in recv), then kill the driver.
    for _ in 0..5000 {
        if seen_read.load(Ordering::SeqCst) {
            break;
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(seen_read.load(Ordering::SeqCst), "driver never saw the RD_READ");

    client.kill_driver(3);
    let res = reader.join().unwrap();
    // The blocked recv was woken by the close event (transport spec §6.3):
    // CLOSE_PEER_DEAD with the driver's exit detail.
    assert_eq!(
        res,
        Err(Error::Stale {
            reason: CLOSE_PEER_DEAD,
            detail: 3
        })
    );

    t.join().unwrap();
}
