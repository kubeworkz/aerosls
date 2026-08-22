//! End-to-end: generated stubs run against the REAL aerosls channel runtime.
//!
//! This is the Rust side of the milestone that replaced the compile-only
//! mock (`tests/mock_aerosls`) with the real transport
//! (`user/aerosls`): the calculator client AND dispatcher are generated
//! from calculator.aeroidl, linked into one binary against the actual
//! `aerosls` crate (`--features host-fake`, which exposes the fake-syscall
//! hook), and driven as two threads over an in-process fake kernel.
//!
//! The round trip exercises the full Polyglot Nexus call path:
//!   client add(5,3) → chan_send (frame: [opcode u16][len u32][a][b])
//!   → fake kernel queue → dispatcher chan_recv (parses the same frame)
//!   → service.add() → chan_send reply (opcode 0, raw [ok][val])
//!   → fake kernel queue → client chan_recv → deserialize Ok(8)
//!
//! A failure here means the generator and the runtime disagree on framing,
//! struct layout, or the FFI symbol ABI — the things the mock could never
//! catch because it was never linked.

use std::fs;
use std::path::PathBuf;
use std::process::Command;

/// The calculator IDL file lives at the project root.
fn calculator_idl_path() -> PathBuf {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest_dir
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("idl")
        .join("calculator.aeroidl")
}

/// The real runtime crate lives at user/aerosls in the project root.
fn aerosls_crate_path() -> PathBuf {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest_dir
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("user")
        .join("aerosls")
}

/// A unique temporary directory for this test run. Cleaned up on drop.
struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(prefix: &str) -> Self {
        let base = std::env::temp_dir();
        let id = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = base.join(format!("{prefix}_{id}"));
        fs::create_dir_all(&path).unwrap();
        TempDir { path }
    }

    fn path(&self) -> &std::path::Path {
        &self.path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// Generate the calculator client module from the IDL.
fn generate_client_code() -> String {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).expect("parse");
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    assert!(tc.errors.is_empty(), "{:?}", tc.errors);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let module = aeroidl_cc::gen_rust::emit_rust(&ast);
    format!("{common}\n\n{module}")
}

/// Generate the calculator dispatcher module from the IDL.
fn generate_dispatch_code() -> String {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).expect("parse");
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    assert!(tc.errors.is_empty(), "{:?}", tc.errors);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let module = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast);
    format!("{common}\n\n{module}")
}

/// The driver: fake kernel + client thread + dispatcher thread.
///
/// The fake models one channel as two numbered directions (1 = request
/// path, 2 = reply path): the client sends on CHAN_W=1 and receives on
/// CHAN_R=2; the dispatcher listens on chan_r=1 and replies on chan_w=2.
const DRIVER: &str = r#"extern crate alloc;

mod calc_client;
mod calc_dispatch;

use std::collections::VecDeque;
use std::sync::Mutex;

// Fake kernel: one FIFO per direction (channel numbers 1 and 2).
static Q1: Mutex<VecDeque<Vec<u8>>> = Mutex::new(VecDeque::new());
static Q2: Mutex<VecDeque<Vec<u8>>> = Mutex::new(VecDeque::new());

fn fake_syscall(num: u64, arg: u64) -> u64 {
    match num {
        302 => {
            // SYS_SLS_CAP_SEND_MSG: stage the payload into the right queue.
            let req = unsafe { &*(arg as *const aerosls::req::SendMsgReq) };
            let payload = unsafe {
                std::slice::from_raw_parts(req.payload, req.payload_len as usize)
            }
            .to_vec();
            match req.ch_w_idx {
                1 => Q1.lock().unwrap().push_back(payload),
                2 => Q2.lock().unwrap().push_back(payload),
                _ => {}
            }
            0
        }
        303 => {
            // SYS_SLS_CAP_RECV_MSG. The generated client's chan_recv is a
            // single-shot non-blocking poll, so this fake stands in for the
            // kernel-side blocking recv: it waits (with a bounded spin) for
            // a message instead of returning EAGAIN on an empty queue.
            let req = unsafe { &mut *(arg as *mut aerosls::req::RecvMsgReq) };
            let mut waited = 0u32;
            let payload = loop {
                let popped = match req.ch_r_idx {
                    1 => Q1.lock().unwrap().pop_front(),
                    2 => Q2.lock().unwrap().pop_front(),
                    _ => None,
                };
                if let Some(p) = popped {
                    break p;
                }
                waited += 1;
                if waited > 200_000_000 {
                    return (-3i64) as u64; // CAP_EAGAIN — fail the call, don't hang
                }
                std::thread::yield_now();
            };
            let n = payload.len().min(req.buf_len as usize);
            if n > 0 {
                unsafe { std::ptr::copy_nonoverlapping(payload.as_ptr(), req.buf, n); }
            }
            req.out_payload_len = payload.len() as u32;
            req.out_tag = 0;
            req.out_flags = 0;
            req.out_n_caps = 0;
            0
        }
        _ => 0,
    }
}

// The common types are duplicated at each generated file's root; the
// dispatcher's trait references its own copies, so the impl must too.
use calc_dispatch::{AEROIDL_ERROR, ArenaSlice, MatrixData, Vec2};

struct CalcSvc;

impl calc_dispatch::calculator_service::CalculatorServiceImpl for CalcSvc {
    fn add(&mut self, a: i32, b: i32) -> Result<i32, AEROIDL_ERROR> {
        Ok(a + b)
    }
    fn div(&mut self, a: i64, b: i64) -> Result<i64, AEROIDL_ERROR> {
        if b == 0 {
            Err(AEROIDL_ERROR { code: 1, message: alloc::string::String::from("div by zero") })
        } else {
            Ok(a / b)
        }
    }
    fn dot(&mut self, a: Vec2, b: Vec2) -> Result<f64, AEROIDL_ERROR> {
        Ok(a.x * b.x + a.y * b.y)
    }
    fn matmul_vec(&mut self, m: MatrixData, v: Vec2) -> Result<Vec2, AEROIDL_ERROR> {
        let _ = m;
        Ok(v)
    }
    fn sqrt_batch(
        &mut self,
        input_cap: u16,
        input_data: *const u8,
        input_len: u32,
    ) -> Result<ArenaSlice<f64>, AEROIDL_ERROR> {
        let _ = (input_cap, input_data, input_len);
        Ok(ArenaSlice::new(0, 0))
    }
    fn heavy_reduce(&mut self, input_cap: u16, input_data: *const u8, input_len: u32) -> u32 {
        let _ = (input_cap, input_data, input_len);
        0
    }
}

fn main() {
    aerosls::set_fake_syscall(Some(fake_syscall));

    // Dispatcher thread: server side. Each dispatch() call handles one
    // message and returns Err(CAP_EAGAIN) when the queue is empty.
    let dispatcher = std::thread::spawn(|| {
        let mut svc = CalcSvc;
        loop {
            match calc_dispatch::calculator_service::dispatch(&mut svc, 1, 2) {
                Ok(()) => {}
                Err(e) if e.code == 4294967293 => { /* CAP_EAGAIN (-3 as u32) */ }
                Err(_) => return,
            }
            std::thread::yield_now();
        }
    });

    // Client endpoint wiring.
    unsafe {
        calc_client::calculator_service::CHAN_W = 1;
        calc_client::calculator_service::CHAN_R = 2;
    }

    // The actual cross-sidecar call.
    let result = calc_client::calculator_service::add(5, 3);
    match result {
        Ok(v) => println!("ADD_RESULT={}", v),
        Err(e) => println!("ADD_ERROR={}", e.code),
    }
    let _ = dispatcher;
}
"#;

fn write_project(dir: &std::path::Path, client: &str, dispatch: &str) {
    let src = dir.join("src");
    fs::create_dir_all(&src).unwrap();

    let aerosls = aerosls_crate_path();
    let aerosls = aerosls.to_string_lossy().replace('\\', "/");
    let cargo_toml = format!(
        r#"[package]
name = "e2e-real-transport"
version = "0.1.0"
edition = "2021"

[dependencies]
aerosls = {{ path = "{aerosls}", features = ["host-fake"] }}

[workspace]
"#
    );
    fs::write(dir.join("Cargo.toml"), cargo_toml).unwrap();
    fs::write(src.join("main.rs"), DRIVER).unwrap();
    fs::write(src.join("calc_client.rs"), client).unwrap();
    fs::write(src.join("calc_dispatch.rs"), dispatch).unwrap();
}

#[test]
fn generated_stubs_round_trip_through_the_real_runtime() {
    let client = generate_client_code();
    let dispatch = generate_dispatch_code();
    let tmp = TempDir::new("aeroidl_real_transport");
    write_project(tmp.path(), &client, &dispatch);

    let output = Command::new("cargo")
        .arg("run")
        .arg("--quiet")
        .current_dir(tmp.path())
        .output()
        .expect("failed to spawn cargo");

    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();

    assert!(
        output.status.success(),
        "cargo run failed:\n{stdout}\n{stderr}"
    );
    assert!(
        stdout.contains("ADD_RESULT=8"),
        "expected ADD_RESULT=8, got:\n{stdout}\n{stderr}"
    );
}
