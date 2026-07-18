/* timi_obj.h — .tmo object file read/write, shared by asm/interp/disasm */
#ifndef TIMI_OBJ_H
#define TIMI_OBJ_H

#include "timi_isa.h"

/* Returns 0 on success, -1 on error (message printed to stderr). */
int timi_obj_write(const char *path, const TimiObject *obj);
int timi_obj_read(const char *path, TimiObject *obj);
void timi_obj_free(TimiObject *obj);

#endif
