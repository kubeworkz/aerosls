//! The RD_* server loop and handlers (the `blockdevice.aeroidl` contract).
//!
//! The driver is passive and connection-agnostic: one event loop waits on
//! the console channel (control events) and every adopted endpoint
//! (requests), serving RD_* on all of them — the same loop shape as the
//! ramdisk driver, parameterized over the block backend. Frame-level garbage
//! (bad magic/version, unknown type, handshake violation) closes the endpoint
//! with `CLOSE_PROTO`; well-formed requests with bad arguments get an error
//! reply with the client's tag echoed — the kernel's reply pairing and the
//! protocol's correlation are the same number.
//!
//! Request buffers arrive as transient MEM grants (read buffers are W-only,
//! write buffers are R-only, per the blockdevice contract). The kernel
//! minted, bounded, and auto-revoked them; the driver re-checks rights and
//! size before a single byte moves (kernel checked rights, driver checks
//! size — neither trusts the other).

use crate::backend::{BackendErr, BlockBackend, MapGrant, MAX_SECTORS, SECTOR_SIZE};
use crate::endpoints::{EndpointSet, EndpointState};
use crate::kapi::{self, GrantedCap, Kernel, SendCap};
use aerosls_proto::*;

/// Why the driver closed an endpoint. `detail` is an `RD_ERR_*` code,
/// carried in the close event (transport spec §6.2).
#[derive(Clone, Copy, Debug)]
pub struct Close {
    pub detail: u32,
}

/// Run the server loop until a fatal kernel error (e.g. the fake kernel's
/// shutdown on driver death). Never returns on a healthy kernel — `wait`
/// blocks forever with `TIMEOUT_NONE`; the driver has no timers (backoff and
/// respawn are the Device Manager's job).
pub fn run<K: Kernel, B: BlockBackend>(
    k: &K,
    eps: &mut EndpointSet,
    dev: &mut B,
) -> Result<(), i32> {
    let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
    let mut caps = [GrantedCap::default(); ChanHeader::MAX_CAPS];

    loop {
        let list = eps.wait_list();
        let (idx, _kind) = k.wait(&list[..eps.wait_len()], kapi::TIMEOUT_NONE)?;
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
                if let Err(close) =
                    dispatch(k, handle, state, rr.tag, dev, &buf[..rr.len], &caps[..rr.n_caps])
                {
                    let _ = k.close(handle, CLOSE_PROTO, close.detail);
                    eps.drop_ep(handle);
                } else if state == EndpointState::AwaitingHandshake {
                    // The handshake request (first RD_INFO, no caps) was
                    // accepted: the endpoint is now active.
                    eps.mark_active(handle);
                }
            }
            _ => {
                // CLOSE (client died / timed out) or an unexpected kind:
                // drop the endpoint. The kernel reaped any transient grants
                // bound to it; nothing to clean here.
                eps.drop_ep(handle);
            }
        }
    }
}

/// Console is control-only: `NEW_CHANNEL` adopts, everything else is a
/// kernel bug we log-and-drop.
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
fn dispatch<K: Kernel, B: BlockBackend>(
    k: &K,
    handle: u32,
    state: EndpointState,
    tag: u32,
    dev: &mut B,
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
    // with no caps. Matches the client's attach sequence, which sends RD_INFO
    // first by construction.
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
        RD_FLUSH => flush_device(k, handle, tag, dev),
        RD_MAP => reply_map(k, handle, tag, dev),
        _ => Err(Close {
            detail: RD_ERR_INVAL as u32,
        }),
    }
}

/// Map a backend failure to its RD_ERR_* code.
fn map_err(e: BackendErr) -> u16 {
    match e {
        BackendErr::Io => RD_ERR_IO,
        BackendErr::Busy => RD_ERR_BUSY,
        BackendErr::NoMem => RD_ERR_NOMEM,
        BackendErr::Ro => RD_ERR_RO,
    }
}

// ── handlers ─────────────────────────────────────────────────────────────────

fn reply_info<K: Kernel, B: BlockBackend>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &B,
) -> Result<(), Close> {
    let mut p = [0u8; 32];
    p[..16].copy_from_slice(&RdFrame::new(RD_INFO, false).encode());
    let flags: u32 = if dev.read_only() { 1 } else { 0 };
    p[16..32].copy_from_slice(&encode_info_body(SECTOR_SIZE, dev.total_sectors(), flags));
    let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
    Ok(())
}

fn read_blocks<K: Kernel, B: BlockBackend>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &mut B,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    let Some((lba, count)) = payload.get(16..).and_then(parse_rw_body) else {
        reply_status(k, handle, tag, RD_READ, RD_ERR_INVAL, 0);
        return Ok(());
    };
    if count == 0 || count > MAX_SECTORS {
        reply_status(k, handle, tag, RD_READ, RD_ERR_INVAL, 0);
        return Ok(());
    }
    let Some(grant) = caps.first() else {
        reply_status(k, handle, tag, RD_READ, RD_ERR_CAP, 0);
        return Ok(());
    };
    // The kernel minted min(held, requested); re-check what actually arrived.
    if grant.rights & W == 0 {
        reply_status(k, handle, tag, RD_READ, RD_ERR_CAP, 0);
        return Ok(());
    }
    let bytes = (count as u64 * BLOCK_SIZE as u64) as usize;
    if grant.len < bytes as u64 {
        reply_status(k, handle, tag, RD_READ, RD_ERR_CAP, 0);
        return Ok(());
    }
    if lba + count as u64 > dev.total_sectors() {
        reply_status(k, handle, tag, RD_READ, RD_ERR_RANGE, 0);
        return Ok(());
    }

    match dev.read(lba, count, grant.base, bytes) {
        Ok(()) => reply_status(k, handle, tag, RD_READ, RD_OK, bytes as u64),
        Err(e) => reply_status(k, handle, tag, RD_READ, map_err(e), 0),
    }
    Ok(())
}

fn write_blocks<K: Kernel, B: BlockBackend>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &mut B,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    if dev.read_only() {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_RO, 0);
        return Ok(());
    }
    let Some((lba, count)) = payload.get(16..).and_then(parse_rw_body) else {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_INVAL, 0);
        return Ok(());
    };
    if count == 0 || count > MAX_SECTORS {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_INVAL, 0);
        return Ok(());
    }
    let Some(grant) = caps.first() else {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_CAP, 0);
        return Ok(());
    };
    // Write buffers are granted R-only (the driver reads from them).
    if grant.rights & R == 0 {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_CAP, 0);
        return Ok(());
    }
    let bytes = (count as u64 * BLOCK_SIZE as u64) as usize;
    if grant.len < bytes as u64 {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_CAP, 0);
        return Ok(());
    }
    if lba + count as u64 > dev.total_sectors() {
        reply_status(k, handle, tag, RD_WRITE, RD_ERR_RANGE, 0);
        return Ok(());
    }

    match dev.write(lba, count, grant.base, bytes) {
        Ok(()) => reply_status(k, handle, tag, RD_WRITE, RD_OK, bytes as u64),
        Err(e) => reply_status(k, handle, tag, RD_WRITE, map_err(e), 0),
    }
    Ok(())
}

fn flush_device<K: Kernel, B: BlockBackend>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &mut B,
) -> Result<(), Close> {
    match dev.flush() {
        Ok(()) => reply_status(k, handle, tag, RD_FLUSH, RD_OK, 0),
        Err(e) => reply_status(k, handle, tag, RD_FLUSH, map_err(e), 0),
    }
    Ok(())
}

/// `RD_MAP`: reply carries a durable view of the whole device, derived from
/// the backend's map grant — the zero-copy path. A backend without a direct
/// map (the real NVMe device) answers `RD_ERR_NOMEM`; clients then use
/// READ/WRITE.
fn reply_map<K: Kernel, B: BlockBackend>(
    k: &K,
    handle: u32,
    tag: u32,
    dev: &B,
) -> Result<(), Close> {
    let Some(m) = dev.map() else {
        reply_status(k, handle, tag, RD_MAP, RD_ERR_NOMEM, 0);
        return Ok(());
    };
    let mut p = [0u8; 16 + 10];
    p[..16].copy_from_slice(&RdFrame::new(RD_MAP, false).encode());
    p[16..].copy_from_slice(&encode_status_body(RD_OK, 0));

    let cap = map_grant_send_cap(&m);
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

fn map_grant_send_cap(m: &MapGrant) -> SendCap {
    let rights = if m.writable { R | W } else { R };
    // v1: device <= 4 GiB (the CapDescriptor len field is u32).
    let len = m.len.min(u32::MAX as u64) as u32;
    SendCap {
        slot: m.slot,
        offset: 0,
        len,
        rights,
        flags: CAP_PERSIST | CAP_MAP,
    }
}

fn reply_status<K: Kernel>(k: &K, handle: u32, tag: u32, ty: u16, status: u16, bytes: u64) {
    let mut p = [0u8; 16 + 10];
    p[..16].copy_from_slice(&RdFrame::new(ty, status != RD_OK).encode());
    p[16..].copy_from_slice(&encode_status_body(status, bytes));
    let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backend_err_mapping() {
        assert_eq!(map_err(BackendErr::Io), RD_ERR_IO);
        assert_eq!(map_err(BackendErr::Busy), RD_ERR_BUSY);
        assert_eq!(map_err(BackendErr::NoMem), RD_ERR_NOMEM);
        assert_eq!(map_err(BackendErr::Ro), RD_ERR_RO);
    }

    #[test]
    fn map_grant_cap_build() {
        let m = MapGrant {
            slot: 4,
            base: 0x1000,
            len: 8 * 512,
            writable: true,
        };
        let c = map_grant_send_cap(&m);
        assert_eq!(c.slot, 4);
        assert_eq!(c.offset, 0);
        assert_eq!(c.len, 8 * 512);
        assert_eq!(c.rights, R | W);
        assert_eq!(c.flags, CAP_PERSIST | CAP_MAP);

        let ro = MapGrant {
            slot: 4,
            base: 0x1000,
            len: 8 * 512,
            writable: false,
        };
        assert_eq!(map_grant_send_cap(&ro).rights, R);
    }
}
