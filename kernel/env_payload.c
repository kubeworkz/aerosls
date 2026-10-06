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
 *     tasks[n_tasks]           names, register form, park save areas
 *     per task: u32 n_pages, then n_pages x {u64 vaddr, u32 crc32}
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
#include "../drivers/nvme_io.h"
#include "kernel_io.h"
#include "timer.h"          /* kernel_tick_counter */

/* USER_PROC_CODE_BASE — cap.c's literal for the image/stack window. Kept
 * as a local mirror; tests/env_checkpoint_restore_check.sh pins it against
 * cap.c's literal so the window cannot drift away from what the create
 * path maps. */
#define EP_IMAGE_VBASE   0x400000000000ULL
#define EP_IMAGE_WINDOW  0x40000000ULL      /* 1 GiB — image, guard page, stack; nothing else maps here */

#define EP_MAX_PAGES     2048u
#define EP_STAGE_BYTES   (4096u * 6u)       /* the index zone's frame ceiling */
#define EP_MAX_TASKS     4u
#define EP_WAIT_TICKS    3000u              /* ~30 s at timer.h's documented ~100 Hz */
#define EP_CONSOLE_MAX   4096u              /* ENV_CONSOLE_BUF */

static struct EnvPayloadRefusal ep_refusal;

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
/* Resolve vaddr -> phys in `cr3`'s address space. Handles 4 KiB PTEs, 2 MiB
 * PDEs and 1 GiB PDPTEs (cap.c's shared-huge-page paths). The USER bit is
 * required at the PTE, the way the map is built; upper levels are walked on
 * PRESENT alone. Returns 1 on a present, user page. */
static int ep_resolve(uint64_t cr3, uint64_t vaddr, uint64_t* out_phys) {
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
        return 1;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[i3] & 0x000ffffffffff000ull);
    if (!(pd[i2] & 1ull)) return 0;
    if (pd[i2] & (1ull << 7)) {
        if (!(pd[i2] & (1ull << 2))) return 0;
        *out_phys = (pd[i2] & 0x000fffffffe00000ull) + (vaddr & 0x1fffffull);
        return 1;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(pd[i2] & 0x000ffffffffff000ull);
    if (!(pt[i1] & 1ull) || !(pt[i1] & (1ull << 2))) return 0;
    *out_phys = (pt[i1] & 0x000ffffffffff000ull) + (vaddr & 0xfffull);
    return 1;
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

/* The index-zone size the restore must reproduce exactly (it places the
 * data zone): regions + tasks + per-task blocks + console. */
static uint32_t ep_index_bytes(uint32_t n_tasks, const uint32_t* task_npages,
                               uint32_t console_len) {
    uint32_t n = 3u * (uint32_t)sizeof(struct EnvPayloadRegion);
    n += n_tasks * (uint32_t)sizeof(struct EnvPayloadTask);
    for (uint32_t t = 0; t < n_tasks; t++)
        n += 4u + task_npages[t] * 12u;
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

        /* Every named sidecar must resolve: a payload missing one of the
         * two would restore a half-environment whose other half's heap the
         * first half still points into. */
        const struct ProcessDescriptor* pds[ENV_CKPT_MAX_TASKS];
        uint32_t np = 0;
        int missing = 0;
        for (uint32_t t = 0; t < rec->n_tasks && t < ENV_CKPT_MAX_TASKS; t++) {
            if (rec->tasks[t].kind == ENV_CKPT_TASK_LINUX) continue; /* E7's */
            struct ProcessDescriptor* pd = ep_find_proc(rec->tasks[t].name);
            if (!pd) { missing = 1; break; }
            pds[np++] = pd;
        }
        if (missing || np == 0) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] capture skipped for partition=%u index=%u: %s\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                missing ? "a named sidecar has no live process" : "no sidecars");
            continue;
        }

        /* Collect the pages: image/stack window + private regions, per
         * task. Shared-arena and lazily-granted frames are deliberately
         * NOT captured (env_payload.h rule 3). */
        uint32_t n_pages = 0;
        int overflow = 0;
        uint32_t task_page_base[ENV_CKPT_MAX_TASKS];
        uint32_t task_npages[ENV_CKPT_MAX_TASKS];
        for (uint32_t t = 0; t < np; t++) {
            task_page_base[t] = n_pages;
            task_npages[t] = 0;
            uint64_t cr3 = pds[t]->cr3;
            if (!cr3) continue;
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
                                if (!ep_in_image_window(v) && !ep_in_regions(rec, v)) continue;
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
                            if (!ep_in_image_window(v) && !ep_in_regions(rec, v)) continue;
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
            if (ep_in_regions(rec, v)) {
                phys = v;                       /* identity: the address IS the frame */
            } else if (!ep_resolve(pds[ep_tnum[pi]]->cr3, v, &phys)) {
                io_fail = 1;                    /* the walk said present; treat as I/O-class failure */
                break;
            }
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
            const struct ProcessDescriptor* pd = pds[t];
            if (ep_proc_parked(pd)) {
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

        kernel_serial_printf(
            "[ENV_PAYLOAD] captured partition=%u index=%u pages=%u "
            "console=%u bytes=%u slot=%u seq=%u\n",
            (unsigned)rec->partition_id, (unsigned)rec->index,
            (unsigned)n_pages, (unsigned)console_len,
            (unsigned)head.bytes, (unsigned)slot,
            (unsigned)rec->sequence);
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
int env_payload_wait_parked(const struct EnvCkptRecord* rec) {
    if (!rec) return 0;
    uint64_t deadline = kernel_tick_counter + EP_WAIT_TICKS;
    for (;;) {
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
            if (rec->tasks[t].kind == ENV_CKPT_TASK_LINUX) continue;
            struct ProcessDescriptor* pd = ep_find_proc(rec->tasks[t].name);
            if (!pd) return 0;                   /* nothing to wait for */
            if (!ep_proc_parked(pd)) { ready = 0; break; }
        }
        if (ready && !env_console_identity_seen(rec->partition_id, rec->index))
            ready = 0;
        if (ready) return 1;
        if (kernel_tick_counter >= deadline) {
            kernel_serial_printf(
                "[ENV_PAYLOAD] wait timed out (partition=%u index=%u): "
                "announced=%d — proceeding on the posture as it stands\n",
                (unsigned)rec->partition_id, (unsigned)rec->index,
                env_console_identity_seen(rec->partition_id, rec->index));
            return 0;
        }
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
        if (np > n_pages || u + np * 12u > EP_STAGE_BYTES) return 0;
        u += np * 12u;
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

    /* Processes and park form: captured PARKED, live PARKED in the SAME
     * recv syscall. The kernel-side park bookkeeping stays this boot's. */
    struct ProcessDescriptor* pds[EP_MAX_TASKS];
    {
        uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion);
        for (uint32_t t = 0; t < head.n_tasks; t++) {
            struct EnvPayloadTask tk;
            ep_copy(&tk, ep_stage + u, (uint32_t)sizeof(tk));
            u += (uint32_t)sizeof(tk);
            struct ProcessDescriptor* pd = ep_find_proc(tk.name);
            if (!pd) { ep_refuse(EP_REFUSE_NO_PROCESS, t, 0); return EP_REFUSE_NO_PROCESS; }
            if (tk.form != ENV_PAYLOAD_FORM_PARKED) {
                ep_refuse(EP_REFUSE_RUNNING, t, 0);
                return EP_REFUSE_RUNNING;
            }
            if (!ep_proc_parked(pd)) {
                ep_refuse(EP_REFUSE_NOT_PARKED, t, 0);
                return EP_REFUSE_NOT_PARKED;
            }
            if (pd->park_syscall != tk.park_syscall) {
                ep_refuse(EP_REFUSE_NOT_PARKED, t, pd->park_syscall);
                return EP_REFUSE_NOT_PARKED;
            }
            pds[t] = pd;
        }
    }

    /* Every destination, before a byte is written: identity for region
     * pages (placement already proven equal), a live frame for image and
     * stack pages. */
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
                uint64_t v = 0;
                for (uint32_t b = 0; b < 8; b++)
                    v |= ((uint64_t)ep_stage[u + b]) << (8u * b);
                u += 12u;
                if (pi >= EP_MAX_PAGES) {
                    ep_refuse(EP_REFUSE_FORMAT, 6, pi);
                    return EP_REFUSE_FORMAT;
                }
                uint64_t phys = 0;
                if (ep_in_regions(cur, v)) {
                    phys = v;
                } else if (!ep_resolve(pds[t]->cr3, v, &phys)) {
                    ep_refuse(EP_REFUSE_NO_MAPPING, t, pi);
                    return EP_REFUSE_NO_MAPPING;
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
                u += 12u;
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
     * boot's — they name boot-local objects the capture cannot carry. */
    {
        uint32_t u = 3u * (uint32_t)sizeof(struct EnvPayloadRegion);
        for (uint32_t t = 0; t < head.n_tasks; t++) {
            struct EnvPayloadTask tk;
            ep_copy(&tk, ep_stage + u, (uint32_t)sizeof(tk));
            u += (uint32_t)sizeof(tk);
            struct ProcessDescriptor* pd = pds[t];
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
