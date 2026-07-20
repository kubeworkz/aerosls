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

## Phase 3 — Lock down CORS — DONE

Implemented: there were actually five hardcoded `Access-Control-Allow-Origin:
*` sites (`http_respond`, `http_respond_raw`, `http_respond_stream`,
`http_respond_program_binary`, `http_options`), not three — found the other
two while implementing. A wildcard could never have served more than "any
origin" anyway, since a browser only accepts a CORS response whose header
exactly matches its own `Origin`, so a fixed allowlist needed a real
per-request reflection, not a second static string.

`CORS_ALLOWED_ORIGINS[]` is a compile-time array (`https://aerosls.kubeworkz.io`,
`http://localhost:3000`, `http://localhost:3001` — confirmed with Dave).
`http_resolve_cors_origin()` reads the request's `Origin:` header once, in
`http_route()` before any response helper runs, and writes either the
matching allowed origin or an empty string into `g_cors_origin_hdr`; all five
sites now append that instead of a literal `*`. Resolved into a file-scope
buffer rather than threaded as a parameter through `http_respond()`'s ~90
call sites — safe for the same reason Phase 1/2's own state is: request
handling is still fully serialized, so nothing else touches that buffer
between it being set and read.

`gcc -fsyntax-only` clean — 0 errors, no new warnings.

## Phase 4 — Real credential check on token issuance — DONE

Implemented: `LeaseToken` (kernel/auth.h) gained `password_key[8]` +
`has_password`. `derive_user_key()` (`kernel/secure_api.c`, the same
primitive `seal` already uses — reused, not a second scheme) is applied to
`"<email>:<password>"` rather than the bare password, as a poor-man's
per-account salt, since the primitive itself has none. This is explicitly
**not** a real cryptographic password hash (no salt of its own, a fixed
non-standard iteration scheme) — documented as such in `LeaseToken`'s own
comment, matching this codebase's established honesty-over-false-security
convention (see `secure_api.c`'s own comment on `sys_sls_secure_seal()`).

`auth_http_issue()` (`POST /auth/token`) now requires the matching password
for any account with `has_password==1`, returning `{"error":"invalid
credentials"}` instead of the token otherwise. Unknown emails still
auto-provision a `ROLE_GUEST` token with no password required — no existing
identity to protect. All four `auth_init()` demo accounts got fixed demo
passwords (`demo-dave`, `demo-bob`, `demo-carol`, `demo-guest`), printed to
the boot log next to their tokens, same "developer can copy-paste it"
convention already used for the tokens themselves.

**Additional finding folded into this phase:** `auth create` (the shell
command that provisions new accounts) had no permission check at all — any
session, including a GUEST one, could mint itself a DB_ADMIN account.
Discovered while implementing the password check, in the exact code this
phase was already touching, so fixed in the same pass rather than filed
separately: `auth create` and `auth revoke` now require the caller to
already be DB_ADMIN or SYSTEM_KERNEL (`catalog_get_role(sess->uid)`).
`auth create` also gained a required trailing `<password>` argument.

**Frontend note:** `slsos-sim` never actually calls `POST /auth/token` —
every request uses one hardcoded `DEMO_TOKEN` constant
(`src/lib/apiFetch.ts`), and the "User Portal" login UI
(`SlsUserPortal.tsx`) is local/cosmetic (`localStorage`), not a real backend
login. So this phase closes a live vulnerability reachable by anyone calling
the API directly (e.g. `curl`), but doesn't change anything the shipped
frontend does today. Building a real per-user login flow that actually calls
this endpoint is a separate, larger frontend project — still an open item,
not attempted here.

Verified via `tests/auth_host_test.c` (extended, linked against the real
`kernel/auth.c` + `kernel/secure_api.c`, 22/22 checks pass, including wrong
password rejected, no password rejected, correct password accepted, and
dave's own demo account now requiring its password) and `gcc -fsyntax-only`
clean on every touched file (0 errors; only pre-existing warnings, none in
new code).

## Phase 5 — TLS (infrastructure, not kernel code) — DONE (config delivered; needs manual install)

Delivered: `deploy/Caddyfile`, targeting `localhost:3001` only. Re-examined
the production topology while writing this: the kernel itself serves the
compiled-in Navigator SPA bundle (`kernel/webapp_bundle.c`, produced by
`make bundle`) directly alongside `/api/*` on port 3001 — confirmed via
`net/http.c`'s own GET-route comment, which explicitly carves out "the
compiled-in Navigator SPA bundle" as public, non-`/api/` traffic on that
same port. So port 3001 is the one real production backend; the Node/Express
dev proxy on port 3000 (`server.ts`) is a local hot-reload convenience
against a locally-running kernel, never exposed publicly, and doesn't need
TLS — the Caddyfile deliberately doesn't front it.

No kernel code changes — the kernel keeps speaking plain HTTP behind the
proxy exactly as every other phase left it. Rate limiting (the bonus named
in this phase's original scope) is included as a commented-out block, since
it requires a custom Caddy build (`xcaddy` + `caddy-ratelimit`), not just a
config change — left as a documented next step rather than silently assumed.

This phase can't be verified with `gcc -fsyntax-only` like Phases 1-4 — it's
a config file that needs installing on the actual server, outside what's
reachable from here. Open question for Dave: does `aerosls.kubeworkz.io`
already have TLS termination somewhere (e.g. Cloudflare) in front of it
today? If so this Caddyfile should either replace that layer or be adapted
to sit behind it in "full" mode rather than compete with it on port 443.

---

## Suggested sequencing

1. Phase 1 + Phase 2 together (coupled; highest leverage; directly fixes the
   failure mode already hit in production).
2. Phase 3 (CORS) — trivial, fold in alongside 1/2 review since it's the same
   file.
3. Phase 4 (real auth) and Phase 5 (TLS, infra-only) — independent of each
   other and of 1/2/3, can run in parallel or in either order.
