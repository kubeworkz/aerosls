/*
 * vecstore.c — Vector Store Roadmap Phase 1 vector collection storage.
 * See vecstore.h for the full design writeup.
 */
#include "vecstore.h"
#include "object_catalog.h"
#include "frame_pool.h"
#include "storage_quota.h"    // Storage Isolation Roadmap Phase 1: per-partition on-disk page quota
#include "kernel_io.h"
#include "persist.h"     // Gap Remediation Phase D -- persist_vecstore_headers()
#include "../user/permissions.h"
#include "../drivers/nvme_io.h"   // Gap Remediation Phase D -- real page persistence
#include "vec_index.h"   // Vector Store Roadmap Phase 6 -- auto-maintenance hooks only, see
                          // vecstore_insert()/vecstore_delete() below and vec_index.h's own
                          // header comment on why this mirrors row_index.c/rowstore.c's
                          // established precedent
#include <stddef.h>

struct VecCollectionHeader vector_collections[VECSTORE_MAX_COLLECTIONS];
uint32_t                   vecstore_next_free_page_id = 0;
// Storage Isolation Roadmap Phase 3: per-partition page sub-range cursors --
// see vecstore.h's header comment for the full design. Initialized by
// vecstore_init() below.
uint32_t                   vecstore_partition_cursor[PARTITION_MAX];

// RAM-cached page pointers -- NULL = not resident this boot. Gap
// Remediation Phase D: this is no longer purely RAM-only (see vecstore.h's
// header comment) -- a page either lives here already, or is lazily loaded
// from NVMe on first access via vecstore_load_page(), exactly mirroring
// rowstore.c's row_pages[]/rowstore_load_page() precedent.
static uint8_t* vec_pages[VECSTORE_MAX_PAGES];

// ─── String / memory helpers (no libc, vs_* here, matching this
// codebase's established per-file convention: rs_* in rowstore.c, ri_* in
// row_index.c, rc_* in row_constraint.c, rj_* in row_journal.c). ──────────
static int vs_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void vs_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static void vs_memset(void* d, uint8_t v, uint32_t n) {
    uint8_t* p = (uint8_t*)d; while (n--) *p++ = v;
}

// ─── Collection lookup ──────────────────────────────────────────────────────
// -1 if not found / not a vector collection. Deliberately checks
// vector_collections[]'s OWN .active flag, not any object_catalog[] field
// -- see vecstore.h's header comment on why there is no catalog-level
// "uses_vectorstore" flag in this phase.
static int find_active_collection(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (!vector_collections[i].active) continue;
        if (vs_streq(object_catalog[i].name, name)) return (int)i;
    }
    return -1;
}

// ─── Page pool ───────────────────────────────────────────────────────────────
// Allocates a brand-new page from the shared bump-allocator pool. No
// reclaim in this first cut, matching rowstore.c's identical posture.
// Gap Remediation Phase D: does NOT flush the fresh frame to NVMe itself --
// same crash-window trade-off rowstore_alloc_page() already accepts (the
// page becomes "real" on NVMe only once vecstore_flush_page() is first
// called on it, matching this file's own new insert-path ordering below).
// Storage Isolation Roadmap Phase 1: quota-checked the same way rowstore_
// alloc_page() now is -- both subsystems share ONE combined per-partition
// page budget via storage_quota.c (identical 4 KiB page size), so this and
// rowstore_alloc_page() are simply the two callers into that one shared
// accounting layer. Denial happens before either cursor advances or any RAM
// frame is touched.
//
// Storage Isolation Roadmap Phase 3: the pool-exhaustion check is now per-
// partition, not global -- mirrors rowstore_alloc_page()'s own Phase 3
// rewrite exactly (see that function's header comment for the full
// reasoning, which applies here unchanged): partition_id's own reserved
// sub-range, real physical isolation instead of one shared elastic pool.
static uint32_t vecstore_alloc_page(uint32_t partition_id) {
    if (partition_id >= PARTITION_MAX) return VECSTORE_INVALID_PAGE;   // fail closed, same posture as storage_quota.c
    uint32_t range_end = (partition_id + 1) * VECSTORE_PAGES_PER_PARTITION;
    if (vecstore_partition_cursor[partition_id] >= range_end) return VECSTORE_INVALID_PAGE;   // this partition's own slice is full
    if (storage_page_reserve(partition_id)) return VECSTORE_INVALID_PAGE;   // over quota -- fail cleanly, cursor untouched
    uint32_t id = vecstore_partition_cursor[partition_id];

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) {
        storage_page_release(partition_id, 1);   // roll back the reservation -- this page never actually happened
        return VECSTORE_INVALID_PAGE;   // don't advance either cursor on failure
    }
    vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
    uint32_t invalid = VECSTORE_INVALID_PAGE;
    vs_memcpy(frame, &invalid, 4);
    vec_pages[id] = frame;

    vecstore_partition_cursor[partition_id]++;
    if (id + 1 > vecstore_next_free_page_id) vecstore_next_free_page_id = id + 1;   // keep the global high-water mark accurate
    return id;
}

// Gap Remediation Phase D: loads page_id into RAM (allocating a fresh frame
// first if needed), lazily reading it from NVMe if it's a previously-
// allocated page this boot hasn't touched yet -- mirrors rowstore.c's
// rowstore_load_page() (and, one level further back, stream.c's
// stream_lazy_load_frame()) exactly. Replaces the old, purely-RAM
// vecstore_page() accessor this file had through Phase 6.
static uint8_t* vecstore_load_page(uint32_t page_id) {
    if (page_id >= VECSTORE_MAX_PAGES) return 0;
    if (vec_pages[page_id]) return vec_pages[page_id];

    uint8_t* frame = (uint8_t*)allocate_physical_ram_frame();
    if (!frame) return 0;

    if (page_id < vecstore_next_free_page_id) {
        // Previously allocated (this boot or a prior one) -- may have real
        // data on NVMe. Errors are swallowed the same way rowstore.c's
        // identical path does: the caller sees whatever's in the zeroed
        // frame, detectable as "no valid entries" rather than a crash.
        //
        // Gap fix: guard with (io_sq && io_cq) exactly like stream.c's
        // identical check in stream_init() -- this file used to call
        // nvme_read_sync()/nvme_write_sync() unconditionally, even on a
        // boot where the NVMe I/O queue was never brought up (e.g. the
        // controller's MMIO BAR landed above the 4 GiB identity map --
        // see kernel.c's own boot-time branch, which skips
        // init_nvme_controller()/nvme_io_init() entirely in that case,
        // leaving io_sq/io_cq NULL and nvme_ctrl unpopulated). Without
        // this guard, nvme_io_submit_sync() dereferenced those NULLs and
        // rang a doorbell at a bogus address computed from a zeroed
        // nvme_ctrl.mmio_base -- nothing real ever acknowledged it, so
        // the completion-poll spun until NVME_IO_TIMEOUT before failing.
        // That timeout (this session's other fix) stopped it from
        // hanging the single-threaded HTTP loop forever, but every
        // insert/flush on an MMIO-above-4GiB boot was still guaranteed to
        // burn the full timeout for nothing. This guard restores the same
        // graceful "RAM-only this boot" degradation stream.c already had.
        if (io_sq && io_cq) {
            vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
            nvme_read_sync(VECSTORE_LBA_BASE + (uint64_t)page_id * 8, frame);
        } else {
            vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
        }
    } else {
        vs_memset(frame, 0, VECSTORE_PAGE_SIZE);
        uint32_t invalid = VECSTORE_INVALID_PAGE;
        vs_memcpy(frame, &invalid, 4);
    }
    vec_pages[page_id] = frame;
    return frame;
}

// Gap Remediation Phase D: mirrors rowstore.c's rowstore_flush_page().
// Gap fix: same (io_sq && io_cq) guard as vecstore_load_page() above -- see
// that function's comment for the full story. No-op (RAM-only, this boot)
// rather than a doomed write when the NVMe I/O queue never came up.
static void vecstore_flush_page(uint32_t page_id) {
    if (page_id >= VECSTORE_MAX_PAGES || !vec_pages[page_id]) return;
    if (!(io_sq && io_cq)) return;
    nvme_write_sync(VECSTORE_LBA_BASE + (uint64_t)page_id * 8, vec_pages[page_id]);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────
void vecstore_init(void) {
    vs_memset(vector_collections, 0, sizeof(vector_collections));
    vs_memset(vec_pages, 0, sizeof(vec_pages));
    vecstore_next_free_page_id = 0;
    // Storage Isolation Roadmap Phase 3: each partition's cursor starts at
    // the beginning of its OWN sub-range -- cold-start default, overwritten
    // by persist.c's restore block if a real Phase-3-format snapshot exists.
    for (uint32_t p = 0; p < PARTITION_MAX; p++) {
        vecstore_partition_cursor[p] = p * VECSTORE_PAGES_PER_PARTITION;
    }
    kernel_serial_print("[VECSTORE] Vector store engine initialised.\n");
}

int vecstore_create_collection(uint32_t caller_uid, const char* collection_name, uint32_t dimension) {
    if (dimension == 0 || dimension > VECSTORE_MAX_DIMENSION) return 1;

    int idx = -1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active) continue;
        if (vs_streq(object_catalog[i].name, collection_name)) { idx = (int)i; break; }
    }
    if (idx < 0) return 1;
    // VectorStore Gap Analysis §1.2 (closed): the one entry point in this
    // file that used to have no catalog_check_access() gate at all --
    // every other one (insert/get/delete/scan below) already gates on it.
    // PERM_WRITE, matching insert/delete's own level: promoting a catalog
    // object to a vector collection mutates it.
    if (!catalog_check_access(caller_uid, collection_name, PERM_WRITE)) return 2;
    if (vector_collections[idx].active) return 1;   // already a vector collection

    struct VecCollectionHeader* h = &vector_collections[idx];
    vs_memset(h, 0, sizeof(*h));
    h->object_id     = object_catalog[idx].object_id;
    h->active         = 1;
    h->dimension      = dimension;
    h->entry_width    = 1 + 8 + dimension * 4;   // tombstone + external_id + float[dimension]
    h->entries_per_page = (VECSTORE_PAGE_SIZE - 4) / h->entry_width;
    h->first_page_id   = VECSTORE_INVALID_PAGE;
    h->last_page_id     = VECSTORE_INVALID_PAGE;

    if (h->entries_per_page == 0) { vs_memset(h, 0, sizeof(*h)); return 1; }   // entry too wide for even one per page

    persist_vecstore_headers();   // Gap Remediation Phase D

    kernel_serial_printf(
        "[VECSTORE] '%s' enabled: dimension=%u, entry_width=%u, %u entr(y/ies)/page.\n",
        collection_name, h->dimension, h->entry_width, h->entries_per_page);
    return 0;
}

// ─── Entry (de)serialization ─────────────────────────────────────────────────
// Entry layout: [0]=tombstone/active byte, [1..8]=external_id (u64, raw
// bytes), [9..]=dimension floats (raw bytes) -- no text round-trip at all,
// unlike rowstore.c's serialize_row()/deserialize_row() (see vecstore.h's
// header comment on why the API boundary here is numeric, not text).
static void vs_write_entry(uint8_t* slot, uint64_t external_id, const struct VecValues* values, uint32_t dimension) {
    slot[0] = 1;
    vs_memcpy(slot + 1, &external_id, 8);
    vs_memcpy(slot + 9, values->values, dimension * 4);
}
static void vs_read_entry(const uint8_t* slot, uint64_t* out_external_id, struct VecValues* out, uint32_t dimension) {
    vs_memcpy(out_external_id, slot + 1, 8);
    out->count = dimension;
    vs_memcpy(out->values, slot + 9, dimension * 4);
}

// ─── VectorStore Gap Analysis §1.3: opt-in external_id uniqueness ─────────
// Internal helper -- walks every active (non-tombstoned) entry in a
// collection looking for a matching external_id, same physical (page, then
// slot) traversal vecstore_collection_scan() above already uses, but
// without a callback indirection or its own catalog_check_access() call
// (the caller -- vecstore_insert() below -- already gated access before
// this runs). Returns 1 on a hit, 0 if the whole collection was walked
// with no match.
static int vs_external_id_exists(struct VecCollectionHeader* h, uint64_t external_id) {
    uint32_t page_id = h->first_page_id;
    while (page_id != VECSTORE_INVALID_PAGE) {
        uint8_t* page = vecstore_load_page(page_id);
        if (!page) break;
        uint32_t next_page; vs_memcpy(&next_page, page, 4);
        uint32_t limit = (page_id == h->last_page_id) ? h->entries_in_last_page : h->entries_per_page;
        for (uint32_t s = 0; s < limit; s++) {
            uint8_t* slot = page + 4 + s * h->entry_width;
            if (!slot[0]) continue;   // tombstoned -- a deleted entry's external_id is free to reuse
            uint64_t ext_id;
            vs_memcpy(&ext_id, slot + 1, 8);
            if (ext_id == external_id) return 1;
        }
        page_id = next_page;
    }
    return 0;
}

int vecstore_set_unique_external_id(uint32_t caller_uid, const char* collection_name, int enabled) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    if (!catalog_check_access(caller_uid, collection_name, PERM_WRITE)) return 2;
    vector_collections[idx].unique_external_id = enabled ? 1 : 0;
    persist_vecstore_headers();   // matches every other header-mutating call's own persistence convention
    kernel_serial_printf("[VECSTORE] '%s' external_id uniqueness -> %s\n",
                         collection_name, enabled ? "ON" : "OFF");
    return 0;
}

// ─── Vector CRUD ─────────────────────────────────────────────────────────────
int vecstore_insert(uint32_t caller_uid, const char* collection_name,
                    uint64_t external_id, const struct VecValues* values,
                    struct VecId* out_id) {
    if (!collection_name || !values) return 3;
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_WRITE)) return 2;
    if (values->count != h->dimension) return 4;
    // VectorStore Gap Analysis §1.3 (closed): opt-in only -- a collection
    // that never called vecstore_set_unique_external_id() has this flag at
    // its zero-initialized default (off), so this branch never runs and
    // behavior is byte-for-byte unchanged from before this fix existed.
    if (h->unique_external_id && vs_external_id_exists(h, external_id)) return 6;

    if (h->first_page_id == VECSTORE_INVALID_PAGE ||
        h->entries_in_last_page >= h->entries_per_page) {
        uint32_t new_page = vecstore_alloc_page(object_catalog[idx].partition_id);
        if (new_page == VECSTORE_INVALID_PAGE) return 5;

        if (h->first_page_id == VECSTORE_INVALID_PAGE) {
            h->first_page_id = new_page;
        } else {
            uint8_t* prev = vecstore_load_page(h->last_page_id);
            if (prev) { vs_memcpy(prev, &new_page, 4); vecstore_flush_page(h->last_page_id); }
        }
        h->last_page_id = new_page;
        h->entries_in_last_page = 0;
        h->page_count++;
    }

    uint8_t* page = vecstore_load_page(h->last_page_id);
    if (!page) return 5;
    uint32_t slot_idx = h->entries_in_last_page;
    uint8_t* slot = page + 4 + slot_idx * h->entry_width;
    vs_write_entry(slot, external_id, values, h->dimension);
    vecstore_flush_page(h->last_page_id);   // Gap Remediation Phase D

    h->entries_in_last_page++;
    h->entry_count++;

    struct VecId new_id = { h->last_page_id, slot_idx };
    if (out_id) *out_id = new_id;
    persist_vecstore_headers();   // Gap Remediation Phase D

    // Phase 6: auto-maintenance -- see vec_index.h's own header comment on
    // why this call is unconditional here (vec_index_notify_insert() is
    // the one that decides whether any index actually needs updating,
    // mirroring row_index_notify_insert()'s identical role in
    // rowstore.c). Not separately access-gated -- catalog_check_access()
    // already ran above.
    vec_index_notify_insert(caller_uid, collection_name, new_id, external_id, values);
    return 0;
}

int vecstore_get(uint32_t caller_uid, const char* collection_name,
                 struct VecId id, uint64_t* out_external_id, struct VecValues* out) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_READ)) return 2;
    if (id.page_id >= vecstore_next_free_page_id) return 3;
    if (id.slot_index >= h->entries_per_page) return 3;

    uint8_t* page = vecstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->entry_width;
    if (!slot[0]) return 3;   // tombstoned / never written

    uint64_t ext_id;
    struct VecValues vals;
    vs_read_entry(slot, &ext_id, &vals, h->dimension);
    if (out_external_id) *out_external_id = ext_id;
    if (out) *out = vals;
    return 0;
}

int vecstore_delete(uint32_t caller_uid, const char* collection_name, struct VecId id) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 1;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_WRITE)) return 2;
    if (id.page_id >= vecstore_next_free_page_id) return 3;
    if (id.slot_index >= h->entries_per_page) return 3;

    uint8_t* page = vecstore_load_page(id.page_id);
    if (!page) return 3;
    uint8_t* slot = page + 4 + id.slot_index * h->entry_width;
    if (!slot[0]) return 3;   // already deleted / never written

    slot[0] = 0;   // tombstone -- slot is NOT reclaimed for reuse in this first cut
    vecstore_flush_page(id.page_id);   // Gap Remediation Phase D
    h->entry_count--;
    persist_vecstore_headers();        // Gap Remediation Phase D

    // Phase 6: auto-maintenance -- see the identical comment in
    // vecstore_insert() above.
    vec_index_notify_delete(collection_name, id);
    return 0;
}

// ─── Scan ────────────────────────────────────────────────────────────────────
uint32_t vecstore_collection_scan(uint32_t caller_uid, const char* collection_name,
                                  VecScanCb cb, void* ctx) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 0;
    struct VecCollectionHeader* h = &vector_collections[idx];
    if (!catalog_check_access(caller_uid, collection_name, PERM_READ)) return 0;

    uint32_t visited = 0;
    uint32_t page_id = h->first_page_id;
    while (page_id != VECSTORE_INVALID_PAGE) {
        uint8_t* page = vecstore_load_page(page_id);
        if (!page) break;
        uint32_t next_page; vs_memcpy(&next_page, page, 4);
        uint32_t limit = (page_id == h->last_page_id) ? h->entries_in_last_page : h->entries_per_page;

        for (uint32_t s = 0; s < limit; s++) {
            uint8_t* slot = page + 4 + s * h->entry_width;
            if (!slot[0]) continue;
            uint64_t ext_id;
            struct VecValues vals;
            vs_read_entry(slot, &ext_id, &vals, h->dimension);
            struct VecId id = { page_id, s };
            if (cb) cb(id, ext_id, &vals, ctx);
            visited++;
        }
        page_id = next_page;
    }
    return visited;
}

// ─── Phase 2: brute-force similarity search ─────────────────────────────────

// Newton-Raphson sqrt -- no libc math exists in this freestanding kernel
// (confirmed by direct audit before this phase started: no sqrt/newton/
// isqrt anywhere in the tree), matching every other numeric helper in this
// codebase's own hand-rolled-per-file convention. Globally convergent for
// any x > 0 from a positive starting guess; the starting guess below (1.0
// for x < 1, x itself otherwise) just shortens the number of iterations
// needed, it doesn't affect correctness. Bounded at 20 iterations with an
// early exit on convergence (float precision reaches its fixed point in
// under 10 iterations for any realistic embedding-magnitude input) rather
// than looping until exact equality, which float rounding can make loop
// forever for some inputs.
// Gap Remediation (post-roadmap x86 boot-build fix): every function on this
// page used to return float by value. The real x86_64-elf-gcc cross-compile
// (this project's Makefile x86-run target, using -mno-sse -mno-sse2 -mno-mmx
// -- deliberately disabled since the CR0.TS/#NM lazy-FPU-switch scaffolding
// in arch/x86/lazy_fpu.c is not actually wired up yet: no IDT vector 7 gate,
// no CR4.OSFXSR/OSXSAVE, no xsetbv anywhere in the tree) failed with "SSE
// register return with SSE disabled" the first time this project was ever
// actually built with its own real cross-compiler -- every previous phase's
// "syntax-check clean" claim used host gcc's default flags, which never
// exercised -mno-sse and so never could have caught this. Confirmed by
// direct experiment (see the roadmap's own Phase A gap) that on x86-64,
// -mno-sse only breaks float/double BY-VALUE RETURN -- float parameters and
// all-local float arithmetic still compile fine (GCC silently falls back to
// stack-passing for params and x87 for local math) -- so the fix is
// narrowly an out-parameter convention on every function that used to
// `return` a float, not a rewrite of the math itself. Internal arithmetic,
// precision, and behavior are all unchanged; only the calling convention is.
void vs_sqrtf(float* out, float x) {
    if (x <= 0.0f) { *out = 0.0f; return; }
    float guess = (x < 1.0f) ? 1.0f : x;
    for (int i = 0; i < 20; i++) {
        float next = 0.5f * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    *out = guess;
}

void vs_dot(float* out, const struct VecValues* a, const struct VecValues* b, uint32_t n) {
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++) s += a->values[i] * b->values[i];
    *out = s;
}

// 1.0f - cosine similarity -- see vecstore.h's header comment on why this
// (not raw similarity) is what this file calls "cosine distance."
void vs_distance_cosine(float* out, const struct VecValues* a, const struct VecValues* b, uint32_t n) {
    float aa, bb, ab;
    vs_dot(&aa, a, a, n);
    vs_dot(&bb, b, b, n);
    float na, nb;
    vs_sqrtf(&na, aa);
    vs_sqrtf(&nb, bb);
    if (na == 0.0f || nb == 0.0f) { *out = 1.0f; return; }   // undefined direction -- neutral, not min or max
    vs_dot(&ab, a, b, n);
    float sim = ab / (na * nb);
    if (sim > 1.0f) sim = 1.0f;     // clamp -- float rounding can push a tiny bit outside [-1,1]
    if (sim < -1.0f) sim = -1.0f;
    *out = 1.0f - sim;
}

void vs_distance_l2(float* out, const struct VecValues* a, const struct VecValues* b, uint32_t n) {
    float s = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float d = a->values[i] - b->values[i];
        s += d * d;
    }
    vs_sqrtf(out, s);
}

// Bounded top-K insertion: out[0..*found) stays sorted ascending by
// distance at all times. O(k) worst case per candidate (a shift, not a
// full re-sort) -- see vecstore.h's header comment on the O(n*k) vs.
// O(n*log k) trade-off this makes deliberately.
void vs_topk_insert(struct VecMatch* out, uint32_t* found, uint32_t k, struct VecMatch cand) {
    if (*found < k) {
        uint32_t pos = *found;
        while (pos > 0 && out[pos - 1].distance > cand.distance) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = cand;
        (*found)++;
    } else if (k > 0 && cand.distance < out[k - 1].distance) {
        uint32_t pos = k - 1;
        while (pos > 0 && out[pos - 1].distance > cand.distance) {
            out[pos] = out[pos - 1];
            pos--;
        }
        out[pos] = cand;
    }
    // else: candidate is worse than every match already kept -- discarded,
    // matching a bounded top-K's whole point (never grows past k).
}

struct vs_search_ctx {
    const struct VecValues* query;
    VecMetric                metric;
    uint32_t                 dimension;
    uint32_t                 k;
    struct VecMatch*         out;
    uint32_t                 found;
};

static void vs_search_cb(struct VecId id, uint64_t external_id, const struct VecValues* v, void* ctxp) {
    struct vs_search_ctx* ctx = (struct vs_search_ctx*)ctxp;
    float dist;
    if (ctx->metric == VEC_METRIC_L2) vs_distance_l2(&dist, ctx->query, v, ctx->dimension);
    else                              vs_distance_cosine(&dist, ctx->query, v, ctx->dimension);
    struct VecMatch cand = { external_id, id, dist };
    vs_topk_insert(ctx->out, &ctx->found, ctx->k, cand);
}

uint32_t vecstore_search(uint32_t caller_uid, const char* collection_name,
                         const struct VecValues* query, VecMetric metric,
                         uint32_t k, struct VecMatch* out) {
    if (!query || !out || k == 0) return 0;
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 0;
    if (query->count != vector_collections[idx].dimension) return 0;

    struct vs_search_ctx ctx = { query, metric, vector_collections[idx].dimension, k, out, 0 };
    // Reuses vecstore_collection_scan()'s own catalog_check_access() gate
    // and page-chain traversal -- no parallel permission path or duplicate
    // scan logic, matching this codebase's established "one real choke
    // point" convention (predicate_table_scan/rowstore_table_scan's own
    // relationship in the RDBMS roadmap is the direct precedent).
    vecstore_collection_scan(caller_uid, collection_name, vs_search_cb, &ctx);
    return ctx.found;
}

// ─── Vector Store Roadmap Phase 4: syscall adapters ("make it live") ─────
// Thin, one-line-of-real-logic adapters -- see vecstore.h's own comment on
// why sys_sls_vec_embed_insert() below is the one place this file includes
// ollama_client.h (already pulled in transitively via vecstore.h), and why
// SYS_SLS_VEC_CREATE exists even though the roadmap's own §6 scope bullet
// only named insert/embed-insert/search.

uint64_t sys_sls_vec_create(struct SLSVecCreateRequest* req) {
    if (!req) return 1;
    req->status = vecstore_create_collection(req->caller_uid, req->collection_name, req->dimension);
    return (uint64_t)req->status;
}

// VectorStore Gap Analysis §1.3 follow-on: same thin-adapter shape as every
// other syscall on this page.
uint64_t sys_sls_vec_set_unique(struct SLSVecSetUniqueRequest* req) {
    if (!req) return 1;
    req->status = vecstore_set_unique_external_id(req->caller_uid, req->collection_name, req->enabled);
    return (uint64_t)req->status;
}

uint64_t sys_sls_vec_insert(struct SLSVecInsertRequest* req) {
    if (!req) return 1;
    req->status = vecstore_insert(req->caller_uid, req->collection_name,
                                  req->external_id, &req->values, &req->out_id);
    return (uint64_t)req->status;
}

// Embeds req->ollama_req.prompt via a real network round-trip to Ollama,
// then stores the resulting embedding -- two independent failure points,
// reported separately (ollama_status vs. insert_status) rather than
// collapsed into one code, so a caller can tell "Ollama never answered"
// apart from "Ollama answered fine but the collection/dimension was
// wrong" -- the same "don't let denial look like absence" discipline named
// in this struct's own header comment, applied to failure REPORTING this
// time rather than reachability.
uint64_t sys_sls_vec_embed_insert(struct SLSVecEmbedInsertRequest* req) {
    if (!req) return 1;

    struct OllamaEmbedResponse resp;
    req->ollama_status = ollama_embed(&req->ollama_req, &resp);
    if (req->ollama_status != 0) {
        req->insert_status = -1;   // never attempted -- distinct from any real vecstore_insert() code (0-5)
        return 1;
    }

    // resp.dimension is already bounded by OLLAMA_MAX_EMBED_DIM, which
    // equals VECSTORE_MAX_DIMENSION (independently declared in each
    // header -- see ollama_client.h's own comment on why they don't share
    // one) -- this cap is belt-and-suspenders, not expected to trigger.
    uint32_t n = resp.dimension;
    if (n > VECSTORE_MAX_DIMENSION) n = VECSTORE_MAX_DIMENSION;

    struct VecValues values;
    values.count = n;
    for (uint32_t i = 0; i < n; i++) values.values[i] = resp.embedding[i];

    req->insert_status = vecstore_insert(req->caller_uid, req->collection_name,
                                         req->external_id, &values, &req->out_id);
    return (uint64_t)req->insert_status;
}

uint64_t sys_sls_vec_search(struct SLSVecSearchRequest* req) {
    if (!req) return 1;

    uint32_t k = req->k;
    req->truncated = (k > VEC_SEARCH_MAX_K) ? 1 : 0;
    if (k > VEC_SEARCH_MAX_K) k = VEC_SEARCH_MAX_K;

    // vecstore_search()'s own documented ambiguity (0 means either "ran
    // and found nothing" or "couldn't run at all") is deliberately
    // preserved here rather than "fixed" at the syscall boundary -- see
    // vecstore_search()'s own header comment for why that's an accepted,
    // named tradeoff, not an oversight. This adapter always returns
    // syscall-success (0); match_count carries the real signal.
    req->match_count = vecstore_search(req->caller_uid, req->collection_name,
                                       &req->query, req->metric, k, req->matches);
    return 0;
}

// ─── VectorStore Interface Roadmap Phase 2: semantic (embed-then-search) ──
// Same embed-first shape as sys_sls_vec_embed_insert() above -- embed
// req->ollama_req.prompt via a real network round-trip, then, only if that
// succeeded, call straight into the already-tested vecstore_search() with
// the resulting vector. Deliberately does NOT reuse
// sys_sls_vec_embed_insert()'s own body: the two adapters share the same
// six-line embed-and-convert prefix by construction (both were written by
// copying this shape, matching this roadmap's own Phase 2 scope note that
// this is "copy-the-pattern-once, not restructure-existing-code" since no
// shared helper currently exists to extract into), not by calling each
// other or a common function -- introducing one now for two five-line call
// sites would be more indirection than the duplication it removes.
uint64_t sys_sls_vec_embed_search(struct SLSVecEmbedSearchRequest* req) {
    if (!req) return 1;

    struct OllamaEmbedResponse resp;
    req->ollama_status = ollama_embed(&req->ollama_req, &resp);
    if (req->ollama_status != 0) {
        req->match_count = 0;   // never attempted -- distinct from "attempted, found nothing"
        return 1;
    }

    uint32_t n = resp.dimension;
    if (n > VECSTORE_MAX_DIMENSION) n = VECSTORE_MAX_DIMENSION;

    struct VecValues query;
    query.count = n;
    for (uint32_t i = 0; i < n; i++) query.values[i] = resp.embedding[i];

    uint32_t k = req->k;
    req->truncated = (k > VEC_SEARCH_MAX_K) ? 1 : 0;
    if (k > VEC_SEARCH_MAX_K) k = VEC_SEARCH_MAX_K;

    // Same documented 0-is-ambiguous match_count contract sys_sls_vec_search()
    // already carries, deliberately preserved rather than "fixed" here --
    // ollama_status is what separates "Ollama never answered" from "Ollama
    // answered fine, the search itself just found nothing," so this adapter
    // still always returns syscall-success (0) once embedding succeeded.
    req->match_count = vecstore_search(req->caller_uid, req->collection_name,
                                       &query, req->metric, k, req->matches);
    return 0;
}

// ─── VectorStore Interface Roadmap Phase 1: single-vector delete ──────────
// Thin adapter, same shape as every other syscall wrapper in this file --
// one line into the already-implemented vecstore_delete() (see this file's
// vecstore_delete() above, which already does the real work: tombstone,
// flush, persist, and vec_index_notify_delete() to keep any HNSW index
// consistent).
uint64_t sys_sls_vec_delete(struct SLSVecDeleteRequest* req) {
    if (!req) return 1;
    req->status = vecstore_delete(req->caller_uid, req->collection_name, req->id);
    return (uint64_t)req->status;
}

// ─── VectorStore Interface Roadmap Phase 1: vfree-time cleanup ────────────
// Called from object_catalog.c's sys_sls_vfree()/catalog_vfree_partition()
// BEFORE either sets the catalog object's .active = 0 -- find_active_
// collection() above requires object_catalog[i].active to still be true to
// find the collection by name, so this must run first or it silently finds
// nothing and leaks exactly the bug this function exists to close (see
// docs/AeroSLS-VectorStore-Interface-Roadmap-v0.1.md Phase 1 for the full
// investigation: confirmed via grep that object_catalog.c never touched
// vector_collections[]/vec_indexes[] at all before this).
//
// A genuine no-op (not an error) if collection_name was never a vector
// collection -- vfree() is the one generic "delete this object" path for
// every object type in this kernel, so it must call this unconditionally,
// and most objects freed are not vector collections at all.
//
// Declared here (not in vecstore.h) and forward-declared via a bare
// `extern` in object_catalog.c instead -- matches this codebase's own
// established convention for this exact kind of "notify a higher-layer
// subsystem" call (see object_catalog.c's own tier_notify_access() extern,
// tier_mgr.c). Avoids a circular header dependency too: vecstore.h already
// includes object_catalog.h (the correct direction), so object_catalog.c/h
// must not include vecstore.h back.
void vecstore_notify_object_freed(const char* collection_name) {
    int idx = find_active_collection(collection_name);
    if (idx < 0) return;   // not a vector collection -- nothing to do

    vector_collections[idx].active = 0;
    persist_vecstore_headers();   // Gap Remediation Phase D

    // Deactivate any HNSW index built over this collection too, same
    // "don't leave a dangling reference" reasoning -- see vec_index.c's
    // own vec_index_notify_collection_freed() for the node-tombstone loop.
    vec_index_notify_collection_freed(collection_name);
}

// ─── Gap Remediation Phase C: collection enumeration ────────────────────────
// Mirrors sys_sls_obj_list()'s own shape exactly (object_catalog.c) -- see
// vecstore.h's own comment on this syscall.
void sys_sls_vec_list(void) {
    // kernel_serial_print() takes exactly one argument (const char*, no
    // format processing at all -- kernel_io.c's own definition) --
    // kernel_serial_printf() is the variadic one. Worth noting: this file's
    // own established precedent, sys_sls_obj_list() (object_catalog.c),
    // calls kernel_serial_print() with a format string PLUS several extra
    // string arguments for its header row -- a real, pre-existing bug (see
    // that file's own Gap Remediation Phase C fix), not a pattern to copy.
    kernel_serial_printf(
        "\n[VECSTORE] Collection Directory\n"
        " %-24s %-6s %-8s %s\n"
        " %-24s %-6s %-8s %s\n",
        "Name", "Dim", "Entries", "Pages",
        "------------------------", "------", "--------", "-----");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active || !vector_collections[i].active) continue;
        struct VecCollectionHeader* h = &vector_collections[i];
        kernel_serial_printf(" %-24s %-6u %-8u %u\n",
                             object_catalog[i].name, h->dimension, h->entry_count, h->page_count);
        shown++;
    }
    if (!shown) kernel_serial_print(" (no vector collections defined)\n");
}

// ─── VectorStore Interface Roadmap follow-on: bulk vector DATA export/
// import. See vecstore.h's own header comment for the full "why this
// format, why per-collection, why 8192 is genuinely tight, auto-indexing-
// for-free, and the inherited external_id-dedup gap" writeup. ────────────

static int vs_append(char* buf, uint32_t* pos, uint32_t max, const char* src) {
    uint32_t i = 0;
    while (src[i]) {
        if (*pos + 1 >= max) return 0;
        buf[(*pos)++] = src[i++];
    }
    buf[*pos] = '\0';
    return 1;
}

static void vs_u64_to_str(uint64_t v, char* buf, uint32_t max) {
    char tmp[24]; uint32_t l = 0;
    if (!v) tmp[l++] = '0';
    else while (v) { tmp[l++] = (char)('0' + (v % 10)); v /= 10; }
    uint32_t n = 0;
    for (int i = (int)l - 1; i >= 0 && n < max - 1; i--) buf[n++] = tmp[i];
    buf[n] = '\0';
}

static void vs_strcpy(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0; for (; i < max - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}

// Fixed 6-decimal-place formatter -- a fresh vecstore.c-local copy of
// rowstore.c's rs_f64_to_str(), matching this codebase's established
// "each file keeps its own small helpers" convention (predicate.c's own
// separate pe_parse_f64 copy is the direct precedent for why this isn't
// extracted/shared instead), adapted for float (this file's native
// vector-component type -- see this file's own VecValues comment on why
// float not double) rather than double. Not a general float printer --
// just enough to round-trip a value written via vs_parse_f32() below.
static void vs_f32_to_str(float v, char* buf, uint32_t max) {
    uint32_t n = 0;
    if (v < 0) { if (n < max - 1) buf[n++] = '-'; v = -v; }
    uint64_t ip = (uint64_t)v;
    float frac = v - (float)ip;
    char ipbuf[24];
    vs_u64_to_str(ip, ipbuf, sizeof(ipbuf));
    for (uint32_t i = 0; ipbuf[i] && n < max - 1; i++) buf[n++] = ipbuf[i];
    if (n < max - 1) buf[n++] = '.';
    for (int d = 0; d < 6 && n < max - 1; d++) {
        frac *= 10.0f;
        int digit = (int)frac;
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        buf[n++] = (char)('0' + digit);
        frac -= (float)digit;
    }
    buf[n] = '\0';
}

// Mirrors rowstore.c's own rs_parse_f64() exactly, just at float
// precision -- no exponent/scientific-notation support, matching that
// function's own documented scope.
static int vs_parse_f32(const char* s, float* out) {
    if (!s || !s[0]) return 1;
    uint32_t i = 0;
    int neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    if (!s[i]) return 1;
    float v = 0.0f;
    int saw_digit = 0;
    for (; s[i] >= '0' && s[i] <= '9'; i++) { v = v * 10.0f + (float)(s[i] - '0'); saw_digit = 1; }
    if (s[i] == '.') {
        i++;
        float frac = 0.1f;
        for (; s[i] >= '0' && s[i] <= '9'; i++) { v += (float)(s[i] - '0') * frac; frac *= 0.1f; saw_digit = 1; }
    }
    if (s[i] != '\0' || !saw_digit) return 1;
    *out = neg ? -v : v;
    return 0;
}

static int vs_parse_u64(const char* s, uint64_t* out) {
    if (!s || !s[0]) return 1;
    uint64_t v = 0;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

// Bounded-by-explicit-end-index tokenizer -- unlike vec_index.c's own
// vse_next_token() (which walks a NUL-terminated single-line buffer), this
// one tokenizes directly out of the caller's full multi-line `text`
// between [pos, end), so vec_data_import() below never needs to copy a
// whole line into a local stack buffer first. That copy would otherwise
// have to be sized for a worst-case VECTOR line (up to VECSTORE_MAX_
// DIMENSION=2048 floats), a real stack-budget risk in the actual kernel
// build -- see vecstore.h's own header comment on why VEC_DATA_EXPORT_
// MAX_LEN itself is deliberately small for the same underlying reason.
static uint32_t vs_next_token_bounded(const char* text, uint32_t pos, uint32_t end,
                                      char* outbuf, uint32_t outmax) {
    while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) pos++;
    uint32_t o = 0;
    while (pos < end && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\r' && text[pos] != '\n') {
        if (o + 1 < outmax) outbuf[o++] = text[pos];
        pos++;
    }
    outbuf[o] = '\0';
    return pos;
}

struct vd_export_ctx {
    char*       buf;
    uint32_t*   pos;
    uint32_t    max;
    const char* collection_name;
    uint32_t    written;
    uint32_t    total;
    int         stop;   // 1 once the buffer is full -- avoids repeatedly retrying a doomed append
    // VectorStore Gap Analysis §1.4 (closed): the first `skip` entries this
    // scan visits are counted into `total` exactly like every other entry,
    // but never serialized -- see vec_data_export()'s own header comment
    // and vecstore.h's matching one for the full resumption-across-calls
    // design this implements.
    uint32_t    skip;
};

// Builds one "VECTOR <collection> <external_id> <v0> ... <v(n-1)>\n" line
// directly into ctx->buf (no intermediate whole-line stack buffer -- see
// vs_next_token_bounded()'s own comment on why that matters here). If the
// line doesn't fully fit, rolls *ctx->pos back to where the line started
// so a half-written vector is never left in the output -- the output
// buffer's own bytes past the last successfully written line are always a
// clean, complete '\0'-terminated prefix.
static void vd_export_cb(struct VecId id, uint64_t external_id, const struct VecValues* values, void* ctxp) {
    (void)id;
    struct vd_export_ctx* ctx = (struct vd_export_ctx*)ctxp;
    ctx->total++;
    if (ctx->stop) return;   // buffer already full -- vectors_total above still stays honest
    if (ctx->total <= ctx->skip) return;   // §1.4: still within the skipped prefix -- counted, not written

    uint32_t line_start = *ctx->pos;
    int ok = vs_append(ctx->buf, ctx->pos, ctx->max, "VECTOR ") &&
             vs_append(ctx->buf, ctx->pos, ctx->max, ctx->collection_name) &&
             vs_append(ctx->buf, ctx->pos, ctx->max, " ");
    if (ok) {
        char idbuf[24];
        vs_u64_to_str(external_id, idbuf, sizeof(idbuf));
        ok = vs_append(ctx->buf, ctx->pos, ctx->max, idbuf);
    }
    for (uint32_t i = 0; ok && i < values->count; i++) {
        char fbuf[32];
        vs_f32_to_str(values->values[i], fbuf, sizeof(fbuf));
        ok = vs_append(ctx->buf, ctx->pos, ctx->max, " ") &&
             vs_append(ctx->buf, ctx->pos, ctx->max, fbuf);
    }
    ok = ok && vs_append(ctx->buf, ctx->pos, ctx->max, "\n");

    if (!ok) {
        *ctx->pos = line_start;      // roll back this line's partial bytes
        ctx->buf[*ctx->pos] = '\0';
        ctx->stop = 1;
        return;
    }
    ctx->written++;
}

uint32_t vec_data_export(uint32_t caller_uid, const char* collection_name,
                         uint32_t skip_count, char* out, uint32_t max,
                         struct VecDataExportResult* result) {
    if (result) {
        result->bytes_written = 0; result->vectors_written = 0; result->vectors_total = 0;
        result->truncated = 0; result->entries_remaining = 0;
    }
    if (!out || max == 0) return 0;
    out[0] = '\0';
    if (!collection_name) return 0;

    // Explicit pre-check, purely so a nonexistent/denied collection
    // returns a clean empty export rather than attempting the header-
    // comment append first -- cosmetic, not a correctness difference
    // (vecstore_collection_scan() below re-checks catalog_check_access()
    // regardless, matching this codebase's "one real choke point"
    // convention -- the same ambiguity vecstore_search()'s own header
    // comment already names applies here too: 0 vectors_written can mean
    // either "no such collection/denied" or "genuinely empty").
    int idx = find_active_collection(collection_name);
    if (idx < 0) return 0;
    if (!catalog_check_access(caller_uid, collection_name, PERM_READ)) return 0;

    uint32_t pos = 0;
    if (!vs_append(out, &pos, max, "# vector-store data export -- collection ") ||
        !vs_append(out, &pos, max, collection_name) ||
        !vs_append(out, &pos, max, "\n")) {
        if (result) { result->bytes_written = pos; result->truncated = 1; }
        return pos;
    }

    struct vd_export_ctx ctx = { out, &pos, max, collection_name, 0, 0, 0, skip_count };
    vecstore_collection_scan(caller_uid, collection_name, vd_export_cb, &ctx);

    if (result) {
        result->bytes_written   = pos;
        result->vectors_written = ctx.written;
        result->vectors_total   = ctx.total;
        // §1.4: accounts for skip_count now, not just this call's own
        // vectors_written -- "how much of the WHOLE collection has this
        // call plus everything before it covered."
        uint32_t covered = skip_count + ctx.written;
        result->entries_remaining = (covered < ctx.total) ? (ctx.total - covered) : 0;
        result->truncated       = (result->entries_remaining > 0) ? 1 : 0;
    }
    return pos;
}

void vec_data_import(uint32_t caller_uid, const char* text, struct VecDataImportResult* out) {
    if (!out) return;
    out->total = 0; out->succeeded = 0; out->failed = 0;
    for (uint32_t i = 0; i < VEC_DATA_IMPORT_MAX_LINES; i++) {
        out->lines[i].ok = 0;
        out->lines[i].offset = 0;
        out->lines[i].error_msg[0] = '\0';
    }
    if (!text) return;

    uint32_t i = 0;
    while (text[i]) {
        uint32_t line_start = i;
        uint32_t j = i;
        while (text[j] && text[j] != '\n') j++;
        uint32_t line_end = j;   // exclusive -- text[line_start..line_end) excludes the trailing '\n'
        i = text[j] ? j + 1 : j;

        uint32_t p = line_start;
        while (p < line_end && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r')) p++;
        if (p >= line_end || text[p] == '#') continue;   // blank or comment line -- not counted

        out->total++;
        uint32_t slot = out->total - 1;
        struct VecDataImportLineResult* lr =
            (slot < VEC_DATA_IMPORT_MAX_LINES) ? &out->lines[slot] : 0;
        if (lr) lr->offset = line_start;

        char kw[16];
        p = vs_next_token_bounded(text, p, line_end, kw, sizeof(kw));

        int ok = 0;
        const char* err = "";

        if (!vs_streq(kw, "VECTOR")) {
            err = "unrecognized line (expected a line starting with VECTOR)";
        } else {
            char coll[OBJECT_NAME_LEN], idtok[24];
            p = vs_next_token_bounded(text, p, line_end, coll, sizeof(coll));
            p = vs_next_token_bounded(text, p, line_end, idtok, sizeof(idtok));
            uint64_t external_id = 0;
            if (coll[0] == '\0' || idtok[0] == '\0' || vs_parse_u64(idtok, &external_id)) {
                err = "malformed VECTOR line (expected: VECTOR <collection> <external_id> <v0> <v1> ...)";
            } else {
                struct VecValues values;
                values.count = 0;
                int bad = 0;
                for (;;) {
                    char tok[32];
                    uint32_t next_p = vs_next_token_bounded(text, p, line_end, tok, sizeof(tok));
                    if (tok[0] == '\0') break;   // no more tokens on this line
                    p = next_p;
                    if (values.count >= VECSTORE_MAX_DIMENSION) { bad = 1; break; }
                    float f;
                    if (vs_parse_f32(tok, &f)) { bad = 1; break; }
                    values.values[values.count++] = f;
                }
                if (bad) {
                    err = "malformed vector component (not a valid number, or more components than VECSTORE_MAX_DIMENSION)";
                } else if (values.count == 0) {
                    err = "VECTOR line has no vector components";
                } else {
                    struct VecId out_id;
                    int rc = vecstore_insert(caller_uid, coll, external_id, &values, &out_id);
                    switch (rc) {
                        case 0: ok = 1; break;
                        case 1: err = "collection not found (import schema definitions first, or check the name)"; break;
                        case 2: err = "permission denied"; break;
                        case 4: err = "dimension mismatch: this line's component count doesn't match the collection's dimension"; break;
                        case 5: err = "vecstore_insert() failed: page pool exhausted"; break;
                        case 6: err = "duplicate external_id rejected (collection has uniqueness enabled, VectorStore Gap Analysis §1.3)"; break;
                        default: err = "vecstore_insert() failed"; break;
                    }
                }
            }
        }

        if (lr) { lr->ok = (uint8_t)ok; vs_strcpy(lr->error_msg, err, sizeof(lr->error_msg)); }
        if (ok) out->succeeded++; else out->failed++;
    }
}

uint64_t sys_sls_vec_data_export(struct SLSVecDataExportRequest* req) {
    if (!req) return 1;
    vec_data_export(req->caller_uid, req->collection_name, req->skip_count, req->out, sizeof(req->out), &req->result);
    return 0;
}

uint64_t sys_sls_vec_data_import(struct SLSVecDataImportRequest* req) {
    if (!req) return 1;
    vec_data_import(req->caller_uid, req->text, &req->result);
    return req->result.failed == 0 ? 0 : 1;
}