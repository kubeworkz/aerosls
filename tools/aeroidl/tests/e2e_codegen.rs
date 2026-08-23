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

    // Verify async method (heavy_reduce) sends an immediate ack.
    // Slice to the NEXT opcode arm (not the `_ =>` catch-all): the dispatch
    // match gained a later arm (OP_REVERSE) whose result-matching pattern
    // would otherwise leak into this slice.
    if let Some(start) = code.find("OP_HEAVY_REDUCE =>") {
        let end = code[start..].find("\n                OP_").unwrap_or(500);
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

// ─── C header generation + interop tests ─────────────────────────────────

fn generate_c_header() -> String {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).unwrap();
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    aeroidl_cc::gen_c::emit_c(&ast)
}

#[test]
fn e2e_c_header_has_guard() {
    let c = generate_c_header();
    assert!(c.starts_with("/**"));
    assert!(c.contains("#ifndef AEROSLS_CALCULATOR_H"));
    assert!(c.contains("#define AEROSLS_CALCULATOR_H"));
    assert!(c.trim_end().ends_with("*/"));
}

#[test]
fn e2e_c_header_has_includes() {
    let c = generate_c_header();
    assert!(c.contains("#include <stdint.h>"));
    assert!(c.contains("#include <stddef.h>"));
    assert!(c.contains("#include <aerosls_cap.h>"));
}

#[test]
fn e2e_c_header_has_enums() {
    let c = generate_c_header();
    assert!(c.contains("typedef enum {"));
    assert!(c.contains("CALC_ERROR_DIVISION_BY_ZERO = 1"));
    assert!(c.contains("CALC_ERROR_OVERFLOW = 2"));
    assert!(c.contains("} CalcErrorKind;"));
}

#[test]
fn e2e_c_header_has_structs() {
    let c = generate_c_header();
    assert!(c.contains("typedef struct {"));
    assert!(c.contains("} CalcError;"));
    assert!(c.contains("} Vec2;"));
    assert!(c.contains("} MatrixData;"));
    assert!(c.contains("double                   x;"));
    assert!(c.contains("double                   y;"));
    assert!(c.contains("uint32_t                 rows;"));
}

#[test]
fn e2e_c_header_has_result_wrapper() {
    let c = generate_c_header();
    assert!(c.contains("} CalcResult;"));
    assert!(c.contains("uint8_t ok;"));
    assert!(c.contains("int32_t  val_i32;"));
    assert!(c.contains("int64_t  val_i64;"));
    assert!(c.contains("double   val_f64;"));
    assert!(c.contains("Vec2 val_vec2;"));
    assert!(c.contains("CalcError err;"));
}

#[test]
fn e2e_c_header_has_opcodes() {
    let c = generate_c_header();
    assert!(c.contains("#define CALC_OP_ADD                      0x0001"));
    assert!(c.contains("#define CALC_OP_DIV                      0x0002"));
    assert!(c.contains("#define CALC_OP_DOT                      0x0003"));
    assert!(c.contains("#define CALC_OP_MATMUL_VEC               0x0004"));
    assert!(c.contains("#define CALC_OP_SQRT_BATCH               0x0005"));
    assert!(c.contains("#define CALC_OP_HEAVY_REDUCE             0x0006"));
}

#[test]
fn e2e_c_header_has_method_stubs() {
    let c = generate_c_header();
    assert!(c.contains("calculator_add("));
    assert!(c.contains("calculator_div("));
    assert!(c.contains("calculator_dot("));
    assert!(c.contains("calculator_matmul_vec("));
    assert!(c.contains("calculator_sqrt_batch("));
    assert!(c.contains("calculator_heavy_reduce("));
    assert!(c.contains("static inline CalcResult"));
}

#[test]
fn e2e_c_header_has_arena_helpers() {
    let c = generate_c_header();
    assert!(c.contains("calculator_alloc_arena_buf("));
    assert!(c.contains("calculator_free_arena_buf("));
    assert!(c.contains("aerosls_arena_alloc("));
    assert!(c.contains("aerosls_arena_free("));
}

#[test]
fn e2e_c_header_balanced_braces() {
    let c = generate_c_header();
    let opens = c.matches('{').count();
    let closes = c.matches('}').count();
    assert_eq!(opens, closes,
            "unbalanced braces: {opens} opens vs {closes} closes");
}

#[test]
fn e2e_c_header_balanced_parens() {
    let c = generate_c_header();
    let opens = c.matches('(').count();
    let closes = c.matches(')').count();
    assert_eq!(opens, closes,
            "unbalanced parens: {opens} opens vs {closes} closes");
}

#[test]
fn e2e_c_header_balanced_ifdef() {
    let c = generate_c_header();
    let ifndef_count = c.matches("#ifndef").count();
    let endif_count = c.matches("#endif").count();
    assert_eq!(ifndef_count, 1, "expected 1 #ifndef, got {ifndef_count}");
    assert_eq!(endif_count, 3, "expected 3 #endif (guard + 2×__cplusplus)");
    assert!(c.contains("#ifdef __cplusplus"));
    assert!(c.contains("extern \"C\" {"));
}

/// Write the generated C header + mock runtime header to a temp dir.
fn setup_c_test_dir() -> (TempDir, PathBuf) {
    let tmp = TempDir::new("aeroidl_c_interop");
    let c_code = generate_c_header();
    let dir = tmp.path().to_path_buf();
    fs::write(dir.join("calculator.h"), &c_code).unwrap();
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let mock_cap = manifest_dir.join("tests").join("mock_aerosls_cap.h");
    fs::copy(&mock_cap, dir.join("aerosls_cap.h")).unwrap();
    let test_c = manifest_dir.join("tests").join("test_calculator.c");
    fs::copy(&test_c, dir.join("test_calculator.c")).unwrap();
    (tmp, dir)
}

fn find_cc() -> Option<String> {
    for cc in &["cc", "gcc", "clang", "tcc"] {
        if Command::new("which")
            .arg(cc)
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
        {
            return Some(cc.to_string());
        }
    }
    None
}

#[test]
fn e2e_c_header_compiles_with_cc() {
    let cc = match find_cc() {
        Some(cc) => cc,
        None => {
            eprintln!("no C compiler found — skipping compilation test");
            return;
        }
    };
    let (_tmp, dir) = setup_c_test_dir();
    let output = Command::new(&cc)
        .arg("-Wall")
        .arg("-Wextra")
        .arg("-Werror")
        .arg("-std=c11")
        .arg("-I").arg(&dir)
        .arg(dir.join("test_calculator.c"))
        .arg("-o").arg(dir.join("test_calculator"))
        .arg("-lm")
        .output()
        .expect("failed to invoke C compiler");
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        panic!("C compilation failed with {cc}:\n{stderr}");
    }
    eprintln!("C compilation succeeded with {cc}");
    let run = Command::new(dir.join("test_calculator"))
        .output()
        .expect("failed to run test_calculator");
    let stdout = String::from_utf8_lossy(&run.stdout);
    assert!(run.status.success(), "test_calculator failed: {stdout}");
    assert!(stdout.contains("All tests passed."),
            "expected 'All tests passed.' in output: {stdout}");
    eprintln!("C test output:\n{stdout}");
}

#[test]
fn e2e_c_header_matches_existing_pattern() {
    let c = generate_c_header();
    assert!(c.contains("CALC_OP_ADD"));
    assert!(c.contains("CALC_OP_DIV"));
    assert!(c.contains("calculator_add("));
    assert!(c.contains("calculator_div("));
    assert!(c.contains("calculator_dot("));
    assert!(c.contains("CalcResult"));
    assert!(c.contains("#include <aerosls_cap.h>"));
    assert!(c.contains("extern \"C\" {
#endif"));
}

// ─── --target all: cross-backend consistency tests ────────────────────────

/// Generate all four backends from a single parse + type-check + AST pass,
/// exactly as `--target all` does. Returns (client, dispatcher, lisp, c).
fn generate_all_backends() -> (String, String, String, String) {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).unwrap();
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let client = format!("{common}\n\n{}", aeroidl_cc::gen_rust::emit_rust(&ast));
    let dispatcher = format!("{common}\n\n{}", aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast));
    let lisp = aeroidl_cc::gen_lisp::emit_lisp(&ast);
    let c = aeroidl_cc::gen_c::emit_c(&ast);
    (client, dispatcher, lisp, c)
}

/// Canonical (method_name, opcode) pairs straight from the typed AST.
fn ast_method_opcodes() -> Vec<(String, u64)> {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).unwrap();
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let mut out = Vec::new();
    if let Some(interfaces) = ast["interfaces"].as_array() {
        for iface in interfaces {
            if let Some(methods) = iface["methods"].as_array() {
                for m in methods {
                    let name = m["name"].as_str().unwrap().to_string();
                    let opcode = m["opcode"].as_u64().unwrap();
                    out.push((name, opcode));
                }
            }
        }
    }
    out
}

/// Extract (METHOD_NAME, opcode) from Rust opcode constants:
/// `pub const OP_ADD: u16 = 1;`
fn rust_opcodes(text: &str) -> Vec<(String, u64)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("pub const OP_") {
            if let Some((name, val)) = rest.split_once(':') {
                if let Some(v) = val.trim().strip_prefix("u16 = ") {
                    let v = v.trim_end_matches(';').trim();
                    if let Ok(n) = v.parse::<u64>() {
                        out.push((name.to_string(), n));
                    }
                }
            }
        }
    }
    out
}

/// Extract (method-name, opcode) from Lisp constants:
/// `(defconstant +op-add+ #x0001)`
fn lisp_opcodes(text: &str) -> Vec<(String, u64)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("(defconstant +op-") {
            if let Some((name, val)) = rest.split_once("+") {
                let hex = val.trim().trim_start_matches("#x").trim_end_matches(')');
                if let Ok(n) = u64::from_str_radix(hex.trim(), 16) {
                    out.push((name.replace('-', "_"), n));
                }
            }
        }
    }
    out
}

/// Extract (METHOD_NAME, opcode) from C defines:
/// `#define CALC_OP_ADD  0x0001`
fn c_opcodes(text: &str) -> Vec<(String, u64)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("#define ") {
            if let Some((left, right)) = rest.split_once(' ') {
                if let Some(name) = left.split("_OP_").nth(1) {
                    let hex = right.trim().trim_start_matches("0x");
                    if let Ok(n) = u64::from_str_radix(hex.trim(), 16) {
                        out.push((name.to_string(), n));
                    }
                }
            }
        }
    }
    out
}

#[test]
fn e2e_all_backends_same_opcodes() {
    let (client, dispatcher, lisp, c) = generate_all_backends();

    let canonical = ast_method_opcodes();
    assert_eq!(canonical.len(), 7,
        "expected 7 methods in calculator.aeroidl, got {}", canonical.len());

    // Rust/C emit UPPERCASE names (OP_ADD), Lisp emits lowercase (op-add).
    // Normalize everything to lowercase snake for comparison.
    let canon: Vec<(String, u64)> = canonical.iter()
        .map(|(n, o)| (n.to_lowercase(), *o))
        .collect();
    let norm = |v: Vec<(String, u64)>| -> Vec<(String, u64)> {
        v.into_iter().map(|(n, o)| (n.to_lowercase(), o)).collect()
    };

    // Rust client and dispatcher use the same const block.
    assert_eq!(norm(rust_opcodes(&client)), canon,
        "Rust client opcodes differ from AST");
    assert_eq!(norm(rust_opcodes(&dispatcher)), canon,
        "Rust dispatcher opcodes differ from AST");
    assert_eq!(norm(lisp_opcodes(&lisp)), canon,
        "Lisp opcodes differ from AST");
    assert_eq!(norm(c_opcodes(&c)), canon,
        "C opcodes differ from AST");
}

#[test]
fn e2e_all_backends_same_method_names() {
    let (client, dispatcher, lisp, c) = generate_all_backends();

    // Every method must appear in every backend, with the backend's own
    // naming convention: rust `add`, lisp `calculator-service-add`,
    // C `calculator_add`.
    let expected: Vec<(String, String, String, String)> = [
        ("add",        "calculator-service-add", "calculator_add"),
        ("div",        "calculator-service-div", "calculator_div"),
        ("dot",        "calculator-service-dot", "calculator_dot"),
        ("matmul_vec", "calculator-service-matmul-vec", "calculator_matmul_vec"),
        ("sqrt_batch", "calculator-service-sqrt-batch", "calculator_sqrt_batch"),
        ("heavy_reduce", "calculator-service-heavy-reduce", "calculator_heavy_reduce"),
    ]
    .iter()
    .map(|(r, l, c_)| (r.to_string(), l.to_string(), c_.to_string(), r.to_string()))
    .collect();

    for (rust_name, lisp_name, c_name, _) in &expected {
        // Rust client: `pub fn add(` ; dispatcher trait: `fn add(&mut self`
        assert!(client.contains(&format!("pub fn {rust_name}(")),
            "client missing method {rust_name}");
        assert!(dispatcher.contains(&format!("fn {rust_name}(&mut self")),
            "dispatcher missing trait method {rust_name}");
        // Lisp: `(defun calculator-service-add (`
        assert!(lisp.contains(&format!("(defun {lisp_name} (")),
            "lisp missing method {lisp_name}");
        // C: `calculator_add(`
        assert!(c.contains(&format!("{c_name}(")),
            "C header missing method {c_name}");
    }
}

/// Canonical (field_name, ownership) pairs for non-inline struct fields.
fn ast_struct_field_ownership() -> Vec<(String, String)> {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).unwrap();
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let mut out = Vec::new();
    if let Some(structs) = ast["structs"].as_array() {
        for s in structs {
            if let Some(fields) = s["fields"].as_array() {
                for f in fields {
                    let own = f["ownership"].as_str().unwrap_or("inline");
                    if own != "inline" {
                        out.push((f["name"].as_str().unwrap().to_string(), own.to_string()));
                    }
                }
            }
        }
    }
    out
}

/// Canonical (method, param, ownership) pairs for params with ownership.
fn ast_param_ownership() -> Vec<(String, String, String)> {
    let src = fs::read_to_string(calculator_idl_path()).unwrap();
    let doc = aeroidl_cc::parser::parse(&src).unwrap();
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let mut out = Vec::new();
    if let Some(interfaces) = ast["interfaces"].as_array() {
        for iface in interfaces {
            if let Some(methods) = iface["methods"].as_array() {
                for m in methods {
                    let mname = m["name"].as_str().unwrap().to_string();
                    if let Some(params) = m["params"].as_array() {
                        for p in params {
                            let own = p["ownership"].as_str().unwrap_or("inline");
                            if own != "inline" {
                                out.push((mname.clone(),
                                          p["name"].as_str().unwrap().to_string(),
                                          own.to_string()));
                            }
                        }
                    }
                }
            }
        }
    }
    out
}

#[test]
fn e2e_all_backends_same_ownership() {
    let (client, dispatcher, lisp, c) = generate_all_backends();

    // ── Struct field ownership must match across backends ──────────────
    let fields = ast_struct_field_ownership();
    assert!(fields.iter().any(|(_, own)| own == "arena"),
        "test fixture: expected an @arena struct field");
    assert!(fields.iter().any(|(_, own)| own == "borrowed"),
        "test fixture: expected an @borrowed struct field");

    for (fname, own) in &fields {
        // Rust client: `pub message: ..., /* @borrowed */`
        assert!(client.lines().any(|l| l.contains(fname) && l.contains(&format!("@{own}"))),
            "client field {fname} missing @{own}");
        // Rust dispatcher: `pub message: ..., // @borrowed`
        assert!(dispatcher.lines().any(|l| l.contains(fname) && l.contains(&format!("@{own}"))),
            "dispatcher field {fname} missing @{own}");
        // C header: `uint16_t /* cap handle */ message; /* @borrowed */`
        assert!(c.lines().any(|l| l.contains(fname) && l.contains(&format!("@{own}"))),
            "C header field {fname} missing @{own}");
    }

    // ── Param ownership must match in Lisp docstrings ──────────────────
    let params = ast_param_ownership();
    assert!(!params.is_empty(), "test fixture: expected owned params");
    for (mname, pname, own) in &params {
        // Lisp docstring: `dot(a: vec2@borrowed, b: vec2@borrowed)`
        assert!(lisp.lines().any(|l| l.contains(mname) && l.contains(&format!("@{own}"))),
            "lisp method {mname} param {pname} missing @{own}");
    }
}

#[test]
fn e2e_all_backends_dispatch_table_present() {
    let (client, dispatcher, lisp, c) = generate_all_backends();

    // Client must send every opcode.
    for (mname, opcode) in ast_method_opcodes() {
        let const_name = format!("OP_{}", mname.to_uppercase());
        assert!(client.contains(&const_name),
            "client missing opcode const {const_name}");
        assert!(dispatcher.contains(&const_name),
            "dispatcher missing opcode const {const_name}");
        assert!(c.contains(&const_name),
            "C header missing opcode const {const_name}");
        let _ = opcode;
    }

    // Dispatcher must match on every opcode; Lisp dispatch must ecase them.
    assert!(dispatcher.contains("match opcode"), "dispatcher missing opcode match");
    assert!(lisp.contains("(ecase opcode"), "lisp dispatch missing ecase");
}

#[test]
fn e2e_all_backends_struct_layout_consistent() {
    // Vec2 appears in all four backends with the same field set.
    let (client, dispatcher, lisp, c) = generate_all_backends();
    for text in [&client, &dispatcher, &lisp, &c] {
        assert!(text.contains("x"), "missing field x");
        assert!(text.contains("y"), "missing field y");
    }
    // MatrixData: rows, cols, elements in all four
    for text in [&client, &dispatcher, &lisp, &c] {
        assert!(text.contains("rows"), "missing field rows");
        assert!(text.contains("cols"), "missing field cols");
        assert!(text.contains("elements"), "missing field elements");
    }
}
