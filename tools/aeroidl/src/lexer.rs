//! Tokeniser — converts `.aeroidl` source text into a stream of tokens,
//! each carrying its byte offset span for precise error reporting.

use std::fmt;

/// Byte-range span in source text (inclusive start, exclusive end).
#[derive(Clone, Copy, Debug, PartialEq, Eq, serde::Serialize)]
pub struct Span {
    pub lo: usize,
    pub hi: usize,
}

impl Span {
    pub fn new(lo: usize, hi: usize) -> Self {
        Self { lo, hi }
    }

    pub fn len(self) -> usize {
        self.hi - self.lo
    }
}

/// Every token kind the lexer can produce.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Token {
    // ── Structural ──────────────────────────────────────────────────────
    LBrace,       // {
    RBrace,       // }
    LParen,       // (
    RParen,       // )
    LAngle,       // <  (also part of @< for generics)
    RAngle,       // >
    LBracket,     // [
    RBracket,     // ]
    Comma,        // ,
    Semi,         // ;
    Colon,        // :
    Eq,           // =
    Arrow,        // ->

    // ── Literals ────────────────────────────────────────────────────────
    Integer(i64),
    StringLit(String),

    // ── Identifiers and keywords ────────────────────────────────────────
    Ident(String),

    // ── Annotations ─────────────────────────────────────────────────────
    At,           // @
    AtAsync,      // @async (recognized as a single token for convenience)

    // ── Special ─────────────────────────────────────────────────────────
    Eof,
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Token::LBrace => write!(f, "{{"),
            Token::RBrace => write!(f, "}}"),
            Token::LParen => write!(f, "("),
            Token::RParen => write!(f, ")"),
            Token::LAngle => write!(f, "<"),
            Token::RAngle => write!(f, ">"),
            Token::LBracket => write!(f, "["),
            Token::RBracket => write!(f, "]"),
            Token::Comma => write!(f, ","),
            Token::Semi => write!(f, ";"),
            Token::Colon => write!(f, ":"),
            Token::Eq => write!(f, "="),
            Token::Arrow => write!(f, "->"),
            Token::Integer(n) => write!(f, "{n}"),
            Token::StringLit(s) => write!(f, "\"{s}\""),
            Token::Ident(s) => write!(f, "{s}"),
            Token::At => write!(f, "@"),
            Token::AtAsync => write!(f, "@async"),
            Token::Eof => write!(f, "<EOF>"),
        }
    }
}

/// A token paired with its source span.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Spanned {
    pub tok: Token,
    pub span: Span,
}

// ── Lexer ───────────────────────────────────────────────────────────────────

/// Lexer state machine.
pub struct Lexer<'a> {
    src: &'a str,
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Lexer<'a> {
    pub fn new(src: &'a str) -> Self {
        Self {
            src,
            bytes: src.as_bytes(),
            pos: 0,
        }
    }

    /// Tokenise the entire source and return a Vec of spanned tokens.
    pub fn tokenize_all(&mut self) -> Result<Vec<Spanned>, LexError> {
        let mut tokens = Vec::new();
        loop {
            let tok = self.next_token()?;
            let at_eof = tok.tok == Token::Eof;
            tokens.push(tok);
            if at_eof {
                break;
            }
        }
        Ok(tokens)
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn peek2(&self) -> Option<(u8, u8)> {
        if self.pos + 1 < self.bytes.len() {
            Some((self.bytes[self.pos], self.bytes[self.pos + 1]))
        } else {
            None
        }
    }

    fn advance(&mut self) -> Option<u8> {
        let b = self.bytes.get(self.pos).copied();
        if b.is_some() {
            self.pos += 1;
        }
        b
    }

    fn skip_whitespace_and_comments(&mut self) {
        while let Some(b) = self.peek() {
            match b {
                b' ' | b'\t' | b'\r' | b'\n' => {
                    self.advance();
                }
                b'/' if self.peek2().map_or(false, |(_, c)| c == b'/') => {
                    // Line comment
                    while self.peek().map_or(false, |c| c != b'\n') {
                        self.advance();
                    }
                }
                _ => break,
            }
        }
    }

    fn next_token(&mut self) -> Result<Spanned, LexError> {
        self.skip_whitespace_and_comments();

        let start = self.pos;
        let Some(b) = self.peek() else {
            return Ok(Spanned {
                tok: Token::Eof,
                span: Span::new(start, start),
            });
        };

        let tok = match b {
            b'{' => { self.advance(); Token::LBrace }
            b'}' => { self.advance(); Token::RBrace }
            b'(' => { self.advance(); Token::LParen }
            b')' => { self.advance(); Token::RParen }
            b'<' => { self.advance(); Token::LAngle }
            b'>' => { self.advance(); Token::RAngle }
            b'[' => { self.advance(); Token::LBracket }
            b']' => { self.advance(); Token::RBracket }
            b',' => { self.advance(); Token::Comma }
            b';' => { self.advance(); Token::Semi }
            b':' => { self.advance(); Token::Colon }
            b'=' => { self.advance(); Token::Eq }
            b'-' if self.peek2().map_or(false, |(_, c)| c == b'>') => {
                self.advance(); self.advance();
                Token::Arrow
            }
            b'@' => {
                self.advance();
                // Check for @async
                if self.bytes[self.pos..].starts_with(b"async")
                    && !self.bytes.get(self.pos + 5).map_or(false, |c| c.is_ascii_alphanumeric() || *c == b'_')
                {
                    for _ in 0..5 { self.advance(); }
                    Token::AtAsync
                } else {
                    Token::At
                }
            }
            b'"' => {
                self.advance();
                self.read_string_lit()?
            }
            b if b.is_ascii_digit() || (b == b'-' && self.peek2().map_or(false, |(_, c)| c.is_ascii_digit())) => {
                self.read_integer()?
            }
            b if b.is_ascii_alphabetic() || b == b'_' => {
                self.read_ident()?
            }
            _ => {
                self.advance();
                return Err(LexError::UnexpectedChar {
                    ch: b as char,
                    offset: start,
                });
            }
        };

        let end = self.pos;
        Ok(Spanned {
            tok,
            span: Span::new(start, end),
        })
    }

    fn read_string_lit(&mut self) -> Result<Token, LexError> {
        let start = self.pos;
        let mut s = String::new();
        loop {
            match self.advance() {
                Some(b'"') => break,
                Some(b'\\') => {
                    match self.advance() {
                        Some(b'n') => s.push('\n'),
                        Some(b't') => s.push('\t'),
                        Some(b'\\') => s.push('\\'),
                        Some(b'"') => s.push('"'),
                        Some(c) => {
                            s.push('\\');
                            s.push(c as char);
                        }
                        None => return Err(LexError::UnterminatedString { offset: start - 1 }),
                    }
                }
                Some(c) => s.push(c as char),
                None => return Err(LexError::UnterminatedString { offset: start - 1 }),
            }
        }
        Ok(Token::StringLit(s))
    }

    fn read_integer(&mut self) -> Result<Token, LexError> {
        let _start = self.pos;
        let negative = if self.peek() == Some(b'-') {
            self.advance();
            true
        } else {
            false
        };

        let mut value: i64 = 0;
        let mut any_digit = false;
        while let Some(b @ b'0'..=b'9') = self.peek() {
            any_digit = true;
            value = value * 10 + (b - b'0') as i64;
            self.advance();
        }

        if !any_digit {
            return Err(LexError::ExpectedDigit { offset: self.pos });
        }

        if negative {
            value = -value;
        }

        Ok(Token::Integer(value))
    }

    fn read_ident(&mut self) -> Result<Token, LexError> {
        let start = self.pos;
        while let Some(b) = self.peek() {
            if b.is_ascii_alphanumeric() || b == b'_' {
                self.advance();
            } else {
                break;
            }
        }
        let ident = &self.src[start..self.pos];
        Ok(Token::Ident(ident.to_string()))
    }
}

// ── Errors ──────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, thiserror::Error)]
pub enum LexError {
    #[error("unexpected character '{ch}' at byte offset {offset}")]
    UnexpectedChar { ch: char, offset: usize },

    #[error("unterminated string literal starting at byte offset {offset}")]
    UnterminatedString { offset: usize },

    #[error("expected digit at byte offset {offset}")]
    ExpectedDigit { offset: usize },
}

// ── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn tok_str(s: &str) -> Vec<Token> {
        Lexer::new(s)
            .tokenize_all()
            .unwrap()
            .into_iter()
            .map(|sp| sp.tok)
            .collect()
    }

    #[test]
    fn empty_source() {
        assert_eq!(tok_str(""), vec![Token::Eof]);
    }

    #[test]
    fn single_tokens() {
        assert_eq!(tok_str("{"), vec![Token::LBrace, Token::Eof]);
        assert_eq!(tok_str("->"), vec![Token::Arrow, Token::Eof]);
    }

    #[test]
    fn annotations() {
        assert_eq!(
            tok_str("@version @async @borrowed"),
            vec![
                Token::At,
                Token::Ident("version".into()),
                Token::AtAsync,
                Token::At,
                Token::Ident("borrowed".into()),
                Token::Eof,
            ]
        );
    }

    #[test]
    fn string_literal() {
        let toks = tok_str(r#""hello world""#);
        assert_eq!(toks[0], Token::StringLit("hello world".into()));
    }

    #[test]
    fn integers() {
        let toks = tok_str("0 -1 42 999");
        assert_eq!(toks, vec![
            Token::Integer(0),
            Token::Integer(-1),
            Token::Integer(42),
            Token::Integer(999),
            Token::Eof,
        ]);
    }

    #[test]
    fn full_declaration() {
        let src = r#"
            @version("aeroidl/1.0")
            enum Foo { A = 0, B = 1 }
        "#;
        let toks = tok_str(src);
        assert!(toks.contains(&Token::Ident("Foo".into())));
        assert!(toks.contains(&Token::Ident("A".into())));
        assert!(toks.contains(&Token::Integer(0)));
    }

    #[test]
    fn comments_ignored() {
        let src = r#"
            // This is a comment
            enum X { A = 0 } // inline
        "#;
        let toks = tok_str(src);
        // Should have: enum X { A = 0 } <EOF> — no comment tokens
        assert!(!toks.iter().any(|t| matches!(t, Token::Ident(s) if s == "This")));
    }

    #[test]
    fn unterminated_string() {
        let result = Lexer::new(r#""oops"#).tokenize_all();
        assert!(result.is_err());
    }

    #[test]
    fn unexpected_char() {
        let result = Lexer::new("$").tokenize_all();
        assert!(result.is_err());
    }
}
