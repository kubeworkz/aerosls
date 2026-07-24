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
#include "../kernel/kernel_io.h"

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

    e1000_transmit_packet(frame_buf, (uint16_t)(ETH_HDR_LEN + dspp_len));
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

void dspp_rx_dispatch(void* buf, uint16_t len) {
    if (!buf || len < sizeof(uint64_t)) return;
    uint64_t magic;
    dspp_memcpy(&magic, buf, sizeof(magic));

    if (magic == DSPP_MIGRATE_MAGIC) {
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
            process_consensus_packet(pkt);
            return;
        case DSPP_CMD_PARTITION_REQUEST_VOTE:
        case DSPP_CMD_PARTITION_VOTE_REPLY:
        case DSPP_CMD_PARTITION_HEARTBEAT:
            process_partition_consensus_packet(pkt);
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
