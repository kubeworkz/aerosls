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

#endif /* SIMI_TRANSLATE_H */
