#ifndef TIMI_TRANSLATE_H
#define TIMI_TRANSLATE_H

#include <stdint.h>

/* Phase 3: translates the named TIMI ServiceBinary object (must already be
 * uploaded and detected as is_timi — see loader.c) to x86-64 machine code
 * via timi_x86_translate(), maps it executable into the given process's
 * page table starting at base_vaddr, and maps one additional scratch data
 * page immediately after it (see timi_x86.h — the r7 convention standing
 * in for real TIMI object/pointer allocation, which doesn't exist yet).
 *
 * Only the "main" entry point is spawnable in this v1 — see the .c file
 * for why. Returns the RIP to enter at (a small kernel-generated stub that
 * calls the translated trampoline and turns its return value into a
 * SYS_SLS_EXIT syscall), or 0 on failure (message logged via
 * kernel_serial_printf).
 */
uint64_t timi_translate_and_map(const char* object_name, uint64_t base_vaddr, uint64_t* pml4);

/* Phase 4: prints one line of activation-cache status (cached / not yet
 * translated) for the named TIMI object — called from loader.c's
 * loader_timi_info() to extend the existing `timi-info` report. */
void timi_activation_info(const char* object_name);

#endif /* TIMI_TRANSLATE_H */
