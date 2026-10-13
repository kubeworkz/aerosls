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
 * What this module deliberately does NOT do (each its own increment):
 *
 *   - It does not WIRE into partition_migrate(). The sender's first caller is
 *     env_migrate_partition() below, increment 3's own entry point, which
 *     folds the environment into the same critical section the streams ride
 *     (pause → lease step-down → transfer → ownership handoff) so an ownership
 *     handoff can never precede its environment's payload — the
 *     `ownership-first` tooth at the environment level. The RECEIVER is live
 *     on the wire today: dspp_rx_dispatch() routes the ENV opcodes here on
 *     every node.
 *
 * What this module DOES restore (P3 increment 4): a complete staging slot is
 * the input to env_migrate_restore(), which replays the staged descriptor
 * through the environment manager's existing create path (P1a rule 2 — a
 * restored environment IS a created environment), verifies what came back is
 * what crossed, pours the staged pages into the fresh environment's regions
 * and durable extent, and calls env_migrate_recv_drop(). The BSP sweep drives
 * it (env_migrate_tick()), so a complete slot whose partition this node owns
 * becomes a live environment without an operator having to notice.
 *
 * The discipline this module keeps NOW, and the reason it exists as its own
 * increment: the source is never touched by the TRANSFER. env_migrate_send()
 * reads the environment's pages and copies them; it kills nothing, frees
 * nothing and drops no record. A transfer that returns short (a lost BEGIN,
 * a page the destination never confirmed) leaves the environment exactly
 * where it was, exactly as it was — the same "never ownership without data"
 * rule partition_migrate() applies to streams, applied to an environment's
 * whole state before the caller is allowed to act on the count.
 *
 * What P3 increment 5 adds is the OTHER half, and it is gated the same way:
 * the source's teardown (records, sidecars, and with it the deferred frame
 * reclamation) happens only after the destination has confirmed — over the
 * wire, on the source's poll — that the environment is LIVE WITH ITS
 * CONTENTS there. Before that word arrives, nothing here moves: a source
 * torn down early is an environment that lives nowhere if the destination
 * never restores, which is the exact defect the increment exists to end.
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
    ENV_MIGRATE_REFUSE_UNREPLAYED= 8, /* a record of this partition still awaits this boot's
                                       * replay: the environment does not exist here yet, so
                                       * its region bases name nothing this node owns */
    ENV_MIGRATE_REFUSE_UNFROZEN  = 9, /* the partition-scoped freeze refused: the partition is
                                       * not held paused (or is PARTITION_SYSTEM), so nothing
                                       * may be read as a consistent instant */

    /* ─── Restore-side statuses (P3 increment 4) ──────────────────────────────
     * What env_migrate_restore() answers. The send-side codes above describe
     * why bytes did NOT leave; these describe why a staged environment did
     * NOT become live. The same latch holds both: the caller that must abort
     * before an ownership handoff and the operator watching a destination
     * both need the refusal BY NAME, not a bare failure. */
    ENV_MIGRATE_REFUSE_NO_SLOT     = 10, /* nothing is staged for (partition, index) */
    ENV_MIGRATE_REFUSE_INCOMPLETE  = 11, /* staged, but not every page has landed */
    ENV_MIGRATE_REFUSE_NOT_OWNER   = 12, /* this node does not own the partition */
    ENV_MIGRATE_REFUSE_RECORD      = 13, /* the staged descriptor failed the record
                                          * layer's own validation — details in
                                          * env_ckpt_last_refusal() */
    ENV_MIGRATE_REFUSE_CREATE      = 14, /* the create round trip failed (refusal
                                          * operand b carries the ENV_* status, or 0
                                          * when the channel never answered) */
    ENV_MIGRATE_REFUSE_IDENTITY    = 15, /* what came back is not what crossed; the
                                          * environment the create made was ended */
    ENV_MIGRATE_REFUSE_PLACEMENT   = 16, /* the fresh environment landed OFF the staged
                                          * region bases: its sidecars' heaps hold raw
                                          * pointers into those regions, so pouring would
                                          * leave every interior pointer dangling —
                                          * env_payload's EP_REFUSE_PLACEMENT rule,
                                          * applied to bytes that arrived by wire */
    ENV_MIGRATE_REFUSE_EXTENT      = 17, /* the fresh record's durable extent disagrees
                                          * with the staged one — the pages crossed for
                                          * an LBA this node did not allocate */
    ENV_MIGRATE_REFUSE_PARKED      = 18, /* the sidecars never reached the parked posture
                                          * the pour may restore into */
};

/* The name of a status, by code — for the operator's log and for a guard that
 * asserts on a word rather than on a number that could be retyped. `static
 * inline` in the header DELIBERATELY: partition_migrate() prints it in its
 * abort line, and a non-inline definition here would be a second external
 * symbol partition.c takes on — every host test that links it would need a
 * second stub for a pure string mapping. A mapping with no state belongs to
 * the header, so the orchestration gains one dependency, not two. */
static inline const char* env_migrate_status_name(int status) {
    switch (status) {
    case ENV_MIGRATE_OK:                 return "ok";
    case ENV_MIGRATE_REFUSE_NO_RECORD:   return "no checkpoint record";
    case ENV_MIGRATE_REFUSE_SYSTEM:      return "PARTITION_SYSTEM never migrates";
    case ENV_MIGRATE_REFUSE_DEST:        return "not a real destination node";
    case ENV_MIGRATE_REFUSE_UNQUIESCED:  return "record captured without a quiesce";
    case ENV_MIGRATE_REFUSE_BEGIN:       return "destination never confirmed the BEGIN";
    case ENV_MIGRATE_REFUSE_PAGE:        return "a page never confirmed";
    case ENV_MIGRATE_REFUSE_READ:        return "source page unreadable";
    case ENV_MIGRATE_REFUSE_UNREPLAYED:  return "environment not yet replayed on this boot";
    case ENV_MIGRATE_REFUSE_UNFROZEN:    return "partition not held paused";
    case ENV_MIGRATE_REFUSE_NO_SLOT:     return "no staged slot for this environment";
    case ENV_MIGRATE_REFUSE_INCOMPLETE:  return "staged slot is incomplete";
    case ENV_MIGRATE_REFUSE_NOT_OWNER:   return "this node does not own the partition";
    case ENV_MIGRATE_REFUSE_RECORD:      return "staged descriptor failed record validation";
    case ENV_MIGRATE_REFUSE_CREATE:      return "the destination's create round trip failed";
    case ENV_MIGRATE_REFUSE_IDENTITY:    return "what came back is not what crossed";
    case ENV_MIGRATE_REFUSE_PLACEMENT:   return "fresh environment landed off the staged bases";
    case ENV_MIGRATE_REFUSE_EXTENT:      return "durable extent disagrees with the staged one";
    case ENV_MIGRATE_REFUSE_PARKED:      return "sidecars never reached the parked posture";
    default:                             return "unknown status";
    }
}

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

/* ─── The migration's environment step (P3 increment 3) ─────────────────────
 * Everything partition_migrate() needs from this module, in one call, so the
 * orchestration grows ONE reference rather than a loop over another module's
 * table (and every host test that links partition.c needs exactly one stub).
 *
 * The discipline, in order:
 *   1. count this partition's records — its environments, as the kernel knows
 *      them — and REFUSE if any is still awaiting this boot's replay;
 *   2. freeze them (env_ckpt_quiesce_partition): the caller already holds the
 *      partition paused, so this sets the flags and pauses nothing;
 *   3. send every one of them, each confirmed page by page by env_migrate_send;
 *   4. thaw on EVERY path out, success or failure — the flags describe the
 *      instant the transfer held, and outlive it only while it holds.
 *
 * Returns 0 only when every environment confirmed, with *out_expected and
 * *out_moved equal; nonzero — and *out_moved < *out_expected — when the caller
 * must abort BEFORE the ownership handoff. The source is untouched either way
 * (env_migrate_send's own rule), so an abort leaves the environment exactly
 * where it was: `streams_expected` vs `streams_relocated`, applied to a
 * partition's whole environments. */
int env_migrate_partition(uint32_t partition_id, uint32_t dest_node_id,
                          uint32_t* out_expected, uint32_t* out_moved);

/* The operands behind the most recent refusal, the same contract
 * env_payload.h gives its refusals: the code (above) is what a guard asserts
 * on, `a`/`b` carry whichever numbers made it specific — the ENV_* status for
 * a failed create, the region kind for a placement refusal. NULL only before
 * the first restore; the send path does not touch it. */
struct EnvMigrateRefusal {
    uint32_t code;   /* an ENV_MIGRATE_REFUSE_* */
    uint32_t a;
    uint32_t b;
};
const struct EnvMigrateRefusal* env_migrate_last_refusal(void);

/* ─── Restore (P3 increment 4): a staged slot becomes a live environment ─────
 * The destination half's second act. A complete staging slot is turned back
 * into a running environment ON THIS NODE, by replaying the staged descriptor
 * through the same ENV_CREATE round trip the HTTP create route and the boot
 * replay both make (P1a rule 2: a restored environment is a created
 * environment, or it is not restored at all), then pouring the staged pages —
 * the bytes the wire carried — into the fresh environment's regions and
 * durable extent.
 *
 * The order is the feature, and it is the boot pass's own (env_service.c's
 * restore-through-create), with the bytes sourced from the staging slot
 * instead of NVMe:
 *
 *   1. the slot exists and is COMPLETE, the record passes the same admissibility
 *      gate the boot replay applies (quiesced; partition exists), and this node
 *      OWNS the partition — a follower restoring bytes for a partition served
 *      elsewhere would be a second environment charging a tenant that does not
 *      know about it;
 *   2. an environment already live at this identity is ENDED first (a return
 *      migration: the source's teardown is confirmation-gated — increment 5
 *      — so a node that already served this environment once may still hold
 *      its stale instance while the confirmation is in flight, and the
 *      staged bytes are the newer truth);
 *   3. the staged record is stored, the create is issued, and what came back
 *      is compared against what crossed (env_ckpt_restore_same_identity). A
 *      create that failed or came back different DROPS the stored record and
 *      ends the environment the create made: refusal over partial application,
 *      so a refused restore leaves neither a half-applied record nor a
 *      half-restored environment behind. The slot is KEPT (the bytes are
 *      intact and a retry — the sweep retries on its own period — may succeed
 *      where this attempt could not);
 *   4. the pour is validated WHOLE before a byte is written: every staged
 *      region must have landed at the base the staged record names (the
 *      sidecars hold raw interior pointers into those regions — the exact
 *      rule env_payload.h's placement refusal exists for), the durable
 *      extents must agree, and the sidecars must be PARKED. A refusal here
 *      leaves the environment standing as the empty replay it already is —
 *      the boot pass's own answer — and DROPS the slot: staged bytes that can
 *      never be applied are partition-charged frames held forever for nothing;
 *   5. the pour itself is straight-line with NO yield (env_payload.h's rule:
 *      Ring-3 runs only while the control plane yields, so nothing can execute
 *      over the memory being poured), then the slot is dropped and the
 *      partition is put back into the state the record recorded — a migration
 *      always leaves its partition paused, because resume happens over here,
 *      by an operator, after the bytes have landed.
 *
 * Returns ENV_MIGRATE_OK when the environment is live with its contents, or
 * the refusal above. env_migrate_tick() is the sweep-driven caller; this
 * entry is direct, so a guard and an operator can both ask for one specific
 * environment by name. */
int env_migrate_restore(uint32_t partition_id, uint32_t index);

/* The BSP sweep's driver: walk the staging table and restore every COMPLETE
 * slot whose partition exists and is owned by this node, on a period
 * (ENV_MIGRATE_RESTORE_PERIOD_TICKS) rather than every sweep, with a bounded
 * number of attempts per slot — a slot whose create can never succeed (a
 * manager that never comes up, an environment this node cannot build) must
 * not log forever and must not hold its frames silently either: the attempts
 * cap ends with one line naming the slot, and env_migrate_staging_count()
 * keeps it countable. Returns how many restores ran.
 *
 * Since P3 increment 5 the same sweep also drives the SOURCE's confirmation
 * walk (the entries env_migrate_partition() armed): poll the destination,
 * tear down on its word, and complete the deferred reclamation when the
 * partition's last environment is gone. Same context, same period, same
 * bounded-retries discipline — the two halves are the two roles of one
 * migration, and the BSP sweep is where periodic node-local work belongs. */
uint32_t env_migrate_tick(uint64_t now);

/* ─── P3 increment 5: the destination confirms, then the source tears down ─
 * The rule this increment exists for: no record may describe an environment
 * that moved. Until now a successful migration left the environment on BOTH
 * nodes' books while its bytes lived on one — the source's records, sidecars
 * and frames all survived (increments 3 and 4 named this boundary). This
 * closes it, and the closing is GATED: the source tears its half down only
 * on the destination's own word that the environment is live with its
 * contents there, never on the bytes having been sent.
 *
 * The pull, not a push: the source polls (LIVE_REQ on the sweep's period,
 * bounded by ENV_MIGRATE_CONFIRM_MAX_TRIES) and the destination answers
 * from its own state (env_migrate_live_query). A lost question or a lost
 * answer is recovered by the next period, and a destination that has staged
 * bytes but has not restored them answers "not live" — staged is not
 * restored, and confirming from staged bytes would have the source destroy
 * the only complete copy.
 *
 * Arming: env_migrate_partition() arms one confirmation entry per
 * environment when every payload confirmed; the sweep then walks each entry
 * through the ownership gates — a partition this node owns again (the
 * migration aborted, or came back) keeps its records, a partition that no
 * longer exists ends with that partition's own teardown, and only
 * confirmed + not-ours runs env_service_destroy(). When the LAST confirmed
 * environment of the partition is gone, the deferred
 * partition_reclaim_all_frames() runs — the reclamation kernel/partition.c
 * Step 5 defers for a partition that carries environments, so the frames a
 * teardown is about to free are never back in the pool under a live
 * sidecar. */

/* How many teardown confirmations are pending on this node (observability,
 * the same contract env_migrate_staging_count() gives the staging table: a
 * confirmation that never arrives must be countable, not suspected). */
uint32_t env_migrate_pending_count(void);

/* Wire entries (called only by net/dspp.c's dispatch). live_query is the
 * destination's answer: 0 only when THIS node owns the partition, the
 * environment's record is live here, and it was restored WITH ITS CONTENTS
 * (the marker the successful pour sets) — never from a record's mere
 * existence, which a refused pour also leaves behind. live_ack is the
 * source's receipt, recorded against the pending entry by identity; a
 * nonzero status is not an error, it is "not yet", and the next period asks
 * again. */
int env_migrate_live_query(uint32_t partition_id, uint32_t index);
int env_migrate_live_ack(uint32_t partition_id, uint32_t index, uint32_t status);

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
