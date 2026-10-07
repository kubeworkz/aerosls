/*
 * env_payload_host_test.c — POSIX-Environments v0.2 Phase P1a: the payload
 * half of the environment checkpoint, against the REAL kernel/env_payload.c.
 *
 * ─── What this exists to prove ────────────────────────────────────────────
 * The payload layer's whole contract is an ordering: capture means only
 * what a frozen instant meant, and restore refuses BEFORE it writes. So
 * every clause here is either a round trip (the bytes that came back are
 * the bytes that were there) or a named refusal (nothing was written and
 * the code says why). The refusal matrix is the point, not a formality:
 * each entry is one way an environment could come back EMPTY or CORRUPT,
 * and the layer's promise is that each one is refused by name instead.
 *
 * The page tables are synthetic: real PML4/PDPT/PD/PT frames built in this
 * test's own statics, walked by env_payload.c's real ep_resolve() over the
 * identity map — the same pointer arithmetic cap_create_sidecar() performs.
 * The "identity region" trick that makes the placement rule testable on a
 * host: the region's base IS the address of a static buffer, so vaddr ==
 * phys exactly as it is in the kernel, and a pour into it is a memcpy into
 * a buffer whose address the test chose (the captured address).
 *
 * The NVMe device is a sparse RAM disk keyed by LBA; a read of a frame
 * nobody wrote returns zeros, which is fresh-media semantics — and is what
 * makes the ABSENT clause (no directory) real rather than stubbed.
 *
 * Build and run:
 *   gcc -Wall -Wextra -std=c11 -I . -I kernel -I arch/x86 \
 *       -o /tmp/env_payload_host_test \
 *       tests/env_payload_host_test.c kernel/env_payload.c kernel/env_ckpt.c
 *   /tmp/env_payload_host_test
 */
#include "tests/partition_host_stubs.h"  /* env_ckpt.c's partition_pause/_resume/_is_paused/_exists */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kernel/env_payload.h"
#include "kernel/env_ckpt.h"
#include "kernel/env_console.h"
#include "kernel/process.h"
#include "kernel/cap.h"
#include "kernel/env_storage.h"   /* env_storage_extent_of link stub, as env_ckpt_host_test's */

static int checks_passed = 0;
static int checks_failed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); checks_failed++; } \
    else         { printf("ok:   %s\n", msg); checks_passed++; } \
} while (0)

/* ─── Link stubs: the record layer's two out-of-scope dependencies ────────── */
static uint32_t g_dirty_mask = 0;
void ckpt_mark_dirty(uint32_t region) {
    if (region < 32) g_dirty_mask |= (1u << region);
}
int env_storage_extent_of(uint32_t partition_id, uint32_t index,
                          uint64_t* out_lba, uint64_t* out_sectors,
                          uint32_t* out_bytes) {
    (void)partition_id; (void)index; (void)out_lba; (void)out_sectors;
    (void)out_bytes;
    return 1;   /* nothing durable to name here — the fields stay 0 */
}

/* ─── Link stubs: kernel_io / timer / process ─────────────────────────────── */
void kernel_serial_printf(const char* fmt, ...) { (void)fmt; }
volatile uint64_t kernel_tick_counter = 0;
void kernel_yield_to_ring3(uint32_t budget_ticks) {
    kernel_tick_counter += budget_ticks;   /* stands in for the timer ISR */
}
struct ProcessDescriptor proc_table[PROC_MAX];

/* ─── The device: a sparse RAM disk keyed by LBA ───────────────────────────── */
void* io_sq = (void*)1;
void* io_cq = (void*)1;
#define DISK_MAX_FRAMES 64
static struct { uint64_t lba; int used; uint8_t data[4096]; } g_disk[DISK_MAX_FRAMES];
static void disk_reset(void) { memset(g_disk, 0, sizeof g_disk); }
int nvme_read_sync(uint64_t slba, void* buf) {
    for (int i = 0; i < DISK_MAX_FRAMES; i++)
        if (g_disk[i].used && g_disk[i].lba == slba) {
            memcpy(buf, g_disk[i].data, 4096);
            return 0;
        }
    memset(buf, 0, 4096);     /* fresh media: nobody wrote this frame */
    return 0;
}
int nvme_write_sync(uint64_t slba, const void* buf) {
    int free_i = -1;
    for (int i = 0; i < DISK_MAX_FRAMES; i++) {
        if (g_disk[i].used && g_disk[i].lba == slba) {
            memcpy(g_disk[i].data, buf, 4096);
            return 0;
        }
        if (!g_disk[i].used && free_i < 0) free_i = i;
    }
    if (free_i < 0) return 1;   /* the test's disk is big enough for its blobs */
    g_disk[free_i].used = 1;
    g_disk[free_i].lba = slba;
    memcpy(g_disk[free_i].data, buf, 4096);
    return 0;
}
static void disk_corrupt(uint64_t lba) {
    for (int i = 0; i < DISK_MAX_FRAMES; i++)
        if (g_disk[i].used && g_disk[i].lba == lba) {
            g_disk[i].data[100] ^= 0xFFu;   /* a torn page, one byte honest */
            return;
        }
}

/* ─── The console: recording stubs ────────────────────────────────────────── */
static uint8_t  g_snap[64];
static uint32_t g_snap_len = 0;
static int      g_snap_ret = 1;
static int      g_inject_ret = 1;
static uint32_t g_inject_env = 0;
static uint32_t g_inject_len = 0xFFFFFFFFu;
static uint8_t  g_inject_bytes[64];
int env_console_snapshot(uint32_t partition, uint32_t env_id,
                         uint8_t* out, uint32_t cap, uint32_t* out_len) {
    (void)partition; (void)env_id;
    if (out_len) *out_len = 0;
    if (g_snap_ret != 1) return g_snap_ret;
    uint32_t n = g_snap_len < cap ? g_snap_len : cap;
    memcpy(out, g_snap, n);
    if (out_len) *out_len = n;
    return 1;
}
int env_console_inject(uint32_t partition, uint32_t env_id,
                       const uint8_t* bytes, uint32_t len) {
    (void)partition;
    if (g_inject_ret != 1) return g_inject_ret;
    g_inject_env = env_id;
    g_inject_len = len;
    memcpy(g_inject_bytes, bytes, len < sizeof g_inject_bytes ? len : sizeof g_inject_bytes);
    return 1;
}
/* The boot-announcement flag the bounded wait reads (env_console_tick sets it
 * in the kernel; here it is a switch the wait cases flip). */
static int g_identity_seen = 1;
int env_console_identity_seen(uint32_t partition, uint32_t index) {
    (void)partition; (void)index;
    return g_identity_seen;
}

/* ─── Synthetic address space ─────────────────────────────────────────────── */
#define IMG_VADDR 0x400000000000ULL     /* cap.c's USER_PROC_CODE_BASE literal */
static uint64_t g_pml4[512] __attribute__((aligned(4096)));
static uint64_t g_pool_pdpt[4][512] __attribute__((aligned(4096)));
static uint64_t g_pool_pd[8][512]   __attribute__((aligned(4096)));
static uint64_t g_pool_pt[8][512]   __attribute__((aligned(4096)));
static int g_n_pdpt, g_n_pd, g_n_pt;
static uint8_t g_img_frame[4096] __attribute__((aligned(4096)));
static uint64_t g_region_buf[512] __attribute__((aligned(4096)));  /* the identity region */
/* The other two required region kinds: real statics the record names but the
 * synthetic page tables never map, so they contribute no captured pages —
 * exactly the shape of a lazily-granted frame the payload rule 3 skips. */
static uint64_t g_rdheap_buf[512] __attribute__((aligned(4096)));
static uint64_t g_rdstor_buf[512] __attribute__((aligned(4096)));

static void map_reset(void) {
    memset(g_pml4, 0, sizeof g_pml4);
    g_n_pdpt = g_n_pd = g_n_pt = 0;
}
static void map_page(uint64_t v, uint64_t phys) {
    uint64_t i4 = (v >> 39) & 0x1ffu, i3 = (v >> 30) & 0x1ffu;
    uint64_t i2 = (v >> 21) & 0x1ffu, i1 = (v >> 12) & 0x1ffu;
    if (!(g_pml4[i4] & 1ull)) {
        memset(g_pool_pdpt[g_n_pdpt], 0, 4096);
        g_pml4[i4] = (uint64_t)(uintptr_t)g_pool_pdpt[g_n_pdpt++] | 0x7ull;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(g_pml4[i4] & 0x000ffffffffff000ull);
    if (!(pdpt[i3] & 1ull)) {
        memset(g_pool_pd[g_n_pd], 0, 4096);
        pdpt[i3] = (uint64_t)(uintptr_t)g_pool_pd[g_n_pd++] | 0x7ull;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    if (!(pd[i2] & 1ull)) {
        memset(g_pool_pt[g_n_pt], 0, 4096);
        pd[i2] = (uint64_t)(uintptr_t)g_pool_pt[g_n_pt++] | 0x7ull;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    pt[i1] = phys | 0x7ull;     /* PRESENT | WRITE | USER — the map's own flags */
}
static void unmap_image_page(void) {
    uint64_t i4 = (IMG_VADDR >> 39) & 0x1ffu, i3 = (IMG_VADDR >> 30) & 0x1ffu;
    uint64_t i2 = (IMG_VADDR >> 21) & 0x1ffu, i1 = (IMG_VADDR >> 12) & 0x1ffu;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(g_pml4[i4] & 0x000ffffffffff000ull);
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    pt[i1] = 0;
}

/* ─── The world: record + sidecar + captured bytes ────────────────────────── */
#define T_PART 1u
#define T_IDX  2u
#define T_SEQ  42ull
#define T_CONSOLE_ENV 7u      /* the env_id the console was bound to at capture */
#define T_NEW_ENV      9u     /* the env_id the replay produced */

static void fill_pattern(uint8_t* p, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed ^ (uint8_t)i);
}

static struct EnvCkptRecord g_scratch;
static void make_record(void) {
    memset(&g_scratch, 0, sizeof g_scratch);
    g_scratch.magic        = ENV_CKPT_REC_MAGIC;
    g_scratch.version      = ENV_CKPT_REC_VERSION;
    g_scratch.size         = (uint32_t)sizeof(struct EnvCkptRecord);
    g_scratch.sequence     = T_SEQ;
    g_scratch.partition_id = T_PART;
    g_scratch.index        = T_IDX;
    g_scratch.env_id       = 5;
    g_scratch.console_id   = T_CONSOLE_ENV;
    g_scratch.flags        = ENV_CKPT_FLAG_QUIESCED;
    g_scratch.n_tasks      = 1;
    g_scratch.n_regions    = ENV_CKPT_MAX_REGIONS;   /* all three, each kind once */
    g_scratch.n_chans      = 0;
    g_scratch.regions[0].base   = (uint64_t)(uintptr_t)g_region_buf;
    g_scratch.regions[0].frames = 1;
    g_scratch.regions[0].kind   = ENV_CKPT_REGION_POSIX_HEAP;
    g_scratch.regions[1].base   = (uint64_t)(uintptr_t)g_rdheap_buf;
    g_scratch.regions[1].frames = 1;
    g_scratch.regions[1].kind   = ENV_CKPT_REGION_RD_HEAP;
    g_scratch.regions[2].base   = (uint64_t)(uintptr_t)g_rdstor_buf;
    g_scratch.regions[2].frames = 1;
    g_scratch.regions[2].kind   = ENV_CKPT_REGION_RD_STORAGE;
    strcpy(g_scratch.tasks[0].name, "aerosls.posix.2");
    g_scratch.tasks[0].kind  = ENV_CKPT_TASK_POSIX_SIDECAR;
    g_scratch.tasks[0].entry = IMG_VADDR;
}

/* Bring the world up and CAPTURE it. `captured_parked` selects the form the
 * capture sees: parked on the console recv (the restorable posture) or
 * mid-compute (the named-refusal posture). Returns capture_all()'s count. */
static uint32_t setup_and_capture(int captured_parked) {
    disk_reset();
    map_reset();
    env_ckpt_reset();
    memset(proc_table, 0, sizeof proc_table);
    memset(&g_disk, 0, sizeof g_disk);
    g_inject_ret = 1; g_inject_env = 0; g_inject_len = 0xFFFFFFFFu;
    g_snap_ret = 1;
    g_snap_len = 4;
    memcpy(g_snap, "OUT!", 4);
    g_dirty_mask = 0;

    fill_pattern(g_img_frame, 4096, 0xA0);
    fill_pattern((uint8_t*)g_region_buf, 4096, 0x5A);
    map_page(IMG_VADDR, (uint64_t)(uintptr_t)g_img_frame);
    map_page((uint64_t)(uintptr_t)g_region_buf,
             (uint64_t)(uintptr_t)g_region_buf);      /* identity, by construction */

    make_record();
    if (env_ckpt_record(&g_scratch) != 0) return 0;

    struct ProcessDescriptor* pd = &proc_table[0];
    pd->active = 1;
    strcpy(pd->name, "aerosls.posix.2");
    pd->cr3 = (uint64_t)(uintptr_t)g_pml4;
    pd->waiting_chan  = captured_parked ? 3u : CAP_NONE;
    pd->waiting_nchans = 0;
    pd->park_syscall  = SYS_SLS_CAP_RECV;
    pd->park_ctx.r11 = 10; pd->park_ctx.rcx = 11; pd->park_ctx.r15 = 12;
    pd->park_ctx.r14 = 13; pd->park_ctx.r13 = 14; pd->park_ctx.r12 = 15;
    pd->park_ctx.rbx = 16; pd->park_ctx.rbp = 17; pd->park_ctx.user_rsp = 18;
    pd->user_rip = IMG_VADDR + 0x1000;
    pd->user_rsp = IMG_VADDR + 0x2000;
    return env_payload_capture_all();
}

/* The reboot's fresh boot: empty frames, a different park, a different
 * channel id — everything a replay legitimately re-decides. */
static void simulate_fresh_boot(void) {
    memset(g_img_frame, 0, 4096);
    memset(g_region_buf, 0, 4096);
    memset(&proc_table[0].park_ctx, 0, sizeof proc_table[0].park_ctx);
    proc_table[0].user_rip = IMG_VADDR + 0x10;   /* entry, not the park point */
    proc_table[0].user_rsp = IMG_VADDR + 0x3000;
    proc_table[0].waiting_chan = 9;               /* a boot-local channel id */
    proc_table[0].park_syscall = SYS_SLS_CAP_RECV;
}

int main(void) {
    printf("=== env_payload: P1a payload capture and restore ===\n\n");

    /* ── 1. The round trip ────────────────────────────────────────────────── */
    CHECK(setup_and_capture(1) == 1,
          "capture writes exactly one payload for the one live record");
    CHECK(env_payload_present(T_PART, T_IDX) == 1,
          "the directory names the payload for (partition, index)");
    CHECK(env_payload_present(T_PART, T_IDX + 1) == 0,
          "a different identity has no payload (the directory is keyed, not sticky)");

    simulate_fresh_boot();
    uint32_t pages = 0, cons = 0;
    int rc = env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_NONE,
          "the restore pours into the replayed environment");
    CHECK(memcmp(g_img_frame,
                 (uint8_t[]){0xA0 ^ 0, 0xA0 ^ 1, 0xA0 ^ 2, 0xA0 ^ 3}, 4) == 0,
          "the image frame's bytes are the captured ones, not the fresh boot's zeros");
    CHECK(memcmp(g_region_buf,
                 (uint8_t[]){0x5A ^ 0, 0x5A ^ 1, 0x5A ^ 2, 0x5A ^ 3}, 4) == 0,
          "the identity region's bytes are back at the CAPTURED address (the placement rule)");
    CHECK(proc_table[0].park_ctx.r11 == 10 && proc_table[0].park_ctx.rcx == 11 &&
          proc_table[0].park_ctx.user_rsp == 18,
          "the park save area is the captured one (the register rule)");
    CHECK(proc_table[0].waiting_chan == 9,
          "the kernel-side park bookkeeping stays THIS boot's (the captured channel id is not resurrected)");
    CHECK(proc_table[0].user_rip == IMG_VADDR + 0x1000,
          "user_rip is the captured park point, not the fresh entry");
    CHECK(pages == 2 && cons == 4,
          "the pour reports the two pages (image + region) and the four console bytes");
    CHECK(g_inject_env == T_NEW_ENV && g_inject_len == 4 &&
          memcmp(g_inject_bytes, "OUT!", 4) == 0,
          "the buffered console bytes are injected under the NEW env_id");
    CHECK(env_payload_last_refusal().code == EP_REFUSE_NONE,
          "a successful pour leaves no refusal latched");

    /* ── 2. The refusal matrix: every way to come back empty or corrupt ───── */
    CHECK(env_payload_restore(T_PART, T_IDX + 1, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_ABSENT,
          "ABSENT: an identity no capture ever wrote is refused, not poured");
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ + 1, &pages, &cons)
              == EP_REFUSE_SEQ,
          "SEQ: a payload from a different checkpoint than the record is refused");

    CHECK(setup_and_capture(1) == 1, "re-capture for the placement refusal");
    {
        /* The replay landed the region somewhere else — the interior
         * pointers in the captured bytes would point at nothing. */
        struct EnvCkptRecord moved;
        memcpy(&moved, &g_scratch, sizeof moved);
        moved.regions[0].base = (uint64_t)(uintptr_t)g_region_buf + 0x10000ull;
        CHECK(env_ckpt_record(&moved) == 0, "the record now names a moved region");
        simulate_fresh_boot();
        CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
                  == EP_REFUSE_PLACEMENT,
              "PLACEMENT: a region away from its captured base refuses the whole pour before a byte moves");
        CHECK(g_region_buf[0] == 0 && g_img_frame[0] == 0,
              "and nothing was written: the fresh boot's zeros are still zeros");
    }

    CHECK(setup_and_capture(1) == 1, "re-capture for the no-process refusal");
    simulate_fresh_boot();
    proc_table[0].active = 0;
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_NO_PROCESS,
          "NO_PROCESS: a captured sidecar with no live process is a named refusal");

    CHECK(setup_and_capture(0) == 1,
          "capture of a mid-compute sidecar still records the payload");
    proc_table[0].waiting_chan = 4;
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_RUNNING,
          "RUNNING: a capture that caught a sidecar mid-compute is replayed empty, by name");

    CHECK(setup_and_capture(1) == 1, "re-capture for the not-parked refusals");
    simulate_fresh_boot();
    proc_table[0].waiting_chan = CAP_NONE;   /* the live sidecar is runnable */
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_NOT_PARKED,
          "NOT_PARKED: a live sidecar that never reached its idle recv refuses the pour");

    CHECK(setup_and_capture(1) == 1, "re-capture for the park-syscall mismatch");
    simulate_fresh_boot();
    proc_table[0].park_syscall = SYS_SLS_CHAN_WAIT;   /* parked, but in ANOTHER recv */
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_NOT_PARKED,
          "NOT_PARKED: parked in a different syscall than the capture recorded is still a refusal");

    CHECK(setup_and_capture(1) == 1, "re-capture for the no-mapping refusal");
    simulate_fresh_boot();
    unmap_image_page();
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_NO_MAPPING,
          "NO_MAPPING: a captured vaddr with no live frame refuses before the pour");

    CHECK(setup_and_capture(1) == 1, "re-capture for the console refusal");
    simulate_fresh_boot();
    g_inject_ret = 0;
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_CONSOLE,
          "CONSOLE: buffered bytes with nowhere to go are a refusal, not a silent drop");
    g_inject_ret = 1;

    CHECK(setup_and_capture(1) == 1, "re-capture for the index-crc refusal");
    simulate_fresh_boot();
    disk_corrupt(ENV_PAYLOAD_SLOT_LBA(0) + 8ull);   /* frame 1 = the index zone */
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_FORMAT,
          "FORMAT: a torn index zone fails its crc before anything in it is believed");

    CHECK(setup_and_capture(1) == 1, "re-capture for the data-crc refusal");
    simulate_fresh_boot();
    disk_corrupt(ENV_PAYLOAD_SLOT_LBA(0) + 16ull);  /* data zone starts at frame 2 */
    CHECK(env_payload_restore(T_PART, T_IDX, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_FORMAT,
          "FORMAT: a torn page fails its own crc in the media pass, before the pour");
    CHECK(g_img_frame[0] == 0,
          "the media pass reading garbage wrote nothing (the fresh boot's zeros stand)");

    /* ── 3. The bounded wait the restore pass stands on ───────────────────── */
    CHECK(setup_and_capture(1) == 1, "re-capture for the wait");
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 1,
          "wait: an already-parked sidecar answers immediately");
    g_identity_seen = 0;
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 0,
          "wait: parked but not yet announced is not the pour's go signal — "
          "the bound answers instead of letting the pour erase the [env-id] "
          "line nobody else can write");
    g_identity_seen = 1;
    proc_table[0].waiting_chan = CAP_NONE;
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 0,
          "wait: a sidecar that never parks times out on the tick bound instead of spinning forever");
    proc_table[0].active = 0;
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 0,
          "wait: a missing sidecar is answered at once (there is nothing to wait for)");

    /* ── 4. The refusal rendering and the latch ───────────────────────────── */
    CHECK(setup_and_capture(1) == 1, "re-capture for the refusal rendering");
    CHECK(env_payload_restore(T_PART, T_IDX + 1, T_NEW_ENV, T_SEQ, &pages, &cons)
              == EP_REFUSE_ABSENT, "a refusal to render exists");
    {
        char why[96];
        struct EnvPayloadRefusal r = env_payload_last_refusal();
        env_payload_refusal_text(&r, why, sizeof why);
        CHECK(why[0] != '\0',
              "the refusal renders to text an operator can read in the serial log");
        env_payload_clear_refusal();
        CHECK(env_payload_last_refusal().code == EP_REFUSE_NONE,
              "clear_refusal() latches back to NONE");
    }

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
