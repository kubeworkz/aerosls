//! JSON emitter — serializes the typed, validated AST into a format that
//! downstream code generators (C, Lisp, Rust) can consume.
//!
//! The JSON output includes all computed metadata: inline sizes, opcode
//! assignments, ownership semantics, and payload byte counts.

use crate::ast::*;
use crate::tycheck::TypeCheckResult;
use serde::Serialize;

/// Top-level JSON output structure.
#[derive(Serialize)]
pub struct EmittedAst {
    pub version: String,
    pub namespace: String,
    pub type_sizes: std::collections::HashMap<String, u64>,
    pub enums: Vec<EmittedEnum>,
    pub structs: Vec<EmittedStruct>,
    pub interfaces: Vec<EmittedInterface>,
}

#[derive(Serialize)]
pub struct EmittedEnum {
    pub name: String,
    pub wire_size: u64,
    pub variants: Vec<EmittedVariant>,
}

#[derive(Serialize)]
pub struct EmittedVariant {
    pub name: String,
    pub discriminant: i64,
}

#[derive(Serialize)]
pub struct EmittedStruct {
    pub name: String,
    pub inline_size: u64,
    pub passed_as_cap: bool, // true if > 128 bytes → MEM cap on wire
    pub fields: Vec<EmittedField>,
}

#[derive(Serialize)]
pub struct EmittedField {
    pub name: String,
    pub ty: TypeJson,
    pub ownership: OwnershipJson,
    pub wire_size: u64,
}

#[derive(Serialize)]
pub struct EmittedInterface {
    pub name: String,
    pub methods: Vec<EmittedMethod>,
}

#[derive(Serialize)]
pub struct EmittedMethod {
    pub name: String,
    pub opcode: u16, // sequential opcode assignment
    pub is_async: bool,
    pub params: Vec<EmittedParam>,
    pub return_type: TypeJson,
    pub ownership: OwnershipJson,
    pub inline_payload_bytes: u64, // total bytes in the request+reply payload
    pub param_cap_count: u8,       // how many MEM cap descriptors are needed
}

#[derive(Serialize)]
pub struct EmittedParam {
    pub name: String,
    pub ty: TypeJson,
    pub ownership: OwnershipJson,
    pub wire_size: u64,
    pub needs_cap: bool, // true if this param requires a MEM cap descriptor
}

/// JSON representation of a type.
#[derive(Serialize)]
pub struct TypeJson {
    pub kind: String, // "i32", "f64", "string", "array", "named", "result", etc.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub inner: Option<Box<TypeJson>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub inner2: Option<Box<TypeJson>>, // for Result<T, E> and Map<K, V>
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,          // for Named types
}

/// JSON representation of ownership.
#[derive(Serialize, Clone, Copy)]
pub enum OwnershipJson {
    #[serde(rename = "inline")]
    Inline,
    #[serde(rename = "borrowed")]
    Borrowed,
    #[serde(rename = "owned")]
    Owned,
    #[serde(rename = "arena")]
    Arena,
}

// ── Public API ──────────────────────────────────────────────────────────────

/// Emit the typed AST as a JSON string.
pub fn emit_json(doc: &Document, tc: &TypeCheckResult) -> String {
    let emitted = build_emitted_ast(doc, tc);
    serde_json::to_string_pretty(&emitted).expect("JSON serialization should not fail")
}

/// Emit the typed AST as a serde_json::Value.
pub fn emit_value(doc: &Document, tc: &TypeCheckResult) -> serde_json::Value {
    let emitted = build_emitted_ast(doc, tc);
    serde_json::to_value(&emitted).expect("JSON serialization should not fail")
}

// ── Builder ─────────────────────────────────────────────────────────────────

fn build_emitted_ast(doc: &Document, tc: &TypeCheckResult) -> EmittedAst {
    let mut enums = Vec::new();
    let mut structs = Vec::new();
    let mut interfaces = Vec::new();
    let mut opcode: u16 = 1; // opcodes start at 1

    for decl in &doc.declarations {
        match decl {
            Decl::Enum(e) => {
                let wire_size = tc.type_sizes.get(&e.name).copied().unwrap_or(4);
                enums.push(EmittedEnum {
                    name: e.name.clone(),
                    wire_size,
                    variants: e
                        .variants
                        .iter()
                        .map(|v| EmittedVariant {
                            name: v.name.clone(),
                            discriminant: v.discriminant,
                        })
                        .collect(),
                });
            }
            Decl::Struct(s) => {
                let inline_size = tc.type_sizes.get(&s.name).copied().unwrap_or(0);
                let passed_as_cap = inline_size > 128;
                structs.push(EmittedStruct {
                    name: s.name.clone(),
                    inline_size,
                    passed_as_cap,
                    fields: s
                        .fields
                        .iter()
                        .map(|f| {
                            let ws = wire_size_of_field(&f.ty, tc);
                            EmittedField {
                                name: f.name.clone(),
                                ty: type_to_json(&f.ty),
                                ownership: ownership_to_json(&f.ownership),
                                wire_size: ws,
                            }
                        })
                        .collect(),
                });
            }
            Decl::Interface(iface) => {
                let mut methods = Vec::new();
                for m in &iface.methods {
                    let is_async =
                        m.annotations.iter().any(|a| matches!(a, Annotation::Async));

                    let param_wire: u64 = m
                        .params
                        .iter()
                        .map(|p| wire_size_of_field(&p.ty, tc))
                        .sum();
                    let reply_wire = wire_size_of_field(&m.return_type, tc);
                    let inline_payload = 8 + param_wire + reply_wire; // header + params + reply

                    let param_cap_count = m
                        .params
                        .iter()
                        .filter(|p| needs_cap(&p.ty, tc))
                        .count() as u8;

                    methods.push(EmittedMethod {
                        name: m.name.clone(),
                        opcode,
                        is_async,
                        params: m
                            .params
                            .iter()
                            .map(|p| {
                                let ws = wire_size_of_field(&p.ty, tc);
                                EmittedParam {
                                    name: p.name.clone(),
                                    ty: type_to_json(&p.ty),
                                    ownership: ownership_to_json(&p.ownership),
                                    wire_size: ws,
                                    needs_cap: needs_cap(&p.ty, tc),
                                }
                            })
                            .collect(),
                        return_type: type_to_json(&m.return_type),
                        ownership: ownership_to_json(
                            &m.params.first().map_or(Ownership::Inline, |p| p.ownership.clone()),
                        ),
                        inline_payload_bytes: inline_payload,
                        param_cap_count,
                    });
                    opcode += 1;
                }
                interfaces.push(EmittedInterface {
                    name: iface.name.clone(),
                    methods,
                });
            }
        }
    }

    EmittedAst {
        version: doc.version.clone(),
        namespace: doc.namespace.clone(),
        type_sizes: tc.type_sizes.clone(),
        enums,
        structs,
        interfaces,
    }
}

fn type_to_json(ty: &Ty) -> TypeJson {
    match ty {
        Ty::I8 => TypeJson { kind: "i8".into(), inner: None, inner2: None, name: None },
        Ty::I16 => TypeJson { kind: "i16".into(), inner: None, inner2: None, name: None },
        Ty::I32 => TypeJson { kind: "i32".into(), inner: None, inner2: None, name: None },
        Ty::I64 => TypeJson { kind: "i64".into(), inner: None, inner2: None, name: None },
        Ty::U8 => TypeJson { kind: "u8".into(), inner: None, inner2: None, name: None },
        Ty::U16 => TypeJson { kind: "u16".into(), inner: None, inner2: None, name: None },
        Ty::U32 => TypeJson { kind: "u32".into(), inner: None, inner2: None, name: None },
        Ty::U64 => TypeJson { kind: "u64".into(), inner: None, inner2: None, name: None },
        Ty::F32 => TypeJson { kind: "f32".into(), inner: None, inner2: None, name: None },
        Ty::F64 => TypeJson { kind: "f64".into(), inner: None, inner2: None, name: None },
        Ty::Bool => TypeJson { kind: "bool".into(), inner: None, inner2: None, name: None },
        Ty::String => TypeJson { kind: "string".into(), inner: None, inner2: None, name: None },
        Ty::Bytes => TypeJson { kind: "bytes".into(), inner: None, inner2: None, name: None },
        Ty::Unit => TypeJson { kind: "unit".into(), inner: None, inner2: None, name: None },
        Ty::Array(inner) => TypeJson {
            kind: "array".into(),
            inner: Some(Box::new(type_to_json(inner))),
            inner2: None,
            name: None,
        },
        Ty::Map(k, v) => TypeJson {
            kind: "map".into(),
            inner: Some(Box::new(type_to_json(k))),
            inner2: Some(Box::new(type_to_json(v))),
            name: None,
        },
        Ty::Named(n) => TypeJson {
            kind: "named".into(),
            inner: None,
            inner2: None,
            name: Some(n.clone()),
        },
        Ty::Option(inner) => TypeJson {
            kind: "option".into(),
            inner: Some(Box::new(type_to_json(inner))),
            inner2: None,
            name: None,
        },
        Ty::Result(ok, err) => TypeJson {
            kind: "result".into(),
            inner: Some(Box::new(type_to_json(ok))),
            inner2: Some(Box::new(type_to_json(err))),
            name: None,
        },
    }
}

fn ownership_to_json(own: &Ownership) -> OwnershipJson {
    match own {
        Ownership::Inline => OwnershipJson::Inline,
        Ownership::Borrowed => OwnershipJson::Borrowed,
        Ownership::Owned => OwnershipJson::Owned,
        Ownership::Arena => OwnershipJson::Arena,
    }
}

/// Compute wire size for the type.
fn wire_size_of_field(ty: &Ty, tc: &TypeCheckResult) -> u64 {
    match ty {
        Ty::I8 | Ty::U8 | Ty::Bool => 1,
        Ty::I16 | Ty::U16 => 2,
        Ty::I32 | Ty::U32 | Ty::F32 => 4,
        Ty::I64 | Ty::U64 | Ty::F64 => 8,
        Ty::String | Ty::Bytes => 8,
        Ty::Array(_) => 8,
        Ty::Map(_, _) => 8,
        Ty::Unit => 0,
        Ty::Named(name) => {
            let full = tc.type_sizes.get(name).copied().unwrap_or(8);
            if full <= 128 { full } else { 8 }
        }
        Ty::Option(inner) => 1 + wire_size_of_field(inner, tc),
        Ty::Result(ok, err) => {
            1 + std::cmp::max(wire_size_of_field(ok, tc), wire_size_of_field(err, tc))
        }
    }
}

/// Returns true if the type requires a MEM cap on the wire.
fn needs_cap(ty: &Ty, tc: &TypeCheckResult) -> bool {
    match ty {
        Ty::String | Ty::Bytes | Ty::Array(_) | Ty::Map(_, _) => true,
        Ty::Named(name) => {
            tc.type_sizes.get(name).copied().map_or(true, |sz| sz > 128)
        }
        Ty::Option(inner) => needs_cap(inner, tc),
        Ty::Result(ok, err) => needs_cap(ok, tc) || needs_cap(err, tc),
        _ => false,
    }
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{parser, tycheck};

    #[test]
    fn emit_calculator_json() {
        let calc = include_str!("../../../idl/calculator.aeroidl");
        let doc = parser::parse(calc).unwrap();
        let tc = tycheck::type_check(&doc);
        let json = emit_json(&doc, &tc);

        // Verify it's valid JSON
        let value: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(value["version"], "aeroidl/1.0");
        assert_eq!(value["namespace"], "aerosls.calculator");

        // Should have enums
        let enums = value["enums"].as_array().unwrap();
        assert!(!enums.is_empty());
        assert_eq!(enums[0]["name"], "CalcErrorKind");

        // Should have structs
        let structs = value["structs"].as_array().unwrap();
        assert!(!structs.is_empty());

        // Should have interfaces
        let interfaces = value["interfaces"].as_array().unwrap();
        assert_eq!(interfaces.len(), 1);
        assert_eq!(interfaces[0]["name"], "CalculatorService");

        let methods = interfaces[0]["methods"].as_array().unwrap();
        assert!(!methods.is_empty());

        // First method should be "add" with opcode 1
        assert_eq!(methods[0]["name"], "add");
        assert_eq!(methods[0]["opcode"], 1);
        assert_eq!(methods[0]["is_async"], false);

        // heavy_reduce should be async
        let heavy = methods.iter().find(|m| m["name"] == "heavy_reduce").unwrap();
        assert_eq!(heavy["is_async"], true);
    }

    #[test]
    fn emit_logging_json() {
        let log = include_str!("../../../idl/logging.aeroidl");
        let doc = parser::parse(log).unwrap();
        let tc = tycheck::type_check(&doc);
        let json = emit_json(&doc, &tc);
        let value: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(value["namespace"], "aerosls.logging");
    }

    #[test]
    fn emit_imageproc_json() {
        let img = include_str!("../../../idl/imageproc.aeroidl");
        let doc = parser::parse(img).unwrap();
        let tc = tycheck::type_check(&doc);
        let json = emit_json(&doc, &tc);
        let value: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(value["namespace"], "aerosls.imageproc");
    }
}
