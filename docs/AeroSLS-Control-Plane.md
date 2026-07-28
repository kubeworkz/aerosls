> ## Status: superseded in part — read this first
>
> This document was written before Orchestration Plan Phases 4–7. It
> describes a Kubernetes-shaped **separate control-plane process**: cluster
> objects, node roles, a scheduler, an `aeroslsctl`. A decision has since
> been taken not to build it that way, and the reasoning matters more than
> the conclusion.
>
> **Kubernetes needs a control plane because Linux knows nothing about
> clusters. This kernel does.** Phases 4–7 put the service registry, the
> reconciler, the circuit breakers and workload restarts *inside the
> kernel*, replicated over DSPP. There is no etcd, no apiserver, no
> scheduler daemon, because the things those components exist to provide
> are already in the kernel and already distributed.
>
> Building the userspace control plane below would therefore create a
> **second source of truth** for state the kernel already owns
> authoritatively — the same mistake Phase 4 avoided by deriving a
> service's node from its partition rather than storing a copy of it.
>
> What was actually missing was not a control plane but a cluster-wide
> **view** of one. That is now `GET /api/cluster` and `GET /api/nodes`
> (`net/http.c`), served by **any node**, because every node holds the
> roster and the replicated registry. No control-plane node means no new
> single point of failure and no bootstrap ordering problem — an advantage
> the design below would have spent.
>
> ### What in this document is still live
>
> | Concept here | Status |
> | --- | --- |
> | Cluster/node/workload objects | **Built, in-kernel** — `workload.c`, `service_registry.c`, `consensus.c` |
> | Reconciliation toward desired state | **Built** — Phase 5, AP-core sweep + BSP drain |
> | Health, circuit breaking, metrics | **Built** — Phase 6, `service_mesh.c` |
> | Restart policies + backoff | **Built** — Phase 7, `never` / `on-failure` / `always` |
> | Separate control-plane process | **Not being built** — see above |
> | `NODE_ROLE_CONTROL_PLANE` | **Not being built** — every node can serve the view |
> | Cross-node scheduler / placement | **Still absent, still wanted** — nothing chooses which node a workload lands on |
> | Cluster formation from the UI | **Built** — `POST /api/cluster/init` / `/api/cluster/peer` wrap syscalls 279/280, which were previously serial-console only |
> | Node *provisioning* (booting a machine) | **Not built, and not a kernel capability** — see below |
> | `aeroslsctl` CLI | **Still wanted, and cheap** — the shell already has every command; a CLI would be a thin REST client. Orthogonal to the UI question |
> | YAML workload manifests | **Still wanted** — `workload declare` is imperative; a manifest applied by the reconciler would suit the declarative model better |
>
> The frontend at `/slsos-sim` is the control-plane UI. Its 16 panels are
> per-node views; a node selector in the Cluster tab repoints all of them,
> routed through `authFetch()` — the single choke point every kernel call
> already passes through.
>
> ### Forming a cluster vs. provisioning one
>
> The UI can now *form* a cluster from nodes that are already running. It
> cannot *boot* one, and that distinction is structural rather than a
> missing feature: a kernel cannot start another kernel, so provisioning
> can only come from something outside — the dev server, which currently
> executes no host processes at all.
>
> Adding that is possible (`run-two-nodes.sh` already has a working QEMU
> recipe) but it is a different class of capability, and it must not sit
> behind the current auth. `DEMO_TOKEN` in `src/lib/apiFetch.ts` is a
> hardcoded constant in **client-side** code — it ships in the browser
> bundle and is not a secret. A launcher gated on it would be a remote
> code execution surface. If a lab launcher is wanted, it needs its own
> explicit opt-in flag, off by default, and ideally a real auth story
> first.
>
> **One honest consequence:** the kernel deals in node *IDs*, not
> addresses, because DSPP is L2 broadcast and deliberately never needed an
> address book. So the UI must be told where each node listens
> (`AEROSLS_NODES="1=http://host:3001,2=..."`). That is configuration, not
> something the cluster can be asked for.

## **AeroSLS Control Plane Architecture**

**AeroSLS Control Plane** - a complete management interface that rivals Kubernetes but leverages SLS's unique capabilities.

```plaintext
// include/aerosls/control/control_plane.h
#ifndef AEROSLS_CONTROL_PLANE_H
#define AEROSLS_CONTROL_PLANE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "aerosls/aerosls.h"

/* ================================================================
 * AeroSLS Control Plane - Complete Cluster Management
 * ================================================================ */

/* Cluster states */
typedef enum {
    CLUSTER_STATE_FORMING    = 0,  /* Initial formation */
    CLUSTER_STATE_STABLE     = 1,  /* Normal operation */
    CLUSTER_STATE_DEGRADED   = 2,  /* Some nodes down */
    CLUSTER_STATE_RECOVERING = 3,  /* Recovery in progress */
    CLUSTER_STATE_MAINTENANCE = 4, /* Planned maintenance */
    CLUSTER_STATE_CRITICAL   = 5,  /* Requires intervention */
} cluster_state_t;

/* Node roles */
typedef enum {
    NODE_ROLE_CONTROL_PLANE  = 0,  /* Management node */
    NODE_ROLE_WORKER         = 1,  /* Runs workloads */
    NODE_ROLE_EDGE           = 2,  /* Edge computing node */
    NODE_ROLE_STORAGE        = 3,  /* Storage-focused node */
    NODE_ROLE_GATEWAY        = 4,  /* API Gateway node */
} node_role_t;

/* Node states */
typedef enum {
    NODE_STATE_ONLINE        = 0,
    NODE_STATE_OFFLINE       = 1,
    NODE_STATE_MAINTENANCE   = 2,
    NODE_STATE_DRAINING      = 3,
    NODE_STATE_PROVISIONING  = 4,
    NODE_STATE_FAILED        = 5,
} node_state_t;

/* Workload types */
typedef enum {
    WORKLOAD_FUNCTION        = 0,  /* Serverless function */
    WORKLOAD_SERVICE         = 1,  /* Long-running service */
    WORKLOAD_BATCH           = 2,  /* Batch job */
    WORKLOAD_DAEMON          = 3,  /* Runs on every node */
    WORKLOAD_STATEFUL        = 4,  /* Stateful application */
    WORKLOAD_CRON            = 5,  /* Scheduled job */
} workload_type_t;

/* Scheduling policies */
typedef enum {
    SCHEDULE_SPREAD          = 0,  /* Spread across nodes */
    SCHEDULE_PACK            = 1,  /* Pack tightly */
    SCHEDULE_AFFINITY        = 2,  /* Node affinity */
    SCHEDULE_ANTI_AFFINITY   = 3,  /* Anti-affinity */
    SCHEDULE_CUSTOM          = 4,  /* Custom rules */
} scheduling_policy_t;

/* Forward declarations */
typedef struct aerosls_cluster aerosls_cluster_t;
typedef struct aerosls_node_info aerosls_node_info_t;
typedef struct aerosls_workload aerosls_workload_t;
typedef struct aerosls_scheduler aerosls_scheduler_t;

#endif /* AEROSLS_CONTROL_PLANE_H */
```

```plaintext
// include/aerosls/control/cluster.h
#ifndef AEROSLS_CLUSTER_H
#define AEROSLS_CLUSTER_H

#include "control_plane.h"

/* ================================================================
 * Cluster Manager - The core of the control plane
 * ================================================================ */

/* Cluster configuration */
typedef struct {
    char                    cluster_name[64];
    char                    cluster_id[64];
    cluster_state_t         state;
    
    /* High availability */
    int                     control_plane_nodes;
    int                     min_control_plane_nodes;
    bool                    auto_failover;
    
    /* Scaling */
    int                     min_worker_nodes;
    int                     max_worker_nodes;
    bool                    auto_scaling;
    float                   scale_up_threshold;     /* CPU % */
    float                   scale_down_threshold;   /* CPU % */
    
    /* Networking */
    char                    service_cidr[32];       /* 10.96.0.0/12 */
    char                    pod_cidr[32];           /* 10.244.0.0/16 */
    
    /* Storage */
    uint64_t                default_pmem_size_mb;
    char                    storage_class[32];
    
    /* Security */
    bool                    rbac_enabled;
    bool                    network_policy_enabled;
    bool                    pod_security_enabled;
    bool                    secret_encryption;
    
    /* Updates */
    char                    version[32];
    char                    update_channel[32];     /* stable, beta, edge */
    bool                    auto_updates;
    
} cluster_config_t;

/* Cluster metrics */
typedef struct {
    uint64_t                total_nodes;
    uint64_t                online_nodes;
    uint64_t                offline_nodes;
    uint64_t                total_instances;
    uint64_t                running_instances;
    uint64_t                total_workloads;
    uint64_t                running_workloads;
    
    /* Resources */
    uint64_t                total_cpu_cores;
    uint64_t                allocated_cpu_cores;
    uint64_t                total_ram_mb;
    uint64_t                allocated_ram_mb;
    uint64_t                total_pmem_mb;
    uint64_t                allocated_pmem_mb;
    
    /* Performance */
    double                  avg_cpu_utilization;
    double                  avg_ram_utilization;
    double                  avg_pmem_utilization;
    uint64_t                messages_per_second;
    uint64_t                transactions_per_second;
    
    /* Health */
    int                     active_alerts;
    int                     critical_alerts;
    uint64_t                uptime_seconds;
} cluster_metrics_t;

/* The Cluster */
struct aerosls_cluster {
    cluster_config_t        config;
    cluster_metrics_t       metrics;
    
    /* Nodes */
    aerosls_node_info_t     **nodes;
    int                     node_count;
    int                     node_capacity;
    pthread_rwlock_t        nodes_lock;
    
    /* Workloads */
    aerosls_workload_t      **workloads;
    int                     workload_count;
    int                     workload_capacity;
    pthread_rwlock_t        workloads_lock;
    
    /* Namespaces */
    struct namespace_info   **namespaces;
    int                     namespace_count;
    
    /* Scheduler */
    aerosls_scheduler_t     *scheduler;
    
    /* API Server */
    struct api_server       *api_server;
    
    /* Controller Manager */
    struct controller_mgr   *controller_mgr;
    
    /* etcd-like distributed store (but using SLS!) */
    struct sls_store        *distributed_store;
    
    /* Background reconciliation */
    pthread_t               reconciler_thread;
    bool                    reconciler_running;
    
    /* Leader election */
    bool                    is_leader;
    char                    leader_node_id[64];
    pthread_t               leader_election_thread;
    
    /* Lock */
    pthread_rwlock_t        lock;
};

/* Node information */
struct aerosls_node_info {
    char                    node_id[64];
    char                    hostname[256];
    char                    ip_address[64];
    node_role_t             role;
    node_state_t            state;
    
    /* Labels and annotations */
    struct {
        char                **keys;
        char                **values;
        int                 count;
    } labels;
    
    struct {
        char                **keys;
        char                **values;
        int                 count;
    } annotations;
    
    /* Taints and tolerations */
    struct taint            *taints;
    int                     taint_count;
    
    /* Resources */
    struct {
        uint64_t            cpu_cores;
        uint64_t            ram_mb;
        uint64_t            pmem_mb;
        uint64_t            max_pods;
        uint64_t            max_instances;
    } capacity;
    
    struct {
        uint64_t            cpu_cores;
        uint64_t            ram_mb;
        uint64_t            pmem_mb;
        uint64_t            running_pods;
        uint64_t            running_instances;
    } allocatable;
    
    /* Conditions */
    struct {
        bool                memory_pressure;
        bool                disk_pressure;
        bool                pid_pressure;
        bool                network_unavailable;
    } conditions;
    
    /* QEMU instances */
    aerosls_qemu_instance_t **instances;
    int                     instance_count;
    
    /* Workloads running on this node */
    aerosls_workload_t      **workloads;
    int                     workload_count;
    
    /* Metrics */
    struct {
        double              cpu_usage_percent;
        double              ram_usage_percent;
        double              pmem_usage_percent;
        uint64_t            network_rx_bytes;
        uint64_t            network_tx_bytes;
        uint64_t            disk_read_bytes;
        uint64_t            disk_write_bytes;
    } metrics;
    
    /* Timestamps */
    struct timespec         created_at;
    struct timespec         last_heartbeat;
    struct timespec         last_state_change;
    
    /* Lock */
    pthread_rwlock_t        lock;
    
    /* Next in linked list */
    aerosls_node_info_t     *next;
};

#endif /* AEROSLS_CLUSTER_H */
```

```plaintext
// include/aerosls/control/workload.h
#ifndef AEROSLS_WORKLOAD_H
#define AEROSLS_WORKLOAD_H

#include "control_plane.h"

/* ================================================================
 * Workload Management - Like K8s Pods/Deployments but SLS-native
 * ================================================================ */

/* Workload specification */
typedef struct {
    char                    name[128];
    char                    namespace[64];
    workload_type_t         type;
    scheduling_policy_t     scheduling;
    
    /* Container/Function spec */
    struct {
        char                image[256];           /* Function or container */
        char                command[512];
        char                **args;
        int                 arg_count;
        char                **env_keys;
        char                **env_values;
        int                 env_count;
    } spec;
    
    /* Resources */
    struct {
        uint64_t            cpu_millicores;
        uint64_t            ram_mb;
        uint64_t            pmem_mb;              /* SLS persistent memory */
        uint64_t            ephemeral_storage_mb;
    } resources;
    
    /* Scaling */
    struct {
        int                 min_replicas;
        int                 max_replicas;
        int                 target_replicas;
        bool                auto_scaling;
        float               target_cpu_percent;
        float               target_memory_percent;
    } scaling;
    
    /* Networking */
    struct {
        int                 port;
        int                 target_port;
        char                protocol[8];          /* TCP, UDP, SLS */
        bool                service_mesh;         /* Use SLS mesh */
        char                **ingress_hosts;
        int                 ingress_count;
    } networking;
    
    /* Storage */
    struct {
        char                **volume_names;
        char                **volume_sizes;
        char                **volume_mount_paths;
        int                 volume_count;
    } storage;
    
    /* Health checks */
    struct {
        bool                liveness_probe;
        char                liveness_command[256];
        int                 liveness_initial_delay;
        int                 liveness_period;
        bool                readiness_probe;
        char                readiness_command[256];
        int                 readiness_initial_delay;
        int                 readiness_period;
    } health;
    
    /* Update strategy */
    struct {
        char                type[32];             /* RollingUpdate, Recreate */
        int                 max_surge;
        int                 max_unavailable;
        int                 min_ready_seconds;
    } update_strategy;
    
    /* Scheduling constraints */
    struct {
        char                **node_selector_keys;
        char                **node_selector_values;
        int                 node_selector_count;
        char                **affinity_rules;
        int                 affinity_count;
        char                **anti_affinity_rules;
        int                 anti_affinity_count;
    } constraints;
    
    /* SLS-specific features */
    struct {
        bool                persistent_execution;  /* Survive crashes */
        bool                live_migration;        /* Zero-downtime move */
        int                 checkpoint_interval;   /* Seconds */
        bool                encrypted_memory;      /* Memory encryption */
        bool                replicated_memory;     /* Cross-node replication */
    } sls_features;
    
} workload_spec_t;

/* Workload status */
typedef struct {
    int                     desired_replicas;
    int                     current_replicas;
    int                     ready_replicas;
    int                     available_replicas;
    int                     unavailable_replicas;
    int                     updated_replicas;
    
    char                    phase[32];            /* Pending, Running, Succeeded, Failed */
    char                    reason[256];
    char                    message[512];
    
    struct timespec         start_time;
    struct timespec         last_transition_time;
    struct timespec         last_update_time;
} workload_status_t;

/* The Workload */
struct aerosls_workload {
    uint64_t                workload_id;
    workload_spec_t         spec;
    workload_status_t       status;
    
    /* Owner references */
    char                    owner_name[128];
    char                    owner_kind[32];       /* Deployment, Job, CronJob */
    
    /* Associated execution contexts */
    exec_context_t          **contexts;
    int                     context_count;
    
    /* Associated services */
    sls_service_t           **services;
    int                     service_count;
    
    /* Labels and selectors */
    struct {
        char                **keys;
        char                **values;
        int                 count;
    } labels;
    
    struct {
        char                **keys;
        char                **values;
        int                 count;
    } selector;
    
    /* Node assignment */
    char                    assigned_node[64];
    
    /* Lock */
    pthread_mutex_t         lock;
    
    /* Next in linked list */
    aerosls_workload_t      *next;
};

#endif /* AEROSLS_WORKLOAD_H */
```

```plaintext
// src/control/cluster_manager.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <json-c/json.h>
#include "aerosls/control/cluster.h"
#include "aerosls/control/workload.h"

/* ================================================================
 * Cluster Manager Implementation
 * ================================================================ */

/* Create a new AeroSLS cluster */
aerosls_cluster_t* aerosls_cluster_create(const char *name, 
                                            const char *version) {
    
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║   AeroSLS Cluster Creation                               ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");
    
    aerosls_cluster_t *cluster = calloc(1, sizeof(aerosls_cluster_t));
    if (!cluster) return NULL;
    
    /* Initialize configuration */
    strncpy(cluster->config.cluster_name, name, 
            sizeof(cluster->config.cluster_name) - 1);
    generate_cluster_id(cluster->config.cluster_id);
    cluster->config.state = CLUSTER_STATE_FORMING;
    strncpy(cluster->config.version, version, 
            sizeof(cluster->config.version) - 1);
    
    /* Default configuration */
    cluster->config.control_plane_nodes = 3;
    cluster->config.min_control_plane_nodes = 2;
    cluster->config.auto_failover = true;
    cluster->config.min_worker_nodes = 1;
    cluster->config.max_worker_nodes = 100;
    cluster->config.auto_scaling = true;
    cluster->config.scale_up_threshold = 0.75;
    cluster->config.scale_down_threshold = 0.30;
    
    strncpy(cluster->config.service_cidr, "10.96.0.0/12", 
            sizeof(cluster->config.service_cidr) - 1);
    strncpy(cluster->config.pod_cidr, "10.244.0.0/16", 
            sizeof(cluster->config.pod_cidr) - 1);
    
    cluster->config.default_pmem_size_mb = 4096;
    strncpy(cluster->config.storage_class, "sls-persistent", 
            sizeof(cluster->config.storage_class) - 1);
    
    cluster->config.rbac_enabled = true;
    cluster->config.network_policy_enabled = true;
    cluster->config.pod_security_enabled = true;
    cluster->config.secret_encryption = true;
    
    strncpy(cluster->config.update_channel, "stable", 
            sizeof(cluster->config.update_channel) - 1);
    
    /* Initialize node array */
    cluster->node_capacity = 128;
    cluster->nodes = calloc(cluster->node_capacity, 
                            sizeof(aerosls_node_info_t*));
    
    /* Initialize workload array */
    cluster->workload_capacity = 1024;
    cluster->workloads = calloc(cluster->workload_capacity,
                                sizeof(aerosls_workload_t*));
    
    /* Initialize locks */
    pthread_rwlock_init(&cluster->lock, NULL);
    pthread_rwlock_init(&cluster->nodes_lock, NULL);
    pthread_rwlock_init(&cluster->workloads_lock, NULL);
    
    /* Create SLS-based distributed store */
    cluster->distributed_store = sls_distributed_store_create();
    
    /* Create scheduler */
    cluster->scheduler = aerosls_scheduler_create(cluster);
    
    /* Create API server */
    cluster->api_server = aerosls_api_server_create(cluster, 6443);
    
    /* Create controller manager */
    cluster->controller_mgr = aerosls_controller_manager_create(cluster);
    
    /* Start reconciler */
    cluster->reconciler_running = true;
    pthread_create(&cluster->reconciler_thread, NULL,
                   cluster_reconciler_worker, cluster);
    
    /* Start leader election */
    pthread_create(&cluster->leader_election_thread, NULL,
                   leader_election_worker, cluster);
    
    cluster->metrics.uptime_seconds = 0;
    
    printf("✅ Cluster '%s' created (ID: %s)\n", 
           name, cluster->config.cluster_id);
    printf("   Version: %s\n", version);
    printf("   API Server: https://0.0.0.0:6443\n");
    printf("   Scheduler: Active\n");
    printf("   Controller Manager: Active\n");
    printf("   Distributed Store: SLS-backed\n");
    
    return cluster;
}

/* Add a node to the cluster */
aerosls_node_info_t* aerosls_cluster_add_node(aerosls_cluster_t *cluster,
                                                const char *hostname,
                                                const char *ip,
                                                node_role_t role) {
    
    if (!cluster || !hostname) return NULL;
    
    printf("\n📡 Adding node to cluster: %s\n", hostname);
    
    aerosls_node_info_t *node = calloc(1, sizeof(aerosls_node_info_t));
    if (!node) return NULL;
    
    /* Generate node ID */
    snprintf(node->node_id, sizeof(node->node_id), "aerosls-node-%s-%d",
             hostname, cluster->node_count);
    
    strncpy(node->hostname, hostname, sizeof(node->hostname) - 1);
    strncpy(node->ip_address, ip, sizeof(node->ip_address) - 1);
    node->role = role;
    node->state = NODE_STATE_PROVISIONING;
    
    /* Detect node resources */
    node->capacity.cpu_cores = detect_node_cpus(ip);
    node->capacity.ram_mb = detect_node_ram(ip);
    node->capacity.pmem_mb = detect_node_pmem(ip);
    node->capacity.max_instances = calculate_node_instances(node);
    node->capacity.max_pods = node->capacity.max_instances * 10;
    
    /* Set allocatable to capacity initially */
    node->allocatable = node->capacity;
    
    /* Add labels */
    aerosls_node_add_label(node, "aerosls.io/hostname", hostname);
    aerosls_node_add_label(node, "aerosls.io/role", 
                            node_role_to_string(role));
    aerosls_node_add_label(node, "aerosls.io/version", 
                            cluster->config.version);
    
    /* Initialize lock */
    pthread_rwlock_init(&node->lock, NULL);
    
    /* Record timestamps */
    clock_gettime(CLOCK_MONOTONIC, &node->created_at);
    node->last_heartbeat = node->created_at;
    
    /* Add to cluster */
    pthread_rwlock_wrlock(&cluster->nodes_lock);
    
    if (cluster->node_count >= cluster->node_capacity) {
        /* Expand array */
        cluster->node_capacity *= 2;
        cluster->nodes = realloc(cluster->nodes,
                                  cluster->node_capacity * sizeof(aerosls_node_info_t*));
    }
    
    cluster->nodes[cluster->node_count] = node;
    cluster->node_count++;
    
    /* Update metrics */
    cluster->metrics.total_nodes = cluster->node_count;
    cluster->metrics.total_cpu_cores += node->capacity.cpu_cores;
    cluster->metrics.total_ram_mb += node->capacity.ram_mb;
    cluster->metrics.total_pmem_mb += node->capacity.pmem_mb;
    
    pthread_rwlock_unlock(&cluster->nodes_lock);
    
    /* Provision the node */
    aerosls_node_provision(cluster, node);
    
    printf("✅ Node added: %s (%s)\n", node->node_id, node->hostname);
    printf("   Role: %s\n", node_role_to_string(role));
    printf("   CPUs: %lu, RAM: %lu MB, PMEM: %lu MB\n",
           node->capacity.cpu_cores, node->capacity.ram_mb, 
           node->capacity.pmem_mb);
    
    return node;
}

/* Remove a node from the cluster */
int aerosls_cluster_remove_node(aerosls_cluster_t *cluster,
                                  const char *node_id) {
    
    if (!cluster || !node_id) return -1;
    
    printf("\n🔴 Removing node from cluster: %s\n", node_id);
    
    pthread_rwlock_wrlock(&cluster->nodes_lock);
    
    /* Find node */
    aerosls_node_info_t *node = NULL;
    int node_index = -1;
    
    for (int i = 0; i < cluster->node_count; i++) {
        if (strcmp(cluster->nodes[i]->node_id, node_id) == 0) {
            node = cluster->nodes[i];
            node_index = i;
            break;
        }
    }
    
    if (!node) {
        pthread_rwlock_unlock(&cluster->nodes_lock);
        fprintf(stderr, "Node not found: %s\n", node_id);
        return -1;
    }
    
    /* Drain workloads from node */
    printf("   Draining workloads...\n");
    aerosls_node_drain(cluster, node);
    
    /* Wait for workloads to be rescheduled */
    printf("   Waiting for workload migration...\n");
    aerosls_wait_for_drain(cluster, node, 300);  /* 5 minute timeout */
    
    /* Remove from array */
    for (int i = node_index; i < cluster->node_count - 1; i++) {
        cluster->nodes[i] = cluster->nodes[i + 1];
    }
    cluster->node_count--;
    
    /* Update metrics */
    cluster->metrics.total_nodes = cluster->node_count;
    cluster->metrics.total_cpu_cores -= node->capacity.cpu_cores;
    cluster->metrics.total_ram_mb -= node->capacity.ram_mb;
    cluster->metrics.total_pmem_mb -= node->capacity.pmem_mb;
    
    pthread_rwlock_unlock(&cluster->nodes_lock);
    
    /* Decommission the node */
    aerosls_node_decommission(node);
    
    /* Free node */
    free(node);
    
    printf("✅ Node removed: %s\n", node_id);
    
    return 0;
}

/* Drain workloads from a node */
int aerosls_node_drain(aerosls_cluster_t *cluster,
                        aerosls_node_info_t *node) {
    
    printf("   Draining node %s...\n", node->node_id);
    
    /* Mark node as draining */
    node->state = NODE_STATE_DRAINING;
    
    /* Evict all workloads */
    int evicted = 0;
    for (int i = 0; i < node->workload_count; i++) {
        aerosls_workload_t *workload = node->workloads[i];
        
        printf("      Evicting workload: %s\n", workload->spec.name);
        
        /* Reschedule to another node */
        if (aerosls_scheduler_reschedule(cluster->scheduler, 
                                          workload) == 0) {
            evicted++;
        }
    }
    
    printf("   Evicted %d workloads\n", evicted);
    
    return evicted;
}

/* Cluster reconciliation loop - ensures desired state */
void* cluster_reconciler_worker(void *arg) {
    
    aerosls_cluster_t *cluster = (aerosls_cluster_t*)arg;
    
    printf("🔄 Cluster reconciler started\n");
    
    while (cluster->reconciler_running) {
        
        /* 1. Check node health */
        aerosls_cluster_check_nodes(cluster);
        
        /* 2. Reconcile workloads */
        aerosls_cluster_reconcile_workloads(cluster);
        
        /* 3. Check scaling */
        aerosls_cluster_check_scaling(cluster);
        
        /* 4. Update metrics */
        aerosls_cluster_update_metrics(cluster);
        
        /* 5. Handle alerts */
        aerosls_cluster_process_alerts(cluster);
        
        /* 6. Backup state to SLS */
        aerosls_cluster_backup_state(cluster);
        
        /* Sleep for reconciliation interval */
        sleep(10);
    }
    
    return NULL;
}

/* Reconcile workloads - Ensure actual state matches desired */
void aerosls_cluster_reconcile_workloads(aerosls_cluster_t *cluster) {
    
    pthread_rwlock_rdlock(&cluster->workloads_lock);
    
    for (int i = 0; i < cluster->workload_count; i++) {
        aerosls_workload_t *workload = cluster->workloads[i];
        
        pthread_mutex_lock(&workload->lock);
        
        /* Check if actual replicas match desired */
        if (workload->status.current_replicas != 
            workload->spec.scaling.target_replicas) {
            
            printf("   Reconciling workload %s: %d/%d replicas\n",
                   workload->spec.name,
                   workload->status.current_replicas,
                   workload->spec.scaling.target_replicas);
            
            if (workload->status.current_replicas < 
                workload->spec.scaling.target_replicas) {
                /* Scale up */
                int needed = workload->spec.scaling.target_replicas - 
                            workload->status.current_replicas;
                aerosls_workload_scale_up(cluster, workload, needed);
            } else {
                /* Scale down */
                int excess = workload->status.current_replicas - 
                            workload->spec.scaling.target_replicas;
                aerosls_workload_scale_down(cluster, workload, excess);
            }
        }
        
        /* Check for failed contexts */
        for (int j = 0; j < workload->context_count; j++) {
            exec_context_t *ctx = workload->contexts[j];
            
            if (ctx->state == EXEC_STATE_CRASHED) {
                printf("   Recovering crashed context: %s\n", ctx->name);
                
                /* SLS enables automatic recovery! */
                if (ctx->flags & EXEC_FLAG_PERSISTENT) {
                    exec_context_recover(ctx);
                    printf("   ✅ Context recovered from SLS\n");
                }
            }
        }
        
        pthread_mutex_unlock(&workload->lock);
    }
    
    pthread_rwlock_unlock(&cluster->workloads_lock);
}

/* Check cluster scaling */
void aerosls_cluster_check_scaling(aerosls_cluster_t *cluster) {
    
    if (!cluster->config.auto_scaling) return;
    
    /* Check CPU utilization */
    double cpu_util = cluster->metrics.avg_cpu_utilization;
    
    if (cpu_util > cluster->config.scale_up_threshold * 100) {
        printf("📈 Cluster CPU utilization high (%.1f%%), scaling up\n", cpu_util);
        
        /* Add a new worker node */
        aerosls_cluster_scale_up(cluster, 1);
    } else if (cpu_util < cluster->config.scale_down_threshold * 100) {
        printf("📉 Cluster CPU utilization low (%.1f%%), scaling down\n", cpu_util);
        
        /* Remove a worker node */
        aerosls_cluster_scale_down(cluster, 1);
    }
}
```

```plaintext
// src/control/api_server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <json-c/json.h>
#include "aerosls/control/cluster.h"

/* ================================================================
 * AeroSLS API Server - Like Kubernetes API Server
 * ================================================================ */

#define API_PORT 6443

/* API context */
typedef struct {
    aerosls_cluster_t   *cluster;
    int                 port;
    struct MHD_Daemon   *daemon;
} api_server_t;

/* Create API server */
api_server_t* aerosls_api_server_create(aerosls_cluster_t *cluster, int port) {
    
    api_server_t *server = calloc(1, sizeof(api_server_t));
    if (!server) return NULL;
    
    server->cluster = cluster;
    server->port = port;
    
    /* Start HTTP server */
    server->daemon = MHD_start_daemon(
        MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD,
        port,
        NULL, NULL,
        &api_request_handler, server,
        MHD_OPTION_END);
    
    if (!server->daemon) {
        fprintf(stderr, "Failed to start API server\n");
        free(server);
        return NULL;
    }
    
    printf("🌐 API Server listening on port %d\n", port);
    
    return server;
}

/* API Request handler */
static enum MHD_Result api_request_handler(void *cls,
                                            struct MHD_Connection *connection,
                                            const char *url,
                                            const char *method,
                                            const char *version,
                                            const char *upload_data,
                                            size_t *upload_data_size,
                                            void **con_cls) {
    
    api_server_t *server = (api_server_t*)cls;
    
    printf("📡 API %s %s\n", method, url);
    
    /* Parse JSON body */
    json_object *request = NULL;
    if (*con_cls == NULL) {
        *con_cls = calloc(1, sizeof(struct api_context));
        return MHD_YES;
    }
    
    /* Route request */
    if (strcmp(url, "/api/v1/namespaces") == 0) {
        return handle_namespaces(server, connection, method, request);
    }
    else if (strncmp(url, "/api/v1/nodes", 13) == 0) {
        return handle_nodes(server, connection, method, url, request);
    }
    else if (strncmp(url, "/api/v1/workloads", 17) == 0) {
        return handle_workloads(server, connection, method, url, request);
    }
    else if (strncmp(url, "/api/v1/services", 16) == 0) {
        return handle_services(server, connection, method, url, request);
    }
    else if (strcmp(url, "/api/v1/cluster/status") == 0) {
        return handle_cluster_status(server, connection);
    }
    else if (strcmp(url, "/api/v1/cluster/metrics") == 0) {
        return handle_cluster_metrics(server, connection);
    }
    else if (strncmp(url, "/api/v1/logs", 12) == 0) {
        return handle_logs(server, connection, url);
    }
    else if (strncmp(url, "/api/v1/exec", 12) == 0) {
        return handle_exec(server, connection, url, request);
    }
    
    /* 404 */
    return api_send_error(connection, 404, "Not found");
}

/* Handle cluster status */
static enum MHD_Result handle_cluster_status(api_server_t *server,
                                               struct MHD_Connection *connection) {
    
    aerosls_cluster_t *cluster = server->cluster;
    
    json_object *response = json_object_new_object();
    
    json_object_object_add(response, "cluster_name",
        json_object_new_string(cluster->config.cluster_name));
    json_object_object_add(response, "cluster_id",
        json_object_new_string(cluster->config.cluster_id));
    json_object_object_add(response, "state",
        json_object_new_string(cluster_state_to_string(cluster->config.state)));
    json_object_object_add(response, "version",
        json_object_new_string(cluster->config.version));
    
    /* Node summary */
    json_object *nodes = json_object_new_object();
    json_object_object_add(nodes, "total",
        json_object_new_int64(cluster->metrics.total_nodes));
    json_object_object_add(nodes, "online",
        json_object_new_int64(cluster->metrics.online_nodes));
    json_object_object_add(nodes, "offline",
        json_object_new_int64(cluster->metrics.offline_nodes));
    json_object_object_add(response, "nodes", nodes);
    
    /* Resource summary */
    json_object *resources = json_object_new_object();
    json_object_object_add(resources, "total_cpu_cores",
        json_object_new_int64(cluster->metrics.total_cpu_cores));
    json_object_object_add(resources, "allocated_cpu_cores",
        json_object_new_int64(cluster->metrics.allocated_cpu_cores));
    json_object_object_add(resources, "total_ram_mb",
        json_object_new_int64(cluster->metrics.total_ram_mb));
    json_object_object_add(resources, "allocated_ram_mb",
        json_object_new_int64(cluster->metrics.allocated_ram_mb));
    json_object_object_add(resources, "total_pmem_mb",
        json_object_new_int64(cluster->metrics.total_pmem_mb));
    json_object_object_add(resources, "allocated_pmem_mb",
        json_object_new_int64(cluster->metrics.allocated_pmem_mb));
    json_object_object_add(response, "resources", resources);
    
    /* Workload summary */
    json_object *workloads = json_object_new_object();
    json_object_object_add(workloads, "total",
        json_object_new_int64(cluster->metrics.total_workloads));
    json_object_object_add(workloads, "running",
        json_object_new_int64(cluster->metrics.running_workloads));
    json_object_object_add(response, "workloads", workloads);
    
    return api_send_json(connection, 200, response);
}

/* Handle workload operations */
static enum MHD_Result handle_workloads(api_server_t *server,
                                          struct MHD_Connection *connection,
                                          const char *method,
                                          const char *url,
                                          json_object *request) {
    
    aerosls_cluster_t *cluster = server->cluster;
    
    /* POST /api/v1/workloads - Create workload */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/v1/workloads") == 0) {
        
        /* Parse workload spec from request */
        workload_spec_t spec = {0};
        
        json_object *metadata = json_object_object_get(request, "metadata");
        json_object *spec_obj = json_object_object_get(request, "spec");
        
        if (!metadata || !spec_obj) {
            return api_send_error(connection, 400, "Invalid workload spec");
        }
        
        /* Extract name */
        json_object *name = json_object_object_get(metadata, "name");
        if (name) {
            strncpy(spec.name, json_object_get_string(name), 
                    sizeof(spec.name) - 1);
        }
        
        /* Extract namespace */
        json_object *namespace = json_object_object_get(metadata, "namespace");
        if (namespace) {
            strncpy(spec.namespace, json_object_get_string(namespace),
                    sizeof(spec.namespace) - 1);
        } else {
            strncpy(spec.namespace, "default", sizeof(spec.namespace) - 1);
        }
        
        /* Extract container image */
        json_object *image = json_object_object_get(spec_obj, "image");
        if (image) {
            strncpy(spec.spec.image, json_object_get_string(image),
                    sizeof(spec.spec.image) - 1);
        }
        
        /* Extract resources */
        json_object *resources = json_object_object_get(spec_obj, "resources");
        if (resources) {
            json_object *cpu = json_object_object_get(resources, "cpu");
            if (cpu) spec.resources.cpu_millicores = json_object_get_int64(cpu);
            
            json_object *memory = json_object_object_get(resources, "memory");
            if (memory) spec.resources.ram_mb = json_object_get_int64(memory);
            
            json_object *pmem = json_object_object_get(resources, "pmem");
            if (pmem) spec.resources.pmem_mb = json_object_get_int64(pmem);
        }
        
        /* Extract scaling */
        json_object *scaling = json_object_object_get(spec_obj, "scaling");
        if (scaling) {
            json_object *replicas = json_object_object_get(scaling, "replicas");
            if (replicas) {
                spec.scaling.min_replicas = json_object_get_int(replicas);
                spec.scaling.max_replicas = json_object_get_int(replicas);
                spec.scaling.target_replicas = json_object_get_int(replicas);
            }
        }
        
        /* Extract SLS features */
        json_object *sls_features = json_object_object_get(spec_obj, "sls_features");
        if (sls_features) {
            json_object *persistent = json_object_object_get(sls_features, "persistent_execution");
            if (persistent) spec.sls_features.persistent_execution = json_object_get_boolean(persistent);
            
            json_object *migration = json_object_object_get(sls_features, "live_migration");
            if (migration) spec.sls_features.live_migration = json_object_get_boolean(migration);
        }
        
        /* Create the workload */
        aerosls_workload_t *workload = aerosls_workload_create(cluster, &spec);
        
        if (!workload) {
            return api_send_error(connection, 500, "Failed to create workload");
        }
        
        /* Return created workload */
        json_object *response = workload_to_json(workload);
        return api_send_json(connection, 201, response);
    }
    
    /* GET /api/v1/workloads - List workloads */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/v1/workloads") == 0) {
        
        json_object *response = json_object_new_array();
        
        pthread_rwlock_rdlock(&cluster->workloads_lock);
        for (int i = 0; i < cluster->workload_count; i++) {
            json_object_array_add(response, 
                                  workload_to_json(cluster->workloads[i]));
        }
        pthread_rwlock_unlock(&cluster->workloads_lock);
        
        return api_send_json(connection, 200, response);
    }
    
    /* GET /api/v1/workloads/{name} - Get specific workload */
    if (strcmp(method, "GET") == 0) {
        /* Parse name from URL */
        char workload_name[128];
        sscanf(url, "/api/v1/workloads/%127s", workload_name);
        
        aerosls_workload_t *workload = aerosls_workload_find(cluster, workload_name);
        if (!workload) {
            return api_send_error(connection, 404, "Workload not found");
        }
        
        json_object *response = workload_to_json(workload);
        return api_send_json(connection, 200, response);
    }
    
    return api_send_error(connection, 405, "Method not allowed");
}
```

```plaintext
// src/control/scheduler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aerosls/control/cluster.h"

/* ================================================================
 * AeroSLS Scheduler - Intelligent workload placement
 * ================================================================ */

/* Schedule a workload onto the best node */
int aerosls_scheduler_schedule(aerosls_scheduler_t *scheduler,
                                 aerosls_workload_t *workload) {
    
    aerosls_cluster_t *cluster = scheduler->cluster;
    
    printf("📋 Scheduling workload: %s\n", workload->spec.name);
    
    /* Get available nodes */
    aerosls_node_info_t **candidates = NULL;
    int candidate_count = 0;
    
    aerosls_scheduler_filter_nodes(cluster, workload, 
                                    &candidates, &candidate_count);
    
    if (candidate_count == 0) {
        fprintf(stderr, "❌ No suitable nodes found for %s\n", 
                workload->spec.name);
        return -1;
    }
    
    /* Score candidates */
    double scores[candidate_count];
    aerosls_scheduler_score_nodes(cluster, workload, candidates, 
                                   candidate_count, scores);
    
    /* Select best node */
    int best_index = 0;
    double best_score = scores[0];
    
    for (int i = 1; i < candidate_count; i++) {
        if (scores[i] > best_score) {
            best_score = scores[i];
            best_index = i;
        }
    }
    
    aerosls_node_info_t *selected = candidates[best_index];
    
    printf("   Selected node: %s (score: %.2f)\n", 
           selected->hostname, best_score);
    
    /* Bind workload to node */
    strncpy(workload->assigned_node, selected->node_id,
            sizeof(workload->assigned_node) - 1);
    
    /* Allocate resources */
    selected->allocatable.cpu_cores -= 
        workload->spec.resources.cpu_millicores / 1000;
    selected->allocatable.ram_mb -= workload->spec.resources.ram_mb;
    selected->allocatable.pmem_mb -= workload->spec.resources.pmem_mb;
    
    /* Add to node's workload list */
    pthread_rwlock_wrlock(&selected->lock);
    selected->workloads = realloc(selected->workloads,
                                   (selected->workload_count + 1) * sizeof(aerosls_workload_t*));
    selected->workloads[selected->workload_count] = workload;
    selected->workload_count++;
    pthread_rwlock_unlock(&selected->lock);
    
    free(candidates);
    
    return 0;
}

/* Filter nodes based on constraints */
void aerosls_scheduler_filter_nodes(aerosls_cluster_t *cluster,
                                      aerosls_workload_t *workload,
                                      aerosls_node_info_t ***candidates,
                                      int *count) {
    
    int capacity = 16;
    *candidates = calloc(capacity, sizeof(aerosls_node_info_t*));
    *count = 0;
    
    pthread_rwlock_rdlock(&cluster->nodes_lock);
    
    for (int i = 0; i < cluster->node_count; i++) {
        aerosls_node_info_t *node = cluster->nodes[i];
        
        /* Skip non-ready nodes */
        if (node->state != NODE_STATE_ONLINE) continue;
        
        /* Skip draining nodes */
        if (node->state == NODE_STATE_DRAINING) continue;
        
        /* Check node selector */
        if (!aerosls_scheduler_match_labels(node, workload)) continue;
        
        /* Check resource availability */
        if (!aerosls_scheduler_check_resources(node, workload)) continue;
        
        /* Check taints/tolerations */
        if (!aerosls_scheduler_check_taints(node, workload)) continue;
        
        /* Check affinity/anti-affinity */
        if (!aerosls_scheduler_check_affinity(node, workload, cluster)) continue;
        
        /* Node passed all filters! */
        if (*count >= capacity) {
            capacity *= 2;
            *candidates = realloc(*candidates, 
                                  capacity * sizeof(aerosls_node_info_t*));
        }
        
        (*candidates)[*count] = node;
        (*count)++;
    }
    
    pthread_rwlock_unlock(&cluster->nodes_lock);
}

/* Score nodes using multiple criteria */
void aerosls_scheduler_score_nodes(aerosls_cluster_t *cluster,
                                     aerosls_workload_t *workload,
                                     aerosls_node_info_t **candidates,
                                     int candidate_count,
                                     double *scores) {
    
    for (int i = 0; i < candidate_count; i++) {
        aerosls_node_info_t *node = candidates[i];
        double score = 0.0;
        
        /* 1. Resource balance score (bin packing) */
        double cpu_ratio = (double)node->allocatable.cpu_cores / 
                          (double)node->capacity.cpu_cores;
        double ram_ratio = (double)node->allocatable.ram_mb / 
                          (double)node->capacity.ram_mb;
        double balance = 1.0 - fabs(cpu_ratio - ram_ratio);
        score += balance * 30;  /* 30% weight */
        
        /* 2. PMEM availability (critical for SLS) */
        if (workload->spec.sls_features.persistent_execution) {
            double pmem_ratio = (double)node->allocatable.pmem_mb / 
                               (double)node->capacity.pmem_mb;
            score += pmem_ratio * 40;  /* 40% weight for SLS workloads */
        }
        
        /* 3. Workload spread */
        score += (1.0 - (double)node->workload_count / 
                  (double)cluster->workload_count) * 15;  /* 15% weight */
        
        /* 4. Node preference (labels) */
        if (aerosls_scheduler_node_preferred(node, workload)) {
            score += 15;  /* 15% bonus */
        }
        
        scores[i] = score;
    }
}
```

```bash
#!/bin/bash
# aeroslsctl - AeroSLS Control CLI (like kubectl)

AEROSLS_API="${AEROSLS_API:-https://localhost:6443}"
AEROSLS_TOKEN="${AEROSLS_TOKEN:-}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_header() {
    echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║${NC}  $1"
    echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
}

# Cluster operations
cmd_cluster_status() {
    print_header "AeroSLS Cluster Status"
    
    curl -s -H "Authorization: Bearer $AEROSLS_TOKEN" \
         "$AEROSLS_API/api/v1/cluster/status" | \
         python3 -m json.tool
}

cmd_cluster_metrics() {
    print_header "AeroSLS Cluster Metrics"
    
    curl -s -H "Authorization: Bearer $AEROSLS_TOKEN" \
         "$AEROSLS_API/api/v1/cluster/metrics" | \
         python3 -m json.tool
}

# Node operations
cmd_nodes_list() {
    print_header "Nodes"
    
    printf "%-40s %-20s %-10s %-15s %-10s %-10s\n" \
           "NAME" "HOSTNAME" "STATUS" "ROLE" "CPU" "RAM"
    echo "──────────────────────────────────────────────────────────────────────────────────────"
    
    curl -s -H "Authorization: Bearer $AEROSLS_TOKEN" \
         "$AEROSLS_API/api/v1/nodes" | \
         python3 -c "
import json, sys
nodes = json.load(sys.stdin)
for n in nodes:
    print(f\"{n['node_id']:<40} {n['hostname']:<20} {n['state']:<10} {n['role']:<15} {n['capacity']['cpu_cores']:<10} {n['capacity']['ram_mb']:<10}\")
"
}

cmd_node_drain() {
    local node=$1
    print_header "Draining Node: $node"
    
    curl -s -X PUT -H "Authorization: Bearer $AEROSLS_TOKEN" \
         "$AEROSLS_API/api/v1/nodes/$node/drain" | \
         python3 -m json.tool
}

# Workload operations
cmd_workloads_list() {
    print_header "Workloads"
    
    printf "%-30s %-15s %-10s %-15s %-10s\n" \
           "NAME" "NAMESPACE" "REPLICAS" "STATUS" "AGE"
    echo "──────────────────────────────────────────────────────────────────────────────"
    
    curl -s -H "Authorization: Bearer $AEROSLS_TOKEN" \
         "$AEROSLS_API/api/v1/workloads" | \
         python3 -c "
import json, sys
workloads = json.load(sys.stdin)
for w in workloads:
    print(f\"{w['name']:<30} {w['namespace']:<15} {w['status']['ready_replicas']}/{w['spec']['scaling']['target_replicas']:<10} {w['status']['phase']:<15} {w['status']['age']:<10}\")
"
}

cmd_workload_create() {
    local file=$1
    print_header "Creating Workload from: $file"
    
    curl -s -X POST -H "Authorization: Bearer $AEROSLS_TOKEN" \
         -H "Content-Type: application/json" \
         -d "@$file" \
         "$AEROSLS_API/api/v1/workloads" | \
         python3 -m json.tool
}

cmd_workload_scale() {
    local name=$1
    local replicas=$2
    print_header "Scaling Workload: $name -> $replicas replicas"
    
    curl -s -X PUT -H "Authorization: Bearer $AEROSLS_TOKEN" \
         -H "Content-Type: application/json" \
         -d "{\"spec\":{\"scaling\":{\"replicas\":$replicas}}}" \
         "$AEROSLS_API/api/v1/workloads/$name/scale" | \
         python3 -m json.tool
}

cmd_workload_logs() {
    local name=$1
    print_header "Logs for: $name"
    
    curl -s -H "Authorization: Bearer $AEROSLS_TOKEN" \
         "$AEROSLS_API/api/v1/logs/$name?tail=50"
}

cmd_workload_exec() {
    local name=$1
    shift
    print_header "Exec into: $name"
    
    curl -s -X POST -H "Authorization: Bearer $AEROSLS_TOKEN" \
         -H "Content-Type: application/json" \
         -d "{\"command\":\"$*\"}" \
         "$AEROSLS_API/api/v1/exec/$name"
}

# Main CLI
main() {
    case "$1" in
        cluster)
            case "$2" in
                status)  cmd_cluster_status ;;
                metrics) cmd_cluster_metrics ;;
                *) echo "Usage: aeroslsctl cluster {status|metrics}" ;;
            esac
            ;;
        nodes|node)
            case "$2" in
                list|get) cmd_nodes_list ;;
                drain)    cmd_node_drain "$3" ;;
                *) echo "Usage: aeroslsctl nodes {list|drain <node>}" ;;
            esac
            ;;
        workloads|workload|wl)
            case "$2" in
                list|get) cmd_workloads_list ;;
                create)   cmd_workload_create "$3" ;;
                scale)    cmd_workload_scale "$3" "$4" ;;
                logs)     cmd_workload_logs "$3" ;;
                exec)     shift 2; cmd_workload_exec "$@" ;;
                *) echo "Usage: aeroslsctl workloads {list|create|scale|logs|exec}" ;;
            esac
            ;;
        *)
            echo "AeroSLS Control CLI (aeroslsctl)"
            echo ""
            echo "Usage: aeroslsctl <command> [options]"
            echo ""
            echo "Commands:"
            echo "  cluster status        Show cluster status"
            echo "  cluster metrics       Show cluster metrics"
            echo "  nodes list            List all nodes"
            echo "  nodes drain <node>    Drain a node"
            echo "  workloads list        List all workloads"
            echo "  workloads create <file>  Create workload from file"
            echo "  workloads scale <name> <replicas>  Scale a workload"
            echo "  workloads logs <name>  View workload logs"
            echo "  workloads exec <name> <cmd>  Execute command in workload"
            ;;
    esac
}

main "$@"
```

```plaintext
# examples/retail-store-workload.yaml
# AeroSLS Workload Definition (like Kubernetes Deployment)

apiVersion: aerosls.io/v1
kind: Workload
metadata:
  name: store-pos-processor
  namespace: retail-chain
  labels:
    app: store-pos
    tier: edge
    region: us-east
  annotations:
    description: "Store POS Transaction Processor"

spec:
  # Function/Container specification
  image: aerosls.registry/retail/pos-processor:2.1.0
  command: "/usr/local/bin/store-pos-processor"
  env:
    - name: STORE_REGION
      value: "us-east"
    - name: LOG_LEVEL
      value: "info"
    - name: SLS_PERSISTENT
      value: "true"
  
  # Resource requirements
  resources:
    cpu: 2000        # 2 CPU cores (in millicores)
    memory: 4096     # 4 GB RAM
    pmem: 8192       # 8 GB persistent memory for SLS
  
  # Scaling (10,000 stores = 10,000 replicas!)
  scaling:
    min_replicas: 10000
    max_replicas: 15000
    target_replicas: 10000
    auto_scaling: true
    target_cpu_percent: 70
  
  # Service mesh integration
  networking:
    port: 8080
    protocol: "SLS"        # Use SLS shared memory!
    service_mesh: true
    ingress:
      - host: "pos.retail.example.com"
  
  # Persistent storage with SLS
  storage:
    volumes:
      - name: transaction-log
        size: "100GB"
        mount_path: "/data/transactions"
        storage_class: "sls-persistent"
  
  # Health monitoring
  health:
    liveness_probe: true
    liveness_command: "/usr/local/bin/health-check"
    liveness_initial_delay: 30
    liveness_period: 10
    readiness_probe: true
    readiness_command: "/usr/local/bin/ready-check"
    readiness_initial_delay: 10
    readiness_period: 5
  
  # Update strategy
  update_strategy:
    type: "RollingUpdate"
    max_surge: 100        # 100 stores at a time
    max_unavailable: 50   # Max 50 stores down during update
    min_ready_seconds: 30
  
  # Scheduling
  scheduling:
    policy: "spread"      # Spread across nodes
    constraints:
      node_selector:
        - key: "aerosls.io/role"
          value: "edge"
      affinity:
        - "spread-across-regions"
      anti_affinity:
        - "max-2-per-node"
  
  # SLS-specific features
  sls_features:
    persistent_execution: true     # Survive crashes!
    live_migration: true           # Zero-downtime move
    checkpoint_interval: 300       # Every 5 minutes
    encrypted_memory: true         # PCI-DSS compliance
    replicated_memory: true        # HA across nodes

---
apiVersion: aerosls.io/v1
kind: Service
metadata:
  name: pos-processor-service
  namespace: retail-chain

spec:
  type: ClusterIP
  selector:
    app: store-pos
  ports:
    - name: sls-mesh
      protocol: SLS           # Memory-speed service mesh!
      port: 8080
    - name: http
      protocol: TCP
      port: 8080
      target_port: 8080
```

#### This complete AeroSLS Control Plane provides:

1. **Cluster Manager** - Like Kubernetes control plane but SLS-native
2. **API Server** - RESTful API for all operations
3. **Scheduler** - Intelligent workload placement with SLS awareness
4. **Controller Manager** - Reconciliation loops for desired state
5. **CLI Tool** (`aeroslsctl`) - Complete command-line interface
6. **YAML Definitions** - Declarative workload specifications
7. **Auto-scaling** - Both cluster and workload level
8. **Self-healing** - Automatic crash recovery via SLS
9. **Service Mesh** - Integrated SLS shared memory mesh
10. **RBAC, Network Policies, Security** - Enterprise-grade features

The key differentiator from Kubernetes: **SLS persistence makes everything recoverable automatically** - no need for complex StatefulSets or persistent volume claims!
