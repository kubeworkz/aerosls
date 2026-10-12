/*
 * env_migrate.c — POSIX-Environments P3 increment 2: the transfer.
 * See env_migrate.h for the contract; this file is the mechanics.
 *
 * The sender mirrors kernel/stream.c's stream_migrate_send_partition()
 * discipline exactly — confirm the BEGIN before sending anything, confirm
 * every fragment of every page, abandon without touching the source on any
 * refusal, retransmit only what did not land — because that discipline is
 * the one partition_migrate() already trusts with a partition's streams, and
 * an environment's footprint deserves the same treatment, not a softer one.
 *
 * The receiver stages pages into PARTITION-CHARGED frames (the same
 * accounting rule the environment's own regions live under: a tenant's bytes
 * count against the tenant, on whichever node holds them), assembles them
 * per-page from the wire fragments, and hands nothing out as complete until
 * every page the record names has landed.
 */
#include "env_migrate.h"
#include "frame_pool.h"
#include "kernel_io.h"
#include "../drivers/nvme_io.h"
#include "../net/dspp.h"
#include "../net/consensus.h"   /* cluster_local_node_id() -- the destination guard */

/* The descriptor blob must fit the wire's BEGIN packet — the record crosses
 * whole by design (env_migrate.h), so a record that outgrew the blob is a
 * build failure, not a truncated migration. */
_Static_assert(sizeof(struct EnvCkptRecord) <= DSPP_ENV_MIGRATE_RECORD_MAX,
               "EnvCkptRecord no longer fits the env-migrate BEGIN packet");

/* extern uint64_t kernel_tick_counter — the ACK waits spin on it exactly as
 * stream.c's do (the timer ISR is what delivers ACKs on real hardware); the
 * host test drives the whole transfer synchronously, so the counter never
 * advances there and the stall bound (not the deadline) is what ends an
 * unanswerable wait. Declared here the way net/dspp.c declares it, for the
 * same reason: no intermediate layer has a use for it. */
extern volatile uint64_t kernel_tick_counter;

/* 8 NVMe sectors per 4 KiB page — the stream family's geometry, reused so
 * the two payload walkers cannot disagree about page boundaries. */
#define ENV_MIGRATE_SECTORS_PER_PAGE 8u

static int env_migrate_status = ENV_MIGRATE_OK;
int env_migrate_last_status(void) { return env_migrate_status; }

uint32_t env_migrate_payload_pages(const struct EnvCkptRecord* rec) {
    if (!rec) return 0;
    uint32_t pages = 0;
    for (uint32_t r = 0; r < rec->n_regions && r < ENV_CKPT_MAX_REGIONS; r++)
        pages += rec->regions[r].frames;
    if (rec->state_sectors != 0)
        pages += (rec->state_sectors + ENV_MIGRATE_SECTORS_PER_PAGE - 1)
                 / ENV_MIGRATE_SECTORS_PER_PAGE;
    return pages;
}

/* ─── Reading one payload page, source side ────────────────────────────────
 * Page numbering is the record's own: every region's frames in record order,
 * then the durable extent's pages. Regions are frame-pool frames —
 * identity-mapped on the real kernel (frame_pool.h's contract), real host
 * pointers under the host test's settable frame fake — so a region page is
 * one memcpy either way. Extent pages come off NVMe, 8 sectors at a time. */
static int env_migrate_read_page(const struct EnvCkptRecord* rec,
                                 uint32_t page_index, uint8_t* buf) {
    uint32_t p = page_index;
    for (uint32_t r = 0; r < rec->n_regions && r < ENV_CKPT_MAX_REGIONS; r++) {
        uint32_t frames = rec->regions[r].frames;
        if (p < frames) {
            const uint8_t* src =
                (const uint8_t*)(rec->regions[r].base + (uint64_t)p * 4096u);
            for (uint32_t b = 0; b < 4096; b++) buf[b] = src[b];
            return 0;
        }
        p -= frames;
    }
    if (rec->state_sectors == 0) return -1;   /* page beyond the payload */
    uint64_t slba = rec->state_lba + (uint64_t)p * ENV_MIGRATE_SECTORS_PER_PAGE;
    return nvme_read_sync(slba, buf);
}

/* ─── Sender ──────────────────────────────────────────────────────────────── */

int env_migrate_send(uint32_t partition_id, uint32_t index, uint32_t dest_node_id) {
    env_migrate_status = ENV_MIGRATE_OK;

    /* The same two identity guards partition_migrate() opens with: the
     * system partition never moves, and node 0 is the 'uninitialized'
     * sentinel, not a destination. An environment's migration inherits the
     * partition's rules rather than inventing softer ones. */
    if (partition_id == PARTITION_SYSTEM) {
        env_migrate_status = ENV_MIGRATE_REFUSE_SYSTEM;
        return env_migrate_status;
    }
    if (dest_node_id == 0 || dest_node_id == cluster_local_node_id()) {
        env_migrate_status = ENV_MIGRATE_REFUSE_DEST;
        return env_migrate_status;
    }

    const struct EnvCkptRecord* rec = env_ckpt_find(partition_id, index);
    if (!rec) {
        env_migrate_status = ENV_MIGRATE_REFUSE_NO_RECORD;
        return env_migrate_status;
    }
    /* A capture that never froze the partition describes a moving target —
     * env_ckpt's own restore rule 2, enforced at the send end too: what would
     * cross is bytes torn between two writes. */
    if ((rec->flags & ENV_CKPT_FLAG_QUIESCED) == 0) {
        env_migrate_status = ENV_MIGRATE_REFUSE_UNQUIESCED;
        return env_migrate_status;
    }

    uint32_t total_pages = env_migrate_payload_pages(rec);
    uint64_t transfer_id = ((uint64_t)partition_id << 32) | (uint64_t)index;

    static uint8_t __attribute__((aligned(4096))) send_page_buf[4096];

    /* ── The BEGIN, confirmed before anything else ───────────────────────
     * Same reason as the stream family's: the destination must have staged
     * the slot (and, for an environment with no pages at all — no regions
     * and no extent — this is the ONLY confirmation that will exist) before
     * the source may believe anything moved. */
    int begin_ok = 0;
    dspp_migrate_arm_begin(transfer_id);
    for (uint32_t attempt = 0; attempt < DSPP_MIGRATE_MAX_ATTEMPTS; attempt++) {
        /* Send at the TOP of each attempt, exactly as stream.c does: the
         * first send happens here rather than before the loop, so the last
         * retransmission is never a frame nobody waits for. */
        dspp_env_migrate_send_begin(transfer_id, dest_node_id, partition_id, index,
                                    total_pages, rec, (uint32_t)sizeof(*rec));
        uint64_t deadline  = kernel_tick_counter + DSPP_MIGRATE_ACK_TIMEOUT_TICKS;
        uint64_t last_tick = kernel_tick_counter;
        uint32_t stalled   = 0;
        while (kernel_tick_counter < deadline) {
            if (dspp_migrate_begin_acked()) { begin_ok = 1; break; }
            if (dspp_migrate_nacked()) break;
            if (kernel_tick_counter != last_tick) { last_tick = kernel_tick_counter; stalled = 0; }
            else if (++stalled > DSPP_MIGRATE_STALL_SPINS) break;
            __asm__ volatile("pause");
        }
        if (begin_ok || dspp_migrate_nacked()) break;
    }
    dspp_migrate_disarm();

    if (!begin_ok) {
        kernel_serial_printf(
            "[ENV_MIGRATE] env (partition %u, index %u): node %u never confirmed "
            "the BEGIN%s -- transfer abandoned, source untouched.\n",
            (unsigned)partition_id, (unsigned)index, (unsigned)dest_node_id,
            dspp_migrate_nacked() ? " (refused: no staging slot)" : "");
        env_migrate_status = ENV_MIGRATE_REFUSE_BEGIN;
        return env_migrate_status;
    }

    /* ── Every page, confirmed fragment by fragment ────────────────────── */
    for (uint32_t p = 0; p < total_pages; p++) {
        if (env_migrate_read_page(rec, p, send_page_buf) != 0) {
            kernel_serial_printf(
                "[ENV_MIGRATE] env (partition %u, index %u): source page %u "
                "unreadable -- transfer abandoned at %u of %u page(s), source "
                "untouched.\n",
                (unsigned)partition_id, (unsigned)index, (unsigned)p,
                (unsigned)p, (unsigned)total_pages);
            env_migrate_status = ENV_MIGRATE_REFUSE_READ;
            return env_migrate_status;
        }

        int page_ok = 0;
        dspp_migrate_arm_page(transfer_id, p);
        dspp_env_migrate_send_page(transfer_id, dest_node_id, partition_id,
                                   p, send_page_buf);
        for (uint32_t attempt = 0; attempt < DSPP_MIGRATE_MAX_ATTEMPTS; attempt++) {
            uint64_t deadline = kernel_tick_counter + DSPP_MIGRATE_ACK_TIMEOUT_TICKS;
            uint64_t last_tick = kernel_tick_counter;
            uint32_t stalled   = 0;
            while (kernel_tick_counter < deadline) {
                if (dspp_migrate_page_acked()) { page_ok = 1; break; }
                if (dspp_migrate_nacked()) break;
                if (kernel_tick_counter != last_tick) { last_tick = kernel_tick_counter; stalled = 0; }
                else if (++stalled > DSPP_MIGRATE_STALL_SPINS) break;
                __asm__ volatile("pause");
            }
            if (page_ok || dspp_migrate_nacked()) break;
            /* Timed out: resend only the fragments the destination never
             * acknowledged — the stream family's exact lesson. */
            for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
                if (dspp_migrate_frag_acked(f)) continue;
                dspp_env_migrate_send_frag(transfer_id, dest_node_id,
                                           partition_id, p, f, send_page_buf);
            }
        }
        dspp_migrate_disarm();

        if (!page_ok) {
            kernel_serial_printf(
                "[ENV_MIGRATE] env (partition %u, index %u): page %u of %u never "
                "confirmed%s -- transfer abandoned, source untouched; the "
                "destination's partial slot is droppable.\n",
                (unsigned)partition_id, (unsigned)index, (unsigned)p,
                (unsigned)total_pages,
                dspp_migrate_nacked() ? " (destination refused)" : "");
            env_migrate_status = ENV_MIGRATE_REFUSE_PAGE;
            return env_migrate_status;
        }
    }

    kernel_serial_printf(
        "[ENV_MIGRATE] env (partition %u, index %u): all %u page(s) confirmed "
        "by node %u; source untouched, awaiting the caller's decision.\n",
        (unsigned)partition_id, (unsigned)index, (unsigned)total_pages,
        (unsigned)dest_node_id);
    return ENV_MIGRATE_OK;
}

/* ─── Receiver ────────────────────────────────────────────────────────────── */

/* A small bounded table, like the stream receive slots': the sender is
 * synchronous, so one transfer is in flight at a time, and the partition-
 * migrate critical section (increment 3) will serialize concurrent moves the
 * way it already does for streams. Runtime state only — see env_migrate.h. */
#define ENV_MIGRATE_STAGING_MAX 2
#define ENV_MIGRATE_MAX_PAGES   1536   /* >= any real payload: 3 regions' frames (1344) + the extent's pages */

struct EnvMigrateSlot {
    uint8_t  active;
    uint8_t  complete;
    uint32_t partition_id;
    uint32_t index;
    uint64_t transfer_id;
    uint32_t total_pages;
    uint32_t pages_staged;     /* distinct pages whose fragments all landed */
    struct EnvCkptRecord record;
    /* Staged bytes: one partition-charged frame per payload page. */
    uint8_t* pages[ENV_MIGRATE_MAX_PAGES];
    /* Per-page assembly state: which of the page's 4 fragments landed, and
     * whether the whole page has already been counted (a duplicate fragment
     * after the page is whole must not double-count it). */
    uint8_t  frags[ENV_MIGRATE_MAX_PAGES][DSPP_MIGRATE_FRAGS_PER_PAGE];
    uint8_t  page_done[ENV_MIGRATE_MAX_PAGES];
};

static struct EnvMigrateSlot env_migrate_staging[ENV_MIGRATE_STAGING_MAX];

uint32_t env_migrate_staging_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++)
        if (env_migrate_staging[i].active) n++;
    return n;
}

static struct EnvMigrateSlot* env_migrate_slot(uint32_t partition_id, uint32_t index) {
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++) {
        struct EnvMigrateSlot* s = &env_migrate_staging[i];
        if (s->active && s->partition_id == partition_id && s->index == index)
            return s;
    }
    return 0;
}

int env_migrate_recv_drop(uint32_t partition_id, uint32_t index) {
    struct EnvMigrateSlot* s = env_migrate_slot(partition_id, index);
    if (!s) return 1;
    for (uint32_t p = 0; p < ENV_MIGRATE_MAX_PAGES; p++) {
        if (s->pages[p]) {
            free_physical_ram_frame_for_partition(s->pages[p], s->partition_id);
            s->pages[p] = 0;
        }
        s->page_done[p] = 0;
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) s->frags[p][f] = 0;
    }
    s->active       = 0;
    s->complete     = 0;
    s->pages_staged = 0;
    return 0;
}

void env_migrate_reset(void) {
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++) {
        struct EnvMigrateSlot* s = &env_migrate_staging[i];
        if (s->active) env_migrate_recv_drop(s->partition_id, s->index);
    }
}

/* Wire entry: the destination has received an ENV BEGIN. Called from
 * net/dspp.c's dispatch (the timer ISR on real hardware). Returns 0 when the
 * slot is staged and ready, nonzero — refused — otherwise; dspp.c turns the
 * value into the BEGIN_ACK's status. */
int env_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                           uint32_t index, uint32_t total_pages,
                           const struct EnvCkptRecord* record, uint32_t record_len) {
    if (!record) return 1;
    if (record_len != (uint32_t)sizeof(struct EnvCkptRecord)) return 1;
    /* Both sides number pages from the same record; a declared total that
     * disagrees with the record means the two kernels would disagree about
     * what "every page landed" means — refused rather than reconciled. */
    if (total_pages != env_migrate_payload_pages(record)) return 1;
    if (total_pages > ENV_MIGRATE_MAX_PAGES) return 1;
    /* No overwriting a slot in flight: a retransmitted BEGIN for an
     * environment already staged is refused, and the sender abandons rather
     * than clobbering. */
    if (env_migrate_slot(partition_id, index)) return 1;

    struct EnvMigrateSlot* s = 0;
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++)
        if (!env_migrate_staging[i].active) { s = &env_migrate_staging[i]; break; }
    if (!s) return 1;

    s->active       = 1;
    s->complete     = 0;
    s->partition_id = partition_id;
    s->index        = index;
    s->transfer_id  = transfer_id;
    s->total_pages  = total_pages;
    s->pages_staged = 0;
    s->record       = *record;
    /* A zero-page environment is complete the moment its BEGIN is accepted:
     * its whole payload IS the record that just crossed. Waiting for a page
     * that will never be sent would stage a slot no restore could ever read
     * -- complete only ever gets set in recv_page below. The sender already
     * treats the confirmed BEGIN as the whole transfer for this case (see
     * the BEGIN note in env_migrate_send); this is the receiving half of
     * the same sentence. */
    s->complete     = (total_pages == 0) ? 1 : 0;
    for (uint32_t p = 0; p < ENV_MIGRATE_MAX_PAGES; p++) {
        s->pages[p] = 0;
        s->page_done[p] = 0;
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) s->frags[p][f] = 0;
    }
    return 0;
}

/* Wire entry: one fragment of one payload page. Returns 0 once the fragment
 * is staged; a nonzero return is a refusal (unknown or wrong transfer, page
 * beyond the record's payload) that dspp.c turns into a PAGE_ACK with
 * status!=0. */
int env_migrate_recv_page(uint64_t transfer_id, uint32_t page_index,
                          uint32_t frag_index, const uint8_t* data) {
    /* The table holds at most the in-flight transfer(s); a fragment that
     * names none of them is late or foreign and is refused, never absorbed
     * into some other environment's slot. */
    struct EnvMigrateSlot* s = 0;
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++) {
        struct EnvMigrateSlot* c = &env_migrate_staging[i];
        if (c->active && c->transfer_id == transfer_id) { s = c; break; }
    }
    if (!s || page_index >= s->total_pages ||
        frag_index >= DSPP_MIGRATE_FRAGS_PER_PAGE) return 1;

    if (!s->pages[page_index]) {
        void* frame = allocate_physical_ram_frame_for_partition(s->partition_id);
        if (!frame) return 1;
        s->pages[page_index] = (uint8_t*)frame;
    }
    uint8_t* dst = s->pages[page_index] + frag_index * DSPP_MIGRATE_FRAG_BYTES;
    for (uint32_t b = 0; b < DSPP_MIGRATE_FRAG_BYTES; b++) dst[b] = data[b];
    s->frags[page_index][frag_index] = 1;

    for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++)
        if (!s->frags[page_index][f]) return 0;   /* fragment stored; page not yet whole */

    if (!s->page_done[page_index]) {              /* first time all four landed */
        s->page_done[page_index] = 1;
        s->pages_staged++;
        if (s->pages_staged == s->total_pages) s->complete = 1;
    }
    return 0;
}

const struct EnvMigrateStaging* env_migrate_recv_find(uint32_t partition_id,
                                                      uint32_t index) {
    struct EnvMigrateSlot* s = env_migrate_slot(partition_id, index);
    if (!s) return 0;
    /* The public view is built from the slot; one static, so a caller may
     * hold it while it inspects (single-threaded kernel paths, like the rest
     * of the migrate machinery). */
    static struct EnvMigrateStaging view;
    view.partition_id = s->partition_id;
    view.index        = s->index;
    view.total_pages  = s->total_pages;
    view.pages_staged = s->pages_staged;
    view.complete     = s->complete;
    view.record       = s->record;
    return &view;
}

const uint8_t* env_migrate_recv_page_ptr(uint32_t partition_id, uint32_t index,
                                         uint32_t page_index) {
    struct EnvMigrateSlot* s = env_migrate_slot(partition_id, index);
    if (!s || page_index >= s->total_pages || !s->pages[page_index]) return 0;
    return s->pages[page_index];
}
