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
    uint8_t  serving;           /* SLSServiceServing, refreshed by the heartbeat probe */
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
    uint8_t  is_remote;         /* answered from the replicated cache, not this node's own table */
    uint8_t  health;            /* SLSServiceHealth -- is this INFORMATION current? */
    uint8_t  serving;           /* SLSServiceServing -- is the ENDPOINT accepting? */
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

/* ─── Cross-node replication ──────────────────────────────────────────
 * Phase 4 shipped a per-node registry: a name registered on node 1 did
 * not resolve on node 2. It does now. Each node ANNOUNCES its own
 * registrations over DSPP and caches what it hears from others.
 *
 * REMOTE ENTRIES ARE A SEPARATE TABLE, and deliberately so:
 *   - They are another node's truth, not this node's, so they must never
 *     be persisted. Restoring a stale cache would resurrect services that
 *     moved or vanished while this node was down.
 *   - LOCAL ALWAYS WINS on a name collision. A node is authoritative
 *     about what it owns, and a remote announcement must never be able to
 *     redirect traffic away from a service actually running here.
 * Keeping them apart makes both of those properties structural rather
 * than rules to remember. */
#define SERVICE_REMOTE_MAX 64

/* ─── Freshness ───────────────────────────────────────────────────────
 * A replicated entry is a CLAIM another node made at a point in time, not
 * a fact. Without an expiry, a node that dies silently -- power loss,
 * cable pulled, kernel panic -- leaves its services resolving forever on
 * every other node in the cluster, sending traffic into a hole. Nothing
 * would ever withdraw them, because withdrawal requires the dead node to
 * send something.
 *
 * So each node re-announces what it owns on a heartbeat, and an entry
 * that stops being refreshed ages out. Ticks are kernel_tick_counter at
 * ~100 Hz (kernel/timer.c), the same unit HTTP_IDLE_TIMEOUT_TICKS uses.
 *
 * The three-way split is deliberate. A binary alive/dead would flip a
 * service out of existence the instant one heartbeat was late, and give
 * an operator no warning; STALE means "this should have been refreshed by
 * now, and has not been" while the entry still resolves. That is
 * information, not a failure.
 *
 *   FRESH   ( < HEARTBEAT*2 )  refreshed recently; resolves normally
 *   STALE   ( < TTL )          overdue but still resolving -- a warning
 *   EXPIRED ( >= TTL )         does not resolve at all
 *
 * TTL is deliberately several heartbeats, not one: a single dropped
 * announcement is normal on a broadcast protocol with no retransmission
 * (see dspp.h), and must not evict a healthy service. */
#define SERVICE_HEARTBEAT_TICKS  500u    /* ~5 s: re-announce interval */
#define SERVICE_REMOTE_TTL_TICKS 2000u   /* ~20 s == 4 missed heartbeats */

typedef enum {
    SVC_HEALTH_FRESH = 0,
    SVC_HEALTH_STALE,
    SVC_HEALTH_EXPIRED,
} SLSServiceHealth;

const char* service_health_name(SLSServiceHealth h);

/* ─── Endpoint liveness ───────────────────────────────────────────────
 * `health` above answers "is this information current?". This answers a
 * different question -- "is the endpoint actually accepting?" -- and the
 * two are deliberately NOT merged. Collapsing them would lose the
 * distinction between "I have not heard lately" and "I have heard, and it
 * is down", which are opposite situations for anyone deciding whether to
 * route or to page someone.
 *
 * ─── How it is observed, and by whom ──────────────────────────────────
 * THE OWNING NODE PROBES ITS OWN ENDPOINTS and reports the result in its
 * heartbeat. No node ever probes another's, which matters: a cross-node
 * probe would need a request/response with a timeout, and every blocking
 * wait here routes through net_event.h's privileged `sti; hlt`. Riding
 * the existing announcement costs one byte and no new round trip.
 *
 *   TCP endpoint -- is a socket LISTENing on that port (net/tcp.c)?
 *   IPC endpoint -- what does the microkernel watchdog say about the
 *                   service owning that port (kernel/microkernel.c)?
 *
 * The IPC case is the strongest signal in the system: the watchdog
 * maintains ONLINE/CRASHED from real crash and restart events, so the
 * answer is observed rather than inferred.
 *
 * ─── What this still does not catch, stated plainly ───────────────────
 * A process that is alive and holding its port but whose handler has
 * wedged reports UP. Detecting that needs an application-level probe --
 * send something, require an answer -- which is a different mechanism and
 * is not built. UNKNOWN is returned honestly rather than guessed: an IPC
 * port with no supervised owner is not something this kernel can judge. */
typedef enum {
    SVC_SERVING_UP = 0,
    SVC_SERVING_DOWN,
    SVC_SERVING_UNKNOWN,
} SLSServiceServing;

const char* service_serving_name(SLSServiceServing v);

/* Probes one endpoint on THIS node. BSP only -- it reads tcp_conns[] and
 * services[], both of which the BSP mutates. */
SLSServiceServing service_probe_local(uint8_t endpoint_kind, uint32_t endpoint_port);

/* Re-probes every local registration and caches the result on each.
 * Called from the heartbeat, so a local entry's serving state is as fresh
 * as the last heartbeat (~5 s), not as fresh as the last lookup. That is
 * deliberate: service_resolve() runs on the AP core during reconciliation
 * and must not read tcp_conns[] concurrently with the BSP. Returns the
 * number of registrations whose state CHANGED. */
uint32_t service_probe_all_local(void);

struct SLSRemoteService {
    char     name[SERVICE_NAME_LEN];
    uint32_t node_id;           /* who announced it -- stored, unlike the local case */
    uint32_t partition_id;
    uint32_t endpoint_port;
    uint32_t owner_uid;
    uint8_t  endpoint_kind;
    uint8_t  active;
    uint8_t  serving;           /* as the OWNING node last reported it */
    uint64_t last_seen_tick;    /* kernel_tick_counter at the last announcement */
};
extern struct SLSRemoteService services_remote[SERVICE_REMOTE_MAX];

/* Called from net/dspp.c on receipt. Learning is idempotent: a repeated
 * announcement updates in place. */
void service_remote_learn(const char* name, uint32_t node_id, uint32_t partition_id,
                          uint8_t endpoint_kind, uint32_t endpoint_port,
                          uint32_t owner_uid, uint8_t serving);
void service_remote_forget(const char* name, uint32_t node_id);

/* Freshness of one cached entry, as of `now`. Returns SVC_HEALTH_EXPIRED
 * for a name that is not cached at all -- "gone" and "never heard of" are
 * the same answer to a caller deciding whether to route there. */
SLSServiceHealth service_remote_health(const char* name, uint64_t now);

/* Frees the slots of entries past TTL. Returns how many were reclaimed.
 *
 * This is a slot-RECLAIM pass, not the thing that makes expiry correct:
 * service_resolve() checks freshness itself, so an expired entry stops
 * resolving whether or not this has run. That split is deliberate -- it
 * means correctness does not depend on how often, or whether, a sweep
 * gets scheduled. Pure memory, no I/O, no persist. */
uint32_t service_remote_expire(uint64_t now);

/* Re-announces every local registration if HEARTBEAT_TICKS have passed
 * since the last one. BSP ONLY -- it transmits, and the NIC TX path is
 * not safe to drive from the AP core concurrently with the HTTP loop.
 * Returns the number announced (0 if it was not yet time). */
uint32_t service_heartbeat_tick(uint64_t now);
/* Drops everything learned from one node -- for when a node is known gone. */
uint32_t service_remote_forget_node(uint32_t node_id);
uint32_t service_remote_count(void);

/* Re-announces every local registration. A node that has just joined
 * learns nothing until the others speak, so this is how an operator (or a
 * later periodic tick) makes the cluster re-sync. */
uint32_t service_announce_all(void);

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
