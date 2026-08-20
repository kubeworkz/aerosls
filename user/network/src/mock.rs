//! In-memory mock network backend for testing.
//!
//! Simulates a loopback network with:
//! - A pool of socket slots (fixed-size array, max 16 sockets).
//! - TCP-like stream sockets backed by in-memory ring buffers.
//! - UDP-like datagram sockets backed by in-memory queues.
//! - A pre-wired loopback path: a connect to `127.0.0.1` creates a
//!   connected pair; a listen + accept creates a new connected pair.
//! - A configurable "remote" service: register a handler for a port
//!   that responds to received data (echo, drop, custom).
//!
//! The mock never blocks; all operations complete synchronously.

extern crate alloc;

use alloc::collections::VecDeque;
use alloc::vec::Vec;
use aerosls_proto::*;

/// Maximum number of sockets the mock supports.
pub const MAX_SOCKETS: usize = 16;

/// Maximum bytes per datagram (safe default; mirrors UDP MTU).
pub const MAX_DGRAM: usize = 1500;

/// Maximum stream ring buffer size (4 KiB per direction; heap-backed
/// in production, stack-safe for tests).
pub const STREAM_BUF: usize = 4096;

/// Socket type (matching NET_* protocol constants).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SockType {
    Stream,
    Datagram,
}

/// Socket state machine.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SockState {
    Created,
    Bound,
    Listening,
    Connected,
    HalfClosed,
    Closed,
}

/// A bound address: `(ip, port)`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Addr {
    pub ip: u32,
    pub port: u16,
}

/// The stream ring buffer: a simple circular byte buffer.
#[derive(Clone, Debug)]
pub struct RingBuf {
    data: [u8; STREAM_BUF],
    head: usize,
    tail: usize,
    full: bool,
}

impl RingBuf {
    fn new() -> Self {
        RingBuf {
            data: [0u8; STREAM_BUF],
            head: 0,
            tail: 0,
            full: false,
        }
    }

    fn len(&self) -> usize {
        if self.full {
            STREAM_BUF
        } else if self.head >= self.tail {
            self.head - self.tail
        } else {
            STREAM_BUF - self.tail + self.head
        }
    }

    fn space(&self) -> usize {
        STREAM_BUF - self.len()
    }

    fn push(&mut self, buf: &[u8]) -> usize {
        let n = buf.len().min(self.space());
        for &b in &buf[..n] {
            self.data[self.head] = b;
            self.head = (self.head + 1) % STREAM_BUF;
        }
        if n > 0 && self.head == self.tail {
            self.full = true;
        }
        n
    }

    fn pop(&mut self, buf: &mut [u8]) -> usize {
        if self.len() == 0 {
            return 0;
        }
        let n = buf.len().min(self.len());
        for b in &mut buf[..n] {
            *b = self.data[self.tail];
            self.tail = (self.tail + 1) % STREAM_BUF;
        }
        self.full = false;
        n
    }

    fn clear(&mut self) {
        self.head = 0;
        self.tail = 0;
        self.full = false;
    }
}

/// A single socket slot in the mock.
#[derive(Clone, Debug)]
pub struct MockSocket {
    pub id: u32,
    pub sock_type: SockType,
    pub state: SockState,
    pub local: Addr,
    pub remote: Addr,
    /// For stream sockets: the partner socket's id (0 if unconnected).
    pub peer_id: u32,
    /// Stream read/write buffers (bidirectional: [0] = recv buf, [1] = send buf).
    pub stream_bufs: [RingBuf; 2],
    /// Datagram receive queue.
    pub dgram_rx: VecDeque<Vec<u8>>,
    /// For listening sockets: queue of pending accept connections.
    pub accept_queue: VecDeque<u32>,
    /// Peer socket's send buffer is closed (EOF).
    pub peer_closed: bool,
}

/// The mock network backend.
#[derive(Clone, Debug)]
pub struct MockNetwork {
    pub sockets: [Option<MockSocket>; MAX_SOCKETS],
    /// Pre-registered service ports.
    services: VecDeque<(u16, ServiceKind)>,
    /// Global accept counter (for auto-assigning port numbers).
    next_port: u16,
}

/// Service kind: what happens when data arrives on a service port.
#[derive(Clone, Debug)]
pub enum ServiceKind {
    Echo,
    Drop,
}

impl MockNetwork {
    pub fn new() -> Self {
        MockNetwork {
            sockets: Default::default(),
            services: VecDeque::new(),
            next_port: 10000,
        }
    }

    /// Register a service on a port (for auto-accept on connect).
    pub fn register_service(&mut self, port: u16, kind: ServiceKind) {
        self.services.push_back((port, kind));
    }

    /// Allocate a new socket slot. Returns its id or None if full.
    fn alloc_slot(&mut self) -> Option<u32> {
        for (i, slot) in self.sockets.iter_mut().enumerate() {
            if slot.is_none() {
                let id = i as u32;
                *slot = Some(MockSocket {
                    id,
                    sock_type: SockType::Stream,
                    state: SockState::Created,
                    local: Addr::default(),
                    remote: Addr::default(),
                    peer_id: 0,
                    stream_bufs: [RingBuf::new(), RingBuf::new()],
                    dgram_rx: VecDeque::new(),
                    accept_queue: VecDeque::new(),
                    peer_closed: false,
                });
                return Some(id);
            }
        }
        None
    }

    fn get_mut(&mut self, id: u32) -> Option<&mut MockSocket> {
        self.sockets.get_mut(id as usize)?.as_mut()
    }

    fn get(&self, id: u32) -> Option<&MockSocket> {
        self.sockets.get(id as usize)?.as_ref()
    }

    /// Create a new socket.
    pub fn socket(&mut self, sock_type: SockType) -> Result<u32, u16> {
        let id = self.alloc_slot().ok_or(NET_ERR_NOMEM)?;
        let sock = self.get_mut(id).unwrap();
        sock.sock_type = match sock_type {
            SockType::Stream => SockType::Stream,
            SockType::Datagram => SockType::Datagram,
        };
        Ok(id)
    }

    /// Bind a socket to a local address.
    pub fn bind(&mut self, id: u32, addr: Addr) -> Result<(), u16> {
        for slot in self.sockets.iter().flatten() {
            if slot.id != id && slot.local == addr && slot.state != SockState::Closed {
                return Err(NET_ERR_ADDRINUSE);
            }
        }
        let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
        sock.local = addr;
        sock.state = SockState::Bound;
        Ok(())
    }

    /// Connect to a remote address.
    pub fn connect(&mut self, id: u32, remote: Addr) -> Result<(), u16> {
        // Check if socket needs auto-bind without holding a mutable borrow.
        let needs_bind = self.get(id).ok_or(NET_ERR_INVAL)?.state == SockState::Created;
        if needs_bind {
            let port = self.next_port;
            self.next_port += 1;
            let sock = self.get_mut(id).unwrap();
            sock.local = Addr { ip: 0x7F000001, port };
            sock.state = SockState::Bound;
        }

        let (local, sock_type) = {
            let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
            sock.remote = remote;
            (sock.local, sock.sock_type)
        };

        match sock_type {
            SockType::Stream => {
                // Find a listening socket on the remote port
                let server_id = self.find_listener(remote.port)
                    .ok_or(NET_ERR_CONNRESET)?;

                let accepted_id = self.alloc_slot().ok_or(NET_ERR_NOMEM)?;
                {
                    let accepted = self.get_mut(accepted_id).unwrap();
                    accepted.sock_type = SockType::Stream;
                    accepted.state = SockState::Connected;
                    accepted.local = remote;
                    accepted.remote = local;
                }

                self.get_mut(id).unwrap().state = SockState::Connected;
                self.get_mut(id).unwrap().peer_id = accepted_id;
                self.get_mut(accepted_id).unwrap().peer_id = id;

                self.get_mut(server_id).unwrap().accept_queue.push_back(accepted_id);

                Ok(())
            }
            SockType::Datagram => {
                // Find the target socket by port and link peers.
                let target = self.sockets.iter().flatten()
                    .find(|s| s.local.port == remote.port && s.sock_type == SockType::Datagram && s.id != id)
                    .map(|s| s.id);
                if let Some(target_id) = target {
                    self.get_mut(id).unwrap().peer_id = target_id;
                    self.get_mut(id).unwrap().state = SockState::Connected;
                    // The target also becomes connected for recv symmetry.
                    self.get_mut(target_id).unwrap().peer_id = id;
                    self.get_mut(target_id).unwrap().state = SockState::Connected;
                } else {
                    self.get_mut(id).unwrap().state = SockState::Connected;
                }
                Ok(())
            }
        }
    }

    /// Find a listening socket bound to the given port.
    fn find_listener(&self, port: u16) -> Option<u32> {
        self.sockets.iter().flatten()
            .find(|s| s.state == SockState::Listening && s.local.port == port)
            .map(|s| s.id)
    }

    /// Listen for connections.
    pub fn listen(&mut self, id: u32) -> Result<(), u16> {
        let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
        if sock.state != SockState::Bound {
            return Err(NET_ERR_INVAL);
        }
        sock.state = SockState::Listening;
        Ok(())
    }

    /// Accept a pending connection.
    pub fn accept(&mut self, id: u32) -> Result<(u32, Addr), u16> {
        let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
        if sock.state != SockState::Listening {
            return Err(NET_ERR_INVAL);
        }
        let peer_id = sock.accept_queue.pop_front()
            .ok_or(NET_ERR_WOULDBLOCK)?;
        let remote_addr = self.get(peer_id).unwrap().remote;
        Ok((peer_id, remote_addr))
    }

    /// Send data on a connected socket.
    pub fn send(&mut self, id: u32, buf: &[u8]) -> Result<usize, u16> {
        let (peer_id, sock_type) = {
            let sock = self.get(id).ok_or(NET_ERR_INVAL)?;
            if sock.state != SockState::Connected {
                return Err(NET_ERR_NOTCONN);
            }
            if sock.peer_id == 0 {
                return Err(NET_ERR_NOTCONN);
            }
            (sock.peer_id, sock.sock_type)
        };

        match sock_type {
            SockType::Stream => {
                let peer = self.get_mut(peer_id).ok_or(NET_ERR_CONNRESET)?;
                if peer.state == SockState::Closed {
                    return Err(NET_ERR_CONNRESET);
                }
                let n = peer.stream_bufs[0].push(buf);
                Ok(n)
            }
            SockType::Datagram => {
                let peer = self.get_mut(peer_id).ok_or(NET_ERR_CONNRESET)?;
                if peer.state == SockState::Closed {
                    return Err(NET_ERR_CONNRESET);
                }
                if buf.len() > MAX_DGRAM {
                    return Err(NET_ERR_MSGSIZE);
                }
                peer.dgram_rx.push_back(buf.to_vec());
                Ok(buf.len())
            }
        }
    }

    /// Receive data from a socket.
    pub fn recv(&mut self, id: u32, buf: &mut [u8]) -> Result<usize, u16> {
        let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
        if sock.state != SockState::Connected && sock.state != SockState::HalfClosed {
            return Err(NET_ERR_NOTCONN);
        }

        match sock.sock_type {
            SockType::Stream => {
                let n = sock.stream_bufs[0].pop(buf);
                if n > 0 {
                    return Ok(n);
                }
                if sock.peer_closed {
                    return Ok(0); // EOF
                }
                Err(NET_ERR_WOULDBLOCK)
            }
            SockType::Datagram => {
                if let Some(dgram) = sock.dgram_rx.pop_front() {
                    let n = dgram.len().min(buf.len());
                    buf[..n].copy_from_slice(&dgram[..n]);
                    Ok(n)
                } else {
                    Err(NET_ERR_WOULDBLOCK)
                }
            }
        }
    }

    /// Shutdown a socket.
    pub fn shutdown(&mut self, id: u32, how: u8) -> Result<(), u16> {
        // Copy the fields we need before the split borrow.
        let (peer_id, state) = {
            let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
            (sock.peer_id, sock.state)
        };
        if (how == 0 || how == 2) && peer_id > 0 {
            if let Some(peer) = self.get_mut(peer_id) {
                peer.peer_closed = true;
            }
        }
        if how == 1 || how == 2 && state == SockState::Connected {
            if let Some(sock) = self.get_mut(id) {
                sock.state = SockState::HalfClosed;
            }
            if peer_id > 0 {
                if let Some(peer) = self.get_mut(peer_id) {
                    peer.peer_closed = true;
                }
            }
        }
        Ok(())
    }

    /// Close a socket.
    pub fn close_socket(&mut self, id: u32) -> Result<(), u16> {
        let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
        let peer_id = sock.peer_id;
        sock.state = SockState::Closed;
        sock.stream_bufs[0].clear();
        sock.stream_bufs[1].clear();
        sock.dgram_rx.clear();
        if peer_id > 0 {
            if let Some(peer) = self.get_mut(peer_id) {
                peer.peer_closed = true;
                peer.peer_id = 0;
            }
        }
        Ok(())
    }

    /// Poll a socket for readability/writability.
    pub fn poll(&self, id: u32) -> Result<u16, u16> {
        let sock = self.get(id).ok_or(NET_ERR_INVAL)?;
        let mut events: u16 = 0;
        match sock.sock_type {
            SockType::Stream => {
                if sock.stream_bufs[0].len() > 0 || sock.peer_closed {
                    events |= 0x001; // POLLIN
                }
                if sock.stream_bufs[1].space() > 0 && !sock.peer_closed {
                    events |= 0x004; // POLLOUT
                }
            }
            SockType::Datagram => {
                if !sock.dgram_rx.is_empty() {
                    events |= 0x001; // POLLIN
                }
                events |= 0x004; // POLLOUT
            }
        }
        Ok(events)
    }

    /// Deliver data to a socket (for testing: simulate incoming data).
    pub fn inject_data(&mut self, id: u32, buf: &[u8]) -> Result<usize, u16> {
        let sock = self.get_mut(id).ok_or(NET_ERR_INVAL)?;
        match sock.sock_type {
            SockType::Stream => {
                let n = sock.stream_bufs[0].push(buf);
                Ok(n)
            }
            SockType::Datagram => {
                if buf.len() > MAX_DGRAM {
                    return Err(NET_ERR_MSGSIZE);
                }
                sock.dgram_rx.push_back(buf.to_vec());
                Ok(buf.len())
            }
        }
    }
}

impl Default for MockNetwork {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stream_connect_and_send() {
        let mut net = MockNetwork::new();
        let server = net.socket(SockType::Stream).unwrap();
        net.bind(server, Addr { ip: 0, port: 80 }).unwrap();
        net.listen(server).unwrap();

        let client = net.socket(SockType::Stream).unwrap();
        net.connect(client, Addr { ip: 0x7F000001, port: 80 }).unwrap();

        let (accepted, _) = net.accept(server).unwrap();

        net.send(client, b"hello").unwrap();
        let mut buf = [0u8; 64];
        let n = net.recv(accepted, &mut buf).unwrap();
        assert_eq!(&buf[..n], b"hello");

        net.send(accepted, b"world").unwrap();
        let n = net.recv(client, &mut buf).unwrap();
        assert_eq!(&buf[..n], b"world");
    }

    #[test]
    fn udp_send_recv() {
        let mut net = MockNetwork::new();
        let a = net.socket(SockType::Datagram).unwrap();
        net.bind(a, Addr { ip: 0, port: 1000 }).unwrap();
        let b = net.socket(SockType::Datagram).unwrap();
        net.bind(b, Addr { ip: 0, port: 2000 }).unwrap();

        net.connect(a, Addr { ip: 0, port: 2000 }).unwrap();
        net.send(a, b"ping").unwrap();

        let mut buf = [0u8; 64];
        let n = net.recv(b, &mut buf).unwrap();
        assert_eq!(&buf[..n], b"ping");
    }

    #[test]
    fn close_propagates() {
        let mut net = MockNetwork::new();
        let server = net.socket(SockType::Stream).unwrap();
        net.bind(server, Addr { ip: 0, port: 90 }).unwrap();
        net.listen(server).unwrap();

        let client = net.socket(SockType::Stream).unwrap();
        net.connect(client, Addr { ip: 0x7F000001, port: 90 }).unwrap();

        let (accepted, _) = net.accept(server).unwrap();
        net.close_socket(client).unwrap();

        let mut buf = [0u8; 64];
        assert_eq!(net.recv(accepted, &mut buf).unwrap(), 0); // EOF
    }

    #[test]
    fn poll_reports_events() {
        let mut net = MockNetwork::new();
        let server = net.socket(SockType::Stream).unwrap();
        net.bind(server, Addr { ip: 0, port: 81 }).unwrap();
        net.listen(server).unwrap();

        let client = net.socket(SockType::Stream).unwrap();
        net.connect(client, Addr { ip: 0x7F000001, port: 81 }).unwrap();

        let (accepted, _) = net.accept(server).unwrap();

        assert_eq!(net.poll(client).unwrap(), 0x004);

        net.send(accepted, b"data").unwrap();
        let events = net.poll(client).unwrap();
        assert!(events & 0x001 != 0);
    }
}
