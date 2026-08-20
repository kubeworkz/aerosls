//! AeroSLS shared sidecar protocol definitions.
//!
//! Single source of truth for the wire format shared by the ramdisk driver
//! sidecar and its clients (the POSIX sidecar's block cache). `#![no_std]` so
//! it links into any sidecar without libc.
//!
//! Layering (per `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md` §8):
//!
//! ```text
//! Layer 3  RD_* protocol frames  — this crate (payload contents)
//! Layer 2  channel envelope      — this crate (magic/kind/flags/tag/caps)
//! Layer 1  transport             — the kernel (queueing, FIFO, wakeups)
//! Layer 0  caps                  — the kernel capability layer
//! ```
//!
//! Framing note (deviation from `docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`
//! §5.2, per the transport spec's layering note): the RD_* frame is the
//! *payload* of the channel envelope. The request id (`tag`) and the
//! capability arguments live in the envelope; the RD_* frame header carries
//! only magic/version/type/error-flag. The Phase 2 doc's combined frame was
//! the earlier draft; the transport spec (the later, canonical doc) splits the
//! two, and this crate implements the split.
//!
//! Byte order: little-endian everywhere, fixed (no negotiation, v1).

#![no_std]

extern crate alloc;

#[cfg(test)]
extern crate std;

/// ── Kernel ABI (shared by every sidecar) ────────────────────────────────────

pub mod kabi;
pub mod kwrap;
pub mod sockops;

/// ── Boot Info Block + packed sidecar manifest (kernel ↔ sidecar contract) ──

pub mod bootinfo;
pub mod manifest;

/// ── Channel envelope (Layer 2, transport spec §3.1) ────────────────────────

pub const CH_MAGIC: [u8; 8] = *b"AEROSCH\x01";
pub const CH_VERSION: u16 = 1;

pub const CH_KIND_MSG: u16 = 0;
pub const CH_KIND_CLOSE: u16 = 1;
pub const CH_KIND_NEW_CHANNEL: u16 = 2;
/// `Kernel::poll`: nothing is queued on the endpoint.
pub const CH_KIND_NONE: u16 = 3;

/// Envelope flags.
pub const F_REPLY: u16 = 0x0001;
pub const F_NO_REPLY: u16 = 0x0002;

/// Channel close reasons (transport spec §6.2).
pub const CLOSE_PEER: u16 = 0x0001;
pub const CLOSE_PEER_DEAD: u16 = 0x0002;
pub const CLOSE_REVOKED: u16 = 0x0003;
pub const CLOSE_PROTO: u16 = 0x0004;
pub const CLOSE_ADMIN: u16 = 0x0005;

/// Capability-argument flags (capability-layer spec §4.2).
pub const CAP_PERSIST: u8 = 0x01;
pub const CAP_MOVE: u8 = 0x02;
pub const CAP_MAP: u8 = 0x04;

/// MEM rights bits (capability-layer spec §1.1).
pub const R: u8 = 0x01;
pub const W: u8 = 0x02;
pub const X: u8 = 0x04;

/// The 28-byte channel envelope header (transport spec §3.1).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChanHeader {
    pub magic: [u8; 8],
    pub version: u16,
    pub kind: u16,
    pub flags: u16,
    pub cap_count: u16,
    pub tag: u32,
    pub payload_len: u16,
    pub rsvd1: u16,
    pub rsvd2: u32,
}

impl ChanHeader {
    pub const SIZE: usize = 28;
    pub const MAX_PAYLOAD: usize = 4096;
    pub const MAX_CAPS: usize = 8;

    pub fn encode(&self) -> [u8; Self::SIZE] {
        let mut b = [0u8; Self::SIZE];
        b[0..8].copy_from_slice(&self.magic);
        put_u16(&mut b, 8, self.version);
        put_u16(&mut b, 10, self.kind);
        put_u16(&mut b, 12, self.flags);
        put_u16(&mut b, 14, self.cap_count);
        put_u32(&mut b, 16, self.tag);
        put_u16(&mut b, 20, self.payload_len);
        put_u16(&mut b, 22, self.rsvd1);
        put_u32(&mut b, 24, self.rsvd2);
        b
    }

    /// Length check only; the caller validates magic/version.
    pub fn parse(b: &[u8]) -> Option<ChanHeader> {
        if b.len() < Self::SIZE {
            return None;
        }
        Some(ChanHeader {
            magic: read8(b, 0),
            version: read_u16(b, 8),
            kind: read_u16(b, 10),
            flags: read_u16(b, 12),
            cap_count: read_u16(b, 14),
            tag: read_u32(b, 16),
            payload_len: read_u16(b, 20),
            rsvd1: read_u16(b, 22),
            rsvd2: read_u32(b, 24),
        })
    }
}

/// Capability argument descriptor — 16 bytes (capability-layer spec §4.1,
/// Phase 2 §5.2).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CapDescriptor {
    pub slot: u32,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    pub flags: u8,
    pub pad: u16,
}

impl CapDescriptor {
    pub const SIZE: usize = 16;

    pub fn encode(&self) -> [u8; Self::SIZE] {
        let mut b = [0u8; Self::SIZE];
        put_u32(&mut b, 0, self.slot);
        put_u32(&mut b, 4, self.offset);
        put_u32(&mut b, 8, self.len);
        b[12] = self.rights;
        b[13] = self.flags;
        put_u16(&mut b, 14, self.pad);
        b
    }

    pub fn parse(b: &[u8]) -> Option<CapDescriptor> {
        if b.len() < Self::SIZE {
            return None;
        }
        Some(CapDescriptor {
            slot: read_u32(b, 0),
            offset: read_u32(b, 4),
            len: read_u32(b, 8),
            rights: b[12],
            flags: b[13],
            pad: read_u16(b, 14),
        })
    }
}

/// ── RD_* protocol (Layer 3) ─────────────────────────────────────────────────

pub const RD_MAGIC: [u8; 8] = *b"AEROSRD\x01";
pub const RD_VERSION: u16 = 1;

pub const RD_INFO: u16 = 1;
pub const RD_READ: u16 = 2;
pub const RD_WRITE: u16 = 3;
pub const RD_FLUSH: u16 = 4;
pub const RD_MAP: u16 = 5;

/// Protocol-level frame flag: set on error replies.
pub const RD_FLAG_ERROR: u16 = 0x0001;

/// Status codes (Phase 2 §5.4).
pub const RD_OK: u16 = 0;
pub const RD_ERR_INVAL: u16 = 1;
pub const RD_ERR_RANGE: u16 = 2;
pub const RD_ERR_CAP: u16 = 3;
pub const RD_ERR_NOMEM: u16 = 4;
pub const RD_ERR_IO: u16 = 5;
pub const RD_ERR_RO: u16 = 6;
pub const RD_ERR_BUSY: u16 = 7;
pub const RD_ERR_PROTO: u16 = 8;

pub const BLOCK_SIZE: u32 = 512;
/// v1 per-request I/O bound (implementation plan §6.6).
pub const MAX_IO: u32 = 64;

/// The 16-byte RD_* frame header — the payload of a channel MSG envelope.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RdFrame {
    pub magic: [u8; 8],
    pub version: u16,
    pub ty: u16,
    pub flags: u16,
    pub pad: u16,
}

impl RdFrame {
    pub const SIZE: usize = 16;

    pub fn new(ty: u16, error: bool) -> RdFrame {
        RdFrame {
            magic: RD_MAGIC,
            version: RD_VERSION,
            ty,
            flags: if error { RD_FLAG_ERROR } else { 0 },
            pad: 0,
        }
    }

    pub fn encode(&self) -> [u8; Self::SIZE] {
        let mut b = [0u8; Self::SIZE];
        b[0..8].copy_from_slice(&self.magic);
        put_u16(&mut b, 8, self.version);
        put_u16(&mut b, 10, self.ty);
        put_u16(&mut b, 12, self.flags);
        put_u16(&mut b, 14, self.pad);
        b
    }

    pub fn is_error(&self) -> bool {
        self.flags & RD_FLAG_ERROR != 0
    }

    /// Length check only; the caller validates magic/version.
    pub fn parse(b: &[u8]) -> Option<RdFrame> {
        if b.len() < Self::SIZE {
            return None;
        }
        Some(RdFrame {
            magic: read8(b, 0),
            version: read_u16(b, 8),
            ty: read_u16(b, 10),
            flags: read_u16(b, 12),
            pad: read_u16(b, 14),
        })
    }
}

/// RD_READ/RD_WRITE request body: `{lba u64, count u32}` (12 bytes).
pub fn encode_rw_body(lba: u64, count: u32) -> [u8; 12] {
    let mut b = [0u8; 12];
    put_u64(&mut b, 0, lba);
    put_u32(&mut b, 8, count);
    b
}

pub fn parse_rw_body(p: &[u8]) -> Option<(u64, u32)> {
    if p.len() < 12 {
        return None;
    }
    Some((read_u64(p, 0), read_u32(p, 8)))
}

/// RD_INFO reply body: `{block_size u32, blocks u64, flags u32}` (16 bytes).
/// `flags` bit0 = read-only.
pub fn encode_info_body(block_size: u32, blocks: u64, flags: u32) -> [u8; 16] {
    let mut b = [0u8; 16];
    put_u32(&mut b, 0, block_size);
    put_u64(&mut b, 4, blocks);
    put_u32(&mut b, 12, flags);
    b
}

pub fn parse_info_body(p: &[u8]) -> Option<(u32, u64, u32)> {
    if p.len() < 16 {
        return None;
    }
    Some((read_u32(p, 0), read_u64(p, 4), read_u32(p, 12)))
}

/// Status reply body: `{status u16, bytes u64}` (10 bytes). Used for
/// RD_READ/RD_WRITE/RD_FLUSH/RD_MAP replies.
pub fn encode_status_body(status: u16, bytes: u64) -> [u8; 10] {
    let mut b = [0u8; 10];
    put_u16(&mut b, 0, status);
    put_u64(&mut b, 2, bytes);
    b
}

pub fn parse_status_body(p: &[u8]) -> Option<(u16, u64)> {
    if p.len() < 10 {
        return None;
    }
    Some((read_u16(p, 0), read_u64(p, 2)))
}

/// ── NET_* protocol (Layer 3 — network driver sidecar) ────────────────────────

pub const NET_MAGIC: [u8; 8] = *b"AEROSNT\x01";
pub const NET_VERSION: u16 = 1;

pub const NET_INFO: u16 = 1;
pub const NET_SOCKET: u16 = 2;
pub const NET_BIND: u16 = 3;
pub const NET_CONNECT: u16 = 4;
pub const NET_LISTEN: u16 = 5;
pub const NET_ACCEPT: u16 = 6;
pub const NET_SEND: u16 = 7;
pub const NET_RECV: u16 = 8;
pub const NET_SHUTDOWN: u16 = 9;
pub const NET_CLOSE_SOCK: u16 = 10;
pub const NET_POLL: u16 = 11;

/// Protocol-level frame flag: set on error replies.
pub const NET_FLAG_ERROR: u16 = 0x0001;

/// NET_* status codes.
pub const NET_OK: u16 = 0;
pub const NET_ERR_INVAL: u16 = 1;
pub const NET_ERR_CONNRESET: u16 = 2;
pub const NET_ERR_NOTCONN: u16 = 3;
pub const NET_ERR_ADDRINUSE: u16 = 4;
pub const NET_ERR_ADDRNOTAVAIL: u16 = 5;
pub const NET_ERR_MSGSIZE: u16 = 6;
pub const NET_ERR_NOMEM: u16 = 7;
pub const NET_ERR_IO: u16 = 8;
pub const NET_ERR_CAP: u16 = 9;
pub const NET_ERR_WOULDBLOCK: u16 = 10;
pub const NET_ERR_INUSE: u16 = 11;
pub const NET_ERR_PROTO: u16 = 12;

/// Socket types.
pub const SOCK_STREAM: u16 = 1;
pub const SOCK_DGRAM: u16 = 2;

/// Socket states.
pub const NET_STATE_LISTENING: u8 = 1;
pub const NET_STATE_CONNECTED: u8 = 2;
pub const NET_STATE_CLOSED: u8 = 3;

/// The 16-byte NET_* frame header — the payload of a channel MSG envelope.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NetFrame {
    pub magic: [u8; 8],
    pub version: u16,
    pub ty: u16,
    pub flags: u16,
    pub pad: u16,
}

impl NetFrame {
    pub const SIZE: usize = 16;

    pub fn new(ty: u16, error: bool) -> NetFrame {
        NetFrame {
            magic: NET_MAGIC,
            version: NET_VERSION,
            ty,
            flags: if error { NET_FLAG_ERROR } else { 0 },
            pad: 0,
        }
    }

    pub fn encode(&self) -> [u8; Self::SIZE] {
        let mut b = [0u8; Self::SIZE];
        b[0..8].copy_from_slice(&self.magic);
        put_u16(&mut b, 8, self.version);
        put_u16(&mut b, 10, self.ty);
        put_u16(&mut b, 12, self.flags);
        put_u16(&mut b, 14, self.pad);
        b
    }

    pub fn is_error(&self) -> bool {
        self.flags & NET_FLAG_ERROR != 0
    }

    /// Length check only; the caller validates magic/version.
    pub fn parse(b: &[u8]) -> Option<NetFrame> {
        if b.len() < Self::SIZE {
            return None;
        }
        Some(NetFrame {
            magic: read8(b, 0),
            version: read_u16(b, 8),
            ty: read_u16(b, 10),
            flags: read_u16(b, 12),
            pad: read_u16(b, 14),
        })
    }
}

/// NET_INFO reply body: `{max_sockets u32, mtu u32, flags u32}` (12 bytes).
pub fn encode_net_info_body(max_sockets: u32, mtu: u32, flags: u32) -> [u8; 12] {
    let mut b = [0u8; 12];
    put_u32(&mut b, 0, max_sockets);
    put_u32(&mut b, 4, mtu);
    put_u32(&mut b, 8, flags);
    b
}

pub fn parse_net_info_body(p: &[u8]) -> Option<(u32, u32, u32)> {
    if p.len() < 12 {
        return None;
    }
    Some((read_u32(p, 0), read_u32(p, 4), read_u32(p, 8)))
}

/// NET_SOCKET request body: `{sock_type u16, protocol u16}` (4 bytes).
pub fn encode_net_socket_body(sock_type: u16, protocol: u16) -> [u8; 4] {
    let mut b = [0u8; 4];
    put_u16(&mut b, 0, sock_type);
    put_u16(&mut b, 2, protocol);
    b
}

pub fn parse_net_socket_body(p: &[u8]) -> Option<(u16, u16)> {
    if p.len() < 4 {
        return None;
    }
    Some((read_u16(p, 0), read_u16(p, 2)))
}

/// Socket address: `{ip u32, port u16}` (6 bytes, IPv4 only in v1).
pub fn encode_sockaddr(ip: u32, port: u16) -> [u8; 6] {
    let mut b = [0u8; 6];
    put_u32(&mut b, 0, ip);
    put_u16(&mut b, 4, port);
    b
}

pub fn parse_sockaddr(p: &[u8]) -> Option<(u32, u16)> {
    if p.len() < 6 {
        return None;
    }
    Some((read_u32(p, 0), read_u16(p, 4)))
}

/// NET_BIND/NET_CONNECT request body: `{addr: [u8; 6]}` (6 bytes).
/// (Reuses sockaddr encoding.)

/// NET_ACCEPT reply body: `{new_sock_id u32, addr: [u8; 6]}` (10 bytes).
pub fn encode_net_accept_body(sock_id: u32, ip: u32, port: u16) -> [u8; 10] {
    let mut b = [0u8; 10];
    put_u32(&mut b, 0, sock_id);
    b[4..10].copy_from_slice(&encode_sockaddr(ip, port));
    b
}

pub fn parse_net_accept_body(p: &[u8]) -> Option<(u32, u32, u16)> {
    if p.len() < 10 {
        return None;
    }
    Some((read_u32(p, 0), read_u32(p, 4), read_u16(p, 8)))
}

/// NET_SHUTDOWN request body: `{sock_id u32, how u8}` (5 bytes).
pub fn encode_net_shutdown_body(sock_id: u32, how: u8) -> [u8; 5] {
    let mut b = [0u8; 5];
    put_u32(&mut b, 0, sock_id);
    b[4] = how;
    b
}

pub fn parse_net_shutdown_body(p: &[u8]) -> Option<(u32, u8)> {
    if p.len() < 5 {
        return None;
    }
    Some((read_u32(p, 0), p[4]))
}

/// NET_POLL reply body: `{sock_id u32, events u16}` (6 bytes).
pub fn encode_net_poll_body(sock_id: u32, events: u16) -> [u8; 6] {
    let mut b = [0u8; 6];
    put_u32(&mut b, 0, sock_id);
    put_u16(&mut b, 4, events);
    b
}

pub fn parse_net_poll_body(p: &[u8]) -> Option<(u32, u16)> {
    if p.len() < 6 {
        return None;
    }
    Some((read_u32(p, 0), read_u16(p, 4)))
}

/// NEW_CHANNEL control-event payload: `{handle u32, rights u16, flags u16,
/// tag u32}` (12 bytes, transport spec §3.1).
pub fn encode_new_channel(handle: u32, rights: u16, flags: u16, tag: u32) -> [u8; 12] {
    let mut b = [0u8; 12];
    put_u32(&mut b, 0, handle);
    put_u16(&mut b, 4, rights);
    put_u16(&mut b, 6, flags);
    put_u32(&mut b, 8, tag);
    b
}

pub fn parse_new_channel(p: &[u8]) -> Option<(u32, u16, u16, u32)> {
    if p.len() < 12 {
        return None;
    }
    Some((read_u32(p, 0), read_u16(p, 4), read_u16(p, 6), read_u32(p, 8)))
}

/// CLOSE control-event payload: `{reason u16, detail u32}` (6 bytes, padded
/// to 8, transport spec §3.1).
pub fn encode_close_body(reason: u16, detail: u32) -> [u8; 8] {
    let mut b = [0u8; 8];
    put_u16(&mut b, 0, reason);
    put_u32(&mut b, 2, detail);
    b
}

pub fn parse_close_body(p: &[u8]) -> Option<(u16, u32)> {
    if p.len() < 6 {
        return None;
    }
    Some((read_u16(p, 0), read_u32(p, 2)))
}

// ── little-endian helpers ────────────────────────────────────────────────────

pub(crate) fn read8(b: &[u8], i: usize) -> [u8; 8] {
    let mut m = [0u8; 8];
    m.copy_from_slice(&b[i..i + 8]);
    m
}

pub(crate) fn read_u16(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}

pub(crate) fn read_u32(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

pub(crate) fn read_u64(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes([
        b[i],
        b[i + 1],
        b[i + 2],
        b[i + 3],
        b[i + 4],
        b[i + 5],
        b[i + 6],
        b[i + 7],
    ])
}

pub(crate) fn put_u16(b: &mut [u8], i: usize, v: u16) {
    b[i..i + 2].copy_from_slice(&v.to_le_bytes());
}

pub(crate) fn put_u32(b: &mut [u8], i: usize, v: u32) {
    b[i..i + 4].copy_from_slice(&v.to_le_bytes());
}

pub(crate) fn put_u64(b: &mut [u8], i: usize, v: u64) {
    b[i..i + 8].copy_from_slice(&v.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_layout() {
        assert_eq!(core::mem::size_of::<ChanHeader>(), 28);
        assert_eq!(core::mem::size_of::<RdFrame>(), 16);
        assert_eq!(core::mem::size_of::<CapDescriptor>(), 16);
    }

    #[test]
    fn chan_header_roundtrip() {
        let h = ChanHeader {
            magic: CH_MAGIC,
            version: CH_VERSION,
            kind: CH_KIND_MSG,
            flags: F_REPLY,
            cap_count: 1,
            tag: 0xDEAD_BEEF,
            payload_len: 42,
            rsvd1: 0,
            rsvd2: 0,
        };
        let bytes = h.encode();
        assert_eq!(&bytes[0..8], &CH_MAGIC);
        let back = ChanHeader::parse(&bytes).unwrap();
        assert_eq!(back, h);
        assert!(ChanHeader::parse(&bytes[..10]).is_none());
    }

    #[test]
    fn rd_frame_roundtrip() {
        let f = RdFrame::new(RD_READ, false);
        let bytes = f.encode();
        let back = RdFrame::parse(&bytes).unwrap();
        assert_eq!(back, f);
        assert_eq!(back.ty, RD_READ);
        assert!(!back.is_error());
        assert!(RdFrame::new(RD_WRITE, true).is_error());
    }

    #[test]
    fn cap_descriptor_roundtrip() {
        let c = CapDescriptor {
            slot: 7,
            offset: 0,
            len: 512,
            rights: W,
            flags: CAP_PERSIST | CAP_MAP,
            pad: 0,
        };
        let back = CapDescriptor::parse(&c.encode()).unwrap();
        assert_eq!(back, c);
    }

    #[test]
    fn rw_body_roundtrip() {
        let b = encode_rw_body(12345, 3);
        assert_eq!(parse_rw_body(&b), Some((12345, 3)));
        assert_eq!(parse_rw_body(&b[..11]), None);
    }

    #[test]
    fn info_body_roundtrip() {
        let b = encode_info_body(BLOCK_SIZE, 65536, 1);
        assert_eq!(parse_info_body(&b), Some((BLOCK_SIZE, 65536, 1)));
    }

    #[test]
    fn status_body_roundtrip() {
        let b = encode_status_body(RD_ERR_RANGE, 0);
        assert_eq!(parse_status_body(&b), Some((RD_ERR_RANGE, 0)));
    }

    #[test]
    fn control_event_payloads() {
        let nc = encode_new_channel(9, 0x7, 0, 3);
        assert_eq!(parse_new_channel(&nc), Some((9, 0x7, 0, 3)));

        let cl = encode_close_body(CLOSE_PEER_DEAD, 7);
        assert_eq!(parse_close_body(&cl), Some((CLOSE_PEER_DEAD, 7)));
    }

    #[test]
    fn net_frame_roundtrip() {
        let f = NetFrame::new(NET_CONNECT, false);
        let bytes = f.encode();
        let back = NetFrame::parse(&bytes).unwrap();
        assert_eq!(back, f);
        assert_eq!(back.ty, NET_CONNECT);
        assert!(!back.is_error());
        assert!(NetFrame::new(NET_SEND, true).is_error());
    }

    #[test]
    fn net_addr_roundtrip() {
        let b = encode_sockaddr(0xC0A80001, 8080);
        assert_eq!(parse_sockaddr(&b), Some((0xC0A80001, 8080)));
    }

    #[test]
    fn net_info_body_roundtrip() {
        let b = encode_net_info_body(16, 1500, 0);
        assert_eq!(parse_net_info_body(&b), Some((16, 1500, 0)));
    }

    #[test]
    fn net_socket_body_roundtrip() {
        let b = encode_net_socket_body(SOCK_STREAM, 0);
        assert_eq!(parse_net_socket_body(&b), Some((SOCK_STREAM, 0)));
    }

    #[test]
    fn magic_bytes() {
        assert_eq!(&CH_MAGIC, b"AEROSCH\x01");
        assert_eq!(&RD_MAGIC, b"AEROSRD\x01");
        assert_eq!(&NET_MAGIC, b"AEROSNT\x01");
    }
}
