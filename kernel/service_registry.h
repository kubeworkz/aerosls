#ifndef SERVICE_REGISTRY_H
#define SERVICE_REGISTRY_H

#include <stdint.h>
#include "object_catalog.h"   /* SLSRole, for the RBAC gate */

/*
 * service_registry.h — name → partition/node/endpoint resolution
 * (Orchestration Plan Phase 4). The one capability the K8s convergence
 * review identified as genuinely ABSENT rather than merely different.
 *
 * ─── A correction to the plan this implements ─────────────────────────
 * The plan said: "a registration is a catalog object; persistence,
 * ownership and RBAC come free." Checked against the actual struct, that
 * does not hold. `struct SLSObjectEntry` has name, partition_id,
 * owner_uid, owner_role and perm_mask -- but nowhere to put an ENDPOINT,
 * which is the entire point of a service registration. Making a
 * registration a catalog object would mean adding service-specific fields
 * to a 128-entry struct that every object type shares, that is persisted
 * and restored on every boot, to serve one caller. That is the wrong
 * trade, so this is a dedicated table instead -- the same idiom
 * partition_owner_table[] and tenants[] already use.
 *
 * The salvageable half of the plan's claim is kept: registrations carry
 * owner_uid and partition_id and are gated through catalog_get_role(),
 * so ownership and RBAC really are the existing mechanisms rather than a
 * second permission system.
 *
 * ─── The design decision that matters: the node is NOT stored ─────────
 * A registration records name → PARTITION, never name → node. The node is
 * derived at resolve time via partition_get_owner_node().
 *
 * This is not a shortcut, it is the point. partition_migrate() already
 * updates partition_owner_table[] when a partition moves (its Step 4).
 * Storing a node id here would make a second copy of a fact that already
 * moves, and that copy would be stale the instant a partition migrated --
 * requiring a reconciliation loop to chase it, which is exactly the
 * machinery Kubernetes needs and this design gets to not build.
 *
 * Because the node is derived, **service resolution follows partition
 * migration automatically**: migrate a partition and every service in it
 * resolves to the new node on the next lookup, with nothing to notify,
 * nothing to invalidate and no window during which the registry is
 * wrong. tests/service_registry_host_test.c asserts exactly this.
 *
 * ─── Relationship to microkernel.c's services[] ───────────────────────
 * Unrelated and deliberately not merged. `services[MAX_SERVICES]` (8,
 * boot-populated) is internal-service SUPERVISION: pid, IPC port, crash
 * state, restart counts, for the five kernel services the microkernel
 * watchdog owns. This is a DISCOVERY registry for workloads, sized and
 * scoped differently, and neither wants the other's fields.
 */

#define SERVICE_MAX        64
#define SERVICE_NAME_LEN   64

/* What kind of address `endpoint_port` is. Kept explicit rather than
 * inferred from the number, because IPC port 0x1003 and TCP port 4099 are
 * the same integer and mean entirely different things. */
typedef enum {
    SVC_ENDPOINT_IPC = 0,   /* kernel/ipc.c port -- only meaningful on the owning node */
    SVC_ENDPOINT_TCP = 1,   /* TCP port -- reachable across the cluster */
} SLSServiceEndpointKind;

struct SLSServiceEntry {
    char     name[SERVICE_NAME_LEN];
    uint32_t partition_id;      /* the node is DERIVED from this -- see above */
    uint32_t endpoint_port;
    uint32_t owner_uid;
    uint8_t  endpoint_kind;     /* SLSServiceEndpointKind */
    uint8_t  active;
};

/* What a lookup returns. `node_id` and `is_local` are computed at resolve
 * time and are not stored anywhere -- that is the whole design. */
struct SLSServiceLocation {
    char     name[SERVICE_NAME_LEN];
    uint32_t partition_id;
    uint32_t node_id;
    uint32_t endpoint_port;
    uint8_t  endpoint_kind;
    uint8_t  is_local;          /* node_id == cluster_local_node_id() */
};

typedef enum {
    SVC_REG_OK = 0,
    SVC_REG_ERR_NAME,           /* empty, or too long */
    SVC_REG_ERR_PARTITION,      /* not an active, defined partition */
    SVC_REG_ERR_FULL,           /* SERVICE_MAX reached */
    SVC_REG_ERR_EXISTS,         /* name already registered (re-register updates instead) */
    SVC_REG_ERR_NOT_FOUND,
    SVC_REG_ERR_PERM,           /* caller's role is below the required level */
    SVC_REG_ERR_ENDPOINT,       /* unknown endpoint kind, or port 0 */
} SLSServiceStatus;

const char* service_status_name(SLSServiceStatus s);

extern struct SLSServiceEntry services_registry[SERVICE_MAX];

void service_registry_init(void);

/* Registers, or updates in place if `name` is already registered by the
 * same owner. Requires ROLE_DB_ADMIN or higher, matching the gate on
 * partition and tenant creation. */
SLSServiceStatus service_register(uint32_t caller_uid, const char* name,
                                  uint32_t partition_id,
                                  SLSServiceEndpointKind kind,
                                  uint32_t endpoint_port);

SLSServiceStatus service_unregister(uint32_t caller_uid, const char* name);

/* The lookup. Fills *out and returns SVC_REG_OK, or SVC_REG_ERR_NOT_FOUND.
 * Deliberately NOT role-gated: resolution is a read of where something
 * lives, the same posture partition_get_owner_node() takes. */
SLSServiceStatus service_resolve(const char* name, struct SLSServiceLocation* out);

uint32_t service_registry_count(void);
uint32_t service_count_for_partition(uint32_t partition_id);

/* Drops every registration belonging to `partition_id`. Called from
 * partition_destroy() -- a service in a destroyed partition resolves to a
 * partition that no longer exists, which is worse than not resolving. */
uint32_t service_unregister_partition(uint32_t partition_id);

/* ─── Syscall surface ─────────────────────────────────────────────────── */
struct SLSServiceRegisterRequest {
    uint32_t caller_uid;
    char     name[SERVICE_NAME_LEN];
    uint32_t partition_id;
    uint32_t endpoint_kind;
    uint32_t endpoint_port;
};

struct SLSServiceResolveRequest {
    char                       name[SERVICE_NAME_LEN];
    struct SLSServiceLocation* out;
};

#define SYS_SLS_SERVICE_REGISTER   281
#define SYS_SLS_SERVICE_UNREGISTER 282
#define SYS_SLS_SERVICE_RESOLVE    283
#define SYS_SLS_SERVICE_LIST       284

uint64_t sys_sls_service_register(struct SLSServiceRegisterRequest* req);
uint64_t sys_sls_service_unregister(struct SLSServiceRegisterRequest* req);
uint64_t sys_sls_service_resolve(struct SLSServiceResolveRequest* req);
void     sys_sls_service_list(void);

#endif /* SERVICE_REGISTRY_H */
