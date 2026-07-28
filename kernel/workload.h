#ifndef WORKLOAD_H
#define WORKLOAD_H

#include <stdint.h>
#include "service_registry.h"

/*
 * workload.h — declarative workload objects + a bounded reconciler
 * (Orchestration Plan Phase 5).
 *
 * Declare what should be true; the kernel converges on it. This is the
 * first thing in this system that acts on its own, so the whole design
 * here is about bounding that: a small, explicit, enumerated set of
 * conditions, every action logged, and a global off switch.
 *
 * ─── Where the reconciler runs, and why the plan's answer changed ─────
 * The plan said: run it on the AP core beside tier_mgr_tick(), and noted
 * the AP core must never call persist_*() (those run on the BSP and share
 * the unlocked DMA staging buffer p_buf). Both halves of that turned out
 * to be right, and together they force a queue. The alternative -- run
 * the whole reconciler on the BSP and persist directly -- was examined
 * and rejected on evidence:
 *
 *   - The BSP's foreground loop is http_server_run(), which is only
 *     entered when a NIC is present (kernel.c step 8).
 *   - Without a NIC the BSP falls through to sls_shell_loop(), which
 *     BLOCKS in read_line() on serial input. There is no idle point in it
 *     at all.
 *
 * So the BSP has no loop that reliably ticks; the AP core's
 * ap_kernel_main() is the only one that does. The reconciler therefore
 * runs on the AP core, performs directly only what does NOT persist, and
 * hands everything else to the BSP through the ring below.
 *
 * ─── The queue, and its honest limitation ─────────────────────────────
 * Single producer (AP core, reconcile_tick), single consumer (BSP,
 * reconcile_drain). Fixed ring, atomic head/tail, no locks -- the same
 * __atomic discipline kernel/smp.c already uses for ap_bootstrap_lock.
 *
 * The BSP drains from two places: http_server_run()'s per-sweep pass, and
 * the top of sls_shell_execute(). LIMITATION, stated rather than
 * discovered: on a NIC-less boot the first of those never runs, so
 * intents apply only when an operator types a command. They are not lost
 * -- the ring holds them and reconcile_tick() is idempotent, so a
 * still-unconverged condition is simply re-detected -- but convergence is
 * not autonomous in that configuration. Making it so needs an idle point
 * in the shell loop, which is a change to read_line(), not to this file.
 *
 * ─── What it reconciles (deliberately four things) ────────────────────
 * Per PARTITION, from all its workloads at once:
 *   any workload wants RUNNING + partition paused  -> resume it
 *   no  workload wants RUNNING + partition running -> pause it
 * Per WORKLOAD, independently:
 *   desired RUNNING + service missing or wrong  -> register it    [persists]
 *   desired STOPPED + service still advertised  -> unregister it  [persists]
 *
 * The partition rule is UNION semantics, and that split is not cosmetic.
 * Several workloads can share a partition, so deciding its run-state per
 * workload lets two declarations with opposite desires oscillate --
 * pausing and resuming each other forever, never settling. The first
 * version of this did exactly that; the host test's scenario 7 caught it.
 * "The partition runs if anything in it wants to run" is deterministic,
 * order-independent, and matches what the words mean.
 *
 * Everything else a real orchestrator does -- restarts, scaling, health,
 * scheduling -- is deliberately absent. Those need a liveness signal this
 * kernel does not have for workloads (see the Phase 4 findings on
 * microkernel.c's watchdog covering only the 5 internal services).
 */

#define WORKLOAD_MAX       32
#define WORKLOAD_NAME_LEN  64

typedef enum {
    WL_DESIRED_STOPPED = 0,
    WL_DESIRED_RUNNING = 1,
} SLSWorkloadDesired;

struct SLSWorkloadEntry {
    char     name[WORKLOAD_NAME_LEN];
    uint32_t partition_id;

    /* The service this workload should expose. Empty name == "declares no
     * service", which is legal: a workload can be a partition that should
     * simply be running. */
    char     service_name[SERVICE_NAME_LEN];
    uint32_t endpoint_port;
    uint8_t  endpoint_kind;      /* SLSServiceEndpointKind */

    /* The PROGRAM this workload runs, as an uploaded object name. Empty
     * == "declares no program", which is legal: a workload can be just a
     * partition + service. When set, the reconciler instantiates it as a
     * LIVE execution context (kernel/workload_ctx.h) -- which is what
     * makes partition_migrate() actually move running work, closing the
     * gap PEC Phase 3 shipped with. */
    char     program_name[WORKLOAD_NAME_LEN];
    char     entry_name[32];

    uint8_t  desired_state;      /* SLSWorkloadDesired */
    uint8_t  active;

    /* Observed, for operator visibility. Not part of desired state.
     *
     * actions_taken counts convergence actions attributable to THIS
     * workload -- which in practice means its own service registration.
     * Partition pause/resume is deliberately NOT counted here: a
     * partition is shared, its run-state is decided once per partition
     * from all its workloads (union semantics, see workload.c), so
     * charging that action to one arbitrary workload would be a made-up
     * number. `converged` has the same scope: it reports whether THIS
     * entry had nothing left to do. */
    uint32_t actions_taken;
    uint8_t  converged;
};

typedef enum {
    WL_OK = 0,
    WL_ERR_NAME,
    WL_ERR_PARTITION,
    WL_ERR_FULL,
    WL_ERR_NOT_FOUND,
    WL_ERR_PERM,
    WL_ERR_ENDPOINT,
} SLSWorkloadStatus;

const char* workload_status_name(SLSWorkloadStatus s);

extern struct SLSWorkloadEntry workloads[WORKLOAD_MAX];

void workload_init(void);

/* Declare, or update an existing declaration in place. DB_ADMIN or
 * higher, matching every other creation path in this kernel. */
SLSWorkloadStatus workload_declare(uint32_t caller_uid, const char* name,
                                   uint32_t partition_id,
                                   SLSWorkloadDesired desired,
                                   const char* service_name,
                                   SLSServiceEndpointKind kind,
                                   uint32_t endpoint_port,
                                   const char* program_name,
                                   const char* entry_name);

SLSWorkloadStatus workload_delete(uint32_t caller_uid, const char* name);
uint32_t          workload_count(void);
struct SLSWorkloadEntry* workload_find(const char* name);

/* ─── The reconciler ──────────────────────────────────────────────────
 * Enabled by default OFF. Something that acts autonomously should be
 * switched on deliberately, and every host test that does not mean to
 * exercise it then gets the old behaviour by construction. */
void reconcile_set_enabled(int on);
int  reconcile_is_enabled(void);

/* One sweep. Safe to call from the AP core: performs only non-persisting
 * convergence directly and enqueues the rest. Returns the number of
 * actions taken plus intents enqueued this sweep (0 == fully converged).
 * Idempotent: a converged system produces 0 forever. */
uint32_t reconcile_tick(void);

/* Drains queued intents and applies them. MUST be called only from the
 * BSP -- it calls persist_*() by design. Returns intents applied. */
uint32_t reconcile_drain(void);

/* Resolves an uploaded program image by object name. Defined in
 * workload.c against loader.c's service_binaries[]; declared here so the
 * host test can substitute a synthesised image without linking the whole
 * loader (and its NVMe/process dependencies) just to hand over a byte
 * buffer. Returns 0 if there is no such uploaded object. */
const uint8_t* workload_find_program(const char* name, uint32_t* out_size);

/* Ring depth and overflow accounting, for the test and the operator.
 * An overflow is not silent: it is counted and logged, and the intent is
 * re-derived on the next sweep because reconcile_tick() re-reads actual
 * state rather than trusting that a previous enqueue succeeded. */
uint32_t reconcile_queue_depth(void);
uint32_t reconcile_queue_dropped(void);

/* ─── Ring test seam ──────────────────────────────────────────────────
 * reconcile_tick() can only produce intents at the rate the reconciler
 * finds work, which is far too slow and too orderly to shake out a memory
 * ordering bug. These two let a test drive the ring directly, at speed,
 * from a real second thread.
 *
 * They exist FOR the test and are honest about it, rather than the test
 * reaching into workload.c's statics or -- worse -- the ring's ordering
 * going unverified because exercising it was inconvenient. They touch
 * nothing but the ring, so they cannot make the production paths behave
 * differently; enqueue_probe() takes the same path wl_enqueue() does.
 *
 * `seq` is echoed back by drain_probe() so a consumer can verify the
 * exact sequence it received: no loss, no duplication, no reordering, and
 * no torn payload. Returns 1 if the ring was full (same as wl_enqueue). */
int      reconcile_enqueue_probe(uint32_t seq);
/* Drains up to `max` intents, writing each one's seq into out_seq[].
 * Returns how many were drained. Applies nothing -- probe intents carry
 * no service action, so this never persists and is safe to call from a
 * test thread. */
uint32_t reconcile_drain_probe(uint32_t* out_seq, uint32_t max);

void sys_sls_workload_list(void);

/* ─── Syscall surface ─────────────────────────────────────────────────── */
struct SLSWorkloadDeclareRequest {
    uint32_t caller_uid;
    char     name[WORKLOAD_NAME_LEN];
    uint32_t partition_id;
    uint32_t desired_state;
    char     service_name[SERVICE_NAME_LEN];
    uint32_t endpoint_kind;
    uint32_t endpoint_port;
    char     program_name[WORKLOAD_NAME_LEN];
    char     entry_name[32];
};

#define SYS_SLS_WORKLOAD_DECLARE   285
#define SYS_SLS_WORKLOAD_DELETE    286
#define SYS_SLS_WORKLOAD_LIST      287
#define SYS_SLS_RECONCILE_ENABLE   288

uint64_t sys_sls_workload_declare(struct SLSWorkloadDeclareRequest* req);
uint64_t sys_sls_workload_delete(struct SLSWorkloadDeclareRequest* req);
uint64_t sys_sls_reconcile_enable(uint32_t on);

#endif /* WORKLOAD_H */
