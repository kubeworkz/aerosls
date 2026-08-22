//! `aerosls` — the real Ring-3 channel runtime behind AeroIDL-generated Rust.
//!
//! The AeroIDL Rust backend (tools/aeroidl `--target rust`) emits stubs that
//! call six `extern "C"` functions — `chan_send`, `chan_recv`,
//! `arena_alloc`, `arena_free`, `arena_mem`, `next_request_id`. Until now
//! those only existed as the panic-on-call mock
//! (`tools/aeroidl/tests/mock_aerosls`) that proved the generated code
//! *compiles*. This crate is the real thing: it issues the Polyglot Nexus
//! message syscalls on the kernel's actual ABI — `SYS_SLS_CAP_SEND_MSG` 302,
//! `SYS_SLS_CAP_RECV_MSG` 303, `SYS_SLS_CAP_ARENA_FREE` 304, plus
//! `SYS_SLS_CAP_ARENA_ALLOC` 290 and the map/unmap pair 295/296 — using the
//! same `syscall`-instruction convention as `user/libsls/sls.h`.
//!
//! Wire framing (identical to the C library `user/libaerocap/aerosls_cap.h`):
//!   - Requests (opcode != 0): the runtime prepends the IDL header
//!     `[opcode: u16 LE][payload_len: u32 LE]` to the stub's args payload,
//!     and sends THAT as the kernel message payload. The generated
//!     dispatcher's `chan_recv` buffer then reads exactly
//!     `[opcode][payload_len][args]` (see gen_dispatcher.rs "Parse header").
//!   - Replies (opcode == 0, the generated convention "0 = response"): the
//!     payload is sent raw, so the client's reply buffer starts at the `ok`
//!     byte.
//!
//! The kernel envelope itself is payload-opaque: it stages the bytes, moves
//! up to 4 MEM caps (each with its byte-level descriptor), and echoes the
//! tag; it never interprets the opcode header.
//!
//! The `target` feature selects the real kernel ABI (inline-asm `syscall`).
//! The default host build is test-only: `sls_syscall` routes through a fake
//! installed by the test harness (same posture as every other sidecar crate
//! in this workspace — see `aerosls-proto`'s `Kernel` trait). The
//! `host-fake` feature exposes that hook to external integration tests.

#![cfg_attr(not(any(test, feature = "host-fake")), no_std)]

pub mod req;
#[cfg(feature = "host-fake")]
pub mod syscall;
#[cfg(not(feature = "host-fake"))]
mod syscall;

use core::ptr;
use req::*;

// ── Constants the generated code may import by path ────────────────────────
// (The generated stubs emit their own copies; these exist for parity with
// the C SDK's aerosls_cap.h so host code can reference one source of truth.)
pub const CAP_PERM_R: u8 = req::CAP_PERM_R;
pub const CAP_PERM_W: u8 = req::CAP_PERM_W;
pub const CAP_PERM_X: u8 = req::CAP_PERM_X;
pub const CAP_NONE: u16 = req::CAP_NONE;
pub const CHAN_FLAG_NO_REPLY: u16 = req::CHAN_FLAG_NO_REPLY as u16;

/// ── Frame scratch buffer ───────────────────────────────────────────────────
/// Requests need the 6-byte IDL header prepended to the stub's payload, so
/// the runtime stages the contiguous frame here before the syscall. A
/// `static` (not a 4 KiB stack array) mirrors cap.c's static-scratch
/// convention and keeps sidecar stack frames small. A tiny spinlock guards
/// it: the real deployment is cooperative single-threaded, but a sidecar
/// with more than one thread (or a two-thread integration test) must not
/// race on the shared frame.
static mut FRAME_SCRATCH: [u8; MSG_MAX_PAYLOAD] = [0u8; MSG_MAX_PAYLOAD];
static FRAME_LOCK: Spinlock = Spinlock::new();

/// Minimal spinlock (same shape as kernel/cap.c's CapSpinlock).
struct Spinlock {
    v: core::sync::atomic::AtomicBool,
}

impl Spinlock {
    const fn new() -> Self {
        Self { v: core::sync::atomic::AtomicBool::new(false) }
    }
    fn acquire(&self) {
        while self
            .v
            .swap(true, core::sync::atomic::Ordering::Acquire)
        {
            core::hint::spin_loop();
        }
    }
    fn release(&self) {
        self.v.store(false, core::sync::atomic::Ordering::Release);
    }
}

/// ── Request-id counter (matches the C SDK's next_request_id contract) ─────
/// Atomic so concurrent senders (multi-thread sidecar, or the two-thread
/// integration test) can never duplicate a tag.
static NEXT_REQ_ID: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);

unsafe fn next_request_id_impl() -> u32 {
    // fetch_add returns the old value; the C counter is `++` from 0, so the
    // first id handed out here is 1. Wraps to 0 after u32::MAX calls, the
    // same as the C SDK's counter.
    NEXT_REQ_ID.fetch_add(1, core::sync::atomic::Ordering::Relaxed).wrapping_add(1)
}

/// ── Arena mapping table (cap → virtual address) ────────────────────────────
/// `arena_mem(cap)` must return a pointer to the cap's pages. Fresh arena
/// allocs always carry the kernel word offset 0, but the runtime can't know
/// a RECEIVED cap's word without asking, so it maps each cap on demand into
/// a reserved region (64 slots × 16 MiB stride at 32 TiB — inside the 47-bit
/// user half, far above the identity map and sidecar image) and remembers
/// the mapping. `arena_free` tears the mapping down with the cap.
const ARENA_MAP_SLOTS: usize = 64;
const ARENA_MAP_BASE: u64 = 0x2000_0000_0000;      /* 32 TiB */
const ARENA_MAP_STRIDE: u64 = 16 * 1024 * 1024;    /* 4096 pages max per cap */
static mut ARENA_MAP: [(u16, u64); ARENA_MAP_SLOTS] = [(CAP_NONE, 0); ARENA_MAP_SLOTS];

// Raw-pointer access to the map: creating references to `static mut` trips
// the `static_mut_refs` lint, and the kernel's cooperative single-threading
// makes a single raw accessor safe (same posture as cap.c's statics).
unsafe fn arena_map_find(cap: u16) -> Option<u64> {
    let p = core::ptr::addr_of!(ARENA_MAP) as *const (u16, u64);
    for i in 0..ARENA_MAP_SLOTS {
        let e = p.add(i);
        if (*e).0 == cap {
            return Some((*e).1);
        }
    }
    None
}

unsafe fn arena_map_take(cap: u16) -> Option<u64> {
    let p = core::ptr::addr_of_mut!(ARENA_MAP) as *mut (u16, u64);
    for i in 0..ARENA_MAP_SLOTS {
        let e = p.add(i);
        if (*e).0 == cap {
            let v = (*e).1;
            (*e) = (CAP_NONE, 0);
            return Some(v);
        }
    }
    None
}

unsafe fn arena_map_insert(cap: u16, vaddr: u64) -> bool {
    let p = core::ptr::addr_of_mut!(ARENA_MAP) as *mut (u16, u64);
    for i in 0..ARENA_MAP_SLOTS {
        let e = p.add(i);
        if (*e).0 == CAP_NONE {
            (*e) = (cap, vaddr);
            return true;
        }
    }
    false
}

// ═══════════════════════════════════════════════════════════════════════════
// The FFI surface the generated code links against
// ═══════════════════════════════════════════════════════════════════════════

/// Send a message on `chan_w`. `opcode` 0 = reply (raw payload), any other
/// value = request (the runtime prepends the `[opcode u16][payload_len u32]`
/// header). `caps` names MEM caps in the CALLER's table; the kernel MOVES
/// them. Returns 0 on success, a negative `CAP_E*` on failure (nothing
/// queued on failure).
///
/// `#[allow(clippy::not_unsafe_ptr_arg_deref)]`: this MUST stay a safe
/// `extern "C"` function — the generated stubs declare it in a plain
/// `extern "C"` block (no `unsafe extern`), and dereferencing the caller's
/// payload/caps pointers is precisely the FFI contract.
#[no_mangle]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn chan_send(
    chan_w: u16,
    opcode: u16,
    payload: *const u8,
    payload_len: u32,
    caps: *const FfiCapDescriptor,
    n_caps: u32,
    flags: u16,
) -> i32 {
    let plen = payload_len as usize;
    if plen > MSG_MAX_PAYLOAD {
        return CAP_ERANGE;
    }
    let wire_len = if opcode != 0 { plen + 6 } else { plen };
    if wire_len > MSG_MAX_PAYLOAD {
        return CAP_ERANGE;
    }
    if plen > 0 && payload.is_null() {
        return CAP_EINVAL;
    }
    if n_caps > MSG_MAX_CAPS as u32 {
        return CAP_ERANGE;
    }
    if n_caps > 0 && caps.is_null() {
        return CAP_EINVAL;
    }

    unsafe {
        // The frame scratch is shared; hold the lock from staging through the
        // syscall (the kernel copies payload → its pool inside the syscall).
        FRAME_LOCK.acquire();
        let mut send_ptr = payload;
        if opcode != 0 {
            let scratch = core::ptr::addr_of_mut!(FRAME_SCRATCH) as *mut u8;
            *scratch.add(0) = (opcode & 0xFF) as u8;
            *scratch.add(1) = ((opcode >> 8) & 0xFF) as u8;
            *scratch.add(2) = (plen & 0xFF) as u8;
            *scratch.add(3) = ((plen >> 8) & 0xFF) as u8;
            *scratch.add(4) = ((plen >> 16) & 0xFF) as u8;
            *scratch.add(5) = ((plen >> 24) & 0xFF) as u8;
            if plen > 0 {
                ptr::copy_nonoverlapping(payload, scratch.add(6), plen);
            }
            send_ptr = scratch;
        }

        let mut req = SendMsgReq {
            ch_w_idx: chan_w,
            n_caps: n_caps as u16,
            tag: next_request_id_impl(),
            flags: flags as u32,
            payload: send_ptr,
            payload_len: wire_len as u32,
            ..SendMsgReq::default()
        };
        if n_caps > 0 {
            let src = core::slice::from_raw_parts(caps, n_caps as usize);
            for (i, d) in src.iter().enumerate() {
                req.caps[i] = desc_to_kernel(d);
            }
        }

        let rc = syscall::sls_syscall(SYS_CAP_SEND_MSG, &req as *const SendMsgReq as u64);
        FRAME_LOCK.release();
        rc as i32
    }
}

/// Receive a message from `chan_r` into `buf` (min(buf_len, payload_len)
/// bytes). Up to `MSG_MAX_CAPS` moved caps are installed into fresh slots in
/// the caller's table; their NEW slot indices are written into `cap_slots`
/// (which must hold at least `MSG_MAX_CAPS` entries — the generated code
/// uses 16) and the count into `*n_slots`. Empty queue returns `CAP_EAGAIN`;
/// the caller retries (kernel-side park is Phase-1.5 territory).
///
/// `#[allow(clippy::not_unsafe_ptr_arg_deref)]`: see `chan_send` — the
/// buffer and slot pointers are the FFI contract, and the function must
/// stay safe for the generated `extern "C"` block.
#[no_mangle]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub extern "C" fn chan_recv(
    chan_r: u16,
    buf: *mut u8,
    buf_len: u32,
    cap_slots: *mut u16,
    n_slots: *mut u16,
) -> i32 {
    unsafe {
        let mut req = RecvMsgReq {
            ch_r_idx: chan_r,
            buf,
            buf_len,
            max_caps: MSG_MAX_CAPS as u16,
            ..RecvMsgReq::default()
        };

        let rc = syscall::sls_syscall(SYS_CAP_RECV_MSG, &mut req as *mut RecvMsgReq as u64);
        if rc != 0 {
            return rc as i32;
        }
        let n = req.out_n_caps as usize;
        if !cap_slots.is_null() {
            for i in 0..n {
                *cap_slots.add(i) = req.out_caps[i].slot;
            }
        }
        if !n_slots.is_null() {
            *n_slots = req.out_n_caps;
        }
        0
    }
}

/// Allocate a buffer from the shared arena. Returns the cap index, or
/// `CAP_NONE` on failure. `CAP_PERM_MAP` is always added to the requested
/// perms so the returned cap can be mapped by `arena_mem`.
#[no_mangle]
pub extern "C" fn arena_alloc(size: u32, perm: u8) -> u16 {
    let npages = ((size as u64).div_ceil(4096)) as u32;
    let npages = if npages == 0 { 1 } else { npages };
    let perm = perm | CAP_PERM_MAP;

    unsafe {
        let req = ArenaAllocReq {
            npages,
            perm: perm as u32,
        };
        let rc = syscall::sls_syscall(SYS_CAP_ARENA_ALLOC, &req as *const ArenaAllocReq as u64);
        if (rc as i64) < 0 {
            return CAP_NONE;
        }
        rc as u16
    }
}

/// Drop ONE reference to an arena cap: the mapping (if any) is torn down,
/// then the kernel decrements the object's refcount, returning its arena
/// frames at refcount 0 — the callee half of the IDL ownership handoff.
#[no_mangle]
pub extern "C" fn arena_free(cap: u16) {
    if cap == CAP_NONE {
        return;
    }
    unsafe {
        if let Some(vaddr) = arena_map_take(cap) {
            let u = UnmapReq {
                cap_idx: cap,
                vaddr,
                ..UnmapReq::default()
            };
            let _ = syscall::sls_syscall(SYS_CAP_UNMAP, &u as *const UnmapReq as u64);
        }
        let r = ArenaFreeReq {
            cap_idx: cap,
            ..ArenaFreeReq::default()
        };
        let _ = syscall::sls_syscall(SYS_CAP_ARENA_FREE, &r as *const ArenaFreeReq as u64);
    }
}

/// Return a direct pointer to the arena pages behind `cap`, mapping them on
/// first access (tries R|W, falls back to R). Returns null on failure —
/// including a cap that lacks `CAP_PERM_MAP` or a full mapping table.
#[no_mangle]
pub extern "C" fn arena_mem(cap: u16) -> *mut u8 {
    if cap == CAP_NONE {
        return ptr::null_mut();
    }
    unsafe {
        if let Some(vaddr) = arena_map_find(cap) {
            return vaddr as *mut u8;
        }
        let map = core::ptr::addr_of!(ARENA_MAP) as *const (u16, u64);
        for slot in 0..ARENA_MAP_SLOTS {
            if (*map.add(slot)).0 != CAP_NONE {
                continue;
            }
            let vaddr = ARENA_MAP_BASE + (slot as u64) * ARENA_MAP_STRIDE;
            let mut m = MapReq {
                cap_idx: cap,
                vaddr,
                flags: (CAP_PERM_R | CAP_PERM_W) as u32,
                ..MapReq::default()
            };
            let mut rc = syscall::sls_syscall(SYS_CAP_MAP, &m as *const MapReq as u64);
            if (rc as i64) < 0 {
                m.flags = CAP_PERM_R as u32;
                rc = syscall::sls_syscall(SYS_CAP_MAP, &m as *const MapReq as u64);
                if (rc as i64) < 0 {
                    return ptr::null_mut();
                }
            }
            if arena_map_insert(cap, vaddr) {
                return vaddr as *mut u8;
            }
            return ptr::null_mut();
        }
        ptr::null_mut()
    }
}

/// Monotonically increasing request id (starts at 1). Shared with
/// `chan_send`'s internal tag so ids are unique across the process.
#[no_mangle]
pub extern "C" fn next_request_id() -> u32 {
    unsafe { next_request_id_impl() }
}

/// Reset the runtime's mutable statics — test-only, so a fresh test starts
/// from the same state as a fresh boot (the harness serializes tests).
#[cfg(test)]
pub(crate) fn reset_test_state() {
    unsafe {
        NEXT_REQ_ID.store(0, core::sync::atomic::Ordering::Relaxed);
        ARENA_MAP = [(CAP_NONE, 0); ARENA_MAP_SLOTS];
    }
}

/// Install the host-build fake syscall handler (integration tests; see the
/// `host-fake` feature). Returns the previously installed handler.
#[cfg(any(test, feature = "host-fake"))]
pub use syscall::set_fake_syscall;

/// Raw syscall entry point — re-exported for integration tests that need
/// to issue syscalls directly (e.g., testing the arena data path with raw
/// request structs). The `host-fake` feature is required.
#[cfg(feature = "host-fake")]
pub use syscall::sls_syscall;

#[cfg(test)]
mod tests;
