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
// Bumped from 8 -> 24 (VectorStore Interface Roadmap Phase 4 verification
// pass): this single pool is shared by every INBOUND http.c connection
// (including the frontend's own recurring background polling of /api/
// health, /api/services, /api/wal, /api/tiers, /api/objects, /api/metrics,
// several requests firing every few seconds) and every OUTBOUND connection
// the kernel itself opens, e.g. ollama_client.c's tcp_connect() to reach
// Ollama. tcp_connect() just returns -1 ("no free slots") if none are free
// -- with only 8 total, a handful of live browser polling connections could
// starve out an outbound Ollama call entirely, which is exactly what live
// testing reproduced: the first embed-insert succeeded before poll traffic
// built up, then embed-search and a second embed-insert both failed with
// ollama_status=-1 while Ollama itself was confirmed healthy and reachable
// via curl -- the kernel simply had no slot left to open a new outbound
// connection with. 24 gives real headroom over the ~6-8 concurrent inbound
// pollers this frontend can generate, at the cost of ~2.3 MiB more static
// RAM (each slot's rbuf[] below plus http.c's own per-slot HTTP_REQ_BUF_SZ
// buffer) -- trivial on this host's available RAM, and a single #define,
// not a redesign, matching this fix's own narrow scope.
#define TCP_MAX_CONNS     24
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
    uint8_t  conn_id;        // index into tcp_conns[]
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
