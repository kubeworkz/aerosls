/*
 * workload_ctx.c — the producer of live execution contexts.
 * See workload_ctx.h for why this exists and what it closes.
 *
 * Freestanding: local helpers, no libc.
 */
#include "workload_ctx.h"
#include "simi_ctx_migrate.h"
#include "workload.h"
#include "loader.h"
#include "kernel_io.h"

/* ─── Local helpers ─────────────────────────────────────────────────── */
static int wc_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void wc_strcpy(char* d, const char* s, int n) {
    int i; for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}
static void wc_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static uint32_t wc_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char* wlctx_status_name(WLCtxStatus s) {
    switch (s) {
        case WLCTX_OK:            return "OK";
        case WLCTX_ERR_FULL:      return "ERR:context-pool-full";
        case WLCTX_ERR_NOT_FOUND: return "ERR:no-live-context";
        case WLCTX_ERR_BAD_IMAGE: return "ERR:not-a-valid-simi-image";
        case WLCTX_ERR_TOO_BIG:   return "ERR:program-exceeds-context-caps";
        case WLCTX_ERR_NO_ENTRY:  return "ERR:no-such-entry-point";
        case WLCTX_ERR_EXISTS:    return "ERR:already-running";
        default:                  return "ERR:unknown";
    }
}

/* One live workload's execution state plus its own naturally-aligned copy
 * of the program image (see the header on why it is a copy). */
struct WLCtxSlot {
    uint8_t  active;
    char     workload[WLCTX_NAME_LEN];
    uint32_t partition_id;

    struct SimiContext ctx;

    uint64_t  instr[WLCTX_MAX_INSTR];
    uint64_t  literals[WLCTX_MAX_LITERALS];
    SimiEntry entries[WLCTX_MAX_ENTRIES];
    SimiName  names[WLCTX_MAX_NAMES];
    SimiObject obj;
};

static struct WLCtxSlot wl_slots[WLCTX_MAX];

void wlctx_init(void) {
    for (int i = 0; i < WLCTX_MAX; i++) wl_slots[i].active = 0;
    simi_ctx_migrate_clear_images();
    kernel_serial_printf("[WLCTX] context pool initialised (%d slots).\n", WLCTX_MAX);
}

static struct WLCtxSlot* wc_find(const char* workload) {
    if (!workload) return 0;
    for (int i = 0; i < WLCTX_MAX; i++)
        if (wl_slots[i].active && wc_streq(wl_slots[i].workload, workload)) return &wl_slots[i];
    return 0;
}

int      wlctx_has(const char* workload)  { return wc_find(workload) != 0; }
uint32_t wlctx_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < WLCTX_MAX; i++) if (wl_slots[i].active) n++;
    return n;
}
struct SimiContext* wlctx_get(const char* workload) {
    struct WLCtxSlot* s = wc_find(workload);
    return s ? &s->ctx : 0;
}

/* ─── .tmo parse ──────────────────────────────────────────────────────
 * Header is five little-endian u32s, then instructions, literals,
 * entries, names, back to back. Every declared count is checked against
 * the buffer BEFORE any of it is read: a corrupt header claiming a huge
 * instruction count would otherwise walk off the end. Same discipline
 * loader.c's simi_validate() applies, re-stated here rather than shared
 * because this needs the offsets too, not just a yes/no. */
#define WC_SIMI_MAGIC 0x314D4954u   /* "TIM1" */

static WLCtxStatus wc_parse(struct WLCtxSlot* s, const uint8_t* img, uint32_t size) {
    if (!img || size < 20) return WLCTX_ERR_BAD_IMAGE;
    if (wc_rd32(img) != WC_SIMI_MAGIC) return WLCTX_ERR_BAD_IMAGE;

    uint32_t n_instr = wc_rd32(img + 4);
    uint32_t n_lit   = wc_rd32(img + 8);
    uint32_t n_ent   = wc_rd32(img + 12);
    uint32_t n_nam   = wc_rd32(img + 16);

    /* Caps first: a program too large to hold is refused outright rather
     * than truncated into a different, silently-wrong program. */
    if (n_instr > WLCTX_MAX_INSTR || n_lit > WLCTX_MAX_LITERALS ||
        n_ent   > WLCTX_MAX_ENTRIES || n_nam > WLCTX_MAX_NAMES)
        return WLCTX_ERR_TOO_BIG;

    /* Then bounds: the declared contents must actually fit in `size`.
     * Accumulated in 64-bit so a hostile header cannot overflow the sum
     * into looking small enough. */
    uint64_t need = 20;
    need += (uint64_t)n_instr * 8u;
    need += (uint64_t)n_lit   * 8u;
    need += (uint64_t)n_ent   * (uint64_t)sizeof(SimiEntry);
    need += (uint64_t)n_nam   * (uint64_t)sizeof(SimiName);
    if (need > (uint64_t)size) return WLCTX_ERR_BAD_IMAGE;

    const uint8_t* p = img + 20;
    wc_memcpy(s->instr, p, n_instr * 8u);                 p += n_instr * 8u;
    wc_memcpy(s->literals, p, n_lit * 8u);                p += n_lit * 8u;
    wc_memcpy(s->entries, p, n_ent * (uint32_t)sizeof(SimiEntry));
    p += n_ent * (uint32_t)sizeof(SimiEntry);
    wc_memcpy(s->names, p, n_nam * (uint32_t)sizeof(SimiName));

    s->obj.magic        = WC_SIMI_MAGIC;
    s->obj.num_instr    = n_instr;
    s->obj.num_literals = n_lit;
    s->obj.num_entries  = n_ent;
    s->obj.num_names    = n_nam;
    s->obj.instr        = s->instr;
    s->obj.literals     = s->literals;
    s->obj.entries      = s->entries;
    s->obj.names        = s->names;
    return WLCTX_OK;
}

WLCtxStatus wlctx_start(const char* workload, const uint8_t* image,
                        uint32_t image_size, const char* entry,
                        uint32_t partition_id) {
    if (!workload || !workload[0]) return WLCTX_ERR_NOT_FOUND;
    if (wc_find(workload))         return WLCTX_ERR_EXISTS;

    struct WLCtxSlot* s = 0;
    for (int i = 0; i < WLCTX_MAX; i++) if (!wl_slots[i].active) { s = &wl_slots[i]; break; }
    if (!s) {
        kernel_serial_printf("[WLCTX] pool full (%d) -- '%s' not started.\n",
                             WLCTX_MAX, workload);
        return WLCTX_ERR_FULL;
    }

    WLCtxStatus rc = wc_parse(s, image, image_size);
    if (rc != WLCTX_OK) {
        kernel_serial_printf("[WLCTX] '%s': %s\n", workload, wlctx_status_name(rc));
        return rc;
    }

    if (simi_interp_init(&s->ctx, &s->obj, entry ? entry : "main") != 0)
        return WLCTX_ERR_NO_ENTRY;

    wc_strcpy(s->workload, workload, WLCTX_NAME_LEN);
    s->partition_id = partition_id;
    s->active       = 1;

    /* THE line this whole file exists for. Until now nothing in the
     * kernel ever called this, so partition_migrate() had nothing to
     * move. The image is registered too: a migrated checkpoint carries a
     * program HASH, not the program, so the receiving node must be able
     * to find a matching image or it will refuse the restore. */
    simi_ctx_migrate_register_image(&s->obj);
    simi_ctx_register(partition_id, workload, &s->ctx);

    kernel_serial_printf("[WLCTX] '%s' started in partition %u at entry '%s' "
                         "(%u instructions) -- now migratable.\n",
                         workload, (unsigned)partition_id,
                         entry ? entry : "main", (unsigned)s->obj.num_instr);
    return WLCTX_OK;
}

WLCtxStatus wlctx_stop(const char* workload) {
    struct WLCtxSlot* s = wc_find(workload);
    if (!s) return WLCTX_ERR_NOT_FOUND;
    simi_ctx_unregister(&s->ctx);
    s->active = 0;
    kernel_serial_printf("[WLCTX] '%s' stopped.\n", workload);
    return WLCTX_OK;
}

uint32_t wlctx_step_all(uint64_t budget) {
    uint32_t stepped = 0;
    for (int i = 0; i < WLCTX_MAX; i++) {
        struct WLCtxSlot* s = &wl_slots[i];
        if (!s->active) continue;
        /* Only OK means "mid-execution, resumable". HALTED and every
         * TRAP_* are terminal; re-entering would just return the same
         * status and burn a sweep. The context stays registered because
         * its final state is still worth migrating and inspecting. */
        if (s->ctx.status != SIMI_STATUS_OK) continue;
        simi_interp_run(&s->ctx, budget);
        stepped++;
    }
    return stepped;
}

void wlctx_list(void) {
    kernel_serial_printf("[WLCTX] %u live context(s):\n", (unsigned)wlctx_count());
    for (int i = 0; i < WLCTX_MAX; i++) {
        struct WLCtxSlot* s = &wl_slots[i];
        if (!s->active) continue;
        kernel_serial_printf("  %-20s partition=%u pc=%u steps=%llu status=%s\n",
                             s->workload, (unsigned)s->partition_id,
                             (unsigned)s->ctx.pc,
                             (unsigned long long)s->ctx.steps,
                             simi_status_name(s->ctx.status));
    }
}

/* ─── Program lookup ──────────────────────────────────────────────────
 * Declared in workload.h, defined HERE rather than in workload.c on
 * purpose: this is the only file that has any business knowing about the
 * uploaded-binary store, and keeping the reference here means a test that
 * exercises the reconciler alone does not have to link loader.c (and
 * through it the NVMe and process machinery) merely to say "no programs
 * are uploaded". Such a test supplies its own two-line version instead. */
const uint8_t* workload_find_program(const char* name, uint32_t* out_size) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (!service_binaries[i].active) continue;
        if (!wc_streq(service_binaries[i].object_name, name)) continue;
        if (out_size) *out_size = service_binaries[i].size;
        return service_binaries[i].data;
    }
    return 0;
}
