//! Cross-language constant consistency test.
//!
//! Every Ring-3 runtime, mock, and generated backend that emits channel-flag
//! or capability-permission constants must agree with the kernel's source of
//! truth (`kernel/cap.h`). This test greps every definition across:
//!
//! ```text
//! kernel/cap.h                        — source of truth (#defines + bit0 comment)
//! user/libaerocap/aerocap.h           — C SDK base (CAP_* names)
//! user/libaerocap/aerosls_cap.h       — C SDK (AEROSLS_* names)
//! user/aerosls/src/req.rs             — Rust runtime
//! tools/aeroidl/tests/mock_aerosls/src/lib.rs  — Rust mock (compile-only)
//! tools/aeroidl/tests/mock_aerosls_cap.h       — C mock (compile-only)
//! tools/aeroidl/src/main.rs           — embedded C mock for `--check` mode
//! generated Rust client + dispatcher  — emit_common_types() output
//! generated C header                  — must resolve through the AEROSLS_* SDK macros
//! ```
//!
//! Any mismatch fails the build, so flag drift like the `CHAN_FLAG_NO_REPLY`
//! 0x0002 → 0x0001 fix (kernel bit0 = async) is caught at test time instead
//! of surfacing as a silent cross-language wire mismatch.
//!
//! The second half pins WIRE STRUCT LAYOUTS: the capability descriptor
//! (`SLSCapDesc`) and the three message-header requests (send/recv/arena
//! free) are parsed from every definition and their C-layout offsets/sizes
//! are compared against the kernel's. The kernel layouts themselves are
//! anchored against the hardcoded ABI table below, so a parser regression
//! cannot make the test silently agree on a wrong layout.
//!
//! Deliberately NOT checked: `docs/AeroSLS-Sidecar-Channels-Transport-Spec-v0.1.md`
//! and `user/proto` — that transport envelope is a different protocol whose
//! `F_NO_REPLY = 0x0002` is unrelated to the kernel message flag.

use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;

// ── Source of truth (kernel/cap.h) ─────────────────────────────────────────

/// (constant name, expected value). `CAP_PERM_RECV` is kernel-internal (same
/// numeric value as `CAP_PERM_W`, but for CHAN_R caps) and has no user-level
/// twin; `CAP_PERM_SHIFT`/`CAP_PERM_MASK` are kernel-internal encodings.
/// `CHAN_FLAG_NO_REPLY` is the user-level name for kernel `ChanMsg.flags`
/// bit0 (async), documented in kernel/cap.h as "bit0 = NO_REPLY".
const TRUTH: &[(&str, u32)] = &[
    ("CAP_PERM_R", 0x01),
    ("CAP_PERM_W", 0x02),
    ("CAP_PERM_X", 0x04),
    ("CAP_PERM_MAP", 0x08),
    ("CAP_NONE", 0xFFFF),
    ("CHAN_FLAG_NO_REPLY", 0x0001),
];

/// Look up the expected value for a constant, accepting the `AEROSLS_` prefix
/// the C SDK uses (`AEROSLS_CAP_PERM_R` → `CAP_PERM_R`).
fn truth_value(name: &str) -> Option<u32> {
    let bare = name.strip_prefix("AEROSLS_").unwrap_or(name);
    TRUTH.iter().find(|(n, _)| *n == bare).map(|(_, v)| *v)
}

// ── File loading ───────────────────────────────────────────────────────────

/// Repo root = two parents above `tools/aeroidl` (CARGO_MANIFEST_DIR).
fn repo_root() -> PathBuf {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest.parent().unwrap().parent().unwrap().to_path_buf()
}

fn read_repo(rel: &str) -> String {
    let p = repo_root().join(rel);
    fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {e}", p.display()))
}

// ── Extractors (line-based; intentionally dependency-free) ────────────────

/// `#define NAME 0x....` from a C header (name, value).
fn extract_c_defines(text: &str) -> Vec<(String, u32)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("#define ") {
            let mut parts = rest.split_whitespace();
            let name = parts.next().unwrap_or("");
            if let Some(hex) = parts.next().and_then(|v| v.strip_prefix("0x")) {
                if let Ok(v) = u32::from_str_radix(hex, 16) {
                    out.push((name.to_string(), v));
                }
            }
        }
    }
    out
}

/// `pub const NAME: ty = 0x....;` from a Rust source file (name, value).
fn extract_rust_consts(text: &str) -> Vec<(String, u32)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("pub const ") {
            if let Some((name, tail)) = rest.split_once(':') {
                if let Some(hex) = tail.split('=').nth(1) {
                    let hex = hex.trim().trim_end_matches(';').trim();
                    if let Some(h) = hex.strip_prefix("0x") {
                        if let Ok(v) = u32::from_str_radix(h, 16) {
                            out.push((name.trim().to_string(), v));
                        }
                    }
                }
            }
        }
    }
    out
}

/// `(defconstant +name+ #x....)` from generated Lisp (name, value).
fn extract_lisp_constants(text: &str) -> Vec<(String, u32)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("(defconstant +") {
            if let Some((name, tail)) = rest.split_once('+') {
                if let Some(hex) = tail.trim().strip_prefix("#x") {
                    let hex = hex.trim_end_matches(')').trim();
                    if let Ok(v) = u32::from_str_radix(hex, 16) {
                        out.push((name.to_string(), v));
                    }
                }
            }
        }
    }
    out
}

// ── Consistency check ──────────────────────────────────────────────────────

/// Assert every constant in `defs` that has a source-of-truth twin agrees,
/// and that every constant in `required` is present with the right value.
fn assert_consistent(defs: &[(String, u32)], label: &str, required: &[&str]) {
    let mut found: Vec<String> = Vec::new();
    for (name, val) in defs {
        if let Some(expect) = truth_value(name) {
            assert_eq!(
                *val, expect,
                "{label}: {name} = 0x{val:04X}, but kernel source of truth is 0x{expect:04X}"
            );
            found.push(name.clone());
        }
    }
    for req in required {
        let bare = req.strip_prefix("AEROSLS_").unwrap_or(req);
        assert!(
            defs.iter().any(|(n, v)| {
                n.strip_prefix("AEROSLS_").unwrap_or(n) == bare && *v == truth_value(req).unwrap()
            }),
            "{label}: missing required constant {req} (kernel value 0x{:04X})",
            truth_value(req).unwrap()
        );
    }
}

// ── Backend generation (same single-pass as `--target all`) ────────────────

fn generate_all_backends() -> (String, String, String, String) {
    let src = read_repo("idl/calculator.aeroidl");
    let doc = aeroidl_cc::parser::parse(&src).unwrap();
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    assert!(tc.errors.is_empty(), "type-check errors: {:?}", tc.errors);
    let ast = aeroidl_cc::emit::emit_value(&doc, &tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let client = format!("{common}\n\n{}", aeroidl_cc::gen_rust::emit_rust(&ast));
    let dispatcher = format!("{common}\n\n{}", aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast));
    let lisp = aeroidl_cc::gen_lisp::emit_lisp(&ast);
    let c = aeroidl_cc::gen_c::emit_c(&ast);
    (client, dispatcher, lisp, c)
}

// ── Tests ──────────────────────────────────────────────────────────────────

#[test]
fn kernel_source_of_truth() {
    let k = read_repo("kernel/cap.h");
    let defs = extract_c_defines(&k);

    // Every kernel #define with a user twin must match TRUTH (self-check).
    assert_consistent(
        &defs,
        "kernel/cap.h",
        &["CAP_PERM_R", "CAP_PERM_W", "CAP_PERM_X", "CAP_PERM_MAP", "CAP_NONE"],
    );

    // CHAN_FLAG_NO_REPLY has no #define in the kernel; it is bit0 of
    // ChanMsg.flags. Assert the comment documents bit0 = NO_REPLY so the
    // user-level 0x0001 has a documented anchor.
    assert!(
        k.contains("bit0 = NO_REPLY"),
        "kernel/cap.h must document ChanMsg.flags bit0 = NO_REPLY (async)"
    );
}

#[test]
fn c_sdk_headers_match_kernel() {
    // Base header (CAP_* names).
    let base = read_repo("user/libaerocap/aerocap.h");
    assert_consistent(
        &extract_c_defines(&base),
        "user/libaerocap/aerocap.h",
        &["CAP_PERM_R", "CAP_PERM_W", "CAP_PERM_MAP", "CAP_NONE"],
    );

    // SDK header (AEROSLS_* names) — the header generated C stubs include.
    let sdk = read_repo("user/libaerocap/aerosls_cap.h");
    assert_consistent(
        &extract_c_defines(&sdk),
        "user/libaerocap/aerosls_cap.h",
        &[
            "AEROSLS_CAP_PERM_R",
            "AEROSLS_CAP_PERM_W",
            "AEROSLS_CAP_NONE",
            "AEROSLS_CHAN_FLAG_NO_REPLY",
        ],
    );
}

#[test]
fn rust_runtime_matches_kernel() {
    let req = read_repo("user/aerosls/src/req.rs");
    assert_consistent(
        &extract_rust_consts(&req),
        "user/aerosls/src/req.rs",
        &["CAP_PERM_R", "CAP_PERM_W", "CAP_PERM_MAP", "CAP_NONE", "CHAN_FLAG_NO_REPLY"],
    );
}

#[test]
fn mocks_match_kernel() {
    // Rust mock — the crate the e2e compilation tests link against.
    let rust_mock = read_repo("tools/aeroidl/tests/mock_aerosls/src/lib.rs");
    assert_consistent(
        &extract_rust_consts(&rust_mock),
        "tools/aeroidl/tests/mock_aerosls/src/lib.rs",
        &["CAP_PERM_R", "CAP_PERM_W", "CAP_NONE", "CHAN_FLAG_NO_REPLY"],
    );

    // C mock — used when the C gate compiles the generated header without
    // the real SDK on the machine.
    let c_mock = read_repo("tools/aeroidl/tests/mock_aerosls_cap.h");
    assert_consistent(
        &extract_c_defines(&c_mock),
        "tools/aeroidl/tests/mock_aerosls_cap.h",
        &[
            "AEROSLS_CAP_PERM_R",
            "AEROSLS_CAP_PERM_W",
            "AEROSLS_CAP_NONE",
            "AEROSLS_CHAN_FLAG_NO_REPLY",
        ],
    );

    // main.rs embeds the same mock header for `--check` mode; it must stay
    // in lockstep with the tests/ copy.
    let main_rs = read_repo("tools/aeroidl/src/main.rs");
    assert_consistent(
        &extract_c_defines(&main_rs),
        "tools/aeroidl/src/main.rs (embedded --check mock)",
        &[
            "AEROSLS_CAP_PERM_R",
            "AEROSLS_CAP_PERM_W",
            "AEROSLS_CAP_PERM_X",
            "AEROSLS_CAP_NONE",
            "AEROSLS_CHAN_FLAG_NO_REPLY",
        ],
    );
}

#[test]
fn generated_rust_backends_match_kernel() {
    let (client, dispatcher, _, _) = generate_all_backends();

    for (code, label) in [
        (&client, "generated Rust client"),
        (&dispatcher, "generated Rust dispatcher"),
    ] {
        assert_consistent(
            &extract_rust_consts(code),
            label,
            &["CAP_PERM_R", "CAP_PERM_W", "CAP_NONE", "CHAN_FLAG_NO_REPLY"],
        );
    }
}

#[test]
fn generated_c_header_resolves_through_sdk_macros() {
    let (_, _, _, c) = generate_all_backends();

    // The generated header must include the SDK and reference its macros
    // rather than hardcoding flag/permission values, so it can never drift
    // independently of `aerosls_cap.h` (pinned in c_sdk_headers_match_kernel).
    assert!(
        c.contains("#include <aerosls_cap.h>"),
        "generated C header must include <aerosls_cap.h>"
    );
    for m in [
        "AEROSLS_CAP_PERM_R",
        "AEROSLS_CAP_PERM_W",
        "AEROSLS_CHAN_FLAG_NO_REPLY",
        "AEROSLS_CAP_NONE",
    ] {
        assert!(
            c.contains(m),
            "generated C header must reference {m} (macro from the SDK), not a literal value"
        );
    }
    // The descriptor type also comes from the SDK (pinned by the layout
    // tests) — the generated header must not define its own.
    assert!(
        c.contains("aerosls_cap_desc_t"),
        "generated C header must reference the SDK's aerosls_cap_desc_t type"
    );

    // And it must define none of these itself — only opcodes and the guard.
    for (name, _) in extract_c_defines(&c) {
        let bare = name.strip_prefix("AEROSLS_").unwrap_or(&name);
        assert!(
            !bare.starts_with("CHAN_FLAG") && !bare.starts_with("CAP_PERM") && bare != "CAP_NONE",
            "generated C header must not hardcode {name}; it resolves through the SDK"
        );
    }
}

#[test]
fn generated_lisp_defines_only_opcodes() {
    let (_, _, lisp, _) = generate_all_backends();

    // Lisp emits only `+op-*+` constants; a stray flag/permission constant
    // would have nowhere to resolve and must not be silently introduced.
    for (name, _) in extract_lisp_constants(&lisp) {
        assert!(
            name.starts_with("op-"),
            "generated Lisp defines unexpected constant +{name}+ (only +op-*+ allowed)"
        );
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Wire struct layout pinning
// ═══════════════════════════════════════════════════════════════════════════
// The capability descriptor and the message-header request structs cross the
// syscall ABI byte-for-byte: the caller builds one and the kernel reads it
// through a raw cast. Every definition (kernel, C SDK, Rust runtime, mocks,
// generated code) must therefore have the same C layout. We parse each
// definition with a small C-layout engine (C alignment rules, 64-bit
// pointers), anchor the KERNEL layouts against the hardcoded ABI table, then
// compare every other definition field-by-field against the kernel.

/// One parsed struct field: name, element type, optional array-length expr.
#[derive(Clone)]
struct Field {
    name: String,
    ty: String,
    arr: Option<String>,
}

/// (total size, [(field name, offset, size)]).
type Layout = (usize, Vec<(String, usize, usize)>);

/// Strip `/* */` and `//` comments (handles Rust `///` doc comments too).
fn strip_comments(text: &str) -> String {
    let mut out = String::new();
    let b = text.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'/' && i + 1 < b.len() {
            if b[i + 1] == b'*' {
                i += 2;
                while i + 1 < b.len() && !(b[i] == b'*' && b[i + 1] == b'/') {
                    i += 1;
                }
                i = (i + 2).min(b.len());
                continue;
            }
            if b[i + 1] == b'/' {
                while i < b.len() && b[i] != b'\n' {
                    i += 1;
                }
                continue;
            }
        }
        out.push(b[i] as char);
        i += 1;
    }
    out
}

/// All C structs as (name, body): `struct NAME { ... }` and anonymous
/// `typedef struct { ... } NAME;` (the trailing name is captured).
fn parse_c_structs(text: &str) -> Vec<(String, String)> {
    let cleaned = strip_comments(text);
    let mut out = Vec::new();
    let mut rest = cleaned.as_str();
    while let Some(pos) = rest.find("struct ") {
        let after = &rest[pos + 7..];
        let name: String = after
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
            .collect();
        let after_name = after[name.len()..].trim_start();
        if let Some(open) = after_name.strip_prefix('{') {
            if let Some(end) = open.find('}') {
                let final_name = if name.is_empty() {
                    // anonymous typedef: the name follows the closing brace
                    open[end + 1..]
                        .trim_start()
                        .chars()
                        .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
                        .collect()
                } else {
                    name.clone()
                };
                if !final_name.is_empty() {
                    out.push((final_name, open[..end].to_string()));
                }
                rest = &open[end + 1..];
                continue;
            }
        }
        rest = after;
    }
    out
}

/// All `pub struct NAME { ... }` as (name, body).
fn parse_rust_structs(text: &str) -> Vec<(String, String)> {
    let cleaned = strip_comments(text);
    let mut out = Vec::new();
    let mut rest = cleaned.as_str();
    while let Some(pos) = rest.find("pub struct ") {
        let after = &rest[pos + 11..];
        let name: String = after
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
            .collect();
        let after_name = after[name.len()..].trim_start();
        if let Some(open) = after_name.strip_prefix('{') {
            if let Some(end) = open.find('}') {
                out.push((name, open[..end].to_string()));
                rest = &open[end + 1..];
                continue;
            }
        }
        rest = after;
    }
    out
}

/// Split a field name like `_pad[4]` into (`_pad`, Some("4")).
fn split_array(name: &str) -> (String, Option<String>) {
    if let Some((n, a)) = name.split_once('[') {
        (n.to_string(), Some(a.trim_end_matches(']').to_string()))
    } else {
        (name.to_string(), None)
    }
}

/// C fields: `uint16_t slot;`, `uint8_t _pad[4];`, `struct SLSCapDesc caps[N];`
fn parse_c_fields(body: &str) -> Vec<Field> {
    let mut fields = Vec::new();
    for chunk in body.split(';') {
        let chunk = chunk.trim();
        if chunk.is_empty() {
            continue;
        }
        let tokens: Vec<&str> = chunk.split_whitespace().collect();
        if tokens.len() < 2 {
            continue;
        }
        let (ty, name) = if tokens[0] == "struct" {
            (format!("struct {}", tokens[1]), tokens[2].to_string())
        } else {
            (tokens[0].to_string(), tokens[1].to_string())
        };
        let (name, arr) = split_array(&name);
        fields.push(Field { name, ty, arr });
    }
    fields
}

/// Rust fields: `pub slot: u32,`, `pub _pad: [u8; 4],`, `pub caps: [CapDesc; N],`
fn parse_rust_fields(body: &str) -> Vec<Field> {
    let mut fields = Vec::new();
    for line in body.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("pub ") {
            if let Some((name, ty)) = rest.split_once(':') {
                let name = name.trim().to_string();
                let ty = ty.trim().trim_end_matches(',').trim().to_string();
                if ty.starts_with('[') && ty.ends_with(']') {
                    let inner = &ty[1..ty.len() - 1];
                    if let Some((elem, n)) = inner.rsplit_once("; ") {
                        fields.push(Field {
                            name,
                            ty: elem.trim().to_string(),
                            arr: Some(n.trim().to_string()),
                        });
                    }
                } else {
                    fields.push(Field { name, ty, arr: None });
                }
            }
        }
    }
    fields
}

/// `#define NAME 4` / `#define NAME 0x10` (decimal AND hex).
fn collect_c_consts(text: &str) -> HashMap<String, u64> {
    let mut m = HashMap::new();
    for line in strip_comments(text).lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("#define ") {
            let mut parts = rest.split_whitespace();
            let name = parts.next().unwrap_or("");
            if let Some(v) = parts.next() {
                let v = v.trim_end_matches('U').trim_end_matches('L');
                let val = if let Some(h) = v.strip_prefix("0x") {
                    u64::from_str_radix(h, 16).ok()
                } else {
                    v.parse::<u64>().ok()
                };
                if let Some(val) = val {
                    m.insert(name.to_string(), val);
                }
            }
        }
    }
    m
}

/// `pub const NAME: usize = 4;` (decimal AND hex, any integer type).
fn collect_rust_consts(text: &str) -> HashMap<String, u64> {
    let mut m = HashMap::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("pub const ") {
            if let Some((name, tail)) = rest.split_once(':') {
                if let Some(eq) = tail.split('=').nth(1) {
                    let eq = eq.trim().trim_end_matches(';').trim();
                    let val = if let Some(h) = eq.strip_prefix("0x") {
                        u64::from_str_radix(h, 16).ok()
                    } else {
                        eq.parse::<u64>().ok()
                    };
                    if let Some(val) = val {
                        m.insert(name.trim().to_string(), val);
                    }
                }
            }
        }
    }
    m
}

/// Scalar (non-array, non-struct) type → (size, align) under C rules.
fn scalar_layout(ty: &str) -> Option<(usize, usize)> {
    match ty {
        "uint8_t" | "int8_t" | "u8" | "i8" | "bool" | "char" => Some((1, 1)),
        "uint16_t" | "int16_t" | "u16" | "i16" => Some((2, 2)),
        "uint32_t" | "int32_t" | "u32" | "i32" | "f32" => Some((4, 4)),
        "uint64_t" | "int64_t" | "u64" | "i64" | "f64" => Some((8, 8)),
        "void*" | "void *" | "const void*" | "const void *" | "*const u8" | "*mut u8" => {
            Some((8, 8))
        }
        _ => None,
    }
}

fn align_up(x: usize, a: usize) -> usize {
    (x + a - 1) / a * a
}

/// Resolve an array length: literal number or a #define/pub-const name.
fn resolve_len(n: &str, consts: &HashMap<String, u64>) -> Option<u64> {
    if let Ok(v) = n.trim().parse::<u64>() {
        return Some(v);
    }
    consts.get(n.trim()).copied()
}

/// Element layout for a field type: scalar, pointer, or nested struct.
fn field_elem_layout(
    ty: &str,
    structs: &HashMap<String, Vec<Field>>,
    consts: &HashMap<String, u64>,
    memo: &mut HashMap<String, (usize, usize)>,
    self_name: &str,
) -> Option<(usize, usize)> {
    if let Some(sz) = scalar_layout(ty) {
        return Some(sz);
    }
    let bare = ty.strip_prefix("struct ").unwrap_or(ty);
    if bare == self_name || !structs.contains_key(bare) {
        return None;
    }
    if let Some(&(sz, al)) = memo.get(bare) {
        return Some((sz, al));
    }
    struct_layout(bare, structs, consts, memo)?;
    memo.get(bare).copied()
}

/// Compute (size, [(field, offset, size)]) for a struct, resolving arrays and
/// nested struct types under C alignment rules. `memo` caches (size, align).
fn struct_layout(
    name: &str,
    structs: &HashMap<String, Vec<Field>>,
    consts: &HashMap<String, u64>,
    memo: &mut HashMap<String, (usize, usize)>,
) -> Option<Layout> {
    let fields = structs.get(name).cloned()?;
    let mut off = 0usize;
    let mut max_align = 1usize;
    let mut out = Vec::new();
    for f in &fields {
        let (elem_sz, elem_align) = field_elem_layout(&f.ty, structs, consts, memo, name)?;
        let (sz, al) = match &f.arr {
            Some(n) => {
                let k = resolve_len(n, consts)? as usize;
                (elem_sz * k, elem_align)
            }
            None => (elem_sz, elem_align),
        };
        off = align_up(off, al);
        max_align = max_align.max(al);
        out.push((f.name.clone(), off, sz));
        off += sz;
    }
    let size = align_up(off, max_align);
    memo.insert(name.to_string(), (size, max_align));
    Some((size, out))
}

// ── Wire-struct matrix ─────────────────────────────────────────────────────
// Each entry: (wire id, [(file, local struct name, kind)]), kernel first.
// "@generated" is the generated Rust client from generate_all_backends().
// "c" files are parsed as C (main.rs holds an embedded C mock), "rust" as
// Rust. The descriptor's `slot` is u16 in the kernel/SDK but u32 (with a
// pad u16) in the generated/mock Rust — a deliberate, documented divergence
// (the runtime truncates via desc_to_kernel); its SIZE is exempt from the
// per-field comparison, its OFFSET is not.
const WIRE_STRUCTS: &[(&str, &[(&str, &str, &str)])] = &[
    (
        "cap descriptor",
        &[
            ("kernel/cap.h", "SLSCapDesc", "c"),
            ("user/libaerocap/aerosls_cap.h", "aerosls_cap_desc_t", "c"),
            ("user/aerosls/src/req.rs", "CapDesc", "rust"),
            ("tools/aeroidl/tests/mock_aerosls_cap.h", "aerosls_cap_desc_t", "c"),
            ("tools/aeroidl/tests/mock_aerosls/src/lib.rs", "CapDescriptor", "rust"),
            ("tools/aeroidl/src/main.rs", "aerosls_cap_desc_t", "c"),
            ("@generated", "CapDescriptor", "rust"),
        ],
    ),
    (
        "send-msg request",
        &[
            ("kernel/cap.h", "SLSCapSendMsgRequest", "c"),
            ("user/libaerocap/aerosls_cap.h", "sls_cap_send_msg_req", "c"),
            ("user/aerosls/src/req.rs", "SendMsgReq", "rust"),
        ],
    ),
    (
        "recv-msg request",
        &[
            ("kernel/cap.h", "SLSCapRecvMsgRequest", "c"),
            ("user/libaerocap/aerosls_cap.h", "sls_cap_recv_msg_req", "c"),
            ("user/aerosls/src/req.rs", "RecvMsgReq", "rust"),
        ],
    ),
    (
        "arena-free request",
        &[
            ("kernel/cap.h", "SLSCapArenaFreeRequest", "c"),
            ("user/libaerocap/aerosls_cap.h", "sls_cap_arena_free_req", "c"),
            ("user/aerosls/src/req.rs", "ArenaFreeReq", "rust"),
        ],
    ),
];

/// Field names whose SIZE may differ from the kernel's (the slot divergence).
const DESC_SIZE_EXEMPT: &[&str] = &["slot"];

/// Load and compute the layout of `struct_name` in `file` (or the generated
/// client when `file == "@generated"`). Panics loudly on parse failure.
fn load_layout(file: &str, struct_name: &str, kind: &str, generated: Option<&str>) -> Layout {
    let text = if file == "@generated" {
        generated.expect("@generated needs the generated client string").to_string()
    } else {
        read_repo(file)
    };
    let structs: HashMap<String, Vec<Field>> = if kind == "c" {
        parse_c_structs(&text)
    } else {
        parse_rust_structs(&text)
    }
    .into_iter()
    .map(|(n, body)| {
        let fields = if kind == "c" {
            parse_c_fields(&body)
        } else {
            parse_rust_fields(&body)
        };
        (n, fields)
    })
    .collect();
    let consts = if kind == "c" {
        collect_c_consts(&text)
    } else {
        collect_rust_consts(&text)
    };
    let mut memo = HashMap::new();
    struct_layout(struct_name, &structs, &consts, &mut memo)
        .unwrap_or_else(|| panic!("could not compute layout of {struct_name} in {file}"))
}

/// Every kernel field must exist at the same offset with the same size (size
/// exempt only for the listed names, e.g. the descriptor's slot), and total
/// sizes must be equal. Extra fields are allowed only in padding zones.
fn assert_layout_consistent(
    wire: &str,
    kernel: &Layout,
    other: &Layout,
    label: &str,
    size_exempt: &[&str],
) {
    assert_eq!(
        kernel.0, other.0,
        "{wire}: {label} total size {} != kernel {}",
        other.0,
        kernel.0
    );
    for (name, koff, ksize) in &kernel.1 {
        let o = other
            .1
            .iter()
            .find(|(n, _, _)| n == name)
            .unwrap_or_else(|| panic!("{wire}: {label} missing kernel field {name}"));
        assert_eq!(
            o.1, *koff,
            "{wire}: {label} field {name} at offset {} != kernel offset {koff}",
            o.1
        );
        if !size_exempt.contains(&name.as_str()) {
            assert_eq!(
                o.2, *ksize,
                "{wire}: {label} field {name} size {} != kernel size {ksize} (wire-layout drift)",
                o.2
            );
        }
    }
}

/// Kernel layouts must equal the hardcoded ABI table. This anchors the
/// parser: if the engine itself regressed (wrong alignment rule, missed
/// field), every comparison would still agree — so the kernel anchor is what
/// keeps the test honest.
fn assert_kernel_anchored(
    table: &[(&str, &[(&str, &str, &str)])],
    wire_idx: usize,
    expected_size: usize,
    expected: &[(&str, usize)],
) {
    let (id, files) = table[wire_idx];
    let (file, sname, kind) = files[0];
    let (size, fields) = load_layout(file, sname, kind, None);
    assert_eq!(size, expected_size, "kernel {id}: total size {size} != expected {expected_size}");
    for (fname, foff) in expected {
        assert!(
            fields.iter().any(|(n, o, _)| n == fname && *o == *foff),
            "kernel {id}: field {fname} missing or not at offset {foff} (parsed: {fields:?})"
        );
    }
}

#[test]
fn kernel_wire_layouts_anchored() {
    // SLSCapDesc: 16 bytes — slot u16 @0, offset u32 @4, len u32 @8,
    // rights u8 @12, flags u8 @13 (2 bytes tail padding).
    assert_kernel_anchored(
        WIRE_STRUCTS,
        0,
        16,
        &[("slot", 0), ("offset", 4), ("len", 8), ("rights", 12), ("flags", 13)],
    );
    // SLSCapSendMsgRequest: 96 bytes.
    assert_kernel_anchored(
        WIRE_STRUCTS,
        1,
        96,
        &[
            ("ch_w_idx", 0),
            ("n_caps", 2),
            ("tag", 8),
            ("flags", 12),
            ("payload_len", 16),
            ("payload", 24),
            ("caps", 32),
        ],
    );
    // SLSCapRecvMsgRequest: 104 bytes.
    assert_kernel_anchored(
        WIRE_STRUCTS,
        2,
        104,
        &[
            ("ch_r_idx", 0),
            ("block", 2),
            ("max_caps", 4),
            ("buf", 8),
            ("buf_len", 16),
            ("out_tag", 24),
            ("out_flags", 28),
            ("out_payload_len", 32),
            ("out_n_caps", 36),
            ("out_caps", 40),
        ],
    );
    // SLSCapArenaFreeRequest: 8 bytes.
    assert_kernel_anchored(WIRE_STRUCTS, 3, 8, &[("cap_idx", 0)]);
}

// ── Legacy single-cap request structs (the Phase-1.5 path) ─────────────────
// The pre-message syscalls (chan_create, send, recv, revoke, map, unmap,
// create_mem, arena_alloc) each take one opaque request struct. The Rust
// runtime has twins only for map/unmap/arena-alloc (the ones its arena
// machinery issues); the send/recv/revoke/chan-create path is C-only, so
// those entries pin kernel ↔ C SDK.
const LEGACY_WIRE_STRUCTS: &[(&str, &[(&str, &str, &str)])] = &[
    (
        "create-mem request",
        &[
            ("kernel/cap.h", "SLSCapCreateMemRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_create_mem_req", "c"),
        ],
    ),
    (
        "arena-alloc request",
        &[
            ("kernel/cap.h", "SLSCapArenaAllocRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_arena_alloc_req", "c"),
            ("user/aerosls/src/req.rs", "ArenaAllocReq", "rust"),
        ],
    ),
    (
        "chan-create request",
        &[
            ("kernel/cap.h", "SLSCapChanCreateRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_chan_create_req", "c"),
        ],
    ),
    (
        "send request",
        &[
            ("kernel/cap.h", "SLSCapSendRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_send_req", "c"),
        ],
    ),
    (
        "recv request",
        &[
            ("kernel/cap.h", "SLSCapRecvRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_recv_req", "c"),
        ],
    ),
    (
        "revoke request",
        &[
            ("kernel/cap.h", "SLSCapRevokeRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_revoke_req", "c"),
        ],
    ),
    (
        "map request",
        &[
            ("kernel/cap.h", "SLSCapMapRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_map_req", "c"),
            ("user/aerosls/src/req.rs", "MapReq", "rust"),
        ],
    ),
    (
        "unmap request",
        &[
            ("kernel/cap.h", "SLSCapUnmapRequest", "c"),
            ("user/libaerocap/aerocap.h", "sls_cap_unmap_req", "c"),
            ("user/aerosls/src/req.rs", "UnmapReq", "rust"),
        ],
    ),
];

#[test]
fn legacy_wire_layouts_anchored() {
    // SLSCapCreateMemRequest: 16 bytes.
    assert_kernel_anchored(
        LEGACY_WIRE_STRUCTS,
        0,
        16,
        &[("phys_base", 0), ("npages", 8), ("perm", 12)],
    );
    // SLSCapArenaAllocRequest: 8 bytes.
    assert_kernel_anchored(LEGACY_WIRE_STRUCTS, 1, 8, &[("npages", 0), ("perm", 4)]);
    // SLSCapChanCreateRequest: 20 bytes.
    assert_kernel_anchored(
        LEGACY_WIRE_STRUCTS,
        2,
        20,
        &[
            ("far_pid", 0),
            ("out_rd", 4),
            ("out_wr", 6),
            ("out_far_rd", 8),
            ("out_far_wr", 10),
            ("_pad", 12),
        ],
    );
    // SLSCapSendRequest: 16 bytes.
    assert_kernel_anchored(
        LEGACY_WIRE_STRUCTS,
        3,
        16,
        &[("ch_w_idx", 0), ("cap_idx", 2), ("_pad", 4), ("cookie", 8)],
    );
    // SLSCapRecvRequest: 16 bytes.
    assert_kernel_anchored(
        LEGACY_WIRE_STRUCTS,
        4,
        16,
        &[
            ("ch_r_idx", 0),
            ("block", 2),
            ("_pad", 3),
            ("release_pid", 4),
            ("cookie", 8),
        ],
    );
    // SLSCapRevokeRequest: 8 bytes.
    assert_kernel_anchored(LEGACY_WIRE_STRUCTS, 5, 8, &[("cap_idx", 0)]);
    // SLSCapMapRequest: 24 bytes.
    assert_kernel_anchored(
        LEGACY_WIRE_STRUCTS,
        6,
        24,
        &[("cap_idx", 0), ("_pad", 2), ("vaddr", 8), ("flags", 16), ("_pad2", 20)],
    );
    // SLSCapUnmapRequest: 16 bytes.
    assert_kernel_anchored(
        LEGACY_WIRE_STRUCTS,
        7,
        16,
        &[("cap_idx", 0), ("_pad", 2), ("vaddr", 8)],
    );
}

#[test]
fn legacy_wire_request_layouts_consistent() {
    for (wire, files) in LEGACY_WIRE_STRUCTS {
        let (file, sname, kind) = files[0];
        let kernel = load_layout(file, sname, kind, None);
        for (file, sname, kind) in &files[1..] {
            let other = load_layout(file, sname, kind, None);
            assert_layout_consistent(wire, &kernel, &other, &format!("{file}::{sname}"), &[]);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Syscall numbers, error codes, and message limits
// ═══════════════════════════════════════════════════════════════════════════
// The constant and layout tests pin permissions, flags, and structs. This
// section pins the remaining numeric ABI: the syscall numbers themselves
// (kernel names them SYS_SLS_CAP_*, the C SDK SLS_SYS_CAP_*, the Rust
// runtime SYS_CAP_*), the CAP_E* error codes, and the message limits
// (CAP_MSG_MAX_CAPS / CAP_MSG_MAX_PAYLOAD). The C side is also pinned in
// tests/aerocap_abi_host_test.c; this extends the same contract to the Rust
// runtime and keeps every number in one grep-able test.

/// `#define NAME <signed int>` — hex or decimal, optional parens and U/L
/// suffixes: `CAP_EINVAL (-1)`, `SLS_SYS_CAP_MAP 295`, `CAP_MSG_MAX_PAYLOAD 4096`.
fn extract_c_ints(text: &str) -> Vec<(String, i64)> {
    let mut out = Vec::new();
    for line in strip_comments(text).lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("#define ") {
            let mut parts = rest.split_whitespace();
            let name = parts.next().unwrap_or("");
            if let Some(v) = parts.next() {
                let v = v.trim().trim_start_matches('(').trim_end_matches(')');
                let v = v.trim_end_matches('U').trim_end_matches('L');
                let val = if let Some(h) = v.strip_prefix("0x") {
                    i64::from_str_radix(h, 16).ok()
                } else {
                    v.parse::<i64>().ok()
                };
                if let Some(val) = val {
                    out.push((name.to_string(), val));
                }
            }
        }
    }
    out
}

/// `pub const NAME: ty = <signed int>;` — hex or decimal, may be negative.
fn extract_rust_ints(text: &str) -> Vec<(String, i64)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("pub const ") {
            if let Some((name, tail)) = rest.split_once(':') {
                if let Some(eq) = tail.split('=').nth(1) {
                    let eq = eq.trim().trim_end_matches(';').trim();
                    let val = if let Some(h) = eq.strip_prefix("0x") {
                        i64::from_str_radix(h, 16).ok()
                    } else {
                        eq.parse::<i64>().ok()
                    };
                    if let Some(val) = val {
                        out.push((name.trim().to_string(), val));
                    }
                }
            }
        }
    }
    out
}

/// Canonical syscall keys (kernel suffix after the SYS_SLS_ prefix) → number.
const SYSCALLS: &[(&str, i64)] = &[
    ("CAP_CREATE_MEM", 289),
    ("CAP_ARENA_ALLOC", 290),
    ("CHAN_CREATE", 291),
    ("CAP_SEND", 292),
    ("CAP_RECV", 293),
    ("CAP_REVOKE", 294),
    ("CAP_MAP", 295),
    ("CAP_UNMAP", 296),
    ("CAP_LIST", 297),
    ("CAP_SEND_MSG", 302),
    ("CAP_RECV_MSG", 303),
    ("CAP_ARENA_FREE", 304),
];

/// Kernel SYS_SLS_CAP_X / SDK SLS_SYS_CAP_X / Rust SYS_CAP_X → CAP_X.
fn norm_syscall(name: &str) -> Option<&str> {
    name.strip_prefix("SYS_SLS_")
        .or_else(|| name.strip_prefix("SLS_SYS_"))
        .or_else(|| name.strip_prefix("SYS_"))
}

fn expected_syscall(key: &str) -> i64 {
    SYSCALLS
        .iter()
        .find(|(k, _)| *k == key)
        .map(|(_, v)| *v)
        .unwrap_or_else(|| panic!("unknown syscall key {key}"))
}

/// Every syscall `file` defines must agree with the kernel; the required set
/// must be present. `kind` is "c" or "rust".
fn assert_syscalls(file: &str, kind: &str, required: &[&str]) {
    let text = read_repo(file);
    let defs = if kind == "c" { extract_c_ints(&text) } else { extract_rust_ints(&text) };
    let mut found: Vec<String> = Vec::new();
    for (name, val) in &defs {
        if let Some(key) = norm_syscall(name) {
            if SYSCALLS.iter().any(|(k, _)| *k == key) {
                assert_eq!(
                    *val,
                    expected_syscall(key),
                    "{file}: {name} = {val}, kernel says {}",
                    expected_syscall(key)
                );
                found.push(key.to_string());
            }
        }
    }
    for req in required {
        assert!(
            found.iter().any(|f| f == req),
            "{file}: missing syscall {req} (kernel {})",
            expected_syscall(req)
        );
    }
}

#[test]
fn syscall_numbers_consistent() {
    // Kernel defines all 12; the C SDK splits them across its two headers;
    // the Rust runtime defines only the syscalls it issues (the legacy
    // send/recv/revoke/chan-create path is C-only).
    assert_syscalls(
        "kernel/cap.h",
        "c",
        &[
            "CAP_CREATE_MEM",
            "CAP_ARENA_ALLOC",
            "CHAN_CREATE",
            "CAP_SEND",
            "CAP_RECV",
            "CAP_REVOKE",
            "CAP_MAP",
            "CAP_UNMAP",
            "CAP_LIST",
            "CAP_SEND_MSG",
            "CAP_RECV_MSG",
            "CAP_ARENA_FREE",
        ],
    );
    assert_syscalls(
        "user/libaerocap/aerocap.h",
        "c",
        &[
            "CAP_CREATE_MEM",
            "CAP_ARENA_ALLOC",
            "CHAN_CREATE",
            "CAP_SEND",
            "CAP_RECV",
            "CAP_REVOKE",
            "CAP_MAP",
            "CAP_UNMAP",
            "CAP_LIST",
        ],
    );
    assert_syscalls(
        "user/libaerocap/aerosls_cap.h",
        "c",
        &["CAP_SEND_MSG", "CAP_RECV_MSG", "CAP_ARENA_FREE"],
    );
    assert_syscalls(
        "user/aerosls/src/req.rs",
        "rust",
        &[
            "CAP_ARENA_ALLOC",
            "CAP_MAP",
            "CAP_UNMAP",
            "CAP_SEND_MSG",
            "CAP_RECV_MSG",
            "CAP_ARENA_FREE",
        ],
    );
}

#[test]
fn error_codes_consistent() {
    // Same names in every language — no prefix normalization needed.
    let expected: &[(&str, i64)] = &[
        ("CAP_EINVAL", -1),
        ("CAP_EBADF", -2),
        ("CAP_EAGAIN", -3),
        ("CAP_ETABLEFULL", -4),
        ("CAP_ECAPREVOKED", -5),
        ("CAP_EALREADY", -6),
        ("CAP_ENOMEM", -7),
        ("CAP_ENOSPC", -8),
        ("CAP_ERANGE", -9),
        ("CAP_ENOSYS", -10),
        ("CAP_ECONFLICT", -11),
    ];
    for (file, kind) in [
        ("kernel/cap.h", "c"),
        ("user/libaerocap/aerocap.h", "c"),
        ("user/aerosls/src/req.rs", "rust"),
    ] {
        let text = read_repo(file);
        let defs = if kind == "c" { extract_c_ints(&text) } else { extract_rust_ints(&text) };
        for (name, want) in expected {
            let got = defs
                .iter()
                .find(|(n, _)| n == name)
                .unwrap_or_else(|| panic!("{file}: missing error code {name}"));
            assert_eq!(got.1, *want, "{file}: {name} = {} != kernel {want}", got.1);
        }
    }
}

#[test]
fn message_limits_consistent() {
    // (file, kind, local name, expected) — the same limit under each
    // language's naming: CAP_MSG_MAX_* / AEROSLS_MSG_MAX_* / MSG_MAX_*.
    let cases: &[(&str, &str, &str, i64)] = &[
        ("kernel/cap.h", "c", "CAP_MSG_MAX_CAPS", 4),
        ("kernel/cap.h", "c", "CAP_MSG_MAX_PAYLOAD", 4096),
        ("user/libaerocap/aerosls_cap.h", "c", "AEROSLS_MSG_MAX_CAPS", 4),
        ("user/libaerocap/aerosls_cap.h", "c", "AEROSLS_MSG_MAX_PAYLOAD", 4096),
        ("user/aerosls/src/req.rs", "rust", "MSG_MAX_CAPS", 4),
        ("user/aerosls/src/req.rs", "rust", "MSG_MAX_PAYLOAD", 4096),
    ];
    for (file, kind, name, want) in cases {
        let text = read_repo(file);
        let defs = if *kind == "c" { extract_c_ints(&text) } else { extract_rust_ints(&text) };
        let got = defs
            .iter()
            .find(|(n, _)| n == name)
            .unwrap_or_else(|| panic!("{file}: missing {name}"));
        assert_eq!(got.1, *want, "{file}: {name} = {} != {want}", got.1);
    }
}

/// kernel/syscall_dispatch.c's cap cases must use exactly the kernel's
/// syscall macros: every `case SYS_SLS_CAP_*` / `case SYS_SLS_CHAN_CREATE`
/// label must be defined in kernel/cap.h, and every cap.h syscall number
/// must reach a dispatch case. A syscall defined but never dispatched is a
/// silent dead entry point; a case with an undefined name is a build break
/// waiting to happen. Because the case labels carry the macro (never a
/// literal number), number agreement is automatic once the label↔define
/// wiring and the define↔number table (syscall_numbers_consistent) both
/// hold — this pins that wiring without needing a C compiler.
#[test]
fn dispatch_cases_wired() {
    let dispatch = read_repo("kernel/syscall_dispatch.c");
    let cap_h = read_repo("kernel/cap.h");

    // The names kernel/cap.h defines for the 12 syscalls (kernel naming).
    let kern_defs: Vec<String> = extract_c_ints(&cap_h)
        .into_iter()
        .filter(|(n, _)| {
            norm_syscall(n)
                .map(|k| SYSCALLS.iter().any(|(kk, _)| *kk == k))
                .unwrap_or(false)
        })
        .map(|(n, _)| n)
        .collect();
    assert_eq!(kern_defs.len(), 12, "expected 12 syscall defines in kernel/cap.h");

    // 1. Every cap-family case label is one of the kernel's 12 macros.
    let mut cases: Vec<String> = Vec::new();
    for line in dispatch.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("case ") {
            if let Some(name) = rest.trim_end_matches(':').strip_prefix("SYS_SLS_") {
                let full = format!("SYS_SLS_{name}");
                if norm_syscall(&full).map(|k| SYSCALLS.iter().any(|(kk, _)| *kk == k)).unwrap_or(false) {
                    assert!(
                        kern_defs.iter().any(|d| *d == full),
                        "dispatch references {full}, which kernel/cap.h does not define"
                    );
                    cases.push(full);
                }
            }
        }
    }
    assert_eq!(
        cases.len(),
        12,
        "expected 12 cap dispatch cases in syscall_dispatch.c, got {} (a duplicate or a renamed case)",
        cases.len()
    );

    // 2. Every kernel/cap.h syscall number reaches a dispatch case.
    for (k, _) in SYSCALLS {
        let full = format!("SYS_SLS_{k}");
        assert!(
            cases.iter().any(|c| *c == full),
            "syscall {full} ({}) is defined in cap.h but never dispatched",
            expected_syscall(k)
        );
    }
}

#[test]
fn wire_cap_descriptor_layout_consistent() {
    let (wire, files) = WIRE_STRUCTS[0];
    let (file, sname, kind) = files[0];
    let kernel = load_layout(file, sname, kind, None);
    let client = generate_all_backends().0;
    for (file, sname, kind) in &files[1..] {
        let other = load_layout(file, sname, kind, Some(&client));
        assert_layout_consistent(wire, &kernel, &other, &format!("{file}::{sname}"), DESC_SIZE_EXEMPT);
    }
}

#[test]
fn wire_message_request_layouts_consistent() {
    let client = generate_all_backends().0; // unused for these structs, kept for symmetry
    let _ = client;
    for (wire, files) in &WIRE_STRUCTS[1..] {
        let (file, sname, kind) = files[0];
        let kernel = load_layout(file, sname, kind, None);
        for (file, sname, kind) in &files[1..] {
            let other = load_layout(file, sname, kind, None);
            assert_layout_consistent(wire, &kernel, &other, &format!("{file}::{sname}"), &[]);
        }
    }
}


