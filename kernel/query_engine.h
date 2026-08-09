#ifndef QUERY_ENGINE_H
#define QUERY_ENGINE_H

#include <stdint.h>
#include "object_catalog.h"

/*
 * Cognitive Direct Query Engine — Phase 7
 *
 * In a Single-Level Storage OS there is no filesystem to traverse and no SQL
 * layer to compile.  The query engine scans the live in-memory object catalog,
 * WAL journal, service registry, and tier statistics directly, routing each
 * natural-language query to the most relevant domain handler via keyword
 * scoring.  This approximates the "Cognitive Direct" query logic shown in
 * the simulator's Deep Thinking Query tab.
 */

// ─── Syscall Numbers ──────────────────────────────────────────────────────────
#define SYS_SLS_QUERY      150   // run a natural-language query against the catalog
#define SYS_SLS_QUERY_SCAN 151   // dump the full catalog as a structured JSON manifest

// ─── Query Domain Tags ────────────────────────────────────────────────────────
// Returned by the router so the shell can show which handler fired.
typedef enum {
    QD_FINANCIAL   = 0,  // ledger / customer / balance / transaction
    QD_TIER        = 1,  // sram / l1 / cache / hot / tier
    QD_SERVICE     = 2,  // service / microkernel / health / crash / pid
    QD_PERMISSIONS = 3,  // ring / permission / role / access / capability
    QD_WAL         = 4,  // wal / journal / recovery / commit / log
    QD_GENERAL     = 5,  // catch-all / "list all" / "show"
} QueryDomain;

static inline const char* query_domain_name(QueryDomain d) {
    switch (d) {
        case QD_FINANCIAL:   return "Financial Ledger Analysis";
        case QD_TIER:        return "Storage Tier Access Check";
        case QD_SERVICE:     return "Microkernel Service Health";
        case QD_PERMISSIONS: return "Object Protection Ring Analysis";
        case QD_WAL:         return "Write-Ahead Log Audit";
        case QD_GENERAL:     return "Full Address Space Scan";
        default:             return "Unknown";
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────
void sys_sls_query(const char* text);
void sys_sls_query_scan(void);   // structured JSON manifest to serial port

#endif /* QUERY_ENGINE_H */
