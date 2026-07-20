/*
 * rv64_exec.c — see rv64_exec.h for what this is and why it exists.
 */
#include "rv64_exec.h"

static int64_t sext(uint32_t val, int bits) {
    uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    uint32_t m = 1u << (bits - 1);
    val &= mask;
    /* (val^m)-m computed in uint32_t arithmetic wraps to a huge unsigned
     * value whenever the result is "negative" — casting THAT straight to
     * int64_t zero-extends the wrapped bit pattern instead of sign-
     * extending the intended negative number. Must land in int32_t first
     * so the wrapped bit pattern is reinterpreted as signed, THEN widen
     * to int64_t, which sign-extends correctly from there. */
    return (int64_t)(int32_t)((val ^ m) - m);
}

static int fetch32(struct RvCpu* cpu, uint64_t addr, uint32_t* out) {
    if (addr + 4 > cpu->mem_size || (addr & 3)) return 0;
    *out = (uint32_t)cpu->mem[addr] | ((uint32_t)cpu->mem[addr+1]<<8) |
           ((uint32_t)cpu->mem[addr+2]<<16) | ((uint32_t)cpu->mem[addr+3]<<24);
    return 1;
}
static int load_mem(struct RvCpu* cpu, uint64_t addr, int width, int is_signed, uint64_t* out) {
    if (addr + (uint64_t)width > cpu->mem_size) return 0;
    uint64_t v = 0;
    for (int i = 0; i < width; i++) v |= ((uint64_t)cpu->mem[addr+i]) << (8*i);
    if (is_signed && width < 8) {
        uint64_t signbit = 1ull << (8*width - 1);
        v = (v ^ signbit) - signbit;
    }
    *out = v;
    return 1;
}
static int store_mem(struct RvCpu* cpu, uint64_t addr, int width, uint64_t val) {
    if (addr + (uint64_t)width > cpu->mem_size) return 0;
    for (int i = 0; i < width; i++) cpu->mem[addr+i] = (uint8_t)((val >> (8*i)) & 0xFF);
    return 1;
}
static void setx(struct RvCpu* cpu, int rd, uint64_t val) {
    if (rd != 0) cpu->x[rd] = val;   /* x0 hardwired to zero */
}

/* v0.3 (Phase 6): host-callback table — see rv64_exec.h's top comment. */
static RvHostFn g_hostfns[RV_EXEC_MAX_HOSTFNS];

void rv64_exec_set_hostfn(int idx, RvHostFn fn) {
    if (idx >= 0 && idx < RV_EXEC_MAX_HOSTFNS) g_hostfns[idx] = fn;
}

const char* rv64_exec_strerror(int code) {
    switch (code) {
        case RV_EXEC_OK: return "ok";
        case RV_EXEC_STEP_LIMIT: return "exceeded step budget (likely infinite loop or decode bug)";
        case RV_EXEC_BAD_INSTR: return "unimplemented or malformed instruction";
        case RV_EXEC_MEM_FAULT: return "fetch/load/store outside guest memory buffer";
        default: return "unknown error";
    }
}

int rv64_exec_run(struct RvCpu* cpu, uint64_t max_steps) {
    for (uint64_t step = 0; step < max_steps; step++) {
        if (cpu->pc == RV_EXEC_SENTINEL_RA) return RV_EXEC_OK;

        uint32_t w;
        if (!fetch32(cpu, cpu->pc, &w)) return RV_EXEC_MEM_FAULT;

        uint32_t opcode = w & 0x7Fu;
        uint32_t rd     = (w>>7) & 0x1Fu;
        uint32_t funct3 = (w>>12) & 0x7u;
        uint32_t rs1    = (w>>15) & 0x1Fu;
        uint32_t rs2    = (w>>20) & 0x1Fu;
        uint32_t funct7 = (w>>25) & 0x7Fu;

        int64_t imm_i = sext((w>>20)&0xFFFu, 12);
        int64_t imm_s = sext((((w>>25)&0x7Fu)<<5) | ((w>>7)&0x1Fu), 12);
        int64_t imm_b = sext((((w>>31)&1u)<<12) | (((w>>7)&1u)<<11) | (((w>>25)&0x3Fu)<<5) | (((w>>8)&0xFu)<<1), 13);
        int64_t imm_u = (int64_t)(int32_t)(w & 0xFFFFF000u);
        int64_t imm_j = sext((((w>>31)&1u)<<20) | (((w>>12)&0xFFu)<<12) | (((w>>20)&1u)<<11) | (((w>>21)&0x3FFu)<<1), 21);

        uint64_t next_pc = cpu->pc + 4;
        uint64_t v1 = cpu->x[rs1], v2 = cpu->x[rs2];

        switch (opcode) {
        case 0x03: { /* LOAD */
            int width, is_signed;
            switch (funct3) {
                case 0x0: width=1; is_signed=1; break;  /* LB */
                case 0x1: width=2; is_signed=1; break;  /* LH */
                case 0x2: width=4; is_signed=1; break;  /* LW */
                case 0x3: width=8; is_signed=0; break;  /* LD */
                case 0x4: width=1; is_signed=0; break;  /* LBU */
                case 0x5: width=2; is_signed=0; break;  /* LHU */
                case 0x6: width=4; is_signed=0; break;  /* LWU */
                default: return RV_EXEC_BAD_INSTR;
            }
            uint64_t out;
            if (!load_mem(cpu, v1 + (uint64_t)imm_i, width, is_signed, &out)) return RV_EXEC_MEM_FAULT;
            setx(cpu, rd, out);
            break;
        }
        case 0x23: { /* STORE */
            int width;
            switch (funct3) { case 0x0: width=1; break; case 0x1: width=2; break;
                               case 0x2: width=4; break; case 0x3: width=8; break;
                               default: return RV_EXEC_BAD_INSTR; }
            if (!store_mem(cpu, v1 + (uint64_t)imm_s, width, v2)) return RV_EXEC_MEM_FAULT;
            break;
        }
        case 0x13: { /* OP-IMM */
            switch (funct3) {
                case 0x0: setx(cpu, rd, v1 + (uint64_t)imm_i); break;                         /* ADDI */
                case 0x4: setx(cpu, rd, v1 ^ (uint64_t)imm_i); break;                          /* XORI */
                case 0x3: setx(cpu, rd, (v1 < (uint64_t)imm_i) ? 1u : 0u); break;               /* SLTIU: imm sign-extended, then compared unsigned per spec */
                case 0x1: setx(cpu, rd, v1 << (imm_i & 0x3F)); break;                          /* SLLI (funct6 assumed 0, not checked) */
                /* Gap Remediation SIMI Phase 12: SRLI/ANDI newly needed by
                 * simi_riscv.c's capability-tag argument-mask codegen
                 * (ld_argmask/emit_call_site/emit_prologue) — nothing
                 * before this phase ever emitted either opcode, so this
                 * decoder never needed to implement them. Same "funct6
                 * assumed 0, not checked" simplification as SLLI above:
                 * every shamt this translator emits is 0-7 (an argument
                 * index), so the SRLI/SRAI distinction (real hardware's
                 * bit 30) never actually matters for anything this
                 * decoder is asked to execute. */
                case 0x5: setx(cpu, rd, v1 >> (imm_i & 0x3F)); break;                          /* SRLI (funct6 assumed 0, not checked) */
                case 0x7: setx(cpu, rd, v1 & (uint64_t)imm_i); break;                          /* ANDI */
                default: return RV_EXEC_BAD_INSTR;
            }
            break;
        }
        case 0x33: { /* OP (register-register) */
            if (funct7 == 0x01) { /* M extension */
                switch (funct3) {
                    case 0x0: setx(cpu, rd, (uint64_t)((int64_t)v1 * (int64_t)v2)); break;               /* MUL */
                    case 0x4: setx(cpu, rd, v2 == 0 ? (uint64_t)-1 : (uint64_t)((int64_t)v1 / (int64_t)v2)); break; /* DIV */
                    case 0x5: setx(cpu, rd, v2 == 0 ? (uint64_t)-1 : v1 / v2); break;                     /* DIVU */
                    case 0x6: setx(cpu, rd, v2 == 0 ? v1 : (uint64_t)((int64_t)v1 % (int64_t)v2)); break; /* REM */
                    case 0x7: setx(cpu, rd, v2 == 0 ? v1 : v1 % v2); break;                               /* REMU */
                    default: return RV_EXEC_BAD_INSTR;
                }
            } else {
                switch (funct3) {
                    case 0x0: setx(cpu, rd, funct7 == 0x20 ? v1 - v2 : v1 + v2); break;   /* SUB / ADD */
                    case 0x1: setx(cpu, rd, v1 << (v2 & 0x3F)); break;                    /* SLL */
                    case 0x2: setx(cpu, rd, (int64_t)v1 < (int64_t)v2 ? 1u : 0u); break;  /* SLT */
                    case 0x3: setx(cpu, rd, v1 < v2 ? 1u : 0u); break;                    /* SLTU */
                    case 0x4: setx(cpu, rd, v1 ^ v2); break;                              /* XOR */
                    case 0x5: setx(cpu, rd, funct7 == 0x20
                                    ? (uint64_t)((int64_t)v1 >> (v2 & 0x3F))              /* SRA */
                                    : (v1 >> (v2 & 0x3F))); break;                        /* SRL */
                    case 0x6: setx(cpu, rd, v1 | v2); break;                              /* OR */
                    case 0x7: setx(cpu, rd, v1 & v2); break;                              /* AND */
                    default: return RV_EXEC_BAD_INSTR;
                }
            }
            break;
        }
        case 0x63: { /* BRANCH */
            int taken;
            switch (funct3) {
                case 0x0: taken = (v1 == v2); break;   /* BEQ */
                case 0x1: taken = (v1 != v2); break;   /* BNE */
                default: return RV_EXEC_BAD_INSTR;     /* BLT/BGE/BLTU/BGEU not emitted by simi_riscv.c */
            }
            if (taken) next_pc = cpu->pc + (uint64_t)imm_b;
            break;
        }
        case 0x6F: /* JAL */
            setx(cpu, rd, cpu->pc + 4);
            next_pc = cpu->pc + (uint64_t)imm_j;
            break;
        case 0x67: { /* JALR */
            if (funct3 != 0) return RV_EXEC_BAD_INSTR;
            uint64_t link = cpu->pc + 4;
            uint64_t target = (v1 + (uint64_t)imm_i) & ~1ull;
            if ((target & RV_EXEC_HOSTFN_MASK) == RV_EXEC_HOSTFN_BASE) {
                /* v0.3 (Phase 6): host-callback sentinel — call the
                 * registered host function instead of fetching guest
                 * instructions at a fake address; a0 (x10) is both the
                 * argument and, per real RV64 ABI convention, where the
                 * result lands. Resume at `link`, exactly as if this had
                 * been a real call that returned immediately. */
                uint32_t idx = (uint32_t)(target & 0xFFFFFFFFull) / RV_EXEC_HOSTFN_STRIDE;
                if (idx >= RV_EXEC_MAX_HOSTFNS || !g_hostfns[idx]) return RV_EXEC_BAD_INSTR;
                uint64_t result = g_hostfns[idx](cpu->x[10]);
                setx(cpu, rd, link);
                cpu->x[10] = result;
                next_pc = link;
            } else {
                next_pc = target;
                setx(cpu, rd, link);
            }
            break;
        }
        case 0x17: /* AUIPC */
            setx(cpu, rd, cpu->pc + (uint64_t)imm_u);
            break;
        case 0x37: /* LUI (unused by simi_riscv.c but harmless to support) */
            setx(cpu, rd, (uint64_t)imm_u);
            break;
        default:
            return RV_EXEC_BAD_INSTR;
        }

        cpu->x[0] = 0;   /* belt-and-suspenders: x0 always reads zero regardless of any setx() call site */
        cpu->pc = next_pc;
    }
    return RV_EXEC_STEP_LIMIT;
}
