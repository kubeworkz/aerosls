//! Integration tests for the network driver against the fake kernel.
//!
//! Each test boots the driver in its own thread and drives it from the
//! client end, then tears the driver down with `kill_driver`.

use aerosls_kernel_sim::{FakeClient, FakeKernel, DRIVER_CONSOLE};
use aerosls_network::endpoints::EndpointSet;
use aerosls_network::kapi::{GrantedCap, SendCap};
use aerosls_network::mock::MockNetwork;
use aerosls_network::server;
use aerosls_proto::*;

/// Boot the network driver in a thread, adopting initial endpoints.
fn boot_driver(fake: FakeKernel) -> std::thread::JoinHandle<()> {
    let mut eps = EndpointSet::new(DRIVER_CONSOLE);
    for h in fake.initial_chan_caps() {
        eps.adopt(h);
    }
    let mut net = MockNetwork::new();
    std::thread::spawn(move || {
        let _ = server::run(&fake, &mut eps, &mut net);
    })
}

fn frame(ty: u16) -> [u8; NetFrame::SIZE] {
    NetFrame::new(ty, false).encode()
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
    let f = NetFrame::parse(&buf[..rr.len]).unwrap();
    let (status, bytes) = parse_status_body(&buf[16..rr.len]).expect("status body");
    (f.ty, status, bytes)
}

// ── handshake tests ─────────────────────────────────────────────────────────

#[test]
fn boot_handshake() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake.clone());

    // Handshake: first message must be NET_INFO.
    client.send(0, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 1);
    let f = NetFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, NET_INFO);
    assert_eq!(f.flags & NET_FLAG_ERROR, 0);
    let (max_sockets, mtu, info_flags) = parse_net_info_body(&buf[16..rr.len]).unwrap();
    assert_eq!(max_sockets, 16);
    assert_eq!(mtu, 1500);
    assert_eq!(info_flags, 0);

    // NET_INFO again is valid (post-handshake).
    client.send(0, 2, 0, &frame(NET_INFO), &[]).unwrap();
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 2);

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn wrong_first_message_closes_endpoint() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake);

    // First message is NET_SOCKET, not NET_INFO → CLOSE_PROTO.
    let body = encode_net_socket_body(SOCK_STREAM, 0);
    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&body);
    client.send(0, 1, 0, &payload, &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.kind, CH_KIND_CLOSE, "expected a close event");
    let (reason, detail) = parse_close_body(&buf[..rr.len]).unwrap();
    assert_eq!(reason, CLOSE_PROTO);
    assert_eq!(detail, NET_ERR_PROTO as u32);

    client.kill_driver(0);
    t.join().unwrap();
}

// ── socket lifecycle tests ──────────────────────────────────────────────────

#[test]
fn create_tcp_socket() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake);

    // Handshake.
    client.send(0, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Create a TCP socket.
    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_STREAM, 0));
    client.send(0, 2, 0, &payload, &[]).unwrap();
    let (_, status, sock_id) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);
    assert_eq!(sock_id, 0); // first socket

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn create_udp_socket() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Create a UDP socket.
    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_DGRAM, 0));
    client.send(0, 2, 0, &payload, &[]).unwrap();
    let (_, status, sock_id) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);
    assert_eq!(sock_id, 0);

    client.kill_driver(0);
    t.join().unwrap();
}

// ── bind + listen + connect + accept + send/recv ────────────────────────────

fn sock_id_req(ty: u16, sock_id: u32, extra: &[u8]) -> Vec<u8> {
    let mut payload = vec![0u8; 16 + 4 + extra.len()];
    payload[..16].copy_from_slice(&frame(ty));
    payload[16..20].copy_from_slice(&sock_id.to_le_bytes());
    payload[20..].copy_from_slice(extra);
    payload
}

#[test]
fn stream_connect_send_recv() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake);

    // Handshake.
    client.send(0, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 1024];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Create server socket.
    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_STREAM, 0));
    client.send(0, 2, 0, &payload, &[]).unwrap();
    let (_, _, server_id) = recv_status(&client, 0, &mut buf, &mut caps);

    // Bind server to port 8080.
    let bind_payload = sock_id_req(NET_BIND, server_id as u32, &encode_sockaddr(0, 8080));
    client.send(0, 3, 0, &bind_payload, &[]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);

    // Listen.
    let listen_payload = sock_id_req(NET_LISTEN, server_id as u32, &[]);
    client.send(0, 4, 0, &listen_payload, &[]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);

    // Create client socket.
    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_STREAM, 0));
    client.send(0, 5, 0, &payload, &[]).unwrap();
    let (_, _, client_id) = recv_status(&client, 0, &mut buf, &mut caps);

    // Connect client to server.
    let connect_payload = sock_id_req(NET_CONNECT, client_id as u32, &encode_sockaddr(0, 8080));
    client.send(0, 6, 0, &connect_payload, &[]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);

    // Accept on server → get accepted socket.
    let accept_payload = sock_id_req(NET_ACCEPT, server_id as u32, &[]);
    client.send(0, 7, 0, &accept_payload, &[]).unwrap();
    let rr = client.recv(0, &mut buf, &mut caps).unwrap();
    let f = NetFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, NET_ACCEPT);
    assert!(!f.is_error());
    let (accepted_id, _, _) = parse_net_accept_body(&buf[16..rr.len]).unwrap();

    // Send "hello" from client to accepted.
    let data_region = client.new_region(b"hello".to_vec(), R);
    let grant = SendCap {
        slot: data_region,
        offset: 0,
        len: 5,
        rights: R,
        flags: 0,
    };
    let mut send_payload = vec![0u8; 20];
    send_payload[..16].copy_from_slice(&frame(NET_SEND));
    send_payload[16..20].copy_from_slice(&(client_id as u32).to_le_bytes());
    client.send(0, 8, 0, &send_payload, &[grant]).unwrap();
    let (_, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);
    assert_eq!(bytes, 5);

    // Recv on accepted socket (write into a W grant).
    let recv_region = client.new_region(vec![0u8; 64], R | W);
    let recv_grant = SendCap {
        slot: recv_region,
        offset: 0,
        len: 64,
        rights: W,
        flags: 0,
    };
    let recv_payload = sock_id_req(NET_RECV, accepted_id as u32, &[]);
    client.send(0, 9, 0, &recv_payload, &[recv_grant]).unwrap();
    let (_, status, bytes) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_OK);
    assert_eq!(bytes, 5);
    assert_eq!(&client.region_bytes(recv_region)[..5], b"hello");

    client.kill_driver(0);
    t.join().unwrap();
}

// ── error paths ─────────────────────────────────────────────────────────────

#[test]
fn bind_duplicate_address() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Create two sockets and bind both to port 80.
    for i in 0u32..2 {
        let mut payload = [0u8; 20];
        payload[..16].copy_from_slice(&frame(NET_SOCKET));
        payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_STREAM, 0));
        client.send(0, 2 + i, 0, &payload, &[]).unwrap();
        let (_, _, sid) = recv_status(&client, 0, &mut buf, &mut caps);

        let bind_payload = sock_id_req(NET_BIND, sid as u32, &encode_sockaddr(0, 80));
        client.send(0, 10 + i, 0, &bind_payload, &[]).unwrap();
        let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
        if i == 0 {
            assert_eq!(status, NET_OK);
        } else {
            assert_eq!(status, NET_ERR_ADDRINUSE);
        }
    }

    client.kill_driver(0);
    t.join().unwrap();
}

#[test]
fn send_requires_cap_grant() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 1);
    let t = boot_driver(fake);

    client.send(0, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let _ = client.recv(0, &mut buf, &mut caps).unwrap();

    // Create + connect (need a server first).
    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_STREAM, 0));
    client.send(0, 2, 0, &payload, &[]).unwrap();
    let (_, _, server_id) = recv_status(&client, 0, &mut buf, &mut caps);

    let bind_payload = sock_id_req(NET_BIND, server_id as u32, &encode_sockaddr(0, 8081));
    client.send(0, 3, 0, &bind_payload, &[]).unwrap();
    let _ = recv_status(&client, 0, &mut buf, &mut caps);

    let listen_payload = sock_id_req(NET_LISTEN, server_id as u32, &[]);
    client.send(0, 4, 0, &listen_payload, &[]).unwrap();
    let _ = recv_status(&client, 0, &mut buf, &mut caps);

    let mut payload = [0u8; 20];
    payload[..16].copy_from_slice(&frame(NET_SOCKET));
    payload[16..20].copy_from_slice(&encode_net_socket_body(SOCK_STREAM, 0));
    client.send(0, 5, 0, &payload, &[]).unwrap();
    let (_, _, client_id) = recv_status(&client, 0, &mut buf, &mut caps);

    let connect_payload = sock_id_req(NET_CONNECT, client_id as u32, &encode_sockaddr(0, 8081));
    client.send(0, 6, 0, &connect_payload, &[]).unwrap();
    let _ = recv_status(&client, 0, &mut buf, &mut caps);

    // SEND without a cap → NET_ERR_CAP.
    let send_payload = sock_id_req(NET_SEND, client_id as u32, &[]);
    client.send(0, 7, 0, &send_payload, &[]).unwrap();
    let (_, status, _) = recv_status(&client, 0, &mut buf, &mut caps);
    assert_eq!(status, NET_ERR_CAP);

    client.kill_driver(0);
    t.join().unwrap();
}

// TODO: fix close_socket_endpoint race — the close+poll sequence sometimes
// gets a truncated reply.  Debug and re-enable.
// #[test]
// fn close_socket_endpoint() { ... }

#[test]
fn new_channel_adoption() {
    let (fake, client) = FakeKernel::new(vec![0u8; 1024], 0);
    let t = boot_driver(fake.clone());
    assert_eq!(fake.initial_chan_caps().len(), 0);

    let ep = client.inject_posix_endpoint().unwrap();

    // Handshake + serve on the adopted endpoint.
    client.send(ep, 1, 0, &frame(NET_INFO), &[]).unwrap();
    let mut buf = [0u8; 128];
    let mut caps = [GrantedCap::default(); 8];
    let rr = client.recv(ep, &mut buf, &mut caps).unwrap();
    assert_eq!(rr.tag, 1);
    let f = NetFrame::parse(&buf[..rr.len]).unwrap();
    assert_eq!(f.ty, NET_INFO);

    client.kill_driver(0);
    t.join().unwrap();
}
