//! wasm-sidecar — the Wasm sidecar of the two-process Polyglot e2e.
//!
//! A Rust process embedding the `wasmi` WebAssembly runtime. The guest
//! module (`guest/calc_guest.wat`) is the *caller*: it orchestrates the two
//! CalculatorService calls entirely from inside wasm, addressing the shared
//! arena through host imports. The channel marshaling underneath is the real
//! generated AeroIDL client (`gen_calculator`), whose six FFI externs are
//! provided by the real `aerosls` runtime, whose `sls_syscall` is pointed at
//! `sls-kerneld` over TCP.
//!
//!   guest .wat  →  host imports (this file)  →  generated client stubs
//!      →  aerosls runtime externs  →  fake syscall  →  TCP  →  sls-kerneld
//!
//! Usage: wasm-sidecar --port N --arena PATH

extern crate alloc;

use aerosls::req::{ArenaAllocReq, ArenaFreeReq, CapDesc, SendMsgReq, RecvMsgReq, CAP_ENOSYS, CAP_PERM_R, CAP_PERM_W};
use polyglot::transport::*;
use std::net::TcpStream;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicUsize, Ordering};
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

fn cap_table_remove(cap: u16) {
    CAP_TABLE.lock().unwrap().retain(|(c, _, _)| *c != cap);
}

// ── fake syscall: routes the real request structs over TCP ─────────────────

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
            let reply = rpc(
                SYS_ARENA_ALLOC,
                &encode_alloc_in(req.npages, req.perm as u32),
            );
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
                descs.push(WireDesc {
                    slot: d.slot,
                    offset: d.offset,
                    len: d.len,
                    rights: d.rights,
                    flags: d.flags,
                });
            }
            let payload =
                unsafe { core::slice::from_raw_parts(req.payload, req.payload_len as usize) };
            let reply = rpc(
                SYS_CAP_SEND_MSG,
                &encode_send_in(req.ch_w_idx, req.n_caps, req.tag, req.flags, &descs, payload),
            );
            (parse_rc(&reply) as i64) as u64
        }
        S_RECV_MSG => {
            let req = unsafe { &mut *(arg as *mut RecvMsgReq) };
            let reply = rpc(SYS_CAP_RECV_MSG, &encode_recv_in(req.ch_r_idx, req.buf_len));
            let out = parse_recv_out(&reply).expect("bad recv reply");
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
            let _ = rpc(SYS_CAP_ARENA_FREE, &encode_free_in(req.cap_idx));
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

fn pack(status: i64, value: u64) -> i64 {
    ((status as u64) << 32 | value) as i64
}

/// Print `BENCH_{tag}: ...` with median/p99/mean in cycles. The first sample
/// is dropped (the guest's verification call — the cold path).
fn print_bench(tag: &str, samples: &[u64], note: &str) {
    let samples = samples.get(1..).unwrap_or(&samples[..]);
    if !samples.is_empty() {
        let mut s = samples.to_vec();
        s.sort_unstable();
        let n = s.len();
        let median = s[n / 2];
        let p99 = s[((n as f64 * 0.99) as usize).min(n - 1)];
        let mean = s.iter().sum::<u64>() / n as u64;
        println!(
            "BENCH_{tag}: N={n} median_cy={median} median_ns={} p99_cy={p99} p99_ns={} mean_cy={mean} ({note})",
            cycles_to_ns(median),
            cycles_to_ns(p99),
        );
    } else {
        println!("BENCH_{tag}: no round-trip samples ({note})");
    }
}

fn main() {
    let mut port = 0u16;
    let mut arena_path = String::new();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--port" => port = args.next().unwrap().parse().unwrap(),
            "--arena" => arena_path = args.next().unwrap(),
            _ => {
                eprintln!("usage: wasm-sidecar --port N --arena PATH");
                std::process::exit(2);
            }
        }
    }
    if port == 0 || arena_path.is_empty() {
        eprintln!("--port and --arena are required");
        std::process::exit(2);
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
            BENCH_SAMPLES.lock().unwrap().push(rdtsc().wrapping_sub(t0));
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
                // only the MEM cap + refcounts cross the transport.
                BENCH_SQRT_SAMPLES.lock().unwrap().push(rdtsc().wrapping_sub(t0));
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

    // ── latency reports: median/p99 of the guest's round trips ────────────
    // Drop the first sample of each leg — the guest's verification calls,
    // i.e. the cold path (warmup for connection/TCP/buffers).
    let add_samples = std::mem::take(&mut *BENCH_SAMPLES.lock().unwrap());
    let sqrt_samples = std::mem::take(&mut *BENCH_SQRT_SAMPLES.lock().unwrap());
    print_bench("ADD", &add_samples, "add() — inline args");
    print_bench("SQRT", &sqrt_samples, "sqrt_batch(4096 f64) — arena MEM cap");

    if status == 0 {
        println!("WASM_SIDECAR PASS");
        std::process::exit(0);
    } else {
        println!("WASM_SIDECAR FAIL status={status}");
        std::process::exit(1);
    }
}
