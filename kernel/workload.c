/*
 * workload.c — declarative workload objects + bounded reconciler
 * (Orchestration Plan Phase 5). See workload.h for the design, including
 * why the reconciler runs on the AP core and what that forces.
 *
 * Freestanding: local helpers, no libc, per this codebase's per-file
 * convention (p_memcpy in persist.c, sr_streq in service_registry.c).
 */
#include "workload.h"
#include "partition.h"
#include "persist.h"
#include "workload_ctx.h"
#include "timer.h"
#include "kernel_io.h"

struct SLSWorkloadEntry workloads[WORKLOAD_MAX];

/* ─── Local helpers ─────────────────────────────────────────────────── */
static int wl_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int wl_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void wl_strcpy(char* d, const char* s, int n) {
    int i; for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}

const char* workload_restart_policy_name(SLSWorkloadRestartPolicy p) {
    switch (p) {
        case WL_RESTART_NEVER:      return "never";
        case WL_RESTART_ON_FAILURE: return "on-failure";
        case WL_RESTART_ALWAYS:     return "always";
        default:                    return "never";
    }
}

const char* workload_status_name(SLSWorkloadStatus s) {
    switch (s) {
        case WL_OK:            return "OK";
        case WL_ERR_NAME:      return "ERR:bad-name";
        case WL_ERR_PARTITION: return "ERR:no-such-partition";
        case WL_ERR_FULL:      return "ERR:workload-table-full";
        case WL_ERR_NOT_FOUND: return "ERR:not-found";
        case WL_ERR_PERM:      return "ERR:requires-DB_ADMIN";
        case WL_ERR_ENDPOINT:  return "ERR:bad-endpoint";
        default:               return "ERR:unknown";
    }
}

/* ─── Declaration ─────────────────────────────────────────────────────── */
void workload_init(void) {
    for (int i = 0; i < WORKLOAD_MAX; i++) workloads[i].active = 0;
    kernel_serial_print("[WORKLOAD] declarative workloads initialised (reconciler OFF).\n");
}

struct SLSWorkloadEntry* workload_find(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < WORKLOAD_MAX; i++)
        if (workloads[i].active && wl_streq(workloads[i].name, name)) return &workloads[i];
    return 0;
}

uint32_t workload_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < WORKLOAD_MAX; i++) if (workloads[i].active) n++;
    return n;
}

/* Saturating age, for the same reason service_registry.c's sr_age() has
 * one: kernel_tick_counter is incremented by whichever core takes the
 * timer IRQ, so a reading can land behind a stamp. An unsigned
 * subtraction would wrap to an enormous age and fire a restart instantly,
 * defeating the backoff entirely. */
static uint64_t wl_age(uint64_t now, uint64_t stamped) {
    return (now > stamped) ? (now - stamped) : 0;
}

/* Exponential backoff before the NEXT attempt, given how many have
 * already been made. Doubles per attempt to a cap:
 *
 *   attempts made:  1      2      3      4    ...
 *   wait before
 *   the next:       base   2x     4x     8x   ... capped at MAX
 *
 * The loop starts at 1, not 0, so the FIRST retry waits `base` rather
 * than 2x. That matters: a transient fault should be retried quickly, and
 * starting a doubling sequence one step in delays every recovery for no
 * reason. The test caught this -- it had been off by one doubling. */
static uint64_t wl_backoff(uint32_t attempts_made) {
    uint64_t b = WL_BACKOFF_BASE_TICKS;
    for (uint32_t i = 1; i < attempts_made && b < (uint64_t)WL_BACKOFF_MAX_TICKS; i++) b *= 2;
    return b > (uint64_t)WL_BACKOFF_MAX_TICKS ? (uint64_t)WL_BACKOFF_MAX_TICKS : b;
}

static int wl_may_mutate(uint32_t caller_uid) {
    return catalog_get_role(caller_uid) <= ROLE_DB_ADMIN;
}

SLSWorkloadStatus workload_declare(uint32_t caller_uid, const char* name,
                                   uint32_t partition_id,
                                   SLSWorkloadDesired desired,
                                   const char* service_name,
                                   SLSServiceEndpointKind kind,
                                   uint32_t endpoint_port,
                                   const char* program_name,
                                   const char* entry_name,
                                   SLSWorkloadRestartPolicy restart_policy) {
    int len = wl_strlen(name);
    if (len == 0 || len >= WORKLOAD_NAME_LEN) return WL_ERR_NAME;
    if (!wl_may_mutate(caller_uid))           return WL_ERR_PERM;
    if (!partition_exists(partition_id))      return WL_ERR_PARTITION;

    /* A declared service must be fully specified or fully absent. A name
     * with no port would reconcile into a registration this kernel would
     * then refuse, looping forever without converging. */
    int slen = wl_strlen(service_name);
    if (slen >= SERVICE_NAME_LEN) return WL_ERR_NAME;
    if (slen > 0) {
        if (endpoint_port == 0) return WL_ERR_ENDPOINT;
        if (kind != SVC_ENDPOINT_IPC && kind != SVC_ENDPOINT_TCP) return WL_ERR_ENDPOINT;
    }

    struct SLSWorkloadEntry* e = workload_find(name);
    if (!e) {
        for (int i = 0; i < WORKLOAD_MAX; i++) {
            if (workloads[i].active) continue;
            e = &workloads[i];
            wl_strcpy(e->name, name, WORKLOAD_NAME_LEN);
            e->active        = 1;
            e->actions_taken = 0;
            e->restart_count = e->restarts_total = 0;
            e->last_restart_tick = e->started_tick = 0;
            e->gave_up = 0;
            break;
        }
        if (!e) return WL_ERR_FULL;
    }

    e->partition_id  = partition_id;
    e->desired_state = (uint8_t)desired;
    wl_strcpy(e->service_name, service_name ? service_name : "", SERVICE_NAME_LEN);
    e->endpoint_kind = (uint8_t)kind;
    e->endpoint_port = endpoint_port;
    wl_strcpy(e->program_name, program_name ? program_name : "", WORKLOAD_NAME_LEN);
    wl_strcpy(e->entry_name, (entry_name && entry_name[0]) ? entry_name : "main",
              (int)sizeof(e->entry_name));
    /* An unrecognised policy value becomes NEVER rather than something
     * more eager: a caller that got this wrong should not thereby opt into
     * autonomous restarts. */
    e->restart_policy = (restart_policy == WL_RESTART_ON_FAILURE ||
                         restart_policy == WL_RESTART_ALWAYS)
                        ? (uint8_t)restart_policy : (uint8_t)WL_RESTART_NEVER;
    /* Re-declaring is an operator intervention: clear the give-up so a
     * corrected declaration is actually retried. */
    e->gave_up       = 0;
    e->restart_count = 0;
    e->converged     = 0;   /* newly declared/changed -- not yet known converged */

    kernel_serial_printf("[WORKLOAD] declared '%s': partition %u, desired=%s%s.\n",
                         e->name, (unsigned)partition_id,
                         desired == WL_DESIRED_RUNNING ? "RUNNING" : "STOPPED",
                         slen > 0 ? ", exposes a service" : "");
    persist_workloads();
    return WL_OK;
}

SLSWorkloadStatus workload_delete(uint32_t caller_uid, const char* name) {
    if (!wl_may_mutate(caller_uid)) return WL_ERR_PERM;
    struct SLSWorkloadEntry* e = workload_find(name);
    if (!e) return WL_ERR_NOT_FOUND;
    e->active = 0;
    kernel_serial_printf("[WORKLOAD] deleted '%s'.\n", name);
    persist_workloads();
    return WL_OK;
}

int workload_clear_giveup(const char* name) {
    struct SLSWorkloadEntry* e = workload_find(name);
    if (!e) return 1;
    e->gave_up       = 0;
    e->restart_count = 0;
    kernel_serial_printf("[WORKLOAD] '%s': give-up cleared, restarts re-armed.\n", e->name);
    return 0;
}

/* ─── Intent ring (AP produces, BSP consumes) ─────────────────────────
 * Power-of-two depth so the index wrap is a mask, and head/tail are read
 * and written with __atomic -- the same discipline kernel/smp.c uses for
 * ap_bootstrap_lock. With exactly one producer and one consumer this
 * needs no lock: the producer only ever advances head, the consumer only
 * ever advances tail, and each observes the other's index atomically. */
#define WL_INTENT_MAX 64   /* must stay a power of two */

/* Sized so a SINGLE sweep can never overflow it. Pass 2 emits at most
 * TWO intents per workload -- one for its service, one for its execution
 * context -- so a full sweep produces at most 2 * WORKLOAD_MAX. Overflow
 * therefore means one specific thing: the BSP has not drained between
 * sweeps. That is what makes the dropped counter diagnostic rather than
 * noise.
 *
 * This assert has already earned itself once: the ring was 32 and the
 * bound was 1 * WORKLOAD_MAX when a workload could only emit a service
 * intent. Adding the context intent doubled the real bound, and the
 * compile error was what said so -- rather than the property quietly
 * becoming false and overflow starting to mean nothing in particular. */
/* Still 2 after Phase 7 added CTX_RESTART: the three context intents
 * (start / restart / stop) sit in one if/else-if chain and are mutually
 * exclusive, so a workload emits at most one service intent and at most
 * one context intent per sweep. Re-derived deliberately rather than
 * assumed -- this assert already caught the bound going stale once. */
#define WL_INTENTS_PER_WORKLOAD 2
_Static_assert(WL_INTENT_MAX >= WL_INTENTS_PER_WORKLOAD * WORKLOAD_MAX,
               "intent ring must absorb a full sweep; see the note above");
_Static_assert((WL_INTENT_MAX & (WL_INTENT_MAX - 1)) == 0,
               "intent ring depth must be a power of two -- the index wrap is a mask");

typedef enum {
    WL_INTENT_REGISTER_SERVICE = 0,
    WL_INTENT_UNREGISTER_SERVICE,
    WL_INTENT_PROBE,            /* test seam only -- see reconcile_enqueue_probe() */
    /* Starting a context reads the uploaded binary store and registers
     * into the migration registry; stopping unregisters. Both are BSP-only
     * work, so both travel as intents rather than being done in the sweep
     * -- the same reason the service actions do. */
    WL_INTENT_CTX_START,
    WL_INTENT_CTX_STOP,
    WL_INTENT_CTX_RESTART,
} WLIntentKind;

struct WLIntent {
    uint8_t  kind;
    uint32_t caller_uid;
    char     service_name[SERVICE_NAME_LEN];
    uint32_t partition_id;
    uint32_t endpoint_port;
    uint8_t  endpoint_kind;
    uint32_t seq;               /* probe sequence number; unused by real intents */
    char     program_name[WORKLOAD_NAME_LEN];   /* CTX_START/STOP */
    char     entry_name[32];                    /* CTX_START */
    char     workload_name[WORKLOAD_NAME_LEN];  /* CTX_START/STOP */
};

static struct WLIntent wl_ring[WL_INTENT_MAX];
static volatile uint32_t wl_head = 0;   /* written by the AP core only */
static volatile uint32_t wl_tail = 0;   /* written by the BSP only */
static volatile uint32_t wl_dropped = 0;

static int wl_reconcile_on = 0;

void reconcile_set_enabled(int on) {
    wl_reconcile_on = on ? 1 : 0;
    kernel_serial_printf("[RECONCILE] %s.\n", wl_reconcile_on ? "ENABLED" : "disabled");
}
int reconcile_is_enabled(void) { return wl_reconcile_on; }

uint32_t reconcile_queue_depth(void) {
    uint32_t h = __atomic_load_n(&wl_head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&wl_tail, __ATOMIC_ACQUIRE);
    return h - t;
}
uint32_t reconcile_queue_dropped(void) {
    return __atomic_load_n(&wl_dropped, __ATOMIC_ACQUIRE);
}

static int wl_enqueue(const struct WLIntent* in) {
    uint32_t h = __atomic_load_n(&wl_head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&wl_tail, __ATOMIC_ACQUIRE);
    if (h - t >= WL_INTENT_MAX) {
        /* Full. Counted and logged, never silent -- and safe to lose,
         * because reconcile_tick() re-reads ACTUAL state every sweep
         * rather than assuming a past enqueue took effect. An intent
         * dropped here is simply re-derived next sweep. */
        __atomic_fetch_add(&wl_dropped, 1, __ATOMIC_RELEASE);
        kernel_serial_print("[RECONCILE] intent queue full -- will retry next sweep.\n");
        return 1;
    }
    wl_ring[h & (WL_INTENT_MAX - 1)] = *in;
    /* Release: the slot's contents must be visible before the consumer
     * can see the advanced head. */
    __atomic_store_n(&wl_head, h + 1, __ATOMIC_RELEASE);
    return 0;
}

uint32_t reconcile_drain(void) {
    uint32_t applied = 0;
    for (;;) {
        uint32_t t = __atomic_load_n(&wl_tail, __ATOMIC_RELAXED);
        uint32_t h = __atomic_load_n(&wl_head, __ATOMIC_ACQUIRE);
        if (t == h) break;

        struct WLIntent in = wl_ring[t & (WL_INTENT_MAX - 1)];
        __atomic_store_n(&wl_tail, t + 1, __ATOMIC_RELEASE);

        /* These persist. That is the entire reason this runs on the BSP
         * and reconcile_tick() does not do it directly. */
        if (in.kind == WL_INTENT_PROBE) { applied++; continue; }   /* test seam; no action */

        if (in.kind == WL_INTENT_CTX_START) {
            uint32_t sz = 0;
            const uint8_t* img = workload_find_program(in.program_name, &sz);
            if (!img) {
                kernel_serial_printf("[RECONCILE] '%s': no uploaded program '%s'.\n",
                                     in.workload_name, in.program_name);
            } else {
                WLCtxStatus rc = wlctx_start(in.workload_name, img, sz,
                                             in.entry_name, in.partition_id);
                kernel_serial_printf("[RECONCILE] start context '%s' -> %s\n",
                                     in.workload_name, wlctx_status_name(rc));
            }
            applied++; continue;
        }
        if (in.kind == WL_INTENT_CTX_STOP) {
            wlctx_stop(in.workload_name);
            applied++; continue;
        }
        if (in.kind == WL_INTENT_CTX_RESTART) {
            WLCtxStatus rc = wlctx_restart(in.workload_name);
            kernel_serial_printf("[RECONCILE] restart context '%s' -> %s\n",
                                 in.workload_name, wlctx_status_name(rc));
            applied++; continue;
        }

        if (in.kind == WL_INTENT_REGISTER_SERVICE) {
            SLSServiceStatus rc = service_register(in.caller_uid, in.service_name,
                                                   in.partition_id,
                                                   (SLSServiceEndpointKind)in.endpoint_kind,
                                                   in.endpoint_port);
            kernel_serial_printf("[RECONCILE] register '%s' -> %s\n",
                                 in.service_name, service_status_name(rc));
        } else {
            SLSServiceStatus rc = service_unregister(in.caller_uid, in.service_name);
            kernel_serial_printf("[RECONCILE] unregister '%s' -> %s\n",
                                 in.service_name, service_status_name(rc));
        }
        applied++;
    }
    return applied;
}

/* ─── Ring test seam ──────────────────────────────────────────────────
 * See workload.h. These go through the SAME wl_enqueue() and the same
 * head/tail protocol the real path uses -- a probe that took a different
 * route would verify nothing about the real one. The payload is filled
 * completely (not just `seq`) so a torn write shows up as a corrupted
 * field rather than being invisible. */
int reconcile_enqueue_probe(uint32_t seq) {
    struct WLIntent in;
    in.kind          = WL_INTENT_PROBE;
    in.caller_uid    = seq;          /* redundant copies: a torn intent */
    in.partition_id  = seq;          /* shows up as these disagreeing    */
    in.endpoint_port = seq;
    in.endpoint_kind = (uint8_t)(seq & 0xFF);
    in.seq           = seq;
    for (int i = 0; i < SERVICE_NAME_LEN; i++)
        in.service_name[i] = (char)('a' + (int)((seq + (uint32_t)i) % 26));
    return wl_enqueue(&in);
}

uint32_t reconcile_drain_probe(uint32_t* out_seq, uint32_t max) {
    uint32_t n = 0;
    while (n < max) {
        uint32_t t = __atomic_load_n(&wl_tail, __ATOMIC_RELAXED);
        uint32_t h = __atomic_load_n(&wl_head, __ATOMIC_ACQUIRE);
        if (t == h) break;

        struct WLIntent in = wl_ring[t & (WL_INTENT_MAX - 1)];
        __atomic_store_n(&wl_tail, t + 1, __ATOMIC_RELEASE);

        if (in.kind != WL_INTENT_PROBE) continue;   /* not ours; ignore */

        /* Torn-payload detection: every redundant copy must agree, and
         * the name must be the pattern this seq implies. Encoded as a
         * poisoned seq so the caller sees it as a mismatch. */
        uint32_t reported = in.seq;
        if (in.caller_uid != in.seq || in.partition_id != in.seq ||
            in.endpoint_port != in.seq ||
            in.endpoint_kind != (uint8_t)(in.seq & 0xFF)) {
            reported = 0xFFFFFFFFu;
        } else {
            for (int i = 0; i < SERVICE_NAME_LEN; i++) {
                if (in.service_name[i] != (char)('a' + (int)((in.seq + (uint32_t)i) % 26))) {
                    reported = 0xFFFFFFFFu; break;
                }
            }
        }
        if (out_seq) out_seq[n] = reported;
        n++;
    }
    return n;
}

/* ─── The sweep ───────────────────────────────────────────────────────
 * Four conditions, each logged. Reads actual state fresh every time, so
 * it is idempotent and safe to re-run after a dropped intent. */
uint32_t reconcile_tick(void) {
    if (!wl_reconcile_on) return 0;
    const uint64_t now = kernel_tick_counter;

    uint32_t actions = 0;

    /* ── Pass 1: partition run-state, decided ONCE per partition ──────
     * A partition is not owned by a single workload -- several can share
     * one. Deciding pause/resume per workload therefore lets two
     * declarations with opposite desired states fight: each sweep, one
     * pauses and the other resumes, forever, and the reconciler never
     * settles. That is not hypothetical; it is what the first version of
     * this function did, and tests/workload_reconcile_host_test.c
     * scenario 7 caught it.
     *
     * The rule is union semantics: a partition runs if ANY active
     * workload in it wants to run. It is deterministic (independent of
     * table order), it matches what the words mean -- a partition has to
     * be up for anything in it to run -- and it removes the oscillation
     * by construction rather than by tie-breaking. A workload that wants
     * to be STOPPED still withdraws its own service in pass 2; what it
     * cannot do is take the whole partition down from under its
     * neighbours. */
    uint8_t want_running[PARTITION_MAX];
    uint8_t has_workload[PARTITION_MAX];
    for (uint32_t p = 0; p < PARTITION_MAX; p++) { want_running[p] = 0; has_workload[p] = 0; }

    for (int i = 0; i < WORKLOAD_MAX; i++) {
        struct SLSWorkloadEntry* w = &workloads[i];
        if (!w->active) continue;
        if (w->partition_id >= PARTITION_MAX) continue;
        if (!partition_exists(w->partition_id)) continue;
        has_workload[w->partition_id] = 1;
        if (w->desired_state == WL_DESIRED_RUNNING) want_running[w->partition_id] = 1;
    }

    for (uint32_t p = 0; p < PARTITION_MAX; p++) {
        if (!has_workload[p]) continue;
        int paused = partition_is_paused(p);
        /* Neither call persists -- partition_paused[] is deliberately
         * ephemeral runtime state (see partition.c) -- so both are safe
         * here on the AP core. */
        if (want_running[p] && paused) {
            partition_resume(p);
            kernel_serial_printf("[RECONCILE] resumed partition %u (a workload wants it running).\n",
                                 (unsigned)p);
            actions++;
        } else if (!want_running[p] && !paused) {
            partition_pause(p);
            kernel_serial_printf("[RECONCILE] paused partition %u (no workload wants it running).\n",
                                 (unsigned)p);
            actions++;
        }
    }

    /* ── Pass 2: per-workload service advertisement ───────────────────
     * No conflict is possible here: a service name belongs to one
     * declaration, so each workload's own service is reconciled
     * independently. */
    for (int i = 0; i < WORKLOAD_MAX; i++) {
        struct SLSWorkloadEntry* w = &workloads[i];
        if (!w->active) continue;

        /* A declaration whose partition has since been destroyed cannot
         * converge. Reported once, then left alone -- retrying something
         * that can never succeed is churn, not resilience. */
        if (!partition_exists(w->partition_id)) {
            if (w->converged) {
                w->converged = 0;
                kernel_serial_printf(
                    "[RECONCILE] '%s' cannot converge: partition %u no longer exists.\n",
                    w->name, (unsigned)w->partition_id);
            }
            continue;
        }

        uint32_t before = actions;

        if (w->service_name[0] != '\0') {
            struct SLSServiceLocation loc;
            int registered = (service_resolve(w->service_name, &loc) == SVC_REG_OK);

            if (w->desired_state == WL_DESIRED_RUNNING) {
                /* Present AND correct. A registration pointing at the
                 * wrong partition or port is as wrong as a missing one. */
                int wrong = registered && (loc.partition_id  != w->partition_id ||
                                           loc.endpoint_port != w->endpoint_port ||
                                           loc.endpoint_kind != w->endpoint_kind);
                if (!registered || wrong) {
                    struct WLIntent in;
                    in.kind          = WL_INTENT_REGISTER_SERVICE;
                    in.caller_uid    = 0;   /* the reconciler acts as the kernel */
                    wl_strcpy(in.service_name, w->service_name, SERVICE_NAME_LEN);
                    in.partition_id  = w->partition_id;
                    in.endpoint_port = w->endpoint_port;
                    in.endpoint_kind = w->endpoint_kind;
                    wl_enqueue(&in);
                    actions++;
                }
            } else if (registered) {
                struct WLIntent in;
                in.kind          = WL_INTENT_UNREGISTER_SERVICE;
                in.caller_uid    = 0;
                wl_strcpy(in.service_name, w->service_name, SERVICE_NAME_LEN);
                in.partition_id  = w->partition_id;
                in.endpoint_port = 0;
                in.endpoint_kind = 0;
                wl_enqueue(&in);
                actions++;
            }
        }

        /* ── Live execution context ───────────────────────────────────
         * The condition that closes PEC Phase 3's reachability gap: a
         * RUNNING workload with a declared program should have a live
         * context, and that context is what partition_migrate() moves.
         *
         * Both directions are queued rather than done here. Starting one
         * reads the uploaded binary store and registers into the
         * migration registry; both are BSP-owned state, and this sweep
         * runs on the AP core. */
        if (w->program_name[0] != '\0') {
            int live = wlctx_has(w->name);
            if (w->desired_state == WL_DESIRED_RUNNING && !live) {
                struct WLIntent in;
                in.kind         = WL_INTENT_CTX_START;
                in.caller_uid   = 0;
                in.partition_id = w->partition_id;
                in.endpoint_port = 0; in.endpoint_kind = 0;
                in.service_name[0] = '\0';
                wl_strcpy(in.workload_name, w->name, WORKLOAD_NAME_LEN);
                wl_strcpy(in.program_name, w->program_name, WORKLOAD_NAME_LEN);
                wl_strcpy(in.entry_name, w->entry_name, (int)sizeof(in.entry_name));
                wl_enqueue(&in);
                w->started_tick = now;   /* the stability window starts here */
                actions++;
            } else if (w->desired_state == WL_DESIRED_RUNNING && live) {
                /* ── Restart (Phase 7) ─────────────────────────────────
                 * The context exists. Is it still running, and if not,
                 * does the operator want it back? */
                int st = wlctx_status_of(w->name);

                /* SIMI_STATUS_OK == still executing. Nothing to do, and
                 * this is where a stable run earns back its attempts. */
                if (st == (int)SIMI_STATUS_OK) {
                    if (w->restart_count > 0 && w->started_tick &&
                        wl_age(now, w->started_tick) >= (uint64_t)WL_STABLE_RESET_TICKS) {
                        kernel_serial_printf(
                            "[RECONCILE] '%s' stable for %u ticks -- restart budget reset.\n",
                            w->name, (unsigned)WL_STABLE_RESET_TICKS);
                        w->restart_count = 0;
                    }
                } else if (st >= 0 && w->gave_up) {
                    /* Already abandoned. Deliberately silent: re-logging
                     * "GIVING UP" on every sweep for the rest of uptime
                     * would bury every other message on the console. The
                     * flag exists so the decision is announced exactly
                     * once and then reported on demand, not repeated. */
                } else if (st >= 0) {
                    /* Terminal. THE distinction: HALTED means the program
                     * RETURNED -- it finished. Restarting it on that basis
                     * turns a batch job into an infinite loop, so only an
                     * explicit ALWAYS does. A TRAP is a genuine fault. */
                    int is_failure = (st != (int)SIMI_STATUS_HALTED);
                    int want =
                        (w->restart_policy == WL_RESTART_ALWAYS) ||
                        (w->restart_policy == WL_RESTART_ON_FAILURE && is_failure);

                    if (want) {
                        if (w->restart_count >= WL_RESTART_MAX_ATTEMPTS) {
                            /* Give up ONCE, loudly, and stop. Something
                             * that has failed this many times needs a
                             * human, not an eleventh attempt -- and a
                             * crash loop must not be able to burn the
                             * machine for the rest of its uptime. */
                            w->gave_up = 1;
                            kernel_serial_printf(
                                "[RECONCILE] '%s' GIVING UP after %u restarts -- "
                                "no further attempts until re-declared or cleared.\n",
                                w->name, (unsigned)w->restart_count);
                        } else if (wl_age(now, w->last_restart_tick) >= wl_backoff(w->restart_count)
                                   || w->last_restart_tick == 0) {
                            struct WLIntent in;
                            in.kind         = WL_INTENT_CTX_RESTART;
                            in.caller_uid   = 0;
                            in.partition_id = w->partition_id;
                            in.endpoint_port = 0; in.endpoint_kind = 0;
                            in.service_name[0] = '\0';
                            wl_strcpy(in.workload_name, w->name, WORKLOAD_NAME_LEN);
                            wl_strcpy(in.program_name, w->program_name, WORKLOAD_NAME_LEN);
                            wl_strcpy(in.entry_name, w->entry_name, (int)sizeof(in.entry_name));
                            wl_enqueue(&in);
                            w->restart_count++;
                            w->restarts_total++;
                            w->last_restart_tick = now;
                            w->started_tick      = now;
                            kernel_serial_printf(
                                "[RECONCILE] '%s' %s -- restart %u/%u queued.\n",
                                w->name, is_failure ? "trapped" : "halted",
                                (unsigned)w->restart_count, WL_RESTART_MAX_ATTEMPTS);
                            actions++;
                        }
                        /* else: inside the backoff window -- deliberately
                         * NOT counted as an action, so a workload waiting
                         * out its backoff still reports converged rather
                         * than making the whole sweep look busy. */
                    }
                }
            } else if (w->desired_state == WL_DESIRED_STOPPED && live) {
                struct WLIntent in;
                in.kind         = WL_INTENT_CTX_STOP;
                in.caller_uid   = 0;
                in.partition_id = w->partition_id;
                in.endpoint_port = 0; in.endpoint_kind = 0;
                in.service_name[0] = '\0';
                wl_strcpy(in.workload_name, w->name, WORKLOAD_NAME_LEN);
                in.program_name[0] = '\0';
                in.entry_name[0]   = '\0';
                wl_enqueue(&in);
                actions++;
            }
        }

        uint32_t took = actions - before;
        w->actions_taken += took;
        w->converged = (took == 0);
    }

    return actions;
}

void sys_sls_workload_list(void) {
    kernel_serial_printf("[WORKLOAD] %u declaration(s), reconciler %s, queue depth %u, dropped %u:\n",
                         (unsigned)workload_count(),
                         reconcile_is_enabled() ? "ON" : "off",
                         (unsigned)reconcile_queue_depth(),
                         (unsigned)reconcile_queue_dropped());
    for (int i = 0; i < WORKLOAD_MAX; i++) {
        if (!workloads[i].active) continue;
        struct SLSWorkloadEntry* w = &workloads[i];
        kernel_serial_printf(
            "  %-18s partition=%u desired=%-7s prog=%-12s restart=%-10s "
            "tries=%u/%u total=%u%s %s\n",
            w->name, (unsigned)w->partition_id,
            w->desired_state == WL_DESIRED_RUNNING ? "RUNNING" : "STOPPED",
            w->program_name[0] ? w->program_name : "-",
            workload_restart_policy_name((SLSWorkloadRestartPolicy)w->restart_policy),
            (unsigned)w->restart_count, WL_RESTART_MAX_ATTEMPTS,
            (unsigned)w->restarts_total,
            w->gave_up ? " [GAVE UP]" : "",
            w->converged ? "[converged]" : "[pending]");
    }
}

/* ─── Syscall surface ─────────────────────────────────────────────────── */
uint64_t sys_sls_workload_declare(struct SLSWorkloadDeclareRequest* req) {
    if (!req) return (uint64_t)WL_ERR_NAME;
    return (uint64_t)workload_declare(req->caller_uid, req->name, req->partition_id,
                                      (SLSWorkloadDesired)req->desired_state,
                                      req->service_name,
                                      (SLSServiceEndpointKind)req->endpoint_kind,
                                      req->endpoint_port,
                                      req->program_name, req->entry_name,
                                      (SLSWorkloadRestartPolicy)req->restart_policy);
}

uint64_t sys_sls_workload_delete(struct SLSWorkloadDeclareRequest* req) {
    if (!req) return (uint64_t)WL_ERR_NAME;
    return (uint64_t)workload_delete(req->caller_uid, req->name);
}

uint64_t sys_sls_reconcile_enable(uint32_t on) {
    reconcile_set_enabled((int)on);
    return 0;
}
