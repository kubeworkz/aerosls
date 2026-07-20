/*
 * simi_disasm.c — Phase 1 SIMI disassembler.
 *
 * Original label names don't survive assembly (Phase 1's .tmo format has
 * no debug/symbol table beyond the exported .entry names), so branch and
 * call targets are printed as resolved absolute instruction indices
 * (@N) rather than the original mnemonic label.
 */
#include "simi_isa.h"
#include "simi_obj.h"
#include <stdio.h>
#include <string.h>

static const char *entry_name_at(SimiObject *obj, uint32_t idx) {
    for (uint32_t i = 0; i < obj->num_entries; i++)
        if (obj->entries[i].offset == idx) return obj->entries[i].name;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s program.tmo\n", argv[0]);
        return 1;
    }
    SimiObject obj = {0};
    if (simi_obj_read(argv[1], &obj) != 0) return 1;

    printf("; %s — %u instructions, %u literals, %u entries, %u names\n",
           argv[1], obj.num_instr, obj.num_literals, obj.num_entries, obj.num_names);
    for (uint32_t i = 0; i < obj.num_entries; i++)
        printf(".entry %s          ; -> @%u\n", obj.entries[i].name, obj.entries[i].offset);
    if (obj.num_literals) {
        printf("; literal pool:\n");
        for (uint32_t i = 0; i < obj.num_literals; i++)
            printf(";   [%u] = %lld (0x%llx)\n", i,
                   (long long)obj.literals[i], (unsigned long long)obj.literals[i]);
    }
    if (obj.num_names) {
        printf("; name pool (v0.3):\n");
        for (uint32_t i = 0; i < obj.num_names; i++)
            printf(";   [%u] = \"%s\"\n", i, obj.names[i].name);
    }
    printf("\n");

    for (uint32_t pc = 0; pc < obj.num_instr; pc++) {
        uint64_t w = obj.instr[pc];
        uint8_t op = simi_op(w);
        uint8_t type = simi_type(w);
        uint16_t rd = simi_rd(w), ra = simi_ra(w);
        uint8_t flags = simi_flags(w);
        const char *label = entry_name_at(&obj, pc);
        if (label) printf("%s:\n", label);

        const char *mnem = (op < OP_COUNT) ? SIMI_OPS[op].name : "???";
        const char *tname = (type < T_COUNT) ? SIMI_TYPE_NAMES[type] : "?";
        SimiFmt fmt = (op < OP_COUNT) ? SIMI_OPS[op].fmt : FMT_NONE;

        printf("    @%-5u %-8s", pc, mnem);
        switch (fmt) {
        case FMT_RRR:
            if (flags & FLAG_IMM)
                printf("r%u, r%u, #%d, %s", rd, ra, simi_imm28(w), tname);
            else
                printf("r%u, r%u, r%u, %s", rd, ra, simi_rb_reg(w), tname);
            break;
        case FMT_RR:
            printf("r%u, r%u, %s", rd, ra, tname);
            break;
        case FMT_MOV:
            printf("r%u, r%u, %s", rd, ra, tname);
            break;
        case FMT_LOADI:
            printf("r%u, #%d, %s", rd, simi_imm28(w), tname);
            break;
        case FMT_LOADI64:
            printf("r%u, [pool %u], %s", rd, simi_rb_raw(w), tname);
            break;
        case FMT_CMP:
            printf("r%u, r%u, r%u, %s, %s", rd, ra, simi_rb_reg(w), tname,
                   flags < REL_COUNT ? SIMI_REL_NAMES[flags] : "?");
            break;
        case FMT_BR: {
            int32_t disp = simi_imm28(w);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + disp);
            printf("@%u (disp %+d)", target, disp);
            break;
        }
        case FMT_BC: {
            int32_t disp = simi_imm28(w);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + disp);
            printf("r%u, @%u (disp %+d)%s", ra, target, disp, (flags & FLAG_INVERT) ? ", INV" : "");
            break;
        }
        case FMT_CALL: {
            int32_t disp = simi_imm28(w);
            uint32_t target = (uint32_t)((int64_t)pc + 1 + disp);
            const char *nm = entry_name_at(&obj, target);
            printf("@%u%s%s", target, nm ? " ; " : "", nm ? nm : "");
            break;
        }
        case FMT_NONE:
            break;
        case FMT_LOAD:
            printf("r%u, r%u, #%d, %s", rd, ra, simi_imm28(w), tname);
            break;
        case FMT_STORE:
            printf("r%u, r%u, #%d, %s", rd, ra, simi_imm28(w), tname);
            break;
        case FMT_LEA:
            printf("r%u, r%u, #%d", rd, ra, simi_imm28(w));
            break;
        case FMT_PTRADD:
            if (flags & FLAG_IMM)
                printf("r%u, r%u, #%d, %s", rd, ra, simi_imm28(w), tname);
            else
                printf("r%u, r%u, r%u, %s", rd, ra, simi_rb_reg(w), tname);
            break;
        case FMT_ENTER:
            printf("#%u", simi_rb_raw(w));
            break;
        case FMT_RESOLVE: {
            uint32_t idx = simi_rb_raw(w);
            const char *nm = (idx < obj.num_names) ? obj.names[idx].name : "?";
            printf("r%u, \"%s\" ; [pool %u]", rd, nm, idx);
            break;
        }
        case FMT_JMPR:
            printf("r%u", ra);
            break;
        }
        printf("\n");
    }
    simi_obj_free(&obj);
    return 0;
}
