/*
 * timi_isa.h — AeroSLS TIMI v0.3 instruction encoding
 *
 * Matches AeroSLS-TIMI-ISA-v0.1.md (v0.2 revision) plus the v0.3 Phase 6
 * extension (§13): object-typed operations (RESOLVE/OBJSIZE/OBJTYPE) and
 * the T_OBJREF type tag — the ODT-equivalent step where TIMI bytecode can
 * name and introspect real SLS catalog objects, not just scalars/pointers.
 *   64-bit word: opcode(8) | type(4) | rD(10) | rA(10) | rB/imm(28) | flags(4)
 *
 * This is Phase 1 bring-up: the .tmo object format defined here is a
 * minimal flat container (instructions + literal pool + entry table +
 * (v0.3) name pool), NOT the final §5 object-embedding format (that's a
 * Phase 2 concern once this ISA is exercised against the real SLS object
 * system).
 */
#ifndef TIMI_ISA_H
#define TIMI_ISA_H

#include <stdint.h>
#include <stddef.h>

/* ---- Opcodes ------------------------------------------------------- */
typedef enum {
    OP_ADD = 0, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR, OP_SAR, OP_NEG,
    OP_MOV, OP_LOADI, OP_LOADI64,
    OP_CMP,
    OP_BR, OP_BC, OP_CALL, OP_RET,
    OP_LOAD, OP_STORE, OP_LEA, OP_PTRADD,
    OP_ENTER, OP_LEAVE,
    /* ---- v0.3 (Phase 6): object-typed / ODT-equivalent operations ---- */
    OP_RESOLVE,   /* rD = base_vaddr of the SLS object named by name-pool[#idx], T_OBJREF (0 if not found) */
    OP_OBJSIZE,   /* rD = byte size of the object referenced by rA (a T_OBJREF/base_vaddr), 0 if invalid */
    OP_OBJTYPE,   /* rD = OBJ_TYPE_* tag of the object referenced by rA, 0xFFFFFFFF if invalid */
    OP_COUNT
} TimiOpcode;

/* ---- Type tags (4 bits, 13 of 16 used) ------------------------------ */
typedef enum {
    T_I8=0, T_I16, T_I32, T_I64,
    T_U8, T_U16, T_U32, T_U64,
    T_F32, T_F64, T_PTR, T_BOOL,
    T_OBJREF,   /* v0.3: same 64-bit representation as T_PTR (a base_vaddr) —
                 * tagged distinctly because it came from RESOLVE rather than
                 * raw pointer arithmetic. No runtime enforcement difference
                 * in v1; this is the semantic hook Phase 7's capability/
                 * authority model (tagged, unforgeable references) hangs
                 * off of. See §13. */
    T_COUNT
} TimiType;

/* ---- CMP relation codes (packed into the 4-bit flags field) -------- */
typedef enum {
    REL_EQ=0, REL_NE, REL_LT, REL_LE, REL_GT, REL_GE,
    REL_LTU, REL_LEU, REL_GTU, REL_GEU,
    REL_COUNT
} TimiRel;

/* Generic flag bits (opcode-dependent meaning) */
#define FLAG_IMM    0x1u   /* rB/imm field is an immediate, not a register */
#define FLAG_INVERT 0x1u   /* BC: branch if rA == 0 instead of rA != 0 */

/* ---- Bit-field packing ---------------------------------------------- */
static inline uint64_t timi_encode(uint8_t op, uint8_t type,
                                    uint16_t rd, uint16_t ra,
                                    uint32_t rb_imm, uint8_t flags) {
    return ((uint64_t)(op & 0xFFu)        << 56) |
           ((uint64_t)(type & 0xFu)       << 52) |
           ((uint64_t)(rd & 0x3FFu)       << 42) |
           ((uint64_t)(ra & 0x3FFu)       << 32) |
           ((uint64_t)(rb_imm & 0xFFFFFFFu) << 4) |
           ((uint64_t)(flags & 0xFu));
}

static inline uint8_t  timi_op(uint64_t w)    { return (uint8_t)((w >> 56) & 0xFFu); }
static inline uint8_t  timi_type(uint64_t w)  { return (uint8_t)((w >> 52) & 0xFu); }
static inline uint16_t timi_rd(uint64_t w)    { return (uint16_t)((w >> 42) & 0x3FFu); }
static inline uint16_t timi_ra(uint64_t w)    { return (uint16_t)((w >> 32) & 0x3FFu); }
static inline uint32_t timi_rb_raw(uint64_t w){ return (uint32_t)((w >> 4) & 0xFFFFFFFu); }
static inline uint8_t  timi_flags(uint64_t w) { return (uint8_t)(w & 0xFu); }

/* rB field as a register number (low 10 bits) */
static inline uint16_t timi_rb_reg(uint64_t w) {
    return (uint16_t)(timi_rb_raw(w) & 0x3FFu);
}

/* rB field as a sign-extended 28-bit immediate */
static inline int32_t timi_imm28(uint64_t w) {
    uint32_t raw = timi_rb_raw(w) & 0xFFFFFFFu;
    if (raw & 0x8000000u) raw |= 0xF0000000u; /* sign-extend */
    return (int32_t)raw;
}

/* ---- Opcode metadata (shared by assembler/disassembler) ------------- */
typedef enum {
    FMT_RRR,      /* OP rD, rA, rB|imm, TYPE          (arith/logic, LOAD/STORE-like ALU ops) */
    FMT_RR,       /* OP rD, rA, TYPE                  (NOT, NEG) */
    FMT_MOV,      /* MOV rD, rA, TYPE */
    FMT_LOADI,    /* LOADI rD, #imm, TYPE */
    FMT_LOADI64,  /* LOADI64 rD, #imm64, TYPE */
    FMT_CMP,      /* CMP rD, rA, rB, TYPE, REL */
    FMT_BR,       /* BR label */
    FMT_BC,       /* BC rA, label [, INV] */
    FMT_CALL,     /* CALL label */
    FMT_NONE,     /* RET / LEAVE */
    FMT_LOAD,     /* LOAD rD, rBase, #disp, TYPE */
    FMT_STORE,    /* STORE rSrc, rBase, #disp, TYPE */
    FMT_LEA,      /* LEA rD, rBase, #disp */
    FMT_PTRADD,   /* PTRADD rD, rBase, rIdx|imm, TYPE */
    FMT_ENTER,    /* ENTER #nregs */
    FMT_RESOLVE   /* RESOLVE rD, "name"  (v0.3) */
} TimiFmt;

typedef struct {
    const char *name;
    TimiFmt fmt;
} TimiOpInfo;

static const TimiOpInfo TIMI_OPS[OP_COUNT] = {
    [OP_ADD]     = {"ADD",     FMT_RRR},
    [OP_SUB]     = {"SUB",     FMT_RRR},
    [OP_MUL]     = {"MUL",     FMT_RRR},
    [OP_DIV]     = {"DIV",     FMT_RRR},
    [OP_MOD]     = {"MOD",     FMT_RRR},
    [OP_AND]     = {"AND",     FMT_RRR},
    [OP_OR]      = {"OR",      FMT_RRR},
    [OP_XOR]     = {"XOR",     FMT_RRR},
    [OP_NOT]     = {"NOT",     FMT_RR},
    [OP_SHL]     = {"SHL",     FMT_RRR},
    [OP_SHR]     = {"SHR",     FMT_RRR},
    [OP_SAR]     = {"SAR",     FMT_RRR},
    [OP_NEG]     = {"NEG",     FMT_RR},
    [OP_MOV]     = {"MOV",     FMT_MOV},
    [OP_LOADI]   = {"LOADI",   FMT_LOADI},
    [OP_LOADI64] = {"LOADI64", FMT_LOADI64},
    [OP_CMP]     = {"CMP",     FMT_CMP},
    [OP_BR]      = {"BR",      FMT_BR},
    [OP_BC]      = {"BC",      FMT_BC},
    [OP_CALL]    = {"CALL",    FMT_CALL},
    [OP_RET]     = {"RET",     FMT_NONE},
    [OP_LOAD]    = {"LOAD",    FMT_LOAD},
    [OP_STORE]   = {"STORE",   FMT_STORE},
    [OP_LEA]     = {"LEA",     FMT_LEA},
    [OP_PTRADD]  = {"PTRADD",  FMT_PTRADD},
    [OP_ENTER]   = {"ENTER",   FMT_ENTER},
    [OP_LEAVE]   = {"LEAVE",   FMT_NONE},
    [OP_RESOLVE] = {"RESOLVE", FMT_RESOLVE},
    [OP_OBJSIZE] = {"OBJSIZE", FMT_RR},
    [OP_OBJTYPE] = {"OBJTYPE", FMT_RR},
};

static const char *TIMI_TYPE_NAMES[T_COUNT] = {
    "i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","ptr","bool","objref"
};

static const char *TIMI_REL_NAMES[REL_COUNT] = {
    "EQ","NE","LT","LE","GT","GE","LTU","LEU","GTU","GEU"
};

static inline int timi_type_signed(TimiType t) {
    return t == T_I8 || t == T_I16 || t == T_I32 || t == T_I64;
}

/* ---- .tmo object file format (Phase 1 bring-up container, v0.3) ----
 * v0.3 adds a fourth pool — object names, for RESOLVE — after the entry
 * table. This is a breaking change from v0.2 (the header grew a field,
 * num_names, so the fixed-size-arithmetic layout differs). No back-compat
 * shim: every .tmo in this project is regenerated from .timi source by
 * this same toolchain, and nothing external consumes the format yet, so
 * a clean bump was simpler and safer than trying to support two header
 * shapes. See AeroSLS-TIMI-ISA-v0.1.md §13.
 */
#define TIMI_MAGIC 0x314D4954u /* "TIM1" little-endian — unchanged from v0.2, still the same base container idea */
#define TIMI_MAX_NAME 32

typedef struct {
    char name[TIMI_MAX_NAME];
    uint32_t offset; /* instruction index */
} TimiEntry;

/* Object-name pool entry (v0.3) — fixed 32-byte slots, same shape as
 * TimiEntry, referenced by RESOLVE's #idx operand (resolved to an index
 * at assemble time from a `.name "..."` directive or an inline string
 * operand — see timi_asm.c). Deliberately NOT reusing TimiEntry's type:
 * entries map name->instruction-offset (a location in THIS object);
 * names are just strings, resolved against the live SLS catalog at
 * runtime, not against anything inside this object. */
typedef struct {
    char name[TIMI_MAX_NAME];
} TimiName;

typedef struct {
    uint32_t magic;
    uint32_t num_instr;
    uint32_t num_literals;
    uint32_t num_entries;
    uint32_t num_names;    /* v0.3 */
    uint64_t *instr;      /* num_instr words */
    uint64_t *literals;   /* num_literals 64-bit values, for LOADI64 */
    TimiEntry *entries;   /* num_entries (name -> instruction offset) */
    TimiName *names;      /* num_names (v0.3: RESOLVE's name pool) */
} TimiObject;

#endif /* TIMI_ISA_H */
