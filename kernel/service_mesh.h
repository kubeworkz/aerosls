#ifndef SERVICE_MESH_H
#define SERVICE_MESH_H

#include <stdint.h>
#include "service_registry.h"

/*
 * service_mesh.h — circuit breaking and per-service metrics
 * (Orchestration Plan Phase 6).
 *
 * Takes the CONCEPTS from docs/AeroSLS-Service-Mesh.md -- a three-state
 * breaker, failure thresholds, per-service counters -- and none of its
 * implementation, which is pthreads, sockets and shared-memory channels
 * that do not exist in this kernel.
 *
 * ─── What a breaker needs, and where this kernel gets it ──────────────
 * A circuit breaker is only as good as the failure signal feeding it. The
 * mesh document's version is fed by an explicit health-check thread that
 * pings each endpoint. This has three sources instead, and two of them
 * need no cooperation from anyone:
 *
 *  1. ENDPOINT PROBE (automatic). Already built for Phase 4's TTL work:
 *     a TCP endpoint with no LISTEN socket, or an IPC endpoint whose
 *     microkernel watchdog says CRASHED, is DOWN. Catches "the process
 *     died".
 *
 *  2. IPC QUEUE SATURATION (automatic). This is the interesting one. The
 *     gap named at the end of Phase 4 was: a process still alive and
 *     holding its port, but whose handler has WEDGED, reports up. It
 *     turns out this kernel already observes that -- ipc_post() returns
 *     -2 and increments ipc_stats.total_dropped when a queue is full, and
 *     a queue only fills because nobody is draining it. So the signal
 *     that supposedly needed a new application-level protocol was
 *     already there, in the queue depth. A saturated queue is a wedged
 *     handler, near enough to act on.
 *
 *  3. EXPLICIT OUTCOME REPORTS. For anything the kernel genuinely cannot
 *     see -- a TCP service that accepts a connection and then answers
 *     wrongly -- the caller tells us. service_report_success()/
 *     _failure(). No magic, and honest about needing cooperation.
 *
 * ─── Why admission is a separate, MUTATING call ───────────────────────
 * service_resolve() reports the breaker state and nothing more: the
 * registry reports, it does not hide, for the same reason a DOWN service
 * still resolves -- collapsing "gone" and "broken" into one answer serves
 * nobody.
 *
 * But HALF_OPEN has to admit exactly ONE trial call and then close the
 * gate again until that trial's outcome is known. That is a state
 * transition, not a read, and it cannot live inside a lookup that callers
 * make for all sorts of reasons. Hence service_call_permitted(): the one
 * place that decides "may I send right now", and the only one that moves
 * the breaker between OPEN and HALF_OPEN.
 */

/* Consecutive failures before the breaker opens. Deliberately not 1: a
 * single dropped IPC message under a momentary burst is not an outage,
 * and a breaker that trips on it makes the system less available rather
 * than more. */
#define MESH_FAIL_THRESHOLD      5

/* How long an OPEN breaker waits before allowing a trial call. ~10 s at
 * the ~100 Hz kernel tick, an order of magnitude above the heartbeat so a
 * recovering service gets re-probed before it gets re-tried. */
#define MESH_OPEN_COOLDOWN_TICKS 1000u

/* Queue occupancy (out of IPC_QUEUE_DEPTH) at which an IPC endpoint is
 * judged to be failing to drain. Not 100%: a queue that is momentarily
 * full is normal, a queue sitting at 7/8 across successive probes is a
 * handler that has stopped keeping up. */
#define MESH_QUEUE_SATURATED_NUM 7
#define MESH_QUEUE_SATURATED_DEN 8

typedef enum {
    CB_CLOSED = 0,    /* normal -- calls permitted */
    CB_OPEN,          /* failing -- calls refused until the cooldown elapses */
    CB_HALF_OPEN,     /* cooldown elapsed -- exactly one trial call permitted */
} SLSBreakerState;

const char* breaker_state_name(SLSBreakerState s);

struct SLSMeshEntry {
    char     name[SERVICE_NAME_LEN];
    uint8_t  active;
    uint8_t  state;               /* SLSBreakerState */

    uint32_t consecutive_failures;
    uint64_t opened_at_tick;      /* when the breaker last went OPEN */
    uint8_t  trial_outstanding;   /* a HALF_OPEN trial has been admitted, outcome unknown */

    /* Per-service metrics. Cumulative, never reset by the breaker -- an
     * operator asking "how bad has this been" wants the history, not the
     * state machine's working set. */
    uint64_t calls_permitted;
    uint64_t calls_refused;       /* refused because the breaker was open */
    uint64_t successes;
    uint64_t failures;
    uint64_t trips;               /* CLOSED -> OPEN transitions */
};

#define MESH_MAX_SERVICES SERVICE_MAX
extern struct SLSMeshEntry mesh_entries[MESH_MAX_SERVICES];

void mesh_init(void);

/* Outcome reporting. Both create the entry on first use, so a caller
 * never has to register anything first.
 *
 * `now` is kernel_tick_counter, and EVERY function here that touches time
 * takes it as a parameter rather than reading the global. That is not
 * ceremony: the first version had service_report_failure() stamp the trip
 * from the global while service_call_permitted() used a passed-in `now`,
 * and the two clocks disagreed -- a breaker could be admitted straight
 * back to HALF_OPEN because its recorded open-time came from a different
 * reading than the one being compared against. The host test caught it.
 * One state machine, one clock, supplied by the caller. */
void service_report_success(const char* name, uint64_t now);
void service_report_failure(const char* name, uint64_t now);

/* THE admission decision. Returns non-zero if a call may proceed now.
 *
 * Mutating by necessity: an OPEN breaker whose cooldown has elapsed
 * transitions to HALF_OPEN here and admits this one caller, then refuses
 * every other until that caller reports an outcome. `now` is
 * kernel_tick_counter, passed in so the decision is testable without
 * waiting ten real seconds. */
int service_call_permitted(const char* name, uint64_t now);

/* Read-only. Returns CB_CLOSED for a name with no entry -- an unknown
 * service has no failure history, and refusing calls to it on that basis
 * would break every service before its first outcome is ever reported. */
SLSBreakerState service_breaker_state(const char* name);
const struct SLSMeshEntry* service_metrics(const char* name);

/* Feeds sources 1 and 2 (endpoint probe, IPC queue saturation) into the
 * breakers for every LOCAL registration. BSP only -- it probes, and the
 * probe reads tcp_conns[] and the microkernel's services[]. Called from
 * the same heartbeat pass that already probes endpoints, so the two
 * automatic signals cost one shared walk. Returns breakers tripped. */
uint32_t mesh_observe_local(uint64_t now);

/* Manual override, for an operator who has fixed the underlying problem
 * and does not want to wait out the cooldown. */
int service_breaker_reset(const char* name);

void sys_sls_mesh_list(void);

#endif /* SERVICE_MESH_H */
