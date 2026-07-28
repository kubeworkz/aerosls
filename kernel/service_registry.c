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

struct SLSServiceEntry services_registry[SERVICE_MAX];

/* ─── Local helpers ─────────────────────────────────────────────────── */
static int sr_streq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int sr_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void sr_strcpy(char* d, const char* s, int n) {
    int i; for (i = 0; i < n - 1 && s && s[i]; i++) d[i] = s[i]; d[i] = '\0';
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

    kernel_serial_printf("[SERVICE] registered '%s' -> partition %u, %s port %u.\n",
                         name, (unsigned)partition_id,
                         kind == SVC_ENDPOINT_IPC ? "IPC" : "TCP",
                         (unsigned)endpoint_port);
    persist_services();
    return SVC_REG_OK;
}

SLSServiceStatus service_unregister(uint32_t caller_uid, const char* name) {
    if (!sr_may_mutate(caller_uid)) return SVC_REG_ERR_PERM;
    struct SLSServiceEntry* e = sr_find(name);
    if (!e) return SVC_REG_ERR_NOT_FOUND;
    e->active = 0;
    kernel_serial_printf("[SERVICE] unregistered '%s'.\n", name);
    persist_services();
    return SVC_REG_OK;
}

SLSServiceStatus service_resolve(const char* name, struct SLSServiceLocation* out) {
    if (!out) return SVC_REG_ERR_NOT_FOUND;
    struct SLSServiceEntry* e = sr_find(name);
    if (!e) return SVC_REG_ERR_NOT_FOUND;

    sr_strcpy(out->name, e->name, SERVICE_NAME_LEN);
    out->partition_id  = e->partition_id;
    out->endpoint_kind = e->endpoint_kind;
    out->endpoint_port = e->endpoint_port;

    /* THE derived field. Not stored, looked up every time -- so a
     * partition that has migrated since registration resolves to its new
     * node with nothing to invalidate. See service_registry.h. */
    out->node_id  = partition_get_owner_node(e->partition_id);
    out->is_local = (uint8_t)(out->node_id == cluster_local_node_id());

    return SVC_REG_OK;
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
    kernel_serial_printf("[SERVICE] %u registration(s):\n", (unsigned)service_registry_count());
    for (int i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        struct SLSServiceEntry* e = &services_registry[i];
        uint32_t node = partition_get_owner_node(e->partition_id);
        kernel_serial_printf("  %-24s partition=%u node=%u %s:%u%s\n",
                             e->name, (unsigned)e->partition_id, (unsigned)node,
                             e->endpoint_kind == SVC_ENDPOINT_IPC ? "ipc" : "tcp",
                             (unsigned)e->endpoint_port,
                             node == cluster_local_node_id() ? " (local)" : "");
    }
}
