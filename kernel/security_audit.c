#include "security_audit.h"
#include "kernel_io.h"
#include "timer.h"   // kernel_tick_counter

// ─── Globals ────────────────────────────────────────────────────────────────
struct SLSAuditEntry security_audit_log_buf[AUDIT_LOG_MAX];
uint32_t             security_audit_log_count = 0;
uint64_t             security_audit_next_id    = 1;

static void sa_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    if (src) { for (; i + 1 < n && src[i]; i++) dst[i] = src[i]; }
    dst[i] = '\0';
}

void security_audit_log(uint32_t uid, const char* action, const char* detail, int granted) {
    if (security_audit_log_count >= AUDIT_LOG_MAX) return;   // bump-allocated, no reclaim -- see header comment

    struct SLSAuditEntry* e = &security_audit_log_buf[security_audit_log_count];
    e->id      = security_audit_next_id++;
    e->tick    = kernel_tick_counter;
    e->uid     = uid;
    e->granted = (uint8_t)(granted ? 1 : 0);
    sa_strncpy(e->action, action, AUDIT_ACTION_LEN);
    sa_strncpy(e->detail, detail, AUDIT_DETAIL_LEN);

    security_audit_log_count++;

    kernel_serial_printf("[AUDIT] #%llu uid=%u action=%s detail=%s granted=%d\n",
                         (unsigned long long)e->id, uid, e->action, e->detail, granted);
}

void sys_sls_audit_list(void) {
    kernel_serial_printf("\n[AUDIT] Security Audit Log (%u of %d capacity)\n",
                         security_audit_log_count, AUDIT_LOG_MAX);
    for (uint32_t i = 0; i < security_audit_log_count; i++) {
        struct SLSAuditEntry* e = &security_audit_log_buf[i];
        kernel_serial_printf(" #%llu tick=%llu uid=%u %s: %s [%s]\n",
                             (unsigned long long)e->id, (unsigned long long)e->tick,
                             e->uid, e->action, e->detail,
                             e->granted ? "OK" : "DENIED");
    }
    if (!security_audit_log_count) kernel_serial_print(" (no audit events logged)\n");
    kernel_serial_print("\n");
}
