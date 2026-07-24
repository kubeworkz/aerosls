#ifndef TCP_QUOTA_H
#define TCP_QUOTA_H

#include <stdint.h>

/*
 * tcp_quota.h -- Multitenant Isolation Gap Analysis §5 item 4 / §7 item 1,
 * Network Fairness Phase 2: per-partition concurrent inbound connection
 * quotas.
 *
 * Split into its own file for the same reason net/http_rate_limit.c was:
 * net/http.c's own dependency graph is too heavy to link for a host test,
 * but this mechanism's real logic has exactly the same two real
 * dependencies http_rate_limit.c already named -- partition_get_for_uid()
 * and PARTITION_MAX -- and deserves real execution, not just
 * compile-check. Takes a uid (not a partition_id) and resolves it
 * internally via partition_get_for_uid(), the identical shape
 * http_partition_rate_check(uint32_t uid) already established, for the
 * same reason: matches every other multitenancy choke point in this
 * codebase that's keyed on partition_id, not uid.
 *
 * ─── Why this is a genuinely different mechanism from http_rate_limit.h,
 * not a duplicate ──────────────────────────────────────────────────────
 * http_partition_rate_check() bounds how many requests a partition submits
 * OVER TIME (a fixed window). It says nothing about how many requests can
 * be simultaneously IN FLIGHT right now. net/http.c's http_server_run()
 * (Architectural Phase 1) deliberately multiplexes many TCP connections at
 * once specifically so slow clients don't block fast ones -- which means a
 * partition whose client(s) open many connections and upload their request
 * bodies slowly (each individually well under the rate-limit window's
 * request count) can still occupy a large share of the shared inbound pool
 * simultaneously, starving other tenants' ability to even get a connection
 * slot, without ever tripping the rate limiter. That's a real, distinct
 * failure mode this mechanism closes.
 *
 * ─── Why attribution happens as soon as headers arrive, not at dispatch
 * ─────────────────────────────────────────────────────────────────────
 * A raw TCP connection is anonymous until an Authorization header has
 * actually been read (net/http.c's own auth_http_extract() is the only
 * thing that ever learns a uid). Waiting until the FULL request (headers +
 * body, per Content-Length) is ready before attributing/quota-checking
 * would miss the exact scenario this exists to catch -- a slow body
 * upload holding a slot open for a long time. auth_http_extract() only
 * ever scans for a complete "Authorization: Bearer <token>\r\n" header
 * line and is safe to call repeatedly against a growing, null-terminated,
 * partial buffer (it simply returns "not found yet" until that line has
 * fully arrived) -- so net/http.c calls it (and this module) on every
 * accumulation sweep, not just once at dispatch, and attributes the
 * connection to its partition the moment identity becomes knowable,
 * holding that attribution for the connection's full remaining lifetime
 * (release happens at the same two teardown points tcp_close() is already
 * called from).
 *
 * ─── Why this only covers the inbound pool ─────────────────────────────
 * net/tcp.h's TCP_INBOUND_MAX_CONNS / TCP_OUTBOUND_RESERVED_CONNS split
 * (Network Fairness Phase 2's other half) already fully protects this
 * kernel's own outbound connections (net/ollama_client.c, net/
 * inference.c) from ever being starved by inbound tenant load -- those
 * connections have no partition_id to attribute anyway, since they're the
 * kernel's own outbound calls, not a tenant's. So this module only ever
 * needs to reason about conn_ids in [0, TCP_INBOUND_MAX_CONNS).
 *
 * ─── Why 0 = unlimited by default ──────────────────────────────────────
 * Same convention as storage_quota.h/frame_pool.c/http_rate_limit.h's own
 * BSS-zero-safe posture: every partition starts unrestricted (today's
 * existing behavior, unchanged until an operator explicitly opts a
 * partition into a cap), and a fresh boot needs no explicit init step.
 */

// Sentinel meaning "this inbound conn_id slot has not been attributed to
// any partition yet" -- distinct from every real partition_id (which are
// always < PARTITION_MAX, far below this value).
#define TCP_CONN_PARTITION_NONE 0xFFFFFFFFu

// Must be called once at startup (net/http.c's http_server_run(), alongside
// its own init loop) before any connection is accepted. Unlike this
// module's quota array (0 = unlimited is safely BSS-zero already), the
// attribution array's "unattributed" sentinel is deliberately NOT 0 --
// partition 0 (PARTITION_SYSTEM) is a real, commonly-used partition, so a
// BSS-zeroed attribution array would misread every fresh slot as "already
// attributed to partition 0" instead of "unattributed." This explicit init
// is the fix, the same "BSS zero isn't automatically the right default"
// carefulness tcp_init()'s own explicit conn_id stamping loop already
// applies for a different field.
void tcp_quota_init(void);

// Attempts to attribute connection conn_id to uid's partition and admit it
// against that partition's connection quota. Idempotent: if conn_id is
// already attributed (to any partition), returns 1 immediately without
// re-checking or double-counting -- callers are expected to invoke this on
// every accumulation sweep until it succeeds, exactly like
// http_partition_rate_check() is invoked fresh per request. Returns 0
// (deny, attributes nothing) if uid's Authorization header hasn't actually
// resolved to a partition yet (auth_http_extract() found nothing, or
// conn_id is out of range) -- callers must NOT treat a 0 return as a hard
// quota-exceeded signal by itself; see tcp_conn_quota_exceeded() below for
// that distinction, or simply retry next sweep.
//
// NOTE: this function does not itself call auth_http_extract() -- the
// caller (net/http.c) already has the partial buffer and calls it first,
// passing the resulting uid in here only once a header was actually found.
// This keeps this module's only real dependency at PARTITION_MAX, matching
// http_rate_limit.h's own "exactly two real dependencies" discipline.
int tcp_conn_attribute(int conn_id, uint32_t uid);

// Releases conn_id's attribution (if any) back to its partition's count.
// Safe to call on a never-attributed or already-released conn_id (no-op).
// Callers (net/http.c) call this at both points a connection slot is
// reclaimed -- normal dispatch-then-close and idle-timeout close -- the
// same two call sites tcp_close() is already invoked from.
void tcp_conn_release(int conn_id);

// Sets partition_id's concurrent inbound connection quota. 0 = unlimited
// (the default for every partition until this is called). Returns 0 on
// success, 1 if partition_id is out of range ([0, PARTITION_MAX)).
int tcp_partition_set_conn_quota(uint32_t partition_id, uint16_t quota);

// Introspection. Both return 0xFFFF for an out-of-range partition_id,
// mirroring storage_quota.h's sentinel convention (scaled to this
// mechanism's uint16_t counters, since a connection count can never
// realistically approach TCP_MAX_CONNS's own already-small range).
uint16_t tcp_partition_get_conn_usage(uint32_t partition_id);
uint16_t tcp_partition_get_conn_quota(uint32_t partition_id);

// Debug/introspection listing, mirrors sys_sls_partition_storage_quota_list()'s style.
void sys_sls_partition_conn_quota_list(void);

// ─── Syscalls ─────────────────────────────────────────────────────────────
// Confirmed as the next free syscall numbers via a fresh grep of every
// kernel/*.h and net/*.h SYS_SLS_* define before picking them (highest
// existing value found: 276, SYS_SLS_PARTITION_STORAGE_QUOTA_LIST), per
// this codebase's own established convention.
#define SYS_SLS_PARTITION_CONN_QUOTA_SET  277
#define SYS_SLS_PARTITION_CONN_QUOTA_LIST 278

struct SLSPartitionConnQuotaSetRequest {
    uint32_t partition_id;
    uint16_t quota;   // 0 = unlimited
};

/* Thin syscall wrapper, same shape as sys_sls_partition_storage_quota_set(). */
uint64_t sys_sls_partition_conn_quota_set(struct SLSPartitionConnQuotaSetRequest* req);

#endif /* TCP_QUOTA_H */
