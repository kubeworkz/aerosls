# aerobackup — `backup.sh`'s workload, as a static single-threaded binary

The snapshot-and-retain half of [`backup/backup.sh`](../backup.sh) (Operational-MVP
Phase D), rewritten in Rust. It is **E7's designated census target**: the Linux ABI
shim's syscall table is built from an `strace` of this binary, per
`tools/linux_abi_candidate.conf` and
[`docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md`](../../docs/AeroSLS-Linux-ABI-Shim-Design-v0.1.md) §5.5.1.

**Not yet the production backup path.** `backup/backup.sh` still runs from cron. This
binary is the candidate the shim gets measured against; swapping the production
cron line over is a separate decision that should follow the census and a bake-in.

## What it does, and what it deliberately does not

`backup.sh` does four things. Only two are the workload:

| Step | Here? | Why |
|---|---|---|
| Copy `sls_storage.img` into the tier | **yes** | this is the work |
| Prune the tier to a retention count | **yes** | this is the work |
| Stop/start the kernel under `pm2` | no | process-manager orchestration, not the workload |
| Poll `/api/health` after restart | no | an HTTP request — and the networking that eliminated the *monitoring* candidate from E7's shortlist, so it cannot be smuggled back in here |

## Three deliberate differences from the shell original

1. **No partial backup can wear the final name.** The original copies straight to
   `sls_storage-<ts>.img`, so an interrupted copy leaves a truncated file that looks
   exactly like a good backup — and retention will keep it in preference to a real,
   older one. Here the copy lands on `<name>.img.partial`, is synced, and is
   `rename`d into place; rename is atomic within a filesystem. Retention ignores
   anything not ending in `.img`, so a stale `.partial` is always a crash artefact
   and never a candidate for "keeping".
2. **The copy is durable before it is named.** `cp` leaves durability to writeback; a
   backup renamed before its bytes reach the disk can be absent after a crash, which
   is the one situation a backup exists for.
3. **`--retain 0` is refused.** The original's `tail -n +$((keep + 1))` with `keep=0`
   deletes every backup *including the one it just took*. It is never what anyone
   means, so the invocation is rejected instead of performed faithfully.

Two smaller notes: the timestamp in the file name is **UTC** (`date` gave local time —
retention does not depend on it, and UTC cannot repeat a name across a DST fall-back),
and the copy preserves sparseness by detecting zero runs rather than by
`cp --sparse=always`, so it needs no libc and is testable as ordinary code.

## Usage

```
aerobackup --storage sls_storage.img --backup-dir /var/backups/aerosls --kind hourly
```

| Flag | Environment | Default |
|---|---|---|
| `--storage` | `STORAGE_IMG` | `sls_storage.img` |
| `--backup-dir` | `BACKUP_DIR` | `/var/backups/aerosls` |
| `--kind hourly\|daily` | `BACKUP_KIND` | `hourly` |
| `--retain <n>` | `KEEP_HOURLY` / `KEEP_DAILY` | `4` / `7` |
| `--now <epoch>` | — | the clock |
| `--dry-run` | — | off |

Backups land in `<backup-dir>/<kind>/` as `sls_storage-<UTC timestamp>.img`, pruned
to the newest `n` **by modification time** — the same rule as
`ls -1t … | tail -n +n+1` — with one thing the shell pipeline leaves undefined made
explicit: `ls -1t` orders equal mtimes by directory order, which is not a contract,
so retention breaks ties on the file name and is therefore deterministic.

Exit: `0` success, `1` the backup or prune failed, `2` the invocation was wrong (the
script exited `1` for every failure including a bad `BACKUP_KIND`; a bad value for a
named option is a usage error).

## Tests

```
cargo test --manifest-path backup/aerobackup/Cargo.toml
```

27 tests, no dependencies, no `unsafe` (`#![forbid(unsafe_code)]`). The retention
test gives backups real mtimes via `File::set_modified` rather than sleeping, so it
is deterministic. Four sabotages were each **observed** to redden the test that
claims to catch them, which is the only reason to believe any of them:

| Sabotage | Reddens |
|---|---|
| drop `plan_prune` from `run()` (the decision exists but nothing calls it) | the prune test |
| count `.partial` copies as backups | the prune test |
| order retention by name instead of mtime | `mtime_decides_not_the_name` |
| make the copy dense instead of sparse-preserving | the sparse-allocation test |

## Building the census target

```
rustup target add x86_64-unknown-linux-musl
tools/linux_syscall_census.sh --run
```

The census tool builds this crate, verifies it against design §5.1's image contract
(`ET_EXEC`, no `PT_INTERP`, no `DT_NEEDED`) and refuses to trace anything that fails
it, then writes `tools/linux_syscall_census.txt` keyed by syscall **number**.
