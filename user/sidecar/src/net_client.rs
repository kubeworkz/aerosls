//! NET_* protocol client — the POSIX sidecar's network driver RPC layer.
//!
//! Mirrors `aerosls_blockcache::BlockCache` structurally: uses
//! `KWrap<K>` / `AWrap<A>` (Arc-shared kernel + allocator) so two
//! components can share the same `K` instance. Owns a channel endpoint
//! to the network driver sidecar and provides a typed API for the
//! NET_* wire protocol.
//!
//! The client is endpoint-agnostic: any handle adopted from the network
//! driver (via the manifest or `NEW_CHANNEL`) can be used — the first
//! message must be `NET_INFO` (handshake gate, matching the driver's
//! `AwaitingHandshake` state).
//!
//! Socket IDs are sidecar-local integers returned by `NET_SOCKET`; the
//! client tracks them for convenience but the driver is the source of
//! truth.

extern crate alloc;

use alloc::collections::BTreeMap;

use aerosls_blockcache::BufferAlloc;
use aerosls_proto::kabi::{GrantedCap, Kernel, RecvResult, SendCap, TIMEOUT_NONE};
use aerosls_proto::kwrap::{AWrap, KWrap};
use aerosls_proto::*;



#[allow(dead_code)]
/// Maximum number of sockets a single client tracks.
const MAX_SOCKS: usize = 16;

/// Socket state as known to the client.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SockState {
    Created,
    Bound,
    Listening,
    Connected,
    HalfClosed,
    Closed,
}

/// Client-side socket metadata (the driver owns the real state).
#[derive(Clone, Debug)]
pub struct ClientSocket {
    pub id: u32,
    pub sock_type: u16,
    pub state: SockState,
    pub local_ip: u32,
    pub local_port: u16,
    pub remote_ip: u32,
    pub remote_port: u16,
}

/// The NET_* protocol client. Generic over `K: Kernel` (shared via
/// `KWrap<K>`) and `A: BufferAlloc` (shared via `AWrap<A>`).
pub struct NetClient<K: Kernel, A: BufferAlloc> {
    k: KWrap<K>,
    chan_w: u32,  // CHAN_W — for send
    chan_r: u32,  // CHAN_R — for recv
    alloc: AWrap<A>,
    next_tag: u32,
    /// Max sockets and MTU from the NET_INFO handshake.
    max_sockets: u32,
    mtu: u32,
    /// Local socket table (client-side mirror).
    socks: BTreeMap<u32, ClientSocket>,
}

/// Error from a NET_* RPC.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NetError {
    /// The driver replied with an error status.
    Status(u16),
    /// Kernel-level failure.
    Kernel(i32),
    /// Protocol violation (bad magic, wrong reply tag, etc.).
    Protocol,
    /// The endpoint was closed by the driver.
    Closed,
}

impl<K: Kernel, A: BufferAlloc> NetClient<K, A> {
    /// Create a client on the given endpoint. The first send/recv must be
    /// `info()` (the handshake gate).
    pub fn new(k: KWrap<K>, chan_w: u32, chan_r: u32, alloc: AWrap<A>) -> Self {
        NetClient {
            k,
            chan_w,
            chan_r,
            alloc,
            next_tag: 1,
            max_sockets: 0,
            mtu: 0,
            socks: BTreeMap::new(),
        }
    }

    /// The channel endpoints to the network driver.
    pub fn chan_w(&self) -> u32 { self.chan_w }
    pub fn chan_r(&self) -> u32 { self.chan_r }

    fn next_tag(&mut self) -> u32 {
        let t = self.next_tag;
        self.next_tag = self.next_tag.wrapping_add(1);
        t
    }

    // ── handshake ────────────────────────────────────────────────────────

    /// NET_INFO: the handshake. Must be the first message. Returns
    /// `(max_sockets, mtu)`.
    pub fn info(&mut self) -> Result<(u32, u32), NetError> {
        let tag = self.next_tag();
        let frame = NetFrame::new(NET_INFO, false);
        // LTO-proof serial trace: write directly via syscall
        {
            let mut msg = [0u8; 32];
            msg[..14].copy_from_slice(b"[NETC] info ");
            let hex = b"0123456789ABCDEF";
            msg[14] = hex[((self.chan_w >> 4) & 0xF) as usize];
            msg[15] = hex[(self.chan_w & 0xF) as usize];
            msg[16] = b' ';
            msg[17] = hex[((self.chan_r >> 4) & 0xF) as usize];
            msg[18] = hex[(self.chan_r & 0xF) as usize];
            msg[19] = b'\n';
            unsafe {
                core::arch::asm!("syscall",
                    inlateout("rax") 165u64 => _,
                    inlateout("rdi") msg.as_ptr() => _,
                    lateout("rcx") _, lateout("r11") _,
                    lateout("rsi") _, lateout("rdx") _,
                    lateout("r8") _, lateout("r9") _, lateout("r10") _,
                    options(nostack),
                );
            }
        }
        self.k
            .send(self.chan_w, tag, 0, &frame.encode(), &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (rr, body) = self.recv_reply_owned(tag, NET_INFO)?;
        let frame = NetFrame::parse(&body).ok_or(NetError::Protocol)?;
        if frame.is_error() {
            let status = parse_status_body(&body[NetFrame::SIZE..])
                .map(|(s, _)| s)
                .unwrap_or(NET_ERR_PROTO);
            return Err(NetError::Status(status));
        }
        let (max_sockets, mtu, _flags) =
            parse_net_info_body(&body[NetFrame::SIZE..]).ok_or(NetError::Protocol)?;
        self.max_sockets = max_sockets;
        self.mtu = mtu;
        Ok((max_sockets, mtu))
    }

    // ── socket lifecycle ─────────────────────────────────────────────────

    /// NET_SOCKET: create a socket. Returns the driver-side socket ID.
    pub fn socket(&mut self, sock_type: u16) -> Result<u32, NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_SOCKET, false).encode());
        payload[NetFrame::SIZE..]
            .copy_from_slice(&encode_net_socket_body(sock_type, 0));

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_SOCKET)?;
        let frame = NetFrame::parse(&body).ok_or(NetError::Protocol)?;
        if frame.is_error() {
            let status = parse_status_body(&body[NetFrame::SIZE..])
                .map(|(s, _)| s)
                .unwrap_or(NET_ERR_PROTO);
            return Err(NetError::Status(status));
        }
        let (_status, sock_id) =
            parse_status_body(&body[NetFrame::SIZE..]).ok_or(NetError::Protocol)?;
        let id = sock_id as u32;
        self.socks.insert(
            id,
            ClientSocket {
                id,
                sock_type,
                state: SockState::Created,
                local_ip: 0,
                local_port: 0,
                remote_ip: 0,
                remote_port: 0,
            },
        );
        Ok(id)
    }

    /// NET_BIND: bind a socket to a local address.
    pub fn bind(&mut self, id: u32, ip: u32, port: u16) -> Result<(), NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4 + 6];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_BIND, false).encode());
        payload[NetFrame::SIZE..NetFrame::SIZE + 4].copy_from_slice(&id.to_le_bytes());
        payload[NetFrame::SIZE + 4..].copy_from_slice(&encode_sockaddr(ip, port));

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_BIND)?;
        let status = Self::status_of(&body)?;
        if status == NET_OK {
            if let Some(s) = self.socks.get_mut(&id) {
                s.local_ip = ip;
                s.local_port = port;
                s.state = SockState::Bound;
            }
        }
        Ok(())
    }

    /// NET_CONNECT: connect to a remote address.
    pub fn connect(&mut self, id: u32, ip: u32, port: u16) -> Result<(), NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4 + 6];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_CONNECT, false).encode());
        payload[NetFrame::SIZE..NetFrame::SIZE + 4].copy_from_slice(&id.to_le_bytes());
        payload[NetFrame::SIZE + 4..].copy_from_slice(&encode_sockaddr(ip, port));

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_CONNECT)?;
        let status = Self::status_of(&body)?;
        if status == NET_OK {
            if let Some(s) = self.socks.get_mut(&id) {
                s.remote_ip = ip;
                s.remote_port = port;
                s.state = SockState::Connected;
            }
        }
        Ok(())
    }

    /// NET_LISTEN: transition a bound socket to listening.
    pub fn listen(&mut self, id: u32) -> Result<(), NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_LISTEN, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&id.to_le_bytes());

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_LISTEN)?;
        let status = Self::status_of(&body)?;
        if status == NET_OK {
            if let Some(s) = self.socks.get_mut(&id) {
                s.state = SockState::Listening;
            }
        }
        Ok(())
    }

    /// NET_ACCEPT: accept a pending connection. Returns
    /// `(new_sock_id, remote_ip, remote_port)`.
    pub fn accept(&mut self, id: u32) -> Result<(u32, u32, u16), NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_ACCEPT, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&id.to_le_bytes());

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_ACCEPT)?;
        let frame = NetFrame::parse(&body).ok_or(NetError::Protocol)?;
        if frame.is_error() {
            let status = parse_status_body(&body[NetFrame::SIZE..])
                .map(|(s, _)| s)
                .unwrap_or(NET_ERR_PROTO);
            return Err(NetError::Status(status));
        }
        let (new_id, rip, rport) =
            parse_net_accept_body(&body[NetFrame::SIZE..]).ok_or(NetError::Protocol)?;

        self.socks.insert(
            new_id,
            ClientSocket {
                id: new_id,
                sock_type: SOCK_STREAM,
                state: SockState::Connected,
                local_ip: 0,
                local_port: 0,
                remote_ip: rip,
                remote_port: rport,
            },
        );
        Ok((new_id, rip, rport))
    }

    // ── data transfer ────────────────────────────────────────────────────

    /// NET_SEND: send data. Returns the number of bytes accepted by the
    /// driver (may be less than `data.len()` for stream sockets).
    pub fn send(&mut self, id: u32, data: &[u8]) -> Result<usize, NetError> {
        let tag = self.next_tag();

        // Grant the buffer R-only to the driver.
        let (grant, base) = self
            .alloc
            .alloc(data.len())
            .map_err(NetError::Kernel)?;
        let send_cap = SendCap {
            rights: R,
            ..grant
        };
        // Safety: the allocator minted this buffer for us.
        unsafe {
            core::ptr::copy_nonoverlapping(data.as_ptr(), base as *mut u8, data.len());
        }

        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_SEND, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&id.to_le_bytes());

        self.k
            .send(self.chan_w, tag, 0, &payload, &[send_cap], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_SEND)?;
        let status = Self::status_of(&body)?;
        if status != NET_OK {
            return Err(NetError::Status(status));
        }
        let (_status, sent) =
            parse_status_body(&body[NetFrame::SIZE..]).ok_or(NetError::Protocol)?;
        Ok(sent as usize)
    }

    /// NET_RECV: receive data into `buf`. Returns the number of bytes read.
    /// On a stream socket with no data and the peer alive, returns
    /// `NetError::Status(NET_ERR_WOULDBLOCK)`.
    pub fn recv(&mut self, id: u32, buf: &mut [u8]) -> Result<usize, NetError> {
        let tag = self.next_tag();

        // Grant the buffer W-only to the driver.
        let (grant, base) = self
            .alloc
            .alloc(buf.len())
            .map_err(NetError::Kernel)?;
        let send_cap = SendCap {
            rights: W,
            ..grant
        };

        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_RECV, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&id.to_le_bytes());

        self.k
            .send(self.chan_w, tag, 0, &payload, &[send_cap], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_RECV)?;
        let status = Self::status_of(&body)?;
        if status != NET_OK {
            return Err(NetError::Status(status));
        }
        let (_status, n) =
            parse_status_body(&body[NetFrame::SIZE..]).ok_or(NetError::Protocol)?;
        let n = n as usize;
        // Safety: the driver wrote into our buffer via the grant.
        unsafe {
            core::ptr::copy_nonoverlapping(base as *const u8, buf.as_mut_ptr(), n);
        }
        Ok(n)
    }

    // ── socket control ───────────────────────────────────────────────────

    /// NET_POLL: check events on a socket. Returns a bitmask (POLLIN=0x1,
    /// POLLOUT=0x4).
    pub fn poll(&mut self, id: u32) -> Result<u16, NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_POLL, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&id.to_le_bytes());

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_POLL)?;
        let frame = NetFrame::parse(&body).ok_or(NetError::Protocol)?;
        if frame.is_error() {
            let status = parse_status_body(&body[NetFrame::SIZE..])
                .map(|(s, _)| s)
                .unwrap_or(NET_ERR_PROTO);
            return Err(NetError::Status(status));
        }
        let (_id, events) =
            parse_net_poll_body(&body[NetFrame::SIZE..]).ok_or(NetError::Protocol)?;
        Ok(events)
    }

    /// NET_SHUTDOWN: half/close a socket. `how`: 0=SHUT_RD, 1=SHUT_WR,
    /// 2=SHUT_RDWR.
    pub fn shutdown(&mut self, id: u32, how: u8) -> Result<(), NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 5];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_SHUTDOWN, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&encode_net_shutdown_body(id, how));

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_SHUTDOWN)?;
        let status = Self::status_of(&body)?;
        if status == NET_OK {
            if let Some(s) = self.socks.get_mut(&id) {
                s.state = match how {
                    0 => SockState::HalfClosed,
                    1 => SockState::HalfClosed,
                    _ => SockState::Closed,
                };
            }
        }
        Ok(())
    }

    /// NET_CLOSE_SOCK: close a socket on the driver side.
    pub fn close_socket(&mut self, id: u32) -> Result<(), NetError> {
        let tag = self.next_tag();
        let mut payload = [0u8; NetFrame::SIZE + 4];
        payload[..NetFrame::SIZE]
            .copy_from_slice(&NetFrame::new(NET_CLOSE_SOCK, false).encode());
        payload[NetFrame::SIZE..].copy_from_slice(&id.to_le_bytes());

        self.k
            .send(self.chan_w, tag, 0, &payload, &[], TIMEOUT_NONE)
            .map_err(NetError::Kernel)?;

        let (_rr, body) = self.recv_reply_owned(tag, NET_CLOSE_SOCK)?;
        let _status = Self::status_of(&body)?;
        self.socks.remove(&id);
        Ok(())
    }

    // ── introspection ────────────────────────────────────────────────────

    /// Look up a client-side socket by ID.
    pub fn get_socket(&self, id: u32) -> Option<&ClientSocket> {
        self.socks.get(&id)
    }

    /// Max sockets from the handshake.
    pub fn max_sockets(&self) -> u32 {
        self.max_sockets
    }

    /// MTU from the handshake.
    pub fn mtu(&self) -> u32 {
        self.mtu
    }

    // ── internal helpers ─────────────────────────────────────────────────

    /// Receive a reply and return the body bytes (everything after the
    /// NetFrame header) as an owned Vec.
    fn recv_reply_owned(&mut self, expected_tag: u32, expected_ty: u16) -> Result<(RecvResult, alloc::vec::Vec<u8>), NetError> {
        // Retry recv with yield.  A single non-blocking recv often returns
        // ERR_STATE because the network sidecar hasn't had time to
        // process the request yet.  yield lets the scheduler run the
        // net sidecar before we retry.
        //
        // NOTE: The kernel's channel transport only propagates F_NO_REPLY
        // through the flags field -- F_REPLY is never visible to the
        // receiver.  So we check kind and tag only.
        extern "C" { fn k_yield(); }
        for _ in 0..20 {
            let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
            let mut caps = [GrantedCap::default(); ChanHeader::MAX_CAPS];
            match self.k.recv(self.chan_r, &mut buf, &mut caps) {
                Ok(rr) => {
                    if rr.kind == CH_KIND_MSG && rr.tag == expected_tag {
                        // Verify the reply's frame type matches the request.
                        // The boot-time NET_INFO handshake retries with the
                        // same tag, so a stale same-tag reply can sit in the
                        // queue; type-checking stops us misparsing it (e.g.
                        // a stray NET_INFO body read as a socket status body
                        // yields a garbage sock_id and bind() INVAL).
                        if let Some(f) = NetFrame::parse(&buf[..rr.len]) {
                            if f.ty == expected_ty {
                                return Ok((rr, alloc::vec::Vec::from(&buf[..rr.len])));
                            }
                        }
                    }
                }
                Err(_) => {}
            }
            unsafe { k_yield(); }
        }
        Err(NetError::Closed)
    }

    /// Extract the status from a reply body (after the NetFrame header).
    fn status_of(body: &[u8]) -> Result<u16, NetError> {
        let frame = NetFrame::parse(body).ok_or(NetError::Protocol)?;
        if frame.is_error() {
            let status = parse_status_body(&body[NetFrame::SIZE..])
                .map(|(s, _)| s)
                .unwrap_or(NET_ERR_PROTO);
            return Err(NetError::Status(status));
        }
        let status = parse_status_body(&body[NetFrame::SIZE..])
            .map(|(s, _)| s)
            .unwrap_or(NET_ERR_PROTO);
        Ok(status)
    }
}

// ── SocketOps trait implementation ───────────────────────────────────────

impl<K: Kernel, A: BufferAlloc> aerosls_proto::sockops::SocketOps for NetClient<K, A> {
    fn socket(&mut self, sock_type: u16) -> Result<u32, u16> {
        NetClient::socket(self, sock_type).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn connect(&mut self, id: u32, ip: u32, port: u16) -> Result<(), u16> {
        NetClient::connect(self, id, ip, port).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn send(&mut self, id: u32, data: &[u8]) -> Result<usize, u16> {
        NetClient::send(self, id, data).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn recv(&mut self, id: u32, buf: &mut [u8]) -> Result<usize, u16> {
        NetClient::recv(self, id, buf).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn bind(&mut self, id: u32, ip: u32, port: u16) -> Result<(), u16> {
        NetClient::bind(self, id, ip, port).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn listen(&mut self, id: u32) -> Result<(), u16> {
        NetClient::listen(self, id).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn accept(&mut self, id: u32) -> Result<u32, u16> {
        NetClient::accept(self, id)
            .map(|(new_id, _, _)| new_id)
            .map_err(|e| match e {
                NetError::Status(s) => s,
                _ => NET_ERR_IO,
            })
    }

    fn close_socket(&mut self, id: u32) -> Result<(), u16> {
        NetClient::close_socket(self, id).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn poll(&mut self, id: u32) -> Result<u16, u16> {
        NetClient::poll(self, id).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }

    fn shutdown(&mut self, id: u32, how: u8) -> Result<(), u16> {
        NetClient::shutdown(self, id, how).map_err(|e| match e {
            NetError::Status(s) => s,
            _ => NET_ERR_IO,
        })
    }
}

// ── tests ────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use aerosls_proto::kabi::ERR_STATE;

    /// Minimal fake kernel for testing: accepts send, rejects recv.
    struct TestKernel;

    impl Kernel for TestKernel {
        fn wait(&self, _: &[u32], _: u64) -> Result<(usize, u16), i32> {
            Err(ERR_STATE)
        }
        fn recv(&self, _: u32, _: &mut [u8], _: &mut [GrantedCap]) -> Result<RecvResult, i32> {
            Err(ERR_STATE)
        }
        fn send(
            &self,
            _: u32,
            _: u32,
            _: u16,
            _: &[u8],
            _: &[SendCap],
            _: u64,
        ) -> Result<(), i32> {
            Ok(())
        }
        fn close(&self, _: u32, _: u16, _: u32) -> Result<(), i32> {
            Ok(())
        }
        fn cap_info(&self, _: u32) -> Result<aerosls_proto::kabi::CapInfo, i32> {
            Err(ERR_STATE)
        }
    }

    #[test]
    fn client_creation() {
        use alloc::sync::Arc;
        let k = KWrap(Arc::new(TestKernel));
        let alloc = AWrap(Arc::new(aerosls_proto::Mutex::new(NoAlloc)));
        let _client: NetClient<TestKernel, NoAlloc> = NetClient::new(k, 7, 8, alloc);
    }

    struct NoAlloc;
    impl BufferAlloc for NoAlloc {
        fn alloc(&mut self, _: usize) -> Result<(SendCap, u64), i32> {
            Err(ERR_STATE)
        }
    }
}
