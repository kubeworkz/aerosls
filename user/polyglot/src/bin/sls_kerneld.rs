//! sls-kerneld — the Polyglot Nexus kernel transport for the two-process e2e.
//!
//! Implements the channel + arena semantics of syscalls 290 / 295 / 296 /
//! 302 / 303 / 304 over TCP (see `polyglot::transport` for the wire format).
//! It owns the shared arena file, the cap table (offset/len/refcount), and
//! the per-channel FIFO queues. Capability ownership follows the IDL
//! annotations: a descriptor flagged `CAP_FLAG_ARENA_OWNED` (0x02) is *moved*
//! — the sender's refcount is dropped when the receiver takes delivery;
//! `@borrowed` caps keep the sender's slot alive and the receiver drops its
//! fresh handle after use.
//!
//! The arena bytes themselves are never touched here — the sidecars map the
//! same file and read/write it directly (zero-copy); this process only keeps
//! the bump cursor and the refcount bookkeeping.
//!
//! Usage: sls-kerneld --port N [--arena-size MB] [--arena-path PATH]

use polyglot::transport::*;
use std::collections::{HashMap, VecDeque};
use std::io::Write;
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

/// Error codes matching the runtime's CAP_E* (negative, widened through u64).
const CAP_EINVAL: i64 = -4;
const CAP_ENOSPC: i64 = -7;
const CAP_ERANGE: i64 = -8;

/// Cap descriptor flag bit for ownership transfer (aerosls_cap.h).
const CAP_FLAG_ARENA_OWNED: u8 = 0x02;

const MSG_MAX_PAYLOAD: usize = 4096;
const MSG_MAX_CAPS: usize = 4;

#[derive(Clone, Copy)]
struct Cap {
    offset: u32,
    len: u32,
    #[allow(dead_code)] // kept for parity with the pinned ABI table
    rights: u8,
    refcount: u32,
}

struct Msg {
    payload: Vec<u8>,
    tag: u32,
    flags: u32,
    /// (sender slot, offset, len, rights, desc flags)
    caps: Vec<(u16, u32, u32, u8, u8)>,
}

struct KernelState {
    arena_size: u32,
    cursor: u32,
    caps: HashMap<u16, Cap>,
    next_cap: u16,
    queues: [VecDeque<Msg>; 2],
}

impl KernelState {
    fn alloc(&mut self, npages: u32, _perm: u32) -> Result<(u16, u32, u32), i64> {
        let len = (npages as u64 * 4096) as u32;
        if self.cursor as u64 + len as u64 > self.arena_size as u64 {
            return Err(CAP_ENOSPC);
        }
        let offset = (self.cursor + 7) & !7;
        let cap = self.next_cap;
        self.next_cap = self.next_cap.wrapping_add(1);
        if cap == 0 || self.next_cap == 0 {
            return Err(CAP_ENOSPC); // counter exhausted / wrapped
        }
        self.caps.insert(
            cap,
            Cap {
                offset,
                len,
                rights: _perm as u8,
                refcount: 1,
            },
        );
        self.cursor = offset + len;
        Ok((cap, offset, len))
    }

    fn send(&mut self, ch_w: u16, tag: u32, flags: u32, descs: &[WireDesc], payload: &[u8]) -> Result<(), i64> {
        let q = match ch_w {
            1 => 0,
            2 => 1,
            _ => return Err(CAP_EINVAL),
        };
        if payload.len() > MSG_MAX_PAYLOAD {
            return Err(CAP_ERANGE);
        }
        if descs.len() > MSG_MAX_CAPS {
            return Err(CAP_ERANGE);
        }
        let mut caps = Vec::with_capacity(descs.len());
        for d in descs {
            match self.caps.get(&d.slot) {
                Some(c) => caps.push((d.slot, c.offset, c.len, d.rights, d.flags)),
                None => return Err(CAP_EINVAL),
            }
        }
        self.queues[q].push_back(Msg {
            payload: payload.to_vec(),
            tag,
            flags,
            caps,
        });
        Ok(())
    }

    /// Pop a message for `ch_r`, minting receiver caps. Moved caps (desc
    /// flags 0x02) decrement the sender's refcount — ownership transfer.
    fn recv(&mut self, ch_r: u16) -> Option<(Msg, Vec<WireDesc>)> {
        let q = match ch_r {
            1 => 0,
            2 => 1,
            _ => return None,
        };
        let msg = self.queues[q].pop_front()?;
        let mut out = Vec::with_capacity(msg.caps.len());
        for (sender_slot, offset, len, rights, desc_flags) in &msg.caps {
            let slot = self.next_cap;
            self.next_cap = self.next_cap.wrapping_add(1);
            self.caps.insert(
                slot,
                Cap {
                    offset: *offset,
                    len: *len,
                    rights: *rights,
                    refcount: 1,
                },
            );
            if *desc_flags & CAP_FLAG_ARENA_OWNED != 0 {
                if let Some(c) = self.caps.get_mut(sender_slot) {
                    if c.refcount > 0 {
                        c.refcount -= 1;
                    }
                }
            }
            out.push(WireDesc {
                slot,
                offset: *offset,
                len: *len,
                rights: *rights,
                flags: *desc_flags,
            });
        }
        Some((msg, out))
    }

    fn free(&mut self, cap: u16) {
        if let Some(c) = self.caps.get_mut(&cap) {
            if c.refcount > 0 {
                c.refcount -= 1;
            }
        }
    }
}

struct Kernel {
    state: Mutex<KernelState>,
    ready: Condvar,
}

fn main() {
    let mut port = 0u16;
    let mut arena_size_mb = 16u32;
    let mut arena_path: Option<String> = None;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--port" => port = args.next().unwrap().parse().unwrap(),
            "--arena-size" => arena_size_mb = args.next().unwrap().parse().unwrap(),
            "--arena-path" => arena_path = Some(args.next().unwrap()),
            _ => {
                eprintln!("usage: sls-kerneld --port N [--arena-size MB] [--arena-path PATH]");
                std::process::exit(2);
            }
        }
    }
    if port == 0 {
        eprintln!("--port is required");
        std::process::exit(2);
    }

    let arena_path = arena_path.unwrap_or_else(|| format!("/tmp/sls-arena-{port}.bin"));
    let arena_size = arena_size_mb * 1024 * 1024;

    // Create + size the arena file (sidecars mmap it; the kernel never
    // dereferences the bytes, it only keeps the bump cursor + refcounts).
    {
        use std::os::unix::fs::OpenOptionsExt;
        let f = std::fs::OpenOptions::new()
            .create(true)
            .truncate(true)
            .read(true)
            .write(true)
            .custom_flags(libc::O_CLOEXEC)
            .open(&arena_path)
            .unwrap_or_else(|e| panic!("arena file {arena_path}: {e}"));
        f.set_len(arena_size as u64).expect("ftruncate arena");
        f.sync_all().ok();
    }

    let kernel = Arc::new(Kernel {
        state: Mutex::new(KernelState {
            arena_size,
            cursor: 0,
            caps: HashMap::new(),
            next_cap: 1,
            queues: [VecDeque::new(), VecDeque::new()],
        }),
        ready: Condvar::new(),
    });

    let listener = TcpListener::bind(("127.0.0.1", port)).expect("bind");
    println!("READY arena={arena_path}");
    std::io::stdout().flush().ok();

    for conn in listener.incoming() {
        let Ok(stream) = conn else { continue };
        let kernel = Arc::clone(&kernel);
        thread::spawn(move || {
            let _ = handle_conn(&kernel, stream);
        });
    }
}

fn handle_conn(k: &Kernel, mut stream: TcpStream) -> std::io::Result<()> {
    loop {
        // Blocking read: a full frame or connection EOF. The blocking-recv
        // semantics live at the syscall level (condvar), not here.
        let (syscall, body) = match read_frame(&mut stream) {
            Ok(f) => f,
            Err(_) => return Ok(()), // EOF / connection reset
        };

        // For blocking recv, hold the condvar across the wait so a reply
        // enqueued by the other sidecar wakes this thread.
        let reply: Vec<u8> = match syscall {
            SYS_ARENA_ALLOC => {
                let npages = u32::from_le_bytes(body[0..4].try_into().unwrap_or([0; 4]));
                let perm = u32::from_le_bytes(body[4..8].try_into().unwrap_or([0; 4]));
                let mut s = k.state.lock().unwrap();
                match s.alloc(npages, perm) {
                    Ok((cap, offset, len)) => {
                        let mut b = Vec::with_capacity(12);
                        b.extend_from_slice(&0i32.to_le_bytes());
                        b.extend_from_slice(&cap.to_le_bytes());
                        b.extend_from_slice(&offset.to_le_bytes());
                        b.extend_from_slice(&len.to_le_bytes());
                        b
                    }
                    Err(rc) => encode_rc(rc as i32),
                }
            }
            SYS_CAP_SEND_MSG => {
                let Some(req) = parse_send_in(&body) else {
                    // Malformed frame — reply EINVAL and keep serving.
                    write_frame(&mut stream, SYS_CAP_SEND_MSG, &encode_rc(CAP_EINVAL as i32))?;
                    return Ok(());
                };
                let mut s = k.state.lock().unwrap();
                match s.send(req.ch_w, req.tag, req.flags, &req.descs, &req.payload) {
                    Ok(()) => {
                        k.ready.notify_all();
                        encode_rc(0)
                    }
                    Err(rc) => encode_rc(rc as i32),
                }
            }
            SYS_CAP_RECV_MSG => {
                let ch_r = u16::from_le_bytes(body[0..2].try_into().unwrap_or([0; 2]));
                let buf_len = u32::from_le_bytes(body[2..6].try_into().unwrap_or([0; 4]));
                let mut s = k.state.lock().unwrap();
                loop {
                    if let Some((msg, descs)) = s.recv(ch_r) {
                        let payload: Vec<u8> = msg
                            .payload
                            .iter()
                            .take(buf_len.min(MSG_MAX_PAYLOAD as u32) as usize)
                            .cloned()
                            .collect();
                        break encode_recv_out(0, &payload, msg.tag, msg.flags, &descs);
                    }
                    // Nothing queued: block (kernel park semantics — the
                    // generated client stubs do a single recv and must not
                    // observe EAGAIN on a live call).
                    s = k.ready.wait(s).unwrap();
                }
            }
            SYS_CAP_ARENA_FREE => {
                let cap = u16::from_le_bytes(body[0..2].try_into().unwrap_or([0; 2]));
                let mut s = k.state.lock().unwrap();
                s.free(cap);
                encode_rc(0)
            }
            SYS_CAP_MAP | SYS_CAP_UNMAP => encode_rc(0), // sidecars map the file once
            _ => encode_rc(CAP_EINVAL as i32),
        };
        write_frame(&mut stream, syscall, &reply)?;
    }
}
