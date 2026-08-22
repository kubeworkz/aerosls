//! Wire protocol for the two-process kernel transport.
//!
//! `sls-kerneld` implements the channel + arena semantics of the Polyglot
//! Nexus syscalls (290 arena-alloc, 295 map, 296 unmap, 302 send-msg,
//! 303 recv-msg, 304 arena-free) over a length-prefixed TCP framing. The
//! request bodies are the *exact* wire layouts the ABI-pinning suite
//! (`tools/aeroidl/tests/cross_lang_constants.rs`) anchors: the kernel
//! `SLSCapSendMsgRequest`/`SLSCapRecvMsgRequest` field order, cap
//! descriptors, and error codes. Only the pointers the real kernel resolves
//! internally are replaced by the payload bytes themselves.
//!
//! Frame: `[u32 LE body_len][u32 LE syscall_num][body...]`
//!
//! Syscall bodies:
//!   290 ARENA_ALLOC in :  [npages u32][perm u32]
//!                     out: [rc i32][cap u16][offset u32][len u32]
//!   295 MAP / 296 UNMAP  out: [rc i32]                      (no-op; the
//!                     sidecars map the arena file once at boot)
//!   302 SEND_MSG    in :  [ch_w u16][n_caps u16][tag u32][flags u32]
//!                         + n_caps × [slot u16][offset u32][len u32]
//!                                        [rights u8][flags u8]   (12 B)
//!                         + payload bytes
//!                     out: [rc i32]
//!   303 RECV_MSG    in :  [ch_r u16][buf_len u32]
//!                     out: [rc i32][out_payload_len u32][out_tag u32]
//!                         [out_flags u32][out_n_caps u16]
//!                         + n × 12 B cap descriptors
//!                         + payload bytes
//!   304 ARENA_FREE  in :  [cap_idx u16]
//!                     out: [rc i32]
//!
//! All integers little-endian. The cap descriptor here is the kernel-side
//! `CapDesc` (slot u16, offset u32, len u32, rights u8, flags u8, pad u16).
//!
//! Error codes: rc is 0 on success or a negative `CAP_E*` (the runtime
//! widens negatives through u64 the same way the kernel ABI does).

use std::io::{Read, Write};
use std::net::TcpStream;

pub const SYS_ARENA_ALLOC: u32 = 290;
pub const SYS_CAP_MAP: u32 = 295;
pub const SYS_CAP_UNMAP: u32 = 296;
pub const SYS_CAP_SEND_MSG: u32 = 302;
pub const SYS_CAP_RECV_MSG: u32 = 303;
pub const SYS_CAP_ARENA_FREE: u32 = 304;

/// The 12-byte wire cap descriptor (kernel-side CapDesc, fields in order).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WireDesc {
    pub slot: u16,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    pub flags: u8,
}

impl WireDesc {
    pub const SIZE: usize = 12;
}

/// Write one frame: `[body_len u32][syscall u32][body]`.
pub fn write_frame(stream: &mut TcpStream, syscall: u32, body: &[u8]) -> std::io::Result<()> {
    let mut hdr = [0u8; 8];
    hdr[0..4].copy_from_slice(&(body.len() as u32).to_le_bytes());
    hdr[4..8].copy_from_slice(&syscall.to_le_bytes());
    // Single write: avoids the Nagle/delayed-ACK interaction between the
    // header and body packets on request/response exchanges.
    let mut frame = Vec::with_capacity(8 + body.len());
    frame.extend_from_slice(&hdr);
    frame.extend_from_slice(body);
    stream.write_all(&frame)?;
    stream.flush()
}

/// Read one frame; returns `(syscall, body)`. Blocks until a full frame or EOF.
pub fn read_frame(stream: &mut TcpStream) -> std::io::Result<(u32, Vec<u8>)> {
    let mut hdr = [0u8; 8];
    stream.read_exact(&mut hdr)?;
    let body_len = u32::from_le_bytes(hdr[0..4].try_into().unwrap()) as usize;
    let syscall = u32::from_le_bytes(hdr[4..8].try_into().unwrap());
    let mut body = vec![0u8; body_len];
    stream.read_exact(&mut body)?;
    Ok((syscall, body))
}

// ── body builders (sidecar → kernel) ───────────────────────────────────────

pub fn encode_alloc_in(npages: u32, perm: u32) -> Vec<u8> {
    let mut b = Vec::with_capacity(8);
    b.extend_from_slice(&npages.to_le_bytes());
    b.extend_from_slice(&perm.to_le_bytes());
    b
}

pub fn encode_send_in(
    ch_w: u16,
    n_caps: u16,
    tag: u32,
    flags: u32,
    descs: &[WireDesc],
    payload: &[u8],
) -> Vec<u8> {
    let mut b = Vec::with_capacity(12 + descs.len() * WireDesc::SIZE + payload.len());
    b.extend_from_slice(&ch_w.to_le_bytes());
    b.extend_from_slice(&n_caps.to_le_bytes());
    b.extend_from_slice(&tag.to_le_bytes());
    b.extend_from_slice(&flags.to_le_bytes());
    for d in descs {
        b.extend_from_slice(&d.slot.to_le_bytes());
        b.extend_from_slice(&d.offset.to_le_bytes());
        b.extend_from_slice(&d.len.to_le_bytes());
        b.push(d.rights);
        b.push(d.flags);
        // 2 + 4 + 4 + 1 + 1 = 12 bytes — the pinned CapDesc wire layout
    }
    b.extend_from_slice(payload);
    b
}

pub fn encode_recv_in(ch_r: u16, buf_len: u32) -> Vec<u8> {
    let mut b = Vec::with_capacity(6);
    b.extend_from_slice(&ch_r.to_le_bytes());
    b.extend_from_slice(&buf_len.to_le_bytes());
    b
}

pub fn encode_free_in(cap_idx: u16) -> Vec<u8> {
    cap_idx.to_le_bytes().to_vec()
}

// ── body parsers (kernel → sidecar) ────────────────────────────────────────

/// Parsed `SEND_MSG` request body.
pub struct SendIn {
    pub ch_w: u16,
    pub tag: u32,
    pub flags: u32,
    pub descs: Vec<WireDesc>,
    pub payload: Vec<u8>,
}

pub fn parse_send_in(body: &[u8]) -> Option<SendIn> {
    if body.len() < 12 {
        return None;
    }
    let ch_w = u16::from_le_bytes(body[0..2].try_into().ok()?);
    let n_caps = u16::from_le_bytes(body[2..4].try_into().ok()?);
    let tag = u32::from_le_bytes(body[4..8].try_into().ok()?);
    let flags = u32::from_le_bytes(body[8..12].try_into().ok()?);
    let need = 12 + n_caps as usize * WireDesc::SIZE;
    if body.len() < need {
        return None;
    }
    let mut descs = Vec::with_capacity(n_caps as usize);
    for i in 0..n_caps as usize {
        let o = 12 + i * WireDesc::SIZE;
        descs.push(WireDesc {
            slot: u16::from_le_bytes(body[o..o + 2].try_into().ok()?),
            offset: u32::from_le_bytes(body[o + 2..o + 6].try_into().ok()?),
            len: u32::from_le_bytes(body[o + 6..o + 10].try_into().ok()?),
            rights: body[o + 10],
            flags: body[o + 11],
        });
    }
    Some(SendIn {
        ch_w,
        tag,
        flags,
        descs,
        payload: body[need..].to_vec(),
    })
}

/// Encode a `RECV_MSG` reply body: rc + out fields + cap descriptors + payload.
pub fn encode_recv_out(
    rc: i32,
    payload: &[u8],
    tag: u32,
    flags: u32,
    descs: &[WireDesc],
) -> Vec<u8> {
    let mut b = Vec::with_capacity(22 + descs.len() * WireDesc::SIZE + payload.len());
    b.extend_from_slice(&rc.to_le_bytes());
    b.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    b.extend_from_slice(&tag.to_le_bytes());
    b.extend_from_slice(&flags.to_le_bytes());
    b.extend_from_slice(&(descs.len() as u16).to_le_bytes());
    for d in descs {
        b.extend_from_slice(&d.slot.to_le_bytes());
        b.extend_from_slice(&d.offset.to_le_bytes());
        b.extend_from_slice(&d.len.to_le_bytes());
        b.push(d.rights);
        b.push(d.flags);
        // 12 bytes per descriptor
    }
    b.extend_from_slice(payload);
    b
}

/// Parsed `RECV_MSG` reply body.
pub struct RecvOut {
    pub rc: i32,
    pub payload: Vec<u8>,
    pub tag: u32,
    pub flags: u32,
    pub descs: Vec<WireDesc>,
}

pub fn parse_recv_out(body: &[u8]) -> Option<RecvOut> {
    if body.len() < 18 {
        return None;
    }
    let rc = i32::from_le_bytes(body[0..4].try_into().ok()?);
    let plen = u32::from_le_bytes(body[4..8].try_into().ok()?) as usize;
    let tag = u32::from_le_bytes(body[8..12].try_into().ok()?);
    let flags = u32::from_le_bytes(body[12..16].try_into().ok()?);
    let n = u16::from_le_bytes(body[16..18].try_into().ok()?) as usize;
    let need = 18 + n * WireDesc::SIZE;
    if body.len() < need + plen {
        return None;
    }
    let mut descs = Vec::with_capacity(n);
    for i in 0..n {
        let o = 18 + i * WireDesc::SIZE;
        descs.push(WireDesc {
            slot: u16::from_le_bytes(body[o..o + 2].try_into().ok()?),
            offset: u32::from_le_bytes(body[o + 2..o + 6].try_into().ok()?),
            len: u32::from_le_bytes(body[o + 6..o + 10].try_into().ok()?),
            rights: body[o + 10],
            flags: body[o + 11],
        });
    }
    Some(RecvOut {
        rc,
        payload: body[need..need + plen].to_vec(),
        tag,
        flags,
        descs,
    })
}

/// Encode a simple `[rc i32]` reply.
pub fn encode_rc(rc: i32) -> Vec<u8> {
    rc.to_le_bytes().to_vec()
}

pub fn parse_rc(body: &[u8]) -> i32 {
    if body.len() < 4 {
        i32::MIN
    } else {
        i32::from_le_bytes(body[0..4].try_into().unwrap())
    }
}
