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

echo "[deploy] Pulling latest aerosls2..."
if ! git pull; then
    echo "[deploy] FAILED: git pull (aerosls2) failed. Aborting -- nothing rebuilt or restarted."
    exit 1
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

# ─── Post-build assertion: is the image built from the source on disk? ─────
# `make` succeeding proves the compiler ran. It does not prove the compiler
# ran over the sources you think are there -- a partial sync, a stale object
# a dependency rule missed, or a repo that was never pulled all produce a
# clean build of the wrong program.
#
# So this samples literal strings from the qemu/sls sources and requires them
# to be present in the linked binary. A string is a good probe precisely
# because it is inert: it cannot be optimised away, it survives with no
# debug info, and finding it proves that specific text was compiled.
#
# Samples the LAST occurrence in each file rather than the first: files are
# appended to far more often than prepended, so the most recent edit is the
# most likely to be the one that did not make it across. Checking the first
# string would have passed happily in the exact case that motivated this.
echo "[deploy] Verifying the built image matches the sources on disk..."
if ! command -v strings >/dev/null; then
    echo "[deploy] FAILED: 'strings' not found (binutils). Cannot verify the build."
    echo "         Install binutils rather than skipping -- a verification that"
    echo "         silently does nothing is worse than none, because it is trusted."
    exit 1
fi

KERNEL_BIN="my_sls_kernel.bin"
[ -f "$KERNEL_BIN" ] || { echo "[deploy] FAILED: $KERNEL_BIN missing after a successful make."; exit 1; }

command -v perl >/dev/null || { echo "[deploy] FAILED: perl not found; needed to strip comments before probing."; exit 1; }

probe_missing=0
probes_run=0
probes_skipped=0
for src in ../qemu/sls/sls-launcher.c ../qemu/sls/sls-runtime.c \
           ../qemu/sls/sls-helper-stubs.c kernel/stubs.c \
           net/http.c user/shell.c; do
    [ -f "$src" ] || continue

    # Comments and #directives are stripped FIRST. Both contain quoted text
    # that never reaches the binary -- an #include path, or a comment quoting
    # a compiler message. A first attempt at this skipped that step and drew
    # its probe for kernel_io.c out of a comment (about, of all things, reading
    # absence as evidence), which would have failed a perfectly good build.
    #
    # Then: any complete string literal of 24+ characters containing no printf
    # conversion and no escape. Those three constraints matter -- a conversion
    # or an escape means the bytes in the binary differ from the bytes in the
    # source, so grep -F would not match even on a correct build.
    probe="$(perl -0pe 's{/\*.*?\*/}{}gs; s{//[^\n]*}{}g; s{^\s*#[^\n]*}{}gm' "$src" 2>/dev/null \
             | grep -ho '"[^"%\\]\{24,\}"' | tail -1 | sed 's/^"//;s/"$//')"

    if [ -z "$probe" ]; then
        # Reported, not silently passed over. Some files genuinely have no
        # qualifying literal (kernel/qemu_sls_mmu.c is one), and knowing which
        # files were NOT checked is part of knowing what this check proved.
        probes_skipped=$((probes_skipped + 1))
        echo "[deploy]   skip $src (no probe-able string literal)"
        continue
    fi

    probes_run=$((probes_run + 1))
    if ! strings "$KERNEL_BIN" | grep -qF -- "$probe"; then
        echo "[deploy] MISMATCH: $src"
        echo "         a string near the end of that file is NOT in $KERNEL_BIN:"
        echo "           \"$probe\""
        probe_missing=$((probe_missing + 1))
    fi
done

if [ "$probes_run" -eq 0 ]; then
    echo "[deploy] FAILED: no probe strings could be extracted from any source, so"
    echo "         nothing was actually verified ($probes_skipped file(s) skipped)."
    echo "         Treating that as a failure rather than a pass -- a check that"
    echo "         examined nothing must not report success."
    exit 1
fi

if [ "$probe_missing" -ne 0 ]; then
    echo "[deploy] FAILED: $probe_missing of $probes_run source file(s) contributed"
    echo "         no matching string to the built image. The build is NOT from the"
    echo "         sources on disk -- most likely a partial sync or a stale object."
    echo "         Aborting: kernel NOT restarted, still running the previous build."
    echo "         Try 'make clean && make x86-iso', and check that ../qemu is current."
    exit 1
fi
echo "[deploy] OK: $probes_run source file(s) verified present in $KERNEL_BIN ($probes_skipped skipped)."

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
