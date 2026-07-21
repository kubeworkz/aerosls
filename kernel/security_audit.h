#ifndef SECURITY_AUDIT_H
#define SECURITY_AUDIT_H

#include <stdint.h>

/*
 * security_audit.h — Navigator-Parity Gap Roadmap Phase 3: security audit
 * log. Deliberately a leaf module (only <stdint.h>) so both auth.c and
 * object_catalog.c can include it without adding either one to the other's
 * dependency graph -- the same "bare extern instead of a #include" avoidance
 * of circular headers this codebase already uses elsewhere (see
 * object_catalog.c's own comment on its vecstore_notify_object_freed()
 * forward declaration) is available here too, but a real leaf header is
 * simpler and there's no circularity risk since this file depends on nothing
 * catalog- or auth-specific.
 *
 * Records three kinds of real security-relevant events, per the roadmap's
 * own scope: auth failures (auth.c: an invalid or expired bearer token),
 * role changes (object_catalog.c: sys_sls_role_set()), and access denials
 * (object_catalog.c: catalog_check_access() returning 0). This is what
 * SlsSecurityDashboard.tsx's "Security Event Log" panel should actually read
 * from once it exists -- see this codebase's Navigator-Parity roadmap doc,
 * Phase 3 -- closing the loop Phase 1 could only partially close (Phase 1
 * found SlsSecurityDashboard.tsx to be an intentional, honestly-labeled
 * simulator with no real backend to wire into at the time; this is that
 * backend, now that it exists).
 *
 * Sizing/overflow posture: a flat bump-allocated array, same convention as
 * kernel/transaction.c's wal_buffer[WAL_MAX_ENTRIES] -- once AUDIT_LOG_MAX
 * entries have been logged, further calls to security_audit_log() are
 * silently dropped (the entry is lost, not overwritten) rather than wrapping
 * a ring buffer. This matches this codebase's own established precedent for
 * fixed-size registries under pressure (row_constraint.h's ROW_CONSTRAINT_MAX,
 * auth.c's own au_token_expired() comment on why expired token slots aren't
 * reclaimed) rather than inventing a new wraparound convention for this one
 * case. A real ring buffer (or periodic archival to NVMe) is a future
 * improvement, named here rather than built in this pass -- 256 entries is
 * enough for a demo/simulated deployment's boot-cycle audit trail, matching
 * the same "smallest real version first" judgment this whole roadmap has
 * used throughout.
 */

#define AUDIT_LOG_MAX     256
#define AUDIT_ACTION_LEN  24
#define AUDIT_DETAIL_LEN  96

struct SLSAuditEntry {
    uint64_t id;                       // monotonically increasing, never reused
    uint64_t tick;                     // kernel_tick_counter at time of logging
    uint32_t uid;                      // 0 for events with no resolved identity (e.g. a bad token)
    char     action[AUDIT_ACTION_LEN]; // "AUTH_FAIL" | "ROLE_CHANGE" | "ACCESS_DENIED"
    char     detail[AUDIT_DETAIL_LEN]; // free-form context: object name, email, role, etc.
    uint8_t  granted;                  // 0 = denial/failure, 1 = change succeeded (role changes only)
};

extern struct SLSAuditEntry security_audit_log_buf[AUDIT_LOG_MAX];
extern uint32_t             security_audit_log_count;   // number of entries currently populated (<= AUDIT_LOG_MAX)
extern uint64_t             security_audit_next_id;     // next id to assign; also a cumulative "how many ever" counter

// Appends one entry. `action`/`detail` are copied and truncated to fit (NUL-
// terminated); a NULL `detail` is treated as empty. No-ops once
// security_audit_log_count reaches AUDIT_LOG_MAX (see this header's own
// comment on the bump-allocated, no-reclaim posture).
void security_audit_log(uint32_t uid, const char* action, const char* detail, int granted);

// Prints the audit log to the serial port, mirrors sys_sls_auth_list()'s own style.
void sys_sls_audit_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// 243 -- after group_profile.h's 237-239 and authlist.h's 240-242.
#define SYS_SLS_AUDIT_LIST 243

#endif /* SECURITY_AUDIT_H */
