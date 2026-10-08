/* net_socket_service.c — see net_socket_service.h. P2 first increment. */
#include "net_socket_service.h"
#include "cap.h"
#include "kernel_io.h"

/* The wire, spelled out in C. user/proto/src/lib.rs is the source of
 * truth for these bytes (NetFrame: magic[8] "AEROSNT\x01", version u16,
 * ty u16, flags u16, pad u16 — 16 bytes little-endian); they are written
 * here because the kernel does not link Rust, and the host test pins the
 * same constants against the client's own layout. */
#define NSS_MAGIC_LEN 8
#define NSS_HDR 16u          /* NetFrame::SIZE */
#define NSS_VERB_INFO 1u     /* NET_INFO — the handshake, answered honestly */
#define NSS_ERR_FLAG 0x0001u /* NET_FLAG_ERROR */
#define NSS_STATUS_CAP 9u    /* NET_ERR_CAP: the admission that does not
                              * exist yet is a capability-shaped refusal */
#define NSS_MTU 1500u
#define NSS_MAX_SOCKS 0u     /* nothing admits yet — P2 increment 1 */
#define NSS_TEXT_CAP 192u    /* room for the longest rendered refusal */

struct NetSockSvc {
    int      used;
    uint32_t partition;
    uint32_t pid;
    uint16_t k_rd;        /* kernel (pid 0) CHAN_R — requests arrive here */
    uint16_t k_wr;        /* kernel (pid 0) CHAN_W — replies leave here */
    char     name[32];    /* the caller's sidecar name: the refusal names it */
};

static struct NetSockSvc nss[NET_SOCKET_SERVICE_MAX];
/* One shared drain buffer, single static, for the same reason
 * env_console.c's is: this tick runs in one non-IRQ kernel context at a
 * time (microkernel_service_poll), and a channel's payload bound is what
 * sizes it — a frame is never silently truncated. */
static uint8_t  nss_buf[4096];
static uint32_t nss_refusals_total = 0;
static uint32_t nss_dropped_total = 0;

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
                     " — nothing admits yet (P2 increment 1)");
    *p = '\0';
    return (uint32_t)(p - out);
}

/* ─── registry ────────────────────────────────────────────────────────────*/

int net_socket_service_register(uint16_t k_rd, uint16_t k_wr,
                                uint32_t partition, uint32_t pid,
                                const char* name) {
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
        "wr=%u — admits nothing yet (P2 increment 1)\n",
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
         * honestly — max_sockets 0 (nothing admits yet), MTU 1500 (the
         * kernel stack's own), flags 0. The verbs below are where the
         * refusal lives; §6 scopes verbs as socket/connect/send/recv/
         * shutdown/close (plus bind/listen/accept), and NET_INFO is the
         * gate that proves a peer speaks the protocol at all. */
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

    /* Every other verb: the refusal by name — the reply echoes the
     * request's ty (the client type-checks it), sets NET_FLAG_ERROR,
     * carries NET_ERR_CAP, and appends the rendered refusal after the
     * status body. The wire's parse reads the status and ignores the
     * text; the text's audience is the host test (which pins the
     * rendering) and the serial line below (which names the caller) —
     * and appending it keeps the answer itself carrying the name. */
    char text[NSS_TEXT_CAP];
    uint32_t tlen = net_socket_refusal_text(ty, text, sizeof text);
    uint8_t reply[16 + 10 + NSS_TEXT_CAP];
    for (uint32_t i = 0; i < 8; i++) reply[i] = nss_buf[i];
    nss_wire_u16(reply + 8, 1);                  /* version */
    nss_wire_u16(reply + 10, ty);                /* echo the verb */
    nss_wire_u16(reply + 12, NSS_ERR_FLAG);      /* NET_FLAG_ERROR */
    nss_wire_u16(reply + 14, 0);                 /* pad */
    nss_wire_u16(reply + 16, NSS_STATUS_CAP);    /* status: NET_ERR_CAP */
    for (uint32_t i = 0; i < 8; i++) reply[18 + i] = 0;  /* value u64 = 0 */
    for (uint32_t i = 0; i < tlen; i++) reply[26 + i] = (uint8_t)text[i];
    nss_refusals_total++;
    kernel_serial_printf("[NET-SOCKET] %s (partition %u, pid %u): %s\n",
                         e->name, (unsigned)e->partition, (unsigned)e->pid,
                         text);
    nss_send(e, reply, 26 + tlen, tag);
}

void net_socket_service_tick(void) {
    for (uint32_t i = 0; i < NET_SOCKET_SERVICE_MAX; i++) {
        struct NetSockSvc* e = &nss[i];
        if (!e->used) continue;

        for (;;) {
            uint32_t plen = 0, tag = 0, flags = 0;
            uint16_t n_caps = 0;
            int r = cap_recv_msg(0, e->k_rd, nss_buf,
                                 (uint32_t)sizeof nss_buf,
                                 &plen, 0, 0, &n_caps, &tag, &flags);
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
