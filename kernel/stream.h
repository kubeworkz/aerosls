#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>
#include <stddef.h>

// ─── Stream store constants ──────────────────────────────────────────────────
#define STREAM_MAX          8       // maximum concurrent stream objects
#define STREAM_DATA_SIZE    65536   // 64 KiB per stream (text, small PDFs, etc.)
#define STREAM_NAME_LEN     64
#define STREAM_MIME_LEN     64

// ─── Stream entry ────────────────────────────────────────────────────────────
struct StreamEntry {
    char     name[STREAM_NAME_LEN];
    char     mime_type[STREAM_MIME_LEN];
    uint8_t  data[STREAM_DATA_SIZE];
    uint32_t size;
    uint8_t  active;
};

// ─── Public API ──────────────────────────────────────────────────────────────
void              stream_init(void);
int               stream_create(const char* name, const char* mime_type);
int               stream_write_chunk(const char* name,
                                     const uint8_t* chunk, uint32_t len,
                                     uint32_t offset, uint8_t is_last);
struct StreamEntry* stream_find(const char* name);
int               stream_list_json(char* buf, int max);

extern struct StreamEntry stream_store[STREAM_MAX];

#endif /* STREAM_H */
