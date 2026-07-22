/*
 * sql_parser.c — Phase 19 (relational layer) lexer + recursive-descent
 * parser. See sql_parser.h for the grammar and design writeup.
 */
#include "sql_parser.h"
#include <stddef.h>

// ─── String / char helpers (no libc — each kernel source file keeps its
// own small copies; sq_* here, matching rs_*/ri_*/pe_* elsewhere). ─────────
static uint32_t sq_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void sq_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}
static void sq_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}
static int sq_is_digit(char c) { return c >= '0' && c <= '9'; }
static int sq_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int sq_is_alnum(char c) { return sq_is_alpha(c) || sq_is_digit(c); }
static char sq_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int sq_streq_ci(const char* a, const char* b) {
    while (*a && *b) { if (sq_lower(*a) != sq_lower(*b)) return 0; a++; b++; }
    return *a == *b;
}
static int sq_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

// ─── Lexer ────────────────────────────────────────────────────────────────
typedef enum {
    TOK_EOF = 0, TOK_IDENT, TOK_NUMBER, TOK_STRING,
    TOK_KW_SELECT, TOK_KW_FROM, TOK_KW_WHERE, TOK_KW_AND, TOK_KW_OR,
    TOK_KW_ORDER, TOK_KW_BY, TOK_KW_ASC, TOK_KW_DESC, TOK_KW_LIMIT,
    TOK_KW_INSERT, TOK_KW_INTO, TOK_KW_VALUES,
    TOK_KW_UPDATE, TOK_KW_SET,
    TOK_KW_DELETE,
    TOK_KW_JOIN, TOK_KW_ON,   // Phase 20
    TOK_KW_GROUP, TOK_KW_HAVING,   // Phase 1 (SQL Feature-Parity Roadmap)
    TOK_KW_LEFT, TOK_KW_OUTER, TOK_KW_AS,   // Phase 2 (SQL Feature-Parity Roadmap)
    TOK_KW_IN, TOK_KW_LIKE,   // Phase 3 (SQL Feature-Parity Roadmap)
    TOK_KW_IS, TOK_KW_NOT, TOK_KW_NULL,   // Phase 4 (SQL Feature-Parity Roadmap)
    TOK_KW_TRUE, TOK_KW_FALSE,
    // Phase 5 (SQL Feature-Parity Roadmap): DDL.
    TOK_KW_CREATE, TOK_KW_TABLE, TOK_KW_ALTER, TOK_KW_ADD, TOK_KW_COLUMN,
    TOK_KW_DROP, TOK_KW_INDEX, TOK_KW_UNIQUE, TOK_KW_REFERENCES,
    TOK_KW_TYPE_STRING, TOK_KW_TYPE_UINT64, TOK_KW_TYPE_FLOAT,
    TOK_KW_TYPE_BOOL, TOK_KW_TYPE_BLOB,
    TOK_STAR, TOK_COMMA, TOK_LPAREN, TOK_RPAREN, TOK_SEMI, TOK_DOT,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_PLUS, TOK_MINUS, TOK_SLASH,   // Phase 3 (SQL Feature-Parity Roadmap): arithmetic operators
    TOK_ERROR,
} SqlTokenKind;

struct SqlToken {
    SqlTokenKind kind;
    char text[RECORD_VAL_LEN];
    // Phase 7 (SQL Feature-Parity Roadmap): the offset into the lexer's
    // src[] where this token's own raw text begins (after any leading
    // whitespace, before any subsequent one) -- set uniformly for every
    // token kind in lex_next() below. Lets a caller capture an exact raw
    // source substring between two tokens (used to extract an embedded
    // subquery's original SQL text, since a subquery is re-parsed and
    // executed from text at exec time rather than carried as a parsed
    // struct -- see predicate.h's Phase 7 note on why).
    uint32_t src_start;
};

struct SqlLexer {
    const char* src;
    uint32_t    pos;
    uint32_t    len;
};

struct KeywordEntry { const char* text; SqlTokenKind kind; };
static const struct KeywordEntry KEYWORDS[] = {
    {"SELECT", TOK_KW_SELECT}, {"FROM", TOK_KW_FROM}, {"WHERE", TOK_KW_WHERE},
    {"AND", TOK_KW_AND}, {"OR", TOK_KW_OR},
    {"ORDER", TOK_KW_ORDER}, {"BY", TOK_KW_BY}, {"ASC", TOK_KW_ASC}, {"DESC", TOK_KW_DESC},
    {"LIMIT", TOK_KW_LIMIT},
    {"INSERT", TOK_KW_INSERT}, {"INTO", TOK_KW_INTO}, {"VALUES", TOK_KW_VALUES},
    {"UPDATE", TOK_KW_UPDATE}, {"SET", TOK_KW_SET},
    {"DELETE", TOK_KW_DELETE},
    {"JOIN", TOK_KW_JOIN}, {"ON", TOK_KW_ON},
    {"GROUP", TOK_KW_GROUP}, {"HAVING", TOK_KW_HAVING},
    {"LEFT", TOK_KW_LEFT}, {"OUTER", TOK_KW_OUTER}, {"AS", TOK_KW_AS},
    {"IN", TOK_KW_IN}, {"LIKE", TOK_KW_LIKE},
    {"IS", TOK_KW_IS}, {"NOT", TOK_KW_NOT}, {"NULL", TOK_KW_NULL},
    {"TRUE", TOK_KW_TRUE}, {"FALSE", TOK_KW_FALSE},
    // Phase 5 (SQL Feature-Parity Roadmap): DDL.
    {"CREATE", TOK_KW_CREATE}, {"TABLE", TOK_KW_TABLE},
    {"ALTER", TOK_KW_ALTER}, {"ADD", TOK_KW_ADD}, {"COLUMN", TOK_KW_COLUMN},
    {"DROP", TOK_KW_DROP}, {"INDEX", TOK_KW_INDEX},
    {"UNIQUE", TOK_KW_UNIQUE}, {"REFERENCES", TOK_KW_REFERENCES},
    // Column-type keywords -- checked ahead of TOK_IDENT in the lexer's own
    // keyword-lookup pass (same mechanism every other keyword uses), so a
    // column named e.g. "string" would need... actually it can't be named
    // that: matching this parser's existing keyword-shadowing behavior for
    // every other reserved word (SELECT, FROM, etc.), a column/table name
    // that collides with a type keyword is not expressible -- named here,
    // not silently surprising.
    {"STRING", TOK_KW_TYPE_STRING}, {"UINT64", TOK_KW_TYPE_UINT64},
    {"FLOAT", TOK_KW_TYPE_FLOAT}, {"BOOL", TOK_KW_TYPE_BOOL},
    {"BLOB", TOK_KW_TYPE_BLOB},
};
#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static struct SqlToken lex_next(struct SqlLexer* lx) {
    struct SqlToken t; t.text[0] = '\0';
    while (lx->pos < lx->len) {
        char ws = lx->src[lx->pos];
        if (ws == ' ' || ws == '\t' || ws == '\n' || ws == '\r') { lx->pos++; continue; }
        break;
    }
    // Phase 7 (SQL Feature-Parity Roadmap): recorded AFTER the whitespace
    // skip above, for every token kind including TOK_EOF -- see SqlToken's
    // own field comment.
    t.src_start = lx->pos;
    if (lx->pos >= lx->len) { t.kind = TOK_EOF; return t; }

    char c = lx->src[lx->pos];

    // number: digit, or '-' immediately followed by a digit (a leading
    // sign is part of the literal). Phase 3 (SQL Feature-Parity Roadmap)
    // added a real MINUS operator token below, but this rule still checked
    // FIRST and unchanged -- so `-` immediately followed by a digit is
    // ALWAYS a negative-number literal, regardless of context, exactly as
    // before Phase 3. This means subtracting a positive numeric literal
    // needs a space after the minus sign (`price - 1`, not `price -1` or
    // `price-1`) to parse as subtraction rather than a single negative-
    // number token -- a real, named limitation, see sql_parser.h's Phase 3
    // note. Subtracting a column (`price - discount`) is unaffected.
    if (sq_is_digit(c) || (c == '-' && lx->pos + 1 < lx->len && sq_is_digit(lx->src[lx->pos + 1]))) {
        uint32_t start = lx->pos;
        if (c == '-') lx->pos++;
        while (lx->pos < lx->len && sq_is_digit(lx->src[lx->pos])) lx->pos++;
        if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
            lx->pos++;
            while (lx->pos < lx->len && sq_is_digit(lx->src[lx->pos])) lx->pos++;
        }
        uint32_t len = lx->pos - start;
        if (len >= RECORD_VAL_LEN) len = RECORD_VAL_LEN - 1;
        for (uint32_t i = 0; i < len; i++) t.text[i] = lx->src[start + i];
        t.text[len] = '\0';
        t.kind = TOK_NUMBER;
        return t;
    }

    // identifier or keyword (case-insensitive keyword match, standard SQL convention)
    if (sq_is_alpha(c)) {
        uint32_t start = lx->pos;
        while (lx->pos < lx->len && sq_is_alnum(lx->src[lx->pos])) lx->pos++;
        uint32_t len = lx->pos - start;
        if (len >= RECORD_VAL_LEN) len = RECORD_VAL_LEN - 1;
        for (uint32_t i = 0; i < len; i++) t.text[i] = lx->src[start + i];
        t.text[len] = '\0';
        for (uint32_t k = 0; k < KEYWORD_COUNT; k++) {
            if (sq_streq_ci(t.text, KEYWORDS[k].text)) { t.kind = KEYWORDS[k].kind; return t; }
        }
        t.kind = TOK_IDENT;
        return t;
    }

    // string literal: single-quoted, '' is an escaped literal quote (standard SQL convention)
    if (c == '\'') {
        lx->pos++;
        uint32_t n = 0;
        while (lx->pos < lx->len) {
            if (lx->src[lx->pos] == '\'') {
                if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '\'') {
                    if (n < RECORD_VAL_LEN - 1) t.text[n++] = '\'';
                    lx->pos += 2;
                    continue;
                }
                lx->pos++;   // consume the closing quote
                break;
            }
            if (n < RECORD_VAL_LEN - 1) t.text[n++] = lx->src[lx->pos];
            lx->pos++;
        }
        t.text[n] = '\0';
        t.kind = TOK_STRING;
        return t;
    }

    switch (c) {
        case '*': lx->pos++; t.kind = TOK_STAR;   return t;
        case ',': lx->pos++; t.kind = TOK_COMMA;  return t;
        case '(': lx->pos++; t.kind = TOK_LPAREN; return t;
        case ')': lx->pos++; t.kind = TOK_RPAREN; return t;
        case ';': lx->pos++; t.kind = TOK_SEMI;   return t;
        case '.': lx->pos++; t.kind = TOK_DOT;    return t;   // Phase 20: table.column qualifiers
        case '=': lx->pos++; t.kind = TOK_EQ;     return t;
        // Phase 3 (SQL Feature-Parity Roadmap): arithmetic operators. '-'
        // only reaches here when NOT immediately followed by a digit (the
        // negative-number-literal rule above already claimed that case) --
        // see this file's Phase 3 note on the resulting minor limitation.
        case '+': lx->pos++; t.kind = TOK_PLUS;   return t;
        case '-': lx->pos++; t.kind = TOK_MINUS;  return t;
        case '/': lx->pos++; t.kind = TOK_SLASH;  return t;
        case '!':
            if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '=') { lx->pos += 2; t.kind = TOK_NE; return t; }
            lx->pos++; t.kind = TOK_ERROR; return t;
        case '<':
            if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '=') { lx->pos += 2; t.kind = TOK_LE; return t; }
            if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '>') { lx->pos += 2; t.kind = TOK_NE; return t; }
            lx->pos++; t.kind = TOK_LT; return t;
        case '>':
            if (lx->pos + 1 < lx->len && lx->src[lx->pos + 1] == '=') { lx->pos += 2; t.kind = TOK_GE; return t; }
            lx->pos++; t.kind = TOK_GT; return t;
        default:
            lx->pos++; t.kind = TOK_ERROR; return t;
    }
}

// ─── Parser ─────────────────────────────────────────────────────────────────
struct SqlParser {
    struct SqlLexer lx;
    struct SqlToken cur;
    int    error;
    char   err[SQL_ERR_MSG_LEN];
};

static void advance(struct SqlParser* p) { p->cur = lex_next(&p->lx); }

static void set_error(struct SqlParser* p, const char* msg) {
    if (p->error) return;   // keep the FIRST error, not the cascade that follows it
    p->error = 1;
    sq_strcpy(p->err, msg, SQL_ERR_MSG_LEN);
}

static int expect(struct SqlParser* p, SqlTokenKind k, const char* msg) {
    if (p->cur.kind != k) { set_error(p, msg); return 0; }
    advance(p);
    return 1;
}

static int parse_compare_op(struct SqlParser* p, PredicateCompareOp* out) {
    switch (p->cur.kind) {
        case TOK_EQ: *out = PRED_OP_EQ; break;
        case TOK_NE: *out = PRED_OP_NE; break;
        case TOK_LT: *out = PRED_OP_LT; break;
        case TOK_GT: *out = PRED_OP_GT; break;
        case TOK_LE: *out = PRED_OP_LE; break;
        case TOK_GE: *out = PRED_OP_GE; break;
        default: set_error(p, "expected a comparison operator (=, !=, <>, <, >, <=, >=)"); return 0;
    }
    advance(p);
    return 1;
}

// column_ref := ident ['.' ident] -- Phase 20: an optional table qualifier,
// stored as one concatenated "table.column" string (see sql_parser.h's
// header comment on why this needs no new field type: existing string
// fields already have room, and every consumer -- predicate.c's
// compare_typed(), sql_exec.c's find_column_index() -- only ever does a
// plain string compare against a layout's column_names[], so a qualified
// name baked into that same string "just works" with zero changes to
// either of those files).
static int parse_column_ref(struct SqlParser* p, char* out, uint32_t max) {
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name"); return 0; }
    char first[RECORD_KEY_LEN];
    sq_strcpy(first, p->cur.text, RECORD_KEY_LEN);
    advance(p);
    if (p->cur.kind != TOK_DOT) {
        sq_strcpy(out, first, max);
        return 1;
    }
    advance(p);
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name after '.'"); return 0; }
    uint32_t i = 0;
    for (; first[i] && i < max - 1; i++) out[i] = first[i];
    if (i < max - 1) out[i++] = '.';
    for (uint32_t j = 0; p->cur.text[j] && i < max - 1; j++) out[i++] = p->cur.text[j];
    out[i] = '\0';
    advance(p);
    return 1;
}

// Splits an already-parsed "qualifier.column" string (from parse_column_ref)
// back into its two parts. Used only by the ON clause, which requires
// qualification (see sql_parser.h) -- SELECT-list/WHERE/ORDER BY column
// refs stay as one string throughout, since their consumers never need the
// two halves separately.
static int split_qualified(const char* in, char* qualifier, uint32_t qmax, char* col, uint32_t cmax) {
    uint32_t dot = 0xFFFFFFFFu;
    for (uint32_t i = 0; in[i]; i++) { if (in[i] == '.') { dot = i; break; } }
    if (dot == 0xFFFFFFFFu) return 0;
    uint32_t qi = 0;
    for (uint32_t i = 0; i < dot && qi < qmax - 1; i++) qualifier[qi++] = in[i];
    qualifier[qi] = '\0';
    uint32_t ci = 0;
    for (uint32_t i = dot + 1; in[i] && ci < cmax - 1; i++) col[ci++] = in[i];
    col[ci] = '\0';
    return 1;
}

// Phase 2 (SQL Feature-Parity Roadmap): parses an optional "[AS] ident"
// alias following a table name in FROM/JOIN. AS is optional, matching real
// SQL practice (see sql_parser.h) -- since no other production can start
// with a bare identifier at this position (the only things that can follow
// a FROM/JOIN table name are AS, JOIN, LEFT, WHERE, GROUP, HAVING, ORDER,
// LIMIT, ';', or end of input -- all of which are keyword tokens, never
// TOK_IDENT), a TOK_IDENT with no AS is unambiguously an alias, not
// something needing a lookahead-and-rewind trick the way aggregate calls
// did. alias_out[0] is left '\0' if no alias was given.
static void parse_optional_alias(struct SqlParser* p, char* alias_out, uint32_t max) {
    alias_out[0] = '\0';
    if (p->cur.kind == TOK_KW_AS) {
        advance(p);
        if (p->cur.kind != TOK_IDENT) { set_error(p, "expected an alias name after AS"); return; }
        sq_strcpy(alias_out, p->cur.text, max);
        advance(p);
    } else if (p->cur.kind == TOK_IDENT) {
        sq_strcpy(alias_out, p->cur.text, max);
        advance(p);
    }
}

// ─── Phase 1 (SQL Feature-Parity Roadmap): aggregate function calls ────────
// COUNT/SUM/AVG/MIN/MAX are deliberately NOT lexer keywords (matching real
// SQL practice, and this file's own KEYWORDS[] table doesn't list them) --
// they're ordinary TOK_IDENT tokens, disambiguated from a plain column_ref
// purely by whether a '(' immediately follows. This means a table with an
// actual column named "count" still parses fine as a plain column_ref, as
// long as it's never itself followed by '(' -- the one case that would be
// genuinely ambiguous doesn't arise in this grammar (a bare column_ref is
// never followed by '(' in any valid statement).
static SqlAggFunc match_agg_fn(const char* ident) {
    if (sq_streq_ci(ident, "COUNT")) return SQL_AGG_COUNT;
    if (sq_streq_ci(ident, "SUM"))   return SQL_AGG_SUM;
    if (sq_streq_ci(ident, "AVG"))   return SQL_AGG_AVG;
    if (sq_streq_ci(ident, "MIN"))   return SQL_AGG_MIN;
    if (sq_streq_ci(ident, "MAX"))   return SQL_AGG_MAX;
    return SQL_AGG_NONE;
}

// select_item := agg_call | column_ref -- shared by SELECT-list parsing and
// HAVING comparison left-hand-side parsing (see sql_parser.h's header
// comment on why HAVING needs this: it must be able to reference an
// aggregate result by the exact same rendered label the SELECT list and
// executor agree on). On success, *label holds the canonical rendered text
// ("COUNT(*)", "SUM(amount)", or just the plain column name for a
// non-aggregate item); *fn is SQL_AGG_NONE for a plain column_ref, else the
// matched function with *arg holding its argument column name (or "*",
// COUNT only).
static int parse_select_item(struct SqlParser* p, char* label, uint32_t label_max,
                             SqlAggFunc* fn, char* arg, uint32_t arg_max) {
    *fn = SQL_AGG_NONE;
    arg[0] = '\0';
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name or aggregate function"); return 0; }

    char ident[RECORD_KEY_LEN];
    sq_strcpy(ident, p->cur.text, RECORD_KEY_LEN);
    SqlAggFunc maybe_fn = match_agg_fn(ident);

    // One-token lookahead: save state, advance past `ident`, check whether
    // '(' follows. Trivial to save/restore here since SqlLexer/SqlToken are
    // plain value structs with no external state -- no real backtracking
    // machinery needed, matching this parser's existing simplicity.
    struct SqlLexer save_lx = p->lx;
    struct SqlToken save_cur = p->cur;
    advance(p);

    if (maybe_fn != SQL_AGG_NONE && p->cur.kind == TOK_LPAREN) {
        advance(p);   // consume '('
        char argbuf[RECORD_KEY_LEN];
        if (maybe_fn == SQL_AGG_COUNT && p->cur.kind == TOK_STAR) {
            sq_strcpy(argbuf, "*", RECORD_KEY_LEN);
            advance(p);
        } else if (p->cur.kind == TOK_STAR) {
            set_error(p, "only COUNT may take '*' as its argument");
            return 0;
        } else {
            if (!parse_column_ref(p, argbuf, RECORD_KEY_LEN)) return 0;
        }
        if (!expect(p, TOK_RPAREN, "expected ')' after aggregate function argument")) return 0;

        *fn = maybe_fn;
        sq_strcpy(arg, argbuf, arg_max);

        static const char* fn_text[] = { "", "COUNT", "SUM", "AVG", "MIN", "MAX" };
        const char* fname = fn_text[maybe_fn];
        uint32_t li = 0;
        for (; fname[li] && li < label_max - 1; li++) label[li] = fname[li];
        if (li < label_max - 1) label[li++] = '(';
        for (uint32_t j = 0; argbuf[j] && li < label_max - 1; j++) label[li++] = argbuf[j];
        if (li < label_max - 1) label[li++] = ')';
        label[li] = '\0';
        return 1;
    }

    // Not an aggregate call after all -- rewind to just after `ident` was
    // first lexed (i.e. p->cur == ident again) and parse it as an ordinary
    // column_ref, which may itself be "table.column".
    p->lx  = save_lx;
    p->cur = save_cur;
    if (!parse_column_ref(p, label, label_max)) return 0;
    return 1;
}

// Phase 4 (SQL Feature-Parity Roadmap): INSERT's VALUES list entries -- a
// plain literal (byte-for-byte the pre-Phase-4 shape) or the NULL keyword.
// Separate from parse_literal() itself (rather than teaching that function
// to accept NULL) because parse_literal()'s other call sites -- IN list
// entries, LIKE patterns, comparison RHS literals -- deliberately do NOT
// accept a bare NULL (see sql_parser.h's Phase 4 note on why `= NULL`/`IN
// (NULL)` aren't given special meaning).
static int parse_insert_value(struct SqlParser* p, char* out, uint32_t max, uint8_t* is_null);

static int parse_literal(struct SqlParser* p, char* out, uint32_t max) {
    switch (p->cur.kind) {
        case TOK_NUMBER:
        case TOK_STRING:
            sq_strcpy(out, p->cur.text, max);
            advance(p);
            return 1;
        case TOK_KW_TRUE:
            sq_strcpy(out, "true", max);
            advance(p);
            return 1;
        case TOK_KW_FALSE:
            sq_strcpy(out, "false", max);
            advance(p);
            return 1;
        default:
            set_error(p, "expected a literal value (number, 'string', TRUE, or FALSE)");
            return 0;
    }
}

static int parse_insert_value(struct SqlParser* p, char* out, uint32_t max, uint8_t* is_null) {
    if (p->cur.kind == TOK_KW_NULL) {
        *is_null = 1;
        out[0] = '\0';
        advance(p);
        return 1;
    }
    *is_null = 0;
    return parse_literal(p, out, max);
}

static int sq_contains_dot(const char* s) {
    for (; *s; s++) if (*s == '.') return 1;
    return 0;
}

// Phase 3 (SQL Feature-Parity Roadmap): arith_operand := ident | number.
// A column reference *usable in arithmetic* is always a single unqualified
// identifier, never "table.column" (see sql_parser.h's Phase 3 note on why
// arithmetic is scoped to single-table WHERE/SET, not JOIN chains) -- but
// this function is also the one-operand lookahead parse_comparison() uses
// to read comparison_lhs BEFORE it knows whether the statement is even
// going to turn out arithmetic, and comparison_lhs must still accept a
// plain qualified column ("e.name") for the byte-compatible non-arithmetic
// path a JOIN's WHERE clause depends on. So this DOES consume an optional
// ".ident" suffix here (byte-identical to parse_column_ref's own handling)
// -- callers that care whether the result is arithmetic-eligible check
// sq_contains_dot(out->text) themselves and skip the arithmetic-operator
// lookahead for a qualified result, since "e.price * 2" is out of scope
// and correctly falls through to a plain (and then likely erroring, which
// is fine -- it's unsupported syntax) comparison_tail call instead.
static int parse_arith_operand(struct SqlParser* p, struct PredArithOperand* out) {
    if (p->cur.kind == TOK_IDENT) {
        out->is_column = 1;
        char first[RECORD_KEY_LEN];
        sq_strcpy(first, p->cur.text, RECORD_KEY_LEN);
        advance(p);
        if (p->cur.kind == TOK_DOT) {
            advance(p);
            if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name after '.'"); return 0; }
            uint32_t i = 0;
            for (; first[i] && i < RECORD_KEY_LEN - 1; i++) out->text[i] = first[i];
            if (i < RECORD_KEY_LEN - 1) out->text[i++] = '.';
            for (uint32_t j = 0; p->cur.text[j] && i < RECORD_KEY_LEN - 1; j++) out->text[i++] = p->cur.text[j];
            out->text[i] = '\0';
            advance(p);
        } else {
            sq_strcpy(out->text, first, RECORD_KEY_LEN);
        }
        return 1;
    }
    if (p->cur.kind == TOK_NUMBER) {
        out->is_column = 0;
        sq_strcpy(out->text, p->cur.text, RECORD_KEY_LEN);
        advance(p);
        return 1;
    }
    set_error(p, "expected a column name or number");
    return 0;
}

static int parse_arith_op_token(struct SqlParser* p, PredArithOp* out) {
    switch (p->cur.kind) {
        case TOK_PLUS:  *out = PRED_ARITH_ADD; break;
        case TOK_MINUS: *out = PRED_ARITH_SUB; break;
        case TOK_STAR:  *out = PRED_ARITH_MUL; break;
        case TOK_SLASH: *out = PRED_ARITH_DIV; break;
        default: return 0;
    }
    return 1;
}

// Phase 7 (SQL Feature-Parity Roadmap): parse_select_body is defined much
// later in this file (it's the full SELECT grammar), but parse_comparison_tail
// below -- defined here, well before it -- needs to invoke a full nested
// SELECT parse (via sql_parse(), declared in sql_parser.h) to validate an
// embedded subquery's shape. sql_parse() itself is already visible via the
// header; this forward declaration is only needed because the *validation*
// path below reads SqlSelectStmt fields by first constructing a full
// SqlStatement via sql_parse(), which doesn't require parse_select_body's
// own declaration at all -- kept here anyway as a forward reference for
// clarity/symmetry with parse_insert_value's forward declaration above.
static void parse_select_body(struct SqlParser* p, struct SqlSelectStmt* s);

// Phase 7 (SQL Feature-Parity Roadmap): dedicated PARSE-TIME-ONLY scratch
// for validating an embedded subquery's shape (via a throwaway sql_parse()
// call) -- deliberately separate from BOTH g_stmt_scratch (sql_parser.c has
// no such global; that's sql_exec.c's) and the Phase 7 runtime resolution
// scratch that sql_exec.c will add for actually *executing* a subquery at
// exec time. This one only ever lives for the duration of one
// try_parse_embedded_subquery() call. Static (not a ~34KB stack local)
// because kernel code paths keep stack frames small by convention.
static struct SqlStatement g_subquery_skip_scratch;

// Guards against a subquery containing ANOTHER embedded subquery: the
// validation parse below (sql_parse() on the captured raw text) reuses this
// same file's parse_comparison_tail() / try_parse_embedded_subquery()
// machinery, which would otherwise recurse into a SECOND sql_parse() call
// still targeting the SAME static g_subquery_skip_scratch -- clobbering the
// outer validation's in-progress result. Scope is deliberately "one level of
// subquery only" (see predicate.h's Phase 7 note), so a nested attempt is
// rejected as a parse error instead of being recursively validated.
static int g_subquery_validating = 0;

// Attempts to parse an embedded, parenthesized SELECT starting at the
// CURRENT '(' token (not yet consumed). Three outcomes:
//   1 -- p->cur was '(' immediately followed by SELECT; the subquery text
//        was captured, validated (must be a plain, non-JOIN, non-aggregate,
//        single-column SELECT), and registered via predicate_add_subquery();
//        *out_subq_idx is set and the parser is positioned just past the
//        closing ')'.
//   0 -- p->cur was NOT '(' followed by SELECT; parser state is rewound to
//        exactly what it was on entry, so the caller can fall through to its
//        normal (non-subquery) parsing path with no side effects.
//  -1 -- p->cur WAS '(' SELECT, but the subquery is malformed or unsupported
//        in shape; set_error() has already been called.
static int try_parse_embedded_subquery(struct SqlParser* p, struct Predicate* pred, uint32_t* out_subq_idx) {
    if (p->cur.kind != TOK_LPAREN) return 0;
    struct SqlLexer save_lx = p->lx;
    struct SqlToken save_cur = p->cur;
    advance(p);   // consume '('
    if (p->cur.kind != TOK_KW_SELECT) {
        p->lx  = save_lx;
        p->cur = save_cur;
        return 0;
    }

    // Token-level paren-depth-aware skip from SELECT to its matching ')',
    // capturing the raw source span [start, end) -- everything between the
    // two parens, i.e. the subquery's own SQL text with no wrapping parens.
    uint32_t start = p->cur.src_start;
    uint32_t depth = 1;
    for (;;) {
        if (p->cur.kind == TOK_EOF) { set_error(p, "unterminated subquery: missing ')'"); return -1; }
        if (p->cur.kind == TOK_LPAREN) { depth++; advance(p); continue; }
        if (p->cur.kind == TOK_RPAREN) {
            depth--;
            if (depth == 0) break;   // this ')' is the subquery's own closer
            advance(p);
            continue;
        }
        advance(p);
    }
    uint32_t end = p->cur.src_start;   // src_start of the matching ')'

    char raw[PREDICATE_SUBQUERY_TEXT_LEN];
    if (end < start || (end - start) >= PREDICATE_SUBQUERY_TEXT_LEN) {
        set_error(p, "subquery too long");
        return -1;
    }
    uint32_t n = end - start;
    for (uint32_t i = 0; i < n; i++) raw[i] = p->lx.src[start + i];
    raw[n] = '\0';

    if (!expect(p, TOK_RPAREN, "expected ')' after subquery")) return -1;

    if (g_subquery_validating) {
        set_error(p, "nested subqueries are not supported");
        return -1;
    }
    g_subquery_validating = 1;
    char verr[SQL_ERR_MSG_LEN];
    int perr = sql_parse(raw, &g_subquery_skip_scratch, verr, SQL_ERR_MSG_LEN);
    g_subquery_validating = 0;

    if (perr) { set_error(p, "invalid subquery: unable to parse"); return -1; }
    if (g_subquery_skip_scratch.kind != SQL_STMT_SELECT) { set_error(p, "subquery must be a SELECT"); return -1; }
    struct SqlSelectStmt* sq = &g_subquery_skip_scratch.u.select;
    if (sq->has_join)       { set_error(p, "subquery with JOIN is not supported"); return -1; }
    if (sq->has_aggregates) { set_error(p, "subquery with an aggregate is not supported"); return -1; }
    if (sq->select_all || sq->column_count != 1) { set_error(p, "subquery must select exactly one column"); return -1; }

    uint32_t idx = predicate_add_subquery(pred, raw);
    if (idx == PREDICATE_INVALID_NODE) { set_error(p, "too many subqueries in one statement"); return -1; }
    *out_subq_idx = idx;
    return 1;
}

// comparison's shared tail, given an already-parsed plain (non-arithmetic)
// left-hand side string `col` -- either a bare column_ref (WHERE) or a
// rendered select_item label (HAVING, see the allow_agg note below). Phase
// 1's plain-comparison shape; Phase 3 (SQL Feature-Parity Roadmap) adds IN
// and LIKE here, since both apply equally to a WHERE column_ref or a
// HAVING select_item label with no special-casing needed. Phase 7 adds
// scalar/IN subqueries, both routed through try_parse_embedded_subquery().
static uint32_t parse_comparison_tail(struct SqlParser* p, struct Predicate* pred, const char* col) {
    // Phase 4 (SQL Feature-Parity Roadmap): IS [NOT] NULL. No literal to
    // parse -- predicate_add_comparison()'s literal param is simply unused
    // for these two ops (see predicate.h).
    if (p->cur.kind == TOK_KW_IS) {
        advance(p);
        int is_not = 0;
        if (p->cur.kind == TOK_KW_NOT) { is_not = 1; advance(p); }
        if (!expect(p, TOK_KW_NULL, "expected NULL after IS/IS NOT")) return PREDICATE_INVALID_NODE;
        uint32_t node = predicate_add_comparison(pred, col, is_not ? PRED_OP_IS_NOT_NULL : PRED_OP_IS_NULL, "");
        if (node == PREDICATE_INVALID_NODE) set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)");
        return node;
    }
    if (p->cur.kind == TOK_KW_IN) {
        advance(p);
        // Phase 7 (SQL Feature-Parity Roadmap): IN (SELECT ...) -- tried
        // first, since p->cur is the still-unconsumed '(' that both this and
        // the literal-list path below share. A non-subquery '(' rewinds
        // cleanly and falls through to the pre-Phase-7 literal-list parse.
        uint32_t subq_idx;
        int sr = try_parse_embedded_subquery(p, pred, &subq_idx);
        if (sr < 0) return PREDICATE_INVALID_NODE;
        if (sr > 0) {
            uint32_t node = predicate_add_comparison(pred, col, PRED_OP_IN_SUBQUERY, "");
            if (node == PREDICATE_INVALID_NODE) { set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)"); return PREDICATE_INVALID_NODE; }
            predicate_mark_subquery(pred, node, subq_idx);
            return node;
        }
        if (!expect(p, TOK_LPAREN, "expected '(' after IN")) return PREDICATE_INVALID_NODE;
        // IN (a, b, c) desugars to (col = a) OR (col = b) OR (col = c) at
        // parse time -- see predicate.h's Phase 3 note for why this needed
        // no predicate.h/.c changes at all.
        uint32_t acc = PREDICATE_INVALID_NODE;
        for (;;) {
            char lit[RECORD_VAL_LEN];
            if (!parse_literal(p, lit, RECORD_VAL_LEN)) return PREDICATE_INVALID_NODE;
            uint32_t eqn = predicate_add_comparison(pred, col, PRED_OP_EQ, lit);
            if (eqn == PREDICATE_INVALID_NODE) { set_error(p, "IN list too long (predicate node pool exhausted)"); return PREDICATE_INVALID_NODE; }
            acc = (acc == PREDICATE_INVALID_NODE) ? eqn : predicate_add_or(pred, acc, eqn);
            if (acc == PREDICATE_INVALID_NODE) { set_error(p, "IN list too long (predicate node pool exhausted)"); return PREDICATE_INVALID_NODE; }
            if (p->cur.kind != TOK_COMMA) break;
            advance(p);
        }
        if (!expect(p, TOK_RPAREN, "expected ')' after IN list")) return PREDICATE_INVALID_NODE;
        return acc;
    }
    if (p->cur.kind == TOK_KW_LIKE) {
        advance(p);
        char pat[RECORD_VAL_LEN];
        if (!parse_literal(p, pat, RECORD_VAL_LEN)) return PREDICATE_INVALID_NODE;
        uint32_t node = predicate_add_comparison(pred, col, PRED_OP_LIKE, pat);
        if (node == PREDICATE_INVALID_NODE) set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)");
        return node;
    }
    PredicateCompareOp op;
    if (!parse_compare_op(p, &op)) return PREDICATE_INVALID_NODE;
    // Phase 7 (SQL Feature-Parity Roadmap): scalar subquery RHS ("= (SELECT
    // ...)"), tried before parse_literal() since a bare '(' is otherwise
    // always a literal-parse error (parse_literal() never accepts parens).
    uint32_t subq_idx;
    int sr = try_parse_embedded_subquery(p, pred, &subq_idx);
    if (sr < 0) return PREDICATE_INVALID_NODE;
    if (sr > 0) {
        uint32_t node = predicate_add_comparison(pred, col, op, "");
        if (node == PREDICATE_INVALID_NODE) { set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)"); return PREDICATE_INVALID_NODE; }
        predicate_mark_subquery(pred, node, subq_idx);
        return node;
    }
    char lit[RECORD_VAL_LEN];
    if (!parse_literal(p, lit, RECORD_VAL_LEN)) return PREDICATE_INVALID_NODE;
    uint32_t node = predicate_add_comparison(pred, col, op, lit);
    if (node == PREDICATE_INVALID_NODE) set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)");
    return node;
}

// comparison := comparison_lhs (compare_op literal | IN '(' literal-list ')' | LIKE literal)
// Phase 1 (SQL Feature-Parity Roadmap): when allow_agg is set (HAVING
// only -- see sql_parser.h), the left-hand side is parsed via
// parse_select_item() instead of parse_column_ref(), so it may be an
// aggregate call ("COUNT(*)") whose rendered label becomes the comparison
// node's column_name -- exactly what a synthetic grouped-result layout's
// own column names will be at exec time (sql_exec.c), so predicate_eval()
// needs no changes at all to evaluate it.
// Phase 3 (SQL Feature-Parity Roadmap): when allow_agg is NOT set (plain
// WHERE/SET comparisons only -- HAVING never reaches this branch), the
// left-hand side may instead be a single arithmetic expression
// (arith_operand [('+'|'-'|'*'|'/') arith_operand]) -- disambiguated by a
// one-operand lookahead: parse the first operand, then check whether an
// arithmetic operator follows. No operator following, AND the operand was
// a plain column (not a number), falls through to the byte-for-byte
// pre-Phase-3 shape via parse_comparison_tail().
static uint32_t parse_comparison(struct SqlParser* p, struct Predicate* pred, int allow_agg) {
    if (allow_agg) {
        char col[RECORD_KEY_LEN];
        SqlAggFunc fn; char arg[RECORD_KEY_LEN];
        if (!parse_select_item(p, col, RECORD_KEY_LEN, &fn, arg, RECORD_KEY_LEN)) return PREDICATE_INVALID_NODE;
        return parse_comparison_tail(p, pred, col);
    }

    struct PredArithOperand op1;
    if (!parse_arith_operand(p, &op1)) return PREDICATE_INVALID_NODE;

    // Phase 3 fix: a qualified column ("table.column", e.g. from a JOIN's
    // WHERE clause) is never a valid arithmetic operand -- go straight to
    // the byte-compatible comparison_tail path without even attempting the
    // arithmetic-operator lookahead, restoring pre-Phase-3 behavior for
    // every qualified WHERE comparison exactly.
    if (op1.is_column && sq_contains_dot(op1.text)) {
        return parse_comparison_tail(p, pred, op1.text);
    }

    PredArithOp arith_op;
    if (!parse_arith_op_token(p, &arith_op)) {
        // No arithmetic operator follows -- op1 must have been a plain
        // column (a bare number here isn't a valid WHERE/SET left-hand
        // side, e.g. "5 = 5" isn't a query this grammar needs to support).
        if (!op1.is_column) { set_error(p, "expected a column reference or arithmetic expression"); return PREDICATE_INVALID_NODE; }
        return parse_comparison_tail(p, pred, op1.text);
    }
    advance(p);   // consume the operator token
    struct PredArithOperand op2;
    if (!parse_arith_operand(p, &op2)) return PREDICATE_INVALID_NODE;

    // Arithmetic comparisons don't get IN/LIKE (see sql_parser.h's Phase 3
    // note) -- always compare_op literal.
    PredicateCompareOp cop;
    if (!parse_compare_op(p, &cop)) return PREDICATE_INVALID_NODE;
    char lit[RECORD_VAL_LEN];
    if (!parse_literal(p, lit, RECORD_VAL_LEN)) return PREDICATE_INVALID_NODE;
    uint32_t node = predicate_add_arith_comparison(pred, op1, arith_op, op2, cop, lit);
    if (node == PREDICATE_INVALID_NODE) set_error(p, "WHERE clause too complex (predicate node pool exhausted)");
    return node;
}

// Forward declaration -- predicate_primary (Phase 3, below) recurses back
// into parse_predicate() for a parenthesized group.
static uint32_t parse_predicate(struct SqlParser* p, struct Predicate* pred, int allow_agg);

// predicate_primary := '(' predicate ')' | comparison -- Phase 3 (SQL
// Feature-Parity Roadmap): the ONLY grammar change parenthesized grouping
// needed. predicate.c's eval_node() already recurses through an
// arbitrarily-nested AND/OR tree; it was always this parser that only ever
// built a flat and_expr (OR and_expr)* shape, never nested by explicit
// parens. A comparison never itself starts with '(' (parse_arith_operand()
// only accepts an identifier or a number), so there's no ambiguity between
// the two alternatives here.
static uint32_t parse_predicate_primary(struct SqlParser* p, struct Predicate* pred, int allow_agg) {
    if (p->cur.kind == TOK_LPAREN) {
        advance(p);
        uint32_t inner = parse_predicate(p, pred, allow_agg);
        if (p->error) return PREDICATE_INVALID_NODE;
        if (!expect(p, TOK_RPAREN, "expected ')' to close grouped predicate")) return PREDICATE_INVALID_NODE;
        return inner;
    }
    return parse_comparison(p, pred, allow_agg);
}

// and_expr := predicate_primary (AND predicate_primary)*
static uint32_t parse_and_expr(struct SqlParser* p, struct Predicate* pred, int allow_agg) {
    uint32_t left = parse_predicate_primary(p, pred, allow_agg);
    if (p->error) return PREDICATE_INVALID_NODE;
    while (p->cur.kind == TOK_KW_AND) {
        advance(p);
        uint32_t right = parse_predicate_primary(p, pred, allow_agg);
        if (p->error) return PREDICATE_INVALID_NODE;
        left = predicate_add_and(pred, left, right);
        if (left == PREDICATE_INVALID_NODE) { set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)"); return left; }
    }
    return left;
}

// predicate := and_expr (OR and_expr)*  -- AND binds tighter than OR for
// free whenever no parens are written; explicit parens (predicate_primary,
// above) let a query override that natural precedence.
static uint32_t parse_predicate(struct SqlParser* p, struct Predicate* pred, int allow_agg) {
    uint32_t left = parse_and_expr(p, pred, allow_agg);
    if (p->error) return PREDICATE_INVALID_NODE;
    while (p->cur.kind == TOK_KW_OR) {
        advance(p);
        uint32_t right = parse_and_expr(p, pred, allow_agg);
        if (p->error) return PREDICATE_INVALID_NODE;
        left = predicate_add_or(pred, left, right);
        if (left == PREDICATE_INVALID_NODE) { set_error(p, "WHERE/HAVING clause too complex (predicate node pool exhausted)"); return left; }
    }
    return left;
}

static void finish_statement(struct SqlParser* p) {
    if (p->error) return;
    if (p->cur.kind == TOK_SEMI) advance(p);
    if (p->cur.kind != TOK_EOF) set_error(p, "unexpected trailing tokens after statement");
}

// Phase 7 (SQL Feature-Parity Roadmap): the actual SELECT grammar, split out
// from parse_select() below so a subquery (embedded inside a WHERE/HAVING
// comparison, parsed by parse_comparison_tail()) can reuse it directly --
// unlike a top-level statement, a subquery is followed by ')' and possibly
// more of the OUTER statement, never EOF/';', so it must NOT call
// finish_statement() itself (parse_select() below still does, for the
// top-level case).
static void parse_select_body(struct SqlParser* p, struct SqlSelectStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume SELECT

    if (p->cur.kind == TOK_STAR) {
        s->select_all = 1;
        advance(p);
    } else {
        uint32_t n = 0;
        int any_agg = 0;
        for (;;) {
            if (n >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many columns in SELECT list"); return; }
            SqlAggFunc fn; char arg[RECORD_KEY_LEN];
            if (!parse_select_item(p, s->columns[n], RECORD_KEY_LEN, &fn, arg, RECORD_KEY_LEN)) return;
            s->agg_fn[n] = fn;
            sq_strcpy(s->agg_arg[n], arg, RECORD_KEY_LEN);
            if (fn != SQL_AGG_NONE) any_agg = 1;
            n++;
            if (p->cur.kind != TOK_COMMA) break;
            advance(p);
        }
        s->column_count = n;
        s->has_aggregates = (uint8_t)any_agg;
    }

    if (!expect(p, TOK_KW_FROM, "expected FROM")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after FROM"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);

    // Phase 2 (SQL Feature-Parity Roadmap): the FROM table's own optional
    // alias, parsed before any JOIN clauses -- see parse_optional_alias().
    parse_optional_alias(p, s->table_alias, OBJECT_NAME_LEN);
    if (p->error) return;

    // Phase 20/Phase 2: join_clause := (JOIN | LEFT [OUTER] JOIN) ident
    // [AS ident] ON column_ref '=' column_ref, repeated up to SQL_MAX_JOINS
    // times (Phase 2 generalized Phase 20's single fixed JOIN into a real
    // chain). Both halves of the ON clause MUST be qualified
    // ("table.column" or "alias.column") -- parse_column_ref() allows but
    // doesn't require a qualifier in general, so that's enforced here
    // specifically via split_qualified() rejecting an unqualified
    // reference, not by the grammar rule itself.
    while (p->cur.kind == TOK_KW_JOIN || p->cur.kind == TOK_KW_LEFT) {
        if (s->join_count >= SQL_MAX_JOINS) { set_error(p, "too many JOINs in one statement"); return; }
        struct SqlJoinClause* jc = &s->joins[s->join_count];
        jc->type = SQL_JOIN_INNER;
        jc->alias[0] = '\0';

        if (p->cur.kind == TOK_KW_LEFT) {
            jc->type = SQL_JOIN_LEFT;
            advance(p);
            if (p->cur.kind == TOK_KW_OUTER) advance(p);   // "LEFT OUTER JOIN" -- OUTER is a no-op here
            if (!expect(p, TOK_KW_JOIN, "expected JOIN after LEFT")) return;
        } else {
            advance(p);   // consume JOIN
        }

        if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after JOIN"); return; }
        sq_strcpy(jc->table, p->cur.text, OBJECT_NAME_LEN);
        advance(p);
        parse_optional_alias(p, jc->alias, OBJECT_NAME_LEN);
        if (p->error) return;

        if (!expect(p, TOK_KW_ON, "expected ON after JOIN table name")) return;

        char left[RECORD_KEY_LEN];
        if (!parse_column_ref(p, left, RECORD_KEY_LEN)) return;
        if (!expect(p, TOK_EQ, "expected '=' in ON clause")) return;
        char right[RECORD_KEY_LEN];
        if (!parse_column_ref(p, right, RECORD_KEY_LEN)) return;

        if (!split_qualified(left, jc->on_left_qualifier, OBJECT_NAME_LEN, jc->on_left_col, RECORD_KEY_LEN)) {
            set_error(p, "ON clause's left side must be table.column or alias.column");
            return;
        }
        if (!split_qualified(right, jc->on_right_qualifier, OBJECT_NAME_LEN, jc->on_right_col, RECORD_KEY_LEN)) {
            set_error(p, "ON clause's right side must be table.column or alias.column");
            return;
        }
        s->join_count++;
    }
    s->has_join = (s->join_count > 0) ? 1 : 0;

    if (p->cur.kind == TOK_KW_WHERE) {
        advance(p);
        predicate_init(&s->where);
        s->where.root = parse_predicate(p, &s->where, 0);
        if (p->error) return;
        s->has_where = 1;
    }

    // Phase 1 (SQL Feature-Parity Roadmap): GROUP BY column_ref -- single
    // column only (see sql_parser.h header comment). GROUP BY combined with
    // JOIN is rejected by the executor (SQL_ERR_GROUP_BY_JOIN_UNSUPPORTED),
    // not here -- the parser doesn't need to know about that scope cut.
    if (p->cur.kind == TOK_KW_GROUP) {
        advance(p);
        if (!expect(p, TOK_KW_BY, "expected BY after GROUP")) return;
        if (!parse_column_ref(p, s->group_by, RECORD_KEY_LEN)) return;
        s->has_group_by = 1;
    }

    // Phase 1: HAVING having_predicate -- only meaningful once the SELECT
    // list has at least one aggregate call (checked here, not deferred to
    // the executor, since a HAVING clause with nothing aggregated to filter
    // on is a query-shape error, not a legitimately-empty-of-meaning one).
    if (p->cur.kind == TOK_KW_HAVING) {
        if (!s->has_aggregates) { set_error(p, "HAVING requires an aggregate function in the SELECT list"); return; }
        advance(p);
        predicate_init(&s->having);
        s->having.root = parse_predicate(p, &s->having, 1);
        if (p->error) return;
        s->has_having = 1;
    }

    if (p->cur.kind == TOK_KW_ORDER) {
        advance(p);
        if (!expect(p, TOK_KW_BY, "expected BY after ORDER")) return;
        // Phase 1 (SQL Feature-Parity Roadmap): ORDER BY accepts the same
        // select_item shape as HAVING (agg_call | column_ref), rendered to
        // the same canonical label ("COUNT(*)") -- needed so a grouped
        // query can write `ORDER BY COUNT(*) DESC` and have it resolve
        // against the synthetic grouped-result layout at exec time
        // (sql_exec.c), the same trick HAVING already uses. For a plain
        // (non-aggregate, non-join) query this is byte-for-byte the old
        // parse_column_ref() behavior -- a bare identifier or "table.column"
        // renders through parse_select_item()'s own fallback path unchanged.
        SqlAggFunc fn; char arg[RECORD_KEY_LEN];
        if (!parse_select_item(p, s->order_by, RECORD_KEY_LEN, &fn, arg, RECORD_KEY_LEN)) return;
        s->has_order_by = 1;
        s->order_desc = 0;
        if (p->cur.kind == TOK_KW_ASC) advance(p);
        else if (p->cur.kind == TOK_KW_DESC) { s->order_desc = 1; advance(p); }
    }

    if (p->cur.kind == TOK_KW_LIMIT) {
        advance(p);
        if (p->cur.kind != TOK_NUMBER) { set_error(p, "expected a number after LIMIT"); return; }
        uint64_t v;
        if (sq_parse_u64(p->cur.text, &v)) { set_error(p, "invalid LIMIT value"); return; }
        s->limit = (uint32_t)v;
        s->has_limit = 1;
        advance(p);
    }
}

static void parse_select(struct SqlParser* p, struct SqlSelectStmt* s) {
    parse_select_body(p, s);
    finish_statement(p);
}

static void parse_insert(struct SqlParser* p, struct SqlInsertStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume INSERT
    if (!expect(p, TOK_KW_INTO, "expected INTO after INSERT")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after INTO"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);

    if (!expect(p, TOK_LPAREN, "expected '(' after table name")) return;
    uint32_t ccount = 0;
    for (;;) {
        if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name"); return; }
        if (ccount >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many columns"); return; }
        sq_strcpy(s->columns[ccount++], p->cur.text, RECORD_KEY_LEN);
        advance(p);
        if (p->cur.kind != TOK_COMMA) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, "expected ')' after column list")) return;

    if (!expect(p, TOK_KW_VALUES, "expected VALUES")) return;

    // Phase 6 (SQL Feature-Parity Roadmap): row 0's tuple parses into the
    // original values[]/is_null[] fields, byte-compatible with every
    // pre-Phase-6 caller -- see sql_parser.h's Phase 6 note.
    if (!expect(p, TOK_LPAREN, "expected '(' after VALUES")) return;
    uint32_t vcount = 0;
    for (;;) {
        char lit[RECORD_VAL_LEN];
        uint8_t is_null;
        if (!parse_insert_value(p, lit, RECORD_VAL_LEN, &is_null)) return;
        if (vcount >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many values"); return; }
        sq_strcpy(s->values[vcount], lit, RECORD_VAL_LEN);
        s->is_null[vcount] = is_null;
        vcount++;
        if (p->cur.kind != TOK_COMMA) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, "expected ')' after value list")) return;
    if (vcount != ccount) { set_error(p, "column count doesn't match value count"); return; }
    s->count = ccount;

    // Phase 6 (SQL Feature-Parity Roadmap): any further ", (...)" tuples are
    // additional rows (multi-row INSERT), each independently checked against
    // the SAME column count (ccount) and stored in extra_values[]/
    // extra_is_null[], bounded by SQL_INSERT_MAX_EXTRA_ROWS.
    s->extra_row_count = 0;
    while (p->cur.kind == TOK_COMMA) {
        advance(p);   // consume ','
        if (!expect(p, TOK_LPAREN, "expected '(' to start the next VALUES tuple")) return;
        if (s->extra_row_count >= SQL_INSERT_MAX_EXTRA_ROWS) {
            set_error(p, "too many VALUES tuples in one INSERT");
            return;
        }
        uint32_t r = s->extra_row_count;
        uint32_t evcount = 0;
        for (;;) {
            char lit[RECORD_VAL_LEN];
            uint8_t is_null;
            if (!parse_insert_value(p, lit, RECORD_VAL_LEN, &is_null)) return;
            if (evcount >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many values"); return; }
            sq_strcpy(s->extra_values[r][evcount], lit, RECORD_VAL_LEN);
            s->extra_is_null[r][evcount] = is_null;
            evcount++;
            if (p->cur.kind != TOK_COMMA) break;
            advance(p);
        }
        if (!expect(p, TOK_RPAREN, "expected ')' after value list")) return;
        if (evcount != ccount) { set_error(p, "column count doesn't match value count"); return; }
        s->extra_row_count++;
    }

    finish_statement(p);
}

// Phase 3 (SQL Feature-Parity Roadmap): SET column = value, where value is
// either a plain literal (byte-for-byte the pre-Phase-3 shape, *is_arith
// left 0) or a single arithmetic expression (e.g. `price = price * 1.1`).
// A literal STRING/TRUE/FALSE token is never an arithmetic operand, so
// those short-circuit straight to parse_literal() unchanged. A NUMBER or
// an identifier needs a one-operand lookahead (parse_arith_operand()) to
// tell whether an operator follows; no operator following a NUMBER operand
// is just that plain numeric literal (also byte-for-byte the pre-Phase-3
// shape); no operator following a COLUMN operand is a query error (a bare
// "SET x = y" copy-another-column shorthand isn't supported).
static int parse_set_value(struct SqlParser* p, char* lit_out, uint32_t lit_max, uint8_t* is_arith,
                           struct PredArithOperand* op1, PredArithOp* arith_op, struct PredArithOperand* op2,
                           uint8_t* is_null) {
    *is_arith = 0;
    *is_null = 0;
    // Phase 4 (SQL Feature-Parity Roadmap): SET column = NULL.
    if (p->cur.kind == TOK_KW_NULL) {
        *is_null = 1;
        lit_out[0] = '\0';
        advance(p);
        return 1;
    }
    if (p->cur.kind == TOK_STRING || p->cur.kind == TOK_KW_TRUE || p->cur.kind == TOK_KW_FALSE) {
        return parse_literal(p, lit_out, lit_max);
    }

    struct PredArithOperand o1;
    if (!parse_arith_operand(p, &o1)) return 0;

    PredArithOp aop;
    if (!parse_arith_op_token(p, &aop)) {
        if (o1.is_column) { set_error(p, "SET value must be a literal or an arithmetic expression"); return 0; }
        sq_strcpy(lit_out, o1.text, lit_max);   // plain NUMBER literal -- pre-Phase-3 shape
        return 1;
    }
    advance(p);
    struct PredArithOperand o2;
    if (!parse_arith_operand(p, &o2)) return 0;

    *is_arith = 1;
    *op1 = o1; *arith_op = aop; *op2 = o2;
    return 1;
}

static void parse_update(struct SqlParser* p, struct SqlUpdateStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume UPDATE
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after UPDATE"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);

    if (!expect(p, TOK_KW_SET, "expected SET")) return;
    uint32_t n = 0;
    for (;;) {
        if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name in SET clause"); return; }
        if (n >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many SET assignments"); return; }
        char col[RECORD_KEY_LEN];
        sq_strcpy(col, p->cur.text, RECORD_KEY_LEN);
        advance(p);
        if (!expect(p, TOK_EQ, "expected '=' in SET clause")) return;

        char lit[RECORD_VAL_LEN];
        uint8_t is_arith, is_null;
        struct PredArithOperand ao1, ao2;
        PredArithOp aop;
        if (!parse_set_value(p, lit, RECORD_VAL_LEN, &is_arith, &ao1, &aop, &ao2, &is_null)) return;

        sq_strcpy(s->set_columns[n], col, RECORD_KEY_LEN);
        s->set_is_arith[n] = is_arith;
        s->set_is_null[n]  = is_null;
        if (is_arith) {
            s->set_arith_op1[n] = ao1;
            s->set_arith_op[n]  = aop;
            s->set_arith_op2[n] = ao2;
            s->set_values[n][0] = '\0';
        } else {
            sq_strcpy(s->set_values[n], lit, RECORD_VAL_LEN);
        }
        n++;
        if (p->cur.kind != TOK_COMMA) break;
        advance(p);
    }
    s->set_count = n;

    if (p->cur.kind == TOK_KW_WHERE) {
        advance(p);
        predicate_init(&s->where);
        s->where.root = parse_predicate(p, &s->where, 0);
        if (p->error) return;
        s->has_where = 1;
    }

    finish_statement(p);
}

static void parse_delete(struct SqlParser* p, struct SqlDeleteStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume DELETE
    if (!expect(p, TOK_KW_FROM, "expected FROM after DELETE")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after FROM"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);

    if (p->cur.kind == TOK_KW_WHERE) {
        advance(p);
        predicate_init(&s->where);
        s->where.root = parse_predicate(p, &s->where, 0);
        if (p->error) return;
        s->has_where = 1;
    }

    finish_statement(p);
}

// ── Phase 5 (SQL Feature-Parity Roadmap): DDL grammar. ──────────────────────
static int parse_type_kw(struct SqlParser* p, SLSFieldType* out) {
    switch (p->cur.kind) {
        case TOK_KW_TYPE_STRING: *out = FIELD_TYPE_STRING; break;
        case TOK_KW_TYPE_UINT64: *out = FIELD_TYPE_UINT64; break;
        case TOK_KW_TYPE_FLOAT:  *out = FIELD_TYPE_FLOAT;  break;
        case TOK_KW_TYPE_BOOL:   *out = FIELD_TYPE_BOOL;   break;
        case TOK_KW_TYPE_BLOB:   *out = FIELD_TYPE_BLOB;   break;
        default: return 0;
    }
    advance(p);
    return 1;
}

// One column_def: ident type_kw [inline constraints in any order/subset].
// NOT NULL / UNIQUE / REFERENCES table(col) each parse independently and
// may repeat-check in any order the user wrote them -- see SqlColumnDef's
// own header comment in sql_parser.h for why a column can carry all three
// (they're independent flags), and why a second instance of the SAME one
// is simply never checked for (matching this parser's general "no
// dedup/validation beyond what breaks the grammar" posture elsewhere).
static void parse_column_def(struct SqlParser* p, struct SqlColumnDef* c) {
    sq_memset(c, 0, sizeof(*c));
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name in CREATE TABLE's column list"); return; }
    sq_strcpy(c->name, p->cur.text, RECORD_KEY_LEN);
    advance(p);
    if (!parse_type_kw(p, &c->type)) {
        set_error(p, "expected a column type (STRING/UINT64/FLOAT/BOOL/BLOB)");
        return;
    }
    for (;;) {
        if (p->cur.kind == TOK_KW_NOT) {
            advance(p);
            if (!expect(p, TOK_KW_NULL, "expected NULL after NOT in a column definition")) return;
            c->not_null = 1;
            continue;
        }
        if (p->cur.kind == TOK_KW_UNIQUE) {
            advance(p);
            c->is_unique = 1;
            continue;
        }
        if (p->cur.kind == TOK_KW_REFERENCES) {
            advance(p);
            if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after REFERENCES"); return; }
            sq_strcpy(c->ref_table, p->cur.text, OBJECT_NAME_LEN);
            advance(p);
            if (!expect(p, TOK_LPAREN, "expected '(' after REFERENCES table name")) return;
            if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name inside REFERENCES(...)"); return; }
            sq_strcpy(c->ref_column, p->cur.text, RECORD_KEY_LEN);
            advance(p);
            if (!expect(p, TOK_RPAREN, "expected ')' after REFERENCES column name")) return;
            c->has_reference = 1;
            continue;
        }
        break;
    }
}

// p->cur == TOK_KW_TABLE on entry -- CREATE was already consumed by
// sql_parse()'s own dispatcher, which needed to peek past it to
// disambiguate CREATE TABLE from CREATE INDEX (single-token lookahead).
static void parse_create_table_body(struct SqlParser* p, struct SqlCreateTableStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume TABLE
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after CREATE TABLE"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);
    if (!expect(p, TOK_LPAREN, "expected '(' after CREATE TABLE table name")) return;

    uint32_t n = 0;
    for (;;) {
        if (n >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many columns"); return; }
        parse_column_def(p, &s->columns[n]);
        if (p->error) return;
        n++;
        if (p->cur.kind != TOK_COMMA) break;
        advance(p);
    }
    s->column_count = n;
    if (!expect(p, TOK_RPAREN, "expected ')' after CREATE TABLE column list")) return;
    finish_statement(p);
}

// p->cur == TOK_KW_INDEX on entry (CREATE already consumed, see above).
static void parse_create_index_body(struct SqlParser* p, struct SqlCreateIndexStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume INDEX
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected an index name after CREATE INDEX"); return; }
    sq_strcpy(s->index_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);
    if (!expect(p, TOK_KW_ON, "expected ON after CREATE INDEX index name")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after ON"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);
    if (!expect(p, TOK_LPAREN, "expected '(' after CREATE INDEX table name")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name inside CREATE INDEX (...)"); return; }
    sq_strcpy(s->column_name, p->cur.text, RECORD_KEY_LEN);
    advance(p);
    if (!expect(p, TOK_RPAREN, "expected ')' after CREATE INDEX column name")) return;
    finish_statement(p);
}

// p->cur == TOK_KW_TABLE on entry (DROP already consumed, mirroring
// CREATE's own dispatcher-side disambiguation between TABLE/INDEX).
static void parse_drop_table_body(struct SqlParser* p, struct SqlDropTableStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume TABLE
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after DROP TABLE"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);
    finish_statement(p);
}

// p->cur == TOK_KW_INDEX on entry (DROP already consumed).
static void parse_drop_index_body(struct SqlParser* p, struct SqlDropIndexStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume INDEX
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected an index name after DROP INDEX"); return; }
    sq_strcpy(s->index_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);
    finish_statement(p);
}

// ALTER TABLE ... ADD COLUMN only -- no ambiguity to disambiguate (ALTER
// always means ALTER TABLE in this grammar), so unlike CREATE/DROP above,
// this one consumes ALTER itself, matching parse_delete()'s own pattern.
static void parse_alter_table(struct SqlParser* p, struct SqlAlterTableStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume ALTER
    if (!expect(p, TOK_KW_TABLE, "expected TABLE after ALTER")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after ALTER TABLE"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);
    if (!expect(p, TOK_KW_ADD, "expected ADD after ALTER TABLE table name (only ADD COLUMN is supported)")) return;
    if (!expect(p, TOK_KW_COLUMN, "expected COLUMN after ADD")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a column name after ADD COLUMN"); return; }
    sq_strcpy(s->column_name, p->cur.text, RECORD_KEY_LEN);
    advance(p);
    if (!parse_type_kw(p, &s->column_type)) {
        set_error(p, "expected a column type (STRING/UINT64/FLOAT/BOOL/BLOB)");
        return;
    }
    finish_statement(p);
}

int sql_parse(const char* text, struct SqlStatement* out, char* err, uint32_t err_max) {
    if (err && err_max) err[0] = '\0';
    if (!text || !out) {
        if (err && err_max) sq_strcpy(err, "null input", err_max);
        return 1;
    }
    uint32_t len = sq_strlen(text);
    if (len > SQL_MAX_TEXT_LEN) {
        if (err && err_max) sq_strcpy(err, "statement too long", err_max);
        return 1;
    }

    struct SqlParser p;
    p.lx.src = text;
    p.lx.pos = 0;
    p.lx.len = len;
    p.error = 0;
    p.err[0] = '\0';
    advance(&p);   // prime cur with the first token

    sq_memset(out, 0, sizeof(*out));
    switch (p.cur.kind) {
        case TOK_KW_SELECT: out->kind = SQL_STMT_SELECT; parse_select(&p, &out->u.select); break;
        case TOK_KW_INSERT: out->kind = SQL_STMT_INSERT; parse_insert(&p, &out->u.insert); break;
        case TOK_KW_UPDATE: out->kind = SQL_STMT_UPDATE; parse_update(&p, &out->u.update); break;
        case TOK_KW_DELETE: out->kind = SQL_STMT_DELETE; parse_delete(&p, &out->u.del);   break;
        // Phase 5 (SQL Feature-Parity Roadmap): DDL. CREATE and DROP each
        // cover two statement shapes (TABLE vs INDEX) that this single-
        // token-lookahead parser can't tell apart from the CREATE/DROP
        // keyword alone -- consume it here, then dispatch on the next
        // token, before out->kind can even be set.
        case TOK_KW_CREATE:
            advance(&p);   // consume CREATE
            if (p.cur.kind == TOK_KW_TABLE) {
                out->kind = SQL_STMT_CREATE_TABLE;
                parse_create_table_body(&p, &out->u.create_table);
            } else if (p.cur.kind == TOK_KW_INDEX) {
                out->kind = SQL_STMT_CREATE_INDEX;
                parse_create_index_body(&p, &out->u.create_index);
            } else {
                out->kind = SQL_STMT_INVALID;
                set_error(&p, "expected TABLE or INDEX after CREATE");
            }
            break;
        case TOK_KW_ALTER:
            out->kind = SQL_STMT_ALTER_TABLE;
            parse_alter_table(&p, &out->u.alter_table);
            break;
        case TOK_KW_DROP:
            advance(&p);   // consume DROP
            if (p.cur.kind == TOK_KW_TABLE) {
                out->kind = SQL_STMT_DROP_TABLE;
                parse_drop_table_body(&p, &out->u.drop_table);
            } else if (p.cur.kind == TOK_KW_INDEX) {
                out->kind = SQL_STMT_DROP_INDEX;
                parse_drop_index_body(&p, &out->u.drop_index);
            } else {
                out->kind = SQL_STMT_INVALID;
                set_error(&p, "expected TABLE or INDEX after DROP");
            }
            break;
        default:
            out->kind = SQL_STMT_INVALID;
            set_error(&p, "unknown statement -- expected SELECT, INSERT, UPDATE, DELETE, "
                          "CREATE TABLE, ALTER TABLE, DROP TABLE, CREATE INDEX, or DROP INDEX");
            break;
    }

    if (p.error) {
        out->kind = SQL_STMT_INVALID;
        if (err && err_max) sq_strcpy(err, p.err, err_max);
        return 1;
    }
    return 0;
}
