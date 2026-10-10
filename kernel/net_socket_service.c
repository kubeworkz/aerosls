/* net_socket_service.c — see net_socket_service.h. P2 increments 1 + 2 + 3.
 * Increment 1 gave a tenant a channel to this kernel-owned service and made
 * it fail every verb by name. Increment 2 — this file's admission path —
 * makes the client verbs work against the kernel's own stack, and puts the
 * one property the whole phase rests on at the connect: a tenant's outbound
 * connection is attributed to its partition and checked against that
 * partition's quota BEFORE the connection exists, so a flood counts against
 * someone and never against nobody. Increment 3 — the listener — is the
 * other half of §6's "client and listener": bind/listen/accept through the
 * same service, each partition's listener reachable only from inside that
 * partition (tcp_conns[] is a single kernel-global pool, so a listener on
 * port P only ever matches an inbound SYN the stack already delivered to P
 * — a cross-partition socket is not representable at the wire). */
#include "net_socket_service.h"
#include "cap.h"
#include "kernel_io.h"

/* The wire, spelled out in C. user/proto/src/lib.rs is the source of
 * truth for these bytes (NetFrame: magic[8] "AEROSNT\x01", version u16,
 * ty u16, flags u16, pad u16 — 16 bytes little-endian); they are written
 * here because the kernel does not link Rust, and the host test pins the
 * same constants against the client's own layout. */
#include "../net/tcp.h"
#include "../net/tcp_quota.h"

#define NSS_MAGIC_LEN 8
#define NSS_HDR 16u          /* NetFrame::SIZE */
#define NSS_VERB_INFO 1u     /* NET_INFO — the handshake, answered honestly */
#define NSS_ERR_FLAG 0x0001u /* NET_FLAG_ERROR */
#define NSS_STATUS_CAP 9u    /* NET_ERR_CAP: the capability-shaped refusal
                              * for the verbs this increment does not build */
#define NSS_STATUS_OK 0u
#define NSS_MTU 1500u
#define NSS_TEXT_CAP 192u    /* room for the longest rendered refusal */

/* user/proto's NET_* verb numbers, spelled out. socket/connect/send/recv/
 * shutdown/close admit (increment 2); bind/listen/accept admit too (increment
 * 3's listener, below), and poll answers the listener/relay loops' readiness
 * question with the spec's events body; everything else is refused as
 * unknown. The full eleven-name table below is the refusal renderer's "by
 * name" source — the out-of-table types are the ones the admit switch
 * refuses. */
#define NSS_VERB_SOCKET   2u
#define NSS_VERB_BIND     3u
#define NSS_VERB_CONNECT  4u
#define NSS_VERB_LISTEN   5u
#define NSS_VERB_ACCEPT   6u
#define NSS_VERB_SEND     7u
#define NSS_VERB_RECV     8u
#define NSS_VERB_SHUTDOWN 9u
#define NSS_VERB_CLOSE    10u
#define NSS_VERB_POLL     11u

/* The grant rights bits the wire speaks (user/proto's R and W). A send's
 * data buffer arrives as an R-only grant; a recv's as a W-only grant — the
 * direction is enforced at the grant, not trusted from the verb, so a
 * tenant cannot write through a buffer it was only allowed to read. */
#define NSS_RIGHT_R 0x1u
#define NSS_RIGHT_W 0x2u

struct NetSock {
    int      used;
    int      conn_id;      /* tcp_conns[] index the kernel stack handed back;
                            * -1 while the socket is not yet bound/connected */
    uint16_t lport;        /* bound local port; 0 while unbound. The LISTEN
                            * state is (used && conn_id == the listener slot
                            * && lport != 0), so the accept poll matches ESTAB-
                            * LISHED conns whose local_port == lport. */
    uint32_t partition;    /* the partition this socket is charged to */
};

struct NetSockSvc {
    int      used;
    uint32_t partition;
    uint32_t pid;
    uint16_t k_rd;        /* kernel (pid 0) CHAN_R — requests arrive here */
    uint16_t k_wr;        /* kernel (pid 0) CHAN_W — replies leave here */
    char     name[32];    /* the caller's sidecar name: the refusal names it */
    struct NetSock socks[NSS_MAX_SOCKS];
};

static struct NetSockSvc nss[NET_SOCKET_SERVICE_MAX];
/* One shared drain buffer, single static, for the same reason
 * env_console.c's is: this tick runs in one non-IRQ kernel context at a
 * time (microkernel_service_poll), and a channel's payload bound is what
 * sizes it — a frame is never silently truncated. */
static uint8_t  nss_buf[4096];
/* The grant descriptors that arrived with the frame currently being
 * handled, captured by the tick's cap_recv_msg so the send/recv handlers
 * can resolve the tenant's data buffer. One slot is enough — the wire
 * carries one buffer grant per operation. */
static struct SLSCapDesc nss_out_caps[CAP_MSG_MAX_CAPS];
static uint16_t nss_out_n_caps = 0;
static uint32_t nss_refusals_total = 0;
static uint32_t nss_dropped_total = 0;
static uint32_t nss_admits_total = 0;
static uint32_t nss_quota_refusals_total = 0;

/* ─── verb names ───────────────────────────────────────────────────────────
 * The refusal's "by name": the table mirrors user/proto's NET_* constants
 * one for one. A type outside the table still gets a refusal — rendered
 * with its number — because "unknown" must fail as honestly as "known". */
static const struct { uint16_t ty; const char* name; } nss_verbs[] = {
    { 1,  "NET_INFO" },
    { 2,  "NET_SOCKET" },
    { 3,  "NET_BIND" },
    { 4,  "NET_CONNECT" },
    { 5,  "NET_LISTEN" },
    { 6,  "NET_ACCEPT" },
    { 7,  "NET_SEND" },
    { 8,  "NET_RECV" },
    { 9,  "NET_SHUTDOWN" },
    { 10, "NET_CLOSE_SOCK" },
    { 11, "NET_POLL" },
};

const char* net_socket_verb_name(uint16_t ty) {
    for (uint32_t i = 0; i < sizeof(nss_verbs) / sizeof(nss_verbs[0]); i++)
        if (nss_verbs[i].ty == ty) return nss_verbs[i].name;
    return 0;
}

/* ─── the refusal renderer ─────────────────────────────────────────────────
 * Hand-rolled because the kernel is freestanding — the same ec_put shape
 * env_ckpt_refusal_text() uses for the same reason. Writes at most cap-1
 * bytes plus the terminator, and always terminates: a truncating renderer
 * is fine, an unterminated one would corrupt the serial transcript. */
static void nss_put(char** p, char* end, const char* s) {
    while (*s && *p < end) *(*p)++ = *s++;
}
static void nss_put_u32(char** p, char* end, uint32_t v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 10) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0 && *p < end) *(*p)++ = tmp[--n];
}

uint32_t net_socket_refusal_text(uint16_t ty, char* out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    char* p = out;
    char* end = out + (cap - 1);
    const char* verb = net_socket_verb_name(ty);
    if (verb) {
        nss_put(&p, end, verb);
    } else {
        nss_put(&p, end, "verb ");
        nss_put_u32(&p, end, ty);
    }
    nss_put(&p, end, " refused by kernel.net.socket"
                     " — not admitted in P2 increment 3");
    *p = '\0';
    return (uint32_t)(p - out);
}

/* ─── registry ────────────────────────────────────────────────────────────*/

int net_socket_service_register(uint16_t k_rd, uint16_t k_wr,
                                uint32_t partition, uint32_t pid,
                                const char* name) {
    /* Arm the attribution/quota table before any connection can be
     * attributed. Required rather than BSS-safe (tcp_quota.h's own note):
     * the "unattributed" sentinel is TCP_CONN_PARTITION_NONE, not 0, so a
     * zero-filled table would misread every slot as charged to partition 0.
     * Idempotent, so a later http.c registration is a no-op — same call
     * net/http.c's http_server_run() makes. */
    tcp_quota_init();
    struct NetSockSvc* e = 0;
    /* Same (partition, pid) re-registering (E5's recycle, where a pid can
     * even be reused): replace the stale entry rather than leaking it, so
     * the registry never fills with dead environments' channels. */
    for (uint32_t i = 0; i < NET_SOCKET_SERVICE_MAX; i++) {
        if (nss[i].used && nss[i].partition == partition &&
            nss[i].pid == pid) { e = &nss[i]; break; }
    }
    if (!e) {
        for (uint32_t i = 0; i < NET_SOCKET_SERVICE_MAX; i++) {
            if (!nss[i].used) { e = &nss[i]; break; }
        }
    }
    if (!e) {
        kernel_serial_printf(
            "[NET-SOCKET] partition %u pid %u: registry full (%d) — socket "
            "service left unregistered\n",
            (unsigned)partition, (unsigned)pid,
            (int)NET_SOCKET_SERVICE_MAX);
        return 0;
    }
    e->used = 1;
    e->partition = partition;
    e->pid = pid;
    e->k_rd = k_rd;
    e->k_wr = k_wr;
    uint32_t i = 0;
    if (name)
        for (; name[i] && i + 1 < sizeof(e->name); i++) e->name[i] = name[i];
    e->name[i] = '\0';
    kernel_serial_printf(
        "[NET-SOCKET] %s (partition %u, pid %u) wired: kernel ends rd=%u "
        "wr=%u — client + listener open (P2 increment 3)\n",
        e->name, (unsigned)partition, (unsigned)pid,
        (unsigned)k_rd, (unsigned)k_wr);
    return 1;
}

int net_socket_service_kernel_slot(uint16_t slot) {
    for (uint32_t i = 0; i < NET_SOCKET_SERVICE_MAX; i++)
        if (nss[i].used && nss[i].k_rd == slot) return 1;
    return 0;
}

uint32_t net_socket_service_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < NET_SOCKET_SERVICE_MAX; i++)
        if (nss[i].used) n++;
    return n;
}

uint32_t net_socket_refusals(void)  { return nss_refusals_total; }
uint32_t net_socket_dropped(void)   { return nss_dropped_total; }
uint32_t net_socket_admits(void)    { return nss_admits_total; }
uint32_t net_socket_quota_refusals(void) { return nss_quota_refusals_total; }

/* ─── sockets ──────────────────────────────────────────────────────────
 * One channel's open sockets, charged to the channel's partition. A
 * socket's whole identity is the kernel conn_id it wraps plus the
 * partition it is attributed to — the partition is stamped at socket
 * creation from the CHANNEL's registration, so it cannot be forged by a
 * later verb, and it is what the connect's quota check reads. */
static struct NetSock* nss_sock(struct NetSockSvc* e, uint32_t id) {
    if (id >= NSS_MAX_SOCKS) return 0;
    if (!e->socks[id].used) return 0;
    return &e->socks[id];
}

/* Resolve a kernel-held slot to its channel — the same derivation
 * env_console.c's ec_chan_of() makes, for the same reason: the tick must
 * notice a peer that closed its end. */
static struct CapChannel* nss_chan_of(uint16_t slot, int* out_kdir) {
    uint64_t w = cap_tables[0].slots[slot].word;
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    if (obj_id >= CAP_OBJECT_MAX) return 0;
    struct CapObject* o = &cap_objects[obj_id];
    if (!o->active || o->kind != CAP_OBJ_KIND_CHAN || o->chan_id >= CAP_CHAN_MAX)
        return 0;
    struct CapChannel* ch = &cap_channels[o->chan_id];
    if (out_kdir) *out_kdir = (0 == ch->end0_pid) ? 0 : 1;
    return ch;
}

static void nss_release(struct NetSockSvc* e) {
    uint16_t rd = e->k_rd;
    /* Close every socket this channel still holds and release its
     * attribution first — a retired environment must not leave a live
     * connection charged to a partition that no longer has a tenant on it.
     * The release is the attribution's other half: admit binds it, this
     * frees it. */
    for (uint32_t s = 0; s < NSS_MAX_SOCKS; s++) {
        if (e->socks[s].used && e->socks[s].conn_id >= 0) {
            tcp_close(e->socks[s].conn_id);
            tcp_conn_release(e->socks[s].conn_id);
        }
        e->socks[s].used = 0;
        e->socks[s].conn_id = -1;
    }
    /* Revoke the kernel end of the channel — the sidecar already closed
     * its half, so this frees the kernel's CHAN_R and destroys the
     * channel, the same close-aware teardown env_console_tick() performs. */
    if (rd != 0xFFFFu) cap_revoke(0, rd);
    e->used = 0;
    e->pid = 0;
    e->partition = 0;
    e->k_rd = 0xFFFFu;
    e->k_wr = 0xFFFFu;
    e->name[0] = '\0';
}

/* ─── the wire itself ─────────────────────────────────────────────────────*/

static void nss_wire_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void nss_wire_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static uint16_t nss_get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int nss_is_net_frame(const uint8_t* b, uint32_t n) {
    static const char magic[NSS_MAGIC_LEN] = { 'A','E','R','O','S','N','T',1 };
    if (n < NSS_HDR) return 0;
    for (uint32_t i = 0; i < NSS_MAGIC_LEN; i++)
        if (b[i] != (uint8_t)magic[i]) return 0;
    return nss_get_u16(b + 8) == 1;   /* version 1 */
}

static void nss_send(struct NetSockSvc* e, const uint8_t* payload,
                     uint32_t len, uint32_t tag) {
    if (cap_send_msg(0, e->k_wr, payload, len, 0, 0, tag, 0) != 0) {
        /* The peer is gone between drain and reply: retire now rather
         * than waiting for the next tick's close check to notice. */
        nss_release(e);
    }
}

/* A clean (non-error) reply: the header echoes the request, flags 0, and a
 * status body the client parses as (status, value). `value` is the socket
 * id for a socket/connect, the byte count for a send/recv. */
static void nss_reply_ok(struct NetSockSvc* e, uint16_t ty, uint32_t tag,
                         uint64_t value) {
    uint8_t reply[16 + 10];
    for (uint32_t i = 0; i < 8; i++) reply[i] = nss_buf[i];
    nss_wire_u16(reply + 8, 1);
    nss_wire_u16(reply + 10, ty);
    nss_wire_u16(reply + 12, 0);              /* flags: not an error */
    nss_wire_u16(reply + 14, 0);
    nss_wire_u16(reply + 16, NSS_STATUS_OK);  /* status: NET_OK */
    for (uint32_t i = 0; i < 8; i++)
        reply[18 + i] = (uint8_t)((value >> (8 * i)) & 0xFFu);
    nss_send(e, reply, (uint32_t)sizeof reply, tag);
}

/* An error reply carrying a rendered reason by name — increment 1's shape,
 * now reserved only for an out-of-table verb type and the malformed-argument
 * arms of an admitted verb (a listen() on an unbound socket, an accept() on
 * a non-listener). The payload appends the text after the status body; the
 * wire's parse reads the status and ignores the text, and the text's audience
 * is the host test, the serial line and the live clause. */
static void nss_refuse(struct NetSockSvc* e, uint16_t ty, uint32_t tag,
                       uint16_t status, const char* why) {
    char text[NSS_TEXT_CAP];
    uint32_t tlen = net_socket_refusal_text(ty, text, sizeof text);
    uint8_t reply[16 + 10 + NSS_TEXT_CAP];
    for (uint32_t i = 0; i < 8; i++) reply[i] = nss_buf[i];
    nss_wire_u16(reply + 8, 1);
    nss_wire_u16(reply + 10, ty);              /* echo the verb */
    nss_wire_u16(reply + 12, NSS_ERR_FLAG);    /* NET_FLAG_ERROR */
    nss_wire_u16(reply + 14, 0);
    nss_wire_u16(reply + 16, status);
    for (uint32_t i = 0; i < 8; i++) reply[18 + i] = 0;  /* value u64 = 0 */
    for (uint32_t i = 0; i < tlen; i++) reply[26 + i] = (uint8_t)text[i];
    nss_refusals_total++;
    kernel_serial_printf("[NET-SOCKET] %s (partition %u, pid %u): %s\n",
                         e->name, (unsigned)e->partition, (unsigned)e->pid,
                         why ? why : text);
    nss_send(e, reply, 26 + tlen, tag);
}

/* Resolve a received grant cap to its (base,len) in kernel address space.
 * `want` is the required right bit: a send needs R on the data it reads, a
 * recv needs W on the buffer it fills. Returns 0 when the frame carried no
 * grant, when the grant is short, or when its rights are wrong for the
 * direction — the refusal for those is a capability refusal, because it
 * IS one: the tenant did not grant the buffer it is now asking the kernel
 * to move bytes through. On success *len is clamped to the grant. */
static void* nss_grant_ptr(const struct SLSCapDesc* caps, uint16_t n_caps,
                           uint8_t want, uint32_t* len) {
    if (!caps || n_caps == 0 || len == 0) return 0;
    const struct SLSCapDesc* d = &caps[0];
    if ((d->rights & want) != want) return 0;
    /* The grant landed in the KERNEL's own table (pid 0 received the
     * message), so resolve the slot there — the same derivation
     * nss_chan_of() makes, applied to a MEM object. A grant is a byte view
     * (offset/len) into that object's region; the kernel identity-maps
     * physical memory, so phys_base + offset is directly readable here. */
    uint64_t w = cap_tables[0].slots[d->slot].word;
    uint32_t obj_id = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    if (obj_id >= CAP_OBJECT_MAX) return 0;
    struct CapObject* o = &cap_objects[obj_id];
    if (!o->active || o->kind != CAP_OBJ_KIND_MEM) return 0;
    uint64_t region = (uint64_t)o->npages * 4096ull;
    uint64_t off = d->offset, l = d->len;
    if (l == 0 || off >= region) return 0;
    if (off + l > region) l = region - off;  /* clamp, never over-read */
    *len = (uint32_t)l;
    return (void*)(uintptr_t)(o->phys_base + off);
}

/* Answer one drained frame. */
static void nss_handle(struct NetSockSvc* e, uint32_t plen, uint32_t tag) {
    if (!nss_is_net_frame(nss_buf, plen)) {
        /* Not a NET_* frame at all. Nothing that speaks to this channel
         * should produce one (the only senders are the boot handshake and
         * NetClient), so this is counted rather than answered: a reply
         * would be parsed as an error by a client that never sent this. */
        nss_dropped_total++;
        return;
    }
    uint16_t ty = nss_get_u16(nss_buf + 10);

    if (ty == NSS_VERB_INFO) {
        /* The handshake: the channel IS typed, so it is answered
         * honestly — max_sockets is the real per-channel ceiling
         * (NSS_MAX_SOCKS), MTU 1500 (the kernel stack's own), flags 0.
         * In increment 1 this said 0 because nothing admitted; increment 2
         * makes it the truth a client can plan against — how many sockets
         * it may hold open before it spends one. */
        uint8_t reply[16 + 12];
        for (uint32_t i = 0; i < 8; i++) reply[i] = nss_buf[i];
        nss_wire_u16(reply + 8, 1);              /* version */
        nss_wire_u16(reply + 10, NSS_VERB_INFO); /* ty */
        nss_wire_u16(reply + 12, 0);             /* flags: not an error */
        nss_wire_u16(reply + 14, 0);             /* pad */
        nss_wire_u32(reply + 16, NSS_MAX_SOCKS);
        nss_wire_u32(reply + 20, NSS_MTU);
        nss_wire_u32(reply + 24, 0);
        nss_send(e, reply, (uint32_t)sizeof reply, tag);
        return;
    }

    /* ── the client verbs this increment admits ──────────────────────
     * socket/connect/send/recv/shutdown/close run against the KERNEL's
     * own stack (net/tcp.c), never drv.network.0. bind/listen/accept are
     * the THIRD increment's listener (below). A connect is attributed to
     * the caller's partition and checked against that partition's quota
     * BEFORE a connection exists. */
    switch (ty) {
    case NSS_VERB_SOCKET: {
        /* {sock_type u16, protocol u16}. Only SOCK_STREAM (1) is built;
         * a datagram socket is refused by name rather than half-created. */
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint16_t stype = nss_get_u16(nss_buf + NSS_HDR);
        if (stype != 1u /* SOCK_STREAM */) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        for (uint32_t s = 0; s < NSS_MAX_SOCKS; s++) {
            if (e->socks[s].used) continue;
            e->socks[s].used = 1;
            e->socks[s].conn_id = -1;          /* not connected yet */
            e->socks[s].partition = e->partition;
            nss_reply_ok(e, ty, tag, s);
            return;
        }
        nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0);   /* table full */
        return;
    }
    case NSS_VERB_CONNECT: {
        /* {sock_id u32, addr [u8;6]}. The socket must exist and be
         * unconnected; then the ADMIT path runs, in this order, before any
         * connection exists: attribute the caller's partition, then check
         * its quota. The conn_id does not exist until tcp_connect returns,
         * so the quota DECISION is made first (usage vs quota, read from
         * the same module) and the attribution is bound the instant the
         * kernel hands a conn_id back — with a mandatory release arm if it
         * is refused. Nothing is left counting against nobody. */
        if (plen < NSS_HDR + 10) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk || sk->conn_id >= 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        /* Decode the sockaddr. The client holds the LOGICAL ip (e.g.
         * 127.0.0.1 = 0x7F000001) and user/proto's encode_sockaddr writes it
         * LITTLE-endian on the wire (put_u32 -> to_le_bytes). The kernel's
         * own IPv4Addr is stored NETWORK order (see include/config.h:
         * KERNEL_STATIC_IP = 0x0F02000A for 10.0.2.15, i.e. bytes 0A 00 02
         * 0F in memory), which is what tcp_connect()/ipv4_send()/arp_lookup
         * all compare and write — tcp_connect()'s (ntohl(dst_ip) & mask)
         * turns that network-order value into logical form for its subnet
         * test. So we decode the wire bytes IN MEMORY ORDER (ap[0] is the
         * most-significant byte of the on-the-wire address), yielding the
         * network-order IPv4Addr directly — the exact inverse of the
         * client's LE encode. Decoding LE here would hand tcp_connect a
         * byte-swapped address and route to the wrong host. */
        const uint8_t* ap = nss_buf + NSS_HDR + 4;
        uint32_t ip = ((uint32_t)ap[0] << 24) | ((uint32_t)ap[1] << 16)
                    | ((uint32_t)ap[2] << 8) | (uint32_t)ap[3];
        uint16_t port = (uint16_t)(ap[4] | ((uint16_t)ap[5] << 8));
        if (ip == 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }

        /* The quota gate, read from the module that owns it. 0 = unlimited
         * is the module's BSS-zero-safe default, so an operator who has not
         * opted a partition in sees byte-identical behaviour. */
        uint16_t quota = tcp_partition_get_conn_quota(e->partition);
        uint16_t usage = tcp_partition_get_conn_usage(e->partition);
        if (quota != 0 && usage >= quota) {
            nss_quota_refusals_total++;
            nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0);
            return;
        }
        int cid = tcp_connect((IPv4Addr)ip, port);
        if (cid < 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        /* Bind the attribution NOW, the moment the connection exists. This
         * is the load-bearing call the unattributed tooth removes: without
         * it a tenant's outbound connection charges no partition and a
         * flood looks like absence. */
        if (!tcp_conn_attribute_partition(cid, e->partition)) {
            tcp_close(cid);
            nss_quota_refusals_total++;
            nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0);
            return;
        }
        sk->conn_id = cid;
        nss_admits_total++;
        nss_reply_ok(e, ty, tag, sid);
        return;
    }
    case NSS_VERB_SEND: {
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk || sk->conn_id < 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t dlen = 0;
        void* data = nss_grant_ptr(nss_out_caps, nss_out_n_caps,
                                   NSS_RIGHT_R, &dlen);
        if (!data) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        int sent = tcp_send(sk->conn_id, data, dlen);
        if (sent < 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        nss_reply_ok(e, ty, tag, (uint64_t)(uint32_t)sent);
        return;
    }
    case NSS_VERB_RECV: {
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk || sk->conn_id < 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t blen = 0;
        void* buf = nss_grant_ptr(nss_out_caps, nss_out_n_caps,
                                  NSS_RIGHT_W, &blen);
        if (!buf) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        int got = tcp_recv(sk->conn_id, buf, (uint16_t)blen);
        if (got < 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        nss_reply_ok(e, ty, tag, (uint64_t)(uint32_t)got);
        return;
    }
    case NSS_VERB_CLOSE:
    case NSS_VERB_SHUTDOWN: {
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        if (sk->conn_id >= 0) {
            tcp_close(sk->conn_id);
            tcp_conn_release(sk->conn_id);   /* the attribution's other half */
            sk->conn_id = -1;
        }
        if (ty == NSS_VERB_CLOSE) sk->used = 0;
        nss_reply_ok(e, ty, tag, sid);
        return;
    }
    case NSS_VERB_BIND: {
        /* {sock_id u32, sockaddr [u8;6]}. The socket must exist and be
         * unbound (conn_id == -1); then the local port is recorded on the
         * socket. No kernel call yet — bind only claims the port on this
         * channel's socket; the stack's own listener slot is drawn at
         * listen(), so a bind that is never followed by listen() costs a
         * conn slot nothing. The sockaddr is decoded the same LE-on-wire →
         * network-order-in-kernel way the connect path documents; the bind
         * port is the low two bytes (host order, as tcp_conns[].local_port
         * is compared against). */
        if (plen < NSS_HDR + 10) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk || sk->conn_id >= 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        const uint8_t* ap = nss_buf + NSS_HDR + 4;
        uint16_t port = (uint16_t)(ap[4] | ((uint16_t)ap[5] << 8));
        if (port == 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        sk->lport = port;
        nss_reply_ok(e, ty, tag, sid);
        return;
    }
    case NSS_VERB_LISTEN: {
        /* {sock_id u32}. The socket must exist, be bound (lport != 0), and
         * not already listening/connected. tcp_listen() draws an INBOUND
         * pool slot (never the outbound-reserved tail) and returns its
         * conn_id, which becomes this socket's listener slot. The listener
         * itself is NOT yet attributed to the partition — attribution binds
         * at accept(), the moment a real inbound connection exists, which
         * is the same admit-at-the-moment rule the connect path keeps. */
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk || sk->conn_id >= 0 || sk->lport == 0) {
            nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return;
        }
        int lid = tcp_listen(sk->lport);
        if (lid < 0) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        sk->conn_id = lid;
        nss_reply_ok(e, ty, tag, sid);
        return;
    }
    case NSS_VERB_ACCEPT: {
        /* {sock_id u32}. The socket must be a listener (conn_id >= 0 and
         * lport bound). The accept is NON-BLOCKING — the same scan
         * tcp_accept() and http.c's own pickup loop do, but one pass per
         * tick instead of a spin/hlt-wait (net_event_hlt_wait() would stall
         * the whole kernel in this non-IRQ tick). No ESTABLISHED conn on the
         * listener's port yet → NET_EAGAIN-style refusal (status 11), which
         * the client's bounded retry loop treats as "poll again", not an
         * error — the same contract the boot handshake's retry keeps. A
         * ready connection is attributed to the partition at the instant it
         * is handed out (the inbound half of the unattributed tooth), its
         * slot becomes a normal connected socket, and the listener stays
         * listening for the next one. */
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk || sk->conn_id < 0 || sk->lport == 0) {
            nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return;
        }
        /* Find a free socket slot to receive the accepted connection. */
        uint32_t ns = NSS_MAX_SOCKS;
        for (uint32_t s = 0; s < NSS_MAX_SOCKS; s++) {
            if (!e->socks[s].used) { ns = s; break; }
        }
        if (ns == NSS_MAX_SOCKS) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        /* Non-blocking scan, http.c:6980's shape: every active ESTABLISHED
         * conn whose local_port matches this listener's port and that is
         * not already this listener's own slot. tcp_conns[] is kernel-global
         * and kernel-identity-mapped, so this is a plain array walk. */
        int found = -1;
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            if (i == sk->conn_id) continue;
            struct TCPConn* c = &tcp_conns[i];
            if (c->active && c->state == TCP_ESTABLISHED &&
                c->local_port == sk->lport) { found = i; break; }
        }
        if (found < 0) {
            /* Nothing pending — NET_EAGAIN (11), not an error: the caller
             * retries. The refusal renderer is NOT used (this is a status,
             * not a refusal-by-name), so the counters stay honest. */
            uint8_t reply[16 + 10];
            for (uint32_t i = 0; i < 8; i++) reply[i] = nss_buf[i];
            nss_wire_u16(reply + 8, 1);
            nss_wire_u16(reply + 10, NSS_VERB_ACCEPT);
            nss_wire_u16(reply + 12, NSS_ERR_FLAG);
            nss_wire_u16(reply + 14, 0);
            nss_wire_u16(reply + 16, 11u);      /* NET_EAGAIN */
            for (uint32_t i = 0; i < 8; i++) reply[18 + i] = 0;
            nss_send(e, reply, 26, tag);
            return;
        }
        /* Attribute the accepted connection NOW — the inbound half of the
         * unattributed tooth. A refusal here (over-quota) closes the conn
         * and leaves it charging nobody, same as the connect path. */
        if (!tcp_conn_attribute_partition(found, e->partition)) {
            tcp_close(found);
            nss_quota_refusals_total++;
            nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0);
            return;
        }
        e->socks[ns].used = 1;
        e->socks[ns].conn_id = found;
        e->socks[ns].lport = 0;                /* an accepted conn is not bound */
        e->socks[ns].partition = e->partition;
        nss_admits_total++;
        nss_reply_ok(e, ty, tag, ns);           /* value = the NEW socket id */
        return;
    }
    case NSS_VERB_POLL: {
        /* {sock_id u32}. The reply body is the spec's NET_POLL reply —
         * {sock_id u32, events u16} (user/proto lib.rs) — NOT the status
         * body: bit0 (0x01) is "ready" in every caller — an ESTABLISHED
         * conn pending on a listener (the ACCEPT arm's exact scan, so
         * poll and accept can never disagree), or a connected socket with
         * bytes buffered or the peer closed (so recv after a ready poll
         * returns the data or the 0 EOF the relay expects). An empty
         * mask ANSWERS with 0 — never a refusal-by-name: every nc -l and
         * relay loop polls first, so a `default`-arm refusal spins the
         * caller through the refusal path forever (caught live: the
         * listener's poll loop flooded refusals until the serial console
         * drain starved out). */
        if (plen < NSS_HDR + 4) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint32_t sid = (uint32_t)nss_buf[NSS_HDR]
                     | ((uint32_t)nss_buf[NSS_HDR + 1] << 8)
                     | ((uint32_t)nss_buf[NSS_HDR + 2] << 16)
                     | ((uint32_t)nss_buf[NSS_HDR + 3] << 24);
        struct NetSock* sk = nss_sock(e, sid);
        if (!sk) { nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0); return; }
        uint64_t events = 0;
        struct TCPConn* c = (sk->conn_id >= 0) ? &tcp_conns[sk->conn_id] : 0;
        if (c && c->state == TCP_LISTEN) {
            /* Accept-ready: the ACCEPT arm's scan, verbatim. */
            for (int i = 0; i < TCP_MAX_CONNS; i++) {
                if (i == sk->conn_id) continue;
                struct TCPConn* p = &tcp_conns[i];
                if (p->active && p->state == TCP_ESTABLISHED &&
                    p->local_port == sk->lport) { events |= 0x01u; break; }
            }
        } else if (c) {
            if (c->rbuf_used > 0) events |= 0x01u;
            if (c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED)
                events |= 0x01u;   /* EOF is readable */
        }
        /* else: bound-but-not-listening / not-connected — honest 0. */
        uint8_t reply[16 + 6];
        for (uint32_t i = 0; i < 8; i++) reply[i] = nss_buf[i];
        nss_wire_u16(reply + 8, 1);
        nss_wire_u16(reply + 10, NSS_VERB_POLL);
        nss_wire_u16(reply + 12, 0);
        nss_wire_u16(reply + 14, 0);
        nss_wire_u32(reply + 16, sid);
        nss_wire_u16(reply + 20, (uint16_t)events);
        nss_send(e, reply, 22, tag);
        return;
    }
    default:
        /* Any verb outside the table: refused by name — increment 1's
         * contract, kept for exactly the types no increment defines. */
        nss_refuse(e, ty, tag, NSS_STATUS_CAP, 0);
        return;
    }
}

void net_socket_service_tick(void) {
    for (uint32_t i = 0; i < NET_SOCKET_SERVICE_MAX; i++) {
        struct NetSockSvc* e = &nss[i];
        if (!e->used) continue;

        for (;;) {
            uint32_t plen = 0, tag = 0, flags = 0;
            nss_out_n_caps = 0;
            int r = cap_recv_msg(0, e->k_rd, nss_buf,
                                 (uint32_t)sizeof nss_buf,
                                 &plen, CAP_MSG_MAX_CAPS, nss_out_caps,
                                 &nss_out_n_caps, &tag, &flags);
            if (r != 0) break;
            if (plen > sizeof nss_buf) plen = (uint32_t)sizeof nss_buf;
            nss_handle(e, plen, tag);
            if (!e->used) break;   /* nss_send retired it mid-drain */
        }
        if (!e->used) continue;

        /* A closed peer ends the channel: the sidecar's half is gone, so
         * retire the registration and revoke the kernel's end — the same
         * close-awareness console_service_tick() and env_console_tick()
         * apply, so a destroyed environment leaves no dead entry behind. */
        int kdir = 0;
        struct CapChannel* ch = nss_chan_of(e->k_rd, &kdir);
        int ended = 0;
        if (!ch) {
            ended = 1;
        } else {
            cap_lock(&ch->lock);
            ended = ch->close_evt[kdir];
            cap_unlock(&ch->lock);
        }
        if (ended) {
            if (!ch) e->k_rd = 0xFFFFu;  /* nothing left to revoke */
            nss_release(e);
        }
    }
}
