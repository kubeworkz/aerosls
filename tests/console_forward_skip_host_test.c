/*
 * console_forward_skip_host_test.c — the payload-pool regression guard for
 * console_service_tick()'s typed-input forward (kernel/console_service.c).
 * Links the REAL kernel/cap.c, kernel/console_service.c, kernel/kernel_io.c
 * and kernel/console.c; port I/O is substituted by the
 * tests/kernel_io_panic_port.h seam, and cap.c's frame/page-table hooks are
 * the same host-memory stubs cap_create_sidecar_host_test.c uses.
 *
 * ─── The bug this exists to prevent from returning ─────────────────────────
 * The kernel forwards every typed serial line to EVERY sidecar console
 * channel (all CHAN_W caps in the kernel table). The non-interactive
 * sidecars (init/dm/ramdisk/network) never recv their console input, so
 * each forwarded line pinned a 4 KiB message-payload frame forever. The
 * pool is 32 frames; after ~7 lines it exhausted and the interactive
 * shell's console — scanned last — starved with CAP_ENOSPC: typed
 * commands were echoed by the UART but never delivered (caught live: a
 * 20-command soak executed only 6).
 *
 * The fix: skip a console whose peer has not drained the previous line
 * (qdepth[dest] > 0). The interactive consumer drains within a pump
 * iteration, so its queue is empty between lines; non-consumers are
 * skipped after their first line, so the pool can never exhaust.
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 * Five console pairs are minted (cap_chan_create): pair 0 = the
 * interactive consumer (the test drains it after every tick, exactly like
 * the sidecar pump), pairs 1-4 = non-consumers that never recv. 30 lines
 * are typed through the real serial_console_poll()/console_feed() path:
 *
 *   1. line 1 reaches ALL five pairs (queues depth 1 each) — the skip
 *      must not suppress the first line;
 *   2. the interactive pair receives all 30 lines, byte-for-byte — pre-fix
 *      it starves at line ~9/30 once the four non-consumers exhaust the
 *      32-frame payload pool (CAP_ENOSPC), so this is the discriminator;
 *   3. the four non-consumer queues stay at depth exactly 1 — the skip;
 *      pre-fix they grow to the 16-message queue bound;
 *   4. the interactive queue is empty after its final drain.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/console_forward_skip_host_test \
 *       tests/console_forward_skip_host_test.c kernel/cap.c \
 *       kernel/console_service.c kernel/kernel_io.c kernel/console.c
 *   /tmp/console_forward_skip_host_test
 */
#include "kernel/cap.h"
#include "kernel/console_service.h"
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

/* ─── cap.c link stubs (same set as cap_create_sidecar_host_test.c) ────── */
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

/* cap_init() carves the cap arena through this hook; return aligned host
 * memory (real pointers, never dereferenced by this test). */
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

/* SYS_SLS_ALLOC_REGION's frame-pool hook (POSIX-Environments E3). This test
 * never issues that syscall; the inert stub just satisfies the linker. */
uint64_t allocate_contiguous_frames_for_partition(uint32_t partition_id,
                                                  uint64_t nframes,
                                                  uint64_t align_frames) {
    (void)partition_id; (void)nframes; (void)align_frames;
    return 0;
}

/* Page-table hooks are only reached by cap_create_sidecar (not called
 * here); inert stubs that satisfy the linker. */
uint64_t user_clone_page_table(void) { return 0; }
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    (void)pml4; (void)vaddr; (void)paddr; (void)flags;
}
int user_map_identity(uint64_t* pml4, uint64_t phys, uint32_t npages, uint64_t flags) {
    (void)pml4; (void)phys; (void)npages; (void)flags;
    return 0;
}

/* ─── kernel_io.c stubs (same set as serial_console_loopback_host_test.c) ── */
int  vga_is_ready(void)        { return 0; }
void vga_putchar(char c)       { (void)c; }
void vga_init(void)            { }
void vga_clear(void)           { }
void vga_print(const char *s)  { (void)s; }
int  smp_cpu_id(void)          { return 0; }
int  smp_cpu_count(void)       { return 1; }
void smp_uniprocessor_tick(void) { }

/* ─── the UART model (kio_test_* declared by the force-included seam) ───── */
static uint8_t  g_rx[512];
static int      g_rx_n, g_rx_i;
static int      g_tx_n;
static uint8_t  g_tx[1024];

void kio_test_outb(uint16_t port, uint8_t val) {
    /* TX bytes are captured so the test can count echo traffic, though no
     * assertion depends on it. */
    (void)port;
    if (g_tx_n < (int)sizeof(g_tx)) g_tx[g_tx_n++] = val;
}

uint8_t kio_test_inb(uint16_t port) {
    switch ((int)(port - SERIAL_COM1_BASE)) {
    case 0: /* RBR: pop the next queued byte */
        return (g_rx_i < g_rx_n) ? g_rx[g_rx_i++] : 0;
    case 5: /* LSR: data-ready only while the queue is nonempty; bit 5
             * (THR-empty) always set so TX-side waits never spin */
        return 0x20 | ((g_rx_i < g_rx_n) ? 0x01 : 0x00);
    default:
        return 0;
    }
}

static void uart_reset(void) { g_rx_n = g_rx_i = 0; g_tx_n = 0; }

static void uart_queue(const char* s) {
    for (; *s && g_rx_n < (int)sizeof(g_rx); s++) g_rx[g_rx_n++] = (uint8_t)*s;
}

/* ─── helpers ────────────────────────────────────────────────────────────── */
/* The channel id a kernel W cap references, for qdepth assertions. */
static uint32_t chan_id_of(uint16_t kernel_w_slot) {
    uint64_t w = cap_tables[0].slots[kernel_w_slot].word;
    uint32_t obj = (uint32_t)((w >> CAP_OBJ_SHIFT) & CAP_OBJ_MASK);
    return cap_objects[obj].chan_id;
}

/* The far end's queue depth for a channel whose end0 is the kernel: the
 * kernel's send dest index is 1, which is exactly the queue the far side
 * receives from (mirror of cap_send_msg/cap_recv_msg). */
static uint32_t far_qdepth(uint16_t kernel_w_slot) {
    struct CapChannel* ch = &cap_channels[chan_id_of(kernel_w_slot)];
    return ch->qdepth[(0 == ch->end0_pid) ? 1 : 0];
}

static uint32_t g_plen, g_tag, g_flags;
static uint16_t g_nc;
static char     g_buf[256];

/* Drain one message the way the sidecar pump does; returns the payload
 * length, or -1 if nothing was queued. */
static int pump_recv(uint32_t pid, uint16_t r_slot) {
    g_plen = 0;
    int r = cap_recv_msg(pid, r_slot, g_buf, sizeof(g_buf), &g_plen,
                         0, 0, &g_nc, &g_tag, &g_flags);
    if (r != 0) return -1;
    return (int)g_plen;
}

#define N_LINES 30
#define N_NONCONSUMERS 4

int main(void) {
    cap_init();
    uart_reset();

    /* Mint five console pairs: the kernel holds CHAN_R + CHAN_W; the far
     * end (the sidecar) holds the far CHAN_R + CHAN_W. Pair 0 is the
     * interactive consumer (pid 100); pairs 1-4 are non-consumers. Pair 0
     * is minted first so its W cap sits in the lowest kernel slot and the
     * forward scan reaches it first — pre-fix, the pool exhausts while
     * scanning the non-consumers and the interactive pair starves. */
    uint16_t kw[5], frd[5];
    for (int i = 0; i < 5; i++) {
        uint16_t krd = CAP_NONE, fwr = CAP_NONE;
        uint32_t far_pid = 100u + (uint32_t)i;
        int r = cap_chan_create(0, far_pid, &krd, &kw[i], &frd[i], &fwr);
        if (r != 0) {
            printf("FAIL: cap_chan_create(pair %d) rc=%d\n", i, r);
            return 1;
        }
    }

    /* ── 1. The first line reaches every pair (the skip must not suppress
     *       the first line: only channels with an undrained peer are
     *       skipped). */
    uart_queue("echo soak0\n");
    console_service_tick();
    for (int i = 0; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof msg,
                 "line 1 queued on pair %d (interactive=%d)", i, i == 0);
        CHECK(far_qdepth(kw[i]) == 1, msg);
    }

    /* ── 2+3. Drain the interactive pair after every tick and type 29
     *         more lines: the interactive pair must receive all 30, the
     *         non-consumers must never accumulate beyond line 1. */
    for (int i = 0; i < N_LINES; i++) {
        char line[64];
        snprintf(line, sizeof line, "echo soak%d\n", i);
        uart_queue(line);
        console_service_tick();
        int n = pump_recv(100, frd[0]);
        char msg[64];
        snprintf(msg, sizeof msg, "interactive pair received line %d", i + 1);
        CHECK(n == (int)strlen(line) &&
              strncmp(g_buf, line, (size_t)n) == 0, msg);
    }

    /* ── 4. Non-consumers stayed at depth 1 (the skip); pre-fix they grow
     *        to the 16-message queue bound (or the pool starves first). */
    for (int i = 1; i < 5; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "non-consumer pair %d queue depth stayed 1", i);
        CHECK(far_qdepth(kw[i]) == 1, msg);
    }

    /* Interactive queue drained clean. */
    CHECK(far_qdepth(kw[0]) == 0, "interactive queue empty after final drain");

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}