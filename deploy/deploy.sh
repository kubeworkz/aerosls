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
# Assumes slsos-sim is checked out as a sibling directory (../slsos-sim),
# matching the Makefile's own `bundle` target, which already expects that
# layout.
set -u

# Confirm/override with `pm2 list` -- this script doesn't know your actual
# process name, only that pm2 is what manages it.
PM2_APP_NAME="${PM2_APP_NAME:-aerosls}"
# Hits the kernel directly, not through nginx/Cloudflare -- this script is
# verifying the KERNEL came up correctly, not the whole public chain, which
# has its own separate failure modes this isn't trying to catch.
HEALTH_URL="${HEALTH_URL:-http://localhost:3001/api/health}"
HEALTH_RETRIES="${HEALTH_RETRIES:-15}"
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

# ─── Node via nvm's default, falling back to the system node ────────────────────────────
# The frontend build must run under a MODERN npm. The system npm 9 on this
# box (node 18) has a known optional-dependencies bug (npm/cli#4828) that
# skips platform-specific native bindings -- @tailwindcss/oxide-linux-x64-gnu
# in particular -- so `npm run build` dies with "Cannot find native binding".
# An interactive shell gets node 24 because .bashrc sources nvm; a
# non-interactive run (cron, CI, a bare ssh command) does not, which is how
# a deploy that built fine by hand fails unattended. Sourcing nvm here makes
# the two identical. If nvm is absent, fall back to whatever node is on PATH.
if [ -s "${NVM_DIR:-$HOME/.nvm}/nvm.sh" ]; then
    # shellcheck disable=SC1090
    . "${NVM_DIR:-$HOME/.nvm}/nvm.sh"
    # `nvm use default` can fail when no default alias exists -- the system
    # node then stays in place, which is the same fallback as no nvm at all.
    nvm use default >/dev/null 2>&1 || true
    echo "[deploy] node: $(node --version) via $(command -v node)"
else
    echo "[deploy] node: $(node --version) (system, no nvm found)"
fi

# The Makefile's own `bundle` target runs `npm run build --silent
# 2>/dev/null || true` -- the `|| true` means a frontend build failure
# there is silently swallowed and `make bundle` proceeds anyway, re-bundling
# whatever old slsos-sim/dist/ happens to already be on disk. That's a
# separate, pre-existing gap in the Makefile itself (not fixed here --
# changing it affects every existing caller of `make bundle`, including
# local dev, a bigger blast radius than this one new script justifies).
# Instead, this script does its own explicit, properly-checked frontend
# pull+build BEFORE calling `make bundle`, so a real frontend build failure
# stops this deploy instead of silently shipping a stale bundle.
echo "[deploy] Pulling latest slsos-sim..."
if ! (cd ../slsos-sim && git pull); then
    echo "[deploy] FAILED: git pull (slsos-sim) failed. Aborting -- nothing rebuilt or restarted."
    exit 1
fi

# ─── ../qemu: pulled because the kernel is BUILT from it ───────────────────
# The Makefile links 15 objects out of ../qemu (tcg/, accel/tcg/ and all of
# sls/ -- see TCG_OBJS and the VPATH line). This script pulled aerosls2 and
# slsos-sim and built from three repositories, so every change under
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

echo "[deploy] Building frontend (npm ci && npm run build)..."
if ! (cd ../slsos-sim && npm ci && npm run build); then
    echo "[deploy] FAILED: frontend build failed. Aborting -- kernel NOT restarted, still running the previous build."
    exit 1
fi

echo "[deploy] Generating kernel/webapp_bundle.c (make bundle)..."
if ! make bundle; then
    echo "[deploy] FAILED: make bundle failed. Aborting -- kernel NOT restarted, still running the previous build."
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

echo "[deploy] Restarting pm2 process '$PM2_APP_NAME'..."
if ! pm2 restart "$PM2_APP_NAME"; then
    echo "[deploy] FAILED: pm2 restart failed. Check 'pm2 list' -- is PM2_APP_NAME=$PM2_APP_NAME the right process name?"
    exit 1
fi

echo "[deploy] Waiting for $HEALTH_URL to answer (up to $((HEALTH_RETRIES * HEALTH_RETRY_DELAY_SECS))s)..."
healthy=0
health_tmp="$(mktemp)"
for i in $(seq 1 "$HEALTH_RETRIES"); do
    if curl -sf --max-time 3 "$HEALTH_URL" > "$health_tmp" 2>/dev/null; then
        healthy=1
        break
    fi
    sleep "$HEALTH_RETRY_DELAY_SECS"
done

if [ "$healthy" -ne 1 ]; then
    echo "[deploy] FAILED: $HEALTH_URL never answered a healthy response."
    echo "[deploy] Last pm2 logs for '$PM2_APP_NAME':"
    pm2 logs "$PM2_APP_NAME" --lines 40 --nostream
    echo "[deploy] The process was restarted but is NOT confirmed healthy -- do not assume this deploy succeeded."
    rm -f "$health_tmp"
    exit 1
fi

echo "[deploy] Healthy:"
cat "$health_tmp"
echo ""
rm -f "$health_tmp"
echo "[deploy] Deploy succeeded."
