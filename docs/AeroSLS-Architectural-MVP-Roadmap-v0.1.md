# AeroSLS Architectural MVP Roadmap v0.1

## 0. Why this doc exists

The gap analysis identified five architectural blockers standing between "impressive
simulator" and "product multiple people can use at once": single-threaded HTTP
serving, global (non-per-user) session state, wide-open CORS, auth with no real
credential check, and no TLS. This doc scopes each one against what's actually in
the codebase today, rather than a from-scratch redesign.

Investigation findings that shape the plan:

- `net/http.c`'s `http_server_run()` is a plain `for(;;)` loop: block on
  `tcp_accept()`, then block-drain exactly one connection via `tcp_recv()`/dispatch
  before accepting the next. This is precisely why the earlier "SQL Console hangs
  forever" bug took down *every* client, not just the one that triggered it — one
  stuck request occupies the server's only execution path.
- The TCP layer itself is **not** the bottleneck. `net/tcp.c` already maintains up
  to `TCP_MAX_CONNS` (8) independent `TCPConn` slots, each with its own state and
  32 KiB receive ring buffer, populated by segment arrival independent of what
  `http.c`'s application loop happens to be doing. The serialization is entirely
  an `http.c` application-loop problem, not a network-stack problem.
- `kernel/scheduler.c`'s `spawn_kernel_thread()` is cooperative and its own comment
  says the function pointer "is invoked directly by the service loop, not via
  context switch" — it's built for microkernel service dispatch (block/wake on a
  vaddr), not general preemptive multithreading. There's no existing primitive for
  running kernel logic on two connections literally in parallel, and building one
  would mean auditing ~40 subsystems (object catalog, row store, journal, tier
  manager, etc.) for thread-safety — far outside MVP scope.
- Session identity (`current_session_uid`, `current_session_gid`, `current_tx_id`
  in `user/shell.c`) is file-scope global state, shared by every request. Safe
  today only because there's exactly one request in flight at a time.
- `POST /auth/token` (`api_auth_token` → `auth_http_issue`) issues a valid bearer
  token for **any** email with no credential check at all — confirmed by reading
  `kernel/auth.c`/`auth.h`: `AuthCreateRequest` and `LeaseToken` have no password
  field whatsoever. The four demo accounts are the only accounts with any
  provenance; anyone else can self-issue a token for any uid/role.
- CORS is wide open (`Access-Control-Allow-Origin: *`, three call sites in
  `net/http.c`).
- There is no TLS anywhere in the hand-rolled net stack.

Given this, the pragmatic path is: fix the application-loop serialization using
the concurrency the TCP layer already has (Phase 1), make session/transaction
state per-connection instead of global (Phase 2 — must follow immediately after
Phase 1, since 1 without 2 introduces real cross-user bugs), then three
comparatively contained/independent fixes (CORS, real auth, TLS via reverse
proxy). True SMP-parallel kernel execution across all subsystems is explicitly
out of scope for MVP — not needed to fix the observed failure mode, and a
multi-week undertaking on its own.

---

## Phase 1 — Non-blocking, multiplexed HTTP loop (`net/http.c` only) — DONE

Implemented: `http_server_run()` rewritten to a per-sweep poll over
`http_conns[TCP_MAX_CONNS]` (indexed directly by `tcp_conns[]` slot/`conn_id`),
replacing the old accept-one/drain-one blocking loop. `http_request_ready()`
extracts the unchanged completeness check (headers + Content-Length, or
buffer-full bail-out) so every tracked connection runs the same logic. Added
`HTTP_IDLE_TIMEOUT_TICKS` (~10s) to reclaim a slot from a connection that opens
and never sends anything — a new failure mode this design introduces, since the
old loop only ever tracked one connection and had no notion of a "wasted slot."
No change to `tcp.c`, `ipv4.c`, or any kernel subsystem. `gcc -fsyntax-only`
clean — 0 errors, 0 new warnings (20 pre-existing, none in the touched
functions).

**Goal:** one slow/stuck client can no longer block every other client. No other
file needs to change.

- Replace the blocking `tcp_accept()` / drain-one-connection loop with a poll
  loop that, each iteration, opportunistically checks the listen socket for a new
  connection *and* advances every active `tcp_conns[]` slot that has unread data.
- Move the current loop-local `req_buf`/`rlen` request-assembly state into a
  per-connection struct (array sized `TCP_MAX_CONNS`), since multiple partial
  requests can now be in flight simultaneously.
- When a connection's buffer holds a complete request (existing
  `\r\n\r\n` + `Content-Length` logic, unchanged), dispatch it through the
  existing handler code exactly as today — one dispatch at a time, no changes to
  command execution. This keeps every kernel subsystem's mutation path
  single-threaded, so nothing downstream needs new locking.
- Add a per-connection idle timeout (ticks since last byte, reusing
  `kernel_tick_counter` the way `auth.c` already does for token TTL) so a client
  that opens a connection and sends nothing doesn't permanently occupy one of the
  8 slots.
- Capacity note: `TCP_MAX_CONNS = 8` and `TCP_RECV_BUF_SZ = 32 KiB` are cheap
  `#define`s to raise later if 8 concurrent connections proves tight for MVP
  traffic — worth revisiting after Phase 1 ships, not before.

**Risk:** contained entirely to `net/http.c`. No change to `tcp.c`, `ipv4.c`,
`e1000.c`, or any kernel subsystem.

## Phase 2 — Per-connection session/transaction state — DONE

Implemented: `sls_shell_execute()` now takes a `struct ShellSession*
sess` (uid/gid/tx_id) defined in a new `user/shell.h`, instead of reading/
writing three shell.c file-scope globals. Internally the function still
reads/writes plain locals named `current_session_uid`/`current_session_gid`/
`current_tx_id` — they're now copied in from `*sess` at entry and written
back at exit, so none of the ~20 existing call sites inside the ~1400-line
dispatch needed to change. `sls_shell_loop()` (the serial console) owns one
persistent `serial_session` exactly as before — zero behavior change there.

`net/http.c`'s `/api/shell/exec` route (the one place this mattered — every
other route already threaded `req_uid` through correctly, confirmed by
reading the existing convention) now keeps a small table of one
`ShellSession` per authenticated uid (`http_shell_session_for()`, capped at
`AUTH_MAX_TOKENS`), reseeding `.uid` from the bearer token on every call so
identity is never trusted from stored state, while `.tx_id` persists across
separate requests from the same authenticated user — needed since Phase 1
made every HTTP request its own short-lived connection with nothing else to
hang session continuity off of. Two different users' sessions can never
collide: uid is both the table's lookup key and the value reseeded from the
token each call.

`gcc -fsyntax-only` clean on both `user/shell.c` and `net/http.c` — 0
errors, no new warnings in either file.

**Goal:** once Phase 1 allows genuinely concurrent requests, two different users
must not share one identity or one open SQL transaction.

- `current_session_uid`/`gid`: bearer-token auth already resolves uid/role per
  request via `auth_http_extract()` (Gap Remediation Phase E). Thread that
  resolved identity through as a parameter to `sls_shell_execute()`/route
  handlers instead of reading/writing the shell.c globals, or store it in the
  Phase 1 per-connection struct (session-per-connection matches the existing
  "lease a token" auth model more naturally than session-per-request).
- `current_tx_id`: currently one system-wide "open transaction." Move into the
  same per-connection struct. `lock_mgr.c` already scopes row locks by `tx_id`,
  so the concurrency-control plumbing underneath is already keyed correctly —
  this is a matter of giving each connection its own `tx_id` slot to write into,
  not building new locking.
- Mechanical but touches several call sites (`shell.c`, `mvcc.c`, `http.c`
  handlers) — the largest diff of the five phases, but a threading-through
  exercise rather than a redesign.

**Must ship together with or immediately after Phase 1** — Phase 1 alone would
make the existing global-state bug live (two real concurrent users instead of
one at a time hitting the same globals).

## Phase 3 — Lock down CORS

- Replace the three hardcoded `Access-Control-Allow-Origin: *` sites in
  `net/http.c` (~L249, L276, preflight handler ~L563) with the actual deployed
  origin(s), ideally sourced from a boot-time config value so dev/staging/prod
  can differ without a rebuild.
- Independent of every other phase. Smallest, lowest-risk change in this doc —
  candidate to do first as a quick win regardless of Phase 1/2 sequencing.

## Phase 4 — Real credential check on token issuance

- Add a password field to account records and verify it in `auth_http_issue()`
  before minting a token — today there is no check at all. `secure_api.c`
  already has a password-derived-key primitive (used by the `seal` shell
  command) that's the natural thing to reuse for hashing credentials, rather
  than introducing a second hashing scheme.
- The four existing `auth_init()` demo accounts become seed data for one org's
  admin/demo users rather than the entire auth model.
- Open product question, not an engineering one: does MVP need self-serve
  signup, or is "admin provisions accounts, users log in with a password"
  sufficient? Worth deciding before implementation, not assumed.
- Independent of Phases 1/2/3.

## Phase 5 — TLS (infrastructure, not kernel code)

- Put a TLS-terminating reverse proxy (Caddy or nginx) in front of both the
  QEMU port-forward (host 3001) and the Node dev proxy (host 3000); it speaks
  HTTPS to clients and plaintext HTTP to the kernel exactly as today. Building
  TLS into the from-scratch net stack is disproportionate effort for MVP.
- Bonus: the same proxy layer is the natural place to add rate limiting
  (previously flagged as lower-priority) essentially for free.
- Independent of every other phase — can be done in parallel by whoever isn't
  touching `http.c`.

---

## Suggested sequencing

1. Phase 1 + Phase 2 together (coupled; highest leverage; directly fixes the
   failure mode already hit in production).
2. Phase 3 (CORS) — trivial, fold in alongside 1/2 review since it's the same
   file.
3. Phase 4 (real auth) and Phase 5 (TLS, infra-only) — independent of each
   other and of 1/2/3, can run in parallel or in either order.
