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
