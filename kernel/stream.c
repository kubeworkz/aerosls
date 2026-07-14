#include "stream.h"
#include "kernel_io.h"
#include "object_catalog.h"
#include "../user/permissions.h"

// Physical frame allocator (frame_pool.c has no header)
extern void* allocate_physical_ram_frame(void);

// ─── Store ───────────────────────────────────────────────────────────────────
struct StreamEntry stream_store[STREAM_MAX];

// ─── Helpers ─────────────────────────────────────────────────────────────────
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

// ─── stream_init ─────────────────────────────────────────────────────────────
void stream_init(void) {
    for (int i=0;i<STREAM_MAX;i++) {
        stream_store[i].active      = 0;
        stream_store[i].size        = 0;
        stream_store[i].frames_used = 0;
        for (int f=0;f<STREAM_MAX_FRAMES;f++) stream_store[i].frames[f] = 0;
    }
    kernel_serial_print("[STREAM] Stream object store ready "
                        "(8 slots, up to 1 MiB each, frame-pool backed).\n");
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
            for (int f=0;f<STREAM_MAX_FRAMES;f++) stream_store[i].frames[f]=0;

            // Register in object catalog so DB engine hooks fire
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
                mr.value[0]='0'; mr.value[1]='\0';
                sys_sls_insert(&mr);
                const char* mtk="mime_type";
                for(j=0;mtk[j]&&j<RECORD_KEY_LEN-1;j++) mr.key[j]=mtk[j]; mr.key[j]='\0';
                st_strncpy(mr.value, stream_store[i].mime_type, RECORD_VAL_LEN);
                sys_sls_insert(&mr);
            }

            kernel_serial_printf("[STREAM] Created '%s' (mime=%s)\n",
                                 name, stream_store[i].mime_type);
            return 0;
        }
    }
    kernel_serial_print("[STREAM] create: store full.\n");
    return 3;
}

// ─── stream_write_chunk ──────────────────────────────────────────────────────
// Writes [chunk, chunk+len) at byte offset [offset] into the named stream.
// Frames are allocated lazily from the physical frame pool as needed.
// Each frame covers exactly 4096 bytes.  A chunk may span multiple frames.
int stream_write_chunk(const char* name, const uint8_t* chunk,
                        uint32_t len, uint32_t offset, uint8_t is_last) {
    struct StreamEntry* se = stream_find(name);
    if (!se) {
        kernel_serial_printf("[STREAM] write: '%s' not found.\n", name);
        return 1;
    }
    if ((uint64_t)offset + len > (uint64_t)STREAM_MAX_FRAMES * 4096) {
        kernel_serial_printf("[STREAM] write: '%s' exceeds 1 MiB limit.\n", name);
        return 2;
    }

    uint32_t written = 0;
    while (written < len) {
        uint32_t abs_byte   = offset + written;
        uint32_t frame_idx  = abs_byte / 4096;
        uint32_t frame_off  = abs_byte % 4096;

        // Allocate this frame if not yet present
        if (!se->frames[frame_idx]) {
            void* frame = allocate_physical_ram_frame();
            if (!frame) {
                kernel_serial_print("[STREAM] write: out of physical frames.\n");
                return 3;
            }
            st_memset(frame, 0, 4096);
            se->frames[frame_idx] = (uint8_t*)frame;
            if (frame_idx + 1 > se->frames_used)
                se->frames_used = frame_idx + 1;
        }

        // Copy into this frame (up to end of frame or end of chunk)
        uint32_t can_write = 4096 - frame_off;
        uint32_t to_write  = len - written;
        if (to_write > can_write) to_write = can_write;
        st_memcpy(se->frames[frame_idx] + frame_off, chunk + written, to_write);
        written += to_write;
    }

    if (offset + len > se->size) se->size = offset + len;

    if (is_last) {
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
        kernel_serial_printf("[STREAM] '%s' finalised: %u bytes across %u frame(s)\n",
                             name, se->size, se->frames_used);
    }
    (void)st_strlen;
    return 0;
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
