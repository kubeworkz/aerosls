//! The kernel ABI for sidecars, re-exported from `aerosls_proto::kabi` (the
//! shared home of the `Kernel` trait, cap types, and error codes), plus the
//! *real* kernel ABI implementation (`RealKernel`) and the sidecar entry
//! point, selected by the `target` feature.
//!
//! The extern "C" declarations below are the *kernel side of the contract*
//! that `kernel/cap.c` and `kernel/chan.c` must implement per
//! `docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md` and
//! `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`. The kernel does not
//! exist yet; these are forward declarations, linked only when the sidecar
//! image is built against it.

pub use aerosls_proto::kabi::*;

// ── Real kernel ABI (feature `target`) ───────────────────────────────────────

#[cfg(feature = "target")]
mod abi {
    use super::*;
    use aerosls_proto::CapDescriptor;

    #[repr(C)]
    struct WaitOut {
        idx: u32,
        kind: u16,
        pad: u16,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct CapRefOut {
        handle: u32,
        rights: u8,
        flags: u8,
        pad: u16,
        base: u64,
        len: u64,
    }

    #[repr(C)]
    struct RecvOut {
        kind: u16,
        flags: u16,
        tag: u32,
        len: u32,
        n_caps: u32,
        needed: u32,
    }

    #[repr(C)]
    struct CapInfoOut {
        ty: u16,
        rights: u16,
        flags: u16,
        base: u64,
        len: u64,
    }

    extern "C" {
        // kernel/chan.c — transport spec §3.4
        fn k_chan_wait(
            chans: *const u32,
            n: u32,
            timeout_ns: u64,
            out: *mut WaitOut,
        ) -> i32;
        fn k_chan_recv(
            chan: u32,
            buf: *mut u8,
            buf_len: u32,
            slots: *mut CapRefOut,
            n_slots: u32,
            out: *mut RecvOut,
        ) -> i32;
        fn k_chan_send(
            chan: u32,
            tag: u32,
            flags: u16,
            payload: *const u8,
            payload_len: u32,
            caps: *const CapDescriptor,
            n_caps: u32,
            timeout_ns: u64,
        ) -> i32;
        fn k_chan_close(chan: u32, reason: u16, detail: u32) -> i32;
        // kernel/cap.c — capability-layer spec §3.5
        fn k_cap_info(handle: u32, out: *mut CapInfoOut) -> i32;
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
            let r = unsafe {
                k_chan_wait(
                    chans.as_ptr(),
                    chans.len() as u32,
                    timeout_ns,
                    &mut out,
                )
            };
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
            let r = unsafe {
                k_chan_recv(
                    chan,
                    buf.as_mut_ptr(),
                    buf.len() as u32,
                    refs.as_mut_ptr(),
                    n_refs,
                    &mut out,
                )
            };
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
            let r = unsafe {
                k_chan_send(
                    chan,
                    tag,
                    flags,
                    payload.as_ptr(),
                    payload.len() as u32,
                    descs.as_ptr(),
                    n as u32,
                    timeout_ns,
                )
            };
            if r != ERR_OK {
                Err(r)
            } else {
                Ok(())
            }
        }

        fn close(&self, chan: u32, reason: u16, detail: u32) -> Result<(), i32> {
            let r = unsafe { k_chan_close(chan, reason, detail) };
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
            let r = unsafe { k_cap_info(handle, &mut out) };
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
    }
}

/// The real kernel ABI implementation (feature `target` only).
#[cfg(feature = "target")]
pub use abi::RealKernel;
