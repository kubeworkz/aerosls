# AeroSLS Operational MVP Roadmap v0.1

## 0. Why this doc exists

The original gap analysis named five operational gaps, distinct from the
architectural ones already closed (see
`docs/AeroSLS-Architectural-MVP-Roadmap-v0.1.md`): no CI/CD, no monitoring,
no test automation, no backup/restore, and a stale reverse-proxy allowlist.
This doc scopes each one against what's actually in the repo today.

Findings that shape the plan:

- `tests/` has 24 host-buildable test files (`auth_host_test.c`,
  `mvcc_host_test.c`, `sql_exec_host_test.c`, `vec_index_host_test.c`, etc.),
  each with its own hand-written `gcc ... && /tmp/foo` command in a header
  comment. Nothing runs them together, and nothing runs them automatically —
  every verification pass this project has done (including every phase of
  the architectural roadmap) has been a person or agent manually invoking
  `gcc -fsyntax-only` and individual test binaries by hand.
- There is no `.github/workflows/` directory or any other CI config anywhere
  in the repo.
- `POST /api/health` (`net/http.c`) already returns real, live data
  (`uptime_ticks`, `object_count`), not just a static "ok" — a reasonable
  foundation. The gap is that nothing external polls it; the only way
  anyone's found out the kernel was unhealthy so far has been a user
  noticing the site was broken.
- Deploy today is: SSH in, `make bundle`, `make x86-iso`, restart the
  process by hand. This is exactly what caused this session's own incident
  — "restarting the services" restarted a stale binary because the rebuild
  step was skipped, and nothing caught that before it reached users.
- No backup or snapshot mechanism exists for `sls_storage.img` (the kernel's
  persistent NVMe-backed disk image) — confirmed via repo-wide search for
  `*backup*`/`*persist*` scripts, which only turned up `kernel/persist.c`
  (the in-kernel persistence *engine*, not an operational backup of its
  output file).
- `slsos-sim/server.ts`'s `kernelProxy` `pathFilter` is a hardcoded array of
  ~20 path prefixes, missing many routes added since it was written
  (`/api/sql`, `/api/schema`, `/api/vec/*`, `/api/partitions`,
  `/api/journal*`, `/api/cursor/*`, `/api/simi/*`, `/api/shell/exec`,
  confirmed via direct comparison against the array). Now lower-severity
  than originally flagged, since Architectural Phase 5 confirmed the kernel
  serves the SPA + `/api/*` directly on :3001 in production — nginx/Caddy
  point there, not through this proxy — but it still governs local dev
  (`npm run dev`), where this exact staleness has already caused real
  confusion once this session (the `/api/tables` 404 investigation, before
  the root cause turned out to be a stale kernel binary).

---

## Phase A — Consolidated test runner + CI — DONE

Implemented: `tests/run_all.sh` extracts each test's own documented build
command from its "Build and run:" header comment (rather than hardcoding a
second copy that could drift — and it already caught real drift once: see
below) and runs all 24 as one command. `tools/syntax_check_all.sh` extracts
the Makefile's real `X86_C_SRC` file list via `make -pn` (also not
hardcoded, for the same reason) and runs `gcc -fsyntax-only` with the same
freestanding flags every manual check this whole project has done used.
`slsos-sim/package.json` gained a `test` script running the existing
`shellCommands.test.ts` host test (previously only ever run by hand).
`.github/workflows/ci.yml` added in both `aerosls2` and `slsos-sim` (two
separate repos, two separate workflows — confirmed via `git remote -v`
in each).

**Caught a real bug immediately:** `tests/auth_host_test.c`'s own build
command was missing `kernel/secure_api.c` — added when Architectural Phase 4
started calling `derive_user_key()`, but the file's header comment was never
updated to match, since that verification was done by hand at the time.
`run_all.sh` failed on its very first real run because of this. Fixed in the
same pass. This is exactly the class of bug this phase exists to catch
automatically instead of by luck.

Verified: `tests/run_all.sh` → 24/24 passed (894 individual checks across
all files). `tools/syntax_check_all.sh` → 76/76 files clean. `npm run test`
(slsos-sim) → 48/48 passed. Both CI workflow YAML files parse cleanly.

## Phase B — Scripted deploy with health-check verification — DONE

Implemented: `deploy/deploy.sh`. Confirmed with Dave that pm2 manages the
kernel process on the server (local dev is unaffected — `make x86-run`
there is unchanged). The script: pulls both `aerosls2` and the sibling
`../slsos-sim` repo (two separate git repos, both need to be current for a
real deploy — easy to miss by hand), explicitly builds the frontend with
its own checked exit code, runs `make bundle && make x86-iso` (aborting
before touching the running process if either build step fails), restarts
the pm2 process, then polls `GET http://localhost:3001/api/health` directly
(bypassing nginx/Cloudflare deliberately — this script verifies the kernel
itself, not the whole public chain) with retries before declaring success.
On any failure it aborts loudly rather than silently leaving whatever state
a partial deploy produced, and a failed health check dumps the last 40 lines
of pm2's logs for immediate diagnosis.

**Related finding, not fixed here:** the Makefile's own `bundle` target
runs the frontend build with `|| true`, silently swallowing a build failure
and re-bundling whatever old `slsos-sim/dist/` happens to be on disk. Not
fixed in this pass since it changes behavior for every existing caller of
`make bundle` (including local dev) — a bigger blast radius than this one
new deploy script justifies. `deploy.sh` works around it by doing its own
properly-checked frontend build *before* calling `make bundle`, so this
script specifically can't ship a stale bundle even though the Makefile
target it calls still could if invoked some other way. Worth fixing in the
Makefile directly as a small separate follow-up.

Verified: `bash -n deploy/deploy.sh` (syntax clean); the git-pull/build/
health-check control flow was reasoned through step by step, but the
end-to-end run against the real pm2 process and live kernel could only
happen on the actual server, outside what's reachable from here — Dave
should do a first supervised run rather than trust this blind.

---

## Phase C — External health monitoring — DONE

Implemented: `monitor/health_check.sh` + `monitor/README.md`. Meant to run
via cron against the **public** URL (`https://aerosls.kubeworkz.io/api/health`)
— a deliberately different check from `deploy.sh`'s own health poll, which
hits the kernel directly to verify one specific deploy in isolation; this
one verifies the whole chain (nginx, Cloudflare, kernel) is actually up for
a real user, on an ongoing basis. It does the one thing a plain uptime
monitor won't: tracks `uptime_ticks` across runs in a small state file and
alerts if it hasn't moved, catching a wedged-but-still-answering process
that a bare "did I get a 200" check can't distinguish from healthy. A drop
in `uptime_ticks` (a legitimate restart) deliberately does *not* alert —
only a value that's identical to last time does. Optional
`ALERT_WEBHOOK_URL` posts to Slack/Discord/etc.; without it, cron's own
default mailto-on-failure behavior is the fallback (worth confirming
outbound mail actually works on this server, not assumed).

Tested against a local mock server, matching the real `jb_uint()` output
format exactly (compact JSON, no space after `:` — read directly from
`net/http.c`, not assumed) across four scenarios: unreachable, stuck
`uptime_ticks`, recovering `uptime_ticks`, and first-run-with-no-prior-state.
All four produced the correct alert/exit-code behavior.

Also recommended (not something to sign up for on Dave's behalf — that's
his step): a free external uptime monitor (UptimeRobot or similar) as a
complementary, simpler, *off-server* check, since a script running on the
same server obviously can't detect that the server or its network path
itself is down. Exact suggested config in `monitor/README.md`.

**Log rotation:** the kernel's serial debug log (`-serial
file:sls_kernel_debug.log` in the Makefile's `x86-run` target) grows
forever with no rotation, and the real production launch command (whatever
pm2 actually runs) isn't in either repo, so the true log path needs
confirming rather than assumed. `monitor/README.md` gives both `pm2 install
pm2-logrotate` (for whatever pm2 itself captures) and a `copytruncate`-based
system `logrotate` template (for the QEMU-written file directly, if that's
what's actually in play) — `copytruncate` specifically because QEMU holds
the file open continuously, so a rename-based rotation would leave it
writing into an now-invisible file instead of a fresh one.

## Phase D — Backup/restore for `sls_storage.img` — DONE

Implemented: `backup/backup.sh` + `backup/restore.sh` + `backup/README.md`.
No filesystem-level snapshot capability known to be available on the host
(no LVM/ZFS/btrfs assumed), so both scripts use the same brief-downtime
approach `deploy/deploy.sh` already established: `pm2 stop` -> `cp
--sparse=always` -> `pm2 start` -> poll `/api/health`, trading a few seconds
of downtime for a copy that's guaranteed consistent rather than risking a
torn read of a raw disk image mid-write.

`backup.sh` takes `BACKUP_KIND=hourly|daily` explicitly (two separate cron
entries, not auto-detected from time of day — that would be brittle against
exactly when cron fires) and prunes each tier to a configured retention
count (`KEEP_HOURLY=4`, `KEEP_DAILY=7` by default) after every successful
run. `restore.sh` requires typing `yes` to confirm, always takes a
timestamped safety copy of the *current* image before touching anything
else — so a wrong call or a bug in the script itself can't destroy state
that was still recoverable — and finishes with a `GET /api/tables`
spot-check (using Dave's fixed demo bearer token by default) so a restore
is confirmed against real data, not just a 200 status.

**Verified:** both scripts pass `bash -n`. Functionally smoke-tested
end-to-end against a throwaway repo copy with fake `pm2`/`curl` shims
(real pm2/a live kernel weren't reachable from this sandbox — same caveat
as `deploy.sh`, do a first supervised run on the real server):
`backup.sh`'s retention pruning was confirmed to keep exactly the
configured count across both kinds in separate directories, and both its
input-validation guards (bad `BACKUP_KIND`, missing `STORAGE_IMG`) and its
`pm2 stop`-failure abort path were confirmed to leave no partial backup
behind. `restore.sh` was confirmed to leave `sls_storage.img` and the
safety-copy state completely untouched when the confirmation prompt is
declined or `pm2 stop` fails, and a full confirmed run was confirmed to
produce a restored image that exactly matches the chosen backup, a safety
copy that exactly matches the pre-restore state, and a working
`/api/tables` spot-check.

## Phase E — Fix the stale `server.ts` proxy allowlist — DONE

**Goal:** local dev (`npm run dev`) stops silently missing routes added
kernel-side after the allowlist was written — the same class of bug that
caused the `/api/tables` confusion earlier this session, even though that
particular case turned out to have a different root cause.

Implemented: `slsos-sim/server.ts`'s `kernelProxy` `pathFilter` switched from
a hardcoded ~20-entry array to a prefix-matching function. The original plan
here was the simpler `pathFilter: (path) => path.startsWith("/api/") ||
path.startsWith("/auth/")` — investigating the actual file before
implementing turned up a real problem with that: `server.ts` also defines
its own local-only Express routes under `/api/health`, `/api/ai/*`, and
`/api/v1/*` (AI generation, the dev sync/memory REST API), registered
*after* `app.use(kernelProxy)` in the middleware chain. A blanket `/api/`
prefix match would have proxied all of those straight to the kernel before
they ever reached their real handlers — breaking AI generation, sync, and
the memory REST API in local dev, the exact kind of silent breakage this
phase exists to prevent, just moved to a different set of routes. Fixed
version explicitly excludes those three local-only prefixes
(`isLocalOnlyApiPath()`) and prefix-matches everything else under `/api/`
and `/auth/`, so new kernel routes are picked up automatically without
resurrecting the local-route conflict.

**Verified:** `npm run lint` (`tsc --noEmit`) clean. Wrote a standalone
25-case functional test of the filter logic (compiled and run with `tsc`+
`node`, mirroring `npm run test`'s own approach): confirmed all three
local-only route families stay unproxied (including sub-paths like
`/api/v1/memory/hexdump`), all of the old array's entries are still
proxied, all of the routes that were actually missing from the old array
(`/api/sql`, `/api/schema`, `/api/vec/*`, `/api/partitions`,
`/api/journal*`, `/api/cursor/*`, `/api/simi/*`) are now proxied without
any further code changes, a hypothetical brand-new future kernel route is
proxied automatically, and non-API paths (SPA routes, static assets) are
correctly left alone. All 25 cases passed. Also re-ran `npm run test`
(the existing 48-check shell-command suite) as a regression check — still
48/48.

Lower priority than Phases A-D, as originally scoped: doesn't affect
production traffic (Architectural Phase 5 confirmed nginx/Cloudflare route
straight to the kernel, bypassing this proxy entirely), only local
development experience.

---

## Suggested sequencing

1. Phase A (CI) first — every subsequent phase's changes should land under
   a CI gate, not before one exists.
2. Phase B (scripted deploy) next — directly closes an incident that's
   already happened once.
3. Phase C (monitoring) and Phase E (proxy allowlist) — small, independent,
   can be done in parallel with anything else.
4. Phase D (backup/restore) — independent of the others; sequence based on
   how much real user data exists yet (more urgent once real users are
   storing real data than while this is still pre-launch).
