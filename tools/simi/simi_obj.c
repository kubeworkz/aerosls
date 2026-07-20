/* simi_obj.c — .tmo object file read/write (v0.3: adds the name pool) */
#include "simi_obj.h"
#include <stdio.h>
#include <stdlib.h>

int simi_obj_write(const char *path, const SimiObject *obj) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }

    uint32_t magic = SIMI_MAGIC;
    fwrite(&magic, sizeof(uint32_t), 1, f);
    fwrite(&obj->num_instr, sizeof(uint32_t), 1, f);
    fwrite(&obj->num_literals, sizeof(uint32_t), 1, f);
    fwrite(&obj->num_entries, sizeof(uint32_t), 1, f);
    fwrite(&obj->num_names, sizeof(uint32_t), 1, f);
    if (obj->num_instr)    fwrite(obj->instr, sizeof(uint64_t), obj->num_instr, f);
    if (obj->num_literals) fwrite(obj->literals, sizeof(uint64_t), obj->num_literals, f);
    if (obj->num_entries)  fwrite(obj->entries, sizeof(SimiEntry), obj->num_entries, f);
    if (obj->num_names)    fwrite(obj->names, sizeof(SimiName), obj->num_names, f);

    fclose(f);
    return 0;
}

int simi_obj_read(const char *path, SimiObject *obj) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    if (fread(&obj->magic, sizeof(uint32_t), 1, f) != 1 || obj->magic != SIMI_MAGIC) {
        fprintf(stderr, "%s: not a SIMI object (bad magic)\n", path);
        fclose(f);
        return -1;
    }
    if (fread(&obj->num_instr, sizeof(uint32_t), 1, f) != 1 ||
        fread(&obj->num_literals, sizeof(uint32_t), 1, f) != 1 ||
        fread(&obj->num_entries, sizeof(uint32_t), 1, f) != 1 ||
        fread(&obj->num_names, sizeof(uint32_t), 1, f) != 1) {
        fprintf(stderr, "%s: truncated header (not a v0.3 object?)\n", path);
        fclose(f);
        return -1;
    }

    obj->instr = obj->num_instr ? malloc(sizeof(uint64_t) * obj->num_instr) : NULL;
    obj->literals = obj->num_literals ? malloc(sizeof(uint64_t) * obj->num_literals) : NULL;
    obj->entries = obj->num_entries ? malloc(sizeof(SimiEntry) * obj->num_entries) : NULL;
    obj->names = obj->num_names ? malloc(sizeof(SimiName) * obj->num_names) : NULL;

    if (obj->num_instr && fread(obj->instr, sizeof(uint64_t), obj->num_instr, f) != obj->num_instr) {
        fprintf(stderr, "%s: truncated instruction stream\n", path);
        fclose(f); return -1;
    }
    if (obj->num_literals && fread(obj->literals, sizeof(uint64_t), obj->num_literals, f) != obj->num_literals) {
        fprintf(stderr, "%s: truncated literal pool\n", path);
        fclose(f); return -1;
    }
    if (obj->num_entries && fread(obj->entries, sizeof(SimiEntry), obj->num_entries, f) != obj->num_entries) {
        fprintf(stderr, "%s: truncated entry table\n", path);
        fclose(f); return -1;
    }
    if (obj->num_names && fread(obj->names, sizeof(SimiName), obj->num_names, f) != obj->num_names) {
        fprintf(stderr, "%s: truncated name pool\n", path);
        fclose(f); return -1;
    }

    fclose(f);
    return 0;
}

void simi_obj_free(SimiObject *obj) {
    free(obj->instr);
    free(obj->literals);
    free(obj->entries);
    free(obj->names);
    obj->instr = NULL; obj->literals = NULL; obj->entries = NULL; obj->names = NULL;
}
