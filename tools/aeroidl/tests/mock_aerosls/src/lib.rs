//! Mock aerosls runtime FFI — stub implementations for compilation testing.
//!
//! These functions match the `extern "C"` signatures in the generated
//! AeroIDL Rust code. They panic on call (never linked in `--check` mode)
//! but allow `rustc --check` / `cargo check` to verify the generated code
//! type-checks and links symbolically.

// ── Constants (must match the generated code's expectations) ────────────────

pub const CAP_PERM_R: u8 = 0x01;
pub const CAP_PERM_W: u8 = 0x02;
pub const CHAN_FLAG_NO_REPLY: u16 = 0x0001;
pub const CAP_NONE: u16 = 0xFFFF;

// ── Types ──────────────────────────────────────────────────────────────────

/// Error type for AeroIDL cross-sidecar calls.
#[allow(non_camel_case_types)]
#[derive(Clone, Debug)]
pub struct AEROIDL_ERROR {
    pub code: u32,
    pub message: String,
}

impl core::fmt::Display for AEROIDL_ERROR {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "AeroIDL error {}: {}", self.code, self.message)
    }
}

/// Result type for AeroIDL cross-sidecar calls.
pub type AeroidlResult<T> = Result<T, AEROIDL_ERROR>;

/// A slice referencing arena-allocated data via a MEM cap handle.
#[derive(Clone, Copy, Debug)]
pub struct ArenaSlice<T> {
    pub cap: u16,
    pub len: u32,
    _marker: core::marker::PhantomData<T>,
}

impl<T> ArenaSlice<T> {
    pub fn new(cap: u16, len: u32) -> Self {
        Self { cap, len, _marker: core::marker::PhantomData }
    }
    pub fn cap_handle(self) -> u16 { self.cap }
    pub fn len(self) -> u32 { self.len }
}

/// Capability argument descriptor (16 bytes, matching kernel cap.h layout).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CapDescriptor {
    pub slot: u32,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    pub flags: u8,
    pub pad: u16,
}

// ── FFI stubs ──────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn chan_send(
    _chan_w: u16,
    _opcode: u16,
    _payload: *const u8,
    _payload_len: u32,
    _caps: *const CapDescriptor,
    _n_caps: u32,
    _flags: u16,
) -> i32 {
    panic!("mock: chan_send called");
}

#[no_mangle]
pub extern "C" fn chan_recv(
    _chan_r: u16,
    _buf: *mut u8,
    _buf_len: u32,
    _cap_slots: *mut u16,
    _n_slots: *mut u16,
) -> i32 {
    panic!("mock: chan_recv called");
}

#[no_mangle]
pub extern "C" fn arena_alloc(_size: u32, _perm: u8) -> u16 {
    panic!("mock: arena_alloc called");
}

#[no_mangle]
pub extern "C" fn arena_free(_cap: u16) {
    panic!("mock: arena_free called");
}

#[no_mangle]
pub extern "C" fn arena_mem(_cap: u16) -> *mut u8 {
    panic!("mock: arena_mem called");
}

#[no_mangle]
pub extern "C" fn next_request_id() -> u32 {
    panic!("mock: next_request_id called");
}
