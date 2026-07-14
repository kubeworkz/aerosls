#include "stream.h"
#include "kernel_io.h"
#include "object_catalog.h"
#include "../user/permissions.h"

// ─── Store (BSS — zero-initialised at boot) ───────────────────────────────────
struct StreamEntry stream_store[STREAM_MAX];

// ─── String helpers ───────────────────────────────────────────────────────────
static size_t st_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }
static int    st_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return 0; a++; b++; }
    return *a == *b;
}
static void st_strncpy(char* d, const char* s, size_t n) {
    size_t i; for (i=0;i<n-1&&s[i];i++) d[i]=s[i]; d[i]='\0';
}
static void st_memcpy(void* d, const void* s, size_t n) {
    uint8_t* dd=(uint8_t*)d; const uint8_t* ss=(const uint8_t*)s;
    while(n--) *dd++=*ss++;
}
static void st_uint_to_str(uint64_t v, char* out, int max) {
    if (max<2){out[0]='\0';return;}
    if (v==0){out[0]='0';out[1]='\0';return;}
    char tmp[21]; int len=0;
    while(v&&len<20){tmp[len++]=(char)('0'+v%10);v/=10;}
    int i; for(i=0;i<len&&i<max-1;i++) out[i]=tmp[len-1-i]; out[i]='\0';
}

// ─── stream_init ─────────────────────────────────────────────────────────────
void stream_init(void) {
    for (int i=0;i<STREAM_MAX;i++) {
        stream_store[i].active = 0;
        stream_store[i].size   = 0;
    }
    kernel_serial_print("[STREAM] Stream object store ready ("
                        "8 slots × 64 KiB).\n");
}

// ─── stream_find ─────────────────────────────────────────────────────────────
struct StreamEntry* stream_find(const char* name) {
    for (int i=0;i<STREAM_MAX;i++)
        if (stream_store[i].active && st_streq(stream_store[i].name, name))
            return &stream_store[i];
    return 0;
}

// ─── stream_create ───────────────────────────────────────────────────────────
// Returns 0 on success, non-zero on error.
int stream_create(const char* name, const char* mime_type) {
    if (!name || !name[0]) return 1;
    if (stream_find(name)) {
        kernel_serial_printf("[STREAM] create: '%s' already exists.\n", name);
        return 2;
    }
    for (int i=0;i<STREAM_MAX;i++) {
        if (!stream_store[i].active) {
            st_strncpy(stream_store[i].name,      name,      STREAM_NAME_LEN);
            st_strncpy(stream_store[i].mime_type,
                       (mime_type && mime_type[0]) ? mime_type
                                                   : "application/octet-stream",
                       STREAM_MIME_LEN);
            stream_store[i].size   = 0;
            stream_store[i].active = 1;

            // Register in the SLS object catalog so DB engine hooks fire
            struct SLSVallocRequest req;
            st_strncpy(req.name, name, OBJECT_NAME_LEN);
            req.type       = OBJ_TYPE_STREAM;
            req.size_pages = 16;   // nominal page count for the catalog entry
            req.owner_uid  = 0;
            req.perm_mask  = PERM_READ | PERM_OWNER;
            uint64_t id = sys_sls_valloc(&req);

            // Seed metadata records (status, size, mime_type) so journal,
            // index, and MQT machinery see the object from creation.
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
int stream_write_chunk(const char* name, const uint8_t* chunk,
                        uint32_t len, uint32_t offset, uint8_t is_last) {
    struct StreamEntry* se = stream_find(name);
    if (!se) {
        kernel_serial_printf("[STREAM] write: '%s' not found.\n", name);
        return 1;
    }
    if (offset + len > STREAM_DATA_SIZE) {
        kernel_serial_printf("[STREAM] write: '%s' exceeds 64 KiB limit.\n", name);
        return 2;
    }
    st_memcpy(se->data + offset, chunk, len);
    if (offset + len > se->size) se->size = offset + len;

    if (is_last) {
        // Update metadata records on the final chunk so journal/MQT see it
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
        kernel_serial_printf("[STREAM] '%s' finalised: %u bytes\n",
                             name, se->size);
    }
    return 0;
}

// ─── stream_list_json ────────────────────────────────────────────────────────
int stream_list_json(char* buf, int max) {
    int pos=0;
    // simple inline JSON builder (mirrors the pattern used in http.c)
    #define SC(c) do { if(pos<max-1) buf[pos++]=(c); } while(0)
    #define SS(s) do { const char* _p=(s); while(*_p&&pos<max-1) buf[pos++]=*_p++; } while(0)
    #define SQ(s) do { SC('"'); SS(s); SC('"'); } while(0)
    #define SK(k) do { SQ(k); SC(':'); } while(0)
    #define SU(v) do { char _t[21]; st_uint_to_str((uint64_t)(v),_t,21); SS(_t); } while(0)

    SC('{'); SK("streams"); SC('[');
    int first=1;
    for (int i=0;i<STREAM_MAX;i++) {
        if (!stream_store[i].active) continue;
        if (!first) SC(','); first=0;
        SC('{');
        SK("name");      SQ(stream_store[i].name);      SC(',');
        SK("mime_type"); SQ(stream_store[i].mime_type);  SC(',');
        SK("size");      SU(stream_store[i].size);
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
