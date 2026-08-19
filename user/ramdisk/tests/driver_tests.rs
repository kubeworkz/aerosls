//! Integration tests for the ramdisk driver against the fake kernel
//! (implementation plan §9). Each test boots the driver in its own thread
//! and drives it from the client end, then tears the driver down with
//! `kill_driver` (which makes the driver's wait return and the thread join).

use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE, DRIVER_STORAGE};
use aerosls_proto::*;
use aerosls_ramdisk::endpoints::EndpointSet;
use aerosls_ramdisk::kapi::{GrantedCap, SendCap};
use aerosls_ramdisk::server::{self, Device};

/// Deterministic, non-trivial storage content: blocks of 512 bytes.
fn storage_image(blocks: usize) -> Vec<u8> {
    let n = blocks * BLOCK_SIZE as usize;
    (0..n).map(|i| (i % 251) as u8).collect()
}

/// Boot the driver the way `rust_entry` would for the initial-table case:
/// adopt every initial CHAN cap except console, then run the server loop.
fn boot_driver(fake: FakeKernel) -> std::thread::JoinHandle<()> {
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

#[test]
fn boot_handshake_then_read() {
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_driver(fake.clone());

    // Handshake: first message must be RD_INFO.
    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 1);
    let f = RdFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, RD_INFO);
    assert_eq!(f.flags & RD_FLAG_ERROR, 0);
    let (bs, blocks, info_flags) = parse_info_body(&buf[16..rr.len]).unwrap();
    assert_eq!(bs, BLOCK_SIZE);
    assert_eq!(blocks, 8);
    assert_eq!(info_flags & 1, 1, "storage must report read-only");

    // RD_INFO again is a valid (post-handshake) request.
    client.send(0, 2, 0, &frame(RD_INFO), &[]).unwrap();
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 2);

    // RD_READ with a proper W grant.
    let region = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 3, 0, &rw_req(RD_READ, 0, 1), &[grant]).unwrap();
    let (ty, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_READ);
    assert_eq!(status, RD_OK);
    assert_eq!(bytes, 512);
    assert_eq!(client.region_bytes(region), storage[..512].to_vec());
    // The transient grant was auto-revoked at the reply.
    assert_eq!(fake.driver_grant_count(), 0);

    // Read a later block.
    let region2 = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region2,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 4, 0, &rw_req(RD_READ, 3, 1), &[grant]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_OK);
    assert_eq!(
        client.region_bytes(region2),
        storage[3 * 512..4 * 512].to_vec()
    );

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn wrong_first_message_closes_endpoint() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_driver(fake);

    // First message is RD_READ, not RD_INFO → the driver closes with
    // CLOSE_PROTO / RD_ERR_PROTO (plan §5.2).
    client.send(0, 1, 0, &rw_req(RD_READ, 0, 1), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_CLOSE, "expected a close event");
    let (reason, detail) = parse_close_body(&buf[..rr.len]).unwrap();
    assert_eq!(reason, CLOSE_PROTO);
    assert_eq!(detail, RD_ERR_PROTO as u32);

    // The channel is now dead from the client's perspective too.
    let region = client.new_region(vec![0u8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    assert!(client.send(0, 2, 0, &rw_req(RD_READ, 0, 1), &[grant]).is_err());

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_requires_writable_grant() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_driver(fake.clone());

    // Handshake first.
    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

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
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

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
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_ERR_CAP);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn read_range_enforced() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

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
fn write_rejected_on_read_only_storage() {
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    let region = client.new_region(vec![0xABu8; 512], R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: R, // write buffers are granted R-only
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_WRITE, 0, 1), &[grant]).unwrap();
    let (ty, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_WRITE);
    assert_eq!(status, RD_ERR_RO);
    // Storage untouched.
    assert_eq!(client.storage_bytes(), storage);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn flush_ok() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    client.send(0, 2, 0, &frame(RD_FLUSH), &[]).unwrap();
    let (ty, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(ty, RD_FLUSH);
    assert_eq!(status, RD_OK);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn map_returns_persist_grant() {
    let storage = storage_image(8);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    client.send(0, 2, 0, &frame(RD_MAP), &[]).unwrap();
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    let f = RdFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, RD_MAP);
    let (status, _) = parse_status_body(&buf[16..rr.len]).unwrap();
    assert_eq!(status, RD_OK);
    assert_eq!(rr.n_caps, 1, "RD_MAP reply must carry the persist grant");

    let g = caps[0];
    let ci = client.cap_info(g.handle).unwrap();
    assert_eq!(ci.ty, 1, "grant must be a MEM cap");
    assert_eq!(ci.rights, R as u16, "grant must be read-only (storage is read-only)");
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
fn map_grant_dies_with_driver() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    client.send(0, 2, 0, &frame(RD_MAP), &[]).unwrap();
    let _rr = client.recv(0, &mut buf, &mut caps).unwrap();
    let g = caps[0];
    assert!(client.cap_info(g.handle).is_ok());

    // Driver death: the kernel revokes the driver's table, which deep-revokes
    // the client's grant through its lineage (plan §8.1, cap spec §3.4).
    client.kill_driver(7);
    assert_eq!(client.cap_info(g.handle), Err(aerosls_ramdisk::kapi::ERR_REVOKED));

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
    // Boot style: no initial POSIX endpoint; it arrives as NEW_CHANNEL on
    // the console channel after the driver starts (transport spec §2.2).
    let (fake, client) = FakeKernel::new(storage_image(8), 0);
    let t = boot_driver(fake.clone());
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
    let (fake, client) = FakeKernel::new(storage.clone(), 2);
    let t = boot_driver(fake);

    // Two clients (client handles 0 and 1), independent windows.
    // Handshake both endpoints first (first message must be RD_INFO).
    client.send(0, 0, 0, &frame(RD_INFO), &[]).unwrap();
    client.send(1, 0, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();
    let _ = client.recv(1, &mut buf, &mut caps).unwrap();

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
fn aliasing_grant_uses_memmove_semantics() {
    let storage = storage_image(16);
    let (fake, client) = FakeKernel::new(storage.clone(), 1);
    let t = boot_driver(fake.clone());

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Backdoor: a client region that aliases the storage region (shared
    // address space; the layout is not secret). Reading block 2 into it must
    // not corrupt anything — memmove semantics.
    let (sbase, slen) = fake.storage_info();
    let region = client.new_region_at(sbase, slen, R | W);
    let grant = SendCap {
        slot: region,
        offset: 0,
        len: 512,
        rights: W,
        flags: 0,
    };
    client.send(0, 2, 0, &rw_req(RD_READ, 2, 1), &[grant]).unwrap();
    let (_, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, RD_OK);
    assert_eq!(bytes, 512);

    // storage[0..512] now holds the (old) storage[1024..1536]; the source
    // range itself is unchanged.
    let st = client.storage_bytes();
    assert_eq!(&st[..512], &storage[1024..1536]);
    assert_eq!(&st[1024..1536], &storage[1024..1536]);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn window_one_enforced() {
    let (fake, client) = FakeKernel::new(storage_image(8), 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // A second request while the first is outstanding is rejected (window=1,
    // capability-layer spec §4.3).
    client.send(0, 2, 0, &frame(RD_FLUSH), &[]).unwrap();
    assert!(client.send(0, 3, 0, &frame(RD_INFO), &[]).is_err());
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
    let t = boot_driver(fake.clone());

    client.send(0, 1, 0, &frame(RD_INFO), &[]).unwrap();
    let mut buf = [0u8; 64];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Client dies with a request in flight: the endpoint's transient grants
    // are revoked at close, whichever side wins the race (reply-revoke or
    // close-revoke) — capability-layer spec §4.3 rule 5.
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
    let rr = client.recv(ep, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_MSG);

    client.kill_driver(0);
    t.join().unwrap();
}
