//! The kernel ABI for sidecars, expressed as a trait.
//!
//! Sidecar cores (the ramdisk driver, the POSIX sidecar's block cache, ...)
//! are written against `Kernel` (static dispatch), which makes them
//! host-testable: `aerosls-kernel-sim` implements the same semantics as a
//! fake, and the real ABI (`RealKernel`, below, behind the `target`
//! feature) links against the kernel proper.
//!
//! The kernel side of the contract lives in `kernel/chan.c` and
//! `kernel/cap.c` per `docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md`
//! and `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`. Behind the
//! `target` feature, this module defines the `k_chan_*`/`k_cap_info`
//! extern "C" symbols as real syscall shims (311-315), so a sidecar image
//! links them; the kernel dispatches those syscalls to its own
//! implementations in `kernel/chan.c`. The host build omits the shims so
//! the crate links cleanly into the test harness.

/// Capability types (capability-layer spec §1.1, respawn decision §2).
/// `CAP_CHAN` aliases the BIB's CHAN_R entry type (2); `CAP_CHAN_W` is
/// the CHAN_W entry type (3) — a wired channel appears in the BIB as TWO
/// named entries (CHAN_R then CHAN_W), so a sidecar that SENDS on a
/// channel (e.g. the console) must resolve the W end, not the first match.
pub const CAP_MEM: u16 = 1;
pub const CAP_CHAN: u16 = 2;
pub const CAP_CHAN_W: u16 = 3;

/// `CAP_NONE` — an absent cap slot (the kernel's CAP_NONE).
pub const CAP_NONE: u16 = 0xFFFF;

/// Kernel error codes (capability-layer spec §7 + transport spec §7).
pub const ERR_OK: i32 = 0;
pub const ERR_NOTFOUND: i32 = 1;
pub const ERR_REVOKED: i32 = 2;
pub const ERR_RIGHTS: i32 = 3;
pub const ERR_RANGE: i32 = 4;
pub const ERR_BUDGET: i32 = 5;
pub const ERR_SPACE: i32 = 6;
pub const ERR_TARGET: i32 = 7;
pub const ERR_STATE: i32 = 8;
pub const ERR_TYPE: i32 = 9;
pub const ERR_PROTO: i32 = 10;
pub const ERR_NOMEM: i32 = 11;
pub const ERR_BUFSZ: i32 = 12;
pub const ERR_TIMEOUT: i32 = 13;
/// Fake-kernel only: the sidecar was torn down (driver "death").
pub const ERR_SHUTDOWN: i32 = 14;

/// Block forever (real kernel) / wait without a deadline.
pub const TIMEOUT_NONE: u64 = u64::MAX;

/// A capability a sidecar received on a message, already minted in its own
/// table by the kernel (capability-layer spec §4.1).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct GrantedCap {
    /// Handle in the receiver's table.
    pub handle: u32,
    /// Rights the kernel actually minted (`min(held, requested)`).
    pub rights: u8,
    /// Base of the sub-region (sender base + offset).
    pub base: u64,
    /// Length of the sub-region.
    pub len: u64,
}

/// A capability argument to attach to a message a sidecar sends.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SendCap {
    /// Handle in the *sender's* table.
    pub slot: u32,
    pub offset: u32,
    pub len: u32,
    pub rights: u8,
    /// `CAP_PERSIST | CAP_MOVE | CAP_MAP` from `aerosls_proto`.
    pub flags: u8,
}

/// Result of a receive: a message, a close event, or a NEW_CHANNEL event.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RecvResult {
    /// `CH_KIND_MSG | CH_KIND_CLOSE | CH_KIND_NEW_CHANNEL`.
    pub kind: u16,
    /// Envelope flags (`F_REPLY | F_NO_REPLY`).
    pub flags: u16,
    /// The request id (echoed in replies).
    pub tag: u32,
    /// Payload bytes written into the buffer.
    pub len: usize,
    /// Granted caps written into the slots array.
    pub n_caps: usize,
}

/// `cap_info` result (capability-layer spec §3.5).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CapInfo {
    pub ty: u16,
    pub rights: u16,
    pub flags: u16,
    pub base: u64,
    pub len: u64,
}

/// The kernel operations a sidecar can perform. All blocking calls honor
/// either a timeout or a close event; a wedged peer can never wedge the
/// caller (transport spec §4 guarantee 7).
pub trait Kernel {
    /// Block until a message or control event is queued on any of `chans`,
    /// or `timeout_ns` elapses. Returns the index into `chans` and the kind
    /// of the head entry. `TIMEOUT_NONE` blocks forever.
    fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32>;

    /// Receive one message/event from `chan`, filling `buf` (payload) and
    /// `slots` (granted caps). A failed recv (undersize, `ERR_BUFSZ`) does
    /// **not** consume the message (transport spec §3.4).
    fn recv(
        &self,
        chan: u32,
        buf: &mut [u8],
        slots: &mut [GrantedCap],
    ) -> Result<RecvResult, i32>;

    /// Send one message. `caps` are validated against the sender's table and
    /// minted (derived) in the receiver's table — never amplified.
    fn send(
        &self,
        chan: u32,
        tag: u32,
        flags: u16,
        payload: &[u8],
        caps: &[SendCap],
        timeout_ns: u64,
    ) -> Result<(), i32>;

    /// Close an endpoint with a reason; idempotent (transport spec §6.1).
    fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32>;

    /// Introspect one of the caller's own caps (capability-layer spec §3.5).
    fn cap_info(&self, handle: u32) -> Result<CapInfo, i32>;

    /// Non-blocking peek: is a message or control event already queued on
    /// `chan`? Returns the head's kind (`CH_KIND_*`, or `CH_KIND_NONE`).
    /// Peeks only — the event is consumed by the next `recv`. This is how
    /// a sidecar learns a peer died *without* an outstanding request
    /// (respawn decision §5 steps 1–3: the kernel queues the close and the
    /// block cache polls before serving a cache hit — a cache hit must
    /// never come from a dead device). Default: nothing queued, for
    /// implementations that never poll.
    fn poll(&self, chan: u32) -> Result<u16, i32> {
        let _ = chan;
        Ok(crate::CH_KIND_NONE)
    }

    /// Spawn a sidecar from a packed manifest blob (`SYS_SLS_CREATE_SIDECAR`,
    /// kernel/cap.c). Returns the parent's messenger channel handles:
    /// `(CHAN_R, CHAN_W)` — the R end receives the child's replies, the W
    /// end sends to the child. The blob must carry the 8-byte image_kaddr
    /// footer (the image the kernel maps lives at that physical address).
    /// Default: unsupported (`ERR_NOTFOUND`), for fakes that never spawn.
    fn create_sidecar(&self, manifest: &[u8]) -> Result<(u32, u32), i32> {
        let _ = manifest;
        Err(ERR_NOTFOUND)
    }
}

// ── Real kernel ABI (feature `target`) ───────────────────────────────────────

#[cfg(feature = "target")]
mod abi {
    use super::*;
    use crate::CapDescriptor;

    #[repr(C)]
    pub struct WaitOut {
        idx: u32,
        kind: u16,
        pad: u16,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct CapRefOut {
        handle: u32,
        rights: u8,
        flags: u8,
        pad: u16,
        base: u64,
        len: u64,
    }

    #[repr(C)]
    pub struct RecvOut {
        kind: u16,
        flags: u16,
        tag: u32,
        len: u32,
        n_caps: u32,
        needed: u32,
    }

    #[repr(C)]
    pub struct CapInfoOut {
        ty: u16,
        rights: u16,
        flags: u16,
        base: u64,
        len: u64,
    }

    // ── Syscall shims (the real kernel side is kernel/chan.c) ─────────────
    // The extern "C" symbols RealKernel links against: each builds the
    // request struct the kernel's syscall wrapper unpacks (syscalls
    // 311-315, layouts mirror kernel/cap.h exactly) and issues the raw
    // syscall instruction. The kernel returns the transport's positive
    // CAP_ERR_* codes (0 = CAP_ERR_OK), so `r != ERR_OK` below is exactly
    // right — but note the kernel's codes are the spec's numbers (1..13),
    // which this module's ERR_* constants already are.

    const SYS_CREATE_SIDECAR: u64 = 310;
    const SYS_CHAN_WAIT: u64 = 311;
    const SYS_CHAN_RECV: u64 = 312;
    const SYS_CHAN_SEND: u64 = 313;
    const SYS_CHAN_CLOSE: u64 = 314;
    const SYS_CAP_INFO: u64 = 315;

    /// The raw syscall instruction (same convention as
    /// `aerosls::syscall::sls_syscall`): number in rax, one arg pointer in
    /// rdi.
    unsafe fn sls_syscall(num: u64, arg: u64) -> u64 {
        let ret: u64;
        core::arch::asm!(
            "syscall",
            inlateout("rax") num => ret,
            in("rdi") arg,
            lateout("rcx") _,
            lateout("r11") _,
            lateout("rsi") _,
            lateout("rdx") _,
            lateout("r8") _,
            lateout("r9") _,
            lateout("r10") _,
            options(nostack),
        );
        ret
    }

    // Request structs — mirror kernel/cap.h's SLSChan*Request layouts
    // (repr(C), explicit padding) so the kernel reads exactly what the
    // shim wrote.

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct ChanWaitReq {
        chans: [u16; 8],
        n_chans: u16,
        _pad: [u8; 4],
        timeout_ns: u64,
        out_idx: u32,
        out_kind: u16,
        _pad2: [u8; 2],
    }

    /// Kernel SLSCapDesc (slot u16 + pad; proto's CapDescriptor is slot
    /// u32 — byte-compatible for offset/len/rights/flags, converted here).
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct KernCapDesc {
        slot: u16,
        _pad: u16,
        offset: u32,
        len: u32,
        rights: u8,
        flags: u8,
    }

    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct ChanCapRef {
        handle: u32,
        rights: u8,
        flags: u8,
        pad: u16,
        base: u64,
        len: u64,
    }

    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct ChanRecvOut {
        kind: u16,
        flags: u16,
        tag: u32,
        len: u32,
        n_caps: u32,
        needed: u32,
    }

    #[repr(C)]
    struct ChanRecvReq {
        chan: u16,
        _pad: [u8; 6],
        buf: *mut u8,
        buf_len: u32,
        n_slots: u32,
        slots: [ChanCapRef; 8],
        out: ChanRecvOut,
    }

    #[repr(C)]
    struct ChanSendReq {
        chan: u16,
        _pad: [u8; 2],
        tag: u32,
        flags: u16,
        _pad2: [u8; 2],
        payload_len: u32,
        _pad3: [u8; 4],
        payload: *const u8,
        caps: [KernCapDesc; 4],
        n_caps: u16,
        _pad4: [u8; 6],
        timeout_ns: u64,
    }

    #[repr(C)]
    struct ChanCloseReq {
        chan: u16,
        _pad: [u8; 2],
        reason: u16,
        _pad2: [u8; 2],
        detail: u32,
    }

    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct CapInfoOutReq {
        ty: u16,
        rights: u16,
        flags: u16,
        pad: u16,
        base: u64,
        len: u64,
    }

    #[repr(C)]
    struct CapInfoReq {
        handle: u16,
        _pad: [u8; 6],
        out: CapInfoOutReq,
    }

    /// Kernel SLSCreateSidecarRequest (kernel/cap.h): the kernel fills
    /// out_ch_r / out_ch_w (the parent's messenger ends) on success.
    #[repr(C)]
    struct CreateSidecarReq {
        manifest: *const u8,
        manifest_len: u32,
        _pad: [u8; 4],
        ch_w_idx: u16,
        console_w_idx: u16,
        out_ch_r: u16,
        out_ch_w: u16,
    }

    /// `k_create_sidecar`: syscall 310. The kernel returns the child's
    /// parent-end CHAN_R slot as the syscall VALUE on success (errors are
    /// negative), and fills `out_ch_r`/`out_ch_w` in the request — the
    /// shim reads both ends from there.
    #[no_mangle]
    pub extern "C" fn k_create_sidecar(
        manifest: *const u8,
        manifest_len: u32,
        out_r: *mut u32,
        out_w: *mut u32,
    ) -> i32 {
        let mut req = CreateSidecarReq {
            manifest,
            manifest_len,
            _pad: [0; 4],
            ch_w_idx: CAP_NONE,
            console_w_idx: CAP_NONE,
            out_ch_r: CAP_NONE,
            out_ch_w: CAP_NONE,
        };
        let rc = unsafe {
            sls_syscall(SYS_CREATE_SIDECAR, &mut req as *mut CreateSidecarReq as u64)
        } as i64;
        if rc < 0 {
            return rc as i32;
        }
        if !out_r.is_null() {
            unsafe { *out_r = req.out_ch_r as u32 };
        }
        if !out_w.is_null() {
            unsafe { *out_w = req.out_ch_w as u32 };
        }
        0
    }

    #[no_mangle]
    pub extern "C" fn k_chan_wait(
        chans: *const u32,
        n: u32,
        timeout_ns: u64,
        out: *mut WaitOut,
    ) -> i32 {
        let mut req = ChanWaitReq {
            chans: [0; 8],
            n_chans: 0,
            _pad: [0; 4],
            timeout_ns,
            out_idx: 0,
            out_kind: 0,
            _pad2: [0; 2],
        };
        let nc = (n as usize).min(8);
        if !chans.is_null() {
            for i in 0..nc {
                req.chans[i] = unsafe { *chans.add(i) } as u16;
            }
        }
        req.n_chans = nc as u16;
        let rc = unsafe { sls_syscall(SYS_CHAN_WAIT, &req as *const ChanWaitReq as u64) };
        if rc == 0 && !out.is_null() {
            unsafe {
                (*out).idx = req.out_idx;
                (*out).kind = req.out_kind;
            }
        }
        rc as i32
    }

    #[no_mangle]
    pub extern "C" fn k_chan_recv(
        chan: u32,
        buf: *mut u8,
        buf_len: u32,
        slots: *mut CapRefOut,
        n_slots: u32,
        out: *mut RecvOut,
    ) -> i32 {
        let mut req = ChanRecvReq {
            chan: chan as u16,
            _pad: [0; 6],
            buf,
            buf_len,
            n_slots: n_slots.min(8),
            slots: [ChanCapRef::default(); 8],
            out: ChanRecvOut::default(),
        };
        let rc = unsafe { sls_syscall(SYS_CHAN_RECV, &mut req as *mut ChanRecvReq as u64) };
        if rc == 0 {
            if !out.is_null() {
                unsafe {
                    (*out).kind = req.out.kind;
                    (*out).flags = req.out.flags;
                    (*out).tag = req.out.tag;
                    (*out).len = req.out.len;
                    (*out).n_caps = req.out.n_caps;
                    (*out).needed = req.out.needed;
                }
            }
            if !slots.is_null() {
                let nc = (req.out.n_caps as usize).min(req.n_slots as usize);
                for i in 0..nc {
                    unsafe {
                        (*slots.add(i)) = CapRefOut {
                            handle: req.slots[i].handle,
                            rights: req.slots[i].rights,
                            flags: req.slots[i].flags,
                            pad: 0,
                            base: req.slots[i].base,
                            len: req.slots[i].len,
                        };
                    }
                }
            }
        }
        rc as i32
    }

    #[no_mangle]
    pub extern "C" fn k_chan_send(
        chan: u32,
        tag: u32,
        flags: u16,
        payload: *const u8,
        payload_len: u32,
        caps: *const CapDescriptor,
        n_caps: u32,
        timeout_ns: u64,
    ) -> i32 {
        let mut req = ChanSendReq {
            chan: chan as u16,
            _pad: [0; 2],
            tag,
            flags,
            _pad2: [0; 2],
            payload_len,
            _pad3: [0; 4],
            payload,
            caps: [KernCapDesc::default(); 4],
            n_caps: 0,
            _pad4: [0; 6],
            timeout_ns,
        };
        let nc = (n_caps as usize).min(4);
        if !caps.is_null() {
            for i in 0..nc {
                let c = unsafe { *caps.add(i) };
                req.caps[i] = KernCapDesc {
                    slot: c.slot as u16,
                    _pad: 0,
                    offset: c.offset,
                    len: c.len,
                    rights: c.rights,
                    flags: c.flags,
                };
            }
        }
        req.n_caps = nc as u16;
        unsafe { sls_syscall(SYS_CHAN_SEND, &req as *const ChanSendReq as u64) as i32 }
    }

    #[no_mangle]
    pub extern "C" fn k_chan_close(chan: u32, reason: u16, detail: u32) -> i32 {
        let req = ChanCloseReq {
            chan: chan as u16,
            _pad: [0; 2],
            reason,
            _pad2: [0; 2],
            detail,
        };
        unsafe { sls_syscall(SYS_CHAN_CLOSE, &req as *const ChanCloseReq as u64) as i32 }
    }

    #[no_mangle]
    pub extern "C" fn k_cap_info(handle: u32, out: *mut CapInfoOut) -> i32 {
        let mut req = CapInfoReq {
            handle: handle as u16,
            _pad: [0; 6],
            out: CapInfoOutReq::default(),
        };
        let rc = unsafe { sls_syscall(SYS_CAP_INFO, &mut req as *mut CapInfoReq as u64) };
        if rc == 0 && !out.is_null() {
            unsafe {
                (*out).ty = req.out.ty;
                (*out).rights = req.out.rights;
                (*out).flags = req.out.flags;
                (*out).base = req.out.base;
                (*out).len = req.out.len;
            }
        }
        rc as i32
    }

    /// The real kernel ABI. Only constructible/usable on the sidecar target.
    pub struct RealKernel;

    impl Kernel for RealKernel {
        fn wait(&self, chans: &[u32], timeout_ns: u64) -> Result<(usize, u16), i32> {
            let mut out = WaitOut {
                idx: 0,
                kind: 0,
                pad: 0,
            };
            let r = k_chan_wait(chans.as_ptr(), chans.len() as u32, timeout_ns, &mut out);
            if r != ERR_OK {
                Err(r)
            } else {
                Ok((out.idx as usize, out.kind))
            }
        }

        fn recv(
            &self,
            chan: u32,
            buf: &mut [u8],
            slots: &mut [GrantedCap],
        ) -> Result<RecvResult, i32> {
            let mut refs = [CapRefOut {
                handle: 0,
                rights: 0,
                flags: 0,
                pad: 0,
                base: 0,
                len: 0,
            }; 8];
            let n_refs = slots.len().min(refs.len()) as u32;
            let mut out = RecvOut {
                kind: 0,
                flags: 0,
                tag: 0,
                len: 0,
                n_caps: 0,
                needed: 0,
            };
            let r = k_chan_recv(
                chan,
                buf.as_mut_ptr(),
                buf.len() as u32,
                refs.as_mut_ptr(),
                n_refs,
                &mut out,
            );
            if r != ERR_OK {
                return Err(r);
            }
            let n = out.n_caps.min(refs.len() as u32) as usize;
            for (i, slot) in slots.iter_mut().enumerate().take(n) {
                *slot = GrantedCap {
                    handle: refs[i].handle,
                    rights: refs[i].rights,
                    base: refs[i].base,
                    len: refs[i].len,
                };
            }
            Ok(RecvResult {
                kind: out.kind,
                flags: out.flags,
                tag: out.tag,
                len: out.len as usize,
                n_caps: out.n_caps as usize,
            })
        }

        fn send(
            &self,
            chan: u32,
            tag: u32,
            flags: u16,
            payload: &[u8],
            caps: &[SendCap],
            timeout_ns: u64,
        ) -> Result<(), i32> {
            let mut descs = [CapDescriptor {
                slot: 0,
                offset: 0,
                len: 0,
                rights: 0,
                flags: 0,
                pad: 0,
            }; 8];
            let n = caps.len().min(descs.len());
            for (i, c) in caps.iter().take(n).enumerate() {
                descs[i] = CapDescriptor {
                    slot: c.slot,
                    offset: c.offset,
                    len: c.len,
                    rights: c.rights,
                    flags: c.flags,
                    pad: 0,
                };
            }
            let r = k_chan_send(
                chan,
                tag,
                flags,
                payload.as_ptr(),
                payload.len() as u32,
                descs.as_ptr(),
                n as u32,
                timeout_ns,
            );
            if r != ERR_OK {
                Err(r)
            } else {
                Ok(())
            }
        }

        fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32> {
            let r = k_chan_close(chan, reason, detail);
            if r != ERR_OK {
                Err(r)
            } else {
                Ok(())
            }
        }

        fn cap_info(&self, handle: u32) -> Result<CapInfo, i32> {
            let mut out = CapInfoOut {
                ty: 0,
                rights: 0,
                flags: 0,
                base: 0,
                len: 0,
            };
            let r = k_cap_info(handle, &mut out);
            if r != ERR_OK {
                Err(r)
            } else {
                Ok(CapInfo {
                    ty: out.ty,
                    rights: out.rights,
                    flags: out.flags,
                    base: out.base,
                    len: out.len,
                })
            }
        }

        fn create_sidecar(&self, manifest: &[u8]) -> Result<(u32, u32), i32> {
            let mut r: u32 = 0;
            let mut w: u32 = 0;
            let rc = k_create_sidecar(manifest.as_ptr(), manifest.len() as u32, &mut r, &mut w);
            if rc != ERR_OK {
                Err(rc)
            } else {
                Ok((r, w))
            }
        }
    }
}

/// The real kernel ABI implementation (feature `target` only).
#[cfg(feature = "target")]
pub use abi::RealKernel;
