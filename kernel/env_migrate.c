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
#include "env_proto.h"          /* ENV_OK / ENV_ERR_* -- the create round trip's answer */
#include "env_payload.h"        /* env_payload_wait_parked() -- the pour's posture gate */
#include "env_service.h"        /* env_service_create()/destroy() -- the create path */
#include "process.h"             /* struct ProcessDescriptor -- the image-window walk's cr3s */
#include "frame_pool.h"
#include "kernel_io.h"
#include "partition.h"          /* partition_exists()/get_owner_node()/pause() -- the restore's gates */
#include "../drivers/nvme_io.h"
#include "../net/dspp.h"
#include "../net/consensus.h"   /* cluster_local_node_id() -- the destination guard */
#include "failover.h"           /* failover_note_heartbeat()/failover_peer_status() -- an intent frame refreshes its sender's liveness */

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

/* The image-window tail the staged payload carries after the payload
 * pages (region frames + durable extent). The reboot payload has always
 * captured the sidecars' image-window frames — the boot stack and the
 * .data/.bss the resumed context RUNS on; a staged payload with only the
 * region frames pours a source heap under a destination sidecar whose own
 * stack frame slots hold different locals, and the restored environment
 * #PF'd on its first console input (live find, 2026-10-23). Collected on
 * the send side from the FROZEN sidecars and re-derived on the restore
 * side from the FRESH sidecars, in env_payload.h's shared canonical order —
 * so no vaddrs cross the wire, and the page count that crossed is what
 * validates the halves. Ceiling: a POSIX sidecar image is ~305 pages and a
 * ramdisk sidecar's ~26; 1024 is generous headroom under the receive
 * side's 4096-page staging ceiling. */
#define ENV_MIGRATE_IMAGE_MAX 1024u
static uint64_t env_mig_img_vaddr[ENV_MIGRATE_IMAGE_MAX];
static struct ProcessDescriptor* env_mig_img_pd[ENV_MIGRATE_IMAGE_MAX];
static uint32_t env_mig_img_count = 0;

static int env_migrate_status = ENV_MIGRATE_OK;
/* Resolved-migration failover grace — see env_migrate.h's
 * ENV_MIGRATE_RESOLVED_GRACE_ definition for the false "declared DEAD"
 * this tail exists to cover. Set at BOTH points where a migration resolves
 * on this node: env_migrate_recv_drop() (the destination's slot is gone)
 * and env_migrate_confirm_disarm() (the source's confirmation walk
 * finished); read by env_migrate_in_flight(). */
static uint64_t env_migrate_resolved_tick = 0;

/* Migration intent — see env_migrate.h's INTENT_GRACE_ block for the
 * idle-observer false "declared DEAD" this pair exists to cover.
 * env_migrate_intent_tick is THIS node's local gate stamp (set by
 * env_migrate_intent_begin(), read by env_migrate_in_flight()); the peer
 * table is what every OTHER node's announce latched here (set by
 * env_migrate_recv_intent(), read by env_migrate_peer_in_flight()). */
static uint64_t env_migrate_intent_tick = 0;
#define ENV_MIGRATE_INTENT_PEERS_MAX 8
struct EnvMigratePeerIntent {
    uint32_t node_id;
    uint8_t  active;
    uint8_t  _pad[3];
    uint64_t until_tick;   /* exclusive: expired the moment now reaches it */
};
static struct EnvMigratePeerIntent env_migrate_peer_intent[ENV_MIGRATE_INTENT_PEERS_MAX];
int env_migrate_last_status(void) { return env_migrate_status; }

/* The operands behind the most recent REFUSAL (restore side). The send side
 * sets env_migrate_status directly and deliberately does not touch this: its
 * refusals have no operands worth carrying -- "a page never confirmed" names
 * itself -- while a restore refusal is only actionable with its numbers (WHICH
 * region broke the placement rule, WHICH ENV_* status the create answered). */
static struct EnvMigrateRefusal env_migrate_refusal = {0, 0, 0};
const struct EnvMigrateRefusal* env_migrate_last_refusal(void) {
    return &env_migrate_refusal;
}

static int env_migrate_refuse(int code, uint32_t a, uint32_t b) {
    env_migrate_status       = code;
    env_migrate_refusal.code = (uint32_t)code;
    env_migrate_refusal.a    = a;
    env_migrate_refusal.b    = b;
    return code;
}

/* P3 increment 5 (defined below, next to the confirmation walk they
 * belong to): the source's pending-teardown table is armed here, at the
 * end of a fully-confirmed send, and the destination's restored-marker is
 * set at the end of a successful pour. */
static void env_migrate_arm_confirms(uint32_t partition_id, uint32_t dest_node_id);
static void env_migrate_restored_mark(uint32_t partition_id, uint32_t index);

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
 * Page numbering is the record's own for the payload pages: every region's
 * frames in record order, then the durable extent's pages; the image-
 * window tail follows (env_migrate_read_image_page below), its count the
 * difference between the BEGIN's declared total and this function's range.
 * Regions are frame-pool frames — identity-mapped on the real kernel
 * (frame_pool.h's contract), real host pointers under the host test's
 * settable frame fake — so a region page is one memcpy either way. Extent
 * pages come off NVMe, 8 sectors at a time. */
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

/* Image-window page k of the tail: resolved at read time (a retransmission
 * re-resolves) in the owning sidecar's page tables — the walk is the one
 * env_migrate_send() froze above, so the frames cannot have moved. */
static int env_migrate_read_image_page(uint32_t k, uint8_t* buf) {
    uint64_t phys = 0;
    if (k >= env_mig_img_count || !env_mig_img_pd[k] ||
        !env_payload_resolve_vaddr(env_mig_img_pd[k]->cr3,
                                   env_mig_img_vaddr[k], &phys))
        return -1;
    for (uint32_t b = 0; b < 4096; b++) buf[b] = ((const uint8_t*)phys)[b];
    return 0;
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

    /* The image-window tail (env_payload.h's shared list), collected under
     * the caller's freeze so the frames cannot move between this walk and
     * the page reads below. */
    uint32_t img_err = 0;
    env_mig_img_count = env_payload_image_window_list(rec, env_mig_img_vaddr,
                                                      env_mig_img_pd,
                                                      ENV_MIGRATE_IMAGE_MAX,
                                                      &img_err);
    if (img_err == 2) {
        kernel_serial_printf(
            "[ENV_MIGRATE] env (partition %u, index %u): more image-window pages than "
            "the %u-page tail ceiling -- transfer abandoned, source untouched.\n",
            (unsigned)partition_id, (unsigned)index, (unsigned)ENV_MIGRATE_IMAGE_MAX);
        env_migrate_status = ENV_MIGRATE_REFUSE_READ;
        return env_migrate_status;
    }
    if (img_err == 1) {
        kernel_serial_printf(
            "[ENV_MIGRATE] env (partition %u, index %u): SOME but not all named sidecars "
            "are live here -- a half-environment's image cannot be staged, transfer "
            "abandoned, source untouched.\n",
            (unsigned)partition_id, (unsigned)index);
        env_migrate_status = ENV_MIGRATE_REFUSE_READ;
        return env_migrate_status;
    }
    if (env_mig_img_count == 0) {
        /* The degenerate record-only shape: a host test's world, and the
         * same page stream the pre-image migrate always sent. Named rather
         * than silent, so a LIVE transfer that lands here is visible in the
         * transcript as "regions and extent only". */
        kernel_serial_printf(
            "[ENV_MIGRATE] env (partition %u, index %u): no live sidecar to collect an "
            "image-window tail from -- sending region and extent pages only.\n",
            (unsigned)partition_id, (unsigned)index);
    }
    /* Posture gate, the reboot capture's own rule (env_payload.h's
     * register rule): the pour resumes the instant these bytes describe,
     * so every collected sidecar must have been frozen PARKED on its
     * channel wait — a mid-compute instant has stack frames no resume can
     * replay, and pouring them would re-create the very mixed state this
     * tail exists to remove. */
    for (uint32_t k = 0; k < env_mig_img_count; k++) {
        if (!env_payload_proc_parked(env_mig_img_pd[k])) {
            kernel_serial_printf(
                "[ENV_MIGRATE] env (partition %u, index %u): a sidecar was frozen "
                "mid-compute, not parked -- the staged instant cannot be resumed, "
                "transfer abandoned, source untouched.\n",
                (unsigned)partition_id, (unsigned)index);
            env_migrate_status = ENV_MIGRATE_REFUSE_UNQUIESCED;
            return env_migrate_status;
        }
    }
    uint32_t payload_pages = env_migrate_payload_pages(rec);
    uint32_t total_pages = payload_pages + env_mig_img_count;
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
            failover_sweep_keepalive();   /* this wait runs ON the sweep: without
                                           * this, an unconfirmed BEGIN stops this
                                           * node's heartbeat stream and observers
                                           * depose it mid-send (2026-10-23) */
            __asm__ volatile("pause");
        }
        if (begin_ok || dspp_migrate_nacked()) break;
    }        dspp_migrate_disarm();

    if (!begin_ok) {
        kernel_serial_printf(
            "[ENV_MIGRATE] env (partition %u, index %u): node %u never confirmed "
            "the BEGIN%s -- transfer abandoned, source untouched.\n",
            (unsigned)partition_id, (unsigned)index, (unsigned)dest_node_id,
            dspp_migrate_nacked() ? " (refused: no staging slot)" : "");
        env_migrate_status = ENV_MIGRATE_REFUSE_BEGIN;
        return env_migrate_status;
    }

    /* ── Every page, confirmed fragment by fragment ──────────────────────
     * Payload pages come off the record (regions, then extent); the tail
     * pages come off the frozen sidecars' image windows. */
    for (uint32_t p = 0; p < total_pages; p++) {
        int page_rd = (p < payload_pages)
                    ? env_migrate_read_page(rec, p, send_page_buf)
                    : env_migrate_read_image_page(p - payload_pages, send_page_buf);
        if (page_rd != 0) {
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
                failover_sweep_keepalive();   /* same as the BEGIN wait: the page
                                              * ACK wait must not silence the leader */
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

/* ─── The migration's environment step ─────────────────────────────────────
 * partition_migrate()'s Step 3c calls exactly this and nothing else from this
 * module — one reference in the orchestration, one stub in a host test, and
 * the whole freeze/send/thaw discipline in one place where it can be proven
 * as a unit (tests/env_migrate_host_test.c, Phase F). */
int env_migrate_partition(uint32_t partition_id, uint32_t dest_node_id,
                          int operator_paused,
                          uint32_t* out_expected, uint32_t* out_moved) {
    uint32_t expected = 0;
    if (out_expected) *out_expected = 0;
    if (out_moved)    *out_moved    = 0;
    env_migrate_status = ENV_MIGRATE_OK;

    /* The control plane's own partition never migrates — named here rather
     * than left to the freeze's refusal, so the caller is told which of the
     * two reasons said no instead of a generic "could not freeze". The
     * orchestration refuses id 0 long before this; this is the belt for
     * every other caller. */
    if (partition_id == PARTITION_SYSTEM) {
        env_migrate_status = ENV_MIGRATE_REFUSE_SYSTEM;
        return env_migrate_status;
    }

    /* 1. The partition's environments are its records — the same table the
     * destroy path keeps in step (env_ckpt_drop_env) and the capture drops
     * dead partitions from. Counted BEFORE anything is read, so "0 of 0" and
     * "0 of N" stay the two different answers partition_migrate() already
     * distinguishes for streams. */
    for (uint32_t i = 0; i < env_ckpt_count; i++)
        if (env_ckpt_table[i].partition_id == partition_id) expected++;
    if (out_expected) *out_expected = expected;
    if (expected == 0) return ENV_MIGRATE_OK;   /* nothing here to move */

    /* A record this boot has not replayed yet describes an environment that
     * does not exist on this node: its region bases are last boot's placement
     * and nothing is mapped there. Reading them would push somebody else's
     * bytes across the wire under this environment's name, so the whole step
     * is refused instead — before a single frame is read. */
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        if (env_ckpt_table[i].partition_id != partition_id) continue;
        if (!env_ckpt_restore_pending_record(partition_id, env_ckpt_table[i].index))
            continue;
        kernel_serial_printf(
            "[ENV_MIGRATE] partition %u: environment index %u is still awaiting this "
            "boot's replay, so it does not exist here and has no payload to send -- "
            "migration ABORTED before the ownership handoff, nothing transferred.\n",
            (unsigned)partition_id, (unsigned)env_ckpt_table[i].index);
        env_migrate_status = ENV_MIGRATE_REFUSE_UNREPLAYED;
        return env_migrate_status;
    }

    /* 2. Freeze them — the caller holds the partition paused already, so this
     * marks a state that exists rather than creating one, and it marks exactly
     * the records counted above or it refused. `operator_paused` is the
     * caller's answer to the one question the flags cannot answer for
     * themselves: partition_migrate() paused this partition for the move, so
     * unless an OPERATOR's pause predates that, the staged records must carry
     * QUIESCED alone — the migration's own freeze is released on the source
     * and is nobody else's business. Marking it anyway made every destination
     * "restore into paused": the environment LIVE by the records, its
     * sidecars never scheduled again, its console accepting input nobody
     * would read (P3 increment 10's live console arm, 2026-10-14). */

    /* 2a. The freeze must LAND on a parked posture: a bounded
     * resume/wait/repause for the MIGRATE's own pause — the pause dance
     * the RESTORE already performs (resume for create, repause before the
     * pour), mirrored here for the SEND. Sidecars wake on a finite
     * deadline (the ramdisk's 200 ms discovery poll), and a partition
     * paused mid-wake never runs them back into their park — the posture
     * gate inside env_migrate_send() would then refuse EVERY attempt for a
     * posture that only needed a moment to settle, and pouring a
     * mid-compute instant would resume it over a parked destination stack
     * (the mixed-state defect this phase exists to prevent). Three
     * exclusions, each the honest one: a partition an OPERATOR paused
     * cannot run its sidecars into a park at all (waiting would burn the
     * bound — its posture is the operator's own doing, and the send's gate
     * answers for it); a partition not held paused is this step's own
     * UNFROZEN refusal below, unchanged; and the wait runs only while the
     * partition runs, so it sits between a resume and the retake — the
     * pause is back in place before this returns, and the caller's own
     * pause bookkeeping (and the quiesce/release interval clause F pins)
     * sees exactly the state it handed in. Bounded and yielding inside
     * env_payload_wait_parked (which pumps the sweep itself), and
     * BEST-EFFORT, same as persist's pre-capture wait: a sidecar that
     * never parks reaches the send's NAMED refusal instead of crossing as
     * a silent mixed state. */
    if (partition_is_paused(partition_id) && !operator_paused) {
        (void)partition_resume(partition_id);
        for (uint32_t i = 0; i < env_ckpt_count; i++) {
            const struct EnvCkptRecord* rec = &env_ckpt_table[i];
            if (rec->partition_id != partition_id) continue;
            (void)env_payload_wait_parked(rec);
        }
        (void)partition_pause(partition_id);
    }

    struct EnvCkptPartitionQuiesce freeze;
    if (env_ckpt_quiesce_partition(partition_id, &freeze, operator_paused) != expected) {
        kernel_serial_printf(
            "[ENV_MIGRATE] partition %u: could not freeze its %u environment(s) -- "
            "this node does not hold the partition paused, so nothing may be read as a "
            "consistent instant. Migration ABORTED before the ownership handoff, nothing "
            "transferred.\n",
            (unsigned)partition_id, (unsigned)expected);
        env_migrate_status = ENV_MIGRATE_REFUSE_UNFROZEN;
        return env_migrate_status;
    }

    /* 3. Send them. A refusal anywhere stops the walk — later environments are
     * not attempted, because the caller is going to abort and each further
     * transfer would be staging bytes for a migration that will not happen. */
    uint32_t moved = 0;
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        if (env_ckpt_table[i].partition_id != partition_id) continue;
        int rc = env_migrate_send(partition_id, env_ckpt_table[i].index, dest_node_id);
        if (rc != ENV_MIGRATE_OK) {
            /* 4. Thaw first: the abort below must not leave flags describing an
             * instant that has already ended. The source is untouched either
             * way — env_migrate_send never kills, frees or drops — so the
             * environment is exactly where it was. */
            env_ckpt_release_partition(&freeze);
            if (out_moved) *out_moved = moved;
            kernel_serial_printf(
                "[ENV_MIGRATE] partition %u: %u of %u environment(s) confirmed by node %u "
                "before index %u was refused (%s) -- ABORTING; ownership must not move "
                "while an environment's payload is still here.\n",
                (unsigned)partition_id, (unsigned)moved, (unsigned)expected,
                (unsigned)dest_node_id, (unsigned)env_ckpt_table[i].index,
                env_migrate_status_name(rc));
            return rc;
        }
        moved++;
    }

    env_ckpt_release_partition(&freeze);
    if (out_moved) *out_moved = moved;
    kernel_serial_printf(
        "[ENV_MIGRATE] partition %u: all %u environment(s) confirmed by node %u; "
        "the caller may hand over ownership.\n",
        (unsigned)partition_id, (unsigned)moved, (unsigned)dest_node_id);

    /* 5 (P3 increment 5): arm the teardown confirmations — one entry per
     * environment just sent. From here the sweep asks node %u whether each
     * environment is live THERE, and tears this node's half down only on
     * its word — the step before this one moved bytes, this one arranges
     * that the records describing them do not outlive the environment.
     * The entries are armed BEFORE the caller's ownership handoff and
     * disarm themselves if ownership never leaves (the caller aborted at
     * Step 4): the sweep tears down only a partition another node now owns.
     * Arming is deliberately AFTER every send succeeded: a partial step is
     * an abort, and an abort's environments are still here. */
    env_migrate_arm_confirms(partition_id, dest_node_id);
    return ENV_MIGRATE_OK;
}

/* ─── Receiver ────────────────────────────────────────────────────────────── */

/* A small bounded table, like the stream receive slots': the sender is
 * synchronous, so one transfer is in flight at a time, and the partition-
 * migrate critical section (increment 3) will serialize concurrent moves the
 * way it already does for streams. Runtime state only — see env_migrate.h. */
#define ENV_MIGRATE_STAGING_MAX 2
#define ENV_MIGRATE_MAX_PAGES   4096   /* >= any real payload, measured on a
                                        * live cluster 2026-10-13: a shell-created
                                        * environment's payload is the 3 region
                                        * frames (POSIX_HEAP 1024 + RD_HEAP 64 +
                                        * RD_STORAGE 256 = 1344) PLUS the two
                                        * sidecars' image/stack frames the create
                                        * charges to the partition PLUS the durable
                                        * extent's pages — which put the old 1536
                                        * cap just under a real BEGIN, refused as
                                        * "too-many" (rc=4) on the wire. 4096 = 16
                                        * MiB of headroom; the staging slot costs
                                        * ~52 KB of BSS each (2 slots). */

struct EnvMigrateSlot {
    uint8_t  active;
    uint8_t  complete;
    uint8_t  restore_tries;    /* P3 increment 4: restore attempts the sweep has spent
                                * on this slot -- bounded so a slot that can never
                                * restore stops logging, while the bytes stay countable */
    uint32_t partition_id;
    uint32_t index;
    uint64_t transfer_id;
    uint32_t total_pages;
    uint64_t opened_tick;      /* kernel_tick_counter at BEGIN — env_migrate_in_flight()
                                * bounds its grace by the slot's age so a wedged
                                * transfer cannot park failover's death detection */
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
    env_migrate_resolved_tick = kernel_tick_counter;   /* resolution point */
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
    s->restore_tries = 0;
    return 0;
}

/* Wire entry: the destination has received an ENV BEGIN. Called from
 * net/dspp.c's dispatch (the timer ISR on real hardware). Returns 0 when the
 * slot is staged and ready, nonzero — refused — otherwise; dspp.c turns the
 * value into the BEGIN_ACK's status. */
int env_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                           uint32_t index, uint32_t total_pages,
                           const struct EnvCkptRecord* record, uint32_t record_len) {
    /* Distinct refusal codes (P3 live arm): every gate below used to return
     * the same 1, and dspp.c's one refusal line then said only "rc=1" —
     * five different defects, indistinguishable on a live cluster where the
     * sender's own log can only say "never confirmed". Any nonzero is still
     * refused exactly as before (dspp.c maps it to a non-zero ACK status;
     * the host tests assert the status, not these numbers). */
    if (!record) return 1;
    if (record_len != (uint32_t)sizeof(struct EnvCkptRecord)) return 2;
    /* Both sides number the PAYLOAD pages from the same record — the one
     * that crossed in the BEGIN. The declared total may additionally carry
     * the image-window tail (env_payload.h's shared order; its count is the
     * difference and is validated against THIS node's fresh sidecar windows
     * at restore), but never fewer payload pages than the record names and
     * never more than the tail ceiling — otherwise the two kernels would
     * disagree about what "every page landed" means, and a bogus tail could
     * stage frames nobody will ever fill. */
    uint32_t payload_pages = env_migrate_payload_pages(record);
    if (total_pages < payload_pages ||
        total_pages > payload_pages + ENV_MIGRATE_IMAGE_MAX) return 3;
    if (total_pages > ENV_MIGRATE_MAX_PAGES) return 4;
    /* No overwriting a slot in flight — but a retransmitted BEGIN for the
     * SAME transfer is answered READY without touching the slot, not
     * refused. transfer_id is derived from the identity ((partition<<32)|
     * index), so a re-BEGIN after a lost or late ACK carries the same id
     * and the same record: refusing it would make the documented
     * retransmission model fail exactly when it is needed. Observed live
     * 2026-10-13: a freshly-booted destination's BEGIN ACK was slow, the
     * source re-asked, rc=5 abandoned the whole migrate mid-send ("a page
     * never confirmed") on a cluster where every node was healthy. Pages
     * re-stage idempotently under the same id. A BEGIN for the same
     * identity with a DIFFERENT record is a different transfer's intent
     * and stays refused (env_migrate_host_test E4). */
    struct EnvMigrateSlot* existing = env_migrate_slot(partition_id, index);
    if (existing) {
        if (existing->transfer_id == transfer_id &&
            existing->record.env_id == record->env_id)
            return 0;   /* the same transfer re-asked: staged, ready */
        return 5;
    }

    struct EnvMigrateSlot* s = 0;
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++)
        if (!env_migrate_staging[i].active) { s = &env_migrate_staging[i]; break; }
    if (!s) return 6;

    s->active       = 1;
    s->complete     = 0;
    s->restore_tries = 0;
    s->partition_id = partition_id;
    s->index        = index;
    s->transfer_id  = transfer_id;
    s->total_pages  = total_pages;
    s->opened_tick  = kernel_tick_counter;
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
        /* Staging frames come from the TOP of the frame pool (frame_pool.h).
         * The restore creates a FRESH environment whose regions land
         * wherever the ascending first-fit watermark currently is; staging
         * this payload ASCENDED would consume the very runs that create is
         * about to be handed, and the destination would land a
         * payload-length past the staged bases — the placement gate's
         * "fresh environment landed off the staged bases" refusal, caught
         * live on the 2-node TCG cluster (2026-10-13) and invisible to host
         * tests, whose two simulated nodes share one allocator history.
         * Top-down staging leaves the watermark exactly where the source's
         * was, so the destination reproduces the recorded bases. Same
         * quota/owner/usage accounting; the drop frees them like any frame. */
        void* frame = allocate_physical_ram_frame_for_partition_top(s->partition_id);
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

/* ─── Restore (P3 increment 4): a staged slot becomes a live environment ─────
 * See env_migrate.h for the full contract; this file owns the order, which is
 * the part that cannot be delegated. The shape is env_service.c's boot
 * replay — resume → create → identity → settle → wait-parked → repause →
 * pour — with the bytes sourced from the staging slot instead of NVMe, and
 * one extra gate the boot pass never needs: the destination must OWN the
 * partition before it may build an environment inside it.
 *
 * The slot-retention rule, stated once because every refusal below follows
 * it: the slot is KEPT while the state is exactly what it was before the
 * attempt (nothing stored, nothing created — a retry can succeed where this
 * attempt could not), and DROPPED the moment the environment exists (a pour
 * refusal leaves the empty replay standing, and retrying a pour into a live
 * environment would double-create). env_migrate_recv_drop() is therefore the
 * last thing a successful restore does, and the last thing an unappliable
 * refusal does — never the first. */
int env_migrate_restore(uint32_t partition_id, uint32_t index) {
    env_migrate_status = ENV_MIGRATE_OK;
    env_migrate_refusal.code = ENV_MIGRATE_OK;
    env_migrate_refusal.a = env_migrate_refusal.b = 0;

    /* 1a. The slot, whole. The public view is a shared static, so the record
     * is copied immediately: the drops below free the slot the view reads. */
    const struct EnvMigrateStaging* view = env_migrate_recv_find(partition_id, index);
    if (!view)
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_NO_SLOT, partition_id, index);
    if (!view->complete)
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_INCOMPLETE,
                                  view->pages_staged, view->total_pages);
    struct EnvCkptRecord staged = view->record;
    /* The SLOT's declared total, NOT the record's payload. The record names
     * the payload pages only (regions + durable extent); the BEGIN's total
     * additionally carries the image-window tail, and the difference between
     * the two IS the tail's page count. Deriving this number from the record
     * made that difference identically ZERO on every restore: the live 3-node
     * suite (2026-10-24, guards_all.log) printed "1344 region page(s), 0
     * image-window page(s) and 256 extent page(s) poured" while the slot
     * carried the tail the source had just sent -- staged, never validated
     * against the fresh sidecar windows (4b'), never poured, and the count
     * agreement that guards both halves never tested. recv_begin already
     * bound the declared total to [payload, payload + ENV_MIGRATE_IMAGE_MAX]
     * (see the gate there), so the subtraction below cannot underflow. */
    uint32_t staged_total_pages = view->total_pages;
    if (staged.partition_id != partition_id || staged.index != index)
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_RECORD,
                                  staged.partition_id, staged.index);

    /* The control plane never migrates (the send side refuses it by name), so
     * a PARTITION_SYSTEM slot is malformed input rather than a tenant to
     * rebuild — refused here rather than discovered mid-create. */
    if (partition_id == PARTITION_SYSTEM)
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_SYSTEM, partition_id, index);

    /* 1b. The boot replay's own admissibility gate, applied verbatim: what
     * crossed must be a quiesced instant and name a partition that exists.
     * One gate for both restores, so a record the boot would refuse cannot be
     * smuggled in through the wire. */
    int admissible = env_ckpt_restore_admissible(&staged);
    if (admissible == ENV_CKPT_REFUSE_UNQUIESCED)
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_UNQUIESCED, index, admissible);
    if (admissible != ENV_CKPT_REFUSE_NONE) {
        if (!partition_exists(partition_id))
            return env_migrate_refuse(ENV_MIGRATE_REFUSE_NOT_OWNER, 1, index);
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_RECORD,
                                  (uint32_t)admissible, index);
    }

    /* 1c. The destination guard. A follower building an environment for a
     * partition served elsewhere would be a second, unaccounted tenant
     * charging its frames — the mirror image of the source's
     * "ownership must not move before the payload" rule. */
    if (partition_get_owner_node(partition_id) != cluster_local_node_id())
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_NOT_OWNER, 0, index);

    /* 2. A return migration: this node already holds an environment at this
     * identity because the source-teardown half has not landed (increment 3
     * named it — the source's records still describe environments that
     * moved). The staged bytes just crossed with ownership returned, so the
     * local instance is the stale one; it is ended, and what crossed is
     * rebuilt. env_service_destroy's own bookkeeping drops its record (the
     * ENV_DESTROY path calls env_ckpt_drop_env), so the store below upserts
     * onto a clean identity. A destroy that answers NOENT — the environment
     * died long ago and only its record outlived it — is exactly as good. */
    const struct EnvCkptRecord* live = env_ckpt_find(partition_id, index);
    if (live && !env_ckpt_restore_pending_record(partition_id, index)) {
        uint16_t d_status = 0; uint32_t d_id = 0, d_part = 0;
        (void)env_service_destroy(live->env_id, partition_id, &d_status, &d_id, &d_part);
        kernel_serial_printf(
            "[ENV_MIGRATE] restore (partition %u, index %u): an environment of this "
            "identity already lived here (env %u) -- the source-teardown half has not "
            "landed, so the stale instance is ended and the staged one rebuilt.\n",
            (unsigned)partition_id, (unsigned)index, (unsigned)live->env_id);
    }

    /* 3a. The staged descriptor enters the live table the same way the boot
     * replay's adopted records did — validated all-or-nothing by the record
     * layer — so the create below refreshes it rather than inventing it, and
     * the periodic checkpoint flush persists it as this node's own record. */
    int rec_rc = env_ckpt_record(&staged);
    if (rec_rc != 0)
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_RECORD, (uint32_t)rec_rc, index);

    /* 3b. The pause dance, exactly the boot pass's: E4's placement gate
     * refuses a paused target, so a partition the record recorded as paused
     * is resumed for the length of the create and put back afterwards by the
     * ledger. From the first resume to the repause the body is straight-line
     * with NO `return` — a bail between them would leave a partition the
     * record said was frozen RUNNING. */
    int resumed = env_ckpt_restore_resume_for_create(partition_id);
    if (resumed < 0) {
        env_ckpt_drop(partition_id, index);   /* nothing half-applied */
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_NOT_OWNER, 1, index);
    }

    uint16_t status = ENV_ERR_INVAL;
    uint32_t new_id = 0;
    int rc = env_service_create(partition_id, index, &status, &new_id);
    if (rc != 0 || status != ENV_OK) {
        env_ckpt_restore_refuse(partition_id, index, ENV_CKPT_REFUSE_CREATE,
                                index, (rc != 0) ? 0u : (uint32_t)status);
        env_ckpt_drop(partition_id, index);
        (void)env_ckpt_restore_repause();
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_CREATE, index,
                                  (rc != 0) ? 0u : (uint32_t)status);
    }

    if (!env_ckpt_restore_same_identity(&staged, env_ckpt_find(partition_id, index))) {
        /* Refusal over partial application, applied to the replay: the create
         * made SOMETHING and it is not what crossed, so it is ended rather
         * than left running as a half-restore nobody could account for. The
         * state is exactly pre-attempt afterwards (the destroy drops the
         * record), so the slot stays and the sweep may retry. */
        env_ckpt_restore_refuse(partition_id, index, ENV_CKPT_REFUSE_IDENTITY,
                                index, new_id);
        uint16_t d_status = 0; uint32_t d_id = 0, d_part = 0;
        (void)env_service_destroy(new_id, partition_id, &d_status, &d_id, &d_part);
        (void)env_ckpt_restore_repause();
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_IDENTITY, index, new_id);
    }

    /* The record is live here now — this boot owes it no replay (it was never
     * armed; settle keeps the pending invariant explicit rather than relying
     * on a never-armed record reading as settled). */
    env_ckpt_restore_settle(partition_id, index);

    /* 4a. Parked, or the pour may not touch a byte: the sidecars must have
     * run to their idle point — the only posture whose memory the staged
     * bytes describe. A yield is legal HERE, inside the resume/repause
     * interval, exactly as in the boot pass. */
    int parked = env_payload_wait_parked(&staged);

    /* The ledger goes back before the pour, the boot pass's order: from the
     * repause on, NOTHING yields. */
    (void)env_ckpt_restore_repause();

    if (!parked) {
        /* The environment stands as the empty replay it already is — the boot
         * pass's own answer to a pour that cannot run. The staged bytes are
         * dropped with the slot: holding partition-charged frames forever for
         * a posture that will not come back is the worse failure, and the
         * refusal above names why the contents did not come back. */
        env_migrate_recv_drop(partition_id, index);
        return env_migrate_refuse(ENV_MIGRATE_REFUSE_PARKED, index, 0);
    }

    /* 4b. The pour, validated WHOLE before a byte: every staged region must
     * land at the base the staged record names. The sidecars' MEM regions are
     * identity-mapped and their heaps hold RAW POINTERS into them — exactly
     * env_payload.h's placement rule — so a fresh environment that landed
     * anywhere else cannot receive these bytes without every interior
     * pointer dangling, silently. The durable extent is different in kind:
     * its coordinates are NODE-LOCAL (each node's band slot comes from its
     * own first-free history, persisted per node), so the record's LBA is a
     * fact about the SOURCE -- measured live: staged slot 5, fresh slot 7,
     * both 2048 sectors. What the pour needs is an extent THIS node owns for
     * this identity, at least as large as the staged one, and the pour below
     * writes THERE -- so a source coordinate can never aim this node's bytes
     * at another tenant's extent. */
    const struct EnvCkptRecord* fresh = env_ckpt_find(partition_id, index);
    uint32_t refuse_code = 0, refuse_a = 0;
    for (uint32_t r = 0; r < staged.n_regions && r < ENV_CKPT_MAX_REGIONS; r++) {
        const struct EnvCkptRegion* fr = 0;
        for (uint32_t k = 0; fresh && k < fresh->n_regions && k < ENV_CKPT_MAX_REGIONS; k++)
            if (fresh->regions[k].kind == staged.regions[r].kind) { fr = &fresh->regions[k]; break; }
        if (!fr || fr->frames != staged.regions[r].frames || fr->base != staged.regions[r].base) {
            refuse_code = ENV_MIGRATE_REFUSE_PLACEMENT;
            refuse_a    = staged.regions[r].kind;
            /* Name both halves: the status says WHICH rule fired but not
             * the delta, and on a live cluster the delta is the whole
             * diagnosis (a payload-length shift is the staging watermark;
             * anything else is a different divergence between the nodes'
             * allocation histories). */
            kernel_serial_printf(
                "[ENV_MIGRATE] restore (partition %u, index %u): region kind %u: "
                "staged base 0x%llx frames %u, fresh base 0x%llx frames %u%s\n",
                (unsigned)partition_id, (unsigned)index,
                (unsigned)staged.regions[r].kind,
                (unsigned long long)staged.regions[r].base,
                (unsigned)staged.regions[r].frames,
                (unsigned long long)(fr ? fr->base : 0ull),
                (unsigned)(fr ? fr->frames : 0u),
                fr ? "" : " (no fresh region of this kind)");
            break;
        }
    }
    if (!refuse_code && staged.state_sectors != 0 &&
        (!fresh || fresh->state_lba == 0 ||
         fresh->state_sectors < staged.state_sectors)) {
        refuse_code = ENV_MIGRATE_REFUSE_EXTENT;
        refuse_a    = (uint32_t)staged.state_sectors;
        /* Name both halves, the placement refusal's own rule: the status
         * says WHICH gate fired but not the delta, and on a live cluster the
         * delta is the whole diagnosis -- "fresh present lba 0 sectors 0"
         * reads as a destination with no durable extent this boot, while a
         * present-but-undersized fresh names the size that fell short. */
        kernel_serial_printf(
            "[ENV_MIGRATE] restore (partition %u, index %u): EXTENT halves: "
            "staged lba %llu sectors %u -- fresh %s lba %llu sectors %u\n",
            (unsigned)partition_id, (unsigned)index,
            (unsigned long long)staged.state_lba,
            (unsigned)staged.state_sectors,
            fresh ? "present" : "MISSING",
            (unsigned long long)(fresh ? fresh->state_lba : 0ull),
            (unsigned)(fresh ? fresh->state_sectors : 0u));
    }
    if (!refuse_code) {
        /* Defensive: a complete slot has a frame behind every page (recv_page
         * counts a page whole only once its frame holds all four fragments),
         * so this is unreachable — checked anyway because the alternative to
         * a named refusal mid-pour is a half-poured environment. */
        for (uint32_t p = 0; p < staged_total_pages; p++) {
            if (!env_migrate_recv_page_ptr(partition_id, index, p)) {
                refuse_code = ENV_MIGRATE_REFUSE_INCOMPLETE;
                refuse_a    = p;
                break;
            }
        }
    }

    /* 4b'. The image-window tail the sender appended after the payload
     * pages (env_payload.h's shared order). Validated WHOLE before the pour
     * touches a byte, like the placement rule it belongs to: the FRESH
     * sidecars' windows must hold exactly the page count that crossed, and
     * every one of their vaddr positions must resolve to a live frame here.
     * Both sides derive the same order from their own live processes, so
     * the count disagreement names both halves — what the source's windows
     * held vs. what this node's fresh sidecars hold. */
    uint32_t payload_pages = env_migrate_payload_pages(&staged);
    uint32_t img_pages = staged_total_pages > payload_pages
                       ? staged_total_pages - payload_pages : 0u;
    static uint64_t env_mig_img_dst[ENV_MIGRATE_IMAGE_MAX];
    if (!refuse_code && img_pages > ENV_MIGRATE_IMAGE_MAX) {
        kernel_serial_printf(
            "[ENV_MIGRATE] restore (partition %u, index %u): %u image-window page(s) "
            "crossed, past the %u-page tail ceiling -- the staged instant cannot be "
            "validated here.\n",
            (unsigned)partition_id, (unsigned)index,
            (unsigned)img_pages, (unsigned)ENV_MIGRATE_IMAGE_MAX);
        refuse_code = ENV_MIGRATE_REFUSE_PLACEMENT;
        refuse_a    = img_pages;
    }
    if (!refuse_code && img_pages > 0) {
        /* Reuses the module's list buffers: on this node they belong to no
         * in-flight send (the sender and this restore never interleave —
         * the keepalive pumps heartbeat and checkpoint only, and the sweep
         * drives this pass serially). */
        uint32_t img_err = 0;
        uint32_t n_img = env_payload_image_window_list(fresh, env_mig_img_vaddr,
                                                       env_mig_img_pd,
                                                       ENV_MIGRATE_IMAGE_MAX,
                                                       &img_err);
        if (img_err || n_img != img_pages) {
            kernel_serial_printf(
                "[ENV_MIGRATE] restore (partition %u, index %u): image-window halves: "
                "the wire carried %u page(s), this node's fresh sidecars' windows hold "
                "%u%s -- the staged instant cannot be rebuilt here.\n",
                (unsigned)partition_id, (unsigned)index,
                (unsigned)img_pages, (unsigned)n_img,
                img_err == 1 ? " (SOME but not all named sidecars are live)"
              : img_err == 2 ? " (more pages than the tail ceiling)" : "");
            refuse_code = ENV_MIGRATE_REFUSE_PLACEMENT;
            refuse_a    = img_pages;
        }
        for (uint32_t k = 0; k < n_img && !refuse_code; k++) {
            if (!env_mig_img_pd[k] ||
                !env_payload_resolve_vaddr(env_mig_img_pd[k]->cr3,
                                           env_mig_img_vaddr[k],
                                           &env_mig_img_dst[k])) {
                kernel_serial_printf(
                    "[ENV_MIGRATE] restore (partition %u, index %u): image-window "
                    "page %u of %u (vaddr 0x%llx) has no live frame at its vaddr "
                    "here -- the staged instant cannot be rebuilt.\n",
                    (unsigned)partition_id, (unsigned)index,
                    (unsigned)k, (unsigned)n_img,
                    (unsigned long long)env_mig_img_vaddr[k]);
                refuse_code = ENV_MIGRATE_REFUSE_PLACEMENT;
                refuse_a    = k;
            }
        }
    }
    if (refuse_code) {
        env_migrate_recv_drop(partition_id, index);
        kernel_serial_printf(
            "[ENV_MIGRATE] restore (partition %u, index %u): %s (a=%u) -- the "
            "environment stands as the empty replay; the staged bytes can never be "
            "applied here and the slot is dropped.\n",
            (unsigned)partition_id, (unsigned)index,
            env_migrate_status_name((int)refuse_code), (unsigned)refuse_a);
        return env_migrate_refuse((int)refuse_code, refuse_a, index);
    }

    /* 4c. The partition goes back to the state the record recorded: a
     * successful migration always leaves its partition PAUSED, because resume
     * happens over here — by an operator, after the bytes have landed. The
     * record carried ENV_CKPT_FLAG_PARTITION_PAUSED across the wire for
     * exactly this (env_ckpt.h's partition-quiesce contract). */
    if (staged.flags & ENV_CKPT_FLAG_PARTITION_PAUSED) {
        (void)partition_pause(partition_id);
        kernel_serial_printf(
            "[ENV_MIGRATE] restore (partition %u, index %u): the record recorded this "
            "partition as paused -- left paused; resume is an operator's call, after "
            "the bytes have landed.\n",
            (unsigned)partition_id, (unsigned)index);
    }

    /* 5. The pour. Straight-line, NO yield from here to the drop (env_payload.h's
     * rule: Ring-3 runs only while the control plane yields, so nothing can
     * execute over the memory being poured). Page numbering is the sender's
     * own: every region's frames in record order, then the durable extent's
     * pages, then the image-window tail — both sides derive it from the SAME
     * record that crossed plus their own live sidecars' windows. */
    uint32_t cursor = 0;
    for (uint32_t r = 0; r < staged.n_regions && r < ENV_CKPT_MAX_REGIONS; r++) {
        const struct EnvCkptRegion* fr = 0;
        for (uint32_t k = 0; k < fresh->n_regions && k < ENV_CKPT_MAX_REGIONS; k++)
            if (fresh->regions[k].kind == staged.regions[r].kind) { fr = &fresh->regions[k]; break; }
        for (uint32_t f = 0; f < staged.regions[r].frames; f++) {
            const uint8_t* src = env_migrate_recv_page_ptr(partition_id, index, cursor++);
            uint8_t* dst = (uint8_t*)(fr->base + (uint64_t)f * 4096u);
            for (uint32_t b = 0; b < 4096; b++) dst[b] = src[b];
            failover_sweep_keepalive();   /* the pour is straight-line but it IS
                                          * the sweep — transmit-only here, no yield,
                                          * so the no-yield rule stands */
        }
    }
    uint32_t region_pages = cursor;

    uint32_t extent_pages = (staged.state_sectors + ENV_MIGRATE_SECTORS_PER_PAGE - 1)
                            / ENV_MIGRATE_SECTORS_PER_PAGE;
    for (uint32_t e = 0; e < extent_pages; e++) {
        const uint8_t* src = env_migrate_recv_page_ptr(partition_id, index, cursor++);
        failover_sweep_keepalive();   /* sync NVMe writes are the pour's long pole:
                                      * keep beating across them too */
        if (nvme_write_sync(fresh->state_lba +
                            (uint64_t)e * ENV_MIGRATE_SECTORS_PER_PAGE, src) != 0) {
            /* The environment exists and its memory holds the contents; the
             * durable half could not be written down. Named, and the slot is
             * dropped (not retried): a retry would re-run the CREATE — which
             * now collides with the environment that succeeded — for an IO
             * whose failure mode nothing here can change. */
            kernel_serial_printf(
                "[ENV_MIGRATE] restore (partition %u, index %u): NVMe write of extent "
                "page %u of %u FAILED -- the environment is live with its contents in "
                "memory, but its durable extent did not come back; the slot is dropped.\n",
                (unsigned)partition_id, (unsigned)index,
                (unsigned)e, (unsigned)extent_pages);
            env_migrate_recv_drop(partition_id, index);
            return env_migrate_refuse(ENV_MIGRATE_REFUSE_EXTENT, 1, index);
        }
    }

    /* The image-window tail: the pages validated whole at 4b' — the fresh
     * sidecars' OWN boot-stack/.data/.bss frames at their resolved
     * positions. This is what makes the poured heap describable again: the
     * resumed context's stack locals, the static allocator state and the
     * `booted` frame all become the instant the regions describe, instead
     * of a destination-side boot's (the live #PF this tail fixes).
     *
     * It is read LAST, after the extent, because that is the sender's own
     * numbering: env_migrate_read_page() serves the whole payload (every
     * region's frames, then the durable extent's pages) and only then does
     * env_migrate_read_image_page() serve the tail. This loop used to sit
     * BETWEEN the regions and the extent, so the moment the tail's count
     * became nonzero (the staged_total_pages fix above) the extent's staged
     * bytes would have been poured into the sidecars' image frames and the
     * tail's bytes written to NVMe — two whole pages of each other. */
    for (uint32_t k = 0; k < img_pages; k++) {
        const uint8_t* src = env_migrate_recv_page_ptr(partition_id, index, cursor++);
        uint8_t* dst = (uint8_t*)env_mig_img_dst[k];
        for (uint32_t b = 0; b < 4096; b++) dst[b] = src[b];
        failover_sweep_keepalive();   /* same rule as the region pour */
    }

    env_migrate_recv_drop(partition_id, index);
    /* P3 increment 5: mark the restored identity BEFORE it can be asked
     * about — from here this node may say "live with its contents" on the
     * wire. Set only on this path: every refusal above left the record
     * standing as an empty replay, and an empty replay must never confirm
     * a source into destroying its complete copy. */
    env_migrate_restored_mark(partition_id, index);
    kernel_serial_printf(
        "[ENV_MIGRATE] restore: env (partition %u, index %u) is LIVE on node %u as "
        "env %u -- %u region page(s), %u image-window page(s) and %u extent page(s) "
        "poured from the staged slot, which is dropped.\n",
        (unsigned)partition_id, (unsigned)index,
        (unsigned)cluster_local_node_id(), (unsigned)new_id,
        (unsigned)region_pages, (unsigned)img_pages, (unsigned)extent_pages);
    return ENV_MIGRATE_OK;
}

/* ─── P3 increment 5: the confirmation machinery ─────────────────────────────
 * Two runtime tables and the walk between them, closing the boundary
 * increments 3 and 4 named: a successful migration used to leave the
 * environment on BOTH nodes' books while its bytes lived on one. Now the
 * source's records, sidecars and frames survive exactly until the
 * destination has said — in its own words, on its own state — that the
 * environment is live with its contents there.
 *
 * Neither table is persisted: both are lifecycle state of this boot's
 * staging table, exactly like the slots themselves, and env_migrate_reset()
 * clears both with it. Both are bounded; the bounds are chosen so that
 * every overflow degrades in the CONSERVATIVE direction — records that
 * describe an environment that moved (countable, nameable, an operator's to
 * finish) — never in the direction of a teardown without the destination's
 * word. */

/* The destination's own knowledge that it restored (p, i) WITH ITS
 * CONTENTS: set by the pour's success tail, nothing else. */
#define ENV_MIGRATE_RESTORED_MAX 16u
struct EnvMigrateRestored {
    uint32_t partition_id;
    uint32_t index;
};
static struct EnvMigrateRestored env_migrate_restored_table[ENV_MIGRATE_RESTORED_MAX];
static uint32_t env_migrate_restored_count = 0;
static uint32_t env_migrate_restored_next  = 0;   /* round-robin when full */

static void env_migrate_restored_mark(uint32_t partition_id, uint32_t index) {
    /* Idempotent: a re-restore of the same identity refreshes nothing and
     * evicts nothing. */
    for (uint32_t i = 0; i < env_migrate_restored_count; i++)
        if (env_migrate_restored_table[i].partition_id == partition_id &&
            env_migrate_restored_table[i].index == index) return;
    uint32_t slot = env_migrate_restored_count < ENV_MIGRATE_RESTORED_MAX
                        ? env_migrate_restored_count++
                        : env_migrate_restored_next++ % ENV_MIGRATE_RESTORED_MAX;
    env_migrate_restored_table[slot].partition_id = partition_id;
    env_migrate_restored_table[slot].index        = index;
}

static int env_migrate_restored_here(uint32_t partition_id, uint32_t index) {
    for (uint32_t i = 0; i < env_migrate_restored_count; i++)
        if (env_migrate_restored_table[i].partition_id == partition_id &&
            env_migrate_restored_table[i].index == index) return 1;
    return 0;
}

/* The source's pending teardowns: armed by env_migrate_partition() after
 * every payload confirmed, walked by the sweep below. One entry per
 * environment — a teardown is per-name, and confirming a whole partition
 * because one of its environments landed would be the same over-reach this
 * increment exists to remove. */
#define ENV_MIGRATE_CONFIRM_MAX        16u
#define ENV_MIGRATE_CONFIRM_MAX_TRIES  60u  /* ~10 min of polls at the sweep's
                                             * period, then stop asking */
#define ENV_MIGRATE_TEARDOWN_MAX_TRIES 5u   /* bounded destroys, then stop */
struct EnvMigrateConfirm {
    uint8_t  active;
    uint8_t  confirmed;      /* the destination answered "live with contents" */
    uint8_t  destroy_tries;  /* teardown attempts since the confirmation */
    uint8_t  stop_logged;    /* the bounded-stop final line has been printed */
    uint16_t tries;          /* LIVE polls spent */
    uint32_t partition_id;
    uint32_t index;
    uint32_t dest_node;
};
static struct EnvMigrateConfirm env_migrate_confirm[ENV_MIGRATE_CONFIRM_MAX];

uint32_t env_migrate_pending_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < ENV_MIGRATE_CONFIRM_MAX; i++)
        if (env_migrate_confirm[i].active) n++;
    return n;
}

/* The failover deferral gate — see env_migrate.h for why a
 * migration-produced silence is not a death and how each grace below is
 * bounded (slot age, post-drop tail, the confirmation walk's tries cap), so
 * a migration that genuinely wedges re-arms death detection instead of
 * disabling it. */
int env_migrate_in_flight(void) {
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++) {
        const struct EnvMigrateSlot* s = &env_migrate_staging[i];
        if (!s->active) continue;
        if (kernel_tick_counter - s->opened_tick < ENV_MIGRATE_SLOT_GRACE_TICKS)
            return 1;
    }
    if (env_migrate_resolved_tick != 0 &&
        kernel_tick_counter - env_migrate_resolved_tick <
            ENV_MIGRATE_RESOLVED_GRACE_TICKS)
        return 1;
    /* The intent face: the local gate covers the blocking PHASE itself (the
     * create RPC wait and the synchronous send), which runs before any slot
     * exists on the destination and before any confirmation is armed — the
     * window in which this node's own declarations were previously
     * undeferred. */
    if (env_migrate_intent_tick != 0 &&
        kernel_tick_counter - env_migrate_intent_tick <
            ENV_MIGRATE_INTENT_GRACE_TICKS)
        return 1;
    if (env_migrate_pending_count() > 0) return 1;
    return 0;
}

/* ─── Migration intent (the idle-observer arm) ──────────────────────────
 * The local stamp and the cluster announce, in that order: the announce
 * must be on the wire before the caller blocks, because the block itself
 * stops this node's only liveness traffic (the leader's heartbeat, from
 * this same sweep). A second begin inside the budget refreshes the stamp
 * and re-announces — cheap, and it lets back-to-back operator actions
 * (create, then migrate) each extend every peer's latch. */
void env_migrate_intent_begin(void) {
    env_migrate_intent_tick = kernel_tick_counter;
    dspp_env_migrate_send_intent((uint32_t)ENV_MIGRATE_INTENT_GRACE_TICKS);
}

void env_migrate_intent_end(void) {
    /* The blocking phase resolved (success OR refusal): release this node's
     * own gate and ask every peer to drop its latch NOW rather than at the
     * budget's end. The announce is one-shot and the budget was sized for
     * the whole phase, so an unreleased intent leaves a blind spot exactly
     * as long as the leftover budget — found live: an operator (or a guard)
     * creating an environment and then killing the node could not be
     * declared DEAD for a full 300 s, and the adoption guard's 240 s wait
     * lost to it. The release rides the SAME broadcast (budget 0 = revoke,
     * the only meaning 0 can have on this wire — recv_intent clamps every
     * nonzero budget, so 0 was previously unrepresentable). A RELEASED
     * frame is droppable: the budget still bounds the latch on its own;
     * the release only makes death detection prompt in the common case.
     * Gated on the local stamp so a sweep period with no begin transmits
     * nothing — the restore loop below calls this after EVERY attempt. */
    if (env_migrate_intent_tick == 0) return;
    env_migrate_intent_tick = 0;
    dspp_env_migrate_send_intent(0);
}

void env_migrate_recv_intent(uint32_t node_id, uint32_t budget_ticks) {
    if (node_id == 0 || node_id == cluster_local_node_id()) return;
    /* Every intent frame — announce or release — crossed the wire from a
     * node that is, at this instant, alive enough to transmit, so it is the
     * same liveness evidence a heartbeat is and must refresh last_seen_tick.
     * This is the release-vs-heartbeat race a live 3-node run exposed
     * (2026-10-23, env_migrate_confirm_live): the source announced intent,
     * blocked in its create for 435 ticks, and on the return
     * env_migrate_intent_end() fired the RELEASE — but the release only
     * dropped the peer's latch without touching last_seen_tick, so the very
     * next failover_tick still saw the 435 ticks of silence the create had
     * accumulated, now UNdeferred, and declared the just-resurrected source
     * DEAD, adopting its partitions mid-create. The source's very next
     * frame is what ends its silence; feeding it here closes the window
     * between "the block ended" and "the next real heartbeat lands".
     *
     * Refresh-only, deliberately: an intent frame proves the SENDER is
     * alive but must not PUT it on the death-watch. Watch entries exist
     * for nodes whose heartbeats were heard (leaders); a follower that
     * announces intent and then stays quiet — followers transmit nothing
     * that feeds last_seen_tick — would gain an entry here and be
     * declared DEAD exactly 300 ticks after its own release, the very
     * false death this call prevents (live 3-node find, 2026-10-23: the
     * deposed source declared dead at +304 ticks post-release). */
    if (failover_peer_status(node_id) >= 0)
        failover_note_heartbeat(node_id, kernel_tick_counter);
    if (budget_ticks == 0) {
        /* The announce's own release: the peer's blocking phase is over and
         * its silence is a death again on the normal terms. Deactivate the
         * latch if one stands; a release for a node this table never latched
         * is a no-op, not an error (the frame may have been reordered ahead
         * of the announce it cancels — begin/end pairs are retransmit-free
         * one-shots — and the worst case is the budget's own bound). */
        for (int i = 0; i < ENV_MIGRATE_INTENT_PEERS_MAX; i++) {
            struct EnvMigratePeerIntent* e = &env_migrate_peer_intent[i];
            if (!e->active || e->node_id != node_id) continue;
            e->active = 0;
            kernel_serial_printf(
                "[ENV_MIGRATE] node %u released its environment-migration intent "
                "-- its silence is a death again on the normal terms.\n",
                (unsigned)node_id);
            return;
        }
        return;
    }
    if (budget_ticks > ENV_MIGRATE_INTENT_GRACE_TICKS)
        budget_ticks = (uint32_t)ENV_MIGRATE_INTENT_GRACE_TICKS;
    if (budget_ticks > ENV_MIGRATE_INTENT_GRACE_TICKS)
        budget_ticks = (uint32_t)ENV_MIGRATE_INTENT_GRACE_TICKS;

    struct EnvMigratePeerIntent* e = 0;
    for (int i = 0; i < ENV_MIGRATE_INTENT_PEERS_MAX; i++) {
        if (env_migrate_peer_intent[i].active &&
            env_migrate_peer_intent[i].node_id == node_id) {
            e = &env_migrate_peer_intent[i];
            break;
        }
        if (!e && !env_migrate_peer_intent[i].active)
            e = &env_migrate_peer_intent[i];
    }
    if (!e) {
        /* Table full: the oldest budget expires soonest on its own; a
         * refusal here must stay countable rather than silent. */
        kernel_serial_printf(
            "[ENV_MIGRATE] node %u announced intent but the peer-intent table "
            "is full (%u) -- its announce is NOT latched; its own death "
            "detection is unaffected, only this node's deferral about it.\n",
            (unsigned)node_id, (unsigned)ENV_MIGRATE_INTENT_PEERS_MAX);
        return;
    }
    if (!e->active) {
        e->node_id = node_id;
        e->active  = 1;
        kernel_serial_printf(
            "[ENV_MIGRATE] node %u announced environment-migration intent "
            "(budget %u s) -- its silence is not a death until then.\n",
            (unsigned)node_id, (unsigned)(budget_ticks / 100u));
    }
    e->until_tick = kernel_tick_counter + budget_ticks;
}

int env_migrate_peer_in_flight(uint32_t node_id) {
    for (int i = 0; i < ENV_MIGRATE_INTENT_PEERS_MAX; i++) {
        const struct EnvMigratePeerIntent* e = &env_migrate_peer_intent[i];
        if (!e->active || e->node_id != node_id) continue;
        if (kernel_tick_counter < e->until_tick) return 1;
        return 0;
    }
    return 0;
}

static struct EnvMigrateConfirm*
env_migrate_confirm_find(uint32_t partition_id, uint32_t index) {
    for (uint32_t i = 0; i < ENV_MIGRATE_CONFIRM_MAX; i++) {
        struct EnvMigrateConfirm* c = &env_migrate_confirm[i];
        if (c->active && c->partition_id == partition_id && c->index == index)
            return c;
    }
    return 0;
}

/* Arm one entry per environment of the partition just sent. Called at the
 * very end of a fully-confirmed env_migrate_partition(), before the caller
 * hands ownership over: the entries only ever fire for a partition THIS
 * node no longer owns, so an aborted handoff leaves them inert (and the
 * sweep disarms them by name when it notices ownership is local). A table
 * too full to arm leaves that environment's records exactly where they
 * are — the conservative direction, printed rather than silently leaked. */
static void env_migrate_arm_confirms(uint32_t partition_id, uint32_t dest_node_id) {
    for (uint32_t i = 0; i < env_ckpt_count; i++) {
        if (env_ckpt_table[i].partition_id != partition_id) continue;
        uint32_t idx = env_ckpt_table[i].index;
        /* Already armed (a retry of the same migration): keep the existing
         * entry — its poll age and confirmation are still valid. */
        if (env_migrate_confirm_find(partition_id, idx)) continue;
        struct EnvMigrateConfirm* c = 0;
        for (uint32_t k = 0; k < ENV_MIGRATE_CONFIRM_MAX; k++)
            if (!env_migrate_confirm[k].active) {
                c = &env_migrate_confirm[k];
                break;
            }
        if (!c) {
            kernel_serial_printf(
                "[ENV_MIGRATE] partition %u, index %u: the confirmation table is full "
                "(%u teardowns still pending) -- this teardown is NOT armed, so its "
                "records stay: records that describe a moved environment (countable "
                "through env_migrate_pending_count()), never a teardown without the "
                "destination's word. Finish it by destroying env by name once the "
                "destination's copy is confirmed.\n",
                (unsigned)partition_id, (unsigned)idx,
                (unsigned)ENV_MIGRATE_CONFIRM_MAX);
            continue;
        }
        c->active        = 1;
        c->confirmed     = 0;
        c->tries         = 0;
        c->destroy_tries = 0;
        c->stop_logged   = 0;
        c->partition_id  = partition_id;
        c->index         = idx;
        c->dest_node     = dest_node_id;
    }
}

static void env_migrate_confirm_disarm(struct EnvMigrateConfirm* c) {
    /* Every caller is a terminal walk end — confirmed and torn down, the
     * partition gone, ownership back, or the bounded give-up. The walk was
     * this node's in-flight migration work, and its counterparty's sweep
     * may just have been blocked behind the same restore and only now be
     * resuming heartbeats: start the resolved tail so that silence is not
     * declared a death (live 2026-10-13: the source declared the live
     * destination DEAD a beat after its own teardown completed). */
    env_migrate_resolved_tick = kernel_tick_counter;
    c->active        = 0;
    c->confirmed     = 0;
    c->tries         = 0;
    c->destroy_tries = 0;
    c->stop_logged   = 0;
}

/* Any entry of this partition still armed? The deferred reclamation waits
 * for the LAST one. */
static int env_migrate_confirm_partition_busy(uint32_t partition_id) {
    for (uint32_t i = 0; i < ENV_MIGRATE_CONFIRM_MAX; i++)
        if (env_migrate_confirm[i].active &&
            env_migrate_confirm[i].partition_id == partition_id) return 1;
    return 0;
}

/* Any RECORDS of this partition still on this node's books? The deferred
 * reclamation also waits for this: an environment whose teardown was never
 * armed (the table-full case) or never succeeded keeps its record, and
 * reclaiming its frames under it would be exactly the double-ownership
 * defect this increment exists to remove. */
static int env_migrate_partition_records_remain(uint32_t partition_id) {
    for (uint32_t i = 0; i < env_ckpt_count; i++)
        if (env_ckpt_table[i].partition_id == partition_id) return 1;
    return 0;
}

/* The destination's wire answer (see env_migrate.h): 0 ("live with
 * contents") only when this node owns the partition, the record is here,
 * and the pour's success tail marked it restored. A staged-but-not-restored
 * slot answers 1 — staged is not restored, and confirming from staged bytes
 * would have the source destroy the only complete copy. */
int env_migrate_live_query(uint32_t partition_id, uint32_t index) {
    if (partition_get_owner_node(partition_id) != cluster_local_node_id()) return 1;
    if (!env_migrate_restored_here(partition_id, index)) return 1;
    const struct EnvCkptRecord* rec = env_ckpt_find(partition_id, index);
    if (!rec) return 1;   /* the instance is gone here */
    if (env_ckpt_restore_pending_record(partition_id, index)) return 1;
    return 0;
}

/* The source's receipt of that answer, recorded against the pending entry
 * by identity. A stale answer (no entry) is dropped by name; a nonzero
 * status is "not yet", not an error — the next period asks again. */
int env_migrate_live_ack(uint32_t partition_id, uint32_t index, uint32_t status) {
    struct EnvMigrateConfirm* c = env_migrate_confirm_find(partition_id, index);
    if (!c) return 1;
    if (status == 0) c->confirmed = 1;
    return 0;
}

/* Test/phase-boundary reset — defined after the machinery it clears (the
 * increment-5 tables above) and clearing both halves of the module's state:
 * staging AND confirmations. A reset that left a pending confirmation would
 * let the next walk tear down an environment nothing ever sent — precisely
 * the defect this increment closes. */
void env_migrate_reset(void) {
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++) {
        struct EnvMigrateSlot* s = &env_migrate_staging[i];
        if (s->active) env_migrate_recv_drop(s->partition_id, s->index);
    }
    for (uint32_t i = 0; i < ENV_MIGRATE_CONFIRM_MAX; i++) {
        struct EnvMigrateConfirm* c = &env_migrate_confirm[i];
        c->active = 0; c->confirmed = 0; c->tries = 0;
        c->destroy_tries = 0; c->stop_logged = 0;
    }
    env_migrate_restored_count = 0;
    env_migrate_restored_next  = 0;
    env_migrate_resolved_tick = 0;    /* a phase boundary is not a migration */
    env_migrate_intent_tick    = 0;
    for (int i = 0; i < ENV_MIGRATE_INTENT_PEERS_MAX; i++)
        env_migrate_peer_intent[i].active = 0;
}

/* ─── The sweep's driver ────────────────────────────────────────────────────
 * Staged bytes do not restore themselves: the wire fills the slot, and this
 * walk turns every COMPLETE slot whose partition this node owns into a live
 * environment — on the sweep, the same context partition_reannounce_tick()
 * and failover_tick() do their periodic work from, because the create round
 * trip yields to init and the NVMe writes belong on the BSP.
 *
 * Two bounds, both deliberate: a PERIOD (the reannounce period — restoring
 * on every sweep would retry a failing create at sweep rate, and a sweep is
 * load-dependent and unbounded), and an ATTEMPTS cap per slot (a create that
 * can never succeed — a manager that never comes up, an environment this
 * node cannot build — stops logging after ENV_MIGRATE_RESTORE_MAX_TRIES,
 * with one final line naming the slot; the bytes stay countable through
 * env_migrate_staging_count() rather than leaking silently). */
#define ENV_MIGRATE_RESTORE_PERIOD_TICKS 1000ull   /* ~10 s at CONSENSUS_TICK_HZ */
#define ENV_MIGRATE_RESTORE_MAX_TRIES   5u

uint32_t env_migrate_tick(uint64_t now) {
    static uint64_t last_attempt = 0;
    if (now < last_attempt + ENV_MIGRATE_RESTORE_PERIOD_TICKS) return 0;
    last_attempt = now;

    uint32_t restored = 0;
    for (int i = 0; i < ENV_MIGRATE_STAGING_MAX; i++) {
        struct EnvMigrateSlot* s = &env_migrate_staging[i];
        if (!s->active || !s->complete) continue;   /* in flight: the sender is
             * synchronous, so a complete slot is the only restorable input */

        if (!partition_exists(s->partition_id)) {
            /* The partition is gone: the bytes charge to a tenant that no
             * longer exists and no restore may rebuild (every gate above
             * would refuse by name). Dropped here so the frames go back with
             * the partition they were charged to. An INCOMPLETE slot of a
             * dead partition is deliberately NOT dropped: it may still be
             * receiving; its teardown is the operator's env_migrate_recv_drop,
             * named rather than guessed. */
            kernel_serial_printf(
                "[ENV_MIGRATE] slot (partition %u, index %u): the partition no longer "
                "exists -- the staged slot is dropped, its frames returned.\n",
                (unsigned)s->partition_id, (unsigned)s->index);
            env_migrate_recv_drop(s->partition_id, s->index);
            continue;
        }
        /* Not ours (yet): the ownership announce may be in flight. The slot
         * waits — its bytes are the tenant's, and this node may well own the
         * partition a moment later. */
        if (partition_get_owner_node(s->partition_id) != cluster_local_node_id())
            continue;
        if (s->restore_tries >= ENV_MIGRATE_RESTORE_MAX_TRIES) continue;

        s->restore_tries++;
        /* The destination announces too (the idle-observer arm, second
         * half): the restore below blocks THIS node's sweep — the create
         * round trip, the parked-posture wait, the pour — and on the
         * consensus leader that sweep carries the only heartbeat stream.
         * The source's announce (partition_migrate) covered the send; this
         * one covers the restore, so no third node — which holds no slot,
         * confirm or intent of its own — declares THIS node dead and
         * adopts the partition under the pour. Observed live on a 3-node
         * run: the restoring destination was declared DEAD at 300 ticks by
         * an idle observer, and escaped adoption only by the role luck of
         * that observer not being leader. Sweep context, so the announce
         * transmit is legal here. */
        env_migrate_intent_begin();
        int rc = env_migrate_restore(s->partition_id, s->index);
        /* The restore returned — the phase it shielded is over. Release the
         * gate and the peers' latches immediately; without this the announce
         * leaves every observer blind for the rest of the 300 s budget after
         * every single restore attempt (found live: a leader killed right
         * after an env create was undeclarable DEAD for minutes — the
         * adoption guard lost to exactly that). */
        env_migrate_intent_end();
        if (rc == ENV_MIGRATE_OK) {
            restored++;
        } else {
            kernel_serial_printf(
                "[ENV_MIGRATE] restore attempt %u of %u for (partition %u, index %u) "
                "refused: %s (a=%u b=%u).\n",
                (unsigned)s->restore_tries, (unsigned)ENV_MIGRATE_RESTORE_MAX_TRIES,
                (unsigned)s->partition_id, (unsigned)s->index,
                env_migrate_status_name(rc),
                (unsigned)env_migrate_refusal.a, (unsigned)env_migrate_refusal.b);
            if (s->active && s->restore_tries >= ENV_MIGRATE_RESTORE_MAX_TRIES)
                kernel_serial_printf(
                    "[ENV_MIGRATE] slot (partition %u, index %u): %u restore attempts "
                    "spent -- the sweep stops trying. The bytes remain staged (countable "
                    "through env_migrate_staging_count()); an operator/test may drop "
                    "the slot, or fix what the last refusal named.\n",
                    (unsigned)s->partition_id, (unsigned)s->index,
                    (unsigned)s->restore_tries);
        }
    }

    /* ─── P3 increment 5: the confirmation walk (the source's half) ───────
     * Same period, same context as the restore loop above: the two halves
     * are the two roles of one migration, and a node can be both in the
     * same period (a partition migrated out while another arrives).
     *
     * Per entry, in order: the gates that say "these records are the truth
     * again" disarm it; a not-yet-confirmed entry polls the destination
     * (one LIVE_REQ per period, bounded, and a stop that stays countable);
     * a CONFIRMED entry runs env_service_destroy() on this node's copy BY
     * NAME — the destroy path's own bookkeeping drops the record, so no
     * record here describes the environment anymore. When the partition's
     * last entry completes and no records of it remain, the reclamation
     * kernel/partition.c Step 5 deferred completes with it. */
    for (uint32_t i = 0; i < ENV_MIGRATE_CONFIRM_MAX; i++) {
        struct EnvMigrateConfirm* c = &env_migrate_confirm[i];
        if (!c->active) continue;

        if (!partition_exists(c->partition_id)) {
            /* The partition is gone: its records and frames die with
             * partition_destroy()'s own teardown, which reclaims for
             * itself. The entry merely stops polling. */
            kernel_serial_printf(
                "[ENV_MIGRATE] teardown (partition %u, index %u): the partition no "
                "longer exists -- the confirmation entry is dropped; its records and "
                "frames went with the partition's own destroy.\n",
                (unsigned)c->partition_id, (unsigned)c->index);
            env_migrate_confirm_disarm(c);
            continue;
        }
        if (partition_get_owner_node(c->partition_id) == cluster_local_node_id()) {
            /* Ownership is local again: the migration aborted after arming,
             * or the environment came back. Either way the records here are
             * the truth again and must stay. */
            kernel_serial_printf(
                "[ENV_MIGRATE] teardown (partition %u, index %u): this node owns the "
                "partition again -- what was armed describes a migration that did not "
                "complete (or a return), and the local records stay.\n",
                (unsigned)c->partition_id, (unsigned)c->index);
            env_migrate_confirm_disarm(c);
            continue;
        }

        if (!c->confirmed) {
            if (c->tries >= ENV_MIGRATE_CONFIRM_MAX_TRIES) {
                if (!c->stop_logged) {
                    c->stop_logged = 1;
                    kernel_serial_printf(
                        "[ENV_MIGRATE] teardown (partition %u, index %u): %u "
                        "confirmation polls of node %u spent -- the sweep stops "
                        "asking. The records stay (countable through "
                        "env_migrate_pending_count()): the destination never said the "
                        "environment is live there, so nothing here may be destroyed.\n",
                        (unsigned)c->partition_id, (unsigned)c->index,
                        (unsigned)c->tries, (unsigned)c->dest_node);
                }
                continue;   /* countable and inert, never force-destroyed */
            }
            c->tries++;
            if (c->tries == 1) {
                kernel_serial_printf(
                    "[ENV_MIGRATE] teardown (partition %u, index %u): asking node %u "
                    "whether the environment is live there -- one poll per period, "
                    "bounded; nothing is destroyed until the word comes back.\n",
                    (unsigned)c->partition_id, (unsigned)c->index,
                    (unsigned)c->dest_node);
            }
            dspp_env_migrate_send_live(((uint64_t)c->partition_id << 32) | c->index,
                                       c->dest_node, c->partition_id, c->index);
            continue;   /* the answer lands as a LIVE_ACK; teardown is next period */
        }

        /* Confirmed by the destination: tear THIS node's half down, by
         * name. The environment id is captured first — the destroy may drop
         * the record under us. */
        const struct EnvCkptRecord* rec = env_ckpt_find(c->partition_id, c->index);
        if (!rec) {
            /* The record is already gone (an operator destroyed the
             * environment by name in the meantime): the goal — no record
             * describing an environment that moved — already holds. */
            env_migrate_confirm_disarm(c);
        } else if (c->destroy_tries >= ENV_MIGRATE_TEARDOWN_MAX_TRIES) {
            if (!c->stop_logged) {
                c->stop_logged = 1;
                kernel_serial_printf(
                    "[ENV_MIGRATE] teardown (partition %u, index %u): %u destroy "
                    "attempts spent, none completed -- the sweep stops trying. The "
                    "record stays (countable): the destination is confirmed, but this "
                    "node's environment could not be ended; an operator may destroy env "
                    "%u by name.\n",
                    (unsigned)c->partition_id, (unsigned)c->index,
                    (unsigned)c->destroy_tries, (unsigned)rec->env_id);
            }
        } else {
            uint16_t d_status = ENV_ERR_INVAL;
            uint32_t d_id = 0, d_part = 0;
            uint32_t env_id = rec->env_id;
            c->destroy_tries++;
            int rc = env_service_destroy(env_id, c->partition_id,
                                         &d_status, &d_id, &d_part);
            if (rc == 0 && d_status == ENV_OK) {
                /* The destroy path's own bookkeeping dropped the record
                 * (env_service.c -> env_ckpt_drop_env), exactly like the
                 * HTTP destroy route. */
                kernel_serial_printf(
                    "[ENV_MIGRATE] teardown (partition %u, index %u): node %u "
                    "confirmed the restore, so env %u is ended here -- its record and "
                    "sidecars are gone; no record on this node describes it.\n",
                    (unsigned)c->partition_id, (unsigned)c->index,
                    (unsigned)c->dest_node, (unsigned)env_id);
                env_migrate_confirm_disarm(c);
            } else if (rc == 0 && d_status == ENV_ERR_NOENT) {
                /* The environment died long ago and only its record
                 * outlived it — exactly as good: the record is dropped, the
                 * goal holds. */
                (void)env_ckpt_drop(c->partition_id, c->index);
                kernel_serial_printf(
                    "[ENV_MIGRATE] teardown (partition %u, index %u): env %u was "
                    "already gone here (no such environment) -- the surviving record "
                    "is dropped, so nothing describes it anymore.\n",
                    (unsigned)c->partition_id, (unsigned)c->index,
                    (unsigned)env_id);
                env_migrate_confirm_disarm(c);
            } else {
                /* No channel, no reply, or a refusal: the environment's
                 * fate is unknown, so the record stays and the attempts
                 * above are bounded. */
                kernel_serial_printf(
                    "[ENV_MIGRATE] teardown (partition %u, index %u): destroy of env "
                    "%u attempt %u of %u did not complete (rc=%d, status=%u) -- the "
                    "record stays and the sweep retries on its period.\n",
                    (unsigned)c->partition_id, (unsigned)c->index,
                    (unsigned)env_id, (unsigned)c->destroy_tries,
                    (unsigned)ENV_MIGRATE_TEARDOWN_MAX_TRIES, rc, (unsigned)d_status);
            }
        }

        /* An entry that will never be active again, for a partition with no
         * armed siblings and no records left, completes the reclamation
         * kernel/partition.c Step 5 deferred. The records-remain gate means
         * a never-armed sibling (the table-full case) keeps its frames with
         * its record — half-reclaiming a partition whose environment still
         * has records here is the double-ownership defect in miniature. */
        if (!c->active &&
            !env_migrate_confirm_partition_busy(c->partition_id) &&
            !env_migrate_partition_records_remain(c->partition_id)) {
            uint32_t frames = partition_reclaim_all_frames(c->partition_id);
            kernel_serial_printf(
                "[ENV_MIGRATE] teardown (partition %u): every environment is confirmed "
                "gone from this node -- the deferred reclamation completes, %u frame(s) "
                "returned. Nothing on this node describes the partition anymore.\n",
                (unsigned)c->partition_id, (unsigned)frames);
        }
    }
    return restored;
}
