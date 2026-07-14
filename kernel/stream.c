#include "stream.h"
#include "kernel_io.h"
#include "object_catalog.h"
#include "../user/permissions.h"
#include "../drivers/nvme_io.h"

extern void* allocate_physical_ram_frame(void);

// ─── Store ───────────────────────────────────────────────────────────────────
struct StreamEntry stream_store[STREAM_MAX];

// ─── String / memory helpers ─────────────────────────────────────────────────
static size_t st_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }
static int    st_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; } return *a==*b;
}
static void st_strncpy(char* d, const char* s, size_t n) {
    size_t i; for(i=0;i<n-1&&s[i];i++) d[i]=s[i]; d[i]='\0';
}
static void st_memcpy(void* d, const void* s, size_t n) {
    uint8_t* dd=(uint8_t*)d; const uint8_t* ss=(const uint8_t*)s;
    while(n--) *dd++=*ss++;
}
static void st_memset(void* d, uint8_t v, size_t n) {
    uint8_t* p=(uint8_t*)d; while(n--) *p++=v;
}
static void st_uint_to_str(uint64_t v, char* out, int max) {
    if(max<2){out[0]='\0';return;}
    if(v==0){out[0]='0';out[1]='\0';return;}
    char tmp[21]; int len=0;
    while(v&&len<20){tmp[len++]=(char)('0'+v%10);v/=10;}
    int i; for(i=0;i<len&&i<max-1;i++) out[i]=tmp[len-1-i]; out[i]='\0';
}

// ─── On-disk directory format ─────────────────────────────────────────────────
// The stream directory occupies exactly one 4-KiB frame (= 8 NVMe sectors)
// at STREAM_DIR_LBA.  Layout:
//
//   Bytes   0-7:    Magic "SLSSTRMX" (uint64 little-endian)
//   Bytes   8-11:   version (uint32)
//   Bytes  12-15:   count of active entries (uint32)
//   Bytes  16-511:  (reserved)
//   Bytes 512-4095: 8 directory entries × 448 bytes each
//     Per entry:
//       0-63:   name[64]
//       64-127: mime_type[64]
//       128-131: size (uint32)
//       132-139: lba_base (uint64)
//       140:    active (uint8)
//       141-447: padding

#define DIR_MAGIC   0x5853525453534C53ULL  // "SLSSTRMX" little-endian
#define DIR_VERSION 1u
#define DIR_HDR_SIZE 512u
#define DIR_ENTRY_SIZE 448u  /* (4096 - 512) / 8 = 448 bytes per entry */

static uint8_t dir_buf[4096];  /* static 4-KiB directory buffer */

static void dir_write_entry(int slot) {
    struct StreamEntry* se = &stream_store[slot];
    uint8_t* entry = dir_buf + DIR_HDR_SIZE + (size_t)slot * DIR_ENTRY_SIZE;
    st_memset(entry, 0, DIR_ENTRY_SIZE);
    st_strncpy((char*)entry,        se->name,      STREAM_NAME_LEN);
    st_strncpy((char*)entry + 64,   se->mime_type, STREAM_MIME_LEN);
    uint32_t sz = se->size;
    entry[128] = (uint8_t)(sz      );
    entry[129] = (uint8_t)(sz >>  8);
    entry[130] = (uint8_t)(sz >> 16);
    entry[131] = (uint8_t)(sz >> 24);
    uint64_t lb = se->lba_base;
    for (int b=0;b<8;b++) entry[132+b] = (uint8_t)(lb >> (b*8));
    entry[140] = se->active;
}

static void dir_read_entry(int slot) {
    const uint8_t* entry = dir_buf + DIR_HDR_SIZE + (size_t)slot * DIR_ENTRY_SIZE;
    struct StreamEntry* se = &stream_store[slot];
    st_strncpy(se->name,      (const char*)entry,       STREAM_NAME_LEN);
    st_strncpy(se->mime_type, (const char*)entry + 64,  STREAM_MIME_LEN);
    se->size = (uint32_t)entry[128]
             | ((uint32_t)entry[129] << 8)
             | ((uint32_t)entry[130] << 16)
             | ((uint32_t)entry[131] << 24);
    uint64_t lb = 0;
    for (int b=0;b<8;b++) lb |= ((uint64_t)entry[132+b]) << (b*8);
    se->lba_base   = lb;
    se->active     = entry[140];
    se->frames_used = 0;
    for (int f=0;f<STREAM_MAX_FRAMES;f++) se->frames[f] = 0;
}

static void stream_persist_directory(void) {
    // Update header in dir_buf
    uint64_t magic   = DIR_MAGIC;
    uint32_t version = DIR_VERSION;
    uint32_t count   = 0;
    for (int i=0;i<STREAM_MAX;i++) if (stream_store[i].active) count++;
    for (int b=0;b<8;b++) dir_buf[b]    = (uint8_t)(magic   >> (b*8));
    for (int b=0;b<4;b++) dir_buf[8+b]  = (uint8_t)(version >> (b*8));
    for (int b=0;b<4;b++) dir_buf[12+b] = (uint8_t)(count   >> (b*8));
    // Refresh all entries
    for (int i=0;i<STREAM_MAX;i++) dir_write_entry(i);
    // Write 4 KiB to NVMe
    int rc = nvme_write_sync(STREAM_DIR_LBA, dir_buf);
    if (rc) kernel_serial_printf("[STREAM] dir persist failed (NVMe rc=%d)\n", rc);
    else    kernel_serial_print ("[STREAM] Directory persisted to NVMe.\n");
}

// ─── stream_init ─────────────────────────────────────────────────────────────
void stream_init(void) {
    // Zero RAM state
    for (int i=0;i<STREAM_MAX;i++) {
        stream_store[i].active      = 0;
        stream_store[i].size        = 0;
        stream_store[i].frames_used = 0;
        stream_store[i].lba_base    = 0;
        for (int f=0;f<STREAM_MAX_FRAMES;f++) stream_store[i].frames[f]=0;
    }

    // Try to restore persisted streams from NVMe directory (if I/O queue is up)
    st_memset(dir_buf, 0, 4096);
    int rc = (io_sq && io_cq) ? nvme_read_sync(STREAM_DIR_LBA, dir_buf) : -1;
    if (rc == 0) {
        uint64_t magic = 0;
        for (int b=0;b<8;b++) magic |= ((uint64_t)dir_buf[b]) << (b*8);
        if (magic == DIR_MAGIC) {
            uint32_t count = (uint32_t)dir_buf[12]
                           | ((uint32_t)dir_buf[13] << 8)
                           | ((uint32_t)dir_buf[14] << 16)
                           | ((uint32_t)dir_buf[15] << 24);
            kernel_serial_printf(
                "[STREAM] Restoring %u stream(s) from NVMe directory.\n", count);
            for (int i=0;i<STREAM_MAX;i++) {
                dir_read_entry(i);
                if (stream_store[i].active) {
                    // Re-register in object catalog so REST API sees it
                    struct SLSVallocRequest req;
                    st_strncpy(req.name, stream_store[i].name, OBJECT_NAME_LEN);
                    req.type       = OBJ_TYPE_STREAM;
                    req.size_pages = 1;
                    req.owner_uid  = 0;
                    req.perm_mask  = PERM_READ | PERM_OWNER;
                    sys_sls_valloc(&req);
                    // Metadata records are re-inserted so DB hooks see them
                    struct SLSRecordRequest mr;
                    int j;
                    for(j=0;j<OBJECT_NAME_LEN-1&&stream_store[i].name[j];j++)
                        mr.name[j]=stream_store[i].name[j]; mr.name[j]='\0';
                    char szstr[12]; st_uint_to_str(stream_store[i].size, szstr, 12);
                    const char* stk="status";
                    for(j=0;stk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=stk[j]; mr.key[j]='\0';
                    mr.value[0]='r';mr.value[1]='e';mr.value[2]='a';mr.value[3]='d';
                    mr.value[4]='y';mr.value[5]='\0';
                    sys_sls_insert(&mr);
                    const char* szk="byte_size";
                    for(j=0;szk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=szk[j]; mr.key[j]='\0';
                    st_strncpy(mr.value, szstr, RECORD_VAL_LEN);
                    sys_sls_insert(&mr);
                    const char* mtk="mime_type";
                    for(j=0;mtk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=mtk[j]; mr.key[j]='\0';
                    st_strncpy(mr.value, stream_store[i].mime_type, RECORD_VAL_LEN);
                    sys_sls_insert(&mr);
                    kernel_serial_printf("[STREAM] Restored '%s' (%u bytes)\n",
                                         stream_store[i].name, stream_store[i].size);
                }
            }
        } else {
            kernel_serial_print("[STREAM] No directory on NVMe — cold start.\n");
        }
    } else {
        kernel_serial_printf("[STREAM] NVMe read failed (rc=%d) — cold start.\n", rc);
    }
    kernel_serial_print("[STREAM] Stream store ready (frame-pool + NVMe persistence).\n");
    (void)st_strlen;
}

// ─── stream_find ─────────────────────────────────────────────────────────────
struct StreamEntry* stream_find(const char* name) {
    for (int i=0;i<STREAM_MAX;i++)
        if (stream_store[i].active && st_streq(stream_store[i].name, name))
            return &stream_store[i];
    return 0;
}

// ─── stream_create ───────────────────────────────────────────────────────────
int stream_create(const char* name, const char* mime_type) {
    if (!name || !name[0]) return 1;
    if (stream_find(name)) {
        kernel_serial_printf("[STREAM] create: '%s' already exists.\n", name);
        return 2;
    }
    for (int i=0;i<STREAM_MAX;i++) {
        if (!stream_store[i].active) {
            st_strncpy(stream_store[i].name, name, STREAM_NAME_LEN);
            st_strncpy(stream_store[i].mime_type,
                       (mime_type && mime_type[0]) ? mime_type
                                                   : "application/octet-stream",
                       STREAM_MIME_LEN);
            stream_store[i].size        = 0;
            stream_store[i].frames_used = 0;
            stream_store[i].active      = 1;
            stream_store[i].lba_base    = STREAM_DATA_LBA_BASE
                                        + (uint64_t)i * STREAM_SECTORS_PER_SLOT;
            for (int f=0;f<STREAM_MAX_FRAMES;f++) stream_store[i].frames[f]=0;

            // Register in catalog + seed metadata records
            struct SLSVallocRequest req;
            st_strncpy(req.name, name, OBJECT_NAME_LEN);
            req.type       = OBJ_TYPE_STREAM;
            req.size_pages = 1;
            req.owner_uid  = 0;
            req.perm_mask  = PERM_READ | PERM_OWNER;
            uint64_t id = sys_sls_valloc(&req);
            if (id) {
                struct SLSRecordRequest mr;
                int j;
                for(j=0;j<OBJECT_NAME_LEN-1&&name[j];j++) mr.name[j]=name[j]; mr.name[j]='\0';
                const char* stk="status";
                for(j=0;stk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=stk[j]; mr.key[j]='\0';
                mr.value[0]='c';mr.value[1]='r';mr.value[2]='e';mr.value[3]='a';
                mr.value[4]='t';mr.value[5]='e';mr.value[6]='d';mr.value[7]='\0';
                sys_sls_insert(&mr);
                const char* szk="byte_size";
                for(j=0;szk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=szk[j]; mr.key[j]='\0';
                mr.value[0]='0';mr.value[1]='\0';
                sys_sls_insert(&mr);
                const char* mtk="mime_type";
                for(j=0;mtk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=mtk[j]; mr.key[j]='\0';
                st_strncpy(mr.value, stream_store[i].mime_type, RECORD_VAL_LEN);
                sys_sls_insert(&mr);
            }
            kernel_serial_printf("[STREAM] Created '%s' (slot %d, lba_base=%llu)\n",
                                 name, i, (unsigned long long)stream_store[i].lba_base);
            return 0;
        }
    }
    kernel_serial_print("[STREAM] create: store full.\n");
    return 3;
}

// ─── stream_write_chunk ──────────────────────────────────────────────────────
int stream_write_chunk(const char* name, const uint8_t* chunk,
                        uint32_t len, uint32_t offset, uint8_t is_last) {
    struct StreamEntry* se = stream_find(name);
    if (!se) {
        kernel_serial_printf("[STREAM] write: '%s' not found.\n", name);
        return 1;
    }
    if ((uint64_t)offset + len > (uint64_t)STREAM_MAX_FRAMES * 4096) {
        kernel_serial_printf("[STREAM] write: '%s' exceeds 64 MiB limit.\n", name);
        return 2;
    }

    uint32_t written = 0;
    while (written < len) {
        uint32_t abs_byte  = offset + written;
        uint32_t frame_idx = abs_byte / 4096;
        uint32_t frame_off = abs_byte % 4096;

        // Allocate frame if needed
        if (!se->frames[frame_idx]) {
            void* frame = allocate_physical_ram_frame();
            if (!frame) { kernel_serial_print("[STREAM] OOM.\n"); return 3; }
            st_memset(frame, 0, 4096);
            se->frames[frame_idx] = (uint8_t*)frame;
            if (frame_idx + 1 > se->frames_used)
                se->frames_used = frame_idx + 1;
        }

        uint32_t can  = 4096 - frame_off;
        uint32_t todo = len - written;
        if (todo > can) todo = can;
        st_memcpy(se->frames[frame_idx] + frame_off, chunk + written, todo);
        written += todo;
    }
    if (offset + len > se->size) se->size = offset + len;

    if (is_last) {
        // Flush all populated frames to NVMe synchronously
        uint32_t flushed = 0;
        for (uint32_t fi = 0; fi < se->frames_used; fi++) {
            if (!se->frames[fi]) continue;
            uint64_t lba = se->lba_base + (uint64_t)fi * 8;
            int rc = nvme_write_sync(lba, se->frames[fi]);
            if (rc) {
                kernel_serial_printf("[STREAM] NVMe write frame %u failed rc=%d\n",
                                     fi, rc);
            } else {
                flushed++;
            }
        }
        // Update metadata records
        struct SLSRecordRequest mr;
        int j;
        for(j=0;j<OBJECT_NAME_LEN-1&&name[j];j++) mr.name[j]=name[j]; mr.name[j]='\0';
        const char* szk="byte_size";
        for(j=0;szk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=szk[j]; mr.key[j]='\0';
        st_uint_to_str((uint64_t)se->size, mr.value, RECORD_VAL_LEN);
        sys_sls_update(&mr);
        const char* stk="status";
        for(j=0;stk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=stk[j]; mr.key[j]='\0';
        mr.value[0]='r';mr.value[1]='e';mr.value[2]='a';mr.value[3]='d';
        mr.value[4]='y';mr.value[5]='\0';
        sys_sls_update(&mr);
        // Persist directory
        stream_persist_directory();
        kernel_serial_printf("[STREAM] '%s': %u bytes, %u frames flushed to NVMe.\n",
                             name, se->size, flushed);
    }
    return 0;
}

// ─── stream_lazy_load_frame ───────────────────────────────────────────────────
// Called by http_respond_stream when a frame is NULL (post-reboot lazy load).
uint8_t* stream_lazy_load_frame(struct StreamEntry* se, uint32_t frame_idx) {
    void* frame = allocate_physical_ram_frame();
    if (!frame) return 0;
    uint64_t lba = se->lba_base + (uint64_t)frame_idx * 8;
    int rc = nvme_read_sync(lba, frame);
    if (rc) {
        kernel_serial_printf("[STREAM] NVMe read frame %u failed rc=%d\n",
                             frame_idx, rc);
        return 0;
    }
    se->frames[frame_idx] = (uint8_t*)frame;
    if (frame_idx + 1 > se->frames_used) se->frames_used = frame_idx + 1;
    return (uint8_t*)frame;
}

// ─── stream_list_json ────────────────────────────────────────────────────────
int stream_list_json(char* buf, int max) {
    int pos=0;
    #define SC(c) do{if(pos<max-1)buf[pos++]=(c);}while(0)
    #define SS(s) do{const char*_p=(s);while(*_p&&pos<max-1)buf[pos++]=*_p++;}while(0)
    #define SQ(s) do{SC('"');SS(s);SC('"');}while(0)
    #define SK(k) do{SQ(k);SC(':');}while(0)
    #define SU(v) do{char _t[21];st_uint_to_str((uint64_t)(v),_t,21);SS(_t);}while(0)

    SC('{'); SK("streams"); SC('[');
    int first=1;
    for (int i=0;i<STREAM_MAX;i++) {
        if (!stream_store[i].active) continue;
        if (!first) SC(','); first=0;
        SC('{');
        SK("name");      SQ(stream_store[i].name);       SC(',');
        SK("mime_type"); SQ(stream_store[i].mime_type);   SC(',');
        SK("size");      SU(stream_store[i].size);        SC(',');
        SK("frames");    SU(stream_store[i].frames_used);
        SC('}');
    }
    SC(']'); SC('}');
    buf[pos]='\0';
    return pos;
    #undef SC
    #undef SS
    #undef SQ
    #undef SK
    #undef SU
}
