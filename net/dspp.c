/*
 * dspp.c — Multi-Node Partition Scaling Roadmap Phase 5: real DSPP
 * page-routing logic. See dspp.h's own header comment for the design
 * writeup and the honest "no RX dispatcher wired up yet" caveat this
 * phase's own findings addendum in the roadmap doc also names.
 */
#include "dspp.h"
#include "consensus.h"
#include "net.h"
#include "e1000.h"
#include "../kernel/object_catalog.h"
#include "../kernel/partition.h"
#include "../kernel/stream.h"   // Multi-Node Partition Scaling Roadmap Phase 7 -- stream_migrate_recv_begin()/_page()
#include "../kernel/simi_ctx_migrate.h"  // PEC Phase 3 -- simi_ctx_migrate_recv_begin()/_chunk()
#include "../kernel/kernel_io.h"

/* The consensus handlers below need a wall-clock reading to restart election
 * timers on an accepted heartbeat or a granted vote (net/consensus.h's
 * "Election timing" block explains why a call count will not do).
 *
 * Declared extern here rather than threaded down as a parameter from
 * net_rx_dispatch(): the RX path is three layers deep (e1000_poll_rx ->
 * net_rx_dispatch -> here), none of the intermediate layers has any use for
 * the value, and widening all of them would break the existing self-echo
 * host test for no benefit. kernel/journal.c reads the counter the same way
 * for the same reason. */
extern volatile uint64_t kernel_tick_counter;

/* See dspp.h. Non-zero means some DSPP message is structurally too large
 * for this link. */
uint64_t dspp_tx_oversize_dropped = 0;

/* See dspp.h. No kernel code assigns to this -- grep before adding one. */
uint16_t dspp_max_wire_payload = DSPP_MAX_WIRE_PAYLOAD;

/* dspp.h has to spell DSPP_MAX_WIRE_PAYLOAD as a literal (it cannot include
 * net.h for ETH_HDR_LEN). This is where the two are held together: if
 * either the MTU or the Ethernet header size ever changes, the build stops
 * rather than the limit quietly becoming wrong in the direction that emits
 * undeliverable frames again. */
_Static_assert(DSPP_MAX_WIRE_PAYLOAD == DSPP_LINK_MTU - ETH_HDR_LEN,
               "DSPP_MAX_WIRE_PAYLOAD has drifted from DSPP_LINK_MTU - ETH_HDR_LEN");

uint32_t dspp_resolve_partition_id(uint64_t system_object_id) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && object_catalog[i].object_id == system_object_id)
            return object_catalog[i].partition_id;
    }
    return 0;   /* not found -- honest "no object, no partition" default (0 == PARTITION_SYSTEM) */
}

int dspp_page_read_allowed(uint32_t partition_id) {
    return partition_is_local(partition_id);
}

int dspp_page_write_allowed(uint32_t partition_id) {
    return partition_is_local(partition_id) && partition_holds_write_lease(partition_id);
}

int process_dspp_page_packet(struct DSPPFullPagePacket* packet) {
    uint32_t partition_id = dspp_resolve_partition_id(packet->header.system_object_id);

    if (packet->header.opcode == DSPP_PAGE_READ_REQ) {
        if (!dspp_page_read_allowed(partition_id)) {
            kernel_serial_printf(
                "[DSPP] READ_REQ denied for object %llu (partition %u not local).\n",
                (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
            return 0;
        }
        /* Real implementation: resolve system_object_id/virtual_address to
         * a physical frame and transmit a DSPP_PAGE_READ_ACK carrying it.
         * No such object-to-physical-frame resolution plumbing exists
         * anywhere in this codebase yet -- a separate, larger integration
         * this phase does not attempt. This phase closes the routing/
         * gating gap (should this request even be serviced here), not the
         * page-move plumbing gap (how the bytes would actually get onto
         * the wire). */
        kernel_serial_printf(
            "[DSPP] READ_REQ for object %llu (partition %u, local) -- allowed.\n",
            (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
        return 1;
    }

    if (packet->header.opcode == DSPP_PAGE_WRITE_REQ) {
        if (!dspp_page_write_allowed(partition_id)) {
            kernel_serial_printf(
                "[DSPP] WRITE_REQ denied for object %llu (partition %u: not local and/or no write lease held).\n",
                (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
            return 0;
        }
        kernel_serial_printf(
            "[DSPP] WRITE_REQ for object %llu (partition %u, local + write lease held) -- allowed.\n",
            (unsigned long long)packet->header.system_object_id, (unsigned)partition_id);
        return 1;
    }

    return 0;   /* not a page-routing opcode this function handles */
}

// ─── Multi-Node Partition Scaling Roadmap Phase 7: real cross-node data
// movement. See dspp.h's own header comment for the full design writeup. ──

static void dspp_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void dspp_strncpy(char* d, const char* s, uint32_t n) {
    uint32_t i; for (i = 0; i + 1 < n && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}

void dspp_transmit_raw(const void* dspp_payload, uint16_t dspp_len) {
    // Static, not stack -- a full DSPPMigratePagePacket/DSPPFullPagePacket
    // plus its Ethernet header is several KiB, more than this freestanding
    // kernel's stack discipline elsewhere budgets for a single local
    // (mirrors kernel/stream.c's own dir_buf[4096]/reloc_src_page[4096]
    // static-not-stack convention for similarly oversized buffers). Sized
    // to the LARGER of the two packet families -- struct DSPPMigratePage
    // Packet (4273 bytes: its header carries stream metadata alongside the
    // page, unlike DSPPPacketHeader's leaner 36 bytes) exceeds struct
    // DSPPFullPagePacket (4132 bytes). Sizing this off only the older
    // struct was a real bug caught by this phase's own host test
    // (cross_node_migration_host_test.c): every DSPP_MIGRATE_PAGE_REQ was
    // silently dropped by the very next guard below, since 4257 (its real
    // dspp_len) > 4132 - ETH_HDR_LEN. Fixed by sizing off whichever struct
    // is actually larger, verified with a real sizeof() check rather than
    // eyeballing it, the same discipline this codebase applies to LBA/frame
    // arithmetic elsewhere.
    static uint8_t frame_buf[ETH_HDR_LEN + (sizeof(struct DSPPMigratePagePacket) > sizeof(struct DSPPFullPagePacket)
                                             ? sizeof(struct DSPPMigratePagePacket)
                                             : sizeof(struct DSPPFullPagePacket))];
    if ((uint32_t)dspp_len > sizeof(frame_buf) - ETH_HDR_LEN) {
        kernel_serial_printf("[DSPP] transmit_raw: dspp_len %u exceeds max frame payload -- dropped, not truncated.\n",
                             (unsigned)dspp_len);
        return;
    }

    /* The link limit, which is far below the buffer limit above and is the
     * one that actually decides whether a frame arrives. See dspp.h for the
     * full account -- briefly: without RCTL.LPE the receiving e1000 discards
     * anything over 1522 bytes, and e1000_poll_rx() has no descriptor
     * chaining, so a 4 KB frame has nowhere to land regardless.
     *
     * Refusing here rather than at the driver is deliberate. e1000_transmit()
     * would hand the descriptor to hardware and return success; the frame
     * would simply never be seen by anyone. A cluster that cannot converge
     * because its votes evaporate is far harder to diagnose than a counter
     * that says so. */
    if ((uint32_t)dspp_len > dspp_max_wire_payload) {
        dspp_tx_oversize_dropped++;
        /* Logged only on the first occurrence and then every 1000th: this
         * sits under a per-page transfer loop, so an unthrottled print
         * would bury the console and slow the very path being diagnosed. */
        if (dspp_tx_oversize_dropped == 1 || (dspp_tx_oversize_dropped % 1000) == 0) {
            kernel_serial_printf(
                "[DSPP] opcode payload of %u B exceeds the %u B link MTU -- dropped (%llu total). "
                "This message cannot cross an Ethernet segment without jumbo frames or "
                "protocol-level fragmentation.\n",
                (unsigned)dspp_len, (unsigned)dspp_max_wire_payload,
                (unsigned long long)dspp_tx_oversize_dropped);
        }
        return;
    }

    struct EthernetHeader* eth = (struct EthernetHeader*)frame_buf;
    // Broadcast destination -- see this function's own header comment
    // (dspp.h) on why: no node-id-to-MAC resolution table exists anywhere
    // in this codebase, so every DSPP frame is L2-broadcast, with
    // point-to-point delivery (where it matters -- migration) self-filtered
    // one layer up via struct DSPPMigrateHeader's node_dest_id field.
    static const MACAddr dspp_bcast_mac = {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};
    eth->dst       = dspp_bcast_mac;
    eth->src       = net_my_mac;
    eth->ethertype = htons(ETHERTYPE_DSPP);
    dspp_memcpy(frame_buf + ETH_HDR_LEN, dspp_payload, dspp_len);

    /* DSPP is L2-only and belongs on the cluster segment, never the
     * management NIC -- see net/e1000.h on why this is a role and not a
     * route. */
    e1000_transmit(NIC_ROLE_CLUSTER, frame_buf, (uint16_t)(ETH_HDR_LEN + dspp_len));
}

void dspp_migrate_send_begin(uint64_t transfer_id, uint32_t node_dest_id,
                              uint32_t partition_id, const char* name,
                              const char* mime_type, uint64_t size,
                              uint32_t frames_used, uint32_t owner_uid) {
    struct DSPPMigrateHeader req;
    req.magic           = DSPP_MIGRATE_MAGIC;
    req.opcode          = DSPP_MIGRATE_BEGIN_REQ;
    req.node_source_id  = (uint16_t)cluster_local_node_id();
    req.node_dest_id    = node_dest_id;
    req.transfer_id     = transfer_id;
    req.partition_id    = partition_id;
    req.page_index      = 0;
    req.status          = 0;
    dspp_strncpy(req.stream_name, name, sizeof(req.stream_name));
    dspp_strncpy(req.stream_mime_type, mime_type, sizeof(req.stream_mime_type));
    req.stream_size        = size;
    req.stream_frames_used = frames_used;
    req.stream_owner_uid   = owner_uid;

    dspp_transmit_raw(&req, (uint16_t)sizeof(req));
}

void dspp_migrate_send_page(uint64_t transfer_id, uint32_t node_dest_id,
                             uint32_t partition_id, uint32_t page_index,
                             const uint8_t* page_data) {
    struct DSPPMigratePagePacket pkt;
    pkt.header.magic          = DSPP_MIGRATE_MAGIC;
    pkt.header.opcode         = DSPP_MIGRATE_PAGE_REQ;
    pkt.header.node_source_id = (uint16_t)cluster_local_node_id();
    pkt.header.node_dest_id   = node_dest_id;
    pkt.header.transfer_id    = transfer_id;
    pkt.header.partition_id   = partition_id;
    pkt.header.page_index     = page_index;
    pkt.header.status         = 0;
    pkt.header.stream_name[0]      = '\0';   // not meaningful for PAGE_REQ, zeroed rather than left garbage
    pkt.header.stream_mime_type[0] = '\0';
    pkt.header.stream_size         = 0;
    pkt.header.stream_frames_used  = 0;
    pkt.header.stream_owner_uid    = 0;
    dspp_memcpy(pkt.page_data, page_data, 4096);

    dspp_transmit_raw(&pkt, (uint16_t)sizeof(pkt));
}

// Sends a DSPP_MIGRATE_BEGIN_ACK/PAGE_ACK back to whoever sent us the
// request just processed -- addressed to their node_source_id, which
// becomes our node_dest_id for the reply, the same source/dest swap every
// request/reply protocol uses.
static void dspp_migrate_send_ack(uint16_t opcode, uint32_t reply_to_node,
                                   uint64_t transfer_id, uint32_t partition_id,
                                   uint32_t page_index, uint8_t status) {
    struct DSPPMigrateHeader ack;
    ack.magic           = DSPP_MIGRATE_MAGIC;
    ack.opcode          = opcode;
    ack.node_source_id  = (uint16_t)cluster_local_node_id();
    ack.node_dest_id    = reply_to_node;
    ack.transfer_id     = transfer_id;
    ack.partition_id    = partition_id;
    ack.page_index      = page_index;
    ack.status          = status;
    ack.stream_name[0]      = '\0';
    ack.stream_mime_type[0] = '\0';
    ack.stream_size         = 0;
    ack.stream_frames_used  = 0;
    ack.stream_owner_uid    = 0;

    dspp_transmit_raw(&ack, (uint16_t)sizeof(ack));
}

void dspp_migrate_rx(struct DSPPMigratePagePacket* packet, uint16_t len) {
    if (!packet || len < sizeof(struct DSPPMigrateHeader)) return;
    struct DSPPMigrateHeader* h = &packet->header;

    // Self-filter: this protocol family is always point-to-point (never
    // legitimately broadcast), and this codebase has no L2 addressing for
    // a specific node -- see struct DSPPMigrateHeader's own comment. A
    // frame not addressed to us is silently ignored, not an error.
    if (h->node_dest_id != cluster_local_node_id()) return;

    if (h->opcode == DSPP_MIGRATE_BEGIN_REQ) {
        int rc = stream_migrate_recv_begin(h->transfer_id, h->partition_id,
                                            h->stream_name, h->stream_mime_type,
                                            h->stream_size, h->stream_frames_used,
                                            h->stream_owner_uid);
        dspp_migrate_send_ack(DSPP_MIGRATE_BEGIN_ACK, h->node_source_id,
                              h->transfer_id, h->partition_id, 0,
                              (uint8_t)(rc == 0 ? 0 : 1));
        return;
    }

    if (h->opcode == DSPP_MIGRATE_PAGE_REQ) {
        if (len < sizeof(struct DSPPMigratePagePacket)) return;  // truncated -- page_data not actually present
        int rc = stream_migrate_recv_page(h->transfer_id, h->page_index, packet->page_data);
        dspp_migrate_send_ack(DSPP_MIGRATE_PAGE_ACK, h->node_source_id,
                              h->transfer_id, h->partition_id, h->page_index,
                              (uint8_t)(rc == 0 ? 0 : 1));
        return;
    }

    // DSPP_MIGRATE_BEGIN_ACK/PAGE_ACK: honestly a no-op here -- see this
    // file's own "fire-and-forget" scope note in dspp.h. Received and
    // silently discarded, not misrouted.
}

/* ─── Live execution-context migration (PEC Phase 3) ──────────────────
 * Wire encode/decode only; kernel/simi_ctx_migrate.c owns what the bytes
 * mean, exactly as kernel/stream.c does for the stream family above.
 *
 * These static assertions enforce the prefix-compatibility contract
 * documented in dspp.h: dspp_rx_dispatch() must read magic, opcode and
 * node_dest_id out of a buffer BEFORE it knows which of the two migrate
 * headers it holds, so those fields must sit at identical offsets. Left
 * to a comment this would drift the first time either struct is edited,
 * and the symptom would be misrouted or silently dropped packets rather
 * than anything that points at the cause. As a static assert it is a
 * compile error instead. */
#define DSPP_SAME_OFFSET(f) \
    _Static_assert(__builtin_offsetof(struct DSPPCtxMigrateHeader, f) == \
                   __builtin_offsetof(struct DSPPMigrateHeader, f), \
                   "DSPP migrate header prefix drifted: " #f)
DSPP_SAME_OFFSET(magic);
DSPP_SAME_OFFSET(opcode);
DSPP_SAME_OFFSET(node_source_id);
DSPP_SAME_OFFSET(node_dest_id);
DSPP_SAME_OFFSET(transfer_id);
DSPP_SAME_OFFSET(partition_id);
DSPP_SAME_OFFSET(status);
/* chunk_index deliberately overlays page_index -- same offset, same
 * meaning (which piece of the transfer this is), different name. */
_Static_assert(__builtin_offsetof(struct DSPPCtxMigrateHeader, chunk_index) ==
               __builtin_offsetof(struct DSPPMigrateHeader, page_index),
               "DSPP migrate header prefix drifted: chunk_index/page_index");
#define DSPP_SVC_SAME_OFFSET(f) \
    _Static_assert(__builtin_offsetof(struct DSPPServiceHeader, f) == \
                   __builtin_offsetof(struct DSPPMigrateHeader, f), \
                   "DSPP service header prefix drifted: " #f)
DSPP_SVC_SAME_OFFSET(magic);
DSPP_SVC_SAME_OFFSET(opcode);
DSPP_SVC_SAME_OFFSET(node_source_id);
DSPP_SVC_SAME_OFFSET(node_dest_id);
DSPP_SVC_SAME_OFFSET(transfer_id);
DSPP_SVC_SAME_OFFSET(partition_id);
DSPP_SVC_SAME_OFFSET(status);
#undef DSPP_SVC_SAME_OFFSET
#undef DSPP_SAME_OFFSET

static void dspp_ctx_hdr_init(struct DSPPCtxMigrateHeader* h, uint16_t opcode,
                              uint64_t transfer_id, uint32_t node_dest_id,
                              uint32_t partition_id, uint32_t chunk_index) {
    h->magic          = DSPP_MIGRATE_MAGIC;
    h->opcode         = opcode;
    h->node_source_id = (uint16_t)cluster_local_node_id();
    h->node_dest_id   = node_dest_id;
    h->transfer_id    = transfer_id;
    h->partition_id   = partition_id;
    h->chunk_index    = chunk_index;
    h->status         = 0;
    /* Tail zeroed rather than left as stack garbage, matching
     * dspp_migrate_send_page()'s treatment of the stream-only fields. */
    h->ctx_name[0]    = '\0';
    h->total_bytes    = 0;
    h->total_chunks   = 0;
    h->chunk_bytes    = 0;
    h->program_hash   = 0;
}

void dspp_ctx_migrate_send_begin(uint64_t transfer_id, uint32_t node_dest_id,
                                 uint32_t partition_id, const char* ctx_name,
                                 uint64_t total_bytes, uint32_t total_chunks,
                                 uint64_t program_hash) {
    struct DSPPCtxMigrateHeader req;
    dspp_ctx_hdr_init(&req, DSPP_MIGRATE_CTX_BEGIN_REQ, transfer_id,
                      node_dest_id, partition_id, 0);
    dspp_strncpy(req.ctx_name, ctx_name ? ctx_name : "", sizeof(req.ctx_name));
    req.total_bytes  = total_bytes;
    req.total_chunks = total_chunks;
    req.program_hash = program_hash;

    dspp_transmit_raw(&req, (uint16_t)sizeof(req));
}

void dspp_ctx_migrate_send_chunk(uint64_t transfer_id, uint32_t node_dest_id,
                                 uint32_t partition_id, uint32_t chunk_index,
                                 const uint8_t* data, uint32_t chunk_bytes) {
    if (!data || chunk_bytes == 0 || chunk_bytes > DSPP_CTX_CHUNK_BYTES) return;

    struct DSPPCtxMigrateChunkPacket pkt;
    dspp_ctx_hdr_init(&pkt.header, DSPP_MIGRATE_CTX_CHUNK_REQ, transfer_id,
                      node_dest_id, partition_id, chunk_index);
    pkt.header.chunk_bytes = chunk_bytes;

    dspp_memcpy(pkt.chunk_data, data, chunk_bytes);
    /* The final chunk is short. Zero the remainder rather than shipping
     * whatever was on the stack: the trailing bytes are outside
     * chunk_bytes and so never reassembled, but they would otherwise put
     * unrelated kernel memory on the wire. */
    for (uint32_t i = chunk_bytes; i < DSPP_CTX_CHUNK_BYTES; i++)
        pkt.chunk_data[i] = 0;

    dspp_transmit_raw(&pkt, (uint16_t)sizeof(pkt));
}

static void dspp_ctx_migrate_send_ack(uint16_t opcode, uint32_t reply_to_node,
                                      uint64_t transfer_id, uint32_t partition_id,
                                      uint32_t chunk_index, uint8_t status) {
    struct DSPPCtxMigrateHeader ack;
    dspp_ctx_hdr_init(&ack, opcode, transfer_id, reply_to_node,
                      partition_id, chunk_index);
    ack.node_dest_id = reply_to_node;
    ack.status       = status;

    dspp_transmit_raw(&ack, (uint16_t)sizeof(ack));
}

void dspp_ctx_migrate_rx(struct DSPPCtxMigrateChunkPacket* packet, uint16_t len) {
    if (!packet || len < sizeof(struct DSPPCtxMigrateHeader)) return;
    struct DSPPCtxMigrateHeader* h = &packet->header;

    /* Same self-filter, same reasoning, as dspp_migrate_rx(). */
    if (h->node_dest_id != cluster_local_node_id()) return;

    if (h->opcode == DSPP_MIGRATE_CTX_BEGIN_REQ) {
        SimiCtxMigStatus rc = simi_ctx_migrate_recv_begin(
            h->transfer_id, h->ctx_name, h->total_bytes,
            h->total_chunks, h->program_hash);
        dspp_ctx_migrate_send_ack(DSPP_MIGRATE_CTX_BEGIN_ACK, h->node_source_id,
                                  h->transfer_id, h->partition_id, 0, (uint8_t)rc);
        return;
    }

    if (h->opcode == DSPP_MIGRATE_CTX_CHUNK_REQ) {
        if (len < sizeof(struct DSPPCtxMigrateChunkPacket)) return;  // truncated -- chunk_data not actually present
        SimiCtxMigStatus rc = simi_ctx_migrate_recv_chunk(
            h->transfer_id, h->chunk_index, packet->chunk_data, h->chunk_bytes);
        dspp_ctx_migrate_send_ack(DSPP_MIGRATE_CTX_CHUNK_ACK, h->node_source_id,
                                  h->transfer_id, h->partition_id,
                                  h->chunk_index, (uint8_t)rc);
        return;
    }

    // CTX_BEGIN_ACK/CTX_CHUNK_ACK: no-op, as with the stream family.
}


/* ─── Service-registry replication ────────────────────────────────────
 * See dspp.h for why this announces rather than queries, and why remote
 * entries live in their own table. */
static void dspp_svc_send(uint16_t opcode, const char* name, uint32_t partition_id,
                          uint8_t endpoint_kind, uint32_t endpoint_port,
                          uint32_t owner_uid, uint8_t serving) {
    struct DSPPServiceHeader h;
    h.magic          = DSPP_MIGRATE_MAGIC;
    h.opcode         = opcode;
    h.node_source_id = (uint16_t)cluster_local_node_id();
    h.node_dest_id   = 0;              /* broadcast -- everyone caches this */
    h.transfer_id    = 0;
    h.partition_id   = partition_id;
    h.chunk_index    = 0;
    h.status         = 0;
    dspp_strncpy(h.service_name, name ? name : "", sizeof(h.service_name));
    h.endpoint_port  = endpoint_port;
    h.endpoint_kind  = endpoint_kind;
    h.owner_uid      = owner_uid;
    h.serving        = serving;

    dspp_transmit_raw(&h, (uint16_t)sizeof(h));
}

void dspp_service_announce(const char* name, uint32_t partition_id,
                           uint8_t endpoint_kind, uint32_t endpoint_port,
                           uint32_t owner_uid, uint8_t serving) {
    /* Silent on a node with no cluster identity. node id 0 is Phase 1's
     * "uninitialized" sentinel, so announcing would tell the segment a
     * service belongs to a node that does not exist -- worse than saying
     * nothing, because a listener would cache it. */
    if (cluster_local_node_id() == 0) return;
    dspp_svc_send(DSPP_SVC_ANNOUNCE, name, partition_id, endpoint_kind,
                  endpoint_port, owner_uid, serving);
}

void dspp_service_withdraw(const char* name) {
    if (cluster_local_node_id() == 0) return;
    dspp_svc_send(DSPP_SVC_WITHDRAW, name, 0, 0, 0, 0, 0);
}

void dspp_service_rx(struct DSPPServiceHeader* h, uint16_t len) {
    if (!h || len < sizeof(struct DSPPServiceHeader)) return;

    /* Ignore our own broadcast. Unlike the migrate families this is not
     * addressed to anyone, so the self-filter is on the SOURCE: caching
     * our own announcement as a remote entry would shadow the local one
     * it came from. */
    if (h->node_source_id == (uint16_t)cluster_local_node_id()) return;

    if (h->opcode == DSPP_SVC_ANNOUNCE) {
        service_remote_learn(h->service_name, h->node_source_id, h->partition_id,
                             h->endpoint_kind, h->endpoint_port, h->owner_uid,
                             h->serving);
    } else if (h->opcode == DSPP_SVC_WITHDRAW) {
        service_remote_forget(h->service_name, h->node_source_id);
    }
}

void dspp_rx_dispatch(void* buf, uint16_t len) {
    if (!buf || len < sizeof(uint64_t)) return;
    uint64_t magic;
    dspp_memcpy(&magic, buf, sizeof(magic));

    if (magic == DSPP_MIGRATE_MAGIC) {
        /* Two header layouts share this magic, and they are different
         * SIZES -- a context header is smaller than a stream one. So the
         * minimum-length check has to come AFTER reading the opcode, not
         * before: gating the whole family on sizeof(DSPPMigrateHeader)
         * would silently drop every legitimate context BEGIN packet for
         * being "too short". Reading opcode first is safe precisely
         * because of the prefix-compatibility asserts above. */
        if (len < sizeof(uint64_t) + sizeof(uint16_t)) return;
        uint16_t opcode;
        dspp_memcpy(&opcode, (uint8_t*)buf + sizeof(uint64_t), sizeof(opcode));

        if (opcode == DSPP_SVC_ANNOUNCE || opcode == DSPP_SVC_WITHDRAW) {
            if (len < sizeof(struct DSPPServiceHeader)) return;
            dspp_service_rx((struct DSPPServiceHeader*)buf, len);
            return;
        }

        if (opcode >= DSPP_MIGRATE_CTX_BEGIN_REQ && opcode <= DSPP_MIGRATE_CTX_CHUNK_ACK) {
            if (len < sizeof(struct DSPPCtxMigrateHeader)) return;
            dspp_ctx_migrate_rx((struct DSPPCtxMigrateChunkPacket*)buf, len);
            return;
        }

        if (len < sizeof(struct DSPPMigrateHeader)) return;
        dspp_migrate_rx((struct DSPPMigratePagePacket*)buf, len);
        return;
    }

    if (magic != DSPP_MAGIC) return;   // unrecognized (including the pre-Phase-5 DSPP_MAGIC_V1) -- silently dropped
    if (len < sizeof(struct DSPPPacketHeader)) return;
    struct DSPPFullPagePacket* pkt = (struct DSPPFullPagePacket*)buf;

    switch (pkt->header.opcode) {
        case DSPP_CMD_REQUEST_VOTE:
        case DSPP_CMD_VOTE_REPLY:
        case DSPP_CMD_HEARTBEAT:
            process_consensus_packet(pkt, kernel_tick_counter);
            return;
        case DSPP_CMD_PARTITION_REQUEST_VOTE:
        case DSPP_CMD_PARTITION_VOTE_REPLY:
        case DSPP_CMD_PARTITION_HEARTBEAT:
            process_partition_consensus_packet(pkt, kernel_tick_counter);
            return;
        case DSPP_PAGE_READ_REQ:
        case DSPP_PAGE_WRITE_REQ:
            process_dspp_page_packet(pkt);
            return;
        default:
            // DSPP_PAGE_READ_ACK/WRITE_ACK and anything else: no-op, same
            // "fire-and-forget, nothing blocks on an incoming ACK yet"
            // posture as the migrate family above.
            return;
    }
}
