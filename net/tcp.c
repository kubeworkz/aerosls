#include "tcp.h"
#include "ipv4.h"
#include "../kernel/kernel_io.h"
#include "../kernel/net_event.h"  /* Phase E: HLT-based yield */

struct TCPConn tcp_conns[TCP_MAX_CONNS];

void tcp_init(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conns[i].active = 0;
        tcp_conns[i].state  = TCP_CLOSED;
        tcp_conns[i].conn_id = (uint8_t)i;
    }
}

// ─── TCP pseudo-header checksum ───────────────────────────────────────────────
uint16_t tcp_checksum(IPv4Addr src, IPv4Addr dst,
                       struct TCPHeader* seg, uint16_t seg_len) {
    // Build the 12-byte TCP pseudo-header in a byte array (network byte order
    // = big-endian).  Then sum ALL 16-bit words identically — by reading each
    // pair of bytes as (bytes[0]<<8 | bytes[1]) to get the network-order value.
    // The TCP segment bytes are ALSO in network byte order in memory, so we sum
    // them the same way.  Mixing the two summation methods (pseudo big-endian,
    // segment little-endian native) was the previous bug.
    uint8_t pseudo[12];
    pseudo[0]=(uint8_t)(src);      pseudo[1]=(uint8_t)(src>>8);
    pseudo[2]=(uint8_t)(src>>16);  pseudo[3]=(uint8_t)(src>>24);
    pseudo[4]=(uint8_t)(dst);      pseudo[5]=(uint8_t)(dst>>8);
    pseudo[6]=(uint8_t)(dst>>16);  pseudo[7]=(uint8_t)(dst>>24);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    pseudo[10]= (uint8_t)(seg_len >> 8);
    pseudo[11]= (uint8_t)(seg_len & 0xFF);

    uint32_t sum = 0;

    // Sum pseudo-header (big-endian byte pairs)
    for (int i = 0; i < 12; i += 2)
        sum += ((uint32_t)pseudo[i] << 8) | pseudo[i + 1];

    // Sum TCP segment (also big-endian byte pairs for consistency)
    const uint8_t* p = (const uint8_t*)seg;
    size_t rem = seg_len;
    while (rem > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; rem -= 2;
    }
    if (rem) sum += (uint32_t)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    // Result is in big-endian logical form; htons converts to the byte order
    // needed when storing directly into the network header field (uint16_t).
    return htons((uint16_t)~sum);
}

// ─── Build and send a TCP segment ─────────────────────────────────────────────
void tcp_send_flags(struct TCPConn* c, uint8_t flags,
                    const void* data, uint16_t dlen) {
    uint16_t seg_len = (uint16_t)(sizeof(struct TCPHeader) + dlen);
    uint8_t* buf = (uint8_t*)net_alloc_buf();
    if (!buf) return;

    struct TCPHeader* hdr = (struct TCPHeader*)buf;
    hdr->src_port   = htons(c->local_port);
    hdr->dst_port   = htons(c->remote_port);
    hdr->seq        = htonl(c->snd_nxt);
    hdr->ack_seq    = htonl(c->rcv_nxt);
    hdr->data_offset= (uint8_t)(5 << 4);  // 20-byte header
    hdr->flags      = flags;
    hdr->window     = htons(TCP_RECV_BUF_SZ);
    hdr->checksum   = 0;
    hdr->urgent     = 0;

    if (dlen) {
        uint8_t* dp = (uint8_t*)(hdr + 1);
        for (uint16_t i = 0; i < dlen; i++) dp[i] = ((const uint8_t*)data)[i];
    }

    hdr->checksum = tcp_checksum(NET_MY_IP, c->remote_ip, hdr, seg_len);

    ipv4_send(c->remote_ip, IP_PROTO_TCP, buf, seg_len);
    net_free_buf(buf);

    if (flags & TCP_FLAG_SYN) c->snd_nxt++;
    if (flags & TCP_FLAG_FIN) c->snd_nxt++;
    c->snd_nxt += dlen;
}

// ─── tcp_handle_segment — called by ipv4_handle_packet ────────────────────────
void tcp_handle_segment(struct IPv4Header* ip, struct TCPHeader* seg,
                        uint16_t seg_len) {
    uint16_t dst_port = ntohs(seg->dst_port);
    uint16_t src_port = ntohs(seg->src_port);
    uint32_t seq      = ntohl(seg->seq);
    uint32_t ack      = ntohl(seg->ack_seq);
    uint8_t  flags    = seg->flags;
    int      hlen     = ((seg->data_offset >> 4) & 0xF) * 4;
    uint8_t* data     = (uint8_t*)seg + hlen;
    uint16_t data_len = (uint16_t)(seg_len - (uint16_t)hlen);

    // Find matching connection or listening slot
    struct TCPConn* found = 0;
    struct TCPConn* listener = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct TCPConn* c = &tcp_conns[i];
        if (!c->active) continue;
        if (c->state == TCP_LISTEN && c->local_port == dst_port) {
            listener = c;
        } else if (c->local_port  == dst_port  &&
                   c->remote_port == src_port   &&
                   c->remote_ip   == ip->src) {
            found = c;
            break;
        }
    }

    // ── SYN: new connection on a listening port ──────────────────────────────
    if (!found && listener && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        // Find a free slot for the new connection
        struct TCPConn* nc = 0;
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            if (!tcp_conns[i].active) { nc = &tcp_conns[i]; break; }
        }
        if (!nc) return;  // no free slots

        nc->active      = 1;
        nc->state       = TCP_SYN_RECEIVED;
        nc->local_port  = dst_port;
        nc->remote_port = src_port;
        nc->remote_ip   = ip->src;
        nc->rcv_nxt     = seq + 1;
        nc->snd_nxt     = 0x12345678;  // ISN
        nc->snd_una     = nc->snd_nxt;
        nc->rbuf_head   = 0;
        nc->rbuf_used   = 0;
        nc->rcv_wnd     = TCP_RECV_BUF_SZ;

        tcp_send_flags(nc, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
        return;
    }

    if (!found) return;
    struct TCPConn* c = found;

    // ── SYN retransmit for a pending SYN_RECEIVED connection ─────────────────
    // Peer retransmitted SYN (our SYN-ACK was lost — likely because ARP for
    // the remote IP wasn't resolved when we first tried to send it).
    // Now that ARP should be resolved, resend the SYN-ACK.
    if (c->state == TCP_SYN_RECEIVED &&
        (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        tcp_send_flags(c, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
        return;
    }

    // ── ACK of our SYN-ACK: connection established ────────────────────────────
    if (c->state == TCP_SYN_RECEIVED &&
        (flags & TCP_FLAG_ACK) && !(flags & TCP_FLAG_SYN)) {
        c->snd_una  = ack;
        c->state    = TCP_ESTABLISHED;
        return;
    }

    // ── Established: incoming data ────────────────────────────────────────────
    if (c->state == TCP_ESTABLISHED) {
        // Update send window
        if (flags & TCP_FLAG_ACK) c->snd_una = ack;

        // Copy data into receive ring buffer
        if (data_len > 0) {
            uint32_t avail = TCP_RECV_BUF_SZ - c->rbuf_used;
            if (data_len > avail) data_len = (uint16_t)avail;
            for (uint16_t i = 0; i < data_len; i++) {
                c->rbuf[(c->rbuf_head + c->rbuf_used) % TCP_RECV_BUF_SZ] = data[i];
                c->rbuf_used++;
            }
            c->rcv_nxt = seq + data_len;

            // ACK the received data
            tcp_send_flags(c, TCP_FLAG_ACK, 0, 0);
        }

        // FIN: peer is closing
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            c->state = TCP_CLOSE_WAIT;
            tcp_send_flags(c, TCP_FLAG_ACK, 0, 0);
        }
        return;
    }

    // ── CLOSE_WAIT: our app has read all data, close cleanly ─────────────────
    if (c->state == TCP_CLOSE_WAIT) {
        // tcp_close() will send FIN when called
        return;
    }

    // ── FIN_WAIT: we sent FIN, peer ACKs it ───────────────────────────────────
    if (c->state == TCP_FIN_WAIT) {
        if (flags & TCP_FLAG_ACK) {
            c->state = TCP_TIME_WAIT;
            // TIME_WAIT for 2MSL would need a timer; mark closed immediately
            c->state  = TCP_CLOSED;
            c->active = 0;
        }
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            tcp_send_flags(c, TCP_FLAG_ACK, 0, 0);
            c->state  = TCP_CLOSED;
            c->active = 0;
        }
    }
}

// ─── Public socket-facing API ─────────────────────────────────────────────────

int tcp_listen(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!tcp_conns[i].active) {
            tcp_conns[i].active     = 1;
            tcp_conns[i].state      = TCP_LISTEN;
            tcp_conns[i].local_port = port;
            tcp_conns[i].remote_port= 0;
            tcp_conns[i].remote_ip  = 0;
            tcp_conns[i].rbuf_head  = 0;
            tcp_conns[i].rbuf_used  = 0;
            return i;
        }
    }
    return -1;
}

// Spin-polls until a SYN_RECEIVED → ESTABLISHED transition for this port
int tcp_accept(int listen_id) {
    if (listen_id < 0 || listen_id >= TCP_MAX_CONNS) return -1;
    uint16_t port = tcp_conns[listen_id].local_port;

    for (;;) {
        // Check all slots for a new ESTABLISHED connection on this port
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            if (i == listen_id) continue;
            struct TCPConn* c = &tcp_conns[i];
            if (c->active && c->local_port == port &&
                c->state == TCP_ESTABLISHED) {
                return i;
            }
        }
        // No connection yet — yield CPU until next timer tick delivers packets.
        // The timer ISR calls net_poll_tick() → e1000_poll_rx() → tcp_handle_segment()
        // which may transition a SYN_RECEIVED connection to ESTABLISHED.
        net_event_hlt_wait();
    }
}

int tcp_recv(int id, void* buf, uint16_t max_len) {
    if (id < 0 || id >= TCP_MAX_CONNS) return -1;
    struct TCPConn* c = &tcp_conns[id];

    // Wait until data arrives or connection closes
    for (;;) {
        if (c->rbuf_used > 0) break;
        if (c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED) return 0;

        // Yield CPU until the next timer tick delivers incoming data.
        // After waking, the loop rechecks rbuf_used.
        net_event_hlt_wait();
    }

    uint16_t n = (uint16_t)(c->rbuf_used < max_len ? c->rbuf_used : max_len);
    for (uint16_t i = 0; i < n; i++) {
        ((uint8_t*)buf)[i] = c->rbuf[c->rbuf_head];
        c->rbuf_head = (c->rbuf_head + 1) % TCP_RECV_BUF_SZ;
        c->rbuf_used--;
    }
    return (int)n;
}

int tcp_send(int id, const void* data, uint16_t len) {
    if (id < 0 || id >= TCP_MAX_CONNS) return -1;
    struct TCPConn* c = &tcp_conns[id];
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return -1;

    // Send in NET_PKT_BUF_SIZE chunks (minus headers)
    const uint16_t CHUNK = (uint16_t)(NET_PKT_BUF_SIZE
                           - ETH_HDR_LEN
                           - (uint16_t)sizeof(struct IPv4Header)
                           - (uint16_t)sizeof(struct TCPHeader));
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t n = (uint16_t)(len - sent);
        if (n > CHUNK) n = CHUNK;
        tcp_send_flags(c, TCP_FLAG_ACK | TCP_FLAG_PSH,
                       (const uint8_t*)data + sent, n);
        sent += n;
    }
    return (int)sent;
}

void tcp_close(int id) {
    if (id < 0 || id >= TCP_MAX_CONNS) return;
    struct TCPConn* c = &tcp_conns[id];
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        tcp_send_flags(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        c->state = TCP_FIN_WAIT;
    } else {
        c->state  = TCP_CLOSED;
        c->active = 0;
    }
}
