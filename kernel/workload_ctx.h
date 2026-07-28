#ifndef WORKLOAD_CTX_H
#define WORKLOAD_CTX_H

#include <stdint.h>
#include "simi_interp.h"

/*
 * workload_ctx.h — the producer of live execution contexts.
 *
 * ─── The gap this closes ──────────────────────────────────────────────
 * PEC Phase 3 built the whole path for moving a running computation
 * between nodes: checkpoint, chunk, transmit, reassemble, resume, all
 * proven end-to-end. And then said, honestly, that it was unreachable
 * from the actual system, because NOTHING IN THIS KERNEL CREATED A
 * LONG-LIVED SimiContext. simi_ctx_migrate_send_partition() iterated a
 * registry that was empty on every boot, so partition_migrate() moved
 * zero contexts, forever.
 *
 * This file is that missing producer. A workload can now declare a
 * program; the reconciler instantiates it as a live interpreted context
 * and registers it with simi_ctx_register(). From that moment
 * partition_migrate() moves real running work, and the Phase 3 capability
 * is reachable from an operator command rather than only from a test.
 *
 * ─── Why images are copied rather than pointed at ─────────────────────
 * An uploaded .tmo sits in service_binaries[].data[] with its header at
 * offset 0, which puts the instruction stream at byte offset 20. A
 * SimiObject wants a `uint64_t*` there -- a misaligned pointer, which is
 * undefined behaviour even where x86 tolerates it, and genuinely faults
 * on the RISC-V target this codebase also builds. So each slot holds its
 * own naturally-aligned copy of the four arrays.
 *
 * That costs the caps below, which are deliberately modest: this is for
 * long-running service workloads, not for the whole corpus. A program
 * exceeding any cap is REFUSED with a distinct status rather than
 * silently truncated -- a truncated instruction stream would run and
 * produce wrong answers.
 *
 * ─── Where execution happens, and where it must not ───────────────────
 * wlctx_step_all() runs on the BSP ONLY. The interpreter's RESOLVE/
 * OBJSIZE/OBJTYPE opcodes call into kernel/simi_runtime.c, which reads
 * the object catalog -- state the BSP mutates. Stepping from the AP core
 * would race it. The reconciler therefore never executes anything; it
 * only queues start/stop intents, and the BSP both applies those and
 * steps the contexts, from its existing drain point.
 */

#define WLCTX_MAX            8      /* live contexts; ~352 KiB each */
#define WLCTX_NAME_LEN       64

/* Per-slot image caps. Sized for service workloads, not for arbitrary
 * programs; see the header note on why exceeding one is a refusal. */
#define WLCTX_MAX_INSTR      4096
#define WLCTX_MAX_LITERALS   1024
#define WLCTX_MAX_ENTRIES      32
#define WLCTX_MAX_NAMES        32

typedef enum {
    WLCTX_OK = 0,
    WLCTX_ERR_FULL,          /* WLCTX_MAX live contexts already */
    WLCTX_ERR_NOT_FOUND,     /* no such live context */
    WLCTX_ERR_BAD_IMAGE,     /* not a valid .tmo, or truncated */
    WLCTX_ERR_TOO_BIG,       /* exceeds one of the caps above */
    WLCTX_ERR_NO_ENTRY,      /* the named entry point does not exist */
    WLCTX_ERR_EXISTS,        /* this workload already has a live context */
} WLCtxStatus;

const char* wlctx_status_name(WLCtxStatus s);

void wlctx_init(void);

/* Instantiates `image` (a flat .tmo) as a live context for `workload`,
 * entered at `entry`, and registers it with simi_ctx_register() so
 * partition_migrate() will move it.
 *
 * BSP ONLY -- it registers into the migration registry and reads the
 * uploaded binary store. */
WLCtxStatus wlctx_start(const char* workload, const uint8_t* image,
                        uint32_t image_size, const char* entry,
                        uint32_t partition_id);

/* Unregisters and frees the slot. BSP only. */
WLCtxStatus wlctx_stop(const char* workload);

int      wlctx_has(const char* workload);
uint32_t wlctx_count(void);
struct SimiContext* wlctx_get(const char* workload);

/* Advances every live, still-runnable context by at most `budget`
 * instructions. Returns how many were actually stepped.
 *
 * BSP ONLY -- see the header. Bounded by construction: a context that
 * HALTs or traps is left registered (its final state is still worth
 * migrating and inspecting) but is not stepped again. */
uint32_t wlctx_step_all(uint64_t budget);

void wlctx_list(void);

#endif /* WORKLOAD_CTX_H */
