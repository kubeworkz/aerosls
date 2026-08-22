//! Recursive-descent parser for AeroIDL.
//!
//! Produces a typed [`crate::ast::Document`] from a token stream.
//! Error recovery: the parser attempts to continue past errors by
//! skipping to the next semicolon or closing brace, collecting all
//! errors before returning.

use crate::ast::*;
use crate::lexer::{LexError, Lexer, Span, Spanned, Token};

// ── Public API ──────────────────────────────────────────────────────────────

/// Parse an AeroIDL source string into a typed AST.
///
/// Returns all collected errors if any are found.
pub fn parse(source: &str) -> Result<Document, Vec<ParseError>> {
    let mut lexer = Lexer::new(source);
    let tokens = lexer.tokenize_all().map_err(|e| vec![ParseError::Lex(e)])?;

    let mut parser = Parser::new(&tokens, source);
    let doc = parser.parse_document();

    if parser.errors.is_empty() {
        Ok(doc)
    } else {
        Err(parser.errors)
    }
}

// ── Error type ──────────────────────────────────────────────────────────────

#[derive(Debug, Clone, thiserror::Error)]
pub enum ParseError {
    #[error(transparent)]
    Lex(#[from] LexError),

    #[error("unexpected token {found} at byte {offset}, expected {expected}")]
    UnexpectedToken {
        found: String,
        expected: String,
        offset: usize,
    },

    #[error("unexpected end of file, expected {expected}")]
    UnexpectedEof { expected: String },

    #[error("{message} at byte offset {offset}")]
    Semantic { message: String, offset: usize },
}

// ── Parser state ────────────────────────────────────────────────────────────

struct Parser<'a> {
    tokens: &'a [Spanned],
    pos: usize,
    errors: Vec<ParseError>,
    src: &'a str,
}

impl<'a> Parser<'a> {
    fn new(tokens: &'a [Spanned], src: &'a str) -> Self {
        Self {
            tokens,
            pos: 0,
            errors: Vec::new(),
            src,
        }
    }

    // ── Token stream helpers ────────────────────────────────────────────

    fn peek(&self) -> &Token {
        self.tokens
            .get(self.pos)
            .map(|s| &s.tok)
            .unwrap_or(&Token::Eof)
    }

    fn peek_span(&self) -> Span {
        self.tokens
            .get(self.pos)
            .map(|s| s.span)
            .unwrap_or(Span::new(self.src.len(), self.src.len()))
    }

    fn advance(&mut self) -> &Spanned {
        let sp = self.tokens.get(self.pos).unwrap();
        if self.pos < self.tokens.len() - 1 {
            self.pos += 1;
        }
        sp
    }

    fn expect(&mut self, expected: &str) -> Result<&Spanned, ParseError> {
        match self.peek() {
            Token::Eof => Err(ParseError::UnexpectedEof {
                expected: expected.to_string(),
            }),
            _ => {
                let sp = self.advance();
                let found = sp.tok.to_string();
                let tok_str = expected;
                // Validate it matches
                let matches = match tok_str {
                    "{" => matches!(&sp.tok, Token::LBrace),
                    "}" => matches!(&sp.tok, Token::RBrace),
                    "(" => matches!(&sp.tok, Token::LParen),
                    ")" => matches!(&sp.tok, Token::RParen),
                    "<" => matches!(&sp.tok, Token::LAngle),
                    ">" => matches!(&sp.tok, Token::RAngle),
                    "[" => matches!(&sp.tok, Token::LBracket),
                    "]" => matches!(&sp.tok, Token::RBracket),
                    "," => matches!(&sp.tok, Token::Comma),
                    ";" => matches!(&sp.tok, Token::Semi),
                    ":" => matches!(&sp.tok, Token::Colon),
                    "=" => matches!(&sp.tok, Token::Eq),
                    "->" => matches!(&sp.tok, Token::Arrow),
                    _ => false,
                };
                if !matches {
                    Err(ParseError::UnexpectedToken {
                        found,
                        expected: expected.to_string(),
                        offset: sp.span.lo,
                    })
                } else {
                    Ok(sp)
                }
            }
        }
    }

    fn expect_ident(&mut self) -> Result<(String, Span), ParseError> {
        match self.peek() {
            Token::Ident(s) => {
                let s = s.clone();
                let span = self.peek_span();
                self.advance();
                Ok((s, span))
            }
            Token::Eof => Err(ParseError::UnexpectedEof {
                expected: "identifier".into(),
            }),
            other => Err(ParseError::UnexpectedToken {
                found: other.to_string(),
                expected: "identifier".into(),
                offset: self.peek_span().lo,
            }),
        }
    }

    fn expect_string(&mut self) -> Result<(String, Span), ParseError> {
        match self.peek().clone() {
            Token::StringLit(s) => {
                let span = self.peek_span();
                self.advance();
                Ok((s, span))
            }
            Token::Eof => Err(ParseError::UnexpectedEof {
                expected: "string".into(),
            }),
            other => Err(ParseError::UnexpectedToken {
                found: other.to_string(),
                expected: "string literal".into(),
                offset: self.peek_span().lo,
            }),
        }
    }

    fn expect_integer(&mut self) -> Result<(i64, Span), ParseError> {
        match self.peek().clone() {
            Token::Integer(n) => {
                let span = self.peek_span();
                self.advance();
                Ok((n, span))
            }
            Token::Eof => Err(ParseError::UnexpectedEof {
                expected: "integer".into(),
            }),
            other => Err(ParseError::UnexpectedToken {
                found: other.to_string(),
                expected: "integer".into(),
                offset: self.peek_span().lo,
            }),
        }
    }

    fn expect_arrow(&mut self) -> Result<Span, ParseError> {
        match self.peek() {
            Token::Arrow => {
                let span = self.peek_span();
                self.advance();
                Ok(span)
            }
            Token::Eof => Err(ParseError::UnexpectedEof {
                expected: "->".into(),
            }),
            other => Err(ParseError::UnexpectedToken {
                found: other.to_string(),
                expected: "->".into(),
                offset: self.peek_span().lo,
            }),
        }
    }

    fn at_end(&self) -> bool {
        matches!(self.peek(), Token::Eof)
    }

    /// Consume the current token if it matches, returning true if consumed.
    fn consume_if<F>(&mut self, pred: F) -> bool
    where
        F: FnOnce(&Token) -> bool,
    {
        if pred(self.peek()) {
            self.advance();
            true
        } else {
            false
        }
    }

    /// Consume an optional semicolon (for headers).
    fn consume_optional_semi(&mut self) {
        self.consume_if(|t| matches!(t, Token::Semi));
    }

    fn error(&mut self, msg: String, offset: usize) -> ParseError {
        let e = ParseError::Semantic { message: msg, offset };
        self.errors.push(e.clone());
        e
    }

    fn recover_to_semi(&mut self) {
        while !matches!(self.peek(), Token::Semi | Token::Eof) {
            self.advance();
        }
        if matches!(self.peek(), Token::Semi) {
            self.advance();
        }
    }


    // ── Document ────────────────────────────────────────────────────────

    fn parse_document(&mut self) -> Document {
        let doc_start = self.peek_span().lo;

        let mut version = String::new();
        let mut namespace = String::new();
        let mut declarations = Vec::new();

        // Parse headers
        while matches!(self.peek(), Token::At) {
            self.advance();
            let (name, _) = self.expect_ident().unwrap_or_else(|e| {
                self.errors.push(e);
                ("unknown".into(), Span::new(0, 0))
            });

            match name.as_str() {
                "version" => {
                    self.consume_if(|t| matches!(t, Token::LParen));
                    if let Ok((v, _)) = self.expect_string() {
                        version = v;
                    }
                    self.consume_if(|t| matches!(t, Token::RParen));
                    self.consume_optional_semi();
                }
                "namespace" => {
                    self.consume_if(|t| matches!(t, Token::LParen));
                    if let Ok((ns, _)) = self.expect_string() {
                        namespace = ns;
                    }
                    self.consume_if(|t| matches!(t, Token::RParen));
                    self.consume_optional_semi();
                }
                other => {
                    self.error(
                        format!("unknown header @{other}"),
                        self.peek_span().lo,
                    );
                    self.recover_to_semi();
                }
            }
        }

        // Parse declarations
        while !self.at_end() {
            match self.peek() {
                Token::Ident(_) => {
                    // Could be a type alias or a declaration keyword
                    // For now, we expect: enum, struct, interface
                    match self.peek() {
                        Token::Ident(ref kw) if kw == "enum" => {
                            if let Some(decl) = self.parse_enum_decl() {
                                declarations.push(Decl::Enum(decl));
                            }
                        }
                        Token::Ident(ref kw) if kw == "struct" => {
                            if let Some(decl) = self.parse_struct_decl() {
                                declarations.push(Decl::Struct(decl));
                            }
                        }
                        Token::Ident(ref kw) if kw == "interface" => {
                            if let Some(decl) = self.parse_interface_decl() {
                                declarations.push(Decl::Interface(decl));
                            }
                        }
                        _ => {
                            self.error(
                                format!("expected declaration keyword (enum/struct/interface), found '{}'", self.peek()),
                                self.peek_span().lo,
                            );
                            self.recover_to_semi();
                        }
                    }
                }
                _ => {
                    self.error(
                        format!("expected declaration, found '{}'", self.peek()),
                        self.peek_span().lo,
                    );
                    self.recover_to_semi();
                }
            }
        }

        Document {
            version,
            namespace,
            declarations,
            span: Span::new(doc_start, self.peek_span().lo),
        }
    }

    // ── Enum ────────────────────────────────────────────────────────────

    fn parse_enum_decl(&mut self) -> Option<EnumDecl> {
        let start = self.peek_span().lo;
        self.advance(); // consume "enum"
        let (name, _) = self.expect_ident().ok()?;
        self.expect("{").ok()?;

        let mut variants = Vec::new();
        while !matches!(self.peek(), Token::RBrace | Token::Eof) {
            let (var_name, var_span) = self.expect_ident().ok()?;
            self.expect("=").ok();
            let (disc, _) = self.expect_integer().ok()?;
            variants.push(EnumVariant {
                name: var_name,
                discriminant: disc,
                span: var_span,
            });
            if matches!(self.peek(), Token::Comma) {
                self.advance();
            }
        }
        self.expect("}").ok()?;

        Some(EnumDecl {
            name,
            variants,
            span: Span::new(start, self.peek_span().lo),
        })
    }

    // ── Struct ──────────────────────────────────────────────────────────

    fn parse_struct_decl(&mut self) -> Option<StructDecl> {
        let start = self.peek_span().lo;
        self.advance(); // consume "struct"
        let (name, _) = self.expect_ident().ok()?;
        self.expect("{").ok()?;

        let mut fields = Vec::new();
        while !matches!(self.peek(), Token::RBrace | Token::Eof) {
            let (field_name, field_span) = self.expect_ident().ok()?;
            self.expect(":").ok();
            let ty = self.parse_type().ok()?;
            let ownership = self.parse_ownership();
            fields.push(Field {
                name: field_name,
                ty,
                ownership,
                span: field_span,
            });
            if matches!(self.peek(), Token::Comma) {
                self.advance();
            }
        }
        self.expect("}").ok()?;

        Some(StructDecl {
            name,
            fields,
            inline_size: None, // computed by type checker
            span: Span::new(start, self.peek_span().lo),
        })
    }

    // ── Interface ───────────────────────────────────────────────────────

    fn parse_interface_decl(&mut self) -> Option<InterfaceDecl> {
        let start = self.peek_span().lo;
        self.advance(); // consume "interface"
        let (name, _) = self.expect_ident().ok()?;
        self.expect("{").ok()?;

        let mut methods = Vec::new();
        while !matches!(self.peek(), Token::RBrace | Token::Eof) {
            if let Some(method) = self.parse_method() {
                methods.push(method);
            } else {
                // Safety: advance past the token that caused parse_method to fail
                // to prevent an infinite loop.
                self.advance();
            }
        }
        self.expect("}").ok()?;

        Some(InterfaceDecl {
            name,
            methods,
            span: Span::new(start, self.peek_span().lo),
        })
    }

    fn parse_method(&mut self) -> Option<Method> {
        let start = self.peek_span().lo;

        // Parse annotations
        let annotations = self.parse_annotations();

        let (name, _) = self.expect_ident().ok()?;
        self.expect("(").ok()?;

        // Parse parameters
        let mut params = Vec::new();
        if !matches!(self.peek(), Token::RParen) {
            loop {
                let (param_name, param_span) = self.expect_ident().ok()?;
                self.expect(":").ok();
                let ty = self.parse_type().ok()?;
                let ownership = self.parse_ownership();
                params.push(Param {
                    name: param_name,
                    ty,
                    ownership,
                    span: param_span,
                });
                if matches!(self.peek(), Token::Comma) {
                    self.advance();
                } else {
                    break;
                }
            }
        }
        self.expect(")").ok()?;
        self.expect_arrow().ok()?;
        let return_type = self.parse_type().ok()?;
        self.parse_ownership(); // consume optional ownership on return type
        self.consume_optional_semi(); // semicolons are optional after method declarations

        Some(Method {
            name,
            params,
            return_type,
            annotations,
            span: Span::new(start, self.peek_span().lo),
            max_payload_bytes: None, // computed by type checker
        })
    }

    // ── Annotations ─────────────────────────────────────────────────────

    fn parse_annotations(&mut self) -> Vec<Annotation> {
        let mut anns = Vec::new();
        loop {
            match self.peek() {
                Token::AtAsync => {
                    self.advance();
                    anns.push(Annotation::Async);
                }
                Token::At => {
                    self.advance();
                    match self.peek().clone() {
                        Token::Ident(name) => {
                            self.advance();
                            let mut args = Vec::new();
                            if matches!(self.peek(), Token::LParen) {
                                self.advance();
                                while !matches!(self.peek(), Token::RParen | Token::Eof) {
                                    if let Ok((arg, _)) = self.expect_string() {
                                        args.push(arg);
                                    }
                                    if matches!(self.peek(), Token::Comma) {
                                        self.advance();
                                    }
                                }
                                self.consume_if(|t| matches!(t, Token::RParen));
                            }
                            match name.as_str() {
                                "async" => anns.push(Annotation::Async),
                                "deprecated" => {
                                    let msg = args.first().cloned().unwrap_or_default();
                                    anns.push(Annotation::Deprecated(msg));
                                }
                                "max_payload" => {
                                    if let Some(n) = args.first().and_then(|s| s.parse::<u64>().ok()) {
                                        anns.push(Annotation::MaxPayload(n));
                                    }
                                }
                                _ => {
                                    self.errors.push(ParseError::Semantic {
                                        message: format!("unknown annotation @{name}"),
                                        offset: self.peek_span().lo,
                                    });
                                }
                            }
                        }
                        _ => {
                            self.errors.push(ParseError::Semantic {
                                message: "expected annotation name after '@'".into(),
                                offset: self.peek_span().lo,
                            });
                            break;
                        }
                    }
                }
                _ => break,
            }
        }
        anns
    }

    // ── Ownership ───────────────────────────────────────────────────────

    fn parse_ownership(&mut self) -> Ownership {
        if matches!(self.peek(), Token::At) {
            self.advance();
            match self.peek() {
                Token::Ident(ref name) => {
                    let name = name.clone();
                    self.advance();
                    match name.as_str() {
                        "borrowed" => Ownership::Borrowed,
                        "owned" => Ownership::Owned,
                        "arena" => Ownership::Arena,
                        _ => {
                            self.errors.push(ParseError::Semantic {
                                message: format!("unknown ownership @{name}"),
                                offset: self.peek_span().lo,
                            });
                            Ownership::Inline
                        }
                    }
                }
                _ => Ownership::Inline,
            }
        } else {
            Ownership::Inline
        }
    }

    // ── Types ───────────────────────────────────────────────────────────

    fn parse_type(&mut self) -> Result<Ty, ParseError> {
        let ty = match self.peek().clone() {
            // Unit type: ()
            Token::LParen if self.pos + 1 < self.tokens.len() && matches!(&self.tokens[self.pos + 1].tok, Token::RParen) => {
                self.advance(); // (
                self.advance(); // )
                Ty::Unit
            }
            Token::Ident(ref name) => {
                let name = name.clone();
                self.advance();
                match name.as_str() {
                    "i8" => Ty::I8,
                    "i16" => Ty::I16,
                    "i32" => Ty::I32,
                    "i64" => Ty::I64,
                    "u8" => Ty::U8,
                    "u16" => Ty::U16,
                    "u32" => Ty::U32,
                    "u64" => Ty::U64,
                    "f32" => Ty::F32,
                    "f64" => Ty::F64,
                    "bool" => Ty::Bool,
                    "string" => Ty::String,
                    "bytes" => Ty::Bytes,
                    "Option" => {
                        self.expect("<")?;
                        let inner = self.parse_type()?;
                        self.parse_ownership(); // consume optional ownership inside generic
                        self.expect(">")?;
                        Ty::Option(Box::new(inner))
                    }
                    "Result" => {
                        self.expect("<")?;
                        let ok = self.parse_type()?;
                        self.parse_ownership(); // consume optional ownership inside generic
                        self.expect(",")?;
                        let err = self.parse_type()?;
                        self.parse_ownership(); // consume optional ownership inside generic
                        self.expect(">")?;
                        Ty::Result(Box::new(ok), Box::new(err))
                    }
                    "map" => {
                        self.expect("<")?;
                        let key = self.parse_type()?;
                        self.parse_ownership(); // consume optional ownership inside generic
                        self.expect(",")?;
                        let val = self.parse_type()?;
                        self.parse_ownership(); // consume optional ownership inside generic
                        self.expect(">")?;
                        Ty::Map(Box::new(key), Box::new(val))
                    }
                    _ => Ty::Named(name),
                }
            }
            _ => {
                return Err(ParseError::UnexpectedToken {
                    found: self.peek().to_string(),
                    expected: "type".into(),
                    offset: self.peek_span().lo,
                });
            }
        };

        // Check for array suffix: Type[]
        if matches!(self.peek(), Token::LBracket) {
            // Peek ahead to see if it's followed by ]
            if self.pos + 1 < self.tokens.len() && matches!(&self.tokens[self.pos + 1].tok, Token::RBracket) {
                self.advance(); // [
                self.advance(); // ]
                return Ok(Ty::Array(Box::new(ty)));
            }
        }

        Ok(ty)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_ok(src: &str) -> Document {
        parse(src).unwrap_or_else(|errs| {
            for e in &errs {
                eprintln!("parse error: {e}");
            }
            panic!("parse failed with {} errors", errs.len());
        })
    }

    #[test]
    fn minimal_document() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test.v1")
        "#);
        assert_eq!(doc.version, "aeroidl/1.0");
        assert_eq!(doc.namespace, "test.v1");
        assert!(doc.declarations.is_empty());
    }

    #[test]
    fn enum_declaration() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            enum LogLevel {
                TRACE = 0,
                DEBUG = 1,
                INFO  = 2,
            }
        "#);
        assert_eq!(doc.declarations.len(), 1);
        match &doc.declarations[0] {
            Decl::Enum(e) => {
                assert_eq!(e.name, "LogLevel");
                assert_eq!(e.variants.len(), 3);
                assert_eq!(e.variants[0].name, "TRACE");
                assert_eq!(e.variants[0].discriminant, 0);
                assert_eq!(e.variants[2].name, "INFO");
                assert_eq!(e.variants[2].discriminant, 2);
            }
            _ => panic!("expected enum"),
        }
    }

    #[test]
    fn struct_declaration() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            struct Vec2 {
                x: f64,
                y: f64,
            }
        "#);
        match &doc.declarations[0] {
            Decl::Struct(s) => {
                assert_eq!(s.name, "Vec2");
                assert_eq!(s.fields.len(), 2);
                assert_eq!(s.fields[0].ty, Ty::F64);
                assert_eq!(s.fields[1].ty, Ty::F64);
            }
            _ => panic!("expected struct"),
        }
    }

    #[test]
    fn struct_with_ownership() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            struct LogRecord {
                message: string @borrowed,
                data: bytes @arena,
            }
        "#);
        match &doc.declarations[0] {
            Decl::Struct(s) => {
                assert_eq!(s.fields[0].ownership, Ownership::Borrowed);
                assert_eq!(s.fields[1].ownership, Ownership::Arena);
            }
            _ => panic!("expected struct"),
        }
    }

    #[test]
    fn interface_declaration() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            interface CalculatorService {
                add(a: i32, b: i32) -> Result<i32, string>;
                @async heavy_reduce(input: f64[] @arena) -> Result<u32, string>;
            }
        "#);
        match &doc.declarations[0] {
            Decl::Interface(iface) => {
                assert_eq!(iface.name, "CalculatorService");
                assert_eq!(iface.methods.len(), 2);

                let m0 = &iface.methods[0];
                assert_eq!(m0.name, "add");
                assert_eq!(m0.params.len(), 2);
                assert_eq!(m0.params[0].ty, Ty::I32);
                assert_eq!(m0.return_type, Ty::Result(Box::new(Ty::I32), Box::new(Ty::String)));
                assert!(m0.annotations.is_empty());

                let m1 = &iface.methods[1];
                assert_eq!(m1.name, "heavy_reduce");
                assert!(m1.annotations.contains(&Annotation::Async));
                assert_eq!(m1.params[0].ty, Ty::Array(Box::new(Ty::F64)));
                assert_eq!(m1.params[0].ownership, Ownership::Arena);
            }
            _ => panic!("expected interface"),
        }
    }

    #[test]
    fn complex_types() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            interface Svc {
                a(x: Option<i32>) -> Result<(), string>;
                b(x: map<string, u32>) -> string;
            }
        "#);
        match &doc.declarations[0] {
            Decl::Interface(iface) => {
                let m0 = &iface.methods[0];
                assert_eq!(m0.params[0].ty, Ty::Option(Box::new(Ty::I32)));
                assert_eq!(m0.return_type, Ty::Result(Box::new(Ty::Unit), Box::new(Ty::String)));

                let m1 = &iface.methods[1];
                assert_eq!(m1.params[0].ty, Ty::Map(Box::new(Ty::String), Box::new(Ty::U32)));
                assert_eq!(m1.return_type, Ty::String);
            }
            _ => panic!("expected interface"),
        }
    }

    #[test]
    fn comments_are_ignored() {
        let doc = parse_ok(r#"
            // This is a comment
            @version("aeroidl/1.0") // inline comment
            @namespace("test")
            // Multi-line
            // comment
            enum X { A = 0 }
        "#);
        assert_eq!(doc.version, "aeroidl/1.0");
        assert_eq!(doc.declarations.len(), 1);
    }

    #[test]
    fn named_type_reference() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            struct Vec2 { x: f64, y: f64 }
            interface Svc {
                dot(a: Vec2 @borrowed) -> f64;
            }
        "#);
        match &doc.declarations[1] {
            Decl::Interface(iface) => {
                assert_eq!(iface.methods[0].params[0].ty, Ty::Named("Vec2".into()));
            }
            _ => panic!("expected interface"),
        }
    }

    #[test]
    fn struct_with_array_field() {
        let doc = parse_ok(r#"
            @version("aeroidl/1.0")
            @namespace("test")
            struct MatrixData {
                rows: u32,
                cols: u32,
                elements: f64[] @arena,
            }
        "#);
        match &doc.declarations[0] {
            Decl::Struct(s) => {
                assert_eq!(s.fields[2].ty, Ty::Array(Box::new(Ty::F64)));
                assert_eq!(s.fields[2].ownership, Ownership::Arena);
            }
            _ => panic!("expected struct"),
        }
    }

    #[test]
    fn parse_all_examples() {
        // calculator.aeroidl
        let calc = include_str!("../../../idl/calculator.aeroidl");
        let doc = parse_ok(calc);
        assert_eq!(doc.version, "aeroidl/1.0");
        assert_eq!(doc.namespace, "aerosls.calculator");
        assert_eq!(doc.declarations.len(), 5); // CalcErrorKind, CalcError, Vec2, MatrixData, CalculatorService

        // logging.aeroidl
        let log = include_str!("../../../idl/logging.aeroidl");
        let doc = parse_ok(log);
        assert_eq!(doc.namespace, "aerosls.logging");
        assert!(!doc.declarations.is_empty());

        // imageproc.aeroidl
        let img = include_str!("../../../idl/imageproc.aeroidl");
        let doc = parse_ok(img);
        assert_eq!(doc.namespace, "aerosls.imageproc");
        assert!(!doc.declarations.is_empty());
    }
}
