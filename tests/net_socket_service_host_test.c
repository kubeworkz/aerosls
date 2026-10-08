/*
 * net_socket_service_host_test.c — the P2 first increment's host guard
 * (POSIX-Environments v0.2 §6, kernel/net_socket_service.c).
 *
 * Links the REAL kernel/cap.c, kernel/net_socket_service.c,
 * kernel/kernel_io.c and kernel/console.c; port I/O is substituted by the
 * tests/kernel_io_panic_port.h seam, and cap.c's frame/page-table hooks are
 * the same host-memory stubs tests/env_console_host_test.c uses. The
 * channels the service is driven over are minted by the real cap_chan_create
 * — nothing here is a mock of the wire it asserts on.
 *
 * ─── The property this exists to protect ───────────────────────────────────
 * P2's first increment gives a tenant manifest a `network` Chan cap whose
 * peer is the KERNEL-OWNED name "kernel.net.socket" (never drv.network.0 —
 * pointing a tenant at the system partition's sidecar would cross the LPAR
 * boundary, which is the system-peer tooth). The kernel mints that channel
 * and registers it with this service. What the service then does is the
 * whole increment: it speaks NET_* and answers every verb with a refusal BY
 * NAME. Nothing admits yet — but the channel exists, is typed, and fails
 * honestly, and that wire is what increments 2-4 write admission into. A
 * service that answered garbage, admitted silently, or refused without a
 * name would each break the contract the rest of the phase is scoped to.
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 *   1. the verb table: all eleven NET_* names, and 0/out-of-range types
 *      resolve to NULL (the renderer's "unknown" arm, not a stale name);
 *   2. the refusal renderer itself — ONE rendering, three callers (host
 *      test, tick's serial line, guard's live clause): exact text for a
 *      known verb, the "verb <n>" arm for an unknown one, the returned
 *      length, and the always-NUL-terminated/truncated-at-cap contract
 *      (an unterminated renderer corrupts the serial transcript);
 *   3. the registry: register/slot-claim, the kernel CHAN_W never claimed,
 *      re-registration REPLACES the same (partition, pid) instead of
 *      leaking (E5 recycle), and a ninth channel is refused when full;
 *   4. the wire: NET_INFO is answered honestly (flags 0, max_sockets 0,
 *      MTU 1500, tag echoed — the handshake proves the peer speaks the
 *      protocol), every other verb gets a NET_FLAG_ERROR reply carrying
 *      NET_ERR_CAP (9) and the rendered refusal by name appended after the
 *      status body, non-NET frames are counted dropped and never answered
 *      (a reply would parse as an error by a client that never sent it),
 *      and the refusal line is logged on serial naming the caller;
 *   5. teardown: a sidecar that closed its end is retired by the next tick
 *      and its kernel slot is no longer claimed, neighbour untouched.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/net_socket_service_host_test \
 *       tests/net_socket_service_host_test.c kernel/cap.c \
 *       kernel/net_socket_service.c kernel/kernel_io.c kernel/console.c
 *   /tmp/net_socket_service_host_test
 */
#include "kernel/cap.h"
#include "kernel/net_socket_service.h"
#include "kernel/kernel_io.h"
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
 * check, so a wording change in the renderer fails one obvious place. */
#define REFUSAL_SUFFIX \
    " refused by kernel.net.socket — nothing admits yet (P2 increment 1)"

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

        char small[10];
        memset(small, 'X', sizeof small);
        n = net_socket_refusal_text(4, small, sizeof small);
        CHECK(n == 9 && small[9] == '\0' && strlen(small) == 9,
              "a small cap truncates and STILL terminates");
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
        CHECK(reply_u32(reply, 16) == 0,
              "max_sockets 0 — nothing admits yet (P2 increment 1)");
        CHECK(reply_u32(reply, 20) == 1500, "MTU 1500 (the kernel stack's own)");
        CHECK(reply_u32(reply, 24) == 0, "the reserved word answers 0");
        CHECK(net_socket_refusals() == 0,
              "the handshake is answered, not refused");
    }

    /* Every other verb: the refusal by name. */
    {
        uint8_t f[16];
        net_frame(f, 2);                       /* NET_SOCKET */
        CHECK(far_send(&a2, f, 16, 43) == 0, "the sidecar sends NET_SOCKET");
        net_socket_service_tick();
        char reply[512];
        uint32_t tag = 0;
        int n = far_recv(&a2, reply, sizeof reply, &tag);
        uint32_t want = (uint32_t)(26 + strlen("NET_SOCKET" REFUSAL_SUFFIX));
        CHECK((uint32_t)n == want,
              "the refusal reply is the status body plus the rendered text");
        CHECK(tag == 43, "the refusal echoes the request's tag too");
        CHECK(reply_u16(reply, 10) == 2,
              "the reply echoes the verb the client type-checks");
        CHECK(reply_u16(reply, 12) == 0x0001,
              "the reply carries NET_FLAG_ERROR");
        CHECK(reply_u16(reply, 16) == 9,
              "the status is NET_ERR_CAP — the admission that does not exist");
        CHECK(reply_u32(reply, 18) == 0 && reply_u32(reply, 22) == 0,
              "the value word is zero");
        CHECK(strcmp(reply + 26, "NET_SOCKET" REFUSAL_SUFFIX) == 0,
              "the reply carries the refusal BY NAME");
        CHECK(net_socket_refusals() == 1, "one refusal counted");
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
        CHECK(net_socket_refusals() == 2, "two refusals counted");
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
        CHECK(net_socket_refusals() == 2,
              "dropped frames are not refusals — nothing was answered");
    }

    /* The serial line: the refusal names the caller, the same rendering. */
    {
        static char ser[1024];
        kernel_serial_capture_start(ser, sizeof ser);
        uint8_t f[16];
        net_frame(f, 7);                       /* NET_SEND */
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
                     "NET_SEND" REFUSAL_SUFFIX) != NULL,
              "serial logs the refusal by name, with the caller's identity");
        CHECK(net_socket_refusals() == 3, "the third refusal counted");
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
