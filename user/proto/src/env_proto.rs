//! ── ENV_* protocol (Layer 3) — POSIX-Environments E4 ───────────────────────
//!
//! The wire format the control plane (the kernel's HTTP server / shell, in C)
//! speaks to the **environment manager** (init, in Rust) over init's kernel-held
//! request channel. It mirrors the RD_* protocol's shape: a 16-byte
//! magic/version/type frame — the payload of a channel MSG envelope — followed
//! by a per-opcode body.
//!
//! A request carries `ENV_CREATE { partition, index }` or
//! `ENV_DESTROY { env_id, partition }`; the reply echoes the frame type and a
//! uniform `{ status, env_id, partition }` body (env_id meaningful only on an
//! `ENV_OK` create). The C control plane mirrors these constants (E4 part 3).

use crate::{put_u16, put_u32, read8, read_u16, read_u32};

pub const ENV_MAGIC: [u8; 8] = *b"AEROSEN\x01";
pub const ENV_VERSION: u16 = 1;

/// Request/reply frame types.
pub const ENV_CREATE: u16 = 1;
pub const ENV_DESTROY: u16 = 2;
/// P1a: "describe environment `env_id`, which must live in `partition`, so the
/// kernel can register its checkpoint record". The reply body is NOT the
/// uniform one — see [`RegisterReply`].
pub const ENV_REGISTER: u16 = 3;

/// Frame flag: set on error replies (mirrors `RD_FLAG_ERROR`).
pub const ENV_FLAG_ERROR: u16 = 0x0001;

/// Reply status codes.
pub const ENV_OK: u16 = 0;
pub const ENV_ERR_INVAL: u16 = 1; // malformed request / bad body
pub const ENV_ERR_NOMEM: u16 = 2; // the frame pool cannot back the environment
pub const ENV_ERR_PART: u16 = 3; //  absent/paused partition, or a refused create
pub const ENV_ERR_FULL: u16 = 4; //  the environment table is full
pub const ENV_ERR_UNSUPP: u16 = 5; // recognised opcode not yet built
pub const ENV_ERR_NOENT: u16 = 6; //  no environment with that env_id (E5)
pub const ENV_ERR_QUOTA: u16 = 7; //  the environment's durable store does not fit
                                  //  the partition's storage quota (P1b) — its own
                                  //  code, never a frame-pool NOMEM

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

/// `ENV_DESTROY` request body: `{ env_id u32, partition u32 }` (8 bytes).
///
/// The partition is carried, not assumed, because the control plane's destroy
/// route is nested under the partition it names (`POST
/// /api/partition/{id}/env/destroy`). Without it a caller could end partition
/// B's environment through a path that names partition A: the env id alone
/// identifies the environment globally, so nothing else in the request would
/// contradict the path. It mirrors `ENV_CREATE`'s `{ partition, index }` — the
/// environment is named by (partition, its identity within it) on both paths.
pub fn encode_destroy_body(env_id: u32, partition: u32) -> [u8; 8] {
    let mut b = [0u8; 8];
    put_u32(&mut b, 0, env_id);
    put_u32(&mut b, 4, partition);
    b
}

pub fn parse_destroy_body(p: &[u8]) -> Option<(u32, u32)> {
    if p.len() < 8 {
        return None;
    }
    Some((read_u32(p, 0), read_u32(p, 4)))
}

// ── ENV_REGISTER (POSIX-Environments v0.2, P1a) ─────────────────────────────
//
// The environment manager is the only side that knows three of the facts a
// checkpoint record needs: the addresses and sizes of the three frame-pool
// regions it allocated for the environment, the four messenger endpoints init
// holds to the environment's sidecars, and the registry names those sidecars
// were created under. ENV_REGISTER is how the kernel asks for them.
//
// The reply's byte layout is the C kernel's `struct EnvCkptRegister`
// (kernel/env_ckpt.h, encoded/decoded in kernel/env_proto.h). Every count, the
// name length and the field order below MUST match it -- tests/
// env_register_pin_check.sh reads both files and refuses to let them drift,
// which is the only thing standing between a rename on one side and a record
// assembled out of another field's bytes.

/// `ENV_REGISTER` request body: `{ env_id u32, partition u32 }` (8 bytes).
pub const REGISTER_BODY_SIZE: usize = 8;

/// How many of each thing the body carries -- the kernel's `ENV_CKPT_*MAX*`.
pub const REGISTER_MAX_REGIONS: usize = 3;
pub const REGISTER_MAX_CHANS: usize = 4;
pub const REGISTER_MAX_TASKS: usize = 4;
/// A task name's field width -- the kernel's `ENV_CKPT_NAME_LEN`.
pub const REGISTER_NAME_LEN: usize = 24;

/// Region kinds (kernel `ENV_CKPT_REGION_*`). Sent with each region rather than
/// implied by its position, so a swap between the two sides is a refusal
/// instead of a heap silently reattached under the other one's name.
pub const REGION_POSIX_HEAP: u32 = 1;
pub const REGION_RD_HEAP: u32 = 2;
pub const REGION_RD_STORAGE: u32 = 3;

/// Task kinds (kernel `ENV_CKPT_TASK_*`). 2 is E7's Linux task, unused today;
/// the kernel refuses a kind it does not know rather than skipping it.
pub const TASK_POSIX_SIDECAR: u32 = 0;
pub const TASK_RAMDISK_SIDECAR: u32 = 1;
pub const TASK_LINUX: u32 = 2;

/// Byte offsets inside the reply body -- the layout, named rather than spelled
/// inline, because these ARE the cross-language contract.
pub const REG_OFF_PARTITION: usize = 0;
pub const REG_OFF_INDEX: usize = 4;
pub const REG_OFF_ENV_ID: usize = 8;
pub const REG_OFF_N_REGIONS: usize = 12;
pub const REG_OFF_REGIONS: usize = 16;
pub const REG_REGION_STRIDE: usize = 16; // { base u64, frames u32, kind u32 }
pub const REG_OFF_N_CHANS: usize = REG_OFF_REGIONS + REGISTER_MAX_REGIONS * REG_REGION_STRIDE;
pub const REG_OFF_CHANS: usize = REG_OFF_N_CHANS + 4;
pub const REG_OFF_N_TASKS: usize = REG_OFF_CHANS + REGISTER_MAX_CHANS * 4;
pub const REG_OFF_TASK_NAMES: usize = REG_OFF_N_TASKS + 4;
pub const REG_OFF_TASK_KINDS: usize =
    REG_OFF_TASK_NAMES + REGISTER_MAX_TASKS * REGISTER_NAME_LEN;

/// The reply body's length. 16 + 3*16 + 4 + 4*4 + 4 + 4*24 + 4*4 = 200.
pub const REGISTER_REPLY_BODY_SIZE: usize = REG_OFF_TASK_KINDS + REGISTER_MAX_TASKS * 4;

/// The largest reply the kernel's `env_service` will accept (frame + the widest
/// body). init's dispatch loop sizes its reply buffer with this.
pub const REPLY_MAX: usize = EnvFrame::SIZE + REGISTER_REPLY_BODY_SIZE;

/// One private region as the manager reports it: its frame base, its length in
/// frames, and which of the three it is. `#[repr(C)]` so its size is the 16
/// bytes the wire stride says, and so a field added here without being added to
/// `encode` shows up as a size the test below pins but the encoder ignores.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct RegisterRegion {
    pub base: u64,
    pub frames: u32,
    pub kind: u32,
}

/// An `ENV_REGISTER` reply: the environment's identity, its three regions, the
/// four messenger endpoints init holds, and the two sidecar names it created.
///
/// Flat fields and `#[repr(C)]` rather than a nested shape, deliberately: this
/// is an image of the kernel's record half, and the pin guard compares the two
/// field for field.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegisterReply {
    pub partition: u32,
    pub index: u32,
    pub env_id: u32,
    pub n_regions: u32,
    pub regions: [RegisterRegion; REGISTER_MAX_REGIONS],
    pub n_chans: u32,
    pub chans: [u32; REGISTER_MAX_CHANS],
    pub n_tasks: u32,
    pub task_names: [[u8; REGISTER_NAME_LEN]; REGISTER_MAX_TASKS],
    pub task_kinds: [u32; REGISTER_MAX_TASKS],
}

impl RegisterReply {
    /// An empty registration for `(partition, index, env_id)`: the identity set,
    /// nothing else. Callers fill in the regions, channels and tasks.
    pub fn empty(partition: u32, index: u32, env_id: u32) -> RegisterReply {
        RegisterReply {
            partition,
            index,
            env_id,
            n_regions: 0,
            regions: [RegisterRegion::default(); REGISTER_MAX_REGIONS],
            n_chans: 0,
            chans: [0; REGISTER_MAX_CHANS],
            n_tasks: 0,
            task_names: [[0u8; REGISTER_NAME_LEN]; REGISTER_MAX_TASKS],
            task_kinds: [0; REGISTER_MAX_TASKS],
        }
    }

    /// Write a task's name into slot `i`, NUL-padded to the field width. A name
    /// that does not fit is truncated here rather than at the kernel: a name
    /// with no terminator inside the field is REFUSED by the record layer, and a
    /// sidecar that could not be named is a sidecar a restore cannot re-register.
    pub fn set_task(&mut self, i: usize, name: &str, kind: u32) {
        if i >= REGISTER_MAX_TASKS {
            return;
        }
        let b = name.as_bytes();
        let n = if b.len() < REGISTER_NAME_LEN { b.len() } else { REGISTER_NAME_LEN - 1 };
        let mut slot = [0u8; REGISTER_NAME_LEN];
        slot[..n].copy_from_slice(&b[..n]);
        self.task_names[i] = slot;
        self.task_kinds[i] = kind;
        if (i + 1) as u32 > self.n_tasks {
            self.n_tasks = (i + 1) as u32;
        }
    }

    /// The wire body: little-endian, field for field, `REGISTER_REPLY_BODY_SIZE`
    /// bytes. u64 fields go out low word first, matching the C decoder.
    pub fn encode(&self) -> [u8; REGISTER_REPLY_BODY_SIZE] {
        let mut b = [0u8; REGISTER_REPLY_BODY_SIZE];
        put_u32(&mut b, REG_OFF_PARTITION, self.partition);
        put_u32(&mut b, REG_OFF_INDEX, self.index);
        put_u32(&mut b, REG_OFF_ENV_ID, self.env_id);
        put_u32(&mut b, REG_OFF_N_REGIONS, self.n_regions);
        for i in 0..REGISTER_MAX_REGIONS {
            let o = REG_OFF_REGIONS + i * REG_REGION_STRIDE;
            put_u32(&mut b, o, (self.regions[i].base & 0xFFFF_FFFF) as u32);
            put_u32(&mut b, o + 4, (self.regions[i].base >> 32) as u32);
            put_u32(&mut b, o + 8, self.regions[i].frames);
            put_u32(&mut b, o + 12, self.regions[i].kind);
        }
        put_u32(&mut b, REG_OFF_N_CHANS, self.n_chans);
        for i in 0..REGISTER_MAX_CHANS {
            put_u32(&mut b, REG_OFF_CHANS + i * 4, self.chans[i]);
        }
        put_u32(&mut b, REG_OFF_N_TASKS, self.n_tasks);
        for i in 0..REGISTER_MAX_TASKS {
            let o = REG_OFF_TASK_NAMES + i * REGISTER_NAME_LEN;
            b[o..o + REGISTER_NAME_LEN].copy_from_slice(&self.task_names[i]);
        }
        for i in 0..REGISTER_MAX_TASKS {
            put_u32(&mut b, REG_OFF_TASK_KINDS + i * 4, self.task_kinds[i]);
        }
        b
    }
}

/// `ENV_REGISTER` request body: `{ env_id u32, partition u32 }` (8 bytes). Same
/// pair, and the same reason, as `ENV_DESTROY`'s: the request names the
/// environment AND the partition it must be in, so the reply cannot be about a
/// different environment than the one asked about.
pub fn encode_register_body(env_id: u32, partition: u32) -> [u8; 8] {
    let mut b = [0u8; 8];
    put_u32(&mut b, 0, env_id);
    put_u32(&mut b, 4, partition);
    b
}

pub fn parse_register_body(p: &[u8]) -> Option<(u32, u32)> {
    if p.len() < REGISTER_BODY_SIZE {
        return None;
    }
    Some((read_u32(p, 0), read_u32(p, 4)))
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
        assert_eq!(parse_destroy_body(&encode_destroy_body(42, 7)), Some((42, 7)));
        assert_eq!(parse_reply_body(&encode_reply_body(ENV_OK, 9, 7)), Some((ENV_OK, 9, 7)));
    }

    /// The destroy body carries BOTH fields, and a body that stops short of
    /// either is refused rather than half-read: the partition is what makes
    /// the control plane's nested destroy route enforce its own `{id}`.
    #[test]
    fn destroy_body_refuses_a_partitionless_request() {
        let full = encode_destroy_body(42, 7);
        assert_eq!(full.len(), 8);
        assert_eq!(parse_destroy_body(&full[..4]), None);
        assert_eq!(parse_destroy_body(&full), Some((42, 7)));
    }

    // ── ENV_REGISTER: the layout the C side (kernel/env_ckpt.h) must match ───

    /// The struct is a padding-free image of the wire body: no gap between a
    /// field and the byte the encoder writes it to. This is the assertion that
    /// fires the day a field is added on one side and not the other.
    #[test]
    fn register_reply_struct_is_the_wire_layout() {
        use core::mem::size_of;
        assert_eq!(size_of::<RegisterRegion>(), 16);
        assert_eq!(REGISTER_REPLY_BODY_SIZE, 200);
        assert_eq!(size_of::<RegisterReply>(), REGISTER_REPLY_BODY_SIZE);
        assert_eq!(REPLY_MAX, 216);
    }

    #[test]
    fn register_body_round_trips() {
        assert_eq!(parse_register_body(&encode_register_body(42, 7)), Some((42, 7)));
        assert_eq!(parse_register_body(&encode_register_body(42, 7)[..4]), None);
    }

    /// Every field lands at the offset the kernel's `ENV_REG_OFF_*` names --
    /// asserted against literals, so this test is the Rust half of the pin and
    /// the C half is tests/env_proto_host_test.c's.
    #[test]
    fn register_reply_encodes_exact_offsets() {
        let mut r = RegisterReply::empty(6, 3, 42);
        r.n_regions = 3;
        r.regions[0] = RegisterRegion { base: 0x1122_3344_5566_7788, frames: 1024, kind: REGION_POSIX_HEAP };
        r.regions[1] = RegisterRegion { base: 0x2000_0000, frames: 64, kind: REGION_RD_HEAP };
        r.regions[2] = RegisterRegion { base: 0x3000_0000, frames: 256, kind: REGION_RD_STORAGE };
        r.n_chans = 4;
        r.chans = [11, 12, 13, 14];
        r.set_task(0, "drv.ramdisk.3", TASK_RAMDISK_SIDECAR);
        r.set_task(1, "aerosls.posix.3", TASK_POSIX_SIDECAR);
        let b = r.encode();

        assert_eq!(read_u32(&b, REG_OFF_PARTITION), 6);
        assert_eq!(read_u32(&b, REG_OFF_INDEX), 3);
        assert_eq!(read_u32(&b, REG_OFF_ENV_ID), 42);
        assert_eq!(read_u32(&b, REG_OFF_N_REGIONS), 3);
        // The high word of the region base lives AFTER the low word -- the
        // rule the C decoder's `lo | hi<<32` relies on.
        assert_eq!(read_u32(&b, REG_OFF_REGIONS), 0x5566_7788);
        assert_eq!(read_u32(&b, REG_OFF_REGIONS + 4), 0x1122_3344);
        assert_eq!(read_u32(&b, REG_OFF_REGIONS + 8), 1024);
        assert_eq!(read_u32(&b, REG_OFF_REGIONS + 12), REGION_POSIX_HEAP);
        assert_eq!(read_u32(&b, REG_OFF_N_CHANS), 4);
        assert_eq!(read_u32(&b, REG_OFF_CHANS), 11);
        assert_eq!(read_u32(&b, REG_OFF_CHANS + 12), 14);
        assert_eq!(read_u32(&b, REG_OFF_N_TASKS), 2);
        assert_eq!(&b[REG_OFF_TASK_NAMES..REG_OFF_TASK_NAMES + 13], b"drv.ramdisk.3");
        assert_eq!(b[REG_OFF_TASK_NAMES + 13], 0);
        assert_eq!(&b[REG_OFF_TASK_NAMES + 24..REG_OFF_TASK_NAMES + 39], b"aerosls.posix.3");
        assert_eq!(read_u32(&b, REG_OFF_TASK_KINDS), TASK_RAMDISK_SIDECAR);
        assert_eq!(read_u32(&b, REG_OFF_TASK_KINDS + 4), TASK_POSIX_SIDECAR);
    }

    /// A name with no room for a terminator is truncated to fit, never allowed
    /// to fill the field: the kernel refuses an unterminated name, and a
    /// silently truncated one would re-register the sidecar under another name.
    #[test]
    fn overlong_task_name_stays_terminated() {
        let mut r = RegisterReply::empty(0, 0, 1);
        r.set_task(0, "a-very-long-sidecar-name-that-will-not-fit", TASK_POSIX_SIDECAR);
        let b = r.encode();
        assert_eq!(b[REG_OFF_TASK_NAMES + REGISTER_NAME_LEN - 1], 0);
        assert_eq!(r.n_tasks, 1);
    }
}
