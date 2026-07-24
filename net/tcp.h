#ifndef TCP_H
#define TCP_H

#include "net.h"

// ─── TCP connection states ────────────────────────────────────────────────────
typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_FIN_WAIT,
    TCP_TIME_WAIT,
    TCP_SYN_SENT,     // client-initiated connect: SYN sent, waiting for SYN-ACK
} TCPState;

// ─── Per-connection buffers and state ────────────────────────────────────────
// History: 8 -> 24 (VectorStore Interface Roadmap Phase 4 verification pass,
// fixing a real starvation bug where inbound frontend polling traffic could
// exhaust every slot and leave ollama_client.c's own outbound tcp_connect()
// with nothing free). 24 -> 512 (Multitenant Isolation Gap Analysis §5 item
// 9 / §7 item 8, capacity sizing for a real ~256-tenant deployment): this
// single pool is still shared by every INBOUND http.c connection AND every
// OUTBOUND connection the kernel itself opens, and §8's own findings
// addendum on network fairness already named this shared-pool structure as
// the reason a per-tenant CONNECTION quota isn't implemented yet (see that
// addendum) -- 512 buys real headroom for many concurrent tenants' inbound
// traffic without starving outbound calls again, but does not by itself fix
// the "one shared, un-partitioned, cluster-wide pool" gap network fairness
// Phase 2 addresses separately. Static RAM cost: each slot's rbuf[] below
// plus http.c's own per-slot HTTP_REQ_BUF_SZ buffer -- roughly 46-47 MiB
// total at 512 slots (up from ~2.3 MiB at 24), a real, deliberate footprint
// increase for real tenant scale, not a side effect to discover later.
#define TCP_MAX_CONNS     512
#define TCP_RECV_BUF_SZ   32768   /* 32 KiB — supports 16 KiB upload chunks (32 KiB hex) */
#define TCP_SEND_BUF_SZ   8192

struct TCPConn {
    TCPState state;
    IPv4Addr remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t snd_nxt;        // next seq to send
    uint32_t snd_una;        // oldest unacknowledged seq
    uint32_t rcv_nxt;        // next expected seq from remote
    uint16_t rcv_wnd;        // our receive window advertised

    // Receive ring buffer
    uint8_t  rbuf[TCP_RECV_BUF_SZ];
    uint32_t rbuf_head;
    uint32_t rbuf_used;

    uint8_t  active;
    // Multitenant Isolation Gap Analysis §5 item 9: widened from uint8_t to
    // uint16_t when TCP_MAX_CONNS crossed 256 -- a uint8_t index would have
    // silently wrapped modulo 256 (slot 256 aliasing slot 0's conn_id, etc.)
    // the moment the pool grew past exactly the size that bug needed to go
    // unnoticed. Nothing currently reads this field back for lookup
    // (confirmed by search — only ever written), but it's a real latent
    // landmine fixed here rather than left for the next person to rediscover.
    uint16_t conn_id;        // index into tcp_conns[]
};

extern struct TCPConn tcp_conns[TCP_MAX_CONNS];

// ─── Public API ───────────────────────────────────────────────────────────────
void tcp_init(void);

// Called by ipv4_handle_packet
void tcp_handle_segment(struct IPv4Header* ip, struct TCPHeader* seg, uint16_t seg_len);

// Called by socket layer
int  tcp_listen(uint16_t port);             // returns conn_id or -1
int  tcp_accept(int listen_id);             // blocks until connection; returns new conn_id
int  tcp_connect(IPv4Addr dst_ip, uint16_t dst_port); // active open; returns conn_id or -1
int  tcp_send(int conn_id, const void* buf, uint32_t len);
int  tcp_recv(int conn_id, void* buf, uint16_t max_len);  // returns bytes read
void tcp_close(int conn_id);

// Internal — also used by http.c
void tcp_send_flags(struct TCPConn* c, uint8_t flags, const void* data, uint16_t dlen);

// TCP pseudo-header checksum
uint16_t tcp_checksum(IPv4Addr src, IPv4Addr dst,
                      struct TCPHeader* seg, uint16_t seg_len);

#endif /* TCP_H */
