/*
 * console_tx_atomic_host_test.c — the atomicity guard for
 * console_service_tick()'s sidecar→UART drain (kernel/console_service.c).
 * Links the REAL kernel/cap.c, kernel/console_service.c, kernel/kernel_io.c
 * and kernel/console.c; port I/O is substituted by the
 * tests/kernel_io_panic_port.h seam, and cap.c's frame/page-table hooks are
 * the same host-memory stubs cap_create_sidecar_host_test.c uses.
 *
 * ─── The bug this exists to prevent from returning ─────────────────────────
 * The drain prints a whole sidecar console message out of ONE static buffer
 * (console_svc_buf), and holds it for as long as the UART takes to shift the
 * bytes: 4 KiB at 115200 baud is ~356 ms. The tick runs on TWO cores — the
 * AP's service poll and the BSP's deferred timer tick — so a second drainer
 * refills that buffer while the first is still printing from it. The first
 * then emits the OTHER message's bytes, and the bytes at the seam are lost
 * or duplicated. Seen in the boot log as a dropped byte ('System ready'
 * arrived as 'ystem ready' in 27 of 158 logs) and as the shell's prompt pair
 * arriving as "$$ " instead of "$ $ ".
 *
 * The fix is the RX half's own remedy: a compare-and-swap admits exactly one
 * drainer per round, and the loser returns with the message still queued.
 * (The RX half already had one, for the same reason: typed input came back
 * as "ecoh" and one of two typed lines was lost outright.)
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 * A console pair is minted (cap_chan_create) and messages are queued from the
 * far end — the sidecar's side of the channel. The UART model's TX hook calls
 * console_service_tick() again from INSIDE the outer tick's print loop, which
 * is exactly the interleaving the two cores produce, minus the timing luck:
 *
 *   1. one message prints whole and in order;
 *   2. two queued messages print whole, in order, with no splice and no lost
 *      byte, even though the other core's tick arrives mid-message — and
 *      that concurrent tick prints NOTHING (it declined the shared buffer);
 *   3. the next tick still drains (the flag is released, not stranded);
 *   4. the byte stream is byte-for-byte what the sidecar sent.
 *
 * (2) is the discriminator: without the single-flight CAS the inner call
 * drains and prints the second message into the middle of the first, so the
 * stream comes out duplicated and truncated. Verified by reverting the CAS
 * and re-running — see the commit message.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/console_tx_atomic_host_test \
 *       tests/console_tx_atomic_host_test.c kernel/cap.c \
 *       kernel/console_service.c kernel/kernel_io.c kernel/console.c
 *   /tmp/console_tx_atomic_host_test
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

/* ─── POSIX-Environments E1: the unified-boot yield ─────────────────────── */
/* kernel/kernel_io.c's read_line() (the shell's idle point) calls the
 * cooperative yield kernel_yield_to_ring3(), whose real definition lives in
 * kernel/process.c — a file this test deliberately does not link. On a
 * non-unified boot no control plane is planted, so the real function returns
 * immediately; the stand-in is therefore behaviourally exact here. Defined
 * HERE rather than in tests/process_host_stubs.h because process.c DEFINES
 * the function (same reasoning as console_forward_skip_host_test.c). */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

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

/* SYS_SLS_FREE_REGION's frame-pool hook (POSIX-Environments E5) — the
 * allocator's counterpart. Reached only by that syscall, which this test never
 * issues; the real one's bitmap/owner/counter behaviour is covered by
 * frame_quota_host_test.c's contiguous scenarios and end to end by
 * tests/env_recycle_boot_check.sh. */
int free_contiguous_frames_for_partition(uint64_t base_addr, uint64_t nframes,
                                         uint32_t partition_id) {
    (void)base_addr; (void)nframes; (void)partition_id;
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

/* ─── the UART model (kio_test_* declared by the force-included seam) ─────
 * LSR bit 5 (THR empty) is always set, so TX never spins; the TX bytes are
 * captured verbatim as the "wire" this test compares. */

#define TX_CAP 4096
static char     g_tx[TX_CAP];
static int      g_tx_n;

/* The concurrent tick. -1 = never; otherwise the TX byte index at which the
 * "other core" calls console_service_tick() again — i.e. INSIDE the outer
 * tick's print loop. */
static int      g_reenter_at = -1;
static int      g_reentry_calls;
static int      g_reentry_bytes;   /* bytes the concurrent tick printed */

void kio_test_outb(uint16_t port, uint8_t val) {
    if (port != SERIAL_COM1_BASE) return;   /* the THR, not MCR/loopback */
    if (g_tx_n < TX_CAP - 1) g_tx[g_tx_n] = (char)val;
    g_tx_n++;                                /* count even past the cap */
    g_tx[g_tx_n < TX_CAP ? g_tx_n : TX_CAP - 1] = '\0';
    if (g_reenter_at >= 0 && g_tx_n == g_reenter_at) {
        int armed = g_reenter_at;
        g_reenter_at = -1;                   /* fire once */
        int before = g_tx_n;
        g_reentry_calls++;
        console_service_tick();              /* the other core's tick */
        g_reentry_bytes += g_tx_n - before;
        g_reenter_at = armed;
    }
}

uint8_t kio_test_inb(uint16_t port) {
    switch ((int)(port - SERIAL_COM1_BASE)) {
    case 5: /* LSR: THR empty (bit 5) set, no input queued (bit 0 clear) */
        return 0x20;
    default:
        return 0;
    }
}

static void wire_reset(void) { g_tx_n = 0; g_tx[0] = '\0'; }

/* ─── helpers ────────────────────────────────────────────────────────────── */
#define FAR_PID 200

static uint16_t g_kw, g_fwr;

/* Queue one message from the sidecar's end of the console channel — what the
 * sidecar pump does with whatever the shell or an applet wrote to fd 1. */
static int queue_msg(const char* s) {
    return cap_send_msg(FAR_PID, g_fwr, s, (uint32_t)strlen(s), 0, 0, 0, 0);
}

int main(void) {
    cap_init();
    wire_reset();

    uint16_t krd = CAP_NONE, frd = CAP_NONE;
    int r = cap_chan_create(0, FAR_PID, &krd, &g_kw, &frd, &g_fwr);
    if (r != 0) {
        printf("FAIL: cap_chan_create rc=%d\n", r);
        return 1;
    }
    CHECK(1, "console pair minted (kernel holds CHAN_R + CHAN_W)");

    /* ── 1. One message prints whole and in order. ───────────────────── */
    wire_reset();
    CHECK(queue_msg("$ ") == 0, "the shell's prompt queued as one message");
    console_service_tick();
    CHECK(strcmp(g_tx, "$ ") == 0,
          "the prompt reached the wire whole: '$ '");

    /* ── 2. The discriminator: the other core's tick arrives in the middle
     *       of a message. With the single-flight CAS it must decline the
     *       shared buffer; without it, it refills the buffer and the outer
     *       loop then prints the second message's bytes twice. ─────────── */
    wire_reset();
    CHECK(queue_msg("$ ") == 0, "prompt queued again");
    CHECK(queue_msg("System ready\r\n") == 0, "status line queued behind it");
    g_reentry_calls = 0;
    g_reentry_bytes = 0;
    g_reenter_at = 1;            /* the concurrent tick lands after byte 1 */
    console_service_tick();
    g_reenter_at = -1;
    int ordered = (strcmp(g_tx, "$ System ready\r\n") == 0);
    CHECK(ordered,
          "both messages printed whole and in order (no splice, no lost byte)");
    if (!ordered) printf("      wire: [%s]\n", g_tx);
    CHECK(g_reentry_calls == 1,
          "the concurrent tick ran (re-entrancy was exercised)");
    CHECK(g_reentry_bytes == 0,
          "the concurrent tick printed nothing — it declined the shared buffer");

    /* ── 3. The flag is released: the next tick still drains. ─────────── */
    wire_reset();
    CHECK(queue_msg("after\r\n") == 0, "a third message queued");
    console_service_tick();
    CHECK(strcmp(g_tx, "after\r\n") == 0,
          "the drain resumes on the next tick (flag released, not stranded)");

    /* ── 4. Byte-for-byte fidelity over a longer message, with a rival tick
     *       at every byte boundary AND a second message already waiting for
     *       it to steal. A rival with an empty queue is harmless in both
     *       builds (there is nothing to refill the buffer with), so the
     *       follow-up message is what gives this scenario its teeth. ──── */
    static const char long_msg[] =
        "[CAP] objects active=31/1024  arena free=16384/16384 frames\r\n";
    wire_reset();
    CHECK(queue_msg(long_msg) == 0, "a 62-byte console line queued");
    CHECK(queue_msg("$ ") == 0, "the shell's next prompt queued behind it");
    g_reentry_calls = 0;
    g_reentry_bytes = 0;
    for (int at = 1; at <= (int)strlen(long_msg); at++) {
        g_reenter_at = at;       /* every byte boundary gets a rival tick */
        console_service_tick();
        g_reenter_at = -1;
    }
    int faithful = (strcmp(g_tx, "[CAP] objects active=31/1024  arena free=16384/16384 frames\r\n$ ") == 0);
    CHECK(faithful,
          "the line then the prompt, whole and once each, byte for byte");
    if (!faithful) printf("      wire: [%s]\n", g_tx);
    CHECK(g_reentry_bytes == 0,
          "none of the 62 rival ticks printed anything");

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
