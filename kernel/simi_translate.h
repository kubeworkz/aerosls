#ifndef SIMI_TRANSLATE_H
#define SIMI_TRANSLATE_H

#include <stdint.h>

/* Phase 3: translates the named SIMI ServiceBinary object (must already be
 * uploaded and detected as is_simi — see loader.c) to x86-64 machine code
 * via simi_x86_translate(), maps it executable into the given process's
 * page table starting at base_vaddr, and maps one additional scratch data
 * page immediately after it (see simi_x86.h — the r7 convention standing
 * in for real SIMI object/pointer allocation, which doesn't exist yet).
 *
 * Only the "main" entry point is spawnable in this v1 — see the .c file
 * for why. Returns the RIP to enter at (a small kernel-generated stub that
 * calls the translated trampoline and turns its return value into a
 * SYS_SLS_EXIT syscall), or 0 on failure (message logged via
 * kernel_serial_printf).
 */
uint64_t simi_translate_and_map(const char* object_name, uint64_t base_vaddr, uint64_t* pml4);

/* Gap Remediation Phase G: structured counterpart to simi_activation_info()
 * below -- same data (cached / not yet translated, page count, entry
 * offset, content hash), filled into an out-struct instead of printed.
 * Returns 1 if a valid cached activation exists (out fully populated),
 * 0 if not yet translated (out->cached=0, other fields zeroed). Backs
 * loader.c's loader_simi_info_query() (net/http.c's GET /api/simi/<name>
 * and the SYS_SLS_SIMI_INFO syscall both read through that, not this
 * function directly). */
struct SimiActivationStatus {
    uint8_t  cached;
    uint32_t code_pages;
    uint32_t entry_offset;
    uint32_t content_hash;
};
int simi_activation_query(const char* object_name, struct SimiActivationStatus* out);

/* Phase 4: prints one line of activation-cache status (cached / not yet
 * translated) for the named SIMI object — called from loader.c's
 * loader_simi_info() to extend the existing `simi-info` report. Gap
 * Remediation Phase G: now a thin wrapper over simi_activation_query()
 * above (single source of truth) rather than its own independent lookup. */
void simi_activation_info(const char* object_name);

/* Phase 14b (LPAR destroy-time SIMI cache story): the two teardown halves
 * of the activation cache, called from loader.c's own vfree paths so the
 * cache's lifetime mirrors the binary store's:
 *
 *   simi_vfree_partition(pid) — the destroy-time half, called from
 *     loader_vfree_partition() during partition_destroy() Step 2. Frees
 *     the code frames of every activation whose partition_id matches
 *     (valid AND retired slots — retired slots keep their frames and tag
 *     for exactly this) and fully resets each slot. Returns the count
 *     freed. See the .c file for the safety argument: this is only safe
 *     after Step 1 has torn down every process in the partition, because
 *     the code frames are SHARED across all of the activation's mappers.
 *
 *   simi_vfree_object(name) — the per-object half, called from
 *     loader_vfree(). RETIRES the slot (the retired flag makes it
 *     unmatchable by future find_activation() calls, so a re-valloc'd
 *     object of the same name translates fresh instead of inheriting a
 *     stale cross-partition activation), then frees the frames at the
 *     earliest safe moment: immediately if no process maps them, or the
 *     moment the LAST mapper's teardown walk drops the Phase 14c mapper
 *     refcount to 0 (see simi_frame_is_cached()); the owning partition's
 *     destroy remains the backstop. Retiring — never freeing while a
 *     mapper is live — is what keeps "an activation's frames are only
 *     mapped by processes in the activation's own partition" true across
 *     name reuse, and what keeps the destroy-time free safe even if a
 *     vfree races a live mapper. Returns 1 if a slot was retired/freed,
 *     0 if no activation existed. */
uint32_t simi_vfree_partition(uint32_t partition_id);
uint32_t simi_vfree_object(const char* name);

/* Phase 14c: the per-process page-table teardown hook. Returns 1 if
 * `paddr` is one of the SHARED cached code frames (the walker must skip
 * freeing it), 0 otherwise. When `seen` is non-NULL (a per-walk uint32_t
 * bitmap the walker carries), a hit also accounts this process's mapping
 * of the owning activation exactly once — decrementing the mapper
 * refcount and, if that was the LAST mapper of a RETIRED activation,
 * freeing its frames immediately. See the .c file for the full comment. */
int simi_frame_is_cached(uint64_t paddr, uint32_t* seen);

#endif /* SIMI_TRANSLATE_H */
