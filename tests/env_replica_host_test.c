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
#include "kernel/frame_pool.h"
#include "kernel/object_catalog.h"
#include "kernel/partition.h"
#include "net/e1000.h"
#include "net/dspp.h"
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

void failover_note_heartbeat(uint32_t node_id, uint64_t now) {
    (void)node_id; (void)now;
}
int failover_peer_status(uint32_t node_id) { (void)node_id; return -1; }
void failover_sweep_keepalive(void) { }

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
#define MAX_CAPTURED_FRAMES 64
#define FRAME_STRIDE (ETH_HDR_LEN + (unsigned)sizeof(struct DSPPEnvMigrateHeader))
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
static void capture_reset(void) {
    captured_frame_count = 0;
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
    dspp_env_migrate_rx(&pkt, (uint16_t)sizeof(pkt));
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
    dspp_env_migrate_rx(&pkt, (uint16_t)sizeof(pkt));
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

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
