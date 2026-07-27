## **AeroSLS Deployment Architecture with QEMU**

QEMU is the perfect foundation for AeroSLS deployment. Here's the complete deployment architecture that makes AeroSLS the default environment on new servers.

```plaintext
// include/aerosls/deploy/qemu_platform.h
#ifndef AEROSLS_QEMU_PLATFORM_H
#define AEROSLS_QEMU_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "aerosls/aerosls.h"

/* QEMU Machine Types optimized for AeroSLS */
typedef enum {
    QEMU_MACHINE_PC         = 0,    /* Standard PC (legacy) */
    QEMU_MACHINE_Q35        = 1,    /* Modern Q35 chipset */
    QEMU_MACHINE_MICROVM    = 2,    /* Minimal microvm (fastest boot) */
    QEMU_MACHINE_VIRT       = 3,    /* Virtual platform (ARM/RISCV) */
    QEMU_MACHINE_AEROSLS    = 4,    /* Custom AeroSLS machine type */
} qemu_machine_type_t;

/* Persistent Memory Backend Types */
typedef enum {
    PMEM_BACKEND_DEVDAX     = 0,    /* /dev/daxX.X (direct access) */
    PMEM_BACKEND_FSDAX      = 1,    /* ext4/xfs with DAX mount */
    PMEM_BACKEND_MEMFILE    = 2,    /* Memory-backed file on PMEM */
    PMEM_BACKEND_NVDIMM     = 3,    /* Virtual NVDIMM device */
    PMEM_BACKEND_CXL        = 4,    /* CXL Type-3 memory device */
} pmem_backend_type_t;

/* AeroSLS QEMU Instance Configuration */
typedef struct {
    /* Machine Configuration */
    qemu_machine_type_t machine_type;
    int                 vcpus;
    int                 cores_per_socket;
    int                 sockets;
    uint64_t            ram_size_mb;
    
    /* Persistent Memory Configuration */
    pmem_backend_type_t pmem_type;
    char                pmem_path[256];     /* Path to PMEM device/file */
    uint64_t            pmem_size_mb;
    bool                pmem_share;         /* Share between instances */
    
    /* Network Configuration */
    char                bridge_name[32];    /* virbr0, aerosls-br0 */
    char                mac_address[18];
    bool                virtio_net;         /* Use virtio-net */
    
    /* Storage Configuration */
    char                boot_image[256];    /* AeroSLS boot image */
    bool                use_cloudinit;      /* Cloud-init for config */
    char                cloudinit_image[256];
    
    /* AeroSLS Specific */
    uint64_t            max_contexts;
    uint64_t            max_services;
    bool                enable_sls;         /* Enable SLS features */
    bool                enable_mesh;        /* Enable service mesh */
    
    /* Performance */
    bool                huge_pages;         /* Use huge pages */
    bool                cpu_pinning;        /* Pin vCPUs to pCPUs */
    bool                numa_pinning;       /* Pin to NUMA nodes */
    char                cpu_affinity[64];   /* CPU affinity mask */
    
    /* Instance Identity */
    char                instance_id[64];
    char                instance_name[128];
    char                cluster_name[64];
    char                node_role[32];      /* worker, manager, edge */
    
} aerosls_qemu_config_t;

/* QEMU Instance Runtime */
typedef struct {
    pid_t               qemu_pid;
    int                 monitor_fd;         /* QMP monitor socket */
    int                 guest_agent_fd;     /* QEMU guest agent */
    aerosls_qemu_config_t config;
    
    /* Instance state */
    bool                running;
    bool                paused;
    uint64_t            uptime_seconds;
    
    /* Migration state */
    bool                migrating;
    char                migration_target[256];
    int                 migration_progress;
    
    /* Statistics */
    struct {
        double          cpu_usage;
        uint64_t        ram_used_mb;
        uint64_t        pmem_used_mb;
        uint64_t        network_rx_bytes;
        uint64_t        network_tx_bytes;
        uint64_t        disk_read_iops;
        uint64_t        disk_write_iops;
    } stats;
    
    /* Linked list */
    struct aerosls_qemu_instance *next;
    struct aerosls_qemu_instance *prev;
    
} aerosls_qemu_instance_t;

/* AeroSLS Node - Can run multiple QEMU instances */
typedef struct {
    char                node_id[64];
    char                hostname[256];
    
    /* Hardware info */
    int                 physical_cores;
    int                 logical_cores;
    uint64_t            total_ram_mb;
    uint64_t            available_ram_mb;
    uint64_t            total_pmem_mb;
    uint64_t            available_pmem_mb;
    int                 numa_nodes;
    
    /* Running instances */
    aerosls_qemu_instance_t *instances;
    int                 instance_count;
    int                 max_instances;
    
    /* Resource allocation */
    uint64_t            allocated_vcpus;
    uint64_t            allocated_ram_mb;
    uint64_t            allocated_pmem_mb;
    
    /* Node state */
    bool                active;
    bool                maintenance;
    struct timespec     uptime;
    
} aerosls_node_t;

#endif /* AEROSLS_QEMU_PLATFORM_H */
```

```plaintext
// src/deploy/qemu_launcher.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <json-c/json.h>
#include "aerosls/deploy/qemu_platform.h"

/* Generate QEMU command line for AeroSLS instance */
char* aerosls_qemu_build_command(aerosls_qemu_config_t *config) {
    
    /* Build QEMU command with optimal settings for AeroSLS */
    static char cmd[8192];
    int offset = 0;
    
    /* Base QEMU binary */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "qemu-system-x86_64 \\\n");
    
    /* Machine type */
    const char *machine;
    switch (config->machine_type) {
    case QEMU_MACHINE_MICROVM:
        machine = "microvm,acpi=off,pit=off,pic=off,rtc=off";
        break;
    case QEMU_MACHINE_Q35:
        machine = "q35,accel=kvm,kernel-irqchip=split";
        break;
    case QEMU_MACHINE_AEROSLS:
        /* Custom AeroSLS-optimized machine */
        machine = "aerosls,accel=kvm,nvdimm=on,cxl=on";
        break;
    default:
        machine = "pc,accel=kvm";
    }
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -machine %s \\\n", machine);
    
    /* CPU configuration */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -cpu host,pmu=off,-kvm-steal-time \\\n"
        "  -smp %d,cores=%d,sockets=%d,threads=1 \\\n",
        config->vcpus, config->cores_per_socket, config->sockets);
    
    /* Memory with huge pages */
    if (config->huge_pages) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -m %luM,slots=4,maxmem=%luM \\\n"
            "  -mem-prealloc \\\n"
            "  -mem-path /dev/hugepages \\\n",
            config->ram_size_mb, 
            config->ram_size_mb * 2);  /* Allow hotplug up to 2x */
    } else {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -m %luM \\\n", config->ram_size_mb);
    }
    
    /* Persistent Memory (NVDIMM) - Key for SLS */
    switch (config->pmem_type) {
    case PMEM_BACKEND_NVDIMM:
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -object memory-backend-file,id=pmem0,"
            "mem-path=%s,size=%luM,share=%s,prealloc=on,align=2M \\\n"
            "  -device nvdimm,id=nvdimm0,memdev=pmem0,"
            "label-size=2M,unarmed=off \\\n",
            config->pmem_path,
            config->pmem_size_mb,
            config->pmem_share ? "on" : "off");
        break;
        
    case PMEM_BACKEND_CXL:
        /* CXL Type-3 memory device */
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -object memory-backend-file,id=cxl-pmem0,"
            "mem-path=%s,size=%luM,share=%s \\\n"
            "  -device cxl-type3,memdev=cxl-pmem0,id=cxl0 \\\n",
            config->pmem_path,
            config->pmem_size_mb,
            config->pmem_share ? "on" : "off");
        break;
        
    case PMEM_BACKEND_MEMFILE:
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -object memory-backend-file,id=pmem0,"
            "mem-path=%s,size=%luM,share=%s \\\n",
            config->pmem_path,
            config->pmem_size_mb,
            config->pmem_share ? "on" : "off");
        break;
        
    default:
        break;
    }
    
    /* Network - virtio for best performance */
    if (config->virtio_net) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -netdev bridge,id=net0,br=%s \\\n"
            "  -device virtio-net-pci,netdev=net0,"
            "mac=%s,mq=on,vectors=%d \\\n",
            config->bridge_name,
            config->mac_address,
            config->vcpus * 2 + 2);
    }
    
    /* Boot disk */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -drive file=%s,format=qcow2,if=virtio,"
        "cache=none,aio=native,discard=unmap \\\n",
        config->boot_image);
    
    /* Cloud-init for automatic configuration */
    if (config->use_cloudinit) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -drive file=%s,format=raw,if=virtio \\\n",
            config->cloudinit_image);
    }
    
    /* QEMU Guest Agent */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -chardev socket,path=/tmp/qga-%s.sock,"
        "server=on,wait=off,id=qga0 \\\n"
        "  -device virtio-serial \\\n"
        "  -device virtserialport,chardev=qga0,name=org.qemu.guest_agent.0 \\\n",
        config->instance_id);
    
    /* QMP Monitor */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -qmp unix:/tmp/qmp-%s.sock,server=on,wait=off \\\n",
        config->instance_id);
    
    /* VNC/SPICE for console access */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -vnc :%d,password=on \\\n",
        get_instance_vnc_port(config));
    
    /* Performance optimizations */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -no-hpet \\\n"           /* Disable HPET for performance */
        "  -no-acpi \\\n"           /* Disable ACPI if using microvm */
        "  -rtc base=utc,clock=host \\\n"
        "  -overcommit mem-lock=on \\\n"  /* Lock memory */
        "  -realtime mlock=on \\\n");     /* Real-time mlock */
    
    /* CPU pinning for performance */
    if (config->cpu_pinning && config->cpu_affinity[0]) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -vcpu vcpupin=%s \\\n", config->cpu_affinity);
    }
    
    /* NUMA pinning */
    if (config->numa_pinning) {
        offset += snprintf(cmd + offset, sizeof(cmd) - offset,
            "  -numa node,nodeid=0,cpus=0-%d,memdev=ram0 \\\n",
            config->vcpus - 1);
    }
    
    /* AeroSLS specific kernel parameters passed to guest */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -append 'console=ttyS0 aerosls.instance_id=%s "
        "aerosls.cluster=%s aerosls.role=%s "
        "aerosls.max_contexts=%lu aerosls.max_services=%lu "
        "aerosls.enable_sls=%d aerosls.enable_mesh=%d' \\\n",
        config->instance_id,
        config->cluster_name,
        config->node_role,
        config->max_contexts,
        config->max_services,
        config->enable_sls,
        config->enable_mesh);
    
    /* Daemonize */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -daemonize \\\n");
    
    /* PID file */
    offset += snprintf(cmd + offset, sizeof(cmd) - offset,
        "  -pidfile /var/run/aerosls/%s.pid\n",
        config->instance_id);
    
    printf("Generated QEMU command:\n%s\n", cmd);
    
    return strdup(cmd);
}

/* Launch AeroSLS QEMU instance */
aerosls_qemu_instance_t* aerosls_qemu_launch(aerosls_qemu_config_t *config) {
    
    printf("🚀 Launching AeroSLS QEMU Instance: %s\n", config->instance_name);
    printf("   Machine: %s\n", config->machine_type == QEMU_MACHINE_AEROSLS ? 
           "AeroSLS Optimized" : "Standard");
    printf("   vCPUs: %d, RAM: %lu MB, PMEM: %lu MB\n",
           config->vcpus, config->ram_size_mb, config->pmem_size_mb);
    
    /* Create runtime directory */
    char run_dir[256];
    snprintf(run_dir, sizeof(run_dir), "/var/run/aerosls");
    mkdir(run_dir, 0755);
    
    /* Build QEMU command */
    char *cmd = aerosls_qemu_build_command(config);
    if (!cmd) {
        fprintf(stderr, "Failed to build QEMU command\n");
        return NULL;
    }
    
    /* Launch QEMU process */
    pid_t pid = fork();
    
    if (pid == 0) {
        /* Child process - execute QEMU */
        char *argv[] = {"/bin/sh", "-c", cmd, NULL};
        execvp("/bin/sh", argv);
        
        /* Should never reach here */
        fprintf(stderr, "Failed to exec QEMU: %s\n", strerror(errno));
        exit(1);
    } else if (pid < 0) {
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        free(cmd);
        return NULL;
    }
    
    /* Create instance structure */
    aerosls_qemu_instance_t *instance = calloc(1, sizeof(aerosls_qemu_instance_t));
    if (!instance) {
        kill(pid, SIGTERM);
        free(cmd);
        return NULL;
    }
    
    instance->qemu_pid = pid;
    memcpy(&instance->config, config, sizeof(aerosls_qemu_config_t));
    instance->running = true;
    
    /* Wait for QEMU to start */
    usleep(500000);  /* 500ms */
    
    /* Connect to QMP monitor */
    char monitor_path[256];
    snprintf(monitor_path, sizeof(monitor_path), 
             "/tmp/qmp-%s.sock", config->instance_id);
    
    instance->monitor_fd = aerosls_qmp_connect(monitor_path);
    if (instance->monitor_fd < 0) {
        fprintf(stderr, "Failed to connect to QMP monitor\n");
    }
    
    /* Connect to guest agent */
    char agent_path[256];
    snprintf(agent_path, sizeof(agent_path), 
             "/tmp/qga-%s.sock", config->instance_id);
    
    instance->guest_agent_fd = aerosls_qga_connect(agent_path);
    if (instance->guest_agent_fd < 0) {
        fprintf(stderr, "Warning: QEMU guest agent not available\n");
    }
    
    /* Verify instance is running */
    if (kill(pid, 0) == 0) {
        printf("✅ AeroSLS Instance '%s' launched (PID: %d)\n", 
               config->instance_name, pid);
        printf("   QMP Monitor: %s\n", monitor_path);
        printf("   Guest Agent: %s\n", agent_path);
    } else {
        fprintf(stderr, "❌ Instance failed to start\n");
        free(instance);
        free(cmd);
        return NULL;
    }
    
    free(cmd);
    return instance;
}

/* Connect to QMP (QEMU Machine Protocol) monitor */
int aerosls_qmp_connect(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    /* QMP handshake */
    char buffer[4096];
    read(fd, buffer, sizeof(buffer));  /* Read greeting */
    
    /* Send QMP capabilities negotiation */
    const char *qmp_cmd = 
        "{\"execute\":\"qmp_capabilities\"}\r\n";
    write(fd, qmp_cmd, strlen(qmp_cmd));
    read(fd, buffer, sizeof(buffer));  /* Read response */
    
    return fd;
}

/* Connect to QEMU Guest Agent */
int aerosls_qga_connect(const char *socket_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}
```

```plaintext
// src/deploy/node_manager.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include "aerosls/deploy/qemu_platform.h"

/* Initialize AeroSLS node */
aerosls_node_t* aerosls_node_init(void) {
    
    aerosls_node_t *node = calloc(1, sizeof(aerosls_node_t));
    if (!node) return NULL;
    
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║     AeroSLS Node Initialization                      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    
    /* Get hostname */
    gethostname(node->hostname, sizeof(node->hostname));
    
    /* Generate node ID */
    snprintf(node->node_id, sizeof(node->node_id), "node-%s-%d",
             node->hostname, getpid());
    
    /* Detect hardware */
    struct sysinfo info;
    sysinfo(&info);
    
    node->physical_cores = sysconf(_SC_NPROCESSORS_CONF) / 2;  /* Physical cores */
    node->logical_cores = sysconf(_SC_NPROCESSORS_CONF);       /* With HT */
    node->total_ram_mb = info.totalram * info.mem_unit / (1024 * 1024);
    node->available_ram_mb = info.freeram * info.mem_unit / (1024 * 1024);
    
    /* Detect NUMA nodes */
    node->numa_nodes = numa_max_node() + 1;
    
    /* Detect persistent memory */
    node->total_pmem_mb = aerosls_detect_pmem_size();
    node->available_pmem_mb = node->total_pmem_mb;
    
    /* Calculate max instances based on resources */
    node->max_instances = calculate_max_instances(node);
    
    /* Initialize instance list */
    node->instances = NULL;
    node->instance_count = 0;
    node->active = true;
    
    clock_gettime(CLOCK_MONOTONIC, &node->uptime);
    
    printf("📊 Node Hardware:\n");
    printf("   Hostname: %s\n", node->hostname);
    printf("   Node ID: %s\n", node->node_id);
    printf("   CPUs: %d physical / %d logical\n", 
           node->physical_cores, node->logical_cores);
    printf("   RAM: %lu MB total / %lu MB available\n",
           node->total_ram_mb, node->available_ram_mb);
    printf("   PMEM: %lu MB total\n", node->total_pmem_mb);
    printf("   NUMA Nodes: %d\n", node->numa_nodes);
    printf("   Max AeroSLS Instances: %d\n", node->max_instances);
    
    return node;
}

/* Calculate optimal number of instances for this node */
int calculate_max_instances(aerosls_node_t *node) {
    
    /* Strategy: Multiple smaller instances for better isolation */
    
    /* Based on CPU: 2-4 vCPUs per instance */
    int cpu_based = node->logical_cores / 2;
    
    /* Based on RAM: Minimum 2GB per instance */
    int ram_based = node->available_ram_mb / 2048;
    
    /* Based on PMEM: Minimum 4GB per instance for SLS */
    int pmem_based = node->available_pmem_mb / 4096;
    
    /* Take the minimum */
    int max = cpu_based;
    if (ram_based < max) max = ram_based;
    if (pmem_based > 0 && pmem_based < max) max = pmem_based;
    
    /* Cap at reasonable limit */
    if (max > 128) max = 128;
    if (max < 1) max = 1;
    
    return max;
}

/* Create optimal instance configuration for this node */
aerosls_qemu_config_t aerosls_node_optimal_config(aerosls_node_t *node,
                                                    const char *role) {
    
    aerosls_qemu_config_t config = {0};
    
    /* Choose machine type */
    config.machine_type = QEMU_MACHINE_AEROSLS;
    
    /* Calculate resources per instance */
    int total_instances = node->max_instances;
    
    config.vcpus = node->logical_cores / total_instances;
    if (config.vcpus < 2) config.vcpus = 2;
    if (config.vcpus > 16) config.vcpus = 16;
    
    config.cores_per_socket = config.vcpus;
    config.sockets = 1;
    
    /* RAM allocation */
    config.ram_size_mb = node->available_ram_mb / total_instances;
    if (config.ram_size_mb < 2048) config.ram_size_mb = 2048;
    if (config.ram_size_mb > 65536) config.ram_size_mb = 65536;
    
    /* PMEM allocation for SLS */
    if (node->total_pmem_mb > 0) {
        config.pmem_type = PMEM_BACKEND_NVDIMM;
        config.pmem_size_mb = node->available_pmem_mb / total_instances;
        strncpy(config.pmem_path, "/dev/pmem0", sizeof(config.pmem_path) - 1);
        config.pmem_share = false;  /* Each instance gets own PMEM */
    }
    
    /* Network */
    strncpy(config.bridge_name, "aerosls-br0", sizeof(config.bridge_name) - 1);
    generate_mac_address(config.mac_address);
    config.virtio_net = true;
    
    /* Storage */
    strncpy(config.boot_image, "/var/lib/aerosls/images/aerosls.qcow2",
            sizeof(config.boot_image) - 1);
    config.use_cloudinit = true;
    strncpy(config.cloudinit_image, "/var/lib/aerosls/cloudinit/${instance}.img",
            sizeof(config.cloudinit_image) - 1);
    
    /* AeroSLS features */
    config.enable_sls = true;
    config.enable_mesh = true;
    config.max_contexts = 1000;
    config.max_services = 100;
    
    /* Performance */
    config.huge_pages = true;
    config.cpu_pinning = true;
    config.numa_pinning = (node->numa_nodes > 1);
    
    /* Role-specific */
    strncpy(config.node_role, role, sizeof(config.node_role) - 1);
    
    return config;
}

/* Deploy an AeroSLS cluster across multiple nodes */
int aerosls_cluster_deploy(aerosls_node_t **nodes, int node_count,
                            int instances_per_node) {
    
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║   Deploying AeroSLS Cluster                           ║\n");
    printf("║   Nodes: %d | Instances per node: %d                  ║\n",
           node_count, instances_per_node);
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    
    int total_instances = 0;
    
    for (int n = 0; n < node_count; n++) {
        aerosls_node_t *node = nodes[n];
        
        printf("═══ Node %d: %s ═══\n", n + 1, node->hostname);
        
        for (int i = 0; i < instances_per_node; i++) {
            /* Create instance config */
            aerosls_qemu_config_t config = aerosls_node_optimal_config(
                node, "worker");
            
            /* Customize per instance */
            snprintf(config.instance_id, sizeof(config.instance_id),
                     "aerosls-%s-%03d", node->hostname, i + 1);
            snprintf(config.instance_name, sizeof(config.instance_name),
                     "AeroSLS Instance %s-%03d", node->hostname, i + 1);
            snprintf(config.cluster_name, sizeof(config.cluster_name),
                     "production-cluster");
            
            /* Launch instance */
            aerosls_qemu_instance_t *instance = aerosls_qemu_launch(&config);
            
            if (instance) {
                /* Add to node's instance list */
                instance->next = node->instances;
                if (node->instances) {
                    node->instances->prev = instance;
                }
                node->instances = instance;
                node->instance_count++;
                total_instances++;
                
                /* Update resource tracking */
                node->allocated_vcpus += config.vcpus;
                node->allocated_ram_mb += config.ram_size_mb;
                node->allocated_pmem_mb += config.pmem_size_mb;
                
                printf("   📊 Node resources: %d/%d vCPUs, %lu/%lu MB RAM\n",
                       node->allocated_vcpus, node->logical_cores,
                       node->allocated_ram_mb, node->total_ram_mb);
            }
        }
    }
    
    printf("\n✅ Cluster deployment complete: %d instances across %d nodes\n",
           total_instances, node_count);
    
    return total_instances;
}
```

```plaintext
// src/deploy/cloudinit_generator.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aerosls/deploy/qemu_platform.h"

/* Generate cloud-init configuration for AeroSLS instance */
int aerosls_generate_cloudinit(aerosls_qemu_config_t *config) {
    
    printf("📝 Generating cloud-init for %s\n", config->instance_name);
    
    /* Create cloud-init directory */
    char ci_dir[256];
    snprintf(ci_dir, sizeof(ci_dir), "/var/lib/aerosls/cloudinit/%s",
             config->instance_id);
    mkdir(ci_dir, 0755);
    
    /* Generate meta-data */
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/meta-data", ci_dir);
    
    FILE *meta = fopen(meta_path, "w");
    if (meta) {
        fprintf(meta, "instance-id: %s\n", config->instance_id);
        fprintf(meta, "local-hostname: %s\n", config->instance_name);
        fprintf(meta, "cluster: %s\n", config->cluster_name);
        fprintf(meta, "role: %s\n", config->node_role);
        fclose(meta);
    }
    
    /* Generate user-data (AeroSLS auto-configuration) */
    char user_path[256];
    snprintf(user_path, sizeof(user_path), "%s/user-data", ci_dir);
    
    FILE *user = fopen(user_path, "w");
    if (user) {
        fprintf(user, "#cloud-config\n");
        fprintf(user, "hostname: %s\n", config->instance_name);
        fprintf(user, "fqdn: %s.local\n", config->instance_name);
        fprintf(user, "\n");
        fprintf(user, "# AeroSLS auto-configuration\n");
        fprintf(user, "write_files:\n");
        fprintf(user, "  - path: /etc/aerosls/config.yaml\n");
        fprintf(user, "    permissions: '0644'\n");
        fprintf(user, "    content: |\n");
        fprintf(user, "      aerosls:\n");
        fprintf(user, "        instance_id: %s\n", config->instance_id);
        fprintf(user, "        instance_name: %s\n", config->instance_name);
        fprintf(user, "        cluster: %s\n", config->cluster_name);
        fprintf(user, "        role: %s\n", config->node_role);
        fprintf(user, "        sls:\n");
        fprintf(user, "          enabled: %s\n", 
                config->enable_sls ? "true" : "false");
        fprintf(user, "          pmem_device: /dev/pmem0\n");
        fprintf(user, "          memory_size: %lu\n", 
                config->pmem_size_mb * 1024 * 1024);
        fprintf(user, "        execution:\n");
        fprintf(user, "          max_contexts: %lu\n", config->max_contexts);
        fprintf(user, "          auto_checkpoint: true\n");
        fprintf(user, "          auto_recover: true\n");
        fprintf(user, "        mesh:\n");
        fprintf(user, "          enabled: %s\n",
                config->enable_mesh ? "true" : "false");
        fprintf(user, "          cluster_name: %s\n", config->cluster_name);
        fprintf(user, "          auto_discover: true\n");
        fprintf(user, "\n");
        fprintf(user, "runcmd:\n");
        fprintf(user, "  - [ systemctl, enable, aerosls ]\n");
        fprintf(user, "  - [ systemctl, start, aerosls ]\n");
        fprintf(user, "  - [ aeroslsctl, join, --cluster, %s, --role, %s ]\n",
                config->cluster_name, config->node_role);
        fclose(user);
    }
    
    /* Generate network config */
    char net_path[256];
    snprintf(net_path, sizeof(net_path), "%s/network-config", ci_dir);
    
    FILE *net = fopen(net_path, "w");
    if (net) {
        fprintf(net, "version: 2\n");
        fprintf(net, "ethernets:\n");
        fprintf(net, "  eth0:\n");
        fprintf(net, "    match:\n");
        fprintf(net, "      macaddress: %s\n", config->mac_address);
        fprintf(net, "    dhcp4: true\n");
        fclose(net);
    }
    
    /* Create ISO image */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "genisoimage -output %s/%s.iso "
        "-volid cidata -joliet -rock %s",
        ci_dir, config->instance_id, ci_dir);
    
    system(cmd);
    
    /* Copy to cloudinit image path */
    snprintf(cmd, sizeof(cmd),
        "cp %s/%s.iso /var/lib/aerosls/cloudinit/%s.img",
        ci_dir, config->instance_id, config->instance_id);
    
    system(cmd);
    
    printf("✅ Cloud-init generated for %s\n", config->instance_name);
    
    return 0;
}
```

```bash
#!/bin/bash
# scripts/aerosls-deploy.sh
# One-command deployment script for AeroSLS on new servers

set -e

echo "╔══════════════════════════════════════════════════════╗"
echo "║   AeroSLS Server Deployment Script                   ║"
echo "║   Makes AeroSLS the default environment              ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# Configuration
AEROSLS_VERSION="1.0.0"
PMEM_SIZE_GB=512
INSTANCES=4
CLUSTER_NAME="production"

# Detect hardware
TOTAL_RAM_GB=$(free -g | awk '/^Mem:/{print $2}')
TOTAL_CPUS=$(nproc)
echo "📊 Hardware detected:"
echo "   CPUs: $TOTAL_CPUS"
echo "   RAM: ${TOTAL_RAM_GB}GB"
echo ""

# Check for persistent memory
PMEM_DEVICE=""
if [ -e /dev/pmem0 ]; then
    PMEM_DEVICE="/dev/pmem0"
    echo "✅ Persistent Memory detected: $PMEM_DEVICE"
elif [ -e /dev/dax0.0 ]; then
    PMEM_DEVICE="/dev/dax0.0"
    echo "✅ DAX device detected: $PMEM_DEVICE"
else
    echo "⚠️  No persistent memory detected - using RAM-based SLS"
    PMEM_DEVICE="/dev/shm/aerosls-pmem"
    dd if=/dev/zero of=$PMEM_DEVICE bs=1G count=$PMEM_SIZE_GB 2>/dev/null
    echo "   Created RAM-based PMEM: ${PMEM_SIZE_GB}GB"
fi

# Install dependencies
echo ""
echo "📦 Installing dependencies..."
apt-get update -qq
apt-get install -y -qq \
    qemu-system-x86 \
    qemu-utils \
    libvirt-daemon-system \
    virtinst \
    cloud-image-utils \
    genisoimage \
    bridge-utils \
    hugepages \
    numactl

# Setup huge pages
echo ""
echo "📊 Configuring huge pages..."
HP_SIZE=$(echo "$TOTAL_RAM_GB * 0.8 * 1024 / 2" | bc)
echo "   Allocating ${HP_SIZE} huge pages (2MB each)"
echo $HP_SIZE > /proc/sys/vm/nr_hugepages
mkdir -p /dev/hugepages
mount -t hugetlbfs hugetlbfs /dev/hugepages || true

# Create network bridge
echo ""
echo "🌐 Creating AeroSLS network bridge..."
cat > /etc/netplan/01-aerosls-net.yaml << EOF
network:
  version: 2
  renderer: networkd
  bridges:
    aerosls-br0:
      interfaces: []
      addresses: [10.0.0.1/24]
      dhcp4: true
EOF
netplan apply 2>/dev/null || true

# Create AeroSLS directories
echo ""
echo "📁 Creating AeroSLS directories..."
mkdir -p /var/lib/aerosls/{images,instances,cloudinit,logs}
mkdir -p /var/run/aerosls
mkdir -p /etc/aerosls

# Download/Create AeroSLS boot image
echo ""
echo "💿 Preparing AeroSLS boot image..."
if [ ! -f /var/lib/aerosls/images/aerosls-base.qcow2 ]; then
    echo "   Creating base image..."
    qemu-img create -f qcow2 \
        /var/lib/aerosls/images/aerosls-base.qcow2 20G
    
    # Install AeroSLS in the image
    virt-install \
        --name aerosls-base \
        --ram 4096 \
        --vcpus 4 \
        --disk /var/lib/aerosls/images/aerosls-base.qcow2 \
        --os-variant ubuntu22.04 \
        --network bridge:aerosls-br0 \
        --graphics none \
        --console pty,target_type=serial \
        --location 'http://archive.ubuntu.com/ubuntu/dists/jammy/main/installer-amd64/' \
        --extra-args 'console=ttyS0,115200n8 serial' \
        --initrd-inject=/etc/aerosls/preseed.cfg \
        --noreboot \
        2>/dev/null || true
fi

# Generate AeroSLS systemd service
echo ""
echo "⚙️  Creating AeroSLS systemd service..."
cat > /etc/systemd/system/aerosls-node.service << 'EOF'
[Unit]
Description=AeroSLS Node Manager
After=network.target libvirtd.service
Before=aerosls-instances@.service

[Service]
Type=forking
ExecStart=/usr/local/bin/aerosls-node --start
ExecStop=/usr/local/bin/aerosls-node --stop
Restart=always
RestartSec=10
User=root
Group=root

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/aerosls-instance@.service << 'EOF'
[Unit]
Description=AeroSLS Instance %i
After=aerosls-node.service
Requires=aerosls-node.service

[Service]
Type=forking
ExecStart=/usr/local/bin/aerosls-instance --start %i
ExecStop=/usr/local/bin/aerosls-instance --stop %i
PIDFile=/var/run/aerosls/%i.pid
Restart=on-failure
RestartSec=30
User=root
Group=root

[Install]
WantedBy=multi-user.target
EOF

# Enable services
systemctl daemon-reload
systemctl enable aerosls-node.service

# Create default configuration
echo ""
echo "⚙️  Creating default AeroSLS configuration..."
cat > /etc/aerosls/node.yaml << EOF
aerosls:
  version: "${AEROSLS_VERSION}"
  cluster: "${CLUSTER_NAME}"
  
  node:
    instances: ${INSTANCES}
    pmem_device: "${PMEM_DEVICE}"
    
  qemu:
    machine_type: "aerosls"
    vcpus_per_instance: $((TOTAL_CPUS / INSTANCES))
    ram_per_instance_mb: $((TOTAL_RAM_GB * 1024 / INSTANCES))
    
  features:
    sls_enabled: true
    mesh_enabled: true
    auto_recovery: true
    huge_pages: true
    cpu_pinning: true

  network:
    bridge: "aerosls-br0"
    
  logging:
    level: "info"
    output: "/var/log/aerosls/node.log"
EOF

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║   AeroSLS Deployment Complete!                        ║"
echo "║                                                       ║"
echo "║   Start AeroSLS:                                      ║"
echo "║     systemctl start aerosls-node                      ║"
echo "║                                                       ║"
echo "║   Enable at boot:                                     ║"
echo "║     systemctl enable aerosls-node                     ║"
echo "║                                                       ║"
echo "║   Check status:                                       ║"
echo "║     aeroslsctl status                                 ║"
echo "║     aeroslsctl instances list                         ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# Start AeroSLS if requested
if [ "$1" = "--start" ]; then
    echo "🚀 Starting AeroSLS..."
    systemctl start aerosls-node
fi
```

## **Key Points of the QEMU-Based Architecture:**

1. **Default Environment**: Every new server runs AeroSLS automatically via systemd
2. **Multiple Instances per Server**: Better isolation through QEMU virtualization
3. **Persistent Memory Passthrough**: NVDIMM/CXL devices passed directly to guests
4. **Cloud-Init Automation**: Instances auto-configure on first boot
5. **Resource Optimization**: Automatic CPU pinning, huge pages, NUMA binding
6. **Cluster-Ready**: Instances automatically join the AeroSLS cluster
7. **One-Command Deployment**: Single script sets up everything

#### The QEMU approach provides:

- **Hardware isolation** between instances
- **Live migration** between physical servers
- **Snapshot/restore** of entire instances
- **Resource limits** per instance
- **Security boundaries** through virtualization

This makes AeroSLS the natural default environment on any new server, similar to how Kubernetes nodes run containers, but with persistent memory and execution contexts instead.
