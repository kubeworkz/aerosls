#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>
#include <stddef.h>

// ─── Stream store constants ──────────────────────────────────────────────────
#define STREAM_MAX          8
#define STREAM_MAX_FRAMES   16384    // 16384 × 4 KiB frames = 64 MiB max per stream
#define STREAM_NAME_LEN     64
#define STREAM_MIME_LEN     64

// ─── Stream entry ────────────────────────────────────────────────────────────
// Data is stored in lazily-allocated 4-KiB frames from the physical frame pool
// (identity-mapped: phys == virt for kernel use).  No static payload array —
// BSS cost is 8 × ~2 KiB (pointer table) ≈ 16 KiB total; actual file data
// is allocated from RAM on demand, so files up to 1 MiB are supported with
// zero BSS penalty.
struct StreamEntry {
    char      name[STREAM_NAME_LEN];
    char      mime_type[STREAM_MIME_LEN];
    uint8_t*  frames[STREAM_MAX_FRAMES]; // NULL until the frame is first written
    uint32_t  size;                      // total bytes written
    uint32_t  frames_used;               // highest frame index allocated + 1
    uint8_t   active;
};

// ─── Public API ──────────────────────────────────────────────────────────────
void               stream_init(void);
int                stream_create(const char* name, const char* mime_type);
int                stream_write_chunk(const char* name,
                                      const uint8_t* chunk, uint32_t len,
                                      uint32_t offset, uint8_t is_last);
struct StreamEntry* stream_find(const char* name);
int                stream_list_json(char* buf, int max);

extern struct StreamEntry stream_store[STREAM_MAX];

#endif /* STREAM_H */
