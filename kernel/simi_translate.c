/*
 * simi_translate.c — Phase 3 kernel-side glue: runs simi_x86_translate()
 * (kernel/simi_x86.c, byte-identical to the host toolchain's copy — see
 * AeroSLS-SIMI-ISA-v0.1.md §9) against an uploaded SIMI ServiceBinary and
 * maps the result into a process's page table, exactly the way
 * loader_load_into_process() already maps ELF64 and flat binaries.
 *
 * Phase 4 adds an activation cache on top of the Phase 3 translator,
 * modeled on System/38's CREATE PROGRAM/activation split: translating a
 * SIMI object to native code happens at most once per distinct object
 * content, not once per spawn. Every subsequent activation
 * (program_spawn() call) reuses the already-translated code pages instead
 * of re-running simi_x86_translate().
 *
 * Why it's safe to share physical code frames across processes, not just
 * cache bytes to re-copy: OBJ_TYPE_PROGRAM objects are always mapped at the
 * SAME fixed virtual address in every process that spawns them
 * (USER_PROC_CODE_BASE, see process.c's program_spawn()) — that's what
 * single-level storage actually buys here, an object's effective address
 * is invariant regardless of which process is looking at it. Because
 * base_vaddr is identical across every activation, the scratch page's
 * virtual address (base_vaddr + code_pages*4096) is too, so the r7
 * scratch-pointer movabs immediate already baked into the shared
 * trampoline code (see simi_x86.c / AeroSLS-SIMI-ISA-v0.1.md §10) is
 * correct for every process without re-translating. The one thing that
 * must NOT be shared is the scratch page's physical backing — SIMI code
 * writes through r7, so each activation gets its own fresh, private,
 * zeroed scratch frame even on a cache hit. Only the code pages (read +
 * exec, never written at runtime) are shared.
 *
 * Cache invalidation: keyed by object_name plus an FNV-1a hash of the
 * uploaded .tmo bytes. A re-upload of the same object name with different
 * content is detected (hash/size mismatch) and forces a retranslation.
 *
 * Gap Remediation SIMI Phase 13: the old cached frames ARE reclaimed now —
 * freed via free_physical_ram_frame() (kernel/frame_pool.c) right after a
 * re-translation succeeds and right before the new frames overwrite the
 * slot. See the "Gap Remediation SIMI Phase 13" comment block below for the
 * exact placement rationale (free only after translation has fully
 * succeeded, so a failed re-translation attempt never destroys a still-good
 * cached activation). Design/rationale: docs/AeroSLS-SIMI-ISA-v0.1.md §16.
 *
 * v1 limitation carried over from Phase 3: only the "main" entry point is
 * spawnable. Threading a caller-chosen entry name through
 * program_spawn()/loader_load_into_process() would touch several existing
 * call sites for no test-suite benefit yet — every SIMI object in this
 * project (Phase 1 test suite, Phase 3 host JIT tests) uses `.entry main`
 * by convention, same as a C program's `main`. Revisit if/when a real
 * multi-entry-point use case shows up.
 */
#include "simi_translate.h"
#include "simi_x86.h"
#include "simi_runtime.h"      /* Phase 6 (v0.3): real RESOLVE/OBJSIZE/OBJTYPE bindings */
#include "loader.h"
#include "kernel_io.h"
#include "process.h"           /* SYS_SLS_EXIT, PROC_NAME_LEN */
#include "../arch/x86/user_paging.h"

extern void* allocate_physical_ram_frame(void);
extern int   free_physical_ram_frame(void* frame);   /* Gap Remediation SIMI Phase 13 */

/* Output buffer for translated native code. Sized generously relative to
 * LOADER_MAX_BINARY_SIZE (16 KiB of SIMI bytecode): the naive
 * load-operate-store codegen expands roughly 5-10x per the Phase 3 host
 * verification suite, so 16 KiB in could plausibly produce on the order of
 * 128-160 KiB out in a worst case. A single static buffer, reused per
 * translate call — matches persist.c's p_buf and mirrors the fact that
 * this kernel has no general-purpose allocator (see kernel/loader.c's
 * ServiceBinary, also a fixed static array). */
#define SIMI_CODE_BUF_SIZE (192 * 1024)
static uint8_t g_simi_code_buf[SIMI_CODE_BUF_SIZE];

/* ── Phase 4: activation cache ──────────────────────────────────────────── */
/* One slot per distinct SIMI object name — mirrors MAX_SERVICE_BINARIES
 * since there is at most one live activation per uploaded SIMI object.
 * Each slot holds the physical frames of a completed translation (code +
 * appended exit stub) so a later spawn of the same object skips straight
 * to page-table mapping instead of re-translating. */
#define SIMI_MAX_ACTIVATIONS   MAX_SERVICE_BINARIES
#define SIMI_ACT_MAX_PAGES     (SIMI_CODE_BUF_SIZE / 4096)   /* 48 pages */

struct SimiActivation {
    char     object_name[PROC_NAME_LEN];   /* [0] == '\0' means unused slot */
    uint8_t  valid;
    // Phase 14c (LPAR eager-free SIMI cache story): 1 = this slot's frames
    // are held only for its live mappers, not for future cache hits — set
    // by simi_vfree_object() on an object whose activation is still mapped
    // by a live process. A retired slot keeps its object_name (for logs /
    // diagnostics) but tt_find_activation()/simi_activation_query() skip
    // it, so it can never serve a future spawn of the same name. When the
    // LAST mapper's teardown drops mappers to 0, the frames are freed
    // immediately (see simi_frame_is_cached()); the owning partition's
    // destroy frees any that outlive every mapper.
    uint8_t  retired;
    // Phase 14c: number of live processes currently mapping this
    // activation's shared code frames. Incremented on every successful
    // simi_translate_and_map() (MISS and HIT — each spawn maps the code
    // pages), decremented exactly once per process per activation by the
    // per-process page-table teardown walker (simi_frame_is_cached()'s
    // `seen` bitmap), which is also where mappers==0 && retired triggers
    // the eager frame free. A VALID, non-retired activation sits at
    // mappers==0 between spawns — that is the cache's resting state, NOT
    // a free trigger; only retired slots are freed on 0.
    uint32_t mappers;
    // Phase 14b (LPAR destroy-time SIMI cache story): the partition that
    // owns this activation, captured at translation time from the
    // ServiceBinary's partition_id (loader.c re-tags that field from the
    // catalog entry on EVERY upload — see the Phase 14a comment there),
    // so the tag always equals the catalog partition of the object that
    // produced this translation. Re-tagged on every cache hit too: a
    // retired slot (simi_vfree_object) reused by a same-content re-upload
    // from a new partition must carry the NEW partition so the OLD
    // partition's destroy can't free frames the new owner maps. Consulted
    // by simi_vfree_partition() to free a partition's activations at
    // partition_destroy() time.
    uint32_t partition_id;
    uint32_t content_hash;
    uint32_t content_size;
    uint32_t code_pages;
    uint32_t entry_off;
    uint64_t frame[SIMI_ACT_MAX_PAGES];    /* physical addrs; page i maps at base_vaddr+i*4096 */
};

static struct SimiActivation g_activations[SIMI_MAX_ACTIVATIONS];

static void tt_memset(void* p, uint8_t v, uint32_t n) {
    uint8_t* b = (uint8_t*)p; while (n--) *b++ = v;
}
static void tt_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    while (n--) *dd++ = *ss++;
}
static int tt_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void tt_strcpy(char* d, const char* s, uint32_t cap) {
    uint32_t i; for (i = 0; i + 1 < cap && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}

/* FNV-1a, 32-bit. Not cryptographic — this only needs to catch "did the
 * uploaded bytes for this object name change since the last translation",
 * not resist a deliberate collision. There's no cross-privilege attacker
 * being defended against here: an SLS object's owner already controls its
 * own object's bytes. */
static uint32_t tt_fnv1a(const uint8_t* data, uint32_t len) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

static struct SimiActivation* tt_find_activation(const char* object_name) {
    for (int i = 0; i < SIMI_MAX_ACTIVATIONS; i++) {
        if (g_activations[i].object_name[0] && !g_activations[i].retired &&
            tt_streq(g_activations[i].object_name, object_name)) {
            return &g_activations[i];
        }
    }
    return 0;
}

static struct SimiActivation* tt_find_or_alloc_activation(const char* object_name) {
    struct SimiActivation* act = tt_find_activation(object_name);
    if (act) return act;
    for (int i = 0; i < SIMI_MAX_ACTIVATIONS; i++) {
        /* Phase 14c: a retired slot keeps its object_name, so it fails the
         * name-empty check anyway; the !retired guard is belt-and-suspenders
         * so a retired slot can never be recycled into a live cache entry
         * (its frames belong to the retiring object's mappers). */
        if (!g_activations[i].object_name[0] && !g_activations[i].retired) {
            tt_strcpy(g_activations[i].object_name, object_name, PROC_NAME_LEN);
            return &g_activations[i];
        }
    }
    return 0;   /* activation table full — translate_and_map() still works, just uncached */
}

/* Diagnostic: reports whether an object currently has a cached native
 * translation. Called from loader.c's loader_simi_info() so the existing
 * `simi-info` report shows activation state alongside the header/entry
 * dump it already prints. */
int simi_activation_query(const char* object_name, struct SimiActivationStatus* out) {
    if (!out) return 0;
    out->cached = 0; out->code_pages = 0; out->entry_offset = 0; out->content_hash = 0;
    struct SimiActivation* act = tt_find_activation(object_name);
    if (!act || !act->valid) return 0;
    out->cached        = 1;
    out->code_pages    = act->code_pages;
    out->entry_offset  = act->entry_off;
    out->content_hash  = act->content_hash;
    return 1;
}

static void simi_activation_slot_reset(struct SimiActivation* act);

/* Phase 2 (Seed Kernel teardown): is this physical frame one of the SHARED
 * cached SIMI code pages? The activation cache's code frames are mapped
 * into every process that spawns the object (correct only because
 * base_vaddr is invariant — see the header comment), so a per-process
 * page-table teardown MUST NOT free them: freeing on the first process's
 * exit would yank the code out from under every other live activation.
 * The per-activation SCRATCH page is never in the cache (each activation
 * gets a fresh frame) and is therefore NOT reported here — teardown frees
 * it as an ordinary owned leaf. Scans the valid activation slots' frame[]
 * arrays; O(SIMI_MAX_ACTIVATIONS * SIMI_ACT_MAX_PAGES) worst case, called
 * once per leaf PTE during teardown only.
 *
 * Phase 14c: when `seen` is non-NULL (the teardown walker passes a
 * per-walk uint32_t bitmap), a hit also ACCOUNTS this process's mapping
 * of the activation exactly once: the walker visits every code page as a
 * separate leaf, so without the bitmap it would decrement mappers once
 * per page; the bitmap makes it once per process per activation. The
 * decrement is where the eager free fires: a RETIRED activation whose
 * last mapper just exited has dead-weight frames, so they are freed here
 * instead of waiting for the owning partition's destroy (see the Phase
 * 14c comment block). A valid, non-retired activation reaching mappers==0
 * is just the cache's resting state between spawns — nothing is freed.
 * Callers that only want the boolean (no process context) pass NULL. */
int simi_frame_is_cached(uint64_t paddr, uint32_t* seen) {
    if (paddr == 0) return 0;
    for (int i = 0; i < SIMI_MAX_ACTIVATIONS; i++) {
        const struct SimiActivation* act = &g_activations[i];
        if (!act->valid) continue;
        for (uint32_t p = 0; p < act->code_pages; p++) {
            if (act->frame[p] == paddr) {
                if (seen && !(*seen & (1u << i))) {
                    *seen |= (1u << i);
                    struct SimiActivation* a = &g_activations[i];
                    if (a->mappers > 0) a->mappers--;
                    if (a->mappers == 0 && a->retired) {
                        /* Last mapper of a retired activation exited: its
                         * frames can never serve a future spawn (retired
                         * slots never match find_activation) and no process
                         * maps them anymore — free now, reclaiming the
                         * memory at the earliest safe moment instead of at
                         * the owning partition's destroy. Safe mid-walk:
                         * the walker already decided to skip this leaf, the
                         * frames are not aliased anywhere else in this
                         * address space, and the walk runs with IF=0 so no
                         * allocation can re-hand them in between. */
                        kernel_serial_printf(
                            "[SIMI] activation vfree (last mapper exited): "
                            "'%s' (%u code page(s))\n",
                            a->object_name, (unsigned)a->code_pages);
                        for (uint32_t q = 0; q < a->code_pages; q++) {
                            free_physical_ram_frame(
                                (void*)(uintptr_t)a->frame[q]);
                        }
                        simi_activation_slot_reset(a);
                    }
                }
                return 1;
            }
        }
    }
    return 0;
}

void simi_activation_info(const char* object_name) {
    struct SimiActivationStatus st;
    if (!simi_activation_query(object_name, &st)) {
        kernel_serial_print(
            "  activation   : not yet translated (next spawn will translate + cache)\n");
        return;
    }
    kernel_serial_printf(
        "  activation   : cached — %u page(s) native code, entry @+0x%x, "
        "content hash 0x%08x\n",
        st.code_pages, st.entry_offset, st.content_hash);
}

uint64_t simi_translate_and_map(const char* object_name, uint64_t base_vaddr, uint64_t* pml4) {
    struct ServiceBinary* sb = 0;
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (service_binaries[i].active &&
            tt_streq(service_binaries[i].object_name, object_name)) {
            sb = &service_binaries[i];
            break;
        }
    }
    if (!sb || !sb->is_simi) {
        kernel_serial_printf(
            "[SIMI] translate: '%s' is not a loaded SIMI object.\n", object_name);
        return 0;
    }

    uint32_t hash = tt_fnv1a(sb->data, sb->size);
    struct SimiActivation* act = tt_find_or_alloc_activation(object_name);

    /* ── Cache hit ─────────────────────────────────────────────────────────
     * Reuse the already-translated code frames as-is: map the SAME
     * physical pages into this process's page table. Correct because those
     * pages' internal offsets and the r7 scratch-pointer immediate were
     * translated against base_vaddr, and base_vaddr is always
     * USER_PROC_CODE_BASE for every OBJ_TYPE_PROGRAM spawn (process.c) —
     * so it's identical this time too. Only the scratch page is
     * per-activation private state and always gets a fresh frame below. */
    if (act && act->valid && act->content_hash == hash && act->content_size == sb->size) {
        /* Phase 14b: re-tag on hit — see the partition_id field comment.
         * A retired slot reused by a same-content re-upload from a new
         * partition must carry the new partition's tag, or the old
         * partition's destroy would free frames the new owner maps. */
        act->partition_id = sb->partition_id;
        for (uint32_t i = 0; i < act->code_pages; i++) {
            user_map_page(pml4, base_vaddr + (uint64_t)i * 4096, act->frame[i],
                          USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_EXEC);
        }
        uint64_t scratch_vaddr = base_vaddr + (uint64_t)act->code_pages * 4096;
        void* scratch_frame = allocate_physical_ram_frame();
        if (!scratch_frame) {
            kernel_serial_print("[SIMI] translate: out of memory mapping scratch page.\n");
            return 0;
        }
        tt_memset(scratch_frame, 0, 4096);
        user_map_page(pml4, scratch_vaddr, (uint64_t)(uintptr_t)scratch_frame,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE | USER_PTE_NOEXEC);

        /* Phase 14c: this process now maps the shared code frames. */
        act->mappers++;
        kernel_serial_printf(
            "[SIMI] '%s' activation cache HIT — reused %u page(s), skipped "
            "translation, fresh scratch page @0x%016lx, entry @0x%016lx\n",
            object_name, act->code_pages, scratch_vaddr, base_vaddr + act->entry_off);
        return base_vaddr + act->entry_off;
    }

    /* ── Cache miss: translate-on-first-use (or after a detected re-upload) ─
     * Unchanged Phase 3 two-pass translate + exit-stub-append pipeline;
     * the only Phase 4 addition is stashing the resulting frames into the
     * activation slot afterward so the next spawn takes the hit path. */
    /* Phase 6 (v0.3): RESOLVE/OBJSIZE/OBJTYPE call these three real
     * kernel functions (simi_runtime.c, backed by the live
     * object_catalog[]) — addresses baked in as translate-time constants,
     * same mechanism as scratch_ptr below. Cast through uintptr_t, not
     * straight to uint64_t, to stay correct if this kernel is ever built
     * for a non-64-bit-pointer target (it isn't today, but see
     * simi_x86.h's own (uint64_t)(uintptr_t) casts for the same reason). */
    uint64_t rt_resolve = (uint64_t)(uintptr_t)simi_rt_resolve;
    uint64_t rt_objsize = (uint64_t)(uintptr_t)simi_rt_objsize;
    uint64_t rt_objtype = (uint64_t)(uintptr_t)simi_rt_objtype;

    uint32_t len1 = 0, off1 = 0;
    int rc = simi_x86_translate(sb->data, sb->size, g_simi_code_buf, SIMI_CODE_BUF_SIZE,
                                 "main", 0, rt_resolve, rt_objsize, rt_objtype, &len1, &off1);
    if (rc != TX_OK) {
        kernel_serial_printf("[SIMI] translate: '%s' failed: %s\n",
                             object_name, simi_x86_strerror(rc));
        return 0;
    }

    uint32_t code_pages = (len1 + 4095) / 4096;
    uint64_t scratch_vaddr = base_vaddr + (uint64_t)code_pages * 4096;

    uint32_t len2 = 0, off2 = 0;
    rc = simi_x86_translate(sb->data, sb->size, g_simi_code_buf, SIMI_CODE_BUF_SIZE,
                             "main", scratch_vaddr, rt_resolve, rt_objsize, rt_objtype, &len2, &off2);
    if (rc != TX_OK || len2 != len1) {
        kernel_serial_printf(
            "[SIMI] translate: '%s' second pass mismatch — internal translator bug, "
            "refusing to map possibly-inconsistent code.\n", object_name);
        return 0;
    }

    /* Append a small kernel-specific outer stub, deliberately kept out of
     * the portable simi_x86.c (that file always ends its trampoline in a
     * plain `ret` so it stays callable/testable as a normal function on
     * the host — see simi_x86.h). Here there's no caller to return to, so
     * this stub calls the translated trampoline for a real return address,
     * then turns RAX (the SIMI program's r0) into a SYS_SLS_EXIT syscall:
     *   call  <inner trampoline>
     *   mov   rdi, rax
     *   mov   rax, 164        ; SYS_SLS_EXIT
     *   syscall
     *   jmp   $                ; safety net if SYS_SLS_EXIT ever returns
     */
    uint32_t p = len2;
    if (p + 32 > SIMI_CODE_BUF_SIZE) {
        kernel_serial_print("[SIMI] translate: no room for exit stub.\n");
        return 0;
    }
    uint8_t* buf = g_simi_code_buf;
    buf[p++] = 0xE8;                                            /* call rel32 */
    int32_t rel = (int32_t)(off2 - (p + 4));
    buf[p++] = (uint8_t)(rel & 0xFF);
    buf[p++] = (uint8_t)((rel >> 8) & 0xFF);
    buf[p++] = (uint8_t)((rel >> 16) & 0xFF);
    buf[p++] = (uint8_t)((rel >> 24) & 0xFF);
    buf[p++] = 0x48; buf[p++] = 0x89; buf[p++] = 0xC7;          /* mov rdi,rax */
    buf[p++] = 0x48; buf[p++] = 0xC7; buf[p++] = 0xC0;          /* mov rax,imm32 (sign-ext) */
    buf[p++] = (uint8_t)(SYS_SLS_EXIT & 0xFF);
    buf[p++] = (uint8_t)((SYS_SLS_EXIT >> 8) & 0xFF);
    buf[p++] = 0x00; buf[p++] = 0x00;
    buf[p++] = 0x0F; buf[p++] = 0x05;                           /* syscall */
    buf[p++] = 0xEB; buf[p++] = 0xFE;                           /* jmp $ (2-byte rel8 self-loop) */
    uint32_t stub_off = len2;
    uint32_t total_len = p;

    uint32_t total_pages = (total_len + 4095) / 4096;
    if (total_pages > SIMI_ACT_MAX_PAGES) {
        /* Can't happen given SIMI_CODE_BUF_SIZE == SIMI_ACT_MAX_PAGES*4096
         * and the p+32>SIMI_CODE_BUF_SIZE check above, but keep the
         * invariant explicit rather than silently overrunning act->frame[]. */
        kernel_serial_print("[SIMI] translate: translated code exceeds activation cache capacity.\n");
        return 0;
    }

    /* Gap Remediation SIMI Phase 13: free this slot's OLD code-page frames
     * before they're overwritten below. Placed here (after translation has
     * fully succeeded, before any old state is touched) so a translation
     * failure earlier in this function never destroys a still-good cached
     * activation -- only a *successful* re-translation retires the old one. */
    if (act && act->valid) {
        for (uint32_t i = 0; i < act->code_pages; i++) {
            free_physical_ram_frame((void*)(uintptr_t)act->frame[i]);
        }
        act->valid = 0;   /* defensive: if anything below still fails (e.g. an
                            * OOM mid-allocation-loop), this slot must report
                            * "not cached" rather than a half-populated
                            * frame[] array claiming to be a valid activation. */
    }

    for (uint32_t i = 0; i < total_pages; i++) {
        void* frame = allocate_physical_ram_frame();
        if (!frame) {
            kernel_serial_print("[SIMI] translate: out of memory mapping native code.\n");
            return 0;
        }
        tt_memset(frame, 0, 4096);
        uint32_t off = i * 4096;
        uint32_t chunk = total_len - off;
        if (chunk > 4096) chunk = 4096;
        tt_memcpy(frame, g_simi_code_buf + off, chunk);
        user_map_page(pml4, base_vaddr + (uint64_t)i * 4096,
                      (uint64_t)(uintptr_t)frame,
                      USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_EXEC);
        if (act) act->frame[i] = (uint64_t)(uintptr_t)frame;
    }

    void* scratch_frame = allocate_physical_ram_frame();
    if (!scratch_frame) {
        kernel_serial_print("[SIMI] translate: out of memory mapping scratch page.\n");
        return 0;
    }
    tt_memset(scratch_frame, 0, 4096);
    user_map_page(pml4, scratch_vaddr, (uint64_t)(uintptr_t)scratch_frame,
                  USER_PTE_PRESENT | USER_PTE_USER | USER_PTE_WRITE | USER_PTE_NOEXEC);

    if (act) {
        act->valid         = 1;
        act->partition_id  = sb->partition_id;   /* Phase 14b: tag at stash */
        act->retired       = 0;   /* Phase 14c: a recycled slot starts live */
        act->mappers       = 1;   /* Phase 14c: this spawn maps the frames */
        act->content_hash  = hash;
        act->content_size  = sb->size;
        act->code_pages    = total_pages;
        act->entry_off     = stub_off;
    } else {
        kernel_serial_printf(
            "[SIMI] '%s' translated but the activation table is full (%d slots) — "
            "this translation will not be cached; every spawn will retranslate.\n",
            object_name, SIMI_MAX_ACTIVATIONS);
    }

    kernel_serial_printf(
        "[SIMI] '%s' activation cache MISS — translated %u bytes native code "
        "across %u page(s) + 1 scratch page @0x%016lx, entry stub @0x%016lx\n",
        object_name, total_len, total_pages, scratch_vaddr, base_vaddr + stub_off);

    return base_vaddr + stub_off;
}

// ─── Phase 14b (LPAR): destroy-time activation-cache teardown ────────────────
// The shared cache is deliberately partition-agnostic in Phase 4 (one slot
// per object name, code frames shared across every process that spawns the
// object — see the header comment for why that's correct). This phase gives
// it a destroy-time story: activations are now tagged with the owning
// partition, and partition teardown frees them. Before this phase, the code
// frames of a destroyed partition's SIMI objects were never reclaimed — the
// frames are allocated via allocate_physical_ram_frame() (owner =
// PARTITION_SYSTEM), so partition_reclaim_all_frames() can't see them, and
// per-process teardown deliberately skips them (simi_frame_is_cached), so
// repeated create -> SIMI-spawn -> destroy cycles leaked code frames until
// the activation table (16 slots) filled up and every spawn degraded to a
// retranslation.
//
// WHY freeing SHARED code frames at destroy is safe (the decision):
//   1. Both spawn paths (process_create and program_spawn_common) enforce
//      catalog_check_access()'s partition boundary, so a process can only
//      ever spawn an object whose catalog partition matches its own —
//      every process mapping an activation's frames lives in the
//      activation's OWN partition (the tag == the catalog partition, see
//      the partition_id field comment).
//   2. partition_destroy() Step 1 (process_kill_partition) synchronously
//      tears down every process in the partition — including all of the
//      activation's mappers — BEFORE Step 2 (catalog/loader vfree) reaches
//      simi_vfree_partition() here. The per-process teardown walks run
//      while the slots are still valid, so they see the code frames as
//      cached and skip them (no double-free); only then does this pass
//      free the frames, when no live process anywhere maps them.
//   Ordering is load-bearing: simi_vfree_partition() must never run before
//   the partition's processes are gone — that is guaranteed by the caller
//   (loader_vfree_partition() runs in partition_destroy()'s Step 2).
//
// The per-object half (simi_vfree_object) RETIRES instead of freeing: a
// vfree can be issued while the object's activation is still mapped by
// live processes (nothing in the syscall ABI forbids it), and freeing
// shared frames out from under a live mapper is exactly the crash
// simi_frame_is_cached() exists to prevent. Retiring sets the retired
// flag so the slot can never match a future find_activation() — a
// re-valloc'd object of the same name therefore translates fresh instead
// of inheriting a stale cross-partition activation, which is what keeps
// invariant (1) above true across name reuse.
//
// Phase 14c (the eager-free refinement): the retire was originally a
// hold-until-partition-destroy retention — safe, but it kept dead-weight
// frames alive arbitrarily long for a vfree'd object nobody spawns
// anymore. This phase tracks exactly how many live processes map each
// activation (mappers, incremented on every successful spawn, decremented
// once per process by the teardown walker via simi_frame_is_cached()'s
// seen-bitmap) and frees a retired activation's frames at the EARLIEST
// safe moment: immediately if no mapper is live at vfree time, otherwise
// the moment the last mapper's teardown walk drops mappers to 0. The
// owning partition's destroy remains the backstop (simi_vfree_partition
// matches retired slots too), so a retired activation is reclaimed exactly
// once — either eagerly or at destroy, never twice, never while a live
// process maps it. A valid, non-retired activation sits at mappers==0
// between spawns; that is the cache's resting state and is NOT a free
// trigger.

static void simi_activation_slot_reset(struct SimiActivation* act) {
    tt_memset(act->frame, 0, sizeof(act->frame));
    act->object_name[0] = '\0';
    act->valid          = 0;
    act->retired        = 0;   /* Phase 14c */
    act->mappers        = 0;   /* Phase 14c */
    act->partition_id   = 0;
    act->content_hash   = 0;
    act->content_size   = 0;
    act->code_pages     = 0;
    act->entry_off      = 0;
}

uint32_t simi_vfree_partition(uint32_t partition_id) {
    uint32_t freed = 0;
    for (int i = 0; i < SIMI_MAX_ACTIVATIONS; i++) {
        struct SimiActivation* act = &g_activations[i];
        if (!act->valid) continue;
        if (act->partition_id != partition_id) continue;
        /* Matches valid AND retired slots: retirement (simi_vfree_object)
         * keeps valid + partition_id + frames, so the owning partition's
         * destroy reclaims the frames exactly once (any slot whose last
         * mapper already exited was freed eagerly by Phase 14c — see
         * simi_frame_is_cached()). */
        if (act->mappers > 0) {
            /* Should be unreachable: partition_destroy() Step 1 killed
             * every process in this partition (and, per the partition
             * boundary, every possible mapper of this activation) before
             * this Step-2 pass runs, and each kill's teardown walk
             * decremented mappers to 0. If it DOES happen, the safety
             * argument that makes destroy-time free safe is broken —
             * refuse rather than yank the frames out from under a live
             * process, and make the invariant violation loud instead of
             * a silent corruption. The frames are leaked (bounded) so the
             * live mapper keeps running. */
            kernel_serial_printf(
                "[SIMI] activation vfree (partition %u teardown): WARNING "
                "'%s' still has %u live mapper(s) — frames NOT freed\n",
                (unsigned)partition_id, act->object_name,
                (unsigned)act->mappers);
            continue;
        }
        kernel_serial_printf(
            "[SIMI] activation vfree (partition %u teardown): '%s' "
            "(%u code page(s))\n",
            (unsigned)partition_id, act->object_name,
            (unsigned)act->code_pages);
        for (uint32_t p = 0; p < act->code_pages; p++) {
            free_physical_ram_frame((void*)(uintptr_t)act->frame[p]);
        }
        simi_activation_slot_reset(act);
        freed++;
    }
    return freed;
}

uint32_t simi_vfree_object(const char* name) {
    if (!name || !name[0]) return 0;
    struct SimiActivation* act = tt_find_activation(name);
    if (!act || !act->valid) return 0;
    /* Phase 14c: retire the slot (so no future find_activation() can match
     * it — a re-valloc'd object of the same name translates fresh instead
     * of inheriting a stale cross-partition activation), then free the
     * frames at the earliest SAFE moment. That moment is now if no live
     * process maps them; otherwise it is when the LAST mapper's teardown
     * walk drops mappers to 0 (simi_frame_is_cached()'s eager free), with
     * the owning partition's destroy as the backstop. Never free while a
     * mapper is live: that is exactly the crash simi_frame_is_cached()
     * exists to prevent. */
    act->retired = 1;
    if (act->mappers == 0) {
        kernel_serial_printf(
            "[SIMI] activation vfree (object vfree): '%s' — no live "
            "mappers, %u code page(s) freed immediately\n",
            name, (unsigned)act->code_pages);
        for (uint32_t p = 0; p < act->code_pages; p++) {
            free_physical_ram_frame((void*)(uintptr_t)act->frame[p]);
        }
        simi_activation_slot_reset(act);
    } else {
        kernel_serial_printf(
            "[SIMI] activation vfree (object vfree): '%s' — retired, %u "
            "live mapper(s); %u code page(s) freed when the last one exits\n",
            name, (unsigned)act->mappers, (unsigned)act->code_pages);
    }
    return 1;
}
