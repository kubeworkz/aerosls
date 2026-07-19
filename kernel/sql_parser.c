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
    TOK_KW_TRUE, TOK_KW_FALSE,
    TOK_STAR, TOK_COMMA, TOK_LPAREN, TOK_RPAREN, TOK_SEMI, TOK_DOT,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_ERROR,
} SqlTokenKind;

struct SqlToken {
    SqlTokenKind kind;
    char text[RECORD_VAL_LEN];
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
    {"TRUE", TOK_KW_TRUE}, {"FALSE", TOK_KW_FALSE},
};
#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

static struct SqlToken lex_next(struct SqlLexer* lx) {
    struct SqlToken t; t.text[0] = '\0';
    while (lx->pos < lx->len) {
        char ws = lx->src[lx->pos];
        if (ws == ' ' || ws == '\t' || ws == '\n' || ws == '\r') { lx->pos++; continue; }
        break;
    }
    if (lx->pos >= lx->len) { t.kind = TOK_EOF; return t; }

    char c = lx->src[lx->pos];

    // number: digit, or '-' immediately followed by a digit (a leading
    // sign is part of the literal -- there is no subtraction operator in
    // this grammar, no arithmetic expressions per scope).
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

// comparison := column_ref compare_op literal
static uint32_t parse_comparison(struct SqlParser* p, struct Predicate* pred) {
    char col[RECORD_KEY_LEN];
    if (!parse_column_ref(p, col, RECORD_KEY_LEN)) return PREDICATE_INVALID_NODE;

    PredicateCompareOp op;
    if (!parse_compare_op(p, &op)) return PREDICATE_INVALID_NODE;

    char lit[RECORD_VAL_LEN];
    if (!parse_literal(p, lit, RECORD_VAL_LEN)) return PREDICATE_INVALID_NODE;

    uint32_t node = predicate_add_comparison(pred, col, op, lit);
    if (node == PREDICATE_INVALID_NODE) set_error(p, "WHERE clause too complex (predicate node pool exhausted)");
    return node;
}

// and_expr := comparison (AND comparison)*
static uint32_t parse_and_expr(struct SqlParser* p, struct Predicate* pred) {
    uint32_t left = parse_comparison(p, pred);
    if (p->error) return PREDICATE_INVALID_NODE;
    while (p->cur.kind == TOK_KW_AND) {
        advance(p);
        uint32_t right = parse_comparison(p, pred);
        if (p->error) return PREDICATE_INVALID_NODE;
        left = predicate_add_and(pred, left, right);
        if (left == PREDICATE_INVALID_NODE) { set_error(p, "WHERE clause too complex (predicate node pool exhausted)"); return left; }
    }
    return left;
}

// predicate := and_expr (OR and_expr)*  -- AND binds tighter than OR for free, no parens needed
static uint32_t parse_predicate(struct SqlParser* p, struct Predicate* pred) {
    uint32_t left = parse_and_expr(p, pred);
    if (p->error) return PREDICATE_INVALID_NODE;
    while (p->cur.kind == TOK_KW_OR) {
        advance(p);
        uint32_t right = parse_and_expr(p, pred);
        if (p->error) return PREDICATE_INVALID_NODE;
        left = predicate_add_or(pred, left, right);
        if (left == PREDICATE_INVALID_NODE) { set_error(p, "WHERE clause too complex (predicate node pool exhausted)"); return left; }
    }
    return left;
}

static void finish_statement(struct SqlParser* p) {
    if (p->error) return;
    if (p->cur.kind == TOK_SEMI) advance(p);
    if (p->cur.kind != TOK_EOF) set_error(p, "unexpected trailing tokens after statement");
}

static void parse_select(struct SqlParser* p, struct SqlSelectStmt* s) {
    sq_memset(s, 0, sizeof(*s));
    advance(p);   // consume SELECT

    if (p->cur.kind == TOK_STAR) {
        s->select_all = 1;
        advance(p);
    } else {
        uint32_t n = 0;
        for (;;) {
            if (n >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many columns in SELECT list"); return; }
            if (!parse_column_ref(p, s->columns[n], RECORD_KEY_LEN)) return;
            n++;
            if (p->cur.kind != TOK_COMMA) break;
            advance(p);
        }
        s->column_count = n;
    }

    if (!expect(p, TOK_KW_FROM, "expected FROM")) return;
    if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after FROM"); return; }
    sq_strcpy(s->table_name, p->cur.text, OBJECT_NAME_LEN);
    advance(p);

    // Phase 20: join_clause := JOIN ident ON column_ref '=' column_ref
    // Both halves of the ON clause MUST be qualified ("table.column") --
    // parse_column_ref() allows but doesn't require a qualifier in
    // general, so that's enforced here specifically via split_qualified()
    // rejecting an unqualified reference, not by the grammar rule itself.
    if (p->cur.kind == TOK_KW_JOIN) {
        advance(p);
        if (p->cur.kind != TOK_IDENT) { set_error(p, "expected a table name after JOIN"); return; }
        sq_strcpy(s->join_table, p->cur.text, OBJECT_NAME_LEN);
        advance(p);
        if (!expect(p, TOK_KW_ON, "expected ON after JOIN table name")) return;

        char left[RECORD_KEY_LEN];
        if (!parse_column_ref(p, left, RECORD_KEY_LEN)) return;
        if (!expect(p, TOK_EQ, "expected '=' in ON clause")) return;
        char right[RECORD_KEY_LEN];
        if (!parse_column_ref(p, right, RECORD_KEY_LEN)) return;

        if (!split_qualified(left, s->join_left_qualifier, OBJECT_NAME_LEN, s->join_left_col, RECORD_KEY_LEN)) {
            set_error(p, "ON clause's left side must be table.column");
            return;
        }
        if (!split_qualified(right, s->join_right_qualifier, OBJECT_NAME_LEN, s->join_right_col, RECORD_KEY_LEN)) {
            set_error(p, "ON clause's right side must be table.column");
            return;
        }
        s->has_join = 1;
    }

    if (p->cur.kind == TOK_KW_WHERE) {
        advance(p);
        predicate_init(&s->where);
        s->where.root = parse_predicate(p, &s->where);
        if (p->error) return;
        s->has_where = 1;
    }

    if (p->cur.kind == TOK_KW_ORDER) {
        advance(p);
        if (!expect(p, TOK_KW_BY, "expected BY after ORDER")) return;
        if (!parse_column_ref(p, s->order_by, RECORD_KEY_LEN)) return;
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
    if (!expect(p, TOK_LPAREN, "expected '(' after VALUES")) return;
    uint32_t vcount = 0;
    for (;;) {
        char lit[RECORD_VAL_LEN];
        if (!parse_literal(p, lit, RECORD_VAL_LEN)) return;
        if (vcount >= ROWSTORE_MAX_COLUMNS) { set_error(p, "too many values"); return; }
        sq_strcpy(s->values[vcount++], lit, RECORD_VAL_LEN);
        if (p->cur.kind != TOK_COMMA) break;
        advance(p);
    }
    if (!expect(p, TOK_RPAREN, "expected ')' after value list")) return;

    if (vcount != ccount) { set_error(p, "column count doesn't match value count"); return; }
    s->count = ccount;

    finish_statement(p);
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
        if (!parse_literal(p, lit, RECORD_VAL_LEN)) return;
        sq_strcpy(s->set_columns[n], col, RECORD_KEY_LEN);
        sq_strcpy(s->set_values[n], lit, RECORD_VAL_LEN);
        n++;
        if (p->cur.kind != TOK_COMMA) break;
        advance(p);
    }
    s->set_count = n;

    if (p->cur.kind == TOK_KW_WHERE) {
        advance(p);
        predicate_init(&s->where);
        s->where.root = parse_predicate(p, &s->where);
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
        s->where.root = parse_predicate(p, &s->where);
        if (p->error) return;
        s->has_where = 1;
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
        default:
            out->kind = SQL_STMT_INVALID;
            set_error(&p, "unknown statement -- expected SELECT, INSERT, UPDATE, or DELETE");
            break;
    }

    if (p.error) {
        out->kind = SQL_STMT_INVALID;
        if (err && err_max) sq_strcpy(err, p.err, err_max);
        return 1;
    }
    return 0;
}
