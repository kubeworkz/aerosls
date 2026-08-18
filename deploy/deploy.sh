#!/usr/bin/env bash
# deploy/deploy.sh -- Operational Phase B
# (docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md).
#
# Replaces "SSH in, make bundle, make x86-iso, restart by hand" -- exactly
# the sequence that produced this project's own real incident (surfaced
# during Architectural Phase 1/2's own investigation): "restarting the
# services" restarted a STALE binary because the rebuild step got silently
# skipped, and nothing caught that before a user did. This script can't
# skip the rebuild step, and it verifies the result via /api/health before
# declaring success instead of trusting that a restarted process is a
# working one.
#
# Run this ON THE SERVER (aerosls.kubeworkz.io), from inside the aerosls2
# checkout, where pm2 manages the kernel process (confirmed with Dave --
# local dev is unaffected, `make x86-run` there is unchanged).
#
# Assumes the kernel's build dependencies (../qemu) are checked out as
# sibling directories. The webapp bundle does NOT need ../slsos-sim: it is
# committed as kernel/webapp_bundle.c and built from the committed file (see
# the bundle note further down).
set -u

# Confirm/override with `pm2 list` -- this script doesn't know your actual
# process name, only that pm2 is what manages it.
PM2_APP_NAME="${PM2_APP_NAME:-aerosls-kernel}"
# Hits the kernel directly, not through nginx/Cloudflare -- this script is
# verifying the KERNEL came up correctly, not the whole public chain, which
# has its own separate failure modes this isn't trying to catch.
HEALTH_URL="${HEALTH_URL:-http://localhost:3001/api/health}"
# 30 x 2s = 60s: the window must cover old-process port release PLUS a
# fresh TCG boot of the new kernel. The pre-fix check "succeeded" in the
# first poll because it accepted the dying process -- the honest wait is
# longer than that.
HEALTH_RETRIES="${HEALTH_RETRIES:-30}"
HEALTH_RETRY_DELAY_SECS="${HEALTH_RETRY_DELAY_SECS:-2}"

cd "$(dirname "$0")/.."   # aerosls2 repo root

# ─── This script pulls the repo that contains this script ─────────────────
# So a change to deploy.sh itself does not take effect until the NEXT run --
# the current instance was loaded before the pull. Worse, bash reads a script
# incrementally by byte offset, so a file that changes underneath a running
# shell can resume at the wrong place in the new text.
#
# That is not hypothetical: a fixed deploy.sh was pulled and the run that
# pulled it still reported the OLD version's error message, which looked
# exactly like the fix not having been made at all. Diagnosing that cost a
# round trip, on top of the four already spent on stale binaries.
#
# So: hash before, hash after, re-exec if it changed. SELF_REEXEC guards
# against a loop if something makes the hash unstable.
_self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
_hash_before=""
command -v md5sum >/dev/null && _hash_before="$(md5sum "$_self" 2>/dev/null | cut -d' ' -f1)"

# ─── The tracked bundle must be clean BEFORE the pull ──────────────────────
# Older deploys regenerated kernel/webapp_bundle.c from the server's local
# dist and left the tracked file DIRTY (the 2026-08-18 incident). The next
# pull then refuses to merge over it with a bare "local changes would be
# overwritten", which looks like a network failure. The committed file is
# canonical -- a dirty bundle is drift by definition, so discard it here,
# deliberately, before git has a chance to refuse.
if ! git diff --quiet -- kernel/webapp_bundle.c; then
    echo "[deploy] NOTE: kernel/webapp_bundle.c is locally modified (a previous deploy's"
    echo "         regeneration). The committed file is canonical -- discarding the"
    echo "         local copy before pulling."
    git checkout -- kernel/webapp_bundle.c
fi

echo "[deploy] Pulling latest aerosls2..."
if ! git pull; then
    echo "[deploy] FAILED: git pull (aerosls2) failed. Aborting -- nothing rebuilt or restarted."
    exit 1
fi

if [ -z "${SELF_REEXEC:-}" ] && [ -n "$_hash_before" ]; then
    _hash_after="$(md5sum "$_self" 2>/dev/null | cut -d' ' -f1)"
    if [ -n "$_hash_after" ] && [ "$_hash_after" != "$_hash_before" ]; then
        echo "[deploy] deploy.sh changed in that pull -- re-executing the new version"
        echo "         rather than continuing with the copy bash already read."
        SELF_REEXEC=1 exec bash "$_self" "$@"
    fi
fi

# ─── The webapp bundle is COMMITTED, and this deploy builds from it ────────
# kernel/webapp_bundle.c (the compiled-in Navigator SPA) is a TRACKED file
# in X86_C_SRC. The committed file is canonical -- CI builds the kernel from
# it with no ../slsos-sim present at all, so a deploy that regenerated it
# from the server's local dist produced a DIFFERENT kernel than CI for the
# same commit. That happened on 2026-08-18: deploy.sh pulled the unpinned
# sibling ../slsos-sim, ran `npm ci && npm run build`, then `make bundle`
# overwrote the tracked file from the server's 942 KiB dist while the repo's
# committed bundle was 870 KiB -- the server served a webapp that did not
# exist in the repo, and every deploy dirtied the working tree.
#
# The webapp now flows through git like any other source: change slsos-sim,
# run `make bundle`, COMMIT the regenerated kernel/webapp_bundle.c, then
# deploy. tests/webapp_bundle_guard_check.sh (part of the guard gate below)
# proves the committed file matches the recorded slsos-sim revision, so a
# webapp advance that was not regenerated and committed fails the deploy
# instead of silently drifting. This script deliberately does NOT touch
# slsos-sim: no git pull, no npm, no make bundle.

# ─── ../qemu: pulled because the kernel is BUILT from it ───────────────────
# The Makefile links 15 objects out of ../qemu (tcg/, accel/tcg/ and all of
# sls/ -- see TCG_OBJS and the VPATH line). This script pulls aerosls2 and
# ../qemu and builds from two repositories, so every change under
# qemu/sls/ silently deployed as whatever happened to be on the server's
# disk.
#
# That is not theoretical. In one session it produced four separate runs
# against a binary nobody intended: a banner still reading "(TCI)" after the
# string was changed, missing EXEC/TRANSLATE lines that had definitely been
# added, and finally a partial state where sls-launcher.c's ARENA reporting
# was present while its ALLOC reporting and the sls-runtime.c counters it
# calls were both absent. Each one cost a full diagnostic round spent looking
# for a bug in code that was not running.
#
# Not fatal if the repo is missing entirely -- some checkouts build against a
# vendored copy -- but a pull that FAILS is fatal, because that is the case
# where a stale tree gets linked while looking like it succeeded.
if [ -d ../qemu/.git ]; then
    echo "[deploy] Pulling latest qemu (kernel links 15 objects from it)..."
    if ! (cd ../qemu && git pull); then
        echo "[deploy] FAILED: git pull (qemu) failed. Aborting -- the kernel links"
        echo "         TCG and sls/ objects from ../qemu, so building now would ship"
        echo "         a mix of new aerosls2 code against a stale QEMU tree."
        exit 1
    fi
elif [ -d ../qemu ]; then
    echo "[deploy] NOTE: ../qemu exists but is not a git checkout -- not pulled."
    echo "         Changes under qemu/sls/ must be copied there by hand, and the"
    echo "         post-build assertion below is the only thing that will notice"
    echo "         if they were not."
else
    echo "[deploy] FAILED: ../qemu not found. The kernel cannot link without it."
    exit 1
fi

echo "[deploy] Building kernel ISO (make x86-iso)..."
if ! make x86-iso; then
    echo "[deploy] FAILED: make x86-iso failed. Aborting -- kernel NOT restarted, still running the previous build."
    exit 1
fi

# ─── Post-build assertion: is the image NEWER than every source it needs? ──
# `make` succeeding proves the compiler ran. It does not prove it ran over the
# sources you think are on disk -- a repo that was never pulled, a partial
# sync, or a dependency rule that missed a header all produce a clean build of
# the wrong program. That happened four times in one session: a banner still
# reading "(TCI)" after the string had been changed, missing report lines that
# had definitely been added, and a half-synced sls/ where one file's changes
# were present and another's were not.
#
# ─── Why mtime and not string probing ──────────────────────────────────────
# The first version of this check extracted a string literal from each source
# and looked for it in the binary. It was wrong twice, for two unrelated
# reasons, and the second one blocked a perfectly good deploy:
#
#   1. It drew probes out of COMMENTS, which never reach the binary.
#   2. After that was fixed it matched the GAP BETWEEN two string literals --
#      from the closing quote of one, through the intervening code, to the
#      opening quote of the next -- yielding "probes" like
#          " : (p == end && end > sp) ? "
#      that are C code, not data, and are correctly absent from the image.
#
# A regular expression cannot tell an opening quote from a closing one; that
# needs a tokeniser. Two failures from two unrelated causes says the approach
# was wrong, not the pattern -- so this checks a property that requires no
# parsing at all: NOTHING THE KERNEL IS BUILT FROM MAY BE NEWER THAN THE
# KERNEL. git pull sets mtimes to checkout time, so a pulled-but-not-rebuilt
# file is caught exactly.
echo "[deploy] Verifying the built image is newer than every source..."

KERNEL_BIN="my_sls_kernel.bin"
[ -f "$KERNEL_BIN" ] || { echo "[deploy] FAILED: $KERNEL_BIN missing after a successful make."; exit 1; }

SRC_ROOTS=""
for d in kernel arch net user ../qemu/sls ../qemu/tcg ../qemu/accel/tcg ../qemu/include; do
    [ -d "$d" ] && SRC_ROOTS="$SRC_ROOTS $d"
done
[ -n "$SRC_ROOTS" ] || { echo "[deploy] FAILED: no source directories found; cannot verify."; exit 1; }

# -newer is strictly greater, so a source touched in the same second as the
# link does not trip it. That is the right way round: this must not cry wolf.
STALE="$(find $SRC_ROOTS \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name '*.asm' -o -name '*.S' \) \
         -newer "$KERNEL_BIN" 2>/dev/null | head -20)"

if [ -n "$STALE" ]; then
    echo "[deploy] FAILED: these sources are NEWER than $KERNEL_BIN, so the image"
    echo "         was not built from them:"
    echo "$STALE" | sed 's/^/           /'
    echo "         The build did not pick them up -- usually a dependency rule that"
    echo "         does not track headers across the ../qemu boundary."
    echo "         Aborting: kernel NOT restarted, still running the previous build."
    echo "         Try 'make clean && make x86-iso'."
    exit 1
fi

CHECKED="$(find $SRC_ROOTS \( -name '*.c' -o -name '*.h' -o -name '*.inc' -o -name '*.asm' -o -name '*.S' \) 2>/dev/null | wc -l)"
[ "$CHECKED" -gt 0 ] || { echo "[deploy] FAILED: found 0 source files to check. A check that examined nothing must not pass."; exit 1; }
echo "[deploy] OK: $CHECKED source file(s) all older than $KERNEL_BIN."

# ─── The *_check.sh guards + *_smoke.sh teeth, as a hard gate ─────────────
# Five guards live in tests/, each written after the bug it catches had
# already happened and cost a diagnostic round:
#
#   stack_frame_budget_check.sh   a frame that does not fit the stack that
#                                 exists -- 276 KB against 64 KiB, silent for
#                                 months while it overwrote .bss
#   kernel_image_end_check.sh     .bootstrap_stack orphaned above
#                                 _kernel_image_end, so the frame allocator
#                                 handed out live kernel stack
#   no_tls_relocations_check.sh   TLS relocations in a kernel that has no TLS
#   makefile_sources_check.sh     X86_C_SRC drifting from what is on disk
#   commands_doc_check.sh         COMMANDS.md drifting from the real commands
#
# Until recently nothing ran them: tests/run_all.sh globs *_host_test.c and
# these are shell scripts, so they passed review and inspected nothing.
#
# The *_smoke.sh teeth run right after, via tests/run_guard_smokes.sh: each
# smoke deliberately breaks a guard's input and asserts the guard fails. A
# guard that passes is only half the story — a guard that has gone blind
# passes while inspecting nothing. The three kernel-needing smokes
# (stack_frame_budget, kernel_image_end, no_tls_relocations) can only run
# where the build exists, which is exactly what this gate is.
#
# ─── Why here, and why --require-all ───────────────────────────────────────
# Here, because it is after the build and after the staleness assertion above
# -- so the linked kernel and its objects exist and the guards have something
# real to read -- and before the pm2 restart, so a violation stops the deploy
# instead of shipping. That is the same shape as every other failure in this
# script: abort with the previous build still serving.
#
# --require-all, because on a build host a missing prerequisite is not a
# neutral "nothing to check". The build just succeeded; if a guard cannot
# find the binary or its objects, the build did not produce what it claimed
# and that is itself the finding. "Could not check" must not resemble "it is
# fine" on the path that puts code in front of users. The smokes apply the
# same discipline unconditionally: a smoke that cannot run proves nothing
# and counts as a failure.
#
# Invoked via `bash` rather than executed directly: every tracked *.sh now
# carries the 100755 bit (the repo-wide +x sweep), but calling through bash
# keeps the gate portable across checkouts and filesystems, and CI calls
# run_all.sh the same way.
echo "[deploy] Running guard scripts (tests/run_checks.sh --require-all)..."
if ! bash tests/run_checks.sh --require-all; then
    echo "[deploy] FAILED: a guard script failed against the build just produced."
    echo "         Aborting -- kernel NOT restarted, still running the previous build."
    echo "         These are not style checks. Each one exists because the property"
    echo "         it asserts was violated in shipped code and cost a diagnostic"
    echo "         round to find. Read the output above before overriding anything."
    exit 1
fi

echo "[deploy] Running guard smokes (tests/run_guard_smokes.sh)..."
if ! bash tests/run_guard_smokes.sh; then
    echo "[deploy] FAILED: a guard smoke failed against the build just produced."
    echo "         Aborting -- kernel NOT restarted, still running the previous build."
    echo "         A smoke proves a guard can FAIL, not just pass. A smoke that"
    echo "         cannot run proves nothing and is treated as a failure here."
    exit 1
fi

# ─── Who answers health, and how the restart is verified ───────────────────
# /api/health is served by whatever QEMU holds the forwarded port, so a
# health answer is only meaningful if it comes from the process this
# restart just started. A plain "did anything answer?" loop is satisfied by
# the process being REPLACED: pm2 kills the old QEMU and spawns a new one,
# and if the old one still holds the port while the new one cannot bind
# ("Could not set up host forwarding rule 'tcp::3001-:3000'"), the old
# process keeps answering with a climbing uptime while pm2 crash-loops and
# eventually gives up (status: errored). That is not theoretical: on
# 2026-08-13 a deploy printed "[deploy] Deploy succeeded." while no new
# QEMU was serving at all -- the old process had satisfied the health wait
# during the restart race.
#
# So the pre-restart state is captured first -- the listener PID on the
# health port AND the kernel's own uptime counter -- and the post-restart
# loop accepts an answer only when BOTH prove it is a different, freshly
# booted process. Both identifiers are RELATIONAL (no absolute thresholds):
# the new listener must not be the old PID, and the answering kernel's
# uptime must be younger than the one being replaced. When the service was
# already down before the deploy (no old PID / uptime to compare against),
# any answering listener is accepted -- the deploy is the only thing that
# could have started it.
HEALTH_PORT="$(printf '%s' "$HEALTH_URL" | sed -nE 's#.*:([0-9]+)/.*#\1#p')"
[ -n "$HEALTH_PORT" ] || HEALTH_PORT="3001"

echo "[deploy] Recording pre-restart state (who answers $HEALTH_URL)..."
OLD_PID="$(ss -ltnp 2>/dev/null | awk -v p=":$HEALTH_PORT" 'index($4, p) {print $NF}' | sed 's/.*pid=\([0-9]*\).*/\1/' | head -1)"
OLD_UPTIME="$(curl -sf --max-time 3 "$HEALTH_URL" 2>/dev/null | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("uptime_ticks",""))
except Exception: print("")' 2>/dev/null)"
echo "[deploy]   pre-restart listener pid: ${OLD_PID:-<none>}, kernel uptime_ticks: ${OLD_UPTIME:-<none>}"

echo "[deploy] Restarting pm2 process '$PM2_APP_NAME'..."
if ! pm2 restart "$PM2_APP_NAME"; then
    echo "[deploy] FAILED: pm2 restart failed. Check 'pm2 list' -- is PM2_APP_NAME=$PM2_APP_NAME the right process name?"
    exit 1
fi

echo "[deploy] Waiting for $HEALTH_URL to answer from the NEW process (up to $((HEALTH_RETRIES * HEALTH_RETRY_DELAY_SECS))s)..."
healthy=0
health_tmp="$(mktemp)"
last_pid=""
last_uptime=""
for i in $(seq 1 "$HEALTH_RETRIES"); do
    if curl -sf --max-time 3 "$HEALTH_URL" > "$health_tmp" 2>/dev/null; then
        last_pid="$(ss -ltnp 2>/dev/null | awk -v p=":$HEALTH_PORT" 'index($4, p) {print $NF}' | sed 's/.*pid=\([0-9]*\).*/\1/' | head -1)"
        last_uptime="$(python3 -c 'import json,sys
try: print(json.load(open(sys.argv[1])).get("uptime_ticks",""))
except Exception: print("")' "$health_tmp" 2>/dev/null)"
        # The answer must come from a DIFFERENT process than the one being
        # replaced, and its kernel must be younger. An answer from the old
        # process is exactly the failure this check exists to catch -- keep
        # waiting (or time out below).
        if [ -n "$last_pid" ] \
           && { [ -z "$OLD_PID" ] || [ "$last_pid" != "$OLD_PID" ]; } \
           && { [ -z "$OLD_UPTIME" ] || { [ -n "$last_uptime" ] && [ "$last_uptime" -lt "$OLD_UPTIME" ]; }; }; then
            healthy=1
            break
        fi
    fi
    sleep "$HEALTH_RETRY_DELAY_SECS"
done

if [ "$healthy" -ne 1 ]; then
    echo "[deploy] FAILED: $HEALTH_URL did not answer from the NEW process within the window."
    if [ -n "$last_pid" ]; then
        echo "[deploy]   Last answer came from pid $last_pid (uptime_ticks ${last_uptime:-?})."
        if [ "$last_pid" = "$OLD_PID" ]; then
            echo "[deploy]   That is the SAME process that was being replaced -- the restart"
            echo "[deploy]   race: the old process kept the port while the new one could not"
            echo "[deploy]   bind. Check the error log for 'Could not set up host forwarding rule'."
        elif [ -n "$last_uptime" ] && [ -n "$OLD_UPTIME" ] && [ "$last_uptime" -ge "$OLD_UPTIME" ]; then
            echo "[deploy]   Its uptime ($last_uptime) is not younger than the pre-restart"
            echo "[deploy]   kernel's ($OLD_UPTIME) -- it is not the freshly booted process."
        fi
    else
        echo "[deploy]   Nothing answered at all."
    fi
    echo "[deploy] Last pm2 logs for '$PM2_APP_NAME':"
    pm2 logs "$PM2_APP_NAME" --lines 40 --nostream
    echo "[deploy] The process was restarted but is NOT confirmed healthy -- do not assume this deploy succeeded."
    rm -f "$health_tmp"
    exit 1
fi

echo "[deploy] Healthy (listener pid ${last_pid:-?}, uptime_ticks ${last_uptime:-?}):"
cat "$health_tmp"
echo ""
rm -f "$health_tmp"

# ─── The NEW kernel must serve exactly the committed webapp bundle ─────────
# The health wait proves a fresh process answers /api/health; this proves
# what that process SERVES. The webapp is compiled in from the committed
# kernel/webapp_bundle.c, so drift here means the kernel's HTTP layer is not
# serving the bundle it was built with (or a future deploy regressed to
# regenerating the file) -- the 2026-08-18 failure mode, on the served side.
# tests/webapp_served_check.sh fetches every asset the bundle table names
# and byte-compares against the committed file; its GUARD-KIND: runtime
# marker makes it an owed skip in the pre-restart gate, and THIS call is
# where it is authoritative -- the instance being verified is the one this
# deploy just started.
SERVED_BASE="${SERVED_BASE:-$(printf '%s' "$HEALTH_URL" | sed -E 's#/api/health/?$##')}"
[ -n "$SERVED_BASE" ] || SERVED_BASE="http://localhost:3001"
echo "[deploy] Verifying the NEW kernel serves exactly the committed webapp bundle..."
if ! bash tests/webapp_served_check.sh --url "$SERVED_BASE"; then
    echo "[deploy] FAILED: the restarted kernel does not serve the committed webapp bundle."
    echo "         The new kernel IS running and answered health, but a served asset"
    echo "         drifts from kernel/webapp_bundle.c -- investigate before trusting"
    echo "         this deploy. This is the served-side half of the 2026-08-18"
    echo "         incident: a webapp that exists nowhere in the repo."
    exit 1
fi

echo "[deploy] Deploy succeeded."
