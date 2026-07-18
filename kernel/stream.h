#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>
#include <stddef.h>

// ─── Stream store constants ──────────────────────────────────────────────────
#define STREAM_MAX            8
#define STREAM_MAX_FRAMES     16384    // 16384 × 4 KiB frames = 64 MiB max
#define STREAM_NAME_LEN       64
#define STREAM_MIME_LEN       64

// ─── On-disk layout (sls_storage.img, 512-byte NVMe sectors) ────────────────
// One 4-KiB page = 8 sectors.  All stream I/O is in 4-KiB units (1 frame).
#define STREAM_DIR_LBA        8192ULL   // stream directory: 1 frame (8 sectors)
#define STREAM_DATA_LBA_BASE  65536ULL  // stream data starts here
// Each stream slot occupies STREAM_MAX_FRAMES × 8 sectors = 131072 sectors (64 MiB)
#define STREAM_SECTORS_PER_SLOT  ((uint64_t)STREAM_MAX_FRAMES * 8ULL)

// Compute the first LBA for stream slot i:
//   lba_base(i) = STREAM_DATA_LBA_BASE + i * STREAM_SECTORS_PER_SLOT
// With STREAM_MAX=8: 8 × 131072 × 512 B = 512 MiB used on the 1 GiB NVMe.

// Directory magic (8 bytes, stored in first 8 bytes of the directory page)
#define STREAM_DIR_MAGIC  0x4D525453534C5300ULL   // "SLSSTRMX" little-endian

// ─── Stream entry (RAM) ───────────────────────────────────────────────────────
struct StreamEntry {
    char      name[STREAM_NAME_LEN];
    char      mime_type[STREAM_MIME_LEN];
    uint8_t*  frames[STREAM_MAX_FRAMES];  // NULL = not yet loaded into RAM
    uint32_t  size;
    uint32_t  frames_used;
    uint64_t  lba_base;  // first LBA on NVMe for this stream's data
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
// Lazy-load one 4-KiB frame from NVMe into RAM for a stream entry.
// Called by http_respond_stream when se->frames[i] is NULL after reboot.
uint8_t*           stream_lazy_load_frame(struct StreamEntry* se, uint32_t frame_idx);

extern struct StreamEntry stream_store[STREAM_MAX];

#endif /* STREAM_H */
