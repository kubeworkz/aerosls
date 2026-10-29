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
pub mod catalops;

/// Re-export `spin::Mutex` for use in AWrap constructors across crates.
pub use spin::Mutex;

/// ── Boot Info Block + packed sidecar manifest (kernel ↔ sidecar contract) ──

pub mod bootinfo;
pub mod devreg;
pub mod env_proto;
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
/// P1b: the partition's storage quota refused the write — the quota's own
/// status, deliberately not NOMEM's (a frame-pool exhaustion) and not
/// RANGE's, so the POSIX side can surface EDQUOT and nothing else can
/// masquerade as it (v0.2 §5's "refused with the quota's own error").
pub const RD_ERR_QUOTA: u16 = 9;

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

/// ── CAT_* protocol (POSIX-Environments v0.2 §9.1 — the mediated catalog)
///
/// The client half of kernel/env_catalog.h's wire. The kernel header is the
/// source of truth for the bytes (the kernel does not link Rust, so they are
/// written twice and pinned against each other: the host test reads the
/// kernel's side, and the round-trip tests below read these). Same 16-byte
/// header shape as NetFrame, distinct magic, so a CAT_* frame on a socket
/// channel — or a NET_* frame on a catalog channel — is dropped as
/// not-a-frame rather than answered as a verb.

pub const CAT_MAGIC: [u8; 8] = *b"AEROSCT\x01";
pub const CAT_VERSION: u16 = 1;

/// Header size, and the offsets of the fields a client reads on EVERY reply:
/// status sits at CAT_STATUS on a clean answer and a refusal alike, so the
/// caller reads it without first branching on the error flag, and a verb's
/// payload (or the rendered refusal text) always starts at CAT_BODY.
pub const CAT_HDR: usize = 16;
pub const CAT_STATUS: usize = 16;
pub const CAT_BODY: usize = 18;

pub const CAT_INFO: u16 = 1;
pub const CAT_LIST: u16 = 2;
pub const CAT_STAT: u16 = 3;
pub const CAT_READ: u16 = 4;
/// The SQL path: a read of a row-set table, page by page. Still read-only —
/// a SELECT is the only statement the service admits, so this verb's
/// existence does not put a write verb in the table.
pub const CAT_QUERY: u16 = 5;

pub const CAT_FLAG_ERROR: u16 = 0x0001;

pub const CAT_OK: u16 = 0;
/// "Not in this namespace" — covers both a name that never existed and one
/// in a partition that is not yours. The service refuses to distinguish
/// them; a client that printed them differently would be re-creating the
/// existence oracle §9.1 removes.
pub const CAT_ERR_NOENT: u16 = 1;
pub const CAT_ERR_PERM: u16 = 2;
pub const CAT_ERR_SHAPE: u16 = 3;
pub const CAT_ERR_TYPE: u16 = 4;
/// The verb, the table and the uid were all fine and the SQL engine still
/// refused the statement — the engine's own message arrives as the refusal
/// text, so a caller sees the parse error rather than a number.
pub const CAT_ERR_QUERY: u16 = 5;
pub const CAT_ERR_UNKNOWN: u16 = 9;

pub const CAT_MAX_NAME: usize = 64;   // OBJECT_NAME_LEN
pub const CAT_MAX_KEY: usize = 64;    // RECORD_KEY_LEN
pub const CAT_MAX_VALUE: usize = 256; // RECORD_VAL_LEN
pub const CAT_LIST_PAGE_MAX: u16 = 8;
/// A CAT_LIST reply is fixed-slot: `{type u8, tier u8, pad u16, name[64]}`.
pub const CAT_LIST_ENTRY: usize = 68;
/// `{header[16], status[2], count u16, total u16, partition u32}` — so the
/// entries start at CAT_LIST_HDR - CAT_BODY == 8 bytes into the body.
pub const CAT_LIST_HDR: usize = 26;
/// The rendered refusal, capped where the kernel caps it.
pub const CAT_TEXT_CAP: usize = 192;

/// CAT_QUERY's three bounds, mirrored from env_catalog.h: the statement the
/// parser accepts (SQL_MAX_TEXT_LEN), the rows one page may carry, and the
/// body's byte ceiling. A client that respects the first two never learns
/// the third the hard way; `more` is the service telling it a page stopped
/// at a bound rather than at the end of the result.
pub const CAT_SQL_MAX: usize = 512;
pub const CAT_QUERY_ROWS_MAX: u8 = 4;
pub const CAT_QUERY_BODY_MAX: usize = 3072;

/// The 16-byte CAT_* frame header — the payload of a channel MSG envelope.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatFrame {
    pub magic: [u8; 8],
    pub version: u16,
    pub ty: u16,
    pub flags: u16,
    pub pad: u16,
}

impl CatFrame {
    pub const SIZE: usize = 16;

    pub fn new(ty: u16) -> CatFrame {
        CatFrame {
            magic: CAT_MAGIC,
            version: CAT_VERSION,
            ty,
            flags: 0,
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
        self.flags & CAT_FLAG_ERROR != 0
    }

    /// Length + magic check: `parse` is how the client distinguishes a real
    /// reply from a frame that belongs to another service, so an unparseable
    /// body is a protocol failure and never a status.
    pub fn parse(b: &[u8]) -> Option<CatFrame> {
        if b.len() < Self::SIZE {
            return None;
        }
        let f = CatFrame {
            magic: read8(b, 0),
            version: read_u16(b, 8),
            ty: read_u16(b, 10),
            flags: read_u16(b, 12),
            pad: read_u16(b, 14),
        };
        if f.magic != CAT_MAGIC || f.version != CAT_VERSION {
            return None;
        }
        Some(f)
    }
}

/// One CAT_LIST entry: the kernel's fixed slot, decoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatListEntry {
    pub obj_type: u8,
    pub tier: u8,
    pub name: [u8; CAT_MAX_NAME],
    pub name_len: u8,
}

impl CatListEntry {
    /// The name as text. The kernel writes NUL-terminated ASCII here; an
    /// unterminated slot (a full 64-byte name) is truncated rather than read
    /// past its buffer.
    pub fn name_str(&self) -> &str {
        core::str::from_utf8(&self.name[..self.name_len as usize]).unwrap_or("")
    }
}

/// CAT_INFO reply body: `{max_objects u32, max_name u32, max_value u32,
/// partition u32}` (16 bytes). The last field is the partition the KERNEL
/// computed this boundary from — the identity a tenant can verify rather
/// than trust.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatInfo {
    pub max_objects: u32,
    pub max_name: u32,
    pub max_value: u32,
    pub partition: u32,
}

/// CAT_LIST reply head: how many entries this page holds, how many the
/// namespace holds in total, and the partition the listing was scoped to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatListHead {
    pub count: u16,
    pub total: u16,
    pub partition: u32,
}

/// CAT_STAT reply body: `{type u8, tier u8, pad u16, size_pages u32,
/// owner_uid u32, database_id u32}` (16 bytes).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatStat {
    pub obj_type: u8,
    pub tier: u8,
    pub size_pages: u32,
    pub owner_uid: u32,
    pub database_id: u32,
}

/// CAT_READ reply body: `{key_len u8, pad u8, value_len u16, key[64],
/// value[value_len]}`. Owned, because the value outlives the reply buffer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CatValue {
    pub key: alloc::vec::Vec<u8>,
    pub value: alloc::vec::Vec<u8>,
}

/// The status word of a full reply (header included), or None when the
/// buffer is too short to hold one. Read on every reply before anything
/// else — clean or refused.
pub fn cat_status(reply: &[u8]) -> Option<u16> {
    if reply.len() < CAT_STATUS + 2 {
        return None;
    }
    Some(read_u16(reply, CAT_STATUS))
}

/// The rendered refusal text of a failed reply — the bytes the service put
/// at CAT_BODY ("CAT_STAT refused by kernel.env.catalog — …"). §9.1's whole
/// point is that a refusal is visible in the CALLER's transcript, so the
/// client surfaces it verbatim rather than reducing it to a number.
pub fn cat_text(reply: &[u8]) -> Option<&[u8]> {
    if reply.len() <= CAT_BODY {
        return None;
    }
    Some(&reply[CAT_BODY..])
}

/// Status codes as names, for a client that has no text to show (a status
/// read from a truncated frame). Mirrors the kernel's verb-name table.
pub fn cat_status_name(status: u16) -> &'static str {
    match status {
        CAT_OK => "OK",
        CAT_ERR_NOENT => "NOENT",
        CAT_ERR_PERM => "PERM",
        CAT_ERR_SHAPE => "SHAPE",
        CAT_ERR_TYPE => "TYPE",
        CAT_ERR_QUERY => "QUERY",
        CAT_ERR_UNKNOWN => "UNKNOWN",
        _ => "?",
    }
}

/// CAT_LIST request body: `{page u16, page_size u16}` (4 bytes).
pub fn encode_cat_list_body(page: u16, page_size: u16) -> [u8; 4] {
    let mut b = [0u8; 4];
    put_u16(&mut b, 0, page);
    put_u16(&mut b, 2, page_size);
    b
}

/// CAT_STAT request body: `{name_len u8, name[name_len]}`. Returns the body
/// length, or None for a name the wire cannot carry (empty, or >=
/// CAT_MAX_NAME) — which is a client-side refusal, since the service would
/// answer those CAT_ERR_SHAPE without ever looking the name up.
pub fn encode_cat_stat_body(name: &[u8], out: &mut [u8]) -> Option<usize> {
    if name.is_empty() || name.len() >= CAT_MAX_NAME || out.len() < 1 + name.len() {
        return None;
    }
    out[0] = name.len() as u8;
    out[1..1 + name.len()].copy_from_slice(name);
    Some(1 + name.len())
}

/// CAT_READ request body: `{name_len u8, name[nlen], key_len u8,
/// key[klen]}` — the layout the service's READ arm parses.
pub fn encode_cat_read_body(name: &[u8], key: &[u8], out: &mut [u8]) -> Option<usize> {
    if name.is_empty() || name.len() >= CAT_MAX_NAME
        || key.len() >= CAT_MAX_KEY
        || out.len() < 2 + name.len() + key.len()
    {
        return None;
    }
    out[0] = name.len() as u8;
    out[1..1 + name.len()].copy_from_slice(name);
    out[1 + name.len()] = key.len() as u8;
    let k = 2 + name.len();
    out[k..k + key.len()].copy_from_slice(key);
    Some(2 + name.len() + key.len())
}

/// CAT_INFO reply body (starting at CAT_BODY): 16 bytes as four u32s.
pub fn parse_cat_info_body(body: &[u8]) -> Option<CatInfo> {
    if body.len() < 16 {
        return None;
    }
    Some(CatInfo {
        max_objects: read_u32(body, 0),
        max_name: read_u32(body, 4),
        max_value: read_u32(body, 8),
        partition: read_u32(body, 12),
    })
}

/// CAT_LIST reply head (starting at CAT_BODY): count, total, partition.
pub fn parse_cat_list_head(body: &[u8]) -> Option<CatListHead> {
    if body.len() < 8 {
        return None;
    }
    Some(CatListHead {
        count: read_u16(body, 0),
        total: read_u16(body, 2),
        partition: read_u32(body, 4),
    })
}

/// One CAT_LIST entry, read at `i` (0-based) from a reply body. The entry
/// count comes from the head, never from the reply's byte length, so a
/// short reply is a parse failure rather than a partial entry.
pub fn parse_cat_list_entry(body: &[u8], i: u16) -> Option<CatListEntry> {
    let off = 8 + i as usize * CAT_LIST_ENTRY;
    if body.len() < off + CAT_LIST_ENTRY {
        return None;
    }
    let mut name = [0u8; CAT_MAX_NAME];
    name.copy_from_slice(&body[off + 4..off + 4 + CAT_MAX_NAME]);
    let name_len = name.iter().position(|&c| c == 0).unwrap_or(CAT_MAX_NAME) as u8;
    Some(CatListEntry {
        obj_type: body[off],
        tier: body[off + 1],
        name,
        name_len,
    })
}

/// CAT_STAT reply body: 16 bytes as the object's own metadata.
pub fn parse_cat_stat_body(body: &[u8]) -> Option<CatStat> {
    if body.len() < 16 {
        return None;
    }
    Some(CatStat {
        obj_type: body[0],
        tier: body[1],
        size_pages: read_u32(body, 4),
        owner_uid: read_u32(body, 8),
        database_id: read_u32(body, 12),
    })
}

/// One cell of a CAT_QUERY row: SQL NULL is a first-class answer rather
/// than an empty string, which is the distinction rowstore.h's `null_mask`
/// exists to preserve and a shell needs to print `NULL` instead of nothing.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CatCell {
    pub null: bool,
    pub value: alloc::vec::Vec<u8>,
}

impl CatCell {
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.value).unwrap_or("")
    }
}

/// One page of CAT_QUERY rows: the head, the column names the service
/// reported, and the rows' cells. Owned, because the reply buffer does not
/// outlive the call — same reason CatValue is.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CatRows {
    pub ncols: u8,
    pub nrows: u8,
    pub more: bool,
    pub truncated: bool,
    pub page: u16,
    pub total: u32,
    pub columns: alloc::vec::Vec<alloc::vec::Vec<u8>>,
    pub rows: alloc::vec::Vec<alloc::vec::Vec<CatCell>>,
}

/// CAT_QUERY request body: `{sql_len u16, page u16, limit u8, flags u8,
/// sql[sql_len]}`. Returns the body length, or None for a statement the wire
/// cannot carry (empty, longer than CAT_SQL_MAX, a NUL inside it — a NUL
/// would make the kernel parse a prefix of what the caller believes it sent
/// — or a limit outside 1..=CAT_QUERY_ROWS_MAX, which the service answers
/// CAT_ERR_SHAPE and this refuses without a round trip).
pub fn encode_cat_query_body(sql: &[u8], page: u16, limit: u8, out: &mut [u8]) -> Option<usize> {
    if sql.is_empty()
        || sql.len() > CAT_SQL_MAX
        || sql.contains(&0)
        || limit == 0
        || limit > CAT_QUERY_ROWS_MAX
        || out.len() < 6 + sql.len()
    {
        return None;
    }
    put_u16(out, 0, sql.len() as u16);
    put_u16(out, 2, page);
    out[4] = limit;
    out[5] = 0; // flags: reserved, must be 0
    out[6..6 + sql.len()].copy_from_slice(sql);
    Some(6 + sql.len())
}

/// CAT_QUERY reply body (starting at CAT_BODY): the head, then the column
/// names, then each row's cells. Every length is read off the body itself
/// and bounds-checked against it — a reply that does not hold what its own
/// head promises is a parse failure (EPROTO), never a partial row.
pub fn parse_cat_rows(body: &[u8]) -> Option<CatRows> {
    if body.len() < 12 {
        return None;
    }
    let ncols = body[0] as usize;
    let nrows = body[1] as usize;
    let more = body[2] != 0;
    let truncated = body[3] != 0;
    let page = read_u16(body, 4);
    let total = read_u32(body, 8);
    if ncols > 16 {
        return None;
    }
    let mut off = 12;
    let mut columns = alloc::vec::Vec::with_capacity(ncols);
    for _ in 0..ncols {
        if off >= body.len() {
            return None;
        }
        let n = body[off] as usize;
        off += 1;
        if off + n > body.len() {
            return None;
        }
        columns.push(alloc::vec::Vec::from(&body[off..off + n]));
        off += n;
    }
    let mut rows = alloc::vec::Vec::with_capacity(nrows);
    for _ in 0..nrows {
        let mut row = alloc::vec::Vec::with_capacity(ncols);
        for _ in 0..ncols {
            if off + 2 > body.len() {
                return None;
            }
            let flags = body[off];
            let n = body[off + 1] as usize;
            off += 2;
            if off + n > body.len() {
                return None;
            }
            row.push(CatCell {
                null: flags & 1 != 0,
                value: alloc::vec::Vec::from(&body[off..off + n]),
            });
            off += n;
        }
        rows.push(row);
    }
    Some(CatRows {
        ncols: ncols as u8,
        nrows: nrows as u8,
        more,
        truncated,
        page,
        total,
        columns,
        rows,
    })
}

/// CAT_READ reply body: the echoed key and its value. The key and value
/// lengths come from the body itself (fixed slots make that unambiguous),
/// and a value that does not fit the slot the service wrote is a parse
/// failure, not a truncation.
pub fn parse_cat_read_body(body: &[u8]) -> Option<CatValue> {
    if body.len() < 4 {
        return None;
    }
    let klen = body[0] as usize;
    let vlen = read_u16(body, 2) as usize;
    let key_end = 4 + klen;
    let val_end = 4 + CAT_MAX_KEY + vlen;
    if klen > CAT_MAX_KEY || body.len() < val_end || key_end > body.len() {
        return None;
    }
    Some(CatValue {
        key: alloc::vec::Vec::from(&body[4..key_end]),
        value: alloc::vec::Vec::from(&body[4 + CAT_MAX_KEY..val_end]),
    })
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
        assert_eq!(&CAT_MAGIC, b"AEROSCT\x01");
    }

    // ── CAT_*: §9.1's mediated catalog ───────────────────────────────────

    #[test]
    fn cat_frame_roundtrips_and_refuses_a_foreign_frame() {
        let f = CatFrame::new(CAT_LIST);
        let bytes = f.encode();
        assert_eq!(CatFrame::parse(&bytes).unwrap(), f);
        assert_eq!(f.ty, CAT_LIST);
        assert!(!f.is_error());
        assert_eq!(read_u16(&CatFrame::new(CAT_STAT).encode(), 12), 0);

        // The magic is one byte from NET_MAGIC on purpose — the two services
        // must not be able to parse each other's traffic. A NET_* frame on a
        // catalog channel parses to None, and a truncated header likewise,
        // so a client treats either as protocol failure, never as a status.
        let net = NetFrame::new(NET_INFO, false).encode();
        let differing = net
            .iter()
            .zip(CAT_MAGIC.iter())
            .filter(|(a, b)| a != b)
            .count();
        assert_eq!(differing, 1, "one byte apart, and that byte is the whole distinction");
        assert!(CatFrame::parse(&net).is_none());
        assert!(CatFrame::parse(&bytes[..8]).is_none());
    }

    #[test]
    fn cat_status_is_read_from_the_status_word_not_the_first_body_word() {
        // The offset bug the kernel header records: a body written over the
        // status word made a two-object CAT_LIST read as status 2 (PERM).
        // This is that reply, laid out the way the service lays it out, read
        // back the way a client reads it.
        let mut reply = [0u8; CAT_LIST_HDR + 2 * CAT_LIST_ENTRY];
        reply[..16].copy_from_slice(&CatFrame::new(CAT_LIST).encode());
        put_u16(&mut reply, CAT_STATUS, CAT_OK);
        put_u16(&mut reply, CAT_BODY, 2); // count = 2 — the word at CAT_HDR
        put_u16(&mut reply, CAT_BODY + 2, 4);
        put_u32(&mut reply, CAT_BODY + 4, 3);

        assert_eq!(cat_status(&reply), Some(CAT_OK), "status must not be 2");
        let head = parse_cat_list_head(&reply[CAT_BODY..]).unwrap();
        assert_eq!((head.count, head.total, head.partition), (2, 4, 3));

        // And a refusal reads its status from the same place, with the
        // service's rendered text starting at CAT_BODY.
        let text = b"CAT_STAT refused by kernel.env.catalog: no read";
        let mut bad = [0u8; 96];
        bad[..16].copy_from_slice(&CatFrame::new(CAT_STAT).encode());
        bad[12..14].copy_from_slice(&CAT_FLAG_ERROR.to_le_bytes());
        put_u16(&mut bad, CAT_STATUS, CAT_ERR_PERM);
        bad[CAT_BODY..CAT_BODY + text.len()].copy_from_slice(text);
        assert_eq!(cat_status(&bad), Some(CAT_ERR_PERM));
        assert_eq!(&cat_text(&bad).unwrap()[..text.len()], &text[..]);
    }

    #[test]
    fn cat_list_entries_come_out_of_the_fixed_slots() {
        let mut reply = [0u8; CAT_LIST_HDR + CAT_LIST_ENTRY];
        reply[..16].copy_from_slice(&CatFrame::new(CAT_LIST).encode());
        put_u16(&mut reply, CAT_STATUS, CAT_OK);
        put_u16(&mut reply, CAT_BODY, 1);
        put_u16(&mut reply, CAT_BODY + 2, 7);
        put_u32(&mut reply, CAT_BODY + 4, 5);
        let off = CAT_LIST_HDR;
        reply[off] = 3; // type
        reply[off + 1] = 1; // tier
        reply[off + 4..off + 4 + 7].copy_from_slice(b"app.tab");

        let body = &reply[CAT_BODY..];
        let head = parse_cat_list_head(body).unwrap();
        assert_eq!(head.count, 1);
        let e = parse_cat_list_entry(body, 0).unwrap();
        assert_eq!(e.name_str(), "app.tab");
        assert_eq!((e.obj_type, e.tier), (3, 1));
        // An entry past the reply's own bytes is a parse failure, never a
        // partial name read past the buffer.
        assert!(parse_cat_list_entry(body, 1).is_none());
    }

    #[test]
    fn cat_request_bodies_refuse_what_the_service_would_answer_shape() {
        let mut out = [0u8; 256];
        // Empty and over-long names are refused by the CLIENT, because the
        // service answers exactly those CAT_ERR_SHAPE without a lookup —
        // sending them would buy a round trip to learn nothing.
        assert_eq!(encode_cat_stat_body(b"", &mut out), None);
        assert_eq!(encode_cat_stat_body(&[b'x'; 64], &mut out), None);
        let n = encode_cat_stat_body(b"app.tab", &mut out).unwrap();
        assert_eq!(n, 8);
        assert_eq!(&out[..n], b"\x07app.tab");
        assert_eq!(encode_cat_stat_body(b"app.tab", &mut [0u8; 4]), None);

        assert_eq!(encode_cat_read_body(b"", &[b'k'], &mut out), None);
        assert_eq!(encode_cat_read_body(b"t", &[b'k'; 64], &mut out), None);
    }

    #[test]
    fn cat_read_reply_roundtrips_key_and_value() {
        let mut reply = [0u8; CAT_BODY + 4 + CAT_MAX_KEY + 5];
        reply[..16].copy_from_slice(&CatFrame::new(CAT_READ).encode());
        put_u16(&mut reply, CAT_STATUS, CAT_OK);
        reply[CAT_BODY] = 6; // key_len
        put_u16(&mut reply, CAT_BODY + 2, 5); // value_len
        reply[CAT_BODY + 4..CAT_BODY + 10].copy_from_slice(b"field1");
        reply[CAT_BODY + 4 + CAT_MAX_KEY..CAT_BODY + 4 + CAT_MAX_KEY + 5]
            .copy_from_slice(b"value");

        let v = parse_cat_read_body(&reply[CAT_BODY..]).unwrap();
        assert_eq!(v.key, b"field1");
        assert_eq!(v.value, b"value");
        // A value_len the slot cannot hold is a parse failure, not a
        // truncated read.
        let mut lying = reply[CAT_BODY..].to_vec();
        put_u16(&mut lying, 2, 4096);
        assert!(parse_cat_read_body(&lying).is_none());
    }

    #[test]
    fn cat_stat_body_decodes_the_objects_own_metadata() {
        let mut body = [0u8; 16];
        body[0] = 4; // type
        body[1] = 2; // tier
        put_u32(&mut body, 4, 32);
        put_u32(&mut body, 8, 1001);
        put_u32(&mut body, 12, 7);
        let s = parse_cat_stat_body(&body).unwrap();
        assert_eq!(s.obj_type, 4);
        assert_eq!(s.tier, 2);
        assert_eq!((s.size_pages, s.owner_uid, s.database_id), (32, 1001, 7));
        assert!(parse_cat_stat_body(&body[..15]).is_none());
    }

    #[test]
    fn cat_info_answers_the_partition_the_boundary_computed_from() {
        let mut body = [0u8; 16];
        put_u32(&mut body, 0, 128);
        put_u32(&mut body, 4, 64);
        put_u32(&mut body, 8, 256);
        put_u32(&mut body, 12, 5);
        let i = parse_cat_info_body(&body).unwrap();
        assert_eq!(
            (i.max_objects, i.max_name, i.max_value, i.partition),
            (128, 64, 256, 5)
        );
        assert!(parse_cat_info_body(&body[..12]).is_none());
        assert_eq!(cat_status_name(CAT_ERR_NOENT), "NOENT");
        assert_eq!(cat_status_name(0xBEEF), "?");
    }
}
