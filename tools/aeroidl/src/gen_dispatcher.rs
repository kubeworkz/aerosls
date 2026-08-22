//! Rust dispatcher-side code generator — reads the typed AST JSON and emits a
//! complete Rust module with a channel event loop, opcode dispatch, payload
//! deserialization, and a trait that the user implements.
//!
//! The generated code provides the "server" side of an AeroIDL interface:
//! it sits in a channel receive loop, unpacks incoming `SEND_CAP` messages,
//! calls the appropriate user-implemented method, and sends back a
//! `RECV_CAP` reply with the serialized return value.
//!
//! Usage:
//! ```text
//! aeroidl <input.aeroidl> --target dispatch        # emit to stdout
//! aeroidl <input.aeroidl> --target dispatch -o out.rs
//! ```

use serde_json::Value;

// ── Public API ──────────────────────────────────────────────────────────────

/// Emit a complete Rust dispatcher module from the typed AST JSON.
pub fn emit_dispatcher(ast: &Value) -> String {
    let mut out = String::with_capacity(8192);

    let namespace = ast["namespace"].as_str().unwrap_or("unknown");
    let version = ast["version"].as_str().unwrap_or("unknown");

    out.push_str(&format!(
        "// Auto-generated Rust dispatcher — {namespace}\n"
    ));
    out.push_str(&format!("// AeroIDL version: {version}\n"));
    out.push_str("//\n");
    out.push_str("// This module provides the server-side dispatch loop for the sidecar.\n");
    out.push_str("// The sidecar runtime provides:\n");
    out.push_str("//   chan_send(), chan_recv(), arena_alloc(), arena_free(), arena_mem()\n");
    out.push_str("//\n");
    out.push_str("// Generated from the typed AST — do not edit manually.\n");
    out.push('\n');
    out.push_str("use core::mem;\n\n");

    // ── Enums ────────────────────────────────────────────────────────────
    if let Some(enums) = ast["enums"].as_array() {
        for e in enums {
            emit_enum(&mut out, e);
        }
    }

    // ── Structs ──────────────────────────────────────────────────────────
    if let Some(structs) = ast["structs"].as_array() {
        for s in structs {
            emit_struct(&mut out, s);
        }
    }

    // ── Interfaces ────────────────────────────────────────────────────────
    if let Some(interfaces) = ast["interfaces"].as_array() {
        for iface in interfaces {
            emit_interface(&mut out, ast, iface);
        }
    }

    out
}

// ── Enum generation ─────────────────────────────────────────────────────────

fn emit_enum(out: &mut String, e: &Value) {
    let name = e["name"].as_str().unwrap();
    let variants = e["variants"].as_array().unwrap();

    out.push_str(&format!(
        "/// {name} — AeroIDL enum (wire size: {} bytes)\n",
        e["wire_size"]
    ));
    out.push_str("#[repr(u32)]\n");
    out.push_str("#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\n");
    out.push_str(&format!("pub enum {name} {{\n"));
    for v in variants {
        let vname = v["name"].as_str().unwrap();
        let disc = v["discriminant"].as_i64().unwrap();
        out.push_str(&format!("    {vname} = {disc},\n"));
    }
    out.push_str("}\n\n");

    out.push_str(&format!("impl {name} {{\n"));
    out.push_str("    pub fn from_raw(v: u32) -> Option<Self> {\n");
    out.push_str("        match v {\n");
    for v in variants {
        let vname = v["name"].as_str().unwrap();
        let disc = v["discriminant"].as_i64().unwrap();
        out.push_str(&format!("            {disc} => Some({name}::{vname}),\n"));
    }
    out.push_str("            _ => None,\n");
    out.push_str("        }\n");
    out.push_str("    }\n\n");
    out.push_str("    pub fn to_raw(self) -> u32 {\n");
    out.push_str("        self as u32\n");
    out.push_str("    }\n");
    out.push_str("}\n\n");
}

// ── Struct generation ───────────────────────────────────────────────────────

fn emit_struct(out: &mut String, s: &Value) {
    let name = s["name"].as_str().unwrap();
    let inline_size = s["inline_size"].as_u64().unwrap_or(0);
    let passed_as_cap = s["passed_as_cap"].as_bool().unwrap_or(false);
    let fields = s["fields"].as_array().unwrap();

    out.push_str(&format!(
        "/// {name} — AeroIDL struct (wire size: {inline_size} bytes{}",
        if passed_as_cap { ", passed as MEM cap" } else { "" }
    ));
    out.push_str(")\n");
    out.push_str("#[repr(C)]\n");
    // Only derive Copy if no arena/array fields (arena data isn't Copy)
    let has_arena_field = fields.iter().any(|f| {
        let needs_cap = f["needs_cap"].as_bool().unwrap_or(false);
        let kind = f["ty"]["kind"].as_str().unwrap_or("");
        needs_cap || kind == "array" || kind == "string" || kind == "bytes"
    });
    if has_arena_field {
        out.push_str("#[derive(Clone, Debug)]\n");
    } else {
        out.push_str("#[derive(Clone, Copy, Debug)]\n");
    }
    out.push_str(&format!("pub struct {name} {{\n"));
    for f in fields {
        let fname = f["name"].as_str().unwrap();
        let rty = json_type_to_rust(&f["ty"]);
        let comment = ownership_comment(&f["ownership"]);
        out.push_str(&format!("    pub {fname}: {rty},{comment}\n"));
    }
    out.push_str("}\n\n");

    out.push_str(&format!("impl {name} {{\n"));
    out.push_str(&format!(
        "    pub const WIRE_SIZE: usize = {inline_size};\n"
    ));
    out.push_str("}\n\n");
}

// ── Interface: trait + dispatch ─────────────────────────────────────────────

fn emit_interface(out: &mut String, ast: &Value, iface: &Value) {
    let name = iface["name"].as_str().unwrap();
    let methods = iface["methods"].as_array().unwrap();
    let mod_name = to_snake_case(name);

    out.push_str(&format!("/// {name} — AeroIDL interface (dispatcher side)\n"));
    out.push_str(&format!("pub mod {mod_name} {{\n"));
    out.push_str("    use super::*;\n\n");

    // Opcode constants
    out.push_str("    /// Message opcodes for channel send/recv.\n");
    for m in methods {
        let mname = m["name"].as_str().unwrap();
        let opcode = m["opcode"].as_u64().unwrap();
        let const_name = format!("OP_{}", mname.to_uppercase());
        out.push_str(&format!(
            "    pub const {const_name}: u16 = {opcode};\n"
        ));
    }
    out.push('\n');

    // User trait
    emit_trait(out, methods, name);

    // Dispatch function
    emit_dispatch_fn(out, ast, methods, name);

    out.push_str("}\n\n");
}

// ── Trait generation ────────────────────────────────────────────────────────

fn emit_trait(out: &mut String, methods: &[Value], iface_name: &str) {
    let trait_name = format!("{iface_name}Impl");

    out.push_str(&format!(
        "    /// User-implemented trait for the {iface_name} interface.\n"
    ));
    out.push_str(&format!(
        "    /// Implement this trait on your service struct to handle incoming calls.\n"
    ));
    out.push_str(&format!("    pub trait {trait_name} {{\n"));

    for m in methods {
        let mname = m["name"].as_str().unwrap();
        let params = m["params"].as_array().unwrap();
        let return_type = &m["return_type"];
        let is_async = m["is_async"].as_bool().unwrap_or(false);
        let doc = m.get("doc").and_then(|d| d.as_str()).unwrap_or("");

        let mut sig_params = Vec::new();
        for p in params {
            let pname = p["name"].as_str().unwrap();
            if p["needs_cap"].as_bool().unwrap_or(false) {
                sig_params.push(format!("{pname}_cap: u16"));
                sig_params.push(format!("{pname}_data: *const u8"));
                sig_params.push(format!("{pname}_len: u32"));
            } else {
                let rty = json_type_to_rust(&p["ty"]);
                sig_params.push(format!("{pname}: {rty}"));
            }
        }

        let return_rty = json_type_to_rust(return_type);

        if is_async {
            out.push_str(&format!(
                "        /// `{mname}` (async) — {doc}\n"
            ));
            out.push_str(&format!(
                "        fn {mname}(&mut self, {}) -> {return_rty};\n",
                sig_params.join(", ")
            ));
        } else {
            out.push_str(&format!(
                "        /// `{mname}` — {doc}\n"
            ));
            out.push_str(&format!(
                "        fn {mname}(&mut self, {}) -> AeroidlResult<{return_rty}>;\n",
                sig_params.join(", ")
            ));
        }
    }

    out.push_str("    }\n\n");
}

// ── Dispatch function ───────────────────────────────────────────────────────

fn emit_dispatch_fn(
    out: &mut String,
    ast: &Value,
    methods: &[Value],
    iface_name: &str,
) {
    let trait_name = format!("{iface_name}Impl");
    let mod_name = to_snake_case(iface_name);

    out.push_str(&format!(
        "    /// Main dispatch entry point — call this from the sidecar event loop.\n"
    ));
    out.push_str(&format!(
        "    /// Receives messages from `chan_r`, deserializes the opcode and\n"
    ));
    out.push_str(&format!(
        "    /// arguments, calls the appropriate trait method on `service`, and\n"
    ));
    out.push_str(&format!(
        "    /// sends the serialized reply back on `chan_w`.\n"
    ));
    out.push_str(&format!(
        "    pub fn dispatch<S: {trait_name}>(\n"
    ));
    out.push_str("        service: &mut S,\n");
    out.push_str("        chan_r: u16,\n");
    out.push_str("        chan_w: u16,\n");
    out.push_str("    ) -> Result<(), AEROIDL_ERROR> {\n");
    out.push_str("        let mut recv_buf = [0u8; 4096];\n");
    out.push_str("        let mut reply_buf = [0u8; 4096];\n");
    out.push_str("        let mut recv_caps = [0u16; 16];\n");
    out.push_str("        let mut n_recv_caps: u16 = 0;\n\n");

    out.push_str("        loop {\n");
    out.push_str("            n_recv_caps = 0;\n\n");

    // Receive
    out.push_str("            // ── Receive incoming message ──\n");
    out.push_str("            unsafe {\n");
    out.push_str("                let rc = chan_recv(\n");
    out.push_str("                    chan_r,\n");
    out.push_str("                    recv_buf.as_mut_ptr(),\n");
    out.push_str("                    recv_buf.len() as u32,\n");
    out.push_str("                    recv_caps.as_mut_ptr(),\n");
    out.push_str("                    &mut n_recv_caps as *mut u16,\n");
    out.push_str("                );\n");
    out.push_str("                if rc != 0 {\n");
    out.push_str("                    return Err(AEROIDL_ERROR {\n");
    out.push_str("                        code: rc as u32,\n");
    out.push_str(
        "                        message: alloc::string::String::from(\"chan_recv failed\"),\n",
    );
    out.push_str("                    });\n");
    out.push_str("                }\n");
    out.push_str("            }\n\n");

    // Parse header
    out.push_str("            // ── Parse header: [opcode: u16 LE, payload_len: u32 LE] ──\n");
    out.push_str("            if recv_buf.len() < 6 {\n");
    out.push_str("                continue;\n");
    out.push_str("            }\n");
    out.push_str(
        "            let opcode = u16::from_le_bytes(recv_buf[0..2].try_into().unwrap());\n",
    );
    out.push_str(
        "            let payload_len = u32::from_le_bytes(recv_buf[2..6].try_into().unwrap()) as usize;\n",
    );
    out.push_str("            let payload = &recv_buf[6..6 + payload_len];\n\n");

    // Dispatch by opcode
    out.push_str("            // ── Dispatch by opcode ──\n");
    out.push_str("            let (reply_len, n_reply_caps) = match opcode {\n");

    for m in methods {
        let mname = m["name"].as_str().unwrap();
        let const_name = format!("OP_{}", mname.to_uppercase());
        let params = m["params"].as_array().unwrap();
        let return_type = &m["return_type"];
        let is_async = m["is_async"].as_bool().unwrap_or(false);

        out.push_str(&format!("                {const_name} => {{\n"));

        // Deserialize params
        emit_deserialize_params(out, ast, params, "                    ");

        // Build call args
        let call_args: Vec<String> = params
            .iter()
            .map(|p| {
                let pname = p["name"].as_str().unwrap();
                if p["needs_cap"].as_bool().unwrap_or(false) {
                    format!("{pname}_cap, {pname}_data, {pname}_len")
                } else {
                    pname.to_string()
                }
            })
            .collect();

        if is_async {
            out.push_str(&format!(
                "                    service.{}({});\n",
                mname,
                call_args.join(", ")
            ));
            out.push_str("                    reply_buf[0..4].copy_from_slice(&1u32.to_le_bytes());\n");
            out.push_str("                    (4, 0u16)\n");
        } else {
            let return_rty = json_type_to_rust(return_type);
            out.push_str(&format!(
                "                    let result: AeroidlResult<{return_rty}> = service.{}({});\n",
                mname,
                call_args.join(", ")
            ));

            // Serialize result — handle Result<T, E> dispatch inline
            emit_serialize_result_inline(
                out,
                ast,
                return_type,
                "                    ",
                &format!("{mod_name}"),
            );
        }

        out.push_str("                }\n");
    }

    // Unknown opcode
    out.push_str("                _ => {\n");
    out.push_str("                    reply_buf[0..4].copy_from_slice(&0u32.to_le_bytes());\n");
    out.push_str(
        "                    reply_buf[4..8].copy_from_slice(&0xFFFFu32.to_le_bytes());\n",
    );
    out.push_str("                    (8, 0u16)\n");
    out.push_str("                }\n");

    out.push_str("            };\n\n");

    // Send reply
    out.push_str("            // ── Send reply ──\n");
    out.push_str("            unsafe {\n");
    out.push_str("                let rc = chan_send(\n");
    out.push_str("                    chan_w,\n");
    out.push_str("                    0, // reply opcode (convention: 0 = response)\n");
    out.push_str("                    reply_buf.as_ptr(),\n");
    out.push_str("                    reply_len as u32,\n");
    out.push_str("                    core::ptr::null(),\n");
    out.push_str("                    n_reply_caps as u32,\n");
    out.push_str("                    0, // flags\n");
    out.push_str("                );\n");
    out.push_str("                if rc != 0 {\n");
    out.push_str("                    return Err(AEROIDL_ERROR {\n");
    out.push_str("                        code: rc as u32,\n");
    out.push_str(
        "                        message: alloc::string::String::from(\"chan_send failed\"),\n",
    );
    out.push_str("                    });\n");
    out.push_str("                }\n");
    out.push_str("            }\n");
    out.push_str("        }\n");
    out.push_str("    }\n\n");
}

// ── Parameter deserialization ───────────────────────────────────────────────

fn emit_deserialize_params(out: &mut String, ast: &Value, params: &[Value], indent: &str) {
    let mut offset = 0usize;
    let mut cap_index = 0usize;

    for p in params {
        let pname = p["name"].as_str().unwrap();
        let needs_cap = p["needs_cap"].as_bool().unwrap_or(false);

        if needs_cap {
            let kind = p["ty"]["kind"].as_str().unwrap_or("array");
            out.push_str(&format!(
                "{indent}let {pname}_cap = recv_caps[{cap_index}];\n"
            ));
            if kind == "array" || kind == "bytes" {
                out.push_str(&format!(
                    "{indent}let {pname}_len = u32::from_le_bytes(\n"
                ));
                out.push_str(&format!(
                    "{indent}    payload[{offset}..{offset}+4].try_into().unwrap()\n"
                ));
                out.push_str(&format!("{indent});\n"));
                offset += 4;
            } else {
                out.push_str(&format!("{indent}let {pname}_len = 0u32;\n"));
            }
            out.push_str(&format!(
                "{indent}let {pname}_data = unsafe {{ arena_mem({pname}_cap) }};\n"
            ));
            cap_index += 1;
        } else {
            let ty = &p["ty"];
            let kind = ty["kind"].as_str().unwrap_or("i32");
            let wire_size = p["wire_size"].as_u64().unwrap_or(0) as usize;

            match kind {
                "i8" | "u8" | "bool" => {
                    let rust_type = if kind == "bool" { "bool" } else { kind };
                    out.push_str(&format!(
                        "{indent}let {pname} = payload[{offset}] as {rust_type};\n"
                    ));
                    offset += 1;
                }
                "i16" | "u16" => {
                    out.push_str(&format!(
                        "{indent}let {pname} = {kind}::from_le_bytes(\n"
                    ));
                    out.push_str(&format!(
                        "{indent}    payload[{offset}..{offset}+2].try_into().unwrap()\n"
                    ));
                    out.push_str(&format!("{indent});\n"));
                    offset += 2;
                }
                "i32" | "u32" => {
                    out.push_str(&format!(
                        "{indent}let {pname} = {kind}::from_le_bytes(\n"
                    ));
                    out.push_str(&format!(
                        "{indent}    payload[{offset}..{offset}+4].try_into().unwrap()\n"
                    ));
                    out.push_str(&format!("{indent});\n"));
                    offset += 4;
                }
                "i64" | "u64" => {
                    out.push_str(&format!(
                        "{indent}let {pname} = {kind}::from_le_bytes(\n"
                    ));
                    out.push_str(&format!(
                        "{indent}    payload[{offset}..{offset}+8].try_into().unwrap()\n"
                    ));
                    out.push_str(&format!("{indent});\n"));
                    offset += 8;
                }
                "f32" => {
                    out.push_str(&format!(
                        "{indent}let {pname} = f32::from_bits(u32::from_le_bytes(\n"
                    ));
                    out.push_str(&format!(
                        "{indent}    payload[{offset}..{offset}+4].try_into().unwrap()\n"
                    ));
                    out.push_str(&format!("{indent}));\n"));
                    offset += 4;
                }
                "f64" => {
                    out.push_str(&format!(
                        "{indent}let {pname} = f64::from_bits(u64::from_le_bytes(\n"
                    ));
                    out.push_str(&format!(
                        "{indent}    payload[{offset}..{offset}+8].try_into().unwrap()\n"
                    ));
                    out.push_str(&format!("{indent}));\n"));
                    offset += 8;
                }
                "named" => {
                    let type_name = ty["name"].as_str().unwrap_or("Unknown");
                    let struct_def = find_struct(ast, type_name);
                    if let Some(s) = struct_def {
                        let fields = s["fields"].as_array().unwrap();
                        out.push_str(&format!("{indent}let {pname} = {type_name} {{\n"));
                        for f in fields {
                            let fname = f["name"].as_str().unwrap();
                            let fkind = f["ty"]["kind"].as_str().unwrap_or("i32");
                            let fwire = f["wire_size"].as_u64().unwrap_or(0) as usize;
                            match fkind {
                                "i32" | "u32" => {
                                    out.push_str(&format!(
                                        "{indent}    {fname}: u32::from_le_bytes(payload[{offset}..{offset}+4].try_into().unwrap()) as _,\n"
                                    ));
                                }
                                "i64" | "u64" => {
                                    out.push_str(&format!(
                                        "{indent}    {fname}: u64::from_le_bytes(payload[{offset}..{offset}+8].try_into().unwrap()) as _,\n"
                                    ));
                                }
                                "f32" => {
                                    out.push_str(&format!(
                                        "{indent}    {fname}: f32::from_bits(u32::from_le_bytes(payload[{offset}..{offset}+4].try_into().unwrap())),\n"
                                    ));
                                }
                                "f64" => {
                                    out.push_str(&format!(
                                        "{indent}    {fname}: f64::from_bits(u64::from_le_bytes(payload[{offset}..{offset}+8].try_into().unwrap())),\n"
                                    ));
                                }
                                "named" => {
                                    let nested_name = f["ty"]["name"].as_str().unwrap_or("_");
                                    out.push_str(&format!(
                                        "{indent}    {fname}: {nested_name}::default(), // TODO: deserialize nested\n"
                                    ));
                                }
                                "array" | "bytes" | "map" => {
                                    out.push_str(&format!(
                                        "{indent}    {fname}: ArenaSlice::new(0, 0), // arena field\n"
                                    ));
                                }
                                _ => {
                                    out.push_str(&format!(
                                        "{indent}    {fname}: Default::default(),\n"
                                    ));
                                }
                            }
                            offset += fwire;
                        }
                        out.push_str(&format!("{indent}}};\n"));
                    } else {
                        out.push_str(&format!(
                            "{indent}let {pname}: {type_name} = Default::default();\n"
                        ));
                    }
                }
                _ => {
                    out.push_str(&format!(
                        "{indent}let {pname}: {kind} = Default::default(); // TODO: deserialize {kind}\n"
                    ));
                    offset += wire_size;
                }
            }
        }
    }
}

// ── Result serialization (inline in dispatch arm) ───────────────────────────
//
// Generates the `match result { Ok(val) => { ... }, Err(e) => { ... } }` block
// that writes into `reply_buf` and returns `(reply_len, n_reply_caps)`.
// The generated code uses early-`return` from the match arms to assign
// reply_len and n_reply_caps.

fn emit_serialize_result_inline(
    out: &mut String,
    ast: &Value,
    return_type: &Value,
    indent: &str,
    _mod_name: &str,
) {
    let kind = return_type["kind"].as_str().unwrap_or("unit");

    match kind {
        "unit" => {
            out.push_str(&format!("{indent}match result {{\n"));
            out.push_str(&format!("{indent}    Ok(()) => {{\n"));
            out.push_str(&format!(
                "{indent}        reply_buf[0] = 1;\n"
            ));
            out.push_str(&format!("{indent}        (4, 0u16)\n"));
            out.push_str(&format!("{indent}    }}\n"));
            out.push_str(&format!("{indent}    Err(e) => {{\n"));
            emit_error_serialization(out, indent, "        ");
            out.push_str(&format!("{indent}    }}\n"));
            out.push_str(&format!("{indent}}}\n"));
        }
        "result" => {
            let ok_type = return_type.get("inner").unwrap();
            let ok_kind = ok_type["kind"].as_str().unwrap_or("unit");

            out.push_str(&format!("{indent}match result {{\n"));
            out.push_str(&format!("{indent}    Ok(val) => {{\n"));
            out.push_str(&format!(
                "{indent}        reply_buf[0] = 1;\n"
            ));

            match ok_kind {
                "unit" => {
                    out.push_str(&format!("{indent}        (4, 0u16)\n"));
                }
                "i32" | "u32" => {
                    out.push_str(&format!(
                        "{indent}        reply_buf[4..8].copy_from_slice(&(val as u32).to_le_bytes());\n"
                    ));
                    out.push_str(&format!("{indent}        (8, 0u16)\n"));
                }
                "i64" | "u64" => {
                    out.push_str(&format!(
                        "{indent}        reply_buf[8..16].copy_from_slice(&val.to_le_bytes());\n"
                    ));
                    out.push_str(&format!("{indent}        (16, 0u16)\n"));
                }
                "f64" => {
                    out.push_str(&format!(
                        "{indent}        reply_buf[8..16].copy_from_slice(&val.to_le_bytes());\n"
                    ));
                    out.push_str(&format!("{indent}        (16, 0u16)\n"));
                }
                "named" => {
                    let type_name = ok_type["name"].as_str().unwrap_or("Unknown");
                    let fields = get_struct_field_names(ast, type_name);
                    if !fields.is_empty() {
                        let mut inner_offset = 8;
                        for f in &fields {
                            let fname = f["name"].as_str().unwrap_or("_");
                            let fkind = f["ty"]["kind"].as_str().unwrap_or("i32");
                            let fwire = get_wire_size(&f["ty"]) as usize;
                            match fkind {
                                "i32" | "u32" | "f32" => {
                                    out.push_str(&format!(
                                        "{indent}        reply_buf[{inner_offset}..{inner_offset}+4].copy_from_slice(&(val.{fname} as u32).to_le_bytes());\n"
                                    ));
                                }
                                "i64" | "u64" | "f64" => {
                                    out.push_str(&format!(
                                        "{indent}        reply_buf[{inner_offset}..{inner_offset}+8].copy_from_slice(&(val.{fname} as u64).to_le_bytes());\n"
                                    ));
                                }
                                _ => {
                                    out.push_str(&format!(
                                        "{indent}        // TODO: serialize {fname} ({fkind})\n"
                                    ));
                                }
                            }
                            inner_offset += fwire;
                        }
                        out.push_str(&format!(
                            "{indent}        ({inner_offset}, 0u16)\n"
                        ));
                    } else {
                        out.push_str(&format!("{indent}        (4, 0u16)\n"));
                    }
                }
                "array" => {
                    out.push_str(&format!(
                        "{indent}        reply_buf[4..8].copy_from_slice(&val.len().to_le_bytes());\n"
                    ));
                    out.push_str(&format!(
                        "{indent}        reply_buf[8..10].copy_from_slice(&val.cap_handle().to_le_bytes());\n"
                    ));
                    out.push_str(&format!("{indent}        (10, 0u16)\n"));
                }
                _ => {
                    out.push_str(&format!("{indent}        (4, 0u16)\n"));
                }
            }

            out.push_str(&format!("{indent}    }}\n"));
            out.push_str(&format!("{indent}    Err(e) => {{\n"));
            emit_error_serialization(out, indent, "        ");
            out.push_str(&format!("{indent}    }}\n"));
            out.push_str(&format!("{indent}}}\n"));
        }
        _ => {
            out.push_str(&format!("{indent}match result {{\n"));
            out.push_str(&format!("{indent}    Ok(val) => {{\n"));
            out.push_str(&format!(
                "{indent}        reply_buf[0] = 1;\n"
            ));
            out.push_str(&format!("{indent}        (4, 0u16)\n"));
            out.push_str(&format!("{indent}    }}\n"));
            out.push_str(&format!("{indent}    Err(e) => {{\n"));
            emit_error_serialization(out, indent, "        ");
            out.push_str(&format!("{indent}    }}\n"));
            out.push_str(&format!("{indent}}}\n"));
        }
    }
}

/// Emit error serialization code inside a `match` arm.
/// Assumes `e` is an `AEROIDL_ERROR` and `reply_buf` is in scope.
fn emit_error_serialization(out: &mut String, indent: &str, inner_indent: &str) {
    out.push_str(&format!(
        "{indent}{inner_indent}reply_buf[0] = 0;\n"
    ));
    out.push_str(&format!(
        "{indent}{inner_indent}reply_buf[4..8].copy_from_slice(&e.code.to_le_bytes());\n"
    ));
    out.push_str(&format!(
        "{indent}{inner_indent}let msg_bytes = e.message.as_bytes();\n"
    ));
    out.push_str(&format!(
        "{indent}{inner_indent}let copy_len = core::cmp::min(msg_bytes.len(), reply_buf.len() - 8);\n"
    ));
    out.push_str(&format!(
        "{indent}{inner_indent}reply_buf[8..8 + copy_len].copy_from_slice(&msg_bytes[..copy_len]);\n"
    ));
    out.push_str(&format!(
        "{indent}{inner_indent}(8 + copy_len, 0u16)\n"
    ));
}

// ── Helper functions ────────────────────────────────────────────────────────

fn find_struct<'a>(ast: &'a Value, name: &str) -> Option<&'a Value> {
    ast["structs"]
        .as_array()?
        .iter()
        .find(|s| s["name"].as_str() == Some(name))
}

fn get_struct_field_names(ast: &Value, type_name: &str) -> Vec<Value> {
    find_struct(ast, type_name)
        .and_then(|s| s["fields"].as_array())
        .cloned()
        .unwrap_or_default()
}

fn get_wire_size(ty: &Value) -> u64 {
    let kind = ty["kind"].as_str().unwrap_or("i32");
    match kind {
        "i8" | "u8" | "bool" => 1,
        "i16" | "u16" => 2,
        "i32" | "u32" | "f32" => 4,
        "i64" | "u64" | "f64" => 8,
        "string" | "bytes" | "array" | "map" => 8,
        _ => 8,
    }
}

fn json_type_to_rust(ty: &Value) -> String {
    let kind = ty["kind"].as_str().unwrap_or("i32");
    match kind {
        "i8" => "i8".into(),
        "u8" => "u8".into(),
        "i16" => "i16".into(),
        "u16" => "u16".into(),
        "i32" => "i32".into(),
        "u32" => "u32".into(),
        "i64" => "i64".into(),
        "u64" => "u64".into(),
        "f32" => "f32".into(),
        "f64" => "f64".into(),
        "bool" => "bool".into(),
        "string" | "bytes" => "alloc::string::String".into(),
        "named" => {
            let name = ty["name"].as_str().unwrap_or("Unknown");
            name.to_string()
        }
        "array" => {
            let inner = ty
                .get("inner")
                .map(|i| json_type_to_rust(i))
                .unwrap_or_else(|| "u8".into());
            format!("ArenaSlice<{inner}>")
        }
        "option" => {
            let inner = ty
                .get("inner")
                .map(|i| json_type_to_rust(i))
                .unwrap_or_else(|| "()".into());
            format!("Option<{inner}>")
        }
        "result" => {
            let inner = ty
                .get("inner")
                .map(|i| json_type_to_rust(i))
                .unwrap_or_else(|| "()".into());
            inner
        }
        "unit" => "()".into(),
        "map" => "alloc::collections::BTreeMap<alloc::string::String, alloc::string::String>".into(),
        _ => kind.to_string(),
    }
}

fn ownership_comment(ownership: &Value) -> String {
    let own = ownership.as_str().unwrap_or("inline");
    match own {
        "borrowed" => "  // @borrowed".into(),
        "owned" => "  // @owned".into(),
        "arena" => "  // @arena".into(),
        _ => String::new(),
    }
}

fn to_snake_case(s: &str) -> String {
    let mut result = String::with_capacity(s.len() + 4);
    for (i, ch) in s.chars().enumerate() {
        if ch == '_' {
            result.push('_');
        } else if ch.is_uppercase() {
            if i > 0 {
                let prev = s.as_bytes()[i - 1] as char;
                if prev != '_' && prev.is_lowercase() {
                    result.push('_');
                }
            }
            result.push(ch.to_ascii_lowercase());
        } else {
            result.push(ch);
        }
    }
    result
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn emit_calculator_dispatch() {
        let src = include_str!("../../../idl/calculator.aeroidl");
        let doc = crate::parser::parse(src).expect("parse should succeed");
        let tc = crate::tycheck::type_check(&doc);
        let ast = crate::emit::emit_value(&doc, &tc);
        let code = emit_dispatcher(&ast);

        assert!(code.contains("pub trait CalculatorServiceImpl"));
        assert!(code.contains("fn add(&mut self, a: i32, b: i32)"));
        assert!(code.contains("fn div(&mut self, a: i64, b: i64)"));
        assert!(code.contains("fn dot(&mut self,"));
        assert!(code.contains("fn sqrt_batch(&mut self,"));
        assert!(code.contains("fn heavy_reduce(&mut self,"));
        assert!(code.contains("pub fn dispatch<S: CalculatorServiceImpl>"));
        assert!(code.contains("OP_ADD: u16 = 1"));
        assert!(code.contains("match opcode {"));
        assert!(code.contains("pub enum CalcErrorKind"));
        assert!(code.contains("pub struct Vec2"));
        assert!(code.contains("pub struct MatrixData"));
        // Verify balanced braces
        let opens = code.matches('{').count();
        let closes = code.matches('}').count();
        assert_eq!(opens, closes, "unbalanced braces: {opens} opens vs {closes} closes");
    }

    #[test]
    fn emit_logging_dispatch() {
        let src = include_str!("../../../idl/logging.aeroidl");
        let doc = crate::parser::parse(src).expect("parse should succeed");
        let tc = crate::tycheck::type_check(&doc);
        let ast = crate::emit::emit_value(&doc, &tc);
        let code = emit_dispatcher(&ast);

        assert!(code.contains("pub trait LoggingServiceImpl"));
        assert!(code.contains("fn emit(&mut self,"));
        assert!(code.contains("fn flush(&mut self,"));
        assert!(code.contains("fn set_level(&mut self,"));
        assert!(code.contains("pub fn dispatch<S: LoggingServiceImpl>"));
        assert!(code.contains("OP_EMIT: u16 = 1"));
        let opens = code.matches('{').count();
        let closes = code.matches('}').count();
        assert_eq!(opens, closes, "unbalanced braces");
    }

    #[test]
    fn emit_imageproc_dispatch() {
        let src = include_str!("../../../idl/imageproc.aeroidl");
        let doc = crate::parser::parse(src).expect("parse should succeed");
        let tc = crate::tycheck::type_check(&doc);
        let ast = crate::emit::emit_value(&doc, &tc);
        let code = emit_dispatcher(&ast);

        assert!(code.contains("pub trait ImageProcessingServiceImpl"));
        assert!(code.contains("fn blur(&mut self,"));
        assert!(code.contains("fn resize_inplace(&mut self,"));
        assert!(code.contains("fn histogram(&mut self,"));
        assert!(code.contains("fn composite(&mut self,"));
        assert!(code.contains("pub fn dispatch<S: ImageProcessingServiceImpl>"));
        assert!(code.contains("OP_BLUR: u16 = 1"));
        let opens = code.matches('{').count();
        let closes = code.matches('}').count();
        assert_eq!(opens, closes, "unbalanced braces");
    }

    #[test]
    fn dispatch_snake_case() {
        assert_eq!(to_snake_case("CalculatorService"), "calculator_service");
        assert_eq!(
            to_snake_case("ImageProcessingService"),
            "image_processing_service"
        );
        assert_eq!(to_snake_case("Vec2"), "vec2");
        assert_eq!(to_snake_case("LogLevel"), "log_level");
    }

    #[test]
    fn dispatch_has_trait_method_signatures() {
        let src = include_str!("../../../idl/calculator.aeroidl");
        let doc = crate::parser::parse(src).expect("parse should succeed");
        let tc = crate::tycheck::type_check(&doc);
        let ast = crate::emit::emit_value(&doc, &tc);
        let code = emit_dispatcher(&ast);

        assert!(code.contains("-> AeroidlResult<i32>"));
        assert!(code.contains("-> AeroidlResult<i64>"));
        assert!(code.contains("-> AeroidlResult<f64>"));
        assert!(code.contains("-> AeroidlResult<Vec2>"));
        assert!(code.contains("-> AeroidlResult<ArenaSlice<f64>>"));
    }
}
