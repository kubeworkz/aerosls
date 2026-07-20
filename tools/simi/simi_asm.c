/*
 * simi_asm.c — Phase 1 SIMI assembler.
 *
 * Text .simi source -> binary .tmo object (see simi_isa.h / simi_obj.h).
 *
 * Syntax summary (see README section in this repo's design doc for the
 * rationale; this file is the actual grammar definition):
 *
 *   ; comment to end of line
 *   .entry NAME              ; export label NAME as a callable entry point
 *   label:                   ; define a label at the next instruction
 *   label: MNEMONIC ops...   ; label + instruction on one line
 *
 *   Registers:   r0 .. r1023
 *   Immediates:  #123   #-5
 *   Types:       i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 ptr bool
 *   Relations:   EQ NE LT LE GT GE LTU LEU GTU GEU
 *
 *   ADD/SUB/MUL/DIV/MOD/AND/OR/XOR/SHL/SHR/SAR  rD, rA, rB|#imm, TYPE
 *   NOT/NEG                                     rD, rA, TYPE
 *   MOV                                         rD, rA, TYPE
 *   LOADI                                       rD, #imm, TYPE
 *   LOADI64                                     rD, #imm64, TYPE
 *   CMP                                         rD, rA, rB, TYPE, REL
 *   BR                                          label
 *   BC                                          rA, label [, INV]
 *   CALL                                        label
 *   RET / LEAVE                                 (no operands)
 *   LOAD                                        rD, rBase, #disp, TYPE
 *   STORE                                       rSrc, rBase, #disp, TYPE
 *   LEA                                         rD, rBase, #disp
 *   PTRADD                                      rD, rBase, rIdx|#imm, TYPE
 *   ENTER                                       #nregs
 *
 *   ---- v0.3 (Phase 6): object-typed / ODT-equivalent operations ----
 *   .name NAME                 ; pre-register an object name in the name
 *                               pool (optional — RESOLVE auto-registers a
 *                               name on first use, same as labels don't
 *                               need pre-declaring)
 *   RESOLVE                                     rD, "name" | NAME
 *   OBJSIZE                                     rD, rA, TYPE
 *   OBJTYPE                                     rD, rA, TYPE
 */
#include "simi_isa.h"
#include "simi_obj.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>

#define MAX_LINE 256
#define MAX_INSTR 8192
#define MAX_LABELS 2048
#define MAX_LITERALS 1024
#define MAX_ENTRIES 128
#define MAX_OPERANDS 8
#define MAX_NAMES 256

typedef struct { char name[64]; int index; } Label;

static Label labels[MAX_LABELS];
static int nlabels = 0;

static char entry_names[MAX_ENTRIES][64];
static int nentry_names = 0;

/* v0.3 (Phase 6): object-name pool, referenced by RESOLVE's operand.
 * Populated either explicitly via .name directives or implicitly the
 * first time a given name string is used as a RESOLVE operand — same
 * find-or-add convention as add_literal(), just deduplicated by string
 * instead of accepting duplicates (names are meant to be reused across
 * many RESOLVE sites in the same object, unlike LOADI64 literals). */
static char name_pool[MAX_NAMES][SIMI_MAX_NAME];
static int nname_pool = 0;

static uint64_t out_instr[MAX_INSTR];
static int nout_instr = 0;

static uint64_t out_literals[MAX_LITERALS];
static int nout_literals = 0;

static int errors = 0;
static int cur_line_no = 0;
static const char *cur_file = "";

static void asm_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: error: ", cur_file, cur_line_no);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    errors++;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

static void strip_comment(char *line) {
    char *semi = strchr(line, ';');
    if (semi) *semi = 0;
}

/* Split remainder of an instruction line into mnemonic + operand string */
typedef struct {
    int has_label;
    char label[64];
    int is_directive;
    char directive[32];
    char directive_arg[128];
    int is_instr;
    char mnemonic[32];
    char operand_str[192];
} ClassifiedLine;

static void classify_line(char *raw, ClassifiedLine *c) {
    memset(c, 0, sizeof(*c));
    char buf[MAX_LINE];
    strncpy(buf, raw, MAX_LINE - 1);
    buf[MAX_LINE - 1] = 0;
    strip_comment(buf);
    char *line = trim(buf);
    if (*line == 0) return;

    /* label? look for "ident:" prefix with no space in ident */
    char *colon = strchr(line, ':');
    if (colon) {
        int len = (int)(colon - line);
        int ok = len > 0;
        for (int i = 0; i < len && ok; i++) {
            char ch = line[i];
            if (!(isalnum((unsigned char)ch) || ch == '_')) ok = 0;
        }
        if (ok) {
            c->has_label = 1;
            strncpy(c->label, line, len < 63 ? len : 63);
            c->label[len < 63 ? len : 63] = 0;
            line = trim(colon + 1);
        }
    }
    if (*line == 0) return;

    if (*line == '.') {
        c->is_directive = 1;
        char *sp = line + 1;
        int i = 0;
        while (*sp && !isspace((unsigned char)*sp) && i < 31) c->directive[i++] = *sp++;
        c->directive[i] = 0;
        strncpy(c->directive_arg, trim(sp), sizeof(c->directive_arg) - 1);
        return;
    }

    c->is_instr = 1;
    char *sp = line;
    int i = 0;
    while (*sp && !isspace((unsigned char)*sp) && i < 31) c->mnemonic[i++] = *sp++;
    c->mnemonic[i] = 0;
    strncpy(c->operand_str, trim(sp), sizeof(c->operand_str) - 1);
}

static int split_operands(char *s, char *out[], int max) {
    int n = 0;
    if (*s == 0) return 0;
    char *tok = strtok(s, ",");
    while (tok && n < max) {
        out[n++] = trim(tok);
        tok = strtok(NULL, ",");
    }
    return n;
}

static int find_opcode(const char *name) {
    for (int i = 0; i < OP_COUNT; i++)
        if (strcasecmp(name, SIMI_OPS[i].name) == 0) return i;
    return -1;
}

static int find_type(const char *name) {
    for (int i = 0; i < T_COUNT; i++)
        if (strcasecmp(name, SIMI_TYPE_NAMES[i]) == 0) return i;
    return -1;
}

static int find_rel(const char *name) {
    for (int i = 0; i < REL_COUNT; i++)
        if (strcasecmp(name, SIMI_REL_NAMES[i]) == 0) return i;
    return -1;
}

static int parse_reg(const char *tok) {
    if (tok[0] != 'r' && tok[0] != 'R') return -1;
    const char *p = tok + 1;
    if (!*p) return -1;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) return -1;
    long v = strtol(p, NULL, 10);
    if (v < 0 || v > 1023) return -1;
    return (int)v;
}

static int parse_imm(const char *tok, int64_t *out) {
    if (tok[0] != '#') return 0;
    char *end;
    long long v = strtoll(tok + 1, &end, 0);
    if (*end != 0) return 0;
    *out = v;
    return 1;
}

/* Gap Remediation SIMI Phase 10: float literal syntax for LOADI64 --
 * #<number>f64 or #<number>f32, e.g. #3.14f64, #-1.5f32, #2.5e10f64.
 * Emits the literal's raw IEEE-754 bit pattern (zero-extended to 64 bits
 * for f32) -- the same literal-pool storage LOADI64 already uses for
 * integer literals (docs/AeroSLS-SIMI-ISA-v0.1.md §16 Phase 10, design
 * decision 5: "no new encoding, just a different way to produce the
 * bits"). Returns 1 and fills *out_bits on a real match; returns 0 (not
 * an error) for anything that isn't this syntax, so callers fall through
 * to the existing integer parse_imm() path -- this is a purely additive
 * front-end change. Host-only tool, safe to use strtod/double/float. */
static int parse_float_imm(const char *tok, uint64_t *out_bits) {
    if (tok[0] != '#') return 0;
    size_t len = strlen(tok);
    int is_f64 = (len > 4 && strcmp(tok + len - 3, "f64") == 0);
    int is_f32 = (len > 4 && strcmp(tok + len - 3, "f32") == 0);
    if (!is_f64 && !is_f32) return 0;
    char numbuf[64];
    size_t numlen = len - 1 /* '#' */ - 3 /* "f64"/"f32" */;
    if (numlen == 0 || numlen >= sizeof(numbuf)) return 0;
    memcpy(numbuf, tok + 1, numlen);
    numbuf[numlen] = 0;
    char *end;
    double d = strtod(numbuf, &end);
    if (*end != 0 || numbuf[0] == 0) return 0;   /* not actually a number */
    if (is_f64) {
        union { uint64_t u; double d; } c; c.d = d; *out_bits = c.u;
    } else {
        union { uint32_t u; float f; } c; c.f = (float)d; *out_bits = (uint64_t)c.u;
    }
    return 1;
}

static int find_label(const char *name) {
    for (int i = 0; i < nlabels; i++)
        if (strcmp(labels[i].name, name) == 0) return labels[i].index;
    return -1;
}

static void add_label(const char *name, int index) {
    if (find_label(name) >= 0) { asm_err("duplicate label '%s'", name); return; }
    if (nlabels >= MAX_LABELS) { asm_err("too many labels"); return; }
    strncpy(labels[nlabels].name, name, 63);
    labels[nlabels].name[63] = 0;
    labels[nlabels].index = index;
    nlabels++;
}

/* ---- Pass 1: count instructions, record label positions ------------- */
static void pass1(FILE *f) {
    char line[MAX_LINE];
    int idx = 0;
    cur_line_no = 0;
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        cur_line_no++;
        ClassifiedLine c;
        classify_line(line, &c);
        if (c.has_label) add_label(c.label, idx);
        if (c.is_instr) idx++;
    }
    nout_instr = 0; /* reset for pass2 bookkeeping elsewhere */
    (void)idx;
}

/* ---- Pass 2: emit -----------------------------------------------------
 * Returns via globals out_instr/nout_instr/out_literals/nout_literals.
 */
static void emit(uint64_t w) {
    if (nout_instr >= MAX_INSTR) { asm_err("instruction stream too large"); return; }
    out_instr[nout_instr++] = w;
}

static int add_literal(int64_t v) {
    if (nout_literals >= MAX_LITERALS) { asm_err("literal pool full"); return 0; }
    out_literals[nout_literals] = (uint64_t)v;
    return nout_literals++;
}

/* v0.3 (Phase 6): find-or-add into the object-name pool, deduplicated by
 * exact string match. Truncates to SIMI_MAX_NAME-1 chars, same as
 * SimiName's fixed slot size. */
static int find_or_add_name(const char *name) {
    char buf[SIMI_MAX_NAME];
    strncpy(buf, name, SIMI_MAX_NAME - 1);
    buf[SIMI_MAX_NAME - 1] = 0;
    for (int i = 0; i < nname_pool; i++)
        if (strcmp(name_pool[i], buf) == 0) return i;
    if (nname_pool >= MAX_NAMES) { asm_err("name pool full"); return 0; }
    strcpy(name_pool[nname_pool], buf);
    return nname_pool++;
}

/* Strip a leading/trailing pair of double quotes if present, in place.
 * RESOLVE accepts either "quoted string" or a bare NAME token — SLS
 * object names in this project don't contain spaces/commas, so bare
 * tokens are unambiguous and the quotes are purely cosmetic. */
static char *unquote(char *tok) {
    size_t len = strlen(tok);
    if (len >= 2 && tok[0] == '"' && tok[len - 1] == '"') {
        tok[len - 1] = 0;
        return tok + 1;
    }
    return tok;
}

static void need(int got, int want, const char *mnem) {
    if (got != want)
        asm_err("%s expects %d operand(s), got %d", mnem, want, got);
}

static void assemble_instr(ClassifiedLine *c, int this_idx) {
    int op = find_opcode(c->mnemonic);
    if (op < 0) { asm_err("unknown mnemonic '%s'", c->mnemonic); emit(0); return; }

    char ops_buf[192];
    strncpy(ops_buf, c->operand_str, sizeof(ops_buf) - 1);
    ops_buf[sizeof(ops_buf)-1] = 0;
    char *toks[MAX_OPERANDS];
    int n = split_operands(ops_buf, toks, MAX_OPERANDS);

    SimiFmt fmt = SIMI_OPS[op].fmt;
    int rd = 0, ra = 0, type = 0, flags = 0;
    uint32_t rb = 0;

    switch (fmt) {
    case FMT_RRR: {
        need(n, 4, c->mnemonic);
        if (n < 4) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad src register '%s'", toks[1]); else ra = r;
        int64_t imm;
        if ((r = parse_reg(toks[2])) >= 0) { rb = (uint32_t)r; }
        else if (parse_imm(toks[2], &imm)) {
            if (imm < -134217728LL || imm > 134217727LL) asm_err("immediate out of 28-bit range: %lld", (long long)imm);
            rb = (uint32_t)(imm & 0xFFFFFFF); flags |= FLAG_IMM;
        } else asm_err("bad operand '%s' (expected register or #imm)", toks[2]);
        int t = find_type(toks[3]);
        if (t < 0) asm_err("bad type '%s'", toks[3]); else type = t;
        break;
    }
    case FMT_RR: {
        need(n, 3, c->mnemonic);
        if (n < 3) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad src register '%s'", toks[1]); else ra = r;
        int t = find_type(toks[2]);
        if (t < 0) asm_err("bad type '%s'", toks[2]); else type = t;
        break;
    }
    case FMT_MOV: {
        need(n, 3, c->mnemonic);
        if (n < 3) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad src register '%s'", toks[1]); else ra = r;
        int t = find_type(toks[2]);
        if (t < 0) asm_err("bad type '%s'", toks[2]); else type = t;
        break;
    }
    case FMT_LOADI: {
        need(n, 3, c->mnemonic);
        if (n < 3) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        int64_t imm;
        if (!parse_imm(toks[1], &imm)) asm_err("bad immediate '%s'", toks[1]);
        else if (imm < -134217728LL || imm > 134217727LL) asm_err("immediate out of 28-bit range: %lld (use LOADI64)", (long long)imm);
        else rb = (uint32_t)(imm & 0xFFFFFFF);
        flags |= FLAG_IMM;
        int t = find_type(toks[2]);
        if (t < 0) asm_err("bad type '%s'", toks[2]); else type = t;
        break;
    }
    case FMT_LOADI64: {
        need(n, 3, c->mnemonic);
        if (n < 3) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        /* Gap Remediation SIMI Phase 10: try the float literal syntax
         * first (#3.14f64 / #3.14f32); anything not matching that syntax
         * falls through to the existing integer parse_imm() path
         * unchanged. */
        uint64_t fbits;
        int64_t imm;
        if (parse_float_imm(toks[1], &fbits)) rb = (uint32_t)add_literal((int64_t)fbits);
        else if (!parse_imm(toks[1], &imm)) asm_err("bad immediate '%s'", toks[1]);
        else rb = (uint32_t)add_literal(imm);
        flags |= FLAG_IMM;
        int t = find_type(toks[2]);
        if (t < 0) asm_err("bad type '%s'", toks[2]); else type = t;
        break;
    }
    case FMT_CMP: {
        need(n, 5, c->mnemonic);
        if (n < 5) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad src register '%s'", toks[1]); else ra = r;
        if ((r = parse_reg(toks[2])) < 0) asm_err("bad src register '%s' (CMP is register-only in Phase 1)", toks[2]); else rb = (uint32_t)r;
        int t = find_type(toks[3]);
        if (t < 0) asm_err("bad type '%s'", toks[3]); else type = t;
        int rel = find_rel(toks[4]);
        if (rel < 0) asm_err("bad relation '%s'", toks[4]); else flags = rel;
        break;
    }
    case FMT_BR: {
        need(n, 1, c->mnemonic);
        if (n < 1) { emit(0); return; }
        int target = find_label(toks[0]);
        if (target < 0) asm_err("undefined label '%s'", toks[0]);
        else {
            int32_t disp = target - (this_idx + 1);
            rb = (uint32_t)disp & 0xFFFFFFF;
        }
        break;
    }
    case FMT_BC: {
        if (n != 2 && n != 3) { asm_err("BC expects 2 or 3 operands, got %d", n); emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad condition register '%s'", toks[0]); else ra = r;
        int target = find_label(toks[1]);
        if (target < 0) asm_err("undefined label '%s'", toks[1]);
        else {
            int32_t disp = target - (this_idx + 1);
            rb = (uint32_t)disp & 0xFFFFFFF;
        }
        if (n == 3) {
            if (strcasecmp(toks[2], "INV") == 0) flags |= FLAG_INVERT;
            else asm_err("bad BC modifier '%s' (expected INV)", toks[2]);
        }
        break;
    }
    case FMT_CALL: {
        need(n, 1, c->mnemonic);
        if (n < 1) { emit(0); return; }
        int target = find_label(toks[0]);
        if (target < 0) asm_err("undefined label '%s'", toks[0]);
        else {
            int32_t disp = target - (this_idx + 1);
            rb = (uint32_t)disp & 0xFFFFFFF;
        }
        break;
    }
    case FMT_NONE:
        need(n, 0, c->mnemonic);
        break;
    case FMT_LOAD: {
        need(n, 4, c->mnemonic);
        if (n < 4) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad base register '%s'", toks[1]); else ra = r;
        int64_t imm;
        if (!parse_imm(toks[2], &imm)) asm_err("bad displacement '%s'", toks[2]);
        else rb = (uint32_t)(imm & 0xFFFFFFF);
        int t = find_type(toks[3]);
        if (t < 0) asm_err("bad type '%s'", toks[3]); else type = t;
        break;
    }
    case FMT_STORE: {
        need(n, 4, c->mnemonic);
        if (n < 4) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad src register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad base register '%s'", toks[1]); else ra = r;
        int64_t imm;
        if (!parse_imm(toks[2], &imm)) asm_err("bad displacement '%s'", toks[2]);
        else rb = (uint32_t)(imm & 0xFFFFFFF);
        int t = find_type(toks[3]);
        if (t < 0) asm_err("bad type '%s'", toks[3]); else type = t;
        break;
    }
    case FMT_LEA: {
        need(n, 3, c->mnemonic);
        if (n < 3) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad base register '%s'", toks[1]); else ra = r;
        int64_t imm;
        if (!parse_imm(toks[2], &imm)) asm_err("bad displacement '%s'", toks[2]);
        else rb = (uint32_t)(imm & 0xFFFFFFF);
        type = T_PTR;
        break;
    }
    case FMT_PTRADD: {
        need(n, 4, c->mnemonic);
        if (n < 4) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        if ((r = parse_reg(toks[1])) < 0) asm_err("bad base register '%s'", toks[1]); else ra = r;
        int64_t imm;
        if ((r = parse_reg(toks[2])) >= 0) rb = (uint32_t)r;
        else if (parse_imm(toks[2], &imm)) { rb = (uint32_t)(imm & 0xFFFFFFF); flags |= FLAG_IMM; }
        else asm_err("bad operand '%s'", toks[2]);
        int t = find_type(toks[3]);
        if (t < 0) asm_err("bad type '%s'", toks[3]); else type = t;
        break;
    }
    case FMT_ENTER: {
        need(n, 1, c->mnemonic);
        if (n < 1) { emit(0); return; }
        int64_t imm;
        if (!parse_imm(toks[0], &imm)) asm_err("bad register count '%s'", toks[0]);
        else if (imm < 0 || imm > 1023) asm_err("ENTER register count out of range: %lld", (long long)imm);
        else rb = (uint32_t)imm;
        break;
    }
    case FMT_RESOLVE: {
        /* RESOLVE rD, "name" | NAME  — rD gets the resolved base_vaddr
         * (T_OBJREF) at runtime; rb carries the name-pool index, resolved
         * at assemble time via find-or-add (mirrors LOADI64's literal-pool
         * indexing, minus FLAG_IMM since RESOLVE has no register form). */
        need(n, 2, c->mnemonic);
        if (n < 2) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad dest register '%s'", toks[0]); else rd = r;
        rb = (uint32_t)find_or_add_name(unquote(toks[1]));
        type = T_OBJREF;
        break;
    }
    case FMT_JMPR: {
        /* Gap Remediation SIMI Phase 14: JMPR rA -- rA holds the target
         * as an abstract SIMI instruction index (pc), not a raw address;
         * see simi_isa.h's OP_JMPR comment. Register-only, no label/imm
         * form -- the whole point is the target is a runtime value. */
        need(n, 1, c->mnemonic);
        if (n < 1) { emit(0); return; }
        int r;
        if ((r = parse_reg(toks[0])) < 0) asm_err("bad register '%s'", toks[0]); else ra = r;
        type = T_PTR;
        break;
    }
    }

    emit(simi_encode((uint8_t)op, (uint8_t)type, (uint16_t)rd, (uint16_t)ra, rb, (uint8_t)flags));
}

static void pass2(FILE *f) {
    char line[MAX_LINE];
    int idx = 0;
    cur_line_no = 0;
    rewind(f);
    while (fgets(line, sizeof(line), f)) {
        cur_line_no++;
        ClassifiedLine c;
        classify_line(line, &c);
        if (c.is_directive) {
            if (strcmp(c.directive, "entry") == 0) {
                if (nentry_names >= MAX_ENTRIES) { asm_err("too many .entry directives"); }
                else {
                    strncpy(entry_names[nentry_names], c.directive_arg, 63);
                    entry_names[nentry_names][63] = 0;
                    nentry_names++;
                }
            } else if (strcmp(c.directive, "name") == 0) {
                /* v0.3 (Phase 6): optional pre-registration into the name
                 * pool. Not required — RESOLVE auto-registers on first
                 * use — but lets a program declare the full set of object
                 * names it depends on up front, readable at a glance. */
                find_or_add_name(unquote(c.directive_arg));
            } else {
                asm_err("unknown directive '.%s'", c.directive);
            }
        }
        if (c.is_instr) {
            assemble_instr(&c, idx);
            idx++;
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.simi output.tmo\n", argv[0]);
        return 1;
    }
    cur_file = argv[1];
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }

    pass1(f);
    if (errors) { fprintf(stderr, "%d error(s) in pass 1\n", errors); return 1; }

    pass2(f);
    fclose(f);
    if (errors) { fprintf(stderr, "%d error(s) in pass 2\n", errors); return 1; }

    SimiObject obj = {0};
    obj.num_instr = nout_instr;
    obj.instr = out_instr;
    obj.num_literals = nout_literals;
    obj.literals = out_literals;

    SimiEntry ent[MAX_ENTRIES];
    int nent = 0;
    for (int i = 0; i < nentry_names; i++) {
        int idx = find_label(entry_names[i]);
        if (idx < 0) { fprintf(stderr, "error: .entry '%s' has no matching label\n", entry_names[i]); return 1; }
        strncpy(ent[nent].name, entry_names[i], SIMI_MAX_NAME - 1);
        ent[nent].name[SIMI_MAX_NAME - 1] = 0;
        ent[nent].offset = (uint32_t)idx;
        nent++;
    }
    obj.num_entries = nent;
    obj.entries = ent;

    SimiName names[MAX_NAMES];
    for (int i = 0; i < nname_pool; i++) {
        strncpy(names[i].name, name_pool[i], SIMI_MAX_NAME - 1);
        names[i].name[SIMI_MAX_NAME - 1] = 0;
    }
    obj.num_names = nname_pool;
    obj.names = names;

    if (simi_obj_write(argv[2], &obj) != 0) return 1;
    fprintf(stderr, "assembled %s -> %s (%d instructions, %d literals, %d entries, %d names)\n",
            argv[1], argv[2], nout_instr, nout_literals, nent, nname_pool);
    return 0;
}
