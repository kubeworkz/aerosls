/*
 * env_payload.c — POSIX-Environments v0.2 Phase P1a: the payload half of
 * the environment checkpoint. See kernel/env_payload.h for the design
 * writeup (the placement rule, the register rule, and why the pour never
 * yields).
 *
 * Dependencies are the host-testable set: env_ckpt.h (the records and the
 * captured placement), env_console.h (the buffered bytes), process.h
 * (proc_table, the page tables to walk), cap.h (CAP_NONE, the park
 * syscalls), persist.h (the directory's LBAs), nvme_io.h (the device),
 * kernel_io.h (the two logging functions), timer.h (kernel_tick_counter,
 * for the bounded wait). Host tests stub the NVMe layer with a RAM disk
 * the way tests/rowstore_io_stubs.c does and build synthetic page tables —
 * the walk is plain pointer arithmetic over the identity map, exactly how
 * cap_create_sidecar itself touches page tables.
 *
 * ─── Blob layout (all zone transitions are 4 KiB aligned) ────────────────
 *   frame 0                    EnvPayloadHead (crc32 over the index zone)
 *   frames 1 .. 1+R-1          the index zone:
 *     regions[3]               the captured placement (the placement rule)
 *     tasks[n_tasks]           names, register form, park save areas,
 *                              and (v2) the Linux context/cursors/range
 *     per task: u32 n_pages, then n_pages x {u64 vaddr, u32 crc32, u32 perm}
 *     console[console_len]     the buffered output, non-destructively read
 *     zero pad to the frame boundary
 *   frames 1+R ..             page data, 4 KiB per page, in index order
 *
 * Capture streams the data zone first (the counts are known before any
 * byte moves), then the index frames, then frame 0 — so the head carries a
 * crc over bytes that already exist on media. Restore validates EVERYTHING
 * — head, index crc, placement, processes, park form, every destination
 * mapping, every page crc (read and discarded) — before the pour writes a
 * single byte. That ordering is refusal over partial application applied
 * to memory.
 */
#include "env_payload.h"
#include "env_ckpt.h"
#include "env_console.h"
#include "persist.h"
#include "process.h"
#include "cap.h"            /* CAP_NONE, SYS_SLS_CAP_RECV, SYS_SLS_CHAN_WAIT */
#include "linux_abi.h"      /* E7 increment 20: SYS_LINUX_TRAP_RESUME, the park the Linux form reads */
#include "../drivers/nvme_io.h"
#include "kernel_io.h"
#include "timer.h"          /* kernel_tick_counter */
#include "smp.h"            /* smp_uniprocessor_tick() — the wait's own service pump */
#include "failover.h"       /* failover_sweep_keepalive() — the parked wait runs on the sweep too */
#include "env_replica.h"     /* P3 destination (a): env_replica_push(), the capture-side wire push */
#include "frame_pool.h"     /* E7 increment 21: a replay's missing heap page gets a frame */
#include "../arch/x86/user_paging.h"  /* ... and is mapped with user_map_page */

/* P3 destination (a), increment 2: the strong definition lives in
 * kernel/env_replica.c; this weak default keeps THIS file's dependency
 * surface where it was — tests/env_payload_host_test.c links the payload
 * layer without the replica subsystem and gets "pushed to zero peers",
 * never an undefined symbol. The kernel link takes the strong one. */
__attribute__((weak)) int env_replica_push(const struct EnvCkptRecord* rec) {
    (void)rec;
    return 0;
}

/* USER_PROC_CODE_BASE — cap.c's literal for the image/stack window. Kept
 * as a local mirror; tests/env_checkpoint_restore_check.sh pins it against
 * cap.c's literal so the window cannot drift away from what the create
 * path maps. */
#define EP_IMAGE_VBASE   0x400000000000ULL
#define EP_IMAGE_WINDOW  0x40000000ULL      /* 1 GiB — image, guard page, stack; nothing else maps here */

#define EP_MAX_PAGES     2048u
/* The index zone's frame ceiling. Six frames was the sidecar-only shape;
 * E7 increment 20's live gate capture measured 31,012 index bytes — three
 * tasks (the Linux tenant's EnvPayloadTask is 408 bytes), ~1,845 page
 * entries at 16 bytes and the console — so six frames refused an
 * environment that fits the band. Sixteen frames keeps the ceiling above
 * the slot's own limit: a 16-frame index leaves 2,031 data pages in an
 * 8 MiB slot, and the slot check below refuses a larger capture by name
 * before the index can grow past this buffer. */
#define EP_STAGE_BYTES   (4096u * 16u)
#define EP_MAX_TASKS     4u
/* EP_WAIT_TICKS / EP_WAIT_CEIL_TICKS live in env_payload.h: the wait's two
 * bounds are part of its contract now, and the host test asserts WHICH of the
 * two answered (a stall is a wedged environment, a ceiling is one that never
 * stopped working). */
/* How many consecutive ready passes the parked posture must hold before the
 * wait answers. A replayed environment's boot is not done when its sidecars
 * first look parked: the sidecar's own init can still be issuing ramdisk I/O,
 * and the driver it woke a few ticks later is not parked when the pour checks
 * (measured, increment 21's live run: the wait's first ready pass named both
 * sidecars parked; the pour then saw 'drv.ramdisk.1' roused by the request
 * that had just landed on it). Each settle pass pumps service work and yields
 * to Ring-3, so the woken task actually runs, serves and re-parks before the
 * pass is counted. */
#define EP_WAIT_SETTLE   3u
#define EP_CONSOLE_MAX   4096u              /* ENV_CONSOLE_BUF */

static struct EnvPayloadRefusal ep_refusal;

#if E7G_TOOTH
/* E7 increment 22 (env_payload.h): the tooth the next pour installs. It is
 * declared up here because the selector below it is public; the pages a
 * `descriptor` tooth leaves out of the pour live beside the capture scratch
 * buffers, further down. */
static int ep_tooth;
#endif

void env_payload_clear_refusal(void) {
    ep_refusal.code = EP_REFUSE_NONE;
    ep_refusal.a = 0;
    ep_refusal.b = 0;
}

static void ep_refuse(uint32_t code, uint32_t a, uint32_t b) {
    ep_refusal.code = code;
    ep_refusal.a = a;
    ep_refusal.b = b;
}

struct EnvPayloadRefusal env_payload_last_refusal(void) { return ep_refusal; }

/* ─── E7 increment 22: the tooth selection (env_payload.h) ────────────────
 * The parser and the names exist in EVERY build (the route's refusal needs
 * to know a name is a tooth to say so), while the breaks below it do not. */
int env_payload_build_has_teeth(void) { return E7G_TOOTH; }

int env_payload_tooth_from_name(const char* name) {
    if (!name) return -1;
    if (name[0] == '\0' || (name[0] == 'n' && name[1] == 'o' &&
                            name[2] == 'n' && name[3] == 'e' &&
                            name[4] == '\0'))
        return ENV_PAYLOAD_TOOTH_NONE;
    if (name[0] == 'd' && name[1] == 'e' && name[2] == 's' &&
        name[3] == 'c' && name[4] == 'r' && name[5] == 'i' &&
        name[6] == 'p' && name[7] == 't' && name[8] == 'o' &&
        name[9] == 'r' && name[10] == '\0')
        return ENV_PAYLOAD_TOOTH_DESCRIPTOR;
    if (name[0] == 'n' && name[1] == 'o' && name[2] == '-' &&
        name[3] == 'f' && name[4] == 'd' && name[5] == '\0')
        return ENV_PAYLOAD_TOOTH_NO_FD;
    if (name[0] == 'n' && name[1] == 'o' && name[2] == '-' &&
        name[3] == 'h' && name[4] == 'e' && name[5] == 'a' &&
        name[6] == 'p' && name[7] == '\0')
        return ENV_PAYLOAD_TOOTH_NO_HEAP;
    if (name[0] == 'f' && name[1] == 's' && name[2] == '_' &&
        name[3] == 'b' && name[4] == 'a' && name[5] == 's' &&
        name[6] == 'e' && name[7] == '\0')
        return ENV_PAYLOAD_TOOTH_FS_BASE;
    return -1;
}

const char* env_payload_tooth_name(int tooth) {
    switch (tooth) {
    case ENV_PAYLOAD_TOOTH_DESCRIPTOR: return "descriptor";
    case ENV_PAYLOAD_TOOTH_NO_FD:      return "no-fd";
    case ENV_PAYLOAD_TOOTH_NO_HEAP:    return "no-heap";
    case ENV_PAYLOAD_TOOTH_FS_BASE:    return "fs_base";
    default:                           return "none";
    }
}

void env_payload_set_tooth(int tooth) {
#if E7G_TOOTH
    ep_tooth = (tooth == ENV_PAYLOAD_TOOTH_DESCRIPTOR ||
                tooth == ENV_PAYLOAD_TOOTH_NO_FD ||
                tooth == ENV_PAYLOAD_TOOTH_NO_HEAP ||
                tooth == ENV_PAYLOAD_TOOTH_FS_BASE)
                   ? tooth : ENV_PAYLOAD_TOOTH_NONE;
#else
    (void)tooth;   /* no arms in this build: the route refuses the request */
#endif
}

void env_payload_refusal_text(const struct EnvPayloadRefusal* r,
                              char* out, uint32_t cap) {
    if (!out || cap == 0) return;
    const char* t = "ok";
    switch (r->code) {
    case EP_REFUSE_NONE:       t = "ok"; break;
    case EP_REFUSE_ABSENT:     t = "no payload was captured for this environment"; break;
    case EP_REFUSE_IO:         t = "the payload could not be read from the device"; break;
    case EP_REFUSE_FORMAT:     t = "the payload's format or bytes are not this build's"; break;
    case EP_REFUSE_SEQ:        t = "the payload belongs to a different checkpoint than the record"; break;
    case EP_REFUSE_PLACEMENT:  t = "the replay placed the environment's regions away from their captured bases"; break;
    case EP_REFUSE_TOO_BIG:    t = "the payload does not fit its band slot"; break;
    case EP_REFUSE_NO_PROCESS: t = "a captured sidecar has no live process to pour into"; break;
    case EP_REFUSE_RUNNING:    t = "the capture caught a sidecar mid-compute, not parked"; break;
    case EP_REFUSE_NOT_PARKED: t = "a live sidecar is not parked in the recv the capture recorded"; break;
    case EP_REFUSE_NO_MAPPING: t = "a captured page has no live frame at its vaddr"; break;
    case EP_REFUSE_CONSOLE:    t = "the replayed environment has no console for the buffered bytes"; break;
    case EP_REFUSE_SKIPPED:    t = "the restore was asked for metadata only (payload withheld)"; break;
    case EP_REFUSE_PERM:       t = "a captured page's live permissions are not the captured ones"; break;
    case EP_REFUSE_FORM:       t = "the Linux task was captured parked (queued state this blob does not carry)"; break;
    case EP_REFUSE_SHAPE:      t = "a captured task's kind does not match the slot it was filed under"; break;
    default:                   t = "unknown payload refusal"; break;
    }
    uint32_t i = 0;
    while (t[i] && i + 1 < cap) { out[i] = t[i]; i++; }
    out[i] = '\0';
    if ((r->a || r->b) && i + 20 < cap) {
        const char* pre = " (a=";
        for (uint32_t k = 0; pre[k] && i + 1 < cap; k++) out[i++] = pre[k];
        uint32_t v = r->a, rev[10]; uint32_t rn = 0;
        do { rev[rn++] = v % 10u; v /= 10u; } while (v);
        while (rn && i + 1 < cap) out[i++] = (char)('0' + rev[--rn]);
        const char* mid = " b=";
        for (uint32_t k = 0; mid[k] && i + 1 < cap; k++) out[i++] = mid[k];
        v = r->b; rn = 0;
        do { rev[rn++] = v % 10u; v /= 10u; } while (v);
        if (!rn) rev[rn++] = 0;
        while (rn && i + 1 < cap) out[i++] = (char)('0' + rev[--rn]);
        if (i + 1 < cap) out[i++] = ')';
    }
    out[i] = '\0';
}

/* ─── crc32 (reflected, poly 0xEDB88320), incremental ─────────────────────── */
static uint32_t ep_crc(uint32_t crc, const void* data, uint32_t n) {
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

static void ep_memset(void* dst, uint8_t v, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

static void ep_copy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static int ep_io_ready(void) { return io_sq != 0; }

/* Static scratch. Capture and restore never overlap (a capture runs inside
 * a quiesce, a pour inside a restore pass), so one set serves both, and
 * nothing here is on a kernel stack — every buffer is a frame-sized. */
static union {
    uint64_t vaddr[EP_MAX_PAGES];   /* capture: collected page list */
    uint64_t dest[EP_MAX_PAGES];    /* restore: validated destinations */
} ep_scratch;
static uint8_t  ep_tnum[EP_MAX_PAGES];              /* capture: task index per page */
static uint32_t ep_crcs[EP_MAX_PAGES];              /* capture: per-page crc, staged into the index */
static uint32_t ep_perms[EP_MAX_PAGES];             /* capture: per-page PTE perms (EP_PAGE_PERM_*) */
#if E7G_TOOTH
/* E7 increment 22: the pages a `descriptor` tooth leaves out of the pour.
 * File-static because a restore is one straight-line pass; the tooth's
 * effect on the TENANT travels on ProcessDescriptor.tooth, not from here. */
static uint8_t  ep_skip[EP_MAX_PAGES];

/* The `fs_base` tooth's admissibility test, over the record's own page block:
 * the arm may only move the TLS base to a page the blob says will be mapped,
 * and only when its captured grant is writable — a TLS block is written (the
 * errno slot, the canary) as well as read, so a read-only neighbour would
 * turn the tooth into a fault instead of a wrong value. */
static int ep_page_recorded_writable(const uint8_t* block, uint32_t np,
                                     uint64_t vpage) {
    for (uint32_t k = 0; k < np; k++) {
        const uint8_t* e = block + (uint64_t)k * EP_PAGE_ENTRY_BYTES;
        uint64_t v = 0;
        for (uint32_t b = 0; b < 8; b++) v |= ((uint64_t)e[b]) << (8u * b);
        if ((v & ~0xfffull) != vpage) continue;
        uint32_t perm = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                        ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        return (perm & EP_PAGE_PERM_WRITE) != 0;
    }
    return 0;
}
#endif
static uint8_t  ep_stage[EP_STAGE_BYTES] __attribute__((aligned(4096)));
static uint8_t  ep_page[4096] __attribute__((aligned(4096)));
static uint8_t  ep_console[EP_CONSOLE_MAX];

/* ─── The directory ──────────────────────────────────────────────────────── */
static int ep_dir_read(struct EnvPayloadDirHead* h,
                       struct EnvPayloadDirEntry* e) {
    if (!ep_io_ready()) return -1;
    ep_memset(h, 0, (uint32_t)sizeof(*h));
    ep_memset(e, 0, (uint32_t)(sizeof(*e) * ENV_PAYLOAD_MAX));
    if (nvme_read_sync(PERSIST_ENV_PAYLOAD_HDR_LBA, ep_stage) != 0) return -1;
    ep_copy(h, ep_stage, (uint32_t)sizeof(*h));
    if (h->magic != ENV_PAYLOAD_DIR_MAGIC ||
        h->version != ENV_PAYLOAD_REC_VERSION ||
        h->rec_size != (uint32_t)sizeof(struct EnvPayloadDirEntry)) {
        /* A directory this build cannot read is an ABSENT payload, never a
         * half-read one. */
        return -1;
    }
    if (nvme_read_sync(PERSIST_ENV_PAYLOAD_ENT_LBA, ep_stage) != 0) return -1;
    uint32_t n = h->count;
    if (n > ENV_PAYLOAD_MAX) n = ENV_PAYLOAD_MAX;
    for (uint32_t i = 0; i < n; i++)
        ep_copy(&e[i], ep_stage + i * sizeof(struct EnvPayloadDirEntry),
                (uint32_t)sizeof(struct EnvPayloadDirEntry));
    return (int)n;
}

static int ep_dir_write(const struct EnvPayloadDirEntry* e, uint32_t count) {
    struct EnvPayloadDirHead h;
    h.magic = ENV_PAYLOAD_DIR_MAGIC;
    h.version = ENV_PAYLOAD_REC_VERSION;
    h.count = count;
    h.rec_size = (uint32_t)sizeof(struct EnvPayloadDirEntry);
    h._pad = 0;
    /* Entries first, header last: a torn pair leaves the old header
     * pointing at entries that never changed, never the reverse. */
    ep_memset(ep_stage, 0, 4096);
    for (uint32_t i = 0; i < count && i < ENV_PAYLOAD_MAX; i++)
        ep_copy(ep_stage + i * sizeof(struct EnvPayloadDirEntry), &e[i],
                (uint32_t)sizeof(struct EnvPayloadDirEntry));
    if (nvme_write_sync(PERSIST_ENV_PAYLOAD_ENT_LBA, ep_stage) != 0) return -1;
    ep_memset(ep_stage, 0, 4096);
    ep_copy(ep_stage, &h, (uint32_t)sizeof(h));
    if (nvme_write_sync(PERSIST_ENV_PAYLOAD_HDR_LBA, ep_stage) != 0) return -1;
    return 0;
}

static int ep_dir_find(const struct EnvPayloadDirEntry* e, uint32_t n,
                       uint32_t partition, uint32_t index, uint32_t* out_i) {
    for (uint32_t i = 0; i < n; i++) {
        if ((e[i].flags & ENV_PAYLOAD_F_VALID) &&
            e[i].partition_id == partition && e[i].index == index) {
            if (out_i) *out_i = i;
            return 1;
        }
    }
    return 0;
}

int env_payload_present(uint32_t partition, uint32_t index) {
    struct EnvPayloadDirHead h;
    struct EnvPayloadDirEntry e[ENV_PAYLOAD_MAX];
    int n = ep_dir_read(&h, e);
    if (n <= 0) return 0;
    return ep_dir_find(e, (uint32_t)n, partition, index, 0);
}

/* ─── The page-table walk (identity map: a phys frame is a plain pointer) ── */
/* The permissions a leaf entry grants, in the blob's EP_PAGE_PERM_* bits.
 * Reads the PRESENT entry's own W/U/NX bits, so the value is what the map
 * GRANTED, not what a caller hoped: an ELF segment maps W from p_flags and
 * NX from !PF_X (cap_arch_map_page), a stack/heap page is W|NX
 * (user_map_page). The USER bit is carried too — the walk below refuses a
 * supervisor leaf, and recording it means a restore compares the bit that
 * actually let Ring-3 reach the page. */
static uint32_t ep_perm_of(uint64_t e) {
    uint32_t p = 0;
    if (e & (1ull << 1))  p |= EP_PAGE_PERM_WRITE;
    if (!(e & (1ull << 63))) p |= EP_PAGE_PERM_EXEC;
    if (e & (1ull << 2))  p |= EP_PAGE_PERM_USER;
    return p;
}

/* Resolve vaddr -> phys in `cr3`'s address space. Handles 4 KiB PTEs, 2 MiB
 * PDEs and 1 GiB PDPTEs (cap.c's shared-huge-page paths). The USER bit is
 * required at the leaf, the way the map is built; upper levels are walked on
 * PRESENT alone. Returns 1 on a present, user page; `out_perm` (optional)
 * receives the leaf's EP_PAGE_PERM_* bits — the page index's fourth word. */
static int ep_resolve_ex(uint64_t cr3, uint64_t vaddr, uint64_t* out_phys,
                         uint32_t* out_perm) {
    if (!cr3) return 0;
    uint64_t* pml4 = (uint64_t*)(uintptr_t)cr3;
    uint64_t i4 = (vaddr >> 39) & 0x1ffu, i3 = (vaddr >> 30) & 0x1ffu;
    uint64_t i2 = (vaddr >> 21) & 0x1ffu, i1 = (vaddr >> 12) & 0x1ffu;
    if (!(pml4[i4] & 1ull)) return 0;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[i4] & 0x000ffffffffff000ull);
    if (!(pdpt[i3] & 1ull)) return 0;
    if (pdpt[i3] & (1ull << 7)) {
        if (!(pdpt[i3] & (1ull << 2))) return 0;
        *out_phys = (pdpt[i3] & 0x000fffffc0000000ull) + (vaddr & 0x3fffffffull);
        if (out_perm) *out_perm = ep_perm_of(pdpt[i3]);
        return 1;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    if (!(pd[i2] & 1ull)) return 0;
    if (pd[i2] & (1ull << 7)) {
        if (!(pd[i2] & (1ull << 2))) return 0;
        *out_phys = (pd[i2] & 0x000fffffffe00000ull) + (vaddr & 0x1fffffull);
        if (out_perm) *out_perm = ep_perm_of(pd[i2]);
        return 1;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    if (!(pt[i1] & 1ull) || !(pt[i1] & (1ull << 2))) return 0;
    *out_phys = (pt[i1] & 0x000ffffffffff000ull) + (vaddr & 0xfffull);
    if (out_perm) *out_perm = ep_perm_of(pt[i1]);
    return 1;
}

static int ep_resolve(uint64_t cr3, uint64_t vaddr, uint64_t* out_phys) {
    return ep_resolve_ex(cr3, vaddr, out_phys, (uint32_t*)0);
}

/* Is vaddr inside one of the record's private regions (the identity-mapped
 * ones, where captured interior pointers make the ADDRESS load-bearing)? */
static int ep_in_regions(const struct EnvCkptRecord* rec, uint64_t vaddr) {
    for (uint32_t i = 0; i < rec->n_regions && i < ENV_CKPT_MAX_REGIONS; i++) {
        uint64_t base = rec->regions[i].base;
        uint64_t bytes = (uint64_t)rec->regions[i].frames * 4096ull;
        if (base && vaddr >= base && vaddr < base + bytes) return 1;
    }
    return 0;
}

static int ep_in_image_window(uint64_t vaddr) {
    return vaddr >= EP_IMAGE_VBASE && vaddr < EP_IMAGE_VBASE + EP_IMAGE_WINDOW;
}

/* E7 increment 20: is `vaddr` one of the Linux task's OWN pages?
 *
 * NOT the image window for a Linux child: its page table is CLONED from the
 * POSIX sidecar that spawned it, so the window contains the PARENT's image,
 * guard and stack pages too — and the child's own stack is mapped over some
 * of them. Capturing the window would file the parent's bytes (and stale
 * duplicates of them) under the child. The child's pages are exactly the
 * three ranges the create itself decided:
 *
 *   - the linked ET_EXEC image's page span (`linux_image_lo..hi`, stamped
 *     from the mapper's own PT_LOAD extent — a static musl image lives at
 *     0x400000, not in the window at all),
 *   - the user stack the create mapped for it (`stack_bottom..top`, the
 *     record's own bounds — the child's, not the parent's),
 *   - the budget span `linux_heap_base..limit`, which holds BOTH brk growth
 *     (up from the base) and anonymous maps (down from the limit).
 *
 * A PROT_NONE guard has no frame, so the walk never sees it: the payload
 * carries the pages that exist, not the ranges that were reserved. */
static int ep_linux_page(const struct EnvCkptRecord* rec, uint32_t t,
                         const struct ProcessDescriptor* pd, uint64_t vaddr) {
    const struct EnvCkptTask* tk = &rec->tasks[t];
    if (tk->stack_bottom && vaddr >= tk->stack_bottom &&
        vaddr < tk->stack_top) return 1;
    if (pd->linux_image_lo && vaddr >= pd->linux_image_lo &&
        vaddr < pd->linux_image_hi) return 1;
    if (pd->linux_heap_base && vaddr >= pd->linux_heap_base &&
        vaddr < pd->linux_heap_limit) return 1;
    return 0;
}

static int ep_task_page(const struct EnvCkptRecord* rec, uint32_t t,
                        const struct ProcessDescriptor* pd, uint64_t vaddr) {
    if (rec->tasks[t].kind == ENV_CKPT_TASK_LINUX)
        return ep_linux_page(rec, t, pd, vaddr);
    return ep_in_image_window(vaddr) || ep_in_regions(rec, vaddr);
}

static int ep_proc_parked(const struct ProcessDescriptor* pd) {
    return pd->active &&
           (pd->waiting_chan != CAP_NONE || pd->waiting_nchans > 0);
}

static struct ProcessDescriptor* ep_find_proc(const char* name) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!proc_table[i].active) continue;
        const char* a = proc_table[i].name;
        const char* b = name;
        int match = 1;
        while (*a && *b) { if (*a != *b) { match = 0; break; } a++; b++; }
        if (match && *a == '\0' && *b == '\0') return &proc_table[i];
    }
    return 0;
}

/* ─── The image-window list, shared with the migrate path (P3) ────────────
 * The canonical order both restore paths derive independently: the record's
 * sidecar tasks in record order (E7's Linux tasks skipped, exactly as the
 * capture skips them), ascending vaddr within each task's window. The
 * walk shape is capture_all's own (2 MiB pages walked inline, 1 GiB
 * skipped — nothing user maps one here), window predicate only: the region
 * pages are positional in the migrate stream and are not this list's
 * business. */
uint32_t env_payload_image_window_list(const struct EnvCkptRecord* rec,
                                       uint64_t* vaddrs,
                                       struct ProcessDescriptor** pds,
                                       uint32_t cap, uint32_t* err) {
    if (err) *err = 0;
    if (!rec || !vaddrs || !pds) { if (err) *err = 1; return 0; }
    uint32_t n = 0, resolved = 0, named = 0;
    for (uint32_t t = 0; t < rec->n_tasks && t < ENV_CKPT_MAX_TASKS; t++) {
        if (rec->tasks[t].kind == ENV_CKPT_TASK_LINUX) continue; /* E7's */
        named++;
        struct ProcessDescriptor* pd = ep_find_proc(rec->tasks[t].name);
        if (!pd || !pd->cr3) continue;
        resolved++;
        uint64_t cr3 = pd->cr3;
        uint64_t* pml4 = (uint64_t*)(uintptr_t)cr3;
        int overflow = 0;
        for (uint64_t i4 = 0; i4 < 512 && !overflow; i4++) {
            if (!(pml4[i4] & 1ull)) continue;
            uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[i4] & 0x000ffffffffff000ull);
            for (uint64_t i3 = 0; i3 < 512 && !overflow; i3++) {
                if (!(pdpt[i3] & 1ull)) continue;
                if (pdpt[i3] & (1ull << 7)) continue;   /* 1 GiB: nothing user maps it here */
                /* Named pdir, NOT pd: capture_all's walk spells its page
                 * directory `pd`, but here `pd` is the owning PROCESS (it is
                 * what pds[] stores), and shadowing it would file every page
                 * under the page-directory word instead of the sidecar. */
                uint64_t* pdir = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
                for (uint64_t i2 = 0; i2 < 512 && !overflow; i2++) {
                    if (!(pdir[i2] & 1ull)) continue;
                    uint64_t vbase2m = (i4 << 39) | (i3 << 30) | (i2 << 21);
                    if (pdir[i2] & (1ull << 7)) {
                        if (!(pdir[i2] & (1ull << 2))) continue;
                        for (uint64_t off = 0; off < 0x200000ull; off += 4096ull) {
                            uint64_t v = vbase2m + off;
                            if (!ep_in_image_window(v)) continue;
                            if (n >= cap) { if (err) *err = 2; return 0; }
                            vaddrs[n] = v; pds[n] = pd; n++;
                        }
                        continue;
                    }
                    uint64_t* pt = (uint64_t*)(uintptr_t)(pdir[i2] & 0x000ffffffffff000ull);
                    for (uint64_t i1 = 0; i1 < 512; i1++) {
                        if (!(pt[i1] & 1ull) || !(pt[i1] & (1ull << 2))) continue;
                        uint64_t v = vbase2m | (i1 << 12);
                        if (!ep_in_image_window(v)) continue;
                        if (n >= cap) { if (err) *err = 2; return 0; }
                        vaddrs[n] = v; pds[n] = pd; n++;
                    }
                }
            }
        }
    }
    /* SOME-but-not-all is the half-environment a pour must never resume;
     * NONE-resolving is the degenerate "nothing live here" the migrate
     * path answers with an empty tail (a host test's world, and the same
     * answer the record-only stages below give). */
    if (resolved > 0 && resolved < named) { if (err) *err = 1; return 0; }
    return n;
}

int env_payload_proc_parked(const struct ProcessDescriptor* pd) {
    return ep_proc_parked(pd);
}

int env_payload_resolve_vaddr(uint64_t cr3, uint64_t vaddr,
                              uint64_t* out_phys) {
    if (!out_phys) return 0;
    return ep_resolve(cr3, vaddr, out_phys);
}

/* The index-zone size the restore must reproduce exactly (it places the
 * data zone): regions + tasks + per-task blocks + console. */
static uint32_t ep_index_bytes(uint32_t n_tasks, const uint32_t* task_npages,
                               uint32_t console_len) {
    uint32_t n = 3u * (uint32_t)sizeof(struct EnvPayloadRegion);
    n += n_tasks * (uint32_t)sizeof(struct EnvPayloadTask);
    for (uint32_t t = 0; t < n_tasks; t++)
        n += 4u + task_npages[t] * EP_PAGE_ENTRY_BYTES;
    n += console_len;
    return n;
}

/* ─── capture ─────────────────────────────────────────────────────────────── */
uint32_t env_payload_capture_all(void) {
    if (!ep_io_ready()) {
        kernel_serial_printf(
            "[ENV_PAYLOAD] capture skipped: no NVMe queue this boot "
            "(records still land; a replay will come up empty by name)\n");
        return 0;
    }

    /* The previous directory, for slot reuse: a re-captured identity keeps
     * its slot, so the band does not walk forward on every checkpoint. */
    struct EnvPayloadDirHead oldh;
    struct EnvPayloadDirEntry olde[ENV_PAYLOAD_MAX];
    int oldn = ep_dir_read(&oldh, olde);

    struct EnvPayloadDirEntry ents[ENV_PAYLOAD_MAX];
    ep_memset(ents, 0, (uint32_t)sizeof(ents));
    uint32_t n_ents = 0;
    uint32_t captured = 0;

    for (uint32_t ri = 0; ri < ENV_CKPT_MAX; ri++) {
        const struct EnvCkptRecord* rec = &env_ckpt_table[ri];
        if (rec->magic != ENV_CKPT_REC_MAGIC) continue;

        /* Every named task must resolve: a payload missing one of them
         * would restore a half-environment whose other half's heap the
         * first half still points into — and E7 increment 20 makes the
         * Linux tenant one of the named tasks, so a checkpoint taken
         * before the sidecar has spawned it skips the environment rather
         * than write a record whose third task has no bytes to come back
         * with (the stamp has not landed, so the task does not exist).
         * Increment 21's adopt path will make a not-yet-spawned tenant
         * restorable; until it does, the named refusal is the honest
         * answer and the capture's log says which task was missing. */
        const struct ProcessDescriptor* pds[ENV_CKPT_MAX_TASKS];
        uint32_t np = 0;
        int missing = 0;
        const char* missing_name = "";
        for (uint32_t t = 0; t < rec->n_tasks && t < ENV_CKPT_MAX_TASKS; t++) {
            struct ProcessDescriptor* pd = ep_find_proc(rec->tasks[t].name);
            if (!pd) { missing = 1; missing_name = rec->tasks[t].name; break; }
            pds[np++] = pd;
        }
        if (missing || np == 0) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture skipped for partition=%u index=%u: "
                "%s '%s'\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                missing ? "named task has no live process" : "no tasks",
                missing ? missing_name : "");
            continue;
        }

        /* Collect the pages: image/stack window + private regions, per
         * task. Shared-arena and lazily-granted frames are deliberately
         * NOT captured (env_payload.h rule 3). */
        uint32_t n_pages = 0;
        int overflow = 0;
        uint32_t task_page_base[ENV_CKPT_MAX_TASKS];
        uint32_t task_npages[ENV_CKPT_MAX_TASKS];
        uint8_t  task_form[ENV_CKPT_MAX_TASKS];
        for (uint32_t t = 0; t < np; t++) {
            task_page_base[t] = n_pages;
            task_npages[t] = 0;
            uint64_t cr3 = pds[t]->cr3;
            if (!cr3) continue;
            /* The predicate is per TASK (ep_task_page): a sidecar's page is
             * its window or a private region; a Linux task's is one of its
             * own three ranges. The walk itself is the same. */
            uint64_t* pml4 = (uint64_t*)(uintptr_t)cr3;
            for (uint64_t i4 = 0; i4 < 512 && !overflow; i4++) {
                if (!(pml4[i4] & 1ull)) continue;
                uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[i4] & 0x000ffffffffff000ull);
                for (uint64_t i3 = 0; i3 < 512 && !overflow; i3++) {
                    if (!(pdpt[i3] & 1ull)) continue;
                    if (pdpt[i3] & (1ull << 7)) continue;   /* 1 GiB: nothing user maps it here */
                    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
                    for (uint64_t i2 = 0; i2 < 512 && !overflow; i2++) {
                        if (!(pd[i2] & 1ull)) continue;
                        uint64_t vbase2m = (i4 << 39) | (i3 << 30) | (i2 << 21);
                        if (pd[i2] & (1ull << 7)) {
                            if (!(pd[i2] & (1ull << 2))) continue;
                            for (uint64_t off = 0; off < 0x200000ull; off += 4096ull) {
                                uint64_t v = vbase2m + off;
                                if (!ep_task_page(rec, t, pds[t], v)) continue;
                                if (n_pages >= EP_MAX_PAGES) { overflow = 1; break; }
                                ep_scratch.vaddr[n_pages] = v;
                                ep_tnum[n_pages] = (uint8_t)t;
                                task_npages[t]++;
                                n_pages++;
                            }
                            continue;
                        }
                        uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
                        for (uint64_t i1 = 0; i1 < 512; i1++) {
                            if (!(pt[i1] & 1ull) || !(pt[i1] & (1ull << 2))) continue;
                            uint64_t v = vbase2m | (i1 << 12);
                            if (!ep_task_page(rec, t, pds[t], v)) continue;
                            if (n_pages >= EP_MAX_PAGES) { overflow = 1; break; }
                            ep_scratch.vaddr[n_pages] = v;
                            ep_tnum[n_pages] = (uint8_t)t;
                            task_npages[t]++;
                            n_pages++;
                        }
                    }
                }
            }
        }
        if (overflow || n_pages == 0) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture refused for partition=%u index=%u: "
                "%s (%u pages)\n", (unsigned)rec->partition_id,
                (unsigned)rec->index,
                overflow ? "more pages than the blob ceiling" : "nothing mapped",
                (unsigned)n_pages);
            continue;
        }

        /* Console snapshot: the not-yet-drained output, non-destructively. */
        uint32_t console_len = 0;
        if (env_console_snapshot(rec->partition_id, rec->console_id,
                                 ep_console, EP_CONSOLE_MAX, &console_len) != 1)
            console_len = 0;

        uint32_t index_bytes = ep_index_bytes(np, task_npages, console_len);
        if (index_bytes > EP_STAGE_BYTES) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture refused for partition=%u index=%u: "
                "index zone %u exceeds the %u-byte ceiling\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned)index_bytes, (unsigned)EP_STAGE_BYTES);
            continue;
        }
        uint32_t rest_frames = (index_bytes + 4095u) / 4096u;
        uint32_t data_off_frames = 1u + rest_frames;
        uint64_t sectors = (uint64_t)data_off_frames * 8ull +
                           (uint64_t)n_pages * 8ull;
        if (sectors > ENV_PAYLOAD_SLOT_SECTORS) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture refused for partition=%u index=%u: "
                "%llu sectors does not fit a %llu-sector slot\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned long long)sectors,
                (unsigned long long)ENV_PAYLOAD_SLOT_SECTORS);
            continue;
        }

        /* Slot: keep the previous one for this identity, else first free
         * among the entries written so far. */
        uint32_t slot = ENV_PAYLOAD_MAX;
        if (oldn > 0) {
            uint32_t prev = 0;
            if (ep_dir_find(olde, (uint32_t)oldn, rec->partition_id,
                            rec->index, &prev))
                slot = olde[prev].slot;
        }
        if (slot >= ENV_PAYLOAD_MAX) {
            for (uint32_t s = 0; s < ENV_PAYLOAD_MAX; s++) {
                int used = 0;
                for (uint32_t k = 0; k < n_ents; k++)
                    if ((ents[k].flags & ENV_PAYLOAD_F_VALID) &&
                        ents[k].slot == s) used = 1;
                if (!used) { slot = s; break; }
            }
        }
        if (slot >= ENV_PAYLOAD_MAX) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture refused for partition=%u index=%u: "
                "all %u payload slots are in use\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned)ENV_PAYLOAD_MAX);
            continue;
        }
        uint64_t slot_lba = ENV_PAYLOAD_SLOT_LBA(slot);

        /* Data zone: copy each frozen source frame, crc it, write it. The
         * sources cannot move — this runs inside the quiesce. */
        int io_fail = 0;
        for (uint32_t pi = 0; pi < n_pages && !io_fail; pi++) {
            uint64_t v = ep_scratch.vaddr[pi];
            uint64_t phys = 0;
            uint32_t perm = 0;
            if (ep_in_regions(rec, v)) {
                phys = v;                       /* identity: the address IS the frame */
                /* ...but the PTE is still where the permissions live: a region
                 * page is mapped W in the sidecar's own table, and reading the
                 * grant from the map (not assuming it) is the rule. */
                if (!ep_resolve_ex(pds[ep_tnum[pi]]->cr3, v, &phys, &perm)) {
                    io_fail = 1;
                    break;
                }
            } else if (!ep_resolve_ex(pds[ep_tnum[pi]]->cr3, v, &phys, &perm)) {
                io_fail = 1;                    /* the walk said present; treat as I/O-class failure */
                break;
            }
            ep_perms[pi] = perm;
            ep_memset(ep_page, 0, 4096);
            ep_copy(ep_page, (const void*)(uintptr_t)phys, 4096);
            ep_crcs[pi] = ep_crc(0, ep_page, 4096);
            if (nvme_write_sync(slot_lba + (uint64_t)data_off_frames * 8ull +
                                (uint64_t)pi * 8ull, ep_page) != 0)
                io_fail = 1;
        }
        if (io_fail) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture I/O failed for partition=%u index=%u "
                "(a source page vanished or the device refused a write)\n",
                (unsigned)rec->partition_id, (unsigned)rec->index);
            continue;
        }

        /* Index zone: regions, tasks, per-task page blocks, console. */
        uint32_t used = 0;
        ep_memset(ep_stage, 0, EP_STAGE_BYTES);
        for (uint32_t i = 0; i < rec->n_regions && i < ENV_CKPT_MAX_REGIONS; i++) {
            struct EnvPayloadRegion r;
            r.base = rec->regions[i].base;
            r.frames = rec->regions[i].frames;
            r.kind = rec->regions[i].kind;
            ep_copy(ep_stage + used, &r, (uint32_t)sizeof(r));
            used += (uint32_t)sizeof(r);
        }
        for (uint32_t t = 0; t < np; t++) {
            struct EnvPayloadTask tk;
            ep_memset(&tk, 0, (uint32_t)sizeof(tk));
            ep_copy(tk.name, rec->tasks[t].name, ENV_CKPT_NAME_LEN);
            tk.kind = rec->tasks[t].kind;
            tk.n_pages = task_npages[t];
            const struct ProcessDescriptor* pd = pds[t];
            if (tk.kind == ENV_CKPT_TASK_LINUX) {
                /* The Linux task's form is the one this capture can honestly
                 * resume from. A task parked in a forwarded syscall has a
                 * REQUEST queued in a kernel channel — state this blob does
                 * not carry (B9), so it is recorded as PARKED by name and
                 * the restore refuses it. A task stopped in user space
                 * carries its whole state in ring3_ctx (the timer ISR saves
                 * all 20 words, including the six Linux argument regs), so
                 * that is the restorable FORM_USER. Neither is a guess: the
                 * posture is read off the descriptor the quiesce froze. */
                if (pd->gate_stopped) {
                    /* E7 increment 21: stopped AT the copy continuation (the
                     * fourth read's `syscall`, not yet executed). No request
                     * is in flight, so this is the ONE parked-looking
                     * posture whose whole state is the frame: ring3_ctx was
                     * built at the stop as the sysret would install it, with
                     * rip re-pointed at the instruction. Re-running that trap
                     * after the restore is the continuation. */
                    tk.form = ENV_PAYLOAD_FORM_ENTRY;
                    const uint64_t* c = (const uint64_t*)&pd->ring3_ctx;
                    for (uint32_t r = 0; r < 20; r++) tk.linux_regs[r] = c[r];
                    tk.linux_args[0] = c[9];
                    tk.linux_args[1] = c[10];
                    tk.linux_args[2] = c[11];
                    tk.linux_args[3] = c[5];
                    tk.linux_args[4] = c[7];
                    tk.linux_args[5] = c[6];
                    tk.user_rip = pd->user_rip;
                    tk.user_rsp = pd->user_rsp;
                } else if (ep_proc_parked(pd) &&
                    pd->park_syscall == SYS_LINUX_TRAP_RESUME) {
                    tk.form = ENV_PAYLOAD_FORM_PARKED;
                    tk.park_syscall = pd->park_syscall;
                    tk.park[0] = pd->park_ctx.r11;
                    tk.park[1] = pd->park_ctx.rcx;
                    tk.park[2] = pd->park_ctx.r15;
                    tk.park[3] = pd->park_ctx.r14;
                    tk.park[4] = pd->park_ctx.r13;
                    tk.park[5] = pd->park_ctx.r12;
                    tk.park[6] = pd->park_ctx.rbx;
                    tk.park[7] = pd->park_ctx.rbp;
                    tk.park[8] = pd->park_ctx.user_rsp;
                    tk.user_rip = pd->user_rip;
                    tk.user_rsp = pd->user_rsp;
                    tk.waiting_nchans = pd->waiting_nchans;
                    for (uint32_t a = 0; a < 6; a++)
                        tk.linux_args[a] = pd->park_ctx.linux_args[a];
                } else if (ep_proc_parked(pd)) {
                    /* Parked in something that is not the forward: a shape
                     * a Linux task cannot legitimately be in, and one whose
                     * ring3_ctx is NOT this instant's state. Captured as the
                     * mid-compute posture it effectively is — a named
                     * refusal on restore, never a silent wrong context. */
                    tk.form = ENV_PAYLOAD_FORM_RUNNING;
                } else if (pd->has_ring3_ctx) {
                    const uint64_t* c = (const uint64_t*)&pd->ring3_ctx;
                    tk.form = ENV_PAYLOAD_FORM_USER;
                    for (uint32_t r = 0; r < 20; r++) tk.linux_regs[r] = c[r];
                    /* TaskContext's order (arch/x86/scheduler.h): 9 rdi,
                     * 10 rsi, 11 rdx, 5 r10, 7 r8, 6 r9 — Linux's own
                     * argument order, read from the LIVE context. */
                    tk.linux_args[0] = c[9];
                    tk.linux_args[1] = c[10];
                    tk.linux_args[2] = c[11];
                    tk.linux_args[3] = c[5];
                    tk.linux_args[4] = c[7];
                    tk.linux_args[5] = c[6];
                    tk.user_rip = c[15];   /* rip */
                    tk.user_rsp = c[18];   /* rsp */
                } else {
                    tk.form = ENV_PAYLOAD_FORM_RUNNING;
                }
                tk.fs_base     = pd->fs_base;
                tk.brk_cur     = pd->linux_brk_cur;
                tk.mmap_top    = pd->linux_mmap_top;
                tk.heap_base   = pd->linux_heap_base;
                tk.heap_limit  = pd->linux_heap_limit;
                tk.stack_bottom = rec->tasks[t].stack_bottom;
                tk.stack_top    = rec->tasks[t].stack_top;
                tk.image_lo    = pd->linux_image_lo;
                tk.image_hi    = pd->linux_image_hi;
            } else if (ep_proc_parked(pd)) {
                tk.form = ENV_PAYLOAD_FORM_PARKED;
                tk.park_syscall = pd->park_syscall;
                tk.park[0] = pd->park_ctx.r11;
                tk.park[1] = pd->park_ctx.rcx;
                tk.park[2] = pd->park_ctx.r15;
                tk.park[3] = pd->park_ctx.r14;
                tk.park[4] = pd->park_ctx.r13;
                tk.park[5] = pd->park_ctx.r12;
                tk.park[6] = pd->park_ctx.rbx;
                tk.park[7] = pd->park_ctx.rbp;
                tk.park[8] = pd->park_ctx.user_rsp;
                tk.user_rip = pd->user_rip;
                tk.user_rsp = pd->user_rsp;
                tk.waiting_nchans = pd->waiting_nchans;
            } else {
                tk.form = ENV_PAYLOAD_FORM_RUNNING;
            }
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture: task %u '%s' kind=%u form=%u "
                "park_syscall=%u nchans=%u user_rip=0x%llx user_rsp=0x%llx "
                "fs_base=0x%llx\n",
                (unsigned)t, tk.name, (unsigned)tk.kind, (unsigned)tk.form,
                (unsigned)tk.park_syscall, (unsigned)tk.waiting_nchans,
                (unsigned long long)tk.user_rip,
                (unsigned long long)tk.user_rsp,
                (unsigned long long)tk.fs_base);
            task_form[t] = tk.form;
            ep_copy(ep_stage + used, &tk, (uint32_t)sizeof(tk));
            used += (uint32_t)sizeof(tk);
        }
        for (uint32_t t = 0; t < np; t++) {
            uint32_t n = task_npages[t];
            ep_stage[used + 0] = (uint8_t)(n & 0xff);
            ep_stage[used + 1] = (uint8_t)((n >> 8) & 0xff);
            ep_stage[used + 2] = (uint8_t)((n >> 16) & 0xff);
            ep_stage[used + 3] = (uint8_t)((n >> 24) & 0xff);
            used += 4u;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t pi = task_page_base[t] + k;
                uint64_t v = ep_scratch.vaddr[pi];
                ep_copy(ep_stage + used, &v, 8);
                used += 8;
                ep_copy(ep_stage + used, &ep_crcs[pi], 4);
                used += 4;
                ep_copy(ep_stage + used, &ep_perms[pi], 4);
                used += 4;
            }
        }
        ep_copy(ep_stage + used, ep_console, console_len);
        used += console_len;
        uint32_t crc_index = ep_crc(0, ep_stage, used);

        /* Frames 1.. of the index zone. */
        for (uint32_t f = 0; f < rest_frames; f++) {
            uint32_t off = f * 4096u;
            uint32_t take = used - off;
            if (take > 4096u) take = 4096u;
            ep_memset(ep_page, 0, 4096);
            ep_copy(ep_page, ep_stage + off, take);
            if (nvme_write_sync(slot_lba + 8ull + (uint64_t)f * 8ull,
                                ep_page) != 0) { io_fail = 1; break; }
        }
        if (io_fail) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture I/O failed staging the index for "
                "partition=%u index=%u\n",
                (unsigned)rec->partition_id, (unsigned)rec->index);
            continue;
        }

        /* Frame 0 LAST: the head carries a crc over bytes now on media. */
        struct EnvPayloadHead head;
        ep_memset(&head, 0, (uint32_t)sizeof(head));
        head.magic = ENV_PAYLOAD_BLOB_MAGIC;
        head.version = ENV_PAYLOAD_REC_VERSION;
        head.bytes = (1u + rest_frames + n_pages) * 4096u;
        head.partition_id = rec->partition_id;
        head.index = rec->index;
        head.seq_low = (uint32_t)(rec->sequence & 0xffffffffu);
        head.seq_high = (uint32_t)(rec->sequence >> 32);
        head.n_tasks = np;
        head.n_pages = n_pages;
        head.console_len = console_len;
        head.crc32 = crc_index;
        ep_memset(ep_page, 0, 4096);
        ep_copy(ep_page, &head, (uint32_t)sizeof(head));
        if (nvme_write_sync(slot_lba, ep_page) != 0) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture I/O failed writing the head for "
                "partition=%u index=%u\n",
                (unsigned)rec->partition_id, (unsigned)rec->index);
            continue;
        }

        /* The directory entry for this identity (upsert). */
        uint32_t slot_i = ENV_PAYLOAD_MAX;
        for (uint32_t k = 0; k < n_ents; k++)
            if (ents[k].partition_id == rec->partition_id &&
                ents[k].index == rec->index) slot_i = k;
        if (slot_i >= ENV_PAYLOAD_MAX) {
            if (n_ents >= ENV_PAYLOAD_MAX) continue;
            slot_i = n_ents++;
        }
        ents[slot_i].partition_id = rec->partition_id;
        ents[slot_i].index = rec->index;
        ents[slot_i].slot = slot;
        ents[slot_i].flags = ENV_PAYLOAD_F_VALID;
        ents[slot_i].seq = rec->sequence;
        ents[slot_i].sectors = sectors;
        captured++;

        /* The Linux task's own line: the acceptance for increment 20 reads
         * the page count and the permission kinds off the capture, so they
         * are reported before the pour can be argued about later. Printed
         * only when the record names one (the sidecar-only path's log is
         * byte-for-byte what it was). */
        for (uint32_t t = 0; t < np; t++) {
            if (rec->tasks[t].kind != ENV_CKPT_TASK_LINUX) continue;
            uint32_t n_rx = 0, n_rw = 0, n_other = 0;
            for (uint32_t k = 0; k < task_npages[t]; k++) {
                uint32_t pm = ep_perms[task_page_base[t] + k] &
                              EP_PAGE_PERM_MASK;
                if (pm == (EP_PAGE_PERM_USER | EP_PAGE_PERM_EXEC)) n_rx++;
                else if (pm == (EP_PAGE_PERM_USER | EP_PAGE_PERM_WRITE)) n_rw++;
                else n_other++;
            }
            kernel_serial_printf(
                "[ENV_PAYLOAD] linux task '%s' pages=%u rx=%u rw=%u other=%u "
                "form=%u image=0x%llx..0x%llx heap=0x%llx..0x%llx\n",
                rec->tasks[t].name, (unsigned)task_npages[t],
                (unsigned)n_rx, (unsigned)n_rw, (unsigned)n_other,
                (unsigned)task_form[t],
                (unsigned long long)pds[t]->linux_image_lo,
                (unsigned long long)pds[t]->linux_image_hi,
                (unsigned long long)pds[t]->linux_heap_base,
                (unsigned long long)pds[t]->linux_heap_limit);
        }
        kernel_serial_printf(
            "[ENV_PAYLOAD] captured partition=%u index=%u tasks=%u pages=%u "
            "console=%u bytes=%u slot=%u seq=%u\n",
            (unsigned)rec->partition_id, (unsigned)rec->index,
            (unsigned)np, (unsigned)n_pages, (unsigned)console_len,
            (unsigned)head.bytes, (unsigned)slot,
            (unsigned)rec->sequence);

        /* P3 destination (a), increment 2: the SAME instant this record's
         * payload lands on the local band, its record and pages go to every
         * registered peer — inside the quiesce that owns the freeze this
         * push's posture gates demand (records QUIESCED, sidecars frozen
         * PARKED, both verified by env_ckpt_quiesce_for_capture above).
         * Best-effort by contract, exactly like the payload I/O above: the
         * push logs every non-confirmation by name and returns, never fails
         * the local capture and never keeps a tenant frozen — a peer
         * without a copy is destination (b)'s ladder to report on THAT
         * node, not a reason to hold the capture open. */
        (void)env_replica_push(rec);
    }

    if (ep_dir_write(ents, n_ents) != 0) {
        kernel_serial_printf(
            "[ENV_PAYLOAD] directory write failed — no payload is "
            "discoverable at restore (replays will come up empty by name)\n");
        return 0;
    }
    return captured;
}

/* ─── the bounded wait (the pass calls this where a yield is legal) ──────── */
// The bound is a STALL window, not a wall clock, and that is the whole
// hardening (live run 12, 2026-11-01): `drv.ramdisk.1` was RUNNING -- being
// scheduled, serving the replay's own boot I/O -- when a fixed 30 s deadline
// expired, so the wait answered "not parked", the pour refused by name, and a
// healthy environment was reported as one whose contents could not come back.
// A sidecar that is still being scheduled is still working; only one whose
// tasks have STOPPED being scheduled is out of time. So the window restarts on
// every observed bit of progress (a switch-in on any of this record's sidecar
// tasks, or the [env-id] announcement landing), and EP_WAIT_CEIL_TICKS is the
// absolute stop that keeps a genuinely wedged environment from holding the
// restore open forever. The ceiling stays well under failover's migration
// grace (env_migrate.h: ENV_MIGRATE_SLOT_GRACE_TICKS), because this wait runs
// inside a restore the sweep is already covering.
int env_payload_wait_parked(const struct EnvCkptRecord* rec) {
    if (!rec) return 0;
    uint64_t start          = kernel_tick_counter;
    uint64_t stall_deadline = kernel_tick_counter + EP_WAIT_TICKS;
    uint64_t ceiling        = kernel_tick_counter + EP_WAIT_CEIL_TICKS;
    uint64_t last_progress  = kernel_tick_counter;
    int first_pass = 1;   /* the posture line: once, not once per pump */
    /* One line, the first time this wait passes the point where the OLD fixed
     * deadline would have answered -- not once per window, or a long boot
     * would flood the transcript with its own progress. */
    int outlived_logged = 0;
    uint32_t ready_passes = 0;
    /* The previous pass's progress ledger: each sidecar's switch-in count and
     * the announcement. Both are monotone, so "changed" is the only question
     * ever asked of them. */
    uint32_t last_switch[ENV_CKPT_MAX_TASKS];
    for (uint32_t t = 0; t < ENV_CKPT_MAX_TASKS; t++) last_switch[t] = 0;
    int last_announced = 0;
    for (;;) {
        int progressed = 0;
        /* Parked AND announced — parked alone is not the pour's go signal.
         * A freshly replayed sidecar blocks mid-boot (the ramdisk RD_INFO
         * handshake) in a posture this predicate cannot tell from the
         * captured idle park, and pouring there replaces its half-finished
         * boot with the captured state — after which boot.rs's [env-id] line
         * can never be written by anyone, and the durable guard's post-
         * reboot identity wait reads exactly that line out of this
         * environment's console. The flag is set only by the drain of the
         * sidecar's own bytes and cleared at wiring, so it can only come
         * from THIS boot's announcement. The tasks loop runs first so a
         * missing sidecar is still answered at once, and at capture time
         * the flag has been set since boot (the record exists because the
         * environment ran), so the pre-freeze wait is unchanged in
         * practice. */
        int ready = 1;
        for (uint32_t t = 0; t < rec->n_tasks && t < ENV_CKPT_MAX_TASKS; t++) {
            if (rec->tasks[t].kind == ENV_CKPT_TASK_LINUX) {
                if (first_pass) kernel_serial_printf(
                    "[ENV_PAYLOAD] wait: task %u '%s' kind=%u (linux, skipped)\n",
                    (unsigned)t, rec->tasks[t].name,
                    (unsigned)rec->tasks[t].kind);
                continue;
            }
            struct ProcessDescriptor* pd = ep_find_proc(rec->tasks[t].name);
            if (first_pass) kernel_serial_printf(
                "[ENV_PAYLOAD] wait: task %u '%s' kind=%u live=%s parked=%s "
                "syscall=%u nchans=%u\n",
                (unsigned)t, rec->tasks[t].name,
                (unsigned)rec->tasks[t].kind, pd ? "yes" : "no",
                (pd && ep_proc_parked(pd)) ? "yes" : "no",
                (unsigned)(pd ? pd->park_syscall : 0u),
                (unsigned)(pd ? pd->waiting_nchans : 0u));
            if (!pd) {                           /* nothing to wait for */
                kernel_serial_printf(
                    "[ENV_PAYLOAD] wait: no live process named '%s' "
                    "(record task %u of %u, kind %u) -- there is nothing "
                    "to wait for\n",
                    rec->tasks[t].name, (unsigned)t, (unsigned)rec->n_tasks,
                    (unsigned)rec->tasks[t].kind);
                return 0;
            }
            /* Progress, sampled for EVERY task before any early exit from the
             * readiness test: a task skipped by `break` would leave a stale
             * count behind and read as progress on the next pass, which is the
             * kind of false signal a stall detector must not have. */
            if (!first_pass && pd->switches != last_switch[t]) progressed = 1;
            last_switch[t] = pd->switches;
            if (!ep_proc_parked(pd)) ready = 0;
        }
        first_pass = 0;
        {
            int announced = env_console_identity_seen(rec->partition_id,
                                                      rec->index);
            if (announced != last_announced) progressed = 1;
            last_announced = announced;
            if (ready && !announced) ready = 0;
        }
        /* The settle window (EP_WAIT_SETTLE's comment): a posture that has
         * just become parked must survive the same pump-and-yield cadence the
         * replay's boot uses, or the pour — which checks once, in a straight
         * line — can still see the task that request woke. */
        if (ready) {
            if (++ready_passes >= EP_WAIT_SETTLE) return 1;
        } else {
            ready_passes = 0;
        }
        /* Progress restarts the window (clamped to the ceiling, which never
         * moves). Nothing about this makes a wrong capture look right: the
         * pour still checks the posture once, in its straight line, and still
         * refuses by name if it is not there. What it stops is the reverse —
         * a correct capture being called wrong because a slow boot was still
         * working when a clock ran out. */
        if (progressed) {
            last_progress = kernel_tick_counter;
            stall_deadline = kernel_tick_counter + EP_WAIT_TICKS;
            if (stall_deadline > ceiling) stall_deadline = ceiling;
        }
        /* The old fixed bound would have returned 0 right here. Say so once,
         * and only while the window is still OPEN (kernel_tick_counter <
         * stall_deadline: progress restarted it), so the transcript separates
         * "extended because the environment was still working" from the
         * `stalled` line below, which is the opposite finding. Without this
         * line the drill's proof would be the absence of a refusal, which is
         * also what a wait that never ran looks like. */
        if (!outlived_logged && !ready &&
            kernel_tick_counter - start >= EP_WAIT_TICKS &&
            kernel_tick_counter < stall_deadline) {
            /* WHICH posture the old bound was about to call finished: the
             * number of this record's non-Linux tasks that are not parked
             * RIGHT NOW. "running" is the case run 12 measured (drv.ramdisk.1
             * serving the replay's boot) and the one the drill's assertion
             * pins; "parked but not announced" is a different unfinished
             * boot, and the line should not let the two be confused. */
            unsigned running = 0, n_side = 0;
            for (uint32_t t = 0;
                 t < rec->n_tasks && t < ENV_CKPT_MAX_TASKS; t++) {
                if (rec->tasks[t].kind == ENV_CKPT_TASK_LINUX) continue;
                n_side++;
                const struct ProcessDescriptor* pd =
                    ep_find_proc(rec->tasks[t].name);
                if (!pd || !ep_proc_parked(pd)) running++;
            }
            outlived_logged = 1;
            kernel_serial_printf(
                "[ENV_PAYLOAD] wait: outlived the fixed bound (partition=%u "
                "index=%u): %llu ticks elapsed, quiet %llu, announced=%d, "
                "running=%u/%u -- still being scheduled, so the window was "
                "extended, not expired\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned long long)(kernel_tick_counter - start),
                (unsigned long long)(kernel_tick_counter - last_progress),
                env_console_identity_seen(rec->partition_id, rec->index),
                running, n_side);
        }
        if (kernel_tick_counter >= ceiling) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] wait ended (partition=%u index=%u): ceiling — "
                "%llu ticks and still being scheduled (announced=%d, quiet for "
                "%llu); the posture never arrived, so the pour will refuse by "
                "name\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned long long)(kernel_tick_counter),
                env_console_identity_seen(rec->partition_id, rec->index),
                (unsigned long long)(kernel_tick_counter - last_progress));
            return 0;
        }
        if (kernel_tick_counter >= stall_deadline) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] wait ended (partition=%u index=%u): stalled — "
                "no task of this environment was scheduled for %llu ticks "
                "(announced=%d); the posture never arrived, so the pour will "
                "refuse by name\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                (unsigned long long)(kernel_tick_counter - last_progress),
                env_console_identity_seen(rec->partition_id, rec->index));
            return 0;
        }
        /* Keep the kernel's periodic service work ticking while we wait, then
         * hand the CPU to Ring-3 — the same pair env_service_rpc's wait loop
         * makes (env_service.c), for the same reason.
         *
         * On a uniprocessor boot (QEMU -smp 1: no AP answers the SIPI, and
         * that is the cluster's default) microkernel_service_poll() — and
         * with it env_console_tick(), the drain that reads the sidecar's own
         * bytes into the registry and sets the identity flag the check above
         * reads — runs ONLY from smp_uniprocessor_tick(), which nothing in
         * this loop otherwise calls. Without the call this wait starves its
         * own predicate: the fresh sidecar boots (its [POSIX] lines land in
         * the log), writes [env-id] into its console channel, and the bytes
         * sit undrained until the deadline. Caught live on the 2-node TCG
         * cluster: `[ENV_PAYLOAD] wait ended ... announced=0` followed by
         * the restore's REFUSE_PARKED, with the sidecar's boot complete in
         * the same log. A no-op once an AP is online: the AP's own loop polls
         * microkernel_service_poll() every tick. */
        smp_uniprocessor_tick();
        failover_sweep_keepalive();   /* the restore's parked wait blocks the sweep:
                                      * keep the heartbeat/checkpoint streams alive */
        kernel_yield_to_ring3(50);
    }
}

/* Walk the index zone in ep_stage once, reporting task page-block offsets
 * through the caller's array (used by the restore's later passes). Returns
 * 0 when the layout does not self-consistently fit. */
static int ep_index_blocks(uint32_t n_tasks, uint32_t n_pages,
                           uint32_t* out_block_off, uint32_t* out_console_off,
                           uint32_t* out_total) {
    uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion) +
                 n_tasks * (uint32_t)sizeof(struct EnvPayloadTask);
    if (u > EP_STAGE_BYTES) return 0;
    uint32_t seen = 0;
    for (uint32_t t = 0; t < n_tasks; t++) {
        if (u + 4u > EP_STAGE_BYTES) return 0;
        if (out_block_off) out_block_off[t] = u;
        uint32_t np = (uint32_t)ep_stage[u] |
                      ((uint32_t)ep_stage[u + 1] << 8) |
                      ((uint32_t)ep_stage[u + 2] << 16) |
                      ((uint32_t)ep_stage[u + 3] << 24);
        u += 4u;
        if (np > n_pages || u + np * EP_PAGE_ENTRY_BYTES > EP_STAGE_BYTES) return 0;
        u += np * EP_PAGE_ENTRY_BYTES;
        seen += np;
    }
    if (seen != n_pages) return 0;
    if (u + 0u > EP_STAGE_BYTES) return 0;
    if (out_console_off) *out_console_off = u;
    if (out_total) *out_total = u;
    return 1;
}

/* ─── restore ─────────────────────────────────────────────────────────────── */
int env_payload_restore(uint32_t partition, uint32_t index,
                        uint32_t new_env_id, uint64_t want_seq,
                        uint32_t* out_pages, uint32_t* out_console) {
    env_payload_clear_refusal();
    if (out_pages) *out_pages = 0;
    if (out_console) *out_console = 0;

    struct EnvPayloadDirHead h;
    struct EnvPayloadDirEntry e[ENV_PAYLOAD_MAX];
    int n = ep_dir_read(&h, e);
    if (n <= 0) { ep_refuse(EP_REFUSE_ABSENT, 0, 0); return EP_REFUSE_ABSENT; }
#if E7G_TOOTH
    ep_memset(ep_skip, 0, sizeof(ep_skip));
#endif
    uint32_t di = 0;
    if (!ep_dir_find(e, (uint32_t)n, partition, index, &di)) {
        ep_refuse(EP_REFUSE_ABSENT, 0, 0);
        return EP_REFUSE_ABSENT;
    }
    if (!ep_io_ready()) { ep_refuse(EP_REFUSE_IO, 0, 0); return EP_REFUSE_IO; }

    const struct EnvCkptRecord* cur = env_ckpt_find(partition, index);
    if (!cur) { ep_refuse(EP_REFUSE_NO_PROCESS, 0, 0); return EP_REFUSE_NO_PROCESS; }

    uint64_t slot_lba = ENV_PAYLOAD_SLOT_LBA(e[di].slot);

    /* Head. */
    ep_memset(ep_page, 0, 4096);
    if (nvme_read_sync(slot_lba, ep_page) != 0) {
        ep_refuse(EP_REFUSE_IO, 0, 0);
        return EP_REFUSE_IO;
    }
    struct EnvPayloadHead head;
    ep_copy(&head, ep_page, (uint32_t)sizeof(head));
    if (head.magic != ENV_PAYLOAD_BLOB_MAGIC ||
        head.version != ENV_PAYLOAD_REC_VERSION ||
        head.partition_id != partition || head.index != index) {
        ep_refuse(EP_REFUSE_FORMAT, 0, 0);
        return EP_REFUSE_FORMAT;
    }
    uint64_t seq = ((uint64_t)head.seq_high << 32) | head.seq_low;
    if (seq != want_seq) { ep_refuse(EP_REFUSE_SEQ, 0, 0); return EP_REFUSE_SEQ; }
    if (head.n_pages == 0 || head.n_pages > EP_MAX_PAGES ||
        head.n_tasks == 0 || head.n_tasks > EP_MAX_TASKS ||
        head.console_len > EP_CONSOLE_MAX) {
        ep_refuse(EP_REFUSE_FORMAT, 2, head.n_pages);
        return EP_REFUSE_FORMAT;
    }

    /* Index zone: read frames 1.. until the zone's own layout is covered.
     * The zone size is derived from the entries themselves, so the read is
     * bounded by the same ceiling the capture was. */
    uint32_t block_off[EP_MAX_TASKS];
    uint32_t console_off = 0, total = 0;
    /* First frame always; the rest follow the layout walk. */
    if (nvme_read_sync(slot_lba + 8ull, ep_stage) != 0) {
        ep_refuse(EP_REFUSE_IO, 1, 0);
        return EP_REFUSE_IO;
    }
    /* Read the remaining frames — the capture's exact count is not in the
     * head, so read until the layout walk succeeds or frames run out. */
    {
        uint32_t max_frames = EP_STAGE_BYTES / 4096u;
        for (uint32_t f = 1; f < max_frames; f++) {
            if (ep_index_blocks(head.n_tasks, head.n_pages, block_off,
                                &console_off, &total) &&
                total + head.console_len <= f * 4096u)
                break;                            /* everything needed is in */
            if (f + 1u >= max_frames) {
                ep_refuse(EP_REFUSE_FORMAT, 3, f);
                return EP_REFUSE_FORMAT;
            }
            ep_memset(ep_page, 0, 4096);
            if (nvme_read_sync(slot_lba + 8ull + (uint64_t)f * 8ull,
                               ep_page) != 0) {
                ep_refuse(EP_REFUSE_IO, 1, f);
                return EP_REFUSE_IO;
            }
            ep_copy(ep_stage + f * 4096u, ep_page, 4096u);
        }
    }
    if (!ep_index_blocks(head.n_tasks, head.n_pages, block_off,
                         &console_off, &total) ||
        total + head.console_len > EP_STAGE_BYTES) {
        ep_refuse(EP_REFUSE_FORMAT, 4, 0);
        return EP_REFUSE_FORMAT;
    }
    uint32_t crc_index = ep_crc(0, ep_stage, total + head.console_len);
    if (crc_index != head.crc32) {
        ep_refuse(EP_REFUSE_FORMAT, 5, crc_index & 0xffffu);
        return EP_REFUSE_FORMAT;
    }

    /* Placement: the replay's regions must sit where the captured bytes'
     * interior pointers point, or nothing is poured at all. */
    {
        uint32_t u = 0;
        for (uint32_t i = 0; i < 3; i++) {
            struct EnvPayloadRegion r;
            ep_copy(&r, ep_stage + u, (uint32_t)sizeof(r));
            u += (uint32_t)sizeof(r);
            if (r.kind == 0) continue;
            int found = 0;
            for (uint32_t j = 0; j < cur->n_regions && j < ENV_CKPT_MAX_REGIONS; j++) {
                if (cur->regions[j].kind != r.kind) continue;
                found = 1;
                if (cur->regions[j].base != r.base ||
                    cur->regions[j].frames != r.frames) {
                    ep_refuse(EP_REFUSE_PLACEMENT, r.kind, 1);
                    return EP_REFUSE_PLACEMENT;
                }
                break;
            }
            if (!found) {
                ep_refuse(EP_REFUSE_PLACEMENT, r.kind, 0);
                return EP_REFUSE_PLACEMENT;
            }
        }
    }

    /* Processes and form. A sidecar: captured PARKED and live PARKED in the
     * SAME recv syscall (the kernel-side park bookkeeping stays this boot's).
     * A Linux task (increment 20): captured FORM_USER and a live task the
     * replay has not let run — its whole state is the ring3_ctx the blob
     * carries, and increment 21's adopt path is what guarantees the task is
     * held at the pour (until then a live replay that has already started
     * the tenant is refused here, by name, rather than poured over). */
    struct ProcessDescriptor* pds[EP_MAX_TASKS];
    uint8_t task_kind[EP_MAX_TASKS];
    {
        uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion);
        for (uint32_t t = 0; t < head.n_tasks; t++) {
            struct EnvPayloadTask tk;
            ep_copy(&tk, ep_stage + u, (uint32_t)sizeof(tk));
            u += (uint32_t)sizeof(tk);
            /* The task's own page count and its block's count are the SAME
             * fact, carried twice on purpose: a blob whose halves disagree
             * is one no walk can size, and trusting either alone would read
             * the next task's block as this one's pages. */
            {
                uint32_t bu = block_off[t];
                uint32_t bn = (uint32_t)ep_stage[bu] |
                              ((uint32_t)ep_stage[bu + 1] << 8) |
                              ((uint32_t)ep_stage[bu + 2] << 16) |
                              ((uint32_t)ep_stage[bu + 3] << 24);
                if (bn != tk.n_pages) {
                    ep_refuse(EP_REFUSE_FORMAT, t, bn);
                    return EP_REFUSE_FORMAT;
                }
            }
            task_kind[t] = tk.kind;
#if E7G_TOOTH
            /* E7 increment 22's `descriptor` tooth: the payload's Linux task
             * RECORD is treated as never written — no live process is looked
             * up for it, its pages are not poured and its context is not
             * installed. The sidecars below are untouched, so the pour still
             * lands their bytes (payloads=1) and the gate's adoption clause
             * is the one that reddens. The released child then runs from the
             * create's synthetic context (_start), which is the real
             * consequence of a lost task record. */
            if (ep_tooth == ENV_PAYLOAD_TOOTH_DESCRIPTOR &&
                tk.kind == ENV_CKPT_TASK_LINUX) {
                kernel_serial_printf(
                    "[E7-GATE] tooth descriptor: the payload's linux task "
                    "'%s' (task %u) is DROPPED -- no record, no pages, no "
                    "context; the sidecars pour as usual\n",
                    tk.name, (unsigned)t);
                pds[t] = 0;
                continue;
            }
#endif
            struct ProcessDescriptor* pd = ep_find_proc(tk.name);
            if (!pd) { ep_refuse(EP_REFUSE_NO_PROCESS, t, 0); return EP_REFUSE_NO_PROCESS; }
            int is_linux = tk.kind == ENV_CKPT_TASK_LINUX;
            if (is_linux) {
                /* E7 increment 21: two resumable forms, one install path.
                 * USER is a task the pause caught in ring 3; ENTRY is the
                 * gate's deterministic stop at a `syscall` it has not run.
                 * Both carry the whole state as ring3_ctx (linux_regs), so
                 * both are poured by the same arm below. PARKED is still the
                 * named refusal: a forwarded request is queued state this
                 * blob does not carry (B9). */
                if (tk.form != ENV_PAYLOAD_FORM_USER &&
                    tk.form != ENV_PAYLOAD_FORM_ENTRY) {
                    /* PARKED is a queued request this blob does not carry;
                     * RUNNING is a capture that never had a restorable
                     * instant. Both are refused by their own name. */
                    ep_refuse(tk.form == ENV_PAYLOAD_FORM_PARKED
                                  ? EP_REFUSE_FORM : EP_REFUSE_RUNNING,
                              t, tk.form);
                    return tk.form == ENV_PAYLOAD_FORM_PARKED
                               ? EP_REFUSE_FORM : EP_REFUSE_RUNNING;
                }
                /* E7 increment 21: the replay's create holds its Linux child
                 * (cap_linux_restore_arm), so it is PROC_HELD with no context
                 * of its own — the blob's context is what it will run. A task
                 * that is running, parked, or otherwise live-with-context is
                 * the OLD shape and is refused by name: a pour into it would
                 * overwrite a task that already executed _start. */
                if (ep_proc_parked(pd) || pd->state == PROC_RUNNING ||
                    (!pd->restore_held && !pd->has_ring3_ctx)) {
                    ep_refuse(EP_REFUSE_NOT_PARKED, t, pd->park_syscall);
                    return EP_REFUSE_NOT_PARKED;
                }
            } else {
                if (tk.form != ENV_PAYLOAD_FORM_PARKED) {
                    ep_refuse(EP_REFUSE_RUNNING, t, 0);
                    return EP_REFUSE_RUNNING;
                }
                if (!ep_proc_parked(pd)) {
                    kernel_serial_printf(
                        "[ENV_PAYLOAD] posture: '%s' (task %u) is live but "
                        "not parked (state=%u waiting=%u park_syscall=%u)\n",
                        tk.name, (unsigned)t, (unsigned)pd->state,
                        (unsigned)pd->waiting_chan,
                        (unsigned)pd->park_syscall);
                    ep_refuse(EP_REFUSE_NOT_PARKED, t, 0);
                    return EP_REFUSE_NOT_PARKED;
                }
                if (pd->park_syscall != tk.park_syscall) {
                    kernel_serial_printf(
                        "[ENV_PAYLOAD] posture: '%s' (task %u) is parked in "
                        "syscall %u but the capture recorded %u "
                        "(state=%u nchans=%u)\n",
                        tk.name, (unsigned)t, (unsigned)pd->park_syscall,
                        (unsigned)tk.park_syscall, (unsigned)pd->state,
                        (unsigned)pd->waiting_nchans);
                    ep_refuse(EP_REFUSE_NOT_PARKED, t, pd->park_syscall);
                    return EP_REFUSE_NOT_PARKED;
                }
            }
            pds[t] = pd;
        }
    }

    /* Every destination, before a byte is written: identity for region
     * pages (placement already proven equal), a live frame for image and
     * stack pages — and (increment 20) the live leaf's permissions must be
     * the captured ones. A page that came back writable where the capture
     * saw RX is not the same page: pouring it would re-open executable text
     * for writing, and a map that lost NX would run data. Refused by name,
     * before the first byte, like every other destination fault. */
    {
        uint32_t pi = 0;
        for (uint32_t t = 0; t < head.n_tasks; t++) {
            uint32_t u = block_off[t];
            uint32_t np = (uint32_t)ep_stage[u] |
                          ((uint32_t)ep_stage[u + 1] << 8) |
                          ((uint32_t)ep_stage[u + 2] << 16) |
                          ((uint32_t)ep_stage[u + 3] << 24);
            u += 4u;
#if E7G_TOOTH
            /* A task the descriptor tooth dropped owns no destination: its
             * page entries are stepped over (not validated, not poured). The
             * media pass below still reads and checks their crcs — the bytes
             * are known-good, they are simply not this environment's to
             * receive any more. */
            if (ep_tooth == ENV_PAYLOAD_TOOTH_DESCRIPTOR &&
                task_kind[t] == ENV_CKPT_TASK_LINUX && !pds[t]) {
                for (uint32_t k = 0; k < np; k++, pi++) {
                    if (pi < EP_MAX_PAGES) ep_skip[pi] = 1;
                    u += EP_PAGE_ENTRY_BYTES;
                }
                continue;
            }
#endif
            for (uint32_t k = 0; k < np; k++, pi++) {
                uint64_t v = 0;
                for (uint32_t b = 0; b < 8; b++)
                    v |= ((uint64_t)ep_stage[u + b]) << (8u * b);
                uint32_t want_perm = (uint32_t)ep_stage[u + 12] |
                                     ((uint32_t)ep_stage[u + 13] << 8) |
                                     ((uint32_t)ep_stage[u + 14] << 16) |
                                     ((uint32_t)ep_stage[u + 15] << 24);
                u += EP_PAGE_ENTRY_BYTES;
                if (pi >= EP_MAX_PAGES) {
                    ep_refuse(EP_REFUSE_FORMAT, 6, pi);
                    return EP_REFUSE_FORMAT;
                }
                if ((want_perm & ~EP_PAGE_PERM_MASK) != 0) {
                    ep_refuse(EP_REFUSE_FORMAT, 7, pi);
                    return EP_REFUSE_FORMAT;
                }
                uint64_t phys = 0;
                uint32_t live_perm = 0;
                if (task_kind[t] != ENV_CKPT_TASK_LINUX &&
                    ep_in_regions(cur, v)) {
                    if (!ep_resolve_ex(pds[t]->cr3, v, &phys, &live_perm)) {
                        ep_refuse(EP_REFUSE_NO_MAPPING, t, pi);
                        return EP_REFUSE_NO_MAPPING;
                    }
                    phys = v;   /* identity: the placement check already proved it */
                } else if (!ep_resolve_ex(pds[t]->cr3, v, &phys, &live_perm)) {
                    /* E7 increment 21: a held Linux task's heap pages were
                     * mapped LAZILY by the original run's brk/mmap, so the
                     * replay's fresh address space has no frame at those
                     * vaddrs yet. Map one now, charged to the child's own
                     * partition, with exactly the captured grant — the page
                     * range is the record's/descriptor's own span, never an
                     * arbitrary address the blob could name. */
                    if (task_kind[t] != ENV_CKPT_TASK_LINUX ||
                        !pds[t]->restore_held ||
                        !ep_linux_page(cur, t, pds[t], v)) {
                        ep_refuse(EP_REFUSE_NO_MAPPING, t, pi);
                        return EP_REFUSE_NO_MAPPING;
                    }
                    void* frame = allocate_physical_ram_frame_for_partition(
                                      pds[t]->partition_id);
                    if (!frame) {
                        ep_refuse(EP_REFUSE_NO_MAPPING, t, pi);
                        return EP_REFUSE_NO_MAPPING;
                    }
                    uint64_t flags = USER_PTE_PRESENT;
                    if (want_perm & EP_PAGE_PERM_USER) flags |= USER_PTE_USER;
                    if (want_perm & EP_PAGE_PERM_WRITE) flags |= USER_PTE_WRITE;
                    if (want_perm & EP_PAGE_PERM_EXEC)  flags |= USER_PTE_EXEC;
                    else                                flags |= USER_PTE_NOEXEC;
                    user_map_page((uint64_t*)(uintptr_t)pds[t]->cr3, v,
                                  (uint64_t)(uintptr_t)frame, flags);
                    phys = (uint64_t)(uintptr_t)frame;
                    live_perm = want_perm;
                    kernel_serial_printf(
                        "[ENV_PAYLOAD] mapped a missing Linux page task=%u "
                        "vaddr=0x%llx perm=0x%x for the replay's fresh "
                        "address space\n",
                        (unsigned)t, (unsigned long long)v,
                        (unsigned)(want_perm & EP_PAGE_PERM_MASK));
                }
                if ((live_perm & EP_PAGE_PERM_MASK) !=
                    (want_perm & EP_PAGE_PERM_MASK)) {
                    ep_refuse(EP_REFUSE_PERM, t, pi);
                    return EP_REFUSE_PERM;
                }
                ep_scratch.dest[pi] = phys;
            }
        }
    }

    /* Console: the replayed environment must HAVE one to receive the bytes. */
    if (head.console_len) {
        ep_copy(ep_console, ep_stage + console_off, head.console_len);
        if (env_console_inject(partition, new_env_id, ep_console,
                               head.console_len) != 1) {
            ep_refuse(EP_REFUSE_CONSOLE, 0, 0);
            return EP_REFUSE_CONSOLE;
        }
    }

    /* Media pass: every page crc read and checked, discarded. The pour
     * below writes what it reads; this pass is what makes the first write
     * already-known-good. */
    /* The capture sized the index zone on regions+tasks+blocks+console;
     * ep_index_blocks()'s `total` stops before the console, so add it back
     * to reproduce the same frame count and thus the same data offset. */
    uint32_t index_all = total + head.console_len;
    uint32_t data_off_frames = 1u + (index_all + 4095u) / 4096u;
    {
        uint32_t pi = 0;
        for (uint32_t t = 0; t < head.n_tasks; t++) {
            uint32_t u = block_off[t];
            uint32_t np = (uint32_t)ep_stage[u] |
                          ((uint32_t)ep_stage[u + 1] << 8) |
                          ((uint32_t)ep_stage[u + 2] << 16) |
                          ((uint32_t)ep_stage[u + 3] << 24);
            u += 4u;
            for (uint32_t k = 0; k < np; k++, pi++) {
                uint32_t want = (uint32_t)ep_stage[u + 8] |
                                ((uint32_t)ep_stage[u + 9] << 8) |
                                ((uint32_t)ep_stage[u + 10] << 16) |
                                ((uint32_t)ep_stage[u + 11] << 24);
                u += EP_PAGE_ENTRY_BYTES;
                ep_memset(ep_page, 0, 4096);
                if (nvme_read_sync(slot_lba + (uint64_t)data_off_frames * 8ull +
                                   (uint64_t)pi * 8ull, ep_page) != 0) {
                    ep_refuse(EP_REFUSE_IO, 2, pi);
                    return EP_REFUSE_IO;
                }
                if (ep_crc(0, ep_page, 4096) != want) {
                    ep_refuse(EP_REFUSE_FORMAT, 9, pi);
                    return EP_REFUSE_FORMAT;
                }
            }
        }
    }

    /* ── THE POUR. No yield between here and the last memcpy — the
     * header's rule, pinned by a source clause. ── */
    uint32_t pages_poured = 0;
    for (uint32_t pi = 0; pi < head.n_pages; pi++) {
#if E7G_TOOTH
        if (ep_skip[pi]) continue;   /* the descriptor tooth's dropped task */
#endif
        ep_memset(ep_page, 0, 4096);
        if (nvme_read_sync(slot_lba + (uint64_t)data_off_frames * 8ull +
                           (uint64_t)pi * 8ull, ep_page) != 0) {
            ep_refuse(EP_REFUSE_IO, 3, pi);
            return EP_REFUSE_IO;
        }
        uint8_t* dst = (uint8_t*)(uintptr_t)ep_scratch.dest[pi];
        ep_copy(dst, ep_page, 4096);
        pages_poured++;
    }

    /* Register state: the captured park's USER half over the live
     * process's. Channel ids, the request frame and the deadline stay this
     * boot's — they name boot-local objects the capture cannot carry.
     *
     * A Linux task is not resumed through a park at all: its state is the
     * full ring3_ctx (increment 20), which the next switch-in loads as-is —
     * all 20 words, the six argument registers included. Its TLS base and
     * both cursors come back with it, so the first post-restore brk/mmap
     * resume sees the break the workload had actually reached. */
    {
        uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion);
        for (uint32_t t = 0; t < head.n_tasks; t++) {
            struct EnvPayloadTask tk;
            ep_copy(&tk, ep_stage + u, (uint32_t)sizeof(tk));
            u += (uint32_t)sizeof(tk);
            struct ProcessDescriptor* pd = pds[t];
            if (tk.kind == ENV_CKPT_TASK_LINUX) {
                /* B2 compares this value against the capture's; the `fs_base`
                 * tooth is what makes the two differ. */
                uint64_t adopted_fs = tk.fs_base;
#if E7G_TOOTH
                if (!pd) continue;   /* descriptor tooth: nothing to adopt */
                /* The fd/heap teeth travel on the descriptor: the forward
                 * path reads them from the task the pour just adopted. */
                pd->tooth = (uint8_t)((ep_tooth == ENV_PAYLOAD_TOOTH_NO_FD ||
                                       ep_tooth == ENV_PAYLOAD_TOOTH_NO_HEAP)
                                          ? ep_tooth : ENV_PAYLOAD_TOOTH_NONE);
                /* The `fs_base` tooth (increment 23): move the TLS base one
                 * page off the capture's, into a page the record itself says
                 * will be mapped writable — the page below first (the RW
                 * LOAD's own). Nothing else about the context moves, so the
                 * tenant adopts and the copy reads through; the only wrong
                 * fact at the pour is B2's `fs_base=`, and the stale TCB is
                 * what the later B4/B5 reds record. A tooth with no such
                 * neighbour is loudly NOT armed:
                 * the harness then sees B2 green where it must redden and
                 * fails the run, which is better than a silent control. */
                if (ep_tooth == ENV_PAYLOAD_TOOTH_FS_BASE) {
                    uint32_t np = (uint32_t)ep_stage[block_off[t]] |
                                  ((uint32_t)ep_stage[block_off[t] + 1] << 8) |
                                  ((uint32_t)ep_stage[block_off[t] + 2] << 16) |
                                  ((uint32_t)ep_stage[block_off[t] + 3] << 24);
                    const uint8_t* block = ep_stage + block_off[t] + 4u;
                    uint64_t base = tk.fs_base & ~0xfffull;
                    uint64_t near = 0;
                    if (base >= 4096ull &&
                        ep_page_recorded_writable(block, np, base - 4096ull))
                        near = base - 4096ull;
                    else if (ep_page_recorded_writable(block, np,
                                                       base + 4096ull))
                        near = base + 4096ull;
                    if (!near) {
                        kernel_serial_printf(
                            "[E7-GATE] tooth fs_base: no recorded writable "
                            "page beside fs_base=0x%llx -- the tooth is NOT "
                            "armed\n",
                            (unsigned long long)tk.fs_base);
                    } else {
                        adopted_fs = near | (tk.fs_base & 0xfffull);
                        kernel_serial_printf(
                            "[E7-GATE] tooth fs_base: the record's fs_base "
                            "0x%llx is re-stamped 0x%llx (capture=0x%llx "
                            "restored=0x%llx) -- adoption happens, the "
                            "pages are poured, only the TLS base is one "
                            "page off\n",
                            (unsigned long long)tk.fs_base,
                            (unsigned long long)adopted_fs,
                            (unsigned long long)tk.fs_base,
                            (unsigned long long)adopted_fs);
                    }
                }
#endif
                uint64_t* c = (uint64_t*)&pd->ring3_ctx;
                for (uint32_t r = 0; r < 20; r++) c[r] = tk.linux_regs[r];
                pd->has_ring3_ctx  = 1;
                pd->user_rip       = tk.user_rip;
                pd->user_rsp       = tk.user_rsp;
                pd->fs_base        = adopted_fs;
                pd->linux_brk_cur  = tk.brk_cur;
                pd->linux_mmap_top = tk.mmap_top;
                /* E7 increment 21: the stop is ONE-SHOT, and FORM_ENTRY is
                 * its aftermath. The replay's create is built from the same
                 * FLAG_GATE manifest, so the fresh descriptor came back with
                 * the stop ARMED (gate_profile=1, gate_bytes=0) — leaving it
                 * armed would stop the resumed copy at the same cumulative
                 * offset a second time, mid-continuation, and the task would
                 * park against a sidecar that is waiting for it: the live
                 * run's second hold, exactly. The resumed read is the
                 * continuation and must be forwarded, so the stop is spent
                 * here: the counter names the instant the blob carries (the
                 * bytes this task's replies had already declared) and the
                 * arm is cleared. A sidecar's park frame is untouched — this
                 * is the Linux arm only — and FORM_USER, which was never a
                 * stop, keeps whatever the create armed (it has none). */
                if (tk.form == ENV_PAYLOAD_FORM_ENTRY) {
                    pd->gate_profile = 0;
                    pd->gate_bytes   = PROC_GATE_SRC_OFF;
                }
                /* The run's own evidence line: the context the task will
                 * actually execute when cap_linux_restore_release() puts it
                 * back on the scheduler. Printed at the pour, before the
                 * release, so a later "second _start" cannot be mistaken
                 * for what this arm did. */
                kernel_serial_printf(
                    "[ENV_PAYLOAD] linux task '%s' adopted at rip=0x%llx "
                    "rsp=0x%llx fs_base=0x%llx brk=0x%llx mtop=0x%llx "
                    "form=%u held=%u stop=%s\n",
                    tk.name, (unsigned long long)tk.user_rip,
                    (unsigned long long)tk.user_rsp,
                    (unsigned long long)pd->fs_base,
                    (unsigned long long)tk.brk_cur,
                    (unsigned long long)tk.mmap_top,
                    (unsigned)tk.form, (unsigned)pd->restore_held,
                    pd->gate_profile ? "armed" : "spent");
                continue;
            }
            pd->park_ctx.r11      = tk.park[0];
            pd->park_ctx.rcx      = tk.park[1];
            pd->park_ctx.r15      = tk.park[2];
            pd->park_ctx.r14      = tk.park[3];
            pd->park_ctx.r13      = tk.park[4];
            pd->park_ctx.r12      = tk.park[5];
            pd->park_ctx.rbx      = tk.park[6];
            pd->park_ctx.rbp      = tk.park[7];
            pd->park_ctx.user_rsp = tk.park[8];
            pd->user_rip          = tk.user_rip;
            pd->user_rsp          = tk.user_rsp;
        }
    }

    if (out_pages) *out_pages = pages_poured;
    if (out_console) *out_console = head.console_len;
    kernel_serial_printf(
        "[ENV_PAYLOAD] restored partition=%u index=%u env=%u pages=%u "
        "console=%u bytes\n",
        (unsigned)partition, (unsigned)index, (unsigned)new_env_id,
        (unsigned)pages_poured, (unsigned)head.console_len);
    return EP_REFUSE_NONE;
}
