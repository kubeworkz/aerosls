//! `aeroidl` — the AeroIDL compiler CLI.
//!
//! Usage:
//! ```bash
//! aeroidl <input.aeroidl>                    # parse, type-check, emit JSON to stdout
//! aeroidl <input.aeroidl> -o <output.json>   # emit JSON to file
//! aeroidl --check <input.aeroidl>            # type-check only (no JSON output)
//! aeroidl <input.aeroidl> --target rust      # emit Rust client stubs
//! aeroidl <input.aeroidl> --target dispatch  # emit Rust dispatcher
//! aeroidl <input.aeroidl> --target both      # emit both client + dispatcher
//! aeroidl <input.aeroidl> --target lisp      # emit Common Lisp
//! aeroidl <input.aeroidl> --target all       # emit rust + dispatch + lisp + c
//! aeroidl --check-all <input.aeroidl>        # generate all 4 + compile-check them
//! ```

use std::env;
use std::fs;
use std::process;

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        eprintln!("Usage: aeroidl <input.aeroidl> [-o <output>]");
        eprintln!("       aeroidl --check <input.aeroidl>");
        eprintln!("       aeroidl <input.aeroidl> --target rust [-o <output.rs>]");
        eprintln!("       aeroidl <input.aeroidl> --target dispatch [-o <output.rs>]");
        eprintln!("       aeroidl <input.aeroidl> --target both [-o <prefix.rs>]");
        eprintln!("       aeroidl <input.aeroidl> --target lisp [-o <output.lisp>]");
        eprintln!("       aeroidl <input.aeroidl> --target c [-o <output.h>]");
        eprintln!("       aeroidl <input.aeroidl> --target all  [-o <prefix.rs>]");
        eprintln!("       aeroidl --check-all <input.aeroidl>");
        process::exit(1);
    }

    let mut check_only = false;
    let mut check_all = false;
    let mut input_path = None;
    let mut output_path = None;
    let mut target: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--check" => check_only = true,
            "--check-all" => check_all = true,
            "--target" => {
                i += 1;
                target = args.get(i).cloned();
            }
            "-o" => {
                i += 1;
                output_path = args.get(i).cloned();
            }
            _ if input_path.is_none() => input_path = Some(args[i].clone()),
            other => {
                eprintln!("error: unknown argument '{other}'");
                process::exit(1);
            }
        }
        i += 1;
    }

    let input_path = match input_path {
        Some(p) => p,
        None => {
            eprintln!("error: no input file specified");
            process::exit(1);
        }
    };

    // Read input
    let source = match fs::read_to_string(&input_path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: cannot read '{input_path}': {e}");
            process::exit(1);
        }
    };

    // Parse
    let doc = match aeroidl_cc::parser::parse(&source) {
        Ok(doc) => doc,
        Err(errors) => {
            eprintln!("parse errors in {input_path}:");
            for e in &errors {
                eprintln!("  {e}");
            }
            process::exit(1);
        }
    };

    // Type-check
    let tc = aeroidl_cc::tycheck::type_check(&doc);
    if !tc.errors.is_empty() {
        eprintln!("type-check errors in {input_path}:");
        for e in &tc.errors {
            eprintln!("  {e}");
        }
        process::exit(1);
    }

    if check_only {
        println!("{input_path}: OK ({errors} errors)", errors = tc.errors.len());
        process::exit(0);
    }

    if check_all {
        let exit_code = run_check_all(&input_path, &doc, &tc);
        process::exit(exit_code);
    }

    // Emit output
    match target.as_deref() {
        Some("all") => {
            // Generate all four backends from a single AST parse so the
            // opcodes, types, and wire formats can never drift apart.
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let common = aeroidl_cc::gen_rust::emit_common_types();
            let client = aeroidl_cc::gen_rust::emit_rust(&ast_value);
            let dispatcher = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast_value);
            let lisp = aeroidl_cc::gen_lisp::emit_lisp(&ast_value);
            let c = aeroidl_cc::gen_c::emit_c(&ast_value);

            match output_path {
                Some(path) => {
                    // foo.rs -> foo_client.rs, foo_dispatch.rs, foo.lisp, foo.h
                    let (stem, ext) = match path.rsplit_once('.') {
                        Some((stem, ext)) => (stem, ext),
                        None => (path.as_str(), "rs"),
                    };
                    let client_path = format!("{stem}_client.{ext}");
                    let dispatch_path = format!("{stem}_dispatch.{ext}");
                    let lisp_path = format!("{stem}.lisp");
                    let c_path = format!("{stem}.h");

                    if let Err(e) = fs::write(&client_path, &format!("{common}\n\n{client}")) {
                        eprintln!("error: cannot write '{client_path}': {e}");
                        process::exit(1);
                    }
                    if let Err(e) = fs::write(&dispatch_path, &format!("{common}\n\n{dispatcher}")) {
                        eprintln!("error: cannot write '{dispatch_path}': {e}");
                        process::exit(1);
                    }
                    if let Err(e) = fs::write(&lisp_path, &lisp) {
                        eprintln!("error: cannot write '{lisp_path}': {e}");
                        process::exit(1);
                    }
                    if let Err(e) = fs::write(&c_path, &c) {
                        eprintln!("error: cannot write '{c_path}': {e}");
                        process::exit(1);
                    }
                    eprintln!("wrote {client_path}");
                    eprintln!("wrote {dispatch_path}");
                    eprintln!("wrote {lisp_path}");
                    eprintln!("wrote {c_path}");
                }
                None => {
                    println!("{common}\n\n{client}");
                    println!("\n// ═══════════════════════════════════════════════════════════════════");
                    println!("// DISPATCHER (server side)");
                    println!("// ═══════════════════════════════════════════════════════════════════\n");
                    println!("{common}\n\n{dispatcher}");
                    println!("\n// ═══════════════════════════════════════════════════════════════════");
                    println!("// COMMON LISP");
                    println!("// ═══════════════════════════════════════════════════════════════════\n");
                    println!("{lisp}");
                    println!("\n/* ═══════════════════════════════════════════════════════════════════");
                    println!(" * C HEADER");
                    println!(" * ═══════════════════════════════════════════════════════════════════ */\n");
                    println!("{c}");
                }
            }
        }
        Some("both") => {
            // Generate both client stubs and dispatcher in a single AST pass
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let common = aeroidl_cc::gen_rust::emit_common_types();
            let client = aeroidl_cc::gen_rust::emit_rust(&ast_value);
            let dispatcher = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast_value);

            match output_path {
                Some(path) => {
                    // Derive two filenames: foo.rs -> foo_client.rs + foo_dispatch.rs
                    let (client_path, dispatch_path) = split_output_path(&path);

                    let client_out = format!("{common}\n\n{client}");
                    let dispatch_out = format!("{common}\n\n{dispatcher}");

                    if let Err(e) = fs::write(&client_path, &client_out) {
                        eprintln!("error: cannot write '{client_path}': {e}");
                        process::exit(1);
                    }
                    if let Err(e) = fs::write(&dispatch_path, &dispatch_out) {
                        eprintln!("error: cannot write '{dispatch_path}': {e}");
                        process::exit(1);
                    }
                    eprintln!("wrote {client_path}");
                    eprintln!("wrote {dispatch_path}");
                }
                None => {
                    println!("{common}\n\n{client}");
                    println!("\n// ═══════════════════════════════════════════════════════════════════");
                    println!("// DISPATCHER (server side)");
                    println!("// ═══════════════════════════════════════════════════════════════════\n");
                    println!("{common}\n\n{dispatcher}");
                }
            }
        }
        Some("rust") => {
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let common = aeroidl_cc::gen_rust::emit_common_types();
            let module = aeroidl_cc::gen_rust::emit_rust(&ast_value);
            write_output(&output_path, &format!("{common}\n\n{module}"));
        }
        Some("lisp") => {
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let output = aeroidl_cc::gen_lisp::emit_lisp(&ast_value);
            write_output(&output_path, &output);
        }
        Some("c") => {
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let output = aeroidl_cc::gen_c::emit_c(&ast_value);
            write_output(&output_path, &output);
        }
        Some("dispatch") => {
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let common = aeroidl_cc::gen_rust::emit_common_types();
            let dispatcher = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast_value);
            write_output(&output_path, &format!("{common}\n\n{dispatcher}"));
        }
        Some(other) => {
            eprintln!("error: unknown target '{other}' (supported: rust, lisp, dispatch, both, c, all)");
            process::exit(1);
        }
        None => {
            let output = aeroidl_cc::emit::emit_json(&doc, &tc);
            write_output(&output_path, &output);
        }
    }
}

/// Write output to a file or stdout.
fn write_output(path: &Option<String>, output: &str) {
    match path {
        Some(path) => {
            if let Err(e) = fs::write(path, output) {
                eprintln!("error: cannot write '{path}': {e}");
                process::exit(1);
            }
            eprintln!("wrote {path}");
        }
        None => {
            println!("{output}");
        }
    }
}

/// Split an output path into two files for client and dispatcher.
///
/// ```text
/// foo.rs           -> (foo_client.rs, foo_dispatch.rs)
/// path/to/bar.rs   -> (path/to/bar_client.rs, path/to/bar_dispatch.rs)
/// foo              -> (foo_client, foo_dispatch)
/// ```
fn split_output_path(path: &str) -> (String, String) {
    match path.rsplit_once('.') {
        Some((stem, ext)) => (
            format!("{stem}_client.{ext}"),
            format!("{stem}_dispatch.{ext}"),
        ),
        None => (
            format!("{path}_client"),
            format!("{path}_dispatch"),
        ),
    }
}

// ── --check-all: generate all four backends and compile-check them ────────

use std::path::Path;
use std::process::Command;

/// Generate all four backends into a temp dir and verify each compiles:
/// `cargo check` for the Rust client + dispatcher, gcc/clang for the C
/// header (when a C compiler exists), and a structural paren-balance check
/// for the Lisp output. Returns the process exit code.
fn run_check_all(input_path: &str, doc: &aeroidl_cc::ast::Document, tc: &aeroidl_cc::tycheck::TypeCheckResult) -> i32 {
    // Single AST pass — identical to --target all.
    let ast_value = aeroidl_cc::emit::emit_value(doc, tc);
    let common = aeroidl_cc::gen_rust::emit_common_types();
    let client = aeroidl_cc::gen_rust::emit_rust(&ast_value);
    let dispatcher = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast_value);
    let lisp = aeroidl_cc::gen_lisp::emit_lisp(&ast_value);
    let c = aeroidl_cc::gen_c::emit_c(&ast_value);

    // Temp workspace
    let tmp = std::env::temp_dir().join(format!("aeroidl_check_{}", std::process::id()));
    let _ = fs::remove_dir_all(&tmp);
    if let Err(e) = fs::create_dir_all(&tmp) {
        eprintln!("error: cannot create temp dir {}: {e}", tmp.display());
        return 1;
    }

    let mut all_ok = true;

    // ── 1. Rust client + dispatcher via cargo check ─────────────────────
    all_ok &= check_rust(&tmp, &common, &client, &dispatcher);

    // ── 2. C header via gcc/clang ───────────────────────────────────────
    all_ok &= check_c(&tmp, &c);

    // ── 3. Lisp structural check ────────────────────────────────────────
    all_ok &= check_lisp(&tmp, &lisp);

    let _ = fs::remove_dir_all(&tmp);

    if all_ok {
        println!("{input_path}: all 4 backends OK");
        0
    } else {
        eprintln!("{input_path}: check-all FAILED (see errors above)");
        1
    }
}

/// Write a temp Cargo project with the client + dispatcher as modules and
/// run `cargo check`.
fn check_rust(tmp: &Path, common: &str, client: &str, dispatcher: &str) -> bool {
    let project = tmp.join("rust");
    let src_dir = project.join("src");
    if fs::create_dir_all(&src_dir).is_err() {
        return false;
    }

    let cargo_toml = r#"[package]
name = "aeroidl-check"
version = "0.1.0"
edition = "2021"

[dependencies]
"#;
    if fs::write(project.join("Cargo.toml"), cargo_toml).is_err() {
        return false;
    }

    let lib_rs = "extern crate alloc;\nmod generated_client;\nmod generated_dispatcher;\n";
    if fs::write(src_dir.join("lib.rs"), lib_rs).is_err() {
        return false;
    }
    if fs::write(src_dir.join("generated_client.rs"), format!("{common}\n\n{client}")).is_err() {
        return false;
    }
    if fs::write(src_dir.join("generated_dispatcher.rs"), format!("{common}\n\n{dispatcher}")).is_err() {
        return false;
    }

    let output = match Command::new("cargo")
        .arg("check")
        .arg("--message-format=short")
        .current_dir(&project)
        .output()
    {
        Ok(o) => o,
        Err(e) => {
            eprintln!("rust: cannot run cargo: {e}");
            return false;
        }
    };

    if output.status.success() {
        println!("  rust client + dispatcher : cargo check OK");
        true
    } else {
        eprintln!("rust: cargo check FAILED:\n{}", String::from_utf8_lossy(&output.stderr));
        false
    }
}

/// Write the generated C header plus a mock runtime header and a driver that
/// instantiates every generated type, then compile with gcc/clang/tcc when one
/// is available. If no C compiler exists, report "skipped".
fn check_c(tmp: &Path, c_header: &str) -> bool {
    let c_dir = tmp.join("c");
    if fs::create_dir_all(&c_dir).is_err() {
        return false;
    }
    if fs::write(c_dir.join("generated.h"), c_header).is_err() {
        return false;
    }

    // Mock runtime header: same declarations as tests/mock_aerosls_cap.h
    let mock = r#"#ifndef AEROSLS_CAP_H
#define AEROSLS_CAP_H
#include <stdint.h>
#include <stddef.h>
#define AEROSLS_CAP_PERM_R 0x01
#define AEROSLS_CAP_PERM_W 0x02
#define AEROSLS_CAP_PERM_X 0x04
#define AEROSLS_CAP_FLAG_BORROWED 0x00
#define AEROSLS_CAP_FLAG_ARENA 0x01
#define AEROSLS_CAP_FLAG_ARENA_OWNED 0x02
#define AEROSLS_CAP_NONE 0xFFFF
#define AEROSLS_CHAN_FLAG_NO_REPLY 0x0001
typedef struct { uint16_t slot; uint32_t offset; uint32_t len; uint8_t rights; uint8_t flags; } aerosls_cap_desc_t;
static inline int aerosls_chan_send(uint16_t w, uint32_t op, uint32_t id, const void* p, size_t n, const aerosls_cap_desc_t* c, uint16_t nc, uint32_t f) { (void)w;(void)op;(void)id;(void)p;(void)n;(void)c;(void)nc;(void)f; return 0; }
static inline int aerosls_chan_recv(uint16_t r, void* b, size_t n, uint16_t* cs, uint16_t mx, uint32_t* k, uint32_t* t, size_t* l, uint16_t* nc) { (void)r;(void)b;(void)n;(void)cs;(void)mx;(void)k;(void)t;(void)l;(void)nc; return 0; }
static inline uint16_t aerosls_arena_alloc(size_t bytes, uint8_t perms) { (void)bytes;(void)perms; return AEROSLS_CAP_NONE; }
static inline void aerosls_arena_free(uint16_t cap) { (void)cap; }
static uint32_t g_req_id_counter = 0;
static inline uint32_t aerosls_next_request_id(void) { return ++g_req_id_counter; }
#endif
"#;
    if fs::write(c_dir.join("aerosls_cap.h"), mock).is_err() {
        return false;
    }

    // Driver: include the header so all types + inline stubs are parsed.
    let driver = r#"#include "generated.h"
int main(void) {
    return 0;
}
"#;
    if fs::write(c_dir.join("driver.c"), driver).is_err() {
        return false;
    }

    // Find a C compiler.
    let cc = ["gcc", "clang", "cc", "tcc"]
        .iter()
        .find(|cc| {
            Command::new(cc)
                .arg("--version")
                .output()
                .map(|o| o.status.success())
                .unwrap_or(false)
        });

    let cc = match cc {
        Some(cc) => cc,
        None => {
            println!("  c header                : skipped (no C compiler found)");
            return true;
        }
    };

    let output = match Command::new(cc)
        .arg("-Wall")
        .arg("-Wextra")
        .arg("-std=c11")
        .arg("-I").arg(&c_dir)
        .arg(c_dir.join("driver.c"))
        .arg("-o").arg(c_dir.join("driver"))
        .output()
    {
        Ok(o) => o,
        Err(e) => {
            eprintln!("c: cannot run {cc}: {e}");
            return false;
        }
    };

    if output.status.success() {
        println!("  c header                : {cc} OK");
        true
    } else {
        eprintln!("c: {cc} FAILED:\n{}", String::from_utf8_lossy(&output.stderr));
        false
    }
}

/// Structural check for the Lisp output: balanced parentheses and the
/// presence of the dispatch entry point. (Full Lisp compilation would need
/// a running SBCL/CCL image, which we cannot assume.)
fn check_lisp(tmp: &Path, lisp: &str) -> bool {
    let _ = tmp;
    // Paren balance with Lisp-aware lexing: skip `;` comments and strings.
    let mut depth = 0i32;
    let mut in_string = false;
    let mut chars = lisp.chars().peekable();
    while let Some(ch) = chars.next() {
        if in_string {
            if ch == '\\' {
                let _ = chars.next(); // skip escaped char
            } else if ch == '\"' {
                in_string = false;
            }
            continue;
        }
        match ch {
            '\"' => in_string = true,
            ';' => {
                // Skip to end of line.
                for c in chars.by_ref() {
                    if c == '\n' {
                        break;
                    }
                }
            }
            '(' => depth += 1,
            ')' => depth -= 1,
            _ => {}
        }
    }
    if depth != 0 {
        eprintln!("lisp: unbalanced parens (depth {depth})");
        return false;
    }
    if !lisp.contains("(defun ") || !lisp.contains("(ecase opcode") {
        eprintln!("lisp: missing dispatch entry points");
        return false;
    }
    println!("  lisp                    : structural OK (balanced parens)");
    true
}
