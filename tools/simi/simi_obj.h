/* simi_obj.h — .tmo object file read/write, shared by asm/interp/disasm */
#ifndef SIMI_OBJ_H
#define SIMI_OBJ_H

#include "simi_isa.h"

/* Returns 0 on success, -1 on error (message printed to stderr). */
int simi_obj_write(const char *path, const SimiObject *obj);
int simi_obj_read(const char *path, SimiObject *obj);
void simi_obj_free(SimiObject *obj);

#endif
