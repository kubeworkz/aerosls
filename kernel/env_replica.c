/*
 * env_replica.c — POSIX-Environments v0.2 Phase P3, failover destination (a),
 * increment 1: the RECEIVER half of the proactive replica path.
 * See kernel/env_replica.h for the design writeup (why the transfer cannot
 * be bolted onto the failure, the band layout, and the validation rule).
 *
 * The shape is env_migrate.c's receiver, deliberately: same staging
 * discipline (top-down partition-charged frames so the placement watermark
 * survives, per-page fragment assembly, re-BEGIN idempotence), same
 * refusal-as-named-code contract, same drop-on-refusal so a wedge cannot
 * hold frames forever. What is NEW is the half the migration receiver does
 * not have — a destination for the bytes that outlives the transfer:
 * env_replica_tick() finalizes a completed staging into the band, and the
 * directory is published with payload's torn-pair discipline (entries
 * first, header last) PLUS an invalidate-before-rewrite so an in-place
 * replacement can never leave a reader looking at mixed captures.
 *
 * Dependencies are the host-testable set: env_ckpt.h (the record that
 * crossed), env_migrate.h (env_migrate_payload_pages() + the image-tail
 * ceiling — ONE definition of the wire's page numbering, shared with the
 * sender), frame_pool.h (staging frames), persist.h's LBAs via this
 * header, nvme_io.h (the device), kernel_io.h (the log), timer.h (the
 * stale bound). Host tests stub the NVMe layer with a RAM disk the way
 * tests/env_migrate_host_test.c does.
 */
#include "env_replica.h"
#include "env_migrate.h"        /* env_migrate_payload_pages(), ENV_MIGRATE_IMAGE_MAX */
#include "env_payload.h"        /* the payload band's geometry — adjacency is asserted */
#include "persist.h"             /* PERSIST_ENV_REPLICA_*_LBA */
#include "frame_pool.h"
#include "kernel_io.h"
#include "timer.h"              /* kernel_tick_counter — the stale bound */
#include "../drivers/nvme_io.h"
#include "../net/dspp.h"        /* DSPP_MIGRATE_FRAG_BYTES / _FRAGS_PER_PAGE */
#include "../net/consensus.h"    /* cluster_roster — who to push to */
#include "failover.h"            /* failover_sweep_keepalive() in the ACK waits */

/* ─── Compile-time form checks ─────────────────────────────────────────────
 * The directory is two 4 KiB frames written as a pair; a struct that outgrew
 * the frame would silently truncate on disk, so it is a build failure here
 * (the same discipline env_ckpt.c holds for its own directory). The band
 * adjacency pins this module to the layout it claims: the replica band
 * starts EXACTLY where the payload band ends — if either moves, this
 * static assert names it before a live cluster writes into a neighbour. */
_Static_assert(sizeof(struct EnvReplicaDirEntry) * ENV_REPLICA_MAX <= 4096,
               "the replica directory's entries must fit one 4 KiB frame");
_Static_assert(sizeof(struct EnvReplicaDirHead) <= 4096,
               "the replica directory's header must fit one 4 KiB frame");
_Static_assert(ENV_REPLICA_DATA_LBA_BASE ==
                   ENV_PAYLOAD_DATA_LBA_BASE +
                       ((uint64_t)ENV_PAYLOAD_MAX * ENV_PAYLOAD_SLOT_SECTORS),
               "the replica band must start exactly where the payload band ends");
_Static_assert((uint64_t)ENV_REPLICA_MAX_PAGES * 8ull <=
                   (uint64_t)ENV_REPLICA_SLOT_SECTORS,
               "a slot's page capacity must fit its band slot");

/* ─── Runtime state ────────────────────────────────────────────────────────
 * One table of staging slots (see env_replica.h for the size), one I/O
 * frame for directory reads/writes (never the staged pages — those stay in
 * their own frames until the band write moves them), and the refusal
 * latch. Capture and finalize never overlap for a given slot (a slot is
 * either staging or being finalized, single-threaded on the service poll),
 * so one I/O frame serves every directory touch. */
struct EnvReplicaSlot {
    uint8_t  active;
    uint8_t  complete;
    uint32_t partition_id;
    uint32_t index;
    uint64_t transfer_id;
    uint32_t total_pages;
    uint32_t pages_staged;   /* distinct pages whose fragments all landed */
    uint32_t src_node;       /* provenance for the disk entry */
    uint64_t opened_tick;    /* kernel_tick_counter at BEGIN — the stale bound */
    struct EnvCkptRecord record;
    uint8_t* pages[ENV_REPLICA_MAX_PAGES];
    uint8_t  frags[ENV_REPLICA_MAX_PAGES][DSPP_MIGRATE_FRAGS_PER_PAGE];
    uint8_t  page_done[ENV_REPLICA_MAX_PAGES];
};

static struct EnvReplicaSlot er_staging[ENV_REPLICA_STAGING_MAX];
static struct EnvReplicaRefusal er_refusal;
static uint8_t er_io[4096] __attribute__((aligned(4096)));

/* The directory lives below the wire entries (recv_begin gates on the disk
 * state before it stages a frame), so its two helpers are declared here
 * and defined with their own section further down. */
static int er_dir_read(struct EnvReplicaDirHead* h, struct EnvReplicaDirEntry* e);
static int er_dir_write(const struct EnvReplicaDirEntry* e, uint32_t count);

static int er_io_ready(void) { return io_sq != 0; }

static int er_refuse(uint32_t code, uint32_t a, uint32_t b) {
    er_refusal.code = code;
    er_refusal.a = a;
    er_refusal.b = b;
    return (int)code;
}

const char* env_replica_refusal_name(uint32_t code) {
    switch (code) {
    case ENV_REPLICA_OK:                return "OK";
    case ENV_REPLICA_REFUSE_NO_RECORD:  return "NO_RECORD";
    case ENV_REPLICA_REFUSE_RECORD_LEN: return "RECORD_LEN";
    case ENV_REPLICA_REFUSE_PAGE_COUNT: return "PAGE_COUNT";
    case ENV_REPLICA_REFUSE_TOO_MANY:   return "TOO_MANY";
    case ENV_REPLICA_REFUSE_SLOT_EXISTS:return "SLOT_EXISTS";
    case ENV_REPLICA_REFUSE_TABLE_FULL: return "TABLE_FULL";
    case ENV_REPLICA_REFUSE_NO_DISK_SLOT:return "NO_DISK_SLOT";
    case ENV_REPLICA_REFUSE_UNKNOWN:    return "UNKNOWN_TRANSFER";
    case ENV_REPLICA_REFUSE_RANGE:      return "RANGE";
    case ENV_REPLICA_REFUSE_ALLOC:      return "ALLOC";
    case ENV_REPLICA_REFUSE_INCOMPLETE: return "INCOMPLETE";
    case ENV_REPLICA_REFUSE_IO:         return "IO";
    case ENV_REPLICA_REFUSE_SUPERSEDED: return "SUPERSEDED";
    case ENV_REPLICA_REFUSE_STALE:      return "STALE";
    default:                            return "REFUSE";
    }
}

struct EnvReplicaRefusal env_replica_last_refusal(void) { return er_refusal; }
void env_replica_clear_refusal(void) {
    er_refusal.code = 0;
    er_refusal.a = 0;
    er_refusal.b = 0;
}

/* ─── crc32 (reflected, poly 0xEDB88320), incremental ───────────────────────
 * Byte-for-byte env_payload.c's ep_crc: the receiver's checksum over the
 * staged pages must be computable one page at a time (no concatenation
 * buffer) and must be reproducible by whoever verifies the band later
 * (promotion, the guard). The seed chains through ~crc in/~crc out, so
 * crc = er_crc(crc, page, 4096) over the pages in wire order IS the crc of
 * their concatenation. */
static uint32_t er_crc(uint32_t crc, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* ─── The staging table ───────────────────────────────────────────────────── */

static struct EnvReplicaSlot* er_slot_by_transfer(uint64_t transfer_id) {
    for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX; i++) {
        struct EnvReplicaSlot* s = &er_staging[i];
        if (s->active && s->transfer_id == transfer_id) return s;
    }
    return 0;
}

static struct EnvReplicaSlot* er_slot_by_identity(uint32_t partition_id,
                                                  uint32_t index) {
    for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX; i++) {
        struct EnvReplicaSlot* s = &er_staging[i];
        if (s->active && s->partition_id == partition_id && s->index == index)
            return s;
    }
    return 0;
}

static void er_slot_drop(struct EnvReplicaSlot* s) {
    for (uint32_t p = 0; p < ENV_REPLICA_MAX_PAGES; p++) {
        if (s->pages[p]) {
            free_physical_ram_frame_for_partition(s->pages[p], s->partition_id);
            s->pages[p] = 0;
        }
        s->page_done[p] = 0;
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++)
            s->frags[p][f] = 0;
    }
    s->active       = 0;
    s->complete     = 0;
    s->pages_staged = 0;
}

uint32_t env_replica_staging_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX; i++)
        if (er_staging[i].active) n++;
    return n;
}

/* ─── Wire entry: the BEGIN ────────────────────────────────────────────────
 * net/dspp.c's dispatch calls this for DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ
 * (increment 1: the opcode route; the SENDER that will emit it is increment
 * 2). Returns 0 when the slot is staged and ready, nonzero — refused with
 * the code's meaning in env_replica.h — otherwise; dspp.c turns the value
 * into the BEGIN_ACK's status and logs it by name. */
int env_replica_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                           uint32_t index, uint32_t total_pages,
                           const struct EnvCkptRecord* record,
                           uint32_t record_len, uint32_t src_node) {
    if (!record) return er_refuse(ENV_REPLICA_REFUSE_NO_RECORD, 0, 0);
    if (record_len != (uint32_t)sizeof(struct EnvCkptRecord))
        return er_refuse(ENV_REPLICA_REFUSE_RECORD_LEN, record_len, 0);

    /* The same range gate the migration receiver holds, because it is the
     * same wire: never fewer payload pages than the record names, never a
     * tail past the shared ceiling, never more than a band slot can hold.
     * Both bounds are refused at the door — a bogus total could otherwise
     * stage frames nobody will ever fill (upper) or record a replica that
     * is missing regions (lower). */
    uint32_t payload_pages = env_migrate_payload_pages(record);
    if (total_pages < payload_pages ||
        total_pages > payload_pages + ENV_MIGRATE_IMAGE_MAX)
        return er_refuse(ENV_REPLICA_REFUSE_PAGE_COUNT, total_pages, payload_pages);
    if (total_pages > ENV_REPLICA_MAX_PAGES)
        return er_refuse(ENV_REPLICA_REFUSE_TOO_MANY, total_pages, 0);

    /* A retransmitted BEGIN for the SAME transfer is answered READY without
     * touching the slot (env_migrate.c's observed-live rule: the destination
     * may be slow to ACK and the sender re-asks — refusing that would break
     * the documented retransmission model exactly when it is needed). A
     * BEGIN for the same identity under a DIFFERENT transfer is another
     * capture's intent and stays refused. */
    struct EnvReplicaSlot* existing = er_slot_by_identity(partition_id, index);
    if (existing) {
        if (existing->transfer_id == transfer_id &&
            existing->record.env_id == record->env_id)
            return 0;
        return er_refuse(ENV_REPLICA_REFUSE_SLOT_EXISTS, partition_id, index);
    }

    /* The disk gates, BEFORE frames are staged: a replica this band cannot
     * hold, or an older capture arriving after a newer one already landed,
     * is refused at the door where the sender sees the NACK. An unreadable
     * or never-written directory counts as EMPTY (payload's rule: a
     * directory this build cannot read is absent, never half-read). */
    struct EnvReplicaDirHead dh;
    struct EnvReplicaDirEntry de[ENV_REPLICA_MAX];
    int dn = er_dir_read(&dh, de);
    uint32_t count = (dn > 0) ? (uint32_t)dn : 0;
    int32_t hit = -1;
    for (uint32_t i = 0; i < count; i++)
        if ((de[i].flags & ENV_REPLICA_F_VALID) &&
            de[i].partition_id == partition_id && de[i].index == index) {
            hit = (int32_t)i;
            break;
        }
    if (hit >= 0) {
        if (de[hit].seq > record->sequence)
            return er_refuse(ENV_REPLICA_REFUSE_SUPERSEDED,
                             (uint32_t)record->sequence, 0);
    } else {
        /* A NEW identity needs a band slot nobody names. The steady state
         * with fewer than 8 identities always has one; the 9th (or a full
         * band of other environments) is refused BY NAME — never silently
         * dropped, because destination (b)'s classification ladder must be
         * able to report "no replica, and here is why". */
        uint32_t used = 0;
        for (uint32_t i = 0; i < count; i++)
            if (de[i].flags & ENV_REPLICA_F_VALID) used++;
        if (used >= ENV_REPLICA_MAX)
            return er_refuse(ENV_REPLICA_REFUSE_NO_DISK_SLOT, used, 0);
    }

    struct EnvReplicaSlot* s = 0;
    for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX; i++)
        if (!er_staging[i].active) { s = &er_staging[i]; break; }
    if (!s) return er_refuse(ENV_REPLICA_REFUSE_TABLE_FULL, 0, 0);

    s->active       = 1;
    s->complete     = (total_pages == 0) ? 1 : 0;  /* an empty payload IS the record */
    s->partition_id = partition_id;
    s->index        = index;
    s->transfer_id  = transfer_id;
    s->total_pages  = total_pages;
    s->pages_staged = 0;
    s->src_node     = src_node;
    s->opened_tick  = kernel_tick_counter;
    s->record       = *record;
    for (uint32_t p = 0; p < ENV_REPLICA_MAX_PAGES; p++) {
        s->pages[p] = 0;
        s->page_done[p] = 0;
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++)
            s->frags[p][f] = 0;
    }
    return 0;
}

/* ─── Wire entry: one fragment of one payload page ─────────────────────────
 * env_migrate.c's recv_page, for the replica's own table: unknown transfer,
 * out-of-range page/fragment and a frame the partition cannot get are three
 * DIFFERENT refusals (the migration receiver folded them into one; the
 * replica's guard asserts each by name, so they stay apart here). */
int env_replica_recv_page(uint64_t transfer_id, uint32_t page_index,
                          uint32_t frag_index, const uint8_t* data) {
    struct EnvReplicaSlot* s = er_slot_by_transfer(transfer_id);
    if (!s) return er_refuse(ENV_REPLICA_REFUSE_UNKNOWN, (uint32_t)transfer_id, 0);
    if (page_index >= s->total_pages ||
        frag_index >= DSPP_MIGRATE_FRAGS_PER_PAGE)
        return er_refuse(ENV_REPLICA_REFUSE_RANGE, page_index, frag_index);

    if (!s->pages[page_index]) {
        /* Top-down, exactly as the migration receiver: staging ASCENDED
         * would consume the runs create is about to be handed and land the
         * destination's fresh environment off the staged bases (the
         * placement gate's live find). Top-down leaves the watermark
         * where the source's was. Same quota/owner accounting; the drop
         * frees the frame like any other. */
        void* frame = allocate_physical_ram_frame_for_partition_top(s->partition_id);
        if (!frame) return er_refuse(ENV_REPLICA_REFUSE_ALLOC, page_index, 0);
        s->pages[page_index] = (uint8_t*)frame;
    }
    uint8_t* dst = s->pages[page_index] + frag_index * DSPP_MIGRATE_FRAG_BYTES;
    for (uint32_t b = 0; b < DSPP_MIGRATE_FRAG_BYTES; b++) dst[b] = data[b];
    s->frags[page_index][frag_index] = 1;

    for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++)
        if (!s->frags[page_index][f]) return 0;   /* stored; page not yet whole */

    if (!s->page_done[page_index]) {
        s->page_done[page_index] = 1;
        s->pages_staged++;
        if (s->pages_staged == s->total_pages) s->complete = 1;
    }
    return 0;
}

int env_replica_holds(uint64_t transfer_id) {
    return er_slot_by_transfer(transfer_id) != 0;
}

/* ─── The directory (env_replica.h's layout) ──────────────────────────────── */

static int er_dir_read(struct EnvReplicaDirHead* h,
                       struct EnvReplicaDirEntry* e) {
    if (!er_io_ready()) return -1;
    for (uint32_t i = 0; i < ENV_REPLICA_MAX; i++) e[i].flags = 0;
    if (nvme_read_sync(PERSIST_ENV_REPLICA_HDR_LBA, er_io) != 0) return -1;
    for (uint32_t i = 0; i < sizeof(*h); i++) ((uint8_t*)h)[i] = er_io[i];
    if (h->magic != ENV_REPLICA_DIR_MAGIC ||
        h->version != ENV_REPLICA_REC_VERSION ||
        h->rec_size != (uint32_t)sizeof(struct EnvReplicaDirEntry)) {
        return -1;   /* a directory this build cannot read is ABSENT */
    }
    if (nvme_read_sync(PERSIST_ENV_REPLICA_ENT_LBA, er_io) != 0) return -1;
    uint32_t n = h->count;
    if (n > ENV_REPLICA_MAX) n = ENV_REPLICA_MAX;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t b = 0; b < sizeof(struct EnvReplicaDirEntry); b++)
            ((uint8_t*)&e[i])[b] = er_io[i * sizeof(struct EnvReplicaDirEntry) + b];
    return (int)n;
}

static int er_dir_write(const struct EnvReplicaDirEntry* e, uint32_t count) {
    struct EnvReplicaDirHead h;
    h.magic = ENV_REPLICA_DIR_MAGIC;
    h.version = ENV_REPLICA_REC_VERSION;
    h.count = count;
    h.rec_size = (uint32_t)sizeof(struct EnvReplicaDirEntry);
    h._pad = 0;
    /* Entries first, header last — payload's torn-pair rule: a torn pair
     * leaves the old header pointing at entries that never changed, never
     * the reverse. */
    for (uint32_t i = 0; i < 4096; i++) er_io[i] = 0;
    for (uint32_t i = 0; i < count && i < ENV_REPLICA_MAX; i++)
        for (uint32_t b = 0; b < sizeof(struct EnvReplicaDirEntry); b++)
            er_io[i * sizeof(struct EnvReplicaDirEntry) + b] =
                ((const uint8_t*)&e[i])[b];
    if (nvme_write_sync(PERSIST_ENV_REPLICA_ENT_LBA, er_io) != 0) return -1;
    for (uint32_t i = 0; i < 4096; i++) er_io[i] = 0;
    for (uint32_t i = 0; i < sizeof(h); i++) er_io[i] = ((uint8_t*)&h)[i];
    if (nvme_write_sync(PERSIST_ENV_REPLICA_HDR_LBA, er_io) != 0) return -1;
    return 0;
}

/* ─── Finalize: a completed staging becomes a band fact ────────────────────
 * The order IS the contract — refusal over partial application, made
 * durable:
 *
 *   1. completeness      — every page has a frame (defensive; complete is
 *                          only set that way, but a hole found here is a
 *                          named refusal, never a half-written slot);
 *   2. invalidate        — if this identity already has an entry, its VALID
 *                          flag is cleared and the directory written BEFORE
 *                          a band byte moves, so a reader can only ever see
 *                          ABSENT during a rewrite, never mixed captures;
 *   3. band write        — the staged pages, in wire order, slot-relative;
 *   4. publish           — the entry (flag set, seq, crc, record, source)
 *                          and the directory write — entries first.
 *
 * A failure at 2 or 4 leaves the identity ABSENT (honest: destination
 * (b)'s ladder reports absence by name) and the staged frames dropped with
 * a named code; a failure at 3 with an existing entry leaves it ABSENT too
 * (step 2 already invalidated it). Nothing is ever retried silently —
 * the next capture repairs the band.
 *
 * The crc is computed HERE, over the bytes as staged: the receiver's own
 * checksum of what actually crossed, stored for whoever reads the slot
 * back (promotion and the guard recompute and compare). */
static int er_finalize(struct EnvReplicaSlot* s) {
    if (!er_io_ready()) return 0;   /* device not up yet: not a refusal, retry */

    for (uint32_t p = 0; p < s->total_pages; p++) {
        if (!s->pages[p]) {
            er_refuse(ENV_REPLICA_REFUSE_INCOMPLETE, p, s->total_pages);
            kernel_serial_printf(
                "[ENV_REPLICA] finalize (partition %u, index %u): %s (page %u of %u "
                "has no frame) -- staged frames dropped; no replica is recorded.\n",
                (unsigned)s->partition_id, (unsigned)s->index,
                env_replica_refusal_name(ENV_REPLICA_REFUSE_INCOMPLETE),
                (unsigned)p, (unsigned)s->total_pages);
            er_slot_drop(s);
            return 0;
        }
    }

    struct EnvReplicaDirHead h;
    struct EnvReplicaDirEntry e[ENV_REPLICA_MAX];
    int n = er_dir_read(&h, e);
    uint32_t count = (n > 0) ? (uint32_t)n : 0;
    if (n < 0) count = 0;   /* absent/never-written: start the directory fresh */

    /* Locate this identity; verify this capture supersedes what is there. */
    int32_t existing = -1;
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!(e[i].flags & ENV_REPLICA_F_VALID)) continue;
        used++;
        if (e[i].partition_id == s->partition_id && e[i].index == s->index)
            existing = (int32_t)i;
    }
    if (existing >= 0 && e[existing].seq > s->record.sequence) {
        er_refuse(ENV_REPLICA_REFUSE_SUPERSEDED, (uint32_t)s->record.sequence,
                  (uint32_t)e[existing].seq);
        kernel_serial_printf(
            "[ENV_REPLICA] finalize (partition %u, index %u): %s (incoming seq %u, "
            "recorded seq %u) -- staged frames dropped; the newer replica stands.\n",
            (unsigned)s->partition_id, (unsigned)s->index,
            env_replica_refusal_name(ENV_REPLICA_REFUSE_SUPERSEDED),
            (unsigned)s->record.sequence, (unsigned)e[existing].seq);
        er_slot_drop(s);
        return 0;
    }

    /* Choose the band slot: a replacement keeps its own slot (step 2 has
     * already made it absent, so in-place cannot show mixed captures); a new
     * identity takes a slot no valid entry names. */
    uint32_t slot = 0;
    int have_slot = 0;
    if (existing >= 0) {
        slot = e[existing].slot;
        have_slot = 1;
    } else {
        for (uint32_t cand = 0; cand < ENV_REPLICA_MAX && !have_slot; cand++) {
            int named = 0;
            for (uint32_t i = 0; i < count; i++)
                if ((e[i].flags & ENV_REPLICA_F_VALID) && e[i].slot == cand)
                    named = 1;
            if (!named) { slot = cand; have_slot = 1; }
        }
    }
    if (!have_slot) {
        /* Checked at BEGIN too — this is the race-free re-check: a second
         * identity staged while the band filled. Named, never squeezed in. */
        er_refuse(ENV_REPLICA_REFUSE_NO_DISK_SLOT, used, 0);
        kernel_serial_printf(
            "[ENV_REPLICA] finalize (partition %u, index %u): %s (%u/%u band slots "
            "live) -- staged frames dropped; no replica is recorded.\n",
            (unsigned)s->partition_id, (unsigned)s->index,
            env_replica_refusal_name(ENV_REPLICA_REFUSE_NO_DISK_SLOT),
            (unsigned)used, (unsigned)ENV_REPLICA_MAX);
        er_slot_drop(s);
        return 0;
    }

    /* Step 2: invalidate the old entry before a band byte moves. */
    if (existing >= 0) {
        e[existing].flags &= ~(uint32_t)ENV_REPLICA_F_VALID;
        if (er_dir_write(e, count) != 0) {
            er_refuse(ENV_REPLICA_REFUSE_IO, 2, 0);
            kernel_serial_printf(
                "[ENV_REPLICA] finalize (partition %u, index %u): %s (invalidation "
                "write) -- staged frames dropped; no replica is recorded.\n",
                (unsigned)s->partition_id, (unsigned)s->index,
                env_replica_refusal_name(ENV_REPLICA_REFUSE_IO));
            er_slot_drop(s);
            return 0;
        }
    }

    /* The receiver's own checksum, over the bytes as staged. */
    uint32_t crc = 0;
    for (uint32_t p = 0; p < s->total_pages; p++)
        crc = er_crc(crc, s->pages[p], 4096);

    /* Step 3: the band. Page p at slot_lba + p*8 sectors (4 KiB = 8 x 512). */
    for (uint32_t p = 0; p < s->total_pages; p++) {
        if (nvme_write_sync(ENV_REPLICA_SLOT_LBA(slot) + (uint64_t)p * 8ull,
                            s->pages[p]) != 0) {
            er_refuse(ENV_REPLICA_REFUSE_IO, p, s->total_pages);
            kernel_serial_printf(
                "[ENV_REPLICA] finalize (partition %u, index %u): %s (band page %u "
                "of %u) -- staged frames dropped; no replica is recorded.\n",
                (unsigned)s->partition_id, (unsigned)s->index,
                env_replica_refusal_name(ENV_REPLICA_REFUSE_IO),
                (unsigned)p, (unsigned)s->total_pages);
            er_slot_drop(s);
            return 0;
        }
    }

    /* Step 4: publish the entry (or append the new one). */
    int32_t pub = existing;
    if (pub < 0) {
        if (count >= ENV_REPLICA_MAX) {          /* defensive: BEGIN checked too */
            er_refuse(ENV_REPLICA_REFUSE_NO_DISK_SLOT, count, 1);
            er_slot_drop(s);
            return 0;
        }
        pub = (int32_t)count;
        count++;
        for (uint32_t b = 0; b < sizeof(e[0]); b++) ((uint8_t*)&e[pub])[b] = 0;
    }
    e[pub].partition_id = s->partition_id;
    e[pub].index        = s->index;
    e[pub].slot         = slot;
    e[pub].flags        = ENV_REPLICA_F_VALID;
    e[pub].seq          = s->record.sequence;
    e[pub].pages        = s->total_pages;
    e[pub].src_node     = s->src_node;
    e[pub].blob_crc     = crc;
    e[pub]._pad         = 0;
    e[pub].record       = s->record;
    if (er_dir_write(e, count) != 0) {
        er_refuse(ENV_REPLICA_REFUSE_IO, 3, 0);
        kernel_serial_printf(
            "[ENV_REPLICA] finalize (partition %u, index %u): %s (publish write) -- "
            "staged frames dropped; no replica is recorded.\n",
            (unsigned)s->partition_id, (unsigned)s->index,
            env_replica_refusal_name(ENV_REPLICA_REFUSE_IO));
        er_slot_drop(s);
        return 0;
    }

    kernel_serial_printf(
        "[ENV_REPLICA] finalized (partition %u, index %u, seq %u): %u page(s), "
        "crc=0x%08x -> band slot %u, from node %u\n",
        (unsigned)s->partition_id, (unsigned)s->index,
        (unsigned)s->record.sequence, (unsigned)s->total_pages,
        (unsigned)crc, (unsigned)slot, (unsigned)s->src_node);

    er_slot_drop(s);
    return 1;
}

/* ─── The finalize pass (microkernel.c's service poll) ───────────────────── */
int env_replica_tick(void) {
    int done = 0;
    for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX; i++) {
        struct EnvReplicaSlot* s = &er_staging[i];
        if (!s->active) continue;
        if (!s->complete) {
            /* A transfer whose sender vanished holds partition frames for
             * nothing: bounded by age (env_replica.h's contract), dropped
             * and named — the retry that never came is not our retry to
             * invent. */
            if (kernel_tick_counter - s->opened_tick > ENV_REPLICA_STALE_TICKS) {
                kernel_serial_printf(
                    "[ENV_REPLICA] staging (partition %u, index %u) STALE after "
                    "%u tick(s): %u/%u page(s) staged -- frames returned to the "
                    "partition; no replica is recorded.\n",
                    (unsigned)s->partition_id, (unsigned)s->index,
                    (unsigned)(kernel_tick_counter - s->opened_tick),
                    (unsigned)s->pages_staged, (unsigned)s->total_pages);
                er_refuse(ENV_REPLICA_REFUSE_STALE, s->pages_staged, s->total_pages);
                er_slot_drop(s);
            }
            continue;
        }
        done += er_finalize(s);
    }
    return done;
}

/* ─── The finder ──────────────────────────────────────────────────────────── */

int env_replica_lookup(uint32_t partition_id, uint32_t index,
                       struct EnvReplicaDirEntry* out) {
    struct EnvReplicaDirHead h;
    struct EnvReplicaDirEntry e[ENV_REPLICA_MAX];
    int n = er_dir_read(&h, e);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++)
        if ((e[i].flags & ENV_REPLICA_F_VALID) &&
            e[i].partition_id == partition_id && e[i].index == index) {
            if (out) *out = e[i];
            return 1;
        }
    return 0;
}

int env_replica_present(uint32_t partition_id, uint32_t index) {
    return env_replica_lookup(partition_id, index, 0);
}

uint32_t env_replica_count(void) {
    struct EnvReplicaDirHead h;
    struct EnvReplicaDirEntry e[ENV_REPLICA_MAX];
    int n = er_dir_read(&h, e);
    if (n <= 0) return 0;
    uint32_t live = 0;
    for (int i = 0; i < n; i++)
        if (e[i].flags & ENV_REPLICA_F_VALID) live++;
    return live;
}

int env_replica_read_page(uint32_t partition_id, uint32_t index,
                          uint32_t page, uint8_t* buf) {
    struct EnvReplicaDirEntry e;
    if (!env_replica_lookup(partition_id, index, &e)) return 1;
    if (page >= e.pages || !buf) return 2;
    if (!er_io_ready()) return 3;
    if (nvme_read_sync(ENV_REPLICA_SLOT_LBA(e.slot) + (uint64_t)page * 8ull,
                       buf) != 0) return 4;
    return 0;
}

/* ─── PROMOTION (P3 destination (a), increment 3) ─────────────────────────
 * See env_replica.h for the contract. Stages only: the caller (failover.c)
 * makes step 2 — env_migrate_restore() — and classifies on its status, so
 * one restore decision has one classifier instead of two.
 *
 * Why feed the MIGRATION receiver rather than pour the band into the
 * payload blob: these bytes crossed the wire in the migration page
 * numbering (record whole in the BEGIN; region frames, durable extent,
 * image-window tail in page order) because the replica push IS a migration
 * stream with a different begin opcode. The blob format ([head][tasks]
 * [vaddrs][console]...) never crossed, and its tasks/vaddrs are re-derived
 * from the FRESH sidecars at restore — env_migrate_restore() already does
 * exactly that. A second reader of wire-ordered pages would be a second
 * place for the numbering to rot; this way there is one. */
static struct EnvReplicaRefusal er_promo;   /* last promotion's code + operands */

static int er_promote_refuse(uint32_t code, uint32_t a, uint32_t b) {
    er_promo.code = code;
    er_promo.a = a;
    er_promo.b = b;
    kernel_serial_printf(
        "[ENV_REPLICA] promotion REFUSED: %s (a=%u, b=%u) -- nothing was "
        "staged; refusal over partial application.\n",
        env_replica_promotion_name(code), (unsigned)a, (unsigned)b);
    return (int)code;
}

struct EnvReplicaRefusal env_replica_promotion_last(void) {
    return er_promo;
}

const char* env_replica_promotion_name(uint32_t code) {
    switch (code) {
    case ENV_REPLICA_PROMOTE_OK:                return "ok";
    case ENV_REPLICA_PROMOTE_REFUSE_ABSENT:     return "no replica for this identity";
    case ENV_REPLICA_PROMOTE_REFUSE_RECORD:     return "the replica's record failed validation";
    case ENV_REPLICA_PROMOTE_REFUSE_PAGES:      return "page count disagrees with the record";
    case ENV_REPLICA_PROMOTE_REFUSE_IO:         return "a replica band page could not be read";
    case ENV_REPLICA_PROMOTE_REFUSE_STAGE:      return "the migration receiver refused the feed";
    case ENV_REPLICA_PROMOTE_REFUSE_INCOMPLETE: return "the staged slot came up incomplete";
    default:                                    return "unknown promotion status";
    }
}

int env_replica_promote(uint32_t partition_id, uint32_t index) {
    er_promo.code = ENV_REPLICA_PROMOTE_OK;
    er_promo.a = er_promo.b = 0;

    /* The device first, before the lookup that reads it: with no device,
     * env_replica_lookup() cannot find an entry that exists, and reporting
     * its miss as ABSENT would tell an operator "no replica is held here"
     * about a disk we simply could not read. The gate is also the cheapest
     * refusal — it fires before a directory read, let alone a frame. */
    if (!er_io_ready())
        return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_IO, 0, 0);

    struct EnvReplicaDirEntry e;
    if (!env_replica_lookup(partition_id, index, &e))
        return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_ABSENT, 0, 0);

    int vr = env_ckpt_valid(&e.record);
    if (vr != 0)
        return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_RECORD, (uint32_t)vr, 0);

    /* The directory's own pages field against the record it carries: the
     * same range the BEGIN gate enforced on the way in (payload pages, plus
     * an image tail no larger than the ceiling). A torn directory entry is
     * caught HERE, before a single frame is charged to the partition. */
    uint32_t payload = env_migrate_payload_pages(&e.record);
    if (e.pages < payload || e.pages > payload + ENV_MIGRATE_IMAGE_MAX ||
        e.pages > ENV_REPLICA_MAX_PAGES)
        return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_PAGES, e.pages, payload);

    /* The wire's transfer id, derived the way both families derive it —
     * this makes a re-promote of an identity already staged (a previous
     * attempt's kept slot) answer READY instead of refused, and re-feeds
     * its fragments idempotently. */
    uint64_t tid = ((uint64_t)partition_id << 32) | (uint64_t)index;
    int rc = env_migrate_recv_begin(tid, partition_id, index, e.pages,
                                    &e.record,
                                    (uint32_t)sizeof(struct EnvCkptRecord));
    if (rc != 0)
        return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_STAGE, (uint32_t)rc, 0);

    for (uint32_t p = 0; p < e.pages; p++) {
        if (nvme_read_sync(ENV_REPLICA_SLOT_LBA(e.slot) + (uint64_t)p * 8ull,
                           er_io) != 0) {
            env_migrate_recv_drop(partition_id, index);
            return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_IO, p, 0);
        }
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
            int fr = env_migrate_recv_page(
                tid, p, f, er_io + (uint64_t)f * DSPP_MIGRATE_FRAG_BYTES);
            if (fr != 0) {
                env_migrate_recv_drop(partition_id, index);
                return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_STAGE,
                                         (uint32_t)fr, p);
            }
        }
    }

    /* Defensive: the feed above wrote every page, so complete must hold.
     * If it does not, the slot can never be restored -- drop it rather than
     * leave a slot the sweep would walk forever. */
    const struct EnvMigrateStaging* v = env_migrate_recv_find(partition_id, index);
    if (!v || !v->complete || v->total_pages != e.pages) {
        uint32_t staged = (v) ? v->pages_staged : 0;
        env_migrate_recv_drop(partition_id, index);
        return er_promote_refuse(ENV_REPLICA_PROMOTE_REFUSE_INCOMPLETE,
                                 staged, e.pages);
    }

    kernel_serial_printf(
        "[ENV_REPLICA] PROMOTED replica (partition %u, index %u): %u page(s), "
        "capture seq %llu, provenance node %u -- staged for the migration "
        "restore path.\n",
        (unsigned)partition_id, (unsigned)index, (unsigned)e.pages,
        (unsigned long long)e.seq, (unsigned)e.src_node);
    return ENV_REPLICA_PROMOTE_OK;
}

void env_replica_reset(void) {
    for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX; i++)
        if (er_staging[i].active) er_slot_drop(&er_staging[i]);
    er_refusal.code = 0;
    er_refusal.a = 0;
    er_refusal.b = 0;
    er_promo.code = ENV_REPLICA_PROMOTE_OK;
    er_promo.a = 0;
    er_promo.b = 0;
}

/* ─── The SENDER (increment 2) ───────────────────────────────────────────────
 * The capture-side push: same wire, same page numbering, same retransmission
 * ladder as a migration — via env_migrate_send_stream(), which takes the
 * BEGIN opcode (25, so the peer stages it in ITS replica table) and this
 * module's log tag as parameters. What is new is the LAST mile: staging is
 * proven by the PAGE ACKs, but finalization (the peer's validate → band
 * write → publish) happens on the peer's own service tick and can refuse
 * after every page landed — a refusal the ACKs structurally cannot carry.
 * So the source ASKS. Opcodes 26/27, fire-and-forget on a bounded retry,
 * answered from the peer's DISK directory — which is what makes "the copy
 * is a fact the survivor holds" a checked statement rather than a hopeful
 * one, and what makes a peer-side NO_DISK_SLOT/SUPERSEDED/IO refusal
 * visible HERE instead of only in the peer's log. */

static uint64_t er_confirm_gen;        /* bumped by every answer */
static uint32_t er_confirm_partition;
static uint32_t er_confirm_index;
static int      er_confirm_status;      /* 0 = finalized on the peer */

void env_replica_confirm_note(uint32_t partition_id, uint32_t index,
                              uint32_t seq_low, uint8_t status) {
    (void)seq_low;   /* the answer already reflects seq (the query compared
                      * it); the poll validates by identity, not sequence */
    er_confirm_partition = partition_id;
    er_confirm_index     = index;
    er_confirm_status    = (status == 0) ? 0 : 1;
    er_confirm_gen++;
}

int env_replica_live_query(uint32_t partition_id, uint32_t index,
                           uint32_t seq_low) {
    struct EnvReplicaDirEntry e;
    if (!env_replica_lookup(partition_id, index, &e)) return 1;  /* absent */
    if ((uint32_t)e.seq < seq_low)
        return 2;   /* a replica exists, but an OLDER capture than the one asking */
    return 0;       /* finalized here, this capture or newer */
}

/* The bounded confirm: ask, wait for a FRESH answer about THIS identity,
 * re-ask until the bound or a definitive answer. Only "finalized here" is
 * definitive: the peer's finalize pass runs on its next service poll — one
 * tick after staging completes — so the first question can honestly beat
 * it, and an "absent" reply must RE-ASK rather than end the ladder (live
 * find, 2026-10-28: the first reply returned from the whole function, so
 * one question, one "not yet" and the push reported NOT finalized while the
 * peer finalized a tick later on every peer of every push). Returns 0
 * finalized, 1 the peer answered not-finalized/older on every ask, 2 never
 * answered. The pace between asks spans the peer's service poll — the same
 * period the migration's confirmation walk gets for free by asking once per
 * sweep period — and the ladder's TOTAL span (6 × 120 ticks ≈ 7 s at
 * HZ=100) has to cover the peer's finalize itself: a synchronous band
 * write of the whole payload (1947 pages ≈ 7.6 MiB through emulated NVMe,
 * seconds — not ticks). Live run, 2026-10-28: at 4 × 5 ticks the ladder
 * exhausted in ~250 ms while the band write was still in flight — four
 * honest "absent" answers for a replica that landed moments later. The
 * block this adds sits inside capture's freeze, where the page stream
 * already spends minutes: bounded, and small beside it. */
#define ENV_REPLICA_CONFIRM_TRIES 6u
#define ENV_REPLICA_CONFIRM_PACE_TICKS 120u
static int er_confirm_finalized(uint32_t partition_id, uint32_t index,
                                uint64_t seq, uint32_t dest) {
    uint64_t tid = ((uint64_t)partition_id << 32) | (uint64_t)index;
    int saw_answer = 0;   /* any fresh reply: 1 = the peer keeps saying not-yet */
    for (uint32_t t = 0; t < ENV_REPLICA_CONFIRM_TRIES; t++) {
        uint64_t gen = er_confirm_gen;
        dspp_env_replica_send_live(tid, dest, partition_id, index,
                                   (uint32_t)seq);
        uint64_t deadline  = kernel_tick_counter + DSPP_MIGRATE_ACK_TIMEOUT_TICKS;
        uint64_t last_tick = kernel_tick_counter;
        uint32_t stalled   = 0;
        while (kernel_tick_counter < deadline) {
            if (er_confirm_gen != gen &&
                er_confirm_partition == partition_id &&
                er_confirm_index == index) {
                if (er_confirm_status == 0) return 0;   /* finalized: done */
                saw_answer = 1;                        /* not yet: pace, re-ask */
                break;
            }
            if (kernel_tick_counter != last_tick) {
                last_tick = kernel_tick_counter;
                stalled = 0;
            } else if (++stalled > DSPP_MIGRATE_STALL_SPINS) break;
            failover_sweep_keepalive();   /* this runs inside capture's freeze:
                                           * the same "a blocked sender must keep
                                           * beating" rule the migration waits hold */
            __asm__ volatile("pause");
        }
        /* A lost question is re-asked, exactly like the migration's LIVE
         * poll — and so is an honest "not yet": pace so the next question
         * lands in a LATER service-poll cycle of the peer. The stall bound
         * keeps a host test (whose fake clock only moves when frames fly)
         * from spinning on a pace that nothing will tick. */
        {
            uint64_t pace_deadline = kernel_tick_counter +
                                     ENV_REPLICA_CONFIRM_PACE_TICKS;
            uint64_t pace_last = kernel_tick_counter;
            uint32_t pace_stalled = 0;
            while (kernel_tick_counter < pace_deadline) {
                if (kernel_tick_counter != pace_last) {
                    pace_last = kernel_tick_counter;
                    pace_stalled = 0;
                } else if (++pace_stalled > DSPP_MIGRATE_STALL_SPINS) break;
                failover_sweep_keepalive();
                __asm__ volatile("pause");
            }
        }
    }
    return saw_answer ? 1 : 2;
}

int env_replica_push(const struct EnvCkptRecord* rec) {
    if (!rec) return 0;

    uint32_t confirmed = 0, peers = 0;

    /* Announce BEFORE the first block, release after the last: this push
     * runs inside capture's freeze, where this node transmits nothing but
     * the push's own ACK waits — and the wait loops here cannot save a
     * sweep that IS the blocked context. env_migrate_intent_begin()'s own
     * contract ("the announce must be on the wire before the caller
     * blocks"), reused because it is the same sentence: every intent frame
     * is also liveness evidence for its sender. */
    env_migrate_intent_begin();

    for (uint32_t r = 0; r < cluster_roster_count; r++) {
        if (!cluster_roster[r].active) continue;
        uint32_t peer = cluster_roster[r].node_id;
        if (peer == 0 || peer == cluster_local_node_id()) continue;
        peers++;

        int rc = env_migrate_send_stream(rec, peer,
                                         DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ,
                                         "[ENV_REPLICA]");
        if (rc != ENV_MIGRATE_OK) {
            kernel_serial_printf(
                "[ENV_REPLICA] push (partition %u, index %u, seq %u) to node %u: "
                "pages NOT confirmed (%s) -- no replica exists there; the "
                "destination (b) ladder stands on that node.\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned)rec->sequence, (unsigned)peer,
                env_migrate_status_name(rc));
            continue;
        }

        int cf = er_confirm_finalized(rec->partition_id, rec->index,
                                      rec->sequence, peer);
        if (cf == 0) {
            kernel_serial_printf(
                "[ENV_REPLICA] push (partition %u, index %u, seq %u): FINALIZED on "
                "node %u -- the copy is a fact that survivor holds.\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned)rec->sequence, (unsigned)peer);
            confirmed++;
        } else if (cf == 1) {
            kernel_serial_printf(
                "[ENV_REPLICA] push (partition %u, index %u, seq %u) to node %u: "
                "staged but NOT finalized -- that node's log names the gate; "
                "treat the replica as ABSENT there.\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned)rec->sequence, (unsigned)peer);
        } else {
            kernel_serial_printf(
                "[ENV_REPLICA] push (partition %u, index %u, seq %u) to node %u: "
                "no answer to the confirm poll -- staging unverified; treat the "
                "replica as ABSENT there.\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned)rec->sequence, (unsigned)peer);
        }
    }

    env_migrate_intent_end();

    if (peers == 0) {
        kernel_serial_printf(
            "[ENV_REPLICA] push (partition %u, index %u): no active peer in the "
            "roster -- nothing to replicate to.\n",
            (unsigned)rec->partition_id, (unsigned)rec->index);
    }
    return (int)confirmed;
}
