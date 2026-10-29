/*
 * env_replica.h — POSIX-Environments v0.2 Phase P3, failover destination (a):
 * the proactive replica's RECEIVE path.
 *
 * Why this exists (roadmap §11 Q5, §7's failover bullet):
 * `failover_recover_from()` adopts by ownership; the leader's live broadcast
 * (`failover_live_checkpoint_broadcast()`) is a serialized state tree that
 * fits net/dspp_checkpoint.c's 24 KiB receive buffer, and an environment's
 * 5.5 MiB of regions does not fit it. Destination (b) — restore from the
 * environment's own durable storage on the survivor, or name the gap — has
 * landed. Destination (a), the PASE-grade half, answers "where do the bytes
 * come from when the node holding them is gone?" with: they were ALREADY
 * HERE. At the same instant `env_payload_capture_all()` writes the owner's
 * local payload band, the record and its payload pages are pushed to the
 * peers over the fragment family migration already proved
 * (DSPP_MIGRATE_ENV_BEGIN carries the record whole; the page family moves
 * 1 KiB fragments with a per-fragment ACK). This module is the receiving
 * half: stage the pages, then finalize — validate the staged instant and
 * write it into THIS node's replica band, so a survivor that later adopts
 * the dead node's partition finds a copy instead of a hole.
 *
 * ─── What lands in increment 1 ──────────────────────────────────────────────
 * The receiver only: recv_begin/recv_page (the migration receiver's twin,
 * same staging discipline — top-down partition-charged frames, per-page
 * fragment assembly, re-BEGIN idempotence), the finalize pass driven from
 * microkernel.c's service poll, and the on-disk directory. The SENDER (the
 * capture-side push, a replica mode of env_migrate_send) is increment 2;
 * PROMOTION — install the record into env_ckpt_table and pour the contents
 * through the replay driver destination (b) already proved — is increment 3.
 *
 * ─── Page numbering on the wire ─────────────────────────────────────────────
 * The record's own, exactly as migration sends it (env_migrate.c): every
 * region's frames in record order, then the durable extent's pages, then
 * the image-window tail the sender appended. Both sides derive the order
 * from the SAME record that crossed in the BEGIN, so the receiver's range
 * gate is env_migrate_payload_pages() itself — one definition of "payload
 * pages" shared with the sender's BEGIN.
 *
 * ─── On-disk layout ─────────────────────────────────────────────────────────
 * A directory pair at persist.h's LBAs (header frame + entry frame, written
 * entries-first-header-last so a torn pair points at unchanged entries,
 * never the reverse) and a band of per-environment slots:
 *
 *   ENV_REPLICA_SLOT_LBA(slot) = 1261568 + slot * 16384
 *
 * 1261568 is exactly the payload band's end (asserted at compile time):
 * this band is the payload band's neighbour, 8 slots x 8 MiB, clear of
 * ROWSTORE_LBA_BASE 2000000. Each slot holds the staged pages in wire
 * order (4 KiB per page, page p at slot_lba + p*8) — NOT the payload blob
 * format: the replica stores what the wire carried, and promotion (the
 * only reader) reuses migration's restore half, which speaks exactly this
 * numbering.
 *
 * The directory keys a replica by (partition_id, index) and stores the
 * whole record beside it: promotion needs no second fetch, and the record
 * crossing whole in the BEGIN is the reason it can.
 *
 * ─── Validation, stated as the rule it is ───────────────────────────────────
 * Refusal over partial application: nothing is written until every page is
 * staged and every gate below has passed; a refusal drops the staged frames
 * and latches a distinct code (the guard and the host test read the code,
 * never a bare nonzero). A replica the band cannot hold is REFUSED BY NAME
 * (the 9th distinct identity, or a replacement when all 8 slots are live),
 * never silently dropped — the classification ladder destination (b)
 * reports against must be able to name "no replica" honestly.
 *
 * Band capacity is a stated boundary, not an accident: 8 slots x 8 MiB.
 * An environment larger than 2048 payload pages is refused at BEGIN
 * (ER_REFUSE_TOO_MANY) rather than truncated.
 */
#ifndef ENV_REPLICA_H
#define ENV_REPLICA_H

#include <stdint.h>
#include "env_ckpt.h"        /* struct EnvCkptRecord crosses whole in the BEGIN */

/* ─── The band ─────────────────────────────────────────────────────────────── */
#define ENV_REPLICA_MAX           8u          /* slots = distinct replicas kept */
#define ENV_REPLICA_SLOT_SECTORS  16384ULL    /* 8 MiB, same slot size as a payload */
#define ENV_REPLICA_DATA_LBA_BASE 1261568ULL  /* = payload band's end; asserted in env_replica.c */
#define ENV_REPLICA_SLOT_LBA(slot) \
    (ENV_REPLICA_DATA_LBA_BASE + ((uint64_t)(slot) * ENV_REPLICA_SLOT_SECTORS))
#define ENV_REPLICA_MAX_PAGES     2048u       /* a slot's pages (16384 sectors / 8) */

/* ─── Runtime staging ────────────────────────────────────────────────────────
 * One slot per peer in flight: the sender is synchronous (one replica at a
 * time per node), so 3 covers this cluster's shape — a fourth concurrent
 * BEGIN is refused (ER_REFUSE_TABLE_FULL) and the sender's retry budget
 * absorbs it, the same contract the migration receiver's 2-slot table has. */
#define ENV_REPLICA_STAGING_MAX   3u
#define ENV_REPLICA_STALE_TICKS   12000ull    /* a wedged staging drops after ~120 s
                                               * (100 Hz ticks), frames returned to
                                               * the partition — bounded by age, the
                                               * way env_migrate's opened_tick bounds
                                               * its in-flight grace */
#define ENV_REPLICA_DIR_MAGIC     0x534C5350524C4131ULL  /* "SLSPRLA1" */
#define ENV_REPLICA_REC_VERSION   1u
#define ENV_REPLICA_F_VALID       0x0001u

/* ─── Refusals: distinct by name, latched for the guard ───────────────────── */
enum {
    ENV_REPLICA_OK                  = 0,
    ENV_REPLICA_REFUSE_NO_RECORD    = 1,   /* BEGIN without the record that names it */
    ENV_REPLICA_REFUSE_RECORD_LEN   = 2,   /* record_len != sizeof(EnvCkptRecord) */
    ENV_REPLICA_REFUSE_PAGE_COUNT   = 3,   /* total outside [payload, payload+tail] */
    ENV_REPLICA_REFUSE_TOO_MANY     = 4,   /* total over the band slot's 2048 pages */
    ENV_REPLICA_REFUSE_SLOT_EXISTS  = 5,   /* identity in flight under another transfer */
    ENV_REPLICA_REFUSE_TABLE_FULL   = 6,   /* every staging slot active */
    ENV_REPLICA_REFUSE_NO_DISK_SLOT = 7,   /* directory full: no band slot for a NEW identity */
    ENV_REPLICA_REFUSE_UNKNOWN      = 8,   /* page for a transfer this table does not hold */
    ENV_REPLICA_REFUSE_RANGE        = 9,   /* page or fragment out of range */
    ENV_REPLICA_REFUSE_ALLOC        = 10,  /* no staging frame for the partition */
    ENV_REPLICA_REFUSE_INCOMPLETE   = 11,  /* finalize found a hole (defensive) */
    ENV_REPLICA_REFUSE_IO           = 12,  /* NVMe write of the band or directory failed */
    ENV_REPLICA_REFUSE_SUPERSEDED   = 13,  /* an older capture must not overwrite a newer replica */
    ENV_REPLICA_REFUSE_STALE        = 14,  /* staging wedged past the age bound and dropped */
};

/* ─── On-disk directory ───────────────────────────────────────────────────────
 * One header frame + one entry frame at PERSIST_ENV_REPLICA_*_LBA, written
 * as a pair on every finalize and read on demand (failover/promotion is
 * the only reader — nothing needs it at boot, like the payload directory). */
struct EnvReplicaDirHead {
    uint64_t magic;      /* ENV_REPLICA_DIR_MAGIC */
    uint32_t version;    /* ENV_REPLICA_REC_VERSION */
    uint32_t count;      /* live entries written */
    uint32_t rec_size;   /* sizeof(struct EnvReplicaDirEntry) */
    uint32_t _pad;
};

struct EnvReplicaDirEntry {
    uint32_t partition_id;
    uint32_t index;
    uint32_t slot;        /* 0..7; the LBA is derived, never stored twice */
    uint32_t flags;       /* ENV_REPLICA_F_VALID */
    uint64_t seq;         /* the capture's checkpoint sequence: newer wins */
    uint32_t pages;       /* blob pages that crossed (the sender's declared total) */
    uint32_t src_node;    /* provenance: whose environment this replicates */
    uint32_t blob_crc;    /* crc32 the RECEIVER computed over every staged page */
    uint32_t _pad;
    struct EnvCkptRecord record;  /* the whole record — promotion needs no second fetch */
};

struct EnvReplicaRefusal {
    uint32_t code;        /* ENV_REPLICA_REFUSE_* */
    uint32_t a;           /* the refusal's own number (page index, total, ...) */
    uint32_t b;
};

/* ─── Wire entries (net/dspp.c's dispatch calls these) ──────────────────────
 * Both mirror env_migrate.c's receiver: 0 = accepted, nonzero = refused (the
 * value is the code above, which dspp.c puts in the ACK's status and logs
 * by name). src_node is the BEGIN's node_source_id — provenance the disk
 * entry keeps, because "where did this copy come from" is exactly the
 * question a failover report has to answer later. */
int env_replica_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                           uint32_t index, uint32_t total_pages,
                           const struct EnvCkptRecord* record,
                           uint32_t record_len, uint32_t src_node);
int env_replica_recv_page(uint64_t transfer_id, uint32_t page_index,
                          uint32_t frag_index, const uint8_t* data);
/* Does this transfer belong to the replica table? dspp.c routes PAGE
 * frames by this predicate — a replica's transfer_id is the same
 * (partition<<32)|index construction migration uses, so the BEGIN family
 * (a separate opcode) picks the receiver and the page family follows it. */
int env_replica_holds(uint64_t transfer_id);

/* ─── The finalize pass (microkernel.c's service poll) ──────────────────────
 * Completes every staged-and-complete transfer: validate, write the band
 * slot, publish the directory entry, free the staged frames. Returns how
 * many replicas finalized (0 is the normal answer until a sender exists). */
int env_replica_tick(void);

/* ─── The finder (promotion, increment 3, and the guards) ─────────────────── */
int env_replica_present(uint32_t partition_id, uint32_t index);
int env_replica_lookup(uint32_t partition_id, uint32_t index,
                       struct EnvReplicaDirEntry* out);
int env_replica_read_page(uint32_t partition_id, uint32_t index,
                          uint32_t page, uint8_t* buf);   /* 0 = read, nonzero = absent/short */
uint32_t env_replica_count(void);          /* live directory entries */
uint32_t env_replica_staging_count(void);  /* active staging slots */

/* The refusal latch: the last gate that fired, with its number. The host
 * test and the guard assert on the CODE — a named refusal, never a bare
 * nonzero. */
struct EnvReplicaRefusal env_replica_last_refusal(void);
void         env_replica_clear_refusal(void);
const char*  env_replica_refusal_name(uint32_t code);

/* Host tests only: drop all staging (frees the frames) and clear the
 * latch. The directory lives on the device — the test's RAM disk owns it. */
void env_replica_reset(void);

#endif /* ENV_REPLICA_H */
