//! End-to-end integration test for AeroIDL → Rust code generation.
//!
//! Generates Rust code from calculator.aeroidl, writes it to a temp Cargo
//! project that depends on a mock aerosls crate, and runs `cargo check`
//! to verify the generated code is valid Rust that type-checks.

use std::fs;
use std::path::PathBuf;
use std::process::Command;

/// The calculator IDL file lives at the project root.
fn calculator_idl_path() -> PathBuf {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // manifest_dir = tools/aeroidl, so go up two levels to project root
    manifest_dir.parent().unwrap().parent().unwrap().join("idl").join("calculator.aeroidl")
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

/// Generate Rust client code from calculator.aeroidl via the aeroidl-cc library.
fn generate_rust_code() -> String {
    let idl_path = calculator_idl_path();
    let src = fs::read_to_string(&idl_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", idl_path.display()));

    let doc = aeroidl_cc::parser::parse(&src)
        .unwrap_or_else(|errs| panic!("parse failed: {errs:?}"));

    let tc = aeroidl_cc::tycheck::type_check(&doc);
    assert!(tc.errors.is_empty(), "type-check errors: {:?}", tc.errors);

    let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let module = aeroidl_cc::gen_rust::emit_rust(&ast_value);

    format!("{common}\n\n{module}")
}

/// Generate Rust dispatch code from calculator.aeroidl via the aeroidl-cc library.
fn generate_dispatch_code() -> String {
    let idl_path = calculator_idl_path();
    let src = fs::read_to_string(&idl_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", idl_path.display()));

    let doc = aeroidl_cc::parser::parse(&src)
        .unwrap_or_else(|errs| panic!("parse failed: {errs:?}"));

    let tc = aeroidl_cc::tycheck::type_check(&doc);
    assert!(tc.errors.is_empty(), "type-check errors: {:?}", tc.errors);

    let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let dispatcher = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast_value);

    format!("{common}\n\n{dispatcher}")
}

/// Create a temporary Cargo project that depends on mock-aerosls and contains
/// the generated code as a library.
fn create_test_project(generated_code: &str, tmp_dir: &std::path::Path) {
    let src_dir = tmp_dir.join("src");
    fs::create_dir_all(&src_dir).unwrap();

    // cargo check doesn't need function implementations — the extern "C"
    // block in the generated code declares the symbols and cargo check only
    // verifies types, not linkage. So we don't need the mock at all.
    let cargo_toml = r#"[package]
name = "e2e-test"
version = "0.1.0"
edition = "2021"

[dependencies]
"#;
    fs::write(tmp_dir.join("Cargo.toml"), cargo_toml).unwrap();

    // lib.rs: just include the generated code directly
    let lib_rs = r#"extern crate alloc;
mod generated;
"#;
    fs::write(src_dir.join("lib.rs"), lib_rs).unwrap();

    // Write the generated code as a separate module
    fs::write(src_dir.join("generated.rs"), generated_code).unwrap();
}

/// Run `cargo check` in the given directory and return Ok(stdout+stderr) or
/// Err(message) with the combined output.
fn run_cargo_check(project_dir: &std::path::Path) -> Result<String, String> {
    let output = Command::new("cargo")
        .arg("check")
        .arg("--message-format=short")
        .current_dir(project_dir)
        .output()
        .map_err(|e| format!("failed to spawn cargo: {e}"))?;

    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
    let combined = format!("{stdout}\n{stderr}");

    if output.status.success() {
        Ok(combined)
    } else {
        Err(combined)
    }
}

// ── Tests ──────────────────────────────────────────────────────────────────

#[test]
fn e2e_calculator_rust_compiles() {
    // Step 1: Generate Rust code from calculator.aeroidl
    let generated_code = generate_rust_code();

    // Verify key structural elements are present before compiling
    assert!(generated_code.contains("pub enum CalcErrorKind"), "missing CalcErrorKind enum");
    assert!(generated_code.contains("pub struct Vec2"), "missing Vec2 struct");
    assert!(generated_code.contains("pub mod calculator_service"), "missing calculator_service module");
    assert!(generated_code.contains("pub fn add("), "missing add() function");
    assert!(generated_code.contains("pub fn div("), "missing div() function");
    assert!(generated_code.contains("pub fn dot("), "missing dot() function");
    assert!(generated_code.contains("pub fn sqrt_batch("), "missing sqrt_batch() function");
    assert!(generated_code.contains("pub fn heavy_reduce("), "missing heavy_reduce() function");

    // Step 2: Create a temp Cargo project
    let tmp_dir = TempDir::new("aeroidl_e2e");
    create_test_project(&generated_code, tmp_dir.path());

    // Step 3: Run cargo check
    let result = run_cargo_check(tmp_dir.path());

    match &result {
        Ok(output) => {
            println!("cargo check succeeded:\n{output}");
        }
        Err(output) => {
            panic!("cargo check FAILED:\n{output}");
        }
    }
}

#[test]
fn e2e_generated_code_has_correct_opcodes() {
    let code = generate_rust_code();

    // Verify opcodes match the IDL definition
    assert!(code.contains("pub const OP_ADD: u16 = 1"), "OP_ADD wrong");
    assert!(code.contains("pub const OP_DIV: u16 = 2"), "OP_DIV wrong");
    assert!(code.contains("pub const OP_DOT: u16 = 3"), "OP_DOT wrong");
    assert!(code.contains("pub const OP_MATMUL_VEC: u16 = 4"), "OP_MATMUL_VEC wrong");
    assert!(code.contains("pub const OP_SQRT_BATCH: u16 = 5"), "OP_SQRT_BATCH wrong");
    assert!(code.contains("pub const OP_HEAVY_REDUCE: u16 = 6"), "OP_HEAVY_REDUCE wrong");
}

#[test]
fn e2e_generated_code_has_channel_stubs() {
    let code = generate_rust_code();

    // Verify channel endpoint variables exist
    assert!(code.contains("pub static mut CHAN_W: u16"), "missing CHAN_W");
    assert!(code.contains("pub static mut CHAN_R: u16"), "missing CHAN_R");

    // Verify FFI calls are present
    assert!(code.contains("aerosls::chan_send("), "missing chan_send call");
    assert!(code.contains("aerosls::chan_recv("), "missing chan_recv call");
}

#[test]
fn e2e_generated_code_has_arena_support() {
    let code = generate_rust_code();

    // Verify arena-related code
    assert!(code.contains("ArenaSlice"), "missing ArenaSlice type");
    assert!(code.contains("CapDescriptor"), "missing CapDescriptor type");
    assert!(code.contains("CAP_PERM_R"), "missing CAP_PERM_R constant");
    assert!(code.contains("CHAN_FLAG_NO_REPLY"), "missing CHAN_FLAG_NO_REPLY");
}

#[test]
fn e2e_generated_code_serializes_struct_fields() {
    let code = generate_rust_code();

    // Verify that struct fields are serialized into the payload
    // dot(a: Vec2, b: Vec2) should serialize a.x, a.y, b.x, b.y
    assert!(code.contains("payload[0..0+8].copy_from_slice(&(a.x as u64)"), "dot: missing a.x serialization");
    assert!(code.contains("payload[8..8+8].copy_from_slice(&(a.y as u64)"), "dot: missing a.y serialization");
    assert!(code.contains("payload[16..16+8].copy_from_slice(&(b.x as u64)"), "dot: missing b.x serialization");
    assert!(code.contains("payload[24..24+8].copy_from_slice(&(b.y as u64)"), "dot: missing b.y serialization");
}

#[test]
fn e2e_async_method_uses_no_reply_flag() {
    let code = generate_rust_code();

    // Verify heavy_reduce uses NO_REPLY flag
    assert!(code.contains("CHAN_FLAG_NO_REPLY"), "async method should use NO_REPLY flag");

    // The async method should not call chan_recv (fire-and-forget)
    if let Some(start) = code.find("pub fn heavy_reduce(") {
        // Find the closing brace of the function
        let mut depth = 0i32;
        let mut found_body_start = false;
        for (i, ch) in code[start..].char_indices() {
            match ch {
                '{' => {
                    depth += 1;
                    found_body_start = true;
                }
                '}' => {
                    depth -= 1;
                    if found_body_start && depth == 0 {
                        let body = &code[start..start + i + 1];
                        assert!(!body.contains("aerosls::chan_recv"),
                                "async method should NOT call chan_recv");
                        assert!(body.contains("Ok(())"),
                                "async method should return Ok(())");
                        break;
                    }
                }
                _ => {}
            }
        }
    }
}

#[test]
fn e2e_result_deserialization_present() {
    let code = generate_rust_code();

    // Verify result deserialization patterns exist
    assert!(code.contains("let ok_byte = reply_buf[0]"), "missing ok byte check");
    assert!(code.contains("if ok_byte == 1"), "missing ok byte comparison");
    assert!(code.contains("AEROIDL_ERROR"), "missing error type usage");
}

#[test]
fn e2e_enum_discriminant_roundtrip() {
    let code = generate_rust_code();

    // Verify CalcErrorKind has from_raw and to_raw
    assert!(code.contains("pub fn from_raw(v: u32) -> Option<Self>"), "missing from_raw");
    assert!(code.contains("pub fn to_raw(self) -> u32"), "missing to_raw");
    assert!(code.contains("1 => Some(CalcErrorKind::DIVISION_BY_ZERO)"), "missing DIVISION_BY_ZERO mapping");
}

// ═══════════════════════════════════════════════════════════════════════════
// Dispatch-side (server) e2e tests
// ═══════════════════════════════════════════════════════════════════════════

#[test]
fn e2e_dispatch_rust_compiles() {
    // Step 1: Generate dispatch code from calculator.aeroidl
    let generated_code = generate_dispatch_code();

    // Verify key structural elements are present before compiling
    assert!(generated_code.contains("pub trait CalculatorServiceImpl"),
            "missing CalculatorServiceImpl trait");
    assert!(generated_code.contains("fn add(&mut self, a: i32, b: i32) -> AeroidlResult<i32>"),
            "missing add() in trait");
    assert!(generated_code.contains("fn div(&mut self, a: i64, b: i64) -> AeroidlResult<i64>"),
            "missing div() in trait");
    assert!(generated_code.contains("fn dot(&mut self, a: Vec2, b: Vec2) -> AeroidlResult<f64>"),
            "missing dot() in trait");
    assert!(generated_code.contains("fn sqrt_batch(&mut self, input_cap: u16, input_data: *const u8, input_len: u32) -> AeroidlResult<ArenaSlice<f64>>"),
            "missing sqrt_batch() in trait");
    assert!(generated_code.contains("pub fn dispatch<S: CalculatorServiceImpl>"),
            "missing dispatch() function");

    // Step 2: Create a temp Cargo project
    let tmp_dir = TempDir::new("aeroidl_dispatch_e2e");
    create_test_project(&generated_code, tmp_dir.path());

    // Step 3: Run cargo check
    let result = run_cargo_check(tmp_dir.path());

    match &result {
        Ok(output) => {
            println!("dispatch cargo check succeeded:\n{output}");
        }
        Err(output) => {
            panic!("dispatch cargo check FAILED:\n{output}");
        }
    }
}

#[test]
fn e2e_dispatch_has_correct_opcodes() {
    let code = generate_dispatch_code();

    // Verify opcodes match the IDL definition
    assert!(code.contains("OP_ADD: u16 = 1"), "OP_ADD wrong");
    assert!(code.contains("OP_DIV: u16 = 2"), "OP_DIV wrong");
    assert!(code.contains("OP_DOT: u16 = 3"), "OP_DOT wrong");
    assert!(code.contains("OP_MATMUL_VEC: u16 = 4"), "OP_MATMUL_VEC wrong");
    assert!(code.contains("OP_SQRT_BATCH: u16 = 5"), "OP_SQRT_BATCH wrong");
    assert!(code.contains("OP_HEAVY_REDUCE: u16 = 6"), "OP_HEAVY_REDUCE wrong");
}

#[test]
fn e2e_dispatch_has_trait_with_all_methods() {
    let code = generate_dispatch_code();

    // Verify trait has all 6 methods with correct signatures
    assert!(code.contains("pub trait CalculatorServiceImpl"));
    assert!(code.contains("fn add(&mut self, a: i32, b: i32) -> AeroidlResult<i32>"));
    assert!(code.contains("fn div(&mut self, a: i64, b: i64) -> AeroidlResult<i64>"));
    assert!(code.contains("fn dot(&mut self, a: Vec2, b: Vec2) -> AeroidlResult<f64>"));
    assert!(code.contains("fn matmul_vec(&mut self, m: MatrixData, v: Vec2) -> AeroidlResult<Vec2>"));
    assert!(code.contains("fn sqrt_batch(&mut self, input_cap: u16, input_data: *const u8, input_len: u32) -> AeroidlResult<ArenaSlice<f64>>"));
    // heavy_reduce is async, so return type is bare u32, not AeroidlResult
    assert!(code.contains("fn heavy_reduce(&mut self, input_cap: u16, input_data: *const u8, input_len: u32) -> u32"));
}

#[test]
fn e2e_dispatch_has_opcode_match() {
    let code = generate_dispatch_code();

    // Verify dispatch uses a match statement on opcodes
    assert!(code.contains("match opcode {"), "missing opcode match");
    assert!(code.contains("OP_ADD =>"), "missing OP_ADD arm");
    assert!(code.contains("OP_DIV =>"), "missing OP_DIV arm");
    assert!(code.contains("OP_DOT =>"), "missing OP_DOT arm");
    assert!(code.contains("OP_MATMUL_VEC =>"), "missing OP_MATMUL_VEC arm");
    assert!(code.contains("OP_SQRT_BATCH =>"), "missing OP_SQRT_BATCH arm");
    assert!(code.contains("OP_HEAVY_REDUCE =>"), "missing OP_HEAVY_REDUCE arm");
    // Unknown opcode fallback
    assert!(code.contains("_ =>"), "missing default arm");
}

#[test]
fn e2e_dispatch_has_channel_recv_send() {
    let code = generate_dispatch_code();

    // Verify the dispatch function calls chan_recv and chan_send
    assert!(code.contains("chan_recv(") || code.contains("chan_recv (") || code.contains("fn chan_recv"),
            "missing chan_recv declaration or call");
    assert!(code.contains("chan_send(") || code.contains("chan_send (") || code.contains("fn chan_send"),
            "missing chan_send declaration or call");
}

#[test]
fn e2e_dispatch_deserializes_struct_fields() {
    let code = generate_dispatch_code();

    // Verify struct deserialization in the dispatch arms
    // dot() deserializes two Vec2 structs from payload
    assert!(code.contains("Vec2 {"), "missing Vec2 deserialization");
    assert!(code.contains("x: f64::from_bits("), "missing Vec2.x f64 deserialization");
    assert!(code.contains("y: f64::from_bits("), "missing Vec2.y f64 deserialization");
}

#[test]
fn e2e_dispatch_handles_arena_params() {
    let code = generate_dispatch_code();

    // Verify arena parameter handling: cap handle, data pointer, length
    assert!(code.contains("recv_caps["), "missing cap slot access");
    assert!(code.contains("arena_mem("), "missing arena_mem() call for arena params");
    // sqrt_batch takes an arena parameter
    assert!(code.contains("input_cap = recv_caps[0]"),
            "missing input_cap from recv_caps");
}

#[test]
fn e2e_dispatch_serializes_result_match() {
    let code = generate_dispatch_code();

    // Verify the dispatch arms use match result { Ok(val) => ..., Err(e) => ... }
    assert!(code.contains("match result {"), "missing match result");
    assert!(code.contains("Ok(val) => {"), "missing Ok arm");
    assert!(code.contains("Err(e) => {"), "missing Err arm");
    // Reply buffer write pattern
    assert!(code.contains("reply_buf[0] = 1;"), "missing ok byte write");
    assert!(code.contains("reply_buf[0] = 0;"), "missing error byte write");
}

#[test]
fn e2e_dispatch_async_sends_ack() {
    let code = generate_dispatch_code();

    // Verify async method (heavy_reduce) sends an immediate ack
    if let Some(start) = code.find("OP_HEAVY_REDUCE =>") {
        let end = code[start..].find("\n                _ =>").unwrap_or(500);
        let arm = &code[start..start + end];
        // Async arm should NOT use match result
        assert!(!arm.contains("match result"),
                "async arm should not use match result");
        // Async arm should send ack bytes
        assert!(arm.contains("reply_buf[0..4].copy_from_slice"),
                "async arm should write ack payload");
    }
}

#[test]
fn e2e_dispatch_balanced_braces() {
    let code = generate_dispatch_code();

    let opens = code.matches('{').count();
    let closes = code.matches('}').count();
    assert_eq!(opens, closes,
            "unbalanced braces: {opens} opens vs {closes} closes");
}
