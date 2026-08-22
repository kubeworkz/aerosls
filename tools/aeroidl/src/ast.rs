//! Typed AST — the output of the parser and input to the type checker.
//!
//! Every node carries a [`Span`] for error reporting. The AST is fully
//! resolved: all type names are validated, all ownership annotations are
//! parsed, and all sizes are computed.

use serde::Serialize;

pub use crate::lexer::Span;

/// A named field within a struct.
#[derive(Clone, Debug, Serialize)]
pub struct Field {
    pub name: String,
    pub ty: Ty,
    pub ownership: Ownership,
    pub span: Span,
}

// ── Root ────────────────────────────────────────────────────────────────────

/// A complete `.aeroidl` file.
#[derive(Clone, Debug, Serialize)]
pub struct Document {
    pub version: String,
    pub namespace: String,
    pub declarations: Vec<Decl>,
    pub span: Span,
}

/// Top-level declaration.
#[derive(Clone, Debug, Serialize)]
pub enum Decl {
    Enum(EnumDecl),
    Struct(StructDecl),
    Interface(InterfaceDecl),
}

// ── Enum ────────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize)]
pub struct EnumDecl {
    pub name: String,
    pub variants: Vec<EnumVariant>,
    pub span: Span,
}

#[derive(Clone, Debug, Serialize)]
pub struct EnumVariant {
    pub name: String,
    pub discriminant: i64,
    pub span: Span,
}

// ── Struct ──────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize)]
pub struct StructDecl {
    pub name: String,
    pub fields: Vec<Field>,
    pub span: Span,
    /// Computed: total inline size in bytes (None for variable-size structs).
    pub inline_size: Option<u64>,
}

// ── Interface ───────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize)]
pub struct InterfaceDecl {
    pub name: String,
    pub methods: Vec<Method>,
    pub span: Span,
}

#[derive(Clone, Debug, Serialize)]
pub struct Method {
    pub name: String,
    pub params: Vec<Param>,
    pub return_type: Ty,
    pub annotations: Vec<Annotation>,
    pub span: Span,
    /// Computed: maximum inline payload size in bytes for request + reply.
    pub max_payload_bytes: Option<u64>,
}

#[derive(Clone, Debug, Serialize)]
pub struct Param {
    pub name: String,
    pub ty: Ty,
    pub ownership: Ownership,
    pub span: Span,
}

// ── Annotations ─────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub enum Annotation {
    /// `@async` — fire-and-forget, result on dedicated channel.
    Async,
    /// `@deprecated` — emit warning in generated code.
    Deprecated(String),
    /// `@max_payload(n)` — override the default 4096 byte limit.
    MaxPayload(u64),
}

// ── Types ───────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub enum Ty {
    // Primitives
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    Bool,

    // Dynamic-size types
    String,
    Bytes,

    // Compound
    Array(Box<Ty>),
    Map(Box<Ty>, Box<Ty>),

    // Named (resolved to an enum or struct by the type checker)
    Named(String),

    // Built-in generics
    Option(Box<Ty>),
    Result(Box<Ty>, Box<Ty>),

    // Unit (for void returns)
    Unit,
}

impl Ty {
    /// Returns true if this type is a fixed-size primitive or small struct
    /// that can fit inline in a channel payload.
    pub fn is_fixed_size(&self) -> bool {
        matches!(
            self,
            Ty::I8 | Ty::I16 | Ty::I32 | Ty::I64
            | Ty::U8 | Ty::U16 | Ty::U32 | Ty::U64
            | Ty::F32 | Ty::F64
            | Ty::Bool
            | Ty::Unit
        )
    }

    /// Returns true if this type always requires a MEM cap (arena data).
    pub fn is_arena_type(&self) -> bool {
        matches!(self, Ty::String | Ty::Bytes | Ty::Array(_))
    }
}

// ── Ownership ───────────────────────────────────────────────────────────────

/// Memory ownership annotation on a parameter or field.
#[derive(Clone, Debug, Serialize, PartialEq, Eq, Default)]
pub enum Ownership {
    /// No annotation — default for primitives (in-register).
    #[default]
    Inline,
    /// `@borrowed` — caller retains ownership; callee reads only.
    Borrowed,
    /// `@owned` — callee takes ownership and must free.
    Owned,
    /// `@arena` — lives in shared arena; lifetime via refcount.
    Arena,
}

// ── Computed layout helpers ─────────────────────────────────────────────────

/// Fixed byte size for primitive types. Returns `None` for variable-size types.
pub fn primitive_size(ty: &Ty) -> Option<u64> {
    match ty {
        Ty::I8 | Ty::U8 | Ty::Bool => Some(1),
        Ty::I16 | Ty::U16 => Some(2),
        Ty::I32 | Ty::U32 | Ty::F32 => Some(4),
        Ty::I64 | Ty::U64 | Ty::F64 => Some(8),
        Ty::String | Ty::Bytes => Some(8), // cap handle (u16) + len (u32) = 8 on wire
        Ty::Array(_) => Some(8),           // cap handle (u16) + count (u32) = 8 on wire
        Ty::Map(_, _) => None,             // variable
        Ty::Named(_) => None,              // resolved by type checker
        Ty::Option(inner) => {
            // 1 byte discriminator + inner size (or 0 for None)
            primitive_size(inner).map(|s| 1 + s)
        }
        Ty::Result(_, _) => None, // 1 byte ok + error size — computed after resolution
        Ty::Unit => Some(0),
    }
}

/// Inline byte size for the wire representation of a parameter.
/// - Primitives and small structs (≤ 128 bytes): inline.
/// - Strings, bytes, arrays: always 8 bytes (cap handle).
/// - Large structs: 8 bytes (cap handle to arena allocation).
pub fn wire_size(ty: &Ty, struct_sizes: &std::collections::HashMap<String, u64>) -> Option<u64> {
    match ty {
        Ty::I8 | Ty::U8 | Ty::Bool => Some(1),
        Ty::I16 | Ty::U16 => Some(2),
        Ty::I32 | Ty::U32 | Ty::F32 => Some(4),
        Ty::I64 | Ty::U64 | Ty::F64 => Some(8),
        Ty::String | Ty::Bytes => Some(8),
        Ty::Array(_) => Some(8),
        Ty::Map(_, _) => None,
        Ty::Unit => Some(0),
        Ty::Named(name) => {
            let full_size = struct_sizes.get(name)?;
            if *full_size <= 128 {
                Some(*full_size)
            } else {
                Some(8) // cap handle
            }
        }
        Ty::Option(inner) => {
            wire_size(inner, struct_sizes).map(|s| 1 + s)
        }
        Ty::Result(ok, err) => {
            let ok_size = wire_size(ok, struct_sizes).unwrap_or(0);
            let err_size = wire_size(err, struct_sizes).unwrap_or(0);
            Some(1 + std::cmp::max(ok_size, err_size))
        }
    }
}


