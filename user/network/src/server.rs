//! The `NET_*` server loop and handlers.
//!
//! The driver is passive and connection-agnostic: one event loop waits on the
//! console channel (control events) and every adopted endpoint (requests),
//! serving `NET_*` on all of them. Frame-level garbage (bad magic/version,
//! unknown type, handshake violation) closes the endpoint with
//! `CLOSE_PROTO`; well-formed requests with bad arguments get an error reply
//! with the client's tag echoed.

use crate::endpoints::{EndpointSet, EndpointState};
use crate::kapi::{self, GrantedCap, Kernel};
use crate::mock::{Addr, MockNetwork, SockType};
use aerosls_proto::*;

/// Why the driver closed an endpoint.
#[derive(Clone, Copy, Debug)]
pub struct Close {
    pub detail: u32,
}

/// Run the server loop until a fatal kernel error.
pub fn run<K: Kernel>(k: &K, eps: &mut EndpointSet, net: &mut MockNetwork) -> Result<(), i32> {
    let mut buf = [0u8; ChanHeader::MAX_PAYLOAD];
    let mut caps = [GrantedCap::default(); ChanHeader::MAX_CAPS];

    loop {
        // Re-scan cap table for newly-wired CHAN endpoints (e.g. the POSIX
        // sidecar's "network" channel, wired after this sidecar booted), and
        // prune endpoints whose channel is gone: when a client sidecar dies,
        // teardown REVOKES this sidecar's CHAN_R cap, and the watchdog-
        // respawned client's fresh channel may occupy the same slot. A
        // stale entry keyed by that slot would block re-adoption (adopt is
        // idempotent on the slot) and the respawned client's NET_INFO would
        // never be served — caught live: NETBOOT FAILED (10 recv retries)
        // after the irqtest driver-death/respawn, boot aborted at netcheck.
        for slot in 0u32..16 {
            if eps.is_console(slot) { continue; }
            match k.cap_info(slot) {
                Ok(info) if info.ty == kapi::CAP_CHAN => {
                    eps.adopt(slot);
                }
                _ => {
                    eps.drop_ep(slot);
                }
            }
        }

        let list = eps.wait_list();
        let wlen = eps.wait_len();
        // Use a 200ms deadline for the discovery poll until at least one
        // client has completed the NET_INFO handshake.  Without this, the
        // network sidecar blocks forever before any endpoint is adopted,
        // so the POSIX sidecar's NET_INFO send queues a message that
        // nobody receives (caught live: NET_RECV returns CAP_ERR_STATE).
        // Always poll with a discovery deadline (never TIMEOUT_NONE): after
        // a client's teardown revokes an adopted endpoint, the respawned
        // client's fresh channel is minted LATER (respawn takes ~100ms), so
        // a prune may find nothing to re-adopt. Parking forever on the
        // console channel would then miss the new channel forever — caught
        // live: NETBOOT FAILED (10 recv retries) on the respawned POSIX.
        // A 200ms re-scan cadence is cheap and bounds adoption latency.
        let timeout = 200_000_000; // 200ms discovery poll
        let (idx, _kind) = match k.wait(&list[..wlen], timeout) {
            Ok(r) => r,
            Err(e) if e == kabi::ERR_TIMEOUT => {
                // Discovery-poll deadline expired: re-scan the cap table
                // for newly wired endpoints and try again.  Any other
                // error (ERR_SHUTDOWN, ...) terminates the loop.
                continue;
            }
            Err(e) if e == kabi::ERR_REVOKED || e == kabi::ERR_STATE => {
                // A listed endpoint's channel was revoked/closed out from
                // under us (its client peer died; teardown). Prune the
                // dead entries and re-scan — the respawned client's fresh
                // channel may already occupy the slot. Not fatal: only
                // ERR_SHUTDOWN ends the loop.
                for slot in 0u32..16 {
                    if eps.is_console(slot) { continue; }
                    if !matches!(k.cap_info(slot), Ok(i) if i.ty == kapi::CAP_CHAN) {
                        eps.drop_ep(slot);
                    }
                }
                continue;
            }
            Err(e) => return Err(e),
        };
        let handle = list[idx];
        if eps.is_console(handle) {
            handle_console(k, eps, &mut buf, &mut caps)?;
            continue;
        }

        let rr = match k.recv(handle, &mut buf, &mut caps) {
            Ok(rr) => rr,
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
                    dispatch(k, handle, state, rr.tag, net, &buf[..rr.len], &caps[..rr.n_caps])
                {
                    let _ = k.close(handle, CLOSE_PROTO, close.detail);
                    eps.drop_ep(handle);
                } else if state == EndpointState::AwaitingHandshake {
                    eps.mark_active(handle);
                }
            }
            _ => {
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
                    let _ = k.close(handle, CLOSE_REVOKED, 0);
                }
            }
        }
        _ => {}
    }
    Ok(())
}

/// Dispatch one request.
fn dispatch<K: Kernel>(
    k: &K,
    handle: u32,
    state: EndpointState,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    let frame = NetFrame::parse(payload).ok_or(Close {
        detail: NET_ERR_PROTO as u32,
    })?;
    if frame.magic != NET_MAGIC || frame.version != NET_VERSION {
        return Err(Close {
            detail: NET_ERR_PROTO as u32,
        });
    }

    // Implicit handshake: first message must be NET_INFO with no caps.
    if state == EndpointState::AwaitingHandshake {
        if frame.ty != NET_INFO || !caps.is_empty() {
            return Err(Close {
                detail: NET_ERR_PROTO as u32,
            });
        }
    }

    match frame.ty {
        NET_INFO => reply_info(k, handle, tag, net),
        NET_SOCKET => handle_socket(k, handle, tag, net, payload),
        NET_BIND => handle_bind(k, handle, tag, net, payload),
        NET_CONNECT => handle_connect(k, handle, tag, net, payload),
        NET_LISTEN => handle_listen(k, handle, tag, net, payload),
        NET_ACCEPT => handle_accept(k, handle, tag, net, payload),
        NET_SEND => handle_send(k, handle, tag, net, payload, caps),
        NET_RECV => handle_recv(k, handle, tag, net, payload, caps),
        NET_SHUTDOWN => handle_shutdown(k, handle, tag, net, payload),
        NET_CLOSE_SOCK => handle_close_sock(k, handle, tag, net, payload),
        NET_POLL => handle_poll(k, handle, tag, net, payload),
        _ => Err(Close {
            detail: NET_ERR_INVAL as u32,
        }),
    }
}

// ── helpers ──────────────────────────────────────────────────────────────────

fn net_reply_frame(ty: u16, error: bool) -> [u8; NetFrame::SIZE] {
    NetFrame::new(ty, error).encode()
}

fn reply_status<K: Kernel>(k: &K, handle: u32, tag: u32, ty: u16, status: u16) {
    let mut p = [0u8; 16 + 10];
    p[..16].copy_from_slice(&net_reply_frame(ty, status != NET_OK));
    p[16..].copy_from_slice(&encode_status_body(status, 0));
    let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
}

/// Read a u32 LE value from the payload starting at `offset`.
fn read_u32_le(payload: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        payload[offset],
        payload[offset + 1],
        payload[offset + 2],
        payload[offset + 3],
    ])
}

/// Return the socket-id from the first 4 bytes after the frame header.
fn sock_id_from_payload(payload: &[u8]) -> Result<u32, Close> {
    if payload.len() < NetFrame::SIZE + 4 {
        return Err(Close {
            detail: NET_ERR_INVAL as u32,
        });
    }
    Ok(read_u32_le(payload, NetFrame::SIZE))
}

// ── handlers ────────────────────────────────────────────────────────────────

fn reply_info<K: Kernel>(k: &K, handle: u32, tag: u32, net: &MockNetwork) -> Result<(), Close> {
    let mut p = [0u8; 16 + 12];
    p[..16].copy_from_slice(&net_reply_frame(NET_INFO, false));
    let active = net.sockets.iter().flatten().count() as u32;
    let max = crate::mock::MAX_SOCKETS as u32 - active;
    p[16..28].copy_from_slice(&encode_net_info_body(max, 1500, 0));
    let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
    Ok(())
}

fn handle_socket<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let body = payload.get(NetFrame::SIZE..).and_then(|b| parse_net_socket_body(b));
    let Some((sock_type, _protocol)) = body else {
        reply_status(k, handle, tag, NET_SOCKET, NET_ERR_INVAL);
        return Ok(());
    };

    let st = match sock_type {
        SOCK_STREAM => SockType::Stream,
        SOCK_DGRAM => SockType::Datagram,
        _ => {
            reply_status(k, handle, tag, NET_SOCKET, NET_ERR_INVAL);
            return Ok(());
        }
    };

    match net.socket(st) {
        Ok(id) => {
            let mut p = [0u8; 16 + 10];
            p[..16].copy_from_slice(&net_reply_frame(NET_SOCKET, false));
            p[16..].copy_from_slice(&encode_status_body(NET_OK, id as u64));
            let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_SOCKET, e);
            Ok(())
        }
    }
}

fn handle_bind<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let start = NetFrame::SIZE;
    let sock_id = sock_id_from_payload(payload)?;
    let addr = payload.get(start + 4..start + 10)
        .and_then(|b| parse_sockaddr(b))
        .ok_or(Close { detail: NET_ERR_INVAL as u32 })?;

    let (ip, port) = addr;
    match net.bind(sock_id, Addr { ip, port }) {
        Ok(()) => {
            reply_status(k, handle, tag, NET_BIND, NET_OK);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_BIND, e);
            Ok(())
        }
    }
}

fn handle_connect<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let start = NetFrame::SIZE;
    let sock_id = sock_id_from_payload(payload)?;
    let addr = payload.get(start + 4..start + 10)
        .and_then(|b| parse_sockaddr(b))
        .ok_or(Close { detail: NET_ERR_INVAL as u32 })?;

    let (ip, port) = addr;
    match net.connect(sock_id, Addr { ip, port }) {
        Ok(()) => {
            reply_status(k, handle, tag, NET_CONNECT, NET_OK);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_CONNECT, e);
            Ok(())
        }
    }
}

fn handle_listen<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let sock_id = sock_id_from_payload(payload)?;
    match net.listen(sock_id) {
        Ok(()) => {
            reply_status(k, handle, tag, NET_LISTEN, NET_OK);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_LISTEN, e);
            Ok(())
        }
    }
}

fn handle_accept<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let sock_id = sock_id_from_payload(payload)?;
    match net.accept(sock_id) {
        Ok((peer_id, addr)) => {
            let mut p = [0u8; 16 + 10];
            p[..16].copy_from_slice(&net_reply_frame(NET_ACCEPT, false));
            p[16..26].copy_from_slice(&encode_net_accept_body(peer_id, addr.ip, addr.port));
            let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_ACCEPT, e);
            Ok(())
        }
    }
}

fn handle_send<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    let sock_id = sock_id_from_payload(payload)?;

    // Data comes from the transient buffer grant (R-only buffer).
    let Some(grant) = caps.first() else {
        reply_status(k, handle, tag, NET_SEND, NET_ERR_CAP);
        return Ok(());
    };
    if grant.rights & R == 0 {
        reply_status(k, handle, tag, NET_SEND, NET_ERR_CAP);
        return Ok(());
    }

    // SAFETY: the kernel minted this grant and checked bounds.
    let data = unsafe {
        core::slice::from_raw_parts(grant.base as *const u8, grant.len as usize)
    };

    match net.send(sock_id, data) {
        Ok(n) => {
            let mut p = [0u8; 16 + 10];
            p[..16].copy_from_slice(&net_reply_frame(NET_SEND, false));
            p[16..].copy_from_slice(&encode_status_body(NET_OK, n as u64));
            let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_SEND, e);
            Ok(())
        }
    }
}

fn handle_recv<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
    caps: &[GrantedCap],
) -> Result<(), Close> {
    let sock_id = sock_id_from_payload(payload)?;

    // Write buffer grant (W-only).
    let Some(grant) = caps.first() else {
        reply_status(k, handle, tag, NET_RECV, NET_ERR_CAP);
        return Ok(());
    };
    if grant.rights & W == 0 {
        reply_status(k, handle, tag, NET_RECV, NET_ERR_CAP);
        return Ok(());
    }

    let buf = unsafe {
        core::slice::from_raw_parts_mut(grant.base as *mut u8, grant.len as usize)
    };

    match net.recv(sock_id, buf) {
        Ok(n) => {
            let mut p = [0u8; 16 + 10];
            p[..16].copy_from_slice(&net_reply_frame(NET_RECV, false));
            p[16..].copy_from_slice(&encode_status_body(NET_OK, n as u64));
            let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_RECV, e);
            Ok(())
        }
    }
}

fn handle_shutdown<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let body = payload.get(NetFrame::SIZE..).and_then(|b| parse_net_shutdown_body(b));
    let Some((sock_id, how)) = body else {
        reply_status(k, handle, tag, NET_SHUTDOWN, NET_ERR_INVAL);
        return Ok(());
    };

    match net.shutdown(sock_id, how) {
        Ok(()) => {
            reply_status(k, handle, tag, NET_SHUTDOWN, NET_OK);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_SHUTDOWN, e);
            Ok(())
        }
    }
}

fn handle_close_sock<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &mut MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let sock_id = sock_id_from_payload(payload)?;
    match net.close_socket(sock_id) {
        Ok(()) => {
            reply_status(k, handle, tag, NET_CLOSE_SOCK, NET_OK);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_CLOSE_SOCK, e);
            Ok(())
        }
    }
}

fn handle_poll<K: Kernel>(
    k: &K,
    handle: u32,
    tag: u32,
    net: &MockNetwork,
    payload: &[u8],
) -> Result<(), Close> {
    let sock_id = sock_id_from_payload(payload)?;
    match net.poll(sock_id) {
        Ok(events) => {
            let mut p = [0u8; 16 + 6];
            p[..16].copy_from_slice(&net_reply_frame(NET_POLL, false));
            p[16..22].copy_from_slice(&encode_net_poll_body(sock_id, events));
            let _ = k.send(handle, tag, F_REPLY, &p, &[], kapi::TIMEOUT_NONE);
            Ok(())
        }
        Err(e) => {
            reply_status(k, handle, tag, NET_POLL, e);
            Ok(())
        }
    }
}
