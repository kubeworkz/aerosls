#![cfg(feature = "host-fake")]
//! End-to-end arena smoke: two sidecars (simulating Wasm and Lisp runtimes)
//! exchange a 1 MiB f64 array through the shared arena via MEM caps, using
//! the real aerosls channel syscalls (302/303/304) against an in-process
//! fake kernel.
//!
//! ```text
//! Thread A ("Wasm sidecar")                Thread B ("Lisp sidecar")
//!   │                                         │
//!   ├─ arena_alloc(1 MiB) → cap_a            │
//!   ├─ fill cap_a with 125,000 f64s           │
//!   ├─ chan_send(CHAN_W=1,                    │
//!   │   cap_desc{slot=cap_a, len=1MiB})       │
//!   │─────────── msg ───────────►│            │
//!   │                                         ├─ chan_recv(chan_r=1)
//!   │                                         ├─ access cap_a data (zero-copy)
//!   │                                         ├─ compute sqrt for each f64
//!   │                                         ├─ arena_alloc(1 MiB) → cap_b
//!   │                                         ├─ write results to cap_b
//!   │                                         ├─ chan_send(chan_w=2, reply,
//!   │                                         │   cap_desc{slot=cap_b})
//!   │◄──────── msg ─────────────┤            │
//!   ├─ chan_recv result                      │
//!   ├─ access cap_b, verify sqrt values      │
//!   └─ PASS└─
//! ```
//!
//! The fake kernel models syscalls 290 (arena alloc), 302/303 (message
//! send/recv with MEM cap descriptors), and 304 (arena free). Data access
//! bypasses the runtime's `arena_mem` (which requires kernel PTE mapping)
//! and uses raw pointers from the fake kernel's allocation table.

use aerosls::req::*;
use std::collections::{HashMap, VecDeque};
use std::sync::Mutex;
use std::thread;

// ═══════════════════════════════════════════════════════════════════════════
// Fake kernel: arena + message queues
// ═══════════════════════════════════════════════════════════════════════════

const ARENA_SIZE: usize = 4 * 1024 * 1024; // 4 MiB

struct ArenaEntry {
    data_ptr: *mut u8,
    len: usize,
    refcount: u32,
}

struct FakeMsg {
    payload: Vec<u8>,
    caps: Vec<(u16, u32, u32, u8, u8)>,
    tag: u32,
    flags: u32,
}

struct FakeKernel {
    arena_buf: Vec<u8>,
    arena_ptr: usize,
    caps: HashMap<u16, ArenaEntry>,
    next_cap: u16,
    queues: [VecDeque<FakeMsg>; 2],
}

// SAFETY: the raw pointers in ArenaEntry point into arena_buf, which is
// behind a Mutex and only accessed under the lock. Both threads hold the
// same lock before reading/writing.
unsafe impl Send for FakeKernel {}

impl FakeKernel {
    fn new() -> Self {
        let arena_buf = vec![0u8; ARENA_SIZE];
        FakeKernel {
            arena_buf,
            arena_ptr: 0,
            caps: HashMap::new(),
            next_cap: 1,
            queues: [VecDeque::new(), VecDeque::new()],
        }
    }

    fn alloc_cap(&mut self, len: usize, _perm: u8) -> Option<u16> {
        let aligned = (self.arena_ptr + 7) & !7;
        if aligned + len > ARENA_SIZE {
            return None;
        }
        let data_ptr = unsafe { self.arena_buf.as_mut_ptr().add(aligned) };
        let cap = self.next_cap;
        self.next_cap += 1;
        self.caps.insert(cap, ArenaEntry { data_ptr, len, refcount: 1 });
        self.arena_ptr = aligned + len;
        Some(cap)
    }

    fn data_ptr(&self, cap: u16) -> Option<*mut u8> {
        self.caps.get(&cap).map(|e| e.data_ptr)
    }

    fn data_len(&self, cap: u16) -> Option<usize> {
        self.caps.get(&cap).map(|e| e.len)
    }

    fn dec_ref(&mut self, cap: u16) {
        if let Some(e) = self.caps.get_mut(&cap) {
            if e.refcount > 0 {
                e.refcount -= 1;
            }
        }
    }

    fn is_alive(&self, cap: u16) -> bool {
        self.caps.get(&cap).map_or(false, |e| e.refcount > 0)
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Fake syscall handler — fn pointer (no captures), reads kernel from static
// ═══════════════════════════════════════════════════════════════════════════

static mut KERNEL: Option<&'static Mutex<FakeKernel>> = None;

fn direction_for_sender(ch_w: u16) -> usize {
    match ch_w {
        1 => 0,
        2 => 1,
        _ => 0,
    }
}

fn direction_for_receiver(ch_r: u16) -> usize {
    match ch_r {
        1 => 0,
        2 => 1,
        _ => 0,
    }
}

/// The fake syscall — must be a plain `fn` pointer (no captures) to satisfy
/// `set_fake_syscall`'s `fn(u64,u64)->u64` type.
fn fake_syscall(num: u64, arg: u64) -> u64 {
    let k = unsafe { KERNEL.unwrap() };
    match num {
        // ── SYS_CAP_ARENA_ALLOC (290) ──────────────────────────────────
        290 => {
            let req = unsafe { &*(arg as *const ArenaAllocReq) };
            let mut k = k.lock().unwrap();
            match k.alloc_cap(req.npages as usize * 4096, req.perm as u8) {
                Some(cap) => cap as u64,
                None => (CAP_ENOSPC as i64) as u64,
            }
        }

        // ── SYS_CAP_SEND_MSG (302) ────────────────────────────────────
        302 => {
            let req = unsafe { &*(arg as *const SendMsgReq) };
            let payload = if !req.payload.is_null() && req.payload_len > 0 {
                unsafe { std::slice::from_raw_parts(req.payload, req.payload_len as usize) }
                    .to_vec()
            } else {
                Vec::new()
            };
            let mut caps = Vec::new();
            for i in 0..req.n_caps as usize {
                let d = &req.caps[i];
                caps.push((d.slot, d.offset, d.len, d.rights, d.flags));
            }
            let dir = direction_for_sender(req.ch_w_idx);
            let mut k = k.lock().unwrap();
            k.queues[dir].push_back(FakeMsg { payload, caps, tag: req.tag, flags: req.flags });
            0
        }

        // ── SYS_CAP_RECV_MSG (303) ────────────────────────────────────
        303 => {
            let req = unsafe { &mut *(arg as *mut RecvMsgReq) };
            let dir = direction_for_receiver(req.ch_r_idx);
            let msg = {
                let mut k = k.lock().unwrap();
                k.queues[dir].pop_front()
            };
            let Some(msg) = msg else {
                return (CAP_EAGAIN as i64) as u64;
            };
            let copy = msg.payload.len().min(req.buf_len as usize);
            if copy > 0 && !req.buf.is_null() {
                unsafe { std::ptr::copy_nonoverlapping(msg.payload.as_ptr(), req.buf, copy); }
            }
            req.out_payload_len = msg.payload.len() as u32;
            req.out_tag = msg.tag;
            req.out_flags = msg.flags;
            let n = msg.caps.len().min(req.max_caps as usize);
            req.out_n_caps = n as u16;
            let mut k = k.lock().unwrap();
            for i in 0..n {
                let (sender_slot, offset, len, rights, flags) = msg.caps[i];
                if let Some(e) = k.caps.get(&sender_slot) {
                    let data_ptr = e.data_ptr;
                    let data_len = e.len;
                    // Move semantics: the sender's cap is consumed (refcount
                    // decremented), the receiver gets a fresh cap.
                    if let Some(se) = k.caps.get_mut(&sender_slot) {
                        if se.refcount > 0 { se.refcount -= 1; }
                    }
                    let rcap = k.next_cap;
                    k.next_cap += 1;
                    k.caps.insert(rcap, ArenaEntry { data_ptr, len: data_len, refcount: 1 });
                    req.out_caps[i] = CapDesc { slot: rcap, offset, len, rights, flags };
                }
            }
            0
        }

        // ── SYS_CAP_ARENA_FREE (304) ──────────────────────────────────
        304 => {
            let req = unsafe { &*(arg as *const ArenaFreeReq) };
            let mut k = k.lock().unwrap();
            k.dec_ref(req.cap_idx);
            0
        }

        // ── SYS_CAP_MAP (295) / SYS_CAP_UNMAP (296) ──────────────────
        295 | 296 => 0,

        _ => (CAP_ENOSYS as i64) as u64,
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Test
// ═══════════════════════════════════════════════════════════════════════════

const N_F64: usize = 125_000; // 1 MiB / 8

#[test]
fn two_sidecar_arena_round_trip() {
    // Leak a FakeKernel behind a &'static so the `fn` pointer fake_syscall
    // can reach it through the KERNEL static. (Test-only: this is never freed.)
    let k_static: &'static Mutex<FakeKernel> = Box::leak(Box::new(Mutex::new(FakeKernel::new())));
    unsafe { KERNEL = Some(k_static) };

    // ── Thread B: "Lisp sidecar" ────────────────────────────────────────
    let dispatcher = thread::spawn(move || {
        aerosls::set_fake_syscall(Some(fake_syscall));

        loop {
            // Receive the request (syscall 303).
            let mut recv_buf = [0u8; 16];
            let mut recv_caps = [0u16; 4];
            let mut n_caps: u16 = 0;

            unsafe {
                let mut req = RecvMsgReq {
                    ch_r_idx: 1,
                    max_caps: 4,
                    buf: recv_buf.as_mut_ptr(),
                    buf_len: recv_buf.len() as u32,
                    ..RecvMsgReq::default()
                };
                let rc = aerosls::sls_syscall(303, &mut req as *mut RecvMsgReq as u64);
                if rc != 0 {
                    thread::yield_now();
                    continue;
                }
                n_caps = req.out_n_caps;
                for i in 0..n_caps as usize {
                    recv_caps[i] = req.out_caps[i].slot;
                }
            }

            if n_caps == 0 {
                continue;
            }

            // Read input data from the arena via the cap.
            let input_cap = recv_caps[0];
            let (input_ptr, input_len) = {
                let k = k_static.lock().unwrap();
                (
                    k.data_ptr(input_cap).expect("input cap must exist"),
                    k.data_len(input_cap).expect("input cap must have len"),
                )
            };
            let n_values = input_len / 8;
            let input_slice =
                unsafe { std::slice::from_raw_parts(input_ptr as *const f64, n_values) };

            // Compute sqrt for each value.
            let output: Vec<f64> = input_slice.iter().map(|v| v.sqrt()).collect();

            // Allocate output buffer in the arena.
            let output_bytes = output.len() * 8;
            let output_cap = {
                let mut k = k_static.lock().unwrap();
                k.alloc_cap(output_bytes, CAP_PERM_R | CAP_PERM_W).expect("output alloc")
            };
            let output_ptr = {
                let k = k_static.lock().unwrap();
                k.data_ptr(output_cap).expect("output cap")
            };
            unsafe {
                std::ptr::copy_nonoverlapping(output.as_ptr(), output_ptr as *mut f64, output.len());
            }

            // Send reply with the output cap (syscall 302).
            let reply_desc = [
                CapDesc { slot: output_cap, offset: 0, len: output_bytes as u32, rights: CAP_PERM_R, flags: 1 },
                CapDesc::default(), CapDesc::default(), CapDesc::default(),
            ];
            unsafe {
                let req = SendMsgReq {
                    ch_w_idx: 2,
                    n_caps: 1,
                    tag: 0,
                    flags: 0,
                    payload: std::ptr::null(),
                    payload_len: 0,
                    caps: reply_desc,
                    ..SendMsgReq::default()
                };
                aerosls::sls_syscall(302, &req as *const SendMsgReq as u64);
            }
            // Free the received input cap (ownership consumed by the move).
            {
                let mut k = k_static.lock().unwrap();
                k.dec_ref(recv_caps[0]);
            }
            break;
        }
    });

    // ── Thread A: "Wasm sidecar" ────────────────────────────────────────
    let client = thread::spawn(move || {
        aerosls::set_fake_syscall(Some(fake_syscall));

        // Allocate 1 MiB in the arena and fill with test data.
        let input_cap = {
            let mut k = k_static.lock().unwrap();
            k.alloc_cap(N_F64 * 8, CAP_PERM_R | CAP_PERM_W).expect("input alloc")
        };
        let input_ptr = {
            let k = k_static.lock().unwrap();
            k.data_ptr(input_cap).expect("input cap")
        };
        let input_data: Vec<f64> = (0..N_F64).map(|i| i as f64).collect();
        unsafe {
            std::ptr::copy_nonoverlapping(input_data.as_ptr(), input_ptr as *mut f64, N_F64);
        }

        // Send the MEM cap (syscall 302).
        let input_desc = [
            CapDesc { slot: input_cap, offset: 0, len: (N_F64 * 8) as u32, rights: CAP_PERM_R, flags: 1 },
            CapDesc::default(), CapDesc::default(), CapDesc::default(),
        ];
        unsafe {
            let req = SendMsgReq {
                ch_w_idx: 1,
                n_caps: 1,
                tag: 0xAAAA,
                flags: 0,
                payload: std::ptr::null(),
                payload_len: 0,
                caps: input_desc,
                ..SendMsgReq::default()
            };
            aerosls::sls_syscall(302, &req as *const SendMsgReq as u64);
        }

        // Receive the reply (syscall 303).
        let mut recv_buf = [0u8; 16];
        let mut recv_caps = [0u16; 4];
        let mut n_caps: u16 = 0;
        let mut waited = 0u32;
        loop {
            unsafe {
                let mut req = RecvMsgReq {
                    ch_r_idx: 2,
                    max_caps: 4,
                    buf: recv_buf.as_mut_ptr(),
                    buf_len: recv_buf.len() as u32,
                    ..RecvMsgReq::default()
                };
                let rc = aerosls::sls_syscall(303, &mut req as *mut RecvMsgReq as u64);
                if rc == 0 {
                    n_caps = req.out_n_caps;
                    for i in 0..n_caps as usize {
                        recv_caps[i] = req.out_caps[i].slot;
                    }
                    break;
                }
            }
            waited += 1;
            if waited > 100_000_000 {
                panic!("client recv timed out");
            }
            thread::yield_now();
        }

        // Verify the result.
        assert_eq!(n_caps, 1, "reply must carry one cap");
        let rcap_a = recv_caps[0]; // output cap received from Thread B
        let (output_ptr, output_len) = {
            let k = k_static.lock().unwrap();
            (
                k.data_ptr(rcap_a).expect("output cap"),
                k.data_len(rcap_a).expect("output cap len"),
            )
        };
        let n_out = output_len / 8;
        let output_slice =
            unsafe { std::slice::from_raw_parts(output_ptr as *const f64, n_out) };

        assert_eq!(n_out, N_F64, "output must have same count");
        for (i, &val) in output_slice.iter().enumerate() {
            let expected = (i as f64).sqrt();
            let diff = (val - expected).abs();
            assert!(
                diff < 1e-10 || (i == 0 && diff.is_nan()),
                "mismatch at {i}: got {val}, expected {expected}"
            );
        }

        // ── Ownership verification ──────────────────────────────────────
        // After the move, Thread A's original input_cap was consumed by
        // chan_send (the receiver got a fresh copy). Thread A now holds
        // rcap_A — the output cap received from Thread B. Free it to
        // complete the ownership handoff.
        {
            let mut k = k_static.lock().unwrap();
            // input_cap was consumed by the move (refcount already 0)
            assert!(!k.is_alive(input_cap), "input cap consumed by move");
            // rcap_A (output cap received) is still alive — free it
            assert!(k.is_alive(rcap_a), "received output cap alive");
            k.dec_ref(rcap_a);
            assert!(!k.is_alive(rcap_a), "output cap freed");
        }
    });

    client.join().expect("client panicked");
    dispatcher.join().expect("dispatcher panicked");

    // Final: all caps freed, arena is clean.
    let k = k_static.lock().unwrap();
    let total_refs: u32 = k.caps.values().map(|e| e.refcount).sum();
    assert_eq!(total_refs, 0, "all caps freed (no arena leaks)");
}
