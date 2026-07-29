#include "stream.h"
#include "kernel_io.h"
#include "object_catalog.h"
#include "partition.h"        // Multitenant Isolation Gap Analysis §5 item 3 -- partition_get_for_uid()
#include "frame_pool.h"        // Multitenant Isolation Gap Analysis §5 item 3 -- allocate_physical_ram_frame_for_partition()
#include "../user/permissions.h"
#include "../drivers/nvme_io.h"
#include "../net/dspp.h"   // Multi-Node Partition Scaling Roadmap Phase 7 -- dspp_migrate_send_begin()/_page()

/* Retransmission times its ACK waits against the LAPIC tick (~100 Hz), not
 * against a loop counter -- the same discipline net/consensus.h's election
 * timing documents, for the same reason. Declared extern rather than by
 * including timer.h so a host test can define its own and drive time
 * explicitly, which is exactly what testing a timeout requires. */
extern volatile uint64_t kernel_tick_counter;

// ─── Store ───────────────────────────────────────────────────────────────────
struct StreamEntry stream_store[STREAM_MAX];

// Forward declaration: stream_relocate_partition() (below) and this phase's
// new stream_migrate_send_partition() both call this shared retire helper,
// but it's defined alongside the new Phase 7 functions further down the
// file (kept next to its other new callers rather than moved to the very
// top out of context) -- forward-declared here so stream_relocate_
// partition() (which appears first in the file) can call it.
static void stream_retire_slot(struct StreamEntry* s);

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
// ─── stream_flush_frames ─────────────────────────────────────────────────────
// Flushes every populated frame of `se` to its own LBA, returning how many
// were written.
//
// This used to issue one 4 KiB command per frame -- up to STREAM_MAX_FRAMES
// (16,384) synchronous submit-and-poll round trips for a single 64 MiB
// stream. A stream's pages are separately allocated frame-pool frames, so
// they are scattered in memory and cannot be described by the contiguous
// multi-page path; but a PRP list is natively a scatter list, so
// nvme_write_pages_gather_sync() (drivers/nvme_io.c) describes them to the
// controller directly with no copying at all.
//
// Batching walks runs of CONSECUTIVE populated frames: one NVMe command
// covers one contiguous LBA range, so a NULL frame (a hole that was never
// written) must END the current run rather than be skipped over -- writing
// across a hole would shift every later frame onto the wrong LBA.
//
// Extracted from stream_write_chunk()'s is_last branch so this index
// arithmetic can be tested directly (tests/stream_gather_flush_host_test.c).
// Batching LBA-computing loops is exactly where an off-by-one does not crash
// but silently misplaces data, so it is worth having under test on its own.
uint32_t stream_flush_frames(struct StreamEntry* se) {
    uint32_t flushed = 0;
    const void* run[NVME_MAX_PAGES_PER_XFER];
    uint32_t    run_len   = 0;
    uint32_t    run_start = 0;

    // fi runs one past the end: that final pass carries have==0 and so
    // flushes any still-open run.
    for (uint32_t fi = 0; fi <= se->frames_used; fi++) {
        int have = (fi < se->frames_used) && (se->frames[fi] != 0);

        if (have) {
            if (run_len == 0) run_start = fi;
            run[run_len++] = se->frames[fi];
        }

        // Evaluated AFTER the append, so the frame that fills a batch goes
        // out with that batch exactly once -- an earlier draft flushed and
        // then re-seeded the next run with the same frame, writing it twice
        // and shifting every later frame one LBA early.
        int must_flush = (run_len == NVME_MAX_PAGES_PER_XFER) ||
                         (!have && run_len > 0);
        if (!must_flush) continue;

        uint64_t lba = se->lba_base + (uint64_t)run_start * 8;
        int rc = nvme_write_pages_gather_sync(lba, run, run_len);
        if (rc) {
            kernel_serial_printf("[STREAM] NVMe write frames %u..%u failed rc=%d\n",
                                 run_start, run_start + run_len - 1, rc);
        } else {
            flushed += run_len;
        }
        run_len = 0;
    }
    return flushed;
}

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
        uint32_t flushed = stream_flush_frames(se);
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
        stream_retire_slot(src);

        relocated++;
    }

    if (relocated > 0) stream_persist_directory();
    return relocated;
}

// ─── Multi-Node Partition Scaling Roadmap Phase 7: real cross-node data
// movement ────────────────────────────────────────────────────────────────
// See net/dspp.h's own Phase 7 header comment for the full design writeup
// (wire format, why fire-and-forget, why streams only). The three
// functions below are this file's half of that work; net/dspp.c owns the
// wire encode/decode and calls into these.

// Zeroes a slot's fields back to the cold-start default, mirroring
// stream_create()'s own reverse (nothing has ever explicitly done this
// before this phase -- stream_relocate_partition()'s retire step and both
// new functions below all need the identical reset, so it's a shared
// helper rather than three copies of the same seven-field zero-out).
static void stream_retire_slot(struct StreamEntry* s) {
    st_memset(s->name, 0, STREAM_NAME_LEN);
    st_memset(s->mime_type, 0, STREAM_MIME_LEN);
    s->size         = 0;
    s->frames_used  = 0;
    s->lba_base     = 0;
    s->active       = 0;
    s->owner_uid    = 0;
    s->partition_id = 0;
    for (uint32_t f = 0; f < STREAM_MAX_FRAMES; f++) s->frames[f] = 0;
}

int stream_migrate_send_partition(uint32_t partition_id, uint32_t dest_node_id) {
    if (!(io_sq && io_cq)) return 0;  // no NVMe -- nothing to send, honest no-op

    static uint8_t __attribute__((aligned(4096))) migrate_send_page_buf[4096];

    int sent = 0;
    for (int i = 0; i < STREAM_MAX; i++) {
        struct StreamEntry* src = &stream_store[i];
        if (!src->active || src->partition_id != partition_id) continue;

        // Unique for the lifetime of this one synchronous migrate call --
        // partition_migrate() runs its steps one at a time, never
        // concurrently, so a given slot index can never be mid-migration
        // twice at once. Not intended to be globally unique across reboots
        // or unrelated migrations (nothing on the receive side needs that;
        // see stream_migrate_recv_begin()'s own comment).
        uint64_t transfer_id = ((uint64_t)partition_id << 32) | (uint64_t)i;

        dspp_migrate_send_begin(transfer_id, dest_node_id, partition_id,
                                src->name, src->mime_type, src->size,
                                src->frames_used, src->owner_uid);

        uint64_t src_lba = src->lba_base;
        int stream_confirmed = 1;
        for (uint32_t p = 0; p < src->frames_used; p++) {
            uint64_t slba = src_lba + (uint64_t)p * 8;
            if (nvme_read_sync(slba, migrate_send_page_buf) != 0) {
                kernel_serial_printf(
                    "[STREAM] migrate: read failed for partition %u's stream "
                    "'%s' page %u -- stopping with %d slot(s) sent so far. "
                    "Source left intact.\n",
                    (unsigned)partition_id, src->name, (unsigned)p, sent);
                return sent;
            }

            /* ── Send, then confirm, retransmitting what did not land ─────
             * The wait works because the ACKs are delivered by the TIMER
             * ISR, not by this thread: kernel/timer.c's handler calls
             * net_poll_tick() -> e1000_poll_rx() -> dspp_rx_dispatch() ->
             * dspp_migrate_note_ack(). Spinning here with interrupts enabled
             * is therefore progress, not deadlock -- the same arrangement
             * kernel_sleep_ticks() depends on.
             *
             * Only the missing fragments are resent. Resending a whole page
             * because one slice was lost would multiply traffic by the
             * fragment count on precisely the link already dropping frames. */
            dspp_migrate_arm_page(transfer_id, p);
            dspp_migrate_send_page(transfer_id, dest_node_id, partition_id,
                                   p, migrate_send_page_buf);

            int page_ok = 0;
            for (uint32_t attempt = 0; attempt < DSPP_MIGRATE_MAX_ATTEMPTS; attempt++) {
                uint64_t deadline = kernel_tick_counter + DSPP_MIGRATE_ACK_TIMEOUT_TICKS;
                /* Two bounds, not one. The deadline is the real timeout; the
                 * stall counter is insurance against the clock not running at
                 * all, which would otherwise hang the node here (see
                 * DSPP_MIGRATE_STALL_SPINS in net/dspp.h). */
                uint64_t last_tick = kernel_tick_counter;
                uint32_t stalled   = 0;
                while (kernel_tick_counter < deadline) {
                    if (dspp_migrate_page_acked()) { page_ok = 1; break; }
                    /* A non-zero status is a REFUSAL (no slot, bad index),
                     * not a loss. Retrying an identical request cannot
                     * change the answer, so stop immediately rather than
                     * burning the whole budget on it. */
                    if (dspp_migrate_nacked()) break;

                    if (kernel_tick_counter != last_tick) {
                        last_tick = kernel_tick_counter;
                        stalled = 0;
                    } else if (++stalled > DSPP_MIGRATE_STALL_SPINS) {
                        kernel_serial_print(
                            "[STREAM] migrate: tick counter is not advancing -- timer ISR "
                            "stopped? Abandoning the wait rather than spinning forever.\n");
                        break;
                    }
                    __asm__ volatile("pause");
                }
                if (page_ok || dspp_migrate_nacked()) break;

                /* Timed out. Resend only the unacknowledged slices. */
                uint32_t resent = 0;
                for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
                    if (dspp_migrate_frag_acked(f)) continue;
                    dspp_migrate_send_frag(transfer_id, dest_node_id, partition_id,
                                           p, f, migrate_send_page_buf);
                    resent++;
                }
                kernel_serial_printf(
                    "[STREAM] migrate: page %u of '%s' unacknowledged after attempt %u "
                    "-- retransmitting %u of %u fragment(s).\n",
                    (unsigned)p, src->name, (unsigned)attempt + 1u,
                    (unsigned)resent, (unsigned)DSPP_MIGRATE_FRAGS_PER_PAGE);
            }
            dspp_migrate_disarm();

            if (!page_ok) {
                /* The destination never confirmed this page. Abandon the
                 * stream and -- critically -- do NOT retire the source. See
                 * the retirement decision below. */
                kernel_serial_printf(
                    "[STREAM] migrate: page %u of '%s' NOT confirmed after %u attempt(s)%s "
                    "-- transfer abandoned, source slot %d left intact.\n",
                    (unsigned)p, src->name, (unsigned)DSPP_MIGRATE_MAX_ATTEMPTS,
                    dspp_migrate_nacked() ? " (destination refused)" : "", i);
                stream_confirmed = 0;
                break;
            }
        }

        if (!stream_confirmed) continue;   /* leave src->active alone */

        kernel_serial_printf(
            "[STREAM] migrate: partition %u's stream '%s' (slot %d, %u "
            "page(s)) CONFIRMED received by node %u -- every page acknowledged.\n",
            (unsigned)partition_id, src->name, i, src->frames_used,
            (unsigned)dest_node_id);

        /* ─── Retirement is now conditional, and that is the point ─────────
         * This used to run unconditionally, immediately after the last frame
         * was handed to the NIC. Combined with a wire that silently dropped
         * every page frame (see the roadmap's §9c), that meant a migration
         * deleted the source and delivered nothing -- the worst possible
         * outcome, and invisible.
         *
         * Retransmission would be pointless without this change: retrying a
         * page and then deleting the original regardless is just a slower way
         * to lose data. Reaching here means every page of this stream was
         * acknowledged by the destination, so the source copy is genuinely
         * redundant. Any other outcome leaves it alone and reports why. */
        stream_retire_slot(src);
        sent++;
    }

    if (sent > 0) stream_persist_directory();
    return sent;
}

// Small table correlating an in-flight incoming migration's transfer_id to
// the local slot receiving it, and how many of that slot's pages have
// arrived so far. Sized STREAM_MAX -- at most one migration per slot can
// possibly be in flight at once, the same ceiling stream_store[] itself has.
#define STREAM_MIGRATE_INFLIGHT_MAX STREAM_MAX
struct StreamMigrateInflight {
    uint64_t transfer_id;
    int      slot;
    uint32_t received_pages;
    uint8_t  active;

    /* ── Page reassembly ────────────────────────────────────────────────
     * A 4 KiB page arrives as DSPP_MIGRATE_FRAGS_PER_PAGE separate frames
     * (net/dspp.h), because a whole page does not fit an Ethernet frame.
     * Fragments accumulate here and the page is written to NVMe only once
     * all of them are present -- one write and one verify read per page,
     * the same disk cost as before the split.
     *
     * Staging in RAM rather than read-modify-writing the destination LBA
     * per fragment: the latter needs no state at all and tolerates any
     * arrival order, but costs a read and a write per fragment, and would
     * leave a genuinely half-written page on disk if a fragment were lost.
     * A page that is either fully written or not written at all is the
     * better failure mode for storage. */
    uint32_t staged_page;                              /* which page_index staged[] holds */
    uint8_t  staged_valid;                             /* 0 before the first fragment */
    uint8_t  frag_present[DSPP_MIGRATE_FRAGS_PER_PAGE];
    uint32_t frags_seen;
    uint8_t  staged[4096];
};
static struct StreamMigrateInflight migrate_inflight[STREAM_MIGRATE_INFLIGHT_MAX];

int stream_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                               const char* name, const char* mime_type,
                               uint64_t size, uint32_t frames_used,
                               uint32_t owner_uid) {
    int dst = -1;
    for (int j = 0; j < STREAM_MAX; j++) {
        if (!stream_store[j].active) { dst = j; break; }
    }
    if (dst < 0) {
        kernel_serial_printf(
            "[STREAM] migrate recv: no free local slot for incoming stream "
            "'%s' (transfer %llu) -- denied.\n",
            name, (unsigned long long)transfer_id);
        return 1;
    }

    int inflight_idx = -1;
    for (int k = 0; k < STREAM_MIGRATE_INFLIGHT_MAX; k++) {
        if (!migrate_inflight[k].active) { inflight_idx = k; break; }
    }
    if (inflight_idx < 0) {
        // Can't actually happen -- this table is exactly STREAM_MAX-sized
        // and a free stream_store[] slot was just found above, so at least
        // one inflight row must also be free. Defensive only.
        kernel_serial_print("[STREAM] migrate recv: inflight table unexpectedly full -- denied.\n");
        return 1;
    }

    struct StreamEntry* d = &stream_store[dst];
    st_strncpy(d->name, name, STREAM_NAME_LEN);
    st_strncpy(d->mime_type, mime_type, STREAM_MIME_LEN);
    d->size         = (uint32_t)size;
    d->frames_used  = frames_used;
    d->lba_base     = STREAM_DATA_LBA_BASE + (uint64_t)dst * STREAM_SECTORS_PER_SLOT;
    d->active       = 1;
    d->owner_uid    = owner_uid;
    d->partition_id = partition_id;
    for (uint32_t f = 0; f < STREAM_MAX_FRAMES; f++) d->frames[f] = 0;

    migrate_inflight[inflight_idx].transfer_id    = transfer_id;
    migrate_inflight[inflight_idx].slot           = dst;
    migrate_inflight[inflight_idx].received_pages = 0;
    migrate_inflight[inflight_idx].active         = 1;

    kernel_serial_printf(
        "[STREAM] migrate recv: allocated local slot %d for incoming stream "
        "'%s' (transfer %llu, %u page(s) expected).\n",
        dst, name, (unsigned long long)transfer_id, frames_used);
    return 0;
}

/* Discards whatever is staged and starts collecting `page_index` fresh.
 * Kept separate because the "a new page began before the last one finished"
 * path has to do exactly this AND report, and silently sharing the reset
 * with the normal path is how that report gets dropped later. */
static void migrate_stage_reset(struct StreamMigrateInflight* mi, uint32_t page_index) {
    mi->staged_page  = page_index;
    mi->staged_valid = 1;
    mi->frags_seen   = 0;
    for (uint32_t i = 0; i < DSPP_MIGRATE_FRAGS_PER_PAGE; i++) mi->frag_present[i] = 0;
}

int stream_migrate_recv_page(uint64_t transfer_id, uint32_t page_index,
                              uint32_t frag_index, const uint8_t* frag_data) {
    if (!(io_sq && io_cq)) return 1;
    if (frag_index >= DSPP_MIGRATE_FRAGS_PER_PAGE) {
        kernel_serial_printf(
            "[STREAM] migrate recv: fragment index %u out of range (max %u) -- dropped.\n",
            (unsigned)frag_index, (unsigned)DSPP_MIGRATE_FRAGS_PER_PAGE - 1u);
        return 1;
    }

    int inflight_idx = -1;
    for (int k = 0; k < STREAM_MIGRATE_INFLIGHT_MAX; k++) {
        if (migrate_inflight[k].active && migrate_inflight[k].transfer_id == transfer_id) {
            inflight_idx = k; break;
        }
    }
    if (inflight_idx < 0) {
        kernel_serial_printf(
            "[STREAM] migrate recv: page for unknown transfer %llu -- dropped.\n",
            (unsigned long long)transfer_id);
        return 1;
    }

    struct StreamMigrateInflight* mi = &migrate_inflight[inflight_idx];
    struct StreamEntry* d = &stream_store[mi->slot];
    if (page_index >= d->frames_used) {
        kernel_serial_printf(
            "[STREAM] migrate recv: page_index %u out of range for transfer "
            "%llu's %u expected page(s) -- dropped.\n",
            (unsigned)page_index, (unsigned long long)transfer_id, d->frames_used);
        return 1;
    }

    /* ── Stage the fragment ────────────────────────────────────────────
     * A fragment for a different page than the one in hand means the
     * sender moved on. On a single sender over one L2 segment that only
     * happens if a fragment was lost, so the half-collected page is
     * abandoned rather than written -- and said out loud, because a stream
     * silently missing 4 KiB in the middle is exactly the kind of quiet
     * wrongness this protocol has already produced once. */
    if (!mi->staged_valid || mi->staged_page != page_index) {
        if (mi->staged_valid && mi->frags_seen < DSPP_MIGRATE_FRAGS_PER_PAGE) {
            kernel_serial_printf(
                "[STREAM] migrate recv: page %u abandoned with %u/%u fragments when "
                "page %u began -- transfer %llu will not complete.\n",
                (unsigned)mi->staged_page, (unsigned)mi->frags_seen,
                (unsigned)DSPP_MIGRATE_FRAGS_PER_PAGE, (unsigned)page_index,
                (unsigned long long)transfer_id);
        }
        migrate_stage_reset(mi, page_index);
    }

    /* Idempotent: a duplicate fragment overwrites identical bytes and is
     * not counted twice, so it cannot fake a complete page. */
    for (uint32_t b = 0; b < DSPP_MIGRATE_FRAG_BYTES; b++)
        mi->staged[frag_index * DSPP_MIGRATE_FRAG_BYTES + b] = frag_data[b];
    if (!mi->frag_present[frag_index]) {
        mi->frag_present[frag_index] = 1;
        mi->frags_seen++;
    }

    /* Not whole yet -- nothing goes to disk. */
    if (mi->frags_seen < DSPP_MIGRATE_FRAGS_PER_PAGE) return 0;

    const uint8_t* page_data = mi->staged;
    static uint8_t __attribute__((aligned(4096))) migrate_recv_verify_buf[4096];
    uint64_t dlba = d->lba_base + (uint64_t)page_index * 8;
    if (nvme_write_sync(dlba, page_data) != 0) return 1;
    if (nvme_read_sync(dlba, migrate_recv_verify_buf) != 0) return 1;
    for (uint32_t b = 0; b < 4096; b++) {
        if (migrate_recv_verify_buf[b] != page_data[b]) {
            kernel_serial_printf(
                "[STREAM] migrate recv: page %u verify mismatch for transfer "
                "%llu -- write did not survive readback.\n",
                (unsigned)page_index, (unsigned long long)transfer_id);
            return 1;
        }
    }

    /* The page is on disk and verified. Retire the staging state so a
     * duplicate of its LAST fragment cannot re-run this block and count the
     * same page twice -- which would let a transfer report complete while a
     * later page was still missing. */
    mi->staged_valid = 0;
    mi->received_pages++;
    if (mi->received_pages >= d->frames_used) {
        kernel_serial_printf(
            "[STREAM] migrate recv: transfer %llu complete (%u page(s)) -- "
            "stream '%s' now fully received in local slot %d.\n",
            (unsigned long long)transfer_id, d->frames_used, d->name, mi->slot);
        mi->active = 0;   // this inflight row is free for a future migration
        stream_persist_directory();
    }
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
