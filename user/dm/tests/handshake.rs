//! Device Manager integration tests — the registry handshake end to end
//! against the kernel-sim fake, driving the DM's REAL server core the way
//! init's real handshake drives it (transport spec §3).
//!
//! Model: `FakeKernel` is the DM's view of the kernel (the server runs
//! over it via the `Kernel` trait); `FakeClient` is INIT's view — it sends
//! the registry with the count payload + the read-only MEM cap grant,
//! exactly like `user/init/src/demo.rs::send_registry`, and receives the
//! devices-ready reply.
//!
//! FakeKernel::new(storage, 1) lays out the driver table:
//!   0 = budget MEM, 1 = storage MEM, 2 = console CHAN (kernel-held
//!   peer), 3 = img MEM, 4 = messenger CHAN (client end = handle 0).

use aerosls_dm::server::{
    DmError, DmOutcome, DmServer, DriverOutcome, MSG_DEVICE_REGISTRY, MSG_DEVICES_READY,
};
use aerosls_kernel_sim::{FakeClient, FakeKernel};
use aerosls_proto::devreg::{DevRegError, DeviceEntry, DRIVER_MANIFEST_LEN};
use aerosls_proto::kabi::{GrantedCap, Kernel, SendCap};
use aerosls_proto::{CH_KIND_NONE, CLOSE_PEER, R, W};

const CONSOLE: u32 = 2;
const MESSENGER: u32 = 4;
const CLIENT_END: u32 = 0;

fn dm_pair() -> (DmServer<FakeKernel>, FakeClient) {
    let (dk, client) = FakeKernel::new(Vec::new(), 1);
    // The sim models a channel as ONE driver handle for both ends; the
    // real kernel mints separate CHAN_R and CHAN_W slots.
    (DmServer::new(dk, CONSOLE, MESSENGER, MESSENGER), client)
}

fn entry(driver_manifest: &str, class: u8, subclass: u8) -> DeviceEntry {
    let mut manifest = [0u8; DRIVER_MANIFEST_LEN];
    manifest[..driver_manifest.len()].copy_from_slice(driver_manifest.as_bytes());
    DeviceEntry {
        class_code: class,
        subclass,
        vendor_id: 0x144D,
        device_id: 0xA808,
        pci_slot: 0,
        pci_bus: 0,
        bar0_phys: 0xFEBF_0000,
        irq_line: 11,
        is_64bit_bar: true,
        driver_manifest: manifest,
    }
}

fn registry_blob(entries: &[DeviceEntry]) -> Vec<u8> {
    let mut buf = Vec::with_capacity(4 + entries.len() * 64);
    buf.extend_from_slice(&(entries.len() as u32).to_le_bytes());
    for e in entries {
        buf.push(e.class_code);
        buf.push(e.subclass);
        buf.extend_from_slice(&e.vendor_id.to_le_bytes());
        buf.extend_from_slice(&e.device_id.to_le_bytes());
        buf.push(e.pci_slot);
        buf.push(e.pci_bus);
        buf.extend_from_slice(&e.bar0_phys.to_le_bytes());
        buf.push(e.irq_line);
        buf.push(e.is_64bit_bar as u8);
        buf.push(0);
        buf.push(0);
        buf.extend_from_slice(&e.driver_manifest);
    }
    buf
}

/// Send the registry exactly like init's `send_registry`: MSG_DEVICE_REGISTRY
/// with the count (u32 LE) as the payload and the registry table as a
/// read-only MEM cap grant.
fn send_registry(client: &FakeClient, blob: &[u8], rights: u8) {
    send_registry_caps(client, blob, rights, &[])
}

/// `send_registry` plus the e1000 driver-image grant init attaches second
/// (a fake image region; the DM only reads its base/size, never the bytes).
fn send_registry_caps(client: &FakeClient, blob: &[u8], rights: u8, image: &[u8]) {
    let handle = client.new_region(blob.to_vec(), rights);
    let mut caps = vec![SendCap {
        slot: handle,
        offset: 0,
        len: blob.len() as u32,
        rights,
        flags: 0,
    }];
    if !image.is_empty() {
        let ih = client.new_region(image.to_vec(), rights);
        caps.push(SendCap {
            slot: ih,
            offset: 0,
            len: image.len() as u32,
            rights,
            flags: 0,
        });
    }
    client
        .send(
            CLIENT_END,
            MSG_DEVICE_REGISTRY,
            0,
            &(blob.len() as u32).to_le_bytes(),
            &caps,
        )
        .expect("registry send");
}

#[test]
fn registry_handshake_served_and_replied() {
    let (server, client) = dm_pair();
    let blob = registry_blob(&[
        entry("drv.nvme.0", 0x01, 0x08),
        entry("drv.e1000.0", 0x02, 0x00),
    ]);
    send_registry(&client, &blob, R);

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Ok(DmOutcome::RegistryServed {
            devices: 2,
            // drv.e1000.0 is registered, but this message carried no
            // driver-image grant — no spawn.
            driver: DriverOutcome::NoImageGrant
        })
    );

    // init receives the devices-ready reply: empty payload, no caps.
    let mut rbuf = [0u8; 64];
    let r = client
        .recv(CLIENT_END, &mut rbuf, &mut [GrantedCap::default(); 4])
        .expect("reply recv");
    assert_eq!(r.tag, MSG_DEVICES_READY);
    assert_eq!(r.len, 0);
    assert_eq!(r.n_caps, 0);
}

#[test]
fn handed_off_nic_attempts_the_e1000_spawn() {
    // The full handoff config: registry carries drv.e1000.0 (a role-less
    // NIC) AND the message carries the driver-image grant. The kernel-sim
    // fake cannot spawn (its create_sidecar is the trait default, which
    // refuses), so the DM must attempt the spawn and surface the failure as
    // a non-fatal SpawnError — the handshake itself still succeeds.
    let (server, client) = dm_pair();
    let blob = registry_blob(&[entry("drv.e1000.0", 0x02, 0x00)]);
    send_registry_caps(&client, &blob, R, &[0xEEu8; 0x2000]);

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    match server.serve_one(&mut buf, &mut slots) {
        Ok(DmOutcome::RegistryServed {
            devices: 1,
            driver: DriverOutcome::SpawnError(_),
        }) => {}
        other => panic!("expected a spawn attempt, got {other:?}"),
    }

    // The reply still lands: a refused spawn never blocks the handshake.
    let mut rbuf = [0u8; 64];
    let r = client
        .recv(CLIENT_END, &mut rbuf, &mut [GrantedCap::default(); 4])
        .expect("reply recv");
    assert_eq!(r.tag, MSG_DEVICES_READY);
}

#[test]
fn no_handed_off_nic_spawns_nothing() {
    // A registry with only kernel-owned devices (NVMe marked, e1000
    // absent or driverless) spawns nothing.
    let (server, client) = dm_pair();
    let blob = registry_blob(&[entry("drv.nvme.0", 0x01, 0x08)]);
    send_registry_caps(&client, &blob, R, &[0xEEu8; 0x2000]);

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Ok(DmOutcome::RegistryServed {
            devices: 1,
            driver: DriverOutcome::None
        })
    );
}

#[test]
fn run_loop_serves_registry_then_exits_on_close() {
    let (server, client) = dm_pair();
    let blob = registry_blob(&[entry("drv.nvme.0", 0x01, 0x08)]);
    send_registry(&client, &blob, R);

    // The DM runs its real event loop in the background: blocking wait
    // (park) on the messenger, serve the registry, reply, park again.
    let handle = std::thread::spawn(move || {
        let mut buf = [0u8; 256];
        server.run(&mut buf)
    });

    // init receives the devices-ready reply (the blocking recv is woken
    // by the DM's send).
    let mut rbuf = [0u8; 64];
    let r = client
        .recv(CLIENT_END, &mut rbuf, &mut [GrantedCap::default(); 4])
        .expect("reply recv");
    assert_eq!(r.tag, MSG_DEVICES_READY);

    // init dies: the close wakes the DM's parked wait and the loop exits.
    client.close(CLIENT_END, CLOSE_PEER, 7).unwrap();
    assert_eq!(handle.join().expect("dm thread"), Ok(()));
}

#[test]
fn payload_too_short_refused() {
    let (server, client) = dm_pair();
    let blob = registry_blob(&[]);
    let handle = client.new_region(blob.clone(), R);
    client
        .send(
            CLIENT_END,
            MSG_DEVICE_REGISTRY,
            0,
            &[0u8; 2], // shorter than the 4-byte count
            &[SendCap {
                slot: handle,
                offset: 0,
                len: blob.len() as u32,
                rights: R,
                flags: 0,
            }],
        )
        .unwrap();

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Err(DmError::PayloadTooShort)
    );
    // No reply was sent (a broken init must not wedge the DM).
    assert_eq!(Kernel::poll(&client, CLIENT_END), Ok(CH_KIND_NONE));
}

#[test]
fn missing_registry_cap_refused() {
    let (server, client) = dm_pair();
    client
        .send(CLIENT_END, MSG_DEVICE_REGISTRY, 0, &1u32.to_le_bytes(), &[])
        .unwrap();

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Err(DmError::MissingRegistryCap)
    );
    assert_eq!(Kernel::poll(&client, CLIENT_END), Ok(CH_KIND_NONE));
}

#[test]
fn unreadable_registry_cap_refused() {
    let (server, client) = dm_pair();
    let blob = registry_blob(&[entry("drv.nvme.0", 0x01, 0x08)]);
    // A W-only grant: the DM must refuse to read through it.
    send_registry(&client, &blob, W);

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Err(DmError::RegistryCapNotReadable)
    );
    assert_eq!(Kernel::poll(&client, CLIENT_END), Ok(CH_KIND_NONE));
}

#[test]
fn truncated_registry_table_refused() {
    let (server, client) = dm_pair();
    let mut blob = registry_blob(&[entry("drv.nvme.0", 0x01, 0x08)]);
    // The table claims 2 entries but only holds 1.
    blob[0..4].copy_from_slice(&2u32.to_le_bytes());
    send_registry(&client, &blob, R);

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Err(DmError::BadRegistry(DevRegError::Truncated))
    );
    assert_eq!(Kernel::poll(&client, CLIENT_END), Ok(CH_KIND_NONE));
}

#[test]
fn unknown_message_acknowledged_without_reply() {
    let (server, client) = dm_pair();
    client.send(CLIENT_END, 0x99, 0, b"hi", &[]).unwrap();

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Ok(DmOutcome::Acknowledged { tag: 0x99 })
    );
    assert_eq!(Kernel::poll(&client, CLIENT_END), Ok(CH_KIND_NONE));
}

#[test]
fn init_death_surfaces_as_close() {
    let (server, client) = dm_pair();
    client.close(CLIENT_END, CLOSE_PEER, 42).unwrap();

    let mut buf = [0u8; 256];
    let mut slots = [GrantedCap::default(); 4];
    assert_eq!(
        server.serve_one(&mut buf, &mut slots),
        Ok(DmOutcome::Closed(CLOSE_PEER, 42))
    );
}
