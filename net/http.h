#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

// Start the HTTP server — binds to TCP port 3000 and loops forever.
// Should be called from a dedicated kernel thread or AP core.
void http_server_run(void);

// Used by query_engine to build JSON responses into a buffer instead of serial
typedef struct {
    char*  buf;
    int    pos;
    int    max;
} JSONBuf;

void jb_raw(JSONBuf* j, const char* s);
void jb_str(JSONBuf* j, const char* key, const char* val);
void jb_uint(JSONBuf* j, const char* key, uint64_t val);
void jb_obj_open(JSONBuf* j, const char* key);
void jb_obj_close(JSONBuf* j);
void jb_arr_open(JSONBuf* j, const char* key);
void jb_arr_close(JSONBuf* j);

#endif /* HTTP_H */
