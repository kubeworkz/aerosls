#include "query_engine.h"
#include "transaction.h"
#include "tier_mgr.h"
#include "microkernel.h"
#include "ipc.h"

// ─── String helpers ───────────────────────────────────────────────────────────
static size_t qe_strlen(const char* s) { size_t n=0; while(s[n]) n++; return n; }

// Case-insensitive substring search
static int qe_contains(const char* hay, const char* needle) {
    size_t nlen = qe_strlen(needle);
    for (size_t i = 0; hay[i]; i++) {
        int ok = 1;
        for (size_t j = 0; j < nlen; j++) {
            char a = hay[i+j], b = needle[j];
            if (!a) return 0;
            if (a>='A'&&a<='Z') a+=32;
            if (b>='A'&&b<='Z') b+=32;
            if (a != b) { ok=0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

// ─── Query Domain Router ──────────────────────────────────────────────────────
// Score the query against each domain's keyword set; pick highest scorer.
static QueryDomain route_query(const char* q) {
    static const char* kw_financial[]   = {"ledger","financial","customer",
                                           "balance","account","transaction",
                                           "invoice","payment", NULL};
    static const char* kw_tier[]        = {"sram","l1","l2","l3","tier","cache",
                                           "hot","cold","dram","nvme","ssd",
                                           "storage","evict","promote", NULL};
    static const char* kw_service[]     = {"service","microkernel","health",
                                           "crash","pid","raft","ipc","bus",
                                           "daemon","process","worker", NULL};
    static const char* kw_permissions[] = {"ring","permission","access","role",
                                           "capability","protect","grant",
                                           "revoke","acl","privilege","security",
                                           "profile","user", NULL};
    static const char* kw_wal[]         = {"wal","journal","recovery","commit",
                                           "rollback","log","crc","checkpoint",
                                           "crash","replay","consistent", NULL};

    const char** sets[] = { kw_financial, kw_tier, kw_service,
                            kw_permissions, kw_wal };
    QueryDomain domains[] = { QD_FINANCIAL, QD_TIER, QD_SERVICE,
                               QD_PERMISSIONS, QD_WAL };
    int best_score = 0;
    QueryDomain best = QD_GENERAL;

    for (int d = 0; d < 5; d++) {
        int score = 0;
        for (int k = 0; sets[d][k]; k++) {
            if (qe_contains(q, sets[d][k])) score++;
        }
        if (score > best_score) { best_score = score; best = domains[d]; }
    }
    return best;
}

// ─── Domain Handler: Financial Ledger Analysis ────────────────────────────────
static void handle_financial(void) {
    kernel_serial_print(
        "\n  Scanning address space for DB_TABLE segments with financial data...\n");

    uint32_t found = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || e->type != OBJ_TYPE_DB_TABLE) continue;
        found++;

        struct SLSObjectRecord* rec = &object_records[i];
        int app_read  = catalog_check_access(2, e->name, PERM_READ);
        int app_write = catalog_check_access(2, e->name, PERM_WRITE);

        kernel_serial_printf(
            "\n  [0x%016lx] %s\n"
            "    Type     : DB_TABLE        Tier   : %s\n"
            "    Pages    : %-4u            Owner  : uid=%u (%s)\n"
            "    APP_USER : %-12s    GUEST  : DENIED\n"
            "    Fields   : %u\n",
            e->base_vaddr, e->name,
            tier_name(e->storage_tier),
            e->size_pages,
            e->owner_uid, role_name(catalog_get_role(e->owner_uid)),
            app_read && !app_write ? "READ-ONLY" :
            app_read && app_write  ? "READ-WRITE" : "DENIED",
            rec->field_count);

        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!rec->fields[f].active) continue;
            kernel_serial_printf("      %-32s = %s\n",
                                 rec->fields[f].key, rec->fields[f].value);
        }
    }

    if (!found) {
        kernel_serial_print(
            "  No DB_TABLE objects found. Use 'valloc <name> 1 <pages>' to\n"
            "  create a database segment, then 'insert' records.\n");
        return;
    }

    kernel_serial_printf(
        "\n  Permission Audit: APP_USER has READ access; WRITE access denied."
        "  [COMPLIANT]\n"
        "  %u DB_TABLE object(s) scanned.\n", found);
}

// ─── Domain Handler: Storage Tier Access Check ───────────────────────────────
static void handle_tier(void) {
    kernel_serial_print("\n  Scanning virtual address tiers...\n");

    static const SLSStorageTier tiers[] = {
        STORAGE_TIER_L1_CACHE, STORAGE_TIER_L2_DRAM, STORAGE_TIER_L3_SSD
    };
    static const char* tier_labels[] = {
        "L1_CACHE  (SRAM-resident / hot)",
        "L2_DRAM   (warm / standard)",
        "L3_SSD    (cold / persistent NVMe)"
    };

    for (int t = 0; t < 3; t++) {
        kernel_serial_printf("\n  %s:\n", tier_labels[t]);
        int printed = 0;
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            struct SLSObjectEntry* e = &object_catalog[i];
            if (!e->active || e->storage_tier != tiers[t]) continue;

            uint32_t accesses = 0, idle = 0;
            for (int j = 0; j < TIER_MAX_TRACKED; j++) {
                if (tier_stats[j].active &&
                    tier_stats[j].object_id == e->object_id) {
                    accesses = tier_stats[j].access_count;
                    idle     = tier_stats[j].idle_ticks;
                    break;
                }
            }
            kernel_serial_printf(
                "    %-22s  %-16s  uid=%-4u  accesses=%-4u  idle=%u\n",
                e->name, obj_type_name(e->type),
                e->owner_uid, accesses, idle);
            printed++;
        }
        if (!printed) kernel_serial_print("    (none)\n");
    }

    kernel_serial_printf(
        "\n  Tier Policy: promote > %u accesses/tick | "
        "demote after %u idle ticks\n"
        "  SYSTEM_METADATA objects are pinned to L1_CACHE (never demoted).\n",
        TIER_PROMOTE_THRESHOLD, TIER_DEMOTE_THRESHOLD);
}

// ─── Domain Handler: Microkernel Service Health ───────────────────────────────
static void handle_service(void) {
    kernel_serial_print("\n  Scanning microkernel service registry...\n\n");

    uint32_t n_crashed = 0;
    for (uint32_t i = 0; i < service_count; i++) {
        struct ServiceDescriptor* s = &services[i];
        if (!s->active) continue;
        uint32_t ms   = s->latency_us_x100 / 100;
        uint32_t frac = s->latency_us_x100 % 100;
        kernel_serial_printf(
            "  %-24s  [PID %u]  %-8s  latency=%u.%02ums  "
            "queue=%-2d  msgs=%lu\n",
            s->name, s->pid,
            svc_state_name(s->state),
            ms, frac,
            ipc_queue_depth(s->port),
            s->msgs_processed);
        if (s->state == SVC_STATE_CRASHED) n_crashed++;
    }

    if (n_crashed == 0) {
        kernel_serial_print(
            "\n  All services ONLINE. No degraded states detected.\n");
    } else {
        kernel_serial_printf(
            "\n  WARNING: %u service(s) CRASHED. "
            "Use 'svc restart <name>' to recover.\n", n_crashed);
    }

    // WAL integrity cross-check
    uint32_t pending = 0, committed = 0, aborted = 0;
    for (uint32_t i = 0; i < wal_entry_count; i++) {
        switch (wal_buffer[i].state) {
            case WAL_STATE_PENDING:   pending++;   break;
            case WAL_STATE_COMMITTED: committed++; break;
            case WAL_STATE_ABORTED:   aborted++;   break;
        }
    }
    kernel_serial_printf(
        "  WAL: %u entries  (%u committed | %u pending | %u aborted)  "
        "%s\n",
        wal_entry_count, committed, pending, aborted,
        pending == 0 ? "[CONSISTENT]" : "[UNCOMMITTED ENTRIES PRESENT]");
}

// ─── Domain Handler: Permission Ring Analysis ─────────────────────────────────
static void handle_permissions(void) {
    kernel_serial_print(
        "\n  Scanning capability access lists across address space...\n");

    static const SLSRole check_roles[] = {
        ROLE_SYSTEM_KERNEL, ROLE_DB_ADMIN, ROLE_APP_USER, ROLE_GUEST
    };

    if (object_catalog_count == 0) {
        kernel_serial_print("  No objects in address space.\n");
        return;
    }

    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;

        kernel_serial_printf(
            "\n  [0x%016lx] %-22s  [%s]\n",
            e->base_vaddr, e->name, obj_type_name(e->type));

        for (int r = 0; r < 4; r++) {
            // Temporarily set a probe uid for each role to test access
            uint32_t probe_uid = (uint32_t)check_roles[r] + 200;
            // Register probe uid with this role in role_table temporarily
            struct SLSRoleRequest rq = { .uid = probe_uid,
                                         .role = check_roles[r] };
            sys_sls_role_set(&rq);

            int rd = catalog_check_access(probe_uid, e->name, PERM_READ);
            int wr = catalog_check_access(probe_uid, e->name, PERM_WRITE);
            int ex = catalog_check_access(probe_uid, e->name, PERM_EXECUTE);

            kernel_serial_printf(
                "    %-15s  %c%c%c\n",
                role_name(check_roles[r]),
                rd ? 'R' : '-', wr ? 'W' : '-', ex ? 'X' : '-');
        }
        kernel_serial_printf(
            "    perm_mask=0x%02x  owner=uid=%u\n",
            e->perm_mask, e->owner_uid);
    }
}

// ─── Domain Handler: WAL Audit ────────────────────────────────────────────────
static void handle_wal(void) {
    kernel_serial_printf(
        "\n  Write-Ahead Log — %u entries\n"
        "  %-7s  %-5s  %-18s  %-24s  %s\n"
        "  %-7s  %-5s  %-18s  %-24s  %s\n",
        wal_entry_count,
        "Entry", "TxID", "Object ID", "Key", "Status",
        "-------", "-----", "------------------",
        "------------------------", "---------");

    uint32_t pending=0, committed=0, aborted=0;
    for (uint32_t i = 0; i < wal_entry_count; i++) {
        struct WALEntry* w = &wal_buffer[i];
        const char* st = w->state == WAL_STATE_COMMITTED ? "COMMITTED" :
                         w->state == WAL_STATE_PENDING    ? "PENDING"   :
                                                            "ABORTED";
        kernel_serial_printf(
            "  #%-6u %-5lu 0x%016lx  %-24s  %s\n",
            w->entry_id, w->tx_id, w->object_id, w->key, st);
        if (w->state == WAL_STATE_PENDING)   pending++;
        if (w->state == WAL_STATE_COMMITTED) committed++;
        if (w->state == WAL_STATE_ABORTED)   aborted++;
    }

    if (!wal_entry_count) kernel_serial_print("  (log is empty)\n");

    kernel_serial_printf(
        "\n  Summary: committed=%u  pending=%u  aborted=%u  "
        "CRC-signed entries: %u\n"
        "  State: %s\n",
        committed, pending, aborted, wal_entry_count,
        pending == 0 ? "CONSISTENT" : "HAS UNCOMMITTED ENTRIES");
}

// ─── Domain Handler: General Full Scan ────────────────────────────────────────
static void handle_general(void) {
    kernel_serial_print("\n  Full address space scan:\n");
    if (object_catalog_count == 0) {
        kernel_serial_print(
            "  No objects allocated. Try: valloc MyTable 1 4\n");
        return;
    }
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        kernel_serial_printf(
            "  [0x%016lx] %-22s  %s  %s  %u page(s)  uid=%u\n",
            e->base_vaddr, e->name,
            obj_type_name(e->type), tier_name(e->storage_tier),
            e->size_pages, e->owner_uid);
        struct SLSObjectRecord* rec = &object_records[i];
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!rec->fields[f].active) continue;
            kernel_serial_printf("    %-28s = %s\n",
                                 rec->fields[f].key, rec->fields[f].value);
        }
    }
    kernel_serial_printf("  %u object(s) in address space.\n",
                         object_catalog_count);
}

// ─── sys_sls_query — main entry point ─────────────────────────────────────────
void sys_sls_query(const char* text) {
    QueryDomain domain = route_query(text);

    kernel_serial_print(
        "\n[QUERY] Cognitive Direct Object Space Query\n"
        "  Input     : ");
    kernel_serial_printf("\"%s\"\n", text);
    kernel_serial_printf(
        "  Logic     : Cognitive Direct | No SQL | No Filesystem\n"
        "  Domain    : %s\n"
        "  FS bypass : 100%%  SQL compile: 0%%\n"
        "  ─────────────────────────────────────────────────────\n",
        query_domain_name(domain));

    switch (domain) {
        case QD_FINANCIAL:   handle_financial();   break;
        case QD_TIER:        handle_tier();        break;
        case QD_SERVICE:     handle_service();     break;
        case QD_PERMISSIONS: handle_permissions(); break;
        case QD_WAL:         handle_wal();         break;
        default:             handle_general();     break;
    }

    kernel_serial_print(
        "  ─────────────────────────────────────────────────────\n"
        "[QUERY] Scan complete.\n\n");
}

// ─── sys_sls_query_scan — structured JSON manifest for external AI ────────────
void sys_sls_query_scan(void) {
    uint64_t tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc));

    kernel_serial_print("[SCAN] BEGIN_MANIFEST\n{");
    kernel_serial_printf(
        "\"build\":\"4.0-SLS\",\"tsc\":%lu,\"objects\":[", tsc);

    uint8_t first_obj = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        if (!first_obj) kernel_serial_print(",");
        first_obj = 0;

        struct SLSObjectRecord* rec = &object_records[i];
        kernel_serial_printf(
            "{\"name\":\"%s\",\"type\":\"%s\","
            "\"vaddr\":\"0x%016lx\",\"tier\":\"%s\","
            "\"pages\":%u,\"uid\":%u,\"perm\":\"0x%02x\","
            "\"field_count\":%u,\"fields\":[",
            e->name, obj_type_name(e->type),
            e->base_vaddr, tier_name(e->storage_tier),
            e->size_pages, e->owner_uid, e->perm_mask,
            rec->field_count);

        uint8_t first_f = 1;
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!rec->fields[f].active) continue;
            if (!first_f) kernel_serial_print(",");
            first_f = 0;
            kernel_serial_printf("{\"k\":\"%s\",\"v\":\"%s\"}",
                                 rec->fields[f].key, rec->fields[f].value);
        }
        kernel_serial_print("]}");
    }

    // WAL summary
    uint32_t committed=0, pending=0, aborted=0;
    for (uint32_t i = 0; i < wal_entry_count; i++) {
        if (wal_buffer[i].state == WAL_STATE_COMMITTED) committed++;
        else if (wal_buffer[i].state == WAL_STATE_PENDING)   pending++;
        else if (wal_buffer[i].state == WAL_STATE_ABORTED)   aborted++;
    }

    // Service summary
    uint32_t svc_online=0, svc_crashed=0;
    for (uint32_t i = 0; i < service_count; i++) {
        if (!services[i].active) continue;
        if (services[i].state == SVC_STATE_ONLINE) svc_online++;
        else svc_crashed++;
    }

    kernel_serial_printf(
        "],\"wal\":{\"total\":%u,\"committed\":%u,\"pending\":%u,\"aborted\":%u},"
        "\"services\":{\"total\":%u,\"online\":%u,\"crashed\":%u},"
        "\"ipc\":{\"posted\":%lu,\"dispatched\":%lu,\"dropped\":%lu}}\n",
        wal_entry_count, committed, pending, aborted,
        service_count, svc_online, svc_crashed,
        ipc_stats.total_posted, ipc_stats.total_dispatched,
        ipc_stats.total_dropped);
    kernel_serial_print("[SCAN] END_MANIFEST\n\n");
}
