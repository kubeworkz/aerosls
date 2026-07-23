#include "stream.h"
#include "kernel_io.h"
#include "object_catalog.h"
#include "partition.h"        // Multitenant Isolation Gap Analysis §5 item 3 -- partition_get_for_uid()
#include "frame_pool.h"        // Multitenant Isolation Gap Analysis §5 item 3 -- allocate_physical_ram_frame_for_partition()
#include "../user/permissions.h"
#include "../drivers/nvme_io.h"

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
//       141-144: owner_uid (uint32) -- Multitenant Isolation Gap Analysis §5 item 3
//       145-148: partition_id (uint32) -- Multitenant Isolation Gap Analysis §5 item 3
//       149-447: padding

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
    uint32_t ou = se->owner_uid, pid = se->partition_id;
    for (int b=0;b<4;b++) entry[141+b] = (uint8_t)(ou  >> (b*8));
    for (int b=0;b<4;b++) entry[145+b] = (uint8_t)(pid >> (b*8));
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
    se->owner_uid = (uint32_t)entry[141]
                  | ((uint32_t)entry[142] << 8)
                  | ((uint32_t)entry[143] << 16)
                  | ((uint32_t)entry[144] << 24);
    se->partition_id = (uint32_t)entry[145]
                      | ((uint32_t)entry[146] << 8)
                      | ((uint32_t)entry[147] << 16)
                      | ((uint32_t)entry[148] << 24);
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
                    req.owner_uid  = stream_store[i].owner_uid;   // Multitenant Isolation Gap Analysis §5 item 3 -- was hardcoded 0
                    req.perm_mask  = PERM_READ | PERM_OWNER;
                    req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
                    req.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
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
// Multitenant Isolation Gap Analysis §5 item 3 / §7 item 3: caller_uid is now
// a real parameter, not silently dropped. Stamped onto the StreamEntry as
// owner_uid, and resolved via partition_get_for_uid() into partition_id --
// every frame this stream ever allocates (here and in stream_write_chunk()/
// stream_lazy_load_frame()) is charged against that partition's quota via
// allocate_physical_ram_frame_for_partition(), closing the gap LPAR Phase
// 13's own findings named and left open: stream/blob storage was the one
// unaccounted, unbounded physical-memory consumer any authenticated caller
// could drive without limit.
int stream_create(uint32_t caller_uid, const char* name, const char* mime_type) {
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
            stream_store[i].owner_uid    = caller_uid;
            stream_store[i].partition_id = partition_get_for_uid(caller_uid);
            for (int f=0;f<STREAM_MAX_FRAMES;f++) stream_store[i].frames[f]=0;

            // Register in catalog + seed metadata records
            struct SLSVallocRequest req;
            st_strncpy(req.name, name, OBJECT_NAME_LEN);
            req.type       = OBJ_TYPE_STREAM;
            req.size_pages = 1;
            req.owner_uid  = caller_uid;   // Multitenant Isolation Gap Analysis §5 item 3 -- was hardcoded 0
            req.perm_mask  = PERM_READ | PERM_OWNER;
            req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
            req.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
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

        // Allocate frame if needed -- charged against this stream's owning
        // partition (Multitenant Isolation Gap Analysis §5 item 3), so a
        // configured frame quota (partition_set_frame_quota()) now actually
        // bounds stream/blob growth the same way it already bounds process
        // stack/code and loader segment growth.
        if (!se->frames[frame_idx]) {
            void* frame = allocate_physical_ram_frame_for_partition(se->partition_id);
            if (!frame) { kernel_serial_print("[STREAM] OOM or quota exceeded.\n"); return 3; }
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
    // Multitenant Isolation Gap Analysis §5 item 3: post-reboot reload is
    // still a real physical allocation -- charge it against the stream's
    // owning partition just like the initial write, not the unaccounted path.
    void* frame = allocate_physical_ram_frame_for_partition(se->partition_id);
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

// ─── stream_relocate_partition ────────────────────────────────────────────────
// Multi-Node Partition Scaling Roadmap Phase 6 addendum ("real migration
// data movement", Multitenant Isolation Gap Analysis §7 item 7): the real,
// previously-nonexistent byte-copy step kernel/partition.c's partition_
// migrate() explicitly disclaimed until now ("This function does not copy
// or move any object data"). For every active slot owned by partition_id,
// physically copies its on-disk bytes (page by page, up to frames_used) to
// a fresh destination slot within the same STREAM_MAX pool, verifies each
// page by reading it back and byte-comparing it against what was read from
// the source, then retires the source slot only after every page verified
// clean. "A fresh slot in the same pool" rather than "a different
// machine's storage" is the honest scope here, not a shortcut: AeroSLS has
// exactly one shared NVMe image, and cluster_init() is never invoked from
// any real boot path (confirmed by search -- only host tests call it), so
// there is no second node's physical storage to move bytes onto yet. This
// is the real, general-purpose relocate-and-verify primitive a future
// actual cross-machine transport would still need to run on its sending
// side before putting bytes on a wire this codebase does not have.
//
// Deliberately scoped to stream/blob storage only, not rowstore/vecstore
// table pages: streams already carry a definite owner_uid/partition_id per
// slot (Multitenant Isolation Gap Analysis §5 item 3) with self-contained,
// individually relocatable byte ranges, while rowstore.c/vecstore.c have no
// per-partition page indexing at all yet -- that is Storage Isolation
// Roadmap Phase 1's job (not yet built), a real prerequisite dependency,
// not an oversight here.
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id) {
    (void)dest_node_id;  // not yet load-bearing -- see header comment above
    if (!(io_sq && io_cq)) return 0;  // no NVMe -- nothing to relocate, honest no-op

    static uint8_t __attribute__((aligned(4096))) reloc_src_page[4096];
    static uint8_t __attribute__((aligned(4096))) reloc_verify_page[4096];

    // Tracks which slots were already used as a RELOCATION DESTINATION this
    // call. Without this, a stream copied from slot i to a later slot
    // dst > i would still be sitting there, active, with partition_id still
    // matching, when the outer loop's index i later reaches dst -- and get
    // "relocated" a second time (to yet another slot), potentially
    // ping-ponging a stream back toward a lower-numbered slot including,
    // worst case, straight back into its own original slot. That bug was
    // caught by this function's own host test (migration_data_movement_
    // host_test.c) reporting an inflated relocated count and a stream that
    // ended up in the exact slot it started in -- this flag is the fix:
    // once a slot has been written to as a destination in this call, it is
    // never treated as a fresh source, even though its active flag and
    // partition_id both now legitimately match.
    uint8_t already_dst[STREAM_MAX];
    for (int k = 0; k < STREAM_MAX; k++) already_dst[k] = 0;

    int relocated = 0;
    for (int i = 0; i < STREAM_MAX; i++) {
        if (already_dst[i]) continue;
        struct StreamEntry* src = &stream_store[i];
        if (!src->active || src->partition_id != partition_id) continue;

        int dst = -1;
        for (int j = 0; j < STREAM_MAX; j++) {
            if (j == i || stream_store[j].active) continue;
            dst = j;
            break;
        }
        if (dst < 0) {
            kernel_serial_printf(
                "[STREAM] relocate: no free slot for partition %u's stream "
                "'%s' -- stopping with %d slot(s) relocated so far.\n",
                (unsigned)partition_id, src->name, relocated);
            return relocated;
        }

        uint64_t src_lba = src->lba_base;
        uint64_t dst_lba = STREAM_DATA_LBA_BASE
                          + (uint64_t)dst * STREAM_SECTORS_PER_SLOT;
        uint32_t pages = src->frames_used;
        int ok = 1;
        for (uint32_t p = 0; p < pages; p++) {
            uint64_t slba = src_lba + (uint64_t)p * 8;
            uint64_t dlba = dst_lba + (uint64_t)p * 8;
            if (nvme_read_sync(slba, reloc_src_page) != 0)    { ok = 0; break; }
            if (nvme_write_sync(dlba, reloc_src_page) != 0)   { ok = 0; break; }
            if (nvme_read_sync(dlba, reloc_verify_page) != 0) { ok = 0; break; }
            int match = 1;
            for (uint32_t b = 0; b < 4096; b++) {
                if (reloc_src_page[b] != reloc_verify_page[b]) { match = 0; break; }
            }
            if (!match) { ok = 0; break; }
        }
        if (!ok) {
            kernel_serial_printf(
                "[STREAM] relocate: copy/verify failed for partition %u's "
                "stream '%s' -- source left intact, stopping with %d slot(s) "
                "relocated so far.\n",
                (unsigned)partition_id, src->name, relocated);
            return relocated;
        }

        // Every page verified byte-for-byte on NVMe -- take over identity in
        // the destination slot.
        struct StreamEntry* d = &stream_store[dst];
        st_strncpy(d->name, src->name, STREAM_NAME_LEN);
        st_strncpy(d->mime_type, src->mime_type, STREAM_MIME_LEN);
        d->size          = src->size;
        d->frames_used   = src->frames_used;
        d->lba_base      = dst_lba;
        d->active        = 1;
        d->owner_uid     = src->owner_uid;
        d->partition_id  = src->partition_id;
        for (uint32_t f = 0; f < STREAM_MAX_FRAMES; f++) d->frames[f] = 0;
        // RAM frame pointers intentionally not copied -- they're a cache of
        // already-loaded pages, not source-of-truth data. The destination
        // slot lazily reloads from its own new LBA range the same way any
        // other slot does, via stream_lazy_load_frame().
        already_dst[dst] = 1;  // never re-relocate this slot within this same call -- see the flag's own declaration comment above

        kernel_serial_printf(
            "[STREAM] relocate: partition %u's stream '%s' moved slot %d -> "
            "%d (%u page(s) copied and verified byte-for-byte).\n",
            (unsigned)partition_id, src->name, i, dst, pages);

        // Retire the source slot. Any RAM frame pointers it still had cached
        // are just dropped here, not individually freed -- partition_
        // migrate()'s own next step (partition_reclaim_all_frames()) reclaims
        // every physical frame this partition owns via frame_owner[]
        // tracking, independent of which stream slot referenced them, so
        // there is no double-free or leak risk in leaving that to the
        // caller's subsequent step.
        st_memset(src->name, 0, STREAM_NAME_LEN);
        st_memset(src->mime_type, 0, STREAM_MIME_LEN);
        src->size         = 0;
        src->frames_used  = 0;
        src->lba_base     = 0;
        src->active       = 0;
        src->owner_uid    = 0;
        src->partition_id = 0;
        for (uint32_t f = 0; f < STREAM_MAX_FRAMES; f++) src->frames[f] = 0;

        relocated++;
    }

    if (relocated > 0) stream_persist_directory();
    return relocated;
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
        SK("frames");    SU(stream_store[i].frames_used); SC(',');
        SK("owner_uid");     SU(stream_store[i].owner_uid);     SC(',');   // Multitenant Isolation Gap Analysis §5 item 3
        SK("partition_id");  SU(stream_store[i].partition_id);
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
