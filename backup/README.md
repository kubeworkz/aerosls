# AeroSLS backup/restore for `sls_storage.img` (Operational Phase D)

See `docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md` for the full writeup.

## 1. How it works

`sls_storage.img` is a raw disk image the kernel's NVMe emulation writes to
continuously. There's no known filesystem-level snapshot capability on the
host (no LVM/ZFS/btrfs assumed), so a live copy risks a torn read. Both
scripts trade a few seconds of downtime for a guaranteed-consistent copy:
`pm2 stop` -> `cp --sparse=always` -> `pm2 start` -> poll `/api/health`
before declaring success. Same tradeoff `deploy/deploy.sh` already accepts.

## 2. `backup.sh` via cron

Two separate cron entries, one per retention tier -- `BACKUP_KIND` is always
set explicitly, never auto-detected from the time of day (that would be
brittle against exactly when cron fires):

```
crontab -e
```

```
# Hourly snapshots, keep the newest 4
0 * * * * cd /path/to/aerosls2 && BACKUP_KIND=hourly ./backup/backup.sh >> /var/log/aerosls-backup.log 2>&1

# Daily snapshot, keep the newest 7
0 3 * * * cd /path/to/aerosls2 && BACKUP_KIND=daily ./backup/backup.sh >> /var/log/aerosls-backup.log 2>&1
```

Config vars (all optional, shown with defaults): `PM2_APP_NAME=aerosls`,
`STORAGE_IMG=sls_storage.img`, `BACKUP_DIR=/var/backups/aerosls`,
`KEEP_HOURLY=4`, `KEEP_DAILY=7`, `HEALTH_URL=http://localhost:3001/api/health`.

Backups land in `$BACKUP_DIR/hourly/` and `$BACKUP_DIR/daily/` as
`sls_storage-<timestamp>.img`, pruned to the configured retention count
after each successful run. If the post-restart health check fails, the
backup itself is still kept (it already succeeded) but the script exits
non-zero -- treat that as an incident (the service needs attention), not
just a failed backup.

## 3. Restoring

```
./backup/restore.sh /var/backups/aerosls/hourly/sls_storage-20260720-030000.img
```

Run with no argument to list available backups. The script:

1. Requires typing `yes` at a confirmation prompt -- nothing is touched
   before that.
2. Stops the kernel via pm2.
3. Copies the **current** `sls_storage.img` to a
   `sls_storage.img.pre-restore-<timestamp>` safety copy first, before
   touching anything else -- so a restore that turns out to be the wrong
   call, or a bug in the script itself, doesn't destroy state that might
   still have been recoverable.
4. Copies the chosen backup over `sls_storage.img`.
5. Restarts the kernel and polls `/api/health`.
6. Spot-checks `GET /api/tables` with a bearer token
   (`SPOT_CHECK_TOKEN`, defaults to Dave's fixed demo token) and prints the
   response, so you're looking at real data, not just a 200 status.

The pre-restore safety copy is never deleted automatically -- remove it by
hand once you've confirmed the restore is good.

## 4. Verified this session

Both scripts pass `bash -n`. Functionally smoke-tested end-to-end against a
throwaway repo copy with fake `pm2`/`curl` shims standing in for the real
server processes (real `pm2`/live kernel weren't reachable from this
sandbox -- do a first supervised run on the actual server rather than trust
this blind, same caveat as `deploy.sh`):

- `backup.sh`: retention pruning keeps exactly `KEEP_HOURLY`/`KEEP_DAILY`
  newest files and deletes older ones; `hourly`/`daily` kinds stay in
  separate directories; rejects an invalid `BACKUP_KIND` and a missing
  `STORAGE_IMG` without touching anything; aborts cleanly with no backup
  file left behind if `pm2 stop` fails.
- `restore.sh`: typing anything other than `yes` aborts with the storage
  image and no safety copy created; a full `yes` run correctly restores the
  chosen backup's exact contents, the safety copy exactly matches the
  pre-restore state, and the `/api/tables` spot-check runs and prints real
  output; aborts before copying anything if `pm2 stop` fails; a nonexistent
  backup path is rejected up front.
