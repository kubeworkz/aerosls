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
        process::exit(1);
    }

    let mut check_only = false;
    let mut input_path = None;
    let mut output_path = None;
    let mut target: Option<String> = None;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--check" => check_only = true,
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

    // Emit output
    match target.as_deref() {
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
        Some("dispatch") => {
            let ast_value = aeroidl_cc::emit::emit_value(&doc, &tc);
            let common = aeroidl_cc::gen_rust::emit_common_types();
            let dispatcher = aeroidl_cc::gen_dispatcher::emit_dispatcher(&ast_value);
            write_output(&output_path, &format!("{common}\n\n{dispatcher}"));
        }
        Some(other) => {
            eprintln!("error: unknown target '{other}' (supported: rust, lisp, dispatch, both)");
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
