//! ── ENV_* protocol (Layer 3) — POSIX-Environments E4 ───────────────────────
//!
//! The wire format the control plane (the kernel's HTTP server / shell, in C)
//! speaks to the **environment manager** (init, in Rust) over init's kernel-held
//! request channel. It mirrors the RD_* protocol's shape: a 16-byte
//! magic/version/type frame — the payload of a channel MSG envelope — followed
//! by a per-opcode body.
//!
//! A request carries `ENV_CREATE { partition, index }` or
//! `ENV_DESTROY { env_id }`; the reply echoes the frame type and carries a
//! uniform `{ status, env_id, partition }` body (env_id meaningful only on an
//! `ENV_OK` create). The C control plane mirrors these constants (E4 part 3).

use crate::{put_u16, put_u32, read8, read_u16, read_u32};

pub const ENV_MAGIC: [u8; 8] = *b"AEROSEN\x01";
pub const ENV_VERSION: u16 = 1;

/// Request/reply frame types.
pub const ENV_CREATE: u16 = 1;
pub const ENV_DESTROY: u16 = 2;

/// Frame flag: set on error replies (mirrors `RD_FLAG_ERROR`).
pub const ENV_FLAG_ERROR: u16 = 0x0001;

/// Reply status codes.
pub const ENV_OK: u16 = 0;
pub const ENV_ERR_INVAL: u16 = 1; // malformed request / bad body
pub const ENV_ERR_NOMEM: u16 = 2; // the frame pool cannot back the environment
pub const ENV_ERR_PART: u16 = 3; //  absent/paused partition, or a refused create
pub const ENV_ERR_FULL: u16 = 4; //  the environment table is full
pub const ENV_ERR_UNSUPP: u16 = 5; // recognised opcode not yet built (ENV_DESTROY → E5)

/// The 16-byte ENV_* frame header — the payload of a channel MSG envelope.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EnvFrame {
    pub magic: [u8; 8],
    pub version: u16,
    pub ty: u16,
    pub flags: u16,
    pub pad: u16,
}

impl EnvFrame {
    pub const SIZE: usize = 16;

    pub fn new(ty: u16, error: bool) -> EnvFrame {
        EnvFrame {
            magic: ENV_MAGIC,
            version: ENV_VERSION,
            ty,
            flags: if error { ENV_FLAG_ERROR } else { 0 },
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
        self.flags & ENV_FLAG_ERROR != 0
    }

    /// Length + magic/version check; `None` on a short or foreign frame.
    pub fn parse(b: &[u8]) -> Option<EnvFrame> {
        if b.len() < Self::SIZE {
            return None;
        }
        let f = EnvFrame {
            magic: read8(b, 0),
            version: read_u16(b, 8),
            ty: read_u16(b, 10),
            flags: read_u16(b, 12),
            pad: read_u16(b, 14),
        };
        if f.magic != ENV_MAGIC || f.version != ENV_VERSION {
            return None;
        }
        Some(f)
    }
}

/// `ENV_CREATE` request body: `{ partition u32, index u32 }` (8 bytes). `index`
/// is the environment's identity within its partition — the sidecar names
/// `drv.ramdisk.<index>` / `aerosls.posix.<index>` derive from it, and a
/// human-readable name maps to an index at the control plane.
pub fn encode_create_body(partition: u32, index: u32) -> [u8; 8] {
    let mut b = [0u8; 8];
    put_u32(&mut b, 0, partition);
    put_u32(&mut b, 4, index);
    b
}

pub fn parse_create_body(p: &[u8]) -> Option<(u32, u32)> {
    if p.len() < 8 {
        return None;
    }
    Some((read_u32(p, 0), read_u32(p, 4)))
}

/// `ENV_DESTROY` request body: `{ env_id u32 }` (4 bytes).
pub fn encode_destroy_body(env_id: u32) -> [u8; 4] {
    let mut b = [0u8; 4];
    put_u32(&mut b, 0, env_id);
    b
}

pub fn parse_destroy_body(p: &[u8]) -> Option<u32> {
    if p.len() < 4 {
        return None;
    }
    Some(read_u32(p, 0))
}

/// The uniform reply body: `{ status u16, pad u16, env_id u32, partition u32 }`
/// (12 bytes). `env_id`/`partition` are meaningful only on an `ENV_OK` create.
pub fn encode_reply_body(status: u16, env_id: u32, partition: u32) -> [u8; 12] {
    let mut b = [0u8; 12];
    put_u16(&mut b, 0, status);
    put_u16(&mut b, 2, 0);
    put_u32(&mut b, 4, env_id);
    put_u32(&mut b, 8, partition);
    b
}

pub fn parse_reply_body(p: &[u8]) -> Option<(u16, u32, u32)> {
    if p.len() < 12 {
        return None;
    }
    Some((read_u16(p, 0), read_u32(p, 4), read_u32(p, 8)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_round_trips_and_rejects_foreign() {
        let enc = EnvFrame::new(ENV_CREATE, false).encode();
        let f = EnvFrame::parse(&enc).expect("valid frame parses");
        assert_eq!(f.ty, ENV_CREATE);
        assert!(!f.is_error());
        // Wrong magic is rejected.
        let mut bad = enc;
        bad[0] = 0;
        assert!(EnvFrame::parse(&bad).is_none());
        // Short buffer is rejected.
        assert!(EnvFrame::parse(&enc[..8]).is_none());
    }

    #[test]
    fn bodies_round_trip() {
        assert_eq!(parse_create_body(&encode_create_body(7, 3)), Some((7, 3)));
        assert_eq!(parse_destroy_body(&encode_destroy_body(42)), Some(42));
        assert_eq!(parse_reply_body(&encode_reply_body(ENV_OK, 9, 7)), Some((ENV_OK, 9, 7)));
    }
}
