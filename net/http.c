#include "http.h"
#include "../kernel/entropy.h"
#include "tcp.h"
#include "../kernel/tls_server.h"
#include "../kernel/tls_store.h"
#include "net.h"
#include "http_rate_limit.h"   // Multitenant Isolation Gap Analysis §5 item 4 / §7 item 1
#include "tcp_quota.h"         // Multitenant Isolation Gap Analysis §5 item 4 / §7 item 1, Network Fairness Phase 2
#include "dhcp.h"   // Navigator-Parity Gap Roadmap Phase 5a -- dhcp_is_bound() for GET /api/network/status
#include "../kernel/kernel_io.h"
#include "../kernel/timer.h"
#include "../kernel/smp.h"       // smp_uniprocessor_tick() -- single-CPU fallback
#include "../user/shell.h"      // the console this loop drives between sweeps
#include "../kernel/net_event.h"  // Architectural Phase 1 -- net_event_hlt_wait() for the multiplexed HTTP loop
#include "../kernel/object_catalog.h"
#include "../kernel/transaction.h"
#include "../kernel/microkernel.h"
#include "../kernel/tier_mgr.h"      // also: Navigator-Parity Gap Roadmap Phase 5b -- tier_capacity_totals()
#include "../kernel/webapp.h"
#include "../kernel/bundle.h"
#include "../kernel/journal.h"
#include "../kernel/lock_mgr.h"
#include "../kernel/index_mgr.h"
#include "../kernel/constraint.h"
#include "../kernel/cursor.h"
#include "../kernel/aggregate.h"
#include "../kernel/mqt.h"
#include "../kernel/query_engine.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../kernel/auth.h"
#include "../kernel/loader.h"
#include "../kernel/stream.h"
#include "../user/permissions.h"
#include "../kernel/agent.h"
#include "../kernel/agent_tools.h"
#include "../kernel/rowstore.h"   // Gap Remediation Phase B -- POST /api/tables, GET /api/tables[/name/schema]
#include "../kernel/sql_exec.h"   // Gap Remediation Phase B -- POST /api/sql
#include "../kernel/vec_join.h"   // Gap Remediation Phase C -- POST /api/vec/join
#include "../kernel/vec_index.h"  // Gap Remediation Phase C -- POST /api/vec/indexes, /api/vec/index/search
#include "../kernel/partition.h"  // Gap Remediation Phase F -- partition create/list/destroy/assign/pause/resume
#include "../kernel/failover.h"     // Step 5 wired live -- liveness tick + leader checkpoint broadcast
#include "../kernel/frame_pool.h" // Gap Remediation Phase F -- GET /api/partition/quotas, POST /api/partition/quota
#include "../kernel/storage_quota.h" // Storage Isolation Roadmap Phase 1 -- GET /api/partition/storagequotas, POST /api/partition/storagequota
#include "../drivers/nvme_admin.h" // Navigator-Parity Gap Roadmap Phase 2 -- nvme_get_capacity_bytes()
#include "../kernel/security_audit.h" // Navigator-Parity Gap Roadmap Phase 3 -- GET /api/security/audit
#include "../kernel/group_profile.h"  // Navigator-Parity Gap Roadmap Phase 3 -- GET /api/security/groups
#include "../kernel/authlist.h"       // Navigator-Parity Gap Roadmap Phase 3 -- GET /api/security/authlists
#include "../kernel/database.h"       // Database Namespace & Access Roadmap Phase 4 -- GET /api/security/databases
#include "../kernel/tenant.h"
#include "../kernel/service_registry.h"
#include "../kernel/service_mesh.h"
#include "consensus.h"
/* Declared here rather than by including dspp.h: that header has no include
 * guard of its own (see its note on struct DSPPFullPagePacket), so pulling
 * it in alongside consensus.h double-defines every struct in it. A scalar
 * extern composes safely -- the same reason consensus.h forward-declares
 * struct DSPPFullPagePacket instead of including the header. */
extern uint64_t dspp_tx_oversize_dropped;   /* net/dspp.c */
#include "../kernel/workload.h"
#include "../kernel/workload_ctx.h"         // Multitenant Isolation Gap Analysis §5 item 1 -- GET/POST /api/tenants
#include "../kernel/usage_metering.h" // Multitenant Isolation Gap Analysis §5 item 6 -- GET /api/usage
#include "../kernel/msgqueue.h"       // Navigator-Parity Gap Roadmap Phase 4 -- GET /api/workmgmt/msgqueues
#include "../kernel/ipc.h"            // Shell-Command JSON-Promotion Roadmap -- IPCStats/IPCPostRequest/ipc_post()
#include "../kernel/checkpoint_mgr.h" // Core Backup Strategies Step 1 -- POST /api/checkpoint
#include "../kernel/secure_api.h"     // Shell-Command JSON-Promotion Roadmap -- struct SLSSealRequest

// ─── Simple JSON builder ──────────────────────────────────────────────────────
static void jb_putc(JSONBuf* j, char c) {
    if (j->pos < j->max - 1) j->buf[j->pos++] = c;
}

void jb_raw(JSONBuf* j, const char* s) {
    while (*s) jb_putc(j, *s++);
}

static void jb_esc_str(JSONBuf* j, const char* s) {
    jb_putc(j, '"');
    while (*s) {
        if (*s == '"' || *s == '\\') jb_putc(j, '\\');
        jb_putc(j, *s++);
    }
    jb_putc(j, '"');
}

static void jb_key(JSONBuf* j, const char* k) {
    jb_esc_str(j, k);
    jb_putc(j, ':');
}

void jb_str(JSONBuf* j, const char* key, const char* val) {
    jb_key(j, key);
    jb_esc_str(j, val);
}

// jb_esc_str() above escapes '"' and '\' but not control characters -- fine
// for every existing call site in this file (none of them ever carried
// embedded newlines), but not fine for shell command output (Kernel-Side
// Shell Refactor, docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10.4), which
// routinely spans multiple lines ("ls", "journal dump", "mqt list", ...) --
// a raw '\n' inside a JSON string literal is invalid JSON and would break
// JSON.parse() on the client. Deliberately a new, narrowly-used helper
// rather than changing jb_esc_str() itself: that function has ~40 existing
// call sites in this file, none of which need this, so fixing it in place
// would be unscoped risk for zero benefit.
static void jb_str_multiline(JSONBuf* j, const char* key, const char* val) {
    jb_key(j, key);
    jb_putc(j, '"');
    while (*val) {
        char c = *val++;
        if      (c == '"')  { jb_putc(j, '\\'); jb_putc(j, '"'); }
        else if (c == '\\') { jb_putc(j, '\\'); jb_putc(j, '\\'); }
        else if (c == '\n') { jb_putc(j, '\\'); jb_putc(j, 'n'); }
        else if (c == '\r') { jb_putc(j, '\\'); jb_putc(j, 'r'); }
        else if (c == '\t') { jb_putc(j, '\\'); jb_putc(j, 't'); }
        else jb_putc(j, c);
    }
    jb_putc(j, '"');
}

void jb_uint(JSONBuf* j, const char* key, uint64_t val) {
    jb_key(j, key);
    if (val == 0) { jb_putc(j, '0'); return; }
    char tmp[21]; int len = 0;
    while (val) { tmp[len++] = (char)('0' + val % 10); val /= 10; }
    for (int i = len-1; i >= 0; i--) jb_putc(j, tmp[i]);
}

// Navigator-Parity Gap Roadmap Phase 5a: dotted-decimal IPv4 formatter.
// IPv4Addr is stored in network byte order throughout net.h/dhcp.c (see
// dhcp.c's own boot-log print for the identical ntohl()-then-shift
// convention this mirrors) -- no dotted-IP formatter existed anywhere in
// this codebase before this route needed one.
static void jb_ip(JSONBuf* j, const char* key, IPv4Addr ip_net_order) {
    uint32_t ip = ntohl(ip_net_order);
    char buf[16]; int p = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint8_t octet = (uint8_t)((ip >> shift) & 0xFF);
        if (octet >= 100) { buf[p++] = (char)('0' + octet/100); octet %= 100; buf[p++] = (char)('0' + octet/10); octet %= 10; buf[p++] = (char)('0'+octet); }
        else if (octet >= 10) { buf[p++] = (char)('0' + octet/10); octet %= 10; buf[p++] = (char)('0'+octet); }
        else { buf[p++] = (char)('0'+octet); }
        if (shift) buf[p++] = '.';
    }
    buf[p] = '\0';
    jb_str(j, key, buf);
}

// Navigator-Parity Gap Roadmap Phase 5a: colon-separated MAC formatter.
static void jb_mac(JSONBuf* j, const char* key, MACAddr mac) {
    char buf[18]; int p = 0;
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        buf[p++] = hexd[(mac.b[i] >> 4) & 0xF];
        buf[p++] = hexd[mac.b[i] & 0xF];
        if (i < 5) buf[p++] = ':';
    }
    buf[p] = '\0';
    jb_str(j, key, buf);
}

static void jb_hex(JSONBuf* j, const char* key, uint64_t val) {
    jb_key(j, key);
    jb_putc(j, '"'); jb_raw(j, "0x");
    char tmp[17]; int len = 0;
    if (val == 0) { jb_putc(j, '0'); jb_putc(j, '"'); return; }
    while (val) {
        int d = (int)(val & 0xF);
        tmp[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val >>= 4;
    }
    for (int i = len-1; i >= 0; i--) jb_putc(j, tmp[i]);
    jb_putc(j, '"');
}

void jb_obj_open(JSONBuf* j, const char* key) {
    if (key) { jb_key(j, key); }
    jb_putc(j, '{');
}

void jb_obj_close(JSONBuf* j) { jb_putc(j, '}'); }

void jb_arr_open(JSONBuf* j, const char* key) {
    if (key) { jb_key(j, key); }
    jb_putc(j, '[');
}

void jb_arr_close(JSONBuf* j) { jb_putc(j, ']'); }

// ─── API Handlers ─────────────────────────────────────────────────────────────

static int api_scan(char* body, int max) {
    JSONBuf j = { body, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j, "build", "4.0-SLS"); jb_putc(&j, ',');
    /* Rows that exist in RAM but whose page never reached the disk. Non-zero
     * means data on this node will not survive a reboot, and there is no other
     * way to learn that: the row operations succeed, the rows read back
     * correctly, and only the serial console says otherwise. */
    jb_uint(&j, "rowstore_undurable_writes", rowstore_undurable_writes()); jb_putc(&j, ',');
    jb_uint(&j, "vecstore_undurable_writes", vecstore_undurable_writes()); jb_putc(&j, ',');
    jb_uint(&j, "persist_undurable_writes", persist_undurable_writes()); jb_putc(&j, ',');
    jb_uint(&j, "object_count", object_catalog_count); jb_putc(&j, ',');
    jb_arr_open(&j, "objects");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name); jb_putc(&j, ',');
        jb_str(&j, "type", obj_type_name(e->type)); jb_putc(&j, ',');
        jb_hex(&j, "vaddr", e->base_vaddr); jb_putc(&j, ',');
        jb_str(&j, "tier", tier_name(e->storage_tier)); jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages); jb_putc(&j, ',');
        jb_uint(&j, "uid", e->owner_uid); jb_putc(&j, ',');
        jb_uint(&j, "field_count", object_records[i].field_count); jb_putc(&j, ',');
        jb_arr_open(&j, "fields");
        int ff = 1;
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!object_records[i].fields[f].active) continue;
            if (!ff) jb_putc(&j, ','); ff = 0;
            jb_obj_open(&j, 0);
            jb_str(&j, "k", object_records[i].fields[f].key); jb_putc(&j, ',');
            jb_str(&j, "v", object_records[i].fields[f].value);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j);
    }
    jb_arr_close(&j); jb_putc(&j, ',');
    // WAL summary
    uint32_t committed=0, pending=0, aborted=0;
    for (uint32_t i = 0; i < wal_entry_count; i++) {
        if (wal_buffer[i].state == WAL_STATE_COMMITTED) committed++;
        else if (wal_buffer[i].state == WAL_STATE_PENDING)   pending++;
        else aborted++;
    }
    jb_obj_open(&j, "wal"); jb_putc(&j, '\0'); j.pos--;
    jb_uint(&j, "total", wal_entry_count); jb_putc(&j, ',');
    jb_uint(&j, "committed", committed); jb_putc(&j, ',');
    jb_uint(&j, "pending", pending); jb_putc(&j, ',');
    jb_uint(&j, "aborted", aborted);
    jb_obj_close(&j); jb_putc(&j, ',');
    // Service summary
    uint32_t svc_online=0, svc_crashed=0;
    for (uint32_t i = 0; i < service_count; i++) {
        if (!services[i].active) continue;
        if (services[i].state == SVC_STATE_ONLINE) svc_online++;
        else svc_crashed++;
    }
    jb_obj_open(&j, "services"); jb_putc(&j, '\0'); j.pos--;
    jb_uint(&j, "total", service_count); jb_putc(&j, ',');
    jb_uint(&j, "online", svc_online); jb_putc(&j, ',');
    jb_uint(&j, "crashed", svc_crashed);
    jb_obj_close(&j);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

/* The architecture this kernel was COMPILED for, taken from the compiler's own
 * target macros rather than a build variable or a literal.
 *
 * The distinction is the whole point. A -DARCH="x86-64" passed by the Makefile
 * says what the build system intended; __x86_64__ says what the compiler
 * actually targeted, and only the second one cannot drift. This file is in
 * X86_C_SRC alone today -- the arm64 and RISC-V kernels are minimal and carry
 * no HTTP server -- so a literal "x86-64" would be correct right now and would
 * become a lie, silently and on the wrong screen, the first time net/http.c
 * joins another target's source list.
 *
 * The #error is deliberate. A fourth architecture should fail to build here
 * rather than report "unknown" to a status bar whose entire job is to say what
 * it is running on. */
#if   defined(__x86_64__)
#  define SLS_ARCH_NAME "x86-64"
#elif defined(__aarch64__)
#  define SLS_ARCH_NAME "arm64"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define SLS_ARCH_NAME "riscv64"
#else
#  error "net/http.c: unrecognised target architecture -- add it to SLS_ARCH_NAME above rather than letting /api/health report a guess."
#endif

static int api_health(char* body, int max) {
    JSONBuf j = { body, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j,  "status",       "ok");                     jb_putc(&j, ',');
    jb_str(&j,  "system",       "AeroSLS 4.0");             jb_putc(&j, ',');
    jb_str(&j,  "arch",         SLS_ARCH_NAME);            jb_putc(&j, ',');
    jb_uint(&j, "uptime_ticks", kernel_tick_counter);       jb_putc(&j, ',');
    /* TLS pool accounting. Here rather than only on the serial console
     * because the number that matters -- peak bytes taken from the fixed pool
     * -- has to be readable from whatever machine is driving the browsers,
     * and /api/health is already the endpoint that answers without a token. */
    {
        unsigned long tls_refused = 0; unsigned tls_peak = 0, tls_live = 0;
        size_t pool_bytes = 0, pool_peak = 0, pool_blocks = 0;
        tls_server_stats(&tls_refused, &tls_peak, &tls_live, &pool_bytes, &pool_peak, &pool_blocks);
        jb_uint(&j, "tls_sessions_max", (uint64_t)TLS_SERVER_MAX_SESSIONS); jb_putc(&j, ',');
        jb_uint(&j, "tls_sessions_live", (uint64_t)tls_live);               jb_putc(&j, ',');
        jb_uint(&j, "tls_sessions_peak", (uint64_t)tls_peak);               jb_putc(&j, ',');
        jb_uint(&j, "tls_refused", (uint64_t)tls_refused);                  jb_putc(&j, ',');
        jb_uint(&j, "tls_pool_bytes", (uint64_t)pool_bytes);                jb_putc(&j, ',');
        jb_uint(&j, "tls_pool_peak", (uint64_t)pool_peak);                  jb_putc(&j, ',');
        jb_uint(&j, "tls_pool_blocks", (uint64_t)pool_blocks);              jb_putc(&j, ',');

        /* Whether this boot's CA came off disk or was made fresh. It is the
         * one fact that decides whether an operator's existing import is still
         * good, and the alternative to reporting it is asking them to compare
         * certificate serials across a reboot to find out. tls_ca_stored=0 on
         * a second boot means the import they already did is now worthless,
         * and this is where they see that without reading a serial console. */
        {
            int ca_loaded = 0; uint64_t ca_written = 0;
            tls_store_stats(&ca_loaded, &ca_written);
            jb_uint(&j, "tls_ca_stored",  (uint64_t)(ca_loaded ? 1 : 0));   jb_putc(&j, ',');
            jb_uint(&j, "tls_ca_written", (uint64_t)ca_written);            jb_putc(&j, ',');
        }

        /* Leaf renewal. tls_leaf_renewable is the one to read first: 0 means
         * nothing can replace this leaf when it expires, which is a fault an
         * operator has a year to notice and will only notice if something
         * shows it. tls_leaf_deferrals climbing without tls_leaf_renewals
         * moving means the node is never idle long enough to swap -- also
         * silent, also only visible here. */
        {
            uint64_t renew_at = 0; unsigned long renewals = 0, deferrals = 0;
            int renewable = 0;
            tls_server_renewal_status(&renew_at, &renewals, &deferrals, &renewable);
            jb_uint(&j, "tls_leaf_renew_at",  (uint64_t)renew_at);            jb_putc(&j, ',');
            jb_uint(&j, "tls_leaf_renewals",  (uint64_t)renewals);            jb_putc(&j, ',');
            jb_uint(&j, "tls_leaf_deferrals", (uint64_t)deferrals);           jb_putc(&j, ',');
            jb_uint(&j, "tls_leaf_renewable", (uint64_t)(renewable ? 1 : 0)); jb_putc(&j, ',');
        }
    }
    jb_uint(&j, "object_count", object_catalog_count);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

/* ─── GET /api/entropy ─────────────────────────────────────────────────────
 * Which entropy sources this node actually has, whether the DRBG is seeded,
 * and the boot fingerprint that tests/entropy_boot_diversity_check.sh compares
 * across nodes.
 *
 * Serving the fingerprint publicly is safe by construction and NOT by policy:
 * it is a one-way digest of 32 bytes that were destroyed immediately and are
 * used for nothing else (see kernel/entropy.h). Serving actual DRBG output
 * here to make the test easier would hand out part of the generator's stream,
 * which is a worse bug than the one the test looks for.
 *
 * Behind the same auth as every other /api route. "Safe to publish" is an
 * argument about what an attacker learns from the value, not a reason to skip
 * authentication -- the source inventory alone tells someone which hardware to
 * attack. */
static int api_entropy(char* body, int max) {
    JSONBuf j = { body, 0, max };
    entropy_status_t st;
    entropy_get_status(&st);

    jb_obj_open(&j, 0);
    jb_str(&j,  "ready",       st.ready ? "true" : "false");   jb_putc(&j, ',');
    jb_str(&j,  "rdseed",      st.have_rdseed ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j,  "rdrand",      st.have_rdrand ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j,  "jitter",      st.have_jitter ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "pool_bits",       st.pool_bits);        jb_putc(&j, ',');
    jb_uint(&j, "reseed_count",    st.reseed_count);     jb_putc(&j, ',');
    jb_uint(&j, "health_failures", st.health_failures);  jb_putc(&j, ',');
    jb_uint(&j, "rdseed_retries",  st.rdseed_retries);   jb_putc(&j, ',');

    /* Absent rather than zero when unseeded. A field of 64 zeros would look
     * like a fingerprint and would compare EQUAL across two unseeded nodes --
     * the diversity check would then report a failure whose cause is "no
     * entropy" rather than "identical entropy", which are different problems
     * with different fixes. */
    uint8_t fp[32];
    if (entropy_boot_fingerprint(fp) == ENTROPY_OK) {
        char hex[65];
        static const char* hx = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            hex[i * 2]     = hx[fp[i] >> 4];
            hex[i * 2 + 1] = hx[fp[i] & 0xf];
        }
        hex[64] = 0;
        jb_str(&j, "boot_fingerprint", hex);
    } else {
        jb_str(&j, "boot_fingerprint", "");
    }
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── GET /api/metrics ─────────────────────────────────────────────────────────
// Live kernel instrumentation: access events, tier promotions, IPC latency,
// and (Navigator-Parity Gap Roadmap Phase 2) real CPU/RAM/disk figures.
//
// cpu_idle_ticks/cpu_total_ticks: cumulative counters, not a pre-computed
// percentage -- the caller (SlsSystemHealth.tsx) diffs two consecutive polls
// to get a windowed busy% for the period between them, the same "cumulative
// counter, diff client-side" convention total_accesses/total_promotions
// already established here. Deliberately no kernel-side ring buffer for
// trend history (this phase's original scope draft below floated one): the
// frontend already polls every 5s and can keep its own bounded rolling
// window of real samples client-side just as easily, without adding new
// timer-driven kernel state -- smallest real version, same posture every
// prior phase in this codebase has taken.
static int api_metrics(char* body, int max) {
    JSONBuf j = { body, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "total_accesses",    tier_total_accesses);       jb_putc(&j, ',');
    jb_uint(&j, "total_promotions",  tier_total_promotions);     jb_putc(&j, ',');
    jb_uint(&j, "ipc_posted",        ipc_stats.total_posted);    jb_putc(&j, ',');
    jb_uint(&j, "ipc_dispatched",    ipc_stats.total_dispatched);jb_putc(&j, ',');
    jb_uint(&j, "ipc_avg_latency_ns",ipc_stats.avg_latency_ns);  jb_putc(&j, ',');
    jb_uint(&j, "cpu_idle_ticks",    cpu_idle_wait_count);       jb_putc(&j, ',');
    jb_uint(&j, "cpu_total_ticks",   kernel_tick_counter);       jb_putc(&j, ',');
    jb_uint(&j, "ram_allocated_frames", frame_pool_allocated_count()); jb_putc(&j, ',');
    jb_uint(&j, "ram_total_frames",  frame_pool_total_frames()); jb_putc(&j, ',');
    jb_uint(&j, "disk_capacity_bytes", nvme_get_capacity_bytes());
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── Architectural Phase 3: CORS allowlist ────────────────────────────────────
// (docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md). Replaces the previous
// unconditional "Access-Control-Allow-Origin: *". A browser only ever
// accepts a CORS response whose header exactly matches its own Origin, so a
// single static value could never have covered more than one real origin
// anyway -- the wildcard's only actual effect was allowing literally any
// origin at all, not "these origins" specifically. http_resolve_cors_origin()
// (defined below, after str_find()) reflects the request's own Origin back
// only if it's on the fixed allowlist, into this buffer; every response
// helper appends whatever's in here (empty string if the origin wasn't
// allowed, which is the standard/safe CORS failure mode -- the browser just
// blocks the page's JS from reading the response).
//
// Resolved once per request, in http_route() before any handler runs,
// rather than threaded as a new parameter through http_respond()'s ~90 call
// sites: safe under this server's concurrency model (Architectural Phase 1)
// because request handling is still fully serialized end to end -- one
// request's entire parse/dispatch/respond cycle completes before the next
// begins -- the same reasoning already documented for Phase 1's own
// connection state.
static const char* const CORS_ALLOWED_ORIGINS[] = {
    "https://aerosls.kubeworkz.io",
    "http://localhost:3000",
    "http://localhost:3001",
};
#define CORS_ALLOWED_ORIGINS_COUNT (int)(sizeof(CORS_ALLOWED_ORIGINS) / sizeof(CORS_ALLOWED_ORIGINS[0]))
static char g_cors_origin_hdr[128];  // "" (no header) or "Access-Control-Allow-Origin: <origin>\r\n"

// ─── HTTP response helper ─────────────────────────────────────────────────────
/* ─── One place where bytes leave, so TLS is not nine separate decisions ───
 * Every response path in this file used tcp_send() directly. Rather than teach
 * nine call sites about TLS -- and leave the tenth, written later, to be the
 * one that leaks a plaintext response onto an encrypted connection -- they all
 * go through here.
 *
 * mbedtls_ssl_write() may accept fewer bytes than offered and may ask to be
 * called again with THE SAME arguments, so the retry advances only by what was
 * actually taken. The attempt count is bounded because this runs inside the
 * single poll loop that also drives the serial console: spinning here to push
 * out a response would be the same denial of service the BIO bridge was
 * written to avoid, just moved to the write side. Giving up loses the tail of
 * one response and says so; it does not stall the node. */
#define HTTP_TLS_WRITE_ATTEMPTS 64

/* http_conns[] is declared ~5,700 lines below, next to the poll loop that owns
 * it. Rather than hoist the whole structure up here purely so this function
 * can read one flag, the flag is read through an accessor defined beside the
 * array. */
static int http_conn_is_tls(int conn);

static int http_send(int conn, const void* buf, uint32_t len) {
    if (conn < 0 || conn >= TCP_MAX_CONNS || !http_conn_is_tls(conn)) {
        return tcp_send(conn, buf, len);
    }
    const unsigned char* p = (const unsigned char*)buf;
    uint32_t off = 0;
    for (int attempt = 0; attempt < HTTP_TLS_WRITE_ATTEMPTS && off < len; attempt++) {
        int w = tls_server_write(conn, p + off, (size_t)(len - off));
        if (w < 0) return w;          /* dead session; caller tears down */
        off += (uint32_t)w;           /* w == 0 is back-pressure: try again */
    }
    if (off < len) {
        kernel_serial_printf("[TLS] conn %d: gave up with %u of %u bytes written\n",
                             conn, (unsigned)off, (unsigned)len);
    }
    return (int)off;
}

static void http_respond(int conn, int status, const char* ctype,
                          const char* body, int blen) {
    char hdr[256];
    const char* reason = status == 200 ? "OK" :
                         status == 404 ? "Not Found" :
                         status == 405 ? "Method Not Allowed" : "Error";
    // Build status line + headers into hdr[]
    int hpos = 0;
    const char* sl = "HTTP/1.1 ";
    while (*sl) hdr[hpos++] = *sl++;
    // status code as string
    hdr[hpos++] = (char)('0' + status/100);
    hdr[hpos++] = (char)('0' + (status/10)%10);
    hdr[hpos++] = (char)('0' + status%10);
    hdr[hpos++] = ' ';
    while (*reason) hdr[hpos++] = *reason++;
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';
    const char* hdrs =
        "Content-Type: ";
    while (*hdrs) hdr[hpos++] = *hdrs++;
    while (*ctype) hdr[hpos++] = *ctype++;
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';
    // Content-Length
    const char* cl = "Content-Length: ";
    while (*cl) hdr[hpos++] = *cl++;
    // write blen as decimal
    char tmp[12]; int tl = 0;
    int bl = blen;
    if (bl == 0) { tmp[tl++] = '0'; }
    else { while (bl) { tmp[tl++] = (char)('0' + bl%10); bl /= 10; } }
    for (int i = tl-1; i >= 0; i--) hdr[hpos++] = tmp[i];
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';
    const char* cors = g_cors_origin_hdr;
    while (*cors) hdr[hpos++] = *cors++;
    hdr[hpos++] = '\r'; hdr[hpos++] = '\n';  // end of headers

    http_send(conn, hdr, (uint32_t)hpos);
    if (blen > 0) http_send(conn, body, (uint32_t)blen);
}

// Like http_respond but for binary/large assets from the compiled-in bundle.
// Accepts uint32_t length so files larger than 64 KiB (e.g. the JS bundle)
// are served correctly via tcp_send's internal chunking.
static void http_respond_raw(int conn, const char* ctype,
                              const uint8_t* data, uint32_t blen) {
    char hdr[256];
    int hpos = 0;
    // Status line
    const char* sl = "HTTP/1.1 200 OK\r\nContent-Type: ";
    while (*sl) hdr[hpos++] = *sl++;
    while (*ctype) hdr[hpos++] = *ctype++;
    const char* cl = "\r\nContent-Length: ";
    while (*cl) hdr[hpos++] = *cl++;
    // Write blen as decimal (uint32_t, up to 10 digits)
    char tmp[12]; int tl = 0;
    uint32_t bl = blen;
    if (bl == 0) { tmp[tl++] = '0'; }
    else { while (bl) { tmp[tl++] = (char)('0' + bl % 10); bl /= 10; } }
    for (int i = tl - 1; i >= 0; i--) hdr[hpos++] = tmp[i];
    const char* crlf = "\r\n";
    while (*crlf) hdr[hpos++] = *crlf++;
    const char* cors = g_cors_origin_hdr;
    while (*cors) hdr[hpos++] = *cors++;
    const char* crlf2 = "\r\n";
    while (*crlf2) hdr[hpos++] = *crlf2++;

    http_send(conn, hdr, (uint32_t)hpos);
    if (blen > 0) http_send(conn, data, blen);
}

// Forward declarations for helpers defined in the Phase F section below
static int json_str(const char* json, const char* key, char* out, int max);
static int json_int(const char* json, const char* key);

// ─── POST /auth/token — issue a bearer token for an email ─────────────────────
// Architectural Phase 4: also reads "password" from the body. Missing/empty
// is fine for an unknown email (auto-provisioned GUEST, no password
// required) but will fail auth_http_issue()'s credential check for any
// account that has one set -- see that function's own comment.
static int api_auth_token(const char* body, char* buf, int max) {
    if (!body) {
        const char* err = "{\"error\":\"missing body\"}";
        int n=0; while(err[n]&&n<max-1) buf[n]=err[n++]; buf[n]='\0'; return n;
    }
    char email[AUTH_EMAIL_LEN];
    char password[64];
    json_str(body, "email", email, AUTH_EMAIL_LEN);
    json_str(body, "password", password, sizeof(password));
    return auth_http_issue(email, password, buf, max);
}

// ─── GET /auth/verify — decode and return token metadata ────────────────────
static int api_auth_verify(const char* raw_req, char* buf, int max) {
    uint32_t uid = 0; SLSRole role = ROLE_GUEST;
    int valid = auth_http_extract(raw_req, &uid, &role);
    // Find email for this uid
    const char* email = "(unknown)";
    for (int i=0;i<AUTH_MAX_TOKENS;i++) {
        if (auth_tokens[i].active && auth_tokens[i].uid==uid) {
            email = auth_tokens[i].email; break;
        }
    }
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j, "valid",   valid ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "uid",    uid);                      jb_putc(&j, ',');
    jb_str(&j, "role",    role_name(role));           jb_putc(&j, ',');
    jb_str(&j, "email",   email);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Phase F: HTTP/REST helpers ───────────────────────────────────────────────

// Substring search (freestanding — no libc strstr)
static const char* str_find(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char* h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

static int str_ncmp2(const char* a, const char* b, int n) {
    while (n-- > 0) {
        if (*a != *b) return *a - *b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}

// Parses a plain decimal uint32 out of a raw (non-JSON) path segment --
// VectorStore Gap Analysis §1.4's own "/skip/<N>" path segment is this
// file's first path-segment parameter that isn't a name/identifier, so
// json_int()'s own digit-scanning loop (which expects a `"key":` prefix
// it doesn't have here) doesn't apply; this is that same loop's body,
// reused directly against a bare string instead.
static uint32_t path_parse_u32(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

// Locate the HTTP request body (after the blank line \r\n\r\n)
static const char* http_body(const char* req) {
    for (const char* p = req; p[0]; p++)
        if (p[0]=='\r'&&p[1]=='\n'&&p[2]=='\r'&&p[3]=='\n') return p+4;
    return 0;
}

// Extract "key": "value" from a JSON string; returns chars copied
static int json_str(const char* json, const char* key, char* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p!='"') return 0;
    p++;
    int n = 0;
    while (*p && *p!='"' && n<max-1) out[n++] = *p++;
    out[n] = '\0';
    return n;
}

// Extract "key": N (integer) from a JSON string
static int json_int(const char* json, const char* key) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    int v = 0;
    while (*p>='0'&&*p<='9') { v = v*10 + (*p-'0'); p++; }
    return v;
}

// Extract "key": N (integer, widened to 64 bits) from a JSON string --
// json_int()'s own parse loop widened, needed for vector external_id values
// (Gap Remediation Phase C).
static uint64_t json_uint64(const char* json, const char* key) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    uint64_t v = 0;
    while (*p>='0'&&*p<='9') { v = v*10 + (uint64_t)(*p-'0'); p++; }
    return v;
}

// Extract a JSON array of numbers "key": [n0, n1, ...] into out[] as floats
// -- Gap Remediation Phase C, needed for vector query/insert values. No
// libc strtof (freestanding) -- a hand-rolled decimal/sign/fraction parse,
// the same shape rowstore.c's own rs_parse_f64()/net/ollama_client.c's own
// oc_parse_json_number() already use elsewhere, kept as its own small copy
// here rather than shared (matching this project's established "each file
// keeps its own small helpers" convention -- see vec_join.h's own header
// comment on why). Returns the number of floats written (0..max).
static int json_float_array(const char* json, const char* key, float* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p==' '||*p=='\t'||*p==',') p++;
        if (*p == ']' || !*p) break;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        double v = 0.0;
        while (*p>='0'&&*p<='9') { v = v*10.0 + (double)(*p-'0'); p++; }
        if (*p == '.') {
            p++;
            double frac = 0.1;
            while (*p>='0'&&*p<='9') { v += (double)(*p-'0') * frac; frac *= 0.1; p++; }
        }
        out[n++] = (float)(neg ? -v : v);
        while (*p==' '||*p=='\t') p++;
    }
    return n;
}

// Extract "key": [n0, n1, ...] as uint32_t values -- the integer sibling
// of json_float_array() above, added for POST /api/qemu/bench_sweep. The
// bench sweep takes an explicit loads list so the caller controls which
// block counts get exercised, rather than every value in 1..510 (each
// launch costs arena; a full sweep would exhaust it). Returns the number
// of values written (0..max); 0 on a missing key or a non-array, the same
// contract json_float_array() has. Out-of-range or duplicate values are
// the caller's job to reject -- this just parses.
static int json_uint_array(const char* json, const char* key, uint32_t* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p != '[') return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        while (*p==' '||*p=='\t'||*p==',') p++;
        if (*p == ']' || !*p) break;
        uint64_t v = 0;
        while (*p>='0'&&*p<='9') { v = v*10 + (uint64_t)(*p-'0'); p++; }
        out[n++] = (uint32_t)v;
        while (*p==' '||*p=='\t') p++;
    }
    return n;
}

// Fills out[] with json_str(json, key, ...)'s result, or with def if the
// key was absent/empty -- json_str() itself leaves out[] untouched (not
// even null-terminated) on failure, so a caller that needs a guaranteed
// default must pre-clear first. No libc strcpy (freestanding, and not used
// anywhere else in this file) -- a small hand-rolled copy loop instead,
// matching this codebase's established per-file *_strcpy() convention.
static void json_str_or_default(const char* json, const char* key, char* out, int max, const char* def) {
    out[0] = '\0';
    json_str(json, key, out, max);
    if (!out[0]) {
        int i = 0;
        for (; i < max - 1 && def[i]; i++) out[i] = def[i];
        out[i] = '\0';
    }
}

// Extract "key": F (a single decimal number) as a float -- Gap Remediation
// Phase C, the scalar counterpart to json_float_array() above, needed for
// distance values inside a "matches" array element (POST /api/vec/join).
// Gap Remediation (post-roadmap x86 boot-build fix): out-parameter, not a
// by-value float return -- see kernel/vecstore.c's own header comment on
// why (the real x86-64 cross-build disables SSE, which breaks float
// BY-VALUE RETURN specifically; local double/float math is unaffected).
// Behavior unchanged from the original by-value version: *out is set to
// 0.0f if `key` isn't found, same as the old "return 0.0f" default.
static void json_float(const char* json, const char* key, float* out) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) { *out = 0.0f; return; }
    p += si;
    while (*p==' '||*p=='\t') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    double v = 0.0;
    while (*p>='0'&&*p<='9') { v = v*10.0 + (double)(*p-'0'); p++; }
    if (*p == '.') {
        p++;
        double frac = 0.1;
        while (*p>='0'&&*p<='9') { v += (double)(*p-'0') * frac; frac *= 0.1; p++; }
    }
    *out = (float)(neg ? -v : v);
}

// Extracts the n-th top-level JSON object from a "key": [ {...}, {...} ]
// array into out (a plain, null-terminated copy of just that one object's
// text, braces included) -- Gap Remediation Phase C, needed for POST
// /api/vec/join, the one route on this surface taking an array of OBJECTS
// rather than an array of plain numbers (json_float_array already covers
// those). Assumes no nested objects inside a match entry (true for the
// {external_id, page_id, slot_index, distance} shape this route expects --
// matching exactly what GET .../api/vec/search's own response already
// emits, so a client can round-trip one response straight into the next
// request's body). Returns 1 if index n existed and was copied, 0
// otherwise (array too short, key missing, malformed).
static int json_array_object_at(const char* json, const char* key, int n, char* out, int max) {
    char srch[128]; int si = 0;
    srch[si++] = '"';
    for (int i = 0; key[i]&&si<120; i++) srch[si++] = key[i];
    srch[si++] = '"'; srch[si++] = ':'; srch[si] = '\0';
    const char* p = str_find(json, srch);
    if (!p) return 0;
    p += si;
    while (*p==' '||*p=='\t') p++;
    if (*p != '[') return 0;
    p++;
    int idx = 0;
    while (*p && *p != ']') {
        while (*p==' '||*p=='\t'||*p==',') p++;
        if (*p != '{') break;
        const char* start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        if (idx == n) {
            int len = (int)(p - start);
            if (len >= max) len = max - 1;
            for (int i = 0; i < len; i++) out[i] = start[i];
            out[len] = '\0';
            return 1;
        }
        idx++;
    }
    return 0;
}

// URL-decode: replace + with space and %XX with the byte value
static int url_decode(const char* src, char* dst, int max) {
    int n = 0;
    while (*src && n<max-1) {
        if (*src=='+') { dst[n++]=' '; src++; }
        else if (*src=='%'&&src[1]&&src[2]) {
            uint8_t hi=(src[1]>='a')?src[1]-'a'+10:(src[1]>='A')?src[1]-'A'+10:src[1]-'0';
            uint8_t lo=(src[2]>='a')?src[2]-'a'+10:(src[2]>='A')?src[2]-'A'+10:src[2]-'0';
            dst[n++]=(char)((hi<<4)|lo); src+=3;
        } else { dst[n++]=*src++; }
    }
    dst[n]='\0';
    return n;
}

// Extract ?key=value from a query string (URL-decoded into out)
static int url_param(const char* qs, const char* key, char* out, int max) {
    int klen = 0; while (key[klen]) klen++;
    while (*qs) {
        if (str_ncmp2(qs, key, klen)==0 && qs[klen]=='=') {
            const char* v = qs+klen+1;
            const char* e = v; while (*e&&*e!='&') e++;
            int vl = (int)(e-v); if (vl>=511) vl=510;
            char tmp[512]; for (int i=0;i<vl;i++) tmp[i]=v[i]; tmp[vl]='\0';
            return url_decode(tmp, out, max);
        }
        while (*qs&&*qs!='&') qs++;
        if (*qs=='&') qs++;
    }
    return 0;
}

// ─── CORS preflight ───────────────────────────────────────────────────────────
static void http_options(int conn) {
    char h[256]; int hp = 0;
    const char* sl = "HTTP/1.1 204 No Content\r\n";
    while (*sl) h[hp++] = *sl++;
    const char* co = g_cors_origin_hdr;
    while (*co) h[hp++] = *co++;
    // VectorStore Interface Roadmap Phase 1: DELETE added -- a browser
    // preflights any DELETE request (and any request with a
    // non-"simple" Content-Type like application/json) against this exact
    // list before the real request is ever sent; leaving DELETE off here
    // would have made the new /api/vec/* DELETE routes below completely
    // unreachable from the Navigator SPA even though the routes themselves
    // work fine when called directly (e.g. via curl or the Terminal's own
    // authFetch(), neither of which triggers a CORS preflight).
    const char* rest = "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
                       "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                       "Content-Length: 0\r\n\r\n";
    while (*rest) h[hp++] = *rest++;
    http_send(conn, h, (uint32_t)hp);
}

// Resolves the current request's Origin header against CORS_ALLOWED_ORIGINS
// into g_cors_origin_hdr. Defined here (after str_find()/str_ncmp2(), which
// it needs) rather than next to CORS_ALLOWED_ORIGINS above; called once per
// request from the top of http_route(), before http_options() or any other
// response helper runs.
static void http_resolve_cors_origin(const char* req) {
    g_cors_origin_hdr[0] = '\0';
    const char* o = str_find(req, "Origin:");
    if (!o) o = str_find(req, "origin:");
    if (!o) return;
    o += 7;
    while (*o == ' ') o++;
    const char* end = o;
    while (*end && *end != '\r' && *end != '\n') end++;
    int len = (int)(end - o);
    if (len <= 0 || len >= 96) return;
    for (int i = 0; i < CORS_ALLOWED_ORIGINS_COUNT; i++) {
        const char* allowed = CORS_ALLOWED_ORIGINS[i];
        int alen = 0; while (allowed[alen]) alen++;
        if (alen != len) continue;
        if (str_ncmp2(allowed, o, len) != 0) continue;
        int p = 0;
        const char* pre = "Access-Control-Allow-Origin: ";
        while (*pre) g_cors_origin_hdr[p++] = *pre++;
        for (int k = 0; k < len; k++) g_cors_origin_hdr[p++] = o[k];
        g_cors_origin_hdr[p++] = '\r'; g_cors_origin_hdr[p++] = '\n';
        g_cors_origin_hdr[p] = '\0';
        return;
    }
}

// ─── GET /api/objects ─────────────────────────────────────────────────────────
static int api_objects(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", object_catalog_count); jb_putc(&j, ',');
    jb_arr_open(&j, "objects");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name);            jb_putc(&j, ',');
        jb_str(&j, "type", obj_type_name(e->type)); jb_putc(&j, ',');
        jb_hex(&j, "vaddr", e->base_vaddr);     jb_putc(&j, ',');
        jb_str(&j, "tier", tier_name(e->storage_tier)); jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages);    jb_putc(&j, ',');
        jb_uint(&j, "uid", e->owner_uid);       jb_putc(&j, ',');
        jb_str(&j, "role", role_name(e->owner_role)); jb_putc(&j, ',');
        jb_uint(&j, "field_count", object_records[i].field_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tables — Gap Remediation Phase B ────────────────────────────────
// Enumerates row-set tables (object_catalog[] entries with uses_rowstore
// set -- distinct from OBJ_TYPE_DB_TABLE, which also covers legacy
// single-record KV objects that never called rowstore_create_table()).
// Needed so a Navigator-style table browser can list what exists without
// already knowing a name -- no such enumeration had any HTTP route before
// this (docs/AeroSLS-Gap-Analysis-v0.1.md §5).
static int api_tables_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "tables");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !e->uses_rowstore) continue;
        struct RowTableHeader* h = &table_headers[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name); jb_putc(&j, ',');
        jb_uint(&j, "column_count", h->layout.column_count); jb_putc(&j, ',');
        jb_uint(&j, "row_count", h->row_count); jb_putc(&j, ',');
        jb_uint(&j, "page_count", h->page_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tables/<name>/schema — Gap Remediation Phase B ──────────────────
// Column names + types for one row-set table, derived from its
// RowTableLayout (computed once at rowstore_create_table() time -- see
// rowstore.h). Distinct from the legacy /api/objects/<name>'s own
// best-effort per-field type lookup (which walks object_schemas[] matching
// on live record field keys); this reads the table's own fixed layout
// directly, the authoritative source for a row-set table's real columns.
static int api_table_schema(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !e->uses_rowstore || strcmp(e->name, name) != 0) continue;
        struct RowTableLayout* layout = &table_headers[i].layout;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", e->name); jb_putc(&j, ',');
        jb_arr_open(&j, "columns");
        for (uint32_t c = 0; c < layout->column_count; c++) {
            if (c) jb_putc(&j, ',');
            jb_obj_open(&j, 0);
            jb_str(&j, "name", layout->column_names[c]); jb_putc(&j, ',');
            jb_str(&j, "type", field_type_name(layout->column_types[c]));
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j, 0); jb_str(&j, "error", "table not found"); jb_obj_close(&j);
    j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/schema/export — SQL Feature-Parity Roadmap, Phase 8 follow-on ───
// Reconstructs CREATE TABLE/CREATE INDEX SQL text for every row-set table
// req_uid can read (sql_schema_export(), sql_exec.c) and returns it as one
// multiline JSON string field -- the SQLite convention this phase follows:
// a schema is exported as plain SQL text, not a new structured format. See
// sql_exec.h's own header comment for what's skipped (with a `-- ` comment
// explaining why) rather than silently dropped: RANGE constraints (no SQL
// syntax exists for them in this parser) and any single CREATE TABLE that
// would exceed SQL_MAX_TEXT_LEN once fully reconstructed.
static int api_schema_export(uint32_t req_uid, char* buf, int max) {
    static char sql_out[SQL_SCHEMA_EXPORT_MAX_LEN];
    uint32_t n = sql_schema_export(req_uid, sql_out, sizeof(sql_out));
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str_multiline(&j, "sql", sql_out);
    jb_putc(&j, ',');
    jb_uint(&j, "bytes", n);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/vec/schema/export — VectorStore Interface Roadmap follow-on ─────
// Reconstructs COLLECTION/INDEX definition text for every vector collection
// req_uid can read (vec_schema_export(), vec_index.c) and returns it as one
// multiline JSON string field, same convention api_schema_export() above
// established for SQL text -- see vec_index.h's own header comment for why
// this is a small purpose-built text grammar rather than SQL or JSON, and
// for the named gap that this covers definitions only, never vector data.
static int api_vec_schema_export(uint32_t req_uid, char* buf, int max) {
    static char vec_out[VEC_SCHEMA_EXPORT_MAX_LEN];
    uint32_t n = vec_schema_export(req_uid, vec_out, sizeof(vec_out));
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str_multiline(&j, "text", vec_out);
    jb_putc(&j, ',');
    jb_uint(&j, "bytes", n);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/vec/data/export/<collection>[/skip/<N>] — VectorStore
// Interface Roadmap follow-on: bulk vector DATA export (complementing
// api_vec_schema_export() above, which is definitions only) ────────────────
// Reconstructs "VECTOR <collection> <external_id> <v0> ..." lines for the
// one named collection req_uid can read, via vec_data_export() (vecstore.c).
// Path-segment parameters (not a query string) match this codebase's own
// established /api/tables/<name>/schema convention -- no query-string
// parsing infrastructure exists anywhere in this file (confirmed by grep
// before writing this route), so a path segment is the one real precedent
// to follow rather than inventing a new parsing mechanism for this route
// alone. Reports vectors_written/vectors_total/truncated/entries_remaining
// explicitly -- see vecstore.h's own header comment on why VEC_DATA_EXPORT_
// MAX_LEN is genuinely tight at real embedding dimensions, so truncation
// (and needing more than one call to get everything) is a real, expected
// outcome this response must surface, not hide.
//
// VectorStore Gap Analysis §1.4 (closed): the optional trailing "/skip/<N>"
// segment carries skip_count through to vec_data_export() -- see that
// function's own header comment (vecstore.h) for the full resumption
// design. "/skip/" is a literal marker, not a generic query-string parser:
// collection names are OBJECT_NAME_LEN plain identifiers that can never
// contain a '/' themselves, so splitting the path on the LAST occurrence of
// "/skip/" unambiguously separates the two segments without needing any
// new parsing infrastructure.
static int api_vec_data_export(uint32_t req_uid, const char* collection_name,
                                uint32_t skip_count, char* buf, int max) {
    static char vec_out[VEC_DATA_EXPORT_MAX_LEN];
    struct VecDataExportResult res;
    vec_data_export(req_uid, collection_name, skip_count, vec_out, sizeof(vec_out), &res);
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str_multiline(&j, "text", vec_out); jb_putc(&j, ',');
    jb_uint(&j, "bytes", res.bytes_written); jb_putc(&j, ',');
    jb_uint(&j, "vectors_written", res.vectors_written); jb_putc(&j, ',');
    jb_uint(&j, "vectors_total", res.vectors_total); jb_putc(&j, ',');
    jb_uint(&j, "entries_remaining", res.entries_remaining); jb_putc(&j, ',');
    jb_str(&j, "truncated", res.truncated ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/objects/<name> ──────────────────────────────────────────────────
static int api_object_detail(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || !strcmp(e->name, name)) {
            if (!e->active) continue;
        }
        if (strcmp(e->name, name) != 0) continue;
        jb_obj_open(&j, 0);
        jb_str(&j, "name",  e->name);           jb_putc(&j, ',');
        jb_str(&j, "type",  obj_type_name(e->type)); jb_putc(&j, ',');
        jb_hex(&j, "vaddr", e->base_vaddr);     jb_putc(&j, ',');
        jb_str(&j, "tier",  tier_name(e->storage_tier)); jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages);    jb_putc(&j, ',');
        jb_uint(&j, "uid",  e->owner_uid);      jb_putc(&j, ',');
        jb_uint(&j, "perm", e->perm_mask);      jb_putc(&j, ',');
        // Records
        struct SLSObjectRecord* rec = &object_records[i];
        jb_arr_open(&j, "records");
        int ff = 1;
        for (uint32_t f = 0; f < RECORD_MAX_FIELDS; f++) {
            if (!rec->fields[f].active) continue;
            if (!ff) jb_putc(&j, ','); ff = 0;
            // find schema type
            const char* tname = "STRING";
            for (uint32_t s = 0; s < SCHEMA_MAX_FIELDS; s++) {
                if (object_schemas[i].fields[s].active &&
                    !strcmp(object_schemas[i].fields[s].key,
                                      rec->fields[f].key)) {
                    tname = field_type_name(object_schemas[i].fields[s].type);
                    break;
                }
            }
            jb_obj_open(&j, 0);
            jb_str(&j, "key",   rec->fields[f].key);   jb_putc(&j, ',');
            jb_str(&j, "value", rec->fields[f].value);  jb_putc(&j, ',');
            jb_str(&j, "type",  tname);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j, 0);
    jb_str(&j, "error", "object not found");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/services ────────────────────────────────────────────────────────
static int api_services_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", service_count); jb_putc(&j, ',');
    jb_obj_open(&j, "ipc");
    jb_uint(&j, "posted",     ipc_stats.total_posted);     jb_putc(&j, ',');
    jb_uint(&j, "dispatched", ipc_stats.total_dispatched); jb_putc(&j, ',');
    jb_uint(&j, "dropped",    ipc_stats.total_dropped);
    jb_obj_close(&j); jb_putc(&j, ',');
    jb_arr_open(&j, "services");
    for (uint32_t i = 0; i < service_count; i++) {
        struct ServiceDescriptor* s = &services[i];
        if (!s->active) continue;
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_str(&j,  "name",    s->name);           jb_putc(&j, ',');
        jb_uint(&j, "pid",     s->pid);             jb_putc(&j, ',');
        jb_uint(&j, "port",    s->port);            jb_putc(&j, ',');
        jb_str(&j,  "state",   svc_state_name(s->state)); jb_putc(&j, ',');
        jb_uint(&j, "reboots", s->reboot_count);   jb_putc(&j, ',');
        jb_uint(&j, "msgs",    (uint64_t)s->msgs_processed);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/wal ─────────────────────────────────────────────────────────────
static int api_wal_json(char* buf, int max) {
    uint32_t committed=0, pending=0, aborted=0;
    for (uint32_t i=0;i<wal_entry_count;i++) {
        if (wal_buffer[i].state==WAL_STATE_COMMITTED) committed++;
        else if (wal_buffer[i].state==WAL_STATE_PENDING) pending++;
        else aborted++;
    }
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "total",     wal_entry_count); jb_putc(&j, ',');
    jb_uint(&j, "committed", committed);       jb_putc(&j, ',');
    jb_uint(&j, "pending",   pending);         jb_putc(&j, ',');
    jb_uint(&j, "aborted",   aborted);         jb_putc(&j, ',');
    jb_arr_open(&j, "entries");
    for (uint32_t i=0;i<wal_entry_count;i++) {
        struct WALEntry* w = &wal_buffer[i];
        if (i) jb_putc(&j, ',');
        const char* st = w->state==WAL_STATE_COMMITTED ? "COMMITTED" :
                         w->state==WAL_STATE_PENDING    ? "PENDING"   : "ABORTED";
        jb_obj_open(&j, 0);
        jb_uint(&j, "id",    w->entry_id); jb_putc(&j, ',');
        jb_uint(&j, "tx",    w->tx_id);    jb_putc(&j, ',');
        jb_str(&j,  "key",   w->key);      jb_putc(&j, ',');
        jb_str(&j,  "state", st);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tiers ───────────────────────────────────────────────────────────
static int api_tiers_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    static const SLSStorageTier tiers[]  = { STORAGE_TIER_L1_CACHE, STORAGE_TIER_L2_DRAM, STORAGE_TIER_L3_SSD };
    static const char* tier_keys[] = { "l1_cache", "l2_dram", "l3_ssd" };
    for (int t = 0; t < 3; t++) {
        if (t) jb_putc(&j, ',');
        jb_arr_open(&j, tier_keys[t]);
        int first = 1;
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            struct SLSObjectEntry* e = &object_catalog[i];
            if (!e->active || e->storage_tier != tiers[t]) continue;
            if (!first) jb_putc(&j, ','); first = 0;
            uint32_t acc=0, idle=0;
            for (int s=0;s<TIER_MAX_TRACKED;s++) {
                if (tier_stats[s].active && tier_stats[s].object_id==e->object_id)
                    { acc=tier_stats[s].access_count; idle=tier_stats[s].idle_ticks; break; }
            }
            jb_obj_open(&j, 0);
            jb_str(&j,  "name",     e->name);           jb_putc(&j, ',');
            jb_uint(&j, "accesses", acc);               jb_putc(&j, ',');
            jb_uint(&j, "idle",     idle);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/network/status — Navigator-Parity Gap Roadmap Phase 5a ──────────
// Surfaces the e1000 interface's negotiated IP/gateway/subnet/MAC (net.h's
// runtime globals, updated by dhcp.c) plus TCP connection-pool utilization
// (tcp_conns[]/TCP_MAX_CONNS -- already a real, sized resource, no new
// tracking needed). dhcp_bound distinguishes a real DHCP lease from the
// KERNEL_STATIC_* config.h fallback, so this route doesn't overclaim a live
// lease when none was ever granted.
static const char* tcp_state_name(TCPState s) {
    switch (s) {
        case TCP_CLOSED:       return "CLOSED";
        case TCP_LISTEN:       return "LISTEN";
        case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
        case TCP_ESTABLISHED:  return "ESTABLISHED";
        case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_LAST_ACK:     return "LAST_ACK";
        case TCP_FIN_WAIT:     return "FIN_WAIT";
        case TCP_TIME_WAIT:    return "TIME_WAIT";
        case TCP_SYN_SENT:     return "SYN_SENT";
        default:               return "UNKNOWN";
    }
}

static int api_network_status_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_ip(&j,  "ip",           net_my_ip);       jb_putc(&j, ',');
    jb_ip(&j,  "gateway",      net_gw_ip);       jb_putc(&j, ',');
    jb_ip(&j,  "subnet_mask",  net_subnet_mask); jb_putc(&j, ',');
    jb_mac(&j, "mac",          net_my_mac);      jb_putc(&j, ',');
    jb_str(&j, "dhcp_bound",   dhcp_is_bound() ? "true" : "false"); jb_putc(&j, ',');

    uint32_t active = 0;
    uint32_t by_state[TCP_SYN_SENT + 1]; // dense: one slot per TCPState value
    for (uint32_t s = 0; s <= TCP_SYN_SENT; s++) by_state[s] = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!tcp_conns[i].active) continue;
        active++;
        by_state[tcp_conns[i].state]++;
    }
    jb_obj_open(&j, "tcp_pool");
    jb_uint(&j, "active",   active);        jb_putc(&j, ',');
    jb_uint(&j, "capacity", TCP_MAX_CONNS); jb_putc(&j, ',');
    jb_obj_open(&j, "by_state");
    int first_state = 1;
    for (uint32_t s = 0; s <= TCP_SYN_SENT; s++) {
        if (!by_state[s]) continue;
        if (!first_state) jb_putc(&j, ','); first_state = 0;
        jb_uint(&j, tcp_state_name((TCPState)s), by_state[s]);
    }
    jb_obj_close(&j);
    jb_obj_close(&j); /* tcp_pool */

    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/disk — Navigator-Parity Gap Roadmap Phase 5b ────────────────────
// Combines the already-exposed NVMe capacity (nvme_get_capacity_bytes(),
// Phase 2, also present in /api/metrics as disk_capacity_bytes) with the
// new per-tier bytes-used/object-count breakdown (tier_capacity_totals(),
// tier_mgr.c) -- the one genuinely new computation in this phase. Tier keys
// match /api/tiers's own naming ("l1_cache"/"l2_dram"/"l3_ssd") so the two
// routes stay consistent for any client reading both.
//
// Storage Isolation Roadmap Phase 2 adds a "partitions" array here too:
// storage_quota.c's Phase 1 counters (rowstore+vecstore combined on-disk
// pages), converted to bytes and broken out per-partition, on this same
// route -- the JSON twin of sys_sls_disk_status()'s new serial section.
// No new syscall: storage_get_page_usage()/_quota() are already directly
// callable, the same way api_partition_storagequotas_list() already reads
// them in pages; this just re-exposes them in bytes, next to disk capacity.
static int api_disk_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "capacity_bytes", nvme_get_capacity_bytes()); jb_putc(&j, ',');

    uint64_t bytes_per_tier[TIER_MGR_TIER_COUNT];
    uint32_t count_per_tier[TIER_MGR_TIER_COUNT];
    tier_capacity_totals(bytes_per_tier, count_per_tier);

    static const char* tier_keys[TIER_MGR_TIER_COUNT] = { "l1_cache", "l2_dram", "l3_ssd" };
    jb_obj_open(&j, "tiers");
    for (int t = 0; t < TIER_MGR_TIER_COUNT; t++) {
        if (t) jb_putc(&j, ',');
        jb_obj_open(&j, tier_keys[t]);
        jb_uint(&j, "bytes_used",   bytes_per_tier[t]); jb_putc(&j, ',');
        jb_uint(&j, "object_count", count_per_tier[t]);
        jb_obj_close(&j);
    }
    jb_obj_close(&j); /* tiers */
    jb_putc(&j, ',');

    jb_arr_open(&j, "partitions");
    int first = 1;
    for (uint32_t p = 0; p < PARTITION_MAX; p++) {
        uint64_t page_usage = storage_get_page_usage(p);
        uint64_t page_quota = storage_get_page_quota(p);
        if (page_usage == 0 && page_quota == 0) continue;   // same skip rule as api_partition_storagequotas_list()
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id",      p); jb_putc(&j, ',');
        jb_uint(&j, "disk_bytes_used",   page_usage * 4096ULL); jb_putc(&j, ',');
        jb_uint(&j, "disk_bytes_quota",  page_quota * 4096ULL);
        jb_obj_close(&j);
    }
    jb_arr_close(&j); /* partitions */

    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/processes ───────────────────────────────────────────────────────
static int api_processes_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", proc_count); jb_putc(&j, ',');
    jb_arr_open(&j, "processes");
    int first = 1;
    for (int i = 0; i < PROC_MAX; i++) {
        struct ProcessDescriptor* pd = &proc_table[i];
        if (!pd->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "pid",   pd->pid);                        jb_putc(&j, ',');
        jb_str(&j,  "name",  pd->name);                       jb_putc(&j, ',');
        jb_str(&j,  "state", proc_state_name(pd->state));     jb_putc(&j, ',');
        // Navigator-Parity Gap Roadmap Phase 4: job priority tier.
        jb_str(&j,  "priority", proc_priority_name(pd->priority)); jb_putc(&j, ',');
        jb_uint(&j, "uid",   pd->owner_uid);                  jb_putc(&j, ',');
        jb_hex(&j,  "rip",   pd->user_rip);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/query?q=<text> ─────────────────────────────────────────────────
static int api_query_json(const char* q, char* buf, int max) {
    QueryDomain d = query_domain_for(q);
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str(&j, "query",  q);                       jb_putc(&j, ',');
    jb_str(&j, "domain", query_domain_name(d));    jb_putc(&j, ',');
    // Embed full scan as the data payload
    jb_raw(&j, "\"data\":");
    api_scan(buf + j.pos, max - j.pos - 32);
    int scan_len = 0; while (buf[j.pos + scan_len]) scan_len++;
    j.pos += scan_len;
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── uint64 → decimal string (freestanding; no printf) ─────────────────────
static void prog_u64_to_str(uint64_t v, char* out, int max) {
    if (max < 2) { out[0] = '\0'; return; }
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[21]; int len = 0;
    while (v && len < 20) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
    int i; for (i = 0; i < len && i < max - 1; i++) out[i] = tmp[len - 1 - i];
    out[i] = '\0';
}

// ─── Hex decode helper (used by program upload) ─────────────────────────────
static size_t hex_decode(const char* hex, uint8_t* out, size_t max_bytes) {
    size_t n = 0;
    while (hex[0] && hex[1] && n < max_bytes) {
        char c;
        uint8_t hi, lo;
        c = hex[0];
        hi = (c>='0'&&c<='9') ? (uint8_t)(c-'0')
           : (c>='a'&&c<='f') ? (uint8_t)(c-'a'+10)
           : (c>='A'&&c<='F') ? (uint8_t)(c-'A'+10) : 0xFF;
        c = hex[1];
        lo = (c>='0'&&c<='9') ? (uint8_t)(c-'0')
           : (c>='a'&&c<='f') ? (uint8_t)(c-'a'+10)
           : (c>='A'&&c<='F') ? (uint8_t)(c-'A'+10) : 0xFF;
        if (hi == 0xFF || lo == 0xFF) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

// ─── GET /api/programs ────────────────────────────────────────────────────────
static int api_programs_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "programs");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        struct SLSObjectEntry* e = &object_catalog[i];
        if (!e->active || e->type != OBJ_TYPE_PROGRAM) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j,  "name",  e->name);         jb_putc(&j, ',');
        jb_hex(&j,  "vaddr", e->base_vaddr);   jb_putc(&j, ',');
        jb_uint(&j, "pages", e->size_pages);   jb_putc(&j, ',');
        jb_str(&j,  "tier",  tier_name(e->storage_tier)); jb_putc(&j, ',');
        int bin_loaded = 0;
        uint32_t bin_size = 0;
        const char* bin_fmt = "none";
        for (int b = 0; b < MAX_SERVICE_BINARIES; b++) {
            if (service_binaries[b].active &&
                !strcmp(service_binaries[b].object_name, e->name)) {
                bin_loaded = 1;
                bin_size   = service_binaries[b].size;
                bin_fmt    = binary_format_name(&service_binaries[b]);
                break;
            }
        }
        jb_str(&j,  "binary",       bin_loaded ? "yes" : "no"); jb_putc(&j, ',');
        jb_uint(&j, "binary_bytes", (uint64_t)bin_size);        jb_putc(&j, ',');
        jb_str(&j,  "format",       bin_fmt);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── GET /api/simi/<name> — Gap Remediation Phase G ────────────────────────────
// Structured counterpart to loader_simi_info()'s own console dump -- reads
// through loader_simi_info_query() directly (the same real logic
// loader_simi_info() itself now wraps, see loader.c's own comment), not the
// SYS_SLS_SIMI_INFO syscall -- matching Phase B/C/F's established "read
// kernel state / call the query function directly for a JSON response
// rather than repurposing a console-dump-shaped syscall" precedent.
static int api_simi_info(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    struct SimiInfoResult r;
    loader_simi_info_query(name, &r);

    jb_obj_open(&j, 0);
    jb_str(&j, "name", name); jb_putc(&j, ',');
    const char* status_str =
        r.status == SIMI_INFO_STATUS_OK        ? "ok" :
        r.status == SIMI_INFO_STATUS_NOT_FOUND ? "not_found" :
        r.status == SIMI_INFO_STATUS_NOT_SIMI  ? "not_simi" : "corrupt";
    jb_str(&j, "status", status_str);
    if (r.status == SIMI_INFO_STATUS_NOT_SIMI) {
        jb_putc(&j, ','); jb_str(&j, "format", r.format_name);
    }
    if (r.status == SIMI_INFO_STATUS_OK) {
        jb_putc(&j, ',');
        jb_uint(&j, "num_instr",    r.num_instr);    jb_putc(&j, ',');
        jb_uint(&j, "num_literals", r.num_literals);  jb_putc(&j, ',');
        jb_uint(&j, "num_entries",  r.num_entries);   jb_putc(&j, ',');
        jb_uint(&j, "num_names",    r.num_names);     jb_putc(&j, ',');
        jb_arr_open(&j, "entries");
        for (uint32_t i = 0; i < r.entries_returned; i++) {
            if (i) jb_putc(&j, ',');
            jb_obj_open(&j, 0);
            jb_str(&j, "name", r.entries[i].name); jb_putc(&j, ',');
            jb_uint(&j, "offset", r.entries[i].offset);
            jb_obj_close(&j);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_str(&j, "entries_truncated", r.entries_truncated ? "true" : "false"); jb_putc(&j, ',');
        jb_arr_open(&j, "names");
        for (uint32_t i = 0; i < r.names_returned; i++) {
            if (i) jb_putc(&j, ',');
            jb_esc_str(&j, r.names[i].name);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_str(&j, "names_truncated", r.names_truncated ? "true" : "false"); jb_putc(&j, ',');
        jb_obj_open(&j, "activation");
        jb_str(&j, "cached", r.activation.cached ? "true" : "false");
        if (r.activation.cached) {
            jb_putc(&j, ',');
            jb_uint(&j, "code_pages",   r.activation.code_pages);   jb_putc(&j, ',');
            jb_uint(&j, "entry_offset", r.activation.entry_offset); jb_putc(&j, ',');
            jb_hex(&j,  "content_hash", r.activation.content_hash);
        }
        jb_obj_close(&j);
    }
    jb_obj_close(&j);
    j.buf[j.pos] = '\0';
    return j.pos;
}

// ─── POST /api/program/create ─────────────────────────────────────────────────
static int api_program_create(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) {
        jb_obj_open(&j, 0); jb_str(&j, "error", "missing body");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    struct SLSVallocRequest req;
    req.owner_uid = 0;
    req.perm_mask = PERM_READ | PERM_EXECUTE | PERM_OWNER;
    req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
    req.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
    req.type      = OBJ_TYPE_PROGRAM;
    req.name[0]   = '\0';
    json_str(body, "name", req.name, OBJECT_NAME_LEN);
    req.size_pages = (uint32_t)json_int(body, "pages");
    if (!req.name[0] || !req.size_pages) {
        jb_obj_open(&j, 0); jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "name and pages required");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    uint64_t id = sys_sls_valloc(&req);
    jb_obj_open(&j, 0);
    if (id) {
        /* Step 4: seed metadata records so the full DB1-DB7 hook chain
           (journal, lock, index, constraint, MQT) fires on every
           subsequent upload and spawn lifecycle event. */
        struct SLSRecordRequest mr;
        int i; for (i=0;i<OBJECT_NAME_LEN-1&&req.name[i];i++) mr.name[i]=req.name[i]; mr.name[i]='\0';
        mr.key[0]='\0'; mr.value[0]='\0';
        /* status */
        mr.key[0]='s'; mr.key[1]='t'; mr.key[2]='a'; mr.key[3]='t'; mr.key[4]='u'; mr.key[5]='s'; mr.key[6]='\0';
        mr.value[0]='c'; mr.value[1]='r'; mr.value[2]='e'; mr.value[3]='a'; mr.value[4]='t'; mr.value[5]='e'; mr.value[6]='d'; mr.value[7]='\0';
        sys_sls_insert(&mr);
        /* binary_size */
        const char* bsk = "binary_size"; for (i=0;bsk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=bsk[i]; mr.key[i]='\0';
        mr.value[0]='0'; mr.value[1]='\0';
        sys_sls_insert(&mr);
        /* format */
        const char* fmk = "format"; for (i=0;fmk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=fmk[i]; mr.key[i]='\0';
        mr.value[0]='n'; mr.value[1]='o'; mr.value[2]='n'; mr.value[3]='e'; mr.value[4]='\0';
        sys_sls_insert(&mr);
        /* last_pid */
        const char* lpk = "last_pid"; for (i=0;lpk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=lpk[i]; mr.key[i]='\0';
        mr.value[0]='0'; mr.value[1]='\0';
        sys_sls_insert(&mr);
        jb_str(&j, "ok",   "true");     jb_putc(&j, ',');
        jb_hex(&j, "object_id", id);    jb_putc(&j, ',');
        jb_str(&j, "type", "PROGRAM");
    } else {
        jb_str(&j, "ok",    "false");   jb_putc(&j, ',');
        jb_str(&j, "error", "valloc failed");
    }
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── POST /api/program/upload ─────────────────────────────────────────────────
// Body: {"name":"<obj>","hex":"<hex-bytes>","offset":N,"last":0|1}
//   hex:    lower- or upper-case hex pairs of raw binary bytes
//   offset: byte offset into the program binary this chunk starts at
//   last:   1 = final chunk; triggers size finalisation in binary store
static int api_program_upload(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) {
        jb_obj_open(&j, 0); jb_str(&j, "error", "missing body");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    /* UPLOAD_CHUNK_MAX = 1024 bytes = 2048 hex chars; static avoids stack bloat */
    static char hex_buf[UPLOAD_CHUNK_MAX * 2 + 4];
    static struct SLSUploadRequest ureq;
    hex_buf[0] = '\0';
    ureq.object_name[0] = '\0';
    json_str(body, "name", ureq.object_name, PROC_NAME_LEN);
    json_str(body, "hex",  hex_buf, (int)sizeof(hex_buf));
    int offset = json_int(body, "offset");
    int last   = json_int(body, "last");
    if (!ureq.object_name[0] || !hex_buf[0]) {
        jb_obj_open(&j, 0); jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "name and hex required");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    ureq.byte_offset = (uint32_t)(offset >= 0 ? offset : 0);
    ureq.is_last     = (uint8_t)(last ? 1 : 0);
    ureq.chunk_len   = (uint32_t)hex_decode(hex_buf, ureq.chunk, UPLOAD_CHUNK_MAX);
    jb_obj_open(&j, 0);
    if (ureq.chunk_len == 0) {
        jb_str(&j, "ok",    "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "hex decode produced zero bytes");
    } else {
        uint64_t rc = sys_sls_upload_binary(&ureq);
        jb_str(&j,  "ok",           rc == 0 ? "true" : "false"); jb_putc(&j, ',');
        jb_uint(&j, "bytes_written", (uint64_t)ureq.chunk_len);   jb_putc(&j, ',');
        jb_uint(&j, "offset",        (uint64_t)ureq.byte_offset); jb_putc(&j, ',');
        jb_str(&j,  "final",         ureq.is_last ? "true" : "false");
        /* Step 4: on the final chunk, write metadata records so journal,
           index, and MQT machinery see the completed upload. */
        if (rc == 0 && ureq.is_last) {
            /* Discover binary size + format from the store */
            uint32_t tot_bytes = 0;
            const char* fmt_str = "flat";
            for (int b = 0; b < MAX_SERVICE_BINARIES; b++) {
                if (service_binaries[b].active &&
                    !strcmp(service_binaries[b].object_name, ureq.object_name)) {
                    tot_bytes = service_binaries[b].size;
                    fmt_str   = binary_format_name(&service_binaries[b]);
                    break;
                }
            }
            struct SLSRecordRequest mr;
            int i; for (i=0;i<PROC_NAME_LEN-1&&ureq.object_name[i];i++) mr.name[i]=ureq.object_name[i]; mr.name[i]='\0';
            /* binary_size */
            const char* bsk="binary_size"; for (i=0;bsk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=bsk[i]; mr.key[i]='\0';
            prog_u64_to_str((uint64_t)tot_bytes, mr.value, RECORD_VAL_LEN);
            sys_sls_update(&mr);
            /* format */
            const char* fmk="format"; for (i=0;fmk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=fmk[i]; mr.key[i]='\0';
            for (i=0;fmt_str[i]&&i<RECORD_VAL_LEN-1;i++) mr.value[i]=fmt_str[i]; mr.value[i]='\0';
            sys_sls_update(&mr);
            /* status -> ready */
            const char* stk="status"; for (i=0;stk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=stk[i]; mr.key[i]='\0';
            mr.value[0]='r'; mr.value[1]='e'; mr.value[2]='a'; mr.value[3]='d'; mr.value[4]='y'; mr.value[5]='\0';
            sys_sls_update(&mr);
        }
    }
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── POST /api/program/spawn ──────────────────────────────────────────────────
// Gap Remediation Phase F: req_uid now threads through to program_load() as
// the real caller identity, replacing a hardcoded 0 (PARTITION_SYSTEM/kernel
// identity) named as its own separate gap in docs/AeroSLS-Gap-Analysis-v0.1.md
// §5 and again in the Phase B addendum's own /api/sql comment -- every
// spawned process was silently owned by the kernel regardless of who
// actually authenticated the request, meaning catalog_check_access()'s
// PERM_EXECUTE check inside program_spawn() (kernel/process.c) was checking
// the wrong identity, and every spawned process landed in PARTITION_SYSTEM
// instead of the caller's own partition (kernel/partition.c's
// partition_get_for_uid()). Same req_uid plumbing api_table_create_post()/
// api_sql_post()/api_agent_create() already established -- copying an
// existing pattern, not inventing one.
static int api_program_spawn_handler(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) {
        jb_obj_open(&j, 0); jb_str(&j, "error", "missing body");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    char pname[OBJECT_NAME_LEN];
    pname[0] = '\0';
    json_str(body, "name", pname, OBJECT_NAME_LEN);
    if (!pname[0]) {
        jb_obj_open(&j, 0); jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", "name required");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }
    /* program_load() validates OBJ_TYPE_PROGRAM, calls program_spawn()
       which maps pages into a fresh PML4 and enters Ring-3. */
    uint64_t pid = program_load(pname, req_uid);
    jb_obj_open(&j, 0);
    jb_str(&j,  "ok",  pid ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "pid", pid);
    /* Step 4: on successful spawn, update metadata records so journal
       captures the before/after audit trail and MQTs auto-refresh. */
    if (pid) {
        struct SLSRecordRequest mr;
        int i; for (i=0;i<OBJECT_NAME_LEN-1&&pname[i];i++) mr.name[i]=pname[i]; mr.name[i]='\0';
        /* status -> running */
        const char* stk="status"; for (i=0;stk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=stk[i]; mr.key[i]='\0';
        mr.value[0]='r'; mr.value[1]='u'; mr.value[2]='n'; mr.value[3]='n'; mr.value[4]='i'; mr.value[5]='n'; mr.value[6]='g'; mr.value[7]='\0';
        sys_sls_update(&mr);
        /* last_pid */
        const char* lpk="last_pid"; for (i=0;lpk[i]&&i<RECORD_KEY_LEN-1;i++) mr.key[i]=lpk[i]; mr.key[i]='\0';
        prog_u64_to_str(pid, mr.value, RECORD_VAL_LEN);
        sys_sls_update(&mr);
    }
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── Stream binary download ────────────────────────────────────────────────────
static void http_respond_stream(int conn, struct StreamEntry* se) {
    char hdr[384]; int hp=0;
    const char* sl="HTTP/1.1 200 OK\r\n"; while(*sl) hdr[hp++]=*sl++;
    const char* ct="Content-Type: "; while(*ct) hdr[hp++]=*ct++;
    const char* mt=se->mime_type[0] ? se->mime_type : "application/octet-stream";
    while(*mt) hdr[hp++]=*mt++;
    hdr[hp++]='\r'; hdr[hp++]='\n';
    const char* cd="Content-Disposition: attachment; filename=\"";
    while(*cd) hdr[hp++]=*cd++;
    const char* fn=se->name; while(*fn) hdr[hp++]=*fn++;
    hdr[hp++]='"'; hdr[hp++]='\r'; hdr[hp++]='\n';
    const char* cl="Content-Length: "; while(*cl) hdr[hp++]=*cl++;
    char tmp[12]; int tl=0; uint32_t v=se->size;
    if (!v){tmp[tl++]='0';} else {while(v){tmp[tl++]=(char)('0'+v%10);v/=10;}}
    for(int i=tl-1;i>=0;i--) hdr[hp++]=tmp[i];
    hdr[hp++]='\r'; hdr[hp++]='\n';
    const char* co=g_cors_origin_hdr; while(*co) hdr[hp++]=*co++;
    hdr[hp++]='\r'; hdr[hp++]='\n';
    http_send(conn, hdr, (uint32_t)hp);
    /* Send content frame-by-frame; lazy-load from NVMe if frame not in RAM
       (happens on first download after a reboot). */
    if (se->size > 0) {
        uint32_t remaining = se->size;
        for (uint32_t fi = 0; fi < STREAM_MAX_FRAMES && remaining > 0; fi++) {
            uint8_t* frame_data = se->frames[fi];
            if (!frame_data) {
                frame_data = stream_lazy_load_frame(se, fi);
                if (!frame_data) break;  /* NVMe read failed — truncate response */
            }
            uint32_t to_send = remaining < 4096u ? remaining : 4096u;
            http_send(conn, (const char*)frame_data, to_send);
            remaining -= to_send;
        }
    }
}

// ─── Program binary download ───────────────────────────────────────────────────
static void http_respond_program_binary(int conn, struct ServiceBinary* sb) {
    char hdr[512]; int hp = 0;
    const char* sl = "HTTP/1.1 200 OK\r\n"; while (*sl) hdr[hp++] = *sl++;
    const char* ct = "Content-Type: application/octet-stream\r\n";
    while (*ct) hdr[hp++] = *ct++;
    const char* cd = "Content-Disposition: attachment; filename=\"";
    while (*cd) hdr[hp++] = *cd++;
    const char* fn = sb->object_name; while (*fn) hdr[hp++] = *fn++;
    hdr[hp++] = '"'; hdr[hp++] = '\r'; hdr[hp++] = '\n';
    /* X-Binary-Format header so the downloader knows ELF vs flat vs SIMI */
    const char* xbf = "X-Binary-Format: ";
    while (*xbf) hdr[hp++] = *xbf++;
    const char* fmt = binary_format_name(sb);
    while (*fmt) hdr[hp++] = *fmt++;
    hdr[hp++] = '\r'; hdr[hp++] = '\n';
    const char* cl = "Content-Length: "; while (*cl) hdr[hp++] = *cl++;
    char tmp[12]; int tl = 0; uint32_t v = sb->size;
    if (!v) { tmp[tl++] = '0'; } else { while (v) { tmp[tl++] = (char)('0' + v % 10); v /= 10; } }
    for (int i = tl - 1; i >= 0; i--) hdr[hp++] = tmp[i];
    hdr[hp++] = '\r'; hdr[hp++] = '\n';
    const char* co = g_cors_origin_hdr; while (*co) hdr[hp++] = *co++;
    hdr[hp++] = '\r'; hdr[hp++] = '\n';
    http_send(conn, hdr, (uint32_t)hp);
    if (sb->size > 0)
        http_send(conn, (const char*)sb->data, sb->size);
}

// ─── POST /api/stream/create ──────────────────────────────────────────────────
// Multitenant Isolation Gap Analysis §5 item 3: req_uid now threaded
// through to stream_create() as the real caller identity (was silently
// dropped -- every stream was created as owner_uid 0 regardless of who
// actually authenticated the request), matching every other creation
// route's own "caller_uid comes from the auth gate, never the request body" posture.
static int api_stream_create(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j={buf,0,max};
    if (!body){jb_obj_open(&j,0);jb_str(&j,"error","missing body");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    char sname[STREAM_NAME_LEN], smime[STREAM_MIME_LEN];
    sname[0]=smime[0]='\0';
    json_str(body,"name",sname,STREAM_NAME_LEN);
    json_str(body,"mime",smime,STREAM_MIME_LEN);
    if (!sname[0]){jb_obj_open(&j,0);jb_str(&j,"ok","false");jb_putc(&j,',');jb_str(&j,"error","name required");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    int rc=stream_create(req_uid,sname,smime);
    jb_obj_open(&j,0);
    jb_str(&j,"ok",rc==0?"true":"false");jb_putc(&j,',');
    jb_str(&j,"name",sname);
    if(rc!=0){jb_putc(&j,',');jb_str(&j,"error",rc==2?"already exists":"store full");}
    jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;
}

// ─── POST /api/stream/upload ──────────────────────────────────────────────────
static int api_stream_upload(const char* body, char* buf, int max) {
    JSONBuf j={buf,0,max};
    if (!body){jb_obj_open(&j,0);jb_str(&j,"error","missing body");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    static char st_hex[UPLOAD_CHUNK_MAX*2+4];
    static uint8_t st_chunk[UPLOAD_CHUNK_MAX];
    char sname[STREAM_NAME_LEN]; sname[0]=st_hex[0]='\0';
    json_str(body,"name",sname,STREAM_NAME_LEN);
    json_str(body,"hex", st_hex,(int)sizeof(st_hex));
    int offset=json_int(body,"offset"), last=json_int(body,"last");
    if (!sname[0]||!st_hex[0]){jb_obj_open(&j,0);jb_str(&j,"ok","false");jb_putc(&j,',');jb_str(&j,"error","name and hex required");jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;}
    size_t decoded=hex_decode(st_hex,st_chunk,UPLOAD_CHUNK_MAX);
    jb_obj_open(&j,0);
    if(decoded==0){jb_str(&j,"ok","false");jb_putc(&j,',');jb_str(&j,"error","hex decode produced zero bytes");}
    else{
        int rc=stream_write_chunk(sname,st_chunk,(uint32_t)decoded,(uint32_t)(offset>=0?offset:0),(uint8_t)(last?1:0));
        jb_str(&j,"ok",rc==0?"true":"false");jb_putc(&j,',');
        jb_uint(&j,"bytes_written",(uint64_t)decoded);jb_putc(&j,',');
        jb_str(&j,"final",last?"true":"false");
    }
    jb_obj_close(&j);j.buf[j.pos]='\0';return j.pos;
}

// ─── POST /api/valloc ─────────────────────────────────────────────────────────
static int api_valloc_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVallocRequest req;
    req.owner_uid = 0; req.perm_mask = 0;
    req.partition_id = 0;   // Phase 8: 0 = default to owner_uid's own partition
    // VectorStore Gap Analysis §3: database_id was left unset here
    // (uninitialized stack garbage) until this fix -- every valloc call
    // site except sql_exec.c's CREATE TABLE path had the same bug. 0 (NONE)
    // unless an optional "database" name is given, resolved the same way
    // sql_exec.c's "IN DATABASE" clause does. This is also the direct path
    // a vector collection's underlying catalog object goes through (see
    // api_vec_create_post()'s own comment on requiring POST /api/valloc
    // first) -- tagging it here means catalog_check_access()'s existing
    // database_check_access() fallback already covers the resulting vector
    // collection for free, no vecstore.c changes needed.
    req.database_id = 0;
    char dbname[OBJECT_NAME_LEN];
    dbname[0] = '\0';
    json_str(body, "database", dbname, OBJECT_NAME_LEN);
    if (dbname[0]) req.database_id = database_find_id(dbname);
    json_str(body, "name", req.name, OBJECT_NAME_LEN);
    req.type       = (SLSObjectType)json_int(body, "type");
    req.size_pages = (uint32_t)json_int(body, "pages");
    if (!req.name[0] || !req.size_pages) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name and pages required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t id = sys_sls_valloc(&req);
    jb_obj_open(&j,0);
    if (id) {
        jb_str(&j,"ok","true"); jb_putc(&j,',');
        jb_hex(&j,"object_id",id);
    } else {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","valloc failed");
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/schema — Gap Remediation Phase H ────────────────────────────────
// Closes the "smaller, separate, still-open gap" api_table_create_post()'s
// own comment (right below) has flagged since Phase B: sys_sls_schema_set()
// (SYS_SLS_SCHEMA_SET, object_catalog.c) was already syscall/shell-reachable
// ("schema set <object> <key> <type>", user/shell.c) but had no HTTP route,
// meaning the three-step "create a real SQL table" flow (valloc -> schema
// set (one field at a time) -> POST /api/tables) had its middle step missing
// over HTTP -- the frontend could valloc an empty object and promote it, but
// never actually define its columns. Body: {"name": "<object_name>",
// "columns": [{"name":"<col>", "type":"STRING|UINT64|INT|FLOAT|BOOL"}, ...]}.
// Loops sys_sls_schema_set() once per column (that syscall's own one-field-
// at-a-time contract, not reinvented here), same json_array_object_at()
// array-of-objects idiom /api/vec/join already established, and the same
// type-string aliasing shell.c's own "schema set" command already uses
// (INT accepted as UINT64's alias; unrecognized/absent type defaults to
// STRING). Stops at the first column that fails (object not found, or
// object already promoted to row-store -- schema is frozen post-promotion,
// see object_catalog.c's own guard comment) rather than silently applying a
// partial schema and calling it success.
static int api_schema_set_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSSchemaRequest req;
    json_str(body, "name", req.object_name, OBJECT_NAME_LEN);
    if (!req.object_name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    char colbuf[192];
    char typ[16];
    uint32_t applied = 0;
    int failed = 0;
    for (int n = 0; n < SCHEMA_MAX_FIELDS; n++) {
        if (!json_array_object_at(body, "columns", n, colbuf, (int)sizeof(colbuf))) break;
        json_str(colbuf, "name", req.key, RECORD_KEY_LEN);
        typ[0] = '\0';
        json_str(colbuf, "type", typ, (int)sizeof(typ));
        if      (!strcmp(typ, "UINT64") || !strcmp(typ, "INT")) req.type = FIELD_TYPE_UINT64;
        else if (!strcmp(typ, "FLOAT"))                          req.type = FIELD_TYPE_FLOAT;
        else if (!strcmp(typ, "BOOL"))                           req.type = FIELD_TYPE_BOOL;
        else                                                      req.type = FIELD_TYPE_STRING;
        if (!req.key[0]) { failed = 1; break; }
        uint64_t rc = sys_sls_schema_set(&req);
        if (rc != 0) { failed = 1; break; }
        applied++;
    }
    jb_obj_open(&j,0);
    jb_str(&j, "ok", (!failed && applied) ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.object_name); jb_putc(&j,',');
    jb_uint(&j, "columns_set", applied);
    if (failed) {
        jb_putc(&j,',');
        jb_str(&j, "error", applied == 0 && !req.key[0]
                             ? "malformed or empty columns array"
                             : "schema_set failed (object not found, or already promoted to row-store)");
        jb_putc(&j,',');
        jb_str(&j, "failed_column", req.key);
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/tables — Gap Remediation Phase B ───────────────────────────────
// The live path rowstore_create_table() never had: before this, the only
// way to promote a valloc'd + schema'd catalog object into a real row-set
// table was a host test calling rowstore_create_table() directly (see
// docs/AeroSLS-Gap-Analysis-v0.1.md §2/§5). Body: {"name": "<table_name>"}.
// Does not valloc or schema-set for the caller -- POST /api/valloc
// (type=1/DB_TABLE) covers the first step, POST /api/schema (Gap
// Remediation Phase H, above) covers the second.
static int api_table_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSRowstoreCreateTableRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.table_name, OBJECT_NAME_LEN);
    if (!req.table_name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_rowstore_create_table(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.table_name);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/sql — Gap Remediation Phase B ──────────────────────────────────
// The live path sql_execute() never had over HTTP: SYS_SLS_SQL_EXECUTE
// (Phase 22) was already syscall/shell-reachable, but net/http.c had zero
// "sql" routes at all (docs/AeroSLS-Gap-Analysis-v0.1.md §5) -- confirmed
// by direct inspection before this phase, not assumed. Body:
// {"query": "<statement>"}. Runs one autocommit statement under req_uid
// (the real bearer-token identity, matching the agent/workflow POST routes'
// own req_uid convention -- not a hardcoded uid, unlike the program-spawn
// routes' own separately-named gap in the analysis doc). SELECT results are
// fetched and embedded directly in this one response (up to however many
// rows fit in the shared resp_body buffer -- this file's existing, established
// truncate-silently-at-the-byte-buffer convention, same as every other
// dump-a-collection endpoint here; no new pagination route is added).
struct sql_row_json_ctx { JSONBuf* j; int first; };
static void sql_row_to_json_cb(struct RowId id, const struct RowValues* v, void* ctxp) {
    (void)id;
    struct sql_row_json_ctx* ctx = (struct sql_row_json_ctx*)ctxp;
    if (!ctx->first) jb_putc(ctx->j, ',');
    ctx->first = 0;
    jb_arr_open(ctx->j, 0);
    for (uint32_t i = 0; i < v->count; i++) {
        if (i) jb_putc(ctx->j, ',');
        // Phase 4 (SQL Feature-Parity Roadmap): a real NULL column now
        // round-trips as JSON `null`, not an indistinguishable empty
        // string -- the round-trip guarantee this phase exists to give.
        if (v->null_mask & (1u << i)) jb_raw(ctx->j, "null");
        else                          jb_esc_str(ctx->j, v->values[i]);
    }
    jb_arr_close(ctx->j);
}
// ─── Kernel-Side Shell Refactor (docs/AeroSLS-Web-Terminal-Plan-v0.1.md §10.4) ─
// Now a real header (user/shell.h), not a bare extern -- Architectural
// Phase 2 (docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md) added a struct
// passed by pointer across this translation-unit boundary, and that needs
// an identical layout on both sides, which only a shared header guarantees.
#include "../user/shell.h"

// Must match the constant of the same name in user/shell.c's sls_shell_loop()
// -- no shared header exists for this one value, so both copies carry this
// cross-reference instead (see shell.c's own comment on the same constant).
#define SHELL_EXEC_OUT_CAP 8192

// ─── Architectural Phase 2: one ShellSession per authenticated uid ───────────
// Replaces the single shared, global shell.c session this route used to run
// every caller through regardless of who they were (see the superseded
// comment this replaced, below, and shell.h's comment for the full
// rationale). Sized off AUTH_MAX_TOKENS since that's already this
// codebase's cap on distinct concurrently-valid identities.
#define HTTP_SHELL_SESSION_MAX AUTH_MAX_TOKENS
static struct ShellSession http_shell_sessions[HTTP_SHELL_SESSION_MAX];
static uint8_t             http_shell_session_used[HTTP_SHELL_SESSION_MAX];
static uint32_t            http_shell_session_uid[HTTP_SHELL_SESSION_MAX];

// Finds (or lazily creates) the persistent ShellSession for `uid`, so a
// `tx begin` on one HTTP request is found by `tx commit` on a later one --
// necessary because Phase 1 makes every HTTP request its own short-lived TCP
// connection, so there's no connection to hang session state off of instead.
// Two different uids can never collide since uid is both the lookup key and
// (in api_shell_exec_post() below) reseeded from the bearer token on every
// call rather than trusted from anything stored. Returns 0 if the table is
// full (HTTP_SHELL_SESSION_MAX concurrent distinct identities all with an
// in-progress shell session at once) -- not expected at MVP scale, but
// handled rather than silently reusing the wrong slot.
static struct ShellSession* http_shell_session_for(uint32_t uid) {
    for (int i = 0; i < HTTP_SHELL_SESSION_MAX; i++) {
        if (http_shell_session_used[i] && http_shell_session_uid[i] == uid)
            return &http_shell_sessions[i];
    }
    for (int i = 0; i < HTTP_SHELL_SESSION_MAX; i++) {
        if (!http_shell_session_used[i]) {
            http_shell_session_used[i] = 1;
            http_shell_session_uid[i]  = uid;
            http_shell_sessions[i].uid = uid;
            http_shell_sessions[i].gid = uid;  // no per-request gid to seed from; cosmetic only, see shell.h
            http_shell_sessions[i].tx_id = 0;
            return &http_shell_sessions[i];
        }
    }
    return 0;
}

// Runs the FULL ~90-command shell.c dispatch, not a curated subset -- this
// is what closes every command on §3's "Missing" list (login, role set,
// grant, revoke, chmod, auth create/list/revoke, seal, write, demo,
// upload, load, loader list, svc crash/restart, proc kill, ipc post/stat,
// journal create/purge, tier demote/promote, vfree, workflow addstep,
// webapp set/list/append) without 24 individual new routes -- see §10.6.
static int api_shell_exec_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct ShellSession* sess = http_shell_session_for(req_uid);
    if (!sess) {
        jb_obj_open(&j,0); jb_str(&j,"error","too many concurrent shell sessions"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    sess->uid = req_uid;  // always reseed identity from the bearer token, never trust stored state
    char command[256];
    json_str(body, "command", command, sizeof(command));
    static char shell_out[SHELL_EXEC_OUT_CAP];
    int recognized = sls_shell_execute(command, sess, shell_out, sizeof(shell_out));
    jb_obj_open(&j, 0);
    jb_str(&j, "ok", recognized ? "true" : "false"); jb_putc(&j, ',');
    jb_str_multiline(&j, "output", shell_out);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/qemu/bench, POST /api/qemu/paging ───────────────────────────
// QEMU-SLS is the one major subsystem with no HTTP surface: it was reachable
// only through `qemu bench` / `qemu paging` on the serial console, and the
// frontend retired its /api/shell/exec passthrough when every shell command got
// a purpose-built JSON route. So the browser could not see it at all.
//
// These report the SAME figures the console prints, as fields rather than as
// text. That distinction is the point. A UI that regexes "[SLS-BENCH] CODE
// %llu bytes" breaks silently the first time someone rewords a log line, and
// this project has spent enough time on numbers that drifted apart while
// everything still looked fine.
//
// Every value below is read from a global the bench already sets
// (sls-launcher.h) -- nothing is recomputed here, so the route cannot disagree
// with the console about the same run.
//
// DELIBERATELY NO /api/qemu/run. `qemu run <hex>` feeds arbitrary bytes to the
// guest frontend and the shadow-paging fault path. The frontend implements 18
// opcodes, so it could not run a real binary anyway, and the risk/benefit of
// exposing it over HTTP is the wrong way round.
// Declared locally rather than by including ../qemu/sls/sls-launcher.h:
// X86_CFLAGS carries -I. -Ikernel -Iarch/x86 -Inet and no path into the QEMU
// tree, so that header is not reachable from here. user/shell.c declares the
// same symbols the same way at its own `qemu` handlers. Definitions and full
// commentary live in sls-launcher.h.
extern int      sls_bench_load_path(uint32_t n_loads, uint64_t *cycles, uint32_t *insns);
extern int      sls_bench_load_path_at(uint32_t n_loads, uint64_t prog_gpa,
                                       uint64_t *cycles, uint32_t *insns);
extern int      sls_test_guest_paging(void);
extern int      sls_test_guest_invl(void);
extern int      sls_test_guest_selfmod(void);
extern int      sls_test_guest_compiled(void);
extern int      sls_test_guest_elf(void);
extern int      sls_test_guest_elf_reject(void);
extern int      sls_softmmu_enabled(void);
extern uint64_t sls_heap_used(void);
extern uint64_t sls_heap_total(void);
extern uint64_t sls_last_translate_cycles;
extern uint64_t sls_last_exec_cycles;
extern uint64_t sls_last_code_bytes;
extern uint32_t sls_last_tb_count;
extern uint32_t sls_last_tcache_hits;
extern uint32_t sls_last_tcache_misses;
extern uint64_t sls_last_arena_consumed;

static int api_qemu_bench_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };

    // Default 256 matches user/shell.c's `qemu bench`. The launcher refuses
    // anything above 1022 (16 blocks of the decoder's 64-insn per-TB budget,
    // minus the two setup/halt instructions) -- the old TCG_MAX_INSNS - 2
    // cap assumed one TB held the whole program, which stopped being true
    // when the decoder split blocks at 64. Validated HERE rather than left
    // to the launcher: its refusal goes to the serial console, which an HTTP
    // caller never sees, so it would look like an empty success.
    uint32_t loads = 256;
    if (body) {
        uint64_t v = json_uint64(body, "loads");
        if (v) loads = (uint32_t)v;
    }
    if (loads < 1 || loads > 1022) {
        jb_obj_open(&j, 0);
        jb_str(&j, "error", "loads out of range");
        jb_putc(&j, ',');
        jb_uint(&j, "min", 1); jb_putc(&j, ',');
        jb_uint(&j, "max", 1022); jb_putc(&j, ',');
        jb_uint(&j, "requested", (uint64_t)loads);
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }

    uint64_t cycles = 0;
    uint32_t insns  = 0;
    int rc = sls_bench_load_path(loads, &cycles, &insns);

    // ─── A refused run must SAY it was refused ─────────────────────────────
    // The launcher explains itself on the serial console. An HTTP caller never
    // sees that, so without this the response was ok:"false" and nothing else
    // -- the exact failure the `loads` range check above was written to avoid,
    // repeated one branch further down.
    //
    // The timing and derived fields are dropped too. A refused launch was
    // measured at 37,000,684 total_cycles (the launcher-init cost, bracketed by
    // the bench's own rdtsc) and reported cold:"true", because `cold` is
    // derived from tcache_hits == 0 and no run means no hits. Both look like
    // measurements of something. Neither is.
    if (rc < 0) {
        extern int qemu_sls_guest_paging_on;
        jb_obj_open(&j, 0);
        jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        if (qemu_sls_guest_paging_on) {
            jb_str(&j, "error",
                   "refused: a previous guest enabled paging and this kernel has no reset path");
            jb_putc(&j, ',');
            jb_str(&j, "remedy", "POST /api/node/reboot with {\"confirm\":\"reboot\"}");
            jb_putc(&j, ',');
            jb_str(&j, "defect", "AeroSLS-QEMU-SLS-Guest-Paging-Reset-Defect-v0.1.md");
        } else {
            jb_str(&j, "error", "the guest launch was refused; see the node console for the reason");
        }
        jb_putc(&j, ',');
        jb_uint(&j, "loads", (uint64_t)loads); jb_putc(&j, ',');
        // Kept because they describe the node, not the run that did not happen.
        jb_uint(&j, "arena_used",  sls_heap_used());  jb_putc(&j, ',');
        jb_uint(&j, "arena_total", sls_heap_total()); jb_putc(&j, ',');
        jb_str(&j, "softmmu", sls_softmmu_enabled() ? "on" : "off");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }

    // rc >= 0 from here: the run happened, so every field below describes it.
    jb_obj_open(&j, 0);
    jb_str(&j, "ok", "true");                                          jb_putc(&j, ',');
    jb_uint(&j, "loads",            (uint64_t)loads);                  jb_putc(&j, ',');
    jb_uint(&j, "insns",            (uint64_t)insns);                  jb_putc(&j, ',');
    jb_uint(&j, "total_cycles",     cycles);                           jb_putc(&j, ',');
    jb_uint(&j, "exec_cycles",      sls_last_exec_cycles);             jb_putc(&j, ',');
    jb_uint(&j, "translate_cycles", sls_last_translate_cycles);        jb_putc(&j, ',');
    jb_uint(&j, "blocks",           (uint64_t)sls_last_tb_count);      jb_putc(&j, ',');
    // The A/B figure. Deterministic across every sample this project has taken,
    // which is why it and not the cycle columns is the one to quote.
    jb_uint(&j, "code_bytes",       sls_last_code_bytes);              jb_putc(&j, ',');
    jb_uint(&j, "tcache_hits",      (uint64_t)sls_last_tcache_hits);   jb_putc(&j, ',');
    jb_uint(&j, "tcache_misses",    (uint64_t)sls_last_tcache_misses); jb_putc(&j, ',');
    // Same test the console uses to print "cold: every block was compiled".
    // Derived here so the UI does not re-implement the rule and drift from it.
    jb_str(&j, "cold", sls_last_tcache_hits == 0 ? "true" : "false");  jb_putc(&j, ',');
    // Read, not sampled. A first version bracketed the call with
    // sls_heap_used() and reported 1,508,448 where the console said 1,507,392
    // for the same run: heap_before inside the bench is captured AFTER
    // sls_launcher_init() allocates, so an external sampler also counts the
    // one-time init. It was only wrong on the first bench of a boot, which is
    // the worst kind of wrong -- every retest agreed.
    jb_uint(&j, "arena_consumed",   sls_last_arena_consumed);          jb_putc(&j, ',');
    jb_uint(&j, "arena_used",       sls_heap_used());                  jb_putc(&j, ',');
    jb_uint(&j, "arena_total",      sls_heap_total());                 jb_putc(&j, ',');
    // Which side of the A/B produced these numbers. Without it a caller can
    // compare an ON run against an OFF run and see a 5.4x "improvement" that is
    // only the build flag.
    jb_str(&j, "softmmu", sls_softmmu_enabled() ? "on" : "off");
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── POST /api/qemu/bench_sweep ───────────────────────────────────────────
// The tcache round-trip guard's second shape. The single bench exercises
// ONE program shape: one loads value, one block count. A sweep places each
// loads value's program at its own guest GPA via sls_bench_load_path_at(),
// so several bench programs coexist in guest RAM -- and in the persistent
// translation cache -- without invalidating each other: every program owns
// its own page and its own tcache page digest, so a checkpoint after a
// cold sweep followed by a reboot and the same warm sweep hits EVERY
// value's blocks. That is the round-trip asserted at multiple block counts
// (tests/tcache_roundtrip_check.sh), not just the one 8-block shape.
//
// The response is an array of per-value objects, each carrying exactly the
// fields the single bench reports, read from the same sls_last_* globals so
// the route cannot disagree with the console about the same run. The sweep
// is capped at BENCH_SWEEP_MAX values and each loads value is validated the
// way the single bench does (1..TCG_MAX_INSNS-2), plus a distinctness check:
// two identical values would bench the same GPA twice and the second run's
// "hits" would be indistinguishable from the first's, which a round-trip
// assertion must not silently depend on.
#define BENCH_SWEEP_MAX 16
static int api_qemu_bench_sweep_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    uint32_t loads[BENCH_SWEEP_MAX];
    int n = body ? json_uint_array(body, "loads", loads, BENCH_SWEEP_MAX) : 0;

    const char* err = 0;
    if (n < 1)
        err = "missing or empty \"loads\" array";
    for (int i = 0; !err && i < n; i++) {
        if (loads[i] < 1 || loads[i] > 1022) { err = "loads out of range"; break; }
        for (int k = 0; k < i; k++)
            if (loads[k] == loads[i]) { err = "duplicate loads value"; break; }
    }
    if (err) {
        jb_obj_open(&j, 0);
        jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", err);
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }

    jb_obj_open(&j, 0);
    jb_str(&j, "ok", "true");        jb_putc(&j, ',');
    jb_uint(&j, "count", (uint64_t)n); jb_putc(&j, ',');
    jb_arr_open(&j, "results");
    for (int i = 0; i < n; i++) {
        uint64_t cycles = 0;
        uint32_t insns  = 0;
        /* 128 KiB per slot: the worst case (loads=1022) is a ~6 KiB program
         * plus a ~65 KiB buffer, so no two values touch a shared page. The
         * response carries the GPA so the guard can distinguish the sweep's
         * shape from a single-bench response. */
        uint64_t gpa = (uint64_t)(i + 1) * 0x20000;
        int rc = sls_bench_load_path_at(loads[i], gpa, &cycles, &insns);
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_str(&j, "ok", rc < 0 ? "false" : "true"); jb_putc(&j, ',');
        jb_uint(&j, "loads",            (uint64_t)loads[i]);      jb_putc(&j, ',');
        jb_uint(&j, "gpa",              gpa);                     jb_putc(&j, ',');
        jb_uint(&j, "insns",            (uint64_t)insns);         jb_putc(&j, ',');
        jb_uint(&j, "total_cycles",     cycles);                  jb_putc(&j, ',');
        jb_uint(&j, "exec_cycles",      sls_last_exec_cycles);    jb_putc(&j, ',');
        jb_uint(&j, "translate_cycles", sls_last_translate_cycles); jb_putc(&j, ',');
        jb_uint(&j, "blocks",           (uint64_t)sls_last_tb_count); jb_putc(&j, ',');
        jb_uint(&j, "code_bytes",       sls_last_code_bytes);     jb_putc(&j, ',');
        jb_uint(&j, "tcache_hits",      (uint64_t)sls_last_tcache_hits);   jb_putc(&j, ',');
        jb_uint(&j, "tcache_misses",    (uint64_t)sls_last_tcache_misses); jb_putc(&j, ',');
        jb_str(&j, "cold", sls_last_tcache_hits == 0 ? "true" : "false"); jb_putc(&j, ',');
        jb_uint(&j, "arena_consumed",   sls_last_arena_consumed); jb_putc(&j, ',');
        jb_uint(&j, "arena_used",       sls_heap_used());         jb_putc(&j, ',');
        jb_uint(&j, "arena_total",      sls_heap_total());        jb_putc(&j, ',');
        jb_str(&j, "softmmu", sls_softmmu_enabled() ? "on" : "off");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

// ─── POST /api/node/reboot ─────────────────────────────────────────────────
// Resets this node's machine. See kernel/node_reset.c for the mechanism and
// for why no checkpoint is taken.
//
// REQUIRES {"confirm":"reboot"} IN THE BODY. The bearer token already gates
// this, but a token is carried by every request a page makes -- including ones
// a browser retries on its own. A destructive action reachable at the same
// trust level as GET /api/health wants something that cannot be sent by
// accident, and a literal in the body cannot be.
//
// The client will usually see this request FAIL. That is expected: the reset
// lands before the response finishes crossing the wire. A caller should treat
// a dropped connection here as success and poll for the node coming back,
// rather than reporting an error the operator then investigates.
static int api_node_reboot_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    char confirm[16] = {0};
    if (body) json_str(body, "confirm", confirm, sizeof(confirm));

    if (strcmp(confirm, "reboot") != 0) {
        jb_obj_open(&j, 0);
        jb_str(&j, "error", "reboot requires {\"confirm\":\"reboot\"} in the body");
        jb_putc(&j, ',');
        jb_str(&j, "checkpointed", "false");
        jb_putc(&j, ',');
        jb_str(&j, "note", "nothing is checkpointed by this route -- run 'checkpoint' first if you want the state kept");
        jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
    }

    extern void sls_node_reset(void);
    kernel_serial_printf(
        "[HTTP] POST /api/node/reboot confirmed -- resetting node %u.\n",
        cluster_local_node_id());
    sls_node_reset();          /* does not return */

    /* Unreachable. Present so the function has a defined shape if the reset
     * ever fails to take -- node_reset halts in that case rather than
     * returning, so this is belt and braces, not a real path. */
    jb_obj_open(&j, 0);
    jb_str(&j, "error", "reset did not take");
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_paging_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // Passes only if the shadow walker resolved through the guest's OWN page
    // tables: the identity mapping would return 0, so a pass and a failure are
    // distinguishable rather than both looking like "the guest halted".
    int rc = sls_test_guest_paging();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_invl_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // §3.2 invalidation policy end to end: the guest edits its own PT page,
    // executes INVLPG, and reloads CR3; passes only if every stale shadow
    // translation was dropped, with each stage checked against its own magic.
    int rc = sls_test_guest_invl();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_compiled_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // Step 6.4's first real compiled guest: the gcc -static -nostdlib binary
    // embedded by the launcher (sls/guest/guest.c). Passes only if the guest
    // ran to HLT under the decoder and its result block matches the same
    // arithmetic in C. Until the helpers it crosses are implemented, a stub
    // halts the kernel and this endpoint never answers.
    int rc = sls_test_guest_compiled();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_elf_reject_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M5: a dynamic ELF (PT_INTERP) must be refused loudly with
    // no segment placed -- this kernel has no dynamic loader.
    int rc = sls_test_guest_elf_reject();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_elf_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M5: a real gcc -static ELF64 (sls/guest/hello.c) parsed and
    // placed by sls_elf64_load(), booted through the decoder, whose
    // write(1,...) syscall must print to serial and exit_group(0)
    // must return control to the launcher. Same shape as the
    // compiled endpoint: a halting stub means this never answers.
    int rc = sls_test_guest_elf();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_selfmod_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // Self-modifying guest code (paging off): the store to the code page must
    // fault, bump the page generation, and force a re-translation that serves
    // the patched bytes. Passes only if the guest halts with the patched
    // immediate in EAX.
    int rc = sls_test_guest_selfmod();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_rdclock_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M5: the input-and-clock gate -- sls/guest/rdclock.c compiled
    // -static and embedded by the launcher. The guest drains the
    // boot-data stream via read (syscall 0): head, short-read tail,
    // EOF, plus a nonzero-fd refusal; then clock_gettime (228) on
    // CLOCK_MONOTONIC twice around a spin (sane nsec, non-decreasing
    // total) and an unknown clockid refusal, then exit_group(0).
    int rc = sls_test_guest_rdclock();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_futex_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M5: the last shim-surface gate -- sls/guest/futex.c compiled
    // -static and embedded by the launcher. The guest exercises the
    // futex syscall (202, 6-arg): the WAIT fast path on a word mismatch
    // (-EAGAIN), the PRIVATE bit as a hint, WAKE returning the true
    // count 0, an unknown op (-EINVAL), and a matched-word wait with a
    // 20M-tick timeout that really sleeps (-ETIMEDOUT, with the
    // monotonic clock advanced by about the requested interval), then
    // exit_group(0).
    int rc = sls_test_guest_futex();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_faults_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M7: the fault-semantics gate -- the embedded faults fixture
    // (sls/guest/faults.c) is launched once per fault class (ud2->#UD,
    // div0->#DE, swapgs-at-CPL3->#GP, unmapped paged store->#PF with CR2);
    // each launch must record exactly the expected vector in the launcher's
    // fault record, then the test reports PASS.
    int rc = sls_test_guest_faults();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_sse2_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M6: the SSE2 scalar slice gate -- sls/guest/sse2.c compiled
    // -static WITHOUT the M1-M5 -mno-sse -msoft-float crutch and
    // embedded by the launcher. The x86-64 compiler ABI is SSE2 for
    // scalar floating point, so the fixture's double/float expressions
    // are real SSE2 instructions with runtime (volatile) operands: the
    // 128-bit aligned store, scalar add/sub/mul/div/sqrt, int<->double
    // converts, the float path, ucomisd branches, and the MXCSR
    // round-trip, then exit_group(0).
    int rc = sls_test_guest_sse2();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_brkmmap_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M5: the guest-heap shim gate -- sls/guest/brkmmap.c compiled
    // -static and embedded by the launcher. The guest demands brk
    // (syscall 12), mmap (9, the 6-arg R10/R8/R9 form) and munmap (11)
    // from the Linux-compat shim: grow/shrink the break with a pattern
    // round-trip, map an anonymous private page (zero-read, write,
    // read-back), unmap it (a double-unmap must fail), map again, then
    // exit_group(0). Same shape as the other qemu gates.
    int rc = sls_test_guest_brkmmap();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_qemu_tls_post(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    // M5: the PT_TLS gate -- sls/guest/tls.c compiled -static and embedded
    // by the launcher. The loader must place the TLS template (which the
    // linker parks OUTSIDE every PT_LOAD), zero .tbss, build the TCB
    // (tcbhead_t with self/dt/multiple_threads/stack_guard), and the
    // launcher must install the FS base for the guest's local-exec %fs
    // reads to land. The guest checks every offset and its own TCB, then
    // exit_group(0). Same shape as the compiled endpoint.
    int rc = sls_test_guest_tls();
    jb_obj_open(&j, 0);
    jb_str(&j, "ok",   rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str(&j, "pass", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_uint(&j, "rc", (uint64_t)(rc < 0 ? (uint64_t)(-rc) : (uint64_t)rc));
    jb_obj_close(&j); j.buf[j.pos] = '\0'; return j.pos;
}

static int api_sql_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSSqlRequest req;
    req.caller_uid = req_uid;
    json_str(body, "query", req.sql_text, SQL_MAX_TEXT_LEN);
    uint64_t rc = sys_sls_sql_execute(&req);

    jb_obj_open(&j, 0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j, ',');
    if (rc != 0) {
        jb_uint(&j, "error_code", (uint64_t)req.result.error); jb_putc(&j, ',');
        jb_str(&j, "error", req.result.error_msg);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (req.result.kind == SQL_STMT_SELECT) {
        jb_uint(&j, "row_count", req.result.row_count); jb_putc(&j, ',');
        jb_str(&j, "truncated", req.result.truncated ? "true" : "false"); jb_putc(&j, ',');
        jb_arr_open(&j, "columns");
        for (uint32_t c = 0; c < req.result.column_count; c++) {
            if (c) jb_putc(&j, ',');
            jb_esc_str(&j, req.result.columns[c]);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_arr_open(&j, "rows");
        struct sql_row_json_ctx ctx = { &j, 1 };
        cursor_fetch_rows(req.result.cursor_id, req.result.row_count, sql_row_to_json_cb, &ctx);
        cursor_close(req.result.cursor_id);
        jb_arr_close(&j);
    } else {
        jb_uint(&j, "affected_rows", req.result.affected_rows);
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/schema/import — SQL Feature-Parity Roadmap, Phase 8 follow-on ──
// Body: {"sql": "<one or more ';'-separated statements>"}. Runs the whole
// batch through sql_schema_import() (sql_exec.c) under req_uid, which
// splits on top-level ';' (quote-aware) and executes each statement via
// sql_execute(), continuing past individual failures rather than aborting
// the batch -- so importing a real SQLite-style dump that uses syntax this
// parser doesn't support (PRIMARY KEY, CHECK, DEFAULT, CREATE VIEW, ...)
// fails just that one statement and reports why, not the whole import.
static int api_schema_import_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }

    static char import_text[SQL_SCHEMA_EXPORT_MAX_LEN];
    json_str(body, "sql", import_text, sizeof(import_text));

    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 16,404 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct SqlSchemaImportResult res;
    sql_schema_import(req_uid, import_text, &res);

    jb_obj_open(&j, 0);
    jb_uint(&j, "total", res.total); jb_putc(&j, ',');
    jb_uint(&j, "succeeded", res.succeeded); jb_putc(&j, ',');
    jb_uint(&j, "failed", res.failed); jb_putc(&j, ',');
    if (res.total > SQL_SCHEMA_IMPORT_MAX_STMTS) {
        jb_str(&j, "truncated", "true"); jb_putc(&j, ',');
    }
    jb_arr_open(&j, "statements");
    uint32_t attempted = res.total < SQL_SCHEMA_IMPORT_MAX_STMTS ? res.total : SQL_SCHEMA_IMPORT_MAX_STMTS;
    for (uint32_t i = 0; i < attempted; i++) {
        if (i) jb_putc(&j, ',');
        struct SqlSchemaImportStmtResult* sr = &res.stmts[i];
        jb_obj_open(&j, 0);
        jb_uint(&j, "offset", sr->offset); jb_putc(&j, ',');
        jb_str(&j, "ok", sr->ok ? "true" : "false");
        if (!sr->ok) {
            jb_putc(&j, ',');
            jb_uint(&j, "error_code", (uint64_t)sr->error); jb_putc(&j, ',');
            jb_str(&j, "error", sr->error_msg);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/schema/import — VectorStore Interface Roadmap follow-on ────
// Body: {"text": "<one or more newline-separated COLLECTION/INDEX lines>"}.
// Runs the whole batch through vec_schema_import() (vec_index.c) under
// req_uid, which skips blank/comment lines and replays each COLLECTION/
// INDEX line through vecstore_create_collection()/vec_index_create(),
// continuing past individual failures -- same "one bad line doesn't block
// the rest" posture api_schema_import_post() above already established
// for SQL text.
static int api_vec_schema_import_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }

    static char import_text[VEC_SCHEMA_EXPORT_MAX_LEN];
    json_str(body, "text", import_text, sizeof(import_text));

    struct VecSchemaImportResult res;
    vec_schema_import(req_uid, import_text, &res);

    jb_obj_open(&j, 0);
    jb_uint(&j, "total", res.total); jb_putc(&j, ',');
    jb_uint(&j, "succeeded", res.succeeded); jb_putc(&j, ',');
    jb_uint(&j, "failed", res.failed); jb_putc(&j, ',');
    if (res.total > VEC_SCHEMA_IMPORT_MAX_LINES) {
        jb_str(&j, "truncated", "true"); jb_putc(&j, ',');
    }
    jb_arr_open(&j, "lines");
    uint32_t attempted = res.total < VEC_SCHEMA_IMPORT_MAX_LINES ? res.total : VEC_SCHEMA_IMPORT_MAX_LINES;
    for (uint32_t i = 0; i < attempted; i++) {
        if (i) jb_putc(&j, ',');
        struct VecSchemaImportLineResult* lr = &res.lines[i];
        jb_obj_open(&j, 0);
        jb_uint(&j, "offset", lr->offset); jb_putc(&j, ',');
        jb_str(&j, "ok", lr->ok ? "true" : "false");
        if (!lr->ok) {
            jb_putc(&j, ',');
            jb_str(&j, "error", lr->error_msg);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/data/import — VectorStore Interface Roadmap follow-on ──────
// Body: {"text": "<one or more newline-separated VECTOR lines>"}. Runs the
// whole batch through vec_data_import() (vecstore.c) under req_uid, which
// skips blank/comment lines and replays each VECTOR line through
// vecstore_insert(), continuing past individual failures -- same "one bad
// line doesn't block the rest" posture api_vec_schema_import_post() above
// already established. Import schema definitions FIRST if restoring both
// (see vecstore.h's own header comment on why, and vec_index.h's own for
// the definitions side) -- this route does not enforce that ordering
// itself, matching vecstore_insert()'s own "collection must already
// exist" precondition rather than adding a new one here.
static int api_vec_data_import_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }

    static char import_text[VEC_DATA_EXPORT_MAX_LEN];
    json_str(body, "text", import_text, sizeof(import_text));

    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 18,444 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct VecDataImportResult res;
    vec_data_import(req_uid, import_text, &res);

    jb_obj_open(&j, 0);
    jb_uint(&j, "total", res.total); jb_putc(&j, ',');
    jb_uint(&j, "succeeded", res.succeeded); jb_putc(&j, ',');
    jb_uint(&j, "failed", res.failed); jb_putc(&j, ',');
    if (res.total > VEC_DATA_IMPORT_MAX_LINES) {
        jb_str(&j, "truncated", "true"); jb_putc(&j, ',');
    }
    jb_arr_open(&j, "lines");
    uint32_t attempted = res.total < VEC_DATA_IMPORT_MAX_LINES ? res.total : VEC_DATA_IMPORT_MAX_LINES;
    for (uint32_t i = 0; i < attempted; i++) {
        if (i) jb_putc(&j, ',');
        struct VecDataImportLineResult* lr = &res.lines[i];
        jb_obj_open(&j, 0);
        jb_uint(&j, "offset", lr->offset); jb_putc(&j, ',');
        jb_str(&j, "ok", lr->ok ? "true" : "false");
        if (!lr->ok) {
            jb_putc(&j, ',');
            jb_str(&j, "error", lr->error_msg);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/vec/collections — Gap Remediation Phase C ───────────────────────
// Enumerates vector collections -- reads vector_collections[] directly
// (index-aligned with object_catalog[], same idiom api_tables_list() uses
// for row-set tables) rather than going through SYS_SLS_VEC_LIST, which
// dumps to the serial console, not a return buffer -- matching Phase B's
// own GET-route precedent of reading kernel state directly for a JSON
// response rather than repurposing a console-dump syscall.
static int api_vec_collections_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "collections");
    int first = 1;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (!object_catalog[i].active || !vector_collections[i].active) continue;
        struct VecCollectionHeader* h = &vector_collections[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", object_catalog[i].name); jb_putc(&j, ',');
        jb_uint(&j, "dimension", h->dimension); jb_putc(&j, ',');
        jb_uint(&j, "entry_count", h->entry_count); jb_putc(&j, ',');
        jb_uint(&j, "page_count", h->page_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/vec/indexes — Gap Remediation Phase C ────────────────────────────
// Enumerates HNSW indexes -- reads vec_indexes[] directly, same reasoning
// as api_vec_collections_list() above.
static int api_vec_indexes_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "indexes");
    int first = 1;
    for (uint32_t i = 0; i < VEC_INDEX_MAX; i++) {
        if (!vec_indexes[i].active) continue;
        struct VecIndex* idx = &vec_indexes[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", idx->index_name); jb_putc(&j, ',');
        jb_str(&j, "collection", idx->collection_name); jb_putc(&j, ',');
        jb_str(&j, "metric", idx->metric == VEC_METRIC_L2 ? "l2" : "cosine"); jb_putc(&j, ',');
        jb_uint(&j, "active_count", idx->active_count); jb_putc(&j, ',');
        jb_uint(&j, "node_count", idx->node_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partitions — Gap Remediation Phase F ─────────────────────────────
// Enumerates defined partitions -- reads partition_table[] directly, same
// "read kernel state, don't repurpose a console-dump syscall" idiom as
// api_vec_collections_list() above. Every partition syscall (create, list,
// destroy, assign, pause, resume) was already correctly wired at the
// syscall/dispatch layer since Phase 8/14 -- this is the first time any of
// it is reachable outside a host test or the raw shell (docs/AeroSLS-Gap-
// Remediation-Roadmap-v0.1.md Phase F).
static int api_partitions_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "partitions");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "id", partition_table[i].partition_id); jb_putc(&j, ',');
        jb_str(&j, "name", partition_table[i].name); jb_putc(&j, ',');
        /* The owner node, which nothing exposed before. partition_migrate()
         * refuses a destination that already owns the partition, and only the
         * owner holds the data to send -- so this is the field that decides
         * whether a migration is possible, and it was discoverable only by
         * attempting one and reading the error. See the same addition to
         * `partition list` in kernel/partition.c. */
        jb_uint(&j, "owner_node", partition_get_owner_node(partition_table[i].partition_id));
        jb_putc(&j, ',');
        jb_uint(&j, "frame_usage", (uint32_t)partition_get_frame_usage(i)); jb_putc(&j, ',');
        uint64_t quota = partition_get_frame_quota(i);
        jb_uint(&j, "frame_quota", (uint32_t)quota); jb_putc(&j, ',');
        jb_str(&j, "quota_unlimited", quota == 0 ? "true" : "false");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partition/quotas — Gap Remediation Phase F ───────────────────────
// Same per-partition usage/quota data folded into api_partitions_list() above
// (added there directly, the same "one list, everything about it" shape
// api_tables_list() already established) -- this route exists as a distinct,
// narrower endpoint anyway, mirroring the syscall-level split between
// sys_sls_partition_list() and sys_sls_partition_quota_list() (Phase F also
// gave the latter its first syscall number -- frame_pool.h), so a caller who
// only cares about quota pressure doesn't have to parse the wider payload.
static int api_partition_quotas_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "quotas");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        uint64_t usage = partition_get_frame_usage(i);
        uint64_t quota = partition_get_frame_quota(i);
        if (usage == 0 && quota == 0) continue;   // mirrors sys_sls_partition_quota_list()'s own skip rule
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id", i); jb_putc(&j, ',');
        jb_uint(&j, "usage", (uint32_t)usage); jb_putc(&j, ',');
        jb_uint(&j, "quota", (uint32_t)quota); jb_putc(&j, ',');
        jb_str(&j, "unlimited", quota == 0 ? "true" : "false");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/security/audit — Navigator-Parity Gap Roadmap Phase 3 ───────────
// This is the real backend SlsSecurityDashboard.tsx's "Security Event Log"
// panel should read from -- see security_audit.h's own header comment. A
// flat dump of every entry currently retained (bump-allocated, no reclaim --
// see that header's comment on why), oldest first, same array-plus-count
// shape api_wal_json() already uses for wal_buffer[]/wal_entry_count.
static int api_security_audit_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "count", security_audit_log_count); jb_putc(&j, ',');
    jb_uint(&j, "capacity", AUDIT_LOG_MAX); jb_putc(&j, ',');
    jb_arr_open(&j, "entries");
    for (uint32_t i = 0; i < security_audit_log_count; i++) {
        struct SLSAuditEntry* e = &security_audit_log_buf[i];
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "id",      (uint32_t)e->id);   jb_putc(&j, ',');
        jb_uint(&j, "tick",    (uint32_t)e->tick); jb_putc(&j, ',');
        jb_uint(&j, "uid",     e->uid);            jb_putc(&j, ',');
        jb_str(&j,  "action",  e->action);         jb_putc(&j, ',');
        jb_str(&j,  "detail",  e->detail);         jb_putc(&j, ',');
        jb_str(&j,  "granted", e->granted ? "true" : "false");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/security/groups — Navigator-Parity Gap Roadmap Phase 3 ──────────
static int api_security_groups_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "groups");
    int first = 1;
    for (int i = 0; i < GROUP_TABLE_MAX; i++) {
        if (!group_table[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j,  "name", group_table[i].name); jb_putc(&j, ',');
        jb_str(&j,  "role", role_name(group_table[i].group_role)); jb_putc(&j, ',');
        jb_uint(&j, "member_count", group_table[i].member_count); jb_putc(&j, ',');
        jb_arr_open(&j, "members");
        for (uint32_t m = 0; m < group_table[i].member_count; m++) {
            if (m) jb_putc(&j, ',');
            char numbuf[12]; int nl = 0; uint32_t v = group_table[i].member_uids[m];
            if (!v) numbuf[nl++]='0'; else { char rev[12]; int rn=0; while(v){rev[rn++]=(char)('0'+v%10);v/=10;} while(rn) numbuf[nl++]=rev[--rn]; }
            numbuf[nl]='\0';
            jb_raw(&j, numbuf);
        }
        jb_arr_close(&j);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/security/authlists — Navigator-Parity Gap Roadmap Phase 3 ───────
static int api_security_authlists_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "authlists");
    int first = 1;
    for (int i = 0; i < AUTHLIST_MAX; i++) {
        struct SLSAuthListEntry* l = &authlist_table[i];
        if (!l->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j,  "name", l->name); jb_putc(&j, ',');
        jb_arr_open(&j, "objects");
        for (uint32_t k = 0; k < l->object_count; k++) {
            if (!l->objects[k].active) continue;
            if (k) jb_putc(&j, ',');
            jb_obj_open(&j, 0);
            jb_str(&j,  "name", l->objects[k].object_name); jb_putc(&j, ',');
            jb_uint(&j, "perm_mask", l->objects[k].perm_mask);
            jb_obj_close(&j);
        }
        jb_arr_close(&j); jb_putc(&j, ',');
        jb_uint(&j, "grantee_uid_count", l->grantee_uid_count); jb_putc(&j, ',');
        jb_uint(&j, "grantee_group_count", l->grantee_group_count);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/security/databases — Database Namespace & Access Roadmap
// Phase 4 ──────────────────────────────────────────────────────────────────
// Mirrors api_security_groups_json()/api_security_authlists_json()'s own
// shape exactly: one object per active database, plus (if a grant entry
// exists for that database_id) its grant summary -- database.h's own
// struct SLSDatabaseGrant is one entry per database_id (Phase 3's own
// design, see database.h), so at most one grant object per database here,
// not an array of grants.
static int api_security_databases_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "databases");
    int first = 1;
    for (int i = 0; i < DATABASE_MAX; i++) {
        struct SLSDatabaseEntry* d = &databases[i];
        if (!d->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j,  "name", d->name); jb_putc(&j, ',');
        jb_uint(&j, "database_id", d->database_id); jb_putc(&j, ',');
        jb_uint(&j, "owner_uid", d->owner_uid); jb_putc(&j, ',');

        struct SLSDatabaseGrant* g = 0;
        for (int k = 0; k < DATABASE_GRANT_MAX; k++) {
            if (database_grants[k].active && database_grants[k].database_id == d->database_id) {
                g = &database_grants[k]; break;
            }
        }
        jb_uint(&j, "grantee_uid_count", g ? g->grantee_uid_count : 0); jb_putc(&j, ',');
        jb_uint(&j, "grantee_group_count", g ? g->grantee_group_count : 0); jb_putc(&j, ',');
        jb_uint(&j, "grant_perm_mask", g ? g->perm_mask : 0);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/workmgmt/msgqueues — Navigator-Parity Gap Roadmap Phase 4 ────────
// Depth/contents-visible view of the fixed named-queue table (kernel/
// msgqueue.h) -- the "make queues visible" gap the roadmap called out (the
// underlying IPC bus, kernel/ipc.h, has never had any user-facing view at
// all). Same array-plus-count JSON shape as the Phase 3 security routes
// above. Messages themselves are included (not just depth) since a queue
// this small (MQ_QUEUE_DEPTH=16) is cheap to dump in full and "visibility"
// is the entire point of this route.
static int api_workmgmt_msgqueues_json(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "queues");
    int first = 1;
    for (int i = 0; i < MQ_MAX; i++) {
        struct SLSMsgQueueEntry* q = &mq_table[i];
        if (!q->active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j,  "name",  q->name);          jb_putc(&j, ',');
        jb_uint(&j, "depth", q->count);         jb_putc(&j, ',');
        jb_uint(&j, "capacity", MQ_QUEUE_DEPTH); jb_putc(&j, ',');
        jb_arr_open(&j, "messages");
        // Oldest-first walk of the circular buffer's live entries, without
        // consuming them -- this is a read-only visibility route, not
        // mq_receive(); head/count are only ever advanced by an explicit
        // 'mq receive' call or SYS_SLS_MQ_RECEIVE.
        for (uint32_t k = 0; k < q->count; k++) {
            uint32_t idx = (q->head + k) % MQ_QUEUE_DEPTH;
            if (k) jb_putc(&j, ',');
            jb_obj_open(&j, 0);
            jb_uint(&j, "sender_uid", q->msgs[idx].sender_uid); jb_putc(&j, ',');
            jb_uint(&j, "tick", (uint32_t)q->msgs[idx].tick);   jb_putc(&j, ',');
            jb_str(&j,  "text", q->msgs[idx].text);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partitions — Gap Remediation Phase F ────────────────────────────
// Body: {"name": "<partition_name>"}. Thin wrapper over
// sys_sls_partition_create(), the same shape as api_table_create_post().
// req_role gate added when a gap-analysis pass found this route (and
// api_tenant_create_post() below) reachable by any authenticated,
// non-GUEST role -- unlike api_auth_create_post()/_revoke_post(), which
// already required DB_ADMIN or higher. Creating a partition is exactly the
// kind of tenancy-administration action those two already treat as
// privileged, so this reuses their identical gate and error shape rather
// than inventing a new one.
static int api_partition_create_post(const char* body, char* buf, int max, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    if (req_role > ROLE_DB_ADMIN) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionCreateRequest req;
    req.name[0] = '\0';
    json_str(body, "name", req.name, PARTITION_NAME_LEN);
    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t id = sys_sls_partition_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", id != 0xFFFFFFFFu ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "partition_id", (uint32_t)id);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/assign — Gap Remediation Phase F ─────────────────────
// Body: {"uid": N, "partition_id": N}.
static int api_partition_assign_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionAssignRequest req;
    req.uid          = (uint32_t)json_int(body, "uid");
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    uint64_t rc = sys_sls_partition_assign(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/destroy — Gap Remediation Phase F ────────────────────
// Body: {"partition_id": N}.
static int api_partition_destroy_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    uint32_t partition_id = (uint32_t)json_int(body, "partition_id");
    uint64_t rc = sys_sls_partition_destroy(partition_id);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/pause | /api/partition/resume — Gap Remediation
// Phase F ─────────────────────────────────────────────────────────────────────
// Body: {"partition_id": N}. `resume` selects sys_sls_partition_resume()
// instead of sys_sls_partition_pause() -- same one-function-two-routes shape
// api_tx_post() already established for commit/rollback.
static int api_partition_pause_post(const char* body, char* buf, int max, int resume) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    uint32_t partition_id = (uint32_t)json_int(body, "partition_id");
    uint64_t rc = resume ? sys_sls_partition_resume(partition_id)
                         : sys_sls_partition_pause(partition_id);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/partition/quota — Gap Remediation Phase F ──────────────────────
// Body: {"partition_id": N, "frame_quota": N}. frame_quota=0 means unlimited
// (frame_pool.h's own convention) -- passing it explicitly re-uncaps a
// previously-limited partition, not just an omission default.
static int api_partition_quota_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionQuotaSetRequest req;
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    req.frame_quota  = json_uint64(body, "frame_quota");
    uint64_t rc = sys_sls_partition_quota_set(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partition/cpuweights, POST /api/partition/cpuweight ──────────
// Multitenant Isolation Gap Analysis §5 item 8 / §7 item 8: weighted CPU
// scheduling. Mirrors api_partition_quotas_list()/api_partition_quota_post()
// exactly, just reading/writing partition_get_cpu_weight()/partition_set_
// cpu_weight() (kernel/process.c) instead of the frame-quota pair.
static int api_partition_cpuweights_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "cpuweights");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        uint32_t weight = partition_get_cpu_weight(i);
        if (weight == 1) continue;   // still at the unconfigured default -- nothing interesting to report, mirrors sys_sls_partition_cpu_weight_list()'s own skip rule
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id", i); jb_putc(&j, ',');
        jb_uint(&j, "weight", weight);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_partition_cpuweight_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionCpuWeightSetRequest req;
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    req.weight       = (uint32_t)json_int(body, "weight");
    uint64_t rc = sys_sls_partition_cpu_weight_set(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partition/storagequotas, POST /api/partition/storagequota ───
// Storage Isolation Roadmap Phase 1: per-partition on-disk page quota
// (rowstore+vecstore combined, see storage_quota.h). Mirrors api_partition_
// quotas_list()/api_partition_quota_post() exactly, just reading/writing
// storage_get_page_usage()/_quota()/storage_set_page_quota() (kernel/
// storage_quota.c) instead of the RAM frame-quota pair.
static int api_partition_storagequotas_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "storagequotas");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        uint64_t usage = storage_get_page_usage(i);
        uint64_t quota = storage_get_page_quota(i);
        if (usage == 0 && quota == 0) continue;   // nothing interesting to report, mirrors sys_sls_partition_storage_quota_list()'s own skip rule
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id", i); jb_putc(&j, ',');
        jb_uint(&j, "page_usage", usage); jb_putc(&j, ',');
        jb_uint(&j, "page_quota", quota);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_partition_storagequota_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionStorageQuotaSetRequest req;
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    req.page_quota   = json_uint64(body, "page_quota");
    uint64_t rc = sys_sls_partition_storage_quota_set(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}





/* ─── POST /api/cluster/init, POST /api/cluster/peer ───────────────────
 * Forming a cluster from the control-plane UI. Both wrap syscalls that
 * have existed since the Multi-Node roadmap's Phase 7 addendum but were
 * reachable only from the serial console, which meant cluster formation
 * was the one operator task the web surface could not do at all.
 *
 * These FORM a cluster out of nodes that are already running. Nothing
 * here boots a machine -- a kernel cannot start another kernel, and the
 * only component that could (the dev server) does not execute host
 * processes and deliberately still does not.
 *
 * DB_ADMIN-gated, like every other mutation. Worth stating plainly why
 * that matters more here than elsewhere: cluster_init() RESETS this
 * node's term, role and roster (see consensus.h -- re-init is a fresh
 * start, not a merge), so calling it on a node already in a working
 * cluster drops it out of that cluster. It is not a read-modify-write. */
static int api_cluster_init_post(const char* body, char* buf, int max, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    if (req_role > ROLE_DB_ADMIN) {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint32_t node_id = body ? (uint32_t)json_int(body, "node_id") : 0;
    uint64_t rc = sys_sls_cluster_init(node_id);
    jb_str (&j, "ok", rc == 0 ? "true" : "false"); jb_putc(&j, ',');
    if (rc != 0) {
        /* The only rejection cluster_init() has: 0 is the reserved
         * "uninitialised" sentinel, so it cannot also be a real id. */
        jb_str(&j, "error", "node_id 0 is the reserved uninitialised sentinel");
        jb_putc(&j, ',');
    }
    jb_uint(&j, "node_id", cluster_local_node_id());
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_cluster_peer_post(const char* body, char* buf, int max, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    if (req_role > ROLE_DB_ADMIN) {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint32_t node_id = body ? (uint32_t)json_int(body, "node_id") : 0;
    int rc = cluster_register_peer(node_id);
    /* cluster_register_peer()'s return codes are informative, not just
     * pass/fail -- "already active" is a successful no-op, not an error,
     * and an operator re-adding a peer should be told that rather than
     * shown a failure. */
    const char* detail =
        rc ==  0 ? "added" :
        rc ==  1 ? "already a member (no-op)" :
        rc ==  2 ? "re-activated a previously registered peer" :
        rc == -1 ? "invalid node id (0, or this node's own id)" :
                   "roster full";
    jb_str (&j, "ok", rc >= 0 ? "true" : "false"); jb_putc(&j, ',');
    jb_str (&j, "detail", detail);                 jb_putc(&j, ',');
    jb_uint(&j, "active_nodes", cluster_active_node_count());
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Cluster view (control-plane surface) ─────────────────────────────────
// GET /api/cluster — the roster and this node's consensus state.
// GET /api/nodes   — what is genuinely known about every node, and no more.
//
// ─── Why this lives in the kernel rather than a separate control plane ───
// Kubernetes needs a control-plane process because Linux knows nothing
// about clusters. This kernel does: the service registry, the reconciler,
// the circuit breakers and workload restarts are all in-kernel and
// replicated over DSPP (Orchestration Plan Phases 4-7). A userspace
// aggregator would be a SECOND source of truth for state the kernel
// already owns authoritatively -- the same mistake Phase 4 avoided by
// deriving a service's node from its partition rather than storing it.
//
// The consequence worth keeping: because every node holds the roster and
// the replicated registry, ANY node can answer these. There is no
// control-plane node, so there is no new single point of failure and no
// bootstrap ordering problem.
//
// ─── What one node can and cannot honestly report ────────────────────────
// KNOWN cluster-wide, and returned here:
//   - the roster: which node ids are members
//   - partition ownership: partition_owner_table[] is the authority for
//     where a partition lives, and partition_migrate() keeps it current
//   - services: the registry replicates, and every entry carries the node
//     that owns it (see service_registry.h)
// NOT known about a peer, and deliberately NOT invented here:
//   - its memory, its workloads, its breakers, its uptime. None of that
//     is replicated. A peer is reported as id + membership only, and the
//     caller drills into that node's own /api/* for the rest. Returning a
//     fabricated or stale figure would be worse than returning nothing.
static int api_cluster_view(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "node_id", cluster_local_node_id());                    jb_putc(&j, ',');
    jb_str (&j, "role", consensus_role_name(local_cluster_state.role)); jb_putc(&j, ',');
    jb_uint(&j, "term", local_cluster_state.current_term);              jb_putc(&j, ',');
    jb_uint(&j, "active_nodes", local_cluster_state.active_nodes_count);jb_putc(&j, ',');
    jb_uint(&j, "quorum_threshold", local_cluster_state.stable_quorum_threshold);
    jb_putc(&j, ',');
    /* Non-zero means some DSPP message is too large for the link and is
     * being refused rather than delivered (net/dspp.h). Surfaced right next
     * to role/term because that is the pairing that diagnoses a stuck
     * cluster: CANDIDATE with a climbing term AND a rising drop count is
     * "my votes are not reaching anyone", which otherwise looks identical
     * to "my peers are refusing to vote for me". */
    jb_uint(&j, "dspp_oversize_dropped", (uint32_t)dspp_tx_oversize_dropped);
    jb_putc(&j, ',');
    /* Memory-integrity signals. These are here rather than in a debug corner
     * because their healthy value is a positive statement, not an absence:
     * "no [FRAME] error scrolled past at boot" is indistinguishable from a
     * truncated log or a check that never ran, and the failure they cover is
     * the allocator handing out the kernel's own live stack -- which surfaces
     * as a #GP on a return address made of somebody's payload, several layers
     * away from the write. stack_reserved must be true and
     * stack_frames_withheld must be 0 on a healthy node; a nonzero withheld
     * count is a bug report, not a statistic. */
    jb_str (&j, "stack_reserved", frame_pool_stack_still_reserved() ? "true" : "false");
    jb_putc(&j, ',');
    jb_str (&j, "stack_covered_at_boot", frame_pool_stack_covered ? "true" : "false");
    jb_putc(&j, ',');
    jb_uint(&j, "stack_frames_withheld", (uint32_t)frame_pool_live_stack_withheld);
    jb_putc(&j, ',');
    jb_uint(&j, "peers_autodiscovered", (uint32_t)cluster_peers_autodiscovered);
    jb_putc(&j, ',');
    /* node_id 0 is Phase 1's "cluster_init() was never called" sentinel.
     * Reported explicitly so a UI can say "this node is standalone"
     * rather than drawing a one-node cluster that does not exist. */
    jb_str (&j, "initialised", cluster_local_node_id() != 0 ? "true" : "false");
    jb_putc(&j, ',');
    jb_arr_open(&j, "roster");
    int first = 1;
    for (uint32_t i = 0; i < CLUSTER_NODE_MAX; i++) {
        if (!cluster_roster[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "node_id", cluster_roster[i].node_id); jb_putc(&j, ',');
        jb_str (&j, "self", "false");
        jb_obj_close(&j);
    }
    if (cluster_local_node_id() != 0) {
        if (!first) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "node_id", cluster_local_node_id()); jb_putc(&j, ',');
        jb_str (&j, "self", "true");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_cluster_nodes(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    uint32_t me = cluster_local_node_id();
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "nodes");
    int first = 1;

    /* Self, in full: this is the only node whose detail is first-hand. */
    if (me != 0) {
        jb_obj_open(&j, 0);
        jb_uint(&j, "node_id", me);                                    jb_putc(&j, ',');
        jb_str (&j, "self", "true");                                   jb_putc(&j, ',');
        jb_str (&j, "detail", "first-hand");                           jb_putc(&j, ',');
        jb_str (&j, "role", consensus_role_name(local_cluster_state.role)); jb_putc(&j, ',');
        uint32_t owned = 0;
        for (uint32_t pi = 0; pi < PARTITION_MAX; pi++)
            if (partition_owner_table[pi].active &&
                partition_owner_table[pi].node_id == me) owned++;
        jb_uint(&j, "partitions_owned", owned);                        jb_putc(&j, ',');
        jb_uint(&j, "services_local", service_registry_count());       jb_putc(&j, ',');
        jb_uint(&j, "workloads", workload_count());                    jb_putc(&j, ',');
        jb_uint(&j, "live_contexts", wlctx_count());
        jb_obj_close(&j);
        first = 0;
    }

    /* Peers: membership and what the replicated registry says they own.
     * Everything else about them is genuinely unknown from here. */
    for (uint32_t i = 0; i < CLUSTER_NODE_MAX; i++) {
        if (!cluster_roster[i].active) continue;
        uint32_t nid = cluster_roster[i].node_id;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "node_id", nid);        jb_putc(&j, ',');
        jb_str (&j, "self", "false");       jb_putc(&j, ',');
        /* Named so a UI does not render a peer's blanks as zeroes. */
        jb_str (&j, "detail", "membership-only"); jb_putc(&j, ',');
        uint32_t owned = 0;
        for (uint32_t pi = 0; pi < PARTITION_MAX; pi++)
            if (partition_owner_table[pi].active &&
                partition_owner_table[pi].node_id == nid) owned++;
        jb_uint(&j, "partitions_owned", owned);   jb_putc(&j, ',');
        uint32_t svcs = 0;
        for (uint32_t si = 0; si < SERVICE_REMOTE_MAX; si++)
            if (services_remote[si].active && services_remote[si].node_id == nid) svcs++;
        jb_uint(&j, "services_announced", svcs);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Orchestration Plan Phase 6: GET /api/mesh ────────────────────────────
static int api_mesh_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "breakers");
    int first = 1;
    for (uint32_t i = 0; i < MESH_MAX_SERVICES; i++) {
        if (!mesh_entries[i].active) continue;
        struct SLSMeshEntry* e = &mesh_entries[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str (&j, "name", e->name);                                        jb_putc(&j, ',');
        jb_str (&j, "state", breaker_state_name((SLSBreakerState)e->state)); jb_putc(&j, ',');
        jb_uint(&j, "consecutive_failures", e->consecutive_failures);        jb_putc(&j, ',');
        jb_uint(&j, "successes", e->successes);                              jb_putc(&j, ',');
        jb_uint(&j, "failures", e->failures);                                jb_putc(&j, ',');
        jb_uint(&j, "trips", e->trips);                                      jb_putc(&j, ',');
        jb_uint(&j, "calls_permitted", e->calls_permitted);                  jb_putc(&j, ',');
        jb_uint(&j, "calls_refused", e->calls_refused);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Orchestration Plan Phase 5: declarative workloads ────────────────────
// GET /api/workloads, POST /api/workload, POST /api/reconcile
static int api_workloads_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_str (&j, "reconciler", reconcile_is_enabled() ? "on" : "off"); jb_putc(&j, ',');
    jb_uint(&j, "queue_depth",   reconcile_queue_depth());            jb_putc(&j, ',');
    jb_uint(&j, "queue_dropped", reconcile_queue_dropped());          jb_putc(&j, ',');
    jb_arr_open(&j, "workloads");
    int first = 1;
    for (uint32_t i = 0; i < WORKLOAD_MAX; i++) {
        if (!workloads[i].active) continue;
        struct SLSWorkloadEntry* w = &workloads[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str (&j, "name", w->name);                                   jb_putc(&j, ',');
        jb_uint(&j, "partition_id", w->partition_id);                   jb_putc(&j, ',');
        jb_str (&j, "desired", w->desired_state == WL_DESIRED_RUNNING ? "running" : "stopped");
        jb_putc(&j, ',');
        jb_str (&j, "service_name", w->service_name);                   jb_putc(&j, ',');
        jb_str (&j, "endpoint_kind", w->endpoint_kind == SVC_ENDPOINT_TCP ? "tcp" : "ipc");
        jb_putc(&j, ',');
        jb_uint(&j, "endpoint_port", w->endpoint_port);                 jb_putc(&j, ',');
        jb_str (&j, "program_name", w->program_name);                   jb_putc(&j, ',');
        jb_str (&j, "context_live", wlctx_has(w->name) ? "true" : "false"); jb_putc(&j, ',');
        jb_str (&j, "restart_policy",
                workload_restart_policy_name((SLSWorkloadRestartPolicy)w->restart_policy));
        jb_putc(&j, ',');
        jb_uint(&j, "restart_count", w->restart_count);   jb_putc(&j, ',');
        jb_uint(&j, "restarts_total", w->restarts_total); jb_putc(&j, ',');
        jb_str (&j, "gave_up", w->gave_up ? "true" : "false"); jb_putc(&j, ',');
        jb_uint(&j, "actions_taken", w->actions_taken);                 jb_putc(&j, ',');
        jb_str (&j, "converged", w->converged ? "true" : "false");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_workload_post(const char* body, char* buf, int max,
                             uint32_t req_uid, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    if (!body) {
        jb_str(&j,"ok","false"); jb_putc(&j,','); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (req_role > ROLE_DB_ADMIN) {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    char name[WORKLOAD_NAME_LEN]; name[0] = '\0';
    json_str(body, "name", name, (int)sizeof(name));
    char desired[16]; desired[0] = '\0';
    json_str(body, "desired", desired, (int)sizeof(desired));
    char svc[SERVICE_NAME_LEN]; svc[0] = '\0';
    json_str(body, "service_name", svc, (int)sizeof(svc));
    char kind[8]; kind[0] = '\0';
    json_str(body, "endpoint_kind", kind, (int)sizeof(kind));

    SLSWorkloadDesired d = (desired[0]=='r') ? WL_DESIRED_RUNNING : WL_DESIRED_STOPPED;
    SLSServiceEndpointKind k = (kind[0]=='i') ? SVC_ENDPOINT_IPC : SVC_ENDPOINT_TCP;

    char prog[WORKLOAD_NAME_LEN]; prog[0] = '\0';
    json_str(body, "program_name", prog, (int)sizeof(prog));
    char entry[32]; entry[0] = '\0';
    json_str(body, "entry_name", entry, (int)sizeof(entry));

    char pol[16]; pol[0] = '\0';
    json_str(body, "restart_policy", pol, (int)sizeof(pol));
    SLSWorkloadRestartPolicy rp = (pol[0]=='a') ? WL_RESTART_ALWAYS
                                : (pol[0]=='o') ? WL_RESTART_ON_FAILURE
                                : WL_RESTART_NEVER;

    SLSWorkloadStatus rc = workload_declare(req_uid, name,
                                            (uint32_t)json_int(body, "partition_id"),
                                            d, svc, k,
                                            (uint32_t)json_int(body, "endpoint_port"),
                                            prog, entry, rp);
    jb_str(&j, "ok", rc == WL_OK ? "true" : "false");
    if (rc != WL_OK) { jb_putc(&j, ','); jb_str(&j, "error", workload_status_name(rc)); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_reconcile_post(const char* body, char* buf, int max, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    if (req_role > ROLE_DB_ADMIN) {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    char on[8]; on[0] = '\0';
    if (body) json_str(body, "enabled", on, (int)sizeof(on));
    reconcile_set_enabled(on[0] == 't' || on[0] == '1');
    jb_str(&j, "ok", "true"); jb_putc(&j, ',');
    jb_str(&j, "reconciler", reconcile_is_enabled() ? "on" : "off");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Orchestration Plan Phase 4: service registry ─────────────────────────
// GET /api/services, GET /api/service/resolve?name=..., POST /api/service
//
// `node_id` in these responses is DERIVED from the partition's current
// owner on every request, never stored (kernel/service_registry.h). So a
// GET issued after a partition migrates reports the new node with nothing
// here having been updated or invalidated.
static int api_services_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "remote_cached", service_remote_count()); jb_putc(&j, ',');
    jb_arr_open(&j, "services");
    int first = 1;
    for (uint32_t i = 0; i < SERVICE_MAX; i++) {
        if (!services_registry[i].active) continue;
        struct SLSServiceEntry* e = &services_registry[i];
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str (&j, "name", e->name);                                jb_putc(&j, ',');
        jb_uint(&j, "partition_id", e->partition_id);                jb_putc(&j, ',');
        jb_uint(&j, "node_id", partition_get_owner_node(e->partition_id)); jb_putc(&j, ',');
        jb_str (&j, "endpoint_kind", e->endpoint_kind == SVC_ENDPOINT_TCP ? "tcp" : "ipc");
        jb_putc(&j, ',');
        jb_uint(&j, "endpoint_port", e->endpoint_port);              jb_putc(&j, ',');
        jb_str (&j, "serving", service_serving_name((SLSServiceServing)e->serving));
        jb_putc(&j, ',');
        jb_uint(&j, "owner_uid", e->owner_uid);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_service_resolve(const char* name, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    struct SLSServiceLocation loc;
    SLSServiceStatus rc = service_resolve(name, &loc);
    jb_obj_open(&j, 0);
    if (rc != SVC_REG_OK) {
        jb_str(&j, "ok", "false"); jb_putc(&j, ',');
        jb_str(&j, "error", service_status_name(rc));
    } else {
        jb_str (&j, "ok", "true");                       jb_putc(&j, ',');
        jb_str (&j, "name", loc.name);                   jb_putc(&j, ',');
        jb_uint(&j, "partition_id", loc.partition_id);   jb_putc(&j, ',');
        jb_uint(&j, "node_id", loc.node_id);             jb_putc(&j, ',');
        jb_str (&j, "endpoint_kind", loc.endpoint_kind == SVC_ENDPOINT_TCP ? "tcp" : "ipc");
        jb_putc(&j, ',');
        jb_uint(&j, "endpoint_port", loc.endpoint_port); jb_putc(&j, ',');
        jb_str (&j, "is_local", loc.is_local ? "true" : "false");  jb_putc(&j, ',');
        jb_str (&j, "is_remote", loc.is_remote ? "true" : "false"); jb_putc(&j, ',');
        jb_str (&j, "health", service_health_name((SLSServiceHealth)loc.health)); jb_putc(&j, ',');
        jb_str (&j, "serving", service_serving_name((SLSServiceServing)loc.serving));
        jb_putc(&j, ',');
        jb_str (&j, "breaker", breaker_state_name((SLSBreakerState)loc.breaker));
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_service_post(const char* body, char* buf, int max,
                            uint32_t req_uid, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    if (!body) {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    /* Same DB_ADMIN gate the partition/tenant creation endpoints carry.
     * service_register() re-checks via catalog_get_role() -- this is the
     * HTTP-layer half, not the only one. */
    if (req_role > ROLE_DB_ADMIN) {
        jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    char name[SERVICE_NAME_LEN]; name[0] = '\0';
    json_str(body, "name", name, (int)sizeof(name));
    char kind[8]; kind[0] = '\0';
    json_str(body, "endpoint_kind", kind, (int)sizeof(kind));
    SLSServiceEndpointKind k =
        (kind[0]=='t' && kind[1]=='c' && kind[2]=='p') ? SVC_ENDPOINT_TCP : SVC_ENDPOINT_IPC;

    SLSServiceStatus rc = service_register(req_uid, name,
                                           (uint32_t)json_int(body, "partition_id"),
                                           k, (uint32_t)json_int(body, "endpoint_port"));
    jb_str(&j, "ok", rc == SVC_REG_OK ? "true" : "false");
    if (rc != SVC_REG_OK) { jb_putc(&j, ','); jb_str(&j, "error", service_status_name(rc)); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/partition/connquotas, POST /api/partition/connquota ─────────
// Network Fairness Phase 2 (Multitenant Isolation Gap Analysis §19): this
// mechanism (net/tcp_quota.c) shipped with a syscall and a shell command
// but no HTTP route, discovered as a gap when the frontend dashboard went
// looking for it. Mirrors api_partition_cpuweights_list()/_post() exactly,
// just reading/writing tcp_partition_get_conn_usage()/_quota()/
// sys_sls_partition_conn_quota_set() (net/tcp_quota.c) instead of the CPU-
// weight pair. Unlike cpuweight's "1 == default, skip" rule, both usage and
// quota are meaningful at 0 (0 usage is a normal idle partition, 0 quota is
// the real "unlimited" default) -- so the skip rule here matches storage-
// quota's own "skip only if literally nothing to report" shape instead.
static int api_partition_connquotas_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "connquotas");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        uint16_t usage = tcp_partition_get_conn_usage(i);
        uint16_t quota = tcp_partition_get_conn_quota(i);
        if (usage == 0 && quota == 0) continue;   // nothing interesting to report, mirrors api_partition_storagequotas_list()'s own skip rule
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id", i); jb_putc(&j, ',');
        jb_uint(&j, "conn_usage", usage); jb_putc(&j, ',');
        jb_uint(&j, "conn_quota", quota);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

static int api_partition_connquota_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSPartitionConnQuotaSetRequest req;
    req.partition_id = (uint32_t)json_int(body, "partition_id");
    req.quota        = (uint16_t)json_int(body, "quota");
    uint64_t rc = sys_sls_partition_conn_quota_set(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc == 0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/usage — Multitenant Isolation Gap Analysis §5 item 6 /
// §7 item 6 ──────────────────────────────────────────────────────────────
// Enumerates every active partition's cumulative usage counters -- reads
// partition_table[]/usage_table[] directly, same "read kernel state"
// idiom as api_tenants_list() below. frames_now is a live gauge (current
// frame usage), included alongside the two cumulative totals for context,
// not itself part of the cumulative record -- see usage_metering.h.
static int api_usage_report(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "partitions");
    int first = 1;
    for (uint32_t i = 0; i < PARTITION_MAX; i++) {
        if (!partition_table[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "partition_id", partition_table[i].partition_id); jb_putc(&j, ',');
        jb_str(&j, "name", partition_table[i].name); jb_putc(&j, ',');
        jb_uint(&j, "http_requests_total", usage_metering_get_requests(partition_table[i].partition_id)); jb_putc(&j, ',');
        jb_uint(&j, "frame_ticks_total", usage_metering_get_frame_ticks(partition_table[i].partition_id)); jb_putc(&j, ',');
        jb_uint(&j, "frames_now", partition_get_frame_usage(partition_table[i].partition_id));
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/tenants — Multitenant Isolation Gap Analysis §5 item 1 /
// §7 item 2 ──────────────────────────────────────────────────────────────
// Enumerates defined tenants -- reads tenants[] directly, same "read
// kernel state" idiom as api_partitions_list() above.
static int api_tenants_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "tenants");
    int first = 1;
    for (uint32_t i = 0; i < TENANT_MAX; i++) {
        if (!tenants[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_uint(&j, "id", tenants[i].tenant_id); jb_putc(&j, ',');
        jb_str(&j, "name", tenants[i].name); jb_putc(&j, ',');
        jb_uint(&j, "partition_id", tenants[i].partition_id); jb_putc(&j, ',');
        jb_uint(&j, "database_id", tenants[i].database_id); jb_putc(&j, ',');
        jb_uint(&j, "owner_uid", tenants[i].owner_uid);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/tenants — Multitenant Isolation Gap Analysis §5 item 1 /
// §7 item 2 ──────────────────────────────────────────────────────────────
// Body: {"name": "<tenant_name>"}. Thin wrapper over sys_sls_tenant_
// create(), the same shape api_partition_create_post() above already
// established. caller_uid is the authenticated req_uid, not a client-
// supplied field -- matches api_vec_create_post()'s own "caller_uid comes
// from the auth gate, never the request body" posture.
// req_role gate: same DB_ADMIN+ requirement as api_partition_create_post()
// above, for the same reason (see that function's own comment) --
// tenant_create() itself calls partition_create() as its own Step 1
// (kernel/tenant.c), so leaving this route open to APP_USER would have
// simply been a second, unguarded way to reach the exact privileged
// operation this pass just closed off directly.
static int api_tenant_create_post(const char* body, char* buf, int max, uint32_t req_uid, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    if (req_role > ROLE_DB_ADMIN) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSTenantCreateRequest req;
    req.name[0] = '\0';
    req.caller_uid = req_uid;
    json_str(body, "name", req.name, TENANT_NAME_LEN);
    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t tenant_id = sys_sls_tenant_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", tenant_id != 0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "tenant_id", (uint32_t)tenant_id);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/collections — Gap Remediation Phase C ──────────────────────
// The live path Vector Store Phase 4's SYS_SLS_VEC_CREATE never had over
// HTTP -- syscall/shell-reachable since Phase 4, zero HTTP routes for any
// vector-store capability before this (docs/AeroSLS-Gap-Analysis-v0.1.md
// §7). Body: {"name": "<collection>", "dimension": N}. Requires the
// catalog object to already exist via POST /api/valloc, matching
// SYS_SLS_VEC_CREATE's own precondition.
static int api_vec_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecCreateRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.collection_name, OBJECT_NAME_LEN);
    req.dimension = (uint32_t)json_int(body, "dimension");
    uint64_t rc = sys_sls_vec_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.collection_name); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/collections/unique — VectorStore Gap Analysis §1.3 ─────────
// Live path for vecstore_set_unique_external_id() -- toggles opt-in
// external_id deduplication on an already-created collection. Body:
// {"name": "<collection>", "enabled": 1} (or 0). Deliberately an integer,
// not a JSON boolean literal: json_int() (this file's only integer
// extractor, confirmed by grep -- there is no separate json_bool() anywhere
// in this codebase) only scans decimal digit characters, so a literal
// "true"/"false" would silently parse as 0 every time, which would make
// "enabled": true a silent no-op bug baked into the wire format on day
// one. Every existing route in this file that takes a 0/1-shaped flag
// already keeps this same integer-only convention for the same reason
// (confirmed by grep across this file's own json_int() call sites before
// writing this comment) -- this route follows it rather than being the
// first to assume JSON-boolean support that doesn't exist here.
static int api_vec_set_unique_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecSetUniqueRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.collection_name, OBJECT_NAME_LEN);
    req.enabled = json_int(body, "enabled");
    uint64_t rc = sys_sls_vec_set_unique(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.collection_name); jb_putc(&j,',');
    jb_str(&j, "enabled", req.enabled ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/objects/database — VectorStore Gap Analysis §3 ─────────────────
// Live path for catalog_set_database(): a generic, object-type-agnostic
// retag reaching any catalog object, including an already-promoted vector
// collection (which has no ALTER verb of its own but shares object_catalog[]
// with SQL tables). Body: {"name": "<object>", "database": "<database>"} --
// omit or send an empty "database" to clear the tag back to 0 (NONE).
// catalog_check_access()'s existing database_check_access() fallback
// already runs against this same field for every vecstore.c CRUD call, so
// this route is enough to move a vector collection into (or out of) a
// database's grant list with zero vecstore.c changes.
static int api_object_set_database_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSSetDatabaseRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.object_name, OBJECT_NAME_LEN);
    req.database_name[0] = '\0';
    json_str(body, "database", req.database_name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_object_set_database(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.object_name); jb_putc(&j,',');
    jb_str(&j, "database", req.database_name[0] ? req.database_name : "(none)"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/insert — Gap Remediation Phase C ────────────────────────────
// Body: {"collection": "<name>", "external_id": N, "values": [f0, f1, ...]}.
static int api_vec_insert_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 8,304 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct SLSVecInsertRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.external_id = json_uint64(body, "external_id");
    req.values.count = (uint32_t)json_float_array(body, "values", req.values.values, VECSTORE_MAX_DIMENSION);
    uint64_t rc = sys_sls_vec_insert(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status); jb_putc(&j,',');
    jb_uint(&j, "page_id", req.out_id.page_id); jb_putc(&j,',');
    jb_uint(&j, "slot_index", req.out_id.slot_index);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/embed-insert — Gap Remediation Phase C ──────────────────────
// Body: {"collection": "<name>", "external_id": N, "endpoint_ip": "...",
// "port": N, "model": "...", "prompt": "..."}. endpoint_ip/port default to
// 10.0.2.2:11434 if omitted -- NOT 127.0.0.1. This kernel is itself a full
// OS booted inside QEMU (see boot log's own "[NET] e1000 ... [DHCP] Bound:
// 10.0.2.15 gw 10.0.2.2" lines): "127.0.0.1" from in here means THIS guest's
// own loopback, not the host machine's, so a request to it never reaches a
// host-side Ollama at all -- confirmed live: the guest's outbound connect
// completed and got a real HTTP response (404) even though a host-side
// `tcpdump -i lo` during the same request captured nothing, proving the
// traffic never left the guest. QEMU's own usermode/SLIRP networking (the
// same DHCP-assigned 10.0.2.0/24 range above) exposes host-reachable
// services at 10.0.2.2, the gateway address, which is what actually reaches
// a host-side Ollama instance -- verified live against this exact box.
// Still overridable per-request via endpoint_ip for any other topology
// (Ollama on a different host/container), matching the shell command's own
// convention (user/shell.c).
static int api_vec_embed_insert_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecEmbedInsertRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.external_id = json_uint64(body, "external_id");
    json_str_or_default(body, "endpoint_ip", req.ollama_req.endpoint_ip, OLLAMA_ENDPOINT_LEN, "10.0.2.2");
    int port = json_int(body, "port");
    req.ollama_req.port = (uint16_t)(port ? port : 11434);
    json_str_or_default(body, "model", req.ollama_req.model, OLLAMA_MODEL_LEN, "nomic-embed-text");
    req.ollama_req.prompt[0] = '\0';
    json_str(body, "prompt", req.ollama_req.prompt, OLLAMA_PROMPT_LEN);
    uint64_t rc = sys_sls_vec_embed_insert(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "ollama_status", (uint64_t)req.ollama_status); jb_putc(&j,',');
    jb_uint(&j, "insert_status", (uint64_t)req.insert_status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/search — Gap Remediation Phase C ────────────────────────────
// Body: {"collection": "<name>", "query": [f0, f1, ...], "metric":
// "cosine"|"l2" (default cosine), "k": N}.
static int api_vec_search_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 9,920 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct SLSVecSearchRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.query.count = (uint32_t)json_float_array(body, "query", req.query.values, VECSTORE_MAX_DIMENSION);
    char metric_s[16]; metric_s[0] = '\0';
    json_str(body, "metric", metric_s, (int)sizeof(metric_s));
    req.metric = (!strcmp(metric_s, "l2") || !strcmp(metric_s, "L2")) ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
    req.k = (uint32_t)json_int(body, "k");
    if (!req.k) req.k = 10;
    uint64_t rc = sys_sls_vec_search(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "match_count", req.match_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "matches");
    uint32_t nshown = req.match_count < VEC_SEARCH_MAX_K ? req.match_count : VEC_SEARCH_MAX_K;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.matches[i].external_id); jb_putc(&j, ',');
        jb_uint(&j, "page_id", req.matches[i].id.page_id); jb_putc(&j, ',');
        jb_uint(&j, "slot_index", req.matches[i].id.slot_index); jb_putc(&j, ',');
        jb_key(&j, "distance");
        // distance is a float -- no float formatter in this file's JSONBuf
        // yet; round-trip via the same fixed-6-decimal shape rowstore.c's
        // own rs_f64_to_str() uses, inlined here since it's the only float
        // field this whole API surface has ever needed to emit.
        {
            double v = (double)req.matches[i].distance;
            char fb[32]; int fn = 0;
            if (v < 0) { fb[fn++] = '-'; v = -v; }
            uint64_t ip = (uint64_t)v; double frac = v - (double)ip;
            char ipb[24]; int il = 0;
            if (!ip) ipb[il++] = '0'; else { uint64_t t = ip; while (t) { ipb[il++] = (char)('0'+t%10); t/=10; } }
            for (int k = il-1; k >= 0; k--) fb[fn++] = ipb[k];
            fb[fn++] = '.';
            for (int d = 0; d < 6; d++) { frac *= 10.0; int digit = (int)frac; if (digit<0) digit=0; if (digit>9) digit=9; fb[fn++]=(char)('0'+digit); frac -= (double)digit; }
            fb[fn] = '\0';
            jb_raw(&j, fb);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/indexes — Gap Remediation Phase C ───────────────────────────
// Live path for vec_index_create() (Vector Store Phase 6, HNSW), which had
// no syscall/shell/HTTP surface at all before this pass (vec_index.h's own
// point 6 named it a deliberate "revisit only when a real caller needs it"
// cut). Body: {"name": "<index>", "collection": "<collection>", "metric":
// "cosine"|"l2" (default cosine)}. Does not backfill an already-populated
// collection -- see vec_index.h's own point 7.
static int api_vec_index_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecIndexCreateRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.index_name, OBJECT_NAME_LEN);
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    char metric_s[16]; metric_s[0] = '\0';
    json_str(body, "metric", metric_s, (int)sizeof(metric_s));
    req.metric = (!strcmp(metric_s, "l2") || !strcmp(metric_s, "L2")) ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
    uint64_t rc = sys_sls_vec_index_create(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", req.index_name); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/index/search — Gap Remediation Phase C ──────────────────────
// Live path for vec_index_search() (approximate top-K over the HNSW
// index). Body: {"index": "<name>", "query": [f0, f1, ...], "k": N,
// "ef": N (default = k)}. Response shape matches POST /api/vec/search's own
// (external_id/page_id/slot_index/distance per match) so a client can treat
// exact and approximate search as interchangeable result consumers.
static int api_vec_index_search_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 9,904 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct SLSVecIndexSearchRequest req;
    req.caller_uid = req_uid;
    json_str(body, "index", req.index_name, OBJECT_NAME_LEN);
    req.query.count = (uint32_t)json_float_array(body, "query", req.query.values, VECSTORE_MAX_DIMENSION);
    req.k = (uint32_t)json_int(body, "k");
    if (!req.k) req.k = 10;
    req.ef = (uint32_t)json_int(body, "ef");
    if (!req.ef) req.ef = req.k;
    uint64_t rc = sys_sls_vec_index_search(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "match_count", req.match_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "matches");
    uint32_t nshown = req.match_count < VEC_SEARCH_MAX_K ? req.match_count : VEC_SEARCH_MAX_K;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.matches[i].external_id); jb_putc(&j, ',');
        jb_uint(&j, "page_id", req.matches[i].id.page_id); jb_putc(&j, ',');
        jb_uint(&j, "slot_index", req.matches[i].id.slot_index); jb_putc(&j, ',');
        jb_key(&j, "distance");
        {
            double v = (double)req.matches[i].distance;
            char fb[32]; int fn = 0;
            if (v < 0) { fb[fn++] = '-'; v = -v; }
            uint64_t ip = (uint64_t)v; double frac = v - (double)ip;
            char ipb[24]; int il = 0;
            if (!ip) ipb[il++] = '0'; else { uint64_t t = ip; while (t) { ipb[il++] = (char)('0'+t%10); t/=10; } }
            for (int k = il-1; k >= 0; k--) fb[fn++] = ipb[k];
            fb[fn++] = '.';
            for (int d = 0; d < 6; d++) { frac *= 10.0; int digit = (int)frac; if (digit<0) digit=0; if (digit>9) digit=9; fb[fn++]=(char)('0'+digit); frac -= (double)digit; }
            fb[fn] = '\0';
            jb_raw(&j, fb);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/embed-search — VectorStore Interface Roadmap Phase 2 ────────
// The route this whole roadmap's #1-ranked gap named: search by typing
// query text instead of hand-pasting a float array. Body: {"collection":
// "<name>", "endpoint_ip": "...", "port": N, "model": "...", "prompt":
// "<query text>", "metric": "cosine"|"l2" (default cosine), "k": N}.
// endpoint_ip/port/model default exactly the same way POST /api/vec/
// embed-insert's own route already does, for consistency within this route
// family. Response shape is POST /api/vec/search's own shape (ok/
// match_count/truncated/matches[]) plus one extra field, ollama_status --
// same reasoning as sys_sls_vec_embed_search()'s own struct comment: a
// frontend needs to tell "Ollama never answered" apart from "Ollama
// answered fine, zero matches came back" (both otherwise look like
// match_count == 0).
static int api_vec_embed_search_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 3,992 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct SLSVecEmbedSearchRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    json_str_or_default(body, "endpoint_ip", req.ollama_req.endpoint_ip, OLLAMA_ENDPOINT_LEN, "10.0.2.2");
    int port = json_int(body, "port");
    req.ollama_req.port = (uint16_t)(port ? port : 11434);
    json_str_or_default(body, "model", req.ollama_req.model, OLLAMA_MODEL_LEN, "nomic-embed-text");
    req.ollama_req.prompt[0] = '\0';
    json_str(body, "prompt", req.ollama_req.prompt, OLLAMA_PROMPT_LEN);
    char metric_s[16]; metric_s[0] = '\0';
    json_str(body, "metric", metric_s, (int)sizeof(metric_s));
    req.metric = (!strcmp(metric_s, "l2") || !strcmp(metric_s, "L2")) ? VEC_METRIC_L2 : VEC_METRIC_COSINE;
    req.k = (uint32_t)json_int(body, "k");
    if (!req.k) req.k = 10;
    uint64_t rc = sys_sls_vec_embed_search(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "ollama_status", (uint64_t)req.ollama_status); jb_putc(&j,',');
    jb_uint(&j, "match_count", req.match_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "matches");
    uint32_t nshown = req.match_count < VEC_SEARCH_MAX_K ? req.match_count : VEC_SEARCH_MAX_K;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.matches[i].external_id); jb_putc(&j, ',');
        jb_uint(&j, "page_id", req.matches[i].id.page_id); jb_putc(&j, ',');
        jb_uint(&j, "slot_index", req.matches[i].id.slot_index); jb_putc(&j, ',');
        jb_key(&j, "distance");
        {
            double v = (double)req.matches[i].distance;
            char fb[32]; int fn = 0;
            if (v < 0) { fb[fn++] = '-'; v = -v; }
            uint64_t ip = (uint64_t)v; double frac = v - (double)ip;
            char ipb[24]; int il = 0;
            if (!ip) ipb[il++] = '0'; else { uint64_t t = ip; while (t) { ipb[il++] = (char)('0'+t%10); t/=10; } }
            for (int k = il-1; k >= 0; k--) fb[fn++] = ipb[k];
            fb[fn++] = '.';
            for (int d = 0; d < 6; d++) { frac *= 10.0; int digit = (int)frac; if (digit<0) digit=0; if (digit>9) digit=9; fb[fn++]=(char)('0'+digit); frac -= (double)digit; }
            fb[fn] = '\0';
            jb_raw(&j, fb);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/index/embed-search — VectorStore Interface Roadmap Phase 2 ──
// HNSW counterpart to POST /api/vec/embed-search above. Body: {"index":
// "<name>", "endpoint_ip": "...", "port": N, "model": "...", "prompt":
// "<query text>", "k": N, "ef": N (default = k)}. No "metric" field -- same
// as POST /api/vec/index/search's own body, since an HNSW index's metric is
// fixed at index-creation time, not chosen per query.
static int api_vec_index_embed_search_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    /* Static, not automatic -- see the "one request at a time" note below.
     * This struct carries its result/row array inline, so an automatic here
     * puts the whole array on the kernel stack. Sized by measurement, not
     * guess: this one is 3,976 bytes, and at -O2 GCC inlines this handler into
     * http_route() (single call site), so the cost lands in the ROUTER's
     * frame, not this function's -- which is why it was invisible until
     * -fno-inline moved it back here. */
    static struct SLSVecIndexEmbedSearchRequest req;
    req.caller_uid = req_uid;
    json_str(body, "index", req.index_name, OBJECT_NAME_LEN);
    json_str_or_default(body, "endpoint_ip", req.ollama_req.endpoint_ip, OLLAMA_ENDPOINT_LEN, "10.0.2.2");
    int port = json_int(body, "port");
    req.ollama_req.port = (uint16_t)(port ? port : 11434);
    json_str_or_default(body, "model", req.ollama_req.model, OLLAMA_MODEL_LEN, "nomic-embed-text");
    req.ollama_req.prompt[0] = '\0';
    json_str(body, "prompt", req.ollama_req.prompt, OLLAMA_PROMPT_LEN);
    req.k = (uint32_t)json_int(body, "k");
    if (!req.k) req.k = 10;
    req.ef = (uint32_t)json_int(body, "ef");
    if (!req.ef) req.ef = req.k;
    uint64_t rc = sys_sls_vec_index_embed_search(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "ollama_status", (uint64_t)req.ollama_status); jb_putc(&j,',');
    jb_uint(&j, "match_count", req.match_count); jb_putc(&j,',');
    jb_str(&j, "truncated", req.truncated ? "true" : "false"); jb_putc(&j,',');
    jb_arr_open(&j, "matches");
    uint32_t nshown = req.match_count < VEC_SEARCH_MAX_K ? req.match_count : VEC_SEARCH_MAX_K;
    for (uint32_t i = 0; i < nshown; i++) {
        if (i) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_uint(&j, "external_id", req.matches[i].external_id); jb_putc(&j, ',');
        jb_uint(&j, "page_id", req.matches[i].id.page_id); jb_putc(&j, ',');
        jb_uint(&j, "slot_index", req.matches[i].id.slot_index); jb_putc(&j, ',');
        jb_key(&j, "distance");
        {
            double v = (double)req.matches[i].distance;
            char fb[32]; int fn = 0;
            if (v < 0) { fb[fn++] = '-'; v = -v; }
            uint64_t ip = (uint64_t)v; double frac = v - (double)ip;
            char ipb[24]; int il = 0;
            if (!ip) ipb[il++] = '0'; else { uint64_t t = ip; while (t) { ipb[il++] = (char)('0'+t%10); t/=10; } }
            for (int k = il-1; k >= 0; k--) fb[fn++] = ipb[k];
            fb[fn++] = '.';
            for (int d = 0; d < 6; d++) { frac *= 10.0; int digit = (int)frac; if (digit<0) digit=0; if (digit>9) digit=9; fb[fn++]=(char)('0'+digit); frac -= (double)digit; }
            fb[fn] = '\0';
            jb_raw(&j, fb);
        }
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/index/rebuild — VectorStore Interface Roadmap Phase 3 ──
// Live path for vec_index_rebuild(): clears then repopulates a named HNSW
// index's contents from its current, live collection state -- closes both
// the never-backfills-an-existing-collection gap and cleans up tombstone
// buildup from delete churn. Body: {"index": "<name>"} -- field name
// "index" (not "name") matches this route family's own search routes'
// convention (POST /api/vec/index/search's own body uses "index"), a
// deliberate choice over matching DELETE /api/vec/indexes' "name" field --
// see this phase's own roadmap doc writeup for why.
static int api_vec_index_rebuild_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecIndexRebuildRequest req;
    req.caller_uid = req_uid;
    json_str(body, "index", req.index_name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_vec_index_rebuild(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vec/join — Gap Remediation Phase C ──────────────────────────────
// Live path for vec_join_resolve() (Vector Store Phase 5), which had no
// syscall/shell/HTTP surface at all before this pass. Body: {"table":
// "<name>", "id_column": "<col>", "matches": [{"external_id": N,
// "page_id": N, "slot_index": N, "distance": F}, ...]} -- takes matches
// directly (typically the "matches" array from a prior POST /api/vec/search
// or /api/vec/index/search response, round-tripped as-is) rather than
// re-running a search itself, matching sys_sls_vec_join()'s own real
// contract: this is the join primitive alone, composable with either search
// path via ordinary REST calls rather than a hidden second search.
// ─── Streaming join ────────────────────────────────────────────────────────
// vec_join_resolve() has always delivered rows one at a time through
// VecJoinRowCb, and this handler JSON-encodes them one at a time. The
// 264,192-byte results[] array inside SLSVecJoinRequest exists only so the
// ring-3 syscall ABI (sys_sls_vec_join) can hand a ring-3 caller a flat
// struct -- a caller that consumes rows sequentially never needed it. Going
// through the syscall wrapper here meant materialising all 64 rows just to
// walk them once, which was this function's entire 266,304-byte frame.
//
// One consequence, deliberate: result_count and truncated are emitted AFTER
// the results array rather than before it, because neither is known until the
// last row has streamed. JSON member order is not significant, and nothing in
// tests/ or the frontend reads these positionally (checked before reordering).
struct vj_emit_ctx {
    JSONBuf* j;
    uint32_t seen;      // counts PAST the cap -- this is how truncation is detected,
                        // matching vjs_collect_cb()'s own contract in vec_join.c
    uint32_t emitted;   // how many actually made it into the array
};

static void vj_emit_cb(const struct VecMatch* m, const struct RowValues* row, void* ctxp) {
    struct vj_emit_ctx* c = (struct vj_emit_ctx*)ctxp;
    if (c->seen < VEC_JOIN_MAX_RESULTS) {
        JSONBuf* j = c->j;
        if (c->emitted) jb_putc(j, ',');
        jb_obj_open(j, 0);
        jb_uint(j, "external_id", m->external_id); jb_putc(j, ',');
        jb_arr_open(j, "row");
        for (uint32_t cc = 0; cc < row->count; cc++) {
            if (cc) jb_putc(j, ',');
            // Phase 4 (SQL Feature-Parity Roadmap): same real-null
            // round-trip as sql_row_to_json_cb() above.
            if (row->null_mask & (1u << cc)) jb_raw(j, "null");
            else jb_esc_str(j, row->values[cc]);
        }
        jb_arr_close(j);
        jb_obj_close(j);
        c->emitted++;
    }
    c->seen++;
}

static int api_vec_join_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char table_name[OBJECT_NAME_LEN], id_column[RECORD_KEY_LEN];
    struct VecMatch matches[VEC_SEARCH_MAX_K];   // 1,536 bytes -- the caller-supplied
                                                 // side is small; only results[] was not
    json_str(body, "table", table_name, OBJECT_NAME_LEN);
    json_str(body, "id_column", id_column, RECORD_KEY_LEN);
    uint32_t n = 0;
    char objbuf[256];
    while (n < VEC_SEARCH_MAX_K && json_array_object_at(body, "matches", (int)n, objbuf, (int)sizeof(objbuf))) {
        matches[n].external_id  = json_uint64(objbuf, "external_id");
        matches[n].id.page_id    = (uint32_t)json_int(objbuf, "page_id");
        matches[n].id.slot_index = (uint32_t)json_int(objbuf, "slot_index");
        json_float(objbuf, "distance", &matches[n].distance);
        n++;
    }
    jb_obj_open(&j,0);
    // Unconditionally true, and it was before this change too: sys_sls_vec_join()
    // only ever returned non-zero for a NULL request (see vec_join.h -- "always
    // returns 0"), which a stack/static local could never be.
    jb_str(&j, "ok", "true"); jb_putc(&j,',');
    jb_uint(&j, "match_count", n); jb_putc(&j,',');
    jb_arr_open(&j, "results");
    struct vj_emit_ctx ctx = { &j, 0, 0 };
    vec_join_resolve(req_uid, table_name, id_column, matches, n, vj_emit_cb, &ctx);
    jb_arr_close(&j); jb_putc(&j,',');
    jb_uint(&j, "result_count", ctx.seen); jb_putc(&j,',');
    jb_str(&j, "truncated", ctx.seen > VEC_JOIN_MAX_RESULTS ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── DELETE /api/vec/vector — VectorStore Interface Roadmap Phase 1 ───────
// Live path for vecstore_delete() (already implemented since Vector Store
// Phase 1/2, never had an HTTP route). Body: {"collection": "<name>",
// "page_id": N, "slot_index": N} -- page_id/slot_index come from a prior
// POST /api/vec/insert or /api/vec/search response, not from external_id
// (VecId is a physical address, not keyed by external_id -- see
// vecstore.h's own struct VecId comment).
static int api_vec_delete(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecDeleteRequest req;
    req.caller_uid = req_uid;
    json_str(body, "collection", req.collection_name, OBJECT_NAME_LEN);
    req.id.page_id    = (uint32_t)json_int(body, "page_id");
    req.id.slot_index = (uint32_t)json_int(body, "slot_index");
    uint64_t rc = sys_sls_vec_delete(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── DELETE /api/vec/collections — VectorStore Interface Roadmap Phase 1 ──
// Routes straight through sys_sls_vfree() -- the one generic "delete this
// object" path every object type in this kernel already uses, now fixed
// (object_catalog.c) to also release vector-collection state and any HNSW
// indexes built over it, rather than a dedicated vecstore-only delete
// syscall duplicating that logic. Body: {"name": "<collection>"}. Same
// posture as the rest of this file's vfree-adjacent routes: no additional
// ownership/role check beyond the standard "must be authenticated" gate
// already applied to every DELETE route below -- sys_sls_vfree() itself
// has never done owner/role gating for any object type, a pre-existing
// gap out of scope for this roadmap to fix.
static int api_vec_collection_delete(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN];
    json_str(body, "name", name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_vfree(name);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_str(&j, "name", name);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── DELETE /api/vec/indexes — VectorStore Interface Roadmap Phase 1 ──────
// Live path for vec_index_drop(). Body: {"name": "<index>"}.
static int api_vec_index_delete(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVecIndexDropRequest req;
    req.caller_uid = req_uid;
    json_str(body, "name", req.index_name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_vec_index_drop(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false"); jb_putc(&j,',');
    jb_uint(&j, "status", (uint64_t)req.status);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/record (insert | update | delete) ─────────────────────────────
static int api_record_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSRecordRequest req;
    char op[16] = "insert";
    json_str(body, "object", req.name, OBJECT_NAME_LEN);
    if (!req.name[0]) json_str(body, "obj", req.name, OBJECT_NAME_LEN);
    json_str(body, "key",   req.key,   RECORD_KEY_LEN);
    json_str(body, "value", req.value, RECORD_VAL_LEN);
    json_str(body, "op",    op, 16);
    uint64_t rc = 1;
    if (!strcmp(op, "update")) rc = sys_sls_update(&req);
    else if (!strcmp(op, "delete")) rc = sys_sls_delete(&req);
    else rc = sys_sls_insert(&req);
    jb_obj_open(&j,0);
    jb_str(&j, "ok", rc==0 ? "true" : "false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/tx/begin|commit|rollback ──────────────────────────────────────
static int api_tx_post(const char* op, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    uint32_t tid = kernel_get_current_thread_id();
    uint64_t rc = 1;
    jb_obj_open(&j, 0);
    if (!strcmp(op, "begin")) {
        uint64_t tx = sys_sls_tx_begin(tid);
        jb_str(&j, "ok", tx ? "true" : "false"); jb_putc(&j, ',');
        jb_uint(&j, "tx_id", tx);
    } else if (!strcmp(op, "commit")) {
        rc = sys_sls_tx_commit(tid);
        jb_str(&j, "ok", rc==0 ? "true" : "false");
    } else if (!strcmp(op, "rollback")) {
        rc = sys_sls_tx_rollback(tid);
        jb_str(&j, "ok", rc==0 ? "true" : "false");
    } else {
        jb_str(&j, "error", "unknown tx op");
    }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Phase H: Agent / Workflow REST API ──────────────────────────────────────

// Convert a comma-separated tool name string to an AGENT_TOOL_* bitmask.
// Accepts both "db_select,db_query" and ["db_select","db_query"] formats.
static uint32_t parse_tool_mask(const char* s) {
    uint32_t m = 0;
    if (!s || !s[0]) return 0;
    if (str_find(s, "db_select"))    m |= AGENT_TOOL_DB_SELECT;
    if (str_find(s, "db_insert"))    m |= AGENT_TOOL_DB_INSERT;
    if (str_find(s, "db_query"))     m |= AGENT_TOOL_DB_QUERY;
    if (str_find(s, "stream_read"))  m |= AGENT_TOOL_STREAM_READ;
    if (str_find(s, "stream_write")) m |= AGENT_TOOL_STREAM_WRITE;
    if (str_find(s, "tier_promote")) m |= AGENT_TOOL_TIER_PROMOTE;
    if (str_find(s, "ipc_post"))     m |= AGENT_TOOL_IPC_POST;
    if (str_find(s, "agent_run"))    m |= AGENT_TOOL_AGENT_RUN;
    return m;
}

// ─── GET /api/agents ──────────────────────────────────────────────────────────
static int api_agents_list(char* buf, int max) {
    JSONBuf j = {buf,0,max};
    uint32_t cnt = 0;
    for (int i = 0; i < AGENT_MAX; i++) cnt += agent_table[i].active ? 1 : 0;
    jb_obj_open(&j,0);
    jb_uint(&j,"count",(uint64_t)cnt); jb_putc(&j,',');
    jb_arr_open(&j,"agents");
    int first = 1;
    for (int i = 0; i < AGENT_MAX; i++) {
        if (!agent_table[i].active) continue;
        struct AgentDescriptor* ag = &agent_table[i];
        if (!first) jb_putc(&j,','); first=0;
        jb_obj_open(&j,0);
        jb_str (&j,"name",     ag->name);                        jb_putc(&j,',');
        jb_str (&j,"model",    ag->model);                       jb_putc(&j,',');
        jb_str (&j,"endpoint", ag->inference_endpoint);          jb_putc(&j,',');
        jb_str (&j,"state",    agent_state_name(ag->state));     jb_putc(&j,',');
        jb_uint(&j,"steps",    (uint64_t)ag->step_count);        jb_putc(&j,',');
        jb_uint(&j,"tool_mask",(uint64_t)ag->tool_mask);          jb_putc(&j,',');
        jb_str (&j,"last_answer",ag->last_answer[0]?ag->last_answer:"");
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/agent/<name> ────────────────────────────────────────────────────
static int api_agent_status(const char* name, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    for (int i = 0; i < AGENT_MAX; i++) {
        if (!agent_table[i].active) continue;
        struct AgentDescriptor* ag = &agent_table[i];
        int match = 1;
        for (int k = 0; ag->name[k] || name[k]; k++)
            if (ag->name[k] != name[k]) { match=0; break; }
        if (!match) continue;
        jb_obj_open(&j,0);
        jb_str (&j,"name",      ag->name);                       jb_putc(&j,',');
        jb_str (&j,"model",     ag->model);                      jb_putc(&j,',');
        jb_str (&j,"endpoint",  ag->inference_endpoint);         jb_putc(&j,',');
        jb_str (&j,"state",     agent_state_name(ag->state));    jb_putc(&j,',');
        jb_uint(&j,"steps",     (uint64_t)ag->step_count);       jb_putc(&j,',');
        jb_uint(&j,"tool_mask", (uint64_t)ag->tool_mask);        jb_putc(&j,',');
        jb_uint(&j,"object_id", ag->object_id);                  jb_putc(&j,',');
        jb_uint(&j,"run_count", (uint64_t)ag->run_count);        jb_putc(&j,',');
        jb_uint(&j,"sched_ticks",(uint64_t)ag->schedule_ticks);  jb_putc(&j,',');
        jb_str (&j,"memory_table",ag->memory_table_name[0]?ag->memory_table_name:""); jb_putc(&j,',');
        jb_str (&j,"last_answer",ag->last_answer[0]?ag->last_answer:"");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j,0); jb_str(&j,"error","agent not found");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/agent/create ───────────────────────────────────────────────────
static int api_agent_create(const char* body, char* buf, int max,
                             uint32_t uid) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct AgentCreateRequest req;
    req.name[0]=req.inference_endpoint[0]=req.model[0]=req.system_prompt[0]='\0';
    req.tool_mask=0; req.owner_uid=uid;

    json_str(body,"name",          req.name,               OBJECT_NAME_LEN);
    json_str(body,"endpoint",      req.inference_endpoint, AGENT_ENDPOINT_LEN);
    json_str(body,"model",         req.model,              AGENT_MODEL_LEN);
    json_str(body,"system_prompt", req.system_prompt,      AGENT_PROMPT_LEN);

    static char tools_str[256]; tools_str[0]='\0';
    json_str(body,"tools",tools_str,sizeof(tools_str));
    req.tool_mask = parse_tool_mask(tools_str);

    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_agent_create(&req);
    jb_obj_open(&j,0);
    jb_str (&j,"ok",       rc==0?"true":"false");  jb_putc(&j,',');
    jb_str (&j,"name",     req.name);              jb_putc(&j,',');
    jb_uint(&j,"tool_mask",(uint64_t)req.tool_mask);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/agent/run ──────────────────────────────────────────────────────
// Blocks until the ReAct loop completes (may take several seconds for inference).
static int api_agent_run(const char* body, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct AgentRunRequest req;
    req.name[0]=req.message[0]='\0';
    json_str(body,"name",    req.name,    OBJECT_NAME_LEN);
    json_str(body,"message", req.message, AGENT_PROMPT_LEN);

    if (!req.name[0]||!req.message[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name and message required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_agent_run(&req);

    // Report step_count from the descriptor
    uint32_t steps = 0;
    for (int i = 0; i < AGENT_MAX; i++) {
        if (agent_table[i].active) {
            int m=1;
            for (int k=0; agent_table[i].name[k]||req.name[k]; k++)
                if (agent_table[i].name[k]!=req.name[k]){m=0;break;}
            if (m) { steps=agent_table[i].step_count; break; }
        }
    }
    jb_obj_open(&j,0);
    jb_str (&j,"ok",    rc==0?"true":"false"); jb_putc(&j,',');
    jb_str (&j,"agent", req.name);             jb_putc(&j,',');
    jb_uint(&j,"steps", (uint64_t)steps);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/agent/drop ─────────────────────────────────────────────────────
static int api_agent_drop(const char* body, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body,"name",name,OBJECT_NAME_LEN);
    if (!name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_agent_kill(name);
    jb_obj_open(&j,0);
    jb_str(&j,"ok",rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/workflows ───────────────────────────────────────────────────────
static int api_workflows_list(char* buf, int max) {
    JSONBuf j = {buf,0,max};
    uint32_t cnt = 0;
    for (int i=0;i<WORKFLOW_MAX;i++) cnt += workflow_table[i].active?1:0;
    jb_obj_open(&j,0);
    jb_uint(&j,"count",(uint64_t)cnt); jb_putc(&j,',');
    jb_arr_open(&j,"workflows");
    int first=1;
    for (int i=0;i<WORKFLOW_MAX;i++) {
        if (!workflow_table[i].active) continue;
        struct WorkflowDescriptor* wf = &workflow_table[i];
        if (!first) jb_putc(&j,','); first=0;
        jb_obj_open(&j,0);
        jb_str (&j,"name",  wf->name);                          jb_putc(&j,',');
        jb_str (&j,"state", workflow_state_name(wf->state));    jb_putc(&j,',');
        jb_uint(&j,"steps", (uint64_t)wf->step_count);          jb_putc(&j,',');
        jb_uint(&j,"current_step",(uint64_t)wf->current_step);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/workflow/<name> ─────────────────────────────────────────────────
static int api_workflow_status(const char* name, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    for (int i=0;i<WORKFLOW_MAX;i++) {
        if (!workflow_table[i].active) continue;
        struct WorkflowDescriptor* wf = &workflow_table[i];
        int m=1;
        for (int k=0;wf->name[k]||name[k];k++)
            if (wf->name[k]!=name[k]){m=0;break;}
        if (!m) continue;
        jb_obj_open(&j,0);
        jb_str (&j,"name",         wf->name);                       jb_putc(&j,',');
        jb_str (&j,"state",        workflow_state_name(wf->state)); jb_putc(&j,',');
        jb_uint(&j,"step_count",   (uint64_t)wf->step_count);       jb_putc(&j,',');
        jb_uint(&j,"current_step", (uint64_t)wf->current_step);     jb_putc(&j,',');
        jb_arr_open(&j,"steps");
        for (uint8_t s=0;s<wf->step_count;s++) {
            if (s) jb_putc(&j,',');
            jb_obj_open(&j,0);
            jb_str(&j,"agent",  wf->steps[s].agent_name); jb_putc(&j,',');
            jb_str(&j,"input",  wf->steps[s].input_key);  jb_putc(&j,',');
            jb_str(&j,"output", wf->steps[s].output_key);
            jb_obj_close(&j);
        }
        jb_arr_close(&j);
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    jb_obj_open(&j,0); jb_str(&j,"error","workflow not found");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/workflow/create ────────────────────────────────────────────────
// Body: {"name":"wf","shared_table":"tbl","step_count":2,
//        "step0_agent":"a1","step0_in":"q","step0_out":"r1",
//        "step1_agent":"a2","step1_in":"r1","step1_out":"ans"}
static int api_workflow_create(const char* body, char* buf, int max,
                                uint32_t uid) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct WorkflowCreateRequest req;
    req.name[0]=req.shared_state_table[0]='\0';
    req.step_count=0; req.owner_uid=uid;

    json_str(body,"name",         req.name,               OBJECT_NAME_LEN);
    json_str(body,"shared_table", req.shared_state_table, OBJECT_NAME_LEN);
    int sc = json_int(body,"step_count");
    if (sc < 0) sc = 0;
    if (sc > WORKFLOW_MAX_STEPS) sc = WORKFLOW_MAX_STEPS;
    req.step_count = (uint8_t)sc;

    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    // Parse per-step fields: step0_agent, step0_in, step0_out, ...
    for (int s=0; s<sc; s++) {
        char ka[32],ki[32],ko[32];
        // build key names manually (no sprintf available)
        ka[0]='s';ka[1]='t';ka[2]='e';ka[3]='p';ka[4]=(char)('0'+s);
        ka[5]='_';ka[6]='a';ka[7]='g';ka[8]='e';ka[9]='n';ka[10]='t';ka[11]='\0';
        ki[0]='s';ki[1]='t';ki[2]='e';ki[3]='p';ki[4]=(char)('0'+s);
        ki[5]='_';ki[6]='i';ki[7]='n';ki[8]='\0';
        ko[0]='s';ko[1]='t';ko[2]='e';ko[3]='p';ko[4]=(char)('0'+s);
        ko[5]='_';ko[6]='o';ko[7]='u';ko[8]='t';ko[9]='\0';
        req.steps[s].agent_name[0]=req.steps[s].input_key[0]=req.steps[s].output_key[0]='\0';
        json_str(body,ka,req.steps[s].agent_name,OBJECT_NAME_LEN);
        json_str(body,ki,req.steps[s].input_key, RECORD_KEY_LEN);
        json_str(body,ko,req.steps[s].output_key,RECORD_KEY_LEN);
    }
    uint64_t rc = sys_sls_workflow_create(&req);
    jb_obj_open(&j,0);
    jb_str (&j,"ok",  rc==0?"true":"false"); jb_putc(&j,',');
    jb_str (&j,"name",req.name);             jb_putc(&j,',');
    jb_uint(&j,"steps",(uint64_t)sc);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/workflow/run ───────────────────────────────────────────────────
static int api_workflow_run(const char* body, char* buf, int max) {
    JSONBuf j = {buf,0,max};
    if (!body) {
        jb_obj_open(&j,0); jb_str(&j,"error","missing body");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    static struct WorkflowRunRequest req;
    req.name[0]=req.input[0]='\0';
    json_str(body,"name",  req.name,  OBJECT_NAME_LEN);
    json_str(body,"input", req.input, AGENT_PROMPT_LEN);
    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_workflow_run(&req);
    jb_obj_open(&j,0);
    jb_str(&j,"ok",rc==0?"true":"false"); jb_putc(&j,',');
    jb_str(&j,"name",req.name);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Shell-Command JSON-Promotion Roadmap ──────────────────────────────────────
// Promotes user/shell.c's remaining SHELL_FALLBACK_COMMANDS (slsos-sim's
// shellCommands.ts) to real purpose-built JSON routes, matching every other
// command already in that file's COMMANDS registry, instead of going
// through POST /api/shell/exec's plain-text dispatch. Grouped below in the
// same 5 groups as the roadmap doc: security/session, process/service/IPC,
// journal/tier/object, webapp/workflow, legacy loader.

// ─── Shared helpers ─────────────────────────────────────────────────────────
// find object_id by name, mirroring the identical inline lookup loop that
// user/shell.c's chmod/write/seal handlers each already carry independently
// -- one shared helper here instead of three more copies.
static uint64_t hp_find_object_id(const char* name) {
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && !strcmp(object_catalog[i].name, name))
            return object_catalog[i].object_id;
    }
    return 0;
}

// Mirrors user/shell.c's parse_perm_string() exactly (lowercase r/w/x chars,
// defaults to PERM_READ if none matched) so JSON callers can use the same
// "rw"/"rwx" syntax shell users already do for grant/revoke.
static uint32_t hp_parse_perm_string(const char* s) {
    uint32_t m = 0;
    for (; *s; s++) {
        if (*s == 'r') m |= PERM_READ;
        if (*s == 'w') m |= PERM_WRITE;
        if (*s == 'x') m |= PERM_EXECUTE;
    }
    return m ? m : PERM_READ;
}

// Mirrors user/shell.c's parse_hex() exactly (accepts an optional "0x"/"0X"
// prefix).
static uint32_t hp_parse_hex(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    for (; *s; s++) {
        uint8_t d;
        if (*s >= '0' && *s <= '9') d = (uint8_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (uint8_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (uint8_t)(*s - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

// ─── Group 1: Security / session ───────────────────────────────────────────────

// ─── GET /api/session/whoami ───────────────────────────────────────────────────
// shell.c's "login <uid> <gid>" mutates a LOCAL session variable that only
// exists for the native serial console (current_session_uid/gid) or, over
// HTTP, a per-bearer-token ShellSession that api_shell_exec_post() forcibly
// reseeds from the bearer token on EVERY call ("always reseed identity from
// the bearer token, never trust stored state" -- see
// http_shell_session_for()'s own comment further below). A dedicated route
// that actually let a caller switch effective uid would reopen exactly the
// privilege-escalation hole Architectural Phase 4 closed for auth
// create/revoke, so this is deliberately read-only: it reflects the
// caller's REAL bearer-token identity, which is the honest HTTP equivalent
// of "who am I logged in as" -- not a state-mutating impersonation
// endpoint. "login" itself stays effectively a no-op over HTTP (matches
// syscall_dispatch.c's own SYS_SLS_SET_USER case comment: "shell updates
// session vars itself; no-op here").
static int api_session_whoami(uint32_t uid, SLSRole role, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "uid", uid); jb_putc(&j, ',');
    jb_str(&j, "role", role_name(role));
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/role/set ─────────────────────────────────────────────────────
// Body: {"uid": N, "role": "SYSTEM_KERNEL"|"DB_ADMIN"|"APP_USER"|"GUEST"}.
static int api_role_set_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSRoleRequest req;
    req.uid = (uint32_t)json_int(body, "uid");
    char role_s[24]; role_s[0]='\0';
    json_str(body, "role", role_s, sizeof(role_s));
    if      (!strcmp(role_s, "SYSTEM_KERNEL")) req.role = ROLE_SYSTEM_KERNEL;
    else if (!strcmp(role_s, "DB_ADMIN"))      req.role = ROLE_DB_ADMIN;
    else if (!strcmp(role_s, "APP_USER"))      req.role = ROLE_APP_USER;
    else                                        req.role = ROLE_GUEST;
    uint64_t rc = sys_sls_role_set(&req);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/grant | /api/revoke ─────────────────────────────────────────
// Body: {"uid": N, "object": "name", "perm": "rw"}. `is_grant` selects
// sys_sls_grant()'s add/remove direction -- same one-function-two-routes
// shape api_partition_pause_post() already established for pause/resume.
static int api_grant_post(const char* body, char* buf, int max, int is_grant) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSGrantRequest req;
    req.uid = (uint32_t)json_int(body, "uid");
    req.object_name[0] = '\0';
    json_str(body, "object", req.object_name, OBJECT_NAME_LEN);
    char perm_s[8]; perm_s[0]='\0';
    json_str(body, "perm", perm_s, sizeof(perm_s));
    req.perm_delta = hp_parse_perm_string(perm_s);
    uint64_t rc = sys_sls_grant(&req, is_grant);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/chmod ────────────────────────────────────────────────────────
// Body: {"name": "obj", "mask": "0x1F"}. Inlines the same object_catalog
// perm_mask update syscall_dispatch.c's SYS_SLS_CHMOD case does directly
// (that case is inline in the dispatcher's switch, not a standalone
// callable function -- see this codebase's established "inline manipulation
// of extern kernel state when no function exists" pattern, same as
// "workflow addstep" in Group 4 below).
static int api_chmod_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, OBJECT_NAME_LEN);
    char mask_s[16]; mask_s[0]='\0';
    json_str(body, "mask", mask_s, sizeof(mask_s));
    uint32_t mask = hp_parse_hex(mask_s);
    uint64_t obj_id = hp_find_object_id(name);
    int found = 0;
    if (obj_id) {
        for (uint32_t i = 0; i < object_catalog_count; i++) {
            if (object_catalog[i].active && object_catalog[i].object_id == obj_id) {
                object_catalog[i].perm_mask = mask;
                found = 1;
                break;
            }
        }
    }
    jb_obj_open(&j,0); jb_str(&j,"ok", found?"true":"false");
    if (!found) { jb_putc(&j,','); jb_str(&j,"error","object not found"); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/auth/create ──────────────────────────────────────────────────
// Body: {"email","uid","role","password"}. Gated to DB_ADMIN+ callers, same
// check user/shell.c's own "auth create" carries (Architectural Phase 4 --
// see that command's own header comment for the privilege-escalation gap
// this closes). Returns the plaintext token once, same as shell.c's own
// "[AUTH] Token: ..." print -- there is no other way for a caller to learn
// a freshly-created account's token.
static int api_auth_create_post(const char* body, char* buf, int max, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    if (req_role > ROLE_DB_ADMIN) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct AuthCreateRequest req;
    req.email[0] = '\0';
    json_str(body, "email", req.email, AUTH_EMAIL_LEN);
    req.uid = (uint32_t)json_int(body, "uid");
    char role_s[24]; role_s[0]='\0';
    json_str(body, "role", role_s, sizeof(role_s));
    if      (!strcmp(role_s, "SYSTEM_KERNEL")) req.role = ROLE_SYSTEM_KERNEL;
    else if (!strcmp(role_s, "DB_ADMIN"))      req.role = ROLE_DB_ADMIN;
    else if (!strcmp(role_s, "APP_USER"))      req.role = ROLE_APP_USER;
    else                                        req.role = ROLE_GUEST;
    char pw[64]; pw[0]='\0';
    json_str(body, "password", pw, sizeof(pw));
    uint32_t plen = 0; while (pw[plen]) plen++;
    for (uint32_t k = 0; k < plen && k < sizeof(req.password)-1; k++) req.password[k] = pw[k];
    req.password[plen < sizeof(req.password)-1 ? plen : sizeof(req.password)-1] = '\0';
    req.password_len = plen;
    char tok[AUTH_TOKEN_LEN + 1];
    int ok = auth_create_token(&req, tok);
    jb_obj_open(&j,0); jb_str(&j,"ok", ok?"true":"false");
    if (ok) { jb_putc(&j,','); jb_str(&j,"token", tok); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/auth/tokens ────────────────────────────────────────────────────
// Mirrors sys_sls_auth_list()'s own "first 8 chars only" token-preview
// convention (kernel/auth.c) rather than a fresh design -- never returns a
// full live token over this route.
static int api_auth_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "tokens");
    int first = 1;
    for (int i = 0; i < AUTH_MAX_TOKENS; i++) {
        if (!auth_tokens[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        char preview[10];
        int k = 0; for (; k < 8 && auth_tokens[i].token[k]; k++) preview[k] = auth_tokens[i].token[k];
        preview[k] = '\0';
        jb_obj_open(&j, 0);
        jb_str(&j, "email", auth_tokens[i].email); jb_putc(&j, ',');
        jb_uint(&j, "uid", auth_tokens[i].uid); jb_putc(&j, ',');
        jb_str(&j, "role", role_name(auth_tokens[i].role)); jb_putc(&j, ',');
        jb_str(&j, "token_preview", preview);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/auth/revoke ───────────────────────────────────────────────────
// Body: {"email"}. Same DB_ADMIN+ gate as api_auth_create_post() above, for
// the identical reason (Architectural Phase 4).
static int api_auth_revoke_post(const char* body, char* buf, int max, SLSRole req_role) {
    JSONBuf j = { buf, 0, max };
    if (req_role > ROLE_DB_ADMIN) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","requires DB_ADMIN or higher");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char email[AUTH_EMAIL_LEN]; email[0]='\0';
    json_str(body, "email", email, AUTH_EMAIL_LEN);
    int revoked = auth_revoke_by_email(email);
    jb_obj_open(&j,0); jb_str(&j,"ok", revoked?"true":"false"); jb_putc(&j,',');
    jb_uint(&j, "revoked", (uint32_t)revoked);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/seal ──────────────────────────────────────────────────────────
// Body: {"name","password"}. See secure_api.h's own header comment: this
// binds a password-derived key to the object, it does NOT encrypt the
// object's stored data (Gap Remediation Phase E relabeled this honestly;
// not repeated here as a fresh claim).
static int api_seal_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, OBJECT_NAME_LEN);
    char pw[32]; pw[0]='\0';
    json_str(body, "password", pw, sizeof(pw));
    uint64_t obj_id = hp_find_object_id(name);
    if (!obj_id || !pw[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error", !obj_id ? "object not found" : "password required");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    struct SLSSealRequest req;
    req.system_object_id = obj_id;
    uint32_t pwlen = 0; while (pw[pwlen]) pwlen++;
    for (uint32_t k = 0; k < 32 && k < pwlen; k++) req.user_password[k] = pw[k];
    req.password_len = pwlen < 32 ? pwlen : 31;
    req.encryption_algorithm_flags = 1;
    uint64_t rc = sys_sls_secure_seal(&req);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Group 2: Process / service / IPC ──────────────────────────────────────────

// ─── POST /api/svc/crash | /api/svc/restart ────────────────────────────────
// Body: {"name"}.
static int api_svc_crash_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[SVC_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, SVC_NAME_LEN);
    uint64_t rc = sys_sls_svc_crash(name);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}
static int api_svc_restart_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[SVC_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, SVC_NAME_LEN);
    uint64_t rc = sys_sls_svc_restart(name);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/proc/kill ────────────────────────────────────────────────────
// Body: {"pid"}. process_kill() (kernel/process.c) returns void, so this
// checks proc_table[] for the pid first (same "confirm existence, then act"
// approach api_chmod_post() above uses for its own void-returning inline
// operation) to give the caller a real ok/error rather than always "true".
static int api_proc_kill_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    uint32_t pid = (uint32_t)json_int(body, "pid");
    int found = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].active && proc_table[i].pid == pid) { found = 1; break; }
    }
    if (found) process_kill(pid);
    jb_obj_open(&j,0); jb_str(&j,"ok", found?"true":"false");
    if (!found) { jb_putc(&j,','); jb_str(&j,"error","pid not found"); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/ipc/post ─────────────────────────────────────────────────────
// Body: {"service","opcode"}. opcode is a hex string ("0x0401"), matching
// shell.c's own "ipc post <svc> <opcode_hex>" syntax -- resolves the target
// service's port the same way shell.c does (linear scan of services[]),
// since no by-name lookup function exists for it.
static int api_ipc_post_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char svc_name[SVC_NAME_LEN]; svc_name[0]='\0';
    json_str(body, "service", svc_name, SVC_NAME_LEN);
    char opcode_s[16]; opcode_s[0]='\0';
    json_str(body, "opcode", opcode_s, sizeof(opcode_s));
    uint32_t opcode = hp_parse_hex(opcode_s);
    uint16_t target_port = 0;
    for (uint32_t i = 0; i < service_count; i++) {
        if (!strcmp(services[i].name, svc_name)) { target_port = services[i].port; break; }
    }
    if (!target_port) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","service not found");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    struct IPCPostRequest req;
    req.msg.src_port = 0x0000;
    req.msg.dst_port = target_port;
    req.msg.opcode = opcode;
    req.msg.payload[0]=0; req.msg.payload[1]=0; req.msg.payload[2]=0; req.msg.payload[3]=0;
    req.msg.reply_token = 0;
    uint64_t rc = sys_sls_ipc_post(&req);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false"); jb_putc(&j,',');
    jb_uint(&j, "port", target_port);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/ipc/stat ───────────────────────────────────────────────────────
// shell.c's "ipc stat" is actually aliased to sys_sls_svc_list() in
// syscall_dispatch.c's dispatcher ("combined view" -- see that case's own
// comment), which would just duplicate the existing GET /api/services under
// a different name. This route instead surfaces the real ipc_stats extern
// struct (kernel/ipc.c) plus a per-queue depth breakdown via
// ipc_queue_depth() -- genuinely distinct data that had no JSON route at
// all before this, a more honest promotion of "ipc stat" than literally
// mirroring shell.c's own aliasing quirk would have been.
static int api_ipc_stat(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_uint(&j, "total_posted", ipc_stats.total_posted); jb_putc(&j, ',');
    jb_uint(&j, "total_dispatched", ipc_stats.total_dispatched); jb_putc(&j, ',');
    jb_uint(&j, "total_dropped", ipc_stats.total_dropped); jb_putc(&j, ',');
    jb_uint(&j, "avg_latency_ns", ipc_stats.avg_latency_ns); jb_putc(&j, ',');
    jb_arr_open(&j, "queues");
    for (int p = IPC_PORT_FIRST; p <= IPC_PORT_LAST; p++) {
        if (p != IPC_PORT_FIRST) jb_putc(&j, ',');
        jb_obj_open(&j, 0);
        jb_hex(&j, "port", (uint16_t)p); jb_putc(&j, ',');
        jb_uint(&j, "depth", (uint32_t)ipc_queue_depth((uint16_t)p));
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Group 3: Journal / tier / object ──────────────────────────────────────────

// ─── POST /api/journal ───────────────────────────────────────────────────────
// Body: {"name"}. Mirrors shell.c's "journal create <name>" exactly: a
// valloc of an OBJ_TYPE_JOURNAL object with size_pages=1, perm_mask=0 --
// genuinely just sys_sls_valloc() under a friendlier name/shape, not new
// kernel behavior. (Distinct from journal_attach()/journal_detach(), which
// already have their own /api/journal/attach and /api/journal/detach
// routes from an earlier phase.)
static int api_journal_create_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct SLSVallocRequest req;
    req.name[0] = '\0';
    json_str(body, "name", req.name, OBJECT_NAME_LEN);
    if (!req.name[0]) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","name required"); jb_obj_close(&j);
        j.buf[j.pos]='\0'; return j.pos;
    }
    req.type = OBJ_TYPE_JOURNAL;
    req.size_pages = 1;
    req.owner_uid = req_uid;
    req.perm_mask = 0;
    req.partition_id = 0;
    req.database_id = 0;    // VectorStore Gap Analysis §3: was uninitialized stack garbage until this fix
    uint64_t id = sys_sls_valloc(&req);
    jb_obj_open(&j,0);
    if (id) { jb_str(&j,"ok","true"); jb_putc(&j,','); jb_hex(&j,"object_id",id); }
    else    { jb_str(&j,"ok","false"); jb_putc(&j,','); jb_str(&j,"error","valloc failed"); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/journal/purge ────────────────────────────────────────────────
// Body: {"name"}. journal_purge() (kernel/journal.c) returns void and
// shell.c's own "journal purge" prints nothing either way -- there is
// genuinely no success/failure signal anywhere in this call today, so this
// always reports ok:true rather than fabricating a check the underlying
// function doesn't perform.
static int api_journal_purge_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[64]; name[0]='\0';
    json_str(body, "name", name, sizeof(name));
    journal_purge(name);
    jb_obj_open(&j,0); jb_str(&j,"ok","true");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/tier/promote | /api/tier/demote ─────────────────────────────
// Body: {"name"}.
static int api_tier_promote_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_tier_promote(name);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}
static int api_tier_demote_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_tier_demote(name);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/vfree ─────────────────────────────────────────────────────────
// Body: {"name"}.
static int api_vfree_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, OBJECT_NAME_LEN);
    uint64_t rc = sys_sls_vfree(name);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Group 4: Webapp / workflow ────────────────────────────────────────────────

// ─── POST /api/webapp/set | /api/webapp/append ─────────────────────────────
// Body: {"obj","path","content"}. `is_append` selects
// WebAppSetRequest.append -- same one-function-two-routes shape used
// throughout this file (partition pause/resume, tier promote/demote, etc.).
static int api_webapp_set_post(const char* body, char* buf, int max, int is_append) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    struct WebAppSetRequest req;
    req.obj_name[0] = '\0'; req.path[0] = '\0'; req.content[0] = '\0';
    json_str(body, "obj", req.obj_name, OBJECT_NAME_LEN);
    json_str(body, "path", req.path, WEBAPP_PATH_LEN);
    json_str(body, "content", req.content, WEBAPP_CONTENT_LEN);
    uint32_t clen = 0; while (req.content[clen]) clen++;
    req.content_len = clen;
    req.append = (uint8_t)is_append;
    uint64_t rc = sys_sls_webapp_set(&req);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/webapp/list?obj=<name> ────────────────────────────────────────
// obj omitted or "*" lists every stored asset across every WEB_APP object,
// matching sys_sls_webapp_list()'s own "*" convention (kernel/webapp.c) --
// iterates the extern webapp_store[] directly rather than that function's
// serial-print body, same pattern every other "list" route in this file
// already uses.
static int api_webapp_list(const char* obj_filter, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    int all = (!obj_filter || !obj_filter[0] || !strcmp(obj_filter, "*"));
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "assets");
    int first = 1;
    for (int i = 0; i < WEBAPP_MAX_ASSETS; i++) {
        if (!webapp_store[i].active) continue;
        if (!all && strcmp(webapp_store[i].obj_name, obj_filter)) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "obj", webapp_store[i].obj_name); jb_putc(&j, ',');
        jb_str(&j, "path", webapp_store[i].path); jb_putc(&j, ',');
        jb_str(&j, "mime", webapp_store[i].mime); jb_putc(&j, ',');
        jb_uint(&j, "content_len", webapp_store[i].content_len);
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/workflow/addstep ─────────────────────────────────────────────
// Body: {"workflow","agent","in","out"}. Inlines the same workflow_table[]
// mutation user/shell.c's own "workflow addstep" carries directly (no
// standalone function exists for this -- same "inline manipulation of
// extern kernel state" pattern api_chmod_post() above uses).
static int api_workflow_addstep_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char wf_name[OBJECT_NAME_LEN]; wf_name[0]='\0';
    json_str(body, "workflow", wf_name, OBJECT_NAME_LEN);
    char agent[OBJECT_NAME_LEN]; agent[0]='\0';
    json_str(body, "agent", agent, OBJECT_NAME_LEN);
    char in_key[RECORD_KEY_LEN]; in_key[0]='\0';
    json_str(body, "in", in_key, RECORD_KEY_LEN);
    char out_key[RECORD_KEY_LEN]; out_key[0]='\0';
    json_str(body, "out", out_key, RECORD_KEY_LEN);

    int done = 0;
    for (int wi = 0; wi < WORKFLOW_MAX; wi++) {
        if (!workflow_table[wi].active || strcmp(workflow_table[wi].name, wf_name)) continue;
        uint8_t s = workflow_table[wi].step_count;
        if (s >= WORKFLOW_MAX_STEPS) {
            jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
            jb_str(&j,"error","step table full");
            jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
        }
        // Manual copy loop rather than strncpy -- this file never includes
        // <string.h> and relies on implicit strcmp/strlen declarations
        // throughout, but strncpy specifically had no prior call site here,
        // so it's avoided rather than adding a new implicit-declaration
        // dependency.
        { uint32_t ci = 0; while (agent[ci] && ci < OBJECT_NAME_LEN-1) { workflow_table[wi].steps[s].agent_name[ci] = agent[ci]; ci++; } workflow_table[wi].steps[s].agent_name[ci] = '\0'; }
        { uint32_t ci = 0; while (in_key[ci] && ci < RECORD_KEY_LEN-1) { workflow_table[wi].steps[s].input_key[ci] = in_key[ci]; ci++; } workflow_table[wi].steps[s].input_key[ci] = '\0'; }
        { uint32_t ci = 0; while (out_key[ci] && ci < RECORD_KEY_LEN-1) { workflow_table[wi].steps[s].output_key[ci] = out_key[ci]; ci++; } workflow_table[wi].steps[s].output_key[ci] = '\0'; }
        workflow_table[wi].step_count++;
        done = 1;
        break;
    }
    jb_obj_open(&j,0); jb_str(&j,"ok", done?"true":"false");
    if (!done) { jb_putc(&j,','); jb_str(&j,"error","workflow not found"); }
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── Group 5: Legacy loader ─────────────────────────────────────────────────────
// All five of these are the older TIMI/SIMI-predecessor loader subsystem
// (kernel/loader.h) plus one raw-pointer heap primitive (SYS_SLS_ALLOCATE) --
// already explicitly labeled "legacy" in shellCommands.ts, superseded by
// "program upload"/"stream upload" (loader.h's own newer object-catalog-
// based upload flow, already promoted) and "insert"/"sql" for data writes.
// Included in this pass per explicit user choice rather than skipped, but
// nothing below grants any new capability -- every one of these was already
// reachable at the same trust level through POST /api/shell/exec.

// ─── POST /api/write ─────────────────────────────────────────────────────────
// Body: {"name","payload"}. Legacy raw heap write -- finds the object's
// mapped base_vaddr and writes the payload string directly into that
// address. shell.c's "write" command reaches this via
// do_syscall(SYS_SLS_ALLOCATE, ...), which syscall_dispatch.c actually
// routes to its own `static sls_legacy_allocate()` (NOT kernel/stubs.c's
// sys_sls_allocate() -- that function is undeclared in any header and
// appears to be dead from this call path, reachable only via the raw
// assembly syscall trampoline). sls_legacy_allocate() is itself `static`
// with no header declaration, so its object_id-lookup body is replicated
// inline here rather than called, matching this file's established
// "inline manipulation of extern kernel state when no proper function
// exists" pattern (see api_chmod_post, api_workflow_addstep_post). Since
// AeroSLS is a single flat address space, the numeric base_vaddr is
// directly usable as a pointer in this process too.
static int api_write_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[OBJECT_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, OBJECT_NAME_LEN);
    char payload[4096]; payload[0]='\0';
    json_str(body, "payload", payload, sizeof(payload));
    uint64_t base_vaddr = 0;
    for (uint32_t i = 0; i < object_catalog_count; i++) {
        if (object_catalog[i].active && !strcmp(object_catalog[i].name, name)) {
            base_vaddr = object_catalog[i].base_vaddr;
            break;
        }
    }
    if (!base_vaddr) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","object not found");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    char* ptr = (char*)(uintptr_t)base_vaddr;
    uint32_t i = 0; while (payload[i] && i < 4095) { ptr[i] = payload[i]; i++; }
    ptr[i] = '\0';
    jb_obj_open(&j,0); jb_str(&j,"ok","true");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/demo ──────────────────────────────────────────────────────────
// Body: {"name"}. Writes the built-in demo binary (aerosls_demo_bin[]) to
// the named object, then loads + spawns it -- exact mirror of shell.c's own
// "demo <name>" (upload-then-load in one call).
static int api_demo_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    static struct SLSUploadRequest req;
    req.object_name[0] = '\0';
    json_str(body, "name", req.object_name, PROC_NAME_LEN);
    req.byte_offset = 0;
    req.chunk_len = aerosls_demo_bin_size < UPLOAD_CHUNK_MAX ? aerosls_demo_bin_size : UPLOAD_CHUNK_MAX;
    for (uint32_t i = 0; i < req.chunk_len; i++) req.chunk[i] = aerosls_demo_bin[i];
    req.is_last = 1;
    sys_sls_upload_binary(&req);
    uint64_t entry = sys_sls_load(req.object_name, req_uid);
    jb_obj_open(&j,0); jb_str(&j,"ok", entry!=0?"true":"false");
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/load ───────────────────────────────────────────────────────────
// Body: {"name"}. Loads an already-uploaded binary (via "upload" below, or
// the newer "program upload") and spawns it as a process.
static int api_load_post(const char* body, char* buf, int max, uint32_t req_uid) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    char name[PROC_NAME_LEN]; name[0]='\0';
    json_str(body, "name", name, PROC_NAME_LEN);
    uint64_t entry = sys_sls_load(name, req_uid);
    jb_obj_open(&j,0); jb_str(&j,"ok", entry!=0?"true":"false"); jb_putc(&j,',');
    jb_hex(&j, "entry_point", entry);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── GET /api/loader/list ────────────────────────────────────────────────────
// Iterates the extern service_binaries[] directly (kernel/loader.h), same
// pattern every other "list" route in this file uses, rather than
// loader_list()'s own serial-print body.
static int api_loader_list(char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    jb_obj_open(&j, 0);
    jb_arr_open(&j, "binaries");
    int first = 1;
    for (int i = 0; i < MAX_SERVICE_BINARIES; i++) {
        if (!service_binaries[i].active) continue;
        if (!first) jb_putc(&j, ','); first = 0;
        jb_obj_open(&j, 0);
        jb_str(&j, "name", service_binaries[i].object_name); jb_putc(&j, ',');
        jb_uint(&j, "size", service_binaries[i].size); jb_putc(&j, ',');
        jb_str(&j, "format", binary_format_name(&service_binaries[i]));
        jb_obj_close(&j);
    }
    jb_arr_close(&j);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// ─── POST /api/upload ────────────────────────────────────────────────────────
// Body: {"name","hex"}. Legacy single-shot loader upload (always
// is_last=1, no offset/chunking support) -- note this ends up calling the
// exact same sys_sls_upload_binary()/struct SLSUploadRequest the already-
// existing POST /api/program/upload route does; "upload" is a strict
// subset of that route's own chunked-upload capability, not new
// functionality. Reuses that route's hex_decode() helper rather than
// re-deriving hex parsing a third time in this file.
static int api_upload_post(const char* body, char* buf, int max) {
    JSONBuf j = { buf, 0, max };
    if (!body) { jb_obj_open(&j,0); jb_str(&j,"error","missing body"); jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos; }
    static struct SLSUploadRequest req;
    req.object_name[0] = '\0';
    json_str(body, "name", req.object_name, PROC_NAME_LEN);
    static char hex_buf[UPLOAD_CHUNK_MAX * 2 + 4];
    hex_buf[0] = '\0';
    json_str(body, "hex", hex_buf, (int)sizeof(hex_buf));
    req.byte_offset = 0;
    req.chunk_len = (uint32_t)hex_decode(hex_buf, req.chunk, UPLOAD_CHUNK_MAX);
    req.is_last = 1;
    if (req.chunk_len == 0) {
        jb_obj_open(&j,0); jb_str(&j,"ok","false"); jb_putc(&j,',');
        jb_str(&j,"error","hex decode produced zero bytes");
        jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
    }
    uint64_t rc = sys_sls_upload_binary(&req);
    jb_obj_open(&j,0); jb_str(&j,"ok", rc==0?"true":"false"); jb_putc(&j,',');
    jb_uint(&j, "bytes_written", req.chunk_len);
    jb_obj_close(&j); j.buf[j.pos]='\0'; return j.pos;
}

// req[] is a NUL-terminated string of the raw HTTP request.
static void http_route(int conn, char* req) {
    // Parse: METHOD /path[?qs] HTTP/x.x
    char method[8], path[128], qs[256];
    method[0] = path[0] = qs[0] = '\0';
    char* p = req;
    int mi = 0;
    while (*p && *p != ' ' && mi < 7) method[mi++] = *p++;
    method[mi] = '\0';
    while (*p == ' ') p++;
    int pi = 0;
    while (*p && *p != ' ' && *p != '?' && *p != '\r' && pi < 127) path[pi++] = *p++;
    path[pi] = '\0';
    if (*p == '?') {
        p++; int qi = 0;
        while (*p && *p != ' ' && *p != '\r' && qi < 255) qs[qi++] = *p++;
        qs[qi] = '\0';
    }

    const char* body_ptr = http_body(req);
    static char resp_body[16384];
    int blen = 0;

    // Architectural Phase 3: resolve this request's CORS origin once, before
    // any response helper (including http_options() below) runs.
    http_resolve_cors_origin(req);

    // OPTIONS: CORS preflight
    if (method[0] == 'O') { http_options(conn); return; }

    int is_post = (method[0] == 'P');
    // VectorStore Interface Roadmap Phase 1: first real DELETE routes this
    // server has ever had -- confirmed via grep before this that no method
    // check for "DELETE" existed anywhere in this file. Before this fix, a
    // DELETE request would have silently fallen into the `!is_post` GET
    // branch below (passing its own bearer-token gate, then matching none
    // of the GET routes and hitting the generic 404) rather than being
    // routed to anything meaningful -- this makes it its own real branch
    // instead, same shape as is_post.
    int is_delete = (method[0] == 'D');

    // ── GET routes ────────────────────────────────────────────────────────────
    if (!is_post && !is_delete) {
        // Gap Remediation Phase E: bearer-token gate for GET routes, mirroring
        // the POST gate further below. Scoped to /api/* data routes only --
        // /auth/token and /auth/verify stay public (issuing or checking a
        // token can't itself require a valid one), /api/health stays public
        // as a conventional unauthenticated liveness probe, and everything
        // NOT under /api/ (the compiled-in Navigator SPA bundle + dynamic
        // WEB_APP assets, matched further down) stays public too -- the
        // SPA's own login screen has to be reachable before a caller has a
        // token to present in the first place.
        if (str_find(path, "/api/") == path && strcmp(path, "/api/health") != 0) {
            uint32_t get_uid = 0; SLSRole get_role = ROLE_GUEST;
            auth_http_extract(req, &get_uid, &get_role);
            if (get_role == ROLE_GUEST) {
                const char* e401 = "{\"error\":\"Unauthorized — include Authorization: Bearer <token>\"}";
                http_respond(conn, 401, "application/json", e401, (int)strlen(e401));
                return;
            }
            if (!http_partition_rate_check(get_uid)) {
                const char* e429 = "{\"error\":\"Rate limit exceeded for this partition — try again shortly\"}";
                http_respond(conn, 429, "application/json", e429, (int)strlen(e429));
                return;
            }
            // Multitenant Isolation Gap Analysis §5 item 6 / §7 item 6: record
            // this request against the caller's partition's cumulative usage
            // total -- deliberately separate from http_partition_rate_check()'s
            // own window counter above, which resets and is unsuitable for a
            // running usage total. See usage_metering.h's own header comment.
            usage_metering_record_request(get_uid);
        }
        if (!strcmp(path, "/auth/verify")) {
            blen = api_auth_verify(req, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Shell-Command JSON-Promotion Roadmap: new GET routes ───────────────
        if (!strcmp(path, "/api/session/whoami")) {
            // Re-extracts identity locally (get_uid/get_role above are scoped
            // to the gate block right above, not visible down here) -- same
            // "each dispatch section calls auth_http_extract() with its own
            // local vars" pattern the POST and DELETE blocks below already
            // use independently.
            uint32_t who_uid = 0; SLSRole who_role = ROLE_GUEST;
            auth_http_extract(req, &who_uid, &who_role);
            blen = api_session_whoami(who_uid, who_role, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/auth/tokens")) {
            blen = api_auth_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/ipc/stat")) {
            blen = api_ipc_stat(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/webapp/list")) {
            char obj_filter[OBJECT_NAME_LEN]; obj_filter[0] = '\0';
            url_param(qs, "obj", obj_filter, OBJECT_NAME_LEN);
            blen = api_webapp_list(obj_filter, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/loader/list")) {
            blen = api_loader_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/entropy")) {
            blen = api_entropy(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen);
            return;
        }
        if (!strcmp(path, "/api/health")) {
            blen = api_health(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/checkpoints")) {
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_uint(&j, "count",    checkpoint_count());        jb_putc(&j, ',');
            jb_uint(&j, "last_seq", checkpoint_last_sequence());
            jb_obj_close(&j);
            j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/metrics")) {
            blen = api_metrics(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/scan")) {
            blen = api_scan(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/objects")) {
            blen = api_objects(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/objects/") == path) {
            blen = api_object_detail(path + 13, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase C: GET /api/vec/collections, /api/vec/indexes ──
        if (!strcmp(path, "/api/vec/collections")) {
            blen = api_vec_collections_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/indexes")) {
            blen = api_vec_indexes_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase F: GET /api/partitions, /api/partition/quotas ──
        if (!strcmp(path, "/api/partitions")) {
            blen = api_partitions_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/quotas")) {
            blen = api_partition_quotas_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Multitenant Isolation Gap Analysis §5 item 8: GET /api/partition/cpuweights ──
        if (!strcmp(path, "/api/partition/cpuweights")) {
            blen = api_partition_cpuweights_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Storage Isolation Roadmap Phase 1: GET /api/partition/storagequotas ──
        if (!strcmp(path, "/api/partition/storagequotas")) {
            blen = api_partition_storagequotas_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Network Fairness Phase 2: GET /api/partition/connquotas ────────────
        if (!strcmp(path, "/api/partition/connquotas")) {
            blen = api_partition_connquotas_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Cluster view: GET /api/cluster, GET /api/nodes ─────────────────────
        if (!strcmp(path, "/api/cluster")) {
            blen = api_cluster_view(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/nodes")) {
            blen = api_cluster_nodes(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Orchestration Plan Phase 6: GET /api/mesh ──────────────────────────
        if (!strcmp(path, "/api/mesh")) {
            blen = api_mesh_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Orchestration Plan Phase 5: GET /api/workloads ─────────────────────
        if (!strcmp(path, "/api/workloads")) {
            blen = api_workloads_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Orchestration Plan Phase 4: GET /api/services ──────────────────────
        if (!strcmp(path, "/api/services")) {
            blen = api_services_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Orchestration Plan Phase 4: GET /api/service/resolve/<name> ────────
        if (str_find(path, "/api/service/resolve/") == path) {
            blen = api_service_resolve(path + 21, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Multitenant Isolation Gap Analysis §5 item 6: GET /api/usage ───────
        if (!strcmp(path, "/api/usage")) {
            blen = api_usage_report(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Multitenant Isolation Gap Analysis §5 item 1: GET /api/tenants ─────
        if (!strcmp(path, "/api/tenants")) {
            blen = api_tenants_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase G: GET /api/simi/<name> ──────────────────────
        if (str_find(path, "/api/simi/") == path) {
            blen = api_simi_info(path + 10, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase B: GET /api/tables[/<name>/schema] ──────────
        if (!strcmp(path, "/api/tables")) {
            blen = api_tables_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── SQL Feature-Parity Roadmap, Phase 8 follow-on: GET /api/schema/export ──
        if (!strcmp(path, "/api/schema/export")) {
            uint32_t exp_uid = 0; SLSRole exp_role = ROLE_GUEST;
            auth_http_extract(req, &exp_uid, &exp_role);
            blen = api_schema_export(exp_uid, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── VectorStore Interface Roadmap follow-on: GET /api/vec/schema/export ──
        if (!strcmp(path, "/api/vec/schema/export")) {
            uint32_t vexp_uid = 0; SLSRole vexp_role = ROLE_GUEST;
            auth_http_extract(req, &vexp_uid, &vexp_role);
            blen = api_vec_schema_export(vexp_uid, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── VectorStore Interface Roadmap follow-on: GET /api/vec/data/export/
        // <collection>[/skip/<N>] -- the optional "/skip/<N>" suffix is
        // VectorStore Gap Analysis §1.4 (closed); see api_vec_data_export()'s
        // own comment above for why this is a path segment, not a query
        // string, and why splitting on the "/skip/" marker is unambiguous. ──
        if (str_find(path, "/api/vec/data/export/") == path) {
            const char* rest = path + 21;   // after "/api/vec/data/export/"
            char cname[OBJECT_NAME_LEN]; cname[0] = '\0';
            uint32_t skip_count = 0;
            const char* skip_marker = str_find(rest, "/skip/");
            if (skip_marker) {
                uint32_t n = (uint32_t)(skip_marker - rest);
                if (n >= OBJECT_NAME_LEN) n = OBJECT_NAME_LEN - 1;
                for (uint32_t k = 0; k < n; k++) cname[k] = rest[k];
                cname[n] = '\0';
                skip_count = path_parse_u32(skip_marker + 6);   // 6 == strlen("/skip/")
            } else {
                uint32_t n = 0;
                while (rest[n] && n < OBJECT_NAME_LEN - 1) { cname[n] = rest[n]; n++; }
                cname[n] = '\0';
            }
            if (cname[0]) {
                uint32_t vdexp_uid = 0; SLSRole vdexp_role = ROLE_GUEST;
                auth_http_extract(req, &vdexp_uid, &vdexp_role);
                blen = api_vec_data_export(vdexp_uid, cname, skip_count, resp_body, (int)sizeof(resp_body));
                http_respond(conn, 200, "application/json", resp_body, blen); return;
            }
            const char* e = "{\"error\":\"missing collection name -- expected /api/vec/data/export/<collection>[/skip/<N>]\"}";
            http_respond(conn, 404, "application/json", e, (int)strlen(e)); return;
        }
        if (str_find(path, "/api/tables/") == path) {
            const char* rest = path + 12;   // after "/api/tables/"
            const char* suffix = str_find(rest, "/schema");
            char tname[OBJECT_NAME_LEN]; tname[0] = '\0';
            if (suffix && suffix[7] == '\0') {   // exact trailing "/schema", nothing after
                uint32_t n = (uint32_t)(suffix - rest);
                if (n >= OBJECT_NAME_LEN) n = OBJECT_NAME_LEN - 1;
                for (uint32_t k = 0; k < n; k++) tname[k] = rest[k];
                tname[n] = '\0';
            }
            if (tname[0]) {
                blen = api_table_schema(tname, resp_body, (int)sizeof(resp_body));
                http_respond(conn, 200, "application/json", resp_body, blen); return;
            }
            const char* e = "{\"error\":\"unknown /api/tables/ route -- try /api/tables/<name>/schema\"}";
            http_respond(conn, 404, "application/json", e, (int)strlen(e)); return;
        }
        if (!strcmp(path, "/api/services")) {
            blen = api_services_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Navigator-Parity Gap Roadmap Phase 3: real security backend ────────
        if (!strcmp(path, "/api/security/audit")) {
            blen = api_security_audit_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/security/groups")) {
            blen = api_security_groups_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/security/authlists")) {
            blen = api_security_authlists_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/security/databases")) {
            blen = api_security_databases_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Navigator-Parity Gap Roadmap Phase 4: Work Management visibility ──
        if (!strcmp(path, "/api/workmgmt/msgqueues")) {
            blen = api_workmgmt_msgqueues_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/wal")) {
            blen = api_wal_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tiers")) {
            blen = api_tiers_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Navigator-Parity Gap Roadmap Phase 5a: network status ─────────────
        if (!strcmp(path, "/api/network/status")) {
            blen = api_network_status_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Navigator-Parity Gap Roadmap Phase 5b: disk/storage status ────────
        if (!strcmp(path, "/api/disk")) {
            blen = api_disk_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/processes")) {
            blen = api_processes_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/programs")) {
            blen = api_programs_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/streams")) {
            blen = stream_list_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/stream/") == path) {
            const char* sname = path + 12; /* skip "/api/stream/" */
            struct StreamEntry* se = stream_find(sname);
            if (!se) {
                const char* e = "{\"error\":\"stream not found\"}";
                http_respond(conn, 404, "application/json", e, (int)strlen(e));
            } else {
                http_respond_stream(conn, se);
            }
            return;
        }
        if (str_find(path, "/api/program/") == path) {
            const char* pname = path + 13; /* skip "/api/program/" */
            struct ServiceBinary* sb = 0;
            for (int b = 0; b < MAX_SERVICE_BINARIES; b++) {
                if (service_binaries[b].active &&
                    !str_ncmp2(service_binaries[b].object_name, pname,
                               (int)strlen(pname) + 1)) {
                    sb = &service_binaries[b]; break;
                }
            }
            if (!sb) {
                const char* e = "{\"error\":\"program not found\"}";
                http_respond(conn, 404, "application/json", e, (int)strlen(e));
            } else {
                http_respond_program_binary(conn, sb);
            }
            return;
        }
        if (!strcmp(path, "/api/query")) {
            char q[256] = "show all";
            url_param(qs, "q", q, (int)sizeof(q));
            blen = api_query_json(q, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/constraints[?table=<name>] ───────────────────────────────
        if (!strcmp(path, "/api/constraints")) {
            char tbl[OBJECT_NAME_LEN];
            tbl[0] = '\0';
            url_param(qs, "table", tbl, (int)sizeof(tbl));
            blen = constraints_to_json(tbl, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/cursors — list open cursors ──────────────────────────────
        if (!strcmp(path, "/api/cursors")) {
            blen = cursors_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/cursor/fetch?id=N&n=M ────────────────────────────────────
        if (!strcmp(path, "/api/cursor/fetch")) {
            char sid[16], sn[8];
            sid[0] = sn[0] = '\0';
            url_param(qs, "id", sid, (int)sizeof(sid));
            url_param(qs, "n",  sn,  (int)sizeof(sn));
            uint32_t cid = 0; const char* sp = sid;
            while (*sp >= '0' && *sp <= '9') { cid = cid * 10 + (uint32_t)(*sp - '0'); sp++; }
            uint32_t nrows = 10; sp = sn;
            if (*sp) { nrows = 0; while (*sp >= '0' && *sp <= '9') { nrows = nrows * 10 + (uint32_t)(*sp - '0'); sp++; } }
            if (!nrows) nrows = 10;
            blen = cursor_fetch(cid, nrows, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/cursor/close?id=N ────────────────────────────────────────
        if (!strcmp(path, "/api/cursor/close")) {
            char sid[16]; sid[0] = '\0';
            url_param(qs, "id", sid, (int)sizeof(sid));
            uint32_t cid = 0; const char* sp = sid;
            while (*sp >= '0' && *sp <= '9') { cid = cid * 10 + (uint32_t)(*sp - '0'); sp++; }
            cursor_close(cid);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", "true");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/locks")) {
            blen = lock_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/mqts — list all MQTs ────────────────────────────────────
        if (!strcmp(path, "/api/mqts")) {
            blen = mqts_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/mqt/<name> — read current MQT result table ──────────────
        if (str_find(path, "/api/mqt/") == path) {
            const char* mname = path + 9;
            blen = api_object_detail(mname, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/indexes — list all indexes ───────────────────────────────
        if (!strcmp(path, "/api/indexes")) {
            blen = indexes_to_json(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/index/<name>[?q=<value>] ─────────────────────────────────
        if (str_find(path, "/api/index/") == path) {
            const char* iname = path + 11;
            char q_val[64];
            q_val[0] = '\0';
            url_param(qs, "q", q_val, (int)sizeof(q_val));
            if (q_val[0]) {
                // Exact lookup — return matching record key
                char rec_key[64];
                rec_key[0] = '\0';
                int hit = index_lookup(iname, q_val, rec_key);
                JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
                jb_obj_open(&j, 0);
                jb_str(&j, "hit", hit ? "true" : "false");
                if (hit) { jb_putc(&j, ','); jb_str(&j, "key", rec_key); }
                jb_obj_close(&j); j.buf[j.pos] = '\0';
                http_respond(conn, 200, "application/json", resp_body, j.pos); return;
            }
            blen = index_to_json(iname, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── GET /api/journal/<name>[?since=N] ─────────────────────────────────
        if (str_find(path, "/api/journal/") == path) {
            const char* jname = path + 13;
            uint64_t since = 0;
            char since_s[32] = "0";
            url_param(qs, "since", since_s, (int)sizeof(since_s));
            const char* sp = since_s;
            while (*sp >= '0' && *sp <= '9') { since = since * 10 + (uint64_t)(*sp - '0'); sp++; }
            blen = journal_to_json(jname, since, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/journals")) {
            // List all active journal attachments
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_putc(&j, '[');
            int first = 1;
            for (uint32_t i = 0; i < journal_attachment_count; i++) {
                if (!journal_attachments[i].active) continue;
                if (!first) jb_putc(&j, ',');
                first = 0;
                jb_obj_open(&j, 0);
                jb_str(&j, "journal", journal_attachments[i].journal_name); jb_putc(&j, ',');
                jb_str(&j, "table",   journal_attachments[i].object_name);
                jb_obj_close(&j);
            }
            jb_putc(&j, ']');
            j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── Phase H: Agent & Workflow GET routes ─────────────────────────────
        if (!strcmp(path, "/api/agents")) {
            blen = api_agents_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/agent/") == path) {
            blen = api_agent_status(path + 11, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/workflows")) {
            blen = api_workflows_list(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (str_find(path, "/api/workflow/") == path) {
            blen = api_workflow_status(path + 14, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // Compiled-in bundle (full Navigator SPA) — checked first
        const struct BundleAsset* ba = bundle_find(path);
        if (ba) {
            http_respond_raw(conn, ba->mime, ba->data, ba->len); return;
        }
        // WEB_APP asset lookup (dynamic assets stored via syscall)
        struct WebAsset* asset = webapp_find(path);
        if (asset) {
            http_respond(conn, 200, asset->mime,
                         asset->content, (int)asset->content_len); return;
        }
    }

    // ── POST routes ───────────────────────────────────────────────────────────
    if (is_post) {        // POST /auth/token — public, no auth required
        if (!strcmp(path, "/auth/token")) {
            blen = api_auth_token(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }

        // All other POST routes require at least APP_USER or DB_ADMIN
        uint32_t req_uid = 0; SLSRole req_role = ROLE_GUEST;
        auth_http_extract(req, &req_uid, &req_role);
        if (req_role == ROLE_GUEST) {
            const char* e401 = "{\"error\":\"Unauthorized — include Authorization: Bearer <token>\"}";
            http_respond(conn, 401, "application/json", e401,
                         (int)strlen(e401));
            return;
        }
        if (!http_partition_rate_check(req_uid)) {
            const char* e429 = "{\"error\":\"Rate limit exceeded for this partition — try again shortly\"}";
            http_respond(conn, 429, "application/json", e429, (int)strlen(e429));
            return;
        }
        usage_metering_record_request(req_uid);   // §5 item 6 / §7 item 6 -- see the GET-route site above for the full comment
        if (!strcmp(path, "/api/valloc")) {
            blen = api_valloc_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase H ─────────────────────────────────────────────
        if (!strcmp(path, "/api/schema")) {
            blen = api_schema_set_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase B ────────────────────────────────────────────
        if (!strcmp(path, "/api/tables")) {
            blen = api_table_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/sql")) {
            blen = api_sql_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── SQL Feature-Parity Roadmap, Phase 8 follow-on ──────────────────────
        if (!strcmp(path, "/api/schema/import")) {
            blen = api_schema_import_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/schema/import")) {
            blen = api_vec_schema_import_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/data/import")) {
            blen = api_vec_data_import_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Kernel-Side Shell Refactor ──────────────────────────────────────
        if (!strcmp(path, "/api/shell/exec")) {
            blen = api_shell_exec_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── QEMU-SLS: the guest runtime, reachable as JSON rather than as
        //    console text. See api_qemu_bench_post() for why there is no
        //    /api/qemu/run to go with these. ────────────────────────────────
        if (!strcmp(path, "/api/qemu/bench")) {
            blen = api_qemu_bench_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/bench_sweep")) {
            blen = api_qemu_bench_sweep_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/paging")) {
            blen = api_qemu_paging_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/invl")) {
            blen = api_qemu_invl_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/selfmod")) {
            blen = api_qemu_selfmod_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/tls")) {
            blen = api_qemu_tls_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/brkmmap")) {
            blen = api_qemu_brkmmap_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/rdclock")) {
            blen = api_qemu_rdclock_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/faults")) {
            blen = api_qemu_faults_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/sse2")) {
            blen = api_qemu_sse2_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/futex")) {
            blen = api_qemu_futex_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/compiled")) {
            blen = api_qemu_compiled_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/elf")) {
            blen = api_qemu_elf_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/qemu/elf-reject")) {
            blen = api_qemu_elf_reject_post(resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // Destructive. Requires {"confirm":"reboot"}; see the handler.
        if (!strcmp(path, "/api/node/reboot")) {
            blen = api_node_reboot_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase C: Vector Store HTTP reachability ────────────
        if (!strcmp(path, "/api/vec/collections")) {
            blen = api_vec_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── VectorStore Gap Analysis §1.3 ───────────────────────────────────────
        if (!strcmp(path, "/api/vec/collections/unique")) {
            blen = api_vec_set_unique_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── VectorStore Gap Analysis §3 ─────────────────────────────────────────
        if (!strcmp(path, "/api/objects/database")) {
            blen = api_object_set_database_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/insert")) {
            blen = api_vec_insert_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/embed-insert")) {
            blen = api_vec_embed_insert_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/search")) {
            blen = api_vec_search_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/indexes")) {
            blen = api_vec_index_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/index/search")) {
            blen = api_vec_index_search_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── VectorStore Interface Roadmap Phase 2: semantic (embed-then-search) ──
        if (!strcmp(path, "/api/vec/embed-search")) {
            blen = api_vec_embed_search_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/index/embed-search")) {
            blen = api_vec_index_embed_search_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── VectorStore Interface Roadmap Phase 3: rebuild/backfill ──────────────
        if (!strcmp(path, "/api/vec/index/rebuild")) {
            blen = api_vec_index_rebuild_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/join")) {
            blen = api_vec_join_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/record")) {
            blen = api_record_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/journal/attach|detach ───────────────────────────────────
        if (!strcmp(path, "/api/journal/attach") ||
            !strcmp(path, "/api/journal/detach")) {
            char jname[32];
            char tname[64];
            jname[0] = '\0';
            tname[0] = '\0';
            json_str(body_ptr, "journal", jname, (int)sizeof(jname));
            json_str(body_ptr, "table",   tname, (int)sizeof(tname));
            int rc = (!strcmp(path, "/api/journal/attach"))
                   ? journal_attach(jname, tname)
                   : journal_detach(jname, tname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_str(&j, "ok", rc == 0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/tx/begin")) {
            blen = api_tx_post("begin", resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/index/create|drop|rebuild ───────────────────────────────
        if (!strcmp(path, "/api/index/create")) {
            char iname[OBJECT_NAME_LEN], tname[OBJECT_NAME_LEN], fname[RECORD_KEY_LEN];
            iname[0] = tname[0] = fname[0] = '\0';
            json_str(body_ptr, "name",  iname, (int)sizeof(iname));
            json_str(body_ptr, "table", tname, (int)sizeof(tname));
            json_str(body_ptr, "field", fname, (int)sizeof(fname));
            int rc = index_create(iname, tname, fname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", rc == 0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/index/drop")) {
            char iname[OBJECT_NAME_LEN]; iname[0] = '\0';
            json_str(body_ptr, "name", iname, (int)sizeof(iname));
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_str(&j, "ok", index_drop(iname) == 0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/index/rebuild")) {
            char iname[OBJECT_NAME_LEN]; iname[0] = '\0';
            json_str(body_ptr, "name", iname, (int)sizeof(iname));
            index_rebuild(iname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", "true");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/constraint/add|remove ───────────────────────────────────
        if (!strcmp(path, "/api/constraint/add")) {
            char tbl[OBJECT_NAME_LEN], fld[RECORD_KEY_LEN], typ[16], ref[OBJECT_NAME_LEN];
            tbl[0] = fld[0] = typ[0] = ref[0] = '\0';
            json_str(body_ptr, "table", tbl, (int)sizeof(tbl));
            json_str(body_ptr, "field", fld, (int)sizeof(fld));
            json_str(body_ptr, "type",  typ, (int)sizeof(typ));
            json_str(body_ptr, "ref",   ref, (int)sizeof(ref));
            int rc = 1;
            if (!strcmp(typ, "UNIQUE"))    rc = constraint_add_unique(tbl, fld);
            else if (!strcmp(typ, "NOT_NULL"))  rc = constraint_add_not_null(tbl, fld);
            else if (!strcmp(typ, "REFERENCE")) rc = constraint_add_reference(tbl, fld, ref);
            else if (!strcmp(typ, "RANGE")) {
                char smin[24], smax[24]; smin[0] = smax[0] = '\0';
                json_str(body_ptr, "min", smin, (int)sizeof(smin));
                json_str(body_ptr, "max", smax, (int)sizeof(smax));
                // Parse min/max with simple atoi
                int64_t mn = 0, mx = 0;
                const char* p = smin; if (*p=='-'){mn=-1;p++;} int neg=mn<0; mn=0;
                while(*p>='0'&&*p<='9'){mn=mn*10+(*p-'0');p++;} if(neg) mn=-mn;
                p = smax; neg=0; if (*p=='-'){neg=1;p++;} mx=0;
                while(*p>='0'&&*p<='9'){mx=mx*10+(*p-'0');p++;} if(neg) mx=-mx;
                rc = constraint_add_range(tbl, fld, mn, mx);
            }
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", rc==0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/constraint/remove")) {
            char tbl[OBJECT_NAME_LEN], fld[RECORD_KEY_LEN], typ[16];
            tbl[0] = fld[0] = typ[0] = '\0';
            json_str(body_ptr, "table", tbl, (int)sizeof(tbl));
            json_str(body_ptr, "field", fld, (int)sizeof(fld));
            json_str(body_ptr, "type",  typ, (int)sizeof(typ));
            int t = -1;
            if      (!strcmp(typ, "UNIQUE"))    t = 0;
            else if (!strcmp(typ, "NOT_NULL"))  t = 1;
            else if (!strcmp(typ, "RANGE"))     t = 2;
            else if (!strcmp(typ, "REFERENCE")) t = 3;
            int rc = constraint_remove(tbl, fld, t);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0); jb_str(&j, "ok", rc==0 ? "true" : "false");
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/mqt/create|drop|refresh ────────────────────────────────
        if (!strcmp(path, "/api/mqt/create")) {
            char mname[OBJECT_NAME_LEN], btable[OBJECT_NAME_LEN];
            char fn_s[8], afld[RECORD_KEY_LEN], wfld[RECORD_KEY_LEN];
            char weq[RECORD_VAL_LEN], gfld[RECORD_KEY_LEN];
            mname[0]=btable[0]=fn_s[0]=afld[0]=wfld[0]=weq[0]=gfld[0]='\0';
            json_str(body_ptr, "name",     mname,  OBJECT_NAME_LEN);
            json_str(body_ptr, "table",    btable, OBJECT_NAME_LEN);
            json_str(body_ptr, "fn",       fn_s,   (int)sizeof(fn_s));
            json_str(body_ptr, "field",    afld,   RECORD_KEY_LEN);
            json_str(body_ptr, "where",    wfld,   RECORD_KEY_LEN);
            json_str(body_ptr, "eq",       weq,    RECORD_VAL_LEN);
            json_str(body_ptr, "group_by", gfld,   RECORD_KEY_LEN);
            uint8_t fn = (uint8_t)AGG_COUNT;
            if      (!strcmp(fn_s,"SUM")) fn=(uint8_t)AGG_SUM;
            else if (!strcmp(fn_s,"AVG")) fn=(uint8_t)AGG_AVG;
            else if (!strcmp(fn_s,"MIN")) fn=(uint8_t)AGG_MIN;
            else if (!strcmp(fn_s,"MAX")) fn=(uint8_t)AGG_MAX;
            int rc = mqt_create(mname, btable, fn, afld, wfld, weq, gfld);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",rc==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/mqt/drop")) {
            char mname[OBJECT_NAME_LEN]; mname[0]='\0';
            json_str(body_ptr, "name", mname, OBJECT_NAME_LEN);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",mqt_drop(mname)==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/mqt/refresh")) {
            char mname[OBJECT_NAME_LEN]; mname[0]='\0';
            json_str(body_ptr, "name", mname, OBJECT_NAME_LEN);
            mqt_refresh(mname);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok","true");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/aggregate ────────────────────────────────────────────────        // Body: {"table":"...","fn":"COUNT|SUM|AVG|MIN|MAX",
        //        "field":"...","where":"...","eq":"...",
        //        "group_by":"...","having":N,"order_by":"...","order":"ASC|DESC"}
        // fn="" or omitted → AGG_NONE (ORDER BY only)
        if (!strcmp(path, "/api/aggregate")) {
            struct AggQuery q;
            char fn_s[8];
            q.table[0] = q.agg_field[0] = q.where_field[0] = q.where_eq[0] = '\0';
            q.group_field[0] = q.order_field[0] = fn_s[0] = '\0';
            q.having_min_count = 0; q.order_desc = 0;
            json_str(body_ptr, "table",    q.table,       OBJECT_NAME_LEN);
            json_str(body_ptr, "fn",       fn_s,          (int)sizeof(fn_s));
            json_str(body_ptr, "field",    q.agg_field,   RECORD_KEY_LEN);
            json_str(body_ptr, "where",    q.where_field, RECORD_KEY_LEN);
            json_str(body_ptr, "eq",       q.where_eq,    RECORD_VAL_LEN);
            json_str(body_ptr, "group_by", q.group_field, RECORD_KEY_LEN);
            json_str(body_ptr, "order_by", q.order_field, RECORD_KEY_LEN);
            char ord_dir[8]; ord_dir[0] = '\0';
            json_str(body_ptr, "order",    ord_dir,       (int)sizeof(ord_dir));
            if (ord_dir[0] == 'D' || ord_dir[0] == 'd') q.order_desc = 1;
            int hav = json_int(body_ptr, "having");
            q.having_min_count = (int64_t)(hav > 0 ? hav : 0);
            // Map fn string to enum
            if      (!strcmp(fn_s,"COUNT")) q.fn = (uint8_t)AGG_COUNT;
            else if (!strcmp(fn_s,"SUM"))   q.fn = (uint8_t)AGG_SUM;
            else if (!strcmp(fn_s,"AVG"))   q.fn = (uint8_t)AGG_AVG;
            else if (!strcmp(fn_s,"MIN"))   q.fn = (uint8_t)AGG_MIN;
            else if (!strcmp(fn_s,"MAX"))   q.fn = (uint8_t)AGG_MAX;
            else                            q.fn = (uint8_t)AGG_NONE;
            blen = aggregate_exec(&q, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/cursor/open ─────────────────────────────────────────────
        if (!strcmp(path, "/api/cursor/open")) {
            char tbl[OBJECT_NAME_LEN], wfld[RECORD_KEY_LEN];
            char wval[RECORD_VAL_LEN], oidx[OBJECT_NAME_LEN];
            tbl[0] = wfld[0] = wval[0] = oidx[0] = '\0';
            json_str(body_ptr, "table",  tbl,  (int)sizeof(tbl));
            json_str(body_ptr, "where",  wfld, (int)sizeof(wfld));
            json_str(body_ptr, "eq",     wval, (int)sizeof(wval));
            json_str(body_ptr, "order",  oidx, (int)sizeof(oidx));
            uint32_t cid = cursor_open(tbl, wfld, wval, oidx);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_str(&j, "ok", cid > 0 ? "true" : "false"); jb_putc(&j, ',');
            jb_uint(&j, "cursor_id", cid);
            jb_obj_close(&j); j.buf[j.pos] = '\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        // ── POST /api/program/create|upload|spawn ───────────────────────────────────
        if (!strcmp(path, "/api/program/create")) {
            blen = api_program_create(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/program/upload")) {
            blen = api_program_upload(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/program/spawn")) {
            blen = api_program_spawn_handler(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Gap Remediation Phase F: partition create/assign/destroy/pause/
        // resume/quota — every one of these syscalls was already correctly
        // wired at the dispatch layer; this is the first HTTP surface for any
        // of them. ────────────────────────────────────────────────────────────
        if (!strcmp(path, "/api/partitions")) {
            blen = api_partition_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/assign")) {
            blen = api_partition_assign_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/destroy")) {
            blen = api_partition_destroy_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/pause")) {
            blen = api_partition_pause_post(body_ptr, resp_body, (int)sizeof(resp_body), 0);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/resume")) {
            blen = api_partition_pause_post(body_ptr, resp_body, (int)sizeof(resp_body), 1);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/quota")) {
            blen = api_partition_quota_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/cpuweight")) {
            blen = api_partition_cpuweight_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/storagequota")) {
            blen = api_partition_storagequota_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/partition/connquota")) {
            blen = api_partition_connquota_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Cluster formation: POST /api/cluster/init, /api/cluster/peer ───────
        if (!strcmp(path, "/api/cluster/init")) {
            blen = api_cluster_init_post(body_ptr, resp_body, (int)sizeof(resp_body), req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/cluster/peer")) {
            blen = api_cluster_peer_post(body_ptr, resp_body, (int)sizeof(resp_body), req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Orchestration Plan Phase 5: POST /api/workload, /api/reconcile ─────
        if (!strcmp(path, "/api/workload")) {
            blen = api_workload_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid, req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/reconcile")) {
            blen = api_reconcile_post(body_ptr, resp_body, (int)sizeof(resp_body), req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Orchestration Plan Phase 4: POST /api/service ──────────────────────
        if (!strcmp(path, "/api/service")) {
            blen = api_service_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid, req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Multitenant Isolation Gap Analysis §5 item 1 / §7 item 2:
        // POST /api/tenants — unified tenant_create(). ─────────────────────────
        if (!strcmp(path, "/api/tenants")) {
            blen = api_tenant_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid, req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── POST /api/stream/create|upload ──────────────────────────────────────
        if (!strcmp(path, "/api/stream/create")) {
            blen = api_stream_create(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/stream/upload")) {
            blen = api_stream_upload(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tx/commit")) {
            blen = api_tx_post("commit", resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tx/rollback")) {
            blen = api_tx_post("rollback", resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // ── Phase H: Agent & Workflow POST routes ─────────────────────────────
        if (!strcmp(path, "/api/agent/create")) {
            blen = api_agent_create(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/agent/run")) {
            blen = api_agent_run(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/agent/drop")) {
            blen = api_agent_drop(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/agent/schedule")) {
            char aname[OBJECT_NAME_LEN]; aname[0]='\0';
            char amsg[128];             amsg[0]='\0';
            json_str(body_ptr, "name",    aname, OBJECT_NAME_LEN);
            json_str(body_ptr, "message", amsg,  sizeof(amsg));
            int aticks = json_int(body_ptr, "ticks");
            struct AgentScheduleRequest sr;
            for(int i=0;aname[i]&&i<(int)(sizeof(sr.name)-1);i++) sr.name[i]=aname[i]; sr.name[sizeof(sr.name)-1]='\0';
            for(int i=0;amsg[i] &&i<(int)(sizeof(sr.message)-1);i++) sr.message[i]=amsg[i]; sr.message[sizeof(sr.message)-1]='\0';
            sr.ticks = (uint32_t)(aticks > 0 ? aticks : 0);
            uint64_t rc = sys_sls_agent_schedule(&sr);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",rc==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/agent/unschedule")) {
            char aname[OBJECT_NAME_LEN]; aname[0]='\0';
            json_str(body_ptr, "name", aname, OBJECT_NAME_LEN);
            struct AgentScheduleRequest sr;
            for(int i=0;aname[i]&&i<(int)(sizeof(sr.name)-1);i++) sr.name[i]=aname[i]; sr.name[sizeof(sr.name)-1]='\0';
            sr.message[0]='\0'; sr.ticks=0;
            uint64_t rc = sys_sls_agent_schedule(&sr);
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j,0); jb_str(&j,"ok",rc==0?"true":"false");
            jb_obj_close(&j); j.buf[j.pos]='\0';
            http_respond(conn, 200, "application/json", resp_body, j.pos); return;
        }
        if (!strcmp(path, "/api/workflow/create")) {
            blen = api_workflow_create(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/workflow/run")) {
            blen = api_workflow_run(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }

        // ── Shell-Command JSON-Promotion Roadmap: new POST routes ──────────────
        // Group 1: security / session
        if (!strcmp(path, "/api/role/set")) {
            blen = api_role_set_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/grant")) {
            blen = api_grant_post(body_ptr, resp_body, (int)sizeof(resp_body), 1);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/revoke")) {
            blen = api_grant_post(body_ptr, resp_body, (int)sizeof(resp_body), 0);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/chmod")) {
            blen = api_chmod_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/auth/create")) {
            blen = api_auth_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/auth/revoke")) {
            blen = api_auth_revoke_post(body_ptr, resp_body, (int)sizeof(resp_body), req_role);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/seal")) {
            blen = api_seal_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // Group 2: process / service / IPC
        if (!strcmp(path, "/api/svc/crash")) {
            blen = api_svc_crash_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/svc/restart")) {
            blen = api_svc_restart_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/proc/kill")) {
            blen = api_proc_kill_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/ipc/post")) {
            blen = api_ipc_post_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/checkpoint")) {
            int rc = checkpoint_trigger();
            JSONBuf j = { resp_body, 0, (int)sizeof(resp_body) };
            jb_obj_open(&j, 0);
            jb_uint(&j, "status", (uint64_t)rc);              jb_putc(&j, ',');
            jb_uint(&j, "seq",    checkpoint_last_sequence()); jb_putc(&j, ',');
            jb_uint(&j, "count",  checkpoint_count());
            jb_obj_close(&j);
            j.buf[j.pos] = '\0';
            http_respond(conn, rc == 0 ? 200 : 503, "application/json", resp_body, j.pos); return;
        }
        // Group 3: journal / tier / object
        if (!strcmp(path, "/api/journal")) {
            blen = api_journal_create_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/journal/purge")) {
            blen = api_journal_purge_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tier/promote")) {
            blen = api_tier_promote_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/tier/demote")) {
            blen = api_tier_demote_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vfree")) {
            blen = api_vfree_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // Group 4: webapp / workflow
        if (!strcmp(path, "/api/webapp/set")) {
            blen = api_webapp_set_post(body_ptr, resp_body, (int)sizeof(resp_body), 0);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/webapp/append")) {
            blen = api_webapp_set_post(body_ptr, resp_body, (int)sizeof(resp_body), 1);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/workflow/addstep")) {
            blen = api_workflow_addstep_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        // Group 5: legacy loader
        if (!strcmp(path, "/api/write")) {
            blen = api_write_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/demo")) {
            blen = api_demo_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/load")) {
            blen = api_load_post(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/upload")) {
            blen = api_upload_post(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
    }

    // ── DELETE routes — VectorStore Interface Roadmap Phase 1 ──────────────────
    if (is_delete) {
        // Same "must be authenticated" gate every POST route above already
        // has -- no route in this branch is meant to be reachable by a
        // guest, matching this whole file's established posture that only
        // /api/health, /auth/token, and /auth/verify stay public.
        uint32_t req_uid = 0; SLSRole req_role = ROLE_GUEST;
        auth_http_extract(req, &req_uid, &req_role);
        if (req_role == ROLE_GUEST) {
            const char* e401 = "{\"error\":\"Unauthorized — include Authorization: Bearer <token>\"}";
            http_respond(conn, 401, "application/json", e401, (int)strlen(e401));
            return;
        }
        if (!http_partition_rate_check(req_uid)) {
            const char* e429 = "{\"error\":\"Rate limit exceeded for this partition — try again shortly\"}";
            http_respond(conn, 429, "application/json", e429, (int)strlen(e429));
            return;
        }
        usage_metering_record_request(req_uid);   // §5 item 6 / §7 item 6 -- see the GET-route site above for the full comment
        if (!strcmp(path, "/api/vec/vector")) {
            blen = api_vec_delete(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/collections")) {
            blen = api_vec_collection_delete(body_ptr, resp_body, (int)sizeof(resp_body));
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
        if (!strcmp(path, "/api/vec/indexes")) {
            blen = api_vec_index_delete(body_ptr, resp_body, (int)sizeof(resp_body), req_uid);
            http_respond(conn, 200, "application/json", resp_body, blen); return;
        }
    }

    // 404 fallback
    const char* e404 = "{\"error\":\"not found\"}";
    http_respond(conn, 404, "application/json", e404, (int)strlen(e404));
}

// ─── HTTP server main loop ────────────────────────────────────────────────────
//
// Architectural Phase 1 (docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md).
//
// This used to be a single blocking loop: tcp_accept() ONE connection, then
// tcp_recv() that same connection until its request was complete, dispatch,
// close, repeat. Any client that opened a connection and stalled -- slow
// upload, or just never sending anything -- froze every other client
// indefinitely, because there was only one execution path through this
// function and it was parked inside that one connection's tcp_recv().
//
// tcp_conns[] (net/tcp.c) already tracks up to TCP_MAX_CONNS connections
// independently -- packets for every connection get demuxed into the right
// slot's receive ring buffer by tcp_handle_segment(), which runs off the
// timer IRQ via net_poll_tick(), completely independent of what this loop
// happens to be doing. So the fix is entirely in this loop: poll every
// connection each sweep instead of committing to one at a time. No change
// needed in tcp.c.
//
// Request dispatch (http_route()) still runs one request fully to
// completion before moving to the next connection's request -- this fixes
// I/O concurrency (many clients can be mid-request at once without blocking
// each other), not kernel-state concurrency (two requests never execute
// interleaved). That distinction is deliberate: it's what lets this ship
// without touching lock_mgr.c or any of the ~40 kernel subsystems for
// thread-safety. Making per-request identity/transaction state safe under
// this concurrency is Phase 2 (session/tx globals in user/shell.c), which
// must land alongside or immediately after this.

#define HTTP_REQ_BUF_SZ         65536  // unchanged cap from the old single-buffer version
// ~10s at the timer's documented ~100 Hz (kernel/auth.c's AUTH_TOKEN_TTL_TICKS
// comment establishes the same tick-rate reasoning) -- long enough for a real
// slow client, short enough that an abandoned half-open connection doesn't
// tie up one of only TCP_MAX_CONNS (512) slots indefinitely. That's a new
// failure mode this loop introduces (the old loop had no notion of "wasted
// slot" since it only ever tracked one connection at a time) and needs a
// guard now that many connections can be tracked at once.
#define HTTP_IDLE_TIMEOUT_TICKS  1000

struct HttpConnState {
    uint8_t  in_use;   // 1 while we're accumulating this connection's request
    /* 1 when this connection arrived on the TLS listener. Set at pickup from
     * the local port it landed on, never inferred from the bytes: a client
     * that speaks plaintext to the TLS port must fail the handshake, not be
     * quietly served in the clear. */
    uint8_t  tls;
    // Network Fairness Phase 2: 1 once this connection has been attributed
    // to a partition's connection quota (net/tcp_quota.h) -- set the first
    // sweep an Authorization header is found in the accumulating buffer,
    // well before the request necessarily finishes (a slow body upload
    // must not be able to hide from quota accounting). Reset to 0 whenever
    // this slot is reclaimed for a new connection, alongside in_use.
    uint8_t  attributed;
    int      len;
    uint64_t last_activity_tick;
    char     buf[HTTP_REQ_BUF_SZ];
};
// Indexed directly by tcp_conns[] slot index (conn_id) -- avoids any separate
// allocation bookkeeping, since conn_id is already the stable identity tcp.c
// hands out for the lifetime of a connection.
static struct HttpConnState http_conns[TCP_MAX_CONNS];

/* See http_send(). Bounds are checked by the caller; this is deliberately not
 * defensive twice, so there is one place the range rule lives. */
static int http_conn_is_tls(int conn) { return http_conns[conn].tls != 0; }

// Returns 1 once buf[0..len) holds a complete HTTP request (full headers,
// and full body per Content-Length if one is present), or once `len` has
// reached `cap - 1` and we have to dispatch whatever we've got rather than
// overflow. Extracted, unchanged in behavior, from the old single-connection
// accumulation loop so every tracked connection can run the same check.
static int http_request_ready(const char* buf, int len, int cap) {
    const char* body_start = str_find(buf, "\r\n\r\n");
    if (!body_start) return len >= cap - 1;
    const char* cl = str_find(buf, "Content-Length:");
    if (!cl) cl = str_find(buf, "content-length:");
    if (!cl) return 1;  // headers complete, no body expected
    int clen = 0;
    const char* cp = cl + 15;
    while (*cp == ' ') cp++;
    while (*cp >= '0' && *cp <= '9') { clen = clen * 10 + (*cp++ - '0'); }
    int hdr_end = (int)(body_start - buf) + 4;
    if (len >= hdr_end + clen) return 1;
    return len >= cap - 1;
}

void http_server_run(void) {
    tcp_init();
    tcp_quota_init();   // Network Fairness Phase 2 -- must run before any connection is accepted, see tcp_quota.h
    /* Announce the console immediately. An operator attaching to a node's
     * serial port should see a prompt, not silence that looks like a hang
     * until they guess that typing does something. */
    sls_console_banner();
    sls_console_prompt();
    int listen_fd = tcp_listen(NET_HTTP_PORT);
    if (listen_fd < 0) {
        kernel_serial_print("[HTTP] Failed to bind port 3000.\n");
        return;
    }
    kernel_serial_printf("[HTTP] Listening on port %u. "
                         "GET http://10.0.2.15:3000/api/scan\n",
                         NET_HTTP_PORT);

    /* ── The TLS listener ──────────────────────────────────────────────
     * A SEPARATE PORT, not an upgrade of 3000. The design doc says "on the
     * HTTP port" and this deliberately does not do that yet, because during
     * bring-up the plaintext path is the instrument: it is how the
     * certificate DER gets off the node for `openssl x509` to read, and how
     * the node stays reachable when the handshake is the thing that is
     * broken. Moving TLS onto 3000 is a one-line change once a browser has
     * completed a handshake; doing it first would mean debugging the record
     * layer with no way in.
     *
     * TLS init failing is NOT fatal to HTTP. It is fatal to TLS: no listener
     * is opened, and nothing falls back to serving 8443 in the clear. */
    int tls_listen_fd = -1;
    uint16_t tls_port = 0;
    {
        /* Every name a client might actually use, because a verifier checks
         * the name the CLIENT typed, not the one this node thinks it has.
         * 10.0.2.15 is slirp's guest address -- correct from inside QEMU and
         * useless from outside it. Reaching a node from the host goes through
         * run-cluster.sh's port forward, so the name on the wire is localhost
         * or 127.0.0.1, and a certificate without those fails every verifier
         * that actually checks. The first live handshake had only the guest
         * address and passed solely because curl -k skips verification. */
        static const struct tls_cert_san node_sans[] = {
            { "localhost",  { 0, 0, 0, 0 } },
            { 0,            { 127, 0, 0, 1 } },
            { 0,            { 10, 0, 2, 15 } },
        };
        if (tls_server_init("CN=AeroSLS node CA,O=AeroSLS",
                            "CN=AeroSLS node,O=AeroSLS", node_sans,
                            sizeof node_sans / sizeof node_sans[0]) == TLS_SRV_OK) {
            tls_listen_fd = tcp_listen(NET_HTTPS_PORT);
            if (tls_listen_fd < 0) {
                kernel_serial_print("[TLS] could not bind the TLS port.\n");
            } else {
                tls_port = tcp_conns[tls_listen_fd].local_port;
                kernel_serial_printf("[TLS] Listening on port %u. "
                                     "https://10.0.2.15:%u/\n",
                                     NET_HTTPS_PORT, NET_HTTPS_PORT);
            }
        } else {
            kernel_serial_print("[TLS] not started -- serving plaintext only.\n");
        }
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) { http_conns[i].in_use = 0; http_conns[i].attributed = 0; http_conns[i].tls = 0; }

    for (;;) {
        int did_work = 0;

        /* Leaf renewal. Cheap to call every sweep -- it compares two integers
         * and returns -- and does real work at most once an hour. Placed here,
         * BEFORE connections are picked up, so the zero-live-sessions window it
         * needs is the one that occurs naturally between sweeps rather than one
         * it has to wait for. Nothing re-issued the leaf while a node ran, so
         * its lifetime was a bet that no node runs longer than a year; this is
         * what settles the bet. */
        tls_server_maybe_renew();

        // Opportunistically pick up any newly-ESTABLISHED connection we
        // aren't already tracking -- the same scan tcp_accept() itself did,
        // just one non-blocking pass instead of a spin/hlt-wait loop.
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            if (i == listen_fd) continue;
            struct TCPConn* c = &tcp_conns[i];
            if (i == tls_listen_fd) continue;
            int on_plain = (c->local_port == tcp_conns[listen_fd].local_port);
            int on_tls   = (tls_listen_fd >= 0 && c->local_port == tls_port);
            if (c->active && (on_plain || on_tls) &&
                c->state == TCP_ESTABLISHED && !http_conns[i].in_use) {
                http_conns[i].in_use = 1;
                http_conns[i].attributed = 0;   // Network Fairness Phase 2 -- fresh occupant of this slot, not yet quota-attributed
                http_conns[i].len   = 0;
                http_conns[i].tls   = on_tls ? 1 : 0;
                http_conns[i].last_activity_tick = kernel_tick_counter;
                did_work = 1;

                /* Claim a session slot now, at pickup, so the refusal happens
                 * before any client bytes are read. At the cap the connection
                 * is closed -- never served plaintext, which would be a
                 * downgrade this client has no way to detect. */
                if (on_tls && tls_server_open(i) != TLS_SRV_OK) {
                    kernel_serial_printf("[TLS] conn %d refused: no session slot "
                                         "(cap is %d).\n", i, TLS_SERVER_MAX_SESSIONS);
                    tcp_close(i);
                    http_conns[i].in_use = 0;
                    http_conns[i].tls = 0;
                    /* Currently a no-op -- attributed was set to 0 at pickup a
                     * few lines above and release is documented safe in that
                     * state. Called anyway, because this was the ONE teardown
                     * of five that did not, and an asymmetry that is harmless
                     * only because of a fact established elsewhere is a bug
                     * waiting for that fact to change. */
                    tcp_conn_release(i);
                    http_conns[i].attributed = 0;
                }
            }
        }

        // Advance every connection currently accumulating a request.
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            if (!http_conns[i].in_use) continue;
            struct HttpConnState* hc = &http_conns[i];
            struct TCPConn* c = &tcp_conns[i];

            /* A TLS connection reads nothing as HTTP until the handshake is
             * done. TLS_SRV_HANDSHAKING is the normal case for several
             * sweeps -- mbedTLS asked for bytes that have not arrived -- and
             * yielding here rather than waiting is what stops one client
             * stalling the console this loop also drives. */
            if (hc->tls) {
                int hs = tls_server_handshake(i);
                if (hs == TLS_SRV_HANDSHAKING) {
                    if (c->rbuf_used > 0) { hc->last_activity_tick = kernel_tick_counter; did_work = 1; }
                    if (kernel_tick_counter - hc->last_activity_tick > HTTP_IDLE_TIMEOUT_TICKS) {
                        tls_server_close(i); tcp_close(i);
                        hc->in_use = 0; hc->tls = 0;
                        tcp_conn_release(i); hc->attributed = 0;
                    }
                    continue;
                }
                if (hs != TLS_SRV_OK) {
                    tls_server_close(i); tcp_close(i);
                    hc->in_use = 0; hc->tls = 0;
                    tcp_conn_release(i); hc->attributed = 0;
                    did_work = 1;
                    continue;
                }
            }

            /* `|| hc->tls` is not redundant. mbedTLS buffers a whole record
             * internally; once it has been pulled off TCP, c->rbuf_used is 0
             * while decrypted bytes are still sitting in the ssl context
             * waiting to be read. Gating on the TCP buffer alone would leave
             * a complete request unread until more bytes happened to arrive,
             * or the idle timeout fired -- a stall that looks like a slow
             * client and is not. mbedtls_ssl_read() reports WANT_READ, which
             * tls_server_read() returns as 0, so asking when there is nothing
             * costs one call. */
            if (c->rbuf_used > 0 || hc->tls) {
                int space = (int)sizeof(hc->buf) - 1 - hc->len;
                if (space > 0) {
                    int got = hc->tls
                        ? tls_server_read(i, (unsigned char*)hc->buf + hc->len, (size_t)space)
                        : tcp_recv(i, hc->buf + hc->len, (uint16_t)space);
                    /* A negative read is a dead session -- close_notify, a
                     * fatal alert, or a decrypt failure. Reap it now rather
                     * than let it hold a slot (one of TWO) until the idle
                     * timeout. tcp_recv() does not return negatives here, so
                     * this branch is the TLS path's alone. */
                    if (got < 0) {
                        tls_server_close(i);
                        tcp_close(i);
                        hc->in_use = 0;
                        hc->tls = 0;
                        tcp_conn_release(i);
                        hc->attributed = 0;
                        did_work = 1;
                        continue;
                    }
                    if (got > 0) {
                        hc->len += got;
                        hc->buf[hc->len] = '\0';
                        hc->last_activity_tick = kernel_tick_counter;
                    }
                }
                if (c->rbuf_used > 0) did_work = 1;
            }

            // Network Fairness Phase 2: attribute this connection to its
            // partition's connection quota as soon as an Authorization
            // header is readable, not at dispatch -- see tcp_quota.h's own
            // comment for why waiting for the full (possibly slow) request
            // body would defeat the point. Guarded on hc->len > 0 so this
            // never scans a freshly-picked-up slot's buffer before any real
            // byte has been received and null-terminated into it (this
            // array is reused across connections and isn't cleared at
            // pickup, only hc->len is).
            if (!hc->attributed && hc->len > 0) {
                uint32_t early_uid; SLSRole early_role;
                if (auth_http_extract(hc->buf, &early_uid, &early_role)) {
                    if (!tcp_conn_attribute(i, early_uid)) {
                        // Partition already at its concurrent connection
                        // quota -- refuse now rather than let this
                        // connection keep occupying a shared inbound slot
                        // for however long the rest of its (possibly slow)
                        // body takes to arrive.
                        did_work = 1;
                        const char* e429c = "{\"error\":\"Too many concurrent connections for this partition — try again shortly\"}";
                        http_respond(i, 429, "application/json", e429c, (int)strlen(e429c));
                        tls_server_close(i);   /* no-op unless this conn had a session */
                        tcp_close(i);
                        hc->in_use = 0;
                        hc->tls = 0;
                        hc->attributed = 0;
                        continue;
                    }
                    hc->attributed = 1;
                }
            }

            int peer_done = (!c->active || c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED);
            int ready = hc->len > 0 && http_request_ready(hc->buf, hc->len, (int)sizeof(hc->buf));

            if (ready || (peer_done && c->rbuf_used == 0)) {
                did_work = 1;
                if (hc->len > 0) http_route(i, hc->buf);
                tls_server_close(i);   /* no-op unless this conn had a session */
                tcp_close(i);
                hc->in_use = 0;
                hc->tls = 0;
                tcp_conn_release(i);   // Network Fairness Phase 2 -- safe no-op if never attributed
                hc->attributed = 0;
                continue;
            }

            if (kernel_tick_counter - hc->last_activity_tick > HTTP_IDLE_TIMEOUT_TICKS) {
                did_work = 1;
                tls_server_close(i);   /* no-op unless this conn had a session */
                tcp_close(i);
                hc->in_use = 0;
                hc->tls = 0;
                tcp_conn_release(i);   // Network Fairness Phase 2 -- safe no-op if never attributed
                hc->attributed = 0;
            }
        }

        /* Single-CPU fallback: with no AP there is nobody else running
         * flush_daemon_tick()/microkernel_service_poll(), so the BSP does
         * it. A no-op whenever an AP is online (kernel/smp.h).
         *
         * Deliberately BEFORE reconcile_drain(): this is what runs
         * reconcile_tick(), and draining immediately after means anything
         * it queues is applied in the same sweep rather than waiting for
         * the next one. */
        smp_uniprocessor_tick();

        // Orchestration Plan Phase 5: apply anything the AP-core reconciler
        // queued. This is the BSP, so persist_*() is safe here and is
        // exactly why the queue exists (kernel/workload.h).
        if (reconcile_drain()) did_work = 1;
        /* Advance live execution contexts. BSP only -- the interpreter's
         * RESOLVE/OBJSIZE opcodes read the object catalog, which the AP
         * core must not race (kernel/workload_ctx.h). Small budget: this
         * shares the loop with request service. */
        if (wlctx_step_all(2000)) did_work = 1;

        /* Registry replication upkeep, BSP-side.
         *
         * The heartbeat TRANSMITS, so it must live here and not in the
         * AP-core sweep: the NIC TX path is not safe to drive from two
         * cores at once, and this loop already owns it.
         *
         * The expiry pass is pure memory and could run anywhere; it is
         * here purely to keep both halves of the same mechanism in one
         * place. It only reclaims slots -- service_resolve() checks
         * freshness itself, so an entry past TTL stops resolving whether
         * or not this ever runs. */
        service_heartbeat_tick(kernel_tick_counter);
        service_remote_expire(kernel_tick_counter);

        /* Consensus: the cluster-wide membership heartbeat and the
         * per-partition write leases.
         *
         * ─── Why these are here, and were not anywhere ──────────────────
         * Both had NO caller in the kernel. check_consensus_heartbeat_tick()
         * carried a comment claiming it ran "every 10ms by the kernel timer
         * interrupt handler on Core 3"; a repo-wide grep found only its
         * definition, its prototype and one host test. The result was a
         * cluster that looked healthy and was inert: every node FOLLOWER at
         * term 0 forever, no leader ever elected, and therefore
         * partition_holds_write_lease() false for every partition and
         * dspp_page_write_allowed() (net/dspp.c) permanently closed.
         *
         * They belong on the BSP sweep rather than the AP tick because both
         * TRANSMIT, and the NIC TX path is not safe to drive from two cores
         * at once -- the same reason service_heartbeat_tick() above sits
         * here rather than in reconcile_tick().
         *
         * The sweep rate is load-dependent and unbounded, so neither
         * function may count its own calls: both take kernel_tick_counter
         * and measure real elapsed time against it. See net/consensus.h's
         * "Election timing" block. */
        check_consensus_heartbeat_tick(kernel_tick_counter);
        check_partition_lease_heartbeat_tick(kernel_tick_counter);
        /* Failover (Core Backup Strategies Step 5, wired live):
         * failover_tick() is pure memory and could run anywhere; the
         * leader's checkpoint broadcast TRANSMITS, so like the heartbeat
         * above it must stay on this BSP sweep rather than the AP tick --
         * the NIC TX path is not safe to drive from two cores at once.
         * Together they close the two gaps the comment above names for
         * the consensus heartbeat: peer death is now detected every
         * sweep, and a checkpoint actually flows from leader to
         * followers so the adoption path (failover_recover_from()) has
         * real data when the leader dies. */
        failover_tick(kernel_tick_counter);
        failover_live_checkpoint_broadcast(kernel_tick_counter);
        /* BSP-only half of peer discovery. The ISR that receives consensus
         * frames only sets a bit; the roster mutation has to happen here,
         * where nothing is reading it concurrently. */
        cluster_drain_discovered_peers();
        /* Phase 6: feed the endpoint probe and IPC queue depth into the
         * breakers. AFTER the heartbeat, deliberately -- the heartbeat is
         * what re-probes, so running first would judge breakers on the
         * previous interval's observation. */
        mesh_observe_local(kernel_tick_counter);

        /* ── The console ────────────────────────────────────────────────
         * This loop never returns, so sls_shell_loop() is unreachable on
         * any boot that found a NIC (kernel.c). Without this a clustered
         * node has NO control path: no keyboard driver, and no host port
         * forward possible because net/e1000.c binds a single NIC. Polling
         * here is what gives it a prompt.
         *
         * Same session and same dispatch the physical console uses, so
         * this is not a second, weaker shell -- and no wider than the
         * already-existing POST /api/shell/exec, which runs the same
         * sls_shell_execute() from this same loop. */
        {
            static char console_line[256];
            if (serial_console_poll(console_line, sizeof(console_line))) {
                sls_console_execute_line(console_line);
                sls_console_prompt();
                did_work = 1;
            }
        }

        // Nothing needed attention anywhere this sweep -- halt until the
        // next timer tick instead of busy-spinning (same idiom tcp_accept()/
        // tcp_recv() used before; see kernel/net_event.h).
        if (!did_work) net_event_hlt_wait();
    }
}
