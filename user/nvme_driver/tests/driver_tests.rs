//! Integration tests for the NVMe driver sidecar against the fake kernel
//! (Phase 5 §3 storage stack). Each test boots the driver in its own thread
//! and drives it from the client end, then tears the driver down with
//! `kill_driver` (which makes the driver's wait return and the thread join).
//!
//! The driver under test is the full protocol server (`server::run`) over
//! the `SimBackend` — a RAM region standing in for the controller. The fake
//! kernel's storage arena plays that role (its cap rights stand in for the
//! device's writability). The real MMIO backend (`nvme::NvmeDevice`) shares
//! this exact server and protocol, so the behavioral contract proven here is
//! the same one the hardware path serves; what cannot be exercised on a host
//! is only the MMIO submit path itself (the C driver's own tests draw the
//! same line).

use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE};
use aerosls_nvme_driver::backend::{BackendErr, BlockBackend, MapGrant};
use aerosls_nvme_driver::endpoints::EndpointSet;
use aerosls_nvme_driver::kapi::GrantedCap;
use aerosls_nvme_driver::kapi::SendCap;
use aerosls_nvme_driver::server;
use aerosls_proto::*;

/// Deterministic, non-trivial storage content: blocks of 512 bytes.
fn storage_image(blocks: usize) -> Vec<u8> {
    let n = blocks * BLOCK_SIZE as usize;
    (0..n).map(|i| (i % 251) as u8).collect()
}

/// Boot the server over `dev` the way `rust_entry` would for the
/// initial-table case: adopt every initial CHAN cap except console, then run
/// the loop.
fn boot_with<B: BlockBackend + Send + 'static>(fake: FakeKernel, mut dev: B) -> std::thread::JoinHandle<()> {
    let mut eps = EndpointSet::new(DRIVER_CONSOLE);
    for h in fake.initial_chan_caps() {
        eps.adopt(h);
    }
    std::thread::spawn(move || {
        let _ = server::run(&fake, &mut eps, &mut dev);
    })
}

/// Boot with a `SimBackend` over the fake's storage arena. `writable` and
/// `map_slot` mirror the device's properties (writable NVM, and the
/// zero-copy map that the real NVMe device does NOT have).
fn boot_sim(
    fake: FakeKernel,
    writable: bool,
    map_slot: Option<u32>,
) -> std::thread::JoinHandle<()> {
    let (base, len) = fake.storage_info();
    let mut dev = SimBackend::new(base, len, writable);
    if let Some(s) = map_slot {
        dev = dev.with_map_slot(s);
    }
    boot_with(fake, dev)
}

use aerosls_nvme_driver::backend::SimBackend;

fn frame(ty: u16) -> [u8; 16] {
    RdFrame::new(ty, false).encode()
}

fn rw_req(ty: u16, lba: u64, count: u32) -> [u8; 28] {
    let mut b = [0u8; 28];
    b[..16].copy_from_slice(&frame(ty));
    b[16..24].copy_from_slice(&lba.to_le_bytes());
    b[24..28].copy_from_slice(&count.to_le_bytes());
    b
}

/// Recv one reply and return `(frame_type, status, bytes)`.
fn recv_status(
    client: &FakeClient,
    chan: u32,
    buf: &mut [u8],
    caps: &mut [GrantedCap],
) -> (u16, u16, u64) {
    let rr = client.recv(chan, buf, caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_MSG, "expected a message, got kind {}", rr.kind);
    let f = RdFrame::parse(&buf[..rr.len]).unwrap();
    let (status, bytes) = parse_status_body(&buf[16..rr.len]).expect("status body");
    (f.ty, status, bytes)
}

/// Handshake (RD_INFO) on `chan`; returns the info geometry.
fn handshake(client: &FakeClient, chan: u32) -> (u32, u64, u32) {
    client.send(chan, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(chan, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 1);
    let f = RdFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, RD_INFO);
    assert_eq!(f.flags & RD_FLAG_ERROR, 0);
    parse_info_body(&buf[16..rr.len]).unwrap()
}

#[test]
fn boot_handshake_then_info_then_read() {
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_sim(fake.clone(), true, None);

    // Handshake: first message must be RD_INFO; geometry from the device.
    let (bs, blocks, info_flags) = handshake(&client, 0);
    assert_eq!(bs, 512, "sector size is the 512-byte logical block");
    assert_eq!(blocks, 8);
    assert_eq!(info_flags & 1, 0, "writable device must not report read-only");

    // RD_READ with a proper W grant into block 0 and block 3.
    for (tag, lba, expect) in [(2u32, 0u64, &storage[..512]), (3, 3, &storage[3 * 512..4 * 512])] {
        let region = client.new_region(vec![0u8; 512], R | W);
        let grant = SendCap {
            slot: region,
            offset: 0,
            len: 512,
            rights: W,
            flags: 0,
        };
        client.send(0, tag, 0, &rw_req(RD_READ, lba, 1), &[grant]).unwrap();
        let mut buf = [0u8; 64];
        let mut caps = [GrantedCap::default(); 8];
        let (ty, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
        assert_eq!(ty, RD_READ);
        assert_eq!(status, RD_OK);
        assert_eq!(bytes, 512);
        assert_eq!(client.region_bytes(region), expect);
        // The transient grant was auto-revoked at the reply.
        assert_eq!(fake.driver_grant_count(), 0);
    }

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn wrong_first_message_closes_endpoint() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake, true, None);

    // First message is RD_READ, not RD_INFO → the driver closes with
    // CLOSE_PROTO / RD_ERR_PROTO.
    client.send(0, 1, 0, &rw_req(RD_READ, 0, 1), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_CLOSE, "expected a close event");
    let (reason, detail) = parse_close_body(&buf[..rr.len]).unwrap();
    assert_eq!(reason, CLOSE_PROTO);
    assert_eq!(detail, RD_ERR_PROTO as u32);

    // The channel is now dead from the client's perspective too.
    assert!(client.send(0, 2, 0, &frame(RD_INFO), &[]).is_err());

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_requires_writable_grant() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake.clone(), true, None);

    handshake(&client, 0);

    // The client (buggy) requests an R-only grant for a READ buffer. The
    // kernel faithfully mints R (no amplification); the driver must refuse.
    let region = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: R,
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_READ, 0, 1), &[grant]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_ERR_CAP);
    // Buffer untouched.
    assert_eq!(client.region_bytes(region), vec![0u8; 512]);
    assert_eq!(fake.driver_grant_count(), 0);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_requires_full_size_grant() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    // 256-byte grant for a 512-byte read → RD_ERR_CAP.
    let region = client.new_region(vec![0u8; 256], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 256,
        rights: W,
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_READ, 0, 1), &[grant]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_ERR_CAP);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_range_enforced() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    // lba + count (8 + 1) > blocks (8) → RD_ERR_RANGE.
    let region = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_READ, 8, 1), &[grant]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_ERR_RANGE);

    // Boundary lba + count == blocks succeeds.
    let region = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 3, 0, &rw_req(RD_READ, 7, 1), &[grant]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_OK);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn write_persists_then_read_back() {
    // The NVMe driver is inherently read-write (NVM) — unlike the read-only
    // ramdisk manifest. Write a block, then read it back.
    let storage = storage_image(16);
    let (fake, client) = FakeKernel::new_writable(storage.clone(), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    let payload: Vec<u8> = (0..512).map(|i| (i * 7 % 251) as u8).collect();
    let region = client.new_region(payload.clone(), R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: R, // write buffers are granted R-only
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_WRITE, 5, 1), &[grant]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (ty, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_WRITE);
    assert_eq!(status, RD_OK);
    assert_eq!(bytes, 512);

    // Storage changed at lba 5 only.
    let st = client.storage_bytes();
    assert_eq!(&st[5 * 512..6 * 512], &payload[..]);
    assert_eq!(&st[..5 * 512], &storage[..5 * 512]);

    // Read it back through the protocol.
    let region2 = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region2,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 3, 0, &rw_req(RD_READ, 5, 1), &[grant]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_OK);
    assert_eq!(client.region_bytes(region2), payload);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn write_rejected_on_read_only_backend() {
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_sim(fake, false, None);

    let (_, _, info_flags) = handshake(&client, 0);
    assert_eq!(info_flags & 1, 1, "read-only backend must report read-only");

    let region = client.new_region(vec![0xABu8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: R,
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_WRITE, 0, 1), &[grant]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (ty, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_WRITE);
    assert_eq!(status, RD_ERR_RO);
    // Storage untouched.
    assert_eq!(client.storage_bytes(), storage);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn count_bounds_enforced() {
    let (fake, client) = FakeKernel::new(storage_image(64), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    // count 0 → INVAL.
    client.send(0, 2, 0, &rw_req(RD_READ, 0, 0), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_ERR_INVAL);

    // count above MAX_SECTORS (64) → INVAL, even with a big-enough grant.
    let region = client.new_region(vec![0u8; 64 * 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 64 * 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 3, 0, &rw_req(RD_READ, 0, 65), &[grant]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_ERR_INVAL);

    // Exactly MAX_SECTORS succeeds (a full 32 KiB request).
    client.send(0, 4, 0, &rw_req(RD_READ, 0, 64), &[grant]).unwrap();
    let (_, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_OK);
    assert_eq!(bytes, 64 * 512);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn flush_ok() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    client.send(0, 2, 0, &frame(RD_FLUSH), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (ty, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_FLUSH);
    assert_eq!(status, RD_OK);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn map_returns_persist_grant() {
    // SimBackend with a map slot: RD_MAP hands out a durable view of the
    // whole device (the zero-copy path — the ramdisk behavior).
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new_writable(storage.clone(), 1);
    let t = boot_sim(fake.clone(), true, Some(DRIVER_STORAGE));

    handshake(&client, 0);

    client.send(0, 2, 0, &frame(RD_MAP), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    let f = RdFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, RD_MAP);
    let (status, _) = parse_status_body(&buf[16..rr.len]).unwrap();
    assert_eq!(status, RD_OK);
    assert_eq!(rr.n_caps, 1, "RD_MAP reply must carry the persist grant");

    let g = caps[0];
    let ci = client.cap_info(g.handle).unwrap();
    assert_eq!(ci.ty, 1, "grant must be a MEM cap");
    assert_eq!(ci.rights, (R | W) as u16, "writable device → rw grant");
    assert_eq!(ci.base, client.storage_base());
    assert_eq!(ci.len, client.storage_len());
    // The mapped view is byte-identical to the storage.
    assert_eq!(client.region_bytes(g.handle), storage);

    // The grant is durable: a subsequent request on the same endpoint works.
    client.send(0, 3, 0, &frame(RD_INFO), &[]).unwrap();
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();
    assert!(client.cap_info(g.handle).is_ok());

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn map_unmappable_returns_nomem() {
    // A backend without a direct map (the REAL NVMe device: storage is only
    // reachable via DMA commands, never a MEM region) answers NOMEM.
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    client.send(0, 2, 0, &frame(RD_MAP), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (ty, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_MAP);
    assert_eq!(status, RD_ERR_NOMEM);
    assert_eq!(caps[0].handle, 0, "no grant attached");

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn map_grant_dies_with_driver() {
    // Writable storage cap: the map grant carries R|W (matching the
    // writable device), and the kernel refuses to amplify a read-only cap.
    let (fake, client) = FakeKernel::new_writable(storage_image(8), 1);
    let t = boot_sim(fake, true, Some(DRIVER_STORAGE));

    handshake(&client, 0);

    client.send(0, 2, 0, &frame(RD_MAP), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _rr = client.recv(0, &mut buf, &mut caps).unwrap();
    let g = caps[0];
    assert!(client.cap_info(g.handle).is_ok());

    // Driver death: the kernel revokes the driver's table, which deep-revokes
    // the client's grant through its lineage.
    client.kill_driver(7);
    assert_eq!(
        client.cap_info(g.handle),
        Err(aerosls_nvme_driver::kapi::ERR_REVOKED)
    );

    // ...and the close event is delivered, never lost.
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_CLOSE);
    let (reason, detail) = parse_close_body(&buf[..rr.len]).unwrap();
    assert_eq!(reason, CLOSE_PEER_DEAD);
    assert_eq!(detail, 7);

    t.join().unwrap();
}

#[test]
fn new_channel_adoption_at_boot() {
    // Boot style: no initial endpoint; it arrives as NEW_CHANNEL on the
    // console channel after the driver starts.
    let (fake, client) = FakeKernel::new(storage_image(8), 0);
    let t = boot_sim(fake.clone(), true, None);
    assert_eq!(fake.initial_chan_caps().len(), 0);

    let ep = client.inject_posix_endpoint().unwrap();

    // Handshake + serve on the adopted endpoint.
    client.send(ep, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(ep, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 1);
    let f = RdFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, RD_INFO);

    // A second injection yields a second independent endpoint.
    let ep2 = client.inject_posix_endpoint().unwrap();
    client.send(ep2, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let rr = client.recv(ep2, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_MSG);
    assert_eq!(rr.tag, 1);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn multi_endpoint_interleaving() {
    let storage = storage_image(16);
    let (fake, client) = FakeKernel::new_writable(storage.clone(), 2);
    let t = boot_sim(fake, true, None);

    // Two clients (client handles 0 and 1), independent windows.
    handshake(&client, 0);
    handshake(&client, 1);

    let region_a = client.new_region(vec![0u8; 512], R | W);
    let region_b = client.new_region(vec![0u8; 512], R | W);
    let grant_a = SendCap {
        slot: region_a,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    let grant_b = SendCap {
        slot: region_b,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 1, 0, &rw_req(RD_READ, 0, 1), &[grant_a]).unwrap();
    client.send(1, 1, 0, &rw_req(RD_READ, 4, 1), &[grant_b]).unwrap();

    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (_, sa, _) = recv_status(&client, 0, &mut buf, &mut caps);
    let (_, sb, _) = recv_status(&client, 1, &mut buf, &mut caps);
    assert_eq!(sa, RD_OK);
    assert_eq!(sb, RD_OK);
    assert_eq!(client.region_bytes(region_a), storage[..512].to_vec());
    assert_eq!(client.region_bytes(region_b), storage[4 * 512..5 * 512].to_vec());

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn window_one_enforced() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake, true, None);

    handshake(&client, 0);

    // A second request while the first is outstanding is rejected (window=1).
    client.send(0, 2, 0, &frame(RD_FLUSH), &[]).unwrap();
    assert!(client.send(0, 3, 0, &frame(RD_INFO), &[]).is_err());
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_OK);
    // Window reopened: next request works.
    client.send(0, 3, 0, &frame(RD_INFO), &[]).unwrap();
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 3);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn client_close_drops_endpoint() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_sim(fake.clone(), true, None);

    handshake(&client, 0);

    // Client dies with a request in flight: the endpoint's transient grants
    // are revoked at close, whichever side wins the race (reply-revoke or
    // close-revoke).
    let region = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_READ, 0, 1), &[grant]).unwrap();
    client.close(0, CLOSE_PEER, 0).unwrap();
    assert_eq!(fake.driver_grant_count(), 0);

    // A fresh injected endpoint still works (the driver survived).
    let ep = client.inject_posix_endpoint().unwrap();
    client.send(ep, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(ep, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_MSG);

    client.kill_driver(0);
    t.join().unwrap();
}

/// A backend that fails every I/O with a fixed `BackendErr` — proves the
/// server maps backend failures to RD_ERR_* codes (the real NVMe device
/// returns these from its status-code checks).
struct FailingBackend {
    err: BackendErr,
}

impl BlockBackend for FailingBackend {
    fn total_sectors(&self) -> u64 {
        8
    }

    fn read_only(&self) -> bool {
        false
    }

    fn read(
        &mut self,
        _lba: u64,
        _sectors: u32,
        _dst_addr: u64,
        _dst_len: usize,
    ) -> Result<(), BackendErr> {
        Err(self.err)
    }

    fn write(
        &mut self,
        _lba: u64,
        _sectors: u32,
        _src_addr: u64,
        _src_len: usize,
    ) -> Result<(), BackendErr> {
        Err(self.err)
    }

    fn flush(&mut self) -> Result<(), BackendErr> {
        Err(self.err)
    }

    fn map(&self) -> Option<MapGrant> {
        None
    }
}

#[test]
fn backend_errors_map_to_protocol_codes() {
    for (err, expect) in [
        (BackendErr::Io, RD_ERR_IO),
        (BackendErr::Busy, RD_ERR_BUSY),
        (BackendErr::NoMem, RD_ERR_NOMEM),
    ] {
        let (fake, client) = FakeKernel::new(storage_image(8), 1);
        let t = boot_with(fake, FailingBackend { err });

        handshake(&client, 0);

        // READ failure surfaces the mapped code.
        let region = client.new_region(vec![0u8; 512], R | W);
        let grant = SendCap {
            slot: region,
            offset: 0,
            len: 512,
            rights: W,
            flags: 0,
        };
        client.send(0, 2, 0, &rw_req(RD_READ, 0, 1), &[grant]).unwrap();
        let mut buf = [0u8; 64];
        let mut caps = [GrantedCap::default(); 8];
        let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
        assert_eq!(status, expect);

        // FLUSH too.
        client.send(0, 3, 0, &frame(RD_FLUSH), &[]).unwrap();
        let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
        assert_eq!(status, expect);

        client.kill_driver(0);
        t.join().unwrap();
    }
}
