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
 *       tests/env_payload_host_test.c kernel/env_payload.c kernel/env_ckpt.c \
 *           kernel/linux_thread.c kernel/linux_abi.c
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
#include "kernel/linux_thread.h"
#include "kernel/cap.h"
#include "kernel/linux_abi.h"      /* E7 increment 20: SYS_LINUX_TRAP_RESUME, the parked Linux form */
#include "kernel/env_storage.h"   /* env_storage_extent_of link stub, as env_ckpt_host_test's */
#include "kernel/persist.h"     /* the payload directory's fixed LBAs */

/* I1 link stubs: the group's teardown is not exercised by this payload test,
 * but linux_thread.c references both. */
void cap_table_teardown(uint32_t pid) { (void)pid; }
void user_destroy_page_table(uint64_t pml4_phys) { (void)pml4_phys; }

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
/* The boot-announcement flag the bounded wait reads (env_console_tick sets it
 * in the kernel; here it is a switch the wait cases flip). Declared up here
 * because the yield hook below models the boot's last step — announce, then
 * park — and that is exactly the progress the wait now depends on. */
static int g_identity_seen = 1;
/* The progress the wait reads at run time: the real yield hands the CPU to
 * Ring-3, where the environment's tasks are switched in (process.c's
 * proc_build_resume_frame bumps each one's `switches`). This switch models
 * exactly that — while armed, every yielded slice switches proc_table[0] in —
 * and g_park_at_tick says when the boot finishes and the task parks. */
static int g_switch_each_yield = 0;
static uint64_t g_park_at_tick = 0;
void kernel_yield_to_ring3(uint32_t budget_ticks) {
    kernel_tick_counter += budget_ticks;   /* stands in for the timer ISR */
    if (!g_switch_each_yield) return;
    proc_table[0].switches++;              /* proc_build_resume_frame's ledger */
    if (g_park_at_tick && kernel_tick_counter >= g_park_at_tick &&
        proc_table[0].waiting_chan == CAP_NONE) {
        proc_table[0].waiting_chan = 3u;   /* the boot reached its recv park */
        g_identity_seen = 1;               /* ... after announcing, as boot.rs does */
    }
}
/* env_payload.c's wait loop calls this (kernel/smp.h) the way env_service_rpc's
 * wait does: on a uniprocessor boot it is what runs env_console_tick(), the
 * drain that sets the identity flag the wait reads. Counted so section 3 can
 * pin that the wait actually CALLS it — without the call the wait starves its
 * own predicate: the fresh sidecar writes [env-id], nobody drains it, and the
 * wait times out on announced=0 (caught live on the -smp 1 2-node TCG cluster,
 * 2026-10-13, as REFUSE_PARKED). */
static unsigned g_service_pumps = 0;
void smp_uniprocessor_tick(void) { g_service_pumps++; }
/* The parked wait blocks the RESTORE's sweep (env_migrate.c), so it also
 * pumps the heartbeat/checkpoint streams while it waits -- counted for the
 * same reason as g_service_pumps above (the death-during-create fix,
 * 2026-10-23: a silenced sweep deposes the leader and misdeclares it). */
static unsigned g_keepalive_pumps = 0;
void failover_sweep_keepalive(void) { g_keepalive_pumps++; }
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
    linux_groups_init();   /* I1: slot identity is group-less until the create */
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

/* ─── 5. E7 increment 20: the Linux task rides the payload ────────────────
 * The gate environment's record names THREE tasks: the two sidecars and the
 * Linux tenant. This section builds the tenant's world faithfully and proves
 * the three claims the increment's acceptance reads:
 *
 *   - the capture walks the Linux CR3 and files the task's pages (the linked
 *     image at 0x400000, its mapped stack, its budget span) with the
 *     PERMISSIONS the live PTEs grant — RX image, RW-NX stack/heap;
 *   - a round trip restores those bytes into a fresh boot's frames, with the
 *     task's ring3_ctx, fs_base and both cursors;
 *   - a destination whose live permissions differ from the captured ones is
 *     refused by name before a byte moves.
 *
 * A SECOND synthetic address space is used for the Linux task, not a shared
 * one: in the kernel the child CLONES its parent's tables, so the parent's
 * image window is visible from the child — which is exactly why the capture's
 * Linux predicate is its own three ranges rather than the window. Handing the
 * Linux task its own table here keeps the test's page sets disjoint, so a
 * clause that passes cannot be the parent's pages counted twice. */
#define L_PART  2u
#define L_IDX   1u
#define L_SEQ   77ull
#define L_IMG   0x400000ull          /* a non-PIE static link's text vaddr */
#define L_STACK 0x300000000000ull    /* the create's own stack vaddr */
#define L_HEAP  0x300010000000ull    /* the budget span's base */
#define PTE_RX  (0x1ull | 0x4ull)                 /* P | U, no W: executable */
#define PTE_RWNX (0x1ull | 0x2ull | 0x4ull | (1ull << 63))  /* P | W | U, NX */
#define PTE_SOFT (0x1ull | (1ull << 9) | (1ull << 63))      /* P | SOFT_NONE | NX */

static uint64_t g_pml4_l[512] __attribute__((aligned(4096)));
static uint64_t g_l_pdpt[2][512] __attribute__((aligned(4096)));
static uint64_t g_l_pd[4][512]   __attribute__((aligned(4096)));
static uint64_t g_l_pt[4][512]   __attribute__((aligned(4096)));
static int g_l_n_pdpt, g_l_n_pd, g_l_n_pt;
static int g_l_soft_heap;

/* ─── Blob-format helpers for the v2 and corrupt-permission teeth ─────────
 * These touch the REAL on-disk layout: frame 0 is the blob head, frames 1..
 * are the index zone (regions, tasks, per-task page entries, console), and
 * the head's crc32 covers that index zone. A version-only rewrite changes
 * no crc; a permission rewrite must recompute the writer's exact reflected
 * CRC32 over the index bytes, or the test would pass on a torn blob for the
 * wrong reason. */
#define TEST_INDEX_BYTES 65536u
static uint8_t g_blob_head[4096] __attribute__((aligned(4096)));
static uint8_t g_blob_index[TEST_INDEX_BYTES] __attribute__((aligned(4096)));

static uint32_t test_crc32(uint32_t crc, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^
                  (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}
static uint32_t test_read32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t test_read64(const uint8_t* p) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8u * i);
    return v;
}
static void test_write32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int blob_load_head_and_index(void) {
    if (nvme_read_sync(ENV_PAYLOAD_SLOT_LBA(0), g_blob_head) != 0) return 0;
    struct EnvPayloadHead* h = (struct EnvPayloadHead*)g_blob_head;
    if (h->magic != ENV_PAYLOAD_BLOB_MAGIC || h->bytes < 4096u ||
        (h->bytes & 4095u) != 0 || h->n_pages == 0)
        return 0;
    uint32_t frames = h->bytes / 4096u - 1u - h->n_pages;
    if (frames == 0 || (uint64_t)frames * 4096u > sizeof g_blob_index)
        return 0;
    memset(g_blob_index, 0, sizeof g_blob_index);
    for (uint32_t f = 0; f < frames; f++) {
        if (nvme_read_sync(ENV_PAYLOAD_SLOT_LBA(0) + 8ull +
                           (uint64_t)f * 8ull,
                           g_blob_index + (uint64_t)f * 4096u) != 0)
            return 0;
    }
    return 1;
}

static int blob_index_total(const struct EnvPayloadHead* h, uint32_t* out) {
    uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion) +
                 h->n_tasks * (uint32_t)sizeof(struct EnvPayloadTask);
    uint32_t seen = 0;
    for (uint32_t t = 0; t < h->n_tasks; t++) {
        if (u + 4u > sizeof g_blob_index) return 0;
        uint32_t np = test_read32(g_blob_index + u);
        u += 4u;
        if (np > h->n_pages || u + np * EP_PAGE_ENTRY_BYTES >
            sizeof g_blob_index)
            return 0;
        u += np * EP_PAGE_ENTRY_BYTES;
        seen += np;
    }
    if (seen != h->n_pages || u + h->console_len > sizeof g_blob_index)
        return 0;
    *out = u;
    return 1;
}

static int blob_find_page(const struct EnvPayloadHead* h, uint64_t vaddr,
                          uint32_t* entry_off, uint32_t* perm) {
    uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion) +
                 h->n_tasks * (uint32_t)sizeof(struct EnvPayloadTask);
    for (uint32_t t = 0; t < h->n_tasks; t++) {
        uint32_t np = test_read32(g_blob_index + u);
        u += 4u;
        for (uint32_t k = 0; k < np; k++) {
            if (test_read64(g_blob_index + u) == vaddr) {
                *entry_off = u;
                *perm = test_read32(g_blob_index + u + 12u);
                return 1;
            }
            u += EP_PAGE_ENTRY_BYTES;
        }
    }
    return 0;
}

static int blob_task_at(const struct EnvPayloadHead* h, uint32_t t,
                        struct EnvPayloadTask* out) {
    if (t >= h->n_tasks) return 0;
    uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion) +
                 t * (uint32_t)sizeof(struct EnvPayloadTask);
    memcpy(out, g_blob_index + u, sizeof *out);
    return 1;
}

/* Rewrite just the blob head's version; the directory stays current, so the
 * restore reaches the head check and refuses the mismatched task stride. */
static int blob_set_head_version(uint32_t version) {
    if (!blob_load_head_and_index()) return 0;
    struct EnvPayloadHead* h = (struct EnvPayloadHead*)g_blob_head;
    h->version = version;
    return nvme_write_sync(ENV_PAYLOAD_SLOT_LBA(0), g_blob_head) == 0;
}

/* TG-2b: replace the second page entry of task 0 with a copy of the first
 * but a different CRC. The block count and total page count are unchanged
 * (so the layout walk still succeeds); the restore must name the duplicate
 * before any byte moves. */
static int blob_duplicate_page_bad_crc(void) {
    if (!blob_load_head_and_index()) return 0;
    struct EnvPayloadHead* h = (struct EnvPayloadHead*)g_blob_head;
    uint32_t total = 0;
    if (!blob_index_total(h, &total) || h->n_tasks == 0) return 0;
    uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion) +
                 h->n_tasks * (uint32_t)sizeof(struct EnvPayloadTask);
    uint32_t np = test_read32(g_blob_index + u);
    if (np < 2) return 0;
    uint32_t e0 = u + 4u;
    uint32_t e1 = e0 + EP_PAGE_ENTRY_BYTES;
    memcpy(g_blob_index + e1, g_blob_index + e0, EP_PAGE_ENTRY_BYTES);
    uint32_t crc = test_read32(g_blob_index + e1 + 8u);
    test_write32(g_blob_index + e1 + 8u, crc ^ 1u);
    h->crc32 = test_crc32(0, g_blob_index, total + h->console_len);
    uint32_t frames = h->bytes / 4096u - 1u - h->n_pages;
    for (uint32_t f = 0; f < frames; f++) {
        if (nvme_write_sync(ENV_PAYLOAD_SLOT_LBA(0) + 8ull +
                            (uint64_t)f * 8ull,
                            g_blob_index + (uint64_t)f * 4096u) != 0)
            return 0;
    }
    return nvme_write_sync(ENV_PAYLOAD_SLOT_LBA(0), g_blob_head) == 0;
}

/* Rewrite one soft page's permission word, then recompute the writer's
 * index crc. The page bytes and the page's own crc stay untouched: the
 * refusal must be the permission contradiction, not a torn-data refusal. */
static int blob_corrupt_soft_perm(uint32_t bad_perm) {
    if (!blob_load_head_and_index()) return 0;
    struct EnvPayloadHead* h = (struct EnvPayloadHead*)g_blob_head;
    uint32_t total = 0, off = 0, perm = 0;
    if (!blob_index_total(h, &total) ||
        !blob_find_page(h, L_HEAP, &off, &perm) ||
        !(perm & EP_PAGE_PERM_SOFT))
        return 0;
    test_write32(g_blob_index + off + 12u, bad_perm);
    h->crc32 = test_crc32(0, g_blob_index, total + h->console_len);
    uint32_t frames = h->bytes / 4096u - 1u - h->n_pages;
    for (uint32_t f = 0; f < frames; f++) {
        if (nvme_write_sync(ENV_PAYLOAD_SLOT_LBA(0) + 8ull +
                            (uint64_t)f * 8ull,
                            g_blob_index + (uint64_t)f * 4096u) != 0)
            return 0;
    }
    return nvme_write_sync(ENV_PAYLOAD_SLOT_LBA(0), g_blob_head) == 0;
}

static void lmap_reset(void) {
    memset(g_pml4_l, 0, sizeof g_pml4_l);
    g_l_n_pdpt = g_l_n_pd = g_l_n_pt = 0;
}

static void map_page_l(uint64_t v, uint64_t phys, uint64_t flags) {
    uint64_t i4 = (v >> 39) & 0x1ffu, i3 = (v >> 30) & 0x1ffu;
    uint64_t i2 = (v >> 21) & 0x1ffu, i1 = (v >> 12) & 0x1ffu;
    if (!(g_pml4_l[i4] & 1ull)) {
        memset(g_l_pdpt[g_l_n_pdpt], 0, 4096);
        g_pml4_l[i4] = (uint64_t)(uintptr_t)g_l_pdpt[g_l_n_pdpt++] | 0x7ull;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(g_pml4_l[i4] & 0x000ffffffffff000ull);
    if (!(pdpt[i3] & 1ull)) {
        memset(g_l_pd[g_l_n_pd], 0, 4096);
        pdpt[i3] = (uint64_t)(uintptr_t)g_l_pd[g_l_n_pd++] | 0x7ull;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    if (!(pd[i2] & 1ull)) {
        memset(g_l_pt[g_l_n_pt], 0, 4096);
        pd[i2] = (uint64_t)(uintptr_t)g_l_pt[g_l_n_pt++] | 0x7ull;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    pt[i1] = phys | flags;
}

static uint8_t g_l_img[2][4096] __attribute__((aligned(4096)));
static uint8_t g_l_stk[2][4096] __attribute__((aligned(4096)));
static uint8_t g_l_heap[2][4096] __attribute__((aligned(4096)));

/* ─── E7 increment 21: the two arch/frame seams a held replay uses ────────
 * The real user_paging.c is asm/MSR-bound and cannot be linked here (the
 * same reason the NVMe device is a RAM disk); these install the leaf in the
 * SAME synthetic table the walker reads, and hand out one static frame.
 * The flags ARE the caller's: env_payload.c passes USER_PTE_* bits, whose
 * values are arch/x86/user_paging.h's and this test's PTE_* by identity. */
static uint8_t g_map_alloc[2][4096] __attribute__((aligned(4096)));
static int g_map_alloc_next = 0;
void* allocate_physical_ram_frame_for_partition(uint32_t partition_id) {
    (void)partition_id;
    if (g_map_alloc_next >= 2) return 0;
    return g_map_alloc[g_map_alloc_next++];
}
void user_map_page(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (pml4 == g_pml4_l) map_page_l(vaddr, paddr, flags);
    else                  map_page(vaddr, paddr);
}
static void unmap_page_l(uint64_t v) {
    uint64_t i4 = (v >> 39) & 0x1ffu, i3 = (v >> 30) & 0x1ffu;
    uint64_t i2 = (v >> 21) & 0x1ffu, i1 = (v >> 12) & 0x1ffu;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(g_pml4_l[i4] & 0x000ffffffffff000ull);
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    pt[i1] = 0;
}

/* E7 §10.15: the leaf as the synthetic walker sees it. The frame mask
 * deliberately excludes bit 9, so the returned word keeps the soft state
 * and the NX bit alongside the frame the test is checking. */
static uint64_t leaf_flags_l(uint64_t v) {
    uint64_t i4 = (v >> 39) & 0x1ffu, i3 = (v >> 30) & 0x1ffu;
    uint64_t i2 = (v >> 21) & 0x1ffu, i1 = (v >> 12) & 0x1ffu;
    if (!(g_pml4_l[i4] & 1ull)) return 0;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(g_pml4_l[i4] & 0x000ffffffffff000ull);
    if (!(pdpt[i3] & 1ull)) return 0;
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    if (!(pd[i2] & 1ull)) return 0;
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    return pt[i1];
}

static void make_record_linux(void) {
    memset(&g_scratch, 0, sizeof g_scratch);
    g_scratch.magic        = ENV_CKPT_REC_MAGIC;
    g_scratch.version      = ENV_CKPT_REC_VERSION;
    g_scratch.size         = (uint32_t)sizeof(struct EnvCkptRecord);
    g_scratch.sequence     = L_SEQ;
    g_scratch.partition_id = L_PART;
    g_scratch.index        = L_IDX;
    g_scratch.env_id       = 21;
    g_scratch.console_id   = T_CONSOLE_ENV;
    g_scratch.flags        = ENV_CKPT_FLAG_QUIESCED;
    g_scratch.n_tasks      = 2;
    g_scratch.n_regions    = ENV_CKPT_MAX_REGIONS;
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
    strcpy(g_scratch.tasks[1].name, "e7.gate.user.0");
    g_scratch.tasks[1].kind         = ENV_CKPT_TASK_LINUX;
    g_scratch.tasks[1].entry        = L_IMG;
    g_scratch.tasks[1].stack_bottom = L_STACK;
    g_scratch.tasks[1].stack_top    = L_STACK + 2 * 4096;
    g_scratch.tasks[1].cap_count    = 1;
}

/* The gate world, CAPTURED. `linux_ctx` selects the Linux task's posture:
 * 1 = stopped in user space (the restorable form), 0 = parked in its own
 * forwarded syscall (the queued form the blob cannot resume), 2 = the
 * increment-21 gate stop: PROC_HELD at a `syscall` it has not executed
 * (gate_stopped), which is also resumable and is the form the live run
 * relies on to name src_off=196608 deterministically. */
static uint32_t setup_and_capture_linux(int linux_ctx) {
    disk_reset();
    map_reset();
    lmap_reset();
    env_ckpt_reset();
    memset(proc_table, 0, sizeof proc_table);
    linux_groups_init();   /* I1: re-init after the raw table reset */
    g_snap_ret = 1; g_snap_len = 0; g_inject_ret = 1;
    g_identity_seen = 1;

    fill_pattern((uint8_t*)g_region_buf, 4096, 0x11);
    fill_pattern((uint8_t*)g_rdheap_buf, 4096, 0x22);
    fill_pattern((uint8_t*)g_rdstor_buf, 4096, 0x33);
    map_page((uint64_t)(uintptr_t)g_region_buf, (uint64_t)(uintptr_t)g_region_buf);
    map_page((uint64_t)(uintptr_t)g_rdheap_buf, (uint64_t)(uintptr_t)g_rdheap_buf);
    map_page((uint64_t)(uintptr_t)g_rdstor_buf, (uint64_t)(uintptr_t)g_rdstor_buf);

    for (int i = 0; i < 2; i++) {
        fill_pattern(g_l_img[i], 4096, (uint8_t)(0x40 + i));
        fill_pattern(g_l_stk[i], 4096, (uint8_t)(0x60 + i));
        fill_pattern(g_l_heap[i], 4096, (uint8_t)(0x80 + i));
        map_page_l(L_IMG + (uint64_t)i * 4096,
                   (uint64_t)(uintptr_t)g_l_img[i], PTE_RX);
        map_page_l(L_STACK + (uint64_t)i * 4096,
                   (uint64_t)(uintptr_t)g_l_stk[i], PTE_RWNX);
        map_page_l(L_HEAP + (uint64_t)i * 4096,
                   (uint64_t)(uintptr_t)g_l_heap[i], PTE_RWNX);
    }
    if (g_l_soft_heap)
        map_page_l(L_HEAP, (uint64_t)(uintptr_t)g_l_heap[0], PTE_SOFT);

    make_record_linux();
    if (env_ckpt_record(&g_scratch) != 0) return 0;

    struct ProcessDescriptor* px = &proc_table[0];
    px->active = 1;
    strcpy(px->name, "aerosls.posix.2");
    px->cr3 = (uint64_t)(uintptr_t)g_pml4;
    px->waiting_chan = 3u;
    px->park_syscall = SYS_SLS_CAP_RECV;
    px->park_ctx.r11 = 111;
    px->user_rip = IMG_VADDR;
    px->user_rsp = IMG_VADDR + 0x2000;

    struct ProcessDescriptor* lx = &proc_table[1];
    lx->active = 1;
    lx->pid = 101;
    strcpy(lx->name, "e7.gate.user.0");
    lx->cr3 = (uint64_t)(uintptr_t)g_pml4_l;
    lx->state = PROC_SUSPENDED;
    lx->waiting_chan = CAP_NONE;   /* a user-space stop is not a park */
    lx->has_ring3_ctx = 1;
    lx->fs_base = 0x700000ull;
    /* E7 TG-1 I1: the tenant's span/cursors are the group's. */
    if (linux_group_create(lx) != 0) return 0;
    linux_group_stamp_memory(lx, L_HEAP, L_HEAP + 0x10000ull,
                             L_IMG, L_IMG + 0x2000ull);
    linux_group_set_brk(lx, L_HEAP + 0x2000ull);
    linux_group_set_mmap_top(lx, L_HEAP + 0x4000ull);
    if (linux_ctx == 2) {
        /* The stop: HELD, no park, and ring3_ctx built as the stop builds it
         * — rip at the syscall instruction, rax = the number it will re-run,
         * the fd and count still in rdi/rdx. */
        uint64_t* c = (uint64_t*)&lx->ring3_ctx;
        memset(c, 0, sizeof lx->ring3_ctx);
        lx->state = PROC_HELD;
        lx->gate_profile = 1;
        lx->gate_stopped = 1;
        c[9]  = 3;                /* rdi: fd 3, the staged image */
        c[10] = L_HEAP + 0x1000;  /* rsi: the 64 KiB buffer */
        c[11] = 65536;            /* rdx: count */
        c[14] = 0;                /* rax: read, not yet executed */
        c[15] = L_IMG + 0x60;     /* rip: AT the syscall instruction */
        c[17] = 0x202;            /* rflags */
        c[18] = L_STACK + 0x1000; /* rsp */
        lx->user_rip = c[15];
        lx->user_rsp = c[18];
    } else if (linux_ctx) {
        uint64_t* c = (uint64_t*)&lx->ring3_ctx;
        memset(c, 0, sizeof lx->ring3_ctx);
        c[9]  = 0xA1;  /* rdi */
        c[10] = 0xA2;  /* rsi */
        c[11] = 0xA3;  /* rdx */
        c[5]  = 0xA4;  /* r10 */
        c[7]  = 0xA5;  /* r8  */
        c[6]  = 0xA6;  /* r9  */
        c[15] = L_IMG + 0x40;   /* rip: between syscalls */
        c[17] = 0x202;          /* rflags */
        c[18] = L_STACK + 0x1000;  /* rsp */
        lx->user_rip = c[15];
        lx->user_rsp = c[18];
    } else {
        lx->waiting_chan = 4u;
        lx->park_syscall = SYS_LINUX_TRAP_RESUME;
        lx->park_ctx.r11 = 222;
        lx->park_ctx.rcx = L_IMG + 0x80;
        for (int a = 0; a < 6; a++) lx->park_ctx.linux_args[a] = (uint64_t)(0xB0 + a);
        lx->user_rip = L_IMG + 0x80;
        lx->user_rsp = L_STACK + 0x1000;
    }
    return env_payload_capture_all();
}

/* TG-2b: a two-thread gate group. The leader owns the shared image/heap
 * page blocks; the sibling is a distinct task record with its own context
 * and no shared pages of its own. */
#define G_GROUP 101u
static uint32_t setup_and_capture_group(void) {
    disk_reset();
    map_reset();
    lmap_reset();
    env_ckpt_reset();
    memset(proc_table, 0, sizeof proc_table);
    linux_groups_init();
    g_snap_ret = 1; g_snap_len = 0; g_inject_ret = 1;
    g_identity_seen = 1;
    g_l_soft_heap = 0;

    fill_pattern((uint8_t*)g_region_buf, 4096, 0x11);
    fill_pattern((uint8_t*)g_rdheap_buf, 4096, 0x22);
    fill_pattern((uint8_t*)g_rdstor_buf, 4096, 0x33);
    map_page((uint64_t)(uintptr_t)g_region_buf, (uint64_t)(uintptr_t)g_region_buf);
    map_page((uint64_t)(uintptr_t)g_rdheap_buf, (uint64_t)(uintptr_t)g_rdheap_buf);
    map_page((uint64_t)(uintptr_t)g_rdstor_buf, (uint64_t)(uintptr_t)g_rdstor_buf);

    for (int i = 0; i < 2; i++) {
        fill_pattern(g_l_img[i], 4096, (uint8_t)(0x40 + i));
        fill_pattern(g_l_stk[i], 4096, (uint8_t)(0x60 + i));
        fill_pattern(g_l_heap[i], 4096, (uint8_t)(0x80 + i));
        map_page_l(L_IMG + (uint64_t)i * 4096,
                   (uint64_t)(uintptr_t)g_l_img[i], PTE_RX);
        map_page_l(L_STACK + (uint64_t)i * 4096,
                   (uint64_t)(uintptr_t)g_l_stk[i], PTE_RWNX);
        map_page_l(L_HEAP + (uint64_t)i * 4096,
                   (uint64_t)(uintptr_t)g_l_heap[i], PTE_RWNX);
    }

    make_record_linux();
    g_scratch.n_tasks = 3;
    g_scratch.tasks[1].group_id     = G_GROUP;
    g_scratch.tasks[1].thread_index = 0;
    g_scratch.tasks[1].leader       = 1;
    g_scratch.tasks[2] = g_scratch.tasks[1];
    g_scratch.tasks[2].entry        = L_IMG + 0x100;
    g_scratch.tasks[2].thread_index = 1;
    g_scratch.tasks[2].leader       = 0;
    g_scratch.tasks[2].stack_bottom = 0;
    g_scratch.tasks[2].stack_top    = 0;
    if (env_ckpt_record(&g_scratch) != 0) return 0;

    struct ProcessDescriptor* px = &proc_table[0];
    px->active = 1;
    strcpy(px->name, "aerosls.posix.2");
    px->cr3 = (uint64_t)(uintptr_t)g_pml4;
    px->waiting_chan = 3u;
    px->park_syscall = SYS_SLS_CAP_RECV;
    px->park_ctx.r11 = 111;
    px->user_rip = IMG_VADDR;
    px->user_rsp = IMG_VADDR + 0x2000;

    struct ProcessDescriptor* lx = &proc_table[1];
    lx->active = 1;
    lx->pid = G_GROUP;
    strcpy(lx->name, "e7.gate.user.0");
    lx->cr3 = (uint64_t)(uintptr_t)g_pml4_l;
    lx->state = PROC_SUSPENDED;
    lx->waiting_chan = CAP_NONE;
    lx->has_ring3_ctx = 1;
    lx->fs_base = 0x700000ull;
    if (linux_group_create(lx) != 0) return 0;
    linux_group_stamp_memory(lx, L_HEAP, L_HEAP + 0x10000ull,
                             L_IMG, L_IMG + 0x2000ull);
    linux_group_set_brk(lx, L_HEAP + 0x2000ull);
    linux_group_set_mmap_top(lx, L_HEAP + 0x4000ull);
    {
        uint64_t* c = (uint64_t*)&lx->ring3_ctx;
        memset(c, 0, sizeof lx->ring3_ctx);
        c[9]  = 0xA1; c[10] = 0xA2; c[11] = 0xA3;
        c[5]  = 0xA4; c[7]  = 0xA5; c[6]  = 0xA6;
        c[15] = L_IMG + 0x40;
        c[17] = 0x202;
        c[18] = L_STACK + 0x1000;
        lx->user_rip = c[15];
        lx->user_rsp = c[18];
    }

    struct ProcessDescriptor* sx = &proc_table[2];
    sx->pid = G_GROUP + 1u;
    strcpy(sx->name, "e7.gate.user.0");
    sx->group_index = LINUX_GROUP_NONE;
    sx->state = PROC_SUSPENDED;
    sx->has_ring3_ctx = 1;
    sx->waiting_chan = CAP_NONE;
    sx->fs_base = 0x701000ull;
    {
        uint64_t* c = (uint64_t*)&sx->ring3_ctx;
        c[9]  = 0xD1; c[10] = 0xD2; c[11] = 0xD3;
        c[5]  = 0xD4; c[7]  = 0xD5; c[6]  = 0xD6;
        c[15] = L_IMG + 0x100;
        c[17] = 0x202;
        c[18] = L_STACK + 0x800;
        sx->user_rip = c[15];
        sx->user_rsp = c[18];
    }
    if (linux_group_join_thread(linux_group_of(lx), sx) != 0) return 0;
    sx->active = 1;
    return env_payload_capture_all();
}

static void simulate_fresh_boot_linux(void) {
    for (int i = 0; i < 2; i++) {
        memset(g_l_img[i], 0, 4096);
        memset(g_l_stk[i], 0, 4096);
        memset(g_l_heap[i], 0, 4096);
    }
    memset(&proc_table[1].ring3_ctx, 0, sizeof proc_table[1].ring3_ctx);
    proc_table[1].has_ring3_ctx = 1;   /* the adopt path has a task to pour into */
    proc_table[1].state = PROC_SUSPENDED;
    proc_table[1].waiting_chan = CAP_NONE;
    proc_table[1].fs_base = 0;
    linux_group_set_brk(&proc_table[1], 0);
    linux_group_set_mmap_top(&proc_table[1], 0);
    proc_table[1].user_rip = L_IMG;    /* the fresh entry, not the capture point */
    proc_table[1].user_rsp = L_STACK + 0x2000;
    if (proc_table[2].active) {
        memset(&proc_table[2].ring3_ctx, 0, sizeof proc_table[2].ring3_ctx);
        proc_table[2].has_ring3_ctx = 1;
        proc_table[2].state = PROC_SUSPENDED;
        proc_table[2].waiting_chan = CAP_NONE;
        proc_table[2].fs_base = 0;
        proc_table[2].user_rip = L_IMG + 0x100;
        proc_table[2].user_rsp = L_STACK + 0x800;
    }
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
    CHECK(kernel_tick_counter >= EP_WAIT_TICKS &&
          kernel_tick_counter < EP_WAIT_CEIL_TICKS,
          "wait: ... and a sidecar that stopped being scheduled is out of time at "
          "the STALL window, not at the ceiling");

    /* The run-12 clause (live, 2026-11-01): `drv.ramdisk.1` was still RUNNING —
     * being scheduled, serving the replay's own boot I/O — when the old fixed
     * 30 s deadline expired, so the wait answered "not parked" and the pour
     * refused a healthy environment by name. A task that is still being
     * switched in is still working: the wait must keep going past the old
     * bound and answer 1 once the task parks. */
    CHECK(setup_and_capture(1) == 1, "re-capture for the still-working case");
    g_identity_seen = 0;
    proc_table[0].waiting_chan = CAP_NONE;          /* still running its boot */
    g_switch_each_yield = 1;                        /* ... and being scheduled */
    g_park_at_tick = (uint64_t)EP_WAIT_TICKS + 500u;/* ... which is not instant */
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 1,
          "wait: an environment still being scheduled past the old fixed bound is "
          "waited for until it parks, not reported as a posture that never came");
    CHECK(kernel_tick_counter > EP_WAIT_TICKS,
          "wait: ... and that case really did outlive the bound the live run died on");

    /* ... but progress is not an infinite licence: an environment that keeps
     * being scheduled and never parks is still answered at the ceiling. */
    CHECK(setup_and_capture(1) == 1, "re-capture for the ceiling case");
    g_identity_seen = 0;
    proc_table[0].waiting_chan = CAP_NONE;
    g_switch_each_yield = 1;
    g_park_at_tick = 0;                             /* never parks */
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 0,
          "wait: an environment that never stops being scheduled is still answered "
          "at the ceiling instead of hanging the restore");
    CHECK(kernel_tick_counter >= EP_WAIT_CEIL_TICKS,
          "wait: ... and it was the ceiling that answered, not the stall window");
    g_switch_each_yield = 0;

    proc_table[0].active = 0;
    kernel_tick_counter = 0;
    CHECK(env_payload_wait_parked(&g_scratch) == 0,
          "wait: a missing sidecar is answered at once (there is nothing to wait for)");
    CHECK(g_service_pumps > 0,
          "wait: the loop keeps the service work ticking while it waits — the "
          "identity flag it reads is set by env_console_tick(), which on a "
          "uniprocessor boot only runs from this pump");
    CHECK(g_keepalive_pumps > 0,
          "wait: the loop also pumps the sweep keepalive -- this wait blocks the "
          "restore's sweep, and the heartbeat stream must survive it (2026-10-23)");

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

    /* ── 5. E7 increment 20: the Linux task's capture and round trip ────── */
    CHECK(setup_and_capture_linux(1) == 1,
          "LINUX: the gate environment (sidecar + Linux tenant) captures as one payload");
    simulate_fresh_boot_linux();
    proc_table[0].park_ctx.r11 = 0;   /* the fresh boot's value, not the capture's */
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_NONE,
          "LINUX: the restore pours a payload whose third task is the Linux tenant");
    CHECK(pages == 9,
          "LINUX: nine pages pour — three sidecar region pages + the tenant's two image, two stack and two heap pages");
    CHECK(memcmp(g_l_img[0], (uint8_t[]){0x40 ^ 0, 0x40 ^ 1, 0x40 ^ 2, 0x40 ^ 3}, 4) == 0 &&
          memcmp(g_l_img[1], (uint8_t[]){0x41 ^ 0, 0x41 ^ 1, 0x41 ^ 2, 0x41 ^ 3}, 4) == 0,
          "LINUX: the linked image's bytes come back at 0x400000 (its own span, not the image window)");
    CHECK(memcmp(g_l_stk[0], (uint8_t[]){0x60 ^ 0, 0x60 ^ 1, 0x60 ^ 2, 0x60 ^ 3}, 4) == 0 &&
          memcmp(g_l_heap[1], (uint8_t[]){0x81 ^ 0, 0x81 ^ 1, 0x81 ^ 2, 0x81 ^ 3}, 4) == 0,
          "LINUX: the tenant's stack and budget-span pages come back too");
    {
        const uint64_t* c = (const uint64_t*)&proc_table[1].ring3_ctx;
        CHECK(c[15] == L_IMG + 0x40 && c[18] == L_STACK + 0x1000 &&
              c[9] == 0xA1 && c[5] == 0xA4 && c[6] == 0xA6,
              "LINUX: the full ring3_ctx — rip, rsp and the six argument registers — is the captured one");
    }
    CHECK(proc_table[1].fs_base == 0x700000ull &&
          linux_group_brk(&proc_table[1]) == L_HEAP + 0x2000ull &&
          linux_group_mmap_top(&proc_table[1]) == L_HEAP + 0x4000ull,
          "LINUX: fs_base and both memory cursors come back with the context");
    CHECK(proc_table[1].user_rip == L_IMG + 0x40 &&
          proc_table[1].user_rsp == L_STACK + 0x1000 &&
          proc_table[1].has_ring3_ctx == 1,
          "LINUX: the task resumes at the captured rip/rsp, not at its entry");
    CHECK(proc_table[0].park_ctx.r11 == 111,
          "LINUX: the sidecar task in the SAME payload still gets its park frame (the sidecar path is unchanged)");

    /* ── 6. TG-2b: two contexts, one shared page block ─────────────────── */
    CHECK(setup_and_capture_group() == 1,
          "GROUP: a sidecar plus a two-thread Linux group captures as one payload");
    CHECK(blob_load_head_and_index(),
          "GROUP: the captured v4 blob can be read back");
    {
        struct EnvPayloadHead* h = (struct EnvPayloadHead*)g_blob_head;
        struct EnvPayloadTask t1, t2;
        CHECK(h->n_tasks == 3 && h->n_pages == 9,
              "GROUP: the blob carries three task entries and the nine unique pages (the shared heap is not doubled)");
        CHECK(blob_task_at(h, 1, &t1) && blob_task_at(h, 2, &t2),
              "GROUP: both Linux task entries are present");
        CHECK(t1.group_id == G_GROUP && t1.thread_index == 0 && t1.leader == 1 &&
              t1.n_pages == 6,
              "GROUP: the leader is (group 101, index 0), owns the image/stack/heap pages");
        CHECK(t2.group_id == G_GROUP && t2.thread_index == 1 && t2.leader == 0 &&
              t2.n_pages == 0,
              "GROUP: the sibling is a distinct (group 101, index 1) with no duplicate shared pages");
        CHECK(t2.brk_cur == 0 && t2.mmap_top == 0 && t2.heap_base == 0 &&
              t2.image_lo == 0,
              "GROUP: the sibling carries no second copy of the group's heap/image cursors");
        CHECK(t1.linux_regs[15] == L_IMG + 0x40 &&
              t2.linux_regs[15] == L_IMG + 0x100 &&
              t1.fs_base == 0x700000ull && t2.fs_base == 0x701000ull,
              "GROUP: each thread carries its own rip and fs_base context");
    }
    simulate_fresh_boot_linux();
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_NONE && pages == 9,
          "GROUP: the group replay reaches the pour and writes each shared page once");
    {
        const uint64_t* lc = (const uint64_t*)&proc_table[1].ring3_ctx;
        const uint64_t* sc = (const uint64_t*)&proc_table[2].ring3_ctx;
        CHECK(lc[15] == L_IMG + 0x40 && sc[15] == L_IMG + 0x100 &&
              lc[9] == 0xA1 && sc[9] == 0xD1,
              "GROUP: both thread contexts are installed into their own descriptors");
    }
    CHECK(proc_table[1].fs_base == 0x700000ull &&
          proc_table[2].fs_base == 0x701000ull,
          "GROUP: each thread keeps its own TLS base");
    CHECK(linux_group_brk(&proc_table[1]) == L_HEAP + 0x2000ull &&
          linux_group_mmap_top(&proc_table[1]) == L_HEAP + 0x4000ull,
          "GROUP: the leader's shared cursors are restored once, and the sibling does not overwrite them");

    /* A shared root with the same vaddr twice is only coherent when both
     * records describe the same page. A different CRC is a corrupt shared
     * page index, refused by its own name before the pour. */
    CHECK(setup_and_capture_linux(1) == 1,
          "DUP: a payload is captured for the duplicate-page tooth");
    CHECK(blob_duplicate_page_bad_crc(),
          "DUP: a page entry can be duplicated in place with a different CRC");
    simulate_fresh_boot_linux();
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_DUP && pages == 0 && cons == 0,
          "DUP: one vaddr twice with different bytes is refused by name, before any pour");
    CHECK(env_payload_last_refusal().a == 0 &&
          env_payload_last_refusal().b == 1,
          "DUP: the refusal names the first entry and the duplicate");

    /* E7 §10.15: a soft leaf is captured and restored as a soft leaf.
     * The page table is reset to its fresh shape with the heap page
     * missing; the pour must allocate that page and install the state
     * bit, not silently make it user-accessible again. */
    g_l_soft_heap = 1;
    CHECK(setup_and_capture_linux(1) == 1,
          "SOFT: a present supervisor-only leaf is captured with its bytes");
    g_l_soft_heap = 0;
    CHECK((leaf_flags_l(L_HEAP) & PTE_SOFT) == PTE_SOFT,
          "SOFT: the captured leaf keeps P, NX and SOFT_NONE and clears USER/WRITE");
    simulate_fresh_boot_linux();
    unmap_page_l(L_HEAP);
    g_map_alloc_next = 0;
    proc_table[1].restore_held = 1;
    proc_table[1].has_ring3_ctx = 0;
    proc_table[1].state = PROC_HELD;
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_NONE,
          "SOFT: a missing soft page is mapped by the pour, not refused as NO_MAPPING");
    CHECK(g_map_alloc_next == 1 && pages == 9,
          "SOFT: exactly the missing page got a frame and all nine pages poured");
    {
        uint64_t soft = leaf_flags_l(L_HEAP);
        CHECK((soft & PTE_SOFT) == PTE_SOFT &&
              !(soft & 0x2ull) && !(soft & 0x4ull),
              "SOFT: the restored leaf is P|NX|SOFT_NONE, supervisor-only and not writable");
        CHECK((soft & 0x000ffffffffff000ull) ==
              (uint64_t)(uintptr_t)g_map_alloc[0],
              "SOFT: the soft leaf keeps the frame the pour allocated");
    }
    CHECK(memcmp(g_map_alloc[0],
                 (uint8_t[]){0x80 ^ 0, 0x80 ^ 1, 0x80 ^ 2, 0x80 ^ 3}, 4) == 0,
          "SOFT: the captured bytes are in the newly mapped soft frame");

    /* TG-2b: the task stride grew for group identity. A previous-version
     * blob is refused before its task array is walked; its page blocks would
     * otherwise be read eight bytes late. */
    CHECK(setup_and_capture_linux(1) == 1,
          "V3: a soft-free payload is captured by the v4 writer");
    CHECK(blob_set_head_version(ENV_PAYLOAD_REC_VERSION_PREV),
          "V3: the blob head can carry the previous version for the refusal tooth");
    simulate_fresh_boot_linux();
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_FORMAT && pages == 0 && cons == 0,
          "V3: a v3 blob is refused by format before any pour (the v4 task stride is not its stride)");

    /* A soft page is its own state: USER or W/X alongside it is a
     * contradiction. Rewrite the permission word and recompute the index
     * CRC so the refusal is the permission rule, not a torn blob. */
    g_l_soft_heap = 1;
    CHECK(setup_and_capture_linux(1) == 1,
          "SOFT+USER: a soft page is captured for the contradiction tooth");
    g_l_soft_heap = 0;
    CHECK(blob_corrupt_soft_perm(EP_PAGE_PERM_SOFT | EP_PAGE_PERM_USER),
          "SOFT+USER: the page entry can be made contradictory with a valid index CRC");
    simulate_fresh_boot_linux();
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_FORMAT && pages == 0 && cons == 0,
          "SOFT+USER: the restore refuses the contradictory permission before a byte moves");
    CHECK(env_payload_last_refusal().a == 8,
          "SOFT+USER: the refusal names the permission rule (a=8), not a torn index CRC");

    g_l_soft_heap = 1;
    CHECK(setup_and_capture_linux(1) == 1,
          "SOFT+WX: a soft page is captured for the executable/writable tooth");
    g_l_soft_heap = 0;
    CHECK(blob_corrupt_soft_perm(EP_PAGE_PERM_SOFT | EP_PAGE_PERM_WRITE |
                                 EP_PAGE_PERM_EXEC),
          "SOFT+WX: the page entry can be made writable+executable with a valid index CRC");
    simulate_fresh_boot_linux();
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_FORMAT && pages == 0 && cons == 0,
          "SOFT+WX: a writable+executable soft page is refused by format, not poured");
    CHECK(env_payload_last_refusal().a == 8,
          "SOFT+WX: the refusal names the permission rule (a=8), not a torn index CRC");

    CHECK(setup_and_capture_linux(1) == 1,
          "LINUX: re-capture for the permission tooth");
    simulate_fresh_boot_linux();
    map_page_l(L_IMG, (uint64_t)(uintptr_t)g_l_img[0], PTE_RWNX);
    CHECK(env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons)
              == EP_REFUSE_PERM,
          "PERM: a captured RX image page that is writable at the destination refuses the whole pour");
    CHECK(g_l_img[0][0] == 0 && g_l_heap[0][0] == 0,
          "PERM: and nothing was written — the refusal came before the first byte");
    CHECK(env_payload_last_refusal().a == 1 &&
          env_payload_last_refusal().b == 3,
          "PERM: the refusal names the task (1, the Linux slot) and the flat page index it faulted on (3 — the three sidecar pages precede it)");

    /* E7 increment 21: the deterministic stop's form. */
    CHECK(setup_and_capture_linux(2) == 1,
          "ENTRY: a tenant stopped AT its syscall captures (the gate's named instant)");
    simulate_fresh_boot_linux();
    proc_table[1].gate_stopped = 0;
    proc_table[1].state = PROC_SUSPENDED;
    pages = 0; cons = 0;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_NONE,
          "ENTRY: the stop is a resumable form — poured, not refused");
    {
        const uint64_t* c = (const uint64_t*)&proc_table[1].ring3_ctx;
        CHECK(c[15] == L_IMG + 0x60 && c[14] == 0 &&
              c[9] == 3 && c[11] == 65536,
              "ENTRY: the restored frame is the stopped one — rip AT the syscall, rax=read, fd 3 and count intact");
    }
    CHECK(proc_table[1].user_rip == L_IMG + 0x60,
          "ENTRY: the task resumes AT the continuation, not at _start");
    /* The stop is one-shot: the replay's create rebuilds the descriptor from
     * the same FLAG_GATE manifest, so it arrives armed (gate_profile=1,
     * gate_bytes=0). Pouring FORM_ENTRY spends it — otherwise the resumed
     * copy is stopped again at the same cumulative offset, mid-continuation,
     * which is precisely how increment 21's first live run hung in boot 2. */
    CHECK(proc_table[1].gate_profile == 0 &&
          proc_table[1].gate_bytes == PROC_GATE_SRC_OFF,
          "ENTRY: the stop is spent at the pour — the resumed read is forwarded, not held a second time");

    CHECK(setup_and_capture_linux(0) == 1,
          "LINUX: a tenant parked in its forwarded syscall still captures (the form is recorded, not skipped)");
    CHECK(env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons)
              == EP_REFUSE_FORM,
          "FORM: a parked Linux tenant is refused by name — its queued request is state this blob does not carry");

    /* ── 5b. E7 increment 21: a HELD replay maps what the spawn never did ──
     * The replay's create holds its Linux child (cap_linux_restore_arm), so
     * the fresh child has no ring3_ctx and its heap pages — mapped LAZILY by
     * the original run's brk — do not exist in its address space. The pour
     * must map them, charged to the child's partition, with the captured
     * grant; refusing them as NO_MAPPING would make every real replay refuse
     * on its first grown heap page. */
    CHECK(setup_and_capture_linux(1) == 1,
          "ADOPT: re-capture for the held-replay mapping tooth");
    simulate_fresh_boot_linux();
    unmap_page_l(L_HEAP + 4096);
    g_map_alloc_next = 0;
    proc_table[1].restore_held = 1;    /* the create held it for this pour */
    proc_table[1].has_ring3_ctx = 0;   /* it has never run a single instruction */
    proc_table[1].state = PROC_HELD;
    rc = env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons);
    CHECK(rc == EP_REFUSE_NONE,
          "ADOPT: a held tenant's unmapped heap page does not refuse the pour — the replay maps it");
    CHECK(g_map_alloc_next == 1 && pages == 9,
          "ADOPT: exactly the one missing page got a freshly allocated frame, and all nine pages poured");
    {
        uint64_t phys = 0;
        CHECK(env_payload_resolve_vaddr((uint64_t)(uintptr_t)g_pml4_l,
                                        L_HEAP + 4096, &phys) == 1 &&
              phys == (uint64_t)(uintptr_t)g_map_alloc[0],
              "ADOPT: the new frame is mapped at the captured vaddr, not somewhere the allocator chose");
    }
    CHECK(memcmp(g_map_alloc[0],
                 (uint8_t[]){0x81 ^ 0, 0x81 ^ 1, 0x81 ^ 2, 0x81 ^ 3}, 4) == 0,
          "ADOPT: and the captured bytes are in that new frame");
    CHECK(proc_table[1].user_rip == L_IMG + 0x40 &&
          proc_table[1].has_ring3_ctx == 1,
          "ADOPT: the held child's first instruction is the captured rip, not _start");

    /* The not-held shape is still refused: a live task with a context is the
     * pre-21 replay, and pouring over it would overwrite a task that ran. */
    CHECK(setup_and_capture_linux(1) == 1,
          "ADOPT: re-capture for the not-held refusal");
    simulate_fresh_boot_linux();
    proc_table[1].restore_held = 0;
    proc_table[1].has_ring3_ctx = 0;
    CHECK(env_payload_restore(L_PART, L_IDX, 22, L_SEQ, &pages, &cons)
              == EP_REFUSE_NOT_PARKED,
          "ADOPT: a Linux task neither held nor carrying a context is refused by name — the hold is what makes the pour's target a task that has not run");

    printf("\n%d passed, %d failed\n", checks_passed, checks_failed);
    return checks_failed ? 1 : 0;
}
