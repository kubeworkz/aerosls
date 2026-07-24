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
    // Multitenant Isolation Gap Analysis §5 item 3 / §7 item 3: owner_uid
    // and partition_id, stamped at stream_create() time and never
    // reassigned afterward (mirrors object_catalog's own "owner is fixed
    // at creation" posture). Every frame this stream ever allocates
    // (stream_write_chunk()'s initial write AND stream_lazy_load_frame()'s
    // post-reboot reload) is charged against partition_id via
    // allocate_physical_ram_frame_for_partition() -- see stream.c's own
    // comment on why this, not the plain unaccounted
    // allocate_physical_ram_frame() every prior version of this file used.
    uint32_t  owner_uid;
    uint32_t  partition_id;
};

// ─── Public API ──────────────────────────────────────────────────────────────
void               stream_init(void);
int                stream_create(uint32_t caller_uid, const char* name, const char* mime_type);
int                stream_write_chunk(const char* name,
                                      const uint8_t* chunk, uint32_t len,
                                      uint32_t offset, uint8_t is_last);
struct StreamEntry* stream_find(const char* name);
int                stream_list_json(char* buf, int max);
// Lazy-load one 4-KiB frame from NVMe into RAM for a stream entry.
// Called by http_respond_stream when se->frames[i] is NULL after reboot.
uint8_t*           stream_lazy_load_frame(struct StreamEntry* se, uint32_t frame_idx);

// Multi-Node Partition Scaling Roadmap Phase 6 addendum ("real migration
// data movement", Multitenant Isolation Gap Analysis §7 item 7): relocates
// every active stream slot owned by partition_id to a fresh STREAM_MAX
// slot, physically copying its on-disk bytes page-by-page (not just
// reassigning a pointer) and verifying each page by reading the
// destination back and comparing it against the source before retiring the
// original slot. "dest_node_id" is accepted but not yet load-bearing --
// AeroSLS has exactly one shared NVMe image and no real second node's
// storage exists in any deployment today (cluster_init() is never called
// from a real boot path), so "destination" honestly means a fresh slot
// within the same shared pool, not a different machine. See stream.c's own
// header comment on this function for the full rationale. Returns the
// count of slots successfully relocated; a short count relative to the
// partition's total active-slot count means relocation stopped partway
// (out of free slots, or a copy/verify failure) and should be treated as a
// partial, not total, migration.
int stream_relocate_partition(uint32_t partition_id, uint32_t dest_node_id);

// ─── Multi-Node Partition Scaling Roadmap Phase 7: real cross-node data
// movement ────────────────────────────────────────────────────────────────
// stream_relocate_partition() above is kept exactly as-is (still the right
// primitive when no real cluster is configured -- see kernel/partition.c's
// partition_migrate(), which now branches between the two). These three
// functions are the genuinely new cross-machine path, used only once a real
// cluster exists (cluster_init() has actually been called with a nonzero
// local node id -- see net/consensus.h). "Node" here can be, and in this
// dev environment's own verification is, a second simulated node (a second
// independent stream_store[]-equivalent state and fake NVMe image in a host
// test) rather than literally different physical hardware -- the real code
// below is identical either way; only what backs stream_store[]/nvme_*_sync
// differs between "two real machines" and "one host test process reused
// sequentially for both roles." See net/dspp.h's own Phase 7 header comment
// for the full design writeup (wire format, fire-and-forget scope decision).

// Sender-side: for every active stream slot owned by partition_id, sends
// its full metadata (DSPP_MIGRATE_BEGIN_REQ) and every populated page
// (DSPP_MIGRATE_PAGE_REQ) to dest_node_id over the real DSPP wire (net/
// dspp.c), then retires the local slot -- the same retire-to-zero fields
// stream_relocate_partition() already uses, factored into a shared static
// helper so both functions apply the identical reset. Fire-and-forget: does
// not wait for or verify BEGIN_ACK/PAGE_ACK before retiring the source, a
// disclosed limitation (see dspp.h's own scope note) rather than a full
// reliable-transport handshake -- a dropped packet on an unreliable link
// could lose data in this first cut. Returns the count of slots sent.
int stream_migrate_send_partition(uint32_t partition_id, uint32_t dest_node_id);

// Receiver-side, called by net/dspp.c's dspp_migrate_rx() when a
// DSPP_MIGRATE_BEGIN_REQ addressed to this node arrives: finds a free local
// stream_store[] slot, stamps its metadata, and records transfer_id -> slot
// in a small internal table so subsequent stream_migrate_recv_page() calls
// for the same transfer_id know where to write. The slot is marked active
// immediately (visible in stream_list_json()/stream_find() right away,
// matching how a fresh stream_create() is also visible before any bytes
// have actually been written) but its data is only as complete as however
// many pages have arrived so far -- callers reading a frame before its page
// arrives get whatever stream_lazy_load_frame() returns for a page never
// written to this slot's LBA range (matches this codebase's existing
// "denial looks like absence" posture rather than a new pending/visible
// distinction). Returns 0 on success, 1 if no free slot exists (this node's
// own STREAM_MAX pool is full).
int stream_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                               const char* name, const char* mime_type,
                               uint64_t size, uint32_t frames_used,
                               uint32_t owner_uid);

// Receiver-side, called by net/dspp.c's dspp_migrate_rx() for each
// DSPP_MIGRATE_PAGE_REQ: looks up transfer_id's slot (from stream_migrate_
// recv_begin() above), writes page_data to that slot's own LBA range at
// page_index via nvme_write_sync(), then reads it back and byte-compares --
// the identical copy-then-verify discipline stream_relocate_partition()
// already established, just receiving bytes from the wire instead of
// reading them from a source LBA. Once every one of the slot's frames_used
// pages has arrived, calls stream_persist_directory() so the newly-received
// stream survives this node's own reboot. Returns 0 on success, 1 if
// transfer_id is unknown (no matching stream_migrate_recv_begin() call), if
// page_index is out of range for that slot's frames_used, or on any NVMe
// read/write/verify failure.
int stream_migrate_recv_page(uint64_t transfer_id, uint32_t page_index,
                              const uint8_t* page_data);

extern struct StreamEntry stream_store[STREAM_MAX];

#endif /* STREAM_H */
