#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "../kernel/kernel_io.h"
#include "../kernel/net_event.h"
#include "../include/config.h"

// ─── DHCP message layout (RFC 2131) ──────────────────────────────────────────
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63538263UL  // 99.130.82.99 — note: stored LE below

// DHCP op codes
#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2

// DHCP option tags
#define DHCP_OPT_MSG_TYPE    53
#define DHCP_OPT_SERVER_ID   54
#define DHCP_OPT_REQ_IP      50
#define DHCP_OPT_LEASE_TIME  51
#define DHCP_OPT_SUBNET      1
#define DHCP_OPT_ROUTER      3
#define DHCP_OPT_END         255
#define DHCP_OPT_PAD         0

#define DHCP_MSG_DISCOVER  1
#define DHCP_MSG_OFFER     2
#define DHCP_MSG_REQUEST   3
#define DHCP_MSG_ACK       5
#define DHCP_MSG_NAK       6

// Fixed DHCP magic cookie in network byte order
// RFC 2131: 99.130.83.99 = 0x63825363
#define DHCP_MAGIC_COOKIE  0x63825363UL

// ─── DHCP packet structure ────────────────────────────────────────────────────
// Only the fixed 240-byte header is declared here.  The options field that
// follows is variable-length; we access it via pointer arithmetic after
// casting the raw buffer, using `data + len` as the boundary.
struct DHCPMessage {
    uint8_t  op;
    uint8_t  htype;   // 1 = Ethernet
    uint8_t  hlen;    // 6
    uint8_t  hops;    // 0
    uint32_t xid;     // transaction ID
    uint16_t secs;
    uint16_t flags;   // 0x8000 = broadcast flag
    uint32_t ciaddr;  // client IP (0.0.0.0 before bound)
    uint32_t yiaddr;  // your IP (filled by server in OFFER/ACK)
    uint32_t siaddr;  // server IP
    uint32_t giaddr;  // relay agent IP
    uint8_t  chaddr[16]; // client hardware address (MAC + padding)
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;   // 0x63825363 in host order after ntohl()
    // options[] follow here; length varies per packet
} __attribute__((packed));

// ─── DHCP client state ────────────────────────────────────────────────────────
static volatile uint8_t  dhcp_state         = 0;  // 0=idle,1=offered,2=acked
static volatile uint32_t dhcp_offered_ip    = 0;
static volatile uint32_t dhcp_server_id     = 0;
static volatile uint32_t dhcp_offered_gw    = 0;
// Navigator-Parity Gap Roadmap Phase 5a: previously there was no subnet
// concept anywhere in this client at all -- dhcp_recv() never read
// DHCP_OPT_SUBNET. Added following dhcp_offered_gw's exact pattern (offer
// stashes it, ACK commits it to the real net.h global).
static volatile uint32_t dhcp_offered_subnet = 0;
static const uint32_t    dhcp_xid           = 0xAE524C53UL; // "SLSR" fixed XID

// ─── Option helpers ───────────────────────────────────────────────────────────
static uint8_t* opt_append(uint8_t* p, uint8_t tag, uint8_t len, uint32_t val) {
    *p++ = tag;
    *p++ = len;
    for (int i = len - 1; i >= 0; i--) { *p++ = (uint8_t)(val >> (i * 8)); }
    return p;
}

// ─── DHCP receive callback (registered with UDP on port 68) ──────────────────
static void dhcp_recv(const uint8_t* data, uint16_t len, IPv4Addr src_ip) {
    (void)src_ip;
    // Minimum check: fixed DHCP header (240 bytes) only.
    // The options field varies; the parser uses `end = data + len` for bounds.
    #define DHCP_FIXED_LEN 240  // all fields before options[]
    if (len < DHCP_FIXED_LEN) return;

    const struct DHCPMessage* msg = (const struct DHCPMessage*)data;

    // Verify magic cookie and XID
    if (ntohl(msg->magic) != DHCP_MAGIC_COOKIE) return;
    if (ntohl(msg->xid) != dhcp_xid) return;
    if (msg->op != DHCP_OP_REPLY) return;

    // Parse options — they start immediately after the 240-byte fixed header
    uint8_t  msg_type       = 0;
    uint32_t server_id      = 0;
    uint32_t offered_gw     = 0;
    uint32_t offered_subnet = 0;   // Navigator-Parity Gap Roadmap Phase 5a
    const uint8_t* opt = data + sizeof(struct DHCPMessage);
    const uint8_t* end = data + len;
    while (opt < end && *opt != DHCP_OPT_END) {
        if (*opt == DHCP_OPT_PAD) { opt++; continue; }
        uint8_t tag = *opt++;
        if (opt >= end) break;
        uint8_t olen = *opt++;
        if (opt + olen > end) break;
        if (tag == DHCP_OPT_MSG_TYPE  && olen >= 1) msg_type  = opt[0];
        if (tag == DHCP_OPT_SERVER_ID && olen >= 4)
            server_id = ((uint32_t)opt[0]<<24)|((uint32_t)opt[1]<<16)|
                        ((uint32_t)opt[2]<<8) |opt[3];
        if (tag == DHCP_OPT_ROUTER    && olen >= 4)
            offered_gw= ((uint32_t)opt[0]<<24)|((uint32_t)opt[1]<<16)|
                        ((uint32_t)opt[2]<<8) |opt[3];
        // Navigator-Parity Gap Roadmap Phase 5a: DHCP_OPT_SUBNET (tag 1) was
        // never read here before -- no subnet concept existed anywhere in
        // this client.
        if (tag == DHCP_OPT_SUBNET   && olen >= 4)
            offered_subnet = ((uint32_t)opt[0]<<24)|((uint32_t)opt[1]<<16)|
                             ((uint32_t)opt[2]<<8) |opt[3];
        opt += olen;
    }

    if (msg_type == DHCP_MSG_OFFER && dhcp_state == 0) {
        // Store offered IP (yiaddr is already in network byte order)
        dhcp_offered_ip = msg->yiaddr;
        dhcp_server_id  = htonl(server_id);   // back to network order
        // Convert gateway/subnet to network order for storage
        if (offered_gw) {
            dhcp_offered_gw = htonl(offered_gw);
        }
        if (offered_subnet) {
            dhcp_offered_subnet = htonl(offered_subnet);
        }
        dhcp_state = 1;  // OFFERED
    } else if (msg_type == DHCP_MSG_ACK && dhcp_state == 1) {
        // Commit the IP
        net_my_ip = msg->yiaddr;
        if (dhcp_offered_gw) net_gw_ip = dhcp_offered_gw;
        if (dhcp_offered_subnet) net_subnet_mask = dhcp_offered_subnet;
        dhcp_state = 2;  // ACKED / BOUND
    }
}

// ─── Build and send a DHCP message ───────────────────────────────────────────
// We need a fixed 552-byte buffer (240 fixed + 312 options) for our outgoing
// messages; use a local array rather than sizeof(DHCPMessage) since the struct
// no longer embeds the options[].
#define DHCP_MSG_SIZE  552   // 240 fixed + 312 options field (RFC 2131 minimum)

static void dhcp_send(uint8_t msg_type) {
    uint8_t buf[DHCP_MSG_SIZE];
    for (int i = 0; i < DHCP_MSG_SIZE; i++) buf[i] = 0;
    struct DHCPMessage* msg = (struct DHCPMessage*)buf;

    msg->op    = DHCP_OP_REQUEST;
    msg->htype = 1;  // Ethernet
    msg->hlen  = 6;
    msg->xid   = htonl(dhcp_xid);
    msg->flags = htons(0x8000);  // request broadcast reply (safer before we have an IP)
    msg->magic = htonl(DHCP_MAGIC_COOKIE);

    // Fill client hardware address with our MAC
    for (int i = 0; i < 6; i++) msg->chaddr[i] = net_my_mac.b[i];

    // Build options — write into the bytes immediately after the fixed header
    uint8_t* p = buf + sizeof(struct DHCPMessage);
    p = opt_append(p, DHCP_OPT_MSG_TYPE, 1, msg_type);

    if (msg_type == DHCP_MSG_REQUEST) {
        // REQUEST must include Requested IP (option 50) and Server ID (option 54)
        // both in big-endian host value form
        uint32_t req_ip  = ntohl(dhcp_offered_ip);
        uint32_t srv_id  = ntohl(dhcp_server_id);
        p = opt_append(p, DHCP_OPT_REQ_IP,    4, req_ip);
        p = opt_append(p, DHCP_OPT_SERVER_ID, 4, srv_id);
    }
    *p++ = DHCP_OPT_END;

    // DHCP DISCOVER/REQUEST is sent from 0.0.0.0:68 to 255.255.255.255:67
    udp_send(0, 0xFFFFFFFFUL,
             DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
             buf, DHCP_MSG_SIZE);
}

// ─── Public entry point ───────────────────────────────────────────────────────
void dhcp_start(void) {
    kernel_serial_print("[DHCP] Starting DISCOVER...\n");

    dhcp_state = 0;
    udp_register(DHCP_CLIENT_PORT, dhcp_recv);

    // Send DISCOVER and wait up to ~3 s for an OFFER (timer fires ~100 Hz)
    dhcp_send(DHCP_MSG_DISCOVER);
    for (int tick = 0; tick < 300 && dhcp_state == 0; tick++)
        net_event_hlt_wait();

    if (dhcp_state == 0) {
        kernel_serial_print("[DHCP] No OFFER received — using static IP.\n");
        udp_register(DHCP_CLIENT_PORT, 0);
        return;
    }

    // Send REQUEST and wait for ACK
    dhcp_send(DHCP_MSG_REQUEST);
    for (int tick = 0; tick < 300 && dhcp_state == 1; tick++)
        net_event_hlt_wait();

    udp_register(DHCP_CLIENT_PORT, 0);  // unregister callback

    if (dhcp_state == 2) {
        // net_my_ip, net_gw_ip, and net_subnet_mask are now set; print them
        uint32_t ip = ntohl(net_my_ip);
        uint32_t gw = ntohl(net_gw_ip);
        uint32_t sm = ntohl(net_subnet_mask);
        kernel_serial_printf(
            "[DHCP] Bound: %u.%u.%u.%u  gw %u.%u.%u.%u  subnet %u.%u.%u.%u\n",
            (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF,
            (gw>>24)&0xFF, (gw>>16)&0xFF, (gw>>8)&0xFF, gw&0xFF,
            (sm>>24)&0xFF, (sm>>16)&0xFF, (sm>>8)&0xFF, sm&0xFF);
    } else {
        kernel_serial_print("[DHCP] REQUEST timed out — using static IP.\n");
    }
}

// ─── dhcp_is_bound ────────────────────────────────────────────────────────────
// Navigator-Parity Gap Roadmap Phase 5a.
int dhcp_is_bound(void) {
    return dhcp_state == 2;
}
