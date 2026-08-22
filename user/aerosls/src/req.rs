//! Request structs and constants — byte-for-byte the kernel's view.
//!
//! Every struct here is `#[repr(C)]` and mirrors `kernel/cap.h` exactly:
//! same fields, same order, same sizes, same padding. The ABI pinning in
//! `tests/aerocap_abi_host_test.c` proves the C SDK headers match the
//! kernel; the unit tests in this crate pin the same layouts against these
//! Rust types so the two languages can never drift silently.
//!
//! The kernel is x86-64 only, so `usize`-sized pointer fields are 8 bytes.

#![allow(dead_code)] // struct fields are written by the FFI layer, not read here

use core::mem::{align_of, offset_of, size_of};

/// ── Syscall numbers (kernel/cap.h SYS_SLS_CAP_*) ───────────────────────────
pub const SYS_CAP_ARENA_ALLOC: u64 = 290;
pub const SYS_CAP_MAP: u64 = 295;
pub const SYS_CAP_UNMAP: u64 = 296;
pub const SYS_CAP_SEND_MSG: u64 = 302;
pub const SYS_CAP_RECV_MSG: u64 = 303;
pub const SYS_CAP_ARENA_FREE: u64 = 304;

/// ── Error codes (kernel/cap.h CAP_E*, errno-style negatives) ──────────────
pub const CAP_EINVAL: i32 = -1;
pub const CAP_EBADF: i32 = -2;
pub const CAP_EAGAIN: i32 = -3;
pub const CAP_ETABLEFULL: i32 = -4;
pub const CAP_ECAPREVOKED: i32 = -5;
pub const CAP_EALREADY: i32 = -6;
pub const CAP_ENOMEM: i32 = -7;
pub const CAP_ENOSPC: i32 = -8;
pub const CAP_ERANGE: i32 = -9;
pub const CAP_ENOSYS: i32 = -10;
pub const CAP_ECONFLICT: i32 = -11;

/// ── Capability permissions (kernel/cap.h CAP_PERM_*) ──────────────────────
pub const CAP_PERM_R: u8 = 0x01;
pub const CAP_PERM_W: u8 = 0x02;
pub const CAP_PERM_X: u8 = 0x04;
pub const CAP_PERM_MAP: u8 = 0x08;

/// ── Channel message flags (kernel/cap.h ChanMsg.flags, bit0 = NO_REPLY) ───
pub const CHAN_FLAG_NO_REPLY: u32 = 0x0001;

/// Sentinel "no capability" slot index.
pub const CAP_NONE: u16 = 0xFFFF;

/// Message payload ceiling — the kernel pool buffer size (kernel/cap.h
/// CAP_MSG_MAX_PAYLOAD) and the AeroIDL compiler's own 4 KiB payload bound.
pub const MSG_MAX_PAYLOAD: usize = 4096;

/// Moved caps per message — the kernel's hard ceiling (CAP_MSG_MAX_CAPS).
pub const MSG_MAX_CAPS: usize = 4;

/// ── The capability descriptor on the wire (kernel/cap.h SLSCapDesc) ────────
/// 16 bytes: slot u16 @0, offset u32 @4, len u32 @8, rights u8 @12, flags u8
/// @13 (2 bytes tail padding). `slot` is the SENDER's table slot on send;
/// on recv the kernel replaces it with the receiver's NEW slot index.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CapDesc {
    pub slot: u16,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    pub flags: u8,
}

/// ── Message send request (kernel/cap.h SLSCapSendMsgRequest), 96 bytes ────
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SendMsgReq {
    pub ch_w_idx: u16,
    pub n_caps: u16,
    pub _pad: [u8; 4],
    pub tag: u32,
    pub flags: u32,
    pub payload_len: u32,
    pub _pad2: [u8; 4],
    pub payload: *const u8,
    pub caps: [CapDesc; MSG_MAX_CAPS],
}

/// ── Message recv request (kernel/cap.h SLSCapRecvMsgRequest), 104 bytes ───
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RecvMsgReq {
    pub ch_r_idx: u16,
    pub block: u8,
    pub _pad: [u8; 1],
    pub max_caps: u16,
    pub _pad2: [u8; 2],
    pub buf: *mut u8,
    pub buf_len: u32,
    pub _pad3: [u8; 4],
    pub out_tag: u32,
    pub out_flags: u32,
    pub out_payload_len: u32,
    pub out_n_caps: u16,
    pub _pad4: [u8; 2],
    pub out_caps: [CapDesc; MSG_MAX_CAPS],
}

/// ── Arena alloc request (kernel/cap.h SLSCapArenaAllocRequest), 8 bytes ───
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ArenaAllocReq {
    pub npages: u32,
    pub perm: u32,
}

/// ── Map request (kernel/cap.h SLSCapMapRequest), 24 bytes ─────────────────
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct MapReq {
    pub cap_idx: u16,
    pub _pad: [u8; 6],
    pub vaddr: u64,
    pub flags: u32,
    pub _pad2: [u8; 4],
}

/// ── Unmap request (kernel/cap.h SLSCapUnmapRequest), 16 bytes ─────────────
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UnmapReq {
    pub cap_idx: u16,
    pub _pad: [u8; 6],
    pub vaddr: u64,
}

/// ── Arena free request (kernel/cap.h SLSCapArenaFreeRequest), 8 bytes ─────
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ArenaFreeReq {
    pub cap_idx: u16,
    pub _pad: [u8; 6],
}

/// ── The FFI descriptor the GENERATED code passes to chan_send ──────────────
/// Emitted by tools/aeroidl (gen_rust.rs) as `CapDescriptor`; this is the
/// same 16-byte layout — slot u32 @0, offset u32 @4, len u32 @8, rights u8
/// @12, flags u8 @13, pad u16 @14. The kernel's slot field is u16, so the
/// runtime converts (truncating the high half — table slots are small).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FfiCapDescriptor {
    pub slot: u32,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    pub flags: u8,
    pub pad: u16,
}

/// Convert a generated FFI descriptor to the kernel wire descriptor.
#[inline]
pub fn desc_to_kernel(d: &FfiCapDescriptor) -> CapDesc {
    CapDesc {
        slot: d.slot as u16,
        offset: d.offset,
        len: d.len,
        rights: d.rights,
        flags: d.flags,
    }
}

// ── Compile-time ABI checks ────────────────────────────────────────────────
// These fail the BUILD — not just the tests — if any pinned struct size,
// field offset, or alignment drifts from the kernel ABI
// (docs/AeroSLS-Capability-ABI-Layouts-v0.1.md). A layout change that
// breaks the wire contract surfaces in `cargo build` itself, before any
// test ever runs. The values mirror `layout_tests` below, so the
// compile-time and test-time layers of enforcement can never disagree
// about the numbers they protect.
const _: () = {
    // Capability descriptor (kernel SLSCapDesc): 16 bytes, align 4.
    assert!(size_of::<CapDesc>() == 16);
    assert!(align_of::<CapDesc>() == 4);
    assert!(offset_of!(CapDesc, slot) == 0);
    assert!(offset_of!(CapDesc, offset) == 4);
    assert!(offset_of!(CapDesc, len) == 8);
    assert!(offset_of!(CapDesc, rights) == 12);
    assert!(offset_of!(CapDesc, flags) == 13);

    // Send-msg request (kernel SLSCapSendMsgRequest): 96 bytes, align 8.
    assert!(size_of::<SendMsgReq>() == 96);
    assert!(align_of::<SendMsgReq>() == 8);
    assert!(offset_of!(SendMsgReq, ch_w_idx) == 0);
    assert!(offset_of!(SendMsgReq, n_caps) == 2);
    assert!(offset_of!(SendMsgReq, _pad) == 4);
    assert!(offset_of!(SendMsgReq, tag) == 8);
    assert!(offset_of!(SendMsgReq, flags) == 12);
    assert!(offset_of!(SendMsgReq, payload_len) == 16);
    assert!(offset_of!(SendMsgReq, _pad2) == 20);
    assert!(offset_of!(SendMsgReq, payload) == 24);
    assert!(offset_of!(SendMsgReq, caps) == 32);

    // Recv-msg request (kernel SLSCapRecvMsgRequest): 104 bytes, align 8.
    assert!(size_of::<RecvMsgReq>() == 104);
    assert!(align_of::<RecvMsgReq>() == 8);
    assert!(offset_of!(RecvMsgReq, ch_r_idx) == 0);
    assert!(offset_of!(RecvMsgReq, block) == 2);
    assert!(offset_of!(RecvMsgReq, _pad) == 3);
    assert!(offset_of!(RecvMsgReq, max_caps) == 4);
    assert!(offset_of!(RecvMsgReq, _pad2) == 6);
    assert!(offset_of!(RecvMsgReq, buf) == 8);
    assert!(offset_of!(RecvMsgReq, buf_len) == 16);
    assert!(offset_of!(RecvMsgReq, _pad3) == 20);
    assert!(offset_of!(RecvMsgReq, out_tag) == 24);
    assert!(offset_of!(RecvMsgReq, out_flags) == 28);
    assert!(offset_of!(RecvMsgReq, out_payload_len) == 32);
    assert!(offset_of!(RecvMsgReq, out_n_caps) == 36);
    assert!(offset_of!(RecvMsgReq, _pad4) == 38);
    assert!(offset_of!(RecvMsgReq, out_caps) == 40);

    // Arena-alloc request (kernel SLSCapArenaAllocRequest): 8 bytes, align 4.
    assert!(size_of::<ArenaAllocReq>() == 8);
    assert!(align_of::<ArenaAllocReq>() == 4);
    assert!(offset_of!(ArenaAllocReq, npages) == 0);
    assert!(offset_of!(ArenaAllocReq, perm) == 4);

    // Map request (kernel SLSCapMapRequest): 24 bytes, align 8.
    assert!(size_of::<MapReq>() == 24);
    assert!(align_of::<MapReq>() == 8);
    assert!(offset_of!(MapReq, cap_idx) == 0);
    assert!(offset_of!(MapReq, _pad) == 2);
    assert!(offset_of!(MapReq, vaddr) == 8);
    assert!(offset_of!(MapReq, flags) == 16);
    assert!(offset_of!(MapReq, _pad2) == 20);

    // Unmap request (kernel SLSCapUnmapRequest): 16 bytes, align 8.
    assert!(size_of::<UnmapReq>() == 16);
    assert!(align_of::<UnmapReq>() == 8);
    assert!(offset_of!(UnmapReq, cap_idx) == 0);
    assert!(offset_of!(UnmapReq, _pad) == 2);
    assert!(offset_of!(UnmapReq, vaddr) == 8);

    // Arena-free request (kernel SLSCapArenaFreeRequest): 8 bytes, align 2.
    assert!(size_of::<ArenaFreeReq>() == 8);
    assert!(align_of::<ArenaFreeReq>() == 2);
    assert!(offset_of!(ArenaFreeReq, cap_idx) == 0);
    assert!(offset_of!(ArenaFreeReq, _pad) == 2);

    // The FFI descriptor generated code passes (FfiCapDescriptor): 16 bytes,
    // align 4 — offset/len/rights/flags at the kernel's offsets; the slot is
    // u32 here (truncated by desc_to_kernel), a documented divergence.
    assert!(size_of::<FfiCapDescriptor>() == 16);
    assert!(align_of::<FfiCapDescriptor>() == 4);
    assert!(offset_of!(FfiCapDescriptor, slot) == 0);
    assert!(offset_of!(FfiCapDescriptor, offset) == 4);
    assert!(offset_of!(FfiCapDescriptor, len) == 8);
    assert!(offset_of!(FfiCapDescriptor, rights) == 12);
    assert!(offset_of!(FfiCapDescriptor, flags) == 13);
    assert!(offset_of!(FfiCapDescriptor, pad) == 14);

    // ── C-only legacy requests (documentation only — no Rust twins) ─────
    // The remaining single-cap request structs have NO Rust equivalents:
    // this crate never issues chan_create/send/recv/revoke/create_mem (the
    // legacy path is C-only, exercised by user/libaerocap/aerocap.h and the
    // kernel). The numbers below are therefore comments, not assertions —
    // they are enforced kernel↔C-SDK by tools/aeroidl/tests/
    // cross_lang_constants.rs and documented in
    // docs/AeroSLS-Capability-ABI-Layouts-v0.1.md §3. Kept here so every
    // number of the ABI is discoverable at the one place a sidecar author
    // reads the request structs.
    //
    //   SLSCapCreateMemRequest    16 bytes, align 8
    //     phys_base u64 @0   npages u32 @8   perm u32 @12
    //   SLSCapChanCreateRequest  20 bytes, align 4
    //     far_pid u32 @0   out_rd u16 @4   out_wr u16 @6
    //     out_far_rd u16 @8   out_far_wr u16 @10   _pad u8[6] @12
    //   SLSCapSendRequest        16 bytes, align 8
    //     ch_w_idx u16 @0   cap_idx u16 @2   _pad u8[4] @4   cookie u64 @8
    //   SLSCapRecvRequest        16 bytes, align 8
    //     ch_r_idx u16 @0   block u8 @2   _pad u8[1] @3
    //     release_pid u32 @4   cookie u64 @8
    //   SLSCapRevokeRequest       8 bytes, align 2
    //     cap_idx u16 @0   _pad u8[6] @2
};

#[cfg(test)]
mod layout_tests {
    use super::*;
    use core::mem::{align_of, offset_of, size_of};

    #[test]
    fn kernel_struct_layouts() {
        // Pinned against kernel/cap.h — any drift here is an ABI break.
        assert_eq!(size_of::<CapDesc>(), 16);
        assert_eq!(offset_of!(CapDesc, slot), 0);
        assert_eq!(offset_of!(CapDesc, offset), 4);
        assert_eq!(offset_of!(CapDesc, len), 8);
        assert_eq!(offset_of!(CapDesc, rights), 12);
        assert_eq!(offset_of!(CapDesc, flags), 13);

        assert_eq!(size_of::<SendMsgReq>(), 96);
        assert_eq!(offset_of!(SendMsgReq, ch_w_idx), 0);
        assert_eq!(offset_of!(SendMsgReq, n_caps), 2);
        assert_eq!(offset_of!(SendMsgReq, tag), 8);
        assert_eq!(offset_of!(SendMsgReq, payload_len), 16);
        assert_eq!(offset_of!(SendMsgReq, payload), 24);
        assert_eq!(offset_of!(SendMsgReq, caps), 32);

        assert_eq!(size_of::<RecvMsgReq>(), 104);
        assert_eq!(offset_of!(RecvMsgReq, ch_r_idx), 0);
        assert_eq!(offset_of!(RecvMsgReq, max_caps), 4);
        assert_eq!(offset_of!(RecvMsgReq, buf), 8);
        assert_eq!(offset_of!(RecvMsgReq, buf_len), 16);
        assert_eq!(offset_of!(RecvMsgReq, out_tag), 24);
        assert_eq!(offset_of!(RecvMsgReq, out_payload_len), 32);
        assert_eq!(offset_of!(RecvMsgReq, out_n_caps), 36);
        assert_eq!(offset_of!(RecvMsgReq, out_caps), 40);

        assert_eq!(size_of::<ArenaAllocReq>(), 8);
        assert_eq!(offset_of!(ArenaAllocReq, npages), 0);
        assert_eq!(offset_of!(ArenaAllocReq, perm), 4);

        assert_eq!(size_of::<MapReq>(), 24);
        assert_eq!(offset_of!(MapReq, cap_idx), 0);
        assert_eq!(offset_of!(MapReq, _pad), 2);
        assert_eq!(offset_of!(MapReq, vaddr), 8);
        assert_eq!(offset_of!(MapReq, flags), 16);
        assert_eq!(offset_of!(MapReq, _pad2), 20);

        assert_eq!(size_of::<UnmapReq>(), 16);
        assert_eq!(offset_of!(UnmapReq, cap_idx), 0);
        assert_eq!(offset_of!(UnmapReq, _pad), 2);
        assert_eq!(offset_of!(UnmapReq, vaddr), 8);
        assert_eq!(size_of::<ArenaFreeReq>(), 8);

        // The FFI descriptor the generated code passes must be 16 bytes.
        assert_eq!(size_of::<FfiCapDescriptor>(), 16);
        assert_eq!(offset_of!(FfiCapDescriptor, slot), 0);
        assert_eq!(offset_of!(FfiCapDescriptor, offset), 4);
        assert_eq!(offset_of!(FfiCapDescriptor, len), 8);
        assert_eq!(offset_of!(FfiCapDescriptor, rights), 12);
        assert_eq!(offset_of!(FfiCapDescriptor, flags), 13);
    }

    #[test]
    fn alignment_matches_kernel() {
        // The kernel ABI passes pointers to these structs; alignment must be
        // the natural one (no packed attrs on either side).
        assert_eq!(align_of::<CapDesc>(), 4);
        assert_eq!(align_of::<SendMsgReq>(), 8);
        assert_eq!(align_of::<RecvMsgReq>(), 8);
        assert_eq!(align_of::<FfiCapDescriptor>(), 4);
        assert_eq!(align_of::<ArenaAllocReq>(), 4);
        assert_eq!(align_of::<MapReq>(), 8);
        assert_eq!(align_of::<UnmapReq>(), 8);
        assert_eq!(align_of::<ArenaFreeReq>(), 2);
    }

    #[test]
    fn desc_conversion() {
        let d = FfiCapDescriptor { slot: 7, offset: 0x100, len: 4096, rights: CAP_PERM_R, flags: 1, pad: 0 };
        let k = desc_to_kernel(&d);
        assert_eq!(k.slot, 7);
        assert_eq!(k.offset, 0x100);
        assert_eq!(k.len, 4096);
        assert_eq!(k.rights, CAP_PERM_R);
        assert_eq!(k.flags, 1);
    }
}
