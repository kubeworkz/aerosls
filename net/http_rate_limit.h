#ifndef HTTP_RATE_LIMIT_H
#define HTTP_RATE_LIMIT_H

#include <stdint.h>

/*
 * http_rate_limit.h -- Multitenant Isolation Gap Analysis §5 item 4 / §7
 * item 1: per-partition HTTP request-rate fairness.
 *
 * Split into its own file rather than left inline in net/http.c for the
 * same reason net/dspp.c (Multi-Node Partition Scaling Roadmap Phase 5)
 * and process.c's pick_next_partition()/pick_next_process_in_partition()
 * helpers (LPAR Phase 12) were each pulled out or factored on their own:
 * net/http.c's own dependency graph (rowstore, sql_exec, vec_join, and
 * dozens more) is far too heavy to link for a host test, but this
 * mechanism's actual logic has exactly two real dependencies --
 * partition_get_for_uid() and kernel_tick_counter -- and deserves real
 * execution, not just compile-check, the same "verification ceiling
 * honesty, checked before assumed" discipline the Multi-Node roadmap's
 * own §2 names as a standing principle.
 *
 * ─── Why request-rate, not connection-count ────────────────────────────
 * Connection-level quotas were the other candidate mechanism named in the
 * gap analysis doc, but two real properties of this server rule it out.
 * First, tcp_conns[]/TCP_MAX_CONNS (net/tcp.h) is one small pool shared
 * by BOTH inbound tenant HTTP traffic and the kernel's own outbound
 * connections (e.g. net/ollama_client.c's tcp_connect() to reach Ollama)
 * -- tcp.h's own comment already documents a real incident where inbound
 * polling starved an outbound system call out of that shared pool, so
 * slicing it per-partition would need to first separate inbound from
 * outbound, a larger change this phase doesn't attempt (named, not
 * silently folded in -- see the gap analysis doc's own §7 item 9 on
 * capacity resizing needing a real target tenant count first). Second,
 * http_server_run()'s own header comment documents that request DISPATCH
 * (http_route()) runs one request to completion before moving to the
 * next connection's request -- there is no concurrent-requests-in-flight
 * concept for a per-partition concurrency cap to bound. What IS real and
 * meaningful: how many requests a partition submits over time. A plain
 * fixed-window counter, the same "simple, auditable, first cut" posture
 * LPAR Phase 12 chose (starvation-prevention round robin) over full
 * weighted fairness.
 *
 * ─── Why keyed on partition_id, not uid ────────────────────────────────
 * Matches every other multitenancy choke point in this codebase (catalog
 * access, process spawn, IPC, scheduling, frame quotas): a tenant's other
 * uids sharing the same partition correctly share the same request
 * budget too, since the resource being protected is shared HTTP-serving
 * capacity, not a per-user allowance.
 */

// ~10s at the documented ~100 Hz (kernel/auth.c's AUTH_TOKEN_TTL_TICKS
// comment establishes the same tick-rate reasoning; matches net/http.c's
// own HTTP_IDLE_TIMEOUT_TICKS unit).
#define HTTP_PARTITION_RATE_WINDOW_TICKS 1000

// requests per window per partition -- roughly 8x headroom over this
// frontend's own steady-state background-polling baseline (net/tcp.h's
// own comment: ~6-8 concurrent pollers hitting several /api/* endpoints
// every few seconds), a deliberately generous first cut, named as
// tunable rather than claimed to be a measured production value.
#define HTTP_PARTITION_RATE_LIMIT        100

// Returns 1 if this uid's partition may proceed (and counts the request
// against that partition's current window), 0 if the partition has
// already exhausted its window budget. Callers in net/http.c only ever
// invoke this after a caller has already passed the 401 authentication
// gate, so uid here is always meant to be a real, authenticated identity
// -- an unauthenticated request never reaches this check at all.
// BSS-zero-safe by construction, the same discipline frame_pool.c's
// quota arrays and partition_owner_table[] already established: every
// partition's window_start defaults to 0, which is always more than
// HTTP_PARTITION_RATE_WINDOW_TICKS in the past by the time this server
// has actually booted and started accepting connections, so the very
// first request for any partition correctly opens a fresh window rather
// than needing an explicit init step.
int http_partition_rate_check(uint32_t uid);

#endif /* HTTP_RATE_LIMIT_H */
