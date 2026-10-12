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
 * sender's wiring into partition_migrate()'s critical section (increment 3)
 * and the restore of a staged slot into a live environment (increment 4).
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

static void peer_reset(void) {
    loss_page = -1; loss_frag = 0; loss_times = 0; loss_applied = 0;
    nack_begin = 0; nack_page = 0;
    begin_loss_times = 0; begin_loss_applied = 0;
    begin_ack_count = 0; page_ack_count = 0;
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

    /* ── The frame buffer was never re-entered, and nothing overflowed ──
     * dspp_tx_reentrant_dropped is the guard that would have silently eaten
     * an ACK had any phase nested a transmit inside a transmit; zero is the
     * evidence that the ACK path is the one this test thinks it is. */
    CHECK(dspp_tx_reentrant_dropped == 0,
          "*** no DSPP frame was ever dropped for re-entering the transmit path ***");
    CHECK(dspp_tx_oversize_dropped == oversize_before,
          "*** and none for exceeding the link MTU, across every phase ***");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
