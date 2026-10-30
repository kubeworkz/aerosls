/*
 * env_replica_host_test.c — POSIX-Environments v0.2 Phase P3, failover
 * destination (a), increment 1's teeth: the replica RECEIVE path, driven
 * both ways — the API directly (staging, finalize, the band, the refusal
 * codes) and the REAL dispatch (opcode 25 routed to the replica receiver,
 * the shared PAGE family following via env_replica_holds(), ACKs by the
 * family's own convention).
 *
 * The stub set is env_migrate_host_test.c's, deliberately: this test links
 * the same translation units (env_migrate.c is where the wire's shared
 * page-numbering arithmetic lives — env_migrate_payload_pages() gates
 * every replica BEGIN on it) plus kernel/env_replica.c. The NVMe fake and
 * the frame fake are the same technique for the same reason: a replica is
 * a DISK fact staged through RAM frames, so both halves of its life must
 * be real bytes under the test.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I drivers -I net -I user \
 *       -o /tmp/env_replica_host_test \
 *       tests/env_replica_host_test.c kernel/env_replica.c kernel/env_migrate.c \
 *       kernel/env_ckpt.c net/dspp.c net/dspp_checkpoint.c
 *   /tmp/env_replica_host_test
 */
#include "kernel/stream.h"        /* stream_migrate_recv_* stubs for dspp.c */
#include "kernel/simi_ctx_migrate.h"
#include "kernel/env_ckpt.h"
#include "kernel/env_migrate.h"
#include "kernel/env_replica.h"
#include "kernel/env_proto.h"
#include "kernel/env_service.h"
#include "kernel/env_payload.h"
#include "kernel/checkpoint_delta.h"
#include "kernel/env_storage.h"
#include "kernel/persist.h"      /* PERSIST_ENV_REPLICA_*_LBA — section J patches the real entry frame */
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "net/e1000.h"
#include "net/dspp.h"
#include "net/consensus.h"  /* cluster_roster — what the push walks */
#include "net/net.h"
#include "tests/partition_host_stubs.h"
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
 * Stubs — the identical choices tests/env_migrate_host_test.c makes (see
 * that file for each one's reasoning; nothing here is under test).
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

void kernel_serial_print(const char* s) { (void)s; }
#include <stdarg.h>
void kernel_serial_printf(const char* fmt, ...) {
    if (!getenv("REPLICA_TEST_TRACE")) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
volatile uint64_t kernel_tick_counter = 0;

void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    void* p = aligned_alloc(4096, FRAME_SIZE);
    if (!p) return 0;
    memset(p, 0, FRAME_SIZE);
    return p;
}
/* Top-down staging (frame_pool.h): the replica receiver takes its staging
 * frames from the top exactly as the migration receiver does — same fake,
 * so the alloc-failure arm below exercises the real call path. */
static int g_frame_alloc_fail = 0;
void* allocate_physical_ram_frame_for_partition_top(uint32_t partition_id) {
    if (g_frame_alloc_fail) return 0;
    return allocate_physical_ram_frame_for_partition(partition_id);
}
int free_physical_ram_frame_for_partition(void* frame, uint32_t partition_id) {
    (void)partition_id;
    free(frame);
    return 0;
}

struct SLSObjectEntry object_catalog[CATALOG_MAX_OBJECTS];
uint32_t               object_catalog_count = 0;
int partition_is_local(uint32_t partition_id) { (void)partition_id; return 1; }
int partition_holds_write_lease(uint32_t partition_id) { (void)partition_id; return 1; }
void process_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) { (void)packet; (void)now; }
void process_partition_consensus_packet(struct DSPPFullPagePacket* packet, uint64_t now) { (void)packet; (void)now; }
void partition_sync_upsert(uint32_t partition_id, uint32_t owner_node_id, const char* name, uint32_t source_node_id) { (void)partition_id; (void)owner_node_id; (void)name; (void)source_node_id; }
void partition_sync_withdraw(uint32_t partition_id, uint32_t source_node_id) { (void)partition_id; (void)source_node_id; }
void partition_sync_ownedset(uint32_t source_node_id, uint32_t generation, const uint32_t* ids, uint32_t count) { (void)source_node_id; (void)generation; (void)ids; (void)count; }
void service_remote_learn(const char* name, uint32_t node_id, uint32_t partition_id,
                          uint8_t kind, uint32_t port, uint32_t uid, uint8_t serving) {
    (void)name; (void)node_id; (void)partition_id; (void)kind; (void)port; (void)uid; (void)serving;
}
void service_remote_forget(const char* name, uint32_t node_id) {
    (void)name; (void)node_id;
}

/* The rest of env_migrate.c's surface, unreachable in this test (the send
 * and restore halves never run — only env_migrate_payload_pages(), the
 * shared page-numbering arithmetic, is under use) but needed to link the
 * same translation unit the real BEGIN gate lives in. Faithful stand-ins:
 * a parked environment, an empty image tail, and identity answers. */
int env_service_create(uint32_t partition, uint32_t index,
                       uint16_t* out_status, uint32_t* out_env_id) {
    (void)partition; (void)index;
    if (out_status)  *out_status  = ENV_OK;
    if (out_env_id)  *out_env_id  = 1;
    return 0;
}
int env_service_destroy(uint32_t env_id, uint32_t partition,
                        uint16_t* out_status, uint32_t* out_env_id,
                        uint32_t* out_partition) {
    (void)partition;
    if (out_status)   *out_status   = ENV_OK;
    if (out_env_id)   *out_env_id   = env_id;
    if (out_partition)*out_partition = partition;
    return 0;
}
int env_payload_wait_parked(const struct EnvCkptRecord* rec) {
    (void)rec; return 1;   /* parked: the posture gate this test never walks */
}
uint32_t env_payload_image_window_list(const struct EnvCkptRecord* rec,
                                       uint64_t* vaddrs,
                                       struct ProcessDescriptor** pds,
                                       uint32_t cap, uint32_t* err) {
    (void)rec; (void)vaddrs; (void)pds; (void)cap;
    if (err) *err = 0;
    return 0;               /* this world's sidecars hold no image tail */
}
int env_payload_proc_parked(const struct ProcessDescriptor* pd) {
    (void)pd; return 1;
}
int env_payload_resolve_vaddr(uint64_t cr3, uint64_t vaddr,
                              uint64_t* out_phys) {
    (void)cr3; (void)vaddr; (void)out_phys;
    return 0;
}
uint32_t partition_get_owner_node(uint32_t partition_id) {
    (void)partition_id; return 0;
}
uint32_t partition_reclaim_all_frames(uint32_t partition_id) {
    (void)partition_id; return 0;
}

/* The stream and context receive families, stubbed FAITHFUL (refuse) — this
 * test never emits their opcodes; were one to arrive, refusing is what a
 * node not set up for that family answers. */
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

static uint32_t g_fake_local_node_id = 0;
uint32_t cluster_local_node_id(void) { return g_fake_local_node_id; }
uint32_t cluster_leader_id(void) { return 0; }

/* net/consensus.c is not linked — the roster the push walks is defined
 * here, the way tests/checkpoint_recovery_host_test.c defines it. */
struct ClusterPeer cluster_roster[CLUSTER_NODE_MAX];
uint32_t            cluster_roster_count = 0;

void failover_note_heartbeat(uint32_t node_id, uint64_t now) {
    (void)node_id; (void)now;
}
int failover_peer_status(uint32_t node_id) { (void)node_id; return -1; }
/* The loopback's delivery point — defined below, after the frame capture:
 * the sender's ACK wait pumps this every spin, so frames are delivered from
 * the WAIT, never from inside the transmit stub (dspp_transmit_raw() drops a
 * frame transmitted while another is being built — and the real RX path
 * delivers while a sender waits anyway). */
static void loopback_drain(void);
void failover_sweep_keepalive(void) { loopback_drain(); }

MACAddr net_my_mac;

/* ─── Stateful fake NVMe (identical technique to env_migrate's) ──────────── */
void* io_sq = (void*)1;
void* io_cq = (void*)1;
#define FAKE_NVME_MAX_PAGES 128
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
/* A clean device: no directory, no band — the wire-arm section starts from
 * "this node has never held a replica". */
static void nvme_fake_reset(void) { fake_count = 0; }

/* ─── Capture of whatever the receiver transmits (its ACKs) ───────────────
 * This test never runs the SENDER (that is increment 2) — it hand-builds
 * the wire frames — so the capture only needs to prove what came BACK:
 * an ACK frame with the family's opcode and a status answering 0/nonzero. */
#define MAX_CAPTURED_FRAMES 256
#define FRAME_STRIDE (ETH_HDR_LEN + (unsigned)sizeof(struct DSPPEnvMigrateBeginPacket))
static uint8_t  captured_frame[MAX_CAPTURED_FRAMES][FRAME_STRIDE];
static uint16_t captured_frame_len[MAX_CAPTURED_FRAMES];
static int      captured_frame_count = 0;

void e1000_transmit(NicRole role, void* buf, uint16_t size) {
    (void)role;
    if (captured_frame_count >= MAX_CAPTURED_FRAMES) return;
    uint16_t n = size;
    if (n > FRAME_STRIDE) n = (uint16_t)FRAME_STRIDE;
    memcpy(captured_frame[captured_frame_count], buf, n);
    captured_frame_len[captured_frame_count] = n;
    captured_frame_count++;
    kernel_tick_counter += 1;   /* time passes while frames are on the wire */
}
/* ─── The loopback: one process, both nodes ────────────────────────────────
 * A captured frame is delivered when the sender next PUMPS (its ACK wait),
 * not when it is transmitted: dspp_transmit_raw() drops a frame emitted
 * while another is being built, so the receiver's ACK has to be sent at
 * top level — which is also the real shape, RX delivering while TX waits.
 * Because one process has one local id, delivery rewrites the destination to
 * this process and any source that IS this process to the peer: the frame
 * lands exactly as its real recipient would receive it. g_loopback_tick
 * models the PEER's own service poll running between frames (it is what
 * turns a staged transfer into a finalized replica). */
static int      g_loopback = 0;
static int      g_loopback_tick = 1;
static uint32_t g_loopback_peer = 1;
static int      g_deliver_head = 0;

static void capture_reset(void) {
    captured_frame_count = 0;
    g_deliver_head = 0;
}

static void loopback_drain(void) {
    while (g_loopback && g_deliver_head < captured_frame_count) {
        uint8_t* frame = captured_frame[g_deliver_head];
        uint16_t len   = captured_frame_len[g_deliver_head];
        g_deliver_head++;
        if (len < ETH_HDR_LEN + sizeof(struct DSPPEnvMigrateHeader)) continue;
        struct DSPPEnvMigrateHeader* h =
            (struct DSPPEnvMigrateHeader*)(frame + ETH_HDR_LEN);
        if (h->magic != DSPP_MIGRATE_MAGIC) continue;
        if (g_loopback_tick) env_replica_tick();   /* the peer's service poll */
        h->node_dest_id = cluster_local_node_id();
        if (h->node_source_id == cluster_local_node_id())
            h->node_source_id = (uint16_t)g_loopback_peer;
        if (getenv("REPLICA_TEST_TRACE"))
            printf("[drain] opcode=%u dest=%u len=%u\n",
                   (unsigned)h->opcode, (unsigned)h->node_dest_id,
                   (unsigned)len);
        dspp_rx_dispatch(h, (uint16_t)(len - ETH_HDR_LEN));
    }
}
/* The last transmitted frame as the family's own header (ACKs are a bare
 * DSPPEnvMigrateHeader — dspp_env_migrate_send_ack's own shape). */
static const struct DSPPEnvMigrateHeader* last_frame(void) {
    if (captured_frame_count == 0) return 0;
    if (captured_frame_len[captured_frame_count - 1] <
        ETH_HDR_LEN + sizeof(struct DSPPEnvMigrateHeader)) return 0;
    return (const struct DSPPEnvMigrateHeader*)
        (captured_frame[captured_frame_count - 1] + ETH_HDR_LEN);
}

/* ─── Hand-built wire frames, delivered the way dispatch delivers them ───── */
static void wire_begin(uint64_t tid, uint32_t part, uint32_t idx,
                       uint32_t total, const struct EnvCkptRecord* rec,
                       uint32_t rec_len) {
    static struct DSPPEnvMigrateBeginPacket pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.header.magic          = DSPP_MIGRATE_MAGIC;
    pkt.header.opcode         = DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ;
    pkt.header.node_source_id = 2;
    pkt.header.node_dest_id   = cluster_local_node_id();
    pkt.header.transfer_id    = tid;
    pkt.header.partition_id   = part;
    pkt.header.env_index      = idx;
    pkt.header.total_pages    = total;
    pkt.header.record_len     = rec_len;
    if (rec && rec_len == (uint32_t)sizeof(*rec))
        memcpy(pkt.record, rec, sizeof(*rec));
    dspp_rx_dispatch(&pkt, (uint16_t)sizeof(pkt));
}
static void wire_page(uint64_t tid, uint32_t part, uint32_t page,
                      uint32_t frag, uint8_t fill) {
    static struct DSPPEnvMigratePagePacket pkt;
    static uint8_t data[DSPP_MIGRATE_FRAG_BYTES];
    memset(&pkt, 0, sizeof pkt);
    memset(data, fill, sizeof data);
    pkt.header.magic          = DSPP_MIGRATE_MAGIC;
    pkt.header.opcode         = DSPP_MIGRATE_ENV_PAGE_REQ;
    pkt.header.node_source_id = 2;
    pkt.header.node_dest_id   = cluster_local_node_id();
    pkt.header.transfer_id    = tid;
    pkt.header.partition_id   = part;
    pkt.header.page_index     = page;
    pkt.header.frag_index     = frag;
    memcpy(pkt.page_data, data, DSPP_MIGRATE_FRAG_BYTES);
    dspp_rx_dispatch(&pkt, (uint16_t)sizeof(pkt));
}

/* ══════════════════════════════════════════════════════════════════════════
 * The environment under test. Page numbering is the record's own (regions
 * in record order, then the durable extent): payload = 2 + 1 region frames
 * + 2 extent pages = 5 pages, no image tail — exactly what
 * env_migrate_payload_pages() computes from this record.
 * ══════════════════════════════════════════════════════════════════════ */
#define PART_A   77u
#define PART_B   90u
#define STATE_LBA     8192ull
#define STATE_SECTORS 16ull   /* 2 pages at 8 sectors per page */
#define PAYLOAD_PAGES 5u

static struct EnvCkptRecord mk_record(uint32_t partition, uint32_t index,
                                      uint64_t seq, uint32_t env_id) {
    struct EnvCkptRecord r;
    memset(&r, 0, sizeof r);
    r.magic        = ENV_CKPT_REC_MAGIC;
    r.version      = ENV_CKPT_REC_VERSION;
    r.size         = (uint32_t)sizeof(struct EnvCkptRecord);
    r.sequence     = seq;
    r.partition_id = partition;
    r.index        = index;
    r.env_id       = env_id;
    r.console_id   = 3;
    r.flags        = ENV_CKPT_FLAG_QUIESCED;
    r.n_tasks      = 1;
    r.n_regions    = 2;
    r.regions[0].frames = 2;
    r.regions[0].kind   = ENV_CKPT_REGION_POSIX_HEAP;
    r.regions[1].frames = 1;
    r.regions[1].kind   = ENV_CKPT_REGION_RD_HEAP;
    r.state_lba     = STATE_LBA;
    r.state_sectors = STATE_SECTORS;
    r.state_bytes   = (uint32_t)STATE_SECTORS * 512u;
    strcpy(r.tasks[0].name, "aerosls.posix.0");
    return r;
}

/* Feed one whole page (4 fragments, delivered out of order on purpose —
 * the assembly must not care). fill is the byte every byte of the page
 * carries, so read-back checks are byte-exact. */
static void feed_page(uint64_t tid, uint32_t page, uint8_t fill) {
    static uint8_t frag[DSPP_MIGRATE_FRAG_BYTES];
    memset(frag, fill, sizeof frag);
    const uint32_t order[DSPP_MIGRATE_FRAGS_PER_PAGE] = { 3, 1, 0, 2 };
    for (uint32_t k = 0; k < DSPP_MIGRATE_FRAGS_PER_PAGE; k++)
        env_replica_recv_page(tid, page, order[k], frag);
}
static void feed_all(uint64_t tid, uint8_t fill_base) {
    for (uint32_t p = 0; p < PAYLOAD_PAGES; p++)
        feed_page(tid, p, (uint8_t)(fill_base + p));
}
static int page_holds(uint32_t partition, uint32_t index, uint32_t page,
                      uint8_t fill) {
    static uint8_t buf[4096];
    if (env_replica_read_page(partition, index, page, buf) != 0) return 0;
    for (int b = 0; b < 4096; b++) if (buf[b] != fill) return 0;
    return 1;
}
static uint32_t last_refusal(void) { return env_replica_last_refusal().code; }

int main(void) {
    struct EnvCkptRecord rec;
    struct EnvReplicaDirEntry ent;
    uint64_t tid;

    /* ── A: a clean round trip through the API ───────────────────────────── */
    printf("-- A: clean receive, finalize, read back --\n");
    env_replica_reset();
    nvme_fake_reset();
    CHECK(last_refusal() == 0, "A0: latch starts clear");
    rec = mk_record(PART_A, 0, 7, 100);
    tid = ((uint64_t)PART_A << 32) | 0;
    CHECK(env_migrate_payload_pages(&rec) == PAYLOAD_PAGES,
          "A1: the shared page-numbering arithmetic sees 5 payload pages");
    CHECK(env_replica_recv_begin(tid, PART_A, 0, PAYLOAD_PAGES, &rec,
                                 sizeof rec, 2) == 0,
          "A2: BEGIN staged");
    CHECK(env_replica_staging_count() == 1, "A3: one staging slot active");
    CHECK(!env_replica_holds(((uint64_t)PART_A << 32) | 1),
          "A4: a transfer this table does not hold answers false");
    CHECK(env_replica_holds(tid), "A5: the staged transfer answers holds()");
    CHECK(env_replica_tick() == 0,
          "A6: a partial staging never finalizes (tick reports 0)");
    CHECK(!env_replica_present(PART_A, 0),
          "A7: partial bytes are never visible as a replica");
    feed_all(tid, 0x40);
    CHECK(env_replica_tick() == 1, "A8: complete staging finalizes once");
    CHECK(env_replica_staging_count() == 0, "A9: staged frames returned");
    CHECK(env_replica_present(PART_A, 0), "A10: the replica is findable");
    CHECK(env_replica_count() == 1, "A11: the directory holds one entry");
    CHECK(env_replica_lookup(PART_A, 0, &ent) &&
          ent.seq == 7 && ent.pages == PAYLOAD_PAGES &&
          ent.src_node == 2 && (ent.flags & ENV_REPLICA_F_VALID),
          "A12: the entry carries seq, page count, provenance, VALID");
    for (uint32_t p = 0; p < PAYLOAD_PAGES; p++)
        if (!page_holds(PART_A, 0, p, (uint8_t)(0x40 + p))) {
            CHECK(0, "A13: band page came back byte-exact");
            p = PAYLOAD_PAGES;
        }
    CHECK(page_holds(PART_A, 0, 0, 0x40) && page_holds(PART_A, 0, 4, 0x44),
          "A13: band pages came back byte-exact, in wire order");
    CHECK(last_refusal() == 0, "A14: no refusal was latched on the happy path");
    CHECK(env_replica_read_page(PART_A, 0, PAYLOAD_PAGES, (uint8_t[4096]){0}) != 0,
          "A14b: a page past the entry's count is refused");

    /* ── B: refusals, each by name, none staging anything ────────────────── */
    printf("-- B: the BEGIN/page gates --\n");
    env_replica_reset();
    rec = mk_record(PART_A, 1, 7, 101);
    tid = ((uint64_t)PART_A << 32) | 1;
    CHECK(env_replica_recv_begin(tid, PART_A, 1, PAYLOAD_PAGES, 0,
                                 sizeof rec, 2) ==
          ENV_REPLICA_REFUSE_NO_RECORD, "B1: no record -> NO_RECORD");
    CHECK(env_replica_recv_begin(tid, PART_A, 1, PAYLOAD_PAGES, &rec, 10, 2) ==
          ENV_REPLICA_REFUSE_RECORD_LEN, "B2: wrong record length -> RECORD_LEN");
    CHECK(env_replica_recv_begin(tid, PART_A, 1, PAYLOAD_PAGES - 1, &rec,
                                 sizeof rec, 2) ==
          ENV_REPLICA_REFUSE_PAGE_COUNT, "B3: total under the floor -> PAGE_COUNT");
    CHECK(env_replica_recv_begin(tid, PART_A, 1,
                                 PAYLOAD_PAGES + ENV_MIGRATE_IMAGE_MAX + 1,
                                 &rec, sizeof rec, 2) ==
          ENV_REPLICA_REFUSE_PAGE_COUNT, "B4: tail past the ceiling -> PAGE_COUNT");
    {   /* a record whose payload alone exceeds the band slot's page capacity */
        struct EnvCkptRecord big = mk_record(PART_A, 1, 7, 101);
        big.n_regions = 3;
        big.regions[0].frames = 1100;
        big.regions[1].frames = 500;
        big.regions[2].frames = 500;   /* 2100 payload pages, no tail needed */
        big.state_sectors = 0;
        CHECK(env_migrate_payload_pages(&big) == 2100,
              "B5: the shared arithmetic sees 2100 payload pages");
        CHECK(env_replica_recv_begin(tid, PART_A, 1, 2100, &big,
                                     sizeof big, 2) ==
              ENV_REPLICA_REFUSE_TOO_MANY, "B6: over the band slot -> TOO_MANY");
    }
    CHECK(env_replica_staging_count() == 0, "B7: no refusal staged a slot");
    CHECK(!env_replica_present(PART_A, 1), "B8: no refusal produced a replica");
    CHECK(env_replica_recv_page(((uint64_t)PART_A << 32) | 9, 0, 0,
                                (const uint8_t[1024]){0}) ==
          ENV_REPLICA_REFUSE_UNKNOWN, "B9: a page for an unknown transfer -> UNKNOWN_TRANSFER");
    CHECK(env_replica_recv_begin(tid, PART_A, 1, PAYLOAD_PAGES, &rec,
                                 sizeof rec, 2) == 0, "B10: the valid BEGIN stages");
    CHECK(env_replica_recv_page(tid, 9, 0, (const uint8_t[1024]){0}) ==
          ENV_REPLICA_REFUSE_RANGE, "B11: a page past total -> RANGE");
    CHECK(env_replica_recv_page(tid, 0, DSPP_MIGRATE_FRAGS_PER_PAGE,
                                (const uint8_t[1024]){0}) ==
          ENV_REPLICA_REFUSE_RANGE, "B12: a fragment past the page -> RANGE");
    g_frame_alloc_fail = 1;
    CHECK(env_replica_recv_page(tid, 0, 0, (const uint8_t[1024]){0}) ==
          ENV_REPLICA_REFUSE_ALLOC, "B13: an ungettable frame -> ALLOC");
    g_frame_alloc_fail = 0;
    env_replica_reset();

    /* ── C: in-flight identity conflicts and the staging table's bound ───── */
    printf("-- C: slot-exists and table-full --\n");
    rec = mk_record(PART_A, 5, 7, 555);
    uint64_t tid_c = ((uint64_t)PART_A << 32) | 5;
    CHECK(env_replica_recv_begin(tid_c, PART_A, 5, PAYLOAD_PAGES, &rec,
                                 sizeof rec, 2) == 0, "C1: first BEGIN stages");
    CHECK(env_replica_recv_begin(tid_c, PART_A, 5, PAYLOAD_PAGES, &rec,
                                 sizeof rec, 2) == 0,
          "C2: a re-BEGIN for the same transfer is READY, not refused");
    CHECK(env_replica_staging_count() == 1, "C3: the re-BEGIN did not double-stage");
    CHECK(env_replica_recv_begin(tid_c + 1, PART_A, 5, PAYLOAD_PAGES, &rec,
                                 sizeof rec, 2) ==
          ENV_REPLICA_REFUSE_SLOT_EXISTS,
          "C4: the identity in flight under ANOTHER transfer -> SLOT_EXISTS");
    {   /* fill the staging table to its bound */
        int rc = 0;
        for (uint32_t i = 0; i < ENV_REPLICA_STAGING_MAX && rc == 0; i++) {
            struct EnvCkptRecord r = mk_record(PART_A, 10 + i, 7, 600 + i);
            rc = env_replica_recv_begin(((uint64_t)PART_A << 32) | (10 + i),
                                        PART_A, 10 + i, PAYLOAD_PAGES, &r,
                                        sizeof r, 2);
            if (rc == 0) feed_all(((uint64_t)PART_A << 32) | (10 + i), 0x50);
        }
        CHECK(rc == ENV_REPLICA_REFUSE_TABLE_FULL,
              "C5: one more concurrent transfer -> TABLE_FULL");
        CHECK(env_replica_staging_count() == ENV_REPLICA_STAGING_MAX,
              "C6: exactly the table's bound is staged");
    }
    env_replica_reset();
    nvme_fake_reset();

    /* ── D: the band fills; the ninth identity is refused BY NAME ────────── */
    printf("-- D: band capacity and supersede --\n");
    for (uint32_t i = 0; i < ENV_REPLICA_MAX; i++) {
        struct EnvCkptRecord r = mk_record(PART_B, i, 7, 700 + i);
        uint64_t t = ((uint64_t)PART_B << 32) | i;
        if (env_replica_recv_begin(t, PART_B, i, PAYLOAD_PAGES, &r, sizeof r, 2) != 0)
            break;
        feed_all(t, 0x60);
        env_replica_tick();
    }
    CHECK(env_replica_count() == ENV_REPLICA_MAX, "D1: eight replicas fill the band");
    rec = mk_record(91, 0, 7, 800);
    CHECK(env_replica_recv_begin(((uint64_t)91 << 32) | 0, 91, 0, PAYLOAD_PAGES,
                                 &rec, sizeof rec, 2) ==
          ENV_REPLICA_REFUSE_NO_DISK_SLOT,
          "D2: a ninth distinct identity -> NO_DISK_SLOT (named, not silent)");
    CHECK(env_replica_count() == ENV_REPLICA_MAX, "D3: the refusal changed nothing");
    {   /* an OLDER capture arriving after the newer one already landed */
        struct EnvCkptRecord old = mk_record(PART_B, 0, 3, 700);
        CHECK(env_replica_recv_begin(((uint64_t)PART_B << 32) | 0, PART_B, 0,
                                     PAYLOAD_PAGES, &old, sizeof old, 2) ==
              ENV_REPLICA_REFUSE_SUPERSEDED,
              "D4: an older capture of a replicated identity -> SUPERSEDED");
    }

    /* ── E: a NEWER capture replaces in place — bytes and seq both move ──── */
    printf("-- E: replacement with the band full --\n");
    {
        struct EnvCkptRecord newer = mk_record(PART_B, 0, 8, 700);
        uint64_t t = ((uint64_t)PART_B << 32) | 0;
        CHECK(env_replica_recv_begin(t, PART_B, 0, PAYLOAD_PAGES, &newer,
                                     sizeof newer, 2) == 0,
              "E1: a newer capture stages over a full band");
        feed_all(t, 0x90);   /* different bytes than D's 0x60 fill */
        CHECK(env_replica_tick() == 1, "E2: the replacement finalizes");
        CHECK(env_replica_count() == ENV_REPLICA_MAX,
              "E3: replacement does not grow the directory");
        CHECK(env_replica_lookup(PART_B, 0, &ent) && ent.seq == 8,
              "E4: the entry's sequence advanced to the newer capture");
        CHECK(page_holds(PART_B, 0, 0, 0x90) && page_holds(PART_B, 0, 4, 0x94),
              "E5: the band holds the NEWER bytes, byte-exact");
    }

    /* ── F: the real dispatch — opcode 25 in, ACK out, pages follow ──────── */
    printf("-- F: wire routing through dspp_env_migrate_rx --\n");
    env_replica_reset();
    nvme_fake_reset();
    capture_reset();
    rec = mk_record(PART_A, 3, 7, 103);
    tid = ((uint64_t)PART_A << 32) | 3;
    wire_begin(tid, PART_A, 3, PAYLOAD_PAGES, &rec, sizeof rec);
    {
        const struct DSPPEnvMigrateHeader* ack = last_frame();
        CHECK(ack && ack->opcode == DSPP_MIGRATE_ENV_BEGIN_ACK &&
              ack->status == 0,
              "F1: a replica BEGIN answers a zero-status BEGIN_ACK");
    }
    CHECK(env_replica_staging_count() == 1 &&
          env_replica_holds(tid),
          "F2: the opcode-25 frame staged the REPLICA receiver's slot");
    capture_reset();
    wire_page(tid, PART_A, 0, 0, 0x70);
    {
        const struct DSPPEnvMigrateHeader* ack = last_frame();
        CHECK(ack && ack->opcode == DSPP_MIGRATE_ENV_PAGE_ACK &&
              ack->status == 0,
              "F3: the shared PAGE opcode answered a zero-status PAGE_ACK");
    }
    {   /* the page must have landed in the replica's staging, not migrate's:
         * the replica's own slot now counts page 0 as not yet whole (3 of 4
         * fragments), and the only receiver that could have stored it is
         * the one holds() named. Proven by finishing the page here. */
        wire_page(tid, PART_A, 0, 1, 0x70);
        wire_page(tid, PART_A, 0, 2, 0x70);
        wire_page(tid, PART_A, 0, 3, 0x70);
        for (uint32_t p = 1; p < PAYLOAD_PAGES; p++)
            for (uint32_t f = 0; f < DSPP_MIGRATE_FRAGS_PER_PAGE; f++)
                wire_page(tid, PART_A, p, f, (uint8_t)(0x70 + p));
        CHECK(env_replica_tick() == 1,
              "F4: the wire-staged replica finalizes");
        CHECK(env_replica_present(PART_A, 3),
              "F5: the wire-staged replica is findable");
        CHECK(page_holds(PART_A, 3, 4, 0x74),
              "F6: the wire bytes reached the band byte-exact");
    }
    capture_reset();
    /* A page for a transfer NO receiver holds: the replica table says no
     * (holds() false), so the migration receiver answers — refusal, nonzero
     * ACK, nothing staged anywhere. */
    wire_page(((uint64_t)PART_A << 32) | 60, PART_A, 0, 0, 0x11);
    {
        const struct DSPPEnvMigrateHeader* ack = last_frame();
        CHECK(ack && ack->opcode == DSPP_MIGRATE_ENV_PAGE_ACK &&
              ack->status != 0,
              "F7: a page no receiver holds is refused with a nonzero ACK");
        CHECK(env_replica_staging_count() == 0,
              "F8: the refusal staged nothing");
    }

    /* ── G: a zero-page environment IS its record ────────────────────────── */
    printf("-- G: the empty payload --\n");
    env_replica_reset();
    {
        struct EnvCkptRecord empty;
        memset(&empty, 0, sizeof empty);
        empty.magic   = ENV_CKPT_REC_MAGIC;
        empty.version = ENV_CKPT_REC_VERSION;
        empty.size    = (uint32_t)sizeof empty;
        empty.sequence = 1;
        empty.partition_id = PART_A;
        empty.index = 9;
        empty.env_id = 109;
        CHECK(env_migrate_payload_pages(&empty) == 0,
              "G1: an empty record names zero payload pages");
        uint64_t t = ((uint64_t)PART_A << 32) | 9;
        CHECK(env_replica_recv_begin(t, PART_A, 9, 0, &empty, sizeof empty, 2) == 0,
              "G2: a zero-page BEGIN stages complete");
        CHECK(env_replica_tick() == 1, "G3: it finalizes without a single page");
        CHECK(env_replica_lookup(PART_A, 9, &ent) && ent.pages == 0 &&
              ent.blob_crc == 0,
              "G4: the entry records zero pages and a zero crc");
        CHECK(env_replica_read_page(PART_A, 9, 0, (uint8_t[4096]){0}) != 0,
              "G5: reading a page of an empty replica is refused");
    }

    /* ── H: a staging that wedges is dropped by age, named ───────────────── */
    printf("-- H: the stale bound --\n");
    env_replica_reset();
    rec = mk_record(PART_A, 12, 7, 112);
    tid = ((uint64_t)PART_A << 32) | 12;
    CHECK(env_replica_recv_begin(tid, PART_A, 12, PAYLOAD_PAGES, &rec,
                                 sizeof rec, 2) == 0, "H1: a staging opens");
    feed_page(tid, 0, 0x11);   /* 4 of 5 pages never come */
    kernel_tick_counter += ENV_REPLICA_STALE_TICKS + 1;
    CHECK(env_replica_tick() == 0, "H2: nothing finalizes");
    CHECK(env_replica_staging_count() == 0, "H3: the wedged staging was dropped");
    CHECK(last_refusal() == ENV_REPLICA_REFUSE_STALE, "H4: the drop is named STALE");
    CHECK(!env_replica_present(PART_A, 12), "H5: no replica was recorded");

    /* ── I: the capture-side sender, loop-backed end to end ─────────────── */
    printf("-- I: the push, through stream -> wire -> receiver -> confirm --\n");
    env_replica_reset();
    nvme_fake_reset();
    capture_reset();
    /* One process, two nodes: this process IS the peer (id 1) for delivery,
     * and the local id (2) is what senders stamp and the rewrites honor. */
    g_fake_local_node_id = 2;
    g_loopback_peer = 1;
    cluster_roster_count = 1;
    cluster_roster[0].node_id = 1;
    cluster_roster[0].active = 1;
    {
        /* A record whose regions and extent are REAL bytes — the stream
         * reads them exactly as the capture window would. */
        static uint8_t push_r0[2][4096] __attribute__((aligned(4096)));
        static uint8_t push_r1[1][4096] __attribute__((aligned(4096)));
        for (int b = 0; b < 4096; b++) {
            push_r0[0][b] = 0x80;
            push_r0[1][b] = 0x81;
            push_r1[0][b] = 0x82;
        }
        struct EnvCkptRecord rec2 = mk_record(79, 0, 11, 900);
        /* A record the RECORD LAYER accepts, because promotion's first
         * real gate is env_ckpt_valid(): exactly the three known region
         * kinds once each, every region non-empty, and a task entry a
         * restore could jump to. mk_record()'s two-region shorthand is fine
         * for the wire gates (they count pages, they do not vouch for the
         * descriptor), but the band's only reader vouches. Frames stay
         * 1/1/1 so the wire numbering — PAYLOAD_PAGES, the page fills I6/I7
         * read — is exactly what the two-region shorthand produced. */
        rec2.n_regions      = 3;
        rec2.regions[0].base = (uint64_t)(uintptr_t)push_r0[0];
        rec2.regions[0].frames = 1;
        rec2.regions[0].kind   = ENV_CKPT_REGION_POSIX_HEAP;
        rec2.regions[1].base = (uint64_t)(uintptr_t)push_r0[1];
        rec2.regions[1].frames = 1;
        rec2.regions[1].kind   = ENV_CKPT_REGION_RD_HEAP;
        rec2.regions[2].base = (uint64_t)(uintptr_t)push_r1[0];
        rec2.regions[2].frames = 1;
        rec2.regions[2].kind   = ENV_CKPT_REGION_RD_STORAGE;
        rec2.tasks[0].entry = 0x401000;   /* env_ckpt_valid refuses entry 0 */
        static uint8_t ext[4096] __attribute__((aligned(4096)));
        memset(ext, 0xC1, 4096); nvme_write_sync(STATE_LBA + 0, ext);
        memset(ext, 0xC2, 4096); nvme_write_sync(STATE_LBA + 8, ext);

        g_loopback = 1;
        g_loopback_tick = 1;
        CHECK(env_replica_push(&rec2) == 1,
              "I1: push confirms FINALIZATION on the peer (not merely staging)");
        g_loopback = 0;
        {
            int saw_replica_begin = 0, saw_confirm = 0, saw_pages = 0;
            for (int i = 0; i < captured_frame_count; i++) {
                if (captured_frame_len[i] <
                    ETH_HDR_LEN + sizeof(struct DSPPEnvMigrateHeader)) continue;
                const struct DSPPEnvMigrateHeader* h =
                    (const struct DSPPEnvMigrateHeader*)
                        (captured_frame[i] + ETH_HDR_LEN);
                if (h->magic != DSPP_MIGRATE_MAGIC) continue;
                if (h->opcode == DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ)
                    saw_replica_begin = 1;
                if (h->opcode == DSPP_MIGRATE_ENV_PAGE_REQ) saw_pages = 1;
                if (h->opcode == DSPP_MIGRATE_ENV_REPLICA_LIVE_REQ)
                    saw_confirm = 1;
            }
            CHECK(saw_replica_begin,
                  "I2: a replica BEGIN (opcode 25) went on the wire, not opcode 18");
            CHECK(saw_pages, "I3: the shared PAGE family carried the payload");
            CHECK(saw_confirm,
                  "I4: the confirm question (opcode 26) went out after the pages");
        }
        CHECK(env_replica_present(79, 0), "I5: the peer's band holds the replica");
        CHECK(page_holds(79, 0, 0, 0x80) && page_holds(79, 0, 2, 0x82),
              "I6: region pages crossed byte-exact, in record order");
        CHECK(page_holds(79, 0, 3, 0xC1) && page_holds(79, 0, 4, 0xC2),
              "I7: the durable extent crossed too — the FULL wire numbering");
        CHECK(env_replica_lookup(79, 0, &ent) && ent.seq == 11,
              "I8: the directory records the pushed capture's sequence");

        /* An unquiesced record never crosses: the stream's own gate, fired
         * before a byte is read. */
        struct EnvCkptRecord cold = mk_record(80, 0, 5, 901);
        cold.flags &= ~ENV_CKPT_FLAG_QUIESCED;
        g_loopback = 1;
        CHECK(env_replica_push(&cold) == 0,
              "I9: an unquiesced record is refused at the door");
        g_loopback = 0;
        CHECK(!env_replica_present(80, 0), "I10: the refused push replicated nothing");

        /* The confirm's honesty: a peer whose finalizer never ran can stage
         * EVERY page and still must not be counted as holding a copy. */
        struct EnvCkptRecord noc = rec2;
        noc.partition_id = 81;
        noc.env_id = 902;
        noc.sequence = 12;
        g_loopback = 1;
        g_loopback_tick = 0;   /* the peer's service poll is wedged */
        capture_reset();
        CHECK(env_replica_push(&noc) == 0,
              "I11: staged-but-never-finalized counts ABSENT, not held");
        g_loopback = 0;
        g_loopback_tick = 1;
        {
            /* The ladder must RE-ASK after an honest "not yet": the live
             * run (2026-10-28) found the first reply returning from the
             * WHOLE function — one question, one "absent", and every push
             * on the cluster reported NOT finalized while the peer
             * finalized a tick later. This peer answers "not yet" every
             * time (its finalizer never runs), so more than one question
             * must have gone on the wire. */
            int asks = 0;
            for (int i = 0; i < captured_frame_count; i++) {
                if (captured_frame_len[i] <
                    ETH_HDR_LEN + sizeof(struct DSPPEnvMigrateHeader)) continue;
                const struct DSPPEnvMigrateHeader* h =
                    (const struct DSPPEnvMigrateHeader*)
                        (captured_frame[i] + ETH_HDR_LEN);
                if (h->magic != DSPP_MIGRATE_MAGIC) continue;
                if (h->opcode == DSPP_MIGRATE_ENV_REPLICA_LIVE_REQ) asks++;
            }
            CHECK(asks >= 2,
                  "I11b: the confirm re-asks after a 'not yet' reply instead of ending the ladder");
        }
        CHECK(!env_replica_present(81, 0),
              "I12: staging alone never becomes a findable replica");
        env_replica_reset();   /* drop the wedged staging the tick never took */

        cluster_roster_count = 0;
        CHECK(env_replica_push(&rec2) == 0, "I13: an empty roster pushes nowhere");
        cluster_roster_count = 1;
    }
    g_fake_local_node_id = 0;
    cluster_roster_count = 0;

    /* ── J: PROMOTION — the band's only reader ────────────────────────── */
    /* The fixture is section I's own product: a finalized replica at
     * (79, 0) — five wire-ordered pages standing in the band, sequence 11
     * in the directory — which is exactly what a survivor holds when it
     * adopts the dead node's partition. Promotion must move those bytes
     * into the MIGRATION receiver's staging (one numbering, one reader —
     * the whole reason the band stores wire order rather than the payload
     * blob) and leave a slot the restore can find COMPLETE. Every refusal
     * must be named, latched with its operands, and leave nothing staged:
     * refusal over partial application, made mechanical. */
    printf("-- J: promotion, band -> migration staging --\n");
    env_migrate_reset();   /* the staging table this section feeds */
    {
        CHECK(env_replica_present(79, 0), "J0: section I's finalized replica is the fixture");

        int prc = env_replica_promote(79, 0);
        CHECK(prc == ENV_REPLICA_PROMOTE_OK,
              "*** J1: promotion stages the replica into the migration receiver ***");

        const struct EnvMigrateStaging* v = env_migrate_recv_find(79, 0);
        CHECK(v != 0 && v->complete,
              "J2: the staged slot is COMPLETE — the restore's own precondition");
        CHECK(v != 0 && v->total_pages == PAYLOAD_PAGES &&
              v->record.partition_id == 79 && v->record.env_id == 900,
              "J3: the slot carries the record that crossed and the band's page count");

        int exact = (v != 0);
        if (v) {
            static uint8_t jbuf[4096] __attribute__((aligned(4096)));
            for (uint32_t p = 0; exact && p < v->total_pages; p++) {
                const uint8_t* sp = env_migrate_recv_page_ptr(79, 0, p);
                if (!sp || env_replica_read_page(79, 0, p, jbuf) != 0 ||
                    memcmp(sp, jbuf, 4096) != 0)
                    exact = 0;
            }
        }
        CHECK(exact,
              "*** J4: every staged page is byte-exact against the band — the wire numbering has ONE reader ***");

        /* The handoff: the restore's first gates find what promote laid
         * down. This world has no environment to finish building, so the
         * restore is expected to refuse somewhere PAST the slot gates —
         * what must never happen is NO_SLOT or INCOMPLETE. */
        int rrc = env_migrate_restore(79, 0);
        CHECK(rrc != ENV_MIGRATE_REFUSE_NO_SLOT &&
              rrc != ENV_MIGRATE_REFUSE_INCOMPLETE,
              "*** J5: env_migrate_restore() finds the promoted slot staged and complete — stage and replay are one path ***");
        env_migrate_recv_drop(79, 0);

        /* Absent, by name, with nothing staged — and latched. */
        CHECK(env_replica_promote(79, 1) == ENV_REPLICA_PROMOTE_REFUSE_ABSENT,
              "J6: an identity the directory does not hold answers ABSENT by name");
        CHECK(env_migrate_recv_find(79, 1) == 0, "...and stages nothing for it");
        CHECK(env_replica_promotion_last().code ==
                  ENV_REPLICA_PROMOTE_REFUSE_ABSENT,
              "...the refusal is LATCHED with its code — a guard reads the code, never a bare nonzero");

        /* The device gate, before a frame is charged. */
        io_sq = 0;
        CHECK(env_replica_promote(79, 0) == ENV_REPLICA_PROMOTE_REFUSE_IO,
              "J7: with no device the promotion is refused BEFORE anything is staged");
        io_sq = (void*)1;
        CHECK(env_migrate_recv_find(79, 0) == 0,
              "...and the staging table is untouched");

        /* The migration receiver's ceiling propagates BY NAME: fill its
         * staging table (two slots; zero-page identities, so no frames) and
         * the feed is refused outright rather than half-taken. */
        {
            struct EnvCkptRecord z;
            memset(&z, 0, sizeof z);
            z.magic        = ENV_CKPT_REC_MAGIC;
            z.version      = ENV_CKPT_REC_VERSION;
            z.size         = (uint32_t)sizeof z;
            z.sequence     = 1;
            z.partition_id = 90;
            z.index        = 0;
            z.env_id       = 950;
            struct EnvCkptRecord z1 = z;
            z1.partition_id = 91;
            z1.env_id       = 951;
            uint64_t t0 = ((uint64_t)90 << 32) | 0;
            uint64_t t1 = ((uint64_t)91 << 32) | 0;
            CHECK(env_migrate_recv_begin(t0, 90, 0, 0, &z, sizeof z) == 0 &&
                  env_migrate_recv_begin(t1, 91, 0, 0, &z1, sizeof z1) == 0,
                  "J8: the migration staging table is filled to its ceiling");
            CHECK(env_replica_promote(79, 0) ==
                      ENV_REPLICA_PROMOTE_REFUSE_STAGE,
                  "*** J9: a full staging table refuses the promotion BY NAME — nothing partial is left ***");
            CHECK(env_replica_promotion_last().a == 6,
                  "...with the receiver's own table-full code carried through (a=6, propagation not paraphrase)");
            env_migrate_recv_drop(90, 0);
            env_migrate_recv_drop(91, 0);
        }
        CHECK(env_replica_promote(79, 0) == ENV_REPLICA_PROMOTE_OK,
              "J10: with the table free again the promotion succeeds — the refusal was a state, not a stain");
        env_migrate_recv_drop(79, 0);

        /* A directory entry that disagrees with its own record: patched
         * into the REAL entry frame and restored afterwards, so the guard
         * sees the same bytes an operator's disk would. */
        {
            static uint8_t frame[4096] __attribute__((aligned(4096)));
            struct EnvReplicaDirEntry* ents =
                (struct EnvReplicaDirEntry*)frame;
            int slot_idx = -1;
            CHECK(nvme_read_sync(PERSIST_ENV_REPLICA_ENT_LBA, frame) == 0,
                  "J11: the directory's entry frame reads");
            for (uint32_t i = 0; i < ENV_REPLICA_MAX; i++)
                if ((ents[i].flags & ENV_REPLICA_F_VALID) &&
                    ents[i].partition_id == 79 && ents[i].index == 0) {
                    slot_idx = i;
                    break;
                }
            CHECK(slot_idx >= 0, "J12: the replica's own entry is in that frame");
            if (slot_idx >= 0) {
                struct EnvReplicaDirEntry saved = ents[slot_idx];

                ents[slot_idx].pages = PAYLOAD_PAGES + ENV_MIGRATE_IMAGE_MAX + 1;
                CHECK(nvme_write_sync(PERSIST_ENV_REPLICA_ENT_LBA, frame) == 0,
                      "J13: the entry is patched with a page count its record cannot back");
                CHECK(env_replica_promote(79, 0) ==
                          ENV_REPLICA_PROMOTE_REFUSE_PAGES,
                      "*** J14: a page count outside the record's own range is refused BEFORE any staging ***");
                CHECK(env_migrate_recv_find(79, 0) == 0,
                      "...with nothing staged");

                ents[slot_idx] = saved;   /* back to real pages, broken record */
                ents[slot_idx].record.magic = 0;
                CHECK(nvme_write_sync(PERSIST_ENV_REPLICA_ENT_LBA, frame) == 0,
                      "J15: then the entry's record is broken instead");
                CHECK(env_replica_promote(79, 0) ==
                          ENV_REPLICA_PROMOTE_REFUSE_RECORD,
                      "...and an entry whose record fails validation is refused by name too");
                CHECK(env_migrate_recv_find(79, 0) == 0,
                      "...staging nothing");

                ents[slot_idx] = saved;   /* restore the directory whole */
                CHECK(nvme_write_sync(PERSIST_ENV_REPLICA_ENT_LBA, frame) == 0,
                      "J16: the directory is restored");
                CHECK(env_replica_promote(79, 0) == ENV_REPLICA_PROMOTE_OK,
                      "J17: the restored entry promotes again — the refusals were about the bytes, not a latch");
                env_migrate_recv_drop(79, 0);
            }
        }
    }

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
