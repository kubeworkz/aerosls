/*
 * env_migrate.h — POSIX-Environments P3 increment 2: the environment's
 * payload crosses nodes as a unit.
 *
 * Roadmap v0.2 §7's migration bullet, first half: "checkpoint, transfer,
 * restore, resume" — this module is the TRANSFER. It moves one environment's
 * full footprint — its checkpoint descriptor (struct EnvCkptRecord, P1a) plus
 * every byte the descriptor names: the three region pages (P1's frame-pool
 * regions, identity-mapped) and the durable storage extent's pages (P1b,
 * NVMe, named by the record's state_lba/state_sectors) — from this node to a
 * destination node, page-by-page with per-fragment acknowledgement, over the
 * SAME DSPP migrate wire family the stream path uses (net/dspp.h opcodes
 * 18–21, the ENV family — above the partition family's 15–17; the 11–14
 * range this family was first drawn on belongs to the checkpoint family,
 * which shares the magic and would have silently swallowed the frames).
 *
 * What this module deliberately does NOT do yet (each its own increment):
 *
 *   - It does not WIRE into partition_migrate(). The sender's first caller is
 *     increment 3, which folds the environment into the same critical section
 *     the streams already ride (pause → lease step-down → transfer →
 *     ownership handoff), so an ownership handoff can never precede its
 *     environment's payload — the `ownership-first` tooth at the environment
 *     level. The RECEIVER is live on the wire today: dspp_rx_dispatch() routes
 *     the ENV opcodes here on every node.
 *   - It does not RESTORE. A complete staging slot is the destination half's
 *     input; increment 4 replays the staged descriptor through the
 *     environment manager's existing create path (P1a rule 2 — a restored
 *     environment IS a created environment) and pours the staged pages into
 *     the fresh environment's regions, then calls env_migrate_recv_drop().
 *
 * The discipline this module keeps NOW, and the reason it exists as its own
 * increment: the source is never touched. env_migrate_send() reads the
 * environment's pages and copies them; it kills nothing, frees nothing and
 * drops no record. A transfer that returns short (a lost BEGIN, a page the
 * destination never confirmed) leaves the environment exactly where it was,
 * exactly as it was — the same "never ownership without data" rule
 * partition_migrate() applies to streams, applied to an environment's whole
 * state before the caller is allowed to act on the count.
 */
#ifndef ENV_MIGRATE_H
#define ENV_MIGRATE_H

#include <stdint.h>
#include "env_ckpt.h"

/* What the last env_migrate_send() returned, by name. The return value alone
 * (0 / nonzero) is enough for a caller to abort-before-handover; this is so
 * the operator — and the host test — can tell WHICH gate said no. */
enum EnvMigrateStatus {
    ENV_MIGRATE_OK = 0,
    ENV_MIGRATE_REFUSE_NO_RECORD = 1, /* no live checkpoint record for (partition, index) */
    ENV_MIGRATE_REFUSE_SYSTEM    = 2, /* PARTITION_SYSTEM never migrates (partition_migrate's rule) */
    ENV_MIGRATE_REFUSE_DEST      = 3, /* dest is the node itself, or the 0 'uninitialized' sentinel */
    ENV_MIGRATE_REFUSE_UNQUIESCED= 4, /* the record was captured without a quiesce — a moving target */
    ENV_MIGRATE_REFUSE_BEGIN     = 5, /* the destination refused or never acknowledged the BEGIN */
    ENV_MIGRATE_REFUSE_PAGE      = 6, /* a page never confirmed; the transfer was abandoned */
    ENV_MIGRATE_REFUSE_READ      = 7, /* a source page could not be read (region or NVMe) */
};

/* ─── Sender (the source node) ─────────────────────────────────────────────── */

/* Send the environment (partition_id, index) to dest_node_id. Returns 0 on a
 * fully-confirmed transfer (every page the destination staged and
 * acknowledged), or one of the refusals above — after which the environment
 * is untouched and the caller must NOT hand over ownership or tear anything
 * down (partition_migrate()'s streams_expected rule, applied here).
 *
 * transfer_id is derived ((partition_id << 32) | index), matching the stream
 * family's partition-scoped convention; the destination stages by
 * (partition_id, index), so a retransmitted BEGIN for an environment already
 * being staged is refused rather than clobbering the slot in flight. */
int env_migrate_send(uint32_t partition_id, uint32_t index, uint32_t dest_node_id);

/* The status of the most recent env_migrate_send() on this node. */
int env_migrate_last_status(void);

/* How many payload pages an environment of this record carries: every
 * region's frames, plus the durable extent's pages (8 NVMe sectors per
 * 4 KiB page, the stream family's own sector geometry). Both sides compute
 * this from the SAME record — the one that crossed in the BEGIN — and the
 * destination REFUSES a BEGIN whose declared count disagrees, so the page
 * numbering cannot silently drift between the two kernels. */
uint32_t env_migrate_payload_pages(const struct EnvCkptRecord* rec);

/* ─── Receiver (the destination node) ──────────────────────────────────────── */

/* Staged-slot inspection for the restore half (increment 4) and the host
 * test: the staged descriptor and whether every page has landed. A slot that
 * is not complete is never handed out as if it were — env_migrate_recv_find
 * returns the slot either way, and the caller checks ->complete. */
struct EnvMigrateStaging {
    uint32_t partition_id;
    uint32_t index;
    uint32_t total_pages;
    uint32_t pages_staged;   /* distinct pages fully assembled */
    uint8_t  complete;       /* every page assembled and stored */
    uint8_t  _pad[3];
    struct EnvCkptRecord record;
};

/* The staged slot for (partition_id, index), or NULL. Runtime state only —
 * deliberately never persisted, exactly like the checkpoint table's own
 * pending-replay set: which transfers THIS boot has staged is a property of
 * this boot, and a reboot that found stale staged bytes would restore an
 * environment from a migration that may have been abandoned. */
const struct EnvMigrateStaging* env_migrate_recv_find(uint32_t partition_id,
                                                      uint32_t index);

/* Page `p` of a staged slot, or NULL. The restore pours FROM here. */
const uint8_t* env_migrate_recv_page_ptr(uint32_t partition_id, uint32_t index,
                                         uint32_t page_index);

/* Free the staged slot (its frames go back to the partition they were
 * charged to). This is the ONLY way a slot leaves the table: the restore
 * calls it after a successful pour, and an operator/test calls it to clear
 * an abandoned transfer. Returns 0 if a slot was freed, 1 if there was
 * none. */
int env_migrate_recv_drop(uint32_t partition_id, uint32_t index);

/* How many staging slots are live (observability; the table is small and
 * bounded — an unfilled abandoned slot is a frame leak, and this is what
 * makes that countable instead of merely suspected). */
uint32_t env_migrate_staging_count(void);

/* Reset the staging table (host tests; the real kernel reaches the same
 * state through drops). */
void env_migrate_reset(void);

/* ─── Wire entries (called only by net/dspp.c's dispatch) ───────────────────
 * The two halves of the receive path, split by packet type. Both return 0
 * to acknowledge and nonzero to refuse; dspp.c turns the value straight
 * into the reply's ACK status, so a refusal is visible to the sender as a
 * named non-zero status rather than as silence. */
int env_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                           uint32_t index, uint32_t total_pages,
                           const struct EnvCkptRecord* record,
                           uint32_t record_len);
int env_migrate_recv_page(uint64_t transfer_id, uint32_t page_index,
                          uint32_t frag_index, const uint8_t* data);

#endif /* ENV_MIGRATE_H */
