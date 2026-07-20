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
        if (g_activations[i].object_name[0] &&
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
        if (!g_activations[i].object_name[0]) {
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
