#!/usr/bin/env bash
# backup/backup.sh -- Operational Phase D
# (docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md).
#
# Snapshots sls_storage.img with a retention policy. Copies while the
# kernel is briefly stopped (pm2 stop -> copy -> pm2 start) rather than
# copying a live raw disk image the kernel's NVMe emulation is actively
# writing to -- the host filesystem here isn't known to support a true
# point-in-time snapshot (no LVM/ZFS/btrfs assumed), and a torn read of a
# raw disk image mid-write is a real risk a backup script shouldn't take.
# This trades a few seconds of downtime per backup run for a backup that's
# actually guaranteed consistent -- the same tradeoff deploy.sh already
# accepts (explicitly out of scope for MVP: zero-downtime blue/green).
set -u
cd "$(dirname "$0")/.."

PM2_APP_NAME="${PM2_APP_NAME:-aerosls}"
STORAGE_IMG="${STORAGE_IMG:-sls_storage.img}"
BACKUP_DIR="${BACKUP_DIR:-/var/backups/aerosls}"
HEALTH_URL="${HEALTH_URL:-http://localhost:3001/api/health}"
HEALTH_RETRIES="${HEALTH_RETRIES:-15}"
HEALTH_RETRY_DELAY_SECS="${HEALTH_RETRY_DELAY_SECS:-2}"
KEEP_HOURLY="${KEEP_HOURLY:-4}"
KEEP_DAILY="${KEEP_DAILY:-7}"
# "hourly" or "daily" -- set explicitly via two separate cron entries (see
# backup/README.md), not auto-detected from the current time, which would
# be brittle against exactly when cron happens to fire.
KIND="${BACKUP_KIND:-hourly}"

if [ "$KIND" != "hourly" ] && [ "$KIND" != "daily" ]; then
    echo "[backup] FAILED: BACKUP_KIND must be 'hourly' or 'daily', got '$KIND'."
    exit 1
fi

if [ ! -f "$STORAGE_IMG" ]; then
    echo "[backup] FAILED: $STORAGE_IMG not found (run from the aerosls2 repo root, or set STORAGE_IMG)."
    exit 1
fi

mkdir -p "$BACKUP_DIR/$KIND"

ts="$(date +%Y%m%d-%H%M%S)"
dest="$BACKUP_DIR/$KIND/sls_storage-$ts.img"

echo "[backup] Stopping $PM2_APP_NAME..."
if ! pm2 stop "$PM2_APP_NAME"; then
    echo "[backup] FAILED: pm2 stop failed. Aborting -- not copying a live image."
    exit 1
fi

echo "[backup] Copying $STORAGE_IMG -> $dest..."
if ! cp --sparse=always "$STORAGE_IMG" "$dest"; then
    echo "[backup] FAILED: copy failed. Restarting $PM2_APP_NAME before exiting."
    pm2 start "$PM2_APP_NAME" || pm2 restart "$PM2_APP_NAME"
    exit 1
fi

echo "[backup] Restarting $PM2_APP_NAME..."
pm2 start "$PM2_APP_NAME" || pm2 restart "$PM2_APP_NAME"

echo "[backup] Waiting for $HEALTH_URL..."
healthy=0
for i in $(seq 1 "$HEALTH_RETRIES"); do
    if curl -sf --max-time 3 "$HEALTH_URL" >/dev/null 2>&1; then
        healthy=1
        break
    fi
    sleep "$HEALTH_RETRY_DELAY_SECS"
done
if [ "$healthy" -ne 1 ]; then
    echo "[backup] WARNING: kernel did not come back healthy after restart. The backup at $dest was still taken successfully, but the running service needs attention now -- treat this as an incident, not just a backup-script failure."
    exit 1
fi
echo "[backup] Kernel healthy."

keep_var="KEEP_HOURLY"
[ "$KIND" = "daily" ] && keep_var="KEEP_DAILY"
keep="${!keep_var}"
echo "[backup] Pruning $KIND backups (keeping newest $keep)..."
# shellcheck disable=SC2012
ls -1t "$BACKUP_DIR/$KIND"/sls_storage-*.img 2>/dev/null | tail -n +"$((keep + 1))" | xargs -r rm -f

echo "[backup] Done: $dest"
