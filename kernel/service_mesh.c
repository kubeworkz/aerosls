/*
 * service_mesh.c — circuit breaking and per-service metrics
 * (Orchestration Plan Phase 6). See service_mesh.h for the design and,
 * in particular, for why IPC queue saturation is the signal that closes
 * the wedged-handler gap Phase 4 could not.
 *
 * Freestanding: local helpers, no libc.
 */
#include "service_mesh.h"
#include "ipc.h"
#include "kernel_io.h"

struct SLSMeshEntry mesh_entries[MESH_MAX_SERVICES];

/* ─── Local helpers ─────────────────────────────────────────────────── */
static int mh_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void mh_strcpy(char* d, const char* s, int n) {
    int i; for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}

const char* breaker_state_name(SLSBreakerState s) {
    switch (s) {
        case CB_CLOSED:    return "closed";
        case CB_OPEN:      return "open";
        case CB_HALF_OPEN: return "half-open";
        default:           return "unknown";
    }
}

void mesh_init(void) {
    for (int i = 0; i < MESH_MAX_SERVICES; i++) mesh_entries[i].active = 0;
    kernel_serial_print("[MESH] circuit breakers initialised (all closed).\n");
}

static struct SLSMeshEntry* mh_find(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < MESH_MAX_SERVICES; i++)
        if (mesh_entries[i].active && mh_streq(mesh_entries[i].name, name))
            return &mesh_entries[i];
    return 0;
}

/* Find or create. A breaker appears the first time anything says anything
 * about a service, so no caller has to register one first -- and a
 * service that has never failed simply has no entry, which reads the same
 * as a closed one. */
static struct SLSMeshEntry* mh_get(const char* name) {
    struct SLSMeshEntry* e = mh_find(name);
    if (e) return e;
    if (!name || !name[0]) return 0;
    for (int i = 0; i < MESH_MAX_SERVICES; i++) {
        if (mesh_entries[i].active) continue;
        e = &mesh_entries[i];
        /* Zero everything: a recycled slot must not inherit the previous
         * occupant's failure count and trip on its first call. */
        e->state                = CB_CLOSED;
        e->consecutive_failures = 0;
        e->opened_at_tick       = 0;
        e->trial_outstanding    = 0;
        e->calls_permitted = e->calls_refused = 0;
        e->successes = e->failures = e->trips = 0;
        mh_strcpy(e->name, name, SERVICE_NAME_LEN);
        e->active = 1;
        return e;
    }
    return 0;   /* table full -- see service_report_failure() */
}

/* ─── Outcome reporting ───────────────────────────────────────────────── */
static void mh_trip(struct SLSMeshEntry* e, uint64_t now, const char* why) {
    if (e->state == CB_OPEN) return;
    e->state          = CB_OPEN;
    e->opened_at_tick = now;
    e->trial_outstanding = 0;
    e->trips++;
    kernel_serial_printf("[MESH] breaker OPEN for '%s' after %u consecutive failures (%s).\n",
                         e->name, (unsigned)e->consecutive_failures, why);
}

void service_report_success(const char* name, uint64_t now) {
    (void)now;   /* success needs no timestamp; taken for symmetry, see the header */
    struct SLSMeshEntry* e = mh_get(name);
    if (!e) return;
    e->successes++;
    e->consecutive_failures = 0;
    e->trial_outstanding    = 0;

    /* A success in HALF_OPEN is the whole point of the trial: the service
     * is answering again, so close the breaker. */
    if (e->state == CB_HALF_OPEN) {
        e->state = CB_CLOSED;
        kernel_serial_printf("[MESH] breaker CLOSED for '%s' -- trial call succeeded.\n",
                             e->name);
    }
}

void service_report_failure(const char* name, uint64_t now) {
    struct SLSMeshEntry* e = mh_get(name);
    if (!e) {
        /* Table full. Logged rather than silent: a failure nobody can
         * record is a failure nobody will act on. */
        kernel_serial_printf("[MESH] no breaker slot for '%s' -- failure not tracked.\n",
                             name ? name : "");
        return;
    }
    e->failures++;
    e->consecutive_failures++;
    e->trial_outstanding = 0;

    /* A failure in HALF_OPEN re-opens immediately, without waiting to
     * re-reach the threshold. The trial existed precisely to answer "is it
     * better yet"; the answer was no. */
    if (e->state == CB_HALF_OPEN) {
        e->state = CB_CLOSED;              /* so mh_trip() will act */
        mh_trip(e, now, "trial call failed");
        return;
    }
    if (e->state == CB_CLOSED && e->consecutive_failures >= MESH_FAIL_THRESHOLD)
        mh_trip(e, now, "threshold reached");
}

/* ─── Admission ───────────────────────────────────────────────────────── */
int service_call_permitted(const char* name, uint64_t now) {
    struct SLSMeshEntry* e = mh_find(name);
    if (!e) return 1;   /* nothing known against it -- see service_breaker_state() */

    if (e->state == CB_CLOSED) { e->calls_permitted++; return 1; }

    if (e->state == CB_OPEN) {
        /* Saturating comparison: the tick counter is incremented by
         * whichever core takes the timer IRQ, so a reading can land
         * behind the stamp. Treating that as "no time has passed" is
         * correct; an unsigned subtraction would wrap and admit a trial
         * immediately. Same reasoning as service_registry.c's sr_age(). */
        uint64_t elapsed = (now > e->opened_at_tick) ? (now - e->opened_at_tick) : 0;
        if (elapsed < (uint64_t)MESH_OPEN_COOLDOWN_TICKS) { e->calls_refused++; return 0; }

        e->state = CB_HALF_OPEN;
        e->trial_outstanding = 1;
        e->calls_permitted++;
        kernel_serial_printf("[MESH] breaker HALF-OPEN for '%s' -- admitting one trial call.\n",
                             e->name);
        return 1;
    }

    /* HALF_OPEN: exactly one caller is already through and its outcome is
     * still unknown. Admitting a second would make the trial meaningless
     * -- a herd of calls into a service that is probably still broken is
     * what the breaker exists to prevent. */
    if (e->trial_outstanding) { e->calls_refused++; return 0; }

    /* HALF_OPEN with no outstanding trial: the previous one reported, and
     * reporting moves the state. Reaching here means something cleared
     * the flag without transitioning; admit and re-arm rather than wedge. */
    e->trial_outstanding = 1;
    e->calls_permitted++;
    return 1;
}

SLSBreakerState service_breaker_state(const char* name) {
    struct SLSMeshEntry* e = mh_find(name);
    /* No entry == no failure history. Returning OPEN here would refuse
     * every service before its first outcome was ever reported. */
    return e ? (SLSBreakerState)e->state : CB_CLOSED;
}

const struct SLSMeshEntry* service_metrics(const char* name) { return mh_find(name); }

int service_breaker_reset(const char* name) {
    struct SLSMeshEntry* e = mh_find(name);
    if (!e) return 1;
    e->state                = CB_CLOSED;
    e->consecutive_failures = 0;
    e->trial_outstanding    = 0;
    kernel_serial_printf("[MESH] breaker for '%s' manually reset to closed.\n", e->name);
    return 0;
}

/* ─── Automatic observation ───────────────────────────────────────────
 * Sources 1 and 2 from service_mesh.h. Runs over LOCAL registrations
 * only: this node can observe its own endpoints and nothing else's, and a
 * remote service's breaker belongs on the node that owns it. */
uint32_t mesh_observe_local(uint64_t now) {
    uint32_t tripped = 0;

    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        struct SLSServiceEntry* s = &services_registry[i];

        int failing = 0;
        const char* why = 0;

        /* Source 1: the endpoint probe cached by the heartbeat pass. */
        if (s->serving == SVC_SERVING_DOWN) { failing = 1; why = "endpoint down"; }

        /* Source 2: an IPC queue that is not being drained. THE
         * wedged-handler signal -- the port is bound and the watchdog is
         * happy, but messages are piling up because nothing is reading
         * them. ipc_queue_depth() returns -1 for a port outside the
         * supervised range, which is not evidence of anything. */
        if (!failing && s->endpoint_kind == SVC_ENDPOINT_IPC) {
            int depth = ipc_queue_depth((uint16_t)s->endpoint_port);
            if (depth >= 0 &&
                depth * MESH_QUEUE_SATURATED_DEN >=
                    IPC_QUEUE_DEPTH * MESH_QUEUE_SATURATED_NUM) {
                failing = 1; why = "IPC queue saturated -- handler not draining";
            }
        }

        struct SLSMeshEntry* e = mh_get(s->name);
        if (!e) continue;

        if (!failing) {
            /* An observation of health is not a call succeeding, so it
             * does not close a breaker on its own -- only a real trial
             * call does that. It does reset the consecutive count, so a
             * service that recovers before tripping is not carried
             * towards the threshold by history. */
            if (e->state == CB_CLOSED) e->consecutive_failures = 0;
            continue;
        }

        e->failures++;
        e->consecutive_failures++;
        if (e->state == CB_CLOSED && e->consecutive_failures >= MESH_FAIL_THRESHOLD) {
            mh_trip(e, now, why);
            tripped++;
        }
    }
    return tripped;
}

void sys_sls_mesh_list(void) {
    uint32_t n = 0;
    for (int i = 0; i < MESH_MAX_SERVICES; i++) if (mesh_entries[i].active) n++;
    kernel_serial_printf("[MESH] %u service breaker(s):\n", (unsigned)n);
    for (int i = 0; i < MESH_MAX_SERVICES; i++) {
        if (!mesh_entries[i].active) continue;
        struct SLSMeshEntry* e = &mesh_entries[i];
        kernel_serial_printf(
            "  %-24s %-10s fails=%llu(run %u) ok=%llu trips=%llu permitted=%llu refused=%llu\n",
            e->name, breaker_state_name((SLSBreakerState)e->state),
            (unsigned long long)e->failures, (unsigned)e->consecutive_failures,
            (unsigned long long)e->successes, (unsigned long long)e->trips,
            (unsigned long long)e->calls_permitted,
            (unsigned long long)e->calls_refused);
    }
}
