//! AeroIDL compiler — parses `.aeroidl` interface definitions, validates
//! ownership annotations and payload sizes, and emits a typed AST as JSON
//! for downstream code generators (C, Common Lisp, Rust, etc.).
//!
//! Crate layout:
//!
//! ```text
//! lexer     — tokeniser (source → Token stream with spans)
//! ast       — typed AST node definitions
//! parser    — recursive-descent parser (Token stream → AST)
//! tycheck   — type checker (ownership validation, payload size, idempotency)
//! emit      — JSON serializer for the typed AST
//! ```

pub mod ast;
pub mod emit;
pub mod gen_dispatcher;
pub mod gen_lisp;
pub mod gen_rust;
pub mod lexer;
pub mod parser;
pub mod tycheck;
