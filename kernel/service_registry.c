/*
 * service_registry.c — name → partition/node/endpoint resolution
 * (Orchestration Plan Phase 4). See service_registry.h for the design,
 * including why the node id is derived rather than stored.
 *
 * Freestanding: local helpers, no libc, per this codebase's per-file
 * convention (p_memcpy in persist.c, tn_streq in tenant.c).
 */
#include "service_registry.h"
#include "partition.h"
#include "persist.h"
#include "kernel_io.h"
#include "../net/consensus.h"   /* cluster_local_node_id() */
#include "../net/dspp.h"       /* dspp_service_announce()/_withdraw() */
#include "timer.h"             /* kernel_tick_counter */
#include "microkernel.h"        /* mk_ipc_port_state() -- IPC endpoint liveness */
#include "../net/tcp.h"        /* tcp_port_is_listening() -- TCP endpoint liveness */

struct SLSServiceEntry  services_registry[SERVICE_MAX];
struct SLSRemoteService services_remote[SERVICE_REMOTE_MAX];

/* ─── Local helpers ─────────────────────────────────────────────────── */
static int sr_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int sr_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void sr_strcpy(char* d, const char* s, int n) {
    int i; for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = '\0';
}

const char* service_health_name(SLSServiceHealth h) {
    switch (h) {
        case SVC_HEALTH_FRESH:   return "fresh";
        case SVC_HEALTH_STALE:   return "stale";
        case SVC_HEALTH_EXPIRED: return "expired";
        default:                 return "unknown";
    }
}

const char* service_serving_name(SLSServiceServing v) {
    switch (v) {
        case SVC_SERVING_UP:      return "up";
        case SVC_SERVING_DOWN:    return "down";
        case SVC_SERVING_UNKNOWN: return "unknown";
        default:                  return "unknown";
    }
}

SLSServiceServing service_probe_local(uint8_t endpoint_kind, uint32_t endpoint_port) {
    if (endpoint_kind == SVC_ENDPOINT_TCP) {
        /* A LISTEN socket means something bound the port and is accepting.
         * Absence means nothing is -- which for a service that claims to
         * be there IS the failure worth reporting. */
        return tcp_port_is_listening((uint16_t)endpoint_port)
               ? SVC_SERVING_UP : SVC_SERVING_DOWN;
    }
    if (endpoint_kind == SVC_ENDPOINT_IPC) {
        int st = mk_ipc_port_state((uint16_t)endpoint_port);
        if (st < 0) return SVC_SERVING_UNKNOWN;   /* no supervised owner -- not ours to judge */
        /* DEGRADED counts as DOWN for routing purposes: the watchdog is
         * saying this service is not healthy, and sending it traffic on
         * the strength of "well, it has not fully crashed" is how a
         * degraded service becomes an outage. */
        return (st == SVC_STATE_ONLINE) ? SVC_SERVING_UP : SVC_SERVING_DOWN;
    }
    return SVC_SERVING_UNKNOWN;
}

uint32_t service_probe_all_local(void) {
    uint32_t changed = 0;
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        struct SLSServiceEntry* e = &services_registry[i];
        uint8_t now_serving = (uint8_t)service_probe_local(e->endpoint_kind, e->endpoint_port);
        if (now_serving == e->serving) continue;
        kernel_serial_printf("[SERVICE] '%s' endpoint went %s -> %s.\n", e->name,
                             service_serving_name((SLSServiceServing)e->serving),
                             service_serving_name((SLSServiceServing)now_serving));
        e->serving = now_serving;
        changed++;
    }
    return changed;
}

/* Age of an entry, saturating at 0 rather than wrapping. The tick counter
 * is incremented from whichever core takes the timer IRQ, so a reading
 * taken here can very occasionally be marginally behind one stamped
 * earlier. Treating that as "age 0" is correct -- it means "refreshed at
 * least as recently as now" -- whereas an unsigned subtraction would wrap
 * to an enormous age and expire a perfectly healthy entry. */
static uint64_t sr_age(uint64_t now, uint64_t stamped) {
    return (now > stamped) ? (now - stamped) : 0;
}

static SLSServiceHealth sr_health_of(uint64_t now, uint64_t stamped) {
    uint64_t age = sr_age(now, stamped);
    if (age >= (uint64_t)SERVICE_REMOTE_TTL_TICKS)      return SVC_HEALTH_EXPIRED;
    if (age >= (uint64_t)SERVICE_HEARTBEAT_TICKS * 2u)  return SVC_HEALTH_STALE;
    return SVC_HEALTH_FRESH;
}

const char* service_status_name(SLSServiceStatus s) {
    switch (s) {
        case SVC_REG_OK:            return "OK";
        case SVC_REG_ERR_NAME:      return "ERR:bad-name";
        case SVC_REG_ERR_PARTITION: return "ERR:no-such-partition";
        case SVC_REG_ERR_FULL:      return "ERR:registry-full";
        case SVC_REG_ERR_EXISTS:    return "ERR:already-registered";
        case SVC_REG_ERR_NOT_FOUND: return "ERR:not-found";
        case SVC_REG_ERR_PERM:      return "ERR:requires-DB_ADMIN";
        case SVC_REG_ERR_ENDPOINT:  return "ERR:bad-endpoint";
        default:                    return "ERR:unknown";
    }
}

void service_registry_init(void) {
    for (int i = 0; i < SERVICE_MAX; i++) services_registry[i].active = 0;
    for (int i = 0; i < SERVICE_REMOTE_MAX; i++) services_remote[i].active = 0;
    kernel_serial_print("[SERVICE] service registry initialised (0 registrations).\n");
}

static struct SLSServiceEntry* sr_find(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < SERVICE_MAX; i++)
        if (services_registry[i].active && sr_streq(services_registry[i].name, name))
            return &services_registry[i];
    return 0;
}

/* Mutation requires DB_ADMIN or higher -- the same bar partition and
 * tenant creation use. Resolution deliberately does not; see the header. */
static int sr_may_mutate(uint32_t caller_uid) {
    return catalog_get_role(caller_uid) <= ROLE_DB_ADMIN;
}

SLSServiceStatus service_register(uint32_t caller_uid, const char* name,
                                  uint32_t partition_id,
                                  SLSServiceEndpointKind kind,
                                  uint32_t endpoint_port) {
    int len = sr_strlen(name);
    if (len == 0 || len >= SERVICE_NAME_LEN)              return SVC_REG_ERR_NAME;
    if (!sr_may_mutate(caller_uid))                       return SVC_REG_ERR_PERM;
    if (kind != SVC_ENDPOINT_IPC && kind != SVC_ENDPOINT_TCP) return SVC_REG_ERR_ENDPOINT;
    if (endpoint_port == 0)                               return SVC_REG_ERR_ENDPOINT;

    /* The partition must actually exist. A registration pointing at an
     * undefined partition would resolve to whatever node
     * partition_get_owner_node() reports for a nonexistent partition (0,
     * the uninitialized sentinel) -- a confidently wrong answer, which is
     * worse than refusing to register. */
    if (!partition_exists(partition_id))                  return SVC_REG_ERR_PARTITION;

    /* Re-registering an existing name UPDATES it rather than failing.
     * A service that restarts on a new port must be able to say so, and
     * forcing unregister-then-register would leave a window in which the
     * name does not resolve at all. */
    struct SLSServiceEntry* e = sr_find(name);
    if (!e) {
        for (int i = 0; i < SERVICE_MAX; i++) {
            if (services_registry[i].active) continue;
            e = &services_registry[i];
            sr_strcpy(e->name, name, SERVICE_NAME_LEN);
            e->active = 1;
            break;
        }
        if (!e) {
            kernel_serial_printf("[SERVICE] ERROR: registry full (%d).\n", SERVICE_MAX);
            return SVC_REG_ERR_FULL;
        }
    }

    e->partition_id  = partition_id;
    e->endpoint_kind = (uint8_t)kind;
    e->endpoint_port = endpoint_port;
    e->owner_uid     = caller_uid;
    /* Probe immediately rather than waiting for the first heartbeat: an
     * operator who registers a service and then asks about it should get
     * the truth, not UNKNOWN for up to five seconds. */
    e->serving       = (uint8_t)service_probe_local((uint8_t)kind, endpoint_port);

    kernel_serial_printf("[SERVICE] registered '%s' -> partition %u, %s port %u.\n",
                         name, (unsigned)partition_id,
                         kind == SVC_ENDPOINT_IPC ? "IPC" : "TCP",
                         (unsigned)endpoint_port);
    persist_services();
    /* Tell the cluster. Fire-and-forget, and a no-op on a node with no
     * cluster identity (see dspp_service_announce()). */
    dspp_service_announce(name, partition_id, (uint8_t)kind, endpoint_port,
                          caller_uid, e->serving);
    return SVC_REG_OK;
}

SLSServiceStatus service_unregister(uint32_t caller_uid, const char* name) {
    if (!sr_may_mutate(caller_uid)) return SVC_REG_ERR_PERM;
    struct SLSServiceEntry* e = sr_find(name);
    if (!e) return SVC_REG_ERR_NOT_FOUND;
    e->active = 0;
    kernel_serial_printf("[SERVICE] unregistered '%s'.\n", name);
    persist_services();
    dspp_service_withdraw(name);
    return SVC_REG_OK;
}

static struct SLSRemoteService* sr_find_remote(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < SERVICE_REMOTE_MAX; i++)
        if (services_remote[i].active && sr_streq(services_remote[i].name, name))
            return &services_remote[i];
    return 0;
}

SLSServiceStatus service_resolve(const char* name, struct SLSServiceLocation* out) {
    if (!out) return SVC_REG_ERR_NOT_FOUND;
    struct SLSServiceEntry* e = sr_find(name);
    if (!e) {
        /* LOCAL FIRST, always. Only if this node owns nothing by that name
         * do we consult what other nodes have announced. A remote entry
         * must never be able to shadow a service actually running here. */
        struct SLSRemoteService* r = sr_find_remote(name);
        if (!r) return SVC_REG_ERR_NOT_FOUND;

        /* LAZY EXPIRY. Checked here, at the lookup, rather than relying on
         * a sweep having run: an entry past TTL must not resolve even if
         * nothing has reclaimed its slot yet. Correctness therefore does
         * not depend on sweep scheduling, which on a NIC-less boot may be
         * nothing at all (see service_remote_expire()). */
        SLSServiceHealth h = sr_health_of(kernel_tick_counter, r->last_seen_tick);
        if (h == SVC_HEALTH_EXPIRED) return SVC_REG_ERR_NOT_FOUND;

        sr_strcpy(out->name, r->name, SERVICE_NAME_LEN);
        out->partition_id  = r->partition_id;
        out->endpoint_kind = r->endpoint_kind;
        out->endpoint_port = r->endpoint_port;
        /* The node id IS stored for remote entries, unlike local ones.
         * partition_get_owner_node() only knows about partitions this node
         * has a row for, so deriving it here would give 0 -- the
         * "uninitialized" sentinel -- for a partition that lives
         * elsewhere. The announcing node is the authority on where its own
         * services are, so its claim is what gets cached. */
        out->node_id   = r->node_id;
        out->is_local  = 0;
        out->is_remote = 1;
        out->health    = (uint8_t)h;
        out->serving   = r->serving;   /* as the owning node last reported it */
        return SVC_REG_OK;
    }

    sr_strcpy(out->name, e->name, SERVICE_NAME_LEN);
    out->partition_id  = e->partition_id;
    out->endpoint_kind = e->endpoint_kind;
    out->endpoint_port = e->endpoint_port;

    /* THE derived field. Not stored, looked up every time -- so a
     * partition that has migrated since registration resolves to its new
     * node with nothing to invalidate. See service_registry.h. */
    out->node_id   = partition_get_owner_node(e->partition_id);
    out->is_local  = (uint8_t)(out->node_id == cluster_local_node_id());
    out->is_remote = 0;
    /* A local registration has no freshness question: this node IS the
     * authority for it, and it is true until this node changes it. */
    out->health    = (uint8_t)SVC_HEALTH_FRESH;
    /* Cached from the last heartbeat probe, not probed here: resolve runs
     * on the AP core during reconciliation and must not read tcp_conns[]
     * while the BSP is mutating it. */
    out->serving   = e->serving;

    return SVC_REG_OK;
}

/* ─── Replication receive side ────────────────────────────────────────── */
void service_remote_learn(const char* name, uint32_t node_id, uint32_t partition_id,
                          uint8_t endpoint_kind, uint32_t endpoint_port,
                          uint32_t owner_uid, uint8_t serving) {
    if (!name || !name[0] || node_id == 0) return;

    struct SLSRemoteService* r = sr_find_remote(name);
    if (r && r->node_id != node_id) {
        /* Two nodes claiming the same name. Last announcement wins, and it
         * is logged rather than silently resolved: this is an operator
         * error (a duplicate registration across the cluster), and the
         * only thing worse than picking one is picking one quietly. */
        kernel_serial_printf("[SERVICE] '%s' claimed by node %u and node %u -- taking the newer.\n",
                             name, (unsigned)r->node_id, (unsigned)node_id);
    }
    if (!r) {
        for (int i = 0; i < SERVICE_REMOTE_MAX; i++) {
            if (services_remote[i].active) continue;
            r = &services_remote[i];
            r->active = 1;
            sr_strcpy(r->name, name, SERVICE_NAME_LEN);
            break;
        }
        if (!r) { kernel_serial_print("[SERVICE] remote cache full -- announcement dropped.\n"); return; }
    }
    r->node_id       = node_id;
    r->partition_id  = partition_id;
    r->endpoint_kind = endpoint_kind;
    r->endpoint_port = endpoint_port;
    r->owner_uid     = owner_uid;
    r->serving       = serving;
    r->last_seen_tick = kernel_tick_counter;
    /* Deliberately NOT persisted -- see service_registry.h. */
}

void service_remote_forget(const char* name, uint32_t node_id) {
    struct SLSRemoteService* r = sr_find_remote(name);
    if (!r) return;
    /* Only the node that announced it may withdraw it. Otherwise any node
     * could evict another's registration from every cache in the cluster. */
    if (r->node_id != node_id) return;
    r->active = 0;
}

uint32_t service_remote_forget_node(uint32_t node_id) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_REMOTE_MAX; i++) {
        if (!services_remote[i].active) continue;
        if (services_remote[i].node_id != node_id) continue;
        services_remote[i].active = 0; n++;
    }
    return n;
}

SLSServiceHealth service_remote_health(const char* name, uint64_t now) {
    struct SLSRemoteService* r = sr_find_remote(name);
    if (!r) return SVC_HEALTH_EXPIRED;   /* "gone" and "never heard of" answer the same question */
    return sr_health_of(now, r->last_seen_tick);
}

uint32_t service_remote_expire(uint64_t now) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_REMOTE_MAX; i++) {
        if (!services_remote[i].active) continue;
        if (sr_health_of(now, services_remote[i].last_seen_tick) != SVC_HEALTH_EXPIRED) continue;
        kernel_serial_printf("[SERVICE] '%s' from node %u expired (no announcement in %u ticks).\n",
                             services_remote[i].name,
                             (unsigned)services_remote[i].node_id,
                             (unsigned)SERVICE_REMOTE_TTL_TICKS);
        services_remote[i].active = 0;
        n++;
    }
    return n;
}

uint32_t service_heartbeat_tick(uint64_t now) {
    static uint64_t last_beat = 0;
    static int      beat_armed = 0;
    /* Announce once promptly on the first call rather than waiting a full
     * interval: a node that has just booted should be discoverable in
     * milliseconds, not after the first heartbeat period elapses. */
    if (beat_armed && sr_age(now, last_beat) < (uint64_t)SERVICE_HEARTBEAT_TICKS) return 0;
    beat_armed = 1;
    last_beat  = now;
    /* Probe BEFORE announcing, so what goes on the wire is what is
     * currently true rather than what was true a heartbeat ago. */
    service_probe_all_local();
    return service_announce_all();
}

uint32_t service_remote_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_REMOTE_MAX; i++) if (services_remote[i].active) n++;
    return n;
}

uint32_t service_announce_all(void) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        struct SLSServiceEntry* e = &services_registry[i];
        dspp_service_announce(e->name, e->partition_id, e->endpoint_kind,
                              e->endpoint_port, e->owner_uid, e->serving);
        n++;
    }
    return n;
}

uint32_t service_registry_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_MAX; i++) if (services_registry[i].active) n++;
    return n;
}

uint32_t service_count_for_partition(uint32_t partition_id) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_MAX; i++)
        if (services_registry[i].active && services_registry[i].partition_id == partition_id) n++;
    return n;
}

uint32_t service_unregister_partition(uint32_t partition_id) {
    uint32_t n = 0;
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        if (services_registry[i].partition_id != partition_id) continue;
        services_registry[i].active = 0;
        n++;
    }
    if (n) {
        kernel_serial_printf("[SERVICE] dropped %u registration(s) for destroyed partition %u.\n",
                             (unsigned)n, (unsigned)partition_id);
        persist_services();
    }
    return n;
}

/* ─── Syscall surface ─────────────────────────────────────────────────── */
uint64_t sys_sls_service_register(struct SLSServiceRegisterRequest* req) {
    if (!req) return (uint64_t)SVC_REG_ERR_NAME;
    return (uint64_t)service_register(req->caller_uid, req->name, req->partition_id,
                                      (SLSServiceEndpointKind)req->endpoint_kind,
                                      req->endpoint_port);
}

uint64_t sys_sls_service_unregister(struct SLSServiceRegisterRequest* req) {
    if (!req) return (uint64_t)SVC_REG_ERR_NAME;
    return (uint64_t)service_unregister(req->caller_uid, req->name);
}

uint64_t sys_sls_service_resolve(struct SLSServiceResolveRequest* req) {
    if (!req || !req->out) return (uint64_t)SVC_REG_ERR_NOT_FOUND;
    return (uint64_t)service_resolve(req->name, req->out);
}

void sys_sls_service_list(void) {
    kernel_serial_printf("[SERVICE] %u local registration(s), %u cached from other nodes:\n",
                         (unsigned)service_registry_count(),
                         (unsigned)service_remote_count());
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        struct SLSServiceEntry* e = &services_registry[i];
        uint32_t node = partition_get_owner_node(e->partition_id);
        kernel_serial_printf("  %-24s partition=%u node=%u %s:%u [%s]%s\n",
                             e->name, (unsigned)e->partition_id, (unsigned)node,
                             e->endpoint_kind == SVC_ENDPOINT_IPC ? "ipc" : "tcp",
                             (unsigned)e->endpoint_port,
                             service_serving_name((SLSServiceServing)e->serving),
                             node == cluster_local_node_id() ? " (local)" : "");
        (void)0;
    }
    for (int i = 0; i < SERVICE_REMOTE_MAX; i++) {
        if (!services_remote[i].active) continue;
        struct SLSRemoteService* r = &services_remote[i];
        kernel_serial_printf("  %-24s node=%u %s:%u [remote, %s, %s]\n",
                             r->name, (unsigned)r->node_id,
                             r->endpoint_kind == SVC_ENDPOINT_IPC ? "ipc" : "tcp",
                             (unsigned)r->endpoint_port,
                             service_health_name(sr_health_of(kernel_tick_counter,
                                                              r->last_seen_tick)),
                             service_serving_name((SLSServiceServing)r->serving));
    }
}
