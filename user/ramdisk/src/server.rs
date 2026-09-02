//! The `RD_*` server loop and handlers (implementation plan §5, §6).
//!
//! The driver is passive and connection-agnostic: one event loop waits on the
//! console channel (control events) and every adopted endpoint (requests),
//! serving `RD_*` on all of them. Frame-level garbage (bad magic/version,
//! unknown type, handshake violation) closes the endpoint with
//! `CLOSE_PROTO`; well-formed requests with bad arguments get an error reply
//! with the client's tag echoed — the kernel's reply pairing and the
//! protocol's correlation are the same number.
//!
//! Request buffers arrive as transient MEM grants (read buffers are W-only,
//! write buffers are R-only, per Phase 2 §5.2). The kernel minted, bounded,
//! and auto-revoked them; the driver re-checks rights and size before a
//! single byte moves (§7 of the plan: kernel checked rights, driver checks
//! size, neither trusts the other).

use crate::copy;
use crate::endpoints::{EndpointSet, EndpointState};
use crate::kapi::{self, GrantedCap, Kernel, SendCap};
use aerosls_proto::*;

/// The device the driver serves: a view of the storage region from its own
/// cap (resolved once at boot via `cap_info`).
#[derive(Clone, Copy, Debug)]
pub struct Device {
    /// The driver's storage cap handle (for `RD_MAP` grants).
    pub storage_slot: u32,
    pub storage_base: u64,
    pub storage_len: u64,
    /// Whether the storage cap is writable (from cap rights, not a flag).
    pub storage_writable: bool,
}

impl Device {
    pub fn blocks(&self) -> u64 {
        self.storage_len / (BLOCK_SIZE as u64)
    }

    pub fn read_only(&self) -> bool {
        !self.storage_writable
    }
}

/// Why the driver closed an endpoint. `detail` is an `RD_ERR_*` code, carried
/// in the close event (transport spec §6.2).
#[derive(Clone, Copy, Debug)]
pub struct Close {
    pub detail: u32,
}

/// Run the server loop until a fatal kernel error (e.g. the fake kernel's
/// shutdown on driver death). Never returns on a healthy kernel — `wait`
/// blocks forever with `TIMEOUT_NONE`; the driver has no timers (backoff and
/// respawn are the POSIX core's job).
pub fn run<K: Kernel>(k: &K, eps: &mut EndpointSet, dev: &Device) -> Result<(), i32> {
    use crate::kapi::CAP_CHAN;
    let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
    let mut caps = [GrantedCap::default(); ChanHeader::MAX_CAPS];

    loop {        // Re-scan cap table for newly-wired CHAN endpoints (e.g. the POSIX
        // sidecar's "ramdisk" channel, wired after this sidecar booted).
        // For each CHAN_R, adopt it — the kernel's CHAN_R send fallback
        // (cap_send_msg accepts both CHAN_W with SEND and CHAN_R with
        // RECV) means replies sent on CHAN_R route to the peer's receive
        // queue on the same channel object, so we don't need to find a
        // separate CHAN_W slot.
        for slot in 0u32..16 {
            if eps.is_console(slot) { continue; }
            if let Ok(info) = k.cap_info(slot) {
                if info.ty == CAP_CHAN {
                    eps.adopt(slot);
                }
            }
        }

        let list = eps.wait_list();
        // When no client has connected yet, use a finite deadline to
        // periodically re-scan for newly-wired channels (e.g. the POSIX
        // sidecar's ramdisk channel, wired after this sidecar booted).
        let poll_ns: u64 = if eps.has_active_client() {
            kapi::TIMEOUT_NONE
        } else {
            200_000_000 /* 200 ms — discovery poll */
        };
        let (idx, _kind) = match k.wait(&list[..eps.wait_len()], poll_ns) {
            Ok(r) => r,
            Err(kabi::ERR_SHUTDOWN) => return Ok(()),
            Err(_) => continue, /* timeout: re-scan and retry */
        };
        let handle = list[idx];

        if eps.is_console(handle) {
            handle_console(k, eps, &mut buf, &mut caps)?;
            continue;
        }

        let rr = match k.recv(handle, &mut buf, &mut caps) {
            Ok(rr) => rr,
            // Endpoint died under us; drop and move on (next wait will
            // surface anything else).
            Err(_) => {
                eps.drop_ep(handle);
                continue;
            }
        };

        match rr.kind {
            CH_KIND_MSG => {
                let state = eps
                    .by_handle(handle)
                    .map(|e| e.state)
                    .unwrap_or(EndpointState::AwaitingHandshake);
                // Send replies on the same handle we received on — the
                // kernel's CHAN_R send fallback routes to the peer's
                // receive queue on the same channel object.
                if let Err(close) =
                    dispatch(k, handle, state, rr.tag, dev, &buf[..rr.len], &caps[..rr.n_caps])
                {
                    let _ = k.close(handle, CLOSE_PROTO, close.detail);
                    eps.drop_ep(handle);
                } else if state == EndpointState::AwaitingHandshake {
                    // The handshake request (first RD_INFO, no caps) was
                    // accepted: the endpoint is now active (plan §5.2).
                    eps.mark_active(handle);
                }
            }
            _ => {
                // CLOSE (client died / timed out) or an unexpected kind:
                // drop the endpoint. The kernel reaped any transient grants
                // bound to it; nothing to clean here (plan §8.2).
                eps.drop_ep(handle);
            }
        }
    }
}

/// Console is control-only: `NEW_CHANNEL` adopts, everything else is a
/// kernel bug we log-and-drop. The core has no logging facility (it is
/// console-free by design for testability); callers that want logs wrap the
/// loop.
fn handle_console<K: Kernel>(
    k: &K,
    eps: &mut EndpointSet,
    buf: &mut [u8],
    caps: &mut [GrantedCap],
) -> Result<(), i32> {
    let rr = k.recv(eps.console(), buf, caps)?;
    match rr.kind {
        CH_KIND_NEW_CHANNEL => {
            if let Some((handle, _rights, _flags, _tag)) = parse_new_channel(&buf[..rr.len]) {
                if !eps.adopt(handle) {
                    // Table full: refuse the channel.
                    let _ = k.close(handle, CLOSE_REVOKED, 0);
                }
            }
        }
        // CLOSE on console = kernel closed our control channel (kernel bug);
        // unexpected MSG on console = kernel bug. Both are survivable: keep
        // running; a closed console simply never becomes ready again.
        _ => {}
    }
    Ok(())
}

/// Dispatch one request. Frame-level garbage and handshake violations are
/// `Err(Close)`; argument-level problems are error replies inside the
/// handlers.
fn dispatch<K: Kernel>(
    k: &K,
    handle: u32,
    state: EndpointState,
    tag: u32,
    dev: &Device,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    let frame = RdFrame::parse(payload).ok_or(Close {
        detail: RD_ERR_PROTO as u32,
    })?;
    if frame.magic != RD_MAGIC || frame.version != RD_VERSION {
        return Err(Close {
            detail: RD_ERR_PROTO as u32,
        });
    }

    // Implicit handshake: the first message on any endpoint must be RD_INFO
    // with no caps (plan §5.2). Matches the client's boot and respawn
    // attach sequences, which send RD_INFO first by construction.
    if state == EndpointState::AwaitingHandshake {
        if frame.ty != RD_INFO || !caps.is_empty() {
            return Err(Close {
                detail: RD_ERR_PROTO as u32,
            });
        }
    }

    match frame.ty {
        RD_INFO => reply_info(k, handle, tag, dev),
        RD_READ => read_blocks(k, handle, tag, dev, payload, caps),
        RD_WRITE => write_blocks(k, handle, tag, dev, payload, caps),
        RD_FLUSH => {
            reply_status(k, handle, tag, RD_FLUSH, RD_OK, 0, None);
            Ok(())
        }
        RD_MAP => reply_map(k, handle, tag, dev),
        _ => Err(Close {
            detail: RD_ERR_INVAL as u32,
        }),
    }
}

// ── handlers ─────────────────────────────────────────────────────────────────

fn reply_info<K: Kernel>(k: &K, handle: u32, tag: u32, dev: &Device) -> Result<(), Close> {
    let mut p = [0u8; 32];
    p[..16].copy_from_slice(&RdFrame::new(RD_INFO, false).encode());
    let flags: u32 = if dev.read_only() { 1 } else { 0 };
    p[16..32].copy_from_slice(&encode_info_body(BLOCK_SIZE, dev.blocks(), flags));
    let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
    Ok(())
}

fn read_blocks<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &Device,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    // Bind the client's grant cap up front: it must always travel back in
    // the reply (move-return), on success AND on every error path, so the
    // window=1 client's single granted buffer is never stranded here. The
    // real kernel transport MOVES the cap (no separate mint), so the
    // client holds no other copy of it once we receive it.
    let grant = caps.first().copied();
    let Some(grant) = grant else {
        reply_status(k, handle, tag, RD_READ, RD_ERR_CAP, 0, None);
        return Ok(());
    };
    let Some((lba, count)) = payload.get(16..).and_then(parse_rw_body) else {
        reply_status(k, handle, tag, RD_READ, RD_ERR_INVAL, 0, Some(&grant));
        return Ok(());
    };
    if count == 0 || count > MAX_IO {
        reply_status(k, handle, tag, RD_READ, RD_ERR_INVAL, 0, Some(&grant));
        return Ok(());
    }
    // The kernel minted min(held, requested); re-check what actually arrived.
    if grant.rights & W == 0 {
        reply_status(k, handle, tag, RD_READ, RD_ERR_CAP, 0, Some(&grant));
        return Ok(());
    }
    let bytes = (count as u64 * BLOCK_SIZE as u64) as usize;
    if grant.len < bytes as u64 {
        reply_status(k, handle, tag, RD_READ, RD_ERR_CAP, 0, Some(&grant));
        return Ok(());
    }
    if lba + count as u64 > dev.blocks() {
        reply_status(k, handle, tag, RD_READ, RD_ERR_RANGE, 0, Some(&grant));
        return Ok(());
    }

    let src = (dev.storage_base + lba * BLOCK_SIZE as u64) as *const u8;
    let dst = grant.base as *mut u8;
    // Safety: storage range vs our own cap (checked above), grant range vs
    // the kernel-minted len (checked above), bytes <= MAX_IO * 512.
    unsafe { copy::copy_blocks(src, dst, bytes) };
    reply_status(k, handle, tag, RD_READ, RD_OK, bytes as u64, Some(&grant));
    Ok(())
}

fn write_blocks<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &Device,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    // Structural: the driver's write authority is its storage cap rights
    // (plan §1). With the read-only manifest this always answers RD_ERR_RO.
    let grant = caps.first().copied();
    if dev.read_only() {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_RO, 0, grant.as_ref());
        return Ok(());
    }
    let Some(grant) = grant else {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_CAP, 0, None);
        return Ok(());
    };
    let Some((lba, count)) = payload.get(16..).and_then(parse_rw_body) else {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_INVAL, 0, Some(&grant));
        return Ok(());
    };
    if count == 0 || count > MAX_IO {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_INVAL, 0, Some(&grant));
        return Ok(());
    }
    // Write buffers are granted R-only (the driver reads from them).
    if grant.rights & R == 0 {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_CAP, 0, Some(&grant));
        return Ok(());
    }
    let bytes = (count as u64 * BLOCK_SIZE as u64) as usize;
    if grant.len < bytes as u64 {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_CAP, 0, Some(&grant));
        return Ok(());
    }
    if lba + count as u64 > dev.blocks() {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_RANGE, 0, Some(&grant));
        return Ok(());
    }

    let src = grant.base as *const u8;
    let dst = (dev.storage_base + lba * BLOCK_SIZE as u64) as *mut u8;
    unsafe { copy::copy_blocks(src, dst, bytes) };
    reply_status(k, handle, tag, RD_WRITE, RD_OK, bytes as u64, Some(&grant));
    Ok(())
}

/// Ramdisk in RAM: flush is a no-op. The handler exists so the protocol
/// surface is stable for a future cached backend.
/// Status reply for a request that carried a transient grant. The grant is
/// MOVED back to the client in the reply (move-return), so it can be
/// re-adopted for the next window=1 request. `grant == None` for replies
/// that carried no cap (RD_FLUSH).
fn reply_status<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    ty: u16,
    status: u16,
    bytes: u64,
    grant: Option<&GrantedCap>,
) {
    let mut p = [0u8; 16 + 10];
    p[..16].copy_from_slice(&RdFrame::new(ty, status != RD_OK).encode());
    p[16..].copy_from_slice(&encode_status_body(status, bytes));
    let send_cap = [SendCap {
        slot: grant.map_or(u32::MAX, |g| g.handle),
        offset: 0,
        len: BLOCK_SIZE,
        rights: grant.map_or(0, |g| g.rights),
        flags: 0,
    }];
    let caps: &[SendCap] = grant.map_or(&[], |_| &send_cap);
    let _ = k.send(handle, tag, F_REPLY, &p, caps, kapi::TIMEOUT_NONE);
}

/// `RD_MAP`: reply carries a durable view of the whole storage region,
/// derived from the driver's storage cap (plan §6.5).
fn reply_map<K: Kernel>(k: &K, handle: u32, tag: u32, dev: &Device) -> Result<(), Close> {
    let mut p = [0u8; 16 + 10];
    p[..16].copy_from_slice(&RdFrame::new(RD_MAP, false).encode());
    p[16..].copy_from_slice(&encode_status_body(RD_OK, 0));

    let rights = if dev.storage_writable { R | W } else { R };
    // v1: storage <= 4 GiB (the CapDescriptor len field is u32).
    let len = dev.storage_len.min(u32::MAX as u64) as u32;
    let cap = SendCap {
        slot: dev.storage_slot,
        offset: 0,
        len,
        rights,
        flags: CAP_PERSIST | CAP_MAP,
    };
    let _ = k.send(
        handle,
        tag,
        F_REPLY,
        &p,
        core::slice::from_ref(&cap),
        kapi::TIMEOUT_NONE,
    );
    Ok(())
}
