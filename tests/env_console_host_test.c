/*
 * env_console_host_test.c — the isolation guard for per-environment consoles
 * (POSIX-Environments E6, kernel/env_console.c).
 *
 * Links the REAL kernel/cap.c, kernel/env_console.c, kernel/kernel_io.c and
 * kernel/console.c; port I/O is substituted by the tests/kernel_io_panic_port.h
 * seam, and cap.c's frame/page-table hooks are the same host-memory stubs
 * tests/console_forward_skip_host_test.c uses.
 *
 * ─── The property this exists to protect ───────────────────────────────────
 * E6 gives each tenant POSIX environment its OWN kernel-brokered console
 * channel pair, so one environment's output can never be read by another and
 * one environment's input can never reach another. Before E6 every sidecar's
 * console was the single channel to "kernel.debug.console" and the kernel
 * drained them all to the same serial port — "an environment you cannot reach
 * is not an environment" (G7).
 *
 * The identity rule is the load-bearing part and is the reason this is a host
 * test rather than only a boot guard: a console is attributed to the
 * environment named by its OWN sidecar name (`aerosls.posix.<index>`) in the
 * partition it was created in, and a name that is not exactly that shape must
 * be REFUSED — a console wired to the wrong environment is worse than one that
 * is not wired at all.
 *
 * ─── What this test asserts ────────────────────────────────────────────────
 *   1. name attribution: `aerosls.posix.<n>` registers; every other shape
 *      (the old kernel name, a bare prefix, a non-numeric or trailing-junk
 *      index, another sidecar's name) is refused and leaves no slot claimed;
 *   2. `env_console_kernel_slot` claims exactly the registered kernel ends, so
 *      console_service_tick() skips them (the property that keeps a tenant's
 *      output off the kernel's serial transcript);
 *   3. ISOLATION OUT: output pushed by environment A's sidecar is readable by
 *      A and NOT by B, on the same partition and across partitions;
 *   4. ISOLATION IN: input written for A is queued on A's channel only, with
 *      the newline the POSIX sh applet needs, and B's channel stays empty;
 *   5. unknown/ended environments: read answers 0 / write answers -1;
 *   6. teardown: once A's sidecar closes its end, a tick retires the console
 *      and A is no longer attachable (no console for a dead environment);
 *   7. overflow: more than ENV_CONSOLE_BUF bytes keeps the NEWEST output and
 *      reports what fell off the front via env_console_dropped();
 *   8. the parse itself, called directly: `env_console_name_index` is E6's ONE
 *      answer to "which environment is this name" — cap.c calls it to write
 *      the sidecar's own index into its BootInfoBlock (BIB v3), and the
 *      sidecar announces that index on its own console — so the index in the
 *      announcement and the index a console is filed under must come from it.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -include tests/kernel_io_panic_port.h \
 *       -o /tmp/env_console_host_test \
 *       tests/env_console_host_test.c kernel/cap.c \
 *       kernel/env_console.c kernel/kernel_io.c kernel/console.c
 *   /tmp/env_console_host_test
 */
#include "kernel/cap.h"
#include "kernel/env_console.h"
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
 * link. Defined HERE rather than in tests/process_host_stubs.h because
 * process.c DEFINES the function (same reasoning as
 * console_forward_skip_host_test.c). */
void kernel_yield_to_ring3(uint32_t budget_ticks) { (void)budget_ticks; }

/* ─── cap.c link stubs (same set as console_forward_skip_host_test.c) ────── */
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

/* The UART model: output is swallowed (nothing here asserts on serial traffic
 * — E6's point is that tenant output never reaches it). */
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
/* One environment's console: the kernel's two ends and the sidecar's two. */
struct Pair {
    uint16_t krd, kw, frd, fwr;
    uint32_t far_pid;
};

static int mint_pair(struct Pair* p, uint32_t far_pid) {
    p->far_pid = far_pid;
    return cap_chan_create(0, far_pid, &p->krd, &p->kw, &p->frd, &p->fwr);
}

/* The far end (the sidecar) sends a console line. */
static int far_send(const struct Pair* p, const char* s) {
    return cap_send_msg(p->far_pid, p->fwr, s, (uint32_t)strlen(s), 0, 0, 1, 0);
}

/* The far end drains one message the way the sidecar pump does. */
static int far_recv(const struct Pair* p, char* out, size_t cap) {
    uint32_t plen = 0, tag = 0, flags = 0;
    uint16_t nc = 0;
    int r = cap_recv_msg(p->far_pid, p->frd, (uint8_t*)out, (uint32_t)cap, &plen,
                         0, 0, &nc, &tag, &flags);
    if (r != 0) return -1;
    out[plen < cap ? plen : cap - 1] = '\0';
    return (int)plen;
}

int main(void) {
    cap_init();

    /* ── 1. identity: only `aerosls.posix.<index>` is attributed ─────────── */
    struct Pair a, b, junk;
    if (mint_pair(&a, 100) != 0 || mint_pair(&b, 101) != 0 ||
        mint_pair(&junk, 102) != 0) {
        printf("FAIL: cap_chan_create failed\n");
        return 1;
    }

    CHECK(env_console_register(junk.krd, junk.kw, 1, 102,
                               "kernel.debug.console") == 0,
          "the old shared kernel console name is refused");
    CHECK(env_console_register(junk.krd, junk.kw, 1, 102, "aerosls.posix.") == 0,
          "the bare prefix (no index) is refused");
    CHECK(env_console_register(junk.krd, junk.kw, 1, 102, "aerosls.posix.x") == 0,
          "a non-numeric index is refused");
    CHECK(env_console_register(junk.krd, junk.kw, 1, 102, "aerosls.posix.0x") == 0,
          "an index with trailing junk is refused");
    CHECK(env_console_register(junk.krd, junk.kw, 1, 102, "drv.ramdisk.0") == 0,
          "another sidecar's name is refused");
    CHECK(env_console_kernel_slot(junk.krd) == 0,
          "a refused console claims no kernel slot");
    CHECK(env_console_count() == 0, "no environment is registered yet");

    /* The parse the refusals above all went through, called directly: it is
     * also how cap.c learns the index to put in the sidecar's own BIB (E6's
     * in-band identity — the sidecar announces this index on its own
     * console), so an index accepted here and an index filed by
     * env_console_register are necessarily the same number. */
    {
        uint32_t idx = 99;
        CHECK(env_console_name_index("aerosls.posix.0", &idx) == 1 && idx == 0,
              "the parse reads index 0 (a real environment — the E3 boot spawn)");
        CHECK(env_console_name_index("aerosls.posix.1234", &idx) == 1 && idx == 1234,
              "...and a multi-digit one");
        idx = 42;
        CHECK(env_console_name_index("aerosls.posix.0x", &idx) == 0 && idx == 42,
              "a refusal does not write a bogus index out");
        CHECK(env_console_name_index(NULL, &idx) == 0, "a null name is refused");
        CHECK(env_console_name_index("aerosls.posix.16777216", &idx) == 0,
              "an absurd index (past the 24-bit ceiling) is refused");
    }

    CHECK(env_console_register(a.krd, a.kw, 1, 100, "aerosls.posix.0") == 1,
          "aerosls.posix.0 registers in partition 1");
    CHECK(env_console_register(b.krd, b.kw, 1, 101, "aerosls.posix.1") == 1,
          "aerosls.posix.1 registers in the SAME partition");
    CHECK(env_console_count() == 2, "two environments now have consoles");

    /* ── 2a. env_id binding. The control plane's address is (partition,
     *        env_id) — the pair create and destroy speak — so a console the
     *        manager has not bound is not addressable by id, and id 0 is never
     *        a valid address (it means "none"). */
    {
        char out[16]; uint32_t n = 7;
        CHECK(env_console_read(1, 10, (uint8_t*)out, sizeof out, &n) == 0,
              "an unbound id is not addressable");
    }
    CHECK(env_console_bind_env(1, 0, 10) == 1 &&
          env_console_bind_env(1, 1, 11) == 1,
          "the manager's env_ids bind to the consoles of (1,0) and (1,1)");
    CHECK(env_console_bind_env(1, 42, 12) == 0,
          "binding an index that has no console is refused");
    CHECK(env_console_bind_env(1, 0, 0) == 0,
          "binding id 0 is refused — it is not an address");
    {
        uint32_t p = 0, id = 0, idx = 0, pid = 0;
        CHECK(env_console_entry(0, &p, &id, &idx, &pid) == 1 &&
              p == 1 && id == 10 && idx == 0 && pid == 100,
              "the registry exposes (partition, env_id, index, pid) for listing");
    }

    /* ── 2. console_service must skip exactly these kernel ends ──────────── */
    CHECK(env_console_kernel_slot(a.krd) == 1 && env_console_kernel_slot(b.krd) == 1,
          "both registered kernel CHAN_Rs are claimed (skipped by the console service)");
    CHECK(env_console_kernel_slot(a.kw) == 0,
          "the kernel CHAN_W is not a drain slot and is not claimed as one");
    CHECK(env_console_kernel_slot(junk.kw) == 0,
          "a minted but unregistered slot is not claimed");
    CHECK(env_console_kernel_slot((uint16_t)(CAP_TABLE_ENTRIES - 1)) == 0,
          "the last cap slot is not claimed");

    /* ── 3. ISOLATION OUT: A's output is A's, and B never sees it ────────── */
    far_send(&a, "hi-from-A\n");
    env_console_tick();
    {
        char out[256]; uint32_t n = 0;
        CHECK(env_console_read(1, 10, (uint8_t*)out, sizeof out, &n) == 1 &&
              n == 10 && memcmp(out, "hi-from-A\n", 10) == 0,
              "environment (1, env_id 10) reads exactly its own sidecar's output");
        uint32_t bn = 0;
        CHECK(env_console_read(1, 11, (uint8_t*)out, sizeof out, &bn) == 1 && bn == 0,
              "its neighbour reads NOTHING — same partition, different console");
    }

    /* Cross-partition too: a third environment in another partition. */
    struct Pair c;
    if (mint_pair(&c, 103) != 0 ||
        env_console_register(c.krd, c.kw, 7, 103, "aerosls.posix.9") != 1 ||
        env_console_bind_env(7, 9, 12) != 1) {
        printf("FAIL: could not register the cross-partition console\n");
        return 1;
    }
    far_send(&c, "hi-from-C\n");
    env_console_tick();
    {
        char out[256]; uint32_t n = 0;
        CHECK(env_console_read(7, 12, (uint8_t*)out, sizeof out, &n) == 1 &&
              n == 10 && memcmp(out, "hi-from-C\n", 10) == 0,
              "a console in another partition is read by its own (partition, env_id)");
        uint32_t an = 0;
        CHECK(env_console_read(1, 10, (uint8_t*)out, sizeof out, &an) == 1 && an == 0,
              "reading A again after C spoke yields only what A produced");
    }

    /* A destructive read: the same bytes are not returned twice. */
    {
        char out[256]; uint32_t n = 0;
        CHECK(env_console_read(7, 12, (uint8_t*)out, sizeof out, &n) == 1 && n == 0,
              "output is drained, not re-read");
    }

    /* ── 4. ISOLATION IN: input for A lands on A's channel only ──────────── */
    CHECK(env_console_write(1, 10, (const uint8_t*)"echo hi", 7) == 8,
          "write queues the line plus the newline the sh applet needs");
    {
        char got[64] = {0};
        int n = far_recv(&a, got, sizeof got);
        CHECK(n == 8 && memcmp(got, "echo hi\n", 8) == 0,
              "A's sidecar receives exactly the input written for A");
    }
    {
        char got[64] = {0};
        CHECK(far_recv(&b, got, sizeof got) == -1,
              "B's sidecar receives NOTHING from input written for A");
        CHECK(far_recv(&c, got, sizeof got) == -1,
              "the cross-partition sidecar receives NOTHING either");
    }
    CHECK(env_console_write(1, 10, (const uint8_t*)"x\n", 2) == 2,
          "an already-terminated line is not double-terminated");
    {
        char got[64] = {0};
        (void)far_recv(&a, got, sizeof got);
        CHECK(strcmp(got, "x\n") == 0, "the terminator is added exactly once");
    }

    /* ── 5. unknown environments answer honestly ─────────────────────────── */
    {
        char out[16]; uint32_t n = 123;
        CHECK(env_console_read(1, 99, (uint8_t*)out, sizeof out, &n) == 0 && n == 0,
              "reading an environment with no console answers 0");
        CHECK(env_console_write(1, 99, (const uint8_t*)"z\n", 2) == -1,
              "writing an environment with no console answers -1");
    }

    /* ── 6. teardown: a closed sidecar end retires the console ───────────── */
    cap_revoke(a.far_pid, a.frd);
    cap_revoke(a.far_pid, a.fwr);
    env_console_tick();
    CHECK(env_console_count() == 2,
          "environment (1,0) is retired once its sidecar closes its end");
    {
        char out[16]; uint32_t n = 0;
        CHECK(env_console_read(1, 10, (uint8_t*)out, sizeof out, &n) == 0,
              "a retired environment is no longer attachable");
    }
    {
        char out[16]; uint32_t n = 0;
        CHECK(env_console_read(1, 11, (uint8_t*)out, sizeof out, &n) == 1,
              "its neighbour is untouched by the retirement");
    }
    CHECK(env_console_kernel_slot(a.krd) == 0,
          "the retired kernel end is no longer claimed");

    /* Re-creating the same index (E5 recycle) replaces, never leaks, the
     * entry — the registry must not fill with dead environments' consoles. */
    struct Pair a2;
    if (mint_pair(&a2, 104) != 0) { printf("FAIL: re-mint\n"); return 1; }
    CHECK(env_console_register(a2.krd, a2.kw, 1, 104, "aerosls.posix.0") == 1,
          "re-created environment (1,0) registers again");
    CHECK(env_console_count() == 3, "re-registering an index adds no stale entry");
    CHECK(env_console_bind_env(1, 0, 20) == 1,
          "the re-created environment binds its NEW id");
    {
        char out[16]; uint32_t n = 0;
        CHECK(env_console_read(1, 10, (uint8_t*)out, sizeof out, &n) == 0,
              "the re-created environment's OLD id is no longer addressable");
    }

    /* ── 7. overflow keeps the newest output and reports the drop ────────── */
    {
        /* A message is bounded by the channel's payload size, so an over-long
         * stream arrives as several messages: 6 x 1000 bytes of a constant
         * byte ('0'..'5'), i.e. 6000 bytes against a 4096-byte buffer. */
        uint8_t chunk[1000];
        int sent = 1;
        for (int c = 0; c < 6; c++) {
            memset(chunk, (int)('0' + c), sizeof chunk);
            if (cap_send_msg(a2.far_pid, a2.fwr, chunk, (uint32_t)sizeof chunk,
                             0, 0, (uint32_t)(2 + c), 0) != 0)
                sent = 0;
        }
        CHECK(sent, "a sidecar may send an over-long console stream");
        env_console_tick();
        uint8_t got[ENV_CONSOLE_BUF];
        uint32_t n = 0;
        CHECK(env_console_read(1, 20, got, sizeof got, &n) == 1 &&
              n == ENV_CONSOLE_BUF,
              "the attach buffer is bounded at ENV_CONSOLE_BUF");
        CHECK(env_console_dropped(1, 20) == 1904,
              "exactly the overflow (6000-4096) is reported as dropped");
        CHECK(got[0] == '1',
              "the OLDEST bytes fell off; the buffer keeps the newest");
        CHECK(got[ENV_CONSOLE_BUF - 1] == '5',
              "the newest bytes are the ones retained");
    }

    printf("\n=== %d passed, %d failed ===\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
