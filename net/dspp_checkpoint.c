/*
 * dspp_checkpoint.c — checkpoint transfer over DSPP
 * (Core Backup Strategies, Step 3).
 *
 * Sends/receives a serialized state tree as chunked DSPP packets, using
 * the same fire-and-forget model as the context migration family.
 *
 * Freestanding: local helpers, no libc.
 */
#include "dspp.h"
#include "../kernel/kernel_io.h"
#include "../net/net.h"
#include "../net/consensus.h"   /* cluster_local_node_id() */

/* ─── Local helpers ──────────────────────────────────────────────────── */
static void ck_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d;
    const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void ck_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    while (n--) *p++ = v;
}

/* ─── Receiver reassembly buffer ─────────────────────────────────────── */
#define CKPT_RECV_MAX  (24 * 1024)  /* 24 KiB: enough for ST_MAX_NODES=410 */

static uint8_t  ckpt_recv_buf[CKPT_RECV_MAX];
static uint32_t ckpt_recv_total    = 0;
static uint32_t ckpt_recv_got      = 0;
static uint32_t ckpt_recv_chunks   = 0;
static uint64_t ckpt_recv_seq      = 0;
static uint64_t ckpt_recv_xfer_id  = 0;
static uint8_t  ckpt_recv_active   = 0;
static uint8_t  ckpt_recv_complete = 0;

/* ─── Sender ─────────────────────────────────────────────────────────── */
void dspp_ckpt_send(uint64_t transfer_id, uint32_t node_dest_id,
                    uint64_t sequence, const uint8_t* data, uint32_t data_size) {
    if (!data || data_size == 0) return;

    uint32_t total_chunks = (data_size + DSPP_CKPT_CHUNK_BYTES - 1) / DSPP_CKPT_CHUNK_BYTES;

    /* Send BEGIN */
    struct DSPPCkptHeader begin;
    ck_memset(&begin, 0, sizeof(begin));
    begin.magic          = DSPP_MIGRATE_MAGIC;
    begin.opcode         = DSPP_CKPT_BEGIN_REQ;
    begin.node_source_id = (uint16_t)cluster_local_node_id();
    begin.node_dest_id   = node_dest_id;
    begin.transfer_id    = transfer_id;
    begin.sequence       = sequence;
    begin.total_bytes    = data_size;
    begin.total_chunks   = total_chunks;
    dspp_transmit_raw(&begin, sizeof(begin));

    /* Send chunks */
    for (uint32_t i = 0; i < total_chunks; i++) {
        struct DSPPCkptChunkPacket pkt;
        ck_memset(&pkt, 0, sizeof(pkt));
        pkt.header.magic          = DSPP_MIGRATE_MAGIC;
        pkt.header.opcode         = DSPP_CKPT_CHUNK_REQ;
        pkt.header.node_source_id = (uint16_t)cluster_local_node_id();
        pkt.header.node_dest_id   = node_dest_id;
        pkt.header.transfer_id    = transfer_id;
        pkt.header.chunk_index    = i;
        pkt.header.sequence       = sequence;

        uint32_t offset = i * DSPP_CKPT_CHUNK_BYTES;
        uint32_t remaining = data_size - offset;
        uint32_t chunk_sz = remaining < DSPP_CKPT_CHUNK_BYTES ? remaining : DSPP_CKPT_CHUNK_BYTES;
        pkt.header.chunk_bytes = chunk_sz;
        ck_memcpy(pkt.chunk_data, data + offset, chunk_sz);

        dspp_transmit_raw(&pkt, (uint16_t)(sizeof(struct DSPPCkptHeader) + chunk_sz));
    }

    kernel_serial_printf("[DSPP-CKPT] Sent seq=%llu (%u bytes, %u chunks) -> node %u\n",
                         sequence, data_size, total_chunks, node_dest_id);
}

/* ─── Receiver ───────────────────────────────────────────────────────── */
void dspp_ckpt_rx(struct DSPPCkptChunkPacket* packet, uint16_t len) {
    if (!packet || len < sizeof(struct DSPPCkptHeader)) return;

    struct DSPPCkptHeader* h = &packet->header;

    /* Self-filter: only accept packets addressed to this node */
    if (h->node_dest_id != cluster_local_node_id()) return;

    switch (h->opcode) {
    case DSPP_CKPT_BEGIN_REQ: {
        if (ckpt_recv_active) {
            /* Already receiving a transfer — refuse */
            kernel_serial_print("[DSPP-CKPT] RX: busy, refusing new transfer\n");
            return;
        }
        if (h->total_bytes > CKPT_RECV_MAX) {
            kernel_serial_printf("[DSPP-CKPT] RX: too large (%u > %u)\n",
                                 h->total_bytes, (uint32_t)CKPT_RECV_MAX);
            return;
        }
        ckpt_recv_active   = 1;
        ckpt_recv_complete = 0;
        ckpt_recv_total    = h->total_bytes;
        ckpt_recv_chunks   = h->total_chunks;
        ckpt_recv_seq      = h->sequence;
        ckpt_recv_xfer_id  = h->transfer_id;
        ckpt_recv_got      = 0;
        ck_memset(ckpt_recv_buf, 0, CKPT_RECV_MAX);
        kernel_serial_printf("[DSPP-CKPT] RX: BEGIN seq=%llu, %u bytes, %u chunks\n",
                             h->sequence, h->total_bytes, h->total_chunks);
        break;
    }
    case DSPP_CKPT_CHUNK_REQ: {
        if (!ckpt_recv_active || h->transfer_id != ckpt_recv_xfer_id) return;
        if (h->chunk_index >= ckpt_recv_chunks) return;

        uint32_t offset = h->chunk_index * DSPP_CKPT_CHUNK_BYTES;
        uint32_t chunk_sz = h->chunk_bytes;
        if (chunk_sz > DSPP_CKPT_CHUNK_BYTES) chunk_sz = DSPP_CKPT_CHUNK_BYTES;
        if (offset + chunk_sz > ckpt_recv_total) return;

        uint16_t needed = (uint16_t)(sizeof(struct DSPPCkptHeader) + chunk_sz);
        if (len < needed) return;

        ck_memcpy(ckpt_recv_buf + offset, packet->chunk_data, chunk_sz);
        ckpt_recv_got += chunk_sz;

        /* Check if complete */
        if (ckpt_recv_got >= ckpt_recv_total) {
            ckpt_recv_complete = 1;
            ckpt_recv_active   = 0;
            kernel_serial_printf("[DSPP-CKPT] RX: COMPLETE seq=%llu (%u bytes)\n",
                                 ckpt_recv_seq, ckpt_recv_got);
        }
        break;
    }
    case DSPP_CKPT_BEGIN_ACK:
    case DSPP_CKPT_CHUNK_ACK:
        /* Fire-and-forget: sender doesn't wait for ACKs in this phase */
        break;
    default:
        break;
    }
}

/* ─── Receiver query API ─────────────────────────────────────────────── */
int dspp_ckpt_recv_ready(void) { return ckpt_recv_complete; }

uint32_t dspp_ckpt_recv_size(void) {
    return ckpt_recv_complete ? ckpt_recv_total : 0;
}

void dspp_ckpt_recv_copy(uint8_t* out, uint32_t max) {
    if (!out || !ckpt_recv_complete) return;
    uint32_t n = ckpt_recv_total < max ? ckpt_recv_total : max;
    ck_memcpy(out, ckpt_recv_buf, n);
}

void dspp_ckpt_recv_reset(void) {
    ckpt_recv_active   = 0;
    ckpt_recv_complete = 0;
    ckpt_recv_total    = 0;
    ckpt_recv_got      = 0;
    ckpt_recv_seq      = 0;
}
