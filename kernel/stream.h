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

    /* ─── Set while a cross-node transfer is filling this slot ─────────────
     * stream_migrate_recv_begin() persists the slot BEFORE any page arrives,
     * which it must -- otherwise a stream with no pages is lost when the
     * destination reboots (see net/dspp.h). The cost of that ordering is a
     * window in which a durable slot describes data that has not arrived, and
     * nothing distinguished it from a complete stream: same name, same size,
     * same frame count, all persisted, over empty LBAs.
     *
     * This flag closes the window. Set by recv_begin, cleared only when the
     * final page has been written and verified, and persisted either way. A
     * slot found set at boot is an interrupted transfer.
     *
     * Reaping such a slot is safe rather than merely convenient: the SENDER
     * retires its source only on full confirmation, so an unconfirmed
     * transfer means the original still exists somewhere. The partial copy is
     * redundant by construction. */
    uint8_t   incoming;
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

/* Count of active stream slots belonging to partition_id. partition_migrate()
 * compares this against what stream_migrate_send_partition() actually sent, so
 * that "0 sent" from an empty partition can be told apart from "0 sent"
 * because the destination refused everything -- previously indistinguishable,
 * and the reason a failed migration transferred ownership anyway. */
int stream_count_for_partition(uint32_t partition_id);


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
// helper so both functions apply the identical reset.
//
// NO LONGER fire-and-forget, and this comment used to say it was: the sender
// now waits for a BEGIN_ACK before sending pages, waits for a per-fragment
// PAGE_ACK per page, retransmits only the fragments that were not
// acknowledged, and retires the source slot ONLY if every page was confirmed.
// An unconfirmed transfer leaves the source intact and says why. See
// net/dspp.h's "Sender-side ACK tracking" block for the budget and the
// stalled-clock bound.
//
// Returns the count of slots successfully sent AND confirmed. Compare it
// against stream_count_for_partition() to tell a complete move from a partial
// one -- 0 means "nothing to send" or "everything refused", and the caller
// needs to distinguish those.
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
//
// Takes ONE FRAGMENT, not a whole page. A 4 KiB page does not fit an
// Ethernet frame, so it arrives as DSPP_MIGRATE_FRAGS_PER_PAGE frames
// (net/dspp.h); fragments are staged in the inflight row and the page is
// written to NVMe only once all of them are present. So most calls do no
// disk I/O at all and return 0 having simply banked a fragment -- a return
// of 0 means "accepted", not "the page is now durable".
//
// Duplicate fragments are idempotent and are not double-counted. Fragments
// of one page may arrive in any order. A fragment for a NEW page while the
// current one is incomplete abandons the incomplete page with a log line,
// rather than writing a partially-filled 4 KiB block to disk.
int stream_migrate_recv_page(uint64_t transfer_id, uint32_t page_index,
                              uint32_t frag_index, const uint8_t* frag_data);

/* Writes the 4 KiB stream directory to NVMe. Called internally after every
 * mutation; exposed so a host test can round-trip the directory through this
 * real writer and stream_init()'s real reader. The absence of that round-trip
 * test is why `frames_used` was silently dropped from the snapshot -- see the
 * comment on this function in stream.c. */
void stream_persist_directory(void);

/* Clears every field of a slot. Exposed so a host test can assert that -- an
 * omitted field here means a reused slot inherits stale state, and the
 * transfer flag in particular would get it reaped as an interrupted migration
 * it was never part of. */
void stream_retire_slot_for_test(struct StreamEntry* s);

extern struct StreamEntry stream_store[STREAM_MAX];

/* Flushes every populated frame of `se` to its own LBA, returning the count
 * written. Extracted from stream_write_chunk()'s is_last branch so its
 * run-batching index arithmetic can be tested directly -- batching a loop
 * that writes to computed LBAs is exactly where an off-by-one silently
 * misplaces data instead of crashing. See the definition's own comment and
 * tests/stream_gather_flush_host_test.c. */
uint32_t stream_flush_frames(struct StreamEntry* se);

#endif /* STREAM_H */
