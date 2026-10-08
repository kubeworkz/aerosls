/*
 * net_socket_service_host_test.c — the P2 THIRD increment's host guard
 * (POSIX-Environments v0.2 §6, kernel/net_socket_service.c).
 *
 * Links the REAL kernel/cap.c, kernel/net_socket_service.c,
 * kernel/kernel_io.c, kernel/console.c and net/tcp_quota.c — the
 * attribution + per-partition connection-quota module, executed for real,
 * not reimplemented. Port I/O is substituted by the
 * tests/kernel_io_panic_port.h seam, and cap.c's frame/page-table hooks
 * are the same host-memory stubs tests/env_console_host_test.c uses. The
 * channels are minted by the real cap_chan_create — nothing here is a mock
 * of the wire it asserts on. Only the KERNEL STACK's data path (net/tcp.c:
 * tcp_connect/tcp_listen/tcp_send/tcp_recv/tcp_close plus tcp_conn_release,
 * which tcp_quota.c does not define) is stubbed, because linking the real
 * 16 MiB tcp_conns[] and its ARP/IPv4 egress is out of scope for a host
 * test and is exercised by the live clause instead. Every property that is
 * THIS increment's — the non-blocking listener, the accepted conn's
 * attribution at hand-out, the isolation that falls out of tcp_conns[]
 * being one kernel-global pool — runs against the real code.
 *
 * ─── The property this exists to protect ───────────────────────────────────
 * P2's third increment is the other half of §6's "client and listener":
 * bind/listen/accept through the service, each partition's listener
 * reachable only from inside that partition. The load-bearing part is the
 * NON-BLOCKING accept: tcp_accept() blocks forever via net_event_hlt_wait()
 * (it would stall the whole kernel in this non-IRQ tick), so the service
 * instead does one scan of tcp_conns[] per tick — the same scan http.c's
 * own pickup loop does — for an ESTABLISHED conn on the listener's port.
 * The accepted conn is attributed to the caller's partition at the instant
 * it is handed out (the INBOUND half of the unattributed tooth: an inbound
 * connection with no partition counts against nobody). Isolation is
 * structural, not a check: tcp_conns[] is one kernel-global pool, so a
 * listener on port P only ever matches a SYN the stack already delivered to
 * port P — a cross-partition socket is not representable at this wire.
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 *   1. the verb table and the refusal renderer (unchanged from increment 1):
 *      eleven names, the "verb <n>" arm, the returned length, and the
 *      always-NUL-terminated/truncated-at-cap contract;
 *   2. the registry: register/slot-claim, kernel CHAN_W never claimed,
 *      re-registration REPLACES (E5 recycle), and a ninth channel refused
 *      when full;
 *   3. the wire: NET_INFO is answered honestly with max_sockets the REAL
 *      NSS_MAX_SOCKS, MTU 1500, tag echoed; a non-SOCK_STREAM socket and an
 *      out-of-table verb get NET_FLAG_ERROR + NET_ERR_CAP (9) with the
 *      rendered refusal by name; non-NET frames are counted dropped and
 *      never answered; the refusal is logged on serial naming the caller;
 *   4. the OUTBOUND FLOW (increment 2, kept green): socket() returns a sock
 *      id; connect() decodes the LE sockaddr, attributes the partition,
 *      binds the stubbed conn, and is counted admitted with the partition's
 *      usage +1; send()/recv() resolve REAL R/W grant caps and the stub sees
 *      exactly the tenant's bytes; the LE decode is pinned;
 *   5. the LISTENER FLOW, the increment's heart: bind() records the port,
 *      listen() draws an inbound stack slot, accept() with nothing pending
 *      answers NET_EAGAIN (a STATUS, not a refusal — the counters stay
 *      honest) and with a planted ESTABLISHED conn returns a NEW socket id
 *      whose conn is attributed to the partition (usage +1, admits +1) while
 *      the LISTENER STAYS LISTENING for the next one;
 *   6. the QUOTA TEETH (increment 2, kept green) and the UNATTRIBUTED tooth:
 *      an over-quota connect is refused WITHOUT the stack being called and
 *      WITHOUT attribution (deny-before-side-effect), and a refused accept
 *      (over-quota, no free slot) likewise leaves usage unchanged;
 *   7. teardown: a sidecar that closed its end is retired by the next tick,
 *      its open sockets' and LISTENERS' attributions/stack slots released,
 *      kernel slot unclaimed, neighbour untouched.
 *
 * Build and run:
 *   gcc -no-pie -Wall -Wextra -std=c11 -I . -I kernel -I net -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/net_socket_service_host_test \
 *       tests/net_socket_service_host_test.c kernel/cap.c \
 *       kernel/net_socket_service.c kernel/kernel_io.c kernel/console.c \
 *       net/tcp_quota.c
 *   /tmp/net_socket_service_host_test
 */
#include "kernel/cap.h"
#include "kernel/net_socket_service.h"
#include "kernel/kernel_io.h"
#include "net/tcp.h"           /* IPv4Addr, TCP_MAX_CONNS (stub sizing) */
#include "net/tcp_quota.h"     /* the real attribution/quota module */
#include "tests/process_host_stubs.h"   /* per_cpu_data (weak), etc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    /* "ok:" at column 0 is not cosmetic -- tests/run_all.sh counts checks \
     * with grep -c '^ok:'. */ \
    if (cond) { checks_passed++; printf("ok:   %s\n", msg); } \
    else      { checks_failed++; printf("FAIL: %s\n", msg); } \
} while (0)

/* kernel/kernel_io.c's read_line() calls the cooperative yield, whose real
 * definition is in kernel/process.c — a file this test deliberately does not
 * link. Same reasoning as env_console_host_test.c. */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

/* net/tcp_quota.c's uid form (tcp_conn_attribute) calls this to resolve a
 * bearer-token uid to a partition. This test drives only the PARTITION-DIRECT
 * form (tcp_conn_attribute_partition), never the uid form, so the stub is a
 * link-only placeholder returning PARTITION_DEFAULT — the same convention
 * 15+ other host tests use (see tcp_quota_host_test.c for the settable fake
 * when the uid path itself is under test). */
uint32_t partition_get_for_uid(uint32_t uid) { (void)uid; return 0; }

/* ─── cap.c link stubs (same set as env_console_host_test.c) ────── */
char _kernel_image_end[1];

struct ProcessDescriptor proc_table[PROC_MAX];
uint32_t proc_count = 0;

uint32_t alloc_pid(void) {
    uint32_t best = 0;
    for (int i = 0; i < PROC_MAX; i++)
        if (proc_table[i].active && proc_table[i].pid > best)
            best = proc_table[i].pid;
    return best + 1;
}

uint64_t alloc_proc_syscall_stack(uint32_t partition_id) {
    (void)partition_id;
    return 0x400000007000ULL;
}

uint64_t frame_pool_reserve_contiguous(uint64_t nframes, uint64_t align_frames) {
    (void)align_frames;
    static uint8_t* arena;
    if (!arena) {
        uint8_t* raw = malloc((size_t)nframes * 4096u + 4096);
        if (!raw) return 0;
        arena = (uint8_t*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
        memset(arena, 0, (size_t)nframes * 4096u);
    }
    return (uint64_t)(uintptr_t)arena;
}

void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    uint8_t* raw = malloc(8192);
    if (!raw) return 0;
    return (void*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
}

uint64_t allocate_contiguous_frames_for_partition(uint32_t partition_id,
                                                  uint64_t nframes,
                                                  uint64_t align_frames) {
    (void)partition_id; (void)nframes; (void)align_frames;
    return 0;
}

int free_contiguous_frames_for_partition(uint64_t base_addr, uint64_t nframes,
                                         uint32_t partition_id) {
    (void)base_addr; (void)nframes; (void)partition_id;
    return 0;
}

uint64_t user_clone_page_table(void) { return 0; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages, uint64_t flags) {
    (void)pml4; (void)phys; (void)npages; (void)flags;
    return 0;
}

/* ─── kernel_io.c stubs ──────────────────────────────────────────────────── */
int  vga_is_ready(void)        { return 0; }
void vga_putchar(char c)       { (void)c; }
void vga_init(void)            { }
void vga_clear(void)           { }
void vga_print(const char *s)  { (void)s; }
int  smp_cpu_id(void)          { return 0; }
int  smp_cpu_count(void)       { return 1; }
void smp_uniprocessor_tick(void) { }

/* The UART model: output is swallowed at the port — serial TRAFFIC is
 * asserted through kernel_io.c's own capture buffer (kernel_serial_capture_*
 *), which diverts putchar before the port write. Input is "nothing ready". */
static uint8_t g_rx[512];
static int     g_rx_n, g_rx_i;
void kio_test_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
uint8_t kio_test_inb(uint16_t port) {
    switch ((int)(port - SERIAL_COM1_BASE)) {
    case 0: return (g_rx_i < g_rx_n) ? g_rx[g_rx_i++] : 0;
    case 5: return 0x20 | ((g_rx_i < g_rx_n) ? 0x01 : 0x00);
    default: return 0;
    }
}

/* ─── helpers ────────────────────────────────────────────────────────────── */
/* One sidecar's socket channel: the kernel's two ends and the sidecar's two.
 * Same shape env_console_host_test.c's Pair — cap_chan_create is the real
 * mint the syscall path uses. */
struct Pair {
    uint16_t krd, kw, frd, fwr;
    uint32_t far_pid;
};

static int mint_pair(struct Pair* p, uint32_t far_pid) {
    p->far_pid = far_pid;
    return cap_chan_create(0, far_pid, &p->krd, &p->kw, &p->frd, &p->fwr);
}

/* The far end (the sidecar) sends a raw payload. */
static int far_send(const struct Pair* p, const void* b, uint32_t n,
                    uint32_t tag) {
    return cap_send_msg(p->far_pid, p->fwr, b, n, 0, 0, tag, 0);
}

/* The far end drains one reply, keeping the tag. */
static int far_recv(const struct Pair* p, char* out, size_t cap,
                    uint32_t* out_tag) {
    uint32_t plen = 0, tag = 0, flags = 0;
    uint16_t nc = 0;
    int r = cap_recv_msg(p->far_pid, p->frd, (uint8_t*)out, (uint32_t)cap,
                         &plen, 0, 0, &nc, &tag, &flags);
    if (r != 0) return -1;
    out[plen < cap ? plen : cap - 1] = '\0';
    if (out_tag) *out_tag = tag;
    return (int)plen;
}

/* ─── net/tcp.c stubs — the kernel stack's data path ────────────────
 * tcp_quota.c (the real attribution/quota logic) is LINKED, not stubbed.
 * Only the five tcp.c entry points the service reaches are substituted,
 * because the real tcp_conns[] is 16 MiB and its egress needs ARP/IPv4/
 * a NIC — out of scope for a host test, and covered by the live clause.
 * Each stub records what it was called with so the test can assert the
 * exact conn_id, host-order ip, port, and byte count the service passed.
 * tcp_conn_release() lives in tcp_quota.c (real), so it is NOT stubbed. */
static int  g_tcp_calls_connect, g_tcp_calls_send, g_tcp_calls_recv;
static int  g_tcp_calls_close;
static int  g_tcp_calls_listen;
static int  g_tcp_next_conn = TCP_INBOUND_MAX_CONNS;  /* an outbound-slot id */
/* tcp_listen()'s stub returns an INBOUND-slot id (below
 * TCP_INBOUND_MAX_CONNS), the same pool the real tcp_listen() draws from. */
static int  g_tcp_next_listener = 0;
static IPv4Addr g_tcp_last_ip;
static uint16_t g_tcp_last_port;
static int  g_tcp_last_conn = -1;
static uint32_t g_tcp_last_send_len, g_tcp_last_recv_max;
static uint8_t  g_tcp_sent[256];
static uint32_t g_tcp_sent_len;
/* A recv "canned" reply the stub copies into the tenant's W-grant buffer. */
static uint8_t  g_tcp_recv_data[256];
static uint32_t g_tcp_recv_data_len;

void tcp_stack_reset(void) {
    g_tcp_calls_connect = g_tcp_calls_send = g_tcp_calls_recv = 0;
    g_tcp_calls_close = g_tcp_calls_listen = 0;
    g_tcp_last_ip = 0; g_tcp_last_port = 0; g_tcp_last_conn = -1;
    g_tcp_last_send_len = g_tcp_last_recv_max = 0;
    g_tcp_sent_len = 0;
    g_tcp_recv_data_len = 0;
    g_tcp_next_listener = 0;
}

int tcp_listen(uint16_t port) {
    g_tcp_calls_listen++;
    g_tcp_last_port = port;
    return g_tcp_next_listener++;
}

/* tcp_conns[] — the kernel-global connection pool the service's non-blocking
 * accept scan walks. Defined here because tcp.c (which owns the real one,
 * 16 MiB of statics) is stubbed out entirely. The test plants ESTABLISHED
 * entries directly to drive the accept path. */
struct TCPConn tcp_conns[TCP_MAX_CONNS];

int tcp_connect(IPv4Addr dst_ip, uint16_t dst_port) {
    g_tcp_calls_connect++;
    g_tcp_last_ip = dst_ip;
    g_tcp_last_port = dst_port;
    if (dst_ip == 0) return -1;          /* the service must refuse ip 0 */
    return g_tcp_next_conn++;
}

int tcp_send(int conn_id, const void* buf, uint32_t len) {
    g_tcp_calls_send++;
    g_tcp_last_conn = conn_id;
    g_tcp_last_send_len = len;
    uint32_t n = len < sizeof g_tcp_sent ? len : (uint32_t)sizeof g_tcp_sent;
    memcpy(g_tcp_sent, buf, n);
    g_tcp_sent_len = n;
    return (int)len;
}

int tcp_recv(int conn_id, void* buf, uint16_t max_len) {
    g_tcp_calls_recv++;
    g_tcp_last_conn = conn_id;
    g_tcp_last_recv_max = max_len;
    uint32_t n = g_tcp_recv_data_len < max_len ? g_tcp_recv_data_len : max_len;
    memcpy(buf, g_tcp_recv_data, n);
    return (int)n;
}

void tcp_close(int conn_id) {
    g_tcp_calls_close++;
    g_tcp_last_conn = conn_id;
}

/* The far end sends a raw payload WITH a moved MEM grant (the send/recv
 * data buffer). The descriptor's slot is a cap-table slot in the FAR pid's
 * table; the service resolves it in the KERNEL's table, so this mirrors the
 * real NetClient send path exactly. */
static int far_send_caps(const struct Pair* p, const void* b, uint32_t n,
                         uint32_t tag, const struct SLSCapDesc* d,
                         uint16_t n_caps) {
    return cap_send_msg(p->far_pid, p->fwr, b, n, d, n_caps, tag, 0);
}

/* Build a 16-byte NetFrame request header (user/proto's layout). */
static void net_frame(uint8_t* f, uint16_t ty) {
    static const char magic[8] = { 'A','E','R','O','S','N','T',1 };
    memcpy(f, magic, 8);
    f[8]  = 1; f[9]  = 0;                     /* version 1 */
    f[10] = (uint8_t)(ty & 0xFFu); f[11] = (uint8_t)(ty >> 8);
    f[12] = 0; f[13] = 0; f[14] = 0; f[15] = 0;
}

static uint16_t reply_u16(const char* p, int off) {
    return (uint16_t)(((uint8_t)p[off]) | ((uint16_t)(uint8_t)p[off + 1] << 8));
}
static uint32_t reply_u32(const char* p, int off) {
    return ((uint32_t)(uint8_t)p[off])
         | ((uint32_t)(uint8_t)p[off + 1] << 8)
         | ((uint32_t)(uint8_t)p[off + 2] << 16)
         | ((uint32_t)(uint8_t)p[off + 3] << 24);
}

/* ─── Link stubs for kernel/env_storage.h ────────────────────────────────
 * Same answers env_console_host_test.c uses: nothing is attached, nothing
 * persists, restore of a missing entry is the contract's successful no-op. */
#include "kernel/env_storage.h"
uint64_t env_storage_attach(uint32_t partition_id, uint32_t index,
                            uint64_t region_base, uint64_t region_bytes) {
    (void)partition_id; (void)index; (void)region_base; (void)region_bytes;
    return ENV_STORAGE_ERR_IO;
}
uint64_t env_storage_restore(uint64_t region_base) {
    (void)region_base;
    return ENV_STORAGE_OK;
}
uint64_t env_storage_write(uint64_t region_base, uint32_t lba, uint32_t bytes) {
    (void)region_base; (void)lba; (void)bytes;
    return ENV_STORAGE_ERR_NOENT;
}
uint64_t env_storage_release(uint64_t region_base) {
    (void)region_base;
    return ENV_STORAGE_ERR_NOENT;
}
uint32_t env_storage_owner(uint64_t region_base) {
    (void)region_base;
    return 0xFFFFFFFFu;
}

/* The refusal text's shared suffix — asserted here rather than retyped per
 * check, so a wording change in the renderer fails one obvious place.
 * Increment 3 rotates the wording: the verbs that DO admit now include
 * bind/listen/accept, so the refusal names only what remains — an
 * out-of-table verb type and the malformed-argument arms of an admitted
 * verb. */
#define REFUSAL_SUFFIX \
    " refused by kernel.net.socket — not admitted in P2 increment 3"

int main(void) {
    cap_init();

    /* ── 1. the verb table: eleven names, nothing outside ────────────────── */
    {
        static const char* expect[11] = {
            "NET_INFO", "NET_SOCKET", "NET_BIND", "NET_CONNECT", "NET_LISTEN",
            "NET_ACCEPT", "NET_SEND", "NET_RECV", "NET_SHUTDOWN",
            "NET_CLOSE_SOCK", "NET_POLL",
        };
        for (uint32_t i = 0; i < 11; i++) {
            const char* got = net_socket_verb_name((uint16_t)(i + 1));
            char msg[64];
            snprintf(msg, sizeof msg, "verb %u is named %s",
                     (unsigned)(i + 1), expect[i]);
            CHECK(got && strcmp(got, expect[i]) == 0, msg);
        }
        CHECK(net_socket_verb_name(0) == 0, "verb 0 is outside the table");
        CHECK(net_socket_verb_name(12) == 0, "verb 12 is outside the table");
        CHECK(net_socket_verb_name(0xFFFFu) == 0,
              "an absurd verb resolves to no name");
    }

    /* ── 2. the refusal renderer, called directly ────────────────────────── */
    {
        char t[192];
        uint32_t n = net_socket_refusal_text(4, t, sizeof t);
        CHECK(strcmp(t, "NET_CONNECT" REFUSAL_SUFFIX) == 0,
              "a known verb renders its name plus the one refusal text");
        CHECK(n == strlen(t), "the renderer returns the length it wrote");
        n = net_socket_refusal_text(2, t, sizeof t);
        CHECK(strcmp(t, "NET_SOCKET" REFUSAL_SUFFIX) == 0,
              "NET_SOCKET renders by name too (the verb the live clause drives)");
        n = net_socket_refusal_text(99, t, sizeof t);
        CHECK(strcmp(t, "verb 99" REFUSAL_SUFFIX) == 0,
              "an unknown verb renders with its number, not nothing");
        CHECK(n == strlen(t), "...and the unknown arm's length is honest too");

        char small[11];
        memset(small, 'X', sizeof small);
        n = net_socket_refusal_text(4, small, sizeof small);
        CHECK(n == 10 && small[10] == '\0' && strlen(small) == 10,
              "a small cap truncates and STILL terminates");  /* 10 = cap-1 */
        char one[1] = { 'X' };
        n = net_socket_refusal_text(4, one, 1);
        CHECK(n == 0 && one[0] == '\0',
              "cap 1 yields the empty string, still terminated");
        CHECK(net_socket_refusal_text(4, t, 0) == 0, "cap 0 writes nothing");
        CHECK(net_socket_refusal_text(4, NULL, sizeof t) == 0,
              "a null buffer writes nothing");
    }

    /* ── 3. the registry: claim, replace, fill, refuse ───────────────────── */
    CHECK(net_socket_service_count() == 0,
          "no channel is registered before cap.c wires one");

    struct Pair a, b;
    if (mint_pair(&a, 100) != 0 || mint_pair(&b, 101) != 0) {
        printf("FAIL: cap_chan_create failed\n");
        return 1;
    }
    CHECK(net_socket_service_register(a.krd, a.kw, 1, 100,
                                      "aerosls.posix.0") == 1,
          "the kernel end of the tenant's network cap registers");
    CHECK(net_socket_service_count() == 1, "the registry counts it");
    CHECK(net_socket_service_kernel_slot(a.krd) == 1,
          "its kernel CHAN_R is claimed (console_service must skip it)");
    CHECK(net_socket_service_kernel_slot(a.kw) == 0,
          "the kernel CHAN_W is not a drain slot and is not claimed");
    CHECK(net_socket_service_kernel_slot((uint16_t)(CAP_TABLE_ENTRIES - 1)) == 0,
          "an unminted slot is not claimed");

    CHECK(net_socket_service_register(b.krd, b.kw, 1, 101,
                                      "aerosls.posix.1") == 1,
          "a second sidecar registers in the same partition");
    CHECK(net_socket_service_count() == 2, "two channels are registered");

    /* Re-registration of the same (partition, pid) REPLACES the entry
     * (E5's recycle) — the registry must not fill with dead environments'
     * channels, and the OLD kernel end must stop being skipped by the
     * console service. */
    struct Pair a2;
    if (mint_pair(&a2, 100) != 0) { printf("FAIL: re-mint\n"); return 1; }
    CHECK(net_socket_service_register(a2.krd, a2.kw, 1, 100,
                                      "aerosls.posix.0") == 1,
          "the same (partition, pid) re-registers (environment re-created)");
    CHECK(net_socket_service_count() == 2,
          "re-registering replaces instead of adding a stale entry");
    CHECK(net_socket_service_kernel_slot(a.krd) == 0,
          "the replaced entry's old kernel end is no longer claimed");
    CHECK(net_socket_service_kernel_slot(a2.krd) == 1,
          "the new kernel end is the claimed one");

    /* Fill the registry to NET_SOCKET_SERVICE_MAX and refuse the ninth. */
    {
        struct Pair fill[6];
        int all = 1;
        for (uint32_t i = 0; i < 6; i++) {
            if (mint_pair(&fill[i], (uint32_t)(110 + i)) != 0 ||
                net_socket_service_register(fill[i].krd, fill[i].kw, 1,
                                            (uint32_t)(110 + i),
                                            "aerosls.posix.f") != 1)
                all = 0;
        }
        CHECK(all, "six more sidecars register (registry at capacity)");
        CHECK(net_socket_service_count() == NET_SOCKET_SERVICE_MAX,
              "the registry holds exactly NET_SOCKET_SERVICE_MAX channels");
        struct Pair extra;
        if (mint_pair(&extra, 120) != 0) { printf("FAIL: mint\n"); return 1; }
        CHECK(net_socket_service_register(extra.krd, extra.kw, 1, 120,
                                          "aerosls.posix.x") == 0,
              "a ninth channel is refused — the registry is full");
        CHECK(net_socket_service_count() == NET_SOCKET_SERVICE_MAX,
              "the refused registration claimed no slot of its own");
    }

    /* ── 4. the wire ─────────────────────────────────────────────────────── */
    /* NET_INFO: answered HONESTLY — the channel is typed, so the handshake
     * proves the peer speaks the protocol even though nothing admits. */
    {
        uint8_t f[16];
        net_frame(f, 1);                       /* NET_INFO */
        CHECK(far_send(&a2, f, 16, 42) == 0, "the sidecar sends NET_INFO");
        net_socket_service_tick();
        char reply[512];
        uint32_t tag = 0;
        int n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 28, "NET_INFO is answered with the 28-byte body");
        CHECK(tag == 42, "the reply echoes the request's tag");
        CHECK(reply_u16(reply, 10) == 1, "the reply's verb is NET_INFO");
        CHECK(reply_u16(reply, 12) == 0, "the handshake is not an error");
        CHECK(reply_u32(reply, 16) == NSS_MAX_SOCKS,
              "max_sockets is the real per-channel ceiling (P2 increment 2)");
        CHECK(reply_u32(reply, 20) == 1500, "MTU 1500 (the kernel stack's own)");
        CHECK(reply_u32(reply, 24) == 0, "the reserved word answers 0");
        CHECK(net_socket_refusals() == 0,
              "the handshake is answered, not refused");
    }

    /* The out-of-table verb (99) and the malformed-argument arms of an
     * admitted verb are refused BY NAME: NET_FLAG_ERROR + NET_ERR_CAP (9) +
     * the rendered text after the status body. A bare NET_BIND with no body
     * drives the malformed arm (bind needs a sockaddr); verb 99 drives the
     * out-of-table arm. Neither bind/listen/accept nor any defined verb is
     * refused for being undefined any more — increment 3 admits them. */
    {
        uint8_t f[16];
        net_frame(f, 3);                       /* NET_BIND, no body = malformed */
        CHECK(far_send(&a2, f, 16, 43) == 0, "the sidecar sends a bodyless NET_BIND");
        net_socket_service_tick();
        char reply[512];
        uint32_t tag = 0;
        int n = far_recv(&a2, reply, sizeof reply, &tag);
        uint32_t want = (uint32_t)(26 + strlen("NET_BIND" REFUSAL_SUFFIX));
        CHECK((uint32_t)n == want,
              "the refusal reply is the status body plus the rendered text");
        CHECK(tag == 43, "the refusal echoes the request's tag too");
        CHECK(reply_u16(reply, 10) == 3,
              "the reply echoes the verb the client type-checks");
        CHECK(reply_u16(reply, 12) == 0x0001,
              "the reply carries NET_FLAG_ERROR");
        CHECK(reply_u16(reply, 16) == 9,
              "the status is NET_ERR_CAP — a malformed argument, not an undefined verb");
        CHECK(reply_u32(reply, 18) == 0 && reply_u32(reply, 22) == 0,
              "the value word is zero");
        CHECK(strcmp(reply + 26, "NET_BIND" REFUSAL_SUFFIX) == 0,
              "the reply carries the refusal BY NAME");
        CHECK(net_socket_refusals() == 1, "one refusal counted");
    }

    /* ── 4b. the outbound flow: the increment's heart ────────────────────
     * socket() → connect() (LE decode + attribution + quota) → send()/
     * recv() through REAL R/W grant caps. tcp_quota.c (attribution/quota) is
     * LINKED and real; only tcp.c's five data-path entry points are stubbed.
     * The LE decode is pinned: 127.0.0.1 as the host-order integer
     * 0x7F000001 must reach the stub as that exact integer, not byte-swapped
     * (a swap would route to the wrong host), and the stub's return value is
     * an OUTBOUND-range conn_id, which attribution charges to the partition. */
    {
        tcp_stack_reset();
        char reply[512];
        uint32_t tag;
        int n;

        /* socket(): {sock_type u16 = SOCK_STREAM(1), protocol u16 = 0}. */
        uint8_t sock_req[20];
        net_frame(sock_req, 2);
        sock_req[16] = 1; sock_req[17] = 0; sock_req[18] = 0; sock_req[19] = 0;
        CHECK(far_send(&a2, sock_req, 20, 50) == 0,
              "the sidecar sends NET_SOCKET(SOCK_STREAM)");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 12) == 0 && reply_u16(reply, 16) == 0,
              "NET_SOCKET is answered OK (no longer a refusal)");
        uint32_t sock_a = reply_u32(reply, 18);   /* value = sock id */
        CHECK(sock_a < NSS_MAX_SOCKS, "the socket id is within the channel's table");
        CHECK(net_socket_admits() == 0, "a bare socket() is not yet an admitted connect");

        /* connect(): {sock_id u32 LE, sockaddr {ip u32 LE, port u16 LE}} =
         * 4 + 6 = 10 bytes after the 16-byte header. The client holds the
         * LOGICAL ip 0x7F000001 (127.0.0.1) and encode_sockaddr writes it
         * LITTLE-endian, so the wire bytes are 01 00 00 7F. The service's
         * LE decode recovers 0x7F000001, then converts to the NETWORK order
         * (0x0100007F) tcp_connect()/ipv4_send() require (the kernel stores
         * IPs network-order — see KERNEL_STATIC_IP 0x0F02000A for 10.0.2.15).
         * The stub must see EXACTLY the network-order 127.0.0.1 and port
         * 8000, and return an OUTBOUND-range conn_id that gets attributed
         * to partition 1. */
        /* 26 bytes = 16 hdr + 4 sock_id + 6 sockaddr. */
        uint8_t conn_req[26];
        memset(conn_req, 0, sizeof conn_req);
        net_frame(conn_req, 4);
        conn_req[16] = (uint8_t)sock_a; conn_req[17] = 0; conn_req[18] = 0; conn_req[19] = 0;
        conn_req[20] = 0x01; conn_req[21] = 0x00; conn_req[22] = 0x00; conn_req[23] = 0x7F; /* ip 127.0.0.1 (logical 0x7F000001, LE on wire) */
        conn_req[24] = 0x40; conn_req[25] = 0x1F; /* port 8000 */
        CHECK(far_send(&a2, conn_req, 26, 51) == 0, "the sidecar sends NET_CONNECT(127.0.0.1:8000)");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 12) == 0 && reply_u16(reply, 16) == 0,
              "NET_CONNECT is admitted (answered OK)");
        CHECK(g_tcp_calls_connect == 1, "the connect reached the kernel stack exactly once");
        CHECK(g_tcp_last_ip == 0x0100007Fu,
              "the LE sockaddr decodes to network-order 127.0.0.1 (as tcp_connect wants)");
        CHECK(g_tcp_last_port == 8000, "the port decodes from the LE sockaddr");
        CHECK(net_socket_admits() == 1, "one connect counted admitted");
        CHECK(tcp_partition_get_conn_usage(1) == 1,
              "the admitted connect is attributed to partition 1 (usage 1)");
        CHECK(tcp_partition_get_conn_usage(0) == 0,
              "no other partition was charged — nothing counts against nobody");

        /* send(): {sock_id u32} + a REAL R-grant over a MEM buffer holding
         * "hello". The grant's slot is in the far pid's table; the service
         * resolves it in the kernel table to phys_base + offset and reads
         * the bytes. The stub must see len 5 and those exact bytes. */
        /* A page-aligned heap buffer (same convention as the frame_pool
         * stubs above): cap_create_mem() rejects an unaligned phys_base and
         * any range overlapping [0x100000, _kernel_image_end). -no-pie puts
         * _kernel_image_end at ~0x40xxxx and this malloc sits above it,
         * within the 4 GiB identity map. */
        uint8_t* membuf = NULL;
        uint8_t* membuf_raw = NULL;
        {
            void* raw = malloc(8192);
            if (!raw) { printf("FAIL: malloc for the send buffer\n"); return 1; }
            membuf_raw = (uint8_t*)raw;
            membuf = (uint8_t*)(((uintptr_t)raw + 4095u) & ~(uintptr_t)4095u);
        }
        memset(membuf, 0, 4096);
        memcpy(membuf, "hello", 5);
        uint16_t mem_slot = 0;
        if (cap_create_mem(a2.far_pid, (uint64_t)(uintptr_t)membuf, 1,
                           0x3 /* R|W */, &mem_slot) != 0) {
            printf("FAIL: cap_create_mem for the send buffer\n");
            return 1;
        }
        uint8_t send_req[20];
        net_frame(send_req, 7);                /* NET_SEND */
        send_req[16] = (uint8_t)sock_a; send_req[17] = 0; send_req[18] = 0; send_req[19] = 0;
        struct SLSCapDesc sd = { .slot = mem_slot, .offset = 0, .len = 5,
                                 .rights = 0x1 /* R */, .flags = 0 };
        CHECK(far_send_caps(&a2, send_req, 20, 52, &sd, 1) == 0,
              "the sidecar sends NET_SEND with an R-grant over the data");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0 && reply_u32(reply, 18) == 5,
              "the send reports 5 bytes accepted");
        CHECK(g_tcp_calls_send == 1 && g_tcp_last_send_len == 5,
              "the stack send saw exactly the granted length");
        CHECK(g_tcp_sent_len == 5 && memcmp(g_tcp_sent, "hello", 5) == 0,
              "the stack send saw exactly the tenant's bytes through the R-grant");

        /* recv(): {sock_id u32} + a REAL W-grant. The grant is FRESH — the
         * send above consumed mem_slot (a moved, non-persistent cap), and a
         * real recv carries its own W buffer distinct from the send's R one.
         * The stub fills this new buffer with a canned reply; the service
         * must have resolved the W-grant and reported the byte count. */
        memcpy(g_tcp_recv_data, "world", 5);
        g_tcp_recv_data_len = 5;
        uint8_t* rbuf_raw = (uint8_t*)malloc(8192);
        if (!rbuf_raw) { printf("FAIL: malloc recv buffer\n"); return 1; }
        uint8_t* rbuf = (uint8_t*)(((uintptr_t)rbuf_raw + 4095u) & ~(uintptr_t)4095u);
        memset(rbuf, 0, 4096);
        uint16_t rslot = 0;
        if (cap_create_mem(a2.far_pid, (uint64_t)(uintptr_t)rbuf, 1,
                           0x3 /* R|W */, &rslot) != 0) {
            printf("FAIL: cap_create_mem for the recv buffer\n");
            return 1;
        }
        uint8_t recv_req[20];
        net_frame(recv_req, 8);                /* NET_RECV */
        recv_req[16] = (uint8_t)sock_a; recv_req[17] = 0; recv_req[18] = 0; recv_req[19] = 0;
        struct SLSCapDesc rd = { .slot = rslot, .offset = 0, .len = 16,
                                 .rights = 0x2 /* W */, .flags = 0 };
        CHECK(far_send_caps(&a2, recv_req, 20, 53, &rd, 1) == 0,
              "the sidecar sends NET_RECV with a W-grant over the buffer");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0 && reply_u32(reply, 18) == 5,
              "the recv reports 5 bytes received");
        CHECK(g_tcp_calls_recv == 1 && g_tcp_last_recv_max == 16,
              "the stack recv saw the granted buffer length");
        CHECK(memcmp(rbuf, "world", 5) == 0,
              "the W-grant buffer was filled with the stack's reply");
        free(rbuf_raw);
        free(membuf_raw);
    }

    /* ── 4c. the QUOTA TEETH + the UNATTRIBUTED tooth ────────────────────
     * With partition 1's quota set to its current usage, one more connect
     * must be refused (counted) WITHOUT the stack being called and WITHOUT
     * attribution — deny-before-side-effect. Then the unattributed tooth: a
     * refused connect leaves usage unchanged, so no connection ever counts
     * against nobody. */
    {
        /* Drive the quota gate on a fresh socket. */
        uint8_t sock_req[20];
        char reply[512];
        net_frame(sock_req, 2);
        sock_req[16] = 1; sock_req[17] = 0; sock_req[18] = 0; sock_req[19] = 0;
        CHECK(far_send(&a2, sock_req, 20, 60) == 0, "the sidecar opens a fresh socket");
        net_socket_service_tick();
        (void)far_recv(&a2, reply, sizeof reply, NULL);
        uint32_t sock_q = reply_u32(reply, 18);

        /* Set partition 1's quota to its CURRENT usage (1) so the next
         * connect is over-quota. This is the operator opt-in the module's
         * 0 = unlimited default stands behind. */
        CHECK(tcp_partition_set_conn_quota(1, tcp_partition_get_conn_usage(1)) == 0,
              "partition 1's quota is set to its current usage");
        CHECK(tcp_partition_get_conn_quota(1) == tcp_partition_get_conn_usage(1),
              "the quota reads back what it was set to");

        int before_connect_calls = g_tcp_calls_connect;
        uint16_t before_usage = tcp_partition_get_conn_usage(1);
        uint32_t before_quota_refusals = net_socket_quota_refusals();
        /* A full, valid connect so the ONLY reason it is refused is the
         * quota (not a malformed address). 26 bytes = 16 hdr + 4 sock_id +
         * 6 sockaddr. */
        uint8_t req26[26];
        memset(req26, 0, sizeof req26);
        net_frame(req26, 4);
        req26[16] = (uint8_t)sock_q; req26[17] = 0; req26[18] = 0; req26[19] = 0;
        req26[20] = 0x7F; req26[21] = 0x00; req26[22] = 0x00; req26[23] = 0x01;
        req26[24] = 0x40; req26[25] = 0x1F;   /* 127.0.0.1:8000 */
        CHECK(far_send(&a2, req26, 26, 61) == 0,
              "the sidecar sends an over-quota NET_CONNECT");
        net_socket_service_tick();
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK((uint32_t)n == 26 + strlen("NET_CONNECT" REFUSAL_SUFFIX),
              "the over-quota refusal carries the rendered text");
        CHECK(reply_u16(reply, 12) == 0x0001 && reply_u16(reply, 16) == 9,
              "the over-quota connect is refused (NET_FLAG_ERROR + NET_ERR_CAP)");
        CHECK(g_tcp_calls_connect == before_connect_calls,
              "the refused connect never reached the stack — deny before side-effect");
        CHECK(net_socket_quota_refusals() == before_quota_refusals + 1,
              "one quota refusal counted");
        CHECK(tcp_partition_get_conn_usage(1) == before_usage,
              "the refused connect did not change partition 1's usage");
        CHECK(net_socket_admits() == 1, "the refused connect was not counted admitted");

        /* Lift the quota back to unlimited so teardown below is unaffected. */
        CHECK(tcp_partition_set_conn_quota(1, 0) == 0,
              "partition 1's quota is lifted back to unlimited");
    }

    /* ── 4d. the LISTENER FLOW: the increment's heart ────────────────────
     * bind() → listen() → accept(). tcp_accept() blocks forever in the real
     * stack (net_event_hlt_wait()), so the service does a non-blocking scan
     * of tcp_conns[] per tick — the http.c:6980 pickup shape. The accept
     * with nothing pending answers NET_EAGAIN (a STATUS, not a refusal), and
     * with a planted ESTABLISHED conn on the listener's port it returns a
     * NEW socket id whose conn is attributed to the partition while the
     * LISTENER STAYS LISTENING. tcp_conns[] is the extern tcp.c defines; the
     * test owns its storage (the stub table above replaces tcp.c entirely). */
    {
        tcp_stack_reset();
        char reply[512];
        uint32_t tag;
        int n;

        /* socket() for the listener. */
        uint8_t sock_req[20];
        net_frame(sock_req, 2);
        sock_req[16] = 1; sock_req[17] = 0; sock_req[18] = 0; sock_req[19] = 0;
        CHECK(far_send(&a2, sock_req, 20, 70) == 0, "the sidecar opens a listener socket");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0, "NET_SOCKET(listener) is answered OK");
        uint32_t lst = reply_u32(reply, 18);

        /* bind(): {sock_id u32 LE, sockaddr [u8;6]} — port 8080, ip 0 (any). */
        uint8_t bind_req[26];
        memset(bind_req, 0, sizeof bind_req);
        net_frame(bind_req, 3);                /* NET_BIND */
        bind_req[16] = (uint8_t)lst; bind_req[17] = 0; bind_req[18] = 0; bind_req[19] = 0;
        bind_req[20] = 0; bind_req[21] = 0; bind_req[22] = 0; bind_req[23] = 0; /* ip 0.0.0.0 */
        bind_req[24] = 0x90; bind_req[25] = 0x1F; /* port 8080 (LE) */
        CHECK(far_send(&a2, bind_req, 26, 71) == 0, "the sidecar sends NET_BIND(:8080)");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0,
              "NET_BIND is admitted (no longer a refusal)");

        /* listen(): {sock_id u32 LE}. Draws an inbound stack slot. */
        uint8_t lsn_req[20];
        net_frame(lsn_req, 5);                 /* NET_LISTEN */
        lsn_req[16] = (uint8_t)lst; lsn_req[17] = 0; lsn_req[18] = 0; lsn_req[19] = 0;
        CHECK(far_send(&a2, lsn_req, 20, 72) == 0, "the sidecar sends NET_LISTEN");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0, "NET_LISTEN is admitted");
        CHECK(g_tcp_calls_listen == 1 && g_tcp_last_port == 8080,
              "the listen reached the stack with the bound port");

        /* accept() with NOTHING pending: NET_EAGAIN (status 11), a STATUS
         * not a refusal — the counters must not move. */
        uint8_t acc_req[20];
        net_frame(acc_req, 6);                 /* NET_ACCEPT */
        acc_req[16] = (uint8_t)lst; acc_req[17] = 0; acc_req[18] = 0; acc_req[19] = 0;
        CHECK(far_send(&a2, acc_req, 20, 73) == 0, "the sidecar sends NET_ACCEPT (nothing pending)");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 12) == 0x0001 && reply_u16(reply, 16) == 11,
              "an accept with nothing pending answers NET_EAGAIN (status 11)");
        uint32_t refusals_before = net_socket_refusals();
        uint32_t admits_baseline = net_socket_admits();
        CHECK(net_socket_admits() == admits_baseline,
              "a pending-less accept is not an admit");

        /* Plant an ESTABLISHED conn on port 8080 in tcp_conns[] — the
         * inbound half of the unattributed tooth: it is NOT yet attributed
         * (usage 0) until the service hands it out. */
        int planted = 7;
        tcp_conns[planted].active = 1;
        tcp_conns[planted].state = TCP_ESTABLISHED;
        tcp_conns[planted].local_port = 8080;
        uint16_t usage_before_acc = tcp_partition_get_conn_usage(1);

        /* accept() with the planted conn: returns a NEW socket id, the conn
         * is attributed (usage +1), admits +1, and the listener STAYS. */
        CHECK(far_send(&a2, acc_req, 20, 74) == 0, "the sidecar sends NET_ACCEPT (conn ready)");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0,
              "the ready accept is admitted (no error)");
        uint32_t accepted = reply_u32(reply, 18);
        CHECK(accepted < NSS_MAX_SOCKS && accepted != lst,
              "the accept hands back a NEW socket id, not the listener's");
        CHECK(net_socket_admits() == admits_baseline + 1, "one accept counted admitted");
        CHECK(tcp_partition_get_conn_usage(1) == usage_before_acc + 1,
              "the accepted conn is attributed to the partition (usage +1)");
        CHECK(net_socket_refusals() == refusals_before,
              "the accepted path refused nothing");

        /* The listener STAYS LISTENING — a second accept on the same socket
         * is still valid. The stub does not tear down the accepted conn
         * (tcp_conns[7] is still ESTABLISHED on port 8080, as a real stack
         * would keep it until closed), so the second accept hands it out
         * AGAIN as a fresh socket — proving the listener was not consumed by
         * the first accept. The quota-refusal arm below then pins that an
         * over-quota accept closes the conn and charges nobody (the
         * no-quota tooth's inbound half, increment 4). */
        CHECK(far_send(&a2, acc_req, 20, 75) == 0, "the sidecar sends NET_ACCEPT again");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        CHECK(n == 26 && reply_u16(reply, 16) == 0,
              "the listener is still listening — a second accept succeeds again");
        CHECK(net_socket_admits() == admits_baseline + 2,
              "the second accept is counted admitted too (the listener was not consumed)");

        /* Quota-refusal arm of the accept: on a FRESH channel (clean socket
         * table — the listener-arm channel above has spent its slots), set
         * the partition's quota to its current usage, plant a ready conn, and
         * accept. The conn must be closed and charged to nobody (usage
         * unchanged), refused with a quota counter bump — the inbound half of
         * the no-quota tooth increment 4 will drive. The quota gate sits
         * AFTER the free-slot check, so this must run on a channel with a
         * free slot to reach it. */
        uint8_t sockb[20];
        net_frame(sockb, 2);
        sockb[16] = 1; sockb[17] = 0; sockb[18] = 0; sockb[19] = 0;
        CHECK(far_send(&b, sockb, 20, 80) == 0, "the spare channel opens a listener socket");
        net_socket_service_tick();
        n = far_recv(&b, reply, sizeof reply, &tag);
        uint32_t lstb = reply_u32(reply, 18);
        uint8_t bindb[26];
        memset(bindb, 0, sizeof bindb);
        net_frame(bindb, 3);
        bindb[16] = (uint8_t)lstb; bindb[17] = 0; bindb[18] = 0; bindb[19] = 0;
        bindb[24] = 0x90; bindb[25] = 0x1F;   /* port 8080 */
        CHECK(far_send(&b, bindb, 26, 81) == 0, "the spare channel binds :8080");
        net_socket_service_tick();
        (void)far_recv(&b, reply, sizeof reply, NULL);
        uint8_t lsnb[20];
        net_frame(lsnb, 5);
        lsnb[16] = (uint8_t)lstb; lsnb[17] = 0; lsnb[18] = 0; lsnb[19] = 0;
        CHECK(far_send(&b, lsnb, 20, 82) == 0, "the spare channel listens on :8080");
        net_socket_service_tick();
        (void)far_recv(&b, reply, sizeof reply, NULL);
        CHECK(tcp_partition_set_conn_quota(1, tcp_partition_get_conn_usage(1)) == 0,
              "partition 1's quota is set to its current usage for the accept");
        /* A real tcp_listen() draws a slot from g_tcp_next_conn and can never
         * land on 0 (the conn scan skips the listener's OWN index), but the
         * stub's tcp_listen() pins conn_id 0, so plant the ready conn at a
         * high index — genuinely distinct from the listener — so the scan
         * reaches it instead of skipping it as the listener's own slot. */
        int planted2 = 6;
        memset(&tcp_conns[planted2], 0, sizeof tcp_conns[planted2]);
        tcp_conns[planted2].active = 1;
        tcp_conns[planted2].state = TCP_ESTABLISHED;
        tcp_conns[planted2].local_port = 8080;
        uint16_t usage_pre_q = tcp_partition_get_conn_usage(1);
        uint32_t qref_before = net_socket_quota_refusals();
        int closes_before_q = g_tcp_calls_close;
        uint8_t accb[20];
        net_frame(accb, 6);
        accb[16] = (uint8_t)lstb; accb[17] = 0; accb[18] = 0; accb[19] = 0;
        CHECK(far_send(&b, accb, 20, 83) == 0, "the spare channel sends an over-quota NET_ACCEPT");
        net_socket_service_tick();
        n = far_recv(&b, reply, sizeof reply, &tag);
        CHECK(n == 26 + strlen("NET_ACCEPT" REFUSAL_SUFFIX) &&
              reply_u16(reply, 12) == 0x0001 && reply_u16(reply, 16) == 9,
              "the over-quota accept is refused BY NAME");
        CHECK(g_tcp_calls_close == closes_before_q + 1,
              "the refused accept closed the pending conn — deny before side-effect");
        CHECK(tcp_partition_get_conn_usage(1) == usage_pre_q,
              "the refused accept did not change partition usage (charged nobody)");
        CHECK(net_socket_quota_refusals() == qref_before + 1,
              "one accept quota refusal counted");
        CHECK(tcp_partition_set_conn_quota(1, 0) == 0,
              "partition 1's quota is lifted back to unlimited");

        /* An accept() on a NON-listener (a plain connected socket) is a
         * malformed-argument refusal BY NAME — the guard's listener clause
         * pins that the listener state is actually checked. */
        net_frame(sock_req, 2);
        sock_req[16] = 1; sock_req[17] = 0; sock_req[18] = 0; sock_req[19] = 0;
        (void)far_send(&a2, sock_req, 20, 76);
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, &tag);
        uint32_t plain = reply_u32(reply, 18);
        uint8_t acc_plain[20];
        net_frame(acc_plain, 6);
        acc_plain[16] = (uint8_t)plain; acc_plain[17] = 0; acc_plain[18] = 0; acc_plain[19] = 0;
        CHECK(far_send(&a2, acc_plain, 20, 77) == 0, "the sidecar sends NET_ACCEPT on a non-listener");
        net_socket_service_tick();
        n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK((uint32_t)n == 26 + strlen("NET_ACCEPT" REFUSAL_SUFFIX) &&
              reply_u16(reply, 12) == 0x0001 && reply_u16(reply, 16) == 9,
              "an accept() on a non-listener is refused BY NAME (malformed arg)");
    }

    /* An unknown verb is refused as honestly as a known one. */
    {
        uint8_t f[16];
        net_frame(f, 99);
        CHECK(far_send(&a2, f, 16, 44) == 0, "the sidecar sends verb 99");
        net_socket_service_tick();
        char reply[512];
        int n = far_recv(&a2, reply, sizeof reply, NULL);
        CHECK((uint32_t)n == 26 + strlen("verb 99" REFUSAL_SUFFIX),
              "the unknown verb renders with its number");
        CHECK(reply_u16(reply, 10) == 99 && reply_u16(reply, 12) == 0x0001 &&
              reply_u16(reply, 16) == 9,
              "unknown is answered with the same error shape");
        CHECK(strcmp(reply + 26, "verb 99" REFUSAL_SUFFIX) == 0,
              "the unknown verb's refusal is carried by name too");
        CHECK(net_socket_refusals() == 5,
              "five refusals counted (bodyless bind, over-quota connect, verb 99, accept-on-nonlistener, accept-overquota)");
    }

    /* Non-NET frames are counted, never answered (a reply would parse as an
     * error by a client that never sent it). */
    {
        CHECK(far_send(&a2, "hello", 5, 45) == 0,
              "the sidecar sends a non-NET payload");
        net_socket_service_tick();
        char reply[512];
        CHECK(far_recv(&a2, reply, sizeof reply, NULL) == -1,
              "a non-NET frame gets no reply");
        CHECK(net_socket_dropped() == 1, "it is counted dropped");

        uint8_t f[16];
        memset(f, 0, sizeof f);                /* right size, wrong magic */
        CHECK(far_send(&a2, f, 16, 46) == 0, "the sidecar sends a bad header");
        net_socket_service_tick();
        CHECK(far_recv(&a2, reply, sizeof reply, NULL) == -1,
              "a bad header gets no reply either");
        CHECK(net_socket_dropped() == 2, "and is counted dropped too");
        CHECK(net_socket_refusals() == 5,
              "dropped frames are not refusals — nothing was answered");
    }

    /* The serial line: the refusal names the caller, the same rendering. This
     * capture starts AFTER the accept-on-non-listener above (which logged its
     * own NET_ACCEPT refusal line), so it contains exactly ONE refusal line —
     * the bodyless NET_BIND sent here. */
    {
        static char ser[1024];
        kernel_serial_capture_start(ser, sizeof ser);
        uint8_t f[16];
        net_frame(f, 3);                       /* NET_BIND, bodyless = malformed */
        (void)far_send(&a2, f, 16, 47);
        net_socket_service_tick();
        char reply[512];
        (void)far_recv(&a2, reply, sizeof reply, NULL);
        size_t slen = kernel_serial_capture_stop();
        /* kernel_io adds CR before LF on the port; strip CR so the greps
         * below are about the words, not the line ending. */
        for (size_t i = 0, j = 0; i < slen && j < sizeof ser - 1; i++)
            if (ser[i] != '\r') ser[j++] = ser[i];
        ser[slen < sizeof ser ? slen : sizeof ser - 1] = '\0';
        CHECK(strstr(ser,
                     "[NET-SOCKET] aerosls.posix.0 (partition 1, pid 100): "
                     "NET_BIND" REFUSAL_SUFFIX) != NULL,
              "serial logs the refusal by name, with the caller's identity");
        CHECK(strstr(ser, "NET_CONNECT") == NULL,
              "the earlier over-quota line is not re-logged in this capture");
        CHECK(net_socket_refusals() == 6, "the sixth refusal counted");
    }

    /* ── 5. teardown: a closed sidecar end retires the registration ─────── */
    cap_revoke(a2.far_pid, a2.frd);
    cap_revoke(a2.far_pid, a2.fwr);
    net_socket_service_tick();
    CHECK(net_socket_service_count() == NET_SOCKET_SERVICE_MAX - 1,
          "the closed sidecar's channel is retired by the next tick");
    CHECK(net_socket_service_kernel_slot(a2.krd) == 0,
          "the retired kernel end is no longer claimed");
    CHECK(net_socket_service_kernel_slot(b.krd) == 1,
          "its neighbour is untouched by the retirement");

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
