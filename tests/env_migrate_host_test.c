/*
 * env_migrate_host_test.c -- POSIX-Environments Roadmap v0.2, Phase P3
 * increment 2 (the transfer): a standalone host-buildable test proving an
 * environment's full footprint -- its checkpoint descriptor plus every
 * payload page the descriptor names -- crosses a real DSPP wire round trip
 * between two simulated nodes, linked against the REAL, unmodified
 * kernel/env_migrate.c, kernel/env_ckpt.c, net/dspp.c and
 * net/dspp_checkpoint.c -- not a reimplementation of any of them.
 *
 * ─── Why "two simulated nodes" means what it means here ────────────────────
 * The technique tests/cross_node_migration_host_test.c established for
 * streams, reused unchanged: this codebase compiles exactly one kernel
 * image, so two real machines are two independent processes and cannot be
 * spun up here. "Node B" is therefore the same process replaying -- through
 * the REAL receive path -- the exact Ethernet-framed bytes "node A" really
 * emitted. The fake NVMe backing store is NOT reset between the two, for
 * the same reason that test gives: it stands in for a physical medium, and
 * two real machines are naturally on separate hardware, so a single shared
 * store proves correct wire encode/decode and apply-side fidelity just as
 * well as two would. cluster_local_node_id() is a settable fake (the same
 * one tests/partition_host_test.c established) so one process can represent
 * "whichever node's identity is currently active" without linking
 * net/consensus.c's machinery, which this test has no interest in.
 *
 * ─── The three phases, and the one honest shortcut each takes ──────────────
 *
 * Phase A -- the REAL sender, node A (id 1). env_migrate_send() runs
 * unmodified against a live checkpoint record, every Ethernet-framed DSPP
 * packet e1000_transmit() is handed is captured, and the source is checked
 * afterwards. The capture stub also ANSWERS, because the sender now waits
 * for a BEGIN_ACK and for every fragment's PAGE_ACK: a driver stub that
 * only recorded frames models a peer that has crashed, and every transfer
 * here would (correctly) abandon. The answers are healthy-peer status-0
 * replies for exactly the reason cross_node's are, and that reason is worth
 * stating precisely: they are NOT produced by running the receive path
 * inside the driver call, because net/dspp.c's re-entrancy guard on its one
 * static frame buffer deliberately DROPS any frame nested inside a
 * transmit (dspp_tx_reentrant_dropped) -- which is precisely what a
 * synchronous echo would be. On real hardware the ACK arrives through the
 * timer ISR's separate receive path while the sender spins. Phase A
 * therefore proves the SENDER half (frame content, waiting, retransmit,
 * refusal, source-untouched) and captures the bytes; the receiver half is
 * not stubbed into it, it is run for real next.
 *
 * Phase B -- the REAL receiver, node B (id 2). The process switches
 * identity, the staging table is empty, and every byte captured in Phase A
 * is fed through the real path: net/net.c's own ETHERTYPE_DSPP framing
 * checked, dspp_rx_dispatch(), dspp_env_migrate_rx(), then
 * kernel/env_migrate.c's env_migrate_recv_begin()/_page(). The ACKs that
 * path transmits are captured too, so the receive path's OWN output is
 * asserted rather than assumed. What this proves is the property the whole
 * increment exists for: the bytes node B stages -- descriptor, region
 * pages, and NVMe extent pages alike -- are byte-for-byte the bytes node A's
 * environment held.
 *
 * Phase C -- refusals and retransmission against the real sender, driven
 * through the same peer stub (a lost BEGIN, a lost fragment, a destination
 * that refuses). Every one asserts the source is untouched afterwards:
 * v0.2's "never ownership without data" at the environment level.
 *
 * Phase D -- the sender's own identity guards, which must cost no frames.
 *
 * Phase E -- the receiver's refusals, driven as hand-built frames through
 * the REAL dspp_rx_dispatch(). Frames are built here rather than captured
 * because the sequences under test are ones a correct sender never
 * produces, and Phase B already proved the wire format.
 *
 * What this deliberately does NOT cover, each its own increment: the
 * orchestration around this module (partition_migrate() calling the step,
 * and aborting before the ownership handoff when it falls short) is
 * tests/partition_migrate_phase6_host_test.c's Scenario 7, and the restore
 * of a staged slot into a live environment is increment 4. Phase F below
 * covers THIS module's own half of increment 3 — the whole
 * freeze → send → thaw step, against the real sender and the real wire —
 * which is what partition.c's single call resolves to.
 *
 * Phase H -- the confirmation and the gated teardown (P3 increment 5).
 * Two halves. The DESTINATION half proves what a LIVE_ACK is grounded in:
 * env_migrate_live_query reads this node's own state -- a record alone is
 * not a restoration, staged bytes are not a pour, an unsettled restore is
 * not live. The SOURCE half runs the real env_migrate_partition() (which
 * arms one teardown confirmation per environment) and then the real sweep:
 * every gate that must keep the records -- a refused send, a not-yet
 * answer, ownership coming back, a partition that vanished, a destination
 * that never replies, a teardown that cannot complete -- the one gate that
 * destroys BY NAME on the destination's word, and the deferred frame
 * reclamation that completes only when the last record is gone.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -I user \
 *       -o /tmp/env_migrate_host_test \
 *       tests/env_migrate_host_test.c kernel/env_migrate.c kernel/env_ckpt.c \
 *       net/dspp.c net/dspp_checkpoint.c
 *   /tmp/env_migrate_host_test
 */
#include "kernel/stream.h"        /* stream_migrate_recv_* stubs for dspp.c */
#include "kernel/simi_ctx_migrate.h"
#include "kernel/env_ckpt.h"
#include "kernel/env_migrate.h"
#include "kernel/env_proto.h"        /* ENV_OK / ENV_ERR_* -- the create round trip's answer */
#include "kernel/env_service.h"      /* env_service_create()/destroy() prototypes (Phase G) */
#include "kernel/env_payload.h"      /* env_payload_wait_parked() prototype (Phase G) */
#include "kernel/checkpoint_delta.h"   /* ckpt_mark_dirty, CKPT_REGION_ENV */
#include "kernel/env_storage.h"        /* env_storage_extent_of stub */
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "net/e1000.h"
#include "net/dspp.h"
#include "net/net.h"
#include "tests/partition_host_stubs.h"  /* env_ckpt.c's partition_pause/_resume/_is_paused/_exists */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ══════════════════════════════════════════════════════════════════════════
 * Stubs for kernel/env_ckpt.c's dependencies (the identical choices
 * tests/env_ckpt_host_test.c makes -- see that file for the reasoning).
 * ══════════════════════════════════════════════════════════════════════ */
static uint32_t g_dirty_mask = 0;
void ckpt_mark_dirty(uint32_t region) {
    if (region < 32) g_dirty_mask |= (1u << region);
}
int env_storage_extent_of(uint32_t partition_id, uint32_t index,
                          uint64_t* out_lba, uint64_t* out_sectors,
                          uint32_t* out_bytes) {
    (void)partition_id; (void)index;
    (void)out_lba; (void)out_sectors; (void)out_bytes;
    return 1;   /* nothing to name: the record's own extent fields are set below */
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stubs for kernel/env_migrate.c's and net/dspp.c's dependencies that are
 * not under test here. Frame-pool fake: a host allocation, partition-charged
 * on both sides so the accounting rule the receiver enforces is the real one.
 * ══════════════════════════════════════════════════════════════════════ */
void kernel_serial_print(const char* s) { (void)s; }
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
volatile uint64_t kernel_tick_counter = 0;

void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    void* p = aligned_alloc(4096, FRAME_SIZE);
    if (!p) return 0;
    memset(p, 0, FRAME_SIZE);
    return p;
}
/* The staging side now takes its frames top-down (frame_pool.h): same fake
 * allocator here — the host test has no watermark to preserve, only bytes
 * to hold. */
void* allocate_physical_ram_frame_for_partition_top(uint32_t partition_id) {
    return allocate_physical_ram_frame_for_partition(partition_id);
}
int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id) {
    (void)partition_id;
    free(frame);
    return 0;
}

/* net/dspp.c's routing and policy surface. Permissive stand-ins, the same
 * judgment call used throughout this suite for out-of-scope primitives: the
 * migrate families are what is under test, and dspp_page_read_allowed()'s
 * own logic already has dedicated coverage in dspp_phase5_host_test.c. */
struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
int partition_is_local(uint32_t partition_id) { (void)partition_id; return 1; }
int partition_holds_write_lease(uint32_t partition_id) { (void)partition_id; return 1; }
void process_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) { (void)packet; (void)now; }
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) { (void)packet; (void)now; }
void partition_sync_upsert(uint32_t partition_id, uint32_t owner_node_id, const char* name, uint32_t source_node_id) { (void)partition_id; (void)owner_node_id; (void)name; (void)source_node_id; }
void partition_sync_withdraw(uint32_t partition_id, uint32_t source_node_id) { (void)partition_id; (void)source_node_id; }
void partition_sync_ownedset(uint32_t source_node_id, uint32_t generation, const uint32_t* ids, uint32_t count) { (void)source_node_id; (void)generation; (void)ids; (void)count; }
static int learn_calls = 0, forget_calls = 0;
void service_remote_learn(const char* name, uint32_t node_id, uint32_t partition_id,
                          uint8_t kind, uint32_t port, uint32_t uid, uint8_t serving) {
    (void)name; (void)node_id; (void)partition_id; (void)kind; (void)port; (void)uid; (void)serving;
    learn_calls++;
}
void service_remote_forget(const char* name, uint32_t node_id) {
    (void)name; (void)node_id; forget_calls++;
}

/* ─── The stream and context receive families, stubbed ──────────────────────
 * FAITHFUL. This test never emits STREAM (11-14 is checkpoint, 15-17
 * partition, stream is the generic migrate path) or CTX (15-17's sibling)
 * opcodes, so these are not reached; were one to arrive, refusing (nonzero)
 * is what a node not set up for that family answers. Full coverage lives in
 * tests/cross_node_migration_host_test.c and tests/simi_ctx_migrate_
 * host_test.c respectively. */
int stream_migrate_recv_begin(uint64_t transfer_id, uint32_t partition_id,
                              const char* name, const char* mime_type,
                              uint64_t size, uint32_t frames_used,
                              uint32_t owner_uid) {
    (void)transfer_id; (void)partition_id; (void)name; (void)mime_type;
    (void)size; (void)frames_used; (void)owner_uid;
    return 1;
}
int stream_migrate_recv_page(uint64_t transfer_id, uint32_t page_index,
                             uint32_t frag_index, const uint8_t* data) {
    (void)transfer_id; (void)page_index; (void)frag_index; (void)data;
    return 1;
}
SimiCtxMigStatus simi_ctx_migrate_recv_begin(uint64_t tid, const char* name,
                                             uint64_t total_bytes,
                                             uint32_t total_chunks,
                                             uint64_t program_hash) {
    (void)tid; (void)name; (void)total_bytes; (void)total_chunks; (void)program_hash;
    return SIMI_CTXMIG_ERR_NO_PROGRAM;
}
SimiCtxMigStatus simi_ctx_migrate_recv_chunk(uint64_t tid, uint32_t idx,
                                             const uint8_t* data, uint32_t n) {
    (void)tid; (void)idx; (void)data; (void)n;
    return SIMI_CTXMIG_ERR_NO_TRANSFER;
}

/* ─── Settable node identity (tests/partition_host_test.c's technique) ───── */
static uint32_t g_fake_local_node_id = 0;
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
uint32_t cluster_leader_id(void) { return 0; }

/* kernel/failover.c's liveness stamp, reached from env_migrate_recv_intent()
 * (an intent frame is the same liveness evidence a heartbeat is — the
 * release-vs-heartbeat race this records). A recording stub, not a no-op:
 * the intent tests below assert that BOTH the announce and the release
 * refresh the observer's last-seen stamp, which is what keeps a just-
 * unblocked source from being declared DEAD on the silence its own create
 * accumulated (found live 2026-10-23 on a 3-node cluster). The stubs MODEL
 * the real table: a first heartbeat note WATCHES the peer, and
 * failover_peer_status() answers -1 for a node never noted -- which is how
 * the intent tests below can pin that recv_intent refreshes a WATCHED
 * peer's stamp and never CREATES a watch entry. */
#define FAKE_PEERS_MAX 8
static int      g_watched[FAKE_PEERS_MAX + 1];
static uint32_t g_hb_last_node = 0;
static uint64_t g_hb_last_now  = 0;
static int      g_hb_calls     = 0;
static void fake_watch(uint32_t node_id) {
    if (node_id && node_id <= FAKE_PEERS_MAX) g_watched[node_id] = 1;
}
void failover_note_heartbeat(uint32_t node_id, uint64_t now) {
    fake_watch(node_id);
    g_hb_last_node = node_id;
    g_hb_last_now  = now;
    g_hb_calls++;
}
int failover_peer_status(uint32_t node_id) {
    /* PEER_ALIVE (0) for a watched node; -1 (unknown) for one never noted. */
    return (node_id && node_id <= FAKE_PEERS_MAX && g_watched[node_id]) ? 0 : -1;
}
/* kernel/failover.c's sweep keepalive, pumped by every blocking wait that
 * runs on the sweep (the death-during-create fix: a blocked create/migrate/
 * restore must not silence the leader's heartbeat stream). Counted so the
 * send phases below can pin that the ACK waits really pump it. */
static int g_keepalive_calls = 0;
void failover_sweep_keepalive(void) { g_keepalive_calls++; }

/* net/net.c's real global -- not linked; only the src MAC dspp_transmit_raw
 * stamps into the Ethernet header reads it. */
MACAddr net_my_mac;

/* ─── Stateful fake NVMe (identical technique to cross_node's) ────────────── */
void* io_sq = (void*)1;
void* io_cq = (void*)1;
#define FAKE_NVME_MAX_PAGES 64
static uint64_t fake_lba[FAKE_NVME_MAX_PAGES];
static uint8_t  fake_page[FAKE_NVME_MAX_PAGES][4096];
static int      fake_count = 0;
static int fake_find(uint64_t slba) {
    for (int i = 0; i < fake_count; i++) if (fake_lba[i] == slba) return i;
    return -1;
}
int nvme_read_sync(uint64_t slba, void* buf) {
    int idx = fake_find(slba);
    if (idx < 0) { memset(buf, 0, 4096); return 0; }
    memcpy(buf, fake_page[idx], 4096);
    return 0;
}
int nvme_write_sync(uint64_t slba, const void* buf) {
    int idx = fake_find(slba);
    if (idx < 0) {
        if (fake_count >= FAKE_NVME_MAX_PAGES) return -1;
        idx = fake_count++;
        fake_lba[idx] = slba;
    }
    memcpy(fake_page[idx], buf, 4096);
    return 0;
}

/* ─── Capture + answer: the peer, as a live node B behaves ─────────────────
 * Every frame is captured (Phase B replays the bytes); every ENV request is
 * ALSO answered in place, because the sender blocks on the ACK. Deliberate
 * loss is injected by dropping the answer, which is what makes RETRANSMISSION
 * observable rather than merely present: no answer means the sender's stall
 * bound ends the wait and it retransmits what was never acknowledged. */
#define MAX_CAPTURED_FRAMES 512
#define FRAME_STRIDE (ETH_HDR_LEN + (unsigned)sizeof(struct DSPPEnvMigrateBeginPacket))
static uint8_t  captured_frame[MAX_CAPTURED_FRAMES][FRAME_STRIDE];
static uint16_t captured_frame_len[MAX_CAPTURED_FRAMES];
static int      captured_frame_count = 0;
static NicRole  last_tx_role = NIC_ROLE_NONE;

static int  loss_page = -1, loss_frag = 0, loss_times = 0, loss_applied = 0;
static int  nack_begin = 0, nack_page = 0;
static int  begin_loss_times = 0, begin_loss_applied = 0;
static int  begin_ack_count = 0, page_ack_count = 0;
/* P3 increment 5: LIVE_REQs are fire-and-forget polls -- the source never
 * waits inside the transmit, so the peer deliberately does NOT answer them
 * (each test delivers its own LIVE_ACK through the real dispatch instead).
 * This counts what actually went on the wire, which is how "polled, but
 * torn down anyway" is distinguished from "never asked". */
static int  live_req_count = 0;

static void peer_reset(void) {
    loss_page = -1; loss_frag = 0; loss_times = 0; loss_applied = 0;
    nack_begin = 0; nack_page = 0;
    begin_loss_times = 0; begin_loss_applied = 0;
    begin_ack_count = 0; page_ack_count = 0;
    live_req_count = 0;
}

void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    last_tx_role = role;
    if (captured_frame_count < MAX_CAPTURED_FRAMES) {
        uint16_t n = size;
        if (n > FRAME_STRIDE) n = (uint16_t)FRAME_STRIDE;
        memcpy(captured_frame[captured_frame_count], buf, n);
        captured_frame_len[captured_frame_count] = n;
        captured_frame_count++;
    }
    kernel_tick_counter += 1;   /* time passes while frames are on the wire */

    if (size < ETH_HDR_LEN + sizeof(struct DSPPEnvMigrateHeader)) return;
    struct DSPPEnvMigrateHeader* h =
        (struct DSPPEnvMigrateHeader*)((uint8_t*)buf + ETH_HDR_LEN);
    if (h->magic != DSPP_MIGRATE_MAGIC) return;

    if (h->opcode == DSPP_MIGRATE_ENV_LIVE_REQ) {
        live_req_count++;   /* counted, never answered: the poll's answer is
                             * delivered by the test through the real rx path */
        return;
    }
    if (h->opcode == DSPP_MIGRATE_ENV_BEGIN_REQ) {
        if (nack_begin) {
            dspp_migrate_note_ack(h->transfer_id, DSPP_MIGRATE_ENV_BEGIN_ACK, 0, 0, 1);
            return;
        }
        if (begin_loss_applied < begin_loss_times) { begin_loss_applied++; return; }
        begin_ack_count++;
        dspp_migrate_note_ack(h->transfer_id, DSPP_MIGRATE_ENV_BEGIN_ACK, 0, 0, 0);
        return;
    }
    if (h->opcode != DSPP_MIGRATE_ENV_PAGE_REQ) return;

    if (nack_page) {
        dspp_migrate_note_ack(h->transfer_id, DSPP_MIGRATE_ENV_PAGE_ACK,
                              h->page_index, h->frag_index, 1);
        return;
    }
    if ((int)h->page_index == loss_page && h->frag_index == (uint32_t)loss_frag &&
        loss_applied < loss_times) {
        loss_applied++;
        return;   /* no answer: this fragment "never arrived" */
    }
    page_ack_count++;
    dspp_migrate_note_ack(h->transfer_id, DSPP_MIGRATE_ENV_PAGE_ACK,
                          h->page_index, h->frag_index, 0);
}

/* Feeds one captured Ethernet frame through the real receive path exactly
 * the way net/net.c's net_rx_dispatch() would: check the ethertype, strip
 * the Ethernet header, hand the payload to dspp_rx_dispatch() -- the real
 * function, not a reimplementation of its routing. */
static void deliver(int i) {
    struct EthernetHeader* eth = (struct EthernetHeader*)captured_frame[i];
    if (ntohs(eth->ethertype) != ETHERTYPE_DSPP) {
        printf("FAIL: captured frame %d is not ETHERTYPE_DSPP-framed\n", i);
        checks_failed++;
        return;
    }
    dspp_rx_dispatch(captured_frame[i] + ETH_HDR_LEN,
                     (uint16_t)(captured_frame_len[i] - ETH_HDR_LEN));
}

/* ══════════════════════════════════════════════════════════════════════════
 * The environment under test: partition 77, index 0. Its three regions are
 * real host buffers (frame-pool frames are identity-mapped on the real
 * kernel; a host pointer is the same one memcpy either way), and its durable
 * extent lives in the fake NVMe -- so the payload spans BOTH kinds of page
 * env_migrate_read_page() walks, which is the point: a transfer that only
 * carried one of them would look complete while half the environment stayed
 * behind. ═════════════════════════════════════════════════════════════════ */
#define SRC_PARTITION 77
#define SRC_INDEX     0
#define DST_NODE      2
#define STATE_LBA     8192ull
#define STATE_SECTORS 16ull            /* 2 pages at the stream family's 8-sector geometry */

static uint8_t src_r0[2][4096] __attribute__((aligned(4096)));
static uint8_t src_r1[1][4096] __attribute__((aligned(4096)));
static uint8_t src_r2[1][4096] __attribute__((aligned(4096)));

/* Page numbering is the record's own: region frames in record order, then
 * the extent's pages. Both sides compute it from the SAME record. */
#define SRC_PAGES 6u
static const uint8_t page_fill[SRC_PAGES] = { 0xA0, 0xA1, 0xB1, 0xC1, 0xD1, 0xD2 };

static int all_bytes(const uint8_t* p, uint8_t fill) {
    if (!p) return 0;
    for (int b = 0; b < 4096; b++) if (p[b] != fill) return 0;
    return 1;
}

/* The record, exactly as P1a's gate would accept it. */
static struct EnvCkptRecord g_src_record;
static struct EnvCkptRecord good_record(uint32_t partition, uint32_t index) {
    struct EnvCkptRecord r;
    memset(&r, 0, sizeof r);
    r.magic        = ENV_CKPT_REC_MAGIC;
    r.version      = ENV_CKPT_REC_VERSION;
    r.size         = (uint32_t)sizeof(struct EnvCkptRecord);
    r.sequence     = 7;
    r.partition_id = partition;
    r.index        = index;
    r.env_id       = 100u + index;
    r.console_id   = 3;
    r.flags        = ENV_CKPT_FLAG_QUIESCED;
    r.n_tasks      = 2;
    r.n_regions    = ENV_CKPT_MAX_REGIONS;
    r.n_chans      = 4;
    r.chans[0] = 0x11; r.chans[1] = 0x12; r.chans[2] = 0x13; r.chans[3] = 0x14;

    r.regions[0].base  = (uint64_t)(uintptr_t)src_r0;
    r.regions[0].frames = 2;
    r.regions[0].kind  = ENV_CKPT_REGION_POSIX_HEAP;
    r.regions[1].base  = (uint64_t)(uintptr_t)src_r1;
    r.regions[1].frames = 1;
    r.regions[1].kind  = ENV_CKPT_REGION_RD_HEAP;
    r.regions[2].base  = (uint64_t)(uintptr_t)src_r2;
    r.regions[2].frames = 1;
    r.regions[2].kind  = ENV_CKPT_REGION_RD_STORAGE;

    r.state_lba     = STATE_LBA;
    r.state_sectors = STATE_SECTORS;
    r.state_bytes   = (uint32_t)STATE_SECTORS * 512u;

    strcpy(r.tasks[0].name, "aerosls.posix.0");
    r.tasks[0].kind      = ENV_CKPT_TASK_POSIX_SIDECAR;
    r.tasks[0].cap_count = 5;
    r.tasks[0].entry     = 0x401000ull;
    r.tasks[0].stack_bottom = 0x700000ull;
    r.tasks[0].stack_top    = 0x701000ull;

    strcpy(r.tasks[1].name, "drv.ramdisk.0");
    r.tasks[1].kind      = ENV_CKPT_TASK_RAMDISK_SIDECAR;
    r.tasks[1].cap_count = 4;
    r.tasks[1].entry     = 0x402000ull;
    r.tasks[1].stack_bottom = 0x702000ull;
    r.tasks[1].stack_top    = 0x703000ull;
    return r;
}

/* The source's pages, refilled from their known patterns, so "the source is
 * untouched" is a byte-exact check rather than a comparison against a
 * snapshot this test itself might have disturbed. */
static void fill_source(void) {
    for (int b = 0; b < 4096; b++) {
        src_r0[0][b] = page_fill[0];
        src_r0[1][b] = page_fill[1];
        src_r1[0][b] = page_fill[2];
        src_r2[0][b] = page_fill[3];
    }
    uint8_t pg[4096];
    memset(pg, page_fill[4], 4096);
    nvme_write_sync(STATE_LBA + 0, pg);
    memset(pg, page_fill[5], 4096);
    nvme_write_sync(STATE_LBA + 8, pg);
}
static int source_is_intact(void) {
    if (!all_bytes(src_r0[0], page_fill[0])) return 0;
    if (!all_bytes(src_r0[1], page_fill[1])) return 0;
    if (!all_bytes(src_r1[0], page_fill[2])) return 0;
    if (!all_bytes(src_r2[0], page_fill[3])) return 0;
    uint8_t pg[4096];
    if (nvme_read_sync(STATE_LBA + 0, pg) != 0 || !all_bytes(pg, page_fill[4])) return 0;
    if (nvme_read_sync(STATE_LBA + 8, pg) != 0 || !all_bytes(pg, page_fill[5])) return 0;
    return 1;
}
/* The staged page `p` on the receiving side, if it landed. */
static const uint8_t* staged_page(uint32_t p) {
    return env_migrate_recv_page_ptr(SRC_PARTITION, SRC_INDEX, p);
}

/* ══════════════════════════════════════════════════════════════════════
 * Phase G stand-ins (P3 increment 4: the restore). env_service.c and
 * env_payload.c are deliberately NOT linked: what the restore must prove is
 * its ORDER and its byte discipline against the real staging table, the real
 * record layer and the real NVMe fake — the create round trip itself is
 * init's half and is exercised live by tests/env_checkpoint_restore_check.sh
 * (the same split the boot replay's host coverage already makes).
 * ════════════════════════════════════════════════════════════════════ */
static uint32_t g_fake_owner_node = 1;
uint32_t partition_get_owner_node(uint32_t partition_id) {
    (void)partition_id; return g_fake_owner_node;
}

/* P3 increment 5: the deferred reclamation the confirmation walk runs when
 * the partition's LAST record leaves this node's books. frame_pool.c is not
 * linked here (the per-frame bitmap's own coverage is
 * tests/frame_quota_host_test.c), so this counts calls and arguments -- the
 * scheduler_fairness / partition_migrate phase6 technique: orchestration
 * proven here, frame mechanics proven there. */
static int      g_reclaim_calls = 0;
static uint32_t g_reclaim_last_arg = 0xFFFFFFFFu;
uint32_t partition_reclaim_all_frames(uint32_t partition_id) {
    g_reclaim_calls++;
    g_reclaim_last_arg = partition_id;
    return 0;
}

static int      g_create_rc = 0;              /* -1 = the channel never answered */
static uint16_t g_create_status = ENV_OK;     /* the ENV_* status the create reports */
static uint32_t g_create_env_id = 500;
static int      g_create_calls = 0;
static struct EnvCkptRecord g_create_record;  /* what "init" registers on a successful create */

int env_service_create(uint32_t partition, uint32_t index,
                       uint16_t* out_status, uint32_t* out_env_id) {
    g_create_calls++;
    if (out_status) *out_status = g_create_status;
    if (out_env_id) *out_env_id = g_create_env_id;
    if (g_create_rc != 0) return g_create_rc;
    /* The real create's ENV_REGISTER ends in env_ckpt_record(); this stub
     * does the same with the test's fresh-environment record, whose bases
     * are the DESTINATION's placement. */
    g_create_record.partition_id = partition;
    g_create_record.index        = index;
    g_create_record.env_id       = g_create_env_id;
    (void)env_ckpt_record(&g_create_record);
    return 0;
}

static int      g_destroy_calls = 0;
static uint32_t g_destroy_env_id = 0;
static int      g_destroy_rc = 0;   /* P3 increment 5: -1 models a channel
                                     * that never answers -- no reply, no
                                     * side effects, exactly what a dead
                                     * init leaves behind */
int env_service_destroy(uint32_t env_id, uint32_t partition,
                        uint16_t* out_status, uint32_t* out_env_id,
                        uint32_t* out_partition) {
    g_destroy_calls++;
    g_destroy_env_id = env_id;
    if (g_destroy_rc != 0) return g_destroy_rc;
    if (out_status)   *out_status   = ENV_OK;
    if (out_env_id)   *out_env_id   = env_id;
    if (out_partition)*out_partition= partition;
    /* The real ENV_DESTROY path tells the checkpoint the environment is gone
     * (env_service.c calls env_ckpt_drop_env), so a later store upserts onto
     * a clean identity -- exactly what a return migration needs. */
    (void)env_ckpt_drop_env(partition, env_id);
    return 0;
}

static int g_parked = 1;
int env_payload_wait_parked(const struct EnvCkptRecord* rec) {
    (void)rec; return g_parked;
}

/* The image-window helpers the migrate path shares with the payload layer
 * (env_payload.h's P3 block). This test's world has NO live sidecar
 * processes by default, so the list answers with an empty tail and err=0 —
 * exactly the page stream the pre-image migrate always sent, which keeps
 * every phase's page counts and orderings below unchanged. G1b raises
 * g_img_tail to a REAL tail, so the restore's tail arithmetic (the count is
 * the slot's declared total minus the record's payload), its pour order
 * (regions, extent, then tail — the sender's own numbering) and its
 * halves-agreement gate are all exercised against a declared total of
 * payload + tail. */
static uint32_t g_img_tail = 0;
static uint8_t  g_img_frame[4][4096] __attribute__((aligned(4096)));
/* The fake sidecar the stub hands back. ProcessDescriptor is complete only
 * in the kernel's translation units — this file never touches its fields,
 * it just has to be a non-NULL pointer the restore can carry and pass back
 * to env_payload_resolve_vaddr, so an aligned opaque blob is the honest
 * spelling here. */
static uint64_t g_img_pd_words[32] __attribute__((aligned(16)));
#define G_IMG_PD ((struct ProcessDescriptor*)(void*)g_img_pd_words)
#define G_IMG_VBASE 0x400000000000ull   /* env_payload.c's EP_IMAGE_VBASE */

uint32_t env_payload_image_window_list(const struct EnvCkptRecord* rec,
                                       uint64_t* vaddrs,
                                       struct ProcessDescriptor** pds,
                                       uint32_t cap, uint32_t* err) {
    (void)rec;
    if (err) *err = 0;
    if (g_img_tail > cap) { if (err) *err = 2; return 0; }
    for (uint32_t k = 0; k < g_img_tail; k++) {
        vaddrs[k] = G_IMG_VBASE + (uint64_t)k * 4096ull;
        pds[k]    = G_IMG_PD;
    }
    return g_img_tail;
}
int env_payload_proc_parked(const struct ProcessDescriptor* pd) {
    (void)pd; return 1;
}
int env_payload_resolve_vaddr(uint64_t cr3, uint64_t vaddr, uint64_t* out_phys) {
    (void)cr3;
    if (vaddr < G_IMG_VBASE) return 0;
    uint64_t k = (vaddr - G_IMG_VBASE) / 4096ull;
    if (k >= sizeof g_img_frame / sizeof g_img_frame[0]) return 0;
    *out_phys = (uint64_t)(uintptr_t)&g_img_frame[k][0];
    return 1;
}

/* ── Phase G's own world: the destination's regions and pages ──────────
 * The staged record names the bases the payload crossed FOR (the placement
 * both kernels must agree on); the fake create registers the destination's
 * freshly allocated regions. The happy path uses the same buffers for both
 * (placement matched); the placement-refusal case deliberately splits them. */
#define G_PART 91u
static uint8_t g_live_r0[2][4096] __attribute__((aligned(4096)));
static uint8_t g_live_r1[1][4096] __attribute__((aligned(4096)));
static uint8_t g_live_r2[1][4096] __attribute__((aligned(4096)));
static uint8_t g_other_r0[2][4096] __attribute__((aligned(4096)));
static uint8_t g_other_r1[1][4096] __attribute__((aligned(4096)));
static uint8_t g_other_r2[1][4096] __attribute__((aligned(4096)));
static uint8_t g_pages[8][4096] __attribute__((aligned(4096)));   /* 6 payload + up to 2 tail */

#define G_FILLS { 0x31, 0x32, 0x33, 0x34, 0x41, 0x42 }   /* 4 region pages, 2 extent */

/* A record like good_record's, but with settable region bases -- the
 * placement question every restore case turns on. */
static struct EnvCkptRecord g_record(uint32_t part, uint32_t idx,
                                     const void* r0, const void* r1, const void* r2) {
    struct EnvCkptRecord r = good_record(part, idx);
    r.regions[0].base = (uint64_t)(uintptr_t)r0;
    r.regions[1].base = (uint64_t)(uintptr_t)r1;
    r.regions[2].base = (uint64_t)(uintptr_t)r2;
    return r;
}

static void g_fill_pages(void) {
    const uint8_t fills[6] = G_FILLS;
    for (uint32_t p = 0; p < 6; p++)
        for (uint32_t b = 0; b < 4096; b++) g_pages[p][b] = fills[p];
}

static int g_pages_all(uint32_t p, uint8_t fill) {
    for (uint32_t b = 0; b < 4096; b++) if (g_pages[p][b] != fill) return 0;
    return 1;
}

/* Stage a slot DIRECTLY through the real receive entries. Phase B/F already
 * proved these are fed faithfully by the wire; here the staging is the
 * restore's INPUT, not its subject, so fragments are built rather than
 * captured (the file's Phase E precedent for hand-built frames). */
static void g_stage(uint32_t part, uint32_t idx, const struct EnvCkptRecord* rec,
                    uint32_t n_pages) {
    uint64_t tid = ((uint64_t)part << 32) | (uint64_t)idx;
    if (env_migrate_recv_begin(tid, part, idx, n_pages, rec,
                               (uint32_t)sizeof(*rec)) != 0) return;
    for (uint32_t p = 0; p < n_pages; p++)
        for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++)
            (void)env_migrate_recv_page(tid, p, f,
                                        g_pages[p] + f * DSPP_MIGRATE_FRAG_BYTES);
}

/* ─── Phase H helpers ────────────────────────────────────────────────────────
 * h_send_n registers n environments of `part` (create-time records, the
 * Phase F posture), defines and pauses the partition, and runs the REAL
 * env_migrate_partition() against the answering peer -- the setup every
 * confirmation case starts from, so each case's only subject is the
 * teardown gating itself. Call peer_reset() yourself if a case needs loss
 * injected (peer_reset() clears the knobs, so it must run before they are
 * set). Returns the step's own code, or -100 if a record could not even be
 * registered (a setup failure, not a subject). */
static int h_send_n(uint32_t part, uint32_t n) {
    g_fake_local_node_id = 1;   /* the source's identity for every case */
    g_fake_owner_node    = 1;   /* owned here at send time */
    for (uint32_t i = 0; i < n; i++) {
        struct EnvCkptRecord r = good_record(part, i);
        r.flags = 0;            /* create-time: nothing has ever frozen it */
        if (env_ckpt_record(&r) != 0) return -100;
    }
    host_partition_define(part);
    partition_pause(part);
    uint32_t e = 0, m = 0;
    return env_migrate_partition(part, DST_NODE, 0, &e, &m);
}

/* Deliver a hand-built LIVE_ACK through the REAL dspp_rx_dispatch -- Phase
 * E's precedent for frames a correct sender never produces, except these
 * are normal: the test delivers the destination's answer by hand because
 * the peer stub deliberately does not answer polls. node_dest_id = 1 is the
 * source's identity, so the family's self-filter passes exactly as on the
 * wire. status is the whole answer: 0 = "live with its contents", anything
 * else = "not yet". */
static void h_deliver_live_ack(uint32_t part, uint32_t idx, uint8_t status) {
    struct DSPPEnvMigrateHeader ack;
    memset(&ack, 0, sizeof ack);
    ack.magic          = DSPP_MIGRATE_MAGIC;
    ack.opcode         = DSPP_MIGRATE_ENV_LIVE_ACK;
    ack.node_source_id = (uint16_t)DST_NODE;
    ack.node_dest_id   = 1;
    ack.transfer_id    = ((uint64_t)part << 32) | idx;
    ack.partition_id   = part;
    ack.env_index      = idx;
    ack.status         = status;
    dspp_rx_dispatch(&ack, (uint16_t)sizeof(ack));
}

/* Reset the Phase G fakes to their healthy defaults, between cases. */
static void g_reset_fakes(void) {
    g_fake_owner_node = 2;        /* the destination identity Phase G runs as */
    g_create_rc = 0;
    g_create_status = ENV_OK;
    g_create_env_id = 500;
    g_create_calls = 0;
    g_destroy_calls = 0;
    g_destroy_env_id = 0;
    g_destroy_rc = 0;
    g_parked = 1;
    g_img_tail = 0;   /* the fake image windows hold nothing again */
    memset(&g_create_record, 0, sizeof g_create_record);
}

int main(void) {
    printf("=== env_migrate: P3 increment 2, the transfer ===\n\n");

    g_fake_local_node_id = 1;   /* node A */
    host_partition_define(SRC_PARTITION);
    env_ckpt_reset();
    env_migrate_reset();
    fill_source();
    g_src_record = good_record(SRC_PARTITION, SRC_INDEX);

    /* ── 0. The payload geometry, stated before anything moves ─────────────
     * Both sides derive this from the record that crosses in the BEGIN, so
     * if it is wrong here it is wrong on both sides in the same way and the
     * transfer would still "complete". Asserting the expected number up
     * front is what gives the later counts something to be wrong AGAINST. */
    CHECK(env_ckpt_record(&g_src_record) == 0, "setup: the environment's checkpoint record is stored");
    {
        const struct EnvCkptRecord* r = env_ckpt_find(SRC_PARTITION, SRC_INDEX);
        CHECK(r != 0, "the record is findable by (partition, index)");
        CHECK(r && env_migrate_payload_pages(r) == SRC_PAGES,
              "*** the payload is 6 pages: 4 region frames + 2 durable-extent pages ***");
        struct EnvCkptRecord noext = g_src_record;
        noext.state_sectors = 0;
        CHECK(env_migrate_payload_pages(&noext) == 4,
              "...and 4 without an extent -- the count comes from the record, not a constant");
        CHECK(env_migrate_payload_pages(0) == 0, "a NULL record carries no pages rather than crashing");
    }

    /* ═══ Phase A: the REAL sender as node A ═════════════════════════════ */
    printf("\n-- Phase A: the real sender --\n");
    peer_reset();
    uint64_t oversize_before = dspp_tx_oversize_dropped;
    int rc = env_migrate_send(SRC_PARTITION, SRC_INDEX, DST_NODE);
    CHECK(rc == 0, "*** env_migrate_send() reports a fully-confirmed transfer ***");
    CHECK(env_migrate_last_status() == ENV_MIGRATE_OK, "...and last_status agrees, by name");
    CHECK(begin_ack_count == 1, "the destination acknowledged the BEGIN exactly once");
    CHECK(page_ack_count == (int)(SRC_PAGES * DSPP_MIGRATE_FRAGS_PER_PAGE),
          "and acknowledged every fragment of every page");
    CHECK(last_tx_role == NIC_ROLE_CLUSTER,
          "DSPP left by the CLUSTER interface, not the management one");

    const int expect_frames = 1 + (int)(SRC_PAGES * DSPP_MIGRATE_FRAGS_PER_PAGE);
    CHECK(captured_frame_count == expect_frames,
          "*** one BEGIN plus one frame per fragment of each of the six pages ***");
    CHECK(dspp_tx_oversize_dropped == oversize_before,
          "*** nothing was refused for being over the link MTU ***");
    {
        int all_fit = 1;
        for (int i = 0; i < captured_frame_count; i++)
            if (captured_frame_len[i] > DSPP_LINK_MTU) all_fit = 0;
        CHECK(all_fit, "*** every captured frame is within the 1500-byte MTU, checked on the bytes handed to the driver ***");
    }

    /* Decode the captured BEGIN: the descriptor crossing WHOLE is the whole
     * design of this family (net/dspp.h), so it is read back off the wire. */
    {
        struct DSPPEnvMigrateBeginPacket* pkt =
            (struct DSPPEnvMigrateBeginPacket*)(captured_frame[0] + ETH_HDR_LEN);
        struct DSPPEnvMigrateHeader* h = &pkt->header;
        CHECK(h->magic == DSPP_MIGRATE_MAGIC, "captured frame 0: real DSPP_MIGRATE_MAGIC on the wire");
        CHECK(h->opcode == DSPP_MIGRATE_ENV_BEGIN_REQ, "captured frame 0: the real ENV BEGIN_REQ opcode (18)");
        CHECK(h->node_dest_id == DST_NODE, "captured frame 0: addressed to node 2, the destination passed in");
        CHECK(h->node_source_id == 1, "captured frame 0: stamped with node A's own id");
        CHECK(h->partition_id == SRC_PARTITION && h->env_index == SRC_INDEX,
              "captured frame 0: carries the environment's real (partition, index) identity");
        CHECK(h->total_pages == SRC_PAGES, "captured frame 0: declares the 6 pages both sides will number by");
        CHECK(h->record_len == (uint32_t)sizeof(struct EnvCkptRecord),
              "captured frame 0: the descriptor is the record's exact size -- whole, not truncated");
        CHECK(h->transfer_id == (((uint64_t)SRC_PARTITION << 32) | SRC_INDEX),
              "captured frame 0: transfer_id is the partition-scoped identity the family uses");
        CHECK(memcmp(pkt->record, &g_src_record, sizeof(g_src_record)) == 0,
              "*** the checkpoint descriptor on the wire is byte-for-byte the record in node A's table ***");
    }

    /* Decode one fragment's real bytes, first and last of the transfer. */
    {
        struct DSPPEnvMigratePagePacket* p =
            (struct DSPPEnvMigratePagePacket*)(captured_frame[1] + ETH_HDR_LEN);
        CHECK(p->header.opcode == DSPP_MIGRATE_ENV_PAGE_REQ, "captured frame 1: the real ENV PAGE_REQ opcode (20)");
        CHECK(p->header.page_index == 0 && p->header.frag_index == 0,
              "captured frame 1: page 0's first fragment");
        CHECK(all_bytes(p->page_data, page_fill[0]) || p->page_data[0] == page_fill[0],
              "captured frame 1: carries page 0's real byte content");
        struct DSPPEnvMigratePagePacket* last =
            (struct DSPPEnvMigratePagePacket*)(captured_frame[expect_frames - 1] + ETH_HDR_LEN);
        CHECK(last->header.page_index == SRC_PAGES - 1, "the last captured frame belongs to the last page");
        CHECK(last->header.frag_index == DSPP_MIGRATE_FRAGS_PER_PAGE - 1,
              "...and is its final fragment");
        CHECK(last->page_data[0] == page_fill[5],
              "...carrying the durable extent's real byte content, i.e. the NVMe half crossed too");
    }

    /* The discipline this increment exists to keep: the source is never
     * touched. A transfer that returns 0 has still changed nothing here --
     * the caller decides, in increment 3, inside partition_migrate()'s
     * critical section. */
    CHECK(env_ckpt_find(SRC_PARTITION, SRC_INDEX) != 0,
          "*** the checkpoint record is still on node A after a successful send ***");
    CHECK(source_is_intact(),
          "*** and every payload page -- region and NVMe extent alike -- is byte-identical to what it was ***");
    CHECK(env_migrate_staging_count() == 0,
          "node A staged nothing locally: the sender never writes its own staging table");

    /* ═══ Phase B: node B replays the REAL bytes through the REAL path ═══ */
    printf("\n-- Phase B: the real receiver, replaying node A's bytes --\n");
    const int req_frames = captured_frame_count;
    g_fake_local_node_id = 2;
    env_migrate_reset();

    deliver(0);
    {
        const struct EnvMigrateStaging* st = env_migrate_recv_find(SRC_PARTITION, SRC_INDEX);
        CHECK(st != 0, "*** node B staged a slot upon receiving the real BEGIN_REQ ***");
        CHECK(st && st->record.partition_id == SRC_PARTITION && st->record.index == SRC_INDEX,
              "...holding the environment's real identity from the wire");
        CHECK(st && memcmp(&st->record, &g_src_record, sizeof(g_src_record)) == 0,
              "*** ...and the descriptor is byte-for-byte the record node A sent ***");
        CHECK(st && st->total_pages == SRC_PAGES, "...declaring the same 6 pages node A did");
        CHECK(st && st->pages_staged == 0 && st->complete == 0,
              "...with no page yet -- a slot is not complete until one has landed");
        CHECK(env_migrate_staging_count() == 1, "exactly one staging slot is live");
        CHECK(env_migrate_in_flight() == 1,
              "*** in_flight: a live staging slot defers failover's death detection ***");
    }

    /* Three of page 0's four fragments, and no more. The receiver's whole
     * job is to hand out NOTHING as complete until every fragment of every
     * page has landed, so the first three are the ones that prove it: an
     * assembly state that counted on arrival would already call this page
     * done with a quarter of it missing. */
    for (int i = 1; i <= 3; i++) deliver(i);
    {
        const struct EnvMigrateStaging* st = env_migrate_recv_find(SRC_PARTITION, SRC_INDEX);
        CHECK(st && st->pages_staged == 0,
              "*** three of page 0's four fragments stage NO page -- a quarter of a page is not a page ***");
        CHECK(st && st->complete == 0,
              "...and the slot is still not complete");
    }

    for (int i = 4; i < req_frames; i++) deliver(i);

    {
        const struct EnvMigrateStaging* st = env_migrate_recv_find(SRC_PARTITION, SRC_INDEX);
        CHECK(st && st->pages_staged == SRC_PAGES,
              "*** all six pages assembled from their fragments ***");
        CHECK(st && st->complete == 1, "*** the slot is COMPLETE -- every page the record names has landed ***");
        for (uint32_t p = 0; p < SRC_PAGES; p++) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "*** staged page %u is byte-for-byte node A's page %u (region and NVMe extent alike) ***",
                     p, p);
            CHECK(all_bytes(staged_page(p), page_fill[p]), msg);
        }
    }

    /* The receive path's OWN output: 1 BEGIN_ACK + one PAGE_ACK per fragment,
     * all status 0, all addressed back to node 1. Asserted rather than
     * assumed -- a receiver that staged correctly but never answered would
     * leave every future sender hanging on its stall bound. */
    {
        int ack_frames = captured_frame_count - req_frames;
        CHECK(ack_frames == expect_frames,
              "*** node B answered every frame it received: 1 BEGIN_ACK + 6 x 4 PAGE_ACKs ***");
        int all_ok = 1, all_addr = 0, all_fit = 1;
        struct DSPPEnvMigrateHeader* first =
            (struct DSPPEnvMigrateHeader*)(captured_frame[req_frames] + ETH_HDR_LEN);
        struct DSPPEnvMigrateHeader* lasth =
            (struct DSPPEnvMigrateHeader*)(captured_frame[captured_frame_count - 1] + ETH_HDR_LEN);
        for (int i = req_frames; i < captured_frame_count; i++) {
            struct DSPPEnvMigrateHeader* h =
                (struct DSPPEnvMigrateHeader*)(captured_frame[i] + ETH_HDR_LEN);
            if (h->magic != DSPP_MIGRATE_MAGIC) all_ok = 0;
            if (h->status != 0) all_ok = 0;
            if (h->node_dest_id == 1) all_addr = 1;
            if (captured_frame_len[i] > DSPP_LINK_MTU) all_fit = 0;
            if (h->opcode != DSPP_MIGRATE_ENV_BEGIN_ACK &&
                h->opcode != DSPP_MIGRATE_ENV_PAGE_ACK) all_ok = 0;
        }
        CHECK(all_ok, "*** every ACK node B transmitted is a real ENV ACK reporting success ***");
        CHECK(all_addr && first->opcode == DSPP_MIGRATE_ENV_BEGIN_ACK && first->node_dest_id == 1,
              "...the first is a BEGIN_ACK addressed back to node A");
        CHECK(lasth->opcode == DSPP_MIGRATE_ENV_PAGE_ACK && lasth->page_index == SRC_PAGES - 1,
              "...and the last is a PAGE_ACK for the final page");
        CHECK(all_fit, "...and every one fits the link MTU");
    }

    /* The staged slot is the restore's ONLY input (increment 4), so its
     * teardown has to be real too: dropping it returns the partition-charged
     * frames and leaves nothing behind. */
    CHECK(env_migrate_recv_drop(SRC_PARTITION, SRC_INDEX) == 0, "dropping the staged slot succeeds");
    CHECK(env_migrate_recv_drop(SRC_PARTITION, SRC_INDEX) == 1, "dropping it a second time reports there was none");
    CHECK(env_migrate_staging_count() == 0, "and the staging table is empty again -- no frame leak");
    CHECK(env_migrate_recv_find(SRC_PARTITION, SRC_INDEX) == 0, "a dropped slot is not handed out");
    /* The failover deferral's two other faces: the RESOLVED tail keeps the
     * gate closed after this slot's drop — the source's confirmation walk
     * outlives it — and the tail is BOUNDED — past it the gate re-arms so a
     * genuinely dead peer is still declared (without the tail the live
     * 2-node run's destination declared that live source DEAD and adopted,
     * 2026-10-13). The same tail is set again by the source's own disarm,
     * pinned in Phase H4. */
    CHECK(env_migrate_in_flight() == 1,
          "*** in_flight: a just-dropped slot keeps the resolved tail (the source's confirmation walk outlives it) ***");
    kernel_tick_counter += ENV_MIGRATE_RESOLVED_GRACE_TICKS + 1;
    CHECK(env_migrate_in_flight() == 0,
          "...and the grace is bounded: past the resolved tail the gate re-arms death detection");

    /* The idle-observer arm (docs/AeroSLS-Idle-Observer-Adoption-Finding-
     * v0.1.md): the intent covers the blocking PHASE itself — before any
     * staging slot exists on the destination and before any confirmation is
     * armed, the window in which every node's declarations about the busy
     * source were previously undeferred. It announces on the wire BEFORE
     * the block (a blocked node transmits nothing), and it is bounded like
     * every other face — on this node and on every peer that latched it. */
    env_migrate_intent_begin();
    CHECK(env_migrate_in_flight() == 1,
          "*** in_flight: announced intent covers the blocking phase itself (local gate) ***");
    {
        int saw_intent = 0;
        for (int i = 0; i < captured_frame_count && !saw_intent; i++) {
            const struct DSPPEnvMigrateHeader* h =
                (const struct DSPPEnvMigrateHeader*)(captured_frame[i] + ETH_HDR_LEN);
            if (h->opcode == DSPP_MIGRATE_ENV_INTENT) saw_intent = 1;
        }
        CHECK(saw_intent,
              "*** intent_begin puts the broadcast on the wire BEFORE the block ***");
    }
    kernel_tick_counter += ENV_MIGRATE_INTENT_GRACE_TICKS + 1;
    CHECK(env_migrate_in_flight() == 0,
          "...and the local intent is bounded: past its budget the gate re-arms");

    /* The peer half: what every OBSERVER latches from that broadcast. */
    g_hb_calls = 0; g_hb_last_node = 0;
    /* An UNWATCHED peer (a follower -- it has never been heard beating):
     * the announce latches, but the frame must NOT create a death-watch
     * entry. A watch entry for a quiet follower would be declared DEAD
     * exactly 300 ticks after its own release (the 2026-10-23 live find). */
    env_migrate_recv_intent(5, (uint32_t)ENV_MIGRATE_INTENT_GRACE_TICKS);
    CHECK(env_migrate_peer_in_flight(5) == 1,
          "*** peer intent: an announced budget latches on every node ***");
    CHECK(g_hb_calls == 0,
          "*** ...but a never-beaten (follower) announce creates NO death-watch entry: no heartbeat noted ***");
    fake_watch(5);   /* the observer has now heard node 5 beat, as it would a leader */
    env_migrate_recv_intent(5, (uint32_t)ENV_MIGRATE_INTENT_GRACE_TICKS);
    CHECK(g_hb_calls == 1 && g_hb_last_node == 5,
          "*** a WATCHED announcing peer's stamp IS refreshed by the announce (liveness evidence) ***");
    env_migrate_recv_intent(5, 0x7fffffffu);   /* a peer cannot shield itself past the cap */
    kernel_tick_counter += ENV_MIGRATE_INTENT_GRACE_TICKS + 1;
    CHECK(env_migrate_peer_in_flight(5) == 0,
          "...and it is bounded even against a hostile budget: recv_intent clamps to the cap");
    env_migrate_recv_intent(0, 100);           /* node 0 is the reserved sentinel */
    CHECK(env_migrate_peer_in_flight(0) == 0,
          "node 0 (the reserved sentinel) is never latched");
    {
        uint32_t keep = g_fake_local_node_id;
        g_fake_local_node_id = 9;
        env_migrate_recv_intent(9, 100);
        CHECK(env_migrate_peer_in_flight(9) == 0,
              "a node's own announce is never latched against itself");
        g_fake_local_node_id = keep;
    }

    /* The RELEASE (found live on a 3-node cluster, 2026-10-14): the announce
     * is one-shot and its budget covers the whole blocking phase, so a
     * create that finishes in seconds still blinded every observer for the
     * remaining ~300 s — a leader killed right after an env create could not
     * be declared DEAD, and the adoption guard's 240 s wait lost to the
     * leftover budget. intent_end() releases the local gate at once and
     * broadcasts the same announce with budget 0 — the only meaning 0 can
     * have, since recv_intent clamped every nonzero value — and a peer's
     * latch drops immediately instead of at the budget's end. */
    {
        int frames_before = captured_frame_count;
        env_migrate_intent_begin();
        env_migrate_intent_end();
        CHECK(env_migrate_in_flight() == 0,
              "*** intent_end releases the LOCAL gate immediately, not at the budget's end ***");
        int saw_release = 0;
        for (int i = frames_before; i < captured_frame_count && !saw_release; i++) {
            const struct DSPPEnvMigrateHeader* h =
                (const struct DSPPEnvMigrateHeader*)(captured_frame[i] + ETH_HDR_LEN);
            if (h->opcode == DSPP_MIGRATE_ENV_INTENT && h->record_len == 0)
                saw_release = 1;
        }
        CHECK(saw_release,
              "*** and the budget-0 release crosses the wire on the same broadcast ***");
    }
    env_migrate_recv_intent(6, (uint32_t)ENV_MIGRATE_INTENT_GRACE_TICKS);
    CHECK(env_migrate_peer_in_flight(6) == 1,
          "the peer latch stands before its owner releases it");
    fake_watch(6);   /* the observer has heard node 6 beat, as it would the leader */
    g_hb_calls = 0; g_hb_last_node = 0; g_hb_last_now = 0;
    env_migrate_recv_intent(6, 0);
    CHECK(env_migrate_peer_in_flight(6) == 0,
          "*** recv_intent's budget-0 release drops a peer's latch at once -- death detection is re-armed ***");
    /* The release-vs-heartbeat race (found live 2026-10-23 on a 3-node
     * cluster, env_migrate_confirm_live): the source's create blocked its
     * sweep for 435 ticks, then intent_end() fired the release. The release
     * dropped the latch but did NOT refresh last_seen_tick, so the very next
     * failover_tick still saw 435 ticks of silence — now UNdeferred — and
     * declared the just-resurrected source DEAD, adopting its partitions
     * mid-create. The release is the source's own frame: it is proof of
     * life, so it must refresh the stamp the same way the announce does. */
    CHECK(g_hb_calls == 1 && g_hb_last_node == 6,
          "*** the RELEASE is proof of life too: it refreshes the observer's last-seen stamp ***");
    CHECK(g_hb_last_now == kernel_tick_counter,
          "...stamped with the release's own tick, so the silence the create accrued is forgiven at once");
    env_migrate_recv_intent(6, 0);
    CHECK(env_migrate_peer_in_flight(6) == 0,
          "a release for a node with no latch is a no-op, not an error");
    {
        /* Same asymmetry as the announce: an unwatched (never-beaten) peer's
         * release drops a latch that never existed and creates no watch entry. */
        g_hb_calls = 0;
        env_migrate_recv_intent(7, 0);
        CHECK(env_migrate_peer_in_flight(7) == 0 && g_hb_calls == 0,
              "*** an unwatched peer's release is a pure no-op: no latch, no death-watch entry ***");
    }
    {
        /* The release is gated on the local stamp: the sweep's restore loop
         * calls end() after EVERY attempt, and a period with no begin must
         * transmit nothing — one broadcast per phase, not one per sweep. */
        int frames_before = captured_frame_count;
        env_migrate_intent_end();
        CHECK(captured_frame_count == frames_before,
              "*** end() without a begin transmits nothing (the sweep's every-period call stays quiet) ***");
        env_migrate_intent_begin();
        env_migrate_intent_end();
        frames_before = captured_frame_count;
        env_migrate_intent_end();
        CHECK(captured_frame_count == frames_before,
              "a second end() after a release is a no-op — the release is one-shot like the announce");
    }
    env_migrate_reset();
    CHECK(env_migrate_peer_in_flight(5) == 0 && env_migrate_in_flight() == 0,
          "reset clears both halves of the intent state");

    /* ═══ Phase C: refusals and retransmission, against the real sender ══ */
    printf("\n-- Phase C: loss and refusal leave the source alone --\n");
    g_fake_local_node_id = 1;
    env_migrate_reset();
    /* Each scenario resets the peer FIRST and then arms its knobs: the
     * knobs live in the peer, so a reset after arming would silently model
     * a perfect destination (which is exactly how this block first failed
     * against correct code). */
    #define SEND_NOW() do {                                             \
        captured_frame_count = 0;                                       \
        rc = env_migrate_send(SRC_PARTITION, SRC_INDEX, DST_NODE);      \
    } while (0)

    /* 1. A lost BEGIN is retried and the transfer still completes. */
    {
        peer_reset();
        begin_loss_times = 1;
        SEND_NOW();
        CHECK(begin_loss_applied == 1, "the first BEGIN really was swallowed");
        CHECK(begin_ack_count == 1, "the retry was acknowledged");
        CHECK(rc == 0, "*** a transfer whose BEGIN needed retrying still completes ***");
        CHECK(captured_frame_count == 1 + expect_frames,
              "*** two BEGINs plus the six pages' fragments -- the retry cost one frame, not a restart ***");
    }

    /* 2. A lost FRAGMENT is resent -- only that fragment -- and the transfer
     * still completes. The single most important property: loss is
     * survivable, and the retransmit is per-fragment rather than per-page. */
    {
        peer_reset();
        loss_page = 0; loss_frag = 2; loss_times = 1;
        SEND_NOW();
        CHECK(loss_applied == 1, "the destination really did swallow one fragment");
        CHECK(rc == 0, "*** the transfer COMPLETED despite the loss ***");
        CHECK(captured_frame_count == expect_frames + 1,
              "*** exactly ONE extra frame -- the retransmit is per-fragment, not per-page ***");
        CHECK(captured_frame_count < expect_frames + (int)DSPP_MIGRATE_FRAGS_PER_PAGE,
              "...definitively fewer than resending the whole page would take");
        CHECK(source_is_intact(), "and the source is untouched by a lossy-but-successful transfer");
    }

    /* 3. A destination that never acknowledges the BEGIN: abandon, after the
     * full budget, with the source exactly as it was. */
    {
        peer_reset();
        begin_loss_times = 1000000;
        SEND_NOW();
        CHECK(rc == ENV_MIGRATE_REFUSE_BEGIN, "*** an unacknowledged BEGIN is refused by name ***");
        CHECK(env_migrate_last_status() == ENV_MIGRATE_REFUSE_BEGIN,
              "...and the status says WHICH gate said no, not just that one did");
        CHECK(captured_frame_count == (int)DSPP_MIGRATE_MAX_ATTEMPTS,
              "*** and the BEGIN was RETRANSMITTED the full budget of attempts ***");
        CHECK(env_ckpt_find(SRC_PARTITION, SRC_INDEX) != 0, "the checkpoint record survived");
        CHECK(source_is_intact(), "*** the source is untouched -- no ownership without data ***");
        CHECK(env_migrate_staging_count() == 0, "nothing was staged on this node");
    }

    /* 4. A REFUSAL is not retried: a non-zero ACK status means the
     * destination cannot take this, and resending an identical request
     * cannot change the answer. */
    {
        peer_reset();
        nack_begin = 1;
        SEND_NOW();
        nack_begin = 0;
        CHECK(rc == ENV_MIGRATE_REFUSE_BEGIN, "a refused BEGIN is reported as refused");
        CHECK(captured_frame_count <= 1, "*** a refused BEGIN is NOT retransmitted -- exactly one frame ***");
        CHECK(source_is_intact(), "...and the source is intact");
    }

    /* 5. A page the destination refuses mid-transfer. */
    {
        peer_reset();
        nack_page = 1;
        SEND_NOW();
        nack_page = 0;
        CHECK(rc == ENV_MIGRATE_REFUSE_PAGE, "*** a refused page abandons the transfer by name ***");
        CHECK(captured_frame_count == 1 + (int)DSPP_MIGRATE_FRAGS_PER_PAGE,
              "*** the BEGIN was acknowledged and page 0 was sent once -- no retransmit of a refusal ***");
        CHECK(env_ckpt_find(SRC_PARTITION, SRC_INDEX) != 0 && source_is_intact(),
              "...and the source is untouched: the other five pages never moved and neither did this one");
    }

    /* 6. The clean case did not get slower: no speculative retransmission
     * when nothing is lost. */
    {
        peer_reset();
        SEND_NOW();
        CHECK(rc == 0, "a lossless transfer completes");
        CHECK(captured_frame_count == expect_frames,
              "*** a clean send transmits exactly BEGIN + pages x fragments -- no speculative retransmission ***");
    }
    #undef SEND_NOW

    /* ═══ Phase D: the sender's identity guards cost no frames ═══════════ */
    printf("\n-- Phase D: the sender's own guards --\n");
    {
        peer_reset(); captured_frame_count = 0;

        CHECK(env_migrate_send(PARTITION_SYSTEM, 0, DST_NODE) == ENV_MIGRATE_REFUSE_SYSTEM,
              "PARTITION_SYSTEM never migrates (partition_migrate's own rule, inherited)");
        CHECK(env_migrate_send(SRC_PARTITION, SRC_INDEX, 0) == ENV_MIGRATE_REFUSE_DEST,
              "node 0 is the 'uninitialized' sentinel, not a destination");
        CHECK(env_migrate_send(SRC_PARTITION, SRC_INDEX, 1) == ENV_MIGRATE_REFUSE_DEST,
              "...and neither is this node itself");
        CHECK(env_migrate_send(SRC_PARTITION, 99, DST_NODE) == ENV_MIGRATE_REFUSE_NO_RECORD,
              "an environment with no checkpoint record has nothing to send");
        CHECK(captured_frame_count == 0,
              "*** every refusal above cost ZERO frames -- they are checked before anything is emitted ***");

        struct EnvCkptRecord unq = g_src_record;
        unq.flags = 0;   /* captured without freezing its partition */
        CHECK(env_ckpt_record(&unq) == 0, "setup: an un-quiesced record is storable (it is the capture's business, not the gate's)");
        CHECK(env_migrate_send(SRC_PARTITION, SRC_INDEX, DST_NODE) == ENV_MIGRATE_REFUSE_UNQUIESCED,
              "*** a record captured without a quiesce describes a moving target and is refused ***");
        CHECK(captured_frame_count == 0, "...before a single byte went on the wire");
        CHECK(env_ckpt_record(&g_src_record) == 0, "teardown: the quiesced record is restored");
    }

    /* ═══ Phase E: the receiver's refusals, through the REAL dispatcher ══ */
    printf("\n-- Phase E: the receiver refuses what it must --\n");
    g_fake_local_node_id = 2;
    env_migrate_reset();
    peer_reset();
    captured_frame_count = 0;

    /* Hand-built BEGIN for a fresh environment, used as the base for each
     * refusal below. The record blob is node A's real one. */
    #define MAKE_BEGIN(part, idx, tid, total, rec, reclen) do {                     \
        struct DSPPEnvMigrateBeginPacket bp;                                        \
        memset(&bp, 0, sizeof bp);                                                  \
        bp.header.magic          = DSPP_MIGRATE_MAGIC;                              \
        bp.header.opcode         = DSPP_MIGRATE_ENV_BEGIN_REQ;                      \
        bp.header.node_source_id = 1;                                               \
        bp.header.node_dest_id   = 2;                                               \
        bp.header.transfer_id    = (tid);                                           \
        bp.header.partition_id   = (part);                                           \
        bp.header.env_index      = (idx);                                            \
        bp.header.total_pages    = (total);                                          \
        bp.header.record_len     = (reclen);                                         \
        memcpy(bp.record, (rec), (reclen));                                          \
        dspp_rx_dispatch(&bp, (uint16_t)sizeof(bp));                                 \
    } while (0)

    /* ── E1: a declared page count that disagrees with the record ────────
     * Both sides number pages from the same record; a total that disagrees
     * means the two kernels would disagree about what "every page landed"
     * means. Refused rather than reconciled -- and refused BEFORE a slot is
     * allocated, so the refusal costs nothing to absorb. */
    {
        int before = captured_frame_count;
        MAKE_BEGIN(91, 0, 0x9100, SRC_PAGES - 1, &g_src_record, sizeof(g_src_record));
        CHECK(env_migrate_recv_find(91, 0) == 0,
              "*** a BEGIN whose page count disagrees with its own record stages NOTHING ***");
        CHECK(captured_frame_count == before + 1 &&
              ((struct DSPPEnvMigrateHeader*)(captured_frame[before] + ETH_HDR_LEN))->status != 0,
              "...and it was answered with a non-zero status, not silence");
    }

    /* ── E2: a fragment for a transfer nobody announced ────────────────── */
    {
        int before = captured_frame_count;
        struct DSPPEnvMigratePagePacket fp;
        memset(&fp, 0, sizeof fp);
        fp.header.magic          = DSPP_MIGRATE_MAGIC;
        fp.header.opcode         = DSPP_MIGRATE_ENV_PAGE_REQ;
        fp.header.node_source_id = 1;
        fp.header.node_dest_id   = 2;
        fp.header.transfer_id    = 0xDEADBEEFull;   /* no such transfer */
        fp.header.partition_id   = 91;
        fp.header.page_index     = 0;
        fp.header.frag_index     = 0;
        memset(fp.page_data, 0x5C, DSPP_MIGRATE_FRAG_BYTES);
        dspp_rx_dispatch(&fp, (uint16_t)sizeof(fp));
        CHECK(env_migrate_staging_count() == 0,
              "*** a fragment naming no staged transfer is refused, not absorbed into some other environment's slot ***");
        CHECK(captured_frame_count == before + 1 &&
              ((struct DSPPEnvMigrateHeader*)(captured_frame[before] + ETH_HDR_LEN))->status != 0,
              "...with a non-zero PAGE_ACK status");
    }

    /* ── E3: the happy path here, so the refusals above are not vacuous ──
     * A guard asserting only "something was refused" passes against an
     * implementation that refuses everything. */
    {
        MAKE_BEGIN(91, 0, 0x9100, SRC_PAGES, &g_src_record, sizeof(g_src_record));
        CHECK(env_migrate_recv_find(91, 0) != 0, "the same BEGIN with a matching count IS accepted");
        const struct EnvMigrateStaging* st = env_migrate_recv_find(91, 0);
        CHECK(st && st->record.env_id == g_src_record.env_id, "...holding the real record");
        uint64_t tid = 0x9100;
        for (uint32_t p = 0; p < SRC_PAGES; p++) {
            for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++) {
                struct DSPPEnvMigratePagePacket fp;
                memset(&fp, 0, sizeof fp);
                fp.header.magic          = DSPP_MIGRATE_MAGIC;
                fp.header.opcode         = DSPP_MIGRATE_ENV_PAGE_REQ;
                fp.header.node_source_id = 1;
                fp.header.node_dest_id   = 2;
                fp.header.transfer_id    = tid;
                fp.header.partition_id   = 91;
                fp.header.page_index     = p;
                fp.header.frag_index     = f;
                memset(fp.page_data, page_fill[p], DSPP_MIGRATE_FRAG_BYTES);
                dspp_rx_dispatch(&fp, (uint16_t)sizeof(fp));
            }
        }
        st = env_migrate_recv_find(91, 0);
        CHECK(st && st->complete == 1 && st->pages_staged == SRC_PAGES,
              "*** and it completes once every fragment of every page lands (so E1/E2 are real refusals) ***");
    }

    /* ── E4: a duplicate BEGIN must not clobber the slot in flight ───────
     * A retransmitted BEGIN for an environment already being staged is the
     * normal case on a lossy link. Absorbing it would reset pages_staged and
     * free frames a restore may be about to read. */
    {
        struct EnvCkptRecord other = g_src_record;
        other.env_id = 0xBAD0BADu;
        MAKE_BEGIN(91, 0, 0x9100, SRC_PAGES, &other, sizeof(other));
        const struct EnvMigrateStaging* st = env_migrate_recv_find(91, 0);
        CHECK(st && st->record.env_id == g_src_record.env_id,
              "*** a second BEGIN for an environment already staged is REFUSED, not absorbed ***");
        CHECK(st && st->complete == 1 && st->pages_staged == SRC_PAGES,
              "...and the slot in flight was not reset -- its pages are all still accounted for");
        CHECK(env_migrate_staging_count() == 1, "...with still exactly one slot live");
    }

    /* ── E4b: the same transfer re-asked IS absorbed (answered ready) ────
     * transfer_id derives from the identity, so a re-BEGIN after a lost
     * ACK carries the same id and the same record: that is the SAME
     * transfer asking again, and refusing it breaks the retransmission
     * model exactly when a lossy link needs it (observed live 2026-10-13:
     * a late ACK on a freshly-booted destination turned into rc=5 and a
     * whole healthy migrate abandoned mid-send). The slot must not be
     * touched: E4's refusal protects the bytes, this pin protects the
     * retry. */
    {
        MAKE_BEGIN(91, 0, 0x9100, SRC_PAGES, &g_src_record, sizeof(g_src_record));
        const struct DSPPEnvMigrateHeader* ack =
            (const struct DSPPEnvMigrateHeader*)(captured_frame[captured_frame_count - 1]
                                                 + ETH_HDR_LEN);
        CHECK(ack->opcode == DSPP_MIGRATE_ENV_BEGIN_ACK && ack->status == 0,
              "*** the same transfer's re-BEGIN is answered READY (ACK status 0), not refused ***");
        const struct EnvMigrateStaging* st = env_migrate_recv_find(91, 0);
        CHECK(st && st->record.env_id == g_src_record.env_id,
              "...and the staged record is the one that stands");
        CHECK(st && st->complete == 1 && st->pages_staged == SRC_PAGES,
              "...and the staged slot was not touched -- its pages are all still accounted for");
        CHECK(env_migrate_staging_count() == 1, "...with still exactly one slot live");
    }

    /* ── E5: a frame addressed to another node is ignored, not misapplied ─
     * No L2 addressing for a specific node exists, so node_dest_id is the
     * whole filter -- the same self-filter as every family on this magic. */
    {
        int before_active = captured_frame_count;
        struct DSPPEnvMigrateBeginPacket bp;
        memset(&bp, 0, sizeof bp);
        bp.header.magic          = DSPP_MIGRATE_MAGIC;
        bp.header.opcode         = DSPP_MIGRATE_ENV_BEGIN_REQ;
        bp.header.node_source_id = 1;
        bp.header.node_dest_id   = 99;   /* not node 2 (this node's identity) */
        bp.header.transfer_id    = 0x9900;
        bp.header.partition_id   = 93;
        bp.header.env_index      = 0;
        bp.header.total_pages    = 0;
        bp.header.record_len     = 0;
        dspp_rx_dispatch(&bp, (uint16_t)sizeof(bp));
        CHECK(env_migrate_recv_find(93, 0) == 0,
              "a BEGIN addressed to a different node stages nothing here");
        CHECK(captured_frame_count == before_active,
              "...and no ACK was transmitted for a frame this node correctly ignored");
    }

    /* ── E6: an environment with no payload is complete at the BEGIN ─────
     * With no regions and no extent its whole payload IS the record that
     * crossed, and waiting for a page that will never be sent would stage a
     * slot no restore could ever read. Driven at the wire entry rather than
     * through env_ckpt_record(), because P1a's gate requires all three
     * regions with at least one frame each -- a record this shape cannot
     * enter the table through the create path, but the receive side must
     * still answer it correctly if it ever arrives. */
    {
        struct EnvCkptRecord bare;
        memset(&bare, 0, sizeof bare);
        bare.magic        = ENV_CKPT_REC_MAGIC;
        bare.version      = ENV_CKPT_REC_VERSION;
        bare.size         = (uint32_t)sizeof(struct EnvCkptRecord);
        bare.partition_id = 94;
        bare.index        = 0;
        bare.flags        = ENV_CKPT_FLAG_QUIESCED;
        bare.n_regions    = 0;
        bare.n_tasks      = 0;
        MAKE_BEGIN(94, 0, 0x9400, 0, &bare, sizeof(bare));
        const struct EnvMigrateStaging* st = env_migrate_recv_find(94, 0);
        CHECK(st != 0, "*** an environment with no payload pages is staged at its BEGIN ***");
        CHECK(st && st->complete == 1 && st->total_pages == 0,
              "*** ...and is COMPLETE immediately -- the record WAS the whole transfer ***");
    }

    /* ── E7: a record blob of the wrong length is refused ────────────────
     * The descriptor crosses whole by design; a record_len that is not the
     * struct's exact size means a truncated or foreign descriptor, and a
     * truncated identity is worse than none. */
    {
        env_migrate_reset();
        int before = captured_frame_count;
        MAKE_BEGIN(95, 0, 0x9500, SRC_PAGES, &g_src_record,
                   (uint32_t)sizeof(struct EnvCkptRecord) - 1u);
        CHECK(env_migrate_recv_find(95, 0) == 0,
              "*** a record blob whose length is not the struct's exact size stages NOTHING ***");
        CHECK(captured_frame_count == before + 1 &&
              ((struct DSPPEnvMigrateHeader*)(captured_frame[before] + ETH_HDR_LEN))->status != 0,
              "...and it is answered as refused rather than read past its end");
    }
    #undef MAKE_BEGIN

    env_migrate_reset();
    CHECK(env_migrate_staging_count() == 0, "teardown: the staging table is empty");

    /* ═══ Phase F: the migration's environment step (P3 increment 3) ═══════
     * env_migrate_partition() is the ONE function kernel/partition.c Step 3c
     * calls: count the partition's environments, freeze them, send each, thaw
     * on every path out. Everything below runs against the REAL sender, the
     * REAL record layer and the REAL wire — the orchestration that decides
     * WHEN to call it (and to abort before the handoff when it falls short)
     * is tests/partition_migrate_phase6_host_test.c's Scenario 7.
     *
     * The records here are deliberately create-time records: flags 0, never
     * frozen by anybody. That is what makes the freeze observable — if the
     * step works, a record nobody ever quiesced is sendable while the step
     * runs and bit-for-bit unchanged after it. */
    printf("\n-- Phase F: the whole environment step --\n");
    {
        const uint32_t f_part = 78;
        uint32_t f_exp = 0, f_moved = 0;
        int f_before = 0;

        env_ckpt_reset();
        env_migrate_reset();
        peer_reset();
        g_fake_local_node_id = 1;   /* back to node A: Phase E left this process
                                     * wearing node B's identity, and a sender
                                     * that is its own destination refuses
                                     * (REFUSE_DEST) before it reads a byte */

        struct EnvCkptRecord fa = good_record(f_part, 0);
        struct EnvCkptRecord fb = good_record(f_part, 1);
        fa.flags = 0;              /* create-time: nothing has ever frozen it */
        fb.flags = 0;
        CHECK(env_ckpt_record(&fa) == 0, "setup: two environments registered in partition 78");
        CHECK(env_ckpt_record(&fb) == 0, "setup: ...neither of them ever quiesced");
        host_partition_define(f_part);

        /* ── F1: a partition nobody paused cannot be frozen ───────────────
         * The freeze only ever DESCRIBES a pause that exists; a helper that
         * could take the pause itself would be a second way to freeze a
         * tenant and its release would have to guess whose pause it was. */
        f_before = captured_frame_count;
        int f_rc = env_migrate_partition(f_part, DST_NODE, 0, &f_exp, &f_moved);
        CHECK(f_rc == ENV_MIGRATE_REFUSE_UNFROZEN,
              "*** a partition that is not held paused is REFUSED ***");
        CHECK(env_migrate_last_status() == ENV_MIGRATE_REFUSE_UNFROZEN,
              "...and the refusal is queryable by name, not just nonzero");
        CHECK(f_exp == 2 && f_moved == 0,
              "the counts are honest even on refusal: 2 expected, 0 moved");
        CHECK(captured_frame_count == f_before, "*** not one frame went on the wire ***");
        CHECK(env_ckpt_find(f_part, 0)->flags == 0 &&
              env_ckpt_find(f_part, 1)->flags == 0,
              "...and neither record was marked: a refusal touches nothing");

        /* ── F2: a record this boot has not replayed yet ──────────────────
         * Its region bases are last boot's placement and nothing is mapped
         * there now, so sending it would push whoever's bytes live there
         * across the wire under this environment's name. Refused BEFORE the
         * freeze, before a single page is read. */
        partition_pause(f_part);
        env_ckpt_restore_begin();
        f_before = captured_frame_count;
        f_rc = env_migrate_partition(f_part, DST_NODE, 0, &f_exp, &f_moved);
        CHECK(f_rc == ENV_MIGRATE_REFUSE_UNREPLAYED,
              "*** a record still awaiting this boot's replay is REFUSED: that environment "
              "does not exist on this node, so it has no payload to send ***");
        CHECK(f_exp == 2 && f_moved == 0, "2 expected, 0 moved, again honestly");
        CHECK(captured_frame_count == f_before, "and nothing was transmitted for it");
        CHECK(env_ckpt_find(f_part, 0)->flags == 0,
              "...the freeze never ran, because the gate that precedes it refused");
        {
            uint32_t pp, ii, ee;
            while (env_ckpt_restore_next(&pp, &ii, &ee)) env_ckpt_restore_settle(pp, ii);
            CHECK(env_ckpt_restore_pending() == 0, "teardown: the pending set is drained");
        }

        /* ── F3: the happy path — freeze, send both, thaw ───────────────── */
        peer_reset();
        env_migrate_reset();
        f_before = captured_frame_count;
        f_rc = env_migrate_partition(f_part, DST_NODE, 0, &f_exp, &f_moved);
        CHECK(f_rc == 0, "*** the paused partition's two environments both cross ***");
        CHECK(f_exp == 2 && f_moved == 2, "2 expected, 2 moved -- equal, which is the whole check");
        CHECK(captured_frame_count == f_before + 2 * expect_frames,
              "*** one BEGIN plus one frame per fragment for EACH environment: 2 x the six-page transfer ***");
        CHECK(env_ckpt_find(f_part, 0)->flags == 0 &&
              env_ckpt_find(f_part, 1)->flags == 0,
              "*** the freeze was released: both records are bit-for-bit what they were before it ***");
        CHECK(source_is_intact(),
              "and the source's pages are untouched -- the step reads and copies, it never frees");
        {
            struct DSPPEnvMigrateBeginPacket* bp =
                (struct DSPPEnvMigrateBeginPacket*)(captured_frame[f_before] + ETH_HDR_LEN);
            const struct EnvCkptRecord* wr = (const struct EnvCkptRecord*)bp->record;
            CHECK(bp->header.partition_id == f_part && bp->header.env_index == 0 &&
                  bp->header.total_pages == SRC_PAGES,
                  "the first BEGIN belongs to the right environment and declares its pages");
            CHECK((wr->flags & ENV_CKPT_FLAG_QUIESCED) != 0,
                  "*** the descriptor that CROSSED carries QUIESCED although the table had "
                  "neither -- the freeze is what makes it admissible to the destination's "
                  "restore rule ***");
            /* P3 increment 12, found live (2026-10-14): the migration's own
             * freeze used to cross as PARTITION_PAUSED too, and every
             * destination honored a pause nobody took -- the restored
             * environment was LIVE by the records and dead to the scheduler,
             * its console accepting input nobody would read. The pause here is
             * partition_migrate()'s own (operator_paused = 0), so the bit must
             * NOT cross; F3b is the witness that this is scoping, not
             * deletion. */
            CHECK((wr->flags & ENV_CKPT_FLAG_PARTITION_PAUSED) == 0,
                  "*** ...and NOT PARTITION_PAUSED: the migration's own freeze is nobody "
                  "else's business and must not travel as an operator's pause ***");
        }

        /* Replay both transfers through the REAL receive path as node 2. */
        {
            int f_end = captured_frame_count;
            g_fake_local_node_id = 2;
            env_migrate_reset();
            for (int i = f_before; i < f_end; i++) deliver(i);
            const struct EnvMigrateStaging* s0 = env_migrate_recv_find(f_part, 0);
            const struct EnvMigrateStaging* s1 = env_migrate_recv_find(f_part, 1);
            CHECK(s0 && s0->complete && s1 && s1->complete,
                  "*** the destination holds BOTH environments COMPLETE -- every page of both landed ***");
            CHECK(all_bytes(env_migrate_recv_page_ptr(f_part, 0, 0), page_fill[0]) &&
                  all_bytes(env_migrate_recv_page_ptr(f_part, 1, 3), page_fill[3]),
                  "...and each environment's staged pages are byte-for-byte the source's");
            g_fake_local_node_id = 1;
            env_migrate_reset();
        }

        /* ── F3b: the OPERATOR's pause DOES cross (increment 12's other half) ─
         * The witness that F3's flip is scoping, not deletion: when the pause
         * the caller holds PREDATES the move (partition_migrate()'s
         * was_paused_before_migrate), the staged record must carry
         * PARTITION_PAUSED -- an operator's intent is the one thing that
         * travels as "come back paused". */
        peer_reset();
        env_migrate_reset();
        f_before = captured_frame_count;
        f_rc = env_migrate_partition(f_part, DST_NODE, 1, &f_exp, &f_moved);
        CHECK(f_rc == 0 && f_exp == 2 && f_moved == 2,
              "the operator-paused migrate moves both environments like any other");
        {
            struct DSPPEnvMigrateBeginPacket* bp =
                (struct DSPPEnvMigrateBeginPacket*)(captured_frame[f_before] + ETH_HDR_LEN);
            const struct EnvCkptRecord* wr = (const struct EnvCkptRecord*)bp->record;
            CHECK((wr->flags & ENV_CKPT_FLAG_QUIESCED) != 0 &&
                  (wr->flags & ENV_CKPT_FLAG_PARTITION_PAUSED) != 0,
                  "*** an OPERATOR's pause crosses: QUIESCED|PARTITION_PAUSED when the "
                  "caller held the pause before the move ***");
        }
        CHECK(env_ckpt_find(f_part, 0)->flags == 0 &&
              env_ckpt_find(f_part, 1)->flags == 0,
              "...and the freeze was released here too");
        peer_reset();

        /* ── F4: a shortfall fails the step, and thaws anyway ─────────────
         * The destination refuses every BEGIN. Partial (1 of 2) is proven at
         * the orchestration level, where the abort is observable --
         * partition_migrate_phase6_host_test.c Scenario 7; here the refusal
         * must show up in BOTH counts and the flags must not outlive it. */
        peer_reset();
        nack_begin = 1;
        env_migrate_reset();
        f_before = captured_frame_count;
        f_rc = env_migrate_partition(f_part, DST_NODE, 0, &f_exp, &f_moved);
        CHECK(f_rc == ENV_MIGRATE_REFUSE_BEGIN,
              "*** a destination that refuses the BEGIN fails the whole step ***");
        CHECK(f_exp == 2 && f_moved == 0, "2 expected, 0 moved -- the shortfall is reported, not absorbed");
        CHECK(captured_frame_count == f_before + 1,
              "...after exactly ONE BEGIN: the second environment was not attempted, because "
              "the caller is going to abort");
        CHECK(env_ckpt_find(f_part, 0)->flags == 0 &&
              env_ckpt_find(f_part, 1)->flags == 0,
              "*** the freeze was released on the FAILURE path too -- the flags describe an "
              "instant and do not outlive it ***");
        CHECK(source_is_intact(), "...and the source is intact, so the caller may retry");
        nack_begin = 0;
        peer_reset();

        /* ── F5: 0 of 0 is success ──────────────────────────────────────── */
        host_partition_define(79);
        partition_pause(79);
        f_before = captured_frame_count;
        f_exp = 99; f_moved = 99;
        f_rc = env_migrate_partition(79, DST_NODE, 0, &f_exp, &f_moved);
        CHECK(f_rc == 0 && f_exp == 0 && f_moved == 0,
              "*** a partition holding no environments is 0 of 0 and SUCCEEDS -- the "
              "distinction that keeps 0 of N a failure ***");
        CHECK(captured_frame_count == f_before, "...and nothing at all was transmitted for it");

        /* ── F6: the control plane's own partition, by name ─────────────── */
        f_rc = env_migrate_partition(PARTITION_SYSTEM, DST_NODE, 0, &f_exp, &f_moved);
        CHECK(f_rc == ENV_MIGRATE_REFUSE_SYSTEM,
              "*** PARTITION_SYSTEM is refused BY NAME, before anything is counted or frozen ***");

        env_ckpt_reset();
        env_migrate_reset();
        partition_resume(f_part);
        CHECK(env_ckpt_count_live() == 0, "teardown: the record table is empty again");
        CHECK(env_migrate_staging_count() == 0, "teardown: and so is the staging table");
    }

    /* ═══ Phase G: the restore -- a staged slot becomes a live environment ═
     * P3 increment 4. The wire half (Phases A--E) fills a slot; this phase
     * proves the destination's second act against the REAL staging table,
     * the REAL record layer (env_ckpt.c) and the REAL NVMe fake: replay the
     * staged descriptor through the create path, verify what came back is
     * what crossed, pour the staged pages into the fresh environment's
     * regions and durable extent, then drop the slot.
     *
     * The order is the subject. Every case below pins one link of it: the
     * slot-retention rule (kept while the state is pre-attempt, dropped the
     * moment the environment exists), refusal over partial application (a
     * create that failed or came back different leaves NOTHING behind), the
     * placement rule (bytes that crossed for base X may only land at base X
     * -- the sidecars' heaps hold raw interior pointers into those regions,
     * exactly env_payload.h's rule, applied to wire-delivered bytes), and
     * the pour's byte fidelity (the destination holds the source's bytes,
     * extent included). */
    printf("\n-- Phase G: staged slot -> live environment (P3 increment 4) --\n");
    {
        const uint8_t fills[6] = G_FILLS;
        uint64_t tid;
        (void)tid;

        env_ckpt_reset();
        env_migrate_reset();
        host_partition_stubs_reset();
        host_partition_define(G_PART);
        g_fake_local_node_id = 2;   /* this process IS the destination now */
        g_reset_fakes();            /* owner = 2: this node owns the partition */
        g_fill_pages();

        /* ── G1: the happy path -- live, contents poured, slot dropped ──── */
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            staged.flags |= ENV_CKPT_FLAG_PARTITION_PAUSED;  /* the operator's intent, carried across */
            g_create_record = staged;                 /* placement matched */
            memset(g_live_r0, 0xAA, sizeof g_live_r0);
            memset(g_live_r1, 0xAA, sizeof g_live_r1);
            memset(g_live_r2, 0xAA, sizeof g_live_r2);
            {   /* the destination's durable extent, before the pour */
                uint8_t pg[4096]; memset(pg, 0xBB, 4096);
                nvme_write_sync(STATE_LBA + 0, pg);
                nvme_write_sync(STATE_LBA + 8, pg);
            }
            g_stage(G_PART, 0, &staged, 6);
            CHECK(env_migrate_staging_count() == 1, "setup: one complete slot staged");

            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_OK,
                  "*** a complete slot for an owned partition becomes a LIVE environment ***");
            CHECK(env_migrate_last_status() == ENV_MIGRATE_OK,
                  "...and the status latch says so by name");
            CHECK(g_pages_all(0, fills[0]) &&
                  memcmp(g_live_r0[0], g_pages[0], 4096) == 0 &&
                  memcmp(g_live_r0[1], g_pages[1], 4096) == 0 &&
                  memcmp(g_live_r1[0], g_pages[2], 4096) == 0 &&
                  memcmp(g_live_r2[0], g_pages[3], 4096) == 0,
                  "*** every region page the wire carried is byte-for-byte in the fresh "
                  "environment's regions ***");
            {
                uint8_t pg[4096];
                CHECK(nvme_read_sync(STATE_LBA + 0, pg) == 0 &&
                      memcmp(pg, g_pages[4], 4096) == 0 &&
                      nvme_read_sync(STATE_LBA + 8, pg) == 0 &&
                      memcmp(pg, g_pages[5], 4096) == 0,
                      "*** and the durable extent's pages landed at the record's LBA ***");
            }
            CHECK(env_migrate_staging_count() == 0,
                  "*** the staged slot is DROPPED after a successful pour -- the frames "
                  "go back; the bytes are the environment now, not a copy beside it ***");
            CHECK(env_ckpt_find(G_PART, 0) && env_ckpt_find(G_PART, 0)->env_id == 500,
                  "the live record is the FRESH environment's (create's env_id), not the "
                  "staged descriptor's stale one");
            CHECK(!env_ckpt_restore_pending_record(G_PART, 0),
                  "...and this boot does not still owe it a replay");
            CHECK(host_partition_paused[G_PART] == 1,
                  "*** the record recorded this partition as paused -- the restore left it "
                  "PAUSED: resume happens over here, by an operator, after the bytes land ***");
            CHECK(g_create_calls == 1 && g_destroy_calls == 0,
                  "exactly one create, no destroy: nothing was torn down to get here");
        }

        /* ── G1b: the image-window tail the BEGIN declared crosses too ───
         * The tail's page count is the slot's DECLARED total minus the
         * record's payload. It used to be derived from the record alone,
         * which made it identically ZERO on every restore — found live on
         * the 3-node suite (2026-10-24): "1344 region page(s), 0 image-
         * window page(s) and 256 extent page(s) poured" while the source had
         * just sent a real tail, so the tail was staged, never validated
         * against the fresh windows, never poured, and the count agreement
         * guarding both halves was never tested. This pins the tail's bytes
         * landing — and, because the sender numbers regions, extent, then
         * tail, that the extent still lands in NVMe rather than in the
         * sidecars' image frames. */
        {
            env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
            host_partition_define(G_PART); g_fake_local_node_id = 2; g_reset_fakes();
            g_fill_pages();
            memset(g_pages[6], 0xC1, 4096);        /* tail page 0 */
            memset(g_pages[7], 0xC2, 4096);        /* tail page 1 */
            memset(g_img_frame, 0xEE, sizeof g_img_frame);
            g_img_tail = 2;                        /* this node's fresh windows hold 2 */

            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;              /* placement matched */
            g_stage(G_PART, 0, &staged, 8);        /* 6 payload + 2 tail */
            const struct EnvMigrateStaging* st = env_migrate_recv_find(G_PART, 0);
            CHECK(st && st->complete && st->total_pages == 8,
                  "setup: a slot staged with the record's 6 payload pages PLUS a 2-page tail declared");

            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_OK,
                  "*** a declared tail crosses: a total past the record's payload restores cleanly ***");
            CHECK(memcmp(g_img_frame[0], g_pages[6], 4096) == 0 &&
                  memcmp(g_img_frame[1], g_pages[7], 4096) == 0,
                  "*** ...and the tail's bytes are in the fresh sidecars' image frames "
                  "(the count came from the SLOT, not from the record) ***");
            CHECK(memcmp(g_live_r0[0], g_pages[0], 4096) == 0 &&
                  memcmp(g_live_r0[1], g_pages[1], 4096) == 0 &&
                  memcmp(g_live_r1[0], g_pages[2], 4096) == 0 &&
                  memcmp(g_live_r2[0], g_pages[3], 4096) == 0,
                  "the region pages are still poured first, byte-for-byte");
            {
                uint8_t pg[4096];
                CHECK(nvme_read_sync(STATE_LBA + 0, pg) == 0 &&
                      memcmp(pg, g_pages[4], 4096) == 0 &&
                      nvme_read_sync(STATE_LBA + 8, pg) == 0 &&
                      memcmp(pg, g_pages[5], 4096) == 0,
                      "*** and the EXTENT still landed in NVMe: the pour reads regions, "
                      "extent, THEN tail — the sender's own numbering ***");
            }
            CHECK(env_migrate_staging_count() == 0,
                  "the slot — tail pages included — is dropped after the pour");
        }

        /* ── G1c: a tail THIS node's windows cannot reproduce is refused ─
         * The halves-agreement gate at 4b' only ever runs when a tail
         * actually crossed, so G1b is what makes it reachable; this is its
         * teeth: a wire count the fresh sidecars' windows cannot match names
         * both halves and refuses rather than pouring a half-environment. */
        {
            env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
            host_partition_define(G_PART); g_fake_local_node_id = 2; g_reset_fakes();
            g_fill_pages();
            memset(g_pages[6], 0xC1, 4096);
            memset(g_pages[7], 0xC2, 4096);
            g_img_tail = 0;                        /* nothing mapped here */

            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_stage(G_PART, 0, &staged, 8);        /* the wire still carries 8 */

            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_PLACEMENT,
                  "*** a wire tail this node's fresh windows cannot reproduce is REFUSED by name ***");
            CHECK(env_migrate_last_refusal()->a == 2,
                  "...with the WIRE's page count as the operand (a=2), not this node's 0");
            CHECK(env_migrate_staging_count() == 0,
                  "...and the slot is dropped: the environment exists as an empty replay, never a half-poured one");
            g_img_tail = 0;
        }

        /* ── G2: no slot, and an incomplete one ────────────────────────── */
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(G_PART); g_fake_local_node_id = 2; g_reset_fakes();
        {
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_NO_SLOT,
                  "*** no staged slot is REFUSED BY NAME, not guessed at ***");
            CHECK(env_migrate_last_refusal()->code == (uint32_t)rc,
                  "...and the refusal latch carries the code for a guard to assert");

            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            tid = ((uint64_t)G_PART << 32) | 0;
            (void)env_migrate_recv_begin(tid, G_PART, 0, 6, &staged,
                                         (uint32_t)sizeof(staged));
            rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_INCOMPLETE,
                  "*** a slot whose pages have not all landed is refused -- a partial "
                  "environment is not a restored one ***");
            CHECK(env_migrate_staging_count() == 1,
                  "...and the slot is KEPT: the bytes may still be arriving");
            CHECK(g_create_calls == 0, "...and no create was ever attempted for it");
            env_migrate_reset();
        }

        /* ── G3: the destination must OWN the partition ────────────────── */
        {
            g_fake_owner_node = 1;   /* the partition is served elsewhere */
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_NOT_OWNER &&
                  env_migrate_last_refusal()->a == 0,
                  "*** a follower that does not own the partition REFUSES to build an "
                  "environment in it -- the mirror image of the source's ownership rule ***");
            CHECK(env_migrate_staging_count() == 1 && g_create_calls == 0,
                  "the slot is kept (the announce may be in flight) and nothing was created");
            env_migrate_reset();
            g_fake_owner_node = 2;
        }

        /* ── G4: the boot replay's admissibility gate, verbatim ────────── */
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            staged.flags = 0;   /* captured while its partition ran -- a moving target */
            g_create_record = staged;
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_UNQUIESCED,
                  "*** an unquiesced descriptor is refused the same way the boot replay "
                  "refuses it: what crossed must be a consistent instant ***");
            CHECK(g_create_calls == 0, "...before any create");
            env_migrate_reset();
        }

        /* ── G5: a create that fails leaves NOTHING behind, and retries ── */
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            staged.flags |= ENV_CKPT_FLAG_PARTITION_PAUSED;
            g_create_record = staged;
            g_create_rc = -1;   /* the channel never answered */
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_CREATE &&
                  env_migrate_last_refusal()->b == 0,
                  "*** a create that never answers is refused, operand b=0 (no ENV_* "
                  "status arrived) ***");
            CHECK(env_ckpt_find(G_PART, 0) == 0,
                  "*** and the staged record it stored is DROPPED: refusal over partial "
                  "application -- nothing half-applied stays behind ***");
            CHECK(env_migrate_staging_count() == 1,
                  "the slot is KEPT: nothing was created, so the state is exactly "
                  "pre-attempt and the sweep may retry");

            g_create_rc = 0; g_create_status = ENV_ERR_INVAL;
            rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_CREATE &&
                  env_migrate_last_refusal()->b == (uint32_t)ENV_ERR_INVAL,
                  "...and an ENV_ERR_* answer carries its status in the operand");
            CHECK(env_ckpt_find(G_PART, 0) == 0, "record dropped on that path too");
            g_create_status = ENV_OK;
            env_migrate_reset();
        }

        /* ── G6: what came back must be what crossed ───────────────────── */
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            strcpy(g_create_record.tasks[0].name, "aerosls.posix.9");  /* a DIFFERENT env */
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_IDENTITY,
                  "*** a create that came back as a different environment is REFUSED ***");
            CHECK(g_destroy_calls == 1 && g_destroy_env_id == 500,
                  "*** and the environment the create made is ENDED -- a refused restore "
                  "leaves no half-restored environment behind ***");
            CHECK(env_ckpt_find(G_PART, 0) == 0 && env_migrate_staging_count() == 1,
                  "record gone, slot kept -- the state is pre-attempt again");
            env_migrate_reset();
        }

        /* ── G7: a return migration ends the stale instance first ────────
         * The source-teardown half has not landed (increment 3 named it), so
         * a node that already served this environment once still holds its
         * record. The staged bytes just crossed with ownership returned: the
         * local instance is the stale one. */
        {
            struct EnvCkptRecord stale =
                g_record(G_PART, 7, g_live_r0, g_live_r1, g_live_r2);
            stale.env_id = 42;
            g_destroy_calls = 0; g_destroy_env_id = 0;   /* G6's identity refusal also destroys */
            CHECK(env_ckpt_record(&stale) == 0, "setup: a stale live record from a prior life");
            struct EnvCkptRecord staged =
                g_record(G_PART, 7, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_stage(G_PART, 7, &staged, 6);
            int rc = env_migrate_restore(G_PART, 7);
            CHECK(rc == ENV_MIGRATE_OK,
                  "*** a return migration SUCCEEDS: the staged bytes are the newer truth ***");
            CHECK(g_destroy_calls == 1 && g_destroy_env_id == 42,
                  "*** the stale instance was ended by its own env_id BEFORE the rebuild ***");
            CHECK(env_ckpt_find(G_PART, 7) && env_ckpt_find(G_PART, 7)->env_id == 500,
                  "...and the record now describes the rebuilt environment");
        }

        /* ── G8: the placement rule -- bytes that crossed for base X land at X ─ */
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(G_PART); g_fake_local_node_id = 2; g_reset_fakes();
        g_fill_pages();
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record =
                g_record(G_PART, 0, g_other_r0, g_other_r1, g_other_r2); /* landed elsewhere */
            memset(g_live_r0, 0xAA, sizeof g_live_r0);
            memset(g_other_r0, 0xCC, sizeof g_other_r0);
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_PLACEMENT &&
                  env_migrate_last_refusal()->a == ENV_CKPT_REGION_POSIX_HEAP,
                  "*** an environment that landed OFF the staged bases refuses the pour, "
                  "operand a=the offending region's kind -- its sidecars' heaps hold raw "
                  "pointers into those regions ***");
            CHECK(memcmp(g_live_r0[0], g_pages[0], 4096) != 0 &&
                  memcmp(g_other_r0[0], g_pages[0], 4096) != 0,
                  "*** NOT ONE byte was written: refusal over partial application, at the "
                  "pour ***");
            CHECK(env_ckpt_find(G_PART, 0) != 0,
                  "the environment STANDS as the empty replay -- the boot pass's own answer");
            CHECK(env_migrate_staging_count() == 0,
                  "*** and the slot is DROPPED: staged bytes that can never be applied "
                  "here are frames held forever for nothing ***");
            env_ckpt_reset();
        }

        /* ── G9: the sidecars must be parked before their memory is poured ─ */
        env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(G_PART); g_reset_fakes();
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_parked = 0;
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_PARKED,
                  "*** sidecars that never reached the parked posture refuse the pour ***");
            CHECK(memcmp(g_live_r0[0], g_pages[0], 4096) != 0,
                  "not one byte written");
            CHECK(env_migrate_staging_count() == 0 && env_ckpt_find(G_PART, 0) != 0,
                  "slot dropped, environment stands");
            env_ckpt_reset();
        }

        /* ── G10: extent coordinates are NODE-LOCAL ────────────────────
         * The record's LBA is a fact about the SOURCE (measured live:
         * staged slot 5 = LBA 1124352, fresh slot 7 = LBA 1128448, both
         * 2048 sectors). The pour lands in THIS node's extent -- never the
         * record's slot number, which may hold another tenant's store here. */
        env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(G_PART); g_reset_fakes();
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_create_record.state_lba = STATE_LBA + 64;  /* this node's band slot differs */
            uint8_t pg[4096]; memset(pg, 0xBB, 4096);
            nvme_write_sync(STATE_LBA + 0, pg);  /* the record's LBA -- another store here */
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_OK,
                  "*** a fresh extent at THIS node's own coordinates restores -- the "
                  "source's slot number is not a fact about this node ***");
            nvme_read_sync(STATE_LBA + 0, pg);
            {
                int bb = 1; for (uint32_t b = 0; b < 4096; b++) if (pg[b] != 0xBB) bb = 0;
                CHECK(bb, "...and the record's LBA -- another store's extent here -- "
                          "holds not one poured byte");
            }
            CHECK(nvme_read_sync(STATE_LBA + 64 + 0, pg) == 0 &&
                  memcmp(pg, g_pages[4], 4096) == 0 &&
                  nvme_read_sync(STATE_LBA + 64 + 8, pg) == 0 &&
                  memcmp(pg, g_pages[5], 4096) == 0,
                  "*** the staged extent pages landed in THIS node's extent, byte-for-byte ***");
            CHECK(env_migrate_staging_count() == 0 && env_ckpt_find(G_PART, 0) != 0,
                  "slot dropped, environment stands");
            env_ckpt_reset();
        }

        /* ── G10b: no durable extent HERE refuses by name ─────────────────
         * The gate that replaced cross-node LBA equality: a destination
         * whose store never became durable (the record layer's "0 until
         * then") has nothing to pour into, and the staged extent must not
         * land in RAM's fiction. */
        env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(G_PART); g_reset_fakes();
        {
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_create_record.state_lba     = 0;
            g_create_record.state_sectors = 0;
            g_create_record.state_bytes   = 0;
            uint8_t pg[4096]; memset(pg, 0xBB, 4096);
            nvme_write_sync(STATE_LBA + 0, pg);
            g_stage(G_PART, 0, &staged, 6);
            int rc = env_migrate_restore(G_PART, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_EXTENT,
                  "*** a destination with no durable extent of its own refuses the staged "
                  "pages by name -- 0-until-then is a gate, not a pour target ***");
            CHECK(env_migrate_last_refusal() &&
                  env_migrate_last_refusal()->a == (uint32_t)STATE_SECTORS,
                  "...operand a = the staged extent's size that had nowhere to land");
            nvme_read_sync(STATE_LBA + 0, pg);
            {
                int bb = 1; for (uint32_t b = 0; b < 4096; b++) if (pg[b] != 0xBB) bb = 0;
                CHECK(bb, "...and not one extent byte was written");
            }
            CHECK(env_migrate_staging_count() == 0 && env_ckpt_find(G_PART, 0) != 0,
                  "slot dropped, environment stands");
            env_ckpt_reset();
        }

        /* ── G11: the control plane, by name ───────────────────────────── */
        env_migrate_reset(); host_partition_stubs_reset(); g_reset_fakes();
        {
            struct EnvCkptRecord staged =
                g_record(PARTITION_SYSTEM, 0, g_live_r0, g_live_r1, g_live_r2);
            staged.partition_id = PARTITION_SYSTEM;
            g_stage(PARTITION_SYSTEM, 0, &staged, 6);
            int rc = env_migrate_restore(PARTITION_SYSTEM, 0);
            CHECK(rc == ENV_MIGRATE_REFUSE_SYSTEM,
                  "*** PARTITION_SYSTEM is refused BY NAME on the restore side too ***");
            env_migrate_reset();
        }

        /* ── G12: the sweep -- env_migrate_tick() ────────────────────────
         * The wire does not restore anything by itself; the BSP sweep's
         * driver turns complete slots into environments, on a period, with
         * a bounded attempt count. */
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(G_PART); g_fake_local_node_id = 2; g_reset_fakes();
        g_fill_pages();
        {
            uint64_t now = 1000;
            struct EnvCkptRecord staged =
                g_record(G_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = staged;
            g_stage(G_PART, 0, &staged, 6);

            CHECK(env_migrate_tick(now) == 1,
                  "*** the sweep restores the complete, owned slot: 1 environment live ***");
            CHECK(env_migrate_staging_count() == 0 && env_ckpt_find(G_PART, 0) != 0,
                  "...slot gone, record live");
            CHECK(env_migrate_tick(now + 500) == 0,
                  "the period bounds it: a second sweep inside the period restores nothing");

            /* An incomplete slot is never attempted by the sweep. */
            {
                struct EnvCkptRecord s2 =
                    g_record(G_PART, 1, g_live_r0, g_live_r1, g_live_r2);
                uint64_t t2 = ((uint64_t)G_PART << 32) | 1;
                (void)env_migrate_recv_begin(t2, G_PART, 1, 6, &s2,
                                             (uint32_t)sizeof(s2));
                int before = g_create_calls;
                CHECK(env_migrate_tick(now + 2000) == 0,
                      "an INCOMPLETE slot is never touched by the sweep");
                CHECK(g_create_calls == before, "...no create was attempted for it");
                env_migrate_reset();
            }

            /* The attempts cap: five tries, then the sweep stops -- forever. */
            env_ckpt_reset();
            {
                struct EnvCkptRecord s3 =
                    g_record(G_PART, 2, g_live_r0, g_live_r1, g_live_r2);
                g_create_record = s3;
                g_create_rc = -1;
                g_stage(G_PART, 2, &s3, 6);
                uint64_t t = now + 4000;
                int cap_before = g_create_calls;
                for (int k = 0; k < 12; k++) { (void)env_migrate_tick(t); t += 2000; }
                CHECK(g_create_calls - cap_before == 5,
                      "*** a create that can never succeed is tried exactly 5 times, then "
                      "the sweep stops -- no infinite retry, no silent frame leak ***");
                CHECK(env_migrate_staging_count() == 1,
                      "...and the bytes stay countable rather than vanishing");
                env_migrate_reset();
                g_create_rc = 0;
            }

            /* A slot whose partition is gone: dropped, frames returned. */
            {
                host_partition_define(G_PART + 1);
                struct EnvCkptRecord s4 =
                    g_record(G_PART + 1, 0, g_live_r0, g_live_r1, g_live_r2);
                g_create_record = s4;
                g_stage(G_PART + 1, 0, &s4, 6);
                CHECK(env_migrate_staging_count() == 1, "setup: a slot for partition 92");
                host_partition_destroy(G_PART + 1);
                (void)env_migrate_tick(now + 30000);
                CHECK(env_migrate_staging_count() == 0,
                      "*** the sweep drops the slot of a partition that no longer exists -- "
                      "its frames go back with the tenant they were charged to ***");
            }

            /* A slot owned by ANOTHER node waits -- that node may be us soon. */
            {
                struct EnvCkptRecord s5 =
                    g_record(G_PART, 3, g_live_r0, g_live_r1, g_live_r2);
                g_create_record = s5;
                g_stage(G_PART, 3, &s5, 6);
                g_fake_owner_node = 1;
                int before = g_create_calls;
                CHECK(env_migrate_tick(now + 33000) == 0 && g_create_calls == before,
                      "a slot for a partition owned elsewhere is waited on, not restored");
                CHECK(env_migrate_staging_count() == 1, "...and kept");
                g_fake_owner_node = 2;
                env_migrate_reset();
            }
        }

        env_ckpt_reset();
        env_migrate_reset();
        host_partition_stubs_reset();
        g_fake_local_node_id = 1;
        CHECK(env_ckpt_count_live() == 0, "teardown: the record table is empty again");
        CHECK(env_migrate_staging_count() == 0, "teardown: and so is the staging table");
    }

    /* ═══ Phase H: the destination's word, then the source's teardown ═══
     * P3 increment 5. The rule this whole increment exists for: no record
     * may describe an environment that moved. The DESTINATION half first
     * proves what its word is grounded in; the SOURCE half then runs the
     * real send (which arms the confirmations) and the real sweep against
     * every gate that must keep the records, the one gate that destroys --
     * and the deferred reclamation that completes only after both. */
    printf("\n-- Phase H: nothing is torn down without the destination's word --\n");
    {
        const uint32_t H_PART = 96;
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
        host_partition_define(H_PART);
        g_fake_local_node_id = 2; g_reset_fakes();   /* node 2: the destination */
        g_fill_pages();
        uint64_t h_now = 40000;   /* past Phase G's last sweep reading (34000) */

        /* ── The destination half: what a LIVE_ACK answer is grounded in ── */
        {
            struct EnvCkptRecord r =
                g_record(H_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            CHECK(env_ckpt_record(&r) == 0, "setup: a record stands on this node");
            CHECK(env_migrate_live_query(H_PART, 0) == 1,
                  "*** a record alone never confirms: one never restored WITH ITS "
                  "CONTENTS answers not-live ***");
            g_fake_owner_node = 1;
            CHECK(env_migrate_live_query(H_PART, 0) == 1,
                  "a partition this node does not own answers not-live");
            g_fake_owner_node = 2;

            g_stage(H_PART, 0, &r, 6);
            CHECK(env_migrate_recv_find(H_PART, 0) != 0, "setup: ...staged");
            CHECK(env_migrate_live_query(H_PART, 0) == 1,
                  "*** staged is not restored: a completed slot not yet poured answers "
                  "not-live -- confirming from staged bytes would have the source "
                  "destroy the only complete copy ***");
            env_migrate_reset();
            env_ckpt_reset();

            struct EnvCkptRecord r2 =
                g_record(H_PART, 0, g_live_r0, g_live_r1, g_live_r2);
            g_create_record = r2;
            g_stage(H_PART, 0, &r2, 6);
            h_now += 1000;
            CHECK(env_migrate_tick(h_now) == 1, "setup: the slot restores for real");
            CHECK(env_migrate_live_query(H_PART, 0) == 0,
                  "*** the node that restored WITH ITS CONTENTS answers LIVE ***");

            env_ckpt_restore_begin();
            CHECK(env_migrate_live_query(H_PART, 0) == 1,
                  "an environment whose restore has not settled answers not-live");
            {
                uint32_t pp, ii, ee;
                while (env_ckpt_restore_next(&pp, &ii, &ee))
                    env_ckpt_restore_settle(pp, ii);
            }
            CHECK(env_migrate_live_query(H_PART, 0) == 0,
                  "...and live again once settled");

            (void)env_ckpt_drop(H_PART, 0);
            CHECK(env_migrate_live_query(H_PART, 0) == 1,
                  "an instance that is gone here answers not-live");
        }

        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();

        /* ── H1: a refused send arms NOTHING ──────────────────────────────
         * The destination never saw the bytes, so its environment never
         * came to be there -- and the source's are still whole here. A
         * confirmation armed now could only ever destroy on a word that
         * can never truthfully come. */
        g_fake_local_node_id = 1; g_reset_fakes();
        {
            peer_reset();
            nack_begin = 1;
            int rc = h_send_n(97, 2);
            CHECK(rc == ENV_MIGRATE_REFUSE_BEGIN, "setup: a refused send");
            CHECK(env_migrate_pending_count() == 0,
                  "*** a send the destination refused arms NO teardown confirmation -- "
                  "its environments are still here, so its records must stay ***");
            nack_begin = 0;
            env_migrate_reset(); env_ckpt_reset();
        }

        /* ── H2: success arms one confirmation per environment, and the
         * records survive every step up to the word ─────────────────────── */
        {
            int d_before = g_destroy_calls;
            int rc = h_send_n(97, 2);
            CHECK(rc == 0, "setup: both environments cross");
            CHECK(env_migrate_pending_count() == 2,
                  "*** every confirmed environment arms exactly one teardown "
                  "confirmation -- one teardown, one question ***");
            uint32_t id0 = env_ckpt_find(97, 0)->env_id;
            int reclaim_before = g_reclaim_calls;
            g_fake_owner_node = DST_NODE;   /* Step 4's handoff, simulated */

            int req_before = live_req_count;
            h_now += 1000; (void)env_migrate_tick(h_now);
            CHECK(env_migrate_pending_count() == 2, "both confirmations still pending");
            CHECK(g_destroy_calls == d_before,
                  "*** before the destination's word: not one destroy ran ***");
            CHECK(env_ckpt_find(97, 0) != 0 && env_ckpt_find(97, 1) != 0,
                  "*** ...and BOTH records still stand -- no record may vanish "
                  "before the destination confirms ***");
            CHECK(live_req_count == req_before + 2,
                  "the sweep polled once per pending environment (LIVE_REQ on the wire)");

            /* "not yet" is an answer, not a permission. */
            h_deliver_live_ack(97, 0, 1);
            h_now += 1000; (void)env_migrate_tick(h_now);
            CHECK(g_destroy_calls == d_before && env_ckpt_find(97, 0) != 0,
                  "*** a not-yet answer keeps the record: waiting is not permission ***");

            /* The word. */
            h_deliver_live_ack(97, 0, 0);
            h_now += 1000; (void)env_migrate_tick(h_now);
            CHECK(g_destroy_calls == d_before + 1 && g_destroy_env_id == id0,
                  "*** the destination's word ends THAT environment here, by name ***");
            CHECK(env_ckpt_find(97, 0) == 0 && env_ckpt_find(97, 1) != 0,
                  "*** ...exactly one record gone: the one that moved; the sibling "
                  "whose word never came still stands ***");
            CHECK(env_migrate_pending_count() == 1,
                  "one confirmation is still pending (and countable)");
            CHECK(g_reclaim_calls == reclaim_before,
                  "no frames reclaimed while a record of the partition remains");

            /* H3: ownership comes back -- the records are the truth again. */
            g_fake_owner_node = 1;
            h_now += 1000; (void)env_migrate_tick(h_now);
            CHECK(env_migrate_pending_count() == 0,
                  "*** ownership local again disarms: the records here are the truth "
                  "again and nothing will destroy them ***");
            CHECK(g_destroy_calls == d_before + 1 && env_ckpt_find(97, 1) != 0,
                  "...the sibling's record survives untouched");
            CHECK(g_reclaim_calls == reclaim_before,
                  "...and its frames stay with it");
        }
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();

        /* ── H4: the happy completion -- destroy, then the deferred
         * reclamation, in that order ───────────────────────────────────── */
        {
            int d_before = g_destroy_calls;
            int reclaim_before = g_reclaim_calls;
            int rc = h_send_n(100, 1);
            CHECK(rc == 0, "setup: one environment crosses");
            uint32_t id = env_ckpt_find(100, 0)->env_id;
            CHECK(env_migrate_pending_count() == 1, "one confirmation armed");
            g_fake_owner_node = DST_NODE;
            h_now += 1000; (void)env_migrate_tick(h_now);
            h_deliver_live_ack(100, 0, 0);
            h_now += 1000; (void)env_migrate_tick(h_now);
            CHECK(g_destroy_calls == d_before + 1 && g_destroy_env_id == id,
                  "*** the confirmed environment is ended by name ***");
            CHECK(env_ckpt_find(100, 0) == 0 && env_migrate_pending_count() == 0,
                  "*** ...its record is gone and no confirmation is pending: nothing "
                  "on this node describes it anymore ***");
            /* The disarm that just ran is the source's resolution point:
             * with the record and confirmation both gone, the ONLY thing
             * that can still hold the failover gate closed is the resolved
             * tail this disarm started. (Without it the live 2-node run's
             * source declared the still-blocked destination DEAD one sweep
             * after this teardown completed and adopted it, 2026-10-13.) */
            CHECK(env_migrate_staging_count() == 0 && env_migrate_in_flight() == 1,
                  "*** in_flight: a finished confirmation walk keeps the resolved tail "
                  "-- its counterparty's sweep may still be resuming, and that silence "
                  "is not a death ***");
            CHECK(g_reclaim_calls == reclaim_before + 1 &&
                  g_reclaim_last_arg == 100,
                  "*** and the deferred reclamation completes on the LAST "
                  "confirmation, for THAT partition -- frames come back only once the "
                  "records are gone, never before ***");
        }
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();

        /* ── H5: a partition that no longer exists ends its entry (that
         * partition's own teardown owns its records and frames) ─────────── */
        {
            int d_before = g_destroy_calls;
            int rc = h_send_n(102, 1);
            CHECK(rc == 0, "setup: ...sent");
            g_fake_owner_node = DST_NODE;
            host_partition_destroy(102);
            h_now += 1000; (void)env_migrate_tick(h_now);
            CHECK(env_migrate_pending_count() == 0,
                  "*** a partition that no longer exists ends its confirmation entry -- "
                  "that partition's own destroy owns its records and frames ***");
            CHECK(g_destroy_calls == d_before,
                  "...and no teardown runs against a partition that is already gone");
        }
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();

        /* ── H6: a destination that never answers is asked a bounded number
         * of times, then left alone -- countable, never destroyed ──────── */
        {
            int d_before = g_destroy_calls;
            int rc = h_send_n(103, 1);
            CHECK(rc == 0, "setup: ...sent");
            g_fake_owner_node = DST_NODE;
            int req_before = live_req_count;
            for (int k = 0; k < 62; k++) { h_now += 1000; (void)env_migrate_tick(h_now); }
            CHECK(live_req_count - req_before == 60,
                  "*** a destination that never answers is asked exactly 60 times, then "
                  "the sweep stops asking -- bounded, not a hot loop ***");
            CHECK(g_destroy_calls == d_before,
                  "...and never once torn down in the absence of the word");
            CHECK(env_migrate_pending_count() == 1 && env_ckpt_find(103, 0) != 0,
                  "*** ...with the record still standing AND still countable: a "
                  "confirmation that never arrives must be counted, not suspected ***");
        }
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();

        /* ── H7: a teardown that cannot complete keeps its record (the
         * fate of the environment is unknown -- described, not forgotten)
         * ─────────────────────────────────────────────────────────────── */
        {
            g_destroy_rc = -1;   /* the channel never answers */
            int rc = h_send_n(104, 1);
            CHECK(rc == 0, "setup: ...sent");
            g_fake_owner_node = DST_NODE;
            h_now += 1000; (void)env_migrate_tick(h_now);
            h_deliver_live_ack(104, 0, 0);
            int d_before = g_destroy_calls;
            for (int k = 0; k < 8; k++) { h_now += 1000; (void)env_migrate_tick(h_now); }
            CHECK(g_destroy_calls - d_before == 5,
                  "*** a teardown that cannot complete is attempted exactly 5 times, "
                  "then stops -- bounded, no infinite retry ***");
            CHECK(env_migrate_pending_count() == 1 && env_ckpt_find(104, 0) != 0,
                  "*** ...and the record STAYS: the destination is confirmed, but "
                  "this node's environment could not be ended -- still countable, an "
                  "operator's to finish by name ***");
            g_destroy_rc = 0;
        }
        env_ckpt_reset(); env_migrate_reset(); host_partition_stubs_reset();
        CHECK(env_ckpt_count_live() == 0,
              "teardown: the record table is empty again");
        CHECK(env_migrate_pending_count() == 0,
              "teardown: and no confirmations are pending");
        CHECK(env_migrate_staging_count() == 0,
              "teardown: and so is the staging table");
        CHECK(g_reclaim_calls == 1 && g_reclaim_last_arg == 100,
              "exactly ONE reclamation ran across all of Phase H -- H4's completion: "
              "every other gate kept its records AND their frames, never one without "
              "the other");
        g_fake_local_node_id = 1;
    }

    /* ── The frame buffer was never re-entered, and nothing overflowed ──
     * dspp_tx_reentrant_dropped is the guard that would have silently eaten
     * an ACK had any phase nested a transmit inside a transmit; zero is the
     * evidence that the ACK path is the one this test thinks it is. */
    /* The death-during-create fix, last: every phase above that blocked on
     * the wire (the ACK waits, the pour) runs ON the sweep in the real
     * kernel, so each pump of failover_sweep_keepalive() keeps the
     * heartbeat/checkpoint streams alive mid-block. Counted, not stubbed
     * silently: a wait loop that forgot the pump would leave this at 0. */
    CHECK(g_keepalive_calls > 0,
          "*** the blocking waits (ACK waits, pour) pumped the sweep keepalive: "
          "a blocked send keeps the heartbeat stream alive (death-during-create) ***");
    CHECK(dspp_tx_reentrant_dropped == 0,
          "*** no DSPP frame was ever dropped for re-entering the transmit path ***");
    CHECK(dspp_tx_oversize_dropped == oversize_before,
          "*** and none for exceeding the link MTU, across every phase ***");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
