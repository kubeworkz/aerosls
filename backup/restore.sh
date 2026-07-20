#!/usr/bin/env bash
# backup/restore.sh -- Operational Phase D
# (docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md).
#
# Restores sls_storage.img from a backup taken by backup.sh. Takes a safety
# copy of the CURRENT image first, before touching anything else -- so a
# restore that turns out to be the wrong choice, or a mistake in this
# script itself, doesn't destroy state that might still have been
# recoverable. Requires typing "yes" to confirm before doing anything
# destructive.
set -u
cd "$(dirname "$0")/.."

PM2_APP_NAME="${PM2_APP_NAME:-aerosls}"
STORAGE_IMG="${STORAGE_IMG:-sls_storage.img}"
BACKUP_DIR="${BACKUP_DIR:-/var/backups/aerosls}"
HEALTH_URL="${HEALTH_URL:-http://localhost:3001/api/health}"
HEALTH_RETRIES="${HEALTH_RETRIES:-15}"
HEALTH_RETRY_DELAY_SECS="${HEALTH_RETRY_DELAY_SECS:-2}"
# A real bearer token, for the post-restore spot-check read below. Dave's
# own fixed demo token by default -- Architectural Phase 4 only changed
# password verification at token ISSUANCE (POST /auth/token); it didn't
# change validation of already-active tokens like this one (see that
# phase's own notes). Override with SPOT_CHECK_TOKEN to use a different one.
SPOT_CHECK_TOKEN="${SPOT_CHECK_TOKEN:-deadbeef01234567cafebabe76543210}"

backup_file="${1:-}"
if [ -z "$backup_file" ] || [ ! -f "$backup_file" ]; then
    echo "Usage: $0 <path-to-backup-image>"
    echo ""
    echo "Available backups:"
    find "$BACKUP_DIR" -name 'sls_storage-*.img' 2>/dev/null | sort
    exit 1
fi

echo "[restore] About to STOP $PM2_APP_NAME and replace $STORAGE_IMG with:"
echo "          $backup_file"
read -r -p "[restore] Type 'yes' to continue: " confirm
if [ "$confirm" != "yes" ]; then
    echo "[restore] Aborted -- nothing was touched."
    exit 1
fi

echo "[restore] Stopping $PM2_APP_NAME..."
if ! pm2 stop "$PM2_APP_NAME"; then
    echo "[restore] FAILED: pm2 stop failed. Aborting before touching any files."
    exit 1
fi

safety_copy="${STORAGE_IMG}.pre-restore-$(date +%Y%m%d-%H%M%S)"
echo "[restore] Safety-copying current $STORAGE_IMG -> $safety_copy first..."
if ! cp --sparse=always "$STORAGE_IMG" "$safety_copy"; then
    echo "[restore] FAILED: safety copy failed. Restarting $PM2_APP_NAME and aborting -- refusing to touch $STORAGE_IMG without a safety copy first."
    pm2 start "$PM2_APP_NAME" || pm2 restart "$PM2_APP_NAME"
    exit 1
fi

echo "[restore] Copying $backup_file -> $STORAGE_IMG..."
if ! cp --sparse=always "$backup_file" "$STORAGE_IMG"; then
    echo "[restore] FAILED: restore copy failed partway through. $safety_copy still has your pre-restore state -- $STORAGE_IMG may now be in a bad state. Restarting $PM2_APP_NAME anyway so you can investigate; do not assume it's healthy."
    pm2 start "$PM2_APP_NAME" || pm2 restart "$PM2_APP_NAME"
    exit 1
fi

echo "[restore] Restarting $PM2_APP_NAME..."
pm2 start "$PM2_APP_NAME" || pm2 restart "$PM2_APP_NAME"

echo "[restore] Waiting for $HEALTH_URL..."
healthy=0
for i in $(seq 1 "$HEALTH_RETRIES"); do
    if curl -sf --max-time 3 "$HEALTH_URL" >/dev/null 2>&1; then
        healthy=1
        break
    fi
    sleep "$HEALTH_RETRY_DELAY_SECS"
done
if [ "$healthy" -ne 1 ]; then
    echo "[restore] FAILED: kernel did not come back healthy after restore. Your pre-restore state is safe at $safety_copy -- consider restoring that back if this doesn't recover."
    exit 1
fi
echo "[restore] Kernel healthy. Spot-checking GET /api/tables..."

spot_url="$(echo "$HEALTH_URL" | sed 's#/api/health#/api/tables#')"
if ! curl -sf --max-time 5 -H "Authorization: Bearer $SPOT_CHECK_TOKEN" "$spot_url" > /tmp/aerosls_restore_spotcheck.json; then
    echo "[restore] WARNING: kernel is up but the /api/tables spot-check request failed (bad token? route down?). Investigate before trusting this restore. Your pre-restore state is safe at $safety_copy."
    exit 1
fi

echo "[restore] Spot-check response:"
cat /tmp/aerosls_restore_spotcheck.json
echo ""
echo "[restore] Restore succeeded. Pre-restore safety copy kept at:"
echo "          $safety_copy"
echo "[restore] Delete it manually once you're confident the restore is good."
