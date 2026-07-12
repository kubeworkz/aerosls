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
} TCPState;

// ─── Per-connection buffers and state ────────────────────────────────────────
#define TCP_MAX_CONNS     8
#define TCP_RECV_BUF_SZ   4096
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
int  tcp_send(int conn_id, const void* buf, uint32_t len);
int  tcp_recv(int conn_id, void* buf, uint16_t max_len);  // returns bytes read
void tcp_close(int conn_id);

// Internal — also used by http.c
void tcp_send_flags(struct TCPConn* c, uint8_t flags, const void* data, uint16_t dlen);

// TCP pseudo-header checksum
uint16_t tcp_checksum(IPv4Addr src, IPv4Addr dst,
                      struct TCPHeader* seg, uint16_t seg_len);

#endif /* TCP_H */
