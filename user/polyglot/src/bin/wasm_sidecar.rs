//! wasm-sidecar — the Wasm sidecar of the two-process Polyglot e2e.
//!
//! A Rust process embedding the `wasmi` WebAssembly runtime. The guest
//! module (`guest/calc_guest.wat`) is the *caller*: it orchestrates the two
//! CalculatorService calls entirely from inside wasm, addressing the shared
//! arena through host imports. The channel marshaling underneath is the real
//! generated AeroIDL client (`gen_calculator`), whose six FFI externs are
//! provided by the real `aerosls` runtime, whose `sls_syscall` is pointed at
//! `sls-kerneld` — over TCP (`--transport tcp`) or over the shared-memory
//! rings (`--transport shm`: message rings ring0/ring1, arena rings
//! ring2/ring3), so the same stubs run against both transports.
//!
//!   guest .wat  →  host imports (this file)  →  generated client stubs
//!      →  aerosls runtime externs  →  fake syscall  →  rings/TCP  →  sls-kerneld
//!
//! Usage: wasm-sidecar --port N --arena PATH [--chan PATH] [--transport tcp|shm]

extern crate alloc;

use aerosls::req::{ArenaAllocReq, ArenaFreeReq, CapDesc, SendMsgReq, RecvMsgReq, CAP_ENOSYS, CAP_PERM_R, CAP_PERM_W};
use polyglot::ring::{self, Ring};
use polyglot::transport::*;
use std::net::TcpStream;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::Mutex;

#[path = "../gen_calculator.rs"]
#[allow(non_camel_case_types, unused_imports, unused_variables, dead_code)]
mod gen_calculator;

// ── transport globals (the fake syscall + host imports read these) ─────────

static TRANSPORT: Mutex<Option<TcpStream>> = Mutex::new(None);
/// (cap id, arena offset, len) for every cap this sidecar holds.
static CAP_TABLE: Mutex<Vec<(u16, u32, u32)>> = Mutex::new(Vec::new());
/// Base of the sidecar's MAP_SHARED mapping of the arena file.
static MAP_BASE: AtomicUsize = AtomicUsize::new(0);
/// rdtsc round-trip samples collected inside the `call_add` host import.
static BENCH_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// rdtsc samples for the arena-cap path, collected inside `call_sqrt_batch`.
static BENCH_SQRT_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// Lisp-side compute samples (ns) for the sqrt bench, index-aligned with
/// BENCH_SQRT_SAMPLES (same call order) — the split lets CI attribute the
/// leg's latency to Lisp sqrts (compute) vs ring/dispatch overhead.
static BENCH_SQRT_COMPUTE_NS: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// rdtsc samples for the string path, collected inside `call_reverse`.
static BENCH_STR_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// Lisp-side compute samples (ns) for the add bench, index-aligned with
/// BENCH_SAMPLES — same split convention as the SQRT/STR legs.
static BENCH_ADD_COMPUTE_NS: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// Lisp-side compute samples (ns) for the string bench, index-aligned with
/// BENCH_STR_SAMPLES — same split convention as the SQRT leg.
static BENCH_STR_COMPUTE_NS: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// Baseline round-trip samples (cycles): local wasm-side call, Linux pipe,
/// Unix socketpair — each at 1-byte and 64-KiB payloads (the design doc's
/// T9/T10 and T11/T12 pairs). Collected once per sidecar run by
/// `run_baseline_benches` (transport-independent), so both legs report the
/// same numbers — the design's T1/T9-T12 comparison points.
static BENCH_LOCAL_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
static BENCH_PIPE_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
static BENCH_PIPE_64K_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
static BENCH_SOCK_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
static BENCH_SOCK_64K_SAMPLES: Mutex<Vec<u64>> = Mutex::new(Vec::new());
/// Shared-memory channel rings (set when --transport shm): ring0 = wasm→lisp
/// requests, ring1 = lisp→wasm replies, ring2 = wasm→kerneld arena requests,
/// ring3 = kerneld→wasm arena replies.
static RING0: Mutex<Option<Ring>> = Mutex::new(None);
static RING1: Mutex<Option<Ring>> = Mutex::new(None);
static RING2: Mutex<Option<Ring>> = Mutex::new(None);
static RING3: Mutex<Option<Ring>> = Mutex::new(None);
/// Dedicated async result channel (T13/T14): ring6 = wasm→lisp async
/// requests, ring7 = lisp→wasm async results. The heavy_reduce result
/// arrives here, not on the sync reply ring.
static RING6: Mutex<Option<Ring>> = Mutex::new(None);
static RING7: Mutex<Option<Ring>> = Mutex::new(None);
/// True when the message path goes through the shared ring instead of TCP.
static RING_MODE: AtomicBool = AtomicBool::new(false);
/// Count of async results (T13/T14) received AND verified on the dedicated
/// result ring. Guards against the async section being silently skipped
/// (e.g. a broken transport probe) while the guest still exits 0: the
/// verdict requires BOTH the small (64 f64s -> 2016) and the 1 MiB
/// (131072 f64s -> 4128768) heavy_reduce results to verify, so a tooth
/// that corrupts either variant's expected value makes the leg fail.
static ASYNC_RESULTS_VERIFIED: AtomicU32 = AtomicU32::new(0);
/// T4-T8 payload-size sweep samples, bucketed by size index (6 sizes:
/// 4KiB, 16KiB, 64KiB, 256KiB, 1MiB, 8MiB). One inner Vec per bucket,
/// filled by call_sqrt_sweep / call_str_sweep; the report prints a
/// per-size median and the e2e gates each size's transport sub-linearity
/// (zero-copy claim: the arena bytes never cross the transport, so latency
/// must NOT scale linearly with payload size).
static BENCH_SWEEP_SQRT: Mutex<Vec<Vec<u64>>> = Mutex::new(Vec::new());
static BENCH_SWEEP_SQRT_COMPUTE: Mutex<Vec<Vec<u64>>> = Mutex::new(Vec::new());
static BENCH_SWEEP_STR: Mutex<Vec<Vec<u64>>> = Mutex::new(Vec::new());
static BENCH_SWEEP_STR_COMPUTE: Mutex<Vec<Vec<u64>>> = Mutex::new(Vec::new());

/// Monotonic cycle counter for round-trip timing. On x86_64 this is the real
/// TSC (constant-rate on modern CPUs); elsewhere it falls back to monotonic
/// nanoseconds, so relative deltas are still meaningful.
#[cfg(target_arch = "x86_64")]
fn rdtsc() -> u64 {
    unsafe { core::arch::x86_64::_rdtsc() }
}
#[cfg(not(target_arch = "x86_64"))]
fn rdtsc() -> u64 {
    static START: std::sync::OnceLock<std::time::Instant> = std::sync::OnceLock::new();
    START
        .get_or_init(std::time::Instant::now)
        .elapsed()
        .as_nanos() as u64
}

/// TSC → ns conversion factor (cycles per nanosecond), calibrated once at
/// first use. On non-x86_64 the fallback rdtsc() already returns ns, so the
/// ratio is 1.0 and the ns output stays correct.
static CYCLES_PER_NS: std::sync::OnceLock<f64> = std::sync::OnceLock::new();

fn calibrate_tsc() -> f64 {
    let t0 = rdtsc();
    let i0 = std::time::Instant::now();
    std::thread::sleep(std::time::Duration::from_millis(10));
    let t1 = rdtsc();
    let i1 = std::time::Instant::now();
    let dt_ns = i1.duration_since(i0).as_nanos() as f64;
    t1.wrapping_sub(t0) as f64 / dt_ns
}

/// Convert a cycle-count delta to nanoseconds (wall clock, via calibration).
fn cycles_to_ns(cycles: u64) -> u64 {
    let cpn = *CYCLES_PER_NS.get_or_init(calibrate_tsc);
    (cycles as f64 / cpn) as u64
}

fn rpc(syscall: u32, body: &[u8]) -> Vec<u8> {
    let mut g = TRANSPORT.lock().unwrap();
    let stream = g.as_mut().expect("transport not connected");
    write_frame(stream, syscall, body).expect("write frame");
    let (_, reply) = read_frame(stream).expect("read frame");
    reply
}

/// Record a cap → (offset, len) so host imports can address the arena.
fn cap_table_insert(cap: u16, offset: u32, len: u32) {
    let mut t = CAP_TABLE.lock().unwrap();
    t.retain(|(c, _, _)| *c != cap);
    t.push((cap, offset, len));
}

fn cap_table_offset(cap: u16) -> Option<u32> {
    CAP_TABLE
        .lock()
        .unwrap()
        .iter()
        .find(|(c, _, _)| *c == cap)
        .map(|(_, o, _)| *o)
}

fn cap_table_lookup(cap: u16) -> Option<(u32, u32)> {
    CAP_TABLE
        .lock()
        .unwrap()
        .iter()
        .find(|(c, _, _)| *c == cap)
        .map(|(_, o, l)| (*o, *l))
}

fn ring_mode() -> bool {
    RING_MODE.load(Ordering::SeqCst)
}

/// Arena alloc/free over the shared ring: frame = `[syscall u32][req body]`
/// (the syscall discriminates 290 alloc from 304 free, mirroring the TCP
/// frame header); the reply is the exact TCP reply body, so the parsing
/// below is shared with the TCP path.
fn arena_rpc_ring(syscall: u32, req: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(4 + req.len());
    frame.extend_from_slice(&syscall.to_le_bytes());
    frame.extend_from_slice(req);
    RING2.lock().unwrap().as_ref().unwrap().send(&frame).expect("ring2 send");
    RING3.lock().unwrap().as_ref().unwrap().recv().expect("ring3 recv")
}

fn cap_table_remove(cap: u16) {
    CAP_TABLE.lock().unwrap().retain(|(c, _, _)| *c != cap);
}

// ── fake syscall: routes the real request structs over rings or TCP ────────

// u64 const patterns (match arms can't use `X as u64` expressions).
const S_ARENA_ALLOC: u64 = SYS_ARENA_ALLOC as u64;
const S_CAP_MAP: u64 = SYS_CAP_MAP as u64;
const S_CAP_UNMAP: u64 = SYS_CAP_UNMAP as u64;
const S_SEND_MSG: u64 = SYS_CAP_SEND_MSG as u64;
const S_RECV_MSG: u64 = SYS_CAP_RECV_MSG as u64;
const S_ARENA_FREE: u64 = SYS_CAP_ARENA_FREE as u64;

fn fake_syscall(num: u64, arg: u64) -> u64 {
    match num {
        S_ARENA_ALLOC => {
            let req = unsafe { &*(arg as *const ArenaAllocReq) };
            let reply = if ring_mode() {
                arena_rpc_ring(SYS_ARENA_ALLOC, &encode_alloc_in(req.npages, req.perm as u32))
            } else {
                rpc(
                    SYS_ARENA_ALLOC,
                    &encode_alloc_in(req.npages, req.perm as u32),
                )
            };
            let rc = i32::from_le_bytes(reply[0..4].try_into().unwrap());
            if rc != 0 {
                return (rc as i64) as u64;
            }
            let cap = u16::from_le_bytes(reply[4..6].try_into().unwrap());
            let offset = u32::from_le_bytes(reply[6..10].try_into().unwrap());
            let len = u32::from_le_bytes(reply[10..14].try_into().unwrap());
            cap_table_insert(cap, offset, len);
            cap as u64
        }
        S_SEND_MSG => {
            let req = unsafe { &*(arg as *const SendMsgReq) };
            let mut descs = Vec::with_capacity(req.n_caps as usize);
            for d in &req.caps[..req.n_caps as usize] {
                // Resolve the real arena (offset, len) from the local cap
                // table: in TCP mode kerneld ignores the wire values (it
                // keeps its own table); in ring mode they ARE the memory
                // capability the receiver addresses directly.
                let (off, len) = cap_table_lookup(d.slot).unwrap_or((d.offset, d.len));
                descs.push(WireDesc {
                    slot: d.slot,
                    offset: off,
                    len,
                    rights: d.rights,
                    flags: d.flags,
                });
            }
            let payload =
                unsafe { core::slice::from_raw_parts(req.payload, req.payload_len as usize) };
            if ring_mode() {
                // Zero-copy path: push the request straight into the shared
                // ring — no kernel round trip for the message. Both ring
                // directions use the 303-reply wire body (rc, plen, tag,
                // flags, n_caps, descs, payload) because that is what both
                // sides' chan_recv already parse; in the TCP path kerneld
                // bridged the two formats, here the sidecars talk directly.
                let frame = encode_recv_out(0, payload, req.tag, req.flags, &descs);
                RING0.lock().unwrap().as_ref().unwrap().send(&frame).expect("ring0 send");
                return 0;
            }
            let reply = rpc(
                SYS_CAP_SEND_MSG,
                &encode_send_in(req.ch_w_idx, req.n_caps, req.tag, req.flags, &descs, payload),
            );
            (parse_rc(&reply) as i64) as u64
        }
        S_RECV_MSG => {
            let req = unsafe { &mut *(arg as *mut RecvMsgReq) };
            let out = if ring_mode() {
                // Block until the reply arrives in ring1 (spin on the
                // consumer cursor — the Lisp side pushes it after dispatch).
                let body = RING1.lock().unwrap().as_ref().unwrap().recv().expect("ring1 recv");
                parse_recv_out(&body).expect("bad ring recv body")
            } else {
                let reply = rpc(SYS_CAP_RECV_MSG, &encode_recv_in(req.ch_r_idx, req.buf_len));
                parse_recv_out(&reply).expect("bad recv reply")
            };
            if out.rc != 0 {
                return (out.rc as i64) as u64;
            }
            unsafe {
                if !req.buf.is_null() && !out.payload.is_empty() {
                    core::ptr::copy_nonoverlapping(
                        out.payload.as_ptr(),
                        req.buf,
                        out.payload.len(),
                    );
                }
                req.out_payload_len = out.payload.len() as u32;
                req.out_tag = out.tag;
                req.out_flags = out.flags;
                req.out_n_caps = out.descs.len() as u16;
                for (i, d) in out.descs.iter().enumerate() {
                    req.out_caps[i] = CapDesc {
                        slot: d.slot,
                        offset: d.offset,
                        len: d.len,
                        rights: d.rights,
                        flags: d.flags,
                        ..CapDesc::default()
                    };
                    cap_table_insert(d.slot, d.offset, d.len);
                }
            }
            0
        }
        S_ARENA_FREE => {
            let req = unsafe { &*(arg as *const ArenaFreeReq) };
            if ring_mode() {
                let _ = arena_rpc_ring(SYS_CAP_ARENA_FREE, &encode_free_in(req.cap_idx));
            } else {
                let _ = rpc(SYS_CAP_ARENA_FREE, &encode_free_in(req.cap_idx));
            }
            cap_table_remove(req.cap_idx);
            0
        }
        S_CAP_MAP | S_CAP_UNMAP => 0,
        _ => (CAP_ENOSYS as i64) as u64,
    }
}

// ── host imports for the wasm guest ────────────────────────────────────────

fn arena_ptr(cap: u16, idx: usize) -> *mut u8 {
    let base = MAP_BASE.load(Ordering::SeqCst);
    let off = cap_table_offset(cap).expect("cap not in table") as usize;
    (base + off + idx * 8) as *mut u8
}

/// Byte-granular arena pointer (strings/bytes; no *8 element scaling).
fn arena_byte_ptr(cap: u16, idx: usize) -> *mut u8 {
    let base = MAP_BASE.load(Ordering::SeqCst);
    let off = cap_table_offset(cap).expect("cap not in table") as usize;
    (base + off + idx) as *mut u8
}

fn pack(status: i64, value: u64) -> i64 {
    ((status as u64) << 32 | value) as i64
}

/// Time `f` N times with rdtsc and record every round trip (cycles). The
/// vector is preallocated so nothing allocates during the measurement
/// window. Used for the baseline legs (local call, pipe, Unix socketpair),
/// which are transport-independent and run once per sidecar invocation.
fn bench_roundtrip<F: FnMut()>(n: usize, mut f: F) -> Vec<u64> {
    let mut samples = Vec::with_capacity(n);
    for _ in 0..n {
        let t0 = rdtsc();
        f();
        samples.push(rdtsc().wrapping_sub(t0));
    }
    samples
}

/// Run the design's comparison baselines (T1/T9-T12) once per sidecar run:
/// a local wasm-side function call, Linux pipe write+read round trips at
/// 1 byte and 64 KiB, and Unix socketpair send+recv round trips at the same
/// two sizes. They answer "how much faster is a cross-sidecar call than
/// ordinary IPC" and are gated by the e2e like the cross-sidecar legs.
/// Linux-only (the sidecar already is).
#[cfg(target_os = "linux")]
fn run_baseline_benches() {
    // T1: local call — a plain function call with trivial body, no kernel
    // involvement, ~ns on any modern core.
    let mut sink = 0u64;
    let local = bench_roundtrip(100_000, || {
        sink = sink.wrapping_add(1);
        core::hint::black_box(sink);
    });
    BENCH_LOCAL_SAMPLES.lock().unwrap().extend(local);

    // T9: Linux pipe, 1 byte — write 1 byte, read it back (kernel buffer,
    // two syscalls per round trip).
    let mut fds = [0i32; 2];
    let rc = unsafe { libc::pipe(fds.as_mut_ptr()) };
    assert_eq!(rc, 0, "pipe() failed");
    let pipe = bench_roundtrip(100_000, || {
        let mut buf = [0x5au8; 1];
        unsafe {
            assert_eq!(libc::write(fds[1], buf.as_ptr() as *const _, 1), 1);
            assert_eq!(libc::read(fds[0], buf.as_mut_ptr() as *mut _, 1), 1);
        }
        core::hint::black_box(&buf);
    });
    BENCH_PIPE_SAMPLES.lock().unwrap().extend(pipe);

    // T10: Linux pipe, 64 KiB — write 64 KiB, read it back. Grow the pipe
    // buffer first (F_SETPIPE_SZ) so a full 64 KiB write never blocks on a
    // full buffer (the default 64 KiB pipe capacity would deadlock: the
    // writer would block until a reader drains, but the reader is us, after
    // the write). The payload stays in the arena-sized range the design's
    // H6 covers.
    let rc = unsafe { libc::fcntl(fds[1], libc::F_SETPIPE_SZ, 512 * 1024) };
    assert!(rc >= 0, "F_SETPIPE_SZ failed");
    let pipe_64k = bench_roundtrip(20_000, || {
        let mut buf = [0x5au8; 64 * 1024];
        unsafe {
            assert_eq!(libc::write(fds[1], buf.as_ptr() as *const _, buf.len()), buf.len() as isize);
            assert_eq!(libc::read(fds[0], buf.as_mut_ptr() as *mut _, buf.len()), buf.len() as isize);
        }
        core::hint::black_box(&buf);
    });
    BENCH_PIPE_64K_SAMPLES.lock().unwrap().extend(pipe_64k);
    unsafe {
        libc::close(fds[0]);
        libc::close(fds[1]);
    }

    // T11: Unix socketpair, 1 byte — send 1 byte, recv it back (stream
    // socket, two syscalls per round trip, still the kernel's socket
    // machinery).
    let mut sv = [0i32; 2];
    let rc = unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, sv.as_mut_ptr()) };
    assert_eq!(rc, 0, "socketpair() failed");
    let sock = bench_roundtrip(100_000, || {
        let mut buf = [0x5au8; 1];
        unsafe {
            assert_eq!(libc::send(sv[1], buf.as_ptr() as *const _, 1, 0), 1);
            assert_eq!(libc::recv(sv[0], buf.as_mut_ptr() as *mut _, 1, 0), 1);
        }
        core::hint::black_box(&buf);
    });
    BENCH_SOCK_SAMPLES.lock().unwrap().extend(sock);

    // T12: Unix socketpair, 64 KiB — send 64 KiB, recv it back. MSG_WAITALL
    // makes recv return the full payload instead of a partial stream read;
    // the send buffer is large enough that a single 64 KiB send completes.
    let sock_64k = bench_roundtrip(20_000, || {
        let mut buf = [0x5au8; 64 * 1024];
        unsafe {
            assert_eq!(libc::send(sv[1], buf.as_ptr() as *const _, buf.len(), 0), buf.len() as isize);
            assert_eq!(
                libc::recv(sv[0], buf.as_mut_ptr() as *mut _, buf.len(), libc::MSG_WAITALL),
                buf.len() as isize
            );
        }
        core::hint::black_box(&buf);
    });
    BENCH_SOCK_64K_SAMPLES.lock().unwrap().extend(sock_64k);
    unsafe {
        libc::close(sv[0]);
        libc::close(sv[1]);
    }
}

/// Median/p99/mean report for the baseline legs (no compute split — the
/// baseline work is local, nothing crosses a language boundary).
fn print_bench_baseline(tag: &str, samples: &[u64], note: &str) {
    let samples = samples.get(1..).unwrap_or(&samples[..]);
    if samples.is_empty() {
        println!("BENCH_{tag}: no baseline samples ({note})");
        return;
    }
    let mut s = samples.to_vec();
    s.sort_unstable();
    let n = s.len();
    let median = s[n / 2];
    let p99 = s[((n as f64 * 0.99) as usize).min(n - 1)];
    let mean = s.iter().sum::<u64>() / n as u64;
    println!(
        "BENCH_{tag}: N={n} median_ns={} p99_ns={} mean_ns={} ({note})",
        cycles_to_ns(median),
        cycles_to_ns(p99),
        cycles_to_ns(mean)
    );
}

/// Print `BENCH_{tag}: ...` with median/p99/mean in cycles. The first sample
/// is dropped (the guest's verification call — the cold path).
/// Print `BENCH_SQRT: ...` with the total/compute/transport split. The total
/// is the wasm-side round trip (cycles), compute is the Lisp-side dotimes
/// (ns, carried in the reply), transport is derived as total - compute — so
/// CI can watch whether the leg is Lisp sqrts or ring overhead. The two
/// sample vectors are index-aligned (same call order); the first sample of
/// each is dropped as warmup.
fn print_bench_split(
    tag: &str,
    total_cy: &[u64],
    compute_ns: &[u64],
    note: &str,
    transport: &str,
) {
    let n_common = total_cy.len().min(compute_ns.len());
    let total_cy = total_cy.get(1..n_common).unwrap_or(&total_cy[..]);
    let compute_ns = compute_ns.get(1..n_common).unwrap_or(&compute_ns[..]);
    if !total_cy.is_empty() && total_cy.len() == compute_ns.len() {
        let mut ts = total_cy.to_vec();
        ts.sort_unstable();
        let mut cs = compute_ns.to_vec();
        cs.sort_unstable();
        let n = ts.len();
        let median_cy = ts[n / 2];
        let median_ns = cycles_to_ns(median_cy);
        let p99_ns = cycles_to_ns(ts[((n as f64 * 0.99) as usize).min(n - 1)]);
        let mean_ns = cycles_to_ns(ts.iter().sum::<u64>() / n as u64);
        let median_compute = cs[n / 2];
        let p99_compute = cs[((n as f64 * 0.99) as usize).min(n - 1)];
        // transport = total - compute, clamped at 0 (a Lisp clock that runs
        // ahead of the wasm calibration would otherwise go negative).
        let median_transport = median_ns.saturating_sub(median_compute);
        println!(
            "BENCH_{tag}: N={n} median_cy={median_cy} median_ns={median_ns} p99_ns={p99_ns} \
             compute_ns={median_compute} compute_p99_ns={p99_compute} transport_ns={median_transport} \
             mean_ns={mean_ns} ({note}, transport={transport})"
        );
        let leg = tag.to_ascii_lowercase();
        println!(
            "BENCH_JSON {{\"leg\":\"{leg}\",\"transport\":\"{transport}\",\"n\":{n},\"median_cy\":{median_cy},\"median_ns\":{median_ns},\"compute_ns\":{median_compute},\"transport_ns\":{median_transport},\"p99_ns\":{p99_ns},\"mean_ns\":{mean_ns}}}"
        );
    } else {
        println!("BENCH_{tag}: no round-trip samples ({note}, transport={transport})");
        println!(
            "BENCH_JSON {{\"leg\":\"{}\",\"transport\":\"{transport}\",\"n\":0}}",
            tag.to_ascii_lowercase()
        );
    }
}

/// Print the T4-T8 payload-size sweep: one line per size with the
/// total/compute/transport split (same machinery as print_bench_split) plus
/// a BENCH_JSON line carrying the size (KiB) so CI can archive per-size
/// medians. The zero-copy claim is that transport stays ~flat as the
/// payload grows 4KiB -> 8MiB — the bytes never cross the transport.
fn print_bench_sweep(
    tag: &str,
    total_buckets: &[Vec<u64>],
    compute_buckets: &[Vec<u64>],
    sizes_kib: &[u64],
    note: &str,
    transport: &str,
) {
    for (b, total_cy) in total_buckets.iter().enumerate() {
        let compute_ns = compute_buckets.get(b).cloned().unwrap_or_default();
        let n_common = total_cy.len().min(compute_ns.len());
        let tc = total_cy.get(1..n_common).unwrap_or(&total_cy[..]);
        let cc = compute_ns.get(1..n_common).unwrap_or(&compute_ns[..]);
        let size = sizes_kib.get(b).copied().unwrap_or(0);
        let leg = format!("{}-{}k", tag.to_ascii_lowercase(), size);
        if tc.is_empty() || tc.len() != cc.len() {
            println!("BENCH_SWEEP_{tag}: size={size}KiB no samples ({note}, transport={transport})");
            println!(
                "BENCH_JSON {{\"leg\":\"{leg}\",\"transport\":\"{transport}\",\"size_kib\":{size},\"n\":0}}"
            );
            continue;
        }
        let mut ts = tc.to_vec();
        ts.sort_unstable();
        let mut cs = cc.to_vec();
        cs.sort_unstable();
        let n = ts.len();
        let median_cy = ts[n / 2];
        let median_ns = cycles_to_ns(median_cy);
        let p99_ns = cycles_to_ns(ts[((n as f64 * 0.99) as usize).min(n - 1)]);
        let mean_ns = cycles_to_ns(ts.iter().sum::<u64>() / n as u64);
        let median_compute = cs[n / 2];
        let median_transport = median_ns.saturating_sub(median_compute);
        println!(
            "BENCH_SWEEP_{tag}: size={size}KiB N={n} median_cy={median_cy} median_ns={median_ns} \
             p99_ns={p99_ns} compute_ns={median_compute} transport_ns={median_transport} \
             mean_ns={mean_ns} ({note}, transport={transport})"
        );
        println!(
            "BENCH_JSON {{\"leg\":\"{leg}\",\"transport\":\"{transport}\",\"size_kib\":{size},\"n\":{n},\"median_cy\":{median_cy},\"median_ns\":{median_ns},\"compute_ns\":{median_compute},\"transport_ns\":{median_transport},\"p99_ns\":{p99_ns},\"mean_ns\":{mean_ns}}}"
        );
    }
}

fn main() {
    let mut port = 0u16;
    let mut arena_path = String::new();
    let mut chan_path = String::new();
    let mut transport = String::from("tcp");
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--port" => port = args.next().unwrap().parse().unwrap(),
            "--arena" => arena_path = args.next().unwrap(),
            "--chan" => chan_path = args.next().unwrap(),
            "--transport" => transport = args.next().unwrap(),
            _ => {
                eprintln!("usage: wasm-sidecar --port N --arena PATH [--chan PATH] [--transport tcp|shm]");
                std::process::exit(2);
            }
        }
    }
    if port == 0 || arena_path.is_empty() {
        eprintln!("--port and --arena are required");
        std::process::exit(2);
    }
    if transport != "tcp" && transport != "shm" {
        eprintln!("--transport must be tcp or shm");
        std::process::exit(2);
    }
    if transport == "shm" {
        if chan_path.is_empty() {
            eprintln!("--chan is required with --transport shm");
            std::process::exit(2);
        }
        *RING0.lock().unwrap() = Some(Ring::open(&chan_path, ring::RING0_OFFSET).expect("open ring0"));
        *RING1.lock().unwrap() = Some(Ring::open(&chan_path, ring::RING1_OFFSET).expect("open ring1"));
        *RING2.lock().unwrap() = Some(Ring::open(&chan_path, ring::RING2_OFFSET).expect("open ring2"));
        *RING3.lock().unwrap() = Some(Ring::open(&chan_path, ring::RING3_OFFSET).expect("open ring3"));
        *RING6.lock().unwrap() = Some(Ring::open(&chan_path, ring::RING6_OFFSET).expect("open ring6"));
        *RING7.lock().unwrap() = Some(Ring::open(&chan_path, ring::RING7_OFFSET).expect("open ring7"));
        RING_MODE.store(true, Ordering::SeqCst);
        println!("TRANSPORT shm chan={chan_path}");
    } else {
        println!("TRANSPORT tcp");
    }

    // Map the shared arena file (the zero-copy path: both sidecars see the
    // same pages; the kernel transport only books refcounts).
    let file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(&arena_path)
        .expect("open arena");
    let len = file.metadata().unwrap().len() as usize;
    let base = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_SHARED,
            std::fs::File::as_raw_fd(&file),
            0,
        )
    };
    assert!(base != libc::MAP_FAILED, "mmap arena failed");
    MAP_BASE.store(base as usize, Ordering::SeqCst);
    drop(file);

    // Connect the transport and point the runtime's syscall seam at it.
    let stream = TcpStream::connect(("127.0.0.1", port)).expect("connect to sls-kerneld");
    // Request/response protocol — disable Nagle so the two writes of each
    // frame (header + body) and the ping-pong exchange don't stall on
    // delayed ACKs (the classic ~40ms-per-exchange TCP latency trap).
    stream.set_nodelay(true).expect("set_nodelay");
    *TRANSPORT.lock().unwrap() = Some(stream);
    aerosls::set_fake_syscall(Some(fake_syscall));
    // Bootstrap the generated client's endpoints (ch1 = request, ch2 = reply).
    unsafe {
        gen_calculator::calculator_service::CHAN_W = 1;
        gen_calculator::calculator_service::CHAN_R = 2;
    }

    // ── embed wasmi and instantiate the guest ─────────────────────────────
    let wat = include_str!("../../guest/calc_guest.wat");
    let wasm = wat::parse_str(wat).expect("parse .wat");
    let engine = wasmi::Engine::default();
    let module = wasmi::Module::new(&engine, &wasm[..]).expect("compile module");
    let mut store = wasmi::Store::new(&engine, ());
    let mut linker = wasmi::Linker::new(&engine);

    linker
        .func_wrap("host", "call_add", |_caller: wasmi::Caller<'_, ()>, a: i32, b: i32| -> i64 {
            let t0 = rdtsc();
            let r = match gen_calculator::calculator_service::add(a, b) {
                Ok(v) => pack(0, v as u32 as u64),
                Err(e) => pack(1, e.code as u64),
            };
            // Record the full wasm -> lisp -> wasm round trip in cycles. The
            // push happens after the stop timestamp, so it can't inflate it.
            // The Lisp-reported compute (ns) rides the reply so the report
            // can split total into compute + transport.
            let total = rdtsc().wrapping_sub(t0);
            BENCH_SAMPLES.lock().unwrap().push(total);
            let compute = unsafe { gen_calculator::calculator_service::LAST_ADD_COMPUTE_NS };
            BENCH_ADD_COMPUTE_NS.lock().unwrap().push(compute);
            r
        })
        .unwrap();
    linker
        .func_wrap(
            "host",
            "call_sqrt_batch",
            |_caller: wasmi::Caller<'_, ()>, count: i32, input_cap: i32| -> i64 {
                let count = count as u32;
                let cap = input_cap as u16;
                let t0 = rdtsc();
                let r = match gen_calculator::calculator_service::sqrt_batch(
                    gen_calculator::ArenaSlice::<f64>::new(cap, count),
                    cap,
                    count,
                ) {
                    Ok(slice) => pack(0, slice.cap as u64),
                    Err(e) => pack(1, e.code as u64),
                };
                // Arena-cap round trip: the data stays in the shared mapping;
                // only the MEM cap + refcounts cross the transport. The
                // Lisp-reported compute (ns) is recorded alongside so the
                // report can split total into compute + transport.
                let total = rdtsc().wrapping_sub(t0);
                BENCH_SQRT_SAMPLES.lock().unwrap().push(total);
                let compute = unsafe { gen_calculator::calculator_service::LAST_SQRT_COMPUTE_NS };
                BENCH_SQRT_COMPUTE_NS.lock().unwrap().push(compute);
                r
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "call_sqrt_sweep",
            |_caller: wasmi::Caller<'_, ()>, count: i32, input_cap: i32, bucket: i32| -> i64 {
                let count = count as u32;
                let cap = input_cap as u16;
                let t0 = rdtsc();
                let r = match gen_calculator::calculator_service::sqrt_batch(
                    gen_calculator::ArenaSlice::<f64>::new(cap, count),
                    cap,
                    count,
                ) {
                    Ok(slice) => pack(0, slice.cap as u64),
                    Err(e) => pack(1, e.code as u64),
                };
                // Same arena-cap round trip as call_sqrt_batch, but recorded
                // into the per-size bucket so the report can gate the
                // zero-copy claim: transport must stay ~flat as the payload
                // grows 4KiB -> 1MiB (the bytes never cross the transport).
                let total = rdtsc().wrapping_sub(t0);
                let compute = unsafe { gen_calculator::calculator_service::LAST_SQRT_COMPUTE_NS };
                let mut buckets = BENCH_SWEEP_SQRT.lock().unwrap();
                while buckets.len() <= bucket as usize {
                    buckets.push(Vec::new());
                }
                buckets[bucket as usize].push(total);
                let mut comp = BENCH_SWEEP_SQRT_COMPUTE.lock().unwrap();
                while comp.len() <= bucket as usize {
                    comp.push(Vec::new());
                }
                comp[bucket as usize].push(compute);
                r
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "write_f64",
            |_caller: wasmi::Caller<'_, ()>, cap: i32, idx: i32, bits: i64| {
                unsafe {
                    *(arena_ptr(cap as u16, idx as usize) as *mut u64) = bits as u64;
                }
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "read_f64",
            |_caller: wasmi::Caller<'_, ()>, cap: i32, idx: i32| -> i64 {
                unsafe { *(arena_ptr(cap as u16, idx as usize) as *const u64) as i64 }
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "arena_alloc",
            |_caller: wasmi::Caller<'_, ()>, size: i32| -> i32 {
                aerosls::arena_alloc(size as u32, CAP_PERM_R | CAP_PERM_W) as i32
            },
        )
        .unwrap();
    linker
        .func_wrap("host", "arena_free", |_caller: wasmi::Caller<'_, ()>, cap: i32| {
            aerosls::arena_free(cap as u16);
        })
        .unwrap();
    linker
        .func_wrap("host", "is_shm", |_caller: wasmi::Caller<'_, ()>| -> i32 {
            if ring_mode() { 1 } else { 0 }
        })
        .unwrap();
    linker
        .func_wrap(
            "host",
            "call_reverse",
            |_caller: wasmi::Caller<'_, ()>, cap: i32, len: i32| -> i64 {
                let t0 = rdtsc();
                let r = match gen_calculator::calculator_service::reverse(cap as u16, len as u32) {
                    Ok(slice) => pack(0, slice.cap as u64),
                    Err(e) => pack(1, e.code as u64),
                };
                // String arena-cap round trip: input + reversed output stay in
                // the shared mapping; only the MEM caps + bytes counts cross.
                // The Lisp-reported compute (ns) rides the reply so the
                // report can split total into compute + transport.
                let total = rdtsc().wrapping_sub(t0);
                BENCH_STR_SAMPLES.lock().unwrap().push(total);
                let compute = unsafe { gen_calculator::calculator_service::LAST_REVERSE_COMPUTE_NS };
                BENCH_STR_COMPUTE_NS.lock().unwrap().push(compute);
                r
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "call_str_sweep",
            |_caller: wasmi::Caller<'_, ()>, cap: i32, len: i32, bucket: i32| -> i64 {
                let t0 = rdtsc();
                let r = match gen_calculator::calculator_service::reverse(cap as u16, len as u32) {
                    Ok(slice) => pack(0, slice.cap as u64),
                    Err(e) => pack(1, e.code as u64),
                };
                // Byte/string arena path, recorded into the per-size bucket
                // (T4-T8 sweep): the bytes never cross the transport, so the
                // median must stay sub-linear in payload size.
                let total = rdtsc().wrapping_sub(t0);
                let compute = unsafe { gen_calculator::calculator_service::LAST_REVERSE_COMPUTE_NS };
                let mut buckets = BENCH_SWEEP_STR.lock().unwrap();
                while buckets.len() <= bucket as usize {
                    buckets.push(Vec::new());
                }
                buckets[bucket as usize].push(total);
                let mut comp = BENCH_SWEEP_STR_COMPUTE.lock().unwrap();
                while comp.len() <= bucket as usize {
                    comp.push(Vec::new());
                }
                comp[bucket as usize].push(compute);
                r
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "call_heavy_reduce",
            |_caller: wasmi::Caller<'_, ()>, input_cap: i32, count: i32| -> i64 {
                // Async fire-and-forget: send heavy_reduce with NO_REPLY and
                // return the immediate ACK status. The real result arrives
                // later on the dedicated result ring (await_async_result).
                let r = match gen_calculator::calculator_service::heavy_reduce(
                    gen_calculator::ArenaSlice::<f64>::new(input_cap as u16, count as u32),
                    input_cap as u16,
                    count as u32,
                ) {
                    Ok(()) => pack(0, 0),
                    Err(e) => pack(1, e.code as u64),
                };
                // NO_REPLY means the dispatch loop's immediate ACK still lands
                // on ring1 — consume it so the sync reply path stays clean.
                if ring_mode() {
                    if let Some(ring1) = RING1.lock().unwrap().as_ref() {
                        let _ack = ring1.recv().expect("ack recv");
                    }
                }
                r
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "await_async_result",
            |_caller: wasmi::Caller<'_, ()>, expected: i32| -> i64 {
                // Block on the dedicated result ring until the async result
                // arrives, then parse the 303-reply body: ok@0, u32 value@4..8.
                // The guest passes the expected sum for the variant it ran
                // (T13: 2016, T14: 4128768), so a verified await implies the
                // worker's result was correct AND the right variant ran.
                let body = RING7.lock().unwrap().as_ref().expect("ring7 open").recv().expect("async result recv");
                if body.len() < 22 {
                    return pack(1, 1);
                }
                let ok = body[18];
                let val = u32::from_le_bytes(body[22..26].try_into().unwrap_or([0; 4]));
                if ok == 1 && val == expected as u32 {
                    ASYNC_RESULTS_VERIFIED.fetch_add(1, Ordering::SeqCst);
                }
                pack(if ok == 1 { 0 } else { 1 }, val as u64)
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "write_u8",
            |_caller: wasmi::Caller<'_, ()>, cap: i32, idx: i32, byte: i32| {
                unsafe { *(arena_byte_ptr(cap as u16, idx as usize) as *mut u8) = byte as u8 };
            },
        )
        .unwrap();
    linker
        .func_wrap(
            "host",
            "read_u8",
            |_caller: wasmi::Caller<'_, ()>, cap: i32, idx: i32| -> i32 {
                unsafe { *(arena_byte_ptr(cap as u16, idx as usize) as *const u8) as i32 }
            },
        )
        .unwrap();
    linker
        .func_wrap("host", "log", |caller: wasmi::Caller<'_, ()>, ptr: i32, len: i32| {
            let mem = caller
                .get_export("memory")
                .and_then(|e| e.into_memory())
                .expect("guest memory");
            let data = mem.data(&caller);
            let start = ptr as usize;
            let end = start.saturating_add(len as usize).min(data.len());
            eprintln!("[guest] {}", String::from_utf8_lossy(&data[start..end]));
        })
        .unwrap();

    let instance = linker
        .instantiate(&mut store, &module)
        .expect("instantiate")
        .start(&mut store)
        .expect("start");
    let run = instance
        .get_typed_func::<(), i32>(&mut store, "run")
        .expect("run export");

    let status = run.call(&mut store, ()).expect("guest run");

    // ── baseline reports (T1/T9-T12): local call, pipe, Unix socketpair ──
    // Transport-independent; run once per sidecar invocation so both legs
    // report the same ordinary-IPC comparison points.
    run_baseline_benches();
    let local_samples = std::mem::take(&mut *BENCH_LOCAL_SAMPLES.lock().unwrap());
    let pipe_samples = std::mem::take(&mut *BENCH_PIPE_SAMPLES.lock().unwrap());
    let pipe_64k = std::mem::take(&mut *BENCH_PIPE_64K_SAMPLES.lock().unwrap());
    let sock_samples = std::mem::take(&mut *BENCH_SOCK_SAMPLES.lock().unwrap());
    let sock_64k = std::mem::take(&mut *BENCH_SOCK_64K_SAMPLES.lock().unwrap());
    print_bench_baseline("LOCAL", &local_samples, "local wasm-side function call (T1)");
    print_bench_baseline("PIPE_1B", &pipe_samples, "Linux pipe 1B write+read round trip (T9)");
    print_bench_baseline("PIPE_64K", &pipe_64k, "Linux pipe 64KiB write+read round trip (T10)");
    print_bench_baseline("SOCK_1B", &sock_samples, "Unix socketpair 1B send+recv round trip (T11)");
    print_bench_baseline("SOCK_64K", &sock_64k, "Unix socketpair 64KiB send+recv round trip (T12)");

    // ── latency reports: median/p99 of the guest's round trips ────────────
    // Drop the first sample of each leg — the guest's verification calls,
    // i.e. the cold path (warmup for connection/TCP/buffers).
    let add_samples = std::mem::take(&mut *BENCH_SAMPLES.lock().unwrap());
    let add_compute = std::mem::take(&mut *BENCH_ADD_COMPUTE_NS.lock().unwrap());
    let sqrt_samples = std::mem::take(&mut *BENCH_SQRT_SAMPLES.lock().unwrap());
    let sqrt_compute = std::mem::take(&mut *BENCH_SQRT_COMPUTE_NS.lock().unwrap());
    let str_samples = std::mem::take(&mut *BENCH_STR_SAMPLES.lock().unwrap());
    let str_compute = std::mem::take(&mut *BENCH_STR_COMPUTE_NS.lock().unwrap());
    print_bench_split("ADD", &add_samples, &add_compute, "add() — inline args", &transport);
    print_bench_split("SQRT", &sqrt_samples, &sqrt_compute, "sqrt_batch(4096 f64) — arena MEM cap", &transport);
    print_bench_split("STR", &str_samples, &str_compute, "reverse(32..63 B) — string arena MEM cap", &transport);
    // T4-T8 payload-size sweep: per-size medians over 4KiB..1MiB. The gate
    // (in the e2e) checks transport sub-linearity — the zero-copy claim
    // that the arena bytes never cross the transport.
    let sweep_sqrt = std::mem::take(&mut *BENCH_SWEEP_SQRT.lock().unwrap());
    let sweep_sqrt_c = std::mem::take(&mut *BENCH_SWEEP_SQRT_COMPUTE.lock().unwrap());
    let sweep_str = std::mem::take(&mut *BENCH_SWEEP_STR.lock().unwrap());
    let sweep_str_c = std::mem::take(&mut *BENCH_SWEEP_STR_COMPUTE.lock().unwrap());
    let sizes_kib = [4u64, 16, 64, 256, 1024, 8192];
    print_bench_sweep("SQRT", &sweep_sqrt, &sweep_sqrt_c, &sizes_kib, "sqrt_batch size sweep (T4-T8)", &transport);
    print_bench_sweep("STR", &sweep_str, &sweep_str_c, &sizes_kib, "reverse size sweep (T4-T8)", &transport);

    if status == 0 {
        println!("WASM_SIDECAR PASS");
        // T13/T14 async verdict: on the shm leg the guest MUST have run the
        // heavy_reduce async section (transport probe true) and received BOTH
        // verified results off the dedicated result ring — the small variant
        // (64 f64s -> 2016) and the 1 MiB variant (131072 f64s -> 4128768).
        // The count is incremented only by a successful await_async_result
        // with the right expected sum, so a skipped or failed async path
        // cannot exit 0 silently. Printed after WASM_SIDECAR so CI logs show
        // both lines; the exit code is what the e2e gates on.
        if ring_mode() {
            if ASYNC_RESULTS_VERIFIED.load(Ordering::SeqCst) >= 2 {
                println!("ASYNC_SIDECAR PASS: both heavy_reduce results (T13 + T14) received+verified on result ring");
                std::process::exit(0);
            } else {
                println!(
                    "ASYNC_SIDECAR FAIL: async results never received/verified (verified={})",
                    ASYNC_RESULTS_VERIFIED.load(Ordering::SeqCst)
                );
                std::process::exit(1);
            }
        }
        std::process::exit(0);
    } else {
        println!("WASM_SIDECAR FAIL status={status}");
        std::process::exit(1);
    }
}
