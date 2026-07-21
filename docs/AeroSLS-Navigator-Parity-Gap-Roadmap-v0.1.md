# AeroSLS Navigator-Parity Gap Roadmap v0.1

## 0. Why this doc exists

IBM Navigator for i's core administrative surface groups into four
categories: System Health & Performance, Work Management, Security & Users,
and Network & Storage. This doc checks AeroSLS's real feature set (kernel +
`slsos-sim` frontend) against each, based on direct investigation of the
current code (file:line references below), not assumption — the same
posture every prior roadmap in this repo has taken.

The headline finding: AeroSLS's kernel already has more real backend
substance in most of these categories than the frontend currently shows.
Two of the four frontend surfaces that *should* represent this data —
System Health and Security/Users — are pure client-side simulations wired
to nothing, while the real kernel data they'd need mostly already exists
and is just sitting unused behind other endpoints. That mismatch, not a
missing kernel feature, is the single highest-value gap here.

Five real gaps, in priority order:

1. **The frontend lies about system health and security.**
   `SlsSystemHealth.tsx` derives its "risk score"/"health score" entirely
   from a client-side random-walk (`INITIAL_METRICS` in `src/lib/
   slsEngine.ts`, mutated via `setSystemMetrics`) and never calls
   `authFetch`/`fetch` against `/api/metrics` or `/api/health` at all — its
   "Compact & Optimize Memory" button just scales `pageFaultCount` down
   locally with a timer, no kernel call happens. `SlsSecurityDashboard.tsx`
   and `SlsUserPortal.tsx` are the same story: local `securityLogs` React
   state and a `DEFAULT_PORTAL_USERS` mock array, with zero calls into
   `net/auth.c` or `kernel/object_catalog.c`, even though both have real,
   working HTTP-reachable implementations already. This is worse than a
   missing feature — it's a UI that actively misrepresents what the kernel
   is actually doing.

2. **No real performance telemetry beyond a handful of counters.**
   `GET /api/metrics` (`net/http.c:204` `api_metrics()`) only reports
   `total_accesses`, `total_promotions` (storage tier), and IPC post/
   dispatch counts + average latency. `kernel/dashboard.c` additionally
   tracks page-fault and eviction counts with a rolling-average fault
   latency in raw, uncalibrated CPU cycles (`average_fault_latency_cycles`,
   `dashboard.c:12-37`) — serial-console only, point-in-time, no history
   buffer. There is no CPU utilization/load tracking anywhere (`kernel/
   smp.c` only does AP bring-up via LAPIC IPIs — no per-core busy/idle
   accounting exists), no RAM usage beyond fault/eviction counters, and no
   disk capacity or health reporting (`drivers/nvme.h`/`nvme_admin.h` define
   controller/queue registers but expose no capacity, identify-namespace,
   or SMART-equivalent data anywhere in the driver).

3. **Security has real RBAC but no groups, authorization lists, or audit
   log.** `kernel/auth.c`/`auth.h` implements real bearer-token accounts
   (`struct LeaseToken`, `auth.h:29`) with optional passwords
   (`derive_user_key`-based) and TTL enforcement
   (`AUTH_TOKEN_TTL_TICKS`). `object_catalog.h` defines real per-uid RBAC
   with `catalog_check_access()` (`object_catalog.c:116-170`) — but roles
   are a fixed 4-entry enum (`ROLE_SYSTEM_KERNEL/DB_ADMIN/APP_USER/GUEST`,
   `object_catalog.h:34-37`), not arbitrary named groups an admin creates.
   There is no IBM-i-style authorization list (a named list joining
   multiple objects + multiple users/groups under one grantable unit), and
   no security audit log anywhere — the only journals in this codebase
   (`journal.c`, `row_journal.c`) are database WAL journals, not
   security-event logs.

4. **Work management has real jobs but no priority, hold, or message
   queues.** `struct ProcessDescriptor` (`kernel/process.h:29`) is a
   genuine job unit — pid, state (`RUNNING/SUSPENDED/ZOMBIE`), owner_uid,
   partition_id — with `sys_sls_proc_list()`/`process_kill()` already
   reachable via `GET /api/processes` and the Terminal's `proc list`/`proc
   kill <pid>`. `kernel/partition.h` (LPAR) gives a subsystem-pool-like
   resource/isolation boundary with pause/resume and quotas. Missing:
   per-job priority, per-job HOLD/RELEASE (pause/resume only exists at the
   *partition* level today, not per-process), queued/scheduled jobs, and
   any user-facing message queue — `kernel/ipc.h`'s real IPC layer is a
   fixed 6-port system-service bus plus 16 user ports, with no naming, no
   listing, and no Terminal/UI surface for queue contents.

5. **Network and storage are compiled-in constants, not administrable
   objects.** The network stack (`net/tcp.c`, `net/http.c`, `net/ipv4.c`,
   `net/arp.c`, `net/dhcp.c`, `net/e1000.c`) and storage drivers
   (`drivers/nvme*.c`, `drivers/ahci.c`) are real and functional, and
   storage tiers are exposed via `/api/tiers` — but there is no
   configurable network-interface surface, no start/stop concept for
   "network servers," no DNS/routing configuration, and no disk-unit or
   capacity-management UI. Everything here is a `#define` or hardcoded
   DHCP negotiation, not something an operator configures at runtime. This
   is the category most different in kind from Navigator, which manages
   *configuration* of these things — bringing AeroSLS to real parity here
   is a much larger, more architectural undertaking than the other four,
   which is why it's sequenced last and scoped lightest below.

Each phase below closes one of these, roughly in the order that gets the
most truth-in-UI and real operator value per unit of work.

---

## Phase 1 — Wire real backend data into System Health + Security/User tabs — DONE

**Scope correction made during implementation:** this phase's original scope
(above, preserved below) assumed all three frontend surfaces —
`SlsSystemHealth.tsx`, `SlsSecurityDashboard.tsx`, `SlsUserPortal.tsx` — were
broken dashboards misrepresenting real kernel state. A full read of all
three during implementation (not just the excerpts the initial gap analysis
had seen) showed this was only true for one of them:

- `SlsSystemHealth.tsx`'s underlying `systemMetrics` data was **already
  real** — `App.tsx` has had a genuine 5-second poll loop against
  `/api/health`/`/api/metrics`/`/api/tiers`/`/api/objects` all along,
  feeding real kernel state into this component. The only actual lie was
  the "Compact & Optimize Memory" button, which locally faked a 75%
  reduction in `pageFaultCount` — a value mapped from the kernel's real,
  monotonically-increasing `total_promotions` counter — that the very next
  5s poll tick would have silently overwritten anyway. A real, narrow bug.
- `SlsSecurityDashboard.tsx` is explicitly labeled in its own UI as an
  "Interactive Privilege Simulation" / "Security Context Simulator" for
  exploring hypothetical object-ACL configurations against a self-contained
  mock object model — it never claims to reflect live `object_catalog.c`
  RBAC state, so it isn't the misrepresentation the original scope assumed.
- `SlsUserPortal.tsx` is a complete, self-aware fictional SaaS billing
  product simulator ("Sovereign Flat Memory Cloud // Infrastructure
  Lease," explicitly-labeled "Simulated Balance," fake credits, lease
  tiers, API key management) with no field-level correspondence to
  `auth.c`'s real `LeaseToken` (no balance, no tiers, no API keys there) —
  forcing real accounts into `DEFAULT_PORTAL_USERS` would break a coherent,
  intentional feature rather than fix a lie.

Given this, the user chose (via an in-session decision point) to narrow
Phase 1 to the one real bug rather than rewire the two intentional
simulators. **What was actually built:**

- `App.tsx`: the poll loop's `poll` function was hoisted out of its
  `useEffect` into its own `useCallback` (deps `[]`, unchanged behavior),
  with a second, small `useEffect([poll])` left to call it immediately and
  on the existing 5s interval. This lets `poll` be passed down as a real
  "refresh now" callback rather than only being reachable from inside its
  original closure.
- `SlsSystemHealth.tsx`: the `setSystemMetrics` prop (only ever used by the
  fake-improvement logic) was replaced with `onRefreshNow?: () => void |
  Promise<void>` — `App.tsx`'s real `poll`. The "Compact & Optimize Memory"
  button is now "Refresh Kernel Telemetry": same animated step sequence
  (relabeled to describe the real `/api/health`/`/api/metrics` calls being
  made instead of fictional "sector decompression"), but it now `await`s
  the real `onRefreshNow()` and reports "Telemetry refreshed from live
  kernel" — true regardless of whether the numbers moved — instead of
  claiming a fabricated "System Health Restored to 100%!" improvement to a
  counter that cannot legitimately decrease.

**Deliberately not done, and why:** rewiring `SlsSecurityDashboard.tsx` to
real `catalog_check_access()` state and `SlsUserPortal.tsx` to real
`auth_tokens[]` accounts were both considered and explicitly declined for
this phase — not because they're out of scope forever, but because neither
component is dishonest about what it is today, and forcing real data into
either would change their purpose (from "explore ACL concepts" /
"SaaS billing demo" to "live admin panel") rather than fix a misrepresentation.
If real-account visibility or live-RBAC visibility becomes a wanted feature
later, it should be scoped as its own addition — e.g. a new, clearly-labeled
panel — rather than retrofitted into these two.

**Verification:** `npx tsc --noEmit -p .` clean; live browser pass against
the redeployed kernel confirming the button now reads "Refresh Kernel
Telemetry," clicking it shows "Telemetry refreshed from live kernel" (not
the old false 100%-restored claim), and the underlying fault-rate/total-
access numbers are unchanged by the click itself (only a real poll can move
them now).

---

### Original Phase 1 scope (as first drafted, before the correction above)

**Goal:** stop the frontend from showing invented numbers where real ones
already exist. This is the cheapest, highest-value phase here: no new
kernel work, just replacing client-side simulation with real fetches
against endpoints that already work.

**What already exists and is reusable:** `GET /api/metrics` and `GET
/api/health` (`net/http.c:190-215`) are live and unauthenticated-safe to
poll. `GET /api/processes` (`net/http.c:872`) already lists real jobs.
`net/auth.c`'s `auth_http_extract`/token model and `object_catalog.c`'s
`role_table[]` are real and already gated the same way every other
authenticated route in this app is (`apiFetch.ts`'s shared `authFetch`
helper — the established, single choke point every other panel already
uses, so this phase follows an existing convention rather than inventing a
new fetch pattern).

**What needs to be built:**
- `SlsSystemHealth.tsx`: replace the `INITIAL_METRICS` random-walk with a
  poll loop (matching the 5s cadence the main dashboard already uses
  elsewhere in `App.tsx`) against `/api/metrics` + `/api/health`, deriving
  risk/health scores from real `total_accesses`/`total_promotions`/IPC
  latency instead of a synthetic page-fault percentage. The "Compact &
  Optimize Memory" action either needs a real kernel-side effect to call
  (none currently exists — see Phase 2's note on this) or should be
  honestly relabeled/removed rather than faking a result.
- `SlsSecurityDashboard.tsx`: replace `securityLogs` local state with a
  real feed — this phase can only wire what exists today (RBAC checks via
  `catalog_check_access`), so initially this means surfacing real
  role/permission state per object, not fabricated log entries; a true
  audit trail is Phase 3's job once one exists to read from.
- `SlsUserPortal.tsx`: replace `DEFAULT_PORTAL_USERS` with `GET
  /api/auth/list` (new thin HTTP wrapper over the existing
  `sys_sls_auth_list()`/`auth_tokens[]` if no such route exists yet —
  confirm during implementation) so the portal shows real registered
  accounts instead of a mock array.

**Verification:** `npx tsc --noEmit -p .` clean; live browser pass
confirming each panel's numbers move when real kernel state changes (e.g.
`/api/metrics`' `total_accesses` increases after using another tab,
`/api/processes`-backed data matches `proc list` in the Terminal for the
same moment).

---

## Phase 2 — Real CPU/RAM/disk performance metrics + trend history — DONE

### Scope correction made during implementation

The original draft below (preserved under "Original Phase 2 scope") assumed
three new kernel data sources would need to be built from nothing, plus a
new kernel-side ring buffer for trend history. Investigation confirmed the
first part but corrected the shape of the work, and the ring buffer was
deliberately dropped:

- **CPU:** `kernel/smp.c` indeed has no per-core busy/idle accounting. But
  rather than building new scheduler-accounting state, this reuses
  `kernel/net_event.h`'s pre-existing `net_event_hlt_wait()` — the real
  `sti; hlt` CPU-yield already called throughout `net/http.c`'s main loop,
  `net/tcp.c`, and `net/dhcp.c` whenever there's genuinely nothing to do
  (its own header comment already documents the real "~100% spin-poll →
  <1% hlt-wait" utilization drop this achieves). Added one cumulative
  counter, `cpu_idle_wait_count`, incremented on every call. Named
  explicitly as approximate, not exact — mirroring `AUTH_TOKEN_TTL_TICKS`'s
  own precedent — since a single `hlt_wait()` can be woken by any
  interrupt, not strictly one timer tick, and since this only measures the
  BSP core's own loop: Core 1's `microkernel_service_poll()` loop in
  `kernel/smp.c` never calls `net_event_hlt_wait()` and never truly idles
  (confirmed by reading `ap_kernel_main()`'s fixed
  `while(1) { flush_daemon_tick(); microkernel_service_poll(); kernel_sleep_ticks(10); }`
  loop) — so this is a real but BSP-core-only signal, not whole-system CPU
  utilization.
- **RAM:** `frame_pool.c` had per-partition accounting
  (`partition_get_frame_usage`/`_quota`) but no system-wide total/allocated
  introspection. Added `frame_pool_total_frames()` (the bitmap's fixed
  `TOTAL_FRAMES` capacity) and `frame_pool_allocated_count()` (a live
  popcount over `physical_memory_bitmap`) to `frame_pool.c`/`.h`. The
  popcount deliberately avoids `__builtin_popcountll` — this kernel builds
  freestanding with no libgcc linked, and that builtin can lower to a
  libgcc call depending on optimization level/target flags, the same class
  of ABI pitfall already named and worked around elsewhere in this codebase
  (see the float-return-ABI x86 cross-build fix). Uses a plain Kernighan
  bit-counting loop instead.
- **Disk:** the original draft assumed this would "extend" an
  already-issued Identify-Namespace command path. Investigation found
  `nvme_admin.h`/`.c` had zero Identify-related opcodes or functions before
  this phase — only `CREATE_CQ`/`CREATE_SQ` existed. This was a genuinely
  new admin command path, not an extension: added
  `NVME_ADMIN_CMD_IDENTIFY (0x06)`, `nvme_identify_namespace(nsid)` (issues
  the command, caches `NCAP` from the returned 4KB Identify Namespace Data
  Structure), and `nvme_get_capacity_bytes()` (returns `NCAP * 512`,
  assuming 512-byte logical blocks — deliberately not parsing the LBA
  Format table for the real per-namespace sector size, matching
  `nvme_io.c`'s own existing sector-size assumptions elsewhere in this
  driver). Wired to run once at boot right after the admin queue comes up
  in `kernel.c`, unconditionally (not nested inside the I/O-queue-success
  branch), so disk-capacity reporting works even if I/O queue setup itself
  fails.
- **Ring buffer: dropped.** The original draft's "small ring buffer... so
  the frontend can show a trend line" would have added new timer-driven
  kernel state. Implementation instead lets the frontend keep its own
  bounded rolling window over real 5-second-interval polls client-side —
  the trend line is just as real (built from genuine samples, not
  fabricated), with no new kernel state added. Named directly in
  `api_metrics()`'s own comment as a "smallest real version first"
  simplification.

All five new values (`cpu_idle_ticks`, `cpu_total_ticks`,
`ram_allocated_frames`, `ram_total_frames`, `disk_capacity_bytes`) are
wired into the existing `GET /api/metrics` route, following the same
"cumulative counter, diffed by the caller" convention already established
by that route's `total_accesses`/`total_promotions` fields — `cpu_idle_ticks`/
`cpu_total_ticks` are raw counters, not a pre-computed percentage, so a
client diffing two consecutive polls gets a real windowed CPU busy% for
that interval rather than an instantaneous (and noisier) one.

`App.tsx`'s `poll()` now diffs consecutive `cpu_idle_ticks`/`cpu_total_ticks`
samples (via a `useRef` holding the previous poll's raw values, clamped to
0–100% since `net_event_hlt_wait()` can be called more than once per timer
tick) into a windowed `cpuBusyPercent`, and passes `ram_allocated_frames`/
`ram_total_frames`/`disk_capacity_bytes` straight through into
`SlsSystemMetrics`. `SlsSystemHealth.tsx`'s telemetry popover now shows
CPU BUSY, RAM USED (a simple ratio of the two frame counts), and DISK
CAPACITY (formatted GB/MB) alongside the existing fault-rate figures.

**What was NOT done:** a pre-existing, separate gap was found and
deliberately not fixed in this pass — `kernel.c` calls
`init_nvme_controller()`/`nvme_io_init()` via legacy implicit-int function
declaration (no header, no forward declare for either). A proper
`#include "../drivers/nvme_admin.h"` was added for this phase's own new
function prototypes, but the other two pre-existing implicit declarations
were left as-is and named in a code comment rather than silently expanded
into an unrelated fix.

**Verification performed:** `gcc -fsyntax-only` compile-check across
`kernel.c`, `net_event.c`, `frame_pool.c`, `nvme_admin.c`, `net/http.c`,
`user/shell.c` — zero new errors, only pre-existing unrelated implicit-
declaration warnings and one `-Waddress-of-packed-member` warning matching
an existing pattern already used identically in `nvme_io.c`. Full host
test suite (`tests/run_all.sh`) — 24/24 passed, 0 failed, 0 skipped, no
regressions. Frontend typecheck (`npx tsc --noEmit`) — clean.

### Original Phase 2 scope (as first drafted, before the correction above)

**Goal:** give System Health something real to show beyond tier-access
counters — actual resource utilization, plus enough history to show a
trend, not just a snapshot.

**What already exists and is reusable:** `dashboard.c`'s fault/eviction
counters and the rolling-average latency idiom
(`average_fault_latency_cycles`, updated with a `(prev*7 + new)/8` decay —
`dashboard.c:34-37`) are a real, working pattern for a lightweight rolling
stat; the same idiom can back new CPU/RAM stats without inventing a new
math approach. (Investigation during implementation found
`dashboard_log_fault_start()`/`_end()` are never actually called anywhere
in the codebase — this instrumentation is permanently zero, dead code, not
a live pattern to build on. `net_event_hlt_wait()` and `frame_pool.c`'s
bitmap were used instead; see the correction above.)

**What needs to be built:**
- CPU: `kernel/smp.c` has no per-core busy/idle accounting today. Simplest
  real signal without a full scheduler-accounting subsystem: track idle-
  loop time vs. total tick count per core (a tick-based approximation,
  same "documented ~100 Hz, approximate not exact" honesty `auth.h`'s own
  TTL comment already established for this codebase — see `AUTH_TOKEN_TTL_TICKS`'s
  comment, `auth.h:13-24`).
- RAM: extend beyond fault/eviction counts to actual frame-pool
  utilization — `kernel/frame_pool.c` (referenced by Operational-phase and
  partition-quota work elsewhere in this repo) should already track
  allocated-vs-total frames; expose that directly rather than inferring it
  from fault counters.
- Disk: extend `drivers/nvme_admin.c`'s existing Identify-Namespace admin
  command path (if not already issued at boot) to capture and cache
  `NSZE`/`NCAP` (namespace size/capacity) so `/api/metrics` or a new
  `/api/disk` route can report real capacity — currently this data is
  never requested from the controller at all.
- A small ring buffer (e.g. last 60 samples at whatever poll interval
  Phase 1 settles on) per new stat, so the frontend can show a trend line
  instead of one number — this is the one genuinely new piece of kernel
  state this phase introduces, and should be scoped as small and fixed-
  size as the rest of this codebase's own conventions (cf. `TCP_MAX_CONNS`-
  style fixed arrays, not dynamic allocation).

**Verification:** host test asserting the rolling-average math and ring
buffer behave correctly under synthetic load; compile-check; live pass
confirming `/api/metrics`' new fields move under real load (e.g. spin up
several processes, insert several vectors, and watch CPU/RAM/disk numbers
respond).

---

## Phase 3 — Group profiles, authorization lists, security audit log

**Goal:** move security administration from "4 fixed roles" to something
an operator can actually shape, and give Security Dashboard a real feed to
read from instead of Phase 1's stopgap.

**What already exists and is reusable:** `object_catalog.c`'s
`catalog_check_access()` and `role_table[]` (`object_catalog.h:219`,
`SYS_SLS_ROLE_SET` at `object_catalog.h:201`) are the real enforcement
point every new grouping concept needs to plug into, not bypass. `auth.c`'s
per-account model (`LeaseToken`) is the natural home for group membership.

**What needs to be built:**
- **Group profiles:** a new small fixed-size table (matching
  `role_table[ROLE_TABLE_MAX]`'s own sizing convention) mapping a group
  name to a set of member uids, with `catalog_check_access()` extended to
  check group-derived permissions in addition to the existing per-uid role
  check — additive, not a replacement for the existing 4-role model (which
  stays as the system-level baseline).
- **Authorization lists:** a named list of `{object_name, perm_mask}`
  pairs plus a set of grantee uids/groups — the IBM i pattern of granting
  one list to many objects at once rather than repeating per-object grants.
  Smallest real version: a new syscall pair (`SYS_SLS_AUTHLIST_CREATE`/
  `_GRANT`/`_CHECK`, next free numbers after Phase 2's own additions) plus
  a `catalog_check_access()` fallback path that checks list membership
  after the direct per-object/role checks fail.
- **Security audit log:** a new fixed-size ring buffer (same sizing
  posture as `AUTH_MAX_TOKENS`) recording auth failures, role changes, and
  access denials, with a `GET /api/security/audit` route — this is what
  `SlsSecurityDashboard.tsx`'s `securityLogs` should actually read from
  once it exists, closing the loop Phase 1 could only partially close.

**Verification:** host test covering group-derived access grants,
authorization-list grant/check/revoke, and audit-log entries being written
on real deny events; compile-check; live pass creating a group, granting
an authorization list, and confirming both the Terminal and
`SlsSecurityDashboard.tsx` reflect it.

---

## Phase 4 — Job priority/hold-release + message queue surface

**Goal:** give Work Management real per-job control instead of only
partition-level pause and unconditional kill.

**What already exists and is reusable:** `struct ProcessDescriptor`
(`process.h:29`) and its `ProcState` enum are the right extension point —
adding states/fields here is additive, matching how `partition_id` was
added to this same struct in an earlier phase (`process.h:41`'s own
comment names that precedent). `kernel/ipc.h`'s fixed-port bus is the
right foundation for named queues, not a replacement.

**What needs to be built:**
- **Job priority:** a `priority` field on `ProcessDescriptor`, consulted by
  `schedule_ring3()` (`process.c`) alongside its existing round-robin scan
  — smallest real version is a coarse 3-tier (high/normal/low) scheme, not
  a full weighted scheduler, matching this codebase's consistent "smallest
  real version first" scoping pattern.
- **Job HOLD/RELEASE:** extend `PROC_SUSPENDED` handling (already exists as
  a state) with real syscalls to set it per-pid (`SYS_SLS_PROC_HOLD`/
  `_RELEASE`) rather than only reachable via the coarser partition-level
  pause/resume — `schedule_ring3()` already must skip non-`PROC_RUNNING`
  processes, so this is exposing an existing state transition, not adding
  a new one.
- **Message queues:** a small fixed table of named queues (bounded count,
  bounded per-queue depth, matching every other fixed-size table
  convention in this codebase) layered on top of the existing IPC port
  bus, with `mq create/send/receive/list` syscalls + Terminal commands and
  an HTTP route for listing queue depth/contents — the minimum needed to
  make queues *visible*, which is the actual Navigator-parity gap (today's
  IPC bus has no user-facing view at all).

**Verification:** host test for priority ordering, hold/release state
transitions, and queue create/send/receive/list; compile-check; live pass
via Terminal and (if built in this phase) a Work Management panel.

---

## Phase 5 — Configurable network interfaces + storage-unit visibility

**Goal:** this category is the largest gap in kind (compiled-in constants
vs. administrable objects), so this phase is scoped deliberately light —
visibility and a small amount of runtime configuration, not a full network
administration subsystem.

**What already exists and is reusable:** `net/dhcp.c`'s real DHCP client
already negotiates and holds an assigned IP/gateway at runtime (confirmed
live during Vector Store Phase 4 verification: `10.0.2.15 gw 10.0.2.2`) —
that negotiated state is exactly what a "network line status" view should
surface, it just isn't exposed anywhere today. `/api/tiers` is the existing
precedent for a storage-facing status route.

**What needs to be built:**
- **Network line/interface status (read-only first):** a new `GET
  /api/network/status` surfacing the e1000 interface's negotiated
  IP/gateway/subnet from `dhcp.c`'s own state, plus basic TCP connection
  pool utilization (`tcp_conns[]`/`TCP_MAX_CONNS` — already a real, sized
  resource this phase can report on directly, no new tracking needed).
- **Storage/disk-unit visibility (read-only first):** a `GET /api/disk`
  route surfacing whatever capacity data Phase 2's NVMe Identify-Namespace
  work captured, plus per-tier capacity from the existing storage-tier
  system.
- **Deliberately out of scope for v1:** runtime-configurable IP/DNS/routing
  and true multi-disk/RAID management — these would require rearchitecting
  the network/storage stacks to read config from a mutable source rather
  than compile-time constants and boot-time DHCP negotiation, a
  substantially larger effort than this roadmap's other phases and better
  scoped as its own follow-on doc once the read-only visibility here proves
  useful.

**Verification:** compile-check; live pass confirming `/api/network/status`
matches the kernel's own boot-log-reported DHCP lease and
`/api/disk` matches real NVMe capacity once Phase 2 captures it.

---

## Suggested sequencing

1. **Phase 1** first — no new kernel work, closes the worst problem (a UI
   that actively misrepresents kernel state) using data that already
   exists, and gives every later phase's own new data a place to land in
   the frontend without a second frontend-wiring effort later.
2. **Phase 2** next — real performance data is the most broadly useful
   addition and has no dependency on the security/work-management phases.
3. **Phase 3** — independent of Phase 2, could run in parallel; sequenced
   third because it's the next-highest-value gap (security administration
   depth) and directly gives Phase 1's Security Dashboard stopgap a real
   feed to graduate to.
4. **Phase 4** — independent of Phases 2-3; sequenced fourth since job-
   level control matters most once there's more than a couple of demo
   processes running, which is more likely once the other phases exist.
5. **Phase 5** last, deliberately scoped as read-only visibility rather
   than full configuration, both because it depends on Phase 2's disk-
   capacity work and because true network/storage administration is a
   substantially larger, separate effort best scoped on its own once
   there's a real need for it.
